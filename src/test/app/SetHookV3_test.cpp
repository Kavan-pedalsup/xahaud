//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2016 Ripple Labs Inc.

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
#include <ripple/app/hook/Enum.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/tx/impl/SetHook.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/app/SetHook_wasm.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <unordered_map>
#include <iostream>
#include <ripple/json/json_reader.h>

namespace ripple {

namespace test {

class SetHookV3_test : public beast::unit_test::suite
{
private:
    // helper
    void static overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }

public:
// This is a large fee, large enough that we can set most small test hooks
// without running into fee issues we only want to test fee code specifically in
// fee unit tests, the rest of the time we want to ignore it.
#define HSFEE fee(100'000'000)
#define M(m) memo(m, "", "")
    std::string
    loadHook()
    {
        std::string name = "/Users/darkmatter/projects/ledger-works/xahaud/src/test/app/wasm/custom.wasm";
        if (!std::filesystem::exists(name)) {
            std::cout << "File does not exist: " << name << "\n";
            return "";
        }

        std::ifstream hookFile(name, std::ios::binary);

        if (!hookFile)
        {
            std::cout << "Failed to open file: " << name << "\n";
            return "";
        }

        // Read the file into a vector
        std::vector<char> buffer((std::istreambuf_iterator<char>(hookFile)), std::istreambuf_iterator<char>());
        
        // Check if the buffer is empty
        if (buffer.empty()) {
            std::cout << "File is empty or could not be read properly.\n";
            return "";
        }

        return strHex(buffer);
    }

    void
    testSimple(FeatureBitset features)
    {
        testcase("Test simple");

        using namespace jtx;

        // Env env{*this, features};
        Env env{*this, envconfig(), features, nullptr,
            beast::severities::kTrace
        };

        auto const alice = Account{"alice"};
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();
        env.trust(USD(100000), alice);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env.close();

        std::string hook = loadHook();
        std::cout << "Hook: " << hook << "\n";

        // install the hook on alice
        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set simple"),
            HSFEE);
        env.close();

        // // invoke the hook
        // Json::Value jv = invoke::invoke(alice);
        // Json::Value params{Json::arrayValue};
        // Json::Value pv;
        // Json::Value piv;
        // piv[jss::HookParameterName] = "736F6D6566756E63";
        // piv[jss::HookParameterValue] = "736F6D6566756E63";
        // pv[jss::HookParameter] = piv;
        // params[0u] = pv;
        // jv[jss::HookParameters] = params;
        env(invoke::invoke(alice), M("test simple"), fee(XRP(1)));
    }

    void
    testWithFeatures(FeatureBitset features)
    {
        testSimple(features);
    }

    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeatures(sa);
    }
};
BEAST_DEFINE_TESTSUITE(SetHookV3, app, ripple);
}  // namespace test
}  // namespace ripple
#undef M
