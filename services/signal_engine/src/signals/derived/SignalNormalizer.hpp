/**
 * @file SignalNormalizer.hpp
 * @brief Rolling 252-bar z-score normalizer for all 8 signals.
 *
 * For each signal k and instrument:
 *   ŝ_k(t) = (s_k(t) - μ_k) / σ_k
 *
 *   μ_k = rolling 252-bar mean    of s_k
 *   σ_k = rolling 252-bar std dev of s_k  (sample, N-1 denominator)
 *
 * Uses the O(1) computational formula (same as RealizedVolatility):
 *   var = (Σs² - (Σs)²/n) / (n-1)
 *   std = √max(0, var)
 *
 * Returns valid normalized signals once the window has ≥ 2 observations.
 * Before that, returns the raw value unchanged (no normalization applied).
 *
 * State memory per instrument: 8 signals × 252 entries × 8 bytes = 16 128 bytes.
 * Stored in unordered_map — bar frequency, not in the microsecond hot path.
 */

#pragma once

#include "SignalBundle.hpp"
#include <cstdint>
#include <cmath>
#include <array>
#include <unordered_map>

namespace alpha::signal::signals::derived {

static constexpr uint32_t NORM_WINDOW = 252u; // 1 trading year of 1-min bars

// ─── Result ──────────────────────────────────────────────────────────────────

struct NormalizedBundle {
    uint32_t instrument_token;
    uint64_t timestamp_ns;
    std::array<double, NUM_SIGNALS> z;       // ŝ_k(t) — z-scored signal values
    std::array<double, NUM_SIGNALS> mu;      // rolling mean  μ_k
    std::array<double, NUM_SIGNALS> sigma;   // rolling std   σ_k
    std::array<bool,   NUM_SIGNALS> valid;   // false until ≥2 observations
    uint32_t n;                              // observations in window (up to 252)
};

// ─── Per-(instrument, signal) rolling state ───────────────────────────────────

struct NormSignalState {
    double   window[NORM_WINDOW] = {}; // circular buffer
    double   sum_s               = 0.0;
    double   sum_s2              = 0.0;
    uint32_t head                = 0;
    uint32_t count               = 0;
};

struct InstrumentNormState {
    std::array<NormSignalState, NUM_SIGNALS> signals;
};

// ─── Signal Normalizer ────────────────────────────────────────────────────────

class SignalNormalizer {
public:
    /**
     * @brief Normalize all 8 signals in the bundle using rolling statistics.
     *        Call once per bar close.
     */
    [[nodiscard]]
    NormalizedBundle update(const SignalBundle& bundle) {
        InstrumentNormState& inst = state_[bundle.instrument_token];

        NormalizedBundle out;
        out.instrument_token = bundle.instrument_token;
        out.timestamp_ns     = bundle.timestamp_ns;

        uint32_t min_count = NORM_WINDOW + 1; // track minimum window fill

        for (uint32_t k = 0; k < NUM_SIGNALS; ++k) {
            NormSignalState& ns  = inst.signals[k];
            const double     val = bundle.values[k];

            // ── Evict oldest ────────────────────────────────────────────────
            const uint32_t slot = ns.head % NORM_WINDOW;
            if (ns.count >= NORM_WINDOW) {
                const double old = ns.window[slot];
                ns.sum_s  -= old;
                ns.sum_s2 -= old * old;
            }

            // ── Add new observation ─────────────────────────────────────────
            ns.window[slot]  = val;
            ns.sum_s        += val;
            ns.sum_s2       += val * val;
            ns.head          = slot + 1u;
            ns.count++;

            const uint32_t n = (ns.count < NORM_WINDOW) ? ns.count : NORM_WINDOW;
            if (n < min_count) min_count = n;

            if (n < 2u) {
                out.z[k]     = val;      // passthrough — not enough history
                out.mu[k]    = val;
                out.sigma[k] = 1.0;
                out.valid[k] = false;
                continue;
            }

            // ── μ and σ ─────────────────────────────────────────────────────
            const double nd    = static_cast<double>(n);
            const double mu    = ns.sum_s / nd;
            const double raw   = ns.sum_s2 - (ns.sum_s * ns.sum_s) / nd;
            const double var   = (raw > 0.0) ? raw / (nd - 1.0) : 0.0;
            const double sigma = std::sqrt(var);

            out.mu[k]    = mu;
            out.sigma[k] = sigma;
            out.z[k]     = (sigma > 0.0) ? (val - mu) / sigma : 0.0;
            out.valid[k] = true;
        }

        out.n = (min_count > NORM_WINDOW) ? NORM_WINDOW : min_count;
        return out;
    }

    void reset() noexcept { state_.clear(); }

private:
    std::unordered_map<uint32_t, InstrumentNormState> state_;
};

} // namespace alpha::signal::signals::derived
