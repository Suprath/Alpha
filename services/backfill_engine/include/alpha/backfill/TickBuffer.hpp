#pragma once

#include <vector>
#include <functional>
#include <cstdint>
#include <cmath>
#include <alpha/models/MarketModels.hpp>
#include <alpha/models/BacktestTick.hpp>

namespace alpha {
namespace backfill {

/**
 * PreCalcBar — OHLCV candle enriched with derived signals.
 *
 * Stored in QuestDB precalc_bars table. Allows $BTEST to skip signal
 * re-computation entirely — the hot-loop only reads pre-baked values.
 */
struct PreCalcBar {
    models::Candle  candle;
    uint32_t        instrument_token;
    std::string     symbol;
    std::string     interval;

    // Derived signals (computed by TickBuffer)
    double ofi;          // Order Flow Imbalance proxy: Σ(vol × sign(close-open))
    double kyle_lambda;  // Kyle's Lambda: Σ|ΔP| / Σ|ΔV| over rolling window (₹/share/100sh)
    double vwap;         // VWAP at bar close: Σ(P×V) / ΣV
    double realized_vol; // Close-to-close realized volatility (annualized)
};

/**
 * TickBuffer — staged pipeline: Raw Candles → PreCalcBar → BacktestTick[].
 *
 * Staged architecture rationale:
 *   1. Raw candle in → compute rolling signals → emit PreCalcBar (written to QDB)
 *   2. Normalize bar → 4 synthetic BacktestTicks (OHLC order) → written to QDB
 *
 * Rolling computations use a fixed-size window for O(1) amortized updates.
 *
 * Signal formulas:
 *   OFI proxy   = volume × sign(close − open)  per bar; rolling Σ over window
 *   Kyle's λ    = Σ|close[i] − close[i-1]| / Σvolume[i]  over window (× 100)
 *   VWAP        = Σ(close × volume) / Σvolume  cumulative within session
 *   Realized vol = annualized std-dev of log-returns over window
 *                  σ_annual = σ_bar × √(bars_per_year)
 */
class TickBuffer {
public:
    static constexpr size_t DEFAULT_WINDOW     = 20;
    static constexpr double BARS_PER_YEAR_1M   = 375.0 * 252.0;   // 1-min bars
    static constexpr double BARS_PER_YEAR_5M   = 75.0  * 252.0;   // 5-min bars
    static constexpr double BARS_PER_YEAR_1D   = 252.0;

    using BarCallback  = std::function<void(const PreCalcBar&)>;
    using TickCallback = std::function<void(const models::BacktestTick&,
                                            const std::string& symbol)>;

    /**
     * @param rolling_window  Number of bars for rolling signal computations.
     * @param bars_per_year   For realized-vol annualization (default = 1-min).
     */
    explicit TickBuffer(size_t rolling_window  = DEFAULT_WINDOW,
                        double bars_per_year   = BARS_PER_YEAR_1M);

    /**
     * Ingest a raw candle.
     *   - Computes PreCalcBar and calls bar_cb once.
     *   - Normalizes to 4 BacktestTicks (OHLC) and calls tick_cb for each.
     *   - Both callbacks may be null (skip that output path).
     */
    void push(const models::Candle& candle,
              uint32_t              instrument_token,
              const std::string&    symbol,
              const std::string&    interval,
              BarCallback           bar_cb,
              TickCallback          tick_cb);

    /** Reset all state (call between instruments or sessions). */
    void reset();

    uint64_t bars_processed() const noexcept { return bars_processed_; }

private:
    double compute_ofi()           const;
    double compute_kyle_lambda()   const;
    double compute_realized_vol()  const;

    size_t  rolling_window_;
    double  bars_per_year_;

    // Rolling window storage
    std::vector<models::Candle> window_;     // Circular: last N candles
    size_t                      window_head_{0};
    bool                        window_full_{false};

    // Cumulative VWAP accumulators (reset each session)
    double  cumul_pv_{0.0};
    double  cumul_v_{0.0};

    uint64_t bars_processed_{0};
};

} // namespace backfill
} // namespace alpha
