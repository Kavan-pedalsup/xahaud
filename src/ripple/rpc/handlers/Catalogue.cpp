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
#include <atomic>
#include <future>
#include <thread>

namespace ripple {

static constexpr uint32_t HAS_NEXT_FLAG = 0x80000000;
static constexpr uint32_t SIZE_MASK = 0x0FFFFFFF;
static constexpr size_t NUM_SHARDS =
    16;  // Number of parallel processing shards
static constexpr size_t WRITE_BUFFER_SIZE = 1024 * 1024;  // 1MB write buffer

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

#pragma pack(push, 1)
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

// Buffered writing system to minimize disk I/O
class BufferedWriter
{
    std::vector<uint8_t> buffer;
    std::vector<std::pair<std::streampos, uint32_t>> flagPositions;
    std::ofstream& outfile;
    std::mutex mutex;
    std::streampos currentPos{0};

public:
    explicit BufferedWriter(std::ofstream& out) : outfile(out)
    {
        buffer.reserve(WRITE_BUFFER_SIZE);
    }

    std::streampos
    getPos() const
    {
        return currentPos;
    }

    void
    write(const void* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t currentBufferPos = buffer.size();
        buffer.resize(currentBufferPos + size);
        std::memcpy(buffer.data() + currentBufferPos, data, size);
        currentPos += size;
        flushIfNeeded();
    }

    void
    recordFlagPosition(std::streampos pos, uint32_t flags)
    {
        std::lock_guard<std::mutex> lock(mutex);
        flagPositions.emplace_back(pos, flags);
    }

    void
    flushIfNeeded()
    {
        if (buffer.size() >= WRITE_BUFFER_SIZE)
        {
            flush();
        }
    }

    void
    flush()
    {
        if (!buffer.empty())
        {
            outfile.write(
                reinterpret_cast<const char*>(buffer.data()), buffer.size());
            buffer.clear();
        }
    }

    void
    updateFlags()
    {
        flush();  // Ensure all data is written
        for (const auto& [pos, flags] : flagPositions)
        {
            outfile.seekp(pos);
            outfile.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
        }
        flagPositions.clear();
    }
};

class DataChangeTracker
{
    struct VersionData
    {
        Blob data;
        std::mutex mutex;

        // Add proper constructors
        VersionData() = default;
        VersionData(VersionData&&) = default;
        VersionData&
        operator=(VersionData&&) = default;

        // Copy operations deleted due to mutex
        VersionData(const VersionData&) = delete;
        VersionData&
        operator=(const VersionData&) = delete;
    };

    class KeyDataMap
    {
        std::map<uint256, std::unique_ptr<VersionData>> data;
        std::mutex mutex;

    public:
        std::pair<VersionData*, bool>
        getOrCreate(uint256 const& key)
        {
            std::lock_guard lock(mutex);
            auto [it, inserted] = data.try_emplace(key, nullptr);
            if (inserted)
            {
                it->second = std::make_unique<VersionData>();
            }
            return {it->second.get(), inserted};
        }
    };

    KeyDataMap lastSeenData;

public:
    bool
    hasChanged(uint256 const& key, Serializer const& s)
    {
        auto const& newData = s.peekData();
        auto [versionData, inserted] = lastSeenData.getOrCreate(key);

        std::lock_guard dataLock(versionData->mutex);
        if (inserted || versionData->data != newData)
        {
            versionData->data = newData;
            return true;
        }
        return false;
    }
};

class KeyStore
{
    struct VersionList
    {
        std::vector<std::pair<LedgerIndex, std::shared_ptr<SLE const>>>
            versions;
        std::mutex mutex;

        VersionList() = default;
        VersionList(VersionList&&) = default;
        VersionList&
        operator=(VersionList&&) = default;

        VersionList(const VersionList&) = delete;
        VersionList&
        operator=(const VersionList&) = delete;
    };

    class ShardMap
    {
        std::map<uint256, std::unique_ptr<VersionList>> data;
        std::mutex mutex;

    public:
        VersionList*
        getOrCreate(uint256 const& key)
        {
            std::lock_guard lock(mutex);
            auto [it, inserted] = data.try_emplace(key, nullptr);
            if (inserted)
            {
                it->second = std::make_unique<VersionList>();
            }
            return it->second.get();
        }

        template <typename Callback>
        void
        forEach(Callback&& cb)
        {
            std::lock_guard lock(mutex);
            for (auto& [key, list] : data)
            {
                if (list)
                {
                    std::lock_guard listLock(list->mutex);
                    cb(key, list->versions);
                }
            }
        }
    };

    std::array<ShardMap, NUM_SHARDS> shards;

