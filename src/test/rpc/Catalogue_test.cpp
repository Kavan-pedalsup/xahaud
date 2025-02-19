//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2017 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/beast/unit_test.h>
#include <ripple/protocol/jss.h>
#include <boost/filesystem.hpp>
#include <fstream>
#include <test/jtx.h>

namespace ripple {

class Catalogue_test : public beast::unit_test::suite
{
    // Helper to create test ledger data with complex state changes
    void
    prepareLedgerData(test::jtx::Env& env, int numLedgers)
    {
        using namespace test::jtx;
        Account alice{"alice"};
        Account bob{"bob"};
        Account charlie{"charlie"};

        env.fund(XRP(10000), alice, bob, charlie);
        env.close();

        // Set up trust lines and issue currency
        env(trust(bob, alice["USD"](1000)));
        env(trust(charlie, bob["EUR"](1000)));
        env.close();

        env(pay(alice, bob, alice["USD"](500)));
        env.close();

        // Create and remove an offer to test state deletion
        env(offer(bob, XRP(50), alice["USD"](1)));
        auto offerSeq =
            env.seq(bob) - 1;  // Get the sequence of the offer we just created
        env.close();

        // Cancel the offer
        env(offer_cancel(bob, offerSeq));
        env.close();

        // Create another offer with same account
        env(offer(bob, XRP(60), alice["USD"](2)));
        env.close();

        // Create a trust line and then remove it
        env(trust(charlie, bob["EUR"](1000)));
        env.close();
        env(trust(charlie, bob["EUR"](0)));
        env.close();

        // Recreate the same trust line
        env(trust(charlie, bob["EUR"](2000)));
        env.close();

        // Additional ledgers with various transactions
        for (int i = 0; i < numLedgers; ++i)
        {
            env(pay(alice, bob, XRP(100)));
            env(offer(bob, XRP(50), alice["USD"](1)));
            env.close();
        }
    }

