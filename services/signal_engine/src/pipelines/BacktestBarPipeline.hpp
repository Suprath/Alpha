/**
 * @file BacktestBarPipeline.hpp
 * @brief OHLCV-only signal pipeline for backtest batch mode.
 *
 * Computes only signals that can be derived from OHLCV + VWAP data.
 * Orderflow-dependent signals (OFI, VPIN, TradeDirection, KylesLambda, Entropy)
 * are left at 0.0 — they are NOT approximated.
 *
 * Signals computed:
 *   - LogReturn        r(t) = ln(C_t / C_{t-1})
 *   - RealizedVol      annualized σ from 20-bar rolling window
 *   - RSI(14)          Wilder's smoothed RSI
 *   - MACD(12,26,9)    EMA crossover + signal + histogram
 *   - BollingerBands(20,2) SMA ± 2σ, %B, bandwidth
 *   - VWAPDeviation    session-accumulated VWAP normalized deviation
 *
 * Each bar produces one BacktestSignal written to QuestDB via ILP.
 * Session resets (VWAP, log return) happen when bar timestamp crosses 09:15 IST.
 */

#pragma once

#include <iostream>
#include <cstdint>

#include <core/EnhancedBar.hpp>
#include <core/QuestDBClient.hpp>
#include <signals/bar/LogReturn.hpp>
#include <signals/bar/RealizedVolatility.hpp>
#include <signals/bar/RSI.hpp>
#include <signals/bar/MACD.hpp>
#include <signals/bar/BollingerBands.hpp>
#include <signals/bar/VWAPDeviation.hpp>
#include <alpha/models/BacktestSignal.hpp>

namespace bbt_sig  = alpha::signal::bar_sig;
namespace bar_core = alpha::signal::core;
namespace live_bar = alpha::signal::signals::bar;

namespace alpha::signal::pipelines {

// IST session open = 09:15 = 33300 seconds from midnight UTC+5:30
// In UTC nanoseconds from midnight: (9*3600 + 15*60 - 5*3600 - 30*60) = 13500s = 13500e9 ns/day
static constexpr uint64_t NS_PER_DAY    = 86'400'000'000'000ULL;
static constexpr uint64_t NS_SESSION_OPEN_UTC = 13'500'000'000'000ULL; // 03:45 UTC = 09:15 IST

class BacktestBarPipeline {
public:
    explicit BacktestBarPipeline(bar_core::QuestDBClient& qdb)
        : qdb_(qdb) {}

    /**
     * Process one EnhancedBar (built from OHLCV candle data).
     * buy_volume, sell_volume, bar_ofi expected to be 0 (OHLCV mode).
     * Writes one BacktestSignal row to QuestDB.
     */
    void on_bar(const bar_core::EnhancedBar& bar) {
        // Detect session boundary — reset VWAP and log return state at 09:15 IST
        const uint64_t time_of_day_ns = bar.timestamp_ns % NS_PER_DAY;
        if (time_of_day_ns <= NS_SESSION_OPEN_UTC + 60'000'000'000ULL &&
            last_session_ts_ != bar.timestamp_ns / NS_PER_DAY) {
            log_return_.reset();
            vwap_dev_.reset_session();
            last_session_ts_ = bar.timestamp_ns / NS_PER_DAY;
        }

        // ── Run all OHLCV-derivable signal calculators ────────────────────────
        const auto lr   = log_return_.update(bar);
        const auto rv   = rv_.update(bar.instrument_token, lr.valid ? lr.r : 0.0);
        const auto rsi  = rsi_.update(bar);
        const auto macd = macd_.update(bar);
        const auto bb   = bb_.update(bar);
        const auto vd   = vwap_dev_.update(bar);

        // ── Assemble BacktestSignal ───────────────────────────────────────────
        alpha::models::BacktestSignal sig{};
        sig.timestamp_ns      = bar.timestamp_ns;
        sig.instrument_token  = bar.instrument_token;

        // Bar-level
        sig.log_return        = lr.valid ? lr.r : 0.0;
        sig.realized_vol_ann  = rv.valid ? rv.sigma_ann : 0.0;
        sig.bar_ofi           = 0.0f;  // Orderflow unavailable in backtest mode

        // RSI
        sig.rsi_14   = rsi.valid ? rsi.rsi : 0.0;
        sig.valid_rsi = rsi.valid;

        // MACD
        sig.macd_line      = macd.valid ? macd.macd_line      : 0.0;
        sig.macd_signal    = macd.valid ? macd.macd_signal    : 0.0;
        sig.macd_histogram = macd.valid ? macd.macd_histogram : 0.0;
        sig.valid_macd     = macd.valid;

        // Bollinger Bands
        sig.bb_upper     = bb.valid ? bb.upper     : 0.0;
        sig.bb_middle    = bb.valid ? bb.sma       : 0.0;
        sig.bb_lower     = bb.valid ? bb.lower     : 0.0;
        sig.bb_pct_b     = bb.valid ? bb.pct_b     : 0.0;
        sig.bb_bandwidth = bb.valid ? bb.bandwidth : 0.0;
        sig.valid_bb     = bb.valid;

        // VWAP deviation
        sig.vwap_deviation  = vd.valid ? vd.deviation    : 0.0;
        sig.session_vwap    = vd.valid ? vd.session_vwap : 0.0;
        sig.valid_vwap_dev  = vd.valid;

        // ── Write to QuestDB ──────────────────────────────────────────────────
        const std::string symbol = "TOKEN_" + std::to_string(bar.instrument_token);
        qdb_.write_backtest_signal(sig, symbol);

        ++bars_processed_;
        if (bars_processed_ % 1000 == 0) {
            std::cout << "[BacktestBarPipeline] " << bars_processed_
                      << " bars processed for token " << bar.instrument_token
                      << std::endl;
        }
    }

    void reset_session() noexcept {
        log_return_.reset();
        vwap_dev_.reset_session();
    }

    uint64_t bars_processed() const noexcept { return bars_processed_; }

private:
    bar_core::QuestDBClient& qdb_;

    live_bar::LogReturnCalculator           log_return_;
    live_bar::RealizedVolatilityCalculator  rv_;
    bbt_sig::RSICalculator                  rsi_;
    bbt_sig::MACDCalculator                 macd_;
    bbt_sig::BollingerBandsCalculator       bb_;
    bbt_sig::VWAPDeviationCalculator        vwap_dev_;

    uint64_t bars_processed_  = 0;
    uint64_t last_session_ts_ = 0;  // Day number of last session reset
};

} // namespace alpha::signal::pipelines
