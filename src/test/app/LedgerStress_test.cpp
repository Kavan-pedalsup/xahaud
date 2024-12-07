#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/basics/chrono.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <ripple/protocol/AccountID.h>
#include <algorithm>

namespace ripple {
namespace test {

using namespace jtx;
class LedgerStress_test : public beast::unit_test::suite
{
private:
    static constexpr std::size_t TXN_PER_LEDGER = 100;
    static constexpr std::size_t MAX_TXN_PER_ACCOUNT = 1;
    static constexpr std::chrono::seconds MAX_CLOSE_TIME{6};
    static constexpr std::size_t REQUIRED_ACCOUNTS = 
        (TXN_PER_LEDGER + MAX_TXN_PER_ACCOUNT - 1) / MAX_TXN_PER_ACCOUNT;
    
    // Get number of hardware threads and use half
    const std::size_t NUM_THREADS = 1;//std::max(std::thread::hardware_concurrency() / 2, 1u);

    struct LedgerMetrics {
        std::chrono::milliseconds submitTime{0};
        std::chrono::milliseconds closeTime{0};
        std::size_t txCount{0};
        XRPAmount baseFee{0};
        
        void log(beast::Journal const& journal) const
        {
            std::cout 
                << "Metrics - Submit time: " << submitTime.count() << "ms, "
                << "Close time: " << closeTime.count() << "ms, "
                << "Transaction count: " << txCount
                << "Base fee: " << baseFee;
        }
    };

    // Thread-safe console output
    std::mutex consoleMutex;
    template<typename T>
    void threadSafeLog(T const& message) {
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << message << std::endl;
    }

    XRPAmount
    getEscalatedFee(jtx::Env& env) const
    {
        auto const metrics = env.app().getTxQ().getMetrics(*env.current());
        auto const baseFee = env.current()->fees().base;
        
        auto const feeLevel = mulDiv(
            metrics.medFeeLevel, 
            baseFee, 
            metrics.referenceFeeLevel).second;
            
        auto const escalatedFee = XRPAmount{feeLevel};
        return XRPAmount{escalatedFee.drops() + (escalatedFee.drops())};
    }

    std::vector<jtx::Account> 
    createAccounts(jtx::Env& env, std::size_t count)
    {
        std::vector<jtx::Account> accounts;
        accounts.reserve(count);
        
        for (std::size_t i = 0; i < count; ++i)
        {
            std::string name = "account" + std::to_string(i);
            auto account = jtx::Account(name);
            accounts.push_back(account);
            env.fund(false, XRP(100000), account);

            if (i % 2500 == 0 && i != 0)
                std::cout << "Accounts: " << i << "\n";
        }
        env.close();
        return accounts;
    }
    
    void 
    submitBatchThread(
        jtx::Env& env,
        std::vector<jtx::Account> const& senders,
        std::vector<jtx::Account> const& allAccounts,
        std::size_t startIdx,
        std::size_t endIdx,
        std::size_t txPerAccount)
    {
        static thread_local int tmp = 0;
        std::cout << "submitBatchThread " << reinterpret_cast<uint64_t>(&tmp) << "\n";

        for (std::size_t i = startIdx; i < endIdx; ++i)
        {
            auto const& sender = senders[i];
            
            std::vector<Account> recipients;
            recipients.reserve(allAccounts.size() - 1);
            for (auto const& acct : allAccounts)
            {
                if (acct.id() != sender.id())
                    recipients.push_back(acct);
            }

            auto const escalatedFee = getEscalatedFee(env);
            
            for (std::size_t tx = 0; tx < txPerAccount; ++tx)
            {
                auto const& recipient = recipients[tx % recipients.size()];
                env.inject(pay(sender, recipient, XRP(1)), 
                    fee(escalatedFee),
                    seq(autofill));
            }
        }
    }

