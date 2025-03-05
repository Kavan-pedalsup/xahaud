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

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/Slice.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/LedgerFormats.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/Role.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/Tuning.h>
#include <ripple/shamap/SHAMapItem.h>

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ripple {

static constexpr uint16_t CATALOGUE_VERSION = 1;

#define CATL 0x4C544143UL /*"CATL" in LE*/

#pragma pack(push, 1)  // pack the struct tightly
struct CATLHeader
{
    uint32_t magic = 0x4C544143UL;  // "CATL" in LE
    uint32_t min_ledger;
    uint32_t max_ledger;
    uint16_t version;
    uint16_t network_id;
};
#pragma pack(pop)

// This class handles the processing of ledgers for the catalogue
class CatalogueProcessor
{
private:
    std::vector<std::shared_ptr<Ledger const>>& ledgers;
    std::ofstream& outfile;
    beast::Journal journal;
    std::atomic<bool> aborted{false};

    bool
    writeToFile(const void* data, size_t size)
    {
        outfile.write(reinterpret_cast<const char*>(data), size);
        if (outfile.fail())
        {
            JLOG(journal.error())
                << "Failed to write to output file: " << std::strerror(errno);
            aborted = true;
            return false;
        }
        return true;
    }

    bool
    outputLedger(
        Ledger const& ledger,
        std::optional<std::reference_wrapper<const SHAMap>> prevStateMap =
            std::nullopt)
    {
        uint32_t seq = ledger.info().seq;
        try
        {
            auto ledgerIndex = seq - ledgers.front()->info().seq;
            if (ledgerIndex >= ledgers.size())
                return false;

            auto ledger = ledgers[ledgerIndex];
            if (ledger->info().seq != seq)
            {
                JLOG(journal.error()) << "Ledger sequence mismatch: expected "
                                      << seq << ", got " << ledger->info().seq;
                return false;
            }

            auto const& info = ledger->info();

            // Write ledger header information
            if (!writeToFile(&info.seq, sizeof(info.seq)) ||
                !writeToFile(
                    &info.parentCloseTime, sizeof(info.parentCloseTime)) ||
                !writeToFile(info.hash.data(), 32) ||
                !writeToFile(info.txHash.data(), 32) ||
                !writeToFile(info.accountHash.data(), 32) ||
                !writeToFile(info.parentHash.data(), 32) ||
                !writeToFile(&info.drops, sizeof(info.drops)) ||
                !writeToFile(&info.validated, sizeof(info.validated)) ||
                !writeToFile(&info.accepted, sizeof(info.accepted)) ||
                !writeToFile(&info.closeFlags, sizeof(info.closeFlags)) ||
                !writeToFile(
                    &info.closeTimeResolution,
                    sizeof(info.closeTimeResolution)) ||
                !writeToFile(&info.closeTime, sizeof(info.closeTime)))
            {
                return false;
            }

            size_t stateNodesWritten =
                ledger->stateMap().serializeToStream(outfile, prevStateMap);

            size_t txNodesWritten = ledger->txMap().serializeToStream(outfile);

            JLOG(journal.info()) << "Ledger " << seq << ": Wrote "
                                 << stateNodesWritten << " state nodes, "
                                 << "and " << txNodesWritten << " tx nodes";

            return true;
        }
        catch (std::exception const& e)
        {
            JLOG(journal.error())
                << "Error processing ledger " << seq << ": " << e.what();
            aborted = true;
            return false;
        }
    }

public:
    CatalogueProcessor(
        std::vector<std::shared_ptr<Ledger const>>& ledgers_,
        std::ofstream& outfile_,
        beast::Journal journal_)
        : ledgers(ledgers_), outfile(outfile_), journal(journal_)
    {
        JLOG(journal.info()) << "Created CatalogueProcessor";
    }

    bool
    streamAll()
    {
        JLOG(journal.info())
            << "Starting to stream " << ledgers.size() << " ledgers";

        if (ledgers.empty())
        {
            JLOG(journal.warn()) << "No ledgers to process";
            return false;
        }

        // Process the first ledger completely
        if (!outputLedger(*ledgers.front()))
            return false;

        for (size_t i = 1; i < ledgers.size(); ++i)
        {
            auto ledger = ledgers[i];
            if (!outputLedger(*ledger, ledgers[i - 1]->stateMap()))
                return false;
        }

        return !aborted;
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

    // Check output file isn't already populated and can be written to
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

        std::ofstream testWrite(filepath.c_str(), std::ios::out);
        if (testWrite.fail())
            return rpcError(
                rpcINTERNAL,
                "output_file location is not writeable: " +
                    std::string(strerror(errno)));
        testWrite.close();
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

    std::vector<std::shared_ptr<Ledger const>> lpLedgers;

    // Grab all ledgers of interest
    lpLedgers.reserve(max_ledger - min_ledger + 1);

    for (auto i = min_ledger; i <= max_ledger; ++i)
    {
        std::shared_ptr<Ledger const> ptr;
        auto status = RPC::getLedger(ptr, i, context);
        if (status.toErrorCode() != rpcSUCCESS)  // Status isn't OK
            return rpcError(status);
        if (!ptr)
            return rpcError(rpcLEDGER_MISSING);
        lpLedgers.emplace_back(ptr);
    }

    // Create and write header
    CATLHeader header;

    header.min_ledger = min_ledger;
    header.max_ledger = max_ledger;
    header.version = CATALOGUE_VERSION;
    header.network_id = context.app.config().NETWORK_ID;

    outfile.write(reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to write header: " + std::string(strerror(errno)));

    // Process ledgers and write to file
    CatalogueProcessor processor(lpLedgers, outfile, context.j);

    // Stream all state data first
    if (!processor.streamAll())
    {
        return rpcError(rpcINTERNAL, "Error occurred while processing ledgers");
    }

    Json::Value jvResult;
    jvResult[jss::min_ledger] = min_ledger;
    jvResult[jss::max_ledger] = max_ledger;
    jvResult[jss::output_file] = filepath;
    //    jvResult[jss::bytes_written] = static_cast<Json::UInt>(size);
    jvResult[jss::status] = jss::success;

    return jvResult;
}

Json::Value
doCatalogueLoad(RPC::JsonContext& context)
{
    if (!context.params.isMember(jss::input_file))
        return rpcError(rpcINVALID_PARAMS, "expected input_file");

    std::string filepath = context.params[jss::input_file].asString();
    if (filepath.empty() || filepath.front() != '/')
        return rpcError(
            rpcINVALID_PARAMS,
            "expected input_file: <absolute readable filepath>");

    JLOG(context.j.info()) << "Opening catalogue file: " << filepath;

    // Check if file exists and is readable
    std::ifstream infile(filepath.c_str(), std::ios::in | std::ios::binary);
    if (infile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open input_file: " + std::string(strerror(errno)));

    JLOG(context.j.info()) << "Reading catalogue header...";

    // Read and validate header
    CATLHeader header;
    infile.read(reinterpret_cast<char*>(&header), sizeof(CATLHeader));
    if (infile.fail())
        return rpcError(rpcINTERNAL, "failed to read catalogue header");

    if (header.magic != CATL)
        return rpcError(rpcINVALID_PARAMS, "invalid catalogue file magic");

    JLOG(context.j.info()) << "Catalogue version: " << header.version;
    JLOG(context.j.info()) << "Ledger range: " << header.min_ledger << " - "
                           << header.max_ledger;
    JLOG(context.j.info()) << "Network ID: " << header.network_id;

    if (header.version != CATALOGUE_VERSION)
        return rpcError(
            rpcINVALID_PARAMS,
            "unsupported catalogue version: " + std::to_string(header.version));

    if (header.network_id != context.app.config().NETWORK_ID)
        return rpcError(
            rpcINVALID_PARAMS,
            "catalogue network ID mismatch: " +
                std::to_string(header.network_id));

    uint32_t ledgersLoaded = 0;
    std::shared_ptr<Ledger> prevLedger;

    uint32_t expected_seq = header.min_ledger;

    // Process each ledger sequentially
    while (!infile.eof())
    {
        LedgerInfo info;
        if (!infile.read(
                reinterpret_cast<char*>(&info.seq), sizeof(info.seq)) ||
            !infile.read(
                reinterpret_cast<char*>(&info.parentCloseTime),
                sizeof(info.parentCloseTime)) ||
            !infile.read(reinterpret_cast<char*>(info.hash.data()), 32) ||
            !infile.read(reinterpret_cast<char*>(info.txHash.data()), 32) ||
            !infile.read(
                reinterpret_cast<char*>(info.accountHash.data()), 32) ||
            !infile.read(reinterpret_cast<char*>(info.parentHash.data()), 32) ||
            !infile.read(
                reinterpret_cast<char*>(&info.drops), sizeof(info.drops)) ||
            !infile.read(
                reinterpret_cast<char*>(&info.validated),
                sizeof(info.validated)) ||
            !infile.read(
                reinterpret_cast<char*>(&info.accepted),
                sizeof(info.accepted)) ||
            !infile.read(
                reinterpret_cast<char*>(&info.closeFlags),
                sizeof(info.closeFlags)) ||
            !infile.read(
                reinterpret_cast<char*>(&info.closeTimeResolution),
                sizeof(info.closeTimeResolution)) ||
            !infile.read(
                reinterpret_cast<char*>(&info.closeTime),
                sizeof(info.closeTime)))
        {
            JLOG(context.j.warn()) << "Catalogue load expected but could not "
                                      "read the next ledger header.";
            break;
        }

        JLOG(context.j.info()) << "Found ledger " << info.seq << "...";

        if (info.seq != expected_seq)
        {
            JLOG(context.j.error())
                << "Expected ledger " << expected_seq << ", bailing";
            return rpcError(
                rpcINTERNAL,
                "Unexpected ledger out of sequence in catalogue file");
        }

        // Create a ledger object
        std::shared_ptr<Ledger> ledger;

        if (info.seq == header.min_ledger)
        {
            // Base ledger - create a fresh one
            ledger = std::make_shared<Ledger>(
                info.seq,
                context.app.timeKeeper().closeTime(),
                context.app.config(),
                context.app.getNodeFamily());

            ledger->setLedgerInfo(info);

            // Deserialize the complete state map from leaf nodes
            if (!ledger->stateMap().deserializeFromStream(infile))
            {
                JLOG(context.j.error())
                    << "Failed to deserialize base ledger state";
                return rpcError(
                    rpcINTERNAL, "Failed to load base ledger state");
            }
        }
        else
        {
            // Delta ledger - start with a copy of the previous ledger
            if (!prevLedger)
            {
                JLOG(context.j.error()) << "Missing previous ledger for delta";
                return rpcError(rpcINTERNAL, "Missing previous ledger");
            }

            // Create ledger from previous
            ledger = std::make_shared<Ledger>(
                *prevLedger, context.app.timeKeeper().closeTime());

            ledger->setLedgerInfo(info);

            // Apply delta (only leaf-node changes)
            SHAMap const& prevMap = prevLedger->stateMap();

            if (!ledger->stateMap().deserializeFromStream(infile, prevMap))
            {
                JLOG(context.j.error())
                    << "Failed to apply delta to ledger " << info.seq;
                return rpcError(rpcINTERNAL, "Failed to apply ledger delta");
            }
        }

        // pull in the tx map
        if (!ledger->txMap().deserializeFromStream(infile))
        {
            JLOG(context.j.error())
                << "Failed to apply delta to ledger " << info.seq;
            return rpcError(rpcINTERNAL, "Failed to apply ledger delta");
        }

        // Finalize the ledger
        ledger->stateMap().flushDirty(hotACCOUNT_NODE);
        ledger->txMap().flushDirty(hotTRANSACTION_NODE);

        // Set the ledger as validated
        ledger->setValidated();

        ledger->setAccepted(
            info.closeTime,
            info.closeTimeResolution,
            info.closeFlags & sLCF_NoConsensusTime);

        // Set the proper close flags
        ledger->setCloseFlags(info.closeFlags);

        ledger->setImmutable(true);

        // Store in ledger master
        context.app.getLedgerMaster().storeLedger(ledger);

        if (info.seq == header.max_ledger &&
            context.app.getLedgerMaster().getClosedLedger()->info().seq <
                info.seq)
        {
            // Set as current ledger if this is the latest
            context.app.getLedgerMaster().switchLCL(ledger);
        }

        // Store the ledger
        prevLedger = ledger;
        ledgersLoaded++;
    }

    // Close the file as we've read all the data
    infile.close();

    // Update ledger range in ledger master
    context.app.getLedgerMaster().setLedgerRangePresent(
        header.min_ledger, header.max_ledger);

    JLOG(context.j.info()) << "Catalogue load complete! Loaded "
                           << ledgersLoaded << " ledgers";

    Json::Value jvResult;
    jvResult[jss::ledger_min] = header.min_ledger;
    jvResult[jss::ledger_max] = header.max_ledger;
    jvResult[jss::ledger_count] =
        static_cast<Json::UInt>(header.max_ledger - header.min_ledger + 1);
    jvResult["ledgers_loaded"] = static_cast<Json::UInt>(ledgersLoaded);
    jvResult[jss::status] = jss::success;

    return jvResult;
}

}  // namespace ripple
