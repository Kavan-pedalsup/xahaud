
#ifndef RIPPLE_APP_MAIN_DATAGRAMMONITOR_H_INCLUDED
#define RIPPLE_APP_MAIN_DATAGRAMMONITOR_H_INCLUDED
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/overlay/Overlay.h>
#include <boost/icl/interval_set.hpp>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <thread>
#include <vector>

namespace ripple {

// Magic number for server info packets: 'XDGM' (le) Xahau DataGram Monitor
constexpr uint32_t SERVER_INFO_MAGIC = 0x4D474458;
constexpr uint32_t SERVER_INFO_VERSION = 1;

// Warning flag bits
constexpr uint32_t WARNING_AMENDMENT_BLOCKED = 1 << 0;
constexpr uint32_t WARNING_UNL_BLOCKED = 1 << 1;
constexpr uint32_t WARNING_AMENDMENT_WARNED = 1 << 2;
constexpr uint32_t WARNING_NOT_SYNCED = 1 << 3;

// Time window statistics for rates
struct RateStats
{
    double rate_1m;   // Average rate over last minute
    double rate_5m;   // Average rate over last 5 minutes
    double rate_1h;   // Average rate over last hour
    double rate_24h;  // Average rate over last 24 hours
};

// Structure to represent a ledger sequence range
struct LgrRange
{
    uint32_t start;
    uint32_t end;
};

// Core server metrics in the fixed header
struct ServerInfoHeader
{
    uint32_t magic;               // Magic number to identify packet type
    uint32_t version;             // Protocol version number
    uint32_t network_id;          // Network ID from config
    uint32_t padding1;            // Padding to maintain 8-byte alignment
    uint64_t timestamp;           // System time in microseconds
    uint64_t uptime;              // Server uptime in seconds
    uint64_t io_latency_us;       // IO latency in microseconds
    uint64_t validation_quorum;   // Validation quorum count
    uint32_t peer_count;          // Number of connected peers
    uint32_t node_size;           // Size category (0=tiny through 4=huge)
    uint32_t server_state;        // Operating mode as enum
    uint32_t amendment_blocked;   // Boolean flag (0 or 1)
    uint64_t fetch_pack_size;     // Size of fetch pack cache
    uint64_t proposer_count;      // Number of proposers in last close
    uint64_t converge_time_ms;    // Last convergence time in ms
    uint64_t load_factor;         // Load factor (scaled by 1M for fixed point)
    uint64_t load_base;           // Load base value
    uint64_t reserve_base;        // Reserve base amount
    uint64_t reserve_inc;         // Reserve increment amount
    uint64_t ledger_seq;          // Latest ledger sequence
    uint8_t ledger_hash[32];      // Latest ledger hash
    uint8_t node_public_key[32];  // Node's public key
    uint32_t warning_flags;       // Bitfield of active warnings
    uint32_t ledger_range_count;  // Number of range entries that follow

    // System metrics
    uint64_t process_memory_pages;  // Process memory usage in pages
    uint64_t system_memory_total;   // Total system memory in bytes
    uint64_t system_memory_free;    // Free system memory in bytes
    uint64_t system_memory_used;    // Used system memory in bytes
    uint64_t system_disk_total;     // Total disk space in bytes
    uint64_t system_disk_free;      // Free disk space in bytes
    uint64_t system_disk_used;      // Used disk space in bytes
    double load_avg_1min;           // 1 minute load average
    double load_avg_5min;           // 5 minute load average
    double load_avg_15min;          // 15 minute load average
    uint64_t io_wait_time;          // IO wait time in milliseconds

    // Network and disk rates
    struct
    {
        RateStats network_in;
        RateStats network_out;
        RateStats disk_read;
        RateStats disk_write;
    } rates;
};

// System metrics collected for rate calculations
struct SystemMetrics
{
    uint64_t timestamp;           // When metrics were collected
    uint64_t network_bytes_in;    // Current total bytes in
    uint64_t network_bytes_out;   // Current total bytes out
    uint64_t disk_bytes_read;     // Current total bytes read
    uint64_t disk_bytes_written;  // Current total bytes written
};

class MetricsTracker
{
private:
    static constexpr size_t SAMPLES_1M = 60;    // 1 sample/second for 1 minute
    static constexpr size_t SAMPLES_5M = 300;   // 1 sample/second for 5 minutes
    static constexpr size_t SAMPLES_1H = 3600;  // 1 sample/second for 1 hour
    static constexpr size_t SAMPLES_24H = 1440;  // 1 sample/minute for 24 hours

    std::vector<SystemMetrics> samples_1m{SAMPLES_1M};
    std::vector<SystemMetrics> samples_5m{SAMPLES_5M};
    std::vector<SystemMetrics> samples_1h{SAMPLES_1H};
    std::vector<SystemMetrics> samples_24h{SAMPLES_24H};