    void runStressTest(std::size_t numLedgers)
    {
        testcase("Multithreaded stress test: " + std::to_string(TXN_PER_LEDGER) +
                " txns/ledger for " + std::to_string(numLedgers) +
                " ledgers using " + std::to_string(NUM_THREADS) + " threads");

        Env env{*this, envconfig(many_workers)};

        Application* appPtr = env.getApp();

        std::cout << "Application ptr: " << reinterpret_cast<uint64_t>(appPtr) << "\n";

        auto const journal = env.app().journal("LedgerStressTest");

        env.app().config().MAX_TRANSACTIONS = 100000;

        // Get actual hardware thread count and ensure consistent types
        std::size_t hardwareThreads = static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (hardwareThreads == 0)
            hardwareThreads = 4;  // Fallback if hardware_concurrency() fails
        
        const std::size_t THREAD_COUNT = std::min(NUM_THREADS, hardwareThreads);
        
        threadSafeLog("Using " + std::to_string(THREAD_COUNT) + " hardware threads");
        threadSafeLog("Creating " + std::to_string(REQUIRED_ACCOUNTS) + " accounts");
        
        auto accounts = createAccounts(env, REQUIRED_ACCOUNTS);

        std::vector<LedgerMetrics> metrics;
        metrics.reserve(numLedgers);

        // Create thread pool
        std::vector<std::thread> threadPool;
        threadPool.reserve(THREAD_COUNT);

        for (std::size_t ledger = 0; ledger < numLedgers; ++ledger)
        {
            threadSafeLog("Starting ledger " + std::to_string(ledger));
            LedgerMetrics ledgerMetrics;

            auto submitStart = std::chrono::steady_clock::now();
            ledgerMetrics.baseFee = env.current()->fees().base;

            // Calculate work distribution
            std::size_t txnsPerThread = (TXN_PER_LEDGER + THREAD_COUNT - 1) / THREAD_COUNT;
            std::size_t accountsPerThread = (accounts.size() + THREAD_COUNT - 1) / THREAD_COUNT;

            // Atomic counter for synchronization
            std::atomic<std::size_t> completedTxns{0};
            std::mutex submitMutex;

            // Launch worker threads
            for (std::size_t t = 0; t < THREAD_COUNT; ++t)
            {
                std::size_t startIdx = t * accountsPerThread;
                std::size_t endIdx = std::min(startIdx + accountsPerThread, accounts.size());
                
                threadPool.emplace_back(
                    [&, startIdx, endIdx]() {
                        try {
                            std::size_t localTxnCount = 0;
                            std::size_t targetTxns = txnsPerThread;
                            
                            // Ensure we don't exceed total desired transactions
                            {
                                std::lock_guard<std::mutex> lock(submitMutex);
                                std::size_t remaining = TXN_PER_LEDGER - completedTxns;
                                targetTxns = std::min(txnsPerThread, remaining);
                            }

                            submitBatchThread(
                                env,
                                accounts,
                                accounts,
                                startIdx,
                                endIdx,
                                targetTxns);

                            completedTxns += targetTxns;
                        }
                        catch (const std::exception& e) {
                            threadSafeLog("Thread exception: " + std::string(e.what()));
                        }
                    }
                );
            }

            // Wait for all threads to complete
            for (auto& thread : threadPool)
            {
                if (thread.joinable())
                    thread.join();
            }
            threadPool.clear();

            ledgerMetrics.submitTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - submitStart);

            auto closeStart = std::chrono::steady_clock::now();
            env.close();
            ledgerMetrics.closeTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - closeStart);

            auto const closed = env.closed();
            ledgerMetrics.txCount = closed->txCount();

            ledgerMetrics.log(journal);
            metrics.push_back(ledgerMetrics);

            BEAST_EXPECT(ledgerMetrics.txCount == TXN_PER_LEDGER);

            auto const totalTime = ledgerMetrics.submitTime + ledgerMetrics.closeTime;
            BEAST_EXPECT(totalTime <= std::chrono::duration_cast<std::chrono::milliseconds>(MAX_CLOSE_TIME));

            threadSafeLog("Completed ledger " + std::to_string(ledger) +
                         " in " + std::to_string(totalTime.count()) + "ms" +
                         " using " + std::to_string(THREAD_COUNT) + " threads");
        }
    }



public:
    void run() override
    {
        runStressTest(1);
    }
};

BEAST_DEFINE_TESTSUITE(LedgerStress, app, ripple);

}  // namespace test
}  // namespace ripple
