#ifndef RIPPLE_NODESTORE_MYSQLBACKEND_H_INCLUDED
#define RIPPLE_NODESTORE_MYSQLBACKEND_H_INCLUDED

#include <ripple/basics/contract.h>
#include <ripple/nodestore/Factory.h>
#include <ripple/nodestore/Manager.h>
#include <ripple/nodestore/impl/DecodedBlob.h>
#include <ripple/nodestore/impl/EncodedBlob.h>
#include <ripple/nodestore/impl/codec.h>
#include <boost/beast/core/string.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <mysql/mysql.h>
#include <queue>
#include <sstream>
#include <thread>

namespace ripple {
namespace NodeStore {

// SQL statements as constants
static constexpr auto CREATE_DATABASE = R"SQL(
    CREATE DATABASE IF NOT EXISTS `%s` 
    CHARACTER SET utf8mb4 
    COLLATE utf8mb4_unicode_ci
)SQL";

static constexpr auto CREATE_TABLE = R"SQL(
    CREATE TABLE IF NOT EXISTS `%s` (
        hash BINARY(32) PRIMARY KEY,
        data MEDIUMBLOB NOT NULL,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        INDEX idx_created_at (created_at)
    ) ENGINE=InnoDB
)SQL";

static constexpr auto INSERT_NODE = R"SQL(
    INSERT INTO %s (hash, data) 
    VALUES (?, ?) 
    ON DUPLICATE KEY UPDATE data = VALUES(data)
)SQL";

static constexpr auto SET_ISOLATION_LEVEL = R"SQL(
    SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED
)SQL";

class MySQLConnection
{
private:
    std::unique_ptr<MYSQL, decltype(&mysql_close)> mysql_;
    Config const& config_;
    beast::Journal journal_;
    static constexpr int MAX_RETRY_ATTEMPTS = 3;
    static constexpr auto RETRY_DELAY_MS = 1000;

    bool
    connect()
    {
        mysql_.reset(mysql_init(nullptr));
        if (!mysql_)
            return false;

        // Set connection options
        unsigned int timeout = 5;
        mysql_options(mysql_.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        uint8_t const reconnect = 1;
        mysql_options(mysql_.get(), MYSQL_OPT_RECONNECT, &reconnect);

        // Connect without database first
        auto* conn = mysql_real_connect(
            mysql_.get(),
            config_.mysql->host.c_str(),
            config_.mysql->user.c_str(),
            config_.mysql->pass.c_str(),
            nullptr,  // No database selected yet
            config_.mysql->port,
            nullptr,
            CLIENT_MULTI_STATEMENTS);

        if (!conn)
            return false;

        // Set isolation level for dirty reads
        if (mysql_query(mysql_.get(), SET_ISOLATION_LEVEL))
        {
            JLOG(journal_.warn()) << "Failed to set isolation level: "
                                  << mysql_error(mysql_.get());
            return false;
        }

        // Create database (unconditionally)
        std::string query(1024, '\0');
        int length = snprintf(
            &query[0],
            query.size(),
            CREATE_DATABASE,
            config_.mysql->name.c_str());
        query.resize(length);

        if (mysql_query(mysql_.get(), query.c_str()))
        {
            JLOG(journal_.error())
                << "Failed to create database: " << mysql_error(mysql_.get());
            return false;
        }

        // Now select the database
        if (mysql_select_db(mysql_.get(), config_.mysql->name.c_str()))
        {
            JLOG(journal_.error())
                << "Failed to select database: " << mysql_error(mysql_.get());
            return false;
        }

        return true;
    }

public:
    MySQLConnection(Config const& config, beast::Journal journal)
        : mysql_(nullptr, mysql_close), config_(config), journal_(journal)
    {
        if (!config_.mysql.has_value())
            throw std::runtime_error(
                "[mysql_settings] stanza missing from config!");

        if (config_.mysql->name.empty())
            throw std::runtime_error(
                "Database name missing from mysql_settings!");

        if (!connect())
        {
            Throw<std::runtime_error>(
                std::string("Failed to connect to MySQL: ") +
                (mysql_ ? mysql_error(mysql_.get()) : "initialization failed"));
        }
    }

    MYSQL*
    get()
    {
        return mysql_.get();
    }

    bool
    ensureConnection()
    {
        for (int attempt = 0; attempt < MAX_RETRY_ATTEMPTS; ++attempt)
        {
            if (!mysql_ || mysql_ping(mysql_.get()) != 0)
            {
                JLOG(journal_.warn())
                    << "MySQL connection lost, attempting reconnect (attempt "
                    << (attempt + 1) << "/" << MAX_RETRY_ATTEMPTS << ")";

                if (connect())
                    return true;

                if (attempt < MAX_RETRY_ATTEMPTS - 1)
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(RETRY_DELAY_MS));
                }
            }
            else
            {
                return true;
            }
        }
        return false;
    }

    // Helper method to execute a query with retry logic
    bool
    executeQuery(std::string const& query)
    {
        for (int attempt = 0; attempt < MAX_RETRY_ATTEMPTS; ++attempt)
        {
            if (ensureConnection() && !mysql_query(mysql_.get(), query.c_str()))
                return true;

            if (attempt < MAX_RETRY_ATTEMPTS - 1)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(RETRY_DELAY_MS));
            }
        }
        return false;
    }
};

