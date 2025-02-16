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
#include <ripple/app/tx/apply.h>
#include <ripple/basics/Slice.h>
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
#include <ripple/shamap/SHAMapItem.h>

namespace ripple {

static constexpr uint32_t HAS_NEXT_FLAG = 0x80000000;
static constexpr uint32_t SIZE_MASK = 0x0FFFFFFF;

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

    std::vector<std::shared_ptr<ReadView const>>& ledgers;
    LedgerIndex minSeq;
    std::ofstream& outfile;

    std::vector<LedgerEntry>
    getLedgerIterators(size_t ledgerIndex)
    {
        std::vector<LedgerEntry> result;
        auto begin = ledgers[ledgerIndex]->sles.begin();
        auto end = ledgers[ledgerIndex]->sles.end();
        if (begin != end)
        {
            result.push_back(
                {begin, end, static_cast<LedgerIndex>(minSeq + ledgerIndex)});
        }
        return result;
    }

    std::shared_ptr<SLE const>
    findSLEInLedger(
        const ReadView::key_type& key,
        const std::vector<LedgerEntry>& ledgerIters)
    {
        for (const auto& entry : ledgerIters)
        {
            auto iter = entry.iter;
            while (iter != entry.end)
            {
                if ((*iter)->key() == key)
                    return *iter;
                if ((*iter)->key() > key)
                    break;
                ++iter;
            }
        }
        return nullptr;
    }

    bool
    hasDataChanged(
        std::shared_ptr<SLE const> const& prevSLE,
        std::shared_ptr<SLE const> const& currentSLE)
    {
        if (!prevSLE || !currentSLE)
            return true;

        Serializer s1, s2;
        prevSLE->add(s1);
        currentSLE->add(s2);

        auto const& v1 = s1.peekData();
        auto const& v2 = s2.peekData();

        return v1.size() != v2.size() ||
            !std::equal(v1.begin(), v1.end(), v2.begin());
    }

    void
    writeVersionData(
        const std::shared_ptr<SLE const>& sle,
        LedgerIndex seq,
        bool isDeleted,
        bool hasNext,
        std::streampos& lastFlagsPos)
    {
        uint32_t flagsAndSize = 0;
        Serializer s;

        if (!isDeleted && sle)
        {
            sle->add(s);
            auto const& data = s.peekData();
            flagsAndSize = data.size() & SIZE_MASK;
        }

        if (hasNext)
            flagsAndSize |= HAS_NEXT_FLAG;

        outfile.write(reinterpret_cast<const char*>(&seq), 4);
        lastFlagsPos = outfile.tellp();
        outfile.write(reinterpret_cast<const char*>(&flagsAndSize), 4);

        if (!isDeleted && sle)
        {
            auto const& data = s.peekData();
            outfile.write(
                reinterpret_cast<const char*>(data.data()), data.size());
        }
    }

    void
    writeKeyEntry(const ReadView::key_type& key)
    {
        outfile.write(key.cdata(), 32);

        std::streampos lastFlagsPos;
        bool wrote_any = false;
        std::shared_ptr<SLE const> prevSLE;

        for (size_t i = 0; i < ledgers.size(); ++i)
        {
            auto currentIters = getLedgerIterators(i);
            auto currentSLE = findSLEInLedger(key, currentIters);

            bool shouldWrite = false;
            bool isDeleted = false;

            if (currentSLE)
            {
                // Write if the data has changed from previous version
                if (hasDataChanged(prevSLE, currentSLE))
                {
                    shouldWrite = true;
                }
            }
            else if (prevSLE)
            {
                // Object was deleted
                shouldWrite = true;
                isDeleted = true;
            }

            if (shouldWrite)
            {
                bool hasNext = (i < ledgers.size() - 1);
                writeVersionData(
                    currentSLE, minSeq + i, isDeleted, hasNext, lastFlagsPos);
                wrote_any = true;
            }

            prevSLE = currentSLE;
        }

        if (wrote_any)
        {
            auto currentPos = outfile.tellp();
            outfile.seekp(lastFlagsPos);
            uint32_t finalFlags = SIZE_MASK & outfile.tellp();
            outfile.write(reinterpret_cast<const char*>(&finalFlags), 4);
            outfile.seekp(currentPos);
        }
    }

public:
    StreamingLedgerIterator(
        std::vector<std::shared_ptr<ReadView const>>& ledgers_,
        LedgerIndex minSeq_,
        std::ofstream& out)
        : ledgers(ledgers_), minSeq(minSeq_), outfile(out)
    {
    }

