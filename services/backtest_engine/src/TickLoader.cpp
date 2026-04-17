#include <alpha/backtest/TickLoader.hpp>
#include <alpha/models/BacktestTick.hpp>

#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <fstream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <pqxx/pqxx>

namespace alpha {
namespace backtest {

TickLoader::TickLoader(LoaderMode mode)
    : mode_(mode)
{}

TickLoader::~TickLoader() {
    close_file();
}

// ── BINARY_MMAP mode ───────────────────────────────────────────────────────

const models::BacktestTick* TickLoader::open_file(const std::string& path,
                                                    size_t&            out_count) {
    close_file();

    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0)
        throw std::runtime_error("TickLoader: cannot open " + path + ": " + std::strerror(errno));

    struct stat st{};
    if (::fstat(fd_, &st) < 0)
        throw std::runtime_error("TickLoader: fstat failed: " + std::string(std::strerror(errno)));

    mmap_size_ = static_cast<size_t>(st.st_size);
    if (mmap_size_ < sizeof(models::AlphaFileHeader))
        throw std::runtime_error("TickLoader: file too small to contain header: " + path);

    mmap_base_ = ::mmap(nullptr, mmap_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mmap_base_ == MAP_FAILED) {
        mmap_base_ = nullptr;
        throw std::runtime_error("TickLoader: mmap failed: " + std::string(std::strerror(errno)));
    }

    // Prefetch hints: we read the file sequentially at full speed
    ::madvise(mmap_base_, mmap_size_, MADV_SEQUENTIAL | MADV_WILLNEED);

    // Validate header
    const auto* hdr = reinterpret_cast<const models::AlphaFileHeader*>(mmap_base_);
    if (hdr->magic != models::ALPHA_FILE_MAGIC)
        throw std::runtime_error("TickLoader: invalid magic in " + path);
    if (hdr->version != models::ALPHA_FILE_VERSION)
        throw std::runtime_error("TickLoader: unsupported file version " + std::to_string(hdr->version));
    if (hdr->tick_size_bytes != sizeof(models::BacktestTick))
        throw std::runtime_error("TickLoader: tick_size mismatch — file was built with different struct layout");

    out_count = static_cast<size_t>(hdr->tick_count);

    // Verify file size is consistent
    size_t expected = sizeof(models::AlphaFileHeader)
                    + out_count * sizeof(models::BacktestTick);
    if (mmap_size_ < expected)
        throw std::runtime_error("TickLoader: file truncated — expected " +
                                  std::to_string(expected) + " bytes, got " +
                                  std::to_string(mmap_size_));

