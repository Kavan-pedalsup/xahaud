//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#ifndef RIPPLE_TX_URITOKENUTILS_H_INCLUDED
#define RIPPLE_TX_URITOKENUTILS_H_INCLUDED

#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/Sandbox.h>
#include <ripple/protocol/TER.h>

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
    beast::Journal journal);    

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
    beast::Journal journal);

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
    beast::Journal journal);

}  // namespace uritoken
}  // namespace ripple

#endif  // RIPPLE_TX_URITOKENUTILS_H_INCLUDED