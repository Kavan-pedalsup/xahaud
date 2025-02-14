#ifndef RIPPLE_APP_RDB_BACKEND_MYSQLDATABASE_H_INCLUDED
#define RIPPLE_APP_RDB_BACKEND_MYSQLDATABASE_H_INCLUDED

#include <ripple/app/ledger/AcceptedLedger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/app/ledger/PendingSaves.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/impl/AccountTxPaging.h>
#include <ripple/app/rdb/backend/SQLiteDatabase.h>
#include <mysql/mysql.h>
#include <sstream>

namespace ripple {

struct MySQLDeleter
{
    void
    operator()(MYSQL* mysql)
    {
        if (mysql)
        {
            mysql_close(mysql);
        }
    }
};

// Thread-local MySQL connection
static thread_local std::unique_ptr<MYSQL, MySQLDeleter> threadLocalMySQL_;

class MySQLDatabase : public SQLiteDatabase
{
private:
    Application& app_;
    bool const useTxTables_;

    // Configuration for creating new connections
    struct MySQLConfig
    {
        std::string host;
        std::string user;
        std::string pass;
        std::string name;
        unsigned int port;
    };
    MySQLConfig config_;

    MYSQL*
    initializeConnection()
    {
        MYSQL* mysql = mysql_init(nullptr);
        if (!mysql)
        {
            throw std::runtime_error("Failed to initialize MySQL");
        }

        if (!mysql_real_connect(
                mysql,
                config_.host.c_str(),
                config_.user.c_str(),
                config_.pass.c_str(),
                nullptr,  // Don't select database in connection
                config_.port,
                nullptr,
                0))
        {
            auto error = mysql_error(mysql);
            mysql_close(mysql);
            throw std::runtime_error(
                std::string("Failed to connect to MySQL: ") + error);
        }

        // Try to select the database first
        if (mysql_select_db(mysql, config_.name.c_str()))
        {
            // Database selection failed, try to create it
            std::string create_db_query = "CREATE DATABASE IF NOT EXISTS " +
                std::string(config_.name.c_str());

            if (mysql_query(mysql, create_db_query.c_str()))
            {
                // Creation failed for some reason
                auto error = mysql_error(mysql);
                mysql_close(mysql);
                throw std::runtime_error(
                    std::string("Failed to create database: ") + error);
            }
        }

        // Try selecting again (either after creation or if it existed already)
        if (mysql_select_db(mysql, config_.name.c_str()))
        {
            auto error = mysql_error(mysql);
            mysql_close(mysql);
            throw std::runtime_error(
                std::string("Failed to select database: ") + error);
        }

        return mysql;
    }

    // Get the thread-local MySQL connection, creating it if necessary
    MYSQL*
    getConnection()
    {
        if (!threadLocalMySQL_)
        {
            threadLocalMySQL_.reset(initializeConnection());
        }
        return threadLocalMySQL_.get();
    }

    // Schema creation statements
    static constexpr auto CREATE_LEDGERS_TABLE = R"SQL(
        CREATE TABLE IF NOT EXISTS ledgers (
            ledger_seq BIGINT PRIMARY KEY,
            ledger_hash VARCHAR(64) UNIQUE NOT NULL,
            parent_hash VARCHAR(64) NOT NULL,
            total_coins BIGINT NOT NULL,
            closing_time BIGINT NOT NULL,
            prev_closing_time BIGINT NOT NULL,
            close_time_resolution BIGINT NOT NULL,
            close_flags INT NOT NULL,
            account_hash VARCHAR(64) NOT NULL,
            tx_hash VARCHAR(64) NOT NULL
        )
    )SQL";

    static constexpr auto CREATE_TRANSACTIONS_TABLE = R"SQL(
        CREATE TABLE IF NOT EXISTS transactions (
            tx_hash VARCHAR(64) PRIMARY KEY,
            ledger_seq BIGINT NOT NULL,
            tx_seq INT NOT NULL,
            raw_tx MEDIUMBLOB NOT NULL,
            meta_data MEDIUMBLOB NOT NULL,
            FOREIGN KEY (ledger_seq) REFERENCES ledgers(ledger_seq)
        )
    )SQL";

    static constexpr auto CREATE_ACCOUNT_TRANSACTIONS_TABLE = R"SQL(
        CREATE TABLE IF NOT EXISTS account_transactions (
            account_id VARCHAR(64) NOT NULL,
            tx_hash VARCHAR(64) NOT NULL,
            ledger_seq BIGINT NOT NULL,
            tx_seq INT NOT NULL,
            PRIMARY KEY (account_id, ledger_seq, tx_seq),
            FOREIGN KEY (tx_hash) REFERENCES transactions(tx_hash),
            FOREIGN KEY (ledger_seq) REFERENCES ledgers(ledger_seq)
        )
    )SQL";