    // Return pointer directly into the mmap'd region (zero-copy)
    return reinterpret_cast<const models::BacktestTick*>(
        static_cast<const char*>(mmap_base_) + sizeof(models::AlphaFileHeader));
}

void TickLoader::close_file() noexcept {
    if (mmap_base_ && mmap_base_ != MAP_FAILED) {
        ::munmap(mmap_base_, mmap_size_);
        mmap_base_ = nullptr;
        mmap_size_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ── QUESTDB_SQL mode ───────────────────────────────────────────────────────

std::vector<models::BacktestTick> TickLoader::load_from_questdb(
    const std::string& qdb_pg_dsn,
    uint32_t           instrument_token,
    uint64_t           start_ns,
    uint64_t           end_ns)
{
    std::vector<models::BacktestTick> out;

    pqxx::connection conn(qdb_pg_dsn);
    pqxx::work txn(conn);

    // QuestDB stores timestamps in microseconds. Convert ns → μs for the query.
    // QuestDB timestamp literal: '2024-01-01T09:15:00.000000Z'
    // Use epoch microseconds directly via to_timestamp()
    uint64_t start_us = start_ns / 1000;
    uint64_t end_us   = end_ns   / 1000;

    std::string sql =
        "SELECT "
        "  cast(timestamp as long) as ts_us, "  // microseconds since epoch
        "  instrument_token, last_price, volume, "
        "  bid_price, ask_price, vwap "
        "FROM backtest_ticks "
        "WHERE instrument_token = " + txn.quote(static_cast<int64_t>(instrument_token)) +
        "  AND timestamp >= cast(" + std::to_string(start_us) + " as timestamp) "
        "  AND timestamp <= cast(" + std::to_string(end_us) + " as timestamp) "
        "ORDER BY timestamp";

    auto result = txn.exec(sql);
    out.reserve(result.size());

    for (const auto& row : result) {
        models::BacktestTick t{};
        t.timestamp_ns     = row[0].as<uint64_t>() * 1000ULL;  // μs → ns
        t.instrument_token = row[1].as<uint32_t>();
        t.last_price       = static_cast<float>(row[2].as<double>());
        t.volume           = row[3].as<uint32_t>();
        t.bid_price        = static_cast<float>(row[4].as<double>());
        t.ask_price        = static_cast<float>(row[5].as<double>());
        t.vwap             = static_cast<float>(row[6].as<double>());
        out.push_back(t);
    }

    return out;
}

size_t TickLoader::export_to_alpha_file(
    const std::string& qdb_pg_dsn,
    uint32_t           instrument_token,
    uint64_t           start_ns,
    uint64_t           end_ns,
    const std::string& output_path)
{
    auto ticks = load_from_questdb(qdb_pg_dsn, instrument_token, start_ns, end_ns);
    if (ticks.empty()) return 0;

    // Build header
    models::AlphaFileHeader hdr{};
    hdr.magic            = models::ALPHA_FILE_MAGIC;
    hdr.version          = models::ALPHA_FILE_VERSION;
    hdr.instrument_token = instrument_token;
    hdr.start_ts_ns      = ticks.front().timestamp_ns;
    hdr.end_ts_ns        = ticks.back().timestamp_ns;
    hdr.tick_count       = ticks.size();
    hdr.tick_size_bytes  = sizeof(models::BacktestTick);
    hdr.flags            = 0;

    // Write to file
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("TickLoader: cannot create file: " + output_path);

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(ticks.data()),
              static_cast<std::streamsize>(ticks.size() * sizeof(models::BacktestTick)));

    if (!out)
        throw std::runtime_error("TickLoader: write failed to: " + output_path);

    return ticks.size();
}

// ── Signal loading ─────────────────────────────────────────────────────────────

std::vector<models::BacktestSignal> TickLoader::load_signals_from_questdb(
    const std::string& qdb_pg_dsn,
    uint32_t           instrument_token,
    uint64_t           start_ns,
    uint64_t           end_ns)
{
    std::vector<models::BacktestSignal> out;

    pqxx::connection conn(qdb_pg_dsn);
    pqxx::work txn(conn);

    const uint64_t start_us = start_ns / 1000ULL;
    const uint64_t end_us   = end_ns   / 1000ULL;

    const std::string sql =
        "SELECT cast(timestamp as long) as ts_us, instrument_token, "
        "  log_return, realized_vol_ann, bar_ofi, "
        "  rsi_14, macd_line, macd_signal, macd_histogram, "
        "  bb_upper, bb_middle, bb_lower, bb_pct_b, bb_bandwidth, "
        "  vwap_deviation, session_vwap, "
        "  valid_rsi, valid_macd, valid_bb, valid_vwap_dev "
        "FROM backtest_signals "
        "WHERE instrument_token = " + txn.quote(static_cast<int64_t>(instrument_token)) +
        "  AND timestamp >= cast(" + std::to_string(start_us) + " as timestamp)"
        "  AND timestamp <= cast(" + std::to_string(end_us)   + " as timestamp)"
        " ORDER BY timestamp";

    const auto result = txn.exec(sql);
    out.reserve(result.size());

    for (const auto& row : result) {
        models::BacktestSignal s{};
        s.timestamp_ns     = row[0].as<uint64_t>() * 1000ULL;  // μs → ns
        s.instrument_token = row[1].as<uint32_t>();
        s.log_return       = row[2].as<double>();
        s.realized_vol_ann = row[3].as<double>();
        s.bar_ofi          = static_cast<float>(row[4].as<double>());
        s.rsi_14           = row[5].as<double>();
        s.macd_line        = row[6].as<double>();
        s.macd_signal      = row[7].as<double>();
        s.macd_histogram   = row[8].as<double>();
        s.bb_upper         = row[9].as<double>();
        s.bb_middle        = row[10].as<double>();
        s.bb_lower         = row[11].as<double>();
        s.bb_pct_b         = row[12].as<double>();
        s.bb_bandwidth     = row[13].as<double>();
        s.vwap_deviation   = row[14].as<double>();
        s.session_vwap     = row[15].as<double>();
        s.valid_rsi        = row[16].as<bool>();
        s.valid_macd       = row[17].as<bool>();
        s.valid_bb         = row[18].as<bool>();
        s.valid_vwap_dev   = row[19].as<bool>();
        out.push_back(s);
    }

    return out;
}

SignalMap TickLoader::load_signals_as_map(
    const std::string& qdb_pg_dsn,
    uint32_t           instrument_token,
    uint64_t           start_ns,
    uint64_t           end_ns)
{
    auto signals = load_signals_from_questdb(qdb_pg_dsn, instrument_token, start_ns, end_ns);
    SignalMap map;
    map.reserve(signals.size());

    static constexpr uint64_t ONE_MINUTE_NS = 60'000'000'000ULL;
    for (auto& sig : signals) {
        // Normalise to bar-open minute boundary (key used by SimRunner lookup)
        const uint64_t bar_ts = (sig.timestamp_ns / ONE_MINUTE_NS) * ONE_MINUTE_NS;
        map.emplace(bar_ts, std::move(sig));
    }

    return map;
}

} // namespace backtest
} // namespace alpha
