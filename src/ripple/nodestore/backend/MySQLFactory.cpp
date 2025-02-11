#ifndef RIPPLE_NODESTORE_MYSQLBACKEND_H_INCLUDED
#define RIPPLE_NODESTORE_MYSQLBACKEND_H_INCLUDED

#include <ripple/basics/contract.h>
#include <ripple/nodestore/Factory.h>
#include <ripple/nodestore/Manager.h>
#include <ripple/nodestore/impl/DecodedBlob.h>
#include <ripple/nodestore/impl/EncodedBlob.h>
#include <ripple/nodestore/impl/codec.h>
#include <boost/beast/core/string.hpp>
#include <mysql/mysql.h>
#include <memory>
#include <cstdint>
#include <sstream>

namespace ripple {
namespace NodeStore {

class MySQLBackend : public Backend
{
private:
    std::string const name_;
    beast::Journal journal_;
    bool isOpen_{false};
    std::unique_ptr<MYSQL, decltype(&mysql_close)> mysql_;

    static constexpr auto CREATE_NODES_TABLE = R"SQL(
        CREATE TABLE IF NOT EXISTS nodes (
            hash BINARY(32) PRIMARY KEY,
            data MEDIUMBLOB NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        ) ENGINE=InnoDB
    )SQL";

public:
    MySQLBackend(
        std::size_t keyBytes,
        Section const& keyValues,
        beast::Journal journal)
        : name_(get(keyValues, "path", "nodestore"))
        , journal_(journal)
        , mysql_(mysql_init(nullptr), mysql_close)
    {
        if (!mysql_)
            Throw<std::runtime_error>("Failed to initialize MySQL");

        std::string const host = get(keyValues, "host", "localhost");
        std::string const user = get(keyValues, "user", "ripple");
        std::string const password = get(keyValues, "pass", "");
        std::string const database = get(keyValues, "db", "rippledb");
        uint16_t const port = 
            static_cast<uint16_t>(std::stoul(get(keyValues, "port", "3306")));

        auto* conn = mysql_real_connect(
            mysql_.get(),
            host.c_str(),
            user.c_str(),
            password.c_str(),
            database.c_str(),
            port,
            nullptr,
            0);

        if (!conn)
        {
            Throw<std::runtime_error>(
                std::string("Failed to connect to MySQL: ") +
                mysql_error(mysql_.get()));
        }

        uint8_t const reconnect = 1;
        mysql_options(mysql_.get(), MYSQL_OPT_RECONNECT, &reconnect);
    }

    ~MySQLBackend() override
    {
        close();
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
            Throw<std::runtime_error>("already open");

        if (createIfMissing)
        {
            if (mysql_query(mysql_.get(), CREATE_NODES_TABLE))
            {
                Throw<std::runtime_error>(
                    std::string("Failed to create nodes table: ") +
                    mysql_error(mysql_.get()));
            }
        }

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
        isOpen_ = false;
    }

    Status
    fetch(void const* key, std::shared_ptr<NodeObject>* pObject) override
    {
        if (!isOpen_)
            return notFound;

        uint256 const hash(uint256::fromVoid(key));

        MYSQL_STMT* stmt = mysql_stmt_init(mysql_.get());
        if (!stmt)
            return dataCorrupt;

        std::string const sql = 
            "SELECT data FROM nodes WHERE hash = ?";
        
        if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        MYSQL_BIND bindParam;
        std::memset(&bindParam, 0, sizeof(bindParam));
        bindParam.buffer_type = MYSQL_TYPE_BLOB;
        bindParam.buffer = const_cast<void*>(static_cast<void const*>(hash.data()));
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

        if (mysql_stmt_fetch(stmt))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        std::vector<uint8_t> buffer(length);
        bindResult.buffer = buffer.data();
        bindResult.buffer_length = length;

        if (mysql_stmt_fetch_column(stmt, &bindResult, 0, 0))
        {
            mysql_stmt_close(stmt);
            return dataCorrupt;
        }

        mysql_stmt_close(stmt);

        nudb::detail::buffer decompressed;
        auto const result = 
            nodeobject_decompress(buffer.data(), buffer.size(), decompressed);
        
        DecodedBlob decoded(hash.data(), result.first, result.second);
        if (!decoded.wasOk())
            return dataCorrupt;
        
        *pObject = decoded.createObject();
        return ok;
    }

    std::pair<std::vector<std::shared_ptr<NodeObject>>, Status>
    fetchBatch(std::vector<uint256 const*> const& hashes) override
    {
        std::vector<std::shared_ptr<NodeObject>> results;
        results.reserve(hashes.size());

        if (!isOpen_)
            return {results, notFound};

        if (mysql_query(mysql_.get(), "START TRANSACTION"))
            return {results, dataCorrupt};

        try
        {
            for (auto const& h : hashes)
            {
                std::shared_ptr<NodeObject> nObj;
                Status status = fetch(h->begin(), &nObj);
                results.push_back(status == ok ? nObj : nullptr);
            }

            if (mysql_query(mysql_.get(), "COMMIT"))
                return {results, dataCorrupt};

            return {results, ok};
        }
        catch (...)
        {
            mysql_query(mysql_.get(), "ROLLBACK");
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
        auto const result = 
            nodeobject_compress(encoded.getData(), encoded.getSize(), compressed);

        MYSQL_STMT* stmt = mysql_stmt_init(mysql_.get());
        if (!stmt)
            return;

        std::string const sql = 
            "INSERT INTO nodes (hash, data) VALUES (?, ?) "
            "ON DUPLICATE KEY UPDATE data = VALUES(data)";

        if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()))
        {
            mysql_stmt_close(stmt);
            return;
        }

        MYSQL_BIND bind[2];
        std::memset(bind, 0, sizeof(bind));

        auto const& hash = object->getHash();
        bind[0].buffer_type = MYSQL_TYPE_BLOB;
        bind[0].buffer = const_cast<void*>(static_cast<void const*>(hash.data()));
        bind[0].buffer_length = hash.size();

        bind[1].buffer_type = MYSQL_TYPE_BLOB;
        bind[1].buffer = const_cast<void*>(static_cast<void const*>(result.first));
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

        if (mysql_query(mysql_.get(), "START TRANSACTION"))
            return;

        try
        {
            for (auto const& e : batch)
                store(e);

            if (mysql_query(mysql_.get(), "COMMIT"))
                mysql_query(mysql_.get(), "ROLLBACK");
        }
        catch (...)
        {
            mysql_query(mysql_.get(), "ROLLBACK");
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

        if (mysql_query(mysql_.get(), 
            "SELECT hash, data FROM nodes ORDER BY created_at"))
            return;

        MYSQL_RES* result = mysql_store_result(mysql_.get());
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
                row[1], 
                static_cast<std::size_t>(lengths[1]), 
                decompressed);
            
            DecodedBlob decoded(
                row[0], 
                decomp_result.first, 
                decomp_result.second);

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

#endif // RIPPLE_NODESTORE_MYSQLBACKEND_H_INCLUDED
