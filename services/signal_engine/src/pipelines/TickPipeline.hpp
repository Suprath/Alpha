#pragma once

#include <alpha/models/MarketModels.hpp>
#include <signals/ofi/OFICalculator.hpp>
#include <signals/trade_direction/TradeDirection.hpp>
#include <signals/vpin/VPINCalculator.hpp>
#include <signals/kyles_lambda/KylesLambda.hpp>
#include <signals/entropy/EntropyCalculator.hpp>
#include <core/BarAccumulator.hpp>
#include <iostream>

namespace alpha::signal::pipelines {

/**
 * @brief Microsecond-level tick processing pipeline.
 *
 * Signals computed (in order) on every tick:
 *   1. Trade Direction  — d(t): +1 buy / -1 sell / 0 indeterminate
 *   2. OFI             — ΔQ_bid - ΔQ_ask, normalized ∈ [-1, +1]
 *   3. VPIN            — rolling order-toxicity proxy ∈ [0, 1]
 *   4. Kyle's Lambda   — rolling OLS price-impact estimator
 *   5. Entropy         — L2 depth distribution entropy ∈ [0, ~0.7]
 *   6. Bar Accumulator — tick → 1-minute EnhancedBar (OHLCV + VWAP + signed vol + OFI_bar)
 *
 * Bar close fires the callback set via set_bar_callback().
 */
class TickPipeline {
public:
    static constexpr int64_t OFI_SIGNAL_THRESHOLD = 50'000;
    static constexpr float   VPIN_ALERT_THRESHOLD = 0.70f;

    /**
     * @brief Register the callback invoked on every 1-minute bar close.
     *        Called from main() before the spin loop starts.
     */
    void set_bar_callback(alpha::signal::core::BarAccumulator::BarCallback cb) {
        bar_acc_.set_callback(std::move(cb));
    }

    void on_tick(const alpha::models::Tick& tick) {
        // ── 1. Trade Direction ────────────────────────────────────────────────
        // Must run first — feeds VPIN, BarAccumulator
        const auto td = td_.update(tick);

        // ── 2. OFI ────────────────────────────────────────────────────────────
        const auto ofi = ofi_.update(tick);

        // ── 3. VPIN ───────────────────────────────────────────────────────────
        const auto vpin = vpin_.update(tick, td.direction);

        // ── 4. Kyle's Lambda ──────────────────────────────────────────────────
        const auto kyle = kyle_.update(tick, ofi.ofi);

        // ── 5. Entropy ────────────────────────────────────────────────────────
        const auto entr = entropy_.compute(tick);

        // ── 6. Bar Accumulation ───────────────────────────────────────────────
        // Feeds d(t) so the bar tracks buy/sell volume split and OFI_bar.
        // Bar close callback fires here when a new minute starts.
        bar_acc_.process(tick, td.direction);

#ifndef NDEBUG
        if (ofi.valid && ofi.ofi != 0) {
            std::cout << "[OFI]  token=" << ofi.instrument_token
                      << "  ofi="        << ofi.ofi
                      << "  norm="       << ofi.ofi_normalized
                      << "  cum="        << ofi.cumulative_ofi
                      << '\n';
        }
        if (td.valid && td.direction != 0) {
            std::cout << "[TD]   token=" << td.instrument_token
                      << "  d="          << static_cast<int>(td.direction)
                      << "  cum="        << td.cumulative_direction
                      << '\n';
        }
        if (vpin.bucket_closed) {
            std::cout << "[VPIN] token="  << vpin.instrument_token
                      << "  vpin="        << vpin.vpin
                      << "  buckets="     << vpin.buckets_completed
                      << (vpin.vpin >= VPIN_ALERT_THRESHOLD ? "  *** HIGH TOXICITY ***" : "")
                      << '\n';
        }
        if (kyle.valid) {
            std::cout << "[KYLE] token="  << kyle.instrument_token
                      << "  lambda="      << kyle.lambda
                      << "  n="           << kyle.ticks_in_window
                      << '\n';
        }
        if (entr.valid) {
            std::cout << "[ENTR] token="  << entr.instrument_token
                      << "  H="           << entr.H
                      << "  H_norm="      << entr.H_normalized
                      << '\n';
        }
#endif

        // ── Signal generation ─────────────────────────────────────────────────
        // TODO: inject ShmWriter and publish alpha::models::Signal on threshold cross.
    }

    void set_instrument_adv(uint32_t token, uint64_t adv) noexcept {
        vpin_.set_adv(token, adv);
    }

    void reset_session() noexcept {
        ofi_.reset_cumulative();
        td_.reset_cumulative();
        vpin_.reset();
        kyle_.reset();
    }

private:
    alpha::signal::signals::ofi::OFICalculator                         ofi_;
    alpha::signal::signals::trade_direction::TradeDirectionCalculator  td_;
    alpha::signal::signals::vpin::VPINCalculator                       vpin_;
    alpha::signal::signals::kyles_lambda::KylesLambdaCalculator        kyle_;
    alpha::signal::signals::entropy::EntropyCalculator                 entropy_;
    alpha::signal::core::BarAccumulator                                bar_acc_;
};

} // namespace alpha::signal::pipelines
