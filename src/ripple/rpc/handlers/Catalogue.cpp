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
    std::mutex fileMutex;              // Mutex for synchronizing file writes
    std::atomic<bool> aborted{false};  // Flag to signal process abortion

    // Safely write to the file with proper synchronization
    bool
    writeToFile(const void* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (aborted)
            return false;

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

    // Convert SHAMapNodeType to CatalogueNodeType
    CatalogueNodeType
    convertNodeType(SHAMapNodeType type)
    {
        switch (type)
        {
            case SHAMapNodeType::tnINNER:
                return CatalogueNodeType::INNER;
            case SHAMapNodeType::tnACCOUNT_STATE:
                return CatalogueNodeType::ACCOUNT_STATE;
            case SHAMapNodeType::tnTRANSACTION_NM:
                return CatalogueNodeType::TRANSACTION;
            case SHAMapNodeType::tnTRANSACTION_MD:
                return CatalogueNodeType::TRANSACTION_META;
            default:
                throw std::runtime_error("Unknown node type");
        }
    }

    // Serialize a single node to the output file
    void
    serializeNode(
        SHAMapTreeNode const& node,
        uint32_t sequence,
        SHAMapNodeID const& nodeID,
        bool isRoot,
        DeltaType deltaType = DeltaType::ADDED)
    {
        if (aborted)
            return;

        try
        {
            // Serialize the node with prefix format for consistency
            Serializer s;
            node.serializeWithPrefix(s);

            // Prepare the node header
            NodeHeader header;
            header.size = s.getLength();
            header.sequence = sequence;

            // Set the node type
            header.type = convertNodeType(node.getType());

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

            // Write the header and data to file
            if (!writeToFile(&header, sizeof(header)))
                return;

            auto const& data = s.getData();
            if (!writeToFile(data.data(), data.size()))
                return;
        }
        catch (std::exception const& e)
        {
            JLOG(journal.error()) << "Error serializing node: " << e.what();
            aborted = true;
        }
    }

    // Serialize an entire SHAMap - used for the base (first) ledger
    void
    serializeFullMap(const SHAMap& map, uint32_t sequence)
    {
        if (aborted)
            return;

        try
        {
            using StackEntry = std::pair<SHAMapTreeNode*, SHAMapNodeID>;
            std::stack<StackEntry> nodeStack;

            JLOG(journal.info())
                << "Serializing root node with hash: "
                << to_string(map.root_->getHash().as_uint256());

            // Start with the root
            nodeStack.push({map.root_.get(), SHAMapNodeID()});

            while (!nodeStack.empty() && !aborted)
            {
                auto [node, nodeID] = nodeStack.top();
                nodeStack.pop();

                bool isRoot = (node == map.root_.get());
                serializeNode(*node, sequence, nodeID, isRoot);

                // Add children of inner nodes to the stack
                if (node->isInner())
                {
                    auto inner = static_cast<SHAMapInnerNode*>(node);
                    // Process branches in reverse order so they're processed in
                    // ascending order
                    for (int i = 15; i >= 0; --i)
                    {
                        if (!inner->isEmptyBranch(i))
                        {
                            try
                            {
                                auto childNode = map.descendThrow(inner, i);
                                auto childID = nodeID.getChildNodeID(i);
                                nodeStack.push({childNode, childID});
                            }
                            catch (std::exception const& e)
                            {
                                JLOG(journal.error())
                                    << "Error descending to child " << i << ": "
                                    << e.what();
                                // Continue with other children
                            }
                        }
                    }
                }
            }
        }
        catch (std::exception const& e)
        {
            JLOG(journal.error()) << "Error in serializeFullMap: " << e.what();
            aborted = true;
        }
    }

    // Helper to find a leaf node in a map by key
    SHAMapLeafNode*
    findLeafInMap(const SHAMap& map, uint256 const& key) const
    {
        try
        {
            return map.findKey(key);
        }
        catch (std::exception const& e)
        {
            JLOG(journal.error()) << "Error finding key in map: " << e.what();
            return nullptr;
        }
    }

    // Calculate and serialize deltas between two SHAMaps
    void
    serializeMapDelta(
        const SHAMap& oldMap,
        const SHAMap& newMap,
        uint32_t sequence)
    {
        if (aborted)
            return;

        try
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

            JLOG(journal.info()) << "Found " << differences.size()
                                 << " differences between ledger "
                                 << (sequence - 1) << " and " << sequence;

            // Track processed keys to avoid duplicates
            std::unordered_set<uint256, beast::uhash<>> processedKeys;

            // Process each difference
            for (auto const& [key, deltaItem] : differences)
            {
                if (aborted)
                    return;

                // Skip if already processed
                if (processedKeys.find(key) != processedKeys.end())
                    continue;

                processedKeys.insert(key);

                auto const& oldItem = deltaItem.first;
                auto const& newItem = deltaItem.second;

                // Determine the type of change
                DeltaType type;
                if (!oldItem)
                    type = DeltaType::ADDED;
                else if (!newItem)
                    type = DeltaType::REMOVED;
                else
                    type = DeltaType::MODIFIED;

                if (type == DeltaType::REMOVED)
                {
                    // For removed items, we only need to store a minimal record
                    NodeHeader header;
                    memset(&header, 0, sizeof(header));
                    header.size = 0;  // No data for removed items
                    header.sequence = sequence;
                    header.type =
                        CatalogueNodeType::ACCOUNT_STATE;  // Default type
                    header.isRoot = 0;
                    header.deltaType = DeltaType::REMOVED;
                    header.hash = SHAMapHash(key).as_uint256();
                    header.nodeID = key;

                    if (!writeToFile(&header, sizeof(header)))
                        return;
                }
                else
                {
                    // For added/modified items, find and serialize the node
                    try
                    {
                        auto leaf = findLeafInMap(newMap, key);
                        if (leaf)
                        {
                            // Create a nodeID for the leaf
                            SHAMapNodeID nodeID(SHAMap::leafDepth, key);
                            serializeNode(*leaf, sequence, nodeID, false, type);
                        }
                        else
                        {
                            JLOG(journal.warn())
                                << "Couldn't find node for key " << key
                                << " in ledger " << sequence;
                        }
                    }
                    catch (std::exception const& e)
                    {
                        JLOG(journal.error())
                            << "Error processing delta item: " << e.what();
                        // Continue with other items
                    }
                }
            }
        }
        catch (std::exception const& e)
        {
            JLOG(journal.error()) << "Error in serializeMapDelta: " << e.what();
            aborted = true;
        }
    }

    // Process a batch of ledgers in a worker thread
    void
    processBatch(size_t startIdx, size_t endIdx)
    {
        for (size_t i = startIdx; i < endIdx && i < ledgers.size() && !aborted;
             ++i)
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
                aborted = true;
            }
        }
    }

    // Helper to serialize ledger transaction data
    bool
    serializeLedgerTransactions(
        ReadView const& ledger,
        std::streampos& headerStart)
    {
        auto const& info = ledger.info();

        headerStart = outfile.tellp();
        uint64_t nextHeaderOffset = 0;

        // Reserve space for the next header offset
        if (!writeToFile(&nextHeaderOffset, sizeof(nextHeaderOffset)))
            return false;

        // Write ledger header information
        if (!writeToFile(&info.seq, sizeof(info.seq)) ||
            !writeToFile(&info.parentCloseTime, sizeof(info.parentCloseTime)) ||
            !writeToFile(info.hash.data(), 32) ||
            !writeToFile(info.txHash.data(), 32) ||
            !writeToFile(info.accountHash.data(), 32) ||
            !writeToFile(info.parentHash.data(), 32) ||
            !writeToFile(&info.drops, sizeof(info.drops)) ||
            !writeToFile(&info.validated, sizeof(info.validated)) ||
            !writeToFile(&info.accepted, sizeof(info.accepted)) ||
            !writeToFile(&info.closeFlags, sizeof(info.closeFlags)) ||
            !writeToFile(
                &info.closeTimeResolution, sizeof(info.closeTimeResolution)) ||
            !writeToFile(&info.closeTime, sizeof(info.closeTime)))
        {
            return false;
        }

        // Write transaction data
        try
        {
            for (auto& i : ledger.txs)
            {
                if (aborted)
                    return false;

                assert(i.first);
                auto const& txnId = i.first->getTransactionID();
                if (!writeToFile(txnId.data(), 32))
                    return false;

                Serializer sTxn = i.first->getSerializer();
                uint32_t txnSize = sTxn.getLength();
                if (!writeToFile(&txnSize, sizeof(txnSize)) ||
                    !writeToFile(sTxn.data(), txnSize))
                {
                    return false;
                }

                uint32_t metaSize = 0;
                if (i.second)
                {
                    Serializer sMeta = i.second->getSerializer();
                    metaSize = sMeta.getLength();
                    if (!writeToFile(&metaSize, sizeof(metaSize)) ||
                        !writeToFile(sMeta.data(), metaSize))
                    {
                        return false;
                    }
                }
                else
                {
                    if (!writeToFile(&metaSize, sizeof(metaSize)))
                        return false;
                }
            }
        }
        catch (std::exception const& e)
        {
            JLOG(journal.error()) << "Error serializing transaction in ledger "
                                  << info.seq << ": " << e.what();
            return false;
        }

        return true;
    }

