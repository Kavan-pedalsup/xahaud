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
#include <memory>
#include <mysql/mysql.h>
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

    void
    createTable()  // Renamed from createDatabase to better reflect its purpose
    {
        auto* conn = getConnection();
        if (!conn->ensureConnection())
            Throw<std::runtime_error>("Failed to connect to MySQL server");

        // Create table only
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

public:
    MySQLBackend(
        std::size_t keyBytes,
        Section const& keyValues,
        beast::Journal journal)
        : name_(get(keyValues, "path", "nodestore"))
        , journal_(journal)
        , config_(keyValues.getParent())
    {
        // Sanitize table name
        name_.erase(
            std::unique(
                name_.begin(),
                std::transform(
                    name_.begin(),
                    name_.end(),
                    name_.begin(),
                    [](char c) { return std::isalnum(c) ? c : '_'; })),
            name_.end());

        name_ = "nodes_" + name_;
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
            createTable();  // Only create table if requested

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
        threadConnection_.reset();
        isOpen_ = false;
    }

    Status
    fetch(void const* key, std::shared_ptr<NodeObject>* pObject) override
    {
        if (!isOpen_)
        {
            std::cout << "fetch: Database not open\n";
            return notFound;
        }

        auto* conn = getConnection();
        if (!conn->ensureConnection())
        {
            std::cout << "fetch: Failed to ensure connection\n";
            return dataCorrupt;
        }

        uint256 const hash(uint256::fromVoid(key));

        MYSQL_STMT* stmt = mysql_stmt_init(conn->get());
        if (!stmt)
        {
            std::cout << "fetch: Failed to initialize prepared statement\n";
            return dataCorrupt;
        }

        std::string const sql = "SELECT data FROM " + name_ + " WHERE hash = ?";

        if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()))
        {
            std::cout << "fetch: Failed to prepare statement. Error: "
                      << mysql_stmt_error(stmt) << "\n";
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
            std::cout << "fetch: Failed to bind parameter. Error: "
                      << mysql_stmt_error(stmt) << "\n";
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        if (mysql_stmt_execute(stmt))
        {
            std::cout << "fetch: Failed to execute statement. Error: "
                      << mysql_stmt_error(stmt) << "\n";
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
            std::cout << "fetch: Failed to bind result. Error: "
                      << mysql_stmt_error(stmt) << "\n";
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        if (mysql_stmt_store_result(stmt))
        {
            std::cout << "fetch: Failed to store result. Error: "
                      << mysql_stmt_error(stmt) << "\n";
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        if (mysql_stmt_num_rows(stmt) == 0)
        {
            mysql_stmt_close(stmt);
            return notFound;
        }

        if (mysql_stmt_fetch(stmt))
        {
            std::cout << "fetch: Failed to fetch row. Error: "
                      << mysql_stmt_error(stmt) << "\n";
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        std::cout << "fetch: Retrieved data length: " << length << "\n";

        std::vector<uint8_t> buffer(length);
        bindResult.buffer = buffer.data();
        bindResult.buffer_length = length;

        if (mysql_stmt_fetch_column(stmt, &bindResult, 0, 0))
        {
            std::cout << "fetch: Failed to fetch column. Error: "
                      << mysql_stmt_error(stmt) << "\n";
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        mysql_stmt_close(stmt);

        nudb::detail::buffer decompressed;
        auto const result =
            nodeobject_decompress(buffer.data(), buffer.size(), decompressed);

        std::cout << "fetch: Decompression result - size: " << result.second
                  << ", success: " << (result.first != nullptr) << "\n";

        DecodedBlob decoded(hash.data(), result.first, result.second);
        if (!decoded.wasOk())
        {
            std::cout << "fetch: Blob decoding failed\n";
            return dataCorrupt;
        }

        *pObject = decoded.createObject();
        std::cout << "fetch: Successfully created object\n";
        return ok;
    }

    std::pair<std::vector<std::shared_ptr<NodeObject>>, Status>
    fetchBatch(std::vector<uint256 const*> const& hashes) override
    {
        std::vector<std::shared_ptr<NodeObject>> results;
        results.reserve(hashes.size());

        if (!isOpen_)
            return {results, notFound};

        auto* conn = getConnection();
        if (!conn->ensureConnection())
            return {results, dataCorrupt};

        if (mysql_query(conn->get(), "START TRANSACTION"))
            return {results, dataCorrupt};

        try
        {
            for (auto const& h : hashes)
            {
                std::shared_ptr<NodeObject> nObj;
                Status status = fetch(h->begin(), &nObj);
                results.push_back(status == ok ? nObj : nullptr);
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

        auto* conn = getConnection();
        if (!conn->ensureConnection())
            return;

        EncodedBlob encoded(object);
        nudb::detail::buffer compressed;
        auto const result = nodeobject_compress(
            encoded.getData(), encoded.getSize(), compressed);

        MYSQL_STMT* stmt = mysql_stmt_init(conn->get());
        if (!stmt)
            return;

        std::string const sql = "INSERT INTO " + name_ +
            " (hash, data) VALUES (?, ?) "
            "ON DUPLICATE KEY UPDATE data = VALUES(data)";

        if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()))
        {
            JLOG(journal_.error()) << "Failed to prepare MySQL statement: "
                                   << mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return;
        }

        MYSQL_BIND bind[2];
        std::memset(bind, 0, sizeof(bind));

        auto const& hash = object->getHash();
        bind[0].buffer_type = MYSQL_TYPE_BLOB;
        bind[0].buffer =
            const_cast<void*>(static_cast<void const*>(hash.data()));
        bind[0].buffer_length = hash.size();

        bind[1].buffer_type = MYSQL_TYPE_BLOB;
        bind[1].buffer =
            const_cast<void*>(static_cast<void const*>(result.first));
        bind[1].buffer_length = result.second;

        if (mysql_stmt_bind_param(stmt, bind))
        {
            mysql_stmt_close(stmt);
            return;
        }

        if (mysql_stmt_execute(stmt))
        {
            mysql_stmt_close(stmt);
            return;
        }

        mysql_stmt_close(stmt);
    }

    void
    storeBatch(Batch const& batch) override
    {
        if (!isOpen_)
            return;

        auto* conn = getConnection();
        if (!conn->ensureConnection())
            return;

        if (mysql_query(conn->get(), "START TRANSACTION"))
            return;

        try
        {
            for (auto const& e : batch)
                store(e);

            if (mysql_query(conn->get(), "COMMIT"))
                mysql_query(conn->get(), "ROLLBACK");
        }
        catch (...)
        {
            mysql_query(conn->get(), "ROLLBACK");
            throw;
        }
    }

    void
    sync() override
    {
    }

    void
    for_each(std::function<void(std::shared_ptr<NodeObject>)> f) override
    {
        if (!isOpen_)
            return;

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

            nudb::detail::buffer decompressed;
            auto const decomp_result = nodeobject_decompress(
                row[1], static_cast<std::size_t>(lengths[1]), decompressed);

            DecodedBlob decoded(
                row[0], decomp_result.first, decomp_result.second);

            if (decoded.wasOk())
                f(decoded.createObject());
        }

        mysql_free_result(result);
    }

    int
    getWriteLoad() override
    {
        return 0;
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