    size_t
    getShardIndex(uint256 const& key) const
    {
        return key.data()[0] % NUM_SHARDS;  // Use first byte for sharding
    }

public:
    void
    addEntry(
        uint256 const& key,
        LedgerIndex seq,
        std::shared_ptr<SLE const> const& sle)
    {
        auto shardIdx = getShardIndex(key);
        auto list = shards[shardIdx].getOrCreate(key);
        std::lock_guard listLock(list->mutex);
        list->versions.emplace_back(seq, sle);
    }

    template <typename Callback>
    void
    forEachInShard(size_t shardIdx, Callback&& cb)
    {
        shards[shardIdx].forEach(std::forward<Callback>(cb));
    }
};

class ParallelCatalogueBuilder
{
private:
    std::vector<std::shared_ptr<ReadView const>>& ledgers;
    LedgerIndex minSeq;
    KeyStore keyStore;
    BufferedWriter writer;
    DataChangeTracker changeTracker;

    void
    buildIndices()
    {
        std::vector<std::thread> workers;
        std::atomic<size_t> ledgerIdx{0};

        for (size_t t = 0; t < std::thread::hardware_concurrency(); ++t)
        {
            workers.emplace_back([this, &ledgerIdx]() {
                while (true)
                {
                    size_t idx = ledgerIdx.fetch_add(1);
                    if (idx >= ledgers.size())
                        break;

                    auto& ledger = ledgers[idx];
                    for (auto const& sle : ledger->sles)
                    {
                        keyStore.addEntry(sle->key(), minSeq + idx, sle);
                    }
                }
            });
        }

        for (auto& worker : workers)
            worker.join();
    }

    void
    processKeyShard(size_t shardIdx)
    {
        keyStore.forEachInShard(
            shardIdx, [this](auto const& key, auto const& versions) {
                std::vector<StatePosition> positions;
                std::shared_ptr<SLE const> prevSLE;

                for (auto const& [seq, sle] : versions)
                {
                    Serializer s;
                    sle->add(s);

                    if (changeTracker.hasChanged(key, s))
                    {
                        auto pos = writer.getPos();
                        writer.write(s.data(), s.getLength());
                        positions.push_back(
                            {pos, seq, static_cast<uint32_t>(s.getLength())});
                    }
                    prevSLE = sle;
                }

                if (!positions.empty())
                {
                    writer.write(key.data(), key.size());
                    for (size_t i = 0; i < positions.size(); ++i)
                    {
                        bool hasNext = i < positions.size() - 1;
                        uint32_t flags = positions[i].size;
                        if (hasNext)
                            flags |= HAS_NEXT_FLAG;
                        writer.recordFlagPosition(positions[i].filePos, flags);
                    }
                }
            });
    }

public:
    ParallelCatalogueBuilder(
        std::vector<std::shared_ptr<ReadView const>>& ledgers_,
        LedgerIndex minSeq_,
        std::ofstream& outfile)
        : ledgers(ledgers_), minSeq(minSeq_), writer(outfile)
    {
    }

    void
    build()
    {
        buildIndices();

        std::vector<std::thread> workers;
        for (size_t i = 0; i < NUM_SHARDS; ++i)
        {
            workers.emplace_back([this, i]() { processKeyShard(i); });
        }

        for (auto& worker : workers)
            worker.join();

        writer.updateFlags();
    }
};

void
writeLedgerAndTransactions(
    std::ofstream& outfile,
    std::vector<std::shared_ptr<ReadView const>>& ledgers)
{
    for (auto const& ledger : ledgers)
    {
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
                auto const& txnId = i.first->getTransactionID();
                outfile.write(txnId.cdata(), 32);

                Serializer sTxn = i.first->getSerializer();
                uint32_t txnSize = sTxn.getLength();
                outfile.write(
                    reinterpret_cast<const char*>(&txnSize), sizeof(txnSize));
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
                        reinterpret_cast<const char*>(sMeta.data()), metaSize);
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
            std::cout << e.what() << "\n";
            // Log error but continue processing
        }

