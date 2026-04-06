/**
 * @file BarAccumulator.hpp
 * @brief Tick → EnhancedBar aggregator, replacing the basic TickAccumulator.
 *
 * Accumulates ticks into 1-minute IST-aligned bars. Requires trade direction
 * d(t) (from TradeDirectionCalculator) to compute buy/sell volume split and bar OFI.
 *
 * Bar fields computed:
 *   O, H, L, C   — running OHLC from tick.last_price
 *   Volume        — Σ tick.last_quantity
 *   VWAP          — Σ(P·v) / Σv  (internal running sums, finalized at close)
 *   BuyVolume     — Σ v · 1[d=+1]
 *   SellVolume    — Σ v · 1[d=−1]
 *   TickCount     — Σ 1
 *   BarOFI        — (V^B − V^S) / V  at close
 *
 * Note: d=0 (indeterminate) ticks still contribute to Volume, VWAP, and TickCount
 * but not to BuyVolume or SellVolume.
 */

#pragma once

#include "EnhancedBar.hpp"
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>
#include <unordered_map>
#include <functional>
#include <algorithm>  // std::max, std::min
#include <iostream>

namespace alpha::signal::core {

class BarAccumulator {
public:
    using BarCallback = std::function<void(const EnhancedBar&)>;

    explicit BarAccumulator(BarCallback callback = nullptr)
        : callback_(std::move(callback)) {}

    void set_callback(BarCallback cb) { callback_ = std::move(cb); }

    /**
     * @brief Process one tick and update the current bar for its instrument.
     *
     * @param tick      Current market tick
     * @param direction Trade direction: +1 buy, -1 sell, 0 indeterminate
     *                  (from TradeDirectionCalculator — computed before this call)
     */
    void process(const alpha::models::Tick& tick, int8_t direction) {
        const uint64_t ts_ist  = tick.timestamp_ns + alpha::time::Timestamp::IST_OFFSET_NS;
        const uint64_t bar_ts  = (ts_ist / 60'000'000'000ULL) * 60'000'000'000ULL;

        BarState& s = active_bars_[tick.instrument_token];

        // ── Bar boundary check ────────────────────────────────────────────────
        if (s.bar_ts_ns != 0 && bar_ts > s.bar_ts_ns) {
            // New minute — emit the completed bar, then reset
            if (callback_) callback_(finalize(s, tick.instrument_token));
            s = BarState{};
        }

        const double   price = tick.last_price;
        const uint64_t vol   = static_cast<uint64_t>(tick.last_quantity);

        // ── Initialize new bar ────────────────────────────────────────────────
        if (s.bar_ts_ns == 0) {
            s.bar_ts_ns       = bar_ts;
            s.open            = price;
            s.high            = price;
            s.low             = price;
            s.open_interest   = tick.open_interest;
        }

        // ── Running update ────────────────────────────────────────────────────
        if (price > s.high) s.high = price;
        if (price < s.low)  s.low  = price;
        s.close         = price;
        s.volume        += vol;
        s.pv_sum        += price * static_cast<double>(vol);  // VWAP numerator
        s.tick_count++;
        s.open_interest  = tick.open_interest;

        // Signed volume split
        if (direction > 0) s.buy_volume  += vol;
        else if (direction < 0) s.sell_volume += vol;
    }

private:
    // ── Per-instrument in-progress bar state ──────────────────────────────────

    struct BarState {
        uint64_t bar_ts_ns   = 0;
        double   open        = 0.0;
        double   high        = 0.0;
        double   low         = 0.0;
        double   close       = 0.0;
        double   pv_sum      = 0.0;  // Σ P·v  (VWAP numerator)
        double   open_interest = 0.0;
        uint64_t volume      = 0;
        uint64_t buy_volume  = 0;
        uint64_t sell_volume = 0;
        uint32_t tick_count  = 0;
    };

    // ── Build EnhancedBar from completed BarState ─────────────────────────────

    static EnhancedBar finalize(const BarState& s, uint32_t token) noexcept {
        EnhancedBar bar;
        bar.timestamp_ns     = s.bar_ts_ns;
        bar.instrument_token = token;
        bar.open             = s.open;
        bar.high             = s.high;
        bar.low              = s.low;
        bar.close            = s.close;
        bar.volume           = s.volume;
        bar.open_interest    = s.open_interest;
        bar.buy_volume       = s.buy_volume;
        bar.sell_volume      = s.sell_volume;
        bar.tick_count       = s.tick_count;

        // VWAP_t = Σ(P·v) / Σv
        bar.vwap = (s.volume > 0)
            ? s.pv_sum / static_cast<double>(s.volume)
            : s.close;

        // OFI_bar = (V^B − V^S) / V_t  ∈ [−1, +1]
        if (s.volume > 0) {
            const int64_t imbalance = static_cast<int64_t>(s.buy_volume)
                                    - static_cast<int64_t>(s.sell_volume);
            bar.bar_ofi = static_cast<float>(imbalance)
                        / static_cast<float>(s.volume);
        } else {
            bar.bar_ofi = 0.0f;
        }

        return bar;
    }

    BarCallback callback_;
    std::unordered_map<uint32_t, BarState> active_bars_;
};

} // namespace alpha::signal::core
