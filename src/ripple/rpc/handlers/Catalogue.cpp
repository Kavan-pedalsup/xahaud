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

using time_point = NetClock::time_point;
using duration = NetClock::duration;

static constexpr uint16_t CATALOGUE_VERSION = 1;
#define CATL 0x4C544143UL /*"CATL" in LE*/

#pragma pack(push, 1)  // pack the struct tightly
struct CATLHeader
{
    uint32_t magic = CATL;
    uint32_t min_ledger;
    uint32_t max_ledger;
    uint16_t version;
    uint16_t network_id;
};
#pragma pack(pop)

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
                return rpcError(
                    rpcINVALID_PARAMS,
                    "output_file already exists and is non-empty");
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

    std::vector<std::shared_ptr<Ledger const>> ledgers;
    ledgers.reserve(max_ledger - min_ledger + 1);

    // Grab all ledgers of interest
    for (auto i = min_ledger; i <= max_ledger; ++i)
    {
        std::shared_ptr<Ledger const> ptr;
        auto status = RPC::getLedger(ptr, i, context);
        if (status.toErrorCode() != rpcSUCCESS)  // Status isn't OK
            return rpcError(status);
        if (!ptr)
            return rpcError(rpcLEDGER_MISSING);
        ledgers.emplace_back(ptr);
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

    // Process ledgers with local processor implementation
    auto writeToFile = [&outfile, &context](const void* data, size_t size) {
        outfile.write(reinterpret_cast<const char*>(data), size);
        if (outfile.fail())
        {
            JLOG(context.j.error())
                << "Failed to write to output file: " << std::strerror(errno);
            return false;
        }
        return true;
    };

    auto outputLedger =
        [&writeToFile, &ledgers, &context, &outfile](
            uint32_t seq,
            std::optional<std::reference_wrapper<const SHAMap>> prevStateMap =
                std::nullopt) -> bool {
        try
        {
            auto ledgerIndex = seq - ledgers.front()->info().seq;
            if (ledgerIndex >= ledgers.size())
                return false;

            auto ledger = ledgers[ledgerIndex];
            if (ledger->info().seq != seq)
            {
                JLOG(context.j.error())
                    << "Ledger sequence mismatch: expected " << seq << ", got "
                    << ledger->info().seq;
                return false;
            }

            auto const& info = ledger->info();

            uint64_t closeTime = info.closeTime.time_since_epoch().count();
            uint64_t parentCloseTime =
                info.parentCloseTime.time_since_epoch().count();
            uint32_t closeTimeResolution = info.closeTimeResolution.count();
            uint64_t drops = info.drops.drops();

            // Write ledger header information
            if (!writeToFile(&info.seq, sizeof(info.seq)) ||
                !writeToFile(info.hash.data(), 32) ||
                !writeToFile(info.txHash.data(), 32) ||
                !writeToFile(info.accountHash.data(), 32) ||
                !writeToFile(info.parentHash.data(), 32) ||
                !writeToFile(&drops, sizeof(drops)) ||
                !writeToFile(&info.closeFlags, sizeof(info.closeFlags)) ||
                !writeToFile(
                    &closeTimeResolution, sizeof(closeTimeResolution)) ||
                !writeToFile(&closeTime, sizeof(closeTime)) ||
                !writeToFile(&parentCloseTime, sizeof(parentCloseTime)))
            {
                return false;
            }

            size_t stateNodesWritten =
                ledger->stateMap().serializeToStream(outfile, prevStateMap);
            size_t txNodesWritten = ledger->txMap().serializeToStream(outfile);

            JLOG(context.j.info()) << "Ledger " << seq << ": Wrote "
                                   << stateNodesWritten << " state nodes, "
                                   << "and " << txNodesWritten << " tx nodes";

            return true;
        }
        catch (std::exception const& e)
        {
            JLOG(context.j.error())
                << "Error processing ledger " << seq << ": " << e.what();
            return false;
        }
    };

    // Stream all ledgers
    JLOG(context.j.info()) << "Starting to stream " << ledgers.size()
                           << " ledgers";

    if (ledgers.empty())
    {
        JLOG(context.j.warn()) << "No ledgers to process";
        return rpcError(rpcINTERNAL, "No ledgers to process");
    }

    // Process the first ledger completely
    if (!outputLedger(ledgers.front()->info().seq))
        return rpcError(rpcINTERNAL, "Error occurred while processing ledgers");

    // Process remaining ledgers with diffs
    for (size_t i = 1; i < ledgers.size(); ++i)
    {
        if (!outputLedger(ledgers[i]->info().seq, ledgers[i - 1]->stateMap()))
            return rpcError(
                rpcINTERNAL, "Error occurred while processing ledgers");
    }

    // Get the final file size
    outfile.flush();
    struct stat st;
    if (stat(filepath.c_str(), &st) != 0)
    {
        JLOG(context.j.warn())
            << "Could not get file size: " << std::strerror(errno);
    }

    uint64_t file_size = (stat(filepath.c_str(), &st) == 0) ? st.st_size : 0;
    uint32_t ledgers_written = ledgers.size();

    Json::Value jvResult;
    jvResult[jss::min_ledger] = min_ledger;
    jvResult[jss::max_ledger] = max_ledger;
    jvResult[jss::output_file] = filepath;
    jvResult[jss::file_size] = (Json::UInt)(file_size);
    jvResult[jss::ledgers_written] = static_cast<Json::UInt>(ledgers_written);
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

    // Check file size before attempting to read
    struct stat st;
    if (stat(filepath.c_str(), &st) != 0)
        return rpcError(
            rpcINTERNAL,
            "cannot stat input_file: " + std::string(strerror(errno)));

    uint64_t file_size = st.st_size;

    // Minimal size check: at least a header must be present
    if (file_size < sizeof(CATLHeader))
        return rpcError(
            rpcINVALID_PARAMS,
            "input_file too small (only " + std::to_string(file_size) +
                " bytes), must be at least " +
                std::to_string(sizeof(CATLHeader)) + " bytes");

    JLOG(context.j.info()) << "Catalogue file size: " << file_size << " bytes";

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
    while (!infile.eof() && expected_seq <= header.max_ledger)
    {
        LedgerInfo info;
        uint64_t closeTime = -1;
        uint64_t parentCloseTime = -1;
        uint32_t closeTimeResolution = -1;
        uint64_t drops = -1;

        if (!infile.read(
                reinterpret_cast<char*>(&info.seq), sizeof(info.seq)) ||
            !infile.read(reinterpret_cast<char*>(info.hash.data()), 32) ||
            !infile.read(reinterpret_cast<char*>(info.txHash.data()), 32) ||
            !infile.read(
                reinterpret_cast<char*>(info.accountHash.data()), 32) ||
            !infile.read(reinterpret_cast<char*>(info.parentHash.data()), 32) ||
            !infile.read(reinterpret_cast<char*>(&drops), sizeof(drops)) ||
            !infile.read(
                reinterpret_cast<char*>(&info.closeFlags),
                sizeof(info.closeFlags)) ||
            !infile.read(
                reinterpret_cast<char*>(&closeTimeResolution),
                sizeof(closeTimeResolution)) ||
            !infile.read(
                reinterpret_cast<char*>(&closeTime), sizeof(closeTime)) ||
            !infile.read(
                reinterpret_cast<char*>(&parentCloseTime),
                sizeof(parentCloseTime)))
        {
            JLOG(context.j.warn()) << "Catalogue load expected but could not "
                                      "read the next ledger header.";
            break;
        }

        info.closeTime = time_point{duration{closeTime}};
        info.parentCloseTime = time_point{duration{parentCloseTime}};
        info.closeTimeResolution = duration{closeTimeResolution};
        info.drops = drops;

        JLOG(context.j.info()) << "Found ledger " << info.seq << "...";

        if (info.seq != expected_seq++)
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

            auto snapshot = prevLedger->stateMap().snapShot(true);

            ledger = std::make_shared<Ledger>(
                info,
                context.app.config(),
                context.app.getNodeFamily(),
                *snapshot);

            // Apply delta (only leaf-node changes)
            if (!ledger->stateMap().deserializeFromStream(infile))
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

        ledger->setAccepted(
            info.closeTime,
            info.closeTimeResolution,
            info.closeFlags & sLCF_NoConsensusTime);

        ledger->setValidated();
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
                           << ledgersLoaded << " ledgers from file size "
                           << file_size << " bytes";

    Json::Value jvResult;
    jvResult[jss::ledger_min] = header.min_ledger;
    jvResult[jss::ledger_max] = header.max_ledger;
    jvResult[jss::ledger_count] =
        static_cast<Json::UInt>(header.max_ledger - header.min_ledger + 1);
    jvResult[jss::ledgers_loaded] = static_cast<Json::UInt>(ledgersLoaded);
    jvResult[jss::file_size] = (Json::UInt)(file_size);
    jvResult[jss::status] = jss::success;

    return jvResult;
}

}  // namespace ripple
