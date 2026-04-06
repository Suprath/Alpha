/**
 * @file VRPCalculator.hpp
 * @brief Variance Risk Premium (VRP) + ATM strike selection.
 *
 * The VRP measures the premium that the market pays for variance insurance:
 * a positive VRP means implied variance exceeds realized variance —
 * options are expensive relative to historical volatility.
 *
 * ATM Strike Selection:
 *   K_ATM = argmin_K |K − S(t)|
 *   (strike in the chain closest to the current spot price)
 *
 * ATM Implied Volatility:
 *   IV_ATM = (iv_call + iv_put) / 2 at K_ATM
 *   (mid of call/put IV at the ATM strike; put-call parity holds at ATM)
 *
 * VRP:
 *   VRP(t,T) = IV²_ATM(t,T) − σ²_ann(t)
 *
 *   IV_ATM   = ATM implied volatility (annualized)
 *   σ²_ann   = annualized realized variance (from RealizedVolatilityCalculator)
 *
 *   VRP > 0  → implied variance exceeds realized → options are rich
 *   VRP < 0  → implied variance below realized   → options are cheap (rare)
 *
 * Normalized VRP:
 *   VRP̂(t) = VRP(t) / σ²_ann(t)
 *   (relative to realized level — comparable across vol regimes)
 */

#pragma once

#include "OptionsChain.hpp"
#include <cstdint>
#include <cmath>    // std::abs
#include <limits>

namespace alpha::signal::signals::options {

// ─── Result ──────────────────────────────────────────────────────────────────

struct VRPResult {
    uint32_t instrument_token;
    double   K_atm;         // selected ATM strike
    double   iv_atm;        // ATM implied vol (mid of call/put, annualized)
    double   iv_atm_sq;     // IV²_ATM — implied variance
    double   sigma_ann_sq;  // σ²_ann — annualized realized variance (input)
    double   vrp;           // VRP = IV²_ATM − σ²_ann
    double   vrp_normalized;// VRP̂ = VRP / σ²_ann
    bool     valid;         // false if chain empty, spot ≤ 0, or σ²_ann ≤ 0
};

// ─── VRP Calculator ───────────────────────────────────────────────────────────

/**
 * @brief Stateless VRP calculator.
 *
 * Call compute() whenever the options chain and the latest realized variance
 * are both available (typically at bar close, after RealizedVolatilityCalculator
 * has produced sigma_ann).
 *
 * @param chain         Options chain (must contain at least one valid strike)
 * @param sigma_ann_sq  Annualized realized variance σ²_ann from
 *                      RealizedVolatilityResult::sigma_ann² = (sigma_ann)²
 *                      i.e. caller squares sigma_ann before passing here
 */
class VRPCalculator {
public:
    [[nodiscard]]
    VRPResult compute(const OptionsChain& chain, double sigma_ann_sq) const noexcept {
        VRPResult result;
        result.instrument_token = chain.instrument_token;

        if (chain.strikes.empty() || chain.spot <= 0.0 || sigma_ann_sq <= 0.0) {
            result.K_atm          = 0.0;
            result.iv_atm         = 0.0;
            result.iv_atm_sq      = 0.0;
            result.sigma_ann_sq   = sigma_ann_sq;
            result.vrp            = 0.0;
            result.vrp_normalized = 0.0;
            result.valid          = false;
            return result;
        }

        // ── ATM Strike Selection: K_ATM = argmin_K |K − S| ───────────────────
        const double S = chain.spot;
        double       best_dist = std::numeric_limits<double>::infinity();
        const OptionStrike* atm_strike = nullptr;

        for (const auto& sk : chain.strikes) {
            if (sk.K <= 0.0) continue;
            const double dist = std::abs(sk.K - S);
            if (dist < best_dist) {
                best_dist  = dist;
                atm_strike = &sk;
            }
        }

        if (atm_strike == nullptr) {
            result.valid = false;
            return result;
        }

        // ── ATM IV = mid of call and put IV at K_ATM ─────────────────────────
        // Put-call parity holds at ATM, so mid is the cleanest estimate.
        // Falls back to whichever side is positive if the other is zero.
        double iv_atm;
        if (atm_strike->iv_call > 0.0 && atm_strike->iv_put > 0.0) {
            iv_atm = 0.5 * (atm_strike->iv_call + atm_strike->iv_put);
        } else if (atm_strike->iv_call > 0.0) {
            iv_atm = atm_strike->iv_call;
        } else if (atm_strike->iv_put > 0.0) {
            iv_atm = atm_strike->iv_put;
        } else {
            // No valid IV at ATM strike
            result.valid = false;
            return result;
        }

        // ── VRP = IV²_ATM − σ²_ann ───────────────────────────────────────────
        const double iv_atm_sq = iv_atm * iv_atm;
        const double vrp       = iv_atm_sq - sigma_ann_sq;

        // ── VRP̂ = VRP / σ²_ann ───────────────────────────────────────────────
        const double vrp_norm  = vrp / sigma_ann_sq;

        result.K_atm          = atm_strike->K;
        result.iv_atm         = iv_atm;
        result.iv_atm_sq      = iv_atm_sq;
        result.sigma_ann_sq   = sigma_ann_sq;
        result.vrp            = vrp;
        result.vrp_normalized = vrp_norm;
        result.valid          = true;
        return result;
    }
};

} // namespace alpha::signal::signals::options
