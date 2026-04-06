/**
 * @file BSGamma.hpp
 * @brief Black-Scholes Gamma — pure stateless functions, reused by GEX and VRP.
 *
 * d₁ = [ ln(S/K) + (r + σ²/2)·T ] / (σ·√T)
 *
 * Γ(K,t) = φ(d₁) / (S·σ·√T)
 *
 *   S = spot price
 *   K = strike price
 *   r = risk-free rate (annualized, continuously compounded)
 *   σ = implied volatility (annualized)
 *   T = time to expiry (years)
 *   φ = standard normal PDF = exp(−x²/2) / √(2π)
 *
 * All functions return 0.0 on degenerate inputs (S≤0, K≤0, σ≤0, T≤0).
 */

#pragma once

#include <cmath>   // std::log, std::sqrt, std::exp
#include <cstdint>

namespace alpha::signal::signals::options {

// ─── Constants ───────────────────────────────────────────────────────────────

/// 1 / √(2π) — precomputed for the standard normal PDF
static constexpr double INV_SQRT_2PI = 0.3989422804014326779;

// ─── Standard normal PDF ──────────────────────────────────────────────────────

[[nodiscard]] inline double phi(double x) noexcept {
    return INV_SQRT_2PI * std::exp(-0.5 * x * x);
}

// ─── d₁ ───────────────────────────────────────────────────────────────────────

/**
 * @brief Compute d₁ from the Black-Scholes formula.
 *   d₁ = [ ln(S/K) + (r + σ²/2)·T ] / (σ·√T)
 *
 * Caller must ensure S > 0, K > 0, sigma > 0, T > 0.
 */
[[nodiscard]] inline double bs_d1(double S, double K,
                                  double r, double sigma, double T) noexcept {
    const double sqrt_T = std::sqrt(T);
    return (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * sqrt_T);
}

// ─── Black-Scholes Gamma ──────────────────────────────────────────────────────

/**
 * @brief Compute Black-Scholes Gamma for a single strike.
 *   Γ = φ(d₁) / (S·σ·√T)
 *
 * Gamma is identical for calls and puts at the same (S, K, r, σ, T).
 * Returns 0.0 if any input is non-positive (degenerate option).
 *
 * @param S     Spot price
 * @param K     Strike price
 * @param r     Risk-free rate (annualized)
 * @param sigma Implied volatility (annualized)
 * @param T     Time to expiry (years, e.g. 30/365.0)
 */
[[nodiscard]] inline double bs_gamma(double S, double K,
                                     double r, double sigma, double T) noexcept {
    if (__builtin_expect(S <= 0.0 || K <= 0.0 || sigma <= 0.0 || T <= 0.0, 0)) {
        return 0.0;
    }
    const double sqrt_T = std::sqrt(T);
    const double d1     = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T)
                        / (sigma * sqrt_T);
    return phi(d1) / (S * sigma * sqrt_T);
}

} // namespace alpha::signal::signals::options
