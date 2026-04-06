/**
 * @file RealizedVolatility.hpp
 * @brief Bar-level Realized Volatility — rolling sample standard deviation of log returns.
 *
 * Formula (sample variance, N-1 denominator):
 *   σ²_r(t) = (1 / N-1) · Σ_{i=t-N+1}^{t} (r(i) − r̄)²
 *   σ_r(t)  = √σ²_r(t)
 *
 *   r(i) = ln(C_i / C_{i-1})     (log return, from LogReturnCalculator)
 *   r̄    = (1/N) · Σ r(i)        (rolling mean over the window)
 *   N    = 20 bars                (standard rolling window)
 *
 * Implementation uses the one-pass computational formula to update in O(1):
 *   σ² = ( Σr² − (Σr)² / n ) / (n − 1)
 *
 *   where Σr and Σr² are rolling sums maintained via a circular eviction buffer.
 *   For log returns (|r| ≪ 1), numerical cancellation in this formula is negligible.
 *
 * Frequency: once per bar close (1 minute) — unordered_map overhead is irrelevant.
 */

#pragma once

#include <core/EnhancedBar.hpp>
#include <cstdint>
#include <cmath>          // std::sqrt
#include <unordered_map>

namespace alpha::signal::signals::bar {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr uint32_t RV_WINDOW = 20u; // rolling bar count

/// Annualization factor for NSE 1-minute bars:
///   √(252 trading days × 375 bars/day) = √94500 ≈ 307.409
static constexpr double RV_ANNUALIZATION = 307.4085110396682; // sqrt(252.0 * 375.0)

// ─── Result ──────────────────────────────────────────────────────────────────

struct RealizedVolatilityResult {
    uint32_t instrument_token;
    double   variance;   // σ²_r(t) — sample variance       (per-bar units²)
    double   volatility; // σ_r(t)  = √variance              (per-bar units)
    double   sigma_ann;  // σ_ann(t) = σ_r · √(252 × 375)   (annualized)
    uint32_t n;          // returns in window (1 → RV_WINDOW; grows during warm-up)
    bool     valid;      // false until at least 2 returns are in the window
};

// ─── Per-instrument rolling state ────────────────────────────────────────────

struct RVState {
    double   window[RV_WINDOW] = {}; // circular buffer of the last N log returns
    double   sum_r             = 0.0; // Σ r(i)
    double   sum_r2            = 0.0; // Σ r(i)²
    uint32_t head              = 0;   // next write index (mod RV_WINDOW)
    uint32_t count             = 0;   // total returns ever received
};

// ─── Realized Volatility Calculator ──────────────────────────────────────────

/**
 * @brief Stateful per-instrument realized volatility engine.
 *
 * update() takes the log return r(t) already computed by LogReturnCalculator.
 * Call once per bar close, immediately after LogReturnCalculator::update().
 */
class RealizedVolatilityCalculator {
public:
    /**
     * @brief Ingest one log return and return the updated volatility estimate.
     *
     * @param token  Instrument token
     * @param r      Log return r(t) = ln(C_t / C_{t-1}) from LogReturnCalculator
     */
    [[nodiscard]]
    RealizedVolatilityResult update(uint32_t token, double r) {
        RVState& s = state_[token];

        // ── Evict oldest return from rolling sums ─────────────────────────────
        const uint32_t slot = s.head % RV_WINDOW;

        if (s.count >= RV_WINDOW) {
            // Window full — subtract the value about to be overwritten
            const double old = s.window[slot];
            s.sum_r  -= old;
            s.sum_r2 -= old * old;
        }

        // ── Add new return ────────────────────────────────────────────────────
        s.window[slot]  = r;
        s.sum_r        += r;
        s.sum_r2       += r * r;
        s.head          = slot + 1u;
        s.count++;

        // ── Result ────────────────────────────────────────────────────────────
        RealizedVolatilityResult result;
        result.instrument_token = token;
        result.n     = (s.count < RV_WINDOW) ? s.count : RV_WINDOW;
        result.valid = (result.n >= 2u);

        if (!result.valid) {
            result.variance   = 0.0;
            result.volatility = 0.0;
            result.sigma_ann  = 0.0;
            return result;
        }

        // ── σ² = ( Σr² − (Σr)² / n ) / (n − 1) ──────────────────────────────
        const double n    = static_cast<double>(result.n);
        const double raw  = s.sum_r2 - (s.sum_r * s.sum_r) / n;

        // Guard against floating-point underflow yielding tiny negatives
        const double var  = (raw > 0.0) ? raw / (n - 1.0) : 0.0;

        result.variance   = var;
        result.volatility = std::sqrt(var);
        result.sigma_ann  = result.volatility * RV_ANNUALIZATION;
        return result;
    }

    void reset() noexcept { state_.clear(); }

private:
    std::unordered_map<uint32_t, RVState> state_;
};

} // namespace alpha::signal::signals::bar
