/**
 * @file GEXCalculator.hpp
 * @brief Gamma Exposure (GEX) + regime classifier.
 *
 * GEX aggregates dealer gamma across the full options chain.
 * Positive GEX → dealers are long gamma → they sell rallies and buy dips
 *               → mean-reversion regime (market is self-stabilising).
 * Negative GEX → dealers are short gamma → they chase moves
 *               → momentum / vol-expansion regime.
 *
 * Formula:
 *   GEX(t) = [ Σ_K OI(K,C,t)·Γ(K,C,t) − Σ_K OI(K,P,t)·Γ(K,P,t) ] · L
 *
 *   Γ(K,C,t) = bs_gamma(S, K, r, iv_call, T)
 *   Γ(K,P,t) = bs_gamma(S, K, r, iv_put,  T)   (same formula for calls & puts)
 *   L         = lot size
 *
 * GEX Regime:
 *   GEX > θ⁺  →  mean_reversion
 *   GEX < θ⁻  →  momentum
 *   otherwise  →  neutral
 *
 * θ⁺, θ⁻ are calibrated per instrument (set via set_thresholds()).
 * Default thresholds are zero-symmetric: θ⁺ = +threshold, θ⁻ = −threshold.
 */

#pragma once

#include "BSGamma.hpp"
#include "OptionsChain.hpp"
#include <cstdint>
#include <unordered_map>

namespace alpha::signal::signals::options {

// ─── Regime enum ─────────────────────────────────────────────────────────────

enum class GEXRegime : int8_t {
    MeanReversion = +1,
    Neutral       =  0,
    Momentum      = -1
};

// ─── Result ──────────────────────────────────────────────────────────────────

struct GEXResult {
    uint32_t  instrument_token;
    double    gex;             // raw GEX value (in gamma·lot units)
    double    gex_call;        // call-side contribution  Σ OI_call·Γ_call·L
    double    gex_put;         // put-side contribution   Σ OI_put·Γ_put·L
    GEXRegime regime;          // inferred market regime
    double    theta_pos;       // θ⁺ used for this computation
    double    theta_neg;       // θ⁻ used for this computation
    bool      valid;           // false if chain is empty or spot ≤ 0
};

// ─── Per-instrument config ────────────────────────────────────────────────────

struct GEXConfig {
    double theta_pos = +1e6; // default positive threshold
    double theta_neg = -1e6; // default negative threshold
};

// ─── GEX Calculator ──────────────────────────────────────────────────────────

/**
 * @brief Stateless GEX engine (config stored per instrument, computation is pure).
 *
 * Call compute() whenever the options chain is refreshed (typically each bar
 * or on a dedicated options-chain update event, not on every tick).
 */
class GEXCalculator {
public:
    /**
     * @brief Override the regime thresholds for a specific instrument.
     *        θ⁺ and θ⁻ should be calibrated from historical GEX distribution.
     */
    void set_thresholds(uint32_t token, double theta_pos, double theta_neg) {
        auto& cfg      = config_[token];
        cfg.theta_pos  = theta_pos;
        cfg.theta_neg  = theta_neg;
    }

    /**
     * @brief Compute GEX and regime from a full options chain snapshot.
     *
     * @param chain  OptionsChain for one instrument + expiry
     */
    [[nodiscard]]
    GEXResult compute(const OptionsChain& chain) const {
        GEXResult result;
        result.instrument_token = chain.instrument_token;

        if (chain.strikes.empty() || chain.spot <= 0.0 || chain.T <= 0.0) {
            result.gex      = 0.0;
            result.gex_call = 0.0;
            result.gex_put  = 0.0;
            result.regime   = GEXRegime::Neutral;
            result.theta_pos = 0.0;
            result.theta_neg = 0.0;
            result.valid    = false;
            return result;
        }

        // ── Fetch per-instrument thresholds (or defaults) ─────────────────────
        const auto it = config_.find(chain.instrument_token);
        const GEXConfig& cfg = (it != config_.end()) ? it->second : default_cfg_;

        const double S = chain.spot;
        const double T = chain.T;
        const double r = chain.r;
        const double L = static_cast<double>(chain.lot_size);

        double sum_call = 0.0; // Σ_K OI_call · Γ_call
        double sum_put  = 0.0; // Σ_K OI_put  · Γ_put

        // ── Aggregate across all strikes ──────────────────────────────────────
        for (const auto& sk : chain.strikes) {
            if (sk.K <= 0.0) continue;

            const double gamma_call = bs_gamma(S, sk.K, r, sk.iv_call, T);
            const double gamma_put  = bs_gamma(S, sk.K, r, sk.iv_put,  T);

            sum_call += static_cast<double>(sk.oi_call) * gamma_call;
            sum_put  += static_cast<double>(sk.oi_put)  * gamma_put;
        }

        // ── GEX = (Σ call − Σ put) · L ───────────────────────────────────────
        result.gex_call = sum_call * L;
        result.gex_put  = sum_put  * L;
        result.gex      = result.gex_call - result.gex_put;

        // ── Regime classification ─────────────────────────────────────────────
        if (result.gex > cfg.theta_pos) {
            result.regime = GEXRegime::MeanReversion;
        } else if (result.gex < cfg.theta_neg) {
            result.regime = GEXRegime::Momentum;
        } else {
            result.regime = GEXRegime::Neutral;
        }

        result.theta_pos = cfg.theta_pos;
        result.theta_neg = cfg.theta_neg;
        result.valid     = true;
        return result;
    }

private:
    std::unordered_map<uint32_t, GEXConfig> config_;
    static constexpr GEXConfig              default_cfg_{};
};

} // namespace alpha::signal::signals::options
