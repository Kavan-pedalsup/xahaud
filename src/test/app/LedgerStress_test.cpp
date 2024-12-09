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
    static constexpr std::size_t TXN_PER_LEDGER = 20000;
    static constexpr std::size_t MAX_TXN_PER_ACCOUNT = 5;
    static constexpr std::chrono::seconds MAX_CLOSE_TIME{6};
    static constexpr std::size_t REQUIRED_ACCOUNTS = 
        (TXN_PER_LEDGER + MAX_TXN_PER_ACCOUNT - 1) / MAX_TXN_PER_ACCOUNT;
    
    // Get number of hardware threads and use half
    const std::size_t NUM_THREADS = std::max(std::thread::hardware_concurrency() / 2, 1u);

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

    // Structure to hold work assignment for each thread
    struct ThreadWork {
        std::size_t startAccountIdx;
        std::size_t endAccountIdx;
        std::size_t numTxnsToSubmit;
    };
    
    void 
    submitBatchThread(
        jtx::Env& env,
        std::vector<jtx::Account> const& accounts,
        ThreadWork const& work)
    {
        auto const escalatedFee = getEscalatedFee(env);
        std::size_t txnsSubmitted = 0;
        
        for (std::size_t i = work.startAccountIdx; 
             i < work.endAccountIdx && txnsSubmitted < work.numTxnsToSubmit; ++i)
        {
            auto const& sender = accounts[i];
            
            // Create list of possible recipients (everyone except sender)
            std::vector<Account> recipients;
            recipients.reserve(accounts.size() - 1);
            for (auto const& acct : accounts)
            {
                if (acct.id() != sender.id())
                    recipients.push_back(acct);
            }

            // Calculate how many txns to submit from this account
            std::size_t txnsRemaining = work.numTxnsToSubmit - txnsSubmitted;
            std::size_t txnsForAccount = std::min(MAX_TXN_PER_ACCOUNT, txnsRemaining);
            
            // Submit transactions
            for (std::size_t tx = 0; tx < txnsForAccount; ++tx)
            {
                auto const& recipient = recipients[tx % recipients.size()];
                env.inject(pay(sender, recipient, XRP(1)), 
                    fee(escalatedFee),
                    seq(autofill));
                ++txnsSubmitted;
            }
        }
    }

    void runStressTest(std::size_t numLedgers)
    {
        testcase("Multithreaded stress test: " + std::to_string(TXN_PER_LEDGER) +
                " txns/ledger for " + std::to_string(numLedgers) +
                " ledgers using " + std::to_string(NUM_THREADS) + " threads");

        Env env{*this, envconfig(many_workers)};
        env.app().config().MAX_TRANSACTIONS = 100000;

        auto const journal = env.app().journal("LedgerStressTest");
        
        // Get actual hardware thread count
        std::size_t hardwareThreads = static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (hardwareThreads == 0)
            hardwareThreads = 4;  // Fallback
        
        const std::size_t THREAD_COUNT = std::min(NUM_THREADS, hardwareThreads);
        
        threadSafeLog("Using " + std::to_string(THREAD_COUNT) + " hardware threads");
        threadSafeLog("Creating " + std::to_string(REQUIRED_ACCOUNTS) + " accounts");
        
        auto accounts = createAccounts(env, REQUIRED_ACCOUNTS);

        std::vector<LedgerMetrics> metrics;
        metrics.reserve(numLedgers);

        for (std::size_t ledger = 0; ledger < numLedgers; ++ledger)
        {
            threadSafeLog("Starting ledger " + std::to_string(ledger));
            LedgerMetrics ledgerMetrics;

            auto submitStart = std::chrono::steady_clock::now();
            ledgerMetrics.baseFee = env.current()->fees().base;

            // Pre-calculate exact work distribution for threads
            std::vector<ThreadWork> threadAssignments;
            threadAssignments.reserve(THREAD_COUNT);
            
            std::size_t totalAccountsAssigned = 0;
            std::size_t totalTxnsAssigned = 0;
            std::size_t accountsPerThread = accounts.size() / THREAD_COUNT;
            
            for (std::size_t t = 0; t < THREAD_COUNT; ++t)
            {
                ThreadWork work;
                work.startAccountIdx = totalAccountsAssigned;
                
                // Last thread gets remaining accounts
                if (t == THREAD_COUNT - 1) {
                    work.endAccountIdx = accounts.size();
                    work.numTxnsToSubmit = TXN_PER_LEDGER - totalTxnsAssigned;
                }
                else {
                    work.endAccountIdx = work.startAccountIdx + accountsPerThread;
                    work.numTxnsToSubmit = TXN_PER_LEDGER / THREAD_COUNT;
                }
                
                totalAccountsAssigned = work.endAccountIdx;
                totalTxnsAssigned += work.numTxnsToSubmit;
                threadAssignments.push_back(work);
            }
            
            BEAST_EXPECT(totalTxnsAssigned == TXN_PER_LEDGER);

            // Launch threads with pre-calculated work assignments
            std::vector<std::thread> threads;
            threads.reserve(THREAD_COUNT);
            
            for (std::size_t t = 0; t < THREAD_COUNT; ++t)
            {
                threads.emplace_back(
                    [&env, &accounts, work = threadAssignments[t], this]() {
                        submitBatchThread(env, accounts, work);
                    }
                );
            }

            // Wait for all threads
            for (auto& thread : threads)
            {
                if (thread.joinable())
                    thread.join();
            }

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
        runStressTest(5);
    }
};

BEAST_DEFINE_TESTSUITE(LedgerStress, app, ripple);

}  // namespace test
}  // namespace ripple