    size_t index_1m{0}, index_5m{0}, index_1h{0}, index_24h{0};
    std::chrono::system_clock::time_point last_24h_sample{};
    RateStats
    calculateRates(
        const SystemMetrics& current,
        const std::vector<SystemMetrics>& samples,
        size_t window_size,
        double time_period_seconds)
    {
        RateStats stats{0.0, 0.0, 0.0, 0.0};
        if (window_size == 0)
            return stats;

        auto oldest = samples[window_size - 1];
        double elapsed = (current.timestamp - oldest.timestamp) /
            1000000.0;  // Convert microseconds to seconds
        if (elapsed <= 0)
            return stats;

        uint64_t net_in_diff =
            current.network_bytes_in - oldest.network_bytes_in;
        uint64_t net_out_diff =
            current.network_bytes_out - oldest.network_bytes_out;
        uint64_t disk_read_diff =
            current.disk_bytes_read - oldest.disk_bytes_read;
        uint64_t disk_write_diff =
            current.disk_bytes_written - oldest.disk_bytes_written;

        return {
            static_cast<double>(net_in_diff) / elapsed,
            static_cast<double>(net_out_diff) / elapsed,
            static_cast<double>(disk_read_diff) / elapsed,
            static_cast<double>(disk_write_diff) / elapsed};
    }

public:
    void
    addSample(const SystemMetrics& metrics)
    {
        auto now = std::chrono::system_clock::now();

        // Update 1-minute window (every second)
        samples_1m[index_1m++ % SAMPLES_1M] = metrics;

        // Update 5-minute window (every second)
        samples_5m[index_5m++ % SAMPLES_5M] = metrics;

        // Update 1-hour window (every second)
        samples_1h[index_1h++ % SAMPLES_1H] = metrics;

        // Update 24-hour window (every minute)
        if (last_24h_sample + std::chrono::minutes(1) <= now)
        {
            samples_24h[index_24h++ % SAMPLES_24H] = metrics;
            last_24h_sample = now;
        }
    }

    std::tuple<RateStats, RateStats, RateStats, RateStats>
    getRates(const SystemMetrics& current)
    {
        return {
            calculateRates(
                current, samples_1m, std::min(index_1m, SAMPLES_1M), 60),
            calculateRates(
                current, samples_5m, std::min(index_5m, SAMPLES_5M), 300),
            calculateRates(
                current, samples_1h, std::min(index_1h, SAMPLES_1H), 3600),
            calculateRates(
                current, samples_24h, std::min(index_24h, SAMPLES_24H), 86400)};
    }
};

class DatagramMonitor
{
private:
    Application& app_;
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    MetricsTracker metrics_tracker_;

    struct EndpointInfo
    {
        std::string ip;
        uint16_t port;
        bool is_ipv6;
    };
    EndpointInfo
    parseEndpoint(std::string const& endpoint)
    {
        auto space_pos = endpoint.find(' ');
        if (space_pos == std::string::npos)
            throw std::runtime_error("Invalid endpoint format");

        EndpointInfo info;
        info.ip = endpoint.substr(0, space_pos);
        info.port = std::stoi(endpoint.substr(space_pos + 1));
        info.is_ipv6 = info.ip.find(':') != std::string::npos;
        return info;
    }

