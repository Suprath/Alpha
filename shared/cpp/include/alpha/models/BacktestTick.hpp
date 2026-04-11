#pragma once

#include <cstdint>

namespace alpha::models {

/**
 * BacktestTick — Minimal 32-byte tick for high-throughput replay.
 *
 * Layout optimized for:
 *   - AVX2 SIMD: 8 BacktestTicks per 256-bit YMM register
 *   - NVMe sequential reads: 200M ticks/s @ 6.4 GB/s
 *   - Cache packing: 2 ticks per 64-byte cache line
 *
 * Derived from QuestDB candles via Candle::normalize_to_ticks()
 * or from the live ring buffer export path.
 */
struct alignas(32) BacktestTick {
    uint64_t timestamp_ns;      // Exchange time (IST nanoseconds since epoch)
    uint32_t instrument_token;  // Routing ID — same domain as live Tick
    float    last_price;        // Last traded price
    uint32_t volume;            // Cumulative volume at this tick
    float    bid_price;         // Best bid (0.0 if unavailable)
    float    ask_price;         // Best ask (0.0 if unavailable)
    float    vwap;              // Running VWAP at this tick
    // Implicit 4-byte padding → 32 bytes total
};
static_assert(sizeof(BacktestTick) == 32, "BacktestTick must be exactly 32 bytes");
static_assert(alignof(BacktestTick) == 32, "BacktestTick must be 32-byte aligned");

/**
 * AlphaFileHeader — 64-byte header at offset 0 of every .alpha flat file.
 *
 * After the header, the file contains a contiguous array of BacktestTick[].
 * The file is designed for O_RDONLY + mmap + MADV_SEQUENTIAL iteration.
 */
struct alignas(64) AlphaFileHeader {
    uint64_t magic;             // 0x414C50484154494B ("ALPHATIK" LE)
    uint32_t version;           // Format version — currently 1
    uint32_t instrument_token;  // Which instrument this file covers
    uint64_t start_ts_ns;       // Timestamp of first tick
    uint64_t end_ts_ns;         // Timestamp of last tick
    uint64_t tick_count;        // Number of BacktestTick records that follow
    uint32_t tick_size_bytes;   // sizeof(BacktestTick) — sanity check on load
    uint32_t flags;             // Reserved, must be 0
    uint64_t reserved[2];       // Pad to 64 bytes
};
static_assert(sizeof(AlphaFileHeader) == 64, "AlphaFileHeader must be exactly 64 bytes");

static constexpr uint64_t ALPHA_FILE_MAGIC = 0x414C50484154494BULL;  // "ALPHATIK"
static constexpr uint32_t ALPHA_FILE_VERSION = 1;

} // namespace alpha::models