        auto currentPos = outfile.tellp();
        outfile.seekp(headerStart);
        nextHeaderOffset = currentPos;
        outfile.write(
            reinterpret_cast<const char*>(&nextHeaderOffset),
            sizeof(nextHeaderOffset));
        outfile.seekp(currentPos);
    }
}

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

    // Validate output file
    {
        struct stat st;
        if (stat(filepath.c_str(), &st) == 0)
        {
            if (st.st_size > 0)
            {
                return rpcError(
                    rpcINVALID_PARAMS,
                    "output_file already exists and is non-empty");
            }
        }
        else if (errno != ENOENT)
        {
            return rpcError(
                rpcINTERNAL,
                "cannot stat output_file: " + std::string(strerror(errno)));
        }

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
    lpLedgers.reserve(max_ledger - min_ledger + 1);

    // Load all ledgers
    for (auto i = min_ledger; i <= max_ledger; ++i)
    {
        std::shared_ptr<ReadView const> ptr = nullptr;
        auto jvResult = RPC::lookupLedger(ptr, context);
        if (!ptr)
            return rpcError(rpcLEDGER_MISSING);
        lpLedgers.emplace_back(ptr);
    }

    // Skip header space
    outfile.seekp(sizeof(CATLHeader), std::ios::beg);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to seek in output_file: " + std::string(strerror(errno)));

    // Build catalogue
    ParallelCatalogueBuilder builder(lpLedgers, min_ledger, outfile);
    builder.build();

    // Write header
    auto endPosition = outfile.tellp();
    outfile.seekp(0, std::ios::beg);

    CATLHeader header;
    header.version = 1;
    header.min_ledger = min_ledger;
    header.max_ledger = max_ledger;
    header.network_id = context.app.config().NETWORK_ID;
    header.ledger_tx_offset = endPosition;

    outfile.write(reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to write header: " + std::string(strerror(errno)));

    // Write ledger data - this part remains largely unchanged
    writeLedgerAndTransactions(outfile, lpLedgers);

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
    if (!context.params.isMember(jss::input_file))
        return rpcError(rpcINVALID_PARAMS, "expected input_file");

    bool force = context.params.isMember(jss::force) &&
        context.params[jss::force].asBool();

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

    Json::Value jvResult;
    jvResult[jss::ledger_min] = header.min_ledger;
    jvResult[jss::ledger_max] = header.max_ledger;

    // Track statistics and issues
    uint32_t imported = 0;
    uint32_t skipped = 0;
    uint32_t failed = 0;
    std::vector<uint32_t> failedSeqs;
    std::vector<uint32_t> hashMismatchSeqs;

    auto& ledgerMaster = context.app.getLedgerMaster();

    // Validate and import ledgers
    for (auto const& ledger : ledgers)
    {
        bool shouldImport = true;
        std::string failureReason;

        try
        {
            // Check if ledger already exists
            auto existingLedger =
                ledgerMaster.getLedgerBySeq(ledger->info().seq);
            if (existingLedger)
            {
                if (existingLedger->info().hash == ledger->info().hash)
                {
                    // Exact match - skip
                    ++skipped;
                    shouldImport = false;
                }
                else
                {
                    // Hash mismatch
                    hashMismatchSeqs.push_back(ledger->info().seq);
                    if (!force)
                    {
                        ++skipped;
                        shouldImport = false;
                    }
                }
            }

            if (shouldImport)
            {
                // Verify account state hash
                if (ledger->stateMap().getHash().isNonZero() &&
                    ledger->stateMap().getHash() != ledger->info().accountHash)
                {
                    failureReason = "Account state hash mismatch";
                    throw std::runtime_error(failureReason);
                }

                // Verify transaction set hash
                if (ledger->txMap().getHash().isNonZero() &&
                    ledger->txMap().getHash() != ledger->info().txHash)
                {
                    failureReason = "Transaction set hash mismatch";
                    throw std::runtime_error(failureReason);
                }

                // Additional sanity checks for transactions
                for (auto& i : ledger->txs)
                {
                    if (!i.first)
                    {
                        failureReason = "Invalid transaction found";
                        throw std::runtime_error(failureReason);
                    }

                    // Verify transaction metadata if present
                    if (i.second && i.second->getLedger() != ledger->info().seq)
                    {
                        failureReason =
                            "Transaction metadata sequence mismatch";
                        throw std::runtime_error(failureReason);
                    }
                }

                // All checks passed, import the ledger
                ledger->setValidated();
                ledger->setAccepted(
                    ledger->info().closeTime,
                    ledger->info().closeTimeResolution,
                    ledger->info().closeFlags & sLCF_NoConsensusTime);

                ledgerMaster.storeLedger(ledger);
                ++imported;
            }
        }
        catch (std::exception const& e)
        {
            // Log the failure and continue with next ledger
            failedSeqs.push_back(ledger->info().seq);
            ++failed;
            JLOG(context.j.error()) << "Failed to import ledger "
                                    << ledger->info().seq << ": " << e.what();
        }
    }

    // Report results
    jvResult[jss::imported] = imported;
    jvResult[jss::skipped] = skipped;
    jvResult[jss::failed] = failed;

    if (!hashMismatchSeqs.empty())
    {
        auto& hashMismatches =
            (jvResult[jss::hash_mismatches] = Json::arrayValue);
        for (auto seq : hashMismatchSeqs)
            hashMismatches.append(seq);
    }

    if (!failedSeqs.empty())
    {
        auto& failures = (jvResult[jss::failed_ledgers] = Json::arrayValue);
        for (auto seq : failedSeqs)
            failures.append(seq);
    }

    jvResult[jss::status] = jss::success;
    return jvResult;
}
}  // namespace ripple