    void
    streamAll()
    {
        std::set<ReadView::key_type> allKeys;

        for (size_t i = 0; i < ledgers.size(); ++i)
        {
            auto iters = getLedgerIterators(i);
            for (auto& entry : iters)
            {
                auto iter = entry.iter;
                while (iter != entry.end)
                {
                    allKeys.insert((*iter)->key());
                    ++iter;
                }
            }
        }

        for (const auto& key : allKeys)
        {
            writeKeyEntry(key);
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
        std::shared_ptr<ReadView const> ptr = nullptr;
        auto jvResult = RPC::lookupLedger(ptr, context);
        if (!ptr)
            return rpcError(rpcLEDGER_MISSING);
        lpLedgers.emplace_back(ptr);
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

// Stores file position information for a state entry version
struct StatePosition
{
    std::streampos filePos;  // Position of the data in file
    uint32_t sequence;       // Ledger sequence this version applies to
    uint32_t size;           // Size of the data
};

// Custom comparator for uint256 references
struct uint256RefCompare
{
    bool
    operator()(uint256 const& a, uint256 const& b) const
    {
        return a < b;
    }
};

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

    // Check if file exists and is readable
    std::ifstream infile(filepath.c_str(), std::ios::in | std::ios::binary);
    if (infile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open input_file: " + std::string(strerror(errno)));

    // Read and validate header
    CATLHeader header;
    infile.read(reinterpret_cast<char*>(&header), sizeof(CATLHeader));
    if (infile.fail())
        return rpcError(rpcINTERNAL, "failed to read catalogue header");

    if (std::memcmp(header.magic, "CATL", 4) != 0)
        return rpcError(rpcINVALID_PARAMS, "invalid catalogue file magic");

    if (header.version != 1)
        return rpcError(
            rpcINVALID_PARAMS,
            "unsupported catalogue version: " + std::to_string(header.version));

    if (header.network_id != context.app.config().NETWORK_ID)
        return rpcError(
            rpcINVALID_PARAMS,
            "catalogue network ID mismatch: " +
                std::to_string(header.network_id));

    // Track all unique keys we encounter
    std::set<uint256, uint256RefCompare> allKeys;

    // Map to store latest version of each key per ledger
    // We use references to the keys in allKeys to avoid copies
    std::map<
        std::reference_wrapper<const uint256>,
        std::vector<StatePosition>,
        std::function<bool(
            std::reference_wrapper<const uint256>,
            std::reference_wrapper<const uint256>)>>
        stateVersions(
            [](auto const& a, auto const& b) { return a.get() < b.get(); });

    // First pass: Read all keys and their positions
    while (infile.tellg() < header.ledger_tx_offset)
    {
        uint256 key;
        infile.read(key.cdata(), 32);
        if (infile.fail())
            break;

        auto [keyIt, inserted] = allKeys.insert(key);

        // Either find existing vector of positions or insert a new empty one
        auto [stateIt, stateInserted] = stateVersions.emplace(
            std::cref(*keyIt), std::vector<StatePosition>{});
        std::vector<StatePosition>& positions = stateIt->second;

        uint32_t seq;
        infile.read(reinterpret_cast<char*>(&seq), 4);

        uint32_t flagsAndSize;
        infile.read(reinterpret_cast<char*>(&flagsAndSize), 4);

        uint32_t size = flagsAndSize & SIZE_MASK;
        bool hasNext = (flagsAndSize & HAS_NEXT_FLAG) != 0;

        if (size > 0)
        {
            positions.push_back({infile.tellg(), seq, size});
            infile.seekg(size, std::ios::cur);
        }

        while (hasNext)
        {
            infile.read(reinterpret_cast<char*>(&seq), 4);
            infile.read(reinterpret_cast<char*>(&flagsAndSize), 4);

            size = flagsAndSize & SIZE_MASK;
            hasNext = (flagsAndSize & HAS_NEXT_FLAG) != 0;

            if (size > 0)
            {
                positions.push_back({infile.tellg(), seq, size});
                infile.seekg(size, std::ios::cur);
            }
        }
    }

    // Now read the ledger headers and transactions
    std::vector<std::shared_ptr<Ledger>> ledgers;
    ledgers.reserve(header.max_ledger - header.min_ledger + 1);

    infile.seekg(header.ledger_tx_offset);

    while (!infile.eof())
    {
        uint64_t nextOffset;
        infile.read(reinterpret_cast<char*>(&nextOffset), sizeof(nextOffset));
        if (infile.fail())
            break;

        LedgerInfo info;

        infile.read(reinterpret_cast<char*>(&info.seq), sizeof(info.seq));
        infile.read(
            reinterpret_cast<char*>(&info.parentCloseTime),
            sizeof(info.parentCloseTime));
        infile.read(info.hash.cdata(), 32);
        infile.read(info.txHash.cdata(), 32);
        infile.read(info.accountHash.cdata(), 32);
        infile.read(info.parentHash.cdata(), 32);
        infile.read(reinterpret_cast<char*>(&info.drops), sizeof(info.drops));
        infile.read(
            reinterpret_cast<char*>(&info.validated), sizeof(info.validated));
        infile.read(
            reinterpret_cast<char*>(&info.accepted), sizeof(info.accepted));
        infile.read(
            reinterpret_cast<char*>(&info.closeFlags), sizeof(info.closeFlags));
        infile.read(
            reinterpret_cast<char*>(&info.closeTimeResolution),
            sizeof(info.closeTimeResolution));
        infile.read(
            reinterpret_cast<char*>(&info.closeTime), sizeof(info.closeTime));

        auto ledger = std::make_shared<Ledger>(
            info, context.app.config(), context.app.getNodeFamily());

        // Read transaction data
        while (infile.tellg() < nextOffset)
        {
            uint256 txID;
            infile.read(txID.cdata(), 32);

            uint32_t txnSize;
            infile.read(reinterpret_cast<char*>(&txnSize), sizeof(txnSize));

            std::vector<unsigned char> txnData(txnSize);
            infile.read(reinterpret_cast<char*>(txnData.data()), txnSize);

            uint32_t metaSize;
            infile.read(reinterpret_cast<char*>(&metaSize), sizeof(metaSize));

            std::vector<unsigned char> metaData;
            if (metaSize > 0)
            {
                metaData.resize(metaSize);
                infile.read(reinterpret_cast<char*>(metaData.data()), metaSize);
            }

            auto txn = std::make_shared<STTx>(
                SerialIter{txnData.data(), txnData.size()});

            std::shared_ptr<TxMeta> meta;
            if (!metaData.empty())
            {
                meta = std::make_shared<TxMeta>(txID, ledger->seq(), metaData);
            }

            auto s = std::make_shared<Serializer>();
            txn->add(*s);

            std::shared_ptr<Serializer> metaSerializer;
            if (meta)
            {
                metaSerializer = std::make_shared<Serializer>();
                meta->addRaw(
                    *metaSerializer, meta->getResultTER(), meta->getIndex());
            }

            // Force transaction validity
            forceValidity(
                context.app.getHashRouter(), txID, Validity::SigGoodOnly);

            // Insert the raw transaction
            ledger->rawTxInsert(txID, std::move(s), std::move(metaSerializer));
        }

        ledgers.push_back(std::move(ledger));
    }

    // Now reconstruct the state for each ledger
    for (auto& ledger : ledgers)
    {
        for (auto const& [keyRef, positions] : stateVersions)
        {
            auto const& key = keyRef.get();

            // Find the applicable state version for this ledger
            auto it = std::find_if(
                positions.rbegin(), positions.rend(), [&](auto const& pos) {
                    return pos.sequence <= ledger->info().seq;
                });

            if (it != positions.rend())
            {
                // Read the state data
                infile.seekg(it->filePos);

                std::vector<unsigned char> data(it->size);
                infile.read(reinterpret_cast<char*>(data.data()), it->size);

                // Add the state data directly to the map
                auto item = make_shamapitem(key, makeSlice(data));
                ledger->stateMap().addItem(
                    SHAMapNodeType::tnACCOUNT_STATE, std::move(item));
            }
        }

        ledger->stateMap().flushDirty(hotACCOUNT_NODE);
        ledger->updateSkipList();
    }

    // Import ledgers into the ledger master
    for (auto const& ledger : ledgers)
    {
        ledger->setValidated();
        ledger->setAccepted(
            ledger->info().closeTime,
            ledger->info().closeTimeResolution,
            ledger->info().closeFlags & sLCF_NoConsensusTime);

        context.app.getLedgerMaster().storeLedger(ledger);
    }

    Json::Value jvResult;
    jvResult[jss::ledger_min] = header.min_ledger;
    jvResult[jss::ledger_max] = header.max_ledger;
    jvResult[jss::ledger_count] = static_cast<Json::UInt>(ledgers.size());
    jvResult[jss::status] = jss::success;

    return jvResult;
}
}  // namespace ripple
