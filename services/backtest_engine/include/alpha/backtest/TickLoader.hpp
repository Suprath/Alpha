#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <alpha/models/BacktestTick.hpp>
#include <alpha/models/BacktestSignal.hpp>

namespace alpha {
namespace backtest {
// Keyed by bar-open timestamp_ns (rounded to minute boundary).
// Bar key = (tick.timestamp_ns / 60_000_000_000) * 60_000_000_000
using SignalMap = std::unordered_map<uint64_t, models::BacktestSignal>;
}
}

namespace alpha {
namespace backtest {

/**
 * LoaderMode — selects the data source for TickLoader.
 */
enum class LoaderMode {
    BINARY_MMAP,  // 200M ticks/sec — mmap .alpha flat files (zero-copy)
    QUESTDB_SQL,  // Convenience — QuestDB PostgreSQL wire protocol (port 8812)
};

/**
 * TickLoader — loads BacktestTick data for simulation replay.
 *
 * ── BINARY_MMAP mode (production / performance path) ──────────────────────
 *   The .alpha file format:
 *     [AlphaFileHeader (64 bytes)] [BacktestTick × N (32 bytes each)]
 *
 *   open_file() mmap's the entire file with:
 *     mmap(MAP_PRIVATE | MAP_POPULATE) + madvise(MADV_SEQUENTIAL | MADV_WILLNEED)
 *
 *   Returns a raw const pointer. The SimRunner iterates via pointer arithmetic —
 *   zero deserialization, zero heap allocation on the hot path.
 *
 *   At 32 bytes/tick and 200M ticks/s:  6.4 GB/s memory read bandwidth.
 *   NVMe SSD sequential read: ~7 GB/s. In-RAM (page cache warm): ~50+ GB/s.
 *
 * ── QUESTDB_SQL mode (convenience / small datasets) ────────────────────────
 *   Connects to QuestDB's Postgres wire protocol (port 8812) via libpqxx.
 *   Streams rows into std::vector<BacktestTick>.
 *   Also provides export_to_alpha_file() to convert QDB data to .alpha format.
 *
 * Not thread-safe — one TickLoader per thread.
 */
class TickLoader {
public:
    explicit TickLoader(LoaderMode mode = LoaderMode::BINARY_MMAP);
    ~TickLoader();

    // Non-copyable
    TickLoader(const TickLoader&)            = delete;
    TickLoader& operator=(const TickLoader&) = delete;

    // ── BINARY_MMAP mode ───────────────────────────────────────────────────

    /**
     * Open and mmap a .alpha file.
     * @param path         Path to the .alpha binary file.
     * @param out_count    Set to the number of BacktestTick records.
     * @return Pointer to the first BacktestTick in the mmap'd region.
     *         Valid until close_file() is called.
     * @throws std::runtime_error on invalid header or mmap failure.
     */
    const models::BacktestTick* open_file(const std::string& path,
                                          size_t&            out_count);

    /** Unmap and close the current file. Safe to call multiple times. */
    void close_file() noexcept;

    // ── QUESTDB_SQL mode ───────────────────────────────────────────────────

    /**
     * Load ticks for one instrument and time range from QuestDB.
     *
     * SQL executed:
     *   SELECT timestamp, instrument_token, last_price, volume,
     *          bid_price, ask_price, vwap
     *   FROM backtest_ticks
     *   WHERE instrument_token = $token
     *     AND timestamp BETWEEN $start AND $end
     *   ORDER BY timestamp
     *
     * @param qdb_pg_dsn   "host=questdb port=8812 dbname=qdb user=admin password=quest"
     */
    std::vector<models::BacktestTick> load_from_questdb(
        const std::string& qdb_pg_dsn,
        uint32_t           instrument_token,
        uint64_t           start_ns,
        uint64_t           end_ns);

    /**
     * Export QuestDB data to a binary .alpha file (one-time conversion).
     * After export, use BINARY_MMAP mode for fast replay.
     * @return Number of ticks exported.
     */
    size_t export_to_alpha_file(
        const std::string& qdb_pg_dsn,
        uint32_t           instrument_token,
        uint64_t           start_ns,
        uint64_t           end_ns,
        const std::string& output_path);

    // ── Signal loading (backtest_signals table) ────────────────────────────

    /**
     * Load pre-computed signals from QuestDB backtest_signals table.
     * Written by signal_engine in --backtest-batch mode.
     */
    std::vector<models::BacktestSignal> load_signals_from_questdb(
        const std::string& qdb_pg_dsn,
        uint32_t           instrument_token,
        uint64_t           start_ns,
        uint64_t           end_ns);

    /**
     * Load signals as a timestamp-keyed map for O(1) join in SimRunner.
     * Key = bar-open timestamp_ns = (row.timestamp_ns / 60_000_000_000) * 60_000_000_000
     */
    SignalMap load_signals_as_map(
        const std::string& qdb_pg_dsn,
        uint32_t           instrument_token,
        uint64_t           start_ns,
        uint64_t           end_ns);

private:
    LoaderMode  mode_;
    int         fd_{-1};
    void*       mmap_base_{nullptr};
    size_t      mmap_size_{0};
};

} // namespace backtest
} // namespace alpha
