/**
 * @file KalmanFilter.hpp
 * @brief Bar-level 1D Kalman Filter — tracks the latent mean log return α(t).
 *
 * Model (random walk state, noisy observation):
 *   State:       α(t) = α(t-1) + w(t)      w ~ N(0, Q)   process noise
 *   Observation: r(t) = α(t)   + v(t)      v ~ N(0, R)   measurement noise
 *
 * Predict step:
 *   α̂(t|t-1) = α̂(t-1|t-1)                 (no drift assumption)
 *   P(t|t-1)  = P(t-1|t-1) + Q
 *
 * Update step:
 *   ν(t)      = r(t) − α̂(t|t-1)            innovation (surprise in return)
 *   K(t)      = P(t|t-1) / (P(t|t-1) + R)  Kalman gain ∈ (0, 1)
 *   α̂(t|t)   = α̂(t|t-1) + K(t) · ν(t)    posterior state
 *   P(t|t)    = (1 − K(t)) · P(t|t-1)      posterior covariance
 *
 * Interpretation:
 *   - α̂(t|t) is the filtered estimate of the "true" mean return this bar.
 *   - High K → filter trusts the new observation more (low P, high R is unusual;
 *     high P after many Q additions means more trust in observations).
 *   - ν(t) is the model's surprise — useful as a signal on its own.
 *   - P converges to a steady-state value: P* = (−Q + √(Q²+4QR)) / 2.
 *
 * Default noise parameters (tunable per instrument):
 *   Q = 1e-5   (process noise  — α evolves slowly between bars)
 *   R = 1e-3   (measurement noise — observed r is noisy)
 *   P_init = 1.0  (start with high uncertainty, converges quickly)
 *
 * Frequency: once per bar close — unordered_map overhead is irrelevant.
 */

#pragma once

#include <core/EnhancedBar.hpp>
#include <cstdint>
#include <unordered_map>

namespace alpha::signal::signals::bar {

// ─── Noise defaults ───────────────────────────────────────────────────────────

static constexpr double KF_DEFAULT_Q      = 1e-5; // process noise variance
static constexpr double KF_DEFAULT_R      = 1e-3; // measurement noise variance
static constexpr double KF_DEFAULT_P_INIT = 1.0;  // initial state covariance

// ─── Result ──────────────────────────────────────────────────────────────────

struct KalmanResult {
    uint32_t instrument_token;
    double   alpha_predict; // α̂(t|t-1) — prior state estimate
    double   alpha_hat;     // α̂(t|t)   — posterior (filtered) state estimate
    double   P_predict;     // P(t|t-1)  — prior covariance
    double   P;             // P(t|t)    — posterior covariance
    double   K;             // K(t)      — Kalman gain ∈ (0, 1)
    double   innovation;    // ν(t)      — r(t) − α̂(t|t-1)
    bool     valid;         // false on the very first bar (no prior state)
};

// ─── Per-instrument state ─────────────────────────────────────────────────────

struct KFState {
    double   alpha_hat = 0.0;           // α̂(t-1|t-1)
    double   P         = KF_DEFAULT_P_INIT; // P(t-1|t-1)
    double   Q         = KF_DEFAULT_Q;
    double   R         = KF_DEFAULT_R;
    bool     initialized = false;
};

// ─── Kalman Filter Calculator ─────────────────────────────────────────────────

/**
 * @brief Stateful per-instrument 1D Kalman Filter.
 *
 * Call update() once per bar, passing the log return r(t) from LogReturnCalculator.
 * Optionally call set_noise() before the session to tune Q and R per instrument.
 */
class KalmanFilterCalculator {
public:
    /**
     * @brief Override Q and R noise parameters for a specific instrument.
     *        Call before the session starts.
     */
    void set_noise(uint32_t token, double Q, double R) {
        auto& s = state_[token];
        s.Q = Q;
        s.R = R;
    }

    /**
     * @brief Run one Kalman predict + update cycle.
     *
     * @param token  Instrument token
     * @param r      Log return r(t) = ln(C_t / C_{t-1}) from LogReturnCalculator
     */
    [[nodiscard]]
    KalmanResult update(uint32_t token, double r) {
        KFState& s = state_[token];

        KalmanResult result;
        result.instrument_token = token;

        if (!s.initialized) {
            // First bar — seed the filter with the observed return as initial state
            s.alpha_hat   = r;
            s.P           = KF_DEFAULT_P_INIT;
            s.initialized = true;

            result.alpha_predict = r;
            result.alpha_hat     = r;
            result.P_predict     = s.P;
            result.P             = s.P;
            result.K             = 0.0;
            result.innovation    = 0.0;
            result.valid         = false;
            return result;
        }

        // ── Predict ───────────────────────────────────────────────────────────
        const double alpha_predict = s.alpha_hat;          // α̂(t|t-1) = α̂(t-1|t-1)
        const double P_predict     = s.P + s.Q;            // P(t|t-1)  = P(t-1|t-1) + Q

        // ── Innovation ────────────────────────────────────────────────────────
        const double innovation = r - alpha_predict;       // ν(t) = r(t) − α̂(t|t-1)

        // ── Kalman Gain ───────────────────────────────────────────────────────
        const double K = P_predict / (P_predict + s.R);    // K(t) ∈ (0, 1)

        // ── Update ────────────────────────────────────────────────────────────
        const double alpha_hat = alpha_predict + K * innovation; // α̂(t|t)
        const double P         = (1.0 - K) * P_predict;         // P(t|t)

        // ── Advance state ─────────────────────────────────────────────────────
        s.alpha_hat = alpha_hat;
        s.P         = P;

        result.alpha_predict = alpha_predict;
        result.alpha_hat     = alpha_hat;
        result.P_predict     = P_predict;
        result.P             = P;
        result.K             = K;
        result.innovation    = innovation;
        result.valid         = true;
        return result;
    }

    void reset() noexcept {
        for (auto& [token, s] : state_) {
            s.alpha_hat   = 0.0;
            s.P           = KF_DEFAULT_P_INIT;
            s.initialized = false;
            // Q and R are preserved across session resets (user-configured)
        }
    }

private:
    std::unordered_map<uint32_t, KFState> state_;
};

} // namespace alpha::signal::signals::bar