static thread_local std::unique_ptr<MySQLConnection> threadConnection_;

class MySQLBackend : public Backend
{
private:
    std::string name_;
    beast::Journal journal_;
    bool isOpen_{false};
    Config const& config_;
    static constexpr std::size_t BATCH_SIZE = 1000;
    static constexpr std::size_t MAX_CACHE_SIZE =
        100000;  // Maximum number of entries
    static constexpr std::size_t CACHE_CLEANUP_THRESHOLD =
        120000;  // When to trigger cleanup

    using DataStore = std::map<uint256, std::vector<std::uint8_t>>;
    DataStore cache_;
    std::mutex cacheMutex_;

    // LRU tracking for cache management
    struct CacheEntry
    {
        std::chrono::steady_clock::time_point last_access;
        size_t size;
        bool pending{false};
    };

    std::map<uint256, CacheEntry> cacheMetadata_;
    std::mutex metadataMutex_;
    std::atomic<size_t> currentCacheSize_{0};

    // Background write queue
    struct WriteOp
    {
        uint256 hash;
        std::vector<std::uint8_t> data;
    };
    std::queue<WriteOp> writeQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::atomic<bool> shouldStop_{false};
    std::thread writeThread_;

    MySQLConnection*
    getConnection()
    {
        if (!threadConnection_)
        {
            threadConnection_ =
                std::make_unique<MySQLConnection>(config_, journal_);
        }
        return threadConnection_.get();
    }

    std::string
    sanitizeTableName(std::string name)
    {
        name.erase(
            std::unique(
                name.begin(),
                std::transform(
                    name.begin(),
                    name.end(),
                    name.begin(),
                    [](char c) { return std::isalnum(c) ? c : '_'; })),
            name.end());
        return "nodes_" + name;
    }

    void
    cleanupCache()
    {
        if (currentCacheSize_.load() < CACHE_CLEANUP_THRESHOLD)
            return;

        // Collect entries sorted by last access time
        std::vector<std::pair<uint256, std::chrono::steady_clock::time_point>>
            entries;
        {
            std::lock_guard<std::mutex> metadataLock(metadataMutex_);
            for (const auto& [hash, metadata] : cacheMetadata_)
            {
                if (!metadata.pending)
                    entries.emplace_back(hash, metadata.last_access);
            }
        }

        // Sort by access time, oldest first
        std::sort(
            entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.second < b.second;
            });

