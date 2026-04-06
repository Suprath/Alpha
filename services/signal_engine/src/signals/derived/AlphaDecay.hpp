/**
 * @file AlphaDecay.hpp
 * @brief Alpha decay model — forward-projects the Kalman filtered mean return.
 *
 * Alpha Decay:
 *   α̂(t + Δt) = α̂(t) · e^{-λ̂(t) · Δt}
 *
 *   Δt  = time step in years (for 1-min bars: 1 / (252 × 375))
 *   λ̂   = estimated decay rate (per year)
 *
 * Decay Rate Estimate:
 *   λ̂(t) = λ_0 + λ_1·N(t) + λ_2·V(t) + λ_3·σ²(t)
 *
 *   N(t)  = analyst coverage (external, integer count)
 *   V(t)  = bar volume (scaled ÷ 1e6 to keep coefficient units consistent)
 *   σ²(t) = realized variance (per-bar, from RealizedVolatilityCalculator)
 *
 * Default coefficients (λ_0..λ_3) — tune from historical data:
 *   λ_0 = 0.10   (base decay ~10 years half-life at zero coverage/volume)
 *   λ_1 = 0.01   (coverage increases decay — more eyes → faster arb)
 *   λ_2 = 0.00   (volume coefficient — zero by default, calibrate if needed)
 *   λ_3 = 5.00   (variance increases decay — high vol erodes alpha faster)
 *
 * For 1-min bars, Δt ≈ 1.058e-5 years, so e^{-λΔt} ≈ 1 - λΔt for typical λ.
 */

#pragma once

#include <cstdint>
#include <cmath>         // std::exp
#include <unordered_map>

namespace alpha::signal::signals::derived {

// ─── Constants ───────────────────────────────────────────────────────────────

/// Δt for 1-minute bars (years): 1 / (252 trading days × 375 bars/day)
static constexpr double ALPHA_DECAY_DT = 1.0 / (252.0 * 375.0); // ≈ 1.058e-5

// ─── Decay coefficients ───────────────────────────────────────────────────────

struct DecayCoefficients {
    double lambda_0 = 0.10; // base rate
    double lambda_1 = 0.01; // analyst coverage coefficient
    double lambda_2 = 0.00; // volume coefficient (V in millions)
    double lambda_3 = 5.00; // variance coefficient
};

// ─── Result ──────────────────────────────────────────────────────────────────

struct AlphaDecayResult {
    uint32_t instrument_token;
    double   lambda_hat;     // λ̂(t) — estimated decay rate (per year)
    double   alpha_decayed;  // α̂(t + Δt) = α̂(t) · e^{-λ̂ · Δt}
    double   half_life_bars; // ln(2) / (λ̂ · Δt) — bars until alpha halves
};

// ─── Alpha Decay Calculator ───────────────────────────────────────────────────

/**
 * @brief Stateless alpha decay calculator (no per-instrument memory needed).
 *
 * Call project() at bar close with the current Kalman alpha and market state.
 * Optionally call set_coefficients() to tune per instrument from backtesting.
 */
class AlphaDecayCalculator {
public:
    /**
     * @brief Override decay coefficients for a specific instrument.
     */
    void set_coefficients(uint32_t token, const DecayCoefficients& coeff) {
        coeff_[token] = coeff;
    }

    /**
     * @brief Project alpha forward by one bar (Δt = 1/(252×375) years).
     *
     * @param token     Instrument token
     * @param alpha_hat Current Kalman posterior α̂(t|t)
     * @param N         Analyst coverage count (integer)
     * @param volume    Bar volume V_t (raw shares/lots — scaled internally)
     * @param variance  Realized per-bar variance σ²_r(t)
     * @param dt        Time step in years (default = 1-min bar)
     */
    [[nodiscard]]
    AlphaDecayResult project(uint32_t token,
                             double   alpha_hat,
                             uint32_t N,
                             uint64_t volume,
                             double   variance,
                             double   dt = ALPHA_DECAY_DT) const {
        const auto it   = coeff_.find(token);
        const DecayCoefficients& c = (it != coeff_.end()) ? it->second : default_;

        // ── λ̂(t) = λ_0 + λ_1·N + λ_2·V + λ_3·σ² ───────────────────────
        const double V_scaled = static_cast<double>(volume) * 1e-6; // millions
        const double lambda   = c.lambda_0
                              + c.lambda_1 * static_cast<double>(N)
                              + c.lambda_2 * V_scaled
                              + c.lambda_3 * variance;

        // ── α̂(t + Δt) = α̂(t) · e^{-λ̂ · Δt} ────────────────────────────
        const double decay_factor  = std::exp(-lambda * dt);
        const double alpha_decayed = alpha_hat * decay_factor;

        // ── Half-life in bars: ln(2) / (λ̂ · Δt) ─────────────────────────
        const double half_life = (lambda * dt > 0.0)
            ? 0.6931471805599453 / (lambda * dt)   // ln(2)
            : std::numeric_limits<double>::infinity();

        return AlphaDecayResult{token, lambda, alpha_decayed, half_life};
    }

private:
    std::unordered_map<uint32_t, DecayCoefficients> coeff_;
    static constexpr DecayCoefficients              default_{};
};

} // namespace alpha::signal::signals::derived
