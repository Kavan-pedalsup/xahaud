//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/LedgerFormats.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/Role.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/Tuning.h>

namespace ripple {

#pragma pack(push, 1)  // pack the struct tightly
struct CATLHeader
{
    char magic[4] = {'C', 'A', 'T', 'L'};
    uint32_t version;
    uint32_t min_ledger;
    uint32_t max_ledger;
    uint32_t network_id;
    uint32_t ledger_tx_offset;
};
#pragma pack(pop)

class StreamingLedgerIterator
{
private:
    struct LedgerEntry
    {
        decltype(std::declval<ReadView>().sles.begin()) iter;
        decltype(std::declval<ReadView>().sles.end()) end;
        LedgerIndex seq;

        bool
        operator<(const LedgerEntry& other) const
        {
            return (*iter)->key() < (*other.iter)->key();
        }
    };

    std::vector<LedgerEntry> ledgerIters;
    std::ofstream& outfile;

    // 4-byte flags/size field layout
    static constexpr uint32_t HAS_NEXT_FLAG =
        0x80000000;  // Top bit indicates more revisions
    static constexpr uint32_t SIZE_MASK =
        0x0FFFFFFF;  // Bottom 28 bits for size

    void
    writeKeyEntry(const ReadView::key_type& key)
    {
        // Write the key
        outfile.write(key.cdata(), 32);

        std::vector<std::pair<LedgerIndex, std::shared_ptr<SLE const>>>
            versions;

        auto currentKey = key;
        auto it = ledgerIters.begin();

        while (it != ledgerIters.end() && (*it->iter)->key() == currentKey)
        {
            versions.emplace_back(it->seq, *it->iter);
            ++it->iter;
            if (it->iter == it->end)
            {
                ledgerIters.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Sort versions by ledger sequence
        std::sort(
            versions.begin(), versions.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });

        // Keep track of last written version position for flag updates
        std::streampos lastFlagsPos;
        Serializer lastData;

        for (size_t i = 0; i < versions.size(); ++i)
        {
            auto& [seq, sle] = versions[i];

            // Serialize current version
            Serializer currentData;
            sle->add(currentData);

            // Skip if identical to last version
            if (i > 0 && currentData.peekData() == lastData.peekData())
            {
                continue;
            }

            auto data = currentData.peekData();
            uint32_t flagsAndSize = data.size() & SIZE_MASK;

            // Always set next flag - we'll clear it later if this is the last
            // version
            flagsAndSize |= HAS_NEXT_FLAG;

            // Remember position of flags so we can backtrack if needed
            outfile.write(reinterpret_cast<const char*>(&seq), 4);
            lastFlagsPos = outfile.tellp();
            outfile.write(reinterpret_cast<const char*>(&flagsAndSize), 4);
            outfile.write(
                reinterpret_cast<const char*>(data.data()), data.size());

            lastData = std::move(currentData);
        }

        // Clear the "has next" flag on the last written version
        if (lastFlagsPos)
        {  // If we wrote any versions at all
            auto currentPos = outfile.tellp();
            outfile.seekp(lastFlagsPos);
            uint32_t finalFlags =
                SIZE_MASK & lastData.peekData().size();  // Clear HAS_NEXT_FLAG
            outfile.write(reinterpret_cast<const char*>(&finalFlags), 4);
            outfile.seekp(currentPos);  // Restore position
        }
    }

public:
    StreamingLedgerIterator(
        const std::vector<std::shared_ptr<ReadView const>>& ledgers,
        LedgerIndex minSeq,
        std::ofstream& out)
        : outfile(out)
    {
        // Setup iterators for all ledgers
        ledgerIters.reserve(ledgers.size());
        for (int i = 0; i < ledgers.size(); ++i)
        {
            auto begin = ledgers[i]->sles.begin();
            auto end = ledgers[i]->sles.end();
            if (begin != end)
            {
                ledgerIters.push_back({begin, end, minSeq + i});
            }
        }
        std::sort(ledgerIters.begin(), ledgerIters.end());
    }

    void
    streamAll()
    {
        while (!ledgerIters.empty())
        {
            // Process the smallest key currently available
            auto currentKey = (*ledgerIters.front().iter)->key();
            writeKeyEntry(currentKey);

            // Resort remaining iterators if any
            if (!ledgerIters.empty())
            {
                std::sort(ledgerIters.begin(), ledgerIters.end());
            }
        }
    }
};

Json::Value
doCatalogueCreate(RPC::JsonContext& context)
{
    if (!context.params.isMember(jss::min_ledger) ||
        !context.params.isMember(jss::max_ledger))
        return rpcError(
            rpcINVALID_PARAMS, "expected min_ledger and max_ledger");

    std::string filepath;

    if (!context.params.isMember(jss::output_file) ||
        (filepath = context.params[jss::output_file].asString()).empty() ||
        filepath.front() != '/')
        return rpcError(
            rpcINVALID_PARAMS,
            "expected output_file: <absolute writeable filepath>");

    // check output file isn't already populated and can be written to
    {
        struct stat st;
        if (stat(filepath.c_str(), &st) == 0)
        {  // file exists
            if (st.st_size > 0)
            {
                return rpcError(
                    rpcINVALID_PARAMS,
                    "output_file already exists and is non-empty");
            }
        }
        else if (errno != ENOENT)
            return rpcError(
                rpcINTERNAL,
                "cannot stat output_file: " + std::string(strerror(errno)));

        if (std::ofstream(filepath.c_str(), std::ios::out).fail())
            return rpcError(
                rpcINTERNAL,
                "output_file location is not writeable: " +
                    std::string(strerror(errno)));
    }

    std::ofstream outfile(filepath.c_str(), std::ios::out | std::ios::binary);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to open output_file: " + std::string(strerror(errno)));

    uint32_t min_ledger = context.params[jss::min_ledger].asUInt();
    uint32_t max_ledger = context.params[jss::max_ledger].asUInt();

    if (min_ledger > max_ledger)
        return rpcError(rpcINVALID_PARAMS, "min_ledger must be <= max_ledger");

    std::vector<std::shared_ptr<ReadView const>> lpLedgers;

    lpLedgers.reserve(max_ledger - min_ledger);

    // grab all ledgers of interest

    for (auto i = min_ledger; i <= max_ledger; ++i)
    {
        std::shared_ptr<ReadView const> ptr;
        auto jvResult = RPC::lookupLedger(ptr, context);
        if (!ptr)
            return jvResult;
        lpLedgers[i - min_ledger] = std::move(ptr);
    }

    // execution to here means we'll output the catalogue file
    // we won't write the header yet, but we need to skip forward a header
    // length to start writing

    outfile.seekp(sizeof(CATLHeader), std::ios::beg);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to seek in output_file: " + std::string(strerror(errno)));

    StreamingLedgerIterator streamer(lpLedgers, min_ledger, outfile);
    streamer.streamAll();

    // After streaming is complete, write the header
    auto endPosition = outfile.tellp();  // Remember where we ended

    // Seek back to start
    outfile.seekp(0, std::ios::beg);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to seek to start of file: " + std::string(strerror(errno)));

    // Create and write header
    CATLHeader header;
    header.version = 1;  // or whatever version number you want
    header.min_ledger = min_ledger;
    header.max_ledger = max_ledger;
    header.network_id = context.app.config().NETWORK_ID;
    header.ledger_tx_offset = endPosition;  // store where the data ends

    outfile.write(reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to write header: " + std::string(strerror(errno)));

    outfile.seekp(endPosition);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to seek back to end: " + std::string(strerror(errno)));

    auto writeLedgerAndTransactions =
        [&outfile, &context](std::shared_ptr<ReadView const> const& ledger) {
            auto headerStart = outfile.tellp();

            uint64_t nextHeaderOffset = 0;
            outfile.write(
                reinterpret_cast<const char*>(&nextHeaderOffset),
                sizeof(nextHeaderOffset));

            auto const& info = ledger->info();

            outfile.write(
                reinterpret_cast<const char*>(&info.seq), sizeof(info.seq));
            outfile.write(
                reinterpret_cast<const char*>(&info.parentCloseTime),
                sizeof(info.parentCloseTime));
            outfile.write(info.hash.cdata(), 32);
            outfile.write(info.txHash.cdata(), 32);
            outfile.write(info.accountHash.cdata(), 32);
            outfile.write(info.parentHash.cdata(), 32);
            outfile.write(
                reinterpret_cast<const char*>(&info.drops), sizeof(info.drops));
            outfile.write(
                reinterpret_cast<const char*>(&info.validated),
                sizeof(info.validated));
            outfile.write(
                reinterpret_cast<const char*>(&info.accepted),
                sizeof(info.accepted));
            outfile.write(
                reinterpret_cast<const char*>(&info.closeFlags),
                sizeof(info.closeFlags));
            outfile.write(
                reinterpret_cast<const char*>(&info.closeTimeResolution),
                sizeof(info.closeTimeResolution));
            outfile.write(
                reinterpret_cast<const char*>(&info.closeTime),
                sizeof(info.closeTime));

            try
            {
                for (auto& i : ledger->txs)
                {
                    assert(i.first);
                    auto const& txnId = i.first->getTransactionID();
                    outfile.write(txnId.cdata(), 32);

                    Serializer sTxn = i.first->getSerializer();
                    uint32_t txnSize = sTxn.getLength();
                    outfile.write(
                        reinterpret_cast<const char*>(&txnSize),
                        sizeof(txnSize));
                    outfile.write(
                        reinterpret_cast<const char*>(sTxn.data()), txnSize);

                    uint32_t metaSize = 0;
                    if (i.second)
                    {
                        Serializer sMeta = i.second->getSerializer();
                        metaSize = sMeta.getLength();
                        outfile.write(
                            reinterpret_cast<const char*>(&metaSize),
                            sizeof(metaSize));
                        outfile.write(
                            reinterpret_cast<const char*>(sMeta.data()),
                            metaSize);
                    }
                    else
                    {
                        outfile.write(
                            reinterpret_cast<const char*>(&metaSize),
                            sizeof(metaSize));
                    }
                }
            }
            catch (std::exception const& e)
            {
                JLOG(context.j.error())
                    << "Error serializing transaction in ledger " << info.seq;
            }

            auto currentPos = outfile.tellp();
            outfile.seekp(headerStart);
            nextHeaderOffset = currentPos;
            outfile.write(
                reinterpret_cast<const char*>(&nextHeaderOffset),
                sizeof(nextHeaderOffset));
            outfile.seekp(currentPos);
        };

    // Write all ledger headers and transactions
    for (auto const& ledger : lpLedgers)
    {
        writeLedgerAndTransactions(ledger);
    }

    std::streampos bytes_written = outfile.tellp();
    outfile.close();
    uint64_t size = static_cast<uint64_t>(bytes_written);

    Json::Value jvResult;
    jvResult[jss::min_ledger] = min_ledger;
    jvResult[jss::max_ledger] = max_ledger;
    jvResult[jss::output_file] = filepath;
    jvResult[jss::bytes_written] = static_cast<Json::UInt>(size);
    jvResult[jss::status] = jss::success;

    return jvResult;
}

Json::Value
doCatalogueLoad(RPC::JsonContext& context)
{
    Json::Value jvResult;
    return jvResult;
}

}  // namespace ripple
