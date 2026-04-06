/**
 * @file CUSUM.hpp
 * @brief Bar-level CUSUM (Cumulative Sum) regime-change detector.
 *
 * Detects persistent upward or downward alpha using the Kalman-filtered mean
 * return α̂ as the drift signal and realized variance σ²_r as the noise scale.
 *
 * CUSUM Positive (detects upward alpha):
 *   C⁺(t) = max(0,  C⁺(t-1) + α̂·r(t)/σ²_r − α̂²/(2σ²_r))
 *
 * CUSUM Negative (detects downward alpha):
 *   C⁻(t) = max(0,  C⁻(t-1) − α̂·r(t)/σ²_r − α̂²/(2σ²_r))
 *
 * Optimal threshold (Page-Hinkley log-likelihood ratio):
 *   h* = ln(α̂/c) − λσ²_r/α̂²
 *   c = transaction cost, λ = decay rate
 *
 * Alarm:
 *   fired = 1[C⁺(t) ≥ h*] ∪ 1[C⁻(t) ≥ h*]
 *
 * On alarm, the fired statistic resets to 0 (classic Page-Hinkley restart).
 *
 * Inputs (all computed upstream in BarPipeline):
 *   r(t)    — log return      (LogReturnCalculator)
 *   α̂(t|t) — filtered alpha  (KalmanFilterCalculator)
 *   σ²_r    — realized var    (RealizedVolatilityCalculator)
 *
 * Default parameters (tunable per instrument via set_params()):
 *   c      = 1e-4   (1 basis point transaction cost)
 *   lambda = 0.5    (decay rate — controls threshold sensitivity)
 */

#pragma once

#include <cstdint>
#include <cmath>          // std::log, std::abs
#include <unordered_map>
#include <limits>

namespace alpha::signal::signals::bar {

// ─── Default parameters ───────────────────────────────────────────────────────

static constexpr double CUSUM_DEFAULT_C      = 1e-4; // transaction cost (1 bp)
static constexpr double CUSUM_DEFAULT_LAMBDA = 0.5;  // decay rate

// ─── Result ──────────────────────────────────────────────────────────────────

struct CUSUMResult {
    uint32_t instrument_token;
    double   C_plus;      // C⁺(t) — upward cumulative sum
    double   C_minus;     // C⁻(t) — downward cumulative sum
    double   threshold;   // h* — current alarm threshold
    bool     fired_up;    // C⁺(t) ≥ h* — upward regime detected
    bool     fired_down;  // C⁻(t) ≥ h* — downward regime detected
    bool     fired;       // fired_up || fired_down
    bool     valid;       // false until KF and RV inputs are both valid
};

// ─── Per-instrument state ─────────────────────────────────────────────────────

struct CUSUMState {
    double C_plus  = 0.0;
    double C_minus = 0.0;
    double c_cost  = CUSUM_DEFAULT_C;
    double lambda  = CUSUM_DEFAULT_LAMBDA;
};

// ─── CUSUM Calculator ─────────────────────────────────────────────────────────

/**
 * @brief Stateful per-instrument CUSUM regime detector.
 *
 * Call update() once per bar after LogReturn, RealizedVolatility, and
 * KalmanFilter have all been computed. Valid once KF and RV are both warm.
 *
 * Call set_params() before the session to override c and lambda per instrument.
 */
class CUSUMCalculator {
public:
    void set_params(uint32_t token, double c_cost, double lambda) {
        auto& s    = state_[token];
        s.c_cost   = (c_cost > 0.0) ? c_cost : CUSUM_DEFAULT_C;
        s.lambda   = lambda;
    }

    /**
     * @brief Run one CUSUM update.
     *
     * @param token     Instrument token
     * @param r         Log return r(t) from LogReturnCalculator
     * @param alpha_hat Kalman posterior α̂(t|t) from KalmanFilterCalculator
     * @param variance  Realized variance σ²_r from RealizedVolatilityCalculator
     */
    [[nodiscard]]
    CUSUMResult update(uint32_t token, double r,
                       double alpha_hat, double variance) {
        CUSUMState& s = state_[token];

        CUSUMResult result;
        result.instrument_token = token;

        // ── Guard: variance must be positive, alpha_hat non-zero ─────────────
        if (variance <= 0.0 || alpha_hat == 0.0) {
            result.C_plus    = s.C_plus;
            result.C_minus   = s.C_minus;
            result.threshold = std::numeric_limits<double>::infinity();
            result.fired_up  = false;
            result.fired_down= false;
            result.fired     = false;
            result.valid     = false;
            return result;
        }

        const double inv_var   = 1.0 / variance;
        const double alpha_sq  = alpha_hat * alpha_hat;

        // ── Shared drift penalty term: α̂² / (2σ²_r) ─────────────────────────
        const double drift_penalty = alpha_sq * inv_var * 0.5;

        // ── CUSUM increments ──────────────────────────────────────────────────
        // Signed score:  α̂·r(t) / σ²_r
        const double score = alpha_hat * r * inv_var;

        // C⁺(t) = max(0, C⁺(t-1) + score − drift_penalty)
        s.C_plus  = std::max(0.0, s.C_plus  + score - drift_penalty);

        // C⁻(t) = max(0, C⁻(t-1) − score − drift_penalty)
        s.C_minus = std::max(0.0, s.C_minus - score - drift_penalty);

        // ── Threshold: h* = ln(|α̂| / c) − λσ²_r / α̂² ────────────────────
        // Use |α̂| to keep ln argument positive regardless of sign of alpha.
        // If |α̂| < c → ln term is negative → lower threshold (fires sooner).
        const double abs_alpha = std::abs(alpha_hat);
        const double ln_term   = std::log(abs_alpha / s.c_cost);
        const double decay_term= s.lambda * variance / alpha_sq;
        const double threshold = ln_term - decay_term;

        // ── Alarm detection ───────────────────────────────────────────────────
        // threshold must be positive to be meaningful; if ≤ 0 always fires.
        const bool fired_up   = (s.C_plus  >= threshold);
        const bool fired_down = (s.C_minus >= threshold);

        // Page-Hinkley restart: reset fired statistic to 0
        if (fired_up)   s.C_plus  = 0.0;
        if (fired_down) s.C_minus = 0.0;

        result.C_plus    = s.C_plus;
        result.C_minus   = s.C_minus;
        result.threshold = threshold;
        result.fired_up  = fired_up;
        result.fired_down= fired_down;
        result.fired     = fired_up || fired_down;
        result.valid     = true;
        return result;
    }

    void reset() noexcept {
        for (auto& [token, s] : state_) {
            s.C_plus  = 0.0;
            s.C_minus = 0.0;
            // c_cost and lambda preserved (user-configured)
        }
    }

private:
    std::unordered_map<uint32_t, CUSUMState> state_;
};

} // namespace alpha::signal::signals::bar
