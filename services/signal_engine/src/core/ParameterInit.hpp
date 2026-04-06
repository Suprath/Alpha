/**
 * @file ParameterInit.hpp
 * @brief Centralized parameter initialization from 252-bar historical statistics.
 *
 * All noise parameters, thresholds, and window sizes in this engine are
 * derived from two fundamental quantities:
 *   σ²_r   — 252-bar sample variance of per-bar log returns
 *   r̄      — 252-bar mean per-bar log return
 *
 * These are loaded at session open from QuestDB (previous day's statistics)
 * and passed to ParameterInitializer to produce typed config structs for
 * every downstream calculator.
 *
 * Parameter table:
 * ┌──────────────────┬──────────────────────────────┬──────────────────────┐
 * │ Parameter        │ Formula                      │ Notes                │
 * ├──────────────────┼──────────────────────────────┼──────────────────────┤
 * │ Kalman Q         │ σ²_r / 100                   │ 1% of return var     │
 * │ Kalman R         │ σ²_r                         │ full return var      │
 * │ Kalman P_0       │ σ²_r                         │ same as R            │
 * │ Kalman α̂_0      │ r̄                            │ 252-bar mean return  │
 * │ CUSUM h*         │ ln(α̂/c) - λσ²/α̂²           │ per instrument       │
 * │ VPIN σ_v         │ ADV / 50                     │ 50 buckets standard  │
 * │ Kyle window N    │ 20 ticks                     │ rolling OLS          │
 * │ Entropy levels K │ 5                            │ depth levels fixed   │
 * │ RV window N      │ 20 bars                      │ rolling variance     │
 * │ GEX θ+           │ 95th pct of historical GEX   │ calibrated           │
 * │ GEX θ-           │ 5th  pct of historical GEX   │ calibrated           │
 * └──────────────────┴──────────────────────────────┴──────────────────────┘
 */

#pragma once

#include <cstdint>
#include <cmath>       // std::log, std::sqrt, std::abs
#include <vector>
#include <algorithm>   // std::sort, std::nth_element
#include <limits>

namespace alpha::signal::core {

// ─── Historical statistics (loaded from QuestDB at session open) ──────────────

struct HistoricalStats {
    double   variance_r;   // 252-bar sample variance of per-bar log returns
    double   mean_r;       // 252-bar mean per-bar log return r̄
    uint64_t adv;          // average daily volume (shares/lots)
};

// ─── Typed parameter structs ──────────────────────────────────────────────────

struct KalmanInitParams {
    double Q;           // process noise  = σ²_r / 100
    double R;           // measurement noise = σ²_r
    double P_init;      // initial covariance = σ²_r  (same as R)
    double alpha_init;  // initial state estimate = r̄
};

struct CUSUMInitParams {
    double c;           // transaction cost (per-instrument input)
    double lambda;      // decay rate (per-instrument input)
    // h* is computed at runtime using current α̂ and σ²_r (already in CUSUM.hpp)
};

struct VPINInitParams {
    uint64_t bucket_size; // σ_v = ADV / 50
};

struct GEXThresholds {
    double theta_pos;   // θ+ = 95th percentile of historical GEX
    double theta_neg;   // θ- =  5th percentile of historical GEX
};

// ─── Fixed structural constants (not derived, but documented centrally) ───────

struct EngineConstants {
    static constexpr uint32_t KYLE_WINDOW    = 20u;  // rolling OLS ticks
    static constexpr uint32_t RV_WINDOW      = 20u;  // rolling variance bars
    static constexpr uint32_t NORM_WINDOW    = 252u; // signal normalization bars
    static constexpr uint32_t ENTROPY_LEVELS = 5u;   // L2 depth levels
    static constexpr uint32_t VPIN_BUCKETS   = 50u;  // standard bucket count
};

// ─── Parameter Initializer ────────────────────────────────────────────────────

class ParameterInitializer {
public:
    // ── Kalman Filter ──────────────────────────────────────────────────────────

    /**
     * @brief Derive Kalman noise parameters from 252-bar return statistics.
     *   Q     = σ²_r / 100    (process evolves slowly — 1% of observation noise)
     *   R     = σ²_r          (full return variance as measurement noise)
     *   P_0   = σ²_r          (start with same uncertainty as measurement)
     *   α̂_0  = r̄             (seed filter with historical mean return)
     */
    [[nodiscard]]
    static KalmanInitParams kalman(const HistoricalStats& stats) noexcept {
        const double var = (stats.variance_r > 0.0) ? stats.variance_r : 1e-6;
        return KalmanInitParams{
            var * 0.01,    // Q
            var,           // R
            var,           // P_init
            stats.mean_r   // alpha_init
        };
    }

    // ── CUSUM ─────────────────────────────────────────────────────────────────

    /**
     * @brief Return default CUSUM hyperparameters.
     *        h* is computed at runtime in CUSUMCalculator using current α̂ and σ².
     */
    [[nodiscard]]
    static CUSUMInitParams cusum(double c_cost   = 1e-4,
                                 double lambda   = 0.5) noexcept {
        return CUSUMInitParams{c_cost, lambda};
    }

    // ── VPIN ──────────────────────────────────────────────────────────────────

    /**
     * @brief Compute VPIN bucket size from ADV.
     *   σ_v = ADV / 50   (50 buckets per day — standard)
     */
    [[nodiscard]]
    static VPINInitParams vpin(const HistoricalStats& stats) noexcept {
        const uint64_t bucket = (stats.adv > 0u)
            ? stats.adv / EngineConstants::VPIN_BUCKETS
            : 200'000u; // fallback: 200k shares
        return VPINInitParams{bucket};
    }

    // ── GEX Thresholds ────────────────────────────────────────────────────────

    /**
     * @brief Compute GEX regime thresholds from historical GEX distribution.
     *   θ+ = 95th percentile
     *   θ- =  5th percentile
     *
     * @param gex_history  Vector of historical daily GEX values (any order).
     *                     Requires ≥ 20 observations for meaningful percentiles.
     */
    [[nodiscard]]
    static GEXThresholds gex_thresholds(std::vector<double> gex_history) {
        if (gex_history.size() < 2u) {
            return GEXThresholds{+1e6, -1e6}; // fallback defaults
        }

        std::sort(gex_history.begin(), gex_history.end());
        const size_t n = gex_history.size();

        // Linear interpolation for exact percentile
        auto percentile = [&](double pct) -> double {
            const double pos = pct * static_cast<double>(n - 1u);
            const size_t lo  = static_cast<size_t>(pos);
            const size_t hi  = lo + 1u;
            const double frac = pos - static_cast<double>(lo);
            if (hi >= n) return gex_history[lo];
            return gex_history[lo] * (1.0 - frac) + gex_history[hi] * frac;
        };

        return GEXThresholds{
            percentile(0.95), // θ+
            percentile(0.05)  // θ-
        };
    }

    // ── Annualized variance from per-bar variance ─────────────────────────────

    /**
     * @brief Convert per-bar realized variance to annualized variance.
     *   σ²_ann = σ²_r × 252 × 375
     */
    [[nodiscard]]
    static double annualize_variance(double variance_r) noexcept {
        return variance_r * 252.0 * 375.0;
    }

    /**
     * @brief Convert per-bar realized variance to annualized volatility.
     *   σ_ann = √(σ²_r × 252 × 375)
     */
    [[nodiscard]]
    static double annualize_volatility(double variance_r) noexcept {
        return std::sqrt(annualize_variance(variance_r));
    }
};

} // namespace alpha::signal::core
