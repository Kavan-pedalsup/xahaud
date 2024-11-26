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

namespace ripple {
namespace test {

using namespace jtx;
class LedgerStress_test : public beast::unit_test::suite
{
private:
    static constexpr std::size_t TXN_PER_LEDGER = 20000;
    static constexpr std::size_t MAX_TXN_PER_ACCOUNT = 1;
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
                env(pay(sender, recipient, XRP(1)), 
                    fee(escalatedFee),
                    seq(autofill));
            }
        }
    }

    void
    runStressTest(std::size_t numLedgers)
    {
        testcase("Multithreaded stress test: " + std::to_string(TXN_PER_LEDGER) + 
                " txns/ledger for " + std::to_string(numLedgers) + 
                " ledgers using " + std::to_string(NUM_THREADS) + " threads");

        Env env(*this);
        auto const journal = env.app().journal("LedgerStressTest");

        env.app().config().MAX_TRANSACTIONS = 100000;

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

            // Create threads for parallel submission
            std::vector<std::thread> threads;
            threads.reserve(NUM_THREADS);

            std::size_t accountsPerThread = 
                (accounts.size() + NUM_THREADS - 1) / NUM_THREADS;

            for (std::size_t t = 0; t < NUM_THREADS; ++t)
            {
                std::size_t startIdx = t * accountsPerThread;
                std::size_t endIdx = std::min(startIdx + accountsPerThread, accounts.size());
                
                threads.emplace_back(
                    [this, &env, &accounts, startIdx, endIdx]() {
                        submitBatchThread(
                            env, 
                            accounts, 
                            accounts, 
                            startIdx, 
                            endIdx,
                            MAX_TXN_PER_ACCOUNT);
                    }
                );
            }

            // Wait for all threads to complete
            for (auto& thread : threads)
            {
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
                         " in " + std::to_string(totalTime.count()) + "ms");
        }

        // Print summary
        auto avgSubmitTime = std::chrono::milliseconds(0);
        auto avgCloseTime = std::chrono::milliseconds(0);
        XRPAmount totalFees{0};
        std::size_t totalTxns = 0;

        for (auto const& m : metrics)
        {
            avgSubmitTime += m.submitTime;
            avgCloseTime += m.closeTime;
            totalFees += m.baseFee;
            totalTxns += m.txCount;
        }

        avgSubmitTime /= metrics.size();
        avgCloseTime /= metrics.size();

        threadSafeLog(
            "Test Summary - "
            "Average submit time: " + std::to_string(avgSubmitTime.count()) + "ms, "
            "Average close time: " + std::to_string(avgCloseTime.count()) + "ms, "
            "Average base fee: " + std::to_string(totalFees.drops()/metrics.size()) + " drops, "
            "Total transactions: " + std::to_string(totalTxns));
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
