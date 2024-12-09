#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/basics/chrono.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <thread>
#include <vector>

namespace ripple {
namespace test {

using namespace jtx;
class LedgerStress_test : public beast::unit_test::suite
{
private:
    static constexpr std::size_t TXN_PER_LEDGER = 10000;
    static constexpr std::size_t MAX_TXN_PER_ACCOUNT = 5;  // Increased from 1
    static constexpr std::chrono::seconds MAX_CLOSE_TIME{6};
    static constexpr std::size_t REQUIRED_ACCOUNTS =
        (TXN_PER_LEDGER + MAX_TXN_PER_ACCOUNT - 1) / MAX_TXN_PER_ACCOUNT;

    // Get number of hardware threads and use half
    const std::size_t NUM_THREADS =
        std::max(std::thread::hardware_concurrency() / 2, 1u);

    struct LedgerMetrics
    {
        std::chrono::milliseconds submitTime{0};
        std::chrono::milliseconds closeTime{0};
        std::size_t txCount{0};
        std::size_t successfulTxCount{
            0};                        // Added to track successful transactions
        std::size_t failedTxCount{0};  // Added to track failed transactions
        XRPAmount baseFee{0};

        void
        log(beast::Journal const& journal) const
        {
            std::cout << "Metrics - Submit time: " << submitTime.count()
                      << "ms, "
                      << "Close time: " << closeTime.count() << "ms, "
                      << "Transaction count: " << txCount << ", "
                      << "Successful: " << successfulTxCount << ", "
                      << "Failed: " << failedTxCount << ", "
                      << "Base fee: " << baseFee;
        }
    };

    // Thread-safe console output
    std::mutex consoleMutex;
    std::atomic<std::size_t> totalSuccessfulTxns{0};

    template <typename T>
    void
    threadSafeLog(T const& message)
    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << message << std::endl;
    }

    XRPAmount
    getEscalatedFee(jtx::Env& env) const
    {
        auto const metrics = env.app().getTxQ().getMetrics(*env.current());
        auto const baseFee = env.current()->fees().base;

        auto const feeLevel =
            mulDiv(metrics.medFeeLevel, baseFee, metrics.referenceFeeLevel)
                .second;

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
                threadSafeLog("Accounts created: " + std::to_string(i));
        }
        env.close();
        return accounts;
    }

    // Structure to hold work assignment for each thread
    struct ThreadWork
    {
        std::size_t startAccountIdx;
        std::size_t endAccountIdx;
        std::size_t numTxnsToSubmit;
        std::size_t successfulTxns{0};
        std::size_t failedTxns{0};
    };

    void
    submitBatchThread(
        jtx::Env& env,
        std::vector<jtx::Account> const& accounts,
        ThreadWork& work)  // Changed to non-const reference to update metrics
    {
        auto const escalatedFee = getEscalatedFee(env);
        std::size_t txnsSubmitted = 0;

        // Track sequence numbers for all accounts in this thread's range
        std::map<AccountID, std::uint32_t> seqNumbers;
        for (std::size_t i = work.startAccountIdx; i < work.endAccountIdx; ++i)
        {
            seqNumbers[accounts[i].id()] = env.seq(accounts[i]);
        }

        // Pre-calculate recipient indices for better distribution
        std::vector<std::size_t> recipientIndices;
        recipientIndices.reserve(accounts.size() - 1);
        for (std::size_t i = 0; i < accounts.size(); ++i)
        {
            if (i < work.startAccountIdx || i >= work.endAccountIdx)
            {
                recipientIndices.push_back(i);
            }
        }

        std::size_t recipientIdx = 0;

        for (std::size_t i = work.startAccountIdx;
             i < work.endAccountIdx && txnsSubmitted < work.numTxnsToSubmit;
             ++i)
        {
            auto const& sender = accounts[i];

            // Calculate how many txns to submit from this account
            std::size_t txnsRemaining = work.numTxnsToSubmit - txnsSubmitted;
            std::size_t txnsForAccount =
                std::min(MAX_TXN_PER_ACCOUNT, txnsRemaining);

            // Submit transactions
            for (std::size_t tx = 0; tx < txnsForAccount; ++tx)
            {
                // Select next recipient using round-robin
                auto const& recipient =
                    accounts[recipientIndices[recipientIdx]];
                recipientIdx = (recipientIdx + 1) % recipientIndices.size();

                try
                {
                    env.inject(
                        pay(sender, recipient, XRP(1)),
                        fee(escalatedFee),
                        seq(seqNumbers[sender.id()]));

                    ++work.successfulTxns;
                    seqNumbers[sender.id()]++;
                }
                catch (std::exception const& e)
                {
                    ++work.failedTxns;
                    threadSafeLog(
                        "Exception submitting transaction: " +
                        std::string(e.what()));
                }

                ++txnsSubmitted;
            }
        }
    }