    int
    createSocket(EndpointInfo const& endpoint)
    {
        int sock = socket(endpoint.is_ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
            throw std::runtime_error("Failed to create socket");
        std::cout << "DatagramMonitor createSocket = " << sock << "\n";
        return sock;
    }

    void
    sendPacket(
        int sock,
        EndpointInfo const& endpoint,
        std::vector<uint8_t> const& buffer)
    {
        struct sockaddr_storage addr;
        socklen_t addr_len;

        if (endpoint.is_ipv6)
        {
            struct sockaddr_in6* addr6 =
                reinterpret_cast<struct sockaddr_in6*>(&addr);
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(endpoint.port);
            inet_pton(AF_INET6, endpoint.ip.c_str(), &addr6->sin6_addr);
            addr_len = sizeof(struct sockaddr_in6);
        }
        else
        {
            struct sockaddr_in* addr4 =
                reinterpret_cast<struct sockaddr_in*>(&addr);
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(endpoint.port);
            inet_pton(AF_INET, endpoint.ip.c_str(), &addr4->sin_addr);
            addr_len = sizeof(struct sockaddr_in);
        }

        std::cout << "Datagram monitor sending packet of size " << buffer.size()
                  << "\n";
        sendto(
            sock,
            buffer.data(),
            buffer.size(),
            0,
            reinterpret_cast<struct sockaddr*>(&addr),
            addr_len);
    }

    SystemMetrics
    collectSystemMetrics()
    {
        SystemMetrics metrics;
        metrics.timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        // Get network stats from /proc/net/dev
        std::ifstream net_file("/proc/net/dev");
        std::string line;
        uint64_t total_bytes_in = 0, total_bytes_out = 0;
        while (std::getline(net_file, line))
        {
            if (line.find(':') != std::string::npos)
            {
                uint64_t bytes_in, bytes_out;
                sscanf(
                    line.c_str(),
                    "%*[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                    &bytes_in,
                    &bytes_out);
                total_bytes_in += bytes_in;
                total_bytes_out += bytes_out;
            }
        }
        metrics.network_bytes_in = total_bytes_in;
        metrics.network_bytes_out = total_bytes_out;

        // Get disk stats from /proc/diskstats
        std::ifstream disk_file("/proc/diskstats");
        uint64_t total_bytes_read = 0, total_bytes_written = 0;
        while (std::getline(disk_file, line))
        {
            unsigned int major, minor;
            char dev_name[32];
            uint64_t reads, read_sectors, writes, write_sectors;
            if (sscanf(
                    line.c_str(),
                    "%u %u %s %lu %*u %lu %*u %lu %*u %lu",
                    &major,
                    &minor,
                    dev_name,
                    &reads,
                    &read_sectors,
                    &writes,
                    &write_sectors) == 7)
            {
                // Skip partition entries and non-physical devices
                if (major <= 0 || minor % 16 == 0)
                    continue;
                total_bytes_read +=
                    read_sectors * 512;  // Sectors are 512 bytes
                total_bytes_written += write_sectors * 512;
            }
        }
        metrics.disk_bytes_read = total_bytes_read;
        metrics.disk_bytes_written = total_bytes_written;

        return metrics;
    }
    std::vector<uint8_t>
    generateServerInfo()
    {
        auto& ops = app_.getOPs();

        // Get the RangeSet directly
        auto rangeSet = app_.getLedgerMaster().getCompleteLedgersRangeSet();
        auto currentMetrics = collectSystemMetrics();
        metrics_tracker_.addSample(currentMetrics);

        // Count only non-zero intervals and calculate total size needed
        size_t validRangeCount = 0;
        for (auto const& interval : rangeSet) {
            // Skip intervals where both lower and upper are 0
            if (interval.lower() != 0 || interval.upper() != 0) {
                validRangeCount++;
            }
        }

        size_t totalSize = sizeof(ServerInfoHeader) + (validRangeCount * sizeof(LgrRange));


        // Allocate buffer and initialize header
        std::vector<uint8_t> buffer(totalSize);
        auto* header = reinterpret_cast<ServerInfoHeader*>(buffer.data());
        memset(header, 0, sizeof(ServerInfoHeader));

        // Set magic number and version
        header->magic = SERVER_INFO_MAGIC;
        header->version = SERVER_INFO_VERSION;
        header->network_id = app_.config().NETWORK_ID;
        header->timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        header->uptime = UptimeClock::now().time_since_epoch().count();
        header->io_latency_us = app_.getIOLatency().count();
        header->validation_quorum = app_.validators().quorum();

        if (!app_.config().reporting())
            header->peer_count = app_.overlay().size();

        header->node_size = app_.config().NODE_SIZE;

        // Pack warning flags
        if (ops.isAmendmentBlocked())
            header->warning_flags |= WARNING_AMENDMENT_BLOCKED;
        if (ops.isUNLBlocked())
            header->warning_flags |= WARNING_UNL_BLOCKED;
        if (ops.isAmendmentWarned())
            header->warning_flags |= WARNING_AMENDMENT_WARNED;

        // Pack consensus info
        auto& mConsensus = ops.getConsensus();
        header->proposer_count = mConsensus.prevProposers();
        header->converge_time_ms = mConsensus.prevRoundTime().count();

        // Pack fetch pack size if present
        auto& ledgerMaster = ops.getLedgerMaster();
        auto const lastClosed = ledgerMaster.getClosedLedger();
        auto const validated = ledgerMaster.getValidatedLedger();

        if (lastClosed && validated)
        {
            auto consensus =
                ledgerMaster.getLedgerByHash(lastClosed->info().hash);
            if (!consensus)
                consensus = app_.getInboundLedgers().acquire(
                    lastClosed->info().hash,
                    0,
                    InboundLedger::Reason::CONSENSUS);

            if (consensus &&
                (!ledgerMaster.canBeCurrent(consensus) ||
                 !ledgerMaster.isCompatible(
                     *consensus,
                     app_.journal("DatagramMonitor").debug(),
                     "Not switching")))
            {
                header->warning_flags |= WARNING_NOT_SYNCED;
            }
        }
        else
        {
            // If we don't have both lastClosed and validated ledgers, we're
            // definitely not synced
            header->warning_flags |= WARNING_NOT_SYNCED;
        }

        auto const fp = ledgerMaster.getFetchPackCacheSize();
        if (fp != 0)
            header->fetch_pack_size = fp;

        // Pack load factor info if not reporting
        if (!app_.config().reporting())
        {
            auto const escalationMetrics =
                app_.getTxQ().getMetrics(*app_.openLedger().current());
            auto const loadFactorServer = app_.getFeeTrack().getLoadFactor();
            auto const loadBaseServer = app_.getFeeTrack().getLoadBase();
            auto const loadFactorFeeEscalation =
                mulDiv(
                    escalationMetrics.openLedgerFeeLevel,
                    loadBaseServer,
                    escalationMetrics.referenceFeeLevel)
                    .second;

            header->load_factor = std::max(
                safe_cast<std::uint64_t>(loadFactorServer),
                loadFactorFeeEscalation);
            header->load_base = loadBaseServer;
        }

        // Get system info
        struct sysinfo si;
        if (sysinfo(&si) == 0)
        {
            header->system_memory_total = si.totalram * si.mem_unit;
            header->system_memory_free = si.freeram * si.mem_unit;
            header->system_memory_used =
                header->system_memory_total - header->system_memory_free;
            header->load_avg_1min = si.loads[0] / (float)(1 << SI_LOAD_SHIFT);
            header->load_avg_5min = si.loads[1] / (float)(1 << SI_LOAD_SHIFT);
            header->load_avg_15min = si.loads[2] / (float)(1 << SI_LOAD_SHIFT);
        }

        // Get process memory usage
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        header->process_memory_pages = usage.ru_maxrss;

        // Get disk usage
        struct statvfs fs;
        if (statvfs("/", &fs) == 0)
        {
            header->system_disk_total = fs.f_blocks * fs.f_frsize;
            header->system_disk_free = fs.f_bfree * fs.f_frsize;
            header->system_disk_used =
                header->system_disk_total - header->system_disk_free;
        }

        // Get rate statistics
        auto [rates_1m, rates_5m, rates_1h, rates_24h] =
            metrics_tracker_.getRates(currentMetrics);
        header->rates.network_in = rates_1m;
        header->rates.network_out = rates_5m;
        header->rates.disk_read = rates_1h;
        header->rates.disk_write = rates_24h;

        // Pack ledger info and ranges
        auto lpClosed = ledgerMaster.getValidatedLedger();
        if (!lpClosed && !app_.config().reporting())
            lpClosed = ledgerMaster.getClosedLedger();

        if (lpClosed)
        {
            header->ledger_seq = lpClosed->info().seq;
            auto const& hash = lpClosed->info().hash;
            std::memcpy(header->ledger_hash, hash.data(), 32);
            header->reserve_base = lpClosed->fees().accountReserve(0).drops();
            header->reserve_inc = lpClosed->fees().increment.drops();
        }

        // Pack node public key
        auto const& nodeKey = app_.nodeIdentity().first;
        std::memcpy(header->node_public_key, nodeKey.data(), 32);

        // Set the complete ledger count
        header->ledger_range_count = validRangeCount;

        // Append only non-zero ranges after the header
        auto* rangeData = reinterpret_cast<LgrRange*>(buffer.data() + sizeof(ServerInfoHeader));
        size_t i = 0;
        for (auto const& interval : rangeSet) {
            // Only pack non-zero ranges
            if (interval.lower() != 0 || interval.upper() != 0) {
                rangeData[i].start = interval.lower();
                rangeData[i].end = interval.upper();
                ++i;
            }
        }

        return buffer;
    }
    void
    monitorThread()
    {
        auto endpoint = parseEndpoint(app_.config().DATAGRAM_MONITOR);
        std::cout << "Datagram monitor, endpoint: "
                  << app_.config().DATAGRAM_MONITOR << "\n";
        int sock = createSocket(endpoint);

        while (running_)
        {
            try
            {
                auto info = generateServerInfo();
                sendPacket(sock, endpoint, info);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::cout << "Sending datagram monitor packet\n";
            }
            catch (const std::exception& e)
            {
                // Log error but continue monitoring
                JLOG(app_.journal("DatagramMonitor").error())
                    << "Server info monitor error: " << e.what();
            }
        }

        close(sock);
    }

public:
    DatagramMonitor(Application& app) : app_(app)
    {
    }

    void
    start()
    {
        if (!running_.exchange(true))
        {
            monitor_thread_ =
                std::thread(&DatagramMonitor::monitorThread, this);
        }
    }

    void
    stop()
    {
        if (running_.exchange(false))
        {
            if (monitor_thread_.joinable())
                monitor_thread_.join();
        }
    }

    ~DatagramMonitor()
    {
        stop();
    }
};
}  // namespace ripple
#endif
