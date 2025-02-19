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
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

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

class ParallelLedgerProcessor
{
private:
    // Lightweight structure to track object state changes
    struct StateChange
    {
        uint32_t sequence;
        uint32_t dataSize;
    };

    // A batch is just a subset of keys to process
    struct KeyBatch
    {
        std::vector<ReadView::key_type> keys;
    };

    class ThreadSafeQueue
    {
        std::queue<KeyBatch> queue;
        mutable std::mutex mutex;
        std::condition_variable cond;
        bool finished = false;

    public:
        void
        push(KeyBatch item)
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(std::move(item));
            cond.notify_one();
        }

        bool
        pop(KeyBatch& item)
        {
            std::unique_lock<std::mutex> lock(mutex);
            cond.wait(lock, [this] { return !queue.empty() || finished; });
            if (queue.empty() && finished)
                return false;
            item = std::move(queue.front());
            queue.pop();
            return true;
        }

        void
        markFinished()
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
            cond.notify_all();
        }
    };

    // Thread-safe map to store state changes per key
    class StateChangeMap
    {
        mutable std::mutex mutex;
        hash_map<ReadView::key_type, std::vector<StateChange>> changes;

    public:
        StateChangeMap() = default;

        void
        addChange(ReadView::key_type const& key, StateChange change)
        {
            std::lock_guard<std::mutex> lock(mutex);
            changes[key].push_back(std::move(change));
        }

        auto const&
        getChanges() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return changes;
        }
    };

    std::vector<std::shared_ptr<ReadView const>>& ledgers;
    ThreadSafeQueue workQueue;
    StateChangeMap stateChanges;
    std::vector<std::thread> workers;
    std::atomic<size_t> completedBatches{0};
    const size_t batchSize = 1000;
    const size_t numThreads;
    std::ofstream& outfile;

    std::pair<bool, uint32_t>
    hasDataChanged(
        std::shared_ptr<SLE const> const& prevSLE,
        std::shared_ptr<SLE const> const& currentSLE)
    {
        if (prevSLE && !currentSLE)
            return {true, 0};  // deletion

        if (!prevSLE && !currentSLE)
            return {false, 0};  // still deleted

        Serializer s1, s2;
        currentSLE->add(s2);
        int l2 = s2.getLength();

        if (!prevSLE)
            return {true, l2};

        prevSLE->add(s1);
        int l1 = s1.getLength();

        bool changed = l1 != l2 ||
            !std::equal(s1.peekData().begin(),
                        s1.peekData().end(),
                        s2.peekData().begin());

        return {changed, changed ? l2 : 0};
    }

    void
    processBatch(const KeyBatch& batch)
    {
        // For each key in this batch
        for (const auto& key : batch.keys)
        {
            std::shared_ptr<SLE const> prevSLE;

            // Process the key through all ledgers sequentially
            for (size_t i = 0; i < ledgers.size(); ++i)
            {
                auto& ledger = ledgers[i];
                auto currentSLE = ledger->read(keylet::unchecked(key));
                if (auto [changed, serializedLen] =
                        hasDataChanged(prevSLE, currentSLE);
                    changed)
                    stateChanges.addChange(
                        key, {ledger->info().seq, serializedLen});
                prevSLE = currentSLE;
            }
        }
        completedBatches++;
    }

    void
    workerFunction()
    {
        KeyBatch batch;
        while (workQueue.pop(batch))
        {
            processBatch(batch);
        }
    }

