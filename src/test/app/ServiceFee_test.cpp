//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL-Labs

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

#include <ripple/basics/chrono.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

#include <chrono>

namespace ripple {
namespace test {
struct ServiceFee_test : public beast::unit_test::suite
{
    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        for (bool const withSFee : {true, false})
        {
            auto const amend =
                withSFee ? features : features - featureServiceFee;
            Env env{*this, amend};
            auto const feeDrops = env.current()->fees().base;
            env.fund(XRP(1000), alice, bob, carol);

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);
            auto const preCarol = env.balance(carol);

            env(pay(alice, bob, XRP(10)),
                fee(feeDrops),
                sfee(XRP(1), carol),
                ter(tesSUCCESS));
            env.close();

            auto const postAlice = withSFee
                ? preAlice - feeDrops - XRP(10) - XRP(1)
                : preAlice - feeDrops - XRP(10);
            auto const postBob = preBob + XRP(10);
            auto const postCarol = withSFee ? preCarol + XRP(1) : preCarol;
            BEAST_EXPECT(env.balance(alice) == postAlice);
            BEAST_EXPECT(env.balance(bob) == postBob);
            BEAST_EXPECT(env.balance(carol) == postCarol);
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testEnabled();
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testEnabled(sa);
        // testWithFeats(sa - featureServiceFee);
        // testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(ServiceFee, app, ripple);
}  // namespace test
}  // namespace ripple
