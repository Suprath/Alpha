#pragma once

#include <core/EnhancedBar.hpp>
#include <signals/bar/LogReturn.hpp>
#include <signals/bar/RealizedVolatility.hpp>
#include <signals/bar/KalmanFilter.hpp>
#include <signals/bar/CUSUM.hpp>
#include <iostream>

// Namespace alias — keeps on_bar() readable without long qualified names
namespace bar_sig = alpha::signal::signals::bar;
namespace bar_core = alpha::signal::core;

namespace alpha::signal::pipelines {

/**
 * @brief Bar-level signal processing pipeline.
 *
 * Called once per minute per instrument on every EnhancedBar close.
 * Signals computed (bar frequency):
 *   1. Log Return          — r(t) = ln(C_t / C_{t-1})
 *   2. Realized Volatility — σ_r(t) over rolling 20-bar window
 */
class BarPipeline {
public:
    void on_bar(const bar_core::EnhancedBar& bar) {
        // ── 1. Log Return ─────────────────────────────────────────────────────
        const auto lr = log_return_.update(bar);

        // ── 2. Realized Volatility ────────────────────────────────────────────
        // Feeds r(t) from step 1 — skip on warm-up bar where lr is invalid
        const auto rv = lr.valid
            ? rv_.update(bar.instrument_token, lr.r)
            : bar_sig::RealizedVolatilityResult{bar.instrument_token, 0.0, 0.0, 0.0, 0u, false};

        // ── 3. Kalman Filter (adaptive Q and R from realized variance) ────────
        // Q = σ²_r / 100   (1% of return variance — process evolves slowly)
        // R = σ²_r         (full return variance  — observation is noisy)
        // Updated every bar once the RV window is full (rv.valid), making the
        // filter adaptive to the current volatility regime.
        if (rv.valid) {
            kf_.set_noise(bar.instrument_token,
                          rv.variance * 0.01,  // Q = σ²_r / 100
                          rv.variance);         // R = σ²_r
        }

        const auto kf = lr.valid
            ? kf_.update(bar.instrument_token, lr.r)
            : bar_sig::KalmanResult{bar.instrument_token, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};

        // ── 4. CUSUM Regime Detector ──────────────────────────────────────────
        // Requires KF (α̂) and RV (σ²_r) — both must be valid (warm window)
        const auto cusum = (kf.valid && rv.valid)
            ? cusum_.update(bar.instrument_token, lr.r, kf.alpha_hat, rv.variance)
            : bar_sig::CUSUMResult{bar.instrument_token, 0.0, 0.0, 0.0, false, false, false, false};

#ifndef NDEBUG
        if (lr.valid) {
            std::cout << "[LR]   token=" << lr.instrument_token
                      << "  r="          << lr.r
                      << '\n';
        }
        if (rv.valid) {
            std::cout << "[RV]   token="     << rv.instrument_token
                      << "  sigma="          << rv.volatility
                      << "  sigma_ann="      << rv.sigma_ann
                      << "  var="            << rv.variance
                      << "  n="              << rv.n
                      << '\n';
        }
        if (kf.valid) {
            std::cout << "[KF]   token="     << kf.instrument_token
                      << "  alpha_hat="      << kf.alpha_hat
                      << "  innovation="     << kf.innovation
                      << "  K="             << kf.K
                      << "  P="             << kf.P
                      << '\n';
        }
        if (cusum.valid) {
            std::cout << "[CUSUM] token="    << cusum.instrument_token
                      << "  C+="            << cusum.C_plus
                      << "  C-="            << cusum.C_minus
                      << "  h*="            << cusum.threshold
                      << (cusum.fired ? "  *** REGIME CHANGE ***" : "")
                      << '\n';
        }
        std::cout << "[BAR]  token="   << bar.instrument_token
                  << "  O="           << bar.open
                  << "  H="           << bar.high
                  << "  L="           << bar.low
                  << "  C="           << bar.close
                  << "  V="           << bar.volume
                  << "  VWAP="        << bar.vwap
                  << "  V^B="         << bar.buy_volume
                  << "  V^S="         << bar.sell_volume
                  << "  N="           << bar.tick_count
                  << "  OFI_bar="     << bar.bar_ofi
                  << '\n';
#endif
    }

    void reset_session() noexcept {
        log_return_.reset();
        rv_.reset();
        kf_.reset();
        cusum_.reset();
    }

private:
    bar_sig::LogReturnCalculator          log_return_;
    bar_sig::RealizedVolatilityCalculator rv_;
    bar_sig::KalmanFilterCalculator       kf_;
    bar_sig::CUSUMCalculator              cusum_;
};

} // namespace alpha::signal::pipelines
