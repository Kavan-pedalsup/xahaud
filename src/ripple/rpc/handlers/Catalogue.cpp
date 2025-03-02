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
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ripple {

// This class needs to be declared as a friend in SHAMap.h:
// friend class CatalogueProcessor;
class CatalogueProcessor;

static constexpr uint32_t CATALOGUE_VERSION = 1;

#pragma pack(push, 1)  // pack the struct tightly
struct CATLHeader
{
    char magic[4] = {'C', 'A', 'T', 'L'};
    uint32_t version;
    uint32_t min_ledger;
    uint32_t max_ledger;
    uint32_t network_id;
    uint32_t ledger_tx_offset;
    uint32_t base_ledger_seq;  // Sequence of the base ledger (first full state)
};
#pragma pack(pop)

// Define the node types we'll be serializing
enum class CatalogueNodeType : uint8_t {
    INNER = 1,
    ACCOUNT_STATE = 2,
    TRANSACTION = 3,
    TRANSACTION_META = 4
};

// Type of delta entry
enum class DeltaType : uint8_t { ADDED = 1, MODIFIED = 2, REMOVED = 3 };

// Header for each node in the catalogue file
#pragma pack(push, 1)
struct NodeHeader
{
    uint32_t size;           // Size of the node data (excludes this header)
    uint32_t sequence;       // Ledger sequence this node belongs to
    CatalogueNodeType type;  // Type of node
    uint8_t isRoot;          // 1 if this is a root node, 0 otherwise
    DeltaType deltaType;     // Type of delta (for non-base ledgers)
    uint8_t padding[2];      // Padding for alignment
    uint256 hash;            // Hash of the node
    uint256 nodeID;          // For inner nodes: nodeID; For leaf nodes: key
};
#pragma pack(pop)

// This class handles the processing of ledgers for the catalogue
// It should be declared as a friend in SHAMap.h to access private members
class CatalogueProcessor
{
private:
    std::vector<std::shared_ptr<ReadView const>>& ledgers;
    const size_t numThreads;
    std::ofstream& outfile;
    beast::Journal journal;
    std::mutex fileMutex;  // Mutex for synchronizing file writes

    void
    serializeNode(
        SHAMapTreeNode const& node,
        uint32_t sequence,
        SHAMapNodeID const& nodeID,
        bool isRoot,
        DeltaType deltaType = DeltaType::ADDED)
    {
        Serializer s;

        // First serialize the node to get its data
        Serializer nodeData;
        node.serializeForWire(nodeData);

        // Now create a SHAMapTreeNode from the wire format
        auto newNode = SHAMapTreeNode::makeFromWire(nodeData.slice());

        if (!newNode)
        {
            JLOG(journal.error()) << "Failed to create node from wire format "
                                     "during serialization";
            return;
        }

        // Serialize with prefix (this is what's used when storing nodes)
        newNode->serializeWithPrefix(s);

        // Prepare the node header
        NodeHeader header;
        header.size = s.getLength();
        header.sequence = sequence;

        // Continue with the rest of the function as before...
        if (node.isInner())
        {
            header.type = CatalogueNodeType::INNER;
        }
        else
        {
            auto leafNode = static_cast<SHAMapLeafNode const*>(&node);
            auto nodeType = leafNode->getType();

            if (nodeType == SHAMapNodeType::tnACCOUNT_STATE)
                header.type = CatalogueNodeType::ACCOUNT_STATE;
            else if (nodeType == SHAMapNodeType::tnTRANSACTION_NM)
                header.type = CatalogueNodeType::TRANSACTION;
            else if (nodeType == SHAMapNodeType::tnTRANSACTION_MD)
                header.type = CatalogueNodeType::TRANSACTION_META;
            else
                throw std::runtime_error("Unknown node type");
        }

        header.isRoot = isRoot ? 1 : 0;
        header.deltaType = deltaType;
        header.hash = node.getHash().as_uint256();

        // For inner nodes, store the nodeID; for leaf nodes, store the key
        if (node.isInner())
        {
            header.nodeID = nodeID.getNodeID();
        }
        else
        {
            auto leafNode = static_cast<SHAMapLeafNode const*>(&node);
            header.nodeID = leafNode->peekItem()->key();
        }

        // Thread-safe file write
        // std::lock_guard<std::mutex> lock(fileMutex_);
        outfile.write(reinterpret_cast<const char*>(&header), sizeof(header));
        auto const& data = s.getData();
        outfile.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    // Serialize an entire SHAMap - used for the base (first) ledger
    void
    serializeFullMap(const SHAMap& map, uint32_t sequence)
    {
        using StackEntry = std::pair<SHAMapTreeNode*, SHAMapNodeID>;
        std::stack<StackEntry> nodeStack;

        JLOG(journal.info()) << "Serializing root node with hash: "
                             << to_string(map.root_->getHash().as_uint256());

        // Start with the root
        nodeStack.push({map.root_.get(), SHAMapNodeID()});

        while (!nodeStack.empty())
        {
            auto [node, nodeID] = nodeStack.top();
            nodeStack.pop();

            bool isRoot = (node == map.root_.get());
            serializeNode(*node, sequence, nodeID, isRoot);

            // Add children of inner nodes to the stack
            if (node->isInner())
            {
                auto inner = static_cast<SHAMapInnerNode*>(node);
                for (int i = 0; i < 16; ++i)
                {
                    if (!inner->isEmptyBranch(i))
                    {
                        auto childNode = map.descendThrow(inner, i);
                        auto childID = nodeID.getChildNodeID(i);
                        nodeStack.push({childNode, childID});
                    }
                }
            }
        }
    }

    // Calculate and serialize deltas between two SHAMaps
    void
    serializeMapDelta(
        const SHAMap& oldMap,
        const SHAMap& newMap,
        uint32_t sequence)
    {
        // Use SHAMap's compare method to get differences
        SHAMap::Delta differences;
        if (!oldMap.compare(
                newMap, differences, std::numeric_limits<int>::max()))
        {
            // Too many differences, just serialize the whole map
            JLOG(journal.warn())
                << "Too many differences between ledger " << (sequence - 1)
                << " and " << sequence << ", serializing full state";
            serializeFullMap(newMap, sequence);
            return;
        }

        JLOG(journal.info())
            << "Found " << differences.size() << " differences between ledger "
            << (sequence - 1) << " and " << sequence;

        // Process each difference
        for (auto const& [key, deltaItem] : differences)
        {
            auto const& oldItem = deltaItem.first;
            auto const& newItem = deltaItem.second;

            // Determine the type of change
            DeltaType type;
            if (!oldItem)
            {
                type = DeltaType::ADDED;
            }
            else if (!newItem)
            {
                type = DeltaType::REMOVED;
            }
            else
            {
                type = DeltaType::MODIFIED;
            }

            // Serialize the appropriate item
            if (type == DeltaType::REMOVED)
            {
                // For removed items, we only need to store a minimal record
                // Create a stripped-down header
                NodeHeader header;
                memset(&header, 0, sizeof(header));
                header.size = 0;  // No data for removed items
                header.sequence = sequence;
                header.type =
                    CatalogueNodeType::ACCOUNT_STATE;  // Assume account state
                header.isRoot = 0;
                header.deltaType = DeltaType::REMOVED;
                header.hash = SHAMapHash(key).as_uint256();
                header.nodeID = key;

                std::lock_guard<std::mutex> lock(fileMutex);
                outfile.write(
                    reinterpret_cast<const char*>(&header), sizeof(header));
            }
            else
            {
                // First try to find it directly using the key
                auto leaf = findLeafInMap(newMap, key);
                if (leaf)
                {
                    // Create a nodeID for the leaf
                    SHAMapNodeID nodeID(SHAMap::leafDepth, key);
                    serializeNode(*leaf, sequence, nodeID, false, type);
                }
                else
                {
                    JLOG(journal.warn()) << "Couldn't find node for key " << key
                                         << " in ledger " << sequence;
                }
            }
        }
    }

    // Helper to find a leaf node in a map by key
    SHAMapLeafNode*
    findLeafInMap(const SHAMap& map, uint256 const& key) const
    {
        // Using SHAMap's private walkTowardsKey method through friend access
        return map.findKey(key);
    }

    // Helper to compute the hash of a ledger's state map
    uint256
    getLedgerStateHash(std::shared_ptr<ReadView const> const& ledger) const
    {
        // Get the accountStateHash from the ledger info
        return ledger->info().accountHash;
    }

    // Process a batch of ledgers in a worker thread
    // Each thread computes deltas for its assigned ledgers
    void
    processBatch(size_t startIdx, size_t endIdx)
    {
        for (size_t i = startIdx; i < endIdx && i < ledgers.size(); ++i)
        {
            try
            {
                auto ledger = ledgers[i];
                uint32_t seq = ledger->info().seq;

                auto& stateMap =
                    static_cast<const Ledger*>(ledger.get())->stateMap();

                if (i == 0)
                {
                    // First ledger - serialize the complete state
                    JLOG(journal.info())
                        << "Serializing complete state for base ledger " << seq;
                    serializeFullMap(stateMap, seq);
                }
                else
                {
                    // Delta from previous ledger
                    auto prevLedger = ledgers[i - 1];
                    auto& prevStateMap =
                        static_cast<const Ledger*>(prevLedger.get())
                            ->stateMap();

                    JLOG(journal.info())
                        << "Computing delta for ledger " << seq;
                    serializeMapDelta(prevStateMap, stateMap, seq);
                }

                JLOG(journal.info()) << "Completed ledger " << seq;
            }
            catch (std::exception const& e)
            {
                JLOG(journal.error())
                    << "Error processing ledger: " << e.what();
            }
        }
    }

public:
    CatalogueProcessor(
        std::vector<std::shared_ptr<ReadView const>>& ledgers_,
        std::ofstream& outfile_,
        beast::Journal journal_,
        size_t numThreads_ = std::thread::hardware_concurrency())
        : ledgers(ledgers_)
        , numThreads(1)  // numThreads_ > 0 ? numThreads_ : 1)
        , outfile(outfile_)
        , journal(journal_)
    {
        JLOG(journal.info())
            << "Created CatalogueProcessor with " << numThreads << " threads";
    }

    // Process all ledgers using parallel threads
    void
    streamAll()
    {
        //        numThreads = 1;
        JLOG(journal.info())
            << "Starting to stream " << ledgers.size()
            << " ledgers to catalogue file using " << numThreads << " threads";

        // First ledger must be processed separately to establish the base state
        if (ledgers.empty())
        {
            JLOG(journal.warn()) << "No ledgers to process";
            return;
        }

        // Special case: if there's only one ledger, just process it directly
        if (ledgers.size() == 1 || numThreads == 1)
        {
            processBatch(0, ledgers.size());
            return;
        }

        // For multiple ledgers with multiple threads:
        // - First ledger must be processed first (base state)
        // - Remaining ledgers can be processed in parallel

        // Process the first ledger
        auto baseLedger = ledgers[0];
        uint32_t baseSeq = baseLedger->info().seq;
        auto& baseStateMap =
            static_cast<const Ledger*>(baseLedger.get())->stateMap();

        JLOG(journal.info())
            << "Serializing complete state for base ledger " << baseSeq;
        serializeFullMap(baseStateMap, baseSeq);

        // Now process remaining ledgers in parallel
        std::vector<std::future<void>> futures;

        // Calculate batch size
        size_t remaining = ledgers.size() - 1;  // Skip the first ledger
        size_t batchSize = (remaining + numThreads - 1) / numThreads;

        // Launch worker threads
        for (size_t i = 0; i < numThreads && i * batchSize + 1 < ledgers.size();
             ++i)
        {
            size_t startIdx = i * batchSize + 1;  // +1 to skip the first ledger
            size_t endIdx = std::min(startIdx + batchSize, ledgers.size());

            futures.emplace_back(std::async(
                std::launch::async,
                &CatalogueProcessor::processBatch,
                this,
                startIdx,
                endIdx));
        }

        // Wait for all threads to complete
        for (auto& future : futures)
        {
            future.get();
        }

        JLOG(journal.info()) << "Completed streaming all ledgers";
    }

    // Add transaction data for each ledger
    void
    addTransactions()
    {
        JLOG(journal.info())
            << "Adding transaction data for " << ledgers.size() << " ledgers";

        for (auto const& ledger : ledgers)
        {
            auto const& info = ledger->info();

            auto headerStart = outfile.tellp();
            uint64_t nextHeaderOffset = 0;

            // Reserve space for the next header offset
            outfile.write(
                reinterpret_cast<const char*>(&nextHeaderOffset),
                sizeof(nextHeaderOffset));

            // Write ledger header information
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

            // Write transaction data
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
                JLOG(journal.error())
                    << "Error serializing transaction in ledger " << info.seq
                    << ": " << e.what();
            }

            // Update the next header offset
            auto currentPos = outfile.tellp();
            outfile.seekp(headerStart);
            nextHeaderOffset = currentPos;
            outfile.write(
                reinterpret_cast<const char*>(&nextHeaderOffset),
                sizeof(nextHeaderOffset));
            outfile.seekp(currentPos);

            JLOG(journal.debug())
                << "Added transactions for ledger " << info.seq;
        }

        JLOG(journal.info()) << "Completed adding transaction data";
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

    // Get number of threads to use
    size_t numThreads = std::thread::hardware_concurrency();
    if (context.params.isMember("threads"))
    {
        numThreads = context.params["threads"].asUInt();
        if (numThreads == 0)
            numThreads = std::thread::hardware_concurrency();
    }

    std::vector<std::shared_ptr<ReadView const>> lpLedgers;

    // Grab all ledgers of interest
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

    // Skip forward the header length to begin writing data
    outfile.seekp(sizeof(CATLHeader), std::ios::beg);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to seek in output_file: " + std::string(strerror(errno)));

    // Process ledgers and write to file
    CatalogueProcessor processor(lpLedgers, outfile, context.j, numThreads);

    // Stream all state data first
    processor.streamAll();

    // Remember where we are after all state data
    auto ledgerTxOffset = outfile.tellp();

    // Now add transaction data
    processor.addTransactions();

    // Seek back to start
    outfile.seekp(0, std::ios::beg);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to seek to start of file: " + std::string(strerror(errno)));

    // Create and write header
    CATLHeader header;
    header.version = CATALOGUE_VERSION;
    header.min_ledger = min_ledger;
    header.max_ledger = max_ledger;
    header.network_id = context.app.config().NETWORK_ID;
    header.ledger_tx_offset = ledgerTxOffset;
    header.base_ledger_seq = min_ledger;  // First ledger is always the base

    outfile.write(reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to write header: " + std::string(strerror(errno)));

    std::streampos bytes_written = outfile.tellp();
    outfile.close();
    uint64_t size = static_cast<uint64_t>(bytes_written);

    {
        std::ifstream validateFile(filepath, std::ios::binary);
        CATLHeader validateHeader;
        validateFile.read(
            reinterpret_cast<char*>(&validateHeader), sizeof(CATLHeader));
        if (validateFile.fail() || memcmp(validateHeader.magic, "CATL", 4) != 0)
        {
            JLOG(context.j.error())
                << "Catalogue file appears to be corrupted!";
        }
        else
        {
            JLOG(context.j.info()) << "Catalogue file header verified OK";
        }
        validateFile.close();
    }

    // Return the result
    Json::Value jvResult;
    jvResult[jss::min_ledger] = min_ledger;
    jvResult[jss::max_ledger] = max_ledger;
    jvResult[jss::output_file] = filepath;
    jvResult[jss::bytes_written] = static_cast<Json::UInt>(size);
    jvResult["threads_used"] = static_cast<Json::UInt>(numThreads);
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

    if (std::memcmp(header.magic, "CATL", 4) != 0)
        return rpcError(rpcINVALID_PARAMS, "invalid catalogue file magic");

    JLOG(context.j.info()) << "Catalogue version: " << header.version;
    JLOG(context.j.info()) << "Ledger range: " << header.min_ledger << " - "
                           << header.max_ledger;
    JLOG(context.j.info()) << "Network ID: " << header.network_id;
    JLOG(context.j.info()) << "Ledger/TX offset: " << header.ledger_tx_offset;
    JLOG(context.j.info()) << "Base ledger: " << header.base_ledger_seq;

    if (header.version != CATALOGUE_VERSION)
        return rpcError(
            rpcINVALID_PARAMS,
            "unsupported catalogue version: " + std::to_string(header.version));

    if (header.network_id != context.app.config().NETWORK_ID)
        return rpcError(
            rpcINVALID_PARAMS,
            "catalogue network ID mismatch: " +
                std::to_string(header.network_id));

    // Create maps to store nodes by sequence
    std::map<
        uint32_t,
        std::map<uint256, std::pair<std::vector<uint8_t>, SHAMapNodeID>>>
        nodesByLedger;
    std::map<uint32_t, uint256> rootHashesByLedger;
    std::map<uint32_t, std::vector<std::pair<uint256, DeltaType>>>
        deltasByLedger;

    // Read all nodes from the beginning to the ledger/tx offset
    JLOG(context.j.info()) << "Reading state nodes...";
    infile.seekg(sizeof(CATLHeader), std::ios::beg);

    size_t stateNodeCount = 0;
    size_t addedCount = 0;
    size_t modifiedCount = 0;
    size_t removedCount = 0;

    // First pass: Read all node data
    while (infile.tellg() < header.ledger_tx_offset)
    {
        NodeHeader nodeHeader;
        infile.read(reinterpret_cast<char*>(&nodeHeader), sizeof(nodeHeader));
        if (infile.fail())
            break;

        stateNodeCount++;

        // Count by delta type
        if (nodeHeader.deltaType == DeltaType::ADDED)
            addedCount++;
        else if (nodeHeader.deltaType == DeltaType::MODIFIED)
            modifiedCount++;
        else if (nodeHeader.deltaType == DeltaType::REMOVED)
            removedCount++;

        // Read the node data
        std::vector<uint8_t> nodeData;
        if (nodeHeader.size > 0)
        {
            nodeData.resize(nodeHeader.size);
            infile.read(
                reinterpret_cast<char*>(nodeData.data()), nodeHeader.size);
            if (infile.fail())
                break;
        }

        // Store the node data and track deltas
        SHAMapNodeID nodeID;
        if (nodeHeader.type == CatalogueNodeType::INNER)
        {
            // For inner nodes, recreate the nodeID
            nodeID = SHAMapNodeID::createID(
                0, nodeHeader.nodeID);  // Depth will be recalculated
        }
        else
        {
            // For leaf nodes, create an ID at leafDepth with the key
            nodeID = SHAMapNodeID(SHAMap::leafDepth, nodeHeader.nodeID);
        }

        // Store the node data by ledger sequence
        nodesByLedger[nodeHeader.sequence][nodeHeader.hash] = {
            nodeData, nodeID};

        // Track deltas for non-base ledgers
        if (nodeHeader.sequence != header.base_ledger_seq)
        {
            deltasByLedger[nodeHeader.sequence].emplace_back(
                nodeHeader.hash, nodeHeader.deltaType);
        }

        // If this is a root node, store its hash
        if (nodeHeader.isRoot)
        {
            rootHashesByLedger[nodeHeader.sequence] = nodeHeader.hash;
        }
    }

    JLOG(context.j.info()) << "Read " << stateNodeCount << " state nodes"
                           << " (Added: " << addedCount
                           << ", Modified: " << modifiedCount
                           << ", Removed: " << removedCount << ")";

    // Read transaction data
    std::map<
        uint32_t,
        std::vector<std::pair<std::shared_ptr<STTx>, std::shared_ptr<TxMeta>>>>
        txsByLedger;
    std::map<uint32_t, LedgerInfo> ledgerInfoBySeq;

    JLOG(context.j.info()) << "Reading transaction data...";
    infile.seekg(header.ledger_tx_offset, std::ios::beg);

    size_t txLedgerCount = 0;
    size_t txCount = 0;

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

        // Read hash values using temporary buffer to avoid type issues
        unsigned char hashBuf[32];

        infile.read(reinterpret_cast<char*>(hashBuf), 32);
        info.hash = uint256::fromVoid(hashBuf);

        infile.read(reinterpret_cast<char*>(hashBuf), 32);
        info.txHash = uint256::fromVoid(hashBuf);

        infile.read(reinterpret_cast<char*>(hashBuf), 32);
        info.accountHash = uint256::fromVoid(hashBuf);

        infile.read(reinterpret_cast<char*>(hashBuf), 32);
        info.parentHash = uint256::fromVoid(hashBuf);

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

        // Store ledger info
        ledgerInfoBySeq[info.seq] = info;
        txLedgerCount++;

        // Read transactions until we reach the next ledger or end of file
        auto currentPos = infile.tellg();
        while (currentPos < nextOffset || nextOffset == 0)
        {
            // Read transaction ID with temporary buffer
            unsigned char txIDBuf[32];
            infile.read(reinterpret_cast<char*>(txIDBuf), 32);
            if (infile.eof() || infile.fail())
                break;

            uint256 txID = uint256::fromVoid(txIDBuf);

            // Read transaction data
            uint32_t txnSize;
            infile.read(reinterpret_cast<char*>(&txnSize), sizeof(txnSize));

            std::vector<uint8_t> txnData(txnSize);
            infile.read(reinterpret_cast<char*>(txnData.data()), txnSize);

            // Read metadata if present
            uint32_t metaSize;
            infile.read(reinterpret_cast<char*>(&metaSize), sizeof(metaSize));

            std::vector<uint8_t> metaData;
            if (metaSize > 0)
            {
                metaData.resize(metaSize);
                infile.read(reinterpret_cast<char*>(metaData.data()), metaSize);
            }

            // Create and store transaction objects
            auto txn = std::make_shared<STTx>(
                SerialIter{txnData.data(), txnData.size()});

            std::shared_ptr<TxMeta> meta;
            if (!metaData.empty())
            {
                meta = std::make_shared<TxMeta>(txID, info.seq, metaData);
            }

            txsByLedger[info.seq].emplace_back(txn, meta);
            txCount++;

            // Update current position for next iteration check
            currentPos = infile.tellg();

            if (nextOffset == 0)
                break;
        }

        // Move to the next ledger if there is one
        if (nextOffset != 0)
            infile.seekg(nextOffset, std::ios::beg);
        else
            break;
    }

    JLOG(context.j.info()) << "Read transactions for " << txLedgerCount
                           << " ledgers, total transactions: " << txCount;

    // Now rebuild and load ledgers
    JLOG(context.j.info()) << "Rebuilding ledgers...";

    uint32_t ledgersLoaded = 0;
    std::shared_ptr<Ledger> prevLedger;

    // Process ledgers in sequence order
    for (uint32_t seq = header.min_ledger; seq <= header.max_ledger; seq++)
    {
        JLOG(context.j.info()) << "Loading ledger " << seq;

        // Get ledger info
        auto infoIt = ledgerInfoBySeq.find(seq);
        if (infoIt == ledgerInfoBySeq.end())
        {
            JLOG(context.j.warn()) << "Missing ledger info for ledger " << seq;
            continue;
        }

        // Create a new ledger
        std::shared_ptr<Ledger> ledger;

        if (seq == header.base_ledger_seq)
        {
            // Base ledger - need to completely rebuild from stored nodes
            auto rootIt = rootHashesByLedger.find(seq);
            if (rootIt == rootHashesByLedger.end())
            {
                JLOG(context.j.error())
                    << "Missing root hash for base ledger " << seq;
                continue;
            }

            // Use the correct constructor for base ledger
            ledger = std::make_shared<Ledger>(
                infoIt->second,
                context.app.config(),
                context.app.getNodeFamily());

            // Now build the state tree by adding all nodes for this ledger
            auto& nodesForLedger = nodesByLedger[seq];

            // Start with the root node
            auto rootNodeIt = nodesForLedger.find(rootIt->second);
            if (rootNodeIt == nodesForLedger.end())
            {
                JLOG(context.j.error())
                    << "Missing root node data for base ledger " << seq;
                continue;
            }

            auto const& rootData = nodesByLedger[seq][rootIt->second].first;
            JLOG(context.j.info()) << "Loading root node for ledger " << seq
                                   << ", hash: " << to_string(rootIt->second)
                                   << ", data size: " << rootData.size();

            // Examine first few bytes of data
            if (rootData.size() >= 16)
            {
                std::stringstream ss;
                ss << "Root data first 16 bytes: ";
                for (size_t i = 0; i < 16; ++i)
                    ss << std::hex << std::setw(2) << std::setfill('0')
                       << (int)rootData[i] << " ";
                JLOG(context.j.info()) << ss.str();
            }

            // Try to create the node using makeFromPrefix (what we stored)
            auto testNode = SHAMapTreeNode::makeFromPrefix(
                Slice(rootData.data(), rootData.size()),
                SHAMapHash(rootIt->second));

            if (!testNode)
            {
                JLOG(context.j.error())
                    << "Failed to create test node from prefix format";
                continue;
            }

            JLOG(context.j.info()) << "Created test node with hash: "
                                   << to_string(testNode->getHash());

            auto& [_, rootNodeID] = rootNodeIt->second;

            Serializer s;
            testNode->serializeForWire(s);

            // s.addRaw(Slice(rootData.data(), rootData.size()));
            if (!ledger->stateMap()
                     .addRootNode(
                         SHAMapHash(rootIt->second), s.slice(), nullptr)
                     .isGood())
            {
                JLOG(context.j.error())
                    << "Failed to add root node for base ledger " << seq
                    << ", root hash: " << to_string(rootIt->second)
                    << ", data size: " << rootData.size();
                // Try to dump some of the data for debugging
                if (rootData.size() > 0)
                {
                    std::stringstream ss;
                    ss << "Data: ";
                    for (size_t i = 0;
                         i < std::min<size_t>(32, rootData.size());
                         ++i)
                        ss << std::hex << std::setw(2) << std::setfill('0')
                           << (int)rootData[i] << " ";
                    JLOG(context.j.info()) << ss.str();
                }
                continue;
            }

            // Process all other nodes using a queue approach
            std::queue<uint256> nodeQueue;
            std::set<uint256> processedNodes;

            processedNodes.insert(rootIt->second);

            // Add the root node's children to the queue
            auto rootNode = SHAMapTreeNode::makeFromPrefix(
                Slice(rootData.data(), rootData.size()),
                SHAMapHash(rootIt->second));

            if (rootNode && rootNode->isInner())
            {
                auto innerRoot =
                    std::static_pointer_cast<SHAMapInnerNode>(rootNode);
                for (int i = 0; i < 16; i++)
                {
                    if (!innerRoot->isEmptyBranch(i))
                    {
                        auto childHash =
                            innerRoot->getChildHash(i).as_uint256();
                        nodeQueue.push(childHash);
                    }
                }
            }

            // Process all nodes
            while (!nodeQueue.empty())
            {
                auto nodeHash = nodeQueue.front();
                nodeQueue.pop();

                // Skip if already processed
                if (processedNodes.find(nodeHash) != processedNodes.end())
                    continue;

                processedNodes.insert(nodeHash);

                auto nodeIt = nodesForLedger.find(nodeHash);
                if (nodeIt == nodesForLedger.end())
                {
                    JLOG(context.j.warn())
                        << "Missing node data for hash " << nodeHash;
                    continue;
                }

                auto& [nodeData, nodeID] = nodeIt->second;

                // Add the node to the map
                if (!ledger->stateMap()
                         .addKnownNode(
                             nodeID,
                             Slice(nodeData.data(), nodeData.size()),
                             nullptr)
                         .isGood())
                {
                    JLOG(context.j.warn()) << "Failed to add node " << nodeHash;
                    continue;
                }

                // If this is an inner node, add its children to the queue
                auto node = SHAMapTreeNode::makeFromPrefix(
                    Slice(nodeData.data(), nodeData.size()),
                    SHAMapHash(nodeHash));

                if (node && node->isInner())
                {
                    auto innerNode =
                        std::static_pointer_cast<SHAMapInnerNode>(node);
                    for (int i = 0; i < 16; i++)
                    {
                        if (!innerNode->isEmptyBranch(i))
                        {
                            auto childHash =
                                innerNode->getChildHash(i).as_uint256();
                            nodeQueue.push(childHash);
                        }
                    }
                }
            }
        }
        else
        {
            // Delta-based ledger - start with a snapshot of the previous ledger
            if (!prevLedger)
            {
                JLOG(context.j.error())
                    << "Missing previous ledger for delta update at ledger "
                    << seq;
                continue;
            }

            // Use the correct constructor for delta-based ledger
            ledger =
                std::make_shared<Ledger>(*prevLedger, infoIt->second.closeTime);

            // Apply deltas to the state map
            auto deltaIt = deltasByLedger.find(seq);
            if (deltaIt == deltasByLedger.end())
            {
                JLOG(context.j.warn()) << "No deltas found for ledger " << seq;
            }
            else
            {
                auto& deltas = deltaIt->second;

                for (auto const& [hash, deltaType] : deltas)
                {
                    auto nodeIt = nodesByLedger[seq].find(hash);

                    if (nodeIt == nodesByLedger[seq].end())
                    {
                        JLOG(context.j.warn())
                            << "Missing node data for delta in ledger " << seq;
                        continue;
                    }

                    auto& [nodeData, nodeID] = nodeIt->second;

                    if (deltaType == DeltaType::REMOVED)
                    {
                        // Remove the item from the map
                        if (!ledger->stateMap().delItem(nodeID.getNodeID()))
                        {
                            JLOG(context.j.warn())
                                << "Failed to remove item for delta in ledger "
                                << seq;
                        }
                    }
                    else if (
                        deltaType == DeltaType::ADDED ||
                        deltaType == DeltaType::MODIFIED)
                    {
                        // Create a node from the data
                        auto node = SHAMapTreeNode::makeFromPrefix(
                            Slice(nodeData.data(), nodeData.size()),
                            SHAMapHash(hash));

                        if (!node)
                        {
                            JLOG(context.j.warn())
                                << "Failed to create node from delta data in "
                                   "ledger "
                                << seq;
                            continue;
                        }

                        if (node->isLeaf())
                        {
                            auto leaf =
                                std::static_pointer_cast<SHAMapLeafNode>(node);
                            auto item = leaf->peekItem();

                            // Update or add the item
                            auto nodeType = leaf->getType();

                            if (deltaType == DeltaType::ADDED)
                            {
                                if (!ledger->stateMap().addItem(nodeType, item))
                                {
                                    JLOG(context.j.warn())
                                        << "Failed to add item for delta in "
                                           "ledger "
                                        << seq;
                                }
                            }
                            else
                            {
                                if (!ledger->stateMap().updateGiveItem(
                                        nodeType, item))
                                {
                                    JLOG(context.j.warn())
                                        << "Failed to update item for delta in "
                                           "ledger "
                                        << seq;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Apply transaction data
        auto txIt = txsByLedger.find(seq);
        if (txIt != txsByLedger.end())
        {
            for (auto const& [tx, meta] : txIt->second)
            {
                auto txID = tx->getTransactionID();

                // Add transaction to ledger
                auto s = std::make_shared<Serializer>();
                tx->add(*s);

                std::shared_ptr<Serializer> metaSerializer;
                if (meta)
                {
                    metaSerializer = std::make_shared<Serializer>();
                    meta->addRaw(
                        *metaSerializer,
                        meta->getResultTER(),
                        meta->getIndex());
                }

                ledger->rawTxInsert(
                    txID, std::move(s), std::move(metaSerializer));
            }
        }

        // Finalize the ledger
        ledger->updateSkipList();
        ledger->stateMap().flushDirty(hotACCOUNT_NODE);
        ledger->txMap().flushDirty(hotTRANSACTION_NODE);

        // Set the ledger as validated
        ledger->setValidated();
        ledger->setAccepted(
            infoIt->second.closeTime,
            infoIt->second.closeTimeResolution,
            infoIt->second.closeFlags & sLCF_NoConsensusTime);

        ledger->setImmutable(true);  // Use default parameter

        // Store in ledger master
        context.app.getLedgerMaster().storeLedger(ledger);

        if (seq == header.max_ledger)
        {
            // Set as current ledger if this is the latest
            context.app.getLedgerMaster().switchLCL(ledger);
        }

        prevLedger = ledger;
        ledgersLoaded++;
    }

    // Update ledger range in ledger master
    context.app.getLedgerMaster().setLedgerRangePresent(
        header.min_ledger, header.max_ledger);

    JLOG(context.j.info()) << "Catalogue load complete! Loaded "
                           << ledgersLoaded << " ledgers.";

    Json::Value jvResult;
    jvResult[jss::ledger_min] = header.min_ledger;
    jvResult[jss::ledger_max] = header.max_ledger;
    jvResult[jss::ledger_count] =
        static_cast<Json::UInt>(header.max_ledger - header.min_ledger + 1);
    jvResult["ledgers_loaded"] = static_cast<Json::UInt>(ledgersLoaded);
    jvResult["state_nodes"] = static_cast<Json::UInt>(stateNodeCount);
    jvResult["transactions"] = static_cast<Json::UInt>(txCount);
    jvResult[jss::status] = jss::success;

    return jvResult;
}

}  // namespace ripple