        // Remove oldest entries until we're below target size
        size_t removedSize = 0;
        for (const auto& entry : entries)
        {
            if (currentCacheSize_.load() <= MAX_CACHE_SIZE)
                break;

            {
                std::lock_guard<std::mutex> metadataLock(metadataMutex_);
                auto metaIt = cacheMetadata_.find(entry.first);
                if (metaIt != cacheMetadata_.end())
                {
                    removedSize += metaIt->second.size;
                    cacheMetadata_.erase(metaIt);
                }
            }
            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex_);
                cache_.erase(entry.first);
            }
            currentCacheSize_--;
        }

        JLOG(journal_.debug())
            << "Cache cleanup removed " << removedSize
            << " bytes, current size: " << currentCacheSize_.load();
    }

    void
    updateCacheMetadata(const uint256& hash, size_t size)
    {
        CacheEntry entry{std::chrono::steady_clock::now(), size};
        {
            std::lock_guard<std::mutex> metadataLock(metadataMutex_);
            cacheMetadata_[hash] = entry;
        }

        if (++currentCacheSize_ >= CACHE_CLEANUP_THRESHOLD)
        {
            cleanupCache();
        }
    }

    Status
    fetch(void const* key, std::shared_ptr<NodeObject>* pObject) override
    {
        if (!isOpen_)
            return notFound;

        uint256 const hash(uint256::fromVoid(key));

        // Check cache first
        {
            std::lock_guard<std::mutex> cacheLock(cacheMutex_);
            auto it = cache_.find(hash);
            if (it != cache_.end())
            {
                // Update access time
                {
                    std::lock_guard<std::mutex> metadataLock(metadataMutex_);
                    auto metaIt = cacheMetadata_.find(hash);
                    if (metaIt != cacheMetadata_.end())
                    {
                        metaIt->second.last_access =
                            std::chrono::steady_clock::now();
                    }
                }

                nudb::detail::buffer decompressed;
                auto const result = nodeobject_decompress(
                    it->second.data(), it->second.size(), decompressed);

                DecodedBlob decoded(hash.data(), result.first, result.second);
                if (decoded.wasOk())
                {
                    *pObject = decoded.createObject();
                    return ok;
                }
            }
        }

        // If not in cache, fetch from MySQL
        return fetchFromMySQL(key, pObject);
    }

    void
    startWriteThread()
    {
        writeThread_ = std::thread([this]() {
            while (!shouldStop_)
            {
                std::vector<WriteOp> batch;
                {
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    queueCV_.wait_for(
                        lock, std::chrono::milliseconds(100), [this]() {
                            return !writeQueue_.empty() || shouldStop_;
                        });

                    // Grab up to BATCH_SIZE operations
                    while (!writeQueue_.empty() && batch.size() < BATCH_SIZE)
                    {
                        batch.push_back(std::move(writeQueue_.front()));
                        writeQueue_.pop();
                    }
                }

                if (!batch.empty())
                {
                    auto* conn = getConnection();
                    if (!conn->ensureConnection())
                        continue;

                    if (mysql_query(conn->get(), "START TRANSACTION"))
                        continue;

                    bool success = true;
                    for (auto const& op : batch)
                    {
                        MYSQL_STMT* stmt = mysql_stmt_init(conn->get());
                        if (!stmt)
                        {
                            success = false;
                            break;
                        }

                        std::string const sql = "INSERT INTO " + name_ +
                            " (hash, data) VALUES (?, ?) " +
                            "ON DUPLICATE KEY UPDATE data = VALUES(data)";

                        if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()))
                        {
                            mysql_stmt_close(stmt);
                            success = false;
                            break;
                        }

                        MYSQL_BIND bind[2];
                        std::memset(bind, 0, sizeof(bind));

                        bind[0].buffer_type = MYSQL_TYPE_BLOB;
                        bind[0].buffer = const_cast<void*>(
                            static_cast<void const*>(op.hash.data()));
                        bind[0].buffer_length = op.hash.size();

                        bind[1].buffer_type = MYSQL_TYPE_BLOB;
                        bind[1].buffer = const_cast<uint8_t*>(op.data.data());
                        bind[1].buffer_length = op.data.size();

                        if (mysql_stmt_bind_param(stmt, bind))
                        {
                            mysql_stmt_close(stmt);
                            success = false;
                            break;
                        }

                        if (mysql_stmt_execute(stmt))
                        {
                            mysql_stmt_close(stmt);
                            success = false;
                            break;
                        }

                        mysql_stmt_close(stmt);
                    }

                    if (success)
                    {
                        if (mysql_query(conn->get(), "COMMIT") == 0)
                        {
                            // Clear pending flag for successfully written
                            // entries
                            std::lock_guard<std::mutex> metadataLock(
                                metadataMutex_);
                            for (const auto& op : batch)
                            {
                                auto it = cacheMetadata_.find(op.hash);
                                if (it != cacheMetadata_.end())
                                    it->second.pending = false;
                            }
                        }
                    }
                    else
                        mysql_query(conn->get(), "ROLLBACK");
                }
            }
        });
    }

    void
    queueWrite(uint256 const& hash, std::vector<std::uint8_t> const& data)
    {
        {
            std::lock_guard<std::mutex> metadataLock(metadataMutex_);
            auto& entry = cacheMetadata_[hash];
            entry.pending = true;
        }
        std::lock_guard<std::mutex> lock(queueMutex_);
        writeQueue_.push({hash, data});
        queueCV_.notify_one();
    }

    Status
    fetchFromMySQL(void const* key, std::shared_ptr<NodeObject>* pObject)
    {
        auto* conn = getConnection();
        if (!conn->ensureConnection())
        {
            JLOG(journal_.warn()) << "fetch: Failed to ensure connection";
            return dataCorrupt;
        }

        uint256 const hash(uint256::fromVoid(key));

        MYSQL_STMT* stmt = mysql_stmt_init(conn->get());
        if (!stmt)
            return dataCorrupt;

        std::string const sql = "SELECT data FROM " + name_ + " WHERE hash = ?";

        if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        MYSQL_BIND bindParam;
        std::memset(&bindParam, 0, sizeof(bindParam));
        bindParam.buffer_type = MYSQL_TYPE_BLOB;
        bindParam.buffer =
            const_cast<void*>(static_cast<void const*>(hash.data()));
        bindParam.buffer_length = hash.size();

        if (mysql_stmt_bind_param(stmt, &bindParam))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        if (mysql_stmt_execute(stmt))
        {
            mysql_stmt_close(stmt);
            return notFound;
        }

        MYSQL_BIND bindResult;
        std::memset(&bindResult, 0, sizeof(bindResult));
        uint64_t length = 0;
        bool is_null = false;
        bindResult.buffer_type = MYSQL_TYPE_BLOB;
        bindResult.length = &length;
        bindResult.is_null = &is_null;

        if (mysql_stmt_bind_result(stmt, &bindResult))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        if (mysql_stmt_store_result(stmt))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        if (mysql_stmt_num_rows(stmt) == 0)
        {
            mysql_stmt_close(stmt);
            return notFound;
        }

        std::vector<uint8_t> buffer(16 * 1024 * 1024);  // 16MB buffer
        bindResult.buffer = buffer.data();
        bindResult.buffer_length = buffer.size();

        if (mysql_stmt_fetch(stmt))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        mysql_stmt_close(stmt);

        // Add to cache
        std::vector<uint8_t> cached_data(
            buffer.begin(), buffer.begin() + length);
        cache_.insert_or_assign(hash, cached_data);
        updateCacheMetadata(hash, length);

        nudb::detail::buffer decompressed;
        auto const result = nodeobject_decompress(
            cached_data.data(), cached_data.size(), decompressed);

        DecodedBlob decoded(hash.data(), result.first, result.second);
        if (!decoded.wasOk())
            return dataCorrupt;

        *pObject = decoded.createObject();
        return ok;
    }