public:
    // In the MySQLDatabase constructor, after the mysql_real_connect call:

    // Add this to the private section with other table definitions
    static constexpr auto CREATE_NODES_TABLE = R"SQL(
    CREATE TABLE IF NOT EXISTS nodes (
        id INTEGER PRIMARY KEY AUTO_INCREMENT,
        public_key VARCHAR(64) NOT NULL,
        ledger_hash VARCHAR(64) NOT NULL,
        type VARCHAR(32) NOT NULL,
        data MEDIUMBLOB NOT NULL,
        UNIQUE INDEX idx_key_hash (public_key, ledger_hash)
    )
)SQL";

    // Then modify the constructor:
    MySQLDatabase(Application& app, Config const& config, JobQueue& jobQueue)
        : app_(app), useTxTables_(config.useTxTables())
    {
        if (!config.mysql.has_value())
            throw std::runtime_error(
                "[mysql_settings] stanza missing from config!");

        // Store configuration for creating new connections
        config_.host = config.mysql->host;
        config_.user = config.mysql->user;
        config_.pass = config.mysql->pass;
        config_.name = config.mysql->name;
        config_.port = config.mysql->port;

        // Initialize first connection and create schema
        auto mysql = getConnection();

        // Create database if it doesn't exist
        std::string create_db = "CREATE DATABASE IF NOT EXISTS " + config_.name;
        if (mysql_query(mysql, create_db.c_str()))
            throw std::runtime_error(
                std::string("Failed to create database: ") +
                mysql_error(mysql));

        // Create schema tables
        if (mysql_query(mysql, CREATE_NODES_TABLE))
            throw std::runtime_error(
                std::string("Failed to create nodes table: ") +
                mysql_error(mysql));

        if (mysql_query(mysql, CREATE_LEDGERS_TABLE))
            throw std::runtime_error(
                std::string("Failed to create ledgers table: ") +
                mysql_error(mysql));

        if (useTxTables_)
        {
            if (mysql_query(mysql, CREATE_TRANSACTIONS_TABLE))
                throw std::runtime_error(
                    std::string("Failed to create transactions table: ") +
                    mysql_error(mysql));

            if (mysql_query(mysql, CREATE_ACCOUNT_TRANSACTIONS_TABLE))
                throw std::runtime_error(
                    std::string(
                        "Failed to create account_transactions table: ") +
                    mysql_error(mysql));
        }
    }

    bool
    saveValidatedLedger(
        std::shared_ptr<Ledger const> const& ledger,
        bool current) override
    {
        auto j = app_.journal("Ledger");
        auto seq = ledger->info().seq;

        if (!ledger->info().accountHash.isNonZero())
        {
            JLOG(j.fatal()) << "AH is zero: " << getJson({*ledger, {}});
            assert(false);
            return false;
        }

        // Save the ledger header
        std::stringstream sql;
        sql << "INSERT INTO ledgers ("
            << "ledger_seq, ledger_hash, parent_hash, total_coins, "
            << "closing_time, prev_closing_time, close_time_resolution, "
            << "close_flags, account_hash, tx_hash) VALUES (" << seq << ", "
            << "'" << strHex(ledger->info().hash) << "', "
            << "'" << strHex(ledger->info().parentHash) << "', "
            << ledger->info().drops.drops() << ", "
            << ledger->info().closeTime.time_since_epoch().count() << ", "
            << ledger->info().parentCloseTime.time_since_epoch().count() << ", "
            << ledger->info().closeTimeResolution.count() << ", "
            << ledger->info().closeFlags << ", "
            << "'" << strHex(ledger->info().accountHash) << "', "
            << "'" << strHex(ledger->info().txHash) << "') "
            << "ON DUPLICATE KEY UPDATE "
            << "parent_hash = VALUES(parent_hash), "
            << "total_coins = VALUES(total_coins), "
            << "closing_time = VALUES(closing_time), "
            << "prev_closing_time = VALUES(prev_closing_time), "
            << "close_time_resolution = VALUES(close_time_resolution), "
            << "close_flags = VALUES(close_flags), "
            << "account_hash = VALUES(account_hash), "
            << "tx_hash = VALUES(tx_hash)";

        if (mysql_query(getConnection(), sql.str().c_str()))
        {
            JLOG(j.fatal())
                << "Failed to save ledger: " << mysql_error(getConnection());
            return false;
        }

        if (useTxTables_)
        {
            std::shared_ptr<AcceptedLedger> aLedger;
            try
            {
                aLedger =
                    app_.getAcceptedLedgerCache().fetch(ledger->info().hash);
                if (!aLedger)
                {
                    aLedger = std::make_shared<AcceptedLedger>(ledger, app_);
                    app_.getAcceptedLedgerCache().canonicalize_replace_client(
                        ledger->info().hash, aLedger);
                }
            }
            catch (std::exception const&)
            {
                JLOG(j.warn()) << "An accepted ledger was missing nodes";
                return false;
            }

            // Start a transaction for saving all transactions
            if (mysql_query(getConnection(), "START TRANSACTION"))
            {
                JLOG(j.fatal()) << "Failed to start transaction: "
                                << mysql_error(getConnection());
                return false;
            }

            try
            {
                for (auto const& acceptedLedgerTx : *aLedger)
                {
                    auto const& txn = acceptedLedgerTx->getTxn();
                    auto const& meta = acceptedLedgerTx->getMeta();
                    auto const& id = txn->getTransactionID();

                    // Save transaction
                    std::stringstream txSql;
                    txSql << "INSERT INTO transactions ("
                          << "tx_hash, ledger_seq, tx_seq, raw_tx, meta_data) "
                             "VALUES ("
                          << "'" << strHex(id) << "', " << seq << ", "
                          << acceptedLedgerTx->getTxnSeq() << ", "
                          << "?, ?) "  // Using placeholders for BLOB data
                          << "ON DUPLICATE KEY UPDATE "
                          << "ledger_seq = VALUES(ledger_seq), "
                          << "tx_seq = VALUES(tx_seq), "
                          << "raw_tx = VALUES(raw_tx), "
                          << "meta_data = VALUES(meta_data)";

                    MYSQL_STMT* stmt = mysql_stmt_init(getConnection());
                    if (!stmt)
                    {
                        throw std::runtime_error(
                            "Failed to initialize statement");
                    }

                    if (mysql_stmt_prepare(
                            stmt, txSql.str().c_str(), txSql.str().length()))
                    {
                        mysql_stmt_close(stmt);
                        throw std::runtime_error("Failed to prepare statement");
                    }

                    // Bind parameters for BLOB data
                    MYSQL_BIND bind[2];
                    memset(bind, 0, sizeof(bind));

                    Serializer s;
                    txn->add(s);
                    bind[0].buffer_type = MYSQL_TYPE_BLOB;
                    bind[0].buffer = (void*)s.data();
                    bind[0].buffer_length = s.size();

                    Serializer s2;
                    meta.getAsObject().addWithoutSigningFields(s2);

                    bind[1].buffer_type = MYSQL_TYPE_BLOB;
                    bind[1].buffer = (void*)s2.data();
                    bind[1].buffer_length = s2.size();

                    if (mysql_stmt_bind_param(stmt, bind))
                    {
                        mysql_stmt_close(stmt);
                        throw std::runtime_error("Failed to bind parameters");
                    }

                    if (mysql_stmt_execute(stmt))
                    {
                        mysql_stmt_close(stmt);
                        throw std::runtime_error("Failed to execute statement");
                    }

                    mysql_stmt_close(stmt);

                    // Save account transactions
                    for (auto const& account : meta.getAffectedAccounts())
                    {
                        std::stringstream accTxSql;
                        accTxSql << "INSERT INTO account_transactions ("
                                 << "account_id, tx_hash, ledger_seq, tx_seq) "
                                    "VALUES ("
                                 << "'" << strHex(account) << "', "
                                 << "'" << strHex(id) << "', " << seq << ", "
                                 << acceptedLedgerTx->getTxnSeq() << ") "
                                 << "ON DUPLICATE KEY UPDATE "
                                 << "tx_hash = VALUES(tx_hash)";

                        if (mysql_query(
                                getConnection(), accTxSql.str().c_str()))
                        {
                            throw std::runtime_error(
                                mysql_error(getConnection()));
                        }
                    }

                    app_.getMasterTransaction().inLedger(
                        id,
                        seq,
                        acceptedLedgerTx->getTxnSeq(),
                        app_.config().NETWORK_ID);
                }

                if (mysql_query(getConnection(), "COMMIT"))
                {
                    throw std::runtime_error(mysql_error(getConnection()));
                }
            }
            catch (std::exception const& e)
            {
                JLOG(j.fatal()) << "Error saving transactions: " << e.what();
                mysql_query(getConnection(), "ROLLBACK");
                return false;
            }
        }

        return true;
    }

    std::optional<LedgerIndex>
    getMinLedgerSeq() override
    {
        if (mysql_query(getConnection(), "SELECT MIN(ledger_seq) FROM ledgers"))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = std::stoll(row[0]);
        mysql_free_result(result);
        return seq;
    }

    std::optional<LedgerIndex>
    getTransactionsMinLedgerSeq() override
    {
        if (!useTxTables_)
            return {};

        if (mysql_query(
                getConnection(), "SELECT MIN(ledger_seq) FROM transactions"))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = std::stoll(row[0]);
        mysql_free_result(result);
        return seq;
    }

    std::optional<LedgerIndex>
    getMaxLedgerSeq() override
    {
        if (mysql_query(getConnection(), "SELECT MAX(ledger_seq) FROM ledgers"))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = std::stoll(row[0]);
        mysql_free_result(result);
        return seq;
    }

    void
    deleteTransactionByLedgerSeq(LedgerIndex ledgerSeq) override
    {
        if (!useTxTables_)
            return;

        std::stringstream sql;
        sql << "DELETE FROM account_transactions WHERE ledger_seq = "
            << ledgerSeq;
        mysql_query(getConnection(), sql.str().c_str());

        sql.str("");
        sql << "DELETE FROM transactions WHERE ledger_seq = " << ledgerSeq;
        mysql_query(getConnection(), sql.str().c_str());
    }

    void
    deleteBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        if (useTxTables_)
        {
            std::stringstream sql;
            sql << "DELETE FROM account_transactions WHERE ledger_seq < "
                << ledgerSeq;
            mysql_query(getConnection(), sql.str().c_str());

            sql.str("");
            sql << "DELETE FROM transactions WHERE ledger_seq < " << ledgerSeq;
            mysql_query(getConnection(), sql.str().c_str());
        }

        std::stringstream sql;
        sql << "DELETE FROM ledgers WHERE ledger_seq < " << ledgerSeq;
        mysql_query(getConnection(), sql.str().c_str());
    }

    void
    deleteTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        if (!useTxTables_)
            return;

        std::stringstream sql;
        sql << "DELETE FROM account_transactions WHERE ledger_seq < "
            << ledgerSeq;
        mysql_query(getConnection(), sql.str().c_str());

        sql.str("");
        sql << "DELETE FROM transactions WHERE ledger_seq < " << ledgerSeq;
        mysql_query(getConnection(), sql.str().c_str());
    }

    void
    deleteAccountTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        if (!useTxTables_)
            return;

        std::stringstream sql;
        sql << "DELETE FROM account_transactions WHERE ledger_seq < "
            << ledgerSeq;
        mysql_query(getConnection(), sql.str().c_str());
    }

    std::size_t
    getTransactionCount() override
    {
        if (!useTxTables_)
            return 0;

        if (mysql_query(getConnection(), "SELECT COUNT(*) FROM transactions"))
            return 0;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return 0;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return 0;
        }

        std::size_t count = std::stoull(row[0]);
        mysql_free_result(result);
        return count;
    }

    std::size_t
    getAccountTransactionCount() override
    {
        if (!useTxTables_)
            return 0;

        if (mysql_query(
                getConnection(), "SELECT COUNT(*) FROM account_transactions"))
            return 0;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return 0;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return 0;
        }

        std::size_t count = std::stoull(row[0]);
        mysql_free_result(result);
        return count;
    }

    CountMinMax
    getLedgerCountMinMax() override
    {
        if (mysql_query(
                getConnection(),
                "SELECT COUNT(*), MIN(ledger_seq), MAX(ledger_seq) FROM "
                "ledgers"))
            return {0, 0, 0};

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return {0, 0, 0};

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0] || !row[1] || !row[2])
        {
            mysql_free_result(result);
            return {0, 0, 0};
        }

        CountMinMax ret{
            std::stoull(row[0]),
            static_cast<LedgerIndex>(std::stoll(row[1])),
            static_cast<LedgerIndex>(std::stoll(row[2]))};
        mysql_free_result(result);
        return ret;
    }

    std::optional<LedgerInfo>
    getLedgerInfoByIndex(LedgerIndex ledgerSeq) override
    {
        std::stringstream sql;
        sql << "SELECT ledger_hash, parent_hash, total_coins, closing_time, "
            << "prev_closing_time, close_time_resolution, close_flags, "
            << "account_hash, tx_hash FROM ledgers WHERE ledger_seq = "
            << ledgerSeq;

        if (mysql_query(getConnection(), sql.str().c_str()))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row)
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerInfo info;
        info.seq = ledgerSeq;
        info.hash = uint256(row[0]);
        info.parentHash = uint256(row[1]);
        info.drops = XRPAmount(std::stoull(row[2]));
        info.closeTime =
            NetClock::time_point{NetClock::duration{std::stoll(row[3])}};
        info.parentCloseTime =
            NetClock::time_point{NetClock::duration{std::stoll(row[4])}};
        info.closeTimeResolution = NetClock::duration{std::stoll(row[5])};
        info.closeFlags = std::stoul(row[6]);
        info.accountHash = uint256(row[7]);
        info.txHash = uint256(row[8]);

        mysql_free_result(result);
        return info;
    }

    std::optional<LedgerInfo>
    getLimitedOldestLedgerInfo(LedgerIndex ledgerFirstIndex) override
    {
        std::stringstream sql;
        sql << "SELECT ledger_seq FROM ledgers WHERE ledger_seq >= "
            << ledgerFirstIndex << " ORDER BY ledger_seq ASC LIMIT 1";

        if (mysql_query(getConnection(), sql.str().c_str()))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = std::stoll(row[0]);
        mysql_free_result(result);
        return getLedgerInfoByIndex(seq);
    }

    std::optional<LedgerInfo>
    getLimitedNewestLedgerInfo(LedgerIndex ledgerFirstIndex) override
    {
        std::stringstream sql;
        sql << "SELECT ledger_seq FROM ledgers WHERE ledger_seq >= "
            << ledgerFirstIndex << " ORDER BY ledger_seq DESC LIMIT 1";

        if (mysql_query(getConnection(), sql.str().c_str()))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = std::stoll(row[0]);
        mysql_free_result(result);
        return getLedgerInfoByIndex(seq);
    }

    std::optional<LedgerInfo>
    getLedgerInfoByHash(uint256 const& ledgerHash) override
    {
        std::stringstream sql;
        sql << "SELECT ledger_seq FROM ledgers WHERE ledger_hash = '"
            << strHex(ledgerHash) << "'";

        if (mysql_query(getConnection(), sql.str().c_str()))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = std::stoll(row[0]);
        mysql_free_result(result);
        return getLedgerInfoByIndex(seq);
    }

    uint256
    getHashByIndex(LedgerIndex ledgerIndex) override
    {
        std::stringstream sql;
        sql << "SELECT ledger_hash FROM ledgers WHERE ledger_seq = "
            << ledgerIndex;

        if (mysql_query(getConnection(), sql.str().c_str()))
            return uint256();

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return uint256();

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return uint256();
        }

        uint256 hash(row[0]);
        mysql_free_result(result);
        return hash;
    }

    std::optional<LedgerHashPair>
    getHashesByIndex(LedgerIndex ledgerIndex) override
    {
        std::stringstream sql;
        sql << "SELECT ledger_hash, parent_hash FROM ledgers WHERE ledger_seq "
               "= "
            << ledgerIndex;

        if (mysql_query(getConnection(), sql.str().c_str()))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0] || !row[1])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerHashPair pair{uint256(row[0]), uint256(row[1])};
        mysql_free_result(result);
        return pair;
    }

    std::map<LedgerIndex, LedgerHashPair>
    getHashesByIndex(LedgerIndex minSeq, LedgerIndex maxSeq) override
    {
        std::map<LedgerIndex, LedgerHashPair> result;
        std::stringstream sql;
        sql << "SELECT ledger_seq, ledger_hash, parent_hash FROM ledgers "
            << "WHERE ledger_seq BETWEEN " << minSeq << " AND " << maxSeq
            << " ORDER BY ledger_seq";

        if (mysql_query(getConnection(), sql.str().c_str()))
            return result;

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(sqlResult)))
        {
            LedgerIndex const seq =
                static_cast<LedgerIndex>(std::stoull(row[0]));
            result.emplace(
                seq, LedgerHashPair{uint256{row[1]}, uint256{row[2]}});
        }

        mysql_free_result(sqlResult);
        return result;
    }

    std::optional<LedgerIndex>
    getAccountTransactionsMinLedgerSeq() override
    {
        if (!useTxTables_)
            return {};

        if (mysql_query(
                getConnection(),
                "SELECT MIN(ledger_seq) FROM account_transactions"))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = static_cast<LedgerIndex>(std::stoull(row[0]));
        mysql_free_result(result);
        return seq;
    }

    std::optional<LedgerInfo>
    getNewestLedgerInfo() override
    {
        if (mysql_query(
                getConnection(),
                "SELECT ledger_seq FROM ledgers ORDER BY ledger_seq DESC LIMIT "
                "1"))
            return std::nullopt;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return std::nullopt;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row || !row[0])
        {
            mysql_free_result(result);
            return std::nullopt;
        }

        LedgerIndex seq = static_cast<LedgerIndex>(std::stoull(row[0]));
        mysql_free_result(result);
        return getLedgerInfoByIndex(seq);
    }

    std::variant<AccountTx, TxSearched>
    getTransaction(
        uint256 const& id,
        std::optional<ClosedInterval<std::uint32_t>> const& range,
        error_code_i& ec) override
    {
        if (!useTxTables_)
            return TxSearched::unknown;

        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq "
            << "FROM transactions t WHERE t.tx_hash = '" << strHex(id) << "'";

        if (range)
        {
            sql << " AND t.ledger_seq BETWEEN " << range->first() << " AND "
                << range->last();
        }

        if (mysql_query(getConnection(), sql.str().c_str()))
            return TxSearched::unknown;

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return TxSearched::unknown;

        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row)
        {
            mysql_free_result(result);
            if (range)
            {
                sql.str("");
                sql << "SELECT COUNT(*) FROM ledgers WHERE ledger_seq BETWEEN "
                    << range->first() << " AND " << range->last();

                if (mysql_query(getConnection(), sql.str().c_str()))
                    return TxSearched::unknown;

                result = mysql_store_result(getConnection());
                if (!result)
                    return TxSearched::unknown;

                row = mysql_fetch_row(result);
                if (!row || !row[0])
                {
                    mysql_free_result(result);
                    return TxSearched::unknown;
                }

                std::size_t count = std::stoull(row[0]);
                mysql_free_result(result);

                return (count == (range->last() - range->first() + 1))
                    ? TxSearched::all
                    : TxSearched::some;
            }
            return TxSearched::unknown;
        }

        unsigned long* lengths = mysql_fetch_lengths(result);
        if (!lengths)
        {
            mysql_free_result(result);
            return TxSearched::unknown;
        }

        // Deserialize transaction and metadata
        try
        {
            SerialIter sit(row[0], lengths[0]);
            auto txn = std::make_shared<STTx const>(sit);

            auto meta = std::make_shared<TxMeta>(
                id,
                static_cast<uint32_t>(std::stoull(row[2])),
                Blob(row[1], row[1] + lengths[1]));

            mysql_free_result(result);

            AccountTx at;
            std::string reason;
            at.first = std::make_shared<Transaction>(txn, reason, app_);
            at.first->setStatus(COMMITTED);
            at.first->setLedger(static_cast<LedgerIndex>(std::stoull(row[2])));
            at.second = meta;

            return at;
        }
        catch (std::exception const&)
        {
            mysql_free_result(result);
            return TxSearched::unknown;
        }
    }

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    oldestAccountTxPage(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        static std::uint32_t const page_length(200);
        auto onUnsavedLedger =
            std::bind(saveLedgerAsync, std::ref(app_), std::placeholders::_1);
        AccountTxs ret;
        Application& app = app_;
        auto onTransaction = [&ret, &app](
                                 std::uint32_t ledger_index,
                                 std::string const& status,
                                 Blob&& rawTxn,
                                 Blob&& rawMeta) {
            convertBlobsToTxResult(
                ret, ledger_index, status, rawTxn, rawMeta, app);
        };

        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq, t.tx_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger << " ORDER BY at.ledger_seq, at.tx_seq"
            << " LIMIT " << (options.limit + 1);

        if (mysql_query(getConnection(), sql.str().c_str()))
            return {};

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return {};

        std::optional<AccountTxMarker> marker;
        std::size_t count = 0;
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(result)) &&
               (!options.limit || count < options.limit))
        {
            unsigned long* lengths = mysql_fetch_lengths(result);
            if (!lengths)
                continue;

            Blob rawTxn(row[0], row[0] + lengths[0]);
            Blob rawMeta(row[1], row[1] + lengths[1]);
            std::uint32_t ledgerSeq =
                static_cast<std::uint32_t>(std::stoull(row[2]));
            std::uint32_t txSeq =
                static_cast<std::uint32_t>(std::stoull(row[3]));

            if (count == options.limit)
            {
                marker = AccountTxMarker{ledgerSeq, txSeq};
                break;
            }

            onTransaction(
                ledgerSeq, "COMMITTED", std::move(rawTxn), std::move(rawMeta));
            ++count;
        }

        mysql_free_result(result);
        return {ret, marker};
    }

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    newestAccountTxPage(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        static std::uint32_t const page_length(200);
        auto onUnsavedLedger =
            std::bind(saveLedgerAsync, std::ref(app_), std::placeholders::_1);
        AccountTxs ret;
        Application& app = app_;
        auto onTransaction = [&ret, &app](
                                 std::uint32_t ledger_index,
                                 std::string const& status,
                                 Blob&& rawTxn,
                                 Blob&& rawMeta) {
            convertBlobsToTxResult(
                ret, ledger_index, status, rawTxn, rawMeta, app);
        };

        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq, t.tx_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger
            << " ORDER BY at.ledger_seq DESC, at.tx_seq DESC"
            << " LIMIT " << (options.limit + 1);

        if (mysql_query(getConnection(), sql.str().c_str()))
            return {};

        MYSQL_RES* result = mysql_store_result(getConnection());
        if (!result)
            return {};

        std::optional<AccountTxMarker> marker;
        std::size_t count = 0;
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(result)) &&
               (!options.limit || count < options.limit))
        {
            unsigned long* lengths = mysql_fetch_lengths(result);
            if (!lengths)
                continue;

            Blob rawTxn(row[0], row[0] + lengths[0]);
            Blob rawMeta(row[1], row[1] + lengths[1]);
            std::uint32_t ledgerSeq =
                static_cast<std::uint32_t>(std::stoull(row[2]));
            std::uint32_t txSeq =
                static_cast<std::uint32_t>(std::stoull(row[3]));

            if (count == options.limit)
            {
                marker = AccountTxMarker{ledgerSeq, txSeq};
                break;
            }

            onTransaction(
                ledgerSeq, "COMMITTED", std::move(rawTxn), std::move(rawMeta));
            ++count;
        }

        mysql_free_result(result);
        return {ret, marker};
    }

    bool
    ledgerDbHasSpace(Config const&) override
    {
        // MySQL manages its own space
        return true;
    }

    std::vector<std::shared_ptr<Transaction>>
    getTxHistory(LedgerIndex startIndex) override
    {
        if (!useTxTables_)
            return {};

        std::vector<std::shared_ptr<Transaction>> result;
        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.ledger_seq "
            << "FROM transactions t "
            << "ORDER BY t.ledger_seq DESC, t.tx_seq DESC "
            << "LIMIT 20 OFFSET " << startIndex;

        if (mysql_query(getConnection(), sql.str().c_str()))
            return result;

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(sqlResult)))
        {
            unsigned long* lengths = mysql_fetch_lengths(sqlResult);
            if (!lengths)
                continue;

            try
            {
                SerialIter sit(row[0], lengths[0]);
                auto txn = std::make_shared<STTx const>(sit);
                std::string reason;
                auto tx = std::make_shared<Transaction>(txn, reason, app_);

                auto const ledgerSeq =
                    static_cast<std::uint32_t>(std::stoull(row[1]));
                tx->setStatus(COMMITTED);
                tx->setLedger(ledgerSeq);
                result.push_back(tx);
            }
            catch (std::exception const&)
            {
                // Skip any malformed transactions
                continue;
            }
        }

        mysql_free_result(sqlResult);
        return result;
    }

    bool
    transactionDbHasSpace(Config const&) override
    {
        // MySQL manages its own space
        return true;
    }

    AccountTxs
    getOldestAccountTxs(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        AccountTxs result;
        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger
            << " ORDER BY at.ledger_seq ASC, at.tx_seq ASC ";

        if (!options.bUnlimited)
        {
            sql << "LIMIT " << options.limit;
            if (options.offset)
                sql << " OFFSET " << options.offset;
        }

        if (mysql_query(getConnection(), sql.str().c_str()))
            return result;

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(sqlResult)))
        {
            unsigned long* lengths = mysql_fetch_lengths(sqlResult);
            if (!lengths)
                continue;

            try
            {
                SerialIter sit(row[0], lengths[0]);
                auto txn = std::make_shared<STTx const>(sit);
                std::string reason;
                auto tx = std::make_shared<Transaction>(txn, reason, app_);

                auto const ledgerSeq =
                    static_cast<std::uint32_t>(std::stoull(row[2]));

                auto meta = std::make_shared<TxMeta>(
                    txn->getTransactionID(),
                    ledgerSeq,
                    Blob(row[1], row[1] + lengths[1]));

                tx->setStatus(COMMITTED);
                tx->setLedger(ledgerSeq);

                result.emplace_back(tx, meta);
            }
            catch (std::exception const&)
            {
                continue;
            }
        }

        mysql_free_result(sqlResult);
        return result;
    }

    AccountTxs
    getNewestAccountTxs(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        AccountTxs result;
        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger
            << " ORDER BY at.ledger_seq DESC, at.tx_seq DESC ";

        if (!options.bUnlimited)
        {
            sql << "LIMIT " << options.limit;
            if (options.offset)
                sql << " OFFSET " << options.offset;
        }

        if (mysql_query(getConnection(), sql.str().c_str()))
            return result;

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(sqlResult)))
        {
            unsigned long* lengths = mysql_fetch_lengths(sqlResult);
            if (!lengths)
                continue;

            try
            {
                SerialIter sit(row[0], lengths[0]);
                auto txn = std::make_shared<STTx const>(sit);
                std::string reason;
                auto tx = std::make_shared<Transaction>(txn, reason, app_);

                auto const ledgerSeq =
                    static_cast<std::uint32_t>(std::stoull(row[2]));

                auto meta = std::make_shared<TxMeta>(
                    txn->getTransactionID(),
                    ledgerSeq,
                    Blob(row[1], row[1] + lengths[1]));

                tx->setStatus(COMMITTED);
                tx->setLedger(ledgerSeq);

                result.emplace_back(tx, meta);
            }
            catch (std::exception const&)
            {
                continue;
            }
        }

        mysql_free_result(sqlResult);
        return result;
    }

    MetaTxsList
    getOldestAccountTxsB(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        MetaTxsList result;
        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger
            << " ORDER BY at.ledger_seq ASC, at.tx_seq ASC ";

        if (!options.bUnlimited)
        {
            sql << "LIMIT " << options.limit;
            if (options.offset)
                sql << " OFFSET " << options.offset;
        }

        if (mysql_query(getConnection(), sql.str().c_str()))
            return result;

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(sqlResult)))
        {
            unsigned long* lengths = mysql_fetch_lengths(sqlResult);
            if (!lengths)
                continue;

            auto const ledgerSeq =
                static_cast<std::uint32_t>(std::stoull(row[2]));

            result.emplace_back(
                Blob(row[0], row[0] + lengths[0]),
                Blob(row[1], row[1] + lengths[1]),
                ledgerSeq);
        }

        mysql_free_result(sqlResult);
        return result;
    }

    MetaTxsList
    getNewestAccountTxsB(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        MetaTxsList result;
        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger
            << " ORDER BY at.ledger_seq DESC, at.tx_seq DESC ";

        if (!options.bUnlimited)
        {
            sql << "LIMIT " << options.limit;
            if (options.offset)
                sql << " OFFSET " << options.offset;
        }

        if (mysql_query(getConnection(), sql.str().c_str()))
            return result;

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return result;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(sqlResult)))
        {
            unsigned long* lengths = mysql_fetch_lengths(sqlResult);
            if (!lengths)
                continue;

            auto const ledgerSeq =
                static_cast<std::uint32_t>(std::stoull(row[2]));

            result.emplace_back(
                Blob(row[0], row[0] + lengths[0]),
                Blob(row[1], row[1] + lengths[1]),
                ledgerSeq);
        }

        mysql_free_result(sqlResult);
        return result;
    }

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    oldestAccountTxPageB(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        MetaTxsList result;
        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq, t.tx_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger;

        if (options.marker)
        {
            sql << " AND (at.ledger_seq > " << options.marker->ledgerSeq
                << " OR (at.ledger_seq = " << options.marker->ledgerSeq
                << " AND at.tx_seq > " << options.marker->txnSeq << "))";
        }

        sql << " ORDER BY at.ledger_seq ASC, at.tx_seq ASC ";
        sql << "LIMIT " << (options.limit + 1);

        if (mysql_query(getConnection(), sql.str().c_str()))
            return {};

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return {};

        std::optional<AccountTxMarker> marker;
        std::size_t count = 0;
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(sqlResult)))
        {
            if (count >= options.limit)
            {
                marker = AccountTxMarker{
                    static_cast<std::uint32_t>(std::stoull(row[2])),
                    static_cast<std::uint32_t>(std::stoull(row[3]))};
                break;
            }

            unsigned long* lengths = mysql_fetch_lengths(sqlResult);
            if (!lengths)
                continue;

            auto const ledgerSeq =
                static_cast<std::uint32_t>(std::stoull(row[2]));

            result.emplace_back(
                Blob(row[0], row[0] + lengths[0]),
                Blob(row[1], row[1] + lengths[1]),
                ledgerSeq);

            ++count;
        }

        mysql_free_result(sqlResult);
        return {result, marker};
    }

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    newestAccountTxPageB(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        MetaTxsList result;
        std::stringstream sql;
        sql << "SELECT t.raw_tx, t.meta_data, t.ledger_seq, t.tx_seq "
            << "FROM account_transactions at "
            << "JOIN transactions t ON at.tx_hash = t.tx_hash "
            << "WHERE at.account_id = '" << strHex(options.account) << "' "
            << "AND at.ledger_seq BETWEEN " << options.minLedger << " AND "
            << options.maxLedger;

        if (options.marker)
        {
            sql << " AND (at.ledger_seq < " << options.marker->ledgerSeq
                << " OR (at.ledger_seq = " << options.marker->ledgerSeq
                << " AND at.tx_seq < " << options.marker->txnSeq << "))";
        }

        sql << " ORDER BY at.ledger_seq DESC, at.tx_seq DESC ";
        sql << "LIMIT " << (options.limit + 1);

        if (mysql_query(getConnection(), sql.str().c_str()))
            return {};

        MYSQL_RES* sqlResult = mysql_store_result(getConnection());
        if (!sqlResult)
            return {};

        std::optional<AccountTxMarker> marker;
        std::size_t count = 0;
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(sqlResult)))
        {
            if (count >= options.limit)
            {
                marker = AccountTxMarker{
                    static_cast<std::uint32_t>(std::stoull(row[2])),
                    static_cast<std::uint32_t>(std::stoull(row[3]))};
                break;
            }

            unsigned long* lengths = mysql_fetch_lengths(sqlResult);
            if (!lengths)
                continue;

            auto const ledgerSeq =
                static_cast<std::uint32_t>(std::stoull(row[2]));

            result.emplace_back(
                Blob(row[0], row[0] + lengths[0]),
                Blob(row[1], row[1] + lengths[1]),
                ledgerSeq);

            ++count;
        }

        mysql_free_result(sqlResult);
        return {result, marker};
    }

    std::uint32_t
    getKBUsedAll() override
    {
        std::uint32_t total = 0;

        // Get ledger table size
        if (!mysql_query(
                getConnection(),
                "SELECT ROUND(SUM(data_length + index_length) / 1024) "
                "FROM information_schema.tables "
                "WHERE table_schema = DATABASE() "
                "AND table_name = 'ledgers'"))
        {
            MYSQL_RES* result = mysql_store_result(getConnection());
            if (result)
            {
                MYSQL_ROW row = mysql_fetch_row(result);
                if (row && row[0])
                    total += static_cast<std::uint32_t>(std::stoull(row[0]));
                mysql_free_result(result);
            }
        }

        // Get transaction tables size
        if (useTxTables_)
        {
            if (!mysql_query(
                    getConnection(),
                    "SELECT ROUND(SUM(data_length + index_length) / 1024) "
                    "FROM information_schema.tables "
                    "WHERE table_schema = DATABASE() "
                    "AND (table_name = 'transactions' "
                    "OR table_name = 'account_transactions')"))
            {
                MYSQL_RES* result = mysql_store_result(getConnection());
                if (result)
                {
                    MYSQL_ROW row = mysql_fetch_row(result);
                    if (row && row[0])
                        total +=
                            static_cast<std::uint32_t>(std::stoull(row[0]));
                    mysql_free_result(result);
                }
            }
        }

        return total;
    }

    std::uint32_t
    getKBUsedLedger() override
    {
        std::uint32_t total = 0;

        if (!mysql_query(
                getConnection(),
                "SELECT ROUND(SUM(data_length + index_length) / 1024) "
                "FROM information_schema.tables "
                "WHERE table_schema = DATABASE() "
                "AND table_name = 'ledgers'"))
        {
            MYSQL_RES* result = mysql_store_result(getConnection());
            if (result)
            {
                MYSQL_ROW row = mysql_fetch_row(result);
                if (row && row[0])
                    total = static_cast<std::uint32_t>(std::stoull(row[0]));
                mysql_free_result(result);
            }
        }

        return total;
    }

    std::uint32_t
    getKBUsedTransaction() override
    {
        if (!useTxTables_)
            return 0;

        std::uint32_t total = 0;

        if (!mysql_query(
                getConnection(),
                "SELECT ROUND(SUM(data_length + index_length) / 1024) "
                "FROM information_schema.tables "
                "WHERE table_schema = DATABASE() "
                "AND (table_name = 'transactions' "
                "OR table_name = 'account_transactions')"))
        {
            MYSQL_RES* result = mysql_store_result(getConnection());
            if (result)
            {
                MYSQL_ROW row = mysql_fetch_row(result);
                if (row && row[0])
                    total = static_cast<std::uint32_t>(std::stoull(row[0]));
                mysql_free_result(result);
            }
        }

        return total;
    }

    void
    closeLedgerDB() override
    {
        // No explicit closing needed for MySQL
        // The connection will be closed when mysql_ is destroyed
    }

    void
    closeTransactionDB() override
    {
        // No explicit closing needed for MySQL
        // The connection will be closed when mysql_ is destroyed
    }
};

// Factory function
std::unique_ptr<SQLiteDatabase>
getMySQLDatabase(Application& app, Config const& config, JobQueue& jobQueue)
{
    return std::make_unique<MySQLDatabase>(app, config, jobQueue);
}
}  // namespace ripple
#endif  // RIPPLE_APP_RDB_BACKEND_MYSQLDATABASE_H_INCLUDED
