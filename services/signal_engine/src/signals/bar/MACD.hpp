#pragma once

#include <cstdint>
#include <unordered_map>
#include "../../core/EnhancedBar.hpp"

namespace alpha::signal::bar_sig {

static constexpr uint32_t MACD_FAST   = 12u;
static constexpr uint32_t MACD_SLOW   = 26u;
static constexpr uint32_t MACD_SIGNAL = 9u;

// Multipliers (pre-computed at compile time)
static constexpr double K12 = 2.0 / (MACD_FAST   + 1.0);
static constexpr double K26 = 2.0 / (MACD_SLOW   + 1.0);
static constexpr double K9  = 2.0 / (MACD_SIGNAL + 1.0);

struct MACDState {
    double   ema12 = 0.0;
    double   ema26 = 0.0;
    double   sig9  = 0.0;   // EMA(9) of macd_line
    double   sum12 = 0.0;   // Accumulator for EMA12 seed
    double   sum26 = 0.0;   // Accumulator for EMA26 seed
    double   sum_sig = 0.0; // Accumulator for signal EMA seed
    uint32_t count = 0;
    bool     ema12_live = false;
    bool     ema26_live = false;
    bool     sig_live   = false;
    uint32_t sig_count  = 0;
};

struct MACDResult {
    uint32_t instrument_token;
    double   macd_line      = 0.0;
    double   macd_signal    = 0.0;
    double   macd_histogram = 0.0;
    bool     valid = false;  // true after MACD_SLOW + MACD_SIGNAL bars (35)
};

/**
 * MACDCalculator — MACD(12, 26, 9), stateful per instrument.
 *
 * EMA12 seeds as SMA(12) at bar 12, then live EMA.
 * EMA26 seeds as SMA(26) at bar 26, then live EMA.
 * Signal seeds as SMA(9) of macd_line at bar 26+9=35, then live EMA.
 */
class MACDCalculator {
public:
    [[nodiscard]] MACDResult update(const core::EnhancedBar& bar) {
        MACDResult res;
        res.instrument_token = bar.instrument_token;

        auto& s = state_[bar.instrument_token];
        const double close = bar.close;
        ++s.count;

        // ── Phase 1: Seed EMA12 ──────────────────────────────────────────────
        if (!s.ema12_live) {
            s.sum12 += close;
            if (s.count == MACD_FAST) {
                s.ema12      = s.sum12 / MACD_FAST;
                s.ema12_live = true;
            }
        } else {
            s.ema12 = close * K12 + s.ema12 * (1.0 - K12);
        }

        // ── Phase 2: Seed EMA26 ──────────────────────────────────────────────
        // Only accumulate sum26 during seeding; stop after ema26 goes live to
        // prevent unbounded growth on long sessions (overflow risk).
        if (!s.ema26_live) {
            s.sum26 += close;
            if (s.count == MACD_SLOW) {
                s.ema26      = s.sum26 / MACD_SLOW;
                s.ema26_live = true;
            }
        } else {
            s.ema26 = close * K26 + s.ema26 * (1.0 - K26);
        }

        if (!s.ema26_live) return res;  // Not enough bars yet

        const double macd_line = s.ema12 - s.ema26;

        // ── Phase 3: Seed signal EMA(9) ──────────────────────────────────────
        if (!s.sig_live) {
            s.sum_sig += macd_line;
            ++s.sig_count;
            if (s.sig_count == MACD_SIGNAL) {
                s.sig9     = s.sum_sig / MACD_SIGNAL;
                s.sig_live = true;
            }
        } else {
            s.sig9 = macd_line * K9 + s.sig9 * (1.0 - K9);
        }

        if (!s.sig_live) return res;

        res.macd_line      = macd_line;
        res.macd_signal    = s.sig9;
        res.macd_histogram = macd_line - s.sig9;
        res.valid          = true;
        return res;
    }

    void reset() noexcept { state_.clear(); }

private:
    std::unordered_map<uint32_t, MACDState> state_;
};

} // namespace alpha::signal::bar_sig