    void
    testCatalogueCreateBadInput()
    {
        testcase("catalogue_create: Invalid parameters");
        using namespace test::jtx;
        Env env{*this};

        // No parameters
        {
            auto const result =
                env.client().invoke("catalogue_create", {})[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing min_ledger
        {
            Json::Value params{Json::objectValue};
            params[jss::max_ledger] = 20;
            params[jss::output_file] = "/tmp/test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing max_ledger
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 10;
            params[jss::output_file] = "/tmp/test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing output_file
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 10;
            params[jss::max_ledger] = 20;
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Invalid output path (not absolute)
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 10;
            params[jss::max_ledger] = 20;
            params[jss::output_file] = "test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // min_ledger > max_ledger
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 20;
            params[jss::max_ledger] = 10;
            params[jss::output_file] = "/tmp/test.catl";
            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }
    }

    void
    testCatalogueCreate()
    {
        testcase("catalogue_create: Basic functionality");
        using namespace test::jtx;

        // Create environment and some test ledgers
        Env env{*this};
        prepareLedgerData(env, 5);

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // Create catalogue
        Json::Value params{Json::objectValue};
        params[jss::min_ledger] = 3;
        params[jss::max_ledger] = 5;
        params[jss::output_file] = cataloguePath;

        auto const result =
            env.client().invoke("catalogue_create", params)[jss::result];

        BEAST_EXPECT(result[jss::status] == jss::success);
        BEAST_EXPECT(result[jss::min_ledger] == 3);
        BEAST_EXPECT(result[jss::max_ledger] == 5);
        BEAST_EXPECT(result[jss::output_file] == cataloguePath);
        BEAST_EXPECT(result[jss::bytes_written].asUInt() > 0);

        // Verify file exists and is not empty
        BEAST_EXPECT(boost::filesystem::exists(cataloguePath));
        BEAST_EXPECT(boost::filesystem::file_size(cataloguePath) > 0);

        boost::filesystem::remove_all(tempDir);
    }

    void
    testCatalogueLoadBadInput()
    {
        testcase("catalogue_load: Invalid parameters");
        using namespace test::jtx;
        Env env{*this};

        // No parameters
        {
            auto const result =
                env.client().invoke("catalogue_load", {})[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Missing input_file
        {
            Json::Value params{Json::objectValue};
            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Invalid input path (not absolute)
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = "test.catl";
            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        // Non-existent file
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = "/tmp/nonexistent.catl";
            auto const result =
                env.client().invoke("catalogue_load", params)[jss::result];
            BEAST_EXPECT(result[jss::error] == "internal");
            BEAST_EXPECT(result[jss::status] == "error");
        }
    }

    void
    testCatalogueLoadAndVerify()
    {
        testcase("catalogue_load: Load and verify");
        using namespace test::jtx;

        // Create environment and test data
        Env env{*this};
        prepareLedgerData(env, 5);

        // Store some key state information before catalogue creation
        auto const sourceLedger = env.closed();
        auto const bobKeylet = keylet::account(Account("bob").id());
        auto const charlieKeylet = keylet::account(Account("charlie").id());
        auto const eurTrustKeylet = keylet::line(
            Account("charlie").id(),
            Account("bob").id(),
            Currency(to_currency("EUR")));

        // Get original state entries
        auto const bobAcct = sourceLedger->read(bobKeylet);
        auto const charlieAcct = sourceLedger->read(charlieKeylet);
        auto const eurTrust = sourceLedger->read(eurTrustKeylet);

        BEAST_EXPECT(bobAcct != nullptr);
        BEAST_EXPECT(charlieAcct != nullptr);
        BEAST_EXPECT(eurTrust != nullptr);
        BEAST_EXPECT(
            eurTrust->getFieldAmount(sfLowLimit).mantissa() ==
            2000000000000000ULL);

        // Get initial complete_ledgers range
        auto const originalCompleteLedgers =
            env.app().getLedgerMaster().getCompleteLedgers();

        std::cout << "orgCompleteLedgers: " << originalCompleteLedgers << "\n";

        // Create temporary directory for test files
        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // First create a catalogue
        uint32_t minLedger = 3;
        uint32_t maxLedger = sourceLedger->info().seq;
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = minLedger;
            params[jss::max_ledger] = maxLedger;
            params[jss::output_file] = cataloguePath;

            auto const result =
                env.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == jss::success);
        }

        // Create a new environment for loading with unique port
        Env loadEnv{*this, test::jtx::envconfig(test::jtx::port_increment, 3)};

        // Now load the catalogue
        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = cataloguePath;

            auto const result =
                loadEnv.client().invoke("catalogue_load", params)[jss::result];

            BEAST_EXPECT(result[jss::status] == jss::success);
            BEAST_EXPECT(result[jss::ledger_min] == minLedger);
            BEAST_EXPECT(result[jss::ledger_max] == maxLedger);
            BEAST_EXPECT(
                result[jss::ledger_count] == (maxLedger - minLedger + 1));

            // Verify complete_ledgers reflects loaded ledgers
            auto const newCompleteLedgers =
                loadEnv.app().getLedgerMaster().getCompleteLedgers();

            std::cout << "newCompleteLedgers: " << newCompleteLedgers << "\n";

            BEAST_EXPECT(newCompleteLedgers == originalCompleteLedgers);

            // Verify the loaded state matches the original
            auto const loadedLedger = loadEnv.closed();

            // After loading each ledger
            std::cout << "Original ledger hash: "
                      << to_string(sourceLedger->info().hash)
                      << "\nLoaded ledger hash: "
                      << to_string(loadedLedger->info().hash)
                      << "Original ledger seq: "
                      << to_string(sourceLedger->info().seq)
                      << "\nLoaded ledger seq: "
                      << to_string(loadedLedger->info().seq) << std::endl;

            auto const loadedBobAcct = loadedLedger->read(bobKeylet);
            auto const loadedCharlieAcct = loadedLedger->read(charlieKeylet);
            auto const loadedEurTrust = loadedLedger->read(eurTrustKeylet);

            BEAST_EXPECT(!!loadedBobAcct);
            BEAST_EXPECT(!!loadedCharlieAcct);
            BEAST_EXPECT(!!loadedEurTrust);

            // Compare the serialized forms of the state objects
            bool const loaded =
                loadedBobAcct && loadedCharlieAcct && loadedEurTrust;

            Serializer s1, s2;
            if (loaded)
            {
                bobAcct->add(s1);
                loadedBobAcct->add(s2);
            }
            BEAST_EXPECT(loaded && s1.peekData() == s2.peekData());

            if (loaded)
            {
                s1.erase();
                s2.erase();
                charlieAcct->add(s1);
                loadedCharlieAcct->add(s2);
            }
            BEAST_EXPECT(loaded && s1.peekData() == s2.peekData());

            if (loaded)
            {
                s1.erase();
                s2.erase();
                eurTrust->add(s1);
                loadedEurTrust->add(s2);
            }

            BEAST_EXPECT(loaded && s1.peekData() == s2.peekData());

            // Verify trust line amount matches
            BEAST_EXPECT(
                loaded &&
                loadedEurTrust->getFieldAmount(sfLowLimit).mantissa() ==
                    2000000000);
        }

        boost::filesystem::remove_all(tempDir);
    }

    void
    testNetworkMismatch()
    {
        testcase("catalogue_load: Network ID mismatch");
        using namespace test::jtx;

        // Create environment with different network IDs
        Env env1{*this, envconfig([](std::unique_ptr<Config> cfg) {
                     cfg->NETWORK_ID = 123;
                     return cfg;
                 })};
        prepareLedgerData(env1, 5);

        boost::filesystem::path tempDir =
            boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path();
        boost::filesystem::create_directories(tempDir);

        auto cataloguePath = (tempDir / "test.catl").string();

        // Create catalogue with network ID 123
        {
            Json::Value params{Json::objectValue};
            params[jss::min_ledger] = 3;
            params[jss::max_ledger] = 5;
            params[jss::output_file] = cataloguePath;

            auto const result =
                env1.client().invoke("catalogue_create", params)[jss::result];
            BEAST_EXPECT(result[jss::status] == jss::success);
        }

        // Try to load catalogue in environment with different network ID
        Env env2{*this, envconfig([](std::unique_ptr<Config> cfg) {
                     cfg->NETWORK_ID = 456;
                     return cfg;
                 })};

        {
            Json::Value params{Json::objectValue};
            params[jss::input_file] = cataloguePath;

            auto const result =
                env2.client().invoke("catalogue_load", params)[jss::result];

            BEAST_EXPECT(result[jss::error] == "invalidParams");
            BEAST_EXPECT(result[jss::status] == "error");
        }

        boost::filesystem::remove_all(tempDir);
    }

public:
    void
    run() override
    {
        testCatalogueCreateBadInput();
        testCatalogueCreate();
        testCatalogueLoadBadInput();
        testCatalogueLoadAndVerify();
        testNetworkMismatch();
    }
};

BEAST_DEFINE_TESTSUITE(Catalogue, rpc, ripple);

}  // namespace ripple