    void
    runStressTest(std::size_t numLedgers)
    {
        testcase(
            "Multithreaded stress test: " + std::to_string(TXN_PER_LEDGER) +
            " txns/ledger for " + std::to_string(numLedgers) +
            " ledgers using " + std::to_string(NUM_THREADS) + " threads");

        Env env{*this, envconfig(many_workers)};
        env.app().config().MAX_TRANSACTIONS = TXN_PER_LEDGER;

        auto const journal = env.app().journal("LedgerStressTest");

        // Get actual hardware thread count
        std::size_t hardwareThreads =
            static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (hardwareThreads == 0)
            hardwareThreads = 4;  // Fallback

        const std::size_t THREAD_COUNT = std::min(NUM_THREADS, hardwareThreads);

        threadSafeLog(
            "Using " + std::to_string(THREAD_COUNT) + " hardware threads");
        threadSafeLog(
            "Creating " + std::to_string(REQUIRED_ACCOUNTS) + " accounts");

        auto accounts = createAccounts(env, REQUIRED_ACCOUNTS);

        std::vector<LedgerMetrics> metrics;
        metrics.reserve(numLedgers);

        for (std::size_t ledger = 0; ledger < numLedgers; ++ledger)
        {
            threadSafeLog("Starting ledger " + std::to_string(ledger));
            LedgerMetrics ledgerMetrics;

            auto submitStart = std::chrono::steady_clock::now();
            ledgerMetrics.baseFee = env.current()->fees().base;

            // Calculate even distribution of work
            std::vector<ThreadWork> threadAssignments;
            threadAssignments.reserve(THREAD_COUNT);

            std::size_t baseWorkload = TXN_PER_LEDGER / THREAD_COUNT;
            std::size_t remainder = TXN_PER_LEDGER % THREAD_COUNT;
            std::size_t accountsPerThread = accounts.size() / THREAD_COUNT;
            std::size_t totalAccountsAssigned = 0;

            for (std::size_t t = 0; t < THREAD_COUNT; ++t)
            {
                ThreadWork work;
                work.startAccountIdx = totalAccountsAssigned;
                work.endAccountIdx = (t == THREAD_COUNT - 1)
                    ? accounts.size()
                    : work.startAccountIdx + accountsPerThread;
                work.numTxnsToSubmit = baseWorkload + (t < remainder ? 1 : 0);

                totalAccountsAssigned = work.endAccountIdx;
                threadAssignments.push_back(work);
            }

            // Launch threads with work assignments
            std::vector<std::thread> threads;
            threads.reserve(THREAD_COUNT);

            for (std::size_t t = 0; t < THREAD_COUNT; ++t)
            {
                threads.emplace_back(
                    [&env, &accounts, &work = threadAssignments[t], this]() {
                        submitBatchThread(env, accounts, work);
                    });
            }

            // Wait for all threads
            for (auto& thread : threads)
            {
                if (thread.joinable())
                    thread.join();
            }

            // Aggregate metrics from all threads
            ledgerMetrics.successfulTxCount = 0;
            ledgerMetrics.failedTxCount = 0;
            for (auto const& work : threadAssignments)
            {
                ledgerMetrics.successfulTxCount += work.successfulTxns;
                ledgerMetrics.failedTxCount += work.failedTxns;
            }

            ledgerMetrics.submitTime =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - submitStart);

            auto closeStart = std::chrono::steady_clock::now();
            env.close();
            ledgerMetrics.closeTime =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - closeStart);

            auto const closed = env.closed();
            ledgerMetrics.txCount = closed->txCount();

            ledgerMetrics.log(journal);
            metrics.push_back(ledgerMetrics);

            auto const totalTime =
                ledgerMetrics.submitTime + ledgerMetrics.closeTime;

            // Updated expectations
            BEAST_EXPECT(
                ledgerMetrics.txCount >=
                ledgerMetrics.successfulTxCount * 0.95);  // Allow 5% variance
            BEAST_EXPECT(
                totalTime <=
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    MAX_CLOSE_TIME));

            threadSafeLog(
                "Completed ledger " + std::to_string(ledger) + " in " +
                std::to_string(totalTime.count()) + "ms" + " with " +
                std::to_string(ledgerMetrics.successfulTxCount) +
                " successful transactions using " +
                std::to_string(THREAD_COUNT) + " threads");
        }
    }

public:
    void
    run() override
    {
        runStressTest(5);
    }
};

BEAST_DEFINE_TESTSUITE(LedgerStress, app, ripple);

}  // namespace test
}  // namespace ripple