public:
    MySQLBackend(
        std::size_t keyBytes,
        Section const& keyValues,
        beast::Journal journal)
        : name_(sanitizeTableName(get(keyValues, "path", "nodestore")))
        , journal_(journal)
        , config_(keyValues.getParent())
    {
        startWriteThread();
    }

    ~MySQLBackend()
    {
        shouldStop_ = true;
        queueCV_.notify_all();
        if (writeThread_.joinable())
            writeThread_.join();
    }

    std::string
    getName() override
    {
        return name_;
    }

    void
    open(bool createIfMissing) override
    {
        if (isOpen_)
            Throw<std::runtime_error>("database already open");

        auto* conn = getConnection();
        if (!conn->ensureConnection())
            Throw<std::runtime_error>("Failed to establish MySQL connection");

        if (createIfMissing)
            createTable();

        isOpen_ = true;
    }

    bool
    isOpen() override
    {
        return isOpen_;
    }

    void
    close() override
    {
        // Wait for write queue to empty
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            while (!writeQueue_.empty())
            {
                queueCV_.wait(lock);
            }
        }

        threadConnection_.reset();
        cache_.clear();
        cacheMetadata_.clear();
        currentCacheSize_ = 0;
        isOpen_ = false;
    }

    std::pair<std::vector<std::shared_ptr<NodeObject>>, Status>
    fetchBatch(std::vector<uint256 const*> const& hashes) override
    {
        std::vector<std::shared_ptr<NodeObject>> results;
        results.reserve(hashes.size());

        std::vector<uint256 const*> mysqlFetch;
        mysqlFetch.reserve(hashes.size());

        // First try cache
        for (auto const& h : hashes)
        {
            auto it = cache_.find(*h);
            if (it != cache_.end())
            {
                // Update access time
                auto metaIt = cacheMetadata_.find(*h);
                if (metaIt != cacheMetadata_.end())
                {
                    metaIt->second.last_access =
                        std::chrono::steady_clock::now();
                }

                nudb::detail::buffer decompressed;
                auto const result = nodeobject_decompress(
                    it->second.data(), it->second.size(), decompressed);

                DecodedBlob decoded(h->data(), result.first, result.second);
                if (decoded.wasOk())
                {
                    results.push_back(decoded.createObject());
                    continue;
                }
            }

            mysqlFetch.push_back(h);
            results.push_back(nullptr);  // Placeholder for MySQL fetch
        }

        // If everything was in cache, return early
        if (mysqlFetch.empty())
            return {results, ok};

        // Fetch remaining from MySQL
        auto* conn = getConnection();
        if (!conn->ensureConnection())
            return {results, dataCorrupt};

        if (mysql_query(conn->get(), "START TRANSACTION"))
            return {results, dataCorrupt};

        try
        {
            for (size_t i = 0; i < mysqlFetch.size(); ++i)
            {
                std::shared_ptr<NodeObject> nObj;
                Status status = fetchFromMySQL(mysqlFetch[i]->data(), &nObj);

                // Find the original position in results
                auto originalPos = std::distance(
                    hashes.begin(),
                    std::find(hashes.begin(), hashes.end(), mysqlFetch[i]));

                results[originalPos] = (status == ok ? nObj : nullptr);
            }

            if (mysql_query(conn->get(), "COMMIT"))
                return {results, dataCorrupt};

            return {results, ok};
        }
        catch (...)
        {
            mysql_query(conn->get(), "ROLLBACK");
            throw;
        }
    }

    void
    store(std::shared_ptr<NodeObject> const& object) override
    {
        if (!isOpen_ || !object)
            return;

        EncodedBlob encoded(object);
        nudb::detail::buffer compressed;
        auto const result = nodeobject_compress(
            encoded.getData(), encoded.getSize(), compressed);

        std::vector<std::uint8_t> data(
            static_cast<const std::uint8_t*>(result.first),
            static_cast<const std::uint8_t*>(result.first) + result.second);

        // Update cache immediately
        cache_.insert_or_assign(object->getHash(), data);
        updateCacheMetadata(object->getHash(), data.size());

        // Queue async write to MySQL
        queueWrite(object->getHash(), data);
    }

    void
    storeBatch(Batch const& batch) override
    {
        for (auto const& e : batch)
        {
            if (!e)
                continue;

            EncodedBlob encoded(e);
            nudb::detail::buffer compressed;
            auto const result = nodeobject_compress(
                encoded.getData(), encoded.getSize(), compressed);

            std::vector<std::uint8_t> data(
                static_cast<const std::uint8_t*>(result.first),
                static_cast<const std::uint8_t*>(result.first) + result.second);

            // Update cache immediately
            cache_.insert_or_assign(e->getHash(), data);
            updateCacheMetadata(e->getHash(), data.size());

            // Queue async write to MySQL
            queueWrite(e->getHash(), data);
        }
    }

    void
    sync() override
    {
        // Wait for write queue to empty
        std::unique_lock<std::mutex> lock(queueMutex_);
        while (!writeQueue_.empty())
        {
            queueCV_.wait(lock);
        }
    }

    void
    for_each(std::function<void(std::shared_ptr<NodeObject>)> f) override
    {
        if (!isOpen_)
            return;

        // First, process all cached entries
        std::vector<std::pair<uint256, std::vector<std::uint8_t>>>
            cached_entries;
        for (const auto& entry : cache_)
        {
            cached_entries.push_back(entry);
        }

        for (const auto& entry : cached_entries)
        {
            nudb::detail::buffer decompressed;
            auto const result = nodeobject_decompress(
                entry.second.data(), entry.second.size(), decompressed);

            DecodedBlob decoded(
                entry.first.data(), result.first, result.second);
            if (decoded.wasOk())
                f(decoded.createObject());
        }

        // Then fetch any remaining entries from MySQL
        auto* conn = getConnection();
        if (!conn->ensureConnection())
            return;

        if (mysql_query(
                conn->get(),
                ("SELECT hash, data FROM " + name_ + " ORDER BY created_at")
                    .c_str()))
            return;

        MYSQL_RES* result = mysql_store_result(conn->get());
        if (!result)
            return;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result)))
        {
            unsigned long* lengths = mysql_fetch_lengths(result);
            if (!lengths)
                continue;

            uint256 hash;
            std::memcpy(hash.data(), row[0], hash.size());

            // Skip if already processed from cache
            if (cache_.find(hash) != cache_.end())
                continue;

            nudb::detail::buffer decompressed;
            auto const decomp_result = nodeobject_decompress(
                row[1], static_cast<std::size_t>(lengths[1]), decompressed);

            DecodedBlob decoded(
                hash.data(), decomp_result.first, decomp_result.second);

            if (decoded.wasOk())
            {
                auto obj = decoded.createObject();
                f(obj);

                // Add to cache for future use
                std::vector<std::uint8_t> data(
                    reinterpret_cast<const std::uint8_t*>(row[1]),
                    reinterpret_cast<const std::uint8_t*>(row[1]) + lengths[1]);
                cache_.insert_or_assign(hash, std::move(data));
                updateCacheMetadata(hash, lengths[1]);
            }
        }

        mysql_free_result(result);
    }

    int
    getWriteLoad() override
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return static_cast<int>(writeQueue_.size());
    }

    void
    setDeletePath() override
    {
        close();
    }

    int
    fdRequired() const override
    {
        return 1;
    }

