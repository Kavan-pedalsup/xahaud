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
    static STAmount
    lineBalance(
        jtx::Env const& env,
        jtx::Account const& account,
        jtx::Account const& gw,
        jtx::IOU const& iou)
    {
        auto const sle = env.le(keylet::line(account, gw, iou.currency));
        if (sle && sle->isFieldPresent(sfBalance))
            return (*sle)[sfBalance];
        return STAmount(iou, 0);
    }

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
    testInvalid(FeatureBitset features)
    {
        testcase("invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        // malformed inner object. No Amount // TEMPLATE ERROR
        // malformed inner object. No Destination // TEMPLATE ERROR
        // skipping self service-fee
        {
            Env env{*this, features};
            auto const baseFee = env.current()->fees().base;
            env.fund(XRP(1000), alice, bob);
            env.close();

            auto const preAlice = env.balance(alice);
            auto const amt = XRP(10);
            auto const sfeeAmt = XRP(1);
            env(pay(alice, bob, amt), sfee(sfeeAmt, alice));
            env.close();
            BEAST_EXPECT(env.balance(alice) == preAlice - amt - baseFee);
        }
        // skipping non-positive service-fee
        {
            Env env{*this, features};
            auto const baseFee = env.current()->fees().base;
            env.fund(XRP(1000), alice, bob);
            env.close();

            auto const preAlice = env.balance(alice);
            auto const amt = XRP(10);
            auto const sfeeAmt = XRP(-1);
            env(pay(alice, bob, amt), sfee(sfeeAmt, alice));
            env.close();
            BEAST_EXPECT(env.balance(alice) == preAlice - amt - baseFee);
        }
        // source does not exist.
        {
            // TODO
        }  // destination does not exist.
        {
            Env env{*this, features};
            auto const baseFee = env.current()->fees().base;
            env.fund(XRP(1000), alice, bob);
            env.close();

            auto const preAlice = env.balance(alice);
            auto const amt = XRP(10);
            auto const sfeeAmt = XRP(1);
            env(pay(alice, bob, amt), sfee(sfeeAmt, carol));
            env.close();
            BEAST_EXPECT(env.balance(alice) == preAlice - amt - baseFee);
        }
        // insufficient reserve
        {
            // TODO
        }  // no trustline (source)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env.close();
            env.trust(USD(100000), carol);
            env.close();
            env(pay(gw, carol, USD(10000)));
            env.close();

            auto const preAlice = env.balance(alice, USD);
            auto const preCarol = env.balance(carol, USD);
            BEAST_EXPECT(preAlice == USD(0));

            auto const sfeeAmt = USD(1);
            env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == preAlice);
            BEAST_EXPECT(env.balance(carol, USD) == preCarol);
        }
        // insufficient trustline balance (source)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env.close();
            env.trust(USD(10), alice, carol);
            env.close();
            env(pay(gw, alice, USD(1)));
            env(pay(gw, carol, USD(1)));
            env.close();

            auto const preAlice = env.balance(alice, USD);
            auto const preCarol = env.balance(carol, USD);
            auto const sfeeAmt = USD(10);
            env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == preAlice);
            BEAST_EXPECT(env.balance(carol, USD) == preCarol);
        }
        // no trustline (destination)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            auto const preAlice = env.balance(alice, USD);
            auto const preCarol = env.balance(carol, USD);
            auto const sfeeAmt = USD(1);
            env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == preAlice);
            BEAST_EXPECT(env.balance(carol, USD) == preCarol);
        }
        // insufficient trustline limit (destination)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env.close();
            env.trust(USD(10), alice);
            env.trust(USD(1), carol);
            env.close();
            env(pay(gw, alice, USD(10)));
            env.close();

            auto const preAlice = env.balance(alice, USD);
            auto const preCarol = env.balance(carol, USD);
            auto const sfeeAmt = USD(10);
            env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD) == preAlice);
            BEAST_EXPECT(env.balance(carol, USD) == preCarol);
        }
        // accountSend() failed // INTERNAL ERROR
    }

    void
    testRippleState(FeatureBitset features)
    {
        testcase("ripple_state");
        using namespace test::jtx;
        using namespace std::literals;

        struct TestAccountData
        {
            Account src;
            Account dst;
            Account gw;
            bool hasTrustline;
            bool negative;
        };

        std::array<TestAccountData, 8> tests = {{
            // src > dst && src > issuer && dst no trustline
            {Account("alice2"), Account("bob0"), Account{"gw0"}, false, true},
            // src < dst && src < issuer && dst no trustline
            {Account("carol0"), Account("dan1"), Account{"gw1"}, false, false},
            // dst > src && dst > issuer && dst no trustline
            {Account("dan1"), Account("alice2"), Account{"gw0"}, false, true},
            // dst < src && dst < issuer && dst no trustline
            {Account("bob0"), Account("carol0"), Account{"gw1"}, false, false},
            // src > dst && src > issuer && dst has trustline
            {Account("alice2"), Account("bob0"), Account{"gw0"}, true, true},
            // src < dst && src < issuer && dst has trustline
            {Account("carol0"), Account("dan1"), Account{"gw1"}, true, false},
            // dst > src && dst > issuer && dst has trustline
            {Account("dan1"), Account("alice2"), Account{"gw0"}, true, true},
            // dst < src && dst < issuer && dst has trustline
            {Account("bob0"), Account("carol0"), Account{"gw1"}, true, false},
        }};

        for (auto const& t : tests)
        {
            Env env{*this, features};
            auto const carol = Account("carol");
            auto const USD = t.gw["USD"];
            env.fund(XRP(5000), t.src, t.dst, t.gw, carol);
            env.close();
            if (t.hasTrustline)
                env.trust(USD(100000), t.src, t.dst, carol);
            else
                env.trust(USD(100000), t.src, t.dst);
            env.close();

            env(pay(t.gw, t.src, USD(10000)));
            env(pay(t.gw, t.dst, USD(10000)));
            if (t.hasTrustline)
                env(pay(t.gw, carol, USD(10000)));
            env.close();

            auto const delta = USD(100);
            auto const sfeeAmt = USD(1);
            auto const preSrc = lineBalance(env, t.src, t.gw, USD);
            auto const preDst = lineBalance(env, t.dst, t.gw, USD);
            auto const preCarol = lineBalance(env, carol, t.gw, USD);
            env(pay(t.src, t.dst, delta), sfee(sfeeAmt, carol));
            env.close();

            auto const appliedSFee = t.hasTrustline ? sfeeAmt : USD(0);
            auto const postSrc = t.negative ? (preSrc + delta + appliedSFee)
                                            : (preSrc - delta - appliedSFee);
            auto const postDst =
                t.negative ? (preDst - delta) : (preDst + delta);
            auto const postCarol = t.negative ? (preCarol - appliedSFee)
                                              : (preCarol + appliedSFee);
            BEAST_EXPECT(lineBalance(env, t.src, t.gw, USD) == postSrc);
            BEAST_EXPECT(lineBalance(env, t.dst, t.gw, USD) == postDst);
            BEAST_EXPECT(lineBalance(env, carol, t.gw, USD) == postCarol);
        }
    }

    void
    testGateway(FeatureBitset features)
    {
        testcase("gateway");
        using namespace test::jtx;
        using namespace std::literals;

        struct TestAccountData
        {
            Account acct;
            Account gw;
            bool hasTrustline;
            bool negative;
        };

        std::array<TestAccountData, 4> tests = {{
            // acct no trustline
            // acct > issuer
            {Account("alice2"), Account{"gw0"}, false, true},
            // acct < issuer
            {Account("carol0"), Account{"gw1"}, false, false},

            // acct has trustline
            // acct > issuer
            {Account("alice2"), Account{"gw0"}, true, true},
            // acct < issuer
            {Account("carol0"), Account{"gw1"}, true, false},
        }};

        // test gateway is source
        for (auto const& t : tests)
        {
            Env env{*this, features};
            auto const carol = Account("carol");
            auto const USD = t.gw["USD"];
            env.fund(XRP(5000), t.acct, t.gw, carol);
            env.trust(USD(100000), carol);
            env(pay(t.gw, carol, USD(10000)));
            env.close();

            if (t.hasTrustline)
            {
                env.trust(USD(100000), t.acct);
                env(pay(t.gw, t.acct, USD(10000)));
                env.close();
            }

            auto const preAcct = lineBalance(env, t.acct, t.gw, USD);
            auto const preCarol = lineBalance(env, carol, t.gw, USD);

            auto const delta = USD(100);
            auto const sfeeAmt = USD(1);
            env(pay(t.gw, carol, delta), sfee(sfeeAmt, t.acct));
            env.close();

            auto const appliedSFee = t.hasTrustline ? sfeeAmt : USD(0);
            // Receiver of service fee
            auto const postAcct =
                t.negative ? (preAcct - appliedSFee) : (preAcct + appliedSFee);
            // Receiver of payment
            auto const postCarol =
                t.negative ? (preCarol - delta) : (preCarol + delta);
            BEAST_EXPECT(lineBalance(env, t.acct, t.gw, USD) == postAcct);
            BEAST_EXPECT(lineBalance(env, carol, t.gw, USD) == postCarol);
            BEAST_EXPECT(lineBalance(env, t.gw, t.acct, USD) == postAcct);
            BEAST_EXPECT(lineBalance(env, t.gw, carol, USD) == postCarol);
        }

        // test gateway is destination
        for (auto const& t : tests)
        {
            Env env{*this, features};
            auto const USD = t.gw["USD"];
            env.fund(XRP(5000), t.acct, t.gw);
            env.trust(USD(100000), t.acct);
            env(pay(t.gw, t.acct, USD(10000)));
            env.close();

            auto const preAcct = lineBalance(env, t.acct, t.gw, USD);
            auto const delta = USD(100);
            auto const sfeeAmt = USD(1);
            env(pay(t.acct, t.gw, delta), sfee(sfeeAmt, t.gw));
            env.close();

            // Sender of Payment & Fee
            auto const postAcct = t.negative ? (preAcct + delta + sfeeAmt)
                                             : (preAcct - delta - sfeeAmt);
            BEAST_EXPECT(lineBalance(env, t.acct, t.gw, USD) == postAcct);
            BEAST_EXPECT(lineBalance(env, t.gw, t.acct, USD) == postAcct);
        }
    }

    void
    testRequireAuth(FeatureBitset features)
    {
        testcase("require_auth");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];

        auto const aliceUSD = alice["USD"];
        auto const bobUSD = bob["USD"];
        auto const carolUSD = carol["USD"];

        // test asfRequireAuth
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env(fset(gw, asfRequireAuth));
            env.close();
            env(trust(gw, carolUSD(10000)), txflags(tfSetfAuth));
            env(trust(carol, USD(10000)));
            env(trust(gw, bobUSD(10000)), txflags(tfSetfAuth));
            env(trust(bob, USD(10000)));
            env.close();
            env(pay(gw, carol, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();

            {
                // Alice does not receive service fee because she is not
                // authorized
                auto const preAlice = env.balance(alice, USD);
                auto const sfeeAmt = USD(1);
                env(pay(bob, carol, XRP(100)), sfee(sfeeAmt, alice));
                env.close();

                BEAST_EXPECT(env.balance(alice, USD) == preAlice);
            }

            {
                env(trust(gw, aliceUSD(10000)), txflags(tfSetfAuth));
                env(trust(alice, USD(10000)));
                env.close();
                env(pay(gw, alice, USD(1000)));
                env.close();

                // Alice now receives service fee because she is authorized
                auto const preAlice = env.balance(alice, USD);
                auto const sfeeAmt = USD(1);
                env(pay(bob, carol, XRP(100)), sfee(sfeeAmt, alice));
                env.close();

                BEAST_EXPECT(env.balance(alice, USD) == preAlice + sfeeAmt);
            }
        }
    }

    void
    testFreeze(FeatureBitset features)
    {
        testcase("freeze");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];
        // test Global Freeze
        {
            // setup env
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env.close();
            env.trust(USD(100000), alice, bob, carol);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env(pay(gw, carol, USD(1000)));
            env.close();
            env(fset(gw, asfGlobalFreeze));
            env.close();

            {
                // carol cannot receive because of global freeze
                auto const preCarol = env.balance(carol, USD);
                auto const sfeeAmt = USD(1);
                env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
                env.close();
                BEAST_EXPECT(env.balance(carol, USD) == preCarol);
            }

            {
                // clear global freeze
                env(fclear(gw, asfGlobalFreeze));
                env.close();

                // carol can receive because global freeze is cleared
                auto const preCarol = env.balance(carol, USD);
                auto const sfeeAmt = USD(1);
                env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
                env.close();
                BEAST_EXPECT(env.balance(carol, USD) == preCarol + sfeeAmt);
            }
        }

        // test Individual Freeze
        {
            // Env Setup
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env.close();
            env.trust(USD(100000), alice, bob, carol);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env(pay(gw, carol, USD(1000)));
            env.close();

            // set freeze on carol trustline
            env(trust(gw, USD(10000), carol, tfSetFreeze));
            env.close();

            {
                auto const preCarol = env.balance(carol, USD);
                auto const sfeeAmt = USD(1);
                env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
                env.close();
                BEAST_EXPECT(env.balance(carol, USD) == preCarol);
            }

            {
                // clear freeze on carol trustline
                env(trust(gw, USD(10000), carol, tfClearFreeze));
                env.close();

                auto const preCarol = env.balance(carol, USD);
                auto const sfeeAmt = USD(1);
                env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
                env.close();
                BEAST_EXPECT(env.balance(carol, USD) == preCarol + sfeeAmt);
            }
        }
    }

    void
    testTransferRate(FeatureBitset features)
    {
        testcase("transfer_rate");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];

        // test rate
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice, bob, carol, gw);
            env(rate(gw, 1.25));
            env.close();
            env.trust(USD(100000), alice, carol);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, carol, USD(10000)));
            env.close();

            auto const preAlice = env.balance(alice, USD);
            auto const preCarol = env.balance(carol, USD);
            auto const sfeeAmt = USD(1);
            env(pay(alice, bob, XRP(100)), sfee(sfeeAmt, carol));
            env.close();
            BEAST_EXPECT(
                env.balance(alice, USD) == preAlice - sfeeAmt - USD(0.25));
            BEAST_EXPECT(env.balance(carol, USD) == preCarol + sfeeAmt);
        }

        // test issuer doesnt pay own rate
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice, carol, gw);
            env(rate(gw, 1.25));
            env.close();
            env.trust(USD(100000), carol);
            env.close();
            env(pay(gw, carol, USD(10000)));
            env.close();

            auto const preGwC = -lineBalance(env, carol, gw, USD);
            auto const preCarol = env.balance(carol, USD);
            auto const sfeeAmt = USD(1);
            env(pay(gw, alice, XRP(100)), sfee(sfeeAmt, carol));
            env.close();
            BEAST_EXPECT(-lineBalance(env, carol, gw, USD) == preGwC + sfeeAmt);
            BEAST_EXPECT(env.balance(carol, USD) == preCarol + sfeeAmt);
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testInvalid(features);
        testRippleState(features);
        testGateway(features);
        testRequireAuth(features);
        testFreeze(features);
        testTransferRate(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testEnabled(sa);
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(ServiceFee, app, ripple);
}  // namespace test
}  // namespace ripple