public:
    CatalogueProcessor(
        std::vector<std::shared_ptr<ReadView const>>& ledgers_,
        std::ofstream& outfile_,
        beast::Journal journal_,
        size_t numThreads_ = std::thread::hardware_concurrency())
        : ledgers(ledgers_)
        , numThreads(numThreads_ > 0 ? 1 : 1)  // Force single thread for now
        , outfile(outfile_)
        , journal(journal_)
    {
        JLOG(journal.info())
            << "Created CatalogueProcessor with " << numThreads << " threads";
    }

    // Process all ledgers using single thread for now (to ensure file
    // coherence)
    bool
    streamAll()
    {
        JLOG(journal.info())
            << "Starting to stream " << ledgers.size()
            << " ledgers to catalogue file using " << numThreads << " threads";

        if (ledgers.empty())
        {
            JLOG(journal.warn()) << "No ledgers to process";
            return false;
        }

        // Process ledgers sequentially to ensure file coherence
        processBatch(0, ledgers.size());

        return !aborted;
    }

    // Add transaction data for each ledger
    bool
    addTransactions()
    {
        JLOG(journal.info())
            << "Adding transaction data for " << ledgers.size() << " ledgers";

        std::vector<std::streampos> headerPositions;
        headerPositions.reserve(ledgers.size());

        for (auto const& ledger : ledgers)
        {
            if (aborted)
                return false;

            std::streampos headerStart;
            if (!serializeLedgerTransactions(*ledger, headerStart))
            {
                aborted = true;
                return false;
            }

            headerPositions.push_back(headerStart);
        }

        // Now update all the next header offsets
        for (size_t i = 0; i < headerPositions.size() - 1; ++i)
        {
            std::lock_guard<std::mutex> lock(fileMutex);

            outfile.seekp(headerPositions[i]);
            if (outfile.fail())
            {
                JLOG(journal.error()) << "Failed to seek to header position";
                aborted = true;
                return false;
            }

            uint64_t nextOffset = headerPositions[i + 1];
            outfile.write(
                reinterpret_cast<const char*>(&nextOffset), sizeof(nextOffset));
            if (outfile.fail())
            {
                JLOG(journal.error()) << "Failed to write next header offset";
                aborted = true;
                return false;
            }
        }

        JLOG(journal.info()) << "Completed adding transaction data";
        return true;
    }

    // Check if the process was aborted
    bool
    wasAborted() const
    {
        return aborted;
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
    if (!processor.streamAll())
    {
        return rpcError(rpcINTERNAL, "Error occurred while processing ledgers");
    }

    // Remember where we are after all state data
    auto ledgerTxOffset = outfile.tellp();

    // Now add transaction data
    if (!processor.addTransactions())
    {
        return rpcError(
            rpcINTERNAL, "Error occurred while processing transaction data");
    }

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

    // Validate the newly created file
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

    // Maps to store nodes for each ledger sequence
    std::map<uint32_t, std::map<uint256, std::tuple<Blob, SHAMapNodeID, bool>>>
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
    std::streampos currentPos = infile.tellg();
    while (currentPos < header.ledger_tx_offset && !infile.eof())
    {
        NodeHeader nodeHeader;
        infile.read(reinterpret_cast<char*>(&nodeHeader), sizeof(nodeHeader));
        if (infile.fail())
            break;

        stateNodeCount++;

        // Validate node header
        if (nodeHeader.sequence < header.min_ledger ||
            nodeHeader.sequence > header.max_ledger)
        {
            JLOG(context.j.error())
                << "Invalid node sequence: " << nodeHeader.sequence;
            return rpcError(
                rpcINTERNAL, "Corrupted catalogue file: invalid node sequence");
        }

        // Count by delta type
        if (nodeHeader.deltaType == DeltaType::ADDED)
            addedCount++;
        else if (nodeHeader.deltaType == DeltaType::MODIFIED)
            modifiedCount++;
        else if (nodeHeader.deltaType == DeltaType::REMOVED)
            removedCount++;

        // Read the node data
        Blob nodeData;
        if (nodeHeader.size > 0)
        {
            // Add a reasonable size limit as a safety check
            if (nodeHeader.size > 1024 * 1024)  // 1MB
            {
                JLOG(context.j.error())
                    << "Suspiciously large node size: " << nodeHeader.size;
                return rpcError(
                    rpcINTERNAL,
                    "Corrupted catalogue file: unreasonable node size");
            }

            nodeData.resize(nodeHeader.size);
            infile.read(
                reinterpret_cast<char*>(nodeData.data()), nodeHeader.size);
            if (infile.fail())
                break;
        }

        // Store the node data and track deltas
        SHAMapNodeID nodeID;
        if (nodeHeader.isRoot)
        {
            // Root node has depth 0
            nodeID = SHAMapNodeID();
        }
        else if (
            static_cast<CatalogueNodeType>(nodeHeader.type) ==
            CatalogueNodeType::INNER)
        {
            // For inner nodes, create a basic ID - proper paths created during
            // tree building
            nodeID = SHAMapNodeID::createID(0, nodeHeader.nodeID);
        }
        else
        {
            // For leaf nodes, use leaf depth
            nodeID = SHAMapNodeID(SHAMap::leafDepth, nodeHeader.nodeID);
        }

        // Store the node by ledger sequence and hash
        bool isRoot = (nodeHeader.isRoot == 1);
        nodesByLedger[nodeHeader.sequence][nodeHeader.hash] =
            std::make_tuple(std::move(nodeData), nodeID, isRoot);

        // Track deltas for non-base ledgers
        if (nodeHeader.sequence != header.base_ledger_seq)
        {
            deltasByLedger[nodeHeader.sequence].emplace_back(
                nodeHeader.hash, static_cast<DeltaType>(nodeHeader.deltaType));
        }

        // If this is a root node, store its hash
        if (isRoot)
        {
            rootHashesByLedger[nodeHeader.sequence] = nodeHeader.hash;
        }

        currentPos = infile.tellg();
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

    // Read transaction data for each ledger
    while (!infile.eof())
    {
        uint64_t nextOffset;
        infile.read(reinterpret_cast<char*>(&nextOffset), sizeof(nextOffset));
        if (infile.fail() || infile.eof())
            break;

        LedgerInfo info;
        infile.read(reinterpret_cast<char*>(&info.seq), sizeof(info.seq));
        if (infile.fail())
            break;

        // Validate ledger sequence
        if (info.seq < header.min_ledger || info.seq > header.max_ledger)
        {
            JLOG(context.j.error())
                << "Invalid transaction ledger sequence: " << info.seq;
            return rpcError(
                rpcINTERNAL,
                "Corrupted catalogue file: invalid tx ledger sequence");
        }

        infile.read(
            reinterpret_cast<char*>(&info.parentCloseTime),
            sizeof(info.parentCloseTime));

        // Read hash values using a buffer
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
        while ((nextOffset == 0 || currentPos < nextOffset) && !infile.eof())
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

            // Safety check
            if (txnSize > 1024 * 1024)  // 1MB
            {
                JLOG(context.j.error())
                    << "Suspiciously large transaction size: " << txnSize;
                return rpcError(
                    rpcINTERNAL,
                    "Corrupted catalogue file: unreasonable tx size");
            }

            std::vector<uint8_t> txnData(txnSize);
            infile.read(reinterpret_cast<char*>(txnData.data()), txnSize);

            // Read metadata if present
            uint32_t metaSize;
            infile.read(reinterpret_cast<char*>(&metaSize), sizeof(metaSize));

            std::vector<uint8_t> metaData;
            if (metaSize > 0)
            {
                // Safety check
                if (metaSize > 1024 * 1024)  // 1MB
                {
                    JLOG(context.j.error())
                        << "Suspiciously large metadata size: " << metaSize;
                    return rpcError(
                        rpcINTERNAL,
                        "Corrupted catalogue file: unreasonable metadata size");
                }

                metaData.resize(metaSize);
                infile.read(reinterpret_cast<char*>(metaData.data()), metaSize);
            }

            try
            {
                // Create transaction objects
                auto txn = std::make_shared<STTx>(
                    SerialIter{txnData.data(), txnData.size()});

                std::shared_ptr<TxMeta> meta;
                if (!metaData.empty())
                {
                    meta = std::make_shared<TxMeta>(txID, info.seq, metaData);
                }

                txsByLedger[info.seq].emplace_back(txn, meta);
                txCount++;
            }
            catch (std::exception const& e)
            {
                JLOG(context.j.error())
                    << "Error processing transaction: " << e.what();
                // Continue with other transactions
            }

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

    // Close the file as we've read all the data we need
    infile.close();

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

            // Find the root node
            auto const rootHash = rootIt->second;
            auto nodesIt = nodesByLedger.find(seq);
            if (nodesIt == nodesByLedger.end())
            {
                JLOG(context.j.error())
                    << "Missing nodes for base ledger " << seq;
                continue;
            }

            auto const& nodesForLedger = nodesIt->second;
            auto rootNodeIt = nodesForLedger.find(rootHash);
            if (rootNodeIt == nodesForLedger.end())
            {
                JLOG(context.j.error())
                    << "Missing root node data for base ledger " << seq;
                continue;
            }

            // Create the ledger with the info
            ledger = std::make_shared<Ledger>(
                infoIt->second,
                context.app.config(),
                context.app.getNodeFamily());

            // Now build the state tree by adding nodes
            auto const& [rootData, rootNodeID, isRoot] = rootNodeIt->second;
            JLOG(context.j.info()) << "Loading root node for ledger " << seq
                                   << ", hash: " << to_string(rootHash)
                                   << ", data size: " << rootData.size();

            // Debug output of first bytes
            if (rootData.size() >= 16)
            {
                std::stringstream ss;
                ss << "Root data first 16 bytes: ";
                for (size_t i = 0; i < 16; ++i)
                    ss << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<int>(rootData[i]) << " ";
                JLOG(context.j.info()) << ss.str();
            }

            try
            {
                // Convert from prefix format to wire format for the root node
                auto rootNode = SHAMapTreeNode::makeFromPrefix(
                    Slice(rootData.data(), rootData.size()),
                    SHAMapHash(rootHash));

                if (!rootNode)
                {
                    JLOG(context.j.error())
                        << "Failed to create root node from prefix data";
                    continue;
                }

                // Now serialize it in wire format for addRootNode
                Serializer s;
                rootNode->serializeForWire(s);

                if (!ledger->stateMap()
                         .addRootNode(SHAMapHash(rootHash), s.slice(), nullptr)
                         .isGood())
                {
                    JLOG(context.j.error()) << "Failed to add root node";
                    continue;
                }

                // Build a map of parent-child relationships and proper node IDs
                std::map<uint256, std::vector<std::pair<uint256, int>>>
                    childrenByParent;
                std::map<uint256, SHAMapNodeID> nodeIDMap;

                // Initialize with the root
                nodeIDMap[rootHash] = SHAMapNodeID();

                // First pass: Build parent-child relationships
                for (auto const& [hash, nodeTuple] : nodesForLedger)
                {
                    auto const& nodeData = std::get<0>(nodeTuple);
                    try
                    {
                        auto node = SHAMapTreeNode::makeFromPrefix(
                            Slice(nodeData.data(), nodeData.size()),
                            SHAMapHash(hash));

                        if (node && node->isInner())
                        {
                            auto inner =
                                std::static_pointer_cast<SHAMapInnerNode>(node);
                            for (int i = 0; i < 16; ++i)
                            {
                                if (!inner->isEmptyBranch(i))
                                {
                                    auto childHash =
                                        inner->getChildHash(i).as_uint256();
                                    childrenByParent[hash].emplace_back(
                                        childHash, i);
                                }
                            }
                        }
                    }
                    catch (std::exception const& e)
                    {
                        JLOG(context.j.warn())
                            << "Error analyzing node " << to_string(hash)
                            << ": " << e.what();
                    }
                }

                // Second pass: Build a queue of nodes to process with proper
                // node IDs
                std::queue<std::pair<uint256, SHAMapNodeID>> nodeQueue;
                std::set<uint256> processedNodes;

                // Start with the root (already added)
                processedNodes.insert(rootHash);

                // Queue the root's children
                for (auto const& [childHash, branch] :
                     childrenByParent[rootHash])
                {
                    SHAMapNodeID childID =
                        SHAMapNodeID().getChildNodeID(branch);
                    nodeQueue.push({childHash, childID});
                    nodeIDMap[childHash] = childID;
                }

                // Process all nodes in breadth-first order
                while (!nodeQueue.empty())
                {
                    auto [nodeHash, nodeID] = nodeQueue.front();
                    nodeQueue.pop();

                    // Skip if already processed
                    if (processedNodes.find(nodeHash) != processedNodes.end())
                        continue;

                    processedNodes.insert(nodeHash);

                    // Find this node in our data
                    auto nodeIt = nodesForLedger.find(nodeHash);
                    if (nodeIt == nodesForLedger.end())
                    {
                        JLOG(context.j.error())
                            << "Missing node data for " << to_string(nodeHash);
                        continue;
                    }

                    auto const& [nodeData, origNodeID, isNodeRoot] =
                        nodeIt->second;

                    try
                    {
                        // Convert from prefix format to wire format for adding
                        // node
                        auto node = SHAMapTreeNode::makeFromPrefix(
                            Slice(nodeData.data(), nodeData.size()),
                            SHAMapHash(nodeHash));

                        if (!node)
                        {
                            JLOG(context.j.error())
                                << "Failed to create node from prefix data";
                            continue;
                        }

                        // Now serialize it in wire format
                        Serializer s;
                        node->serializeForWire(s);

                        // Add to the map with proper nodeID
                        auto result = ledger->stateMap().addKnownNode(
                            nodeID, s.slice(), nullptr);

                        if (!result.isGood())
                        {
                            JLOG(context.j.warn())
                                << "Failed to add node " << to_string(nodeHash)
                                << " result: " << result.get();
                        }

                        // Queue children if this is an inner node
                        if (node->isInner() && result.isGood())
                        {
                            for (auto const& [childHash, branch] :
                                 childrenByParent[nodeHash])
                            {
                                SHAMapNodeID childID =
                                    nodeID.getChildNodeID(branch);
                                nodeQueue.push({childHash, childID});
                                nodeIDMap[childHash] = childID;
                            }
                        }
                    }
                    catch (std::exception const& e)
                    {
                        JLOG(context.j.error())
                            << "Error processing node " << to_string(nodeHash)
                            << ": " << e.what();
                    }
                }
            }
            catch (std::exception const& e)
            {
                JLOG(context.j.error())
                    << "Exception processing base ledger: " << e.what();
                continue;
            }
        }
        else
        {
            // For non-base ledgers, start with a copy of the previous ledger
            if (!prevLedger)
            {
                JLOG(context.j.error())
                    << "Missing previous ledger for delta update at ledger "
                    << seq;
                continue;
            }

            // Create the ledger from the previous one
            ledger =
                std::make_shared<Ledger>(*prevLedger, infoIt->second.closeTime);

            // Apply deltas from previous to current ledger
            auto deltaIt = deltasByLedger.find(seq);
            if (deltaIt != deltasByLedger.end())
            {
                auto& deltas = deltaIt->second;
                auto& nodesForLedger = nodesByLedger[seq];

                for (auto const& [hash, deltaType] : deltas)
                {
                    if (deltaType == DeltaType::REMOVED)
                    {
                        // Handle removal - find the key to remove
                        auto nodeIt = nodesForLedger.find(hash);
                        if (nodeIt != nodesForLedger.end())
                        {
                            uint256 const& key =
                                std::get<1>(nodeIt->second).getNodeID();
                            ledger->rawErase(key);
                        }
                    }
                    else
                    {
                        // For added/modified nodes, find and apply
                        auto nodeIt = nodesForLedger.find(hash);
                        if (nodeIt != nodesForLedger.end())
                        {
                            auto const& [nodeData, nodeID, isNodeRoot] =
                                nodeIt->second;

                            try
                            {
                                // Convert from prefix format to wire format
                                auto node = SHAMapTreeNode::makeFromPrefix(
                                    Slice(nodeData.data(), nodeData.size()),
                                    SHAMapHash(hash));

                                if (!node)
                                {
                                    JLOG(context.j.error())
                                        << "Failed to create node from prefix "
                                           "data";
                                    continue;
                                }

                                if (node->isLeaf())
                                {
                                    auto leaf = std::static_pointer_cast<
                                        SHAMapLeafNode>(node);
                                    auto item = leaf->peekItem();

                                    SHAMapNodeType nodeType = leaf->getType();

                                    if (deltaType == DeltaType::ADDED)
                                    {
                                        ledger->stateMap().addItem(
                                            nodeType, item);
                                    }
                                    else
                                    {
                                        ledger->stateMap().updateGiveItem(
                                            nodeType, item);
                                    }
                                }
                                else
                                {
                                    // Serialize for wire format for inner nodes
                                    Serializer s;
                                    node->serializeForWire(s);

                                    // Add node directly to the tree with proper
                                    // node ID
                                    ledger->stateMap().addKnownNode(
                                        nodeID, s.slice(), nullptr);
                                }
                            }
                            catch (std::exception const& e)
                            {
                                JLOG(context.j.error())
                                    << "Error processing delta node: "
                                    << e.what();
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

        ledger->setImmutable(true);

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