private:
    void
    createTable()
    {
        auto* conn = getConnection();
        if (!conn->ensureConnection())
            Throw<std::runtime_error>("Failed to connect to MySQL server");

        std::string query(1024, '\0');
        int length =
            snprintf(&query[0], query.size(), CREATE_TABLE, name_.c_str());
        query.resize(length);

        if (!conn->executeQuery(query))
        {
            JLOG(journal_.error())
                << "Failed to create table: " << mysql_error(conn->get());
            Throw<std::runtime_error>("Failed to create table");
        }
    }
};

class MySQLFactory : public Factory
{
public:
    MySQLFactory()
    {
        Manager::instance().insert(*this);
    }

    ~MySQLFactory() override
    {
        Manager::instance().erase(*this);
    }

    std::string
    getName() const override
    {
        return "MySQL";
    }

    std::unique_ptr<Backend>
    createInstance(
        std::size_t keyBytes,
        Section const& keyValues,
        std::size_t burstSize,
        Scheduler& scheduler,
        beast::Journal journal) override
    {
        return std::make_unique<MySQLBackend>(keyBytes, keyValues, journal);
    }
};

static MySQLFactory mysqlFactory;

}  // namespace NodeStore
}  // namespace ripple

#endif  // RIPPLE_NODESTORE_MYSQLBACKEND_H_INCLUDED