public:
    ParallelLedgerProcessor(
        std::vector<std::shared_ptr<ReadView const>>& ledgers_,
        std::ofstream& outfile_,
        size_t numThreads_ = std::thread::hardware_concurrency())
        : ledgers(ledgers_), numThreads(numThreads_), outfile(outfile_)
    {
    }

    void
    streamAll()
    {
        // Start worker threads
        for (size_t i = 0; i < numThreads; ++i)
        {
            workers.emplace_back([this] { workerFunction(); });
        }

        // Collect all unique keys
        std::set<ReadView::key_type> allKeys;
        for (const auto& ledger : ledgers)
        {
            for (auto const& sle : ledger->sles)
            {
                allKeys.insert(sle->key());
            }
        }

        // Create and queue work batches
        std::vector<ReadView::key_type> keyBatch;
        keyBatch.reserve(batchSize);

        for (const auto& key : allKeys)
        {
            keyBatch.push_back(key);
            if (keyBatch.size() == batchSize)
            {
                workQueue.push({std::move(keyBatch)});
                keyBatch.clear();
                keyBatch.reserve(batchSize);
            }
        }

        if (!keyBatch.empty())
        {
            workQueue.push({std::move(keyBatch)});
        }

        workQueue.markFinished();

        // Wait for all workers to complete
        for (auto& worker : workers)
        {
            worker.join();
        }

        // Write results
        auto const& changes = stateChanges.getChanges();
        for (const auto& [key, keyChanges] : changes)
        {
            outfile.write(
                reinterpret_cast<const char*>(key.data()), key.size());

            for (size_t i = 0; i < keyChanges.size(); ++i)
            {
                auto& change = keyChanges[i];
                bool hasNext = i < keyChanges.size() - 1;

                outfile.write(
                    reinterpret_cast<const char*>(&change.sequence), 4);

                uint32_t flagsAndSize = change.dataSize & SIZE_MASK;
                if (hasNext)
                    flagsAndSize |= HAS_NEXT_FLAG;

                outfile.write(reinterpret_cast<const char*>(&flagsAndSize), 4);

                if (change.dataSize > 0)
                {
                    // Find the ledger with this sequence
                    size_t ledgerIndex =
                        change.sequence - ledgers.front()->info().seq;
                    if (ledgerIndex < ledgers.size())
                    {
                        auto sle =
                            ledgers[ledgerIndex]->read(keylet::unchecked(key));
                        if (sle)
                        {
                            Serializer s;
                            sle->add(s);
                            auto const& data = s.peekData();
                            outfile.write(
                                reinterpret_cast<const char*>(data.data()),
                                data.size());
                        }
                    }
                }
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

    // grab all ledgers of interest
    lpLedgers.reserve(max_ledger - min_ledger + 1);

    for (auto i = min_ledger; i <= max_ledger; ++i)
    {
        std::shared_ptr<ReadView const> ptr;
        auto status = RPC::getLedger(ptr, i, context);
        if (status.toErrorCode() != rpcSUCCESS)  // Status isn't OK
            return rpcError(status);
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

    ParallelLedgerProcessor processor(lpLedgers, outfile);
    processor.streamAll();

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

    std::cout << "Opening catalogue file: " << filepath << std::endl;

    // Check if file exists and is readable
    std::ifstream infile(filepath.c_str(), std::ios::in | std::ios::binary);
    if (infile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open input_file: " + std::string(strerror(errno)));

    std::cout << "Reading catalogue header..." << std::endl;

    // Read and validate header
    CATLHeader header;
    infile.read(reinterpret_cast<char*>(&header), sizeof(CATLHeader));
    if (infile.fail())
        return rpcError(rpcINTERNAL, "failed to read catalogue header");

    if (std::memcmp(header.magic, "CATL", 4) != 0)
        return rpcError(rpcINVALID_PARAMS, "invalid catalogue file magic");

    std::cout << "Catalogue version: " << header.version << std::endl;
    std::cout << "Ledger range: " << header.min_ledger << " - "
              << header.max_ledger << std::endl;
    std::cout << "Network ID: " << header.network_id << std::endl;
    std::cout << "Ledger/TX offset: " << header.ledger_tx_offset << std::endl;

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

    std::cout << "Reading state data..." << std::endl;

    // Map to store state versions for each key
    std::map<
        std::reference_wrapper<const uint256>,
        std::vector<StatePosition>,
        std::function<bool(
            std::reference_wrapper<const uint256>,
            std::reference_wrapper<const uint256>)>>
        stateVersions(
            [](auto const& a, auto const& b) { return a.get() < b.get(); });

    size_t stateEntryCount = 0;
    size_t stateVersionCount = 0;

    // First pass: Read all keys and their positions
    while (infile.tellg() < header.ledger_tx_offset)
    {
        uint256 key;
        infile.read(key.cdata(), 32);
        if (infile.fail())
            break;

        auto [keyIt, inserted] = allKeys.insert(key);
        if (inserted)
        {
            stateEntryCount++;
            if (stateEntryCount % 10000 == 0)
            {
                std::cout << "Processed " << stateEntryCount
                          << " unique state entries..." << std::endl;
            }
        }

        auto [stateIt, stateInserted] = stateVersions.emplace(
            std::cref(*keyIt), std::vector<StatePosition>{});
        std::vector<StatePosition>& positions = stateIt->second;

        uint32_t seq;
        infile.read(reinterpret_cast<char*>(&seq), 4);

        uint32_t flagsAndSize;
        infile.read(reinterpret_cast<char*>(&flagsAndSize), 4);

        uint32_t size = flagsAndSize & SIZE_MASK;
        bool hasNext = (flagsAndSize & HAS_NEXT_FLAG) != 0;

        // Store ALL state changes, including deletions
        positions.push_back({infile.tellg(), seq, size});
        stateVersionCount++;

        if (size > 0)
        {
            infile.seekg(size, std::ios::cur);
        }

        while (hasNext)
        {
            infile.read(reinterpret_cast<char*>(&seq), 4);
            infile.read(reinterpret_cast<char*>(&flagsAndSize), 4);

            size = flagsAndSize & SIZE_MASK;
            hasNext = (flagsAndSize & HAS_NEXT_FLAG) != 0;

            positions.push_back({infile.tellg(), seq, size});
            stateVersionCount++;

            if (size > 0)
            {
                infile.seekg(size, std::ios::cur);
            }
        }
    }

    std::cout << "Found " << stateEntryCount << " unique state entries with "
              << stateVersionCount << " total versions" << std::endl;
    std::cout << "Processing ledgers..." << std::endl;

    // Process ledgers sequentially
    infile.seekg(header.ledger_tx_offset);
    std::shared_ptr<const Ledger> previousLedger;
    uint32_t ledgerCount = 0;
    size_t totalTxCount = 0;

    while (!infile.eof())
    {
        uint64_t nextOffset;
        auto currentPos = infile.tellg();
        infile.read(reinterpret_cast<char*>(&nextOffset), sizeof(nextOffset));
        if (infile.fail())
        {
            std::cout << "Failed to read next offset at position " << currentPos
                      << std::endl;
            break;
        }

        std::cout << "Current file position: " << currentPos
                  << ", Next offset: " << nextOffset << std::endl;

        LedgerInfo info;
        auto ledgerHeaderPos = infile.tellg();
        infile.read(reinterpret_cast<char*>(&info.seq), sizeof(info.seq));

        if (info.seq < header.min_ledger || info.seq > header.max_ledger)
        {
            std::cout << "WARNING: Ledger sequence " << info.seq
                      << " is outside expected range " << header.min_ledger
                      << "-" << header.max_ledger << " at position "
                      << ledgerHeaderPos << std::endl;
        }
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

        std::cout << "Processing ledger " << info.seq
                  << " (hash: " << to_string(info.hash) << ")" << std::endl;

        std::shared_ptr<Ledger> currentLedger;
        do
        {
            // see if we can fetch the previous ledger
            if (!previousLedger && header.min_ledger > 1)
            {
                auto lh = context.app.getLedgerMaster().getHashBySeq(
                    header.min_ledger - 1);
                if (lh != beast::zero)
                    previousLedger =
                        context.app.getLedgerMaster().getLedgerByHash(lh);
            }

            // if we either already made previous ledger or it exists in history
            // use it
            if (previousLedger)
            {
                currentLedger =
                    std::make_shared<Ledger>(*previousLedger, info.closeTime);

                break;
            }

            // otherwise we're building a new one
            std::cout << "Creating initial ledger..." << std::endl;
            currentLedger = std::make_shared<Ledger>(
                info, context.app.config(), context.app.getNodeFamily());

        } while (0);

        size_t txCount = 0;
        // Read and apply transactions
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
                meta = std::make_shared<TxMeta>(
                    txID, currentLedger->seq(), metaData);
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

            forceValidity(
                context.app.getHashRouter(), txID, Validity::SigGoodOnly);

            currentLedger->rawTxInsert(
                txID, std::move(s), std::move(metaSerializer));

            txCount++;
            totalTxCount++;
        }

        std::cout << "Applied " << txCount << " transactions" << std::endl;

        size_t stateChangeCount = 0;
        // Apply state changes for this ledger only
        for (auto const& [keyRef, positions] : stateVersions)
        {
            auto const& key = keyRef.get();

            // Look for a state change specifically for this ledger sequence
            auto it = std::find_if(
                positions.begin(), positions.end(), [&](auto const& pos) {
                    return pos.sequence == currentLedger->info().seq;
                });

            if (it != positions.end())
            {
                // if it exists remove it before possibly recreating it
                if (currentLedger->stateMap().hasItem(key))
                    currentLedger->stateMap().delItem(key);

                if (it->size > 0)
                {
                    // Read and apply state data
                    infile.seekg(it->filePos);
                    std::vector<unsigned char> data(it->size);
                    infile.read(reinterpret_cast<char*>(data.data()), it->size);

                    // create item
                    auto item = make_shamapitem(key, makeSlice(data));
                    currentLedger->stateMap().addItem(
                        SHAMapNodeType::tnACCOUNT_STATE, std::move(item));
                }
                stateChangeCount++;
            }
        }

        std::cout << "Applied " << stateChangeCount << " state changes"
                  << std::endl;

        currentLedger->stateMap().flushDirty(hotACCOUNT_NODE);
        currentLedger->updateSkipList();

        // Finalize and store the ledger
        currentLedger->setValidated();
        currentLedger->setAccepted(
            currentLedger->info().closeTime,
            currentLedger->info().closeTimeResolution,
            currentLedger->info().closeFlags & sLCF_NoConsensusTime);

        // Set the ledger as immutable after all mutations are complete
        currentLedger->setImmutable(true);

        context.app.getLedgerMaster().storeLedger(currentLedger);

        previousLedger = currentLedger;
        ledgerCount++;

        if (nextOffset > 0)
        {
            infile.seekg(nextOffset, std::ios::beg);
            if (infile.fail())
            {
                std::cout << "Failed to seek to next offset " << nextOffset
                          << std::endl;
                break;
            }
        }
    }  // end of while (!infile.eof())

    std::cout << "Catalogue load complete!" << std::endl;
    std::cout << "Processed " << ledgerCount << " ledgers containing "
              << totalTxCount << " transactions" << std::endl;

    Json::Value jvResult;
    jvResult[jss::ledger_min] = header.min_ledger;
    jvResult[jss::ledger_max] = header.max_ledger;
    jvResult[jss::ledger_count] = static_cast<Json::UInt>(ledgerCount);
    jvResult[jss::status] = jss::success;

    return jvResult;
}

}  // namespace ripple
