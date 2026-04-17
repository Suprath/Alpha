#pragma once

#include <cstdint>
#include <cstdbool>

namespace alpha::models {

/**
 * BacktestSignal — Pre-computed bar-level signals for backtest replay.
 *
 * Written by: signal_engine in --backtest-batch mode (one row per 1-minute bar)
 * Read by:    backtest_engine TickLoader via QuestDB Postgres wire
 * Transport:  QuestDB table `backtest_signals`, partitioned by DAY
 *
 * Availability:
 *   OHLCV-derived (always valid after warmup): log_return, realized_vol_ann,
 *   rsi_14, macd_*, bb_*, vwap_deviation
 *
 *   Orderflow-dependent (ZERO in backtest mode — not faked):
 *   bar_ofi — requires tick direction classification (unavailable in backfill data)
 *
 * Timestamp convention:
 *   timestamp_ns = bar OPEN time, 1-minute aligned (IST, UTC epoch nanoseconds)
 *   Join key in SimRunner: tick.timestamp_ns rounded DOWN to minute boundary
 *   i.e. bar_ts = (tick.timestamp_ns / 60_000_000_000ULL) * 60_000_000_000ULL
 */
struct BacktestSignal {
    // ── Identity ──────────────────────────────────────────────────────────────
    uint64_t timestamp_ns;          // Bar open time (UTC epoch ns, 1-minute aligned)
    uint32_t instrument_token;

    // ── Bar-level: OHLCV-derived (always computable from historical data) ─────
    double   log_return;            // ln(C_t / C_{t-1}), 0.0 on first bar
    double   realized_vol_ann;      // Annualized σ from 20-bar rolling window; 0.0 if < 20 bars

    // ── OFI: orderflow-dependent — left 0.0 in backtest mode ─────────────────
    float    bar_ofi;               // (buy_vol − sell_vol) / vol ∈ [−1, +1]; 0.0 in backtest

    // ── RSI(14) ───────────────────────────────────────────────────────────────
    double   rsi_14;                // Wilder's RSI ∈ [0, 100]; 0.0 if not valid
    bool     valid_rsi;             // true after 14 bars

    // ── MACD(12, 26, 9) ───────────────────────────────────────────────────────
    double   macd_line;             // EMA(12) − EMA(26)
    double   macd_signal;           // EMA(9) of macd_line
    double   macd_histogram;        // macd_line − macd_signal
    bool     valid_macd;            // true after 35 bars (26 + 9)

    // ── Bollinger Bands(20, 2.0) ──────────────────────────────────────────────
    double   bb_upper;              // SMA(20) + 2σ
    double   bb_middle;             // SMA(20)
    double   bb_lower;              // SMA(20) − 2σ
    double   bb_pct_b;              // (close − bb_lower) / (bb_upper − bb_lower) ∈ [0, 1]
    double   bb_bandwidth;          // (bb_upper − bb_lower) / bb_middle
    bool     valid_bb;              // true after 20 bars

    // ── VWAP Deviation (session-accumulated) ──────────────────────────────────
    double   vwap_deviation;        // (close − session_vwap) / session_vwap
    double   session_vwap;          // Cumulative VWAP since session open
    bool     valid_vwap_dev;        // true after first bar with volume > 0
};

} // namespace alpha::models
