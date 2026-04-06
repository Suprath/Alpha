/**
 * @file EnhancedBar.hpp
 * @brief Enriched 1-minute bar carrying all bar-level analytics.
 *
 * Produced by BarAccumulator on every bar close and consumed by BarPipeline.
 *
 * Fields:
 *   O_t    = P(first tick in bar)
 *   H_t    = max P(τ)  ∀τ ∈ bar
 *   L_t    = min P(τ)  ∀τ ∈ bar
 *   C_t    = P(last tick in bar)
 *   V_t    = Σ v(τ)
 *   VWAP_t = Σ P(τ)·v(τ) / Σ v(τ)
 *   V^B_t  = Σ v(τ) · 1[d(τ) = +1]   (buyer-initiated)
 *   V^S_t  = Σ v(τ) · 1[d(τ) = −1]   (seller-initiated)
 *   N_t    = Σ 1                       (tick count)
 *   OFI_bar = (V^B_t − V^S_t) / V_t  ∈ [−1, +1]  (computed at bar close)
 */

#pragma once

#include <cstdint>

namespace alpha::signal::core {

struct EnhancedBar {
    // ── Timing ───────────────────────────────────────────────────────────────
    uint64_t timestamp_ns;      // 1-minute bar open time (IST-aligned, UTC epoch ns)
    uint32_t instrument_token;  // NSE instrument token

    // ── OHLCV ─────────────────────────────────────────────────────────────────
    double   open;
    double   high;
    double   low;
    double   close;
    uint64_t volume;            // V_t = Σ v(τ)
    double   open_interest;

    // ── VWAP ──────────────────────────────────────────────────────────────────
    double   vwap;              // VWAP_t = Σ(P·v) / Σv

    // ── Signed volume ─────────────────────────────────────────────────────────
    uint64_t buy_volume;        // V^B_t = Σ v(τ) · 1[d(τ)=+1]
    uint64_t sell_volume;       // V^S_t = Σ v(τ) · 1[d(τ)=−1]

    // ── Counts ────────────────────────────────────────────────────────────────
    uint32_t tick_count;        // N_t

    // ── Bar-level OFI (computed at close) ────────────────────────────────────
    float    bar_ofi;           // (V^B − V^S) / V_t  ∈ [−1, +1]
};

} // namespace alpha::signal::core
