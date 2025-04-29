//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2017 Ripple Labs Inc.

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

#include <ripple/app/tx/impl/URITokenUtils.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/Quality.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {
namespace uritoken {

TER
transfer(
    Sandbox& sb,
    AccountID const& receiver,
    AccountID const& sender,
    Keylet const& kl,
    std::shared_ptr<SLE> const& sleU,
    std::shared_ptr<SLE> const& sleReciever,
    std::shared_ptr<SLE> const& sleSender,
    beast::Journal journal)
{
    // add token to new owner dir
    auto const newPage = sb.dirInsert(
        keylet::ownerDir(receiver), kl, describeOwnerDir(receiver));

    JLOG(journal.trace()) << "Adding URIToken to owner directory "
                            << to_string(kl.key) << ": "
                            << (newPage ? "success" : "failure");

    if (!newPage)
        return tecDIR_FULL;

    // remove from current owner directory
    if (!sb.dirRemove(
            keylet::ownerDir(sender),
            sleU->getFieldU64(sfOwnerNode),
            kl.key,
            true))
    {
        JLOG(journal.fatal())
            << "Could not remove URIToken from owner directory";

        return tefBAD_LEDGER;
    }

    // adjust owner counts
    adjustOwnerCount(sb, sleSender, -1, journal);
    adjustOwnerCount(sb, sleReciever, 1, journal);

    // clean the offer off the object
    sleU->makeFieldAbsent(sfAmount);
    if (sleU->isFieldPresent(sfDestination))
        sleU->makeFieldAbsent(sfDestination);

    // set the new owner of the object
    sleU->setAccountID(sfOwner, receiver);

    // tell the ledger where to find it
    sleU->setFieldU64(sfOwnerNode, *newPage);

    // check each side has sufficient balance remaining to cover the
    // updated ownercounts
    auto hasSufficientReserve = [&](std::shared_ptr<SLE> const& sle) -> bool {
        std::uint32_t const uOwnerCount = sle->getFieldU32(sfOwnerCount);
        return sle->getFieldAmount(sfBalance) >=
            sb.fees().accountReserve(uOwnerCount);
    };

    if (!hasSufficientReserve(sleReciever))
    {
        JLOG(journal.trace()) << "URIToken: buyer " << receiver
                                << " has insufficient reserve to buy";
        return tecINSUFFICIENT_RESERVE;
    }

    // This should only happen if the owner burned their reserves
    // below the needed amount via another transactor. If this
    // happens they should top up their account before selling!
    if (!hasSufficientReserve(sleSender))
    {
        JLOG(journal.warn()) << "URIToken: seller " << sender
                                << " has insufficient reserve to allow purchase!";
        return tecINSUF_RESERVE_SELLER;
    }

    sb.update(sleU);
    sb.update(sleReciever);
    sb.update(sleSender);
    return tesSUCCESS;
}

TER
fullSwap(
    Sandbox& sb,
    STAmount const& priorBalance,
    STAmount const& purchaseAmount,
    STAmount const& saleAmount,
    STAmount const& fee,
    AccountID const& receiver,
    AccountID const& sender,
    AccountID const& issuer,
    Keylet const& kl,
    std::shared_ptr<SLE> const& sleU,
    std::shared_ptr<SLE> const& sleReciever,
    std::shared_ptr<SLE> const& sleSender,
    beast::Journal journal)
{
    if (purchaseAmount < saleAmount)
        return tecINSUFFICIENT_PAYMENT;

    // if it's an xrp sale/purchase then no trustline needed
    if (purchaseAmount.native())
    {
        STAmount needed{sb.fees().accountReserve(
            sleReciever->getFieldU32(sfOwnerCount) + 1)};

        // STAmount const fee = ctx_.tx.getFieldAmount(sfFee).xrp();

        if (needed + fee < needed)
            return tecINTERNAL;

        needed += fee;

        if (needed + purchaseAmount < needed)
            return tecINTERNAL;

        needed += purchaseAmount;

        if (needed > priorBalance)
            return tecINSUFFICIENT_FUNDS;
    }
    else
    {
        // IOU sale
        if (TER result = trustTransferAllowed(
                sb, {receiver, sender}, purchaseAmount.issue(), journal);
            !isTesSuccess(result))
        {
            JLOG(journal.trace())
                << "URIToken::doApply trustTransferAllowed result=" << result;

            return result;
        }

        if (STAmount availableFunds{accountFunds(
                sb, receiver, purchaseAmount, fhZERO_IF_FROZEN, journal)};
            purchaseAmount > availableFunds)
            return tecINSUFFICIENT_FUNDS;
    }

    // execute the funds transfer, we'll check reserves last
    if (TER result =
            accountSend(sb, receiver, sender, purchaseAmount, journal, false);
        !isTesSuccess(result))
        return result;

    if (TER result = transfer(
            sb,
            receiver,
            sender,
            kl,
            sleU,
            sleReciever,
            sleSender,
            journal);
        !isTesSuccess(result))
    {
        JLOG(journal.fatal())
            << "URIToken: transfer failed: " << result;
        return result;
    }
    return tesSUCCESS;
}

TER
offerLock(
    Sandbox& sb,
    STAmount const& priorBalance,
    STAmount const& purchaseAmount,
    AccountID const& receiver,
    AccountID const& sender,
    AccountID const& issuer,
    Keylet const& kl,
    std::shared_ptr<SLE> const& sleU,
    std::shared_ptr<SLE> const& sleReciever,
    std::shared_ptr<SLE> const& sleSender,
    beast::Journal journal)
{
    STAmount deliverAmount = purchaseAmount;
    STAmount royaltyAmount = STAmount{0};

    // Amendment Guard
    if (sleU->isFieldPresent(sfRoyaltyRate))
    {
        auto const royaltyRate = sleU->getFieldU32(sfRoyaltyRate);
        royaltyAmount = multiplyRound(
            deliverAmount, Rate{royaltyRate}, deliverAmount.issue(), true);
        deliverAmount -= royaltyAmount;
        STAmount deliverAmount = purchaseAmount;
        static Rate const parityRate(QUALITY_ONE);
        auto const royaltyRate = [&]() -> Rate {
            if (sleU->isFieldPresent(sfRoyaltyRate))
                return Rate{sleU->getFieldU32(sfRoyaltyRate)};
            return Rate{QUALITY_ONE};
        }();

        if (royaltyRate != parityRate)
            deliverAmount = multiplyRound(
                deliverAmount, royaltyRate, deliverAmount.issue(), true);
        royaltyAmount = deliverAmount - purchaseAmount;
    }
    
    if (isXRP(purchaseAmount))
        (*sleSender)[sfBalance] = (*sleSender)[sfBalance] - purchaseAmount;
    else
    {
        // if (!sb.rules().enabled(featureRoyalties))
        //     return temDISABLED;

        TER const result = trustTransferAllowed(sb, {sender, receiver}, purchaseAmount.issue(), journal);
        if (!isTesSuccess(result))
            return result;

        auto sleLine = sb.peek(keylet::line(sender, purchaseAmount.getIssuer(), purchaseAmount.getCurrency()));
        if (issuer != sender)
        {
            if (!sleLine)
                return tecNO_LINE;

            TER const result = trustAdjustLockedBalance(sb, sleLine, purchaseAmount, 1, journal, WetRun);
            if (!isTesSuccess(result))
                return result;
        }
    }
    return tesSUCCESS;
}

}  // namespace uritoken
}  // namespace ripple
