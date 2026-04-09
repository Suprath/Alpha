/**
 * @file BarPipeline.hpp
 * @brief Bar-level signal processing pipeline — executes once per 1-minute bar close.
 *
 * Processing stages (in order):
 *   1. Log Return          — r(t) = ln(C_t / C_{t-1})
 *   2. Realized Volatility — σ_r over 20-bar rolling window; annualized σ_ann
 *   3. Adaptive Kalman     — tracks latent mean return α̂(t|t); noise tuned by σ²_r
 *   4. CUSUM               — regime change detector using α̂ and σ²_r
 *   5. SignalBundle        — assemble tick + bar signals into 8-element array
 *   6. SignalNormalizer    — rolling 252-bar z-score for each signal
 *   7. CompositeScore      — IC-weighted score + Kelly position sizing
 *   8. AlphaDecay          — project α̂ forward: α̂(t+Δt) = α̂(t)·e^{-λΔt}
 *   9. PnLTracker          — mark-to-market; open/close positions on Kelly trigger
 *  10. SHM publish         — write CompositeScore signal to shared memory ring buffer
 */

#pragma once

#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>
#include <core/EnhancedBar.hpp>
#include <core/TickSignalSnapshot.hpp>
#include <signals/bar/LogReturn.hpp>
#include <signals/bar/RealizedVolatility.hpp>
#include <signals/bar/KalmanFilter.hpp>
#include <signals/bar/CUSUM.hpp>
#include <signals/derived/SignalBundle.hpp>
#include <signals/derived/SignalNormalizer.hpp>
#include <signals/derived/CompositeScore.hpp>
#include <signals/derived/AlphaDecay.hpp>
#include <signals/derived/PnLTracker.hpp>
#include <shm_writer/ShmWriter.hpp>
#include <cstring>
#include <cmath>
#include <iostream>

// Namespace aliases — keeps on_bar() readable without long qualified names
namespace bar_sig  = alpha::signal::signals::bar;
namespace bar_core = alpha::signal::core;
namespace derived  = alpha::signal::signals::derived;

namespace alpha::signal::pipelines {

/// Minimum Kelly fraction to open/maintain a position (5% of capital)
static constexpr double MIN_KELLY_TO_TRADE = 0.05;
/// Fixed position size (shares/lots) — replace with account-fraction sizing later
static constexpr uint64_t DEFAULT_POSITION_SIZE = 100u;

class BarPipeline {
public:
    void set_shm_writer(alpha::signal::ShmWriter* writer) noexcept {
        shm_writer_ = writer;
    }

    /**
     * @brief Process one bar close.
     *
     * @param bar   Enriched 1-minute bar from BarAccumulator.
     * @param snap  Latest tick-level signal values captured from TickPipeline
     *              at the moment of bar close (populated by main.cpp callback).
     */
    void on_bar(const bar_core::EnhancedBar& bar,
                const bar_core::TickSignalSnapshot& snap) {

        // ── 1. Log Return ─────────────────────────────────────────────────────
        const auto lr = log_return_.update(bar);

        // ── 2. Realized Volatility ────────────────────────────────────────────
        const auto rv = lr.valid
            ? rv_.update(bar.instrument_token, lr.r)
            : bar_sig::RealizedVolatilityResult{bar.instrument_token, 0.0, 0.0, 0.0, 0u, false};

        // ── 3. Adaptive Kalman Filter ─────────────────────────────────────────
        if (rv.valid) {
            kf_.set_noise(bar.instrument_token,
                          rv.variance * 0.01,  // Q = σ²_r / 100
                          rv.variance);         // R = σ²_r
        }
        const auto kf = lr.valid
            ? kf_.update(bar.instrument_token, lr.r)
            : bar_sig::KalmanResult{bar.instrument_token, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};

        // ── 4. CUSUM Regime Detector ──────────────────────────────────────────
        const auto cusum = (kf.valid && rv.valid)
            ? cusum_.update(bar.instrument_token, lr.r, kf.alpha_hat, rv.variance)
            : bar_sig::CUSUMResult{bar.instrument_token, 0.0, 0.0, 0.0, false, false, false, false};

        // ── 5. SignalBundle: assemble all 8 signals ───────────────────────────
        derived::SignalBundle bundle;
        bundle.instrument_token          = bar.instrument_token;
        bundle.timestamp_ns              = bar.timestamp_ns;
        bundle[derived::OFI_NORM]        = static_cast<double>(snap.ofi_normalized);
        bundle[derived::TRADE_DIR]       = static_cast<double>(snap.cum_direction);
        bundle[derived::VPIN]            = static_cast<double>(snap.vpin);
        bundle[derived::KYLE_LAMBDA]     = snap.kyle_lambda;
        bundle[derived::ENTROPY]         = static_cast<double>(snap.entropy_norm);
        bundle[derived::LOG_RETURN]      = lr.valid  ? lr.r          : 0.0;
        bundle[derived::RV_ANN]          = rv.valid  ? rv.sigma_ann  : 0.0;
        bundle[derived::BAR_OFI]         = bar.bar_ofi;

        // ── 6. Signal Normalizer: rolling 252-bar z-score ─────────────────────
        const auto norm = normalizer_.update(bundle);

        // ── 7. Composite Score: IC-weighted score + Kelly ─────────────────────
        const auto composite = composite_.compute(norm);

        // ── 8. Alpha Decay: project α̂ forward by one bar ─────────────────────
        const auto decay = alpha_decay_.project(
            bar.instrument_token,
            kf.valid ? kf.alpha_hat : 0.0,
            /*N=*/0,               // analyst coverage — not yet wired
            bar.volume,
            rv.valid ? rv.variance : 0.0
        );

        // ── 9. PnL Tracker: open/close/update positions ───────────────────────
        const int32_t action = (composite.score > 0.0) ? 1 : -1;

        if (!pnl_.has_open_position(bar.instrument_token)) {
            // Open when Kelly fraction clears the threshold
            if (composite.kelly_half >= MIN_KELLY_TO_TRADE) {
                pnl_.open_position(bar.instrument_token, bar.close,
                                   DEFAULT_POSITION_SIZE,
                                   static_cast<int8_t>(action));
            }
        } else {
            // Close if Kelly drops below threshold (signal faded)
            if (composite.kelly_half < MIN_KELLY_TO_TRADE) {
                pnl_.close_position(bar.instrument_token, bar.close);
            }
        }

        const auto pnl_result = pnl_.update(bar.instrument_token, bar.close);

#ifndef NDEBUG
        if (lr.valid) {
            std::cout << "[LR]    token=" << lr.instrument_token
                      << "  r="           << lr.r
                      << '\n';
        }
        if (rv.valid) {
            std::cout << "[RV]    token="    << rv.instrument_token
                      << "  sigma="         << rv.volatility
                      << "  sigma_ann="     << rv.sigma_ann
                      << "  var="           << rv.variance
                      << "  n="             << rv.n
                      << '\n';
        }
        if (kf.valid) {
            std::cout << "[KF]    token="    << kf.instrument_token
                      << "  alpha_hat="     << kf.alpha_hat
                      << "  innovation="    << kf.innovation
                      << "  K="            << kf.K
                      << "  P="            << kf.P
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
        if (composite.valid) {
            std::cout << "[COMP]  token="    << composite.instrument_token
                      << "  score="         << composite.score
                      << "  quality="       << composite.quality
                      << "  p_win="         << composite.p_win
                      << "  kelly="         << composite.kelly_half
                      << "  action="        << action
                      << '\n';
        }
        std::cout << "[DECAY] token="        << bar.instrument_token
                  << "  lambda_hat="         << decay.lambda_hat
                  << "  alpha_decayed="      << decay.alpha_decayed
                  << "  half_life_bars="     << decay.half_life_bars
                  << '\n';
        if (pnl_result.is_open) {
            std::cout << "[PNL]   token="    << pnl_result.instrument_token
                      << "  gross="         << pnl_result.gross_pnl
                      << "  costs="         << pnl_result.costs
                      << "  unrealized="    << pnl_result.unrealized_pnl
                      << '\n';
        }
        std::cout << "[BAR]   token="        << bar.instrument_token
                  << "  O="                  << bar.open
                  << "  H="                  << bar.high
                  << "  L="                  << bar.low
                  << "  C="                  << bar.close
                  << "  V="                  << bar.volume
                  << "  VWAP="               << bar.vwap
                  << "  V^B="                << bar.buy_volume
                  << "  V^S="                << bar.sell_volume
                  << "  N="                  << bar.tick_count
                  << "  OFI_bar="            << bar.bar_ofi
                  << '\n';
#endif

        // ── 10. Publish CompositeScore signal to shared memory ─────────────────
        // Publish whenever score is valid and Kelly clears the minimum threshold.
        if (shm_writer_ && composite.valid
                        && composite.kelly_half >= MIN_KELLY_TO_TRADE) {
            alpha::models::Signal sig{};
            sig.timestamp_ns     = alpha::time::Timestamp::now_ns();
            sig.instrument_token = bar.instrument_token;
            sig.price            = bar.close;
            sig.action           = action;
            sig.confidence       = composite.kelly_half; // Kelly fraction ∈ [0,1]
            sig.strategy_id      = 4; // CompositeScore
            std::snprintf(sig.symbol, sizeof(sig.symbol),
                          "TOKEN_%u", bar.instrument_token);
            shm_writer_->publish(sig);
        }
    }

    void reset_session() noexcept {
        log_return_.reset();
        rv_.reset();
        kf_.reset();
        cusum_.reset();
        normalizer_.reset();
        pnl_.reset();
        // composite_ and alpha_decay_ are stateless per session —
        // IC weights and decay coefficients are preserved across sessions.
    }

private:
    // ── Bar-level signal calculators ──────────────────────────────────────────
    bar_sig::LogReturnCalculator          log_return_;
    bar_sig::RealizedVolatilityCalculator rv_;
    bar_sig::KalmanFilterCalculator       kf_;
    bar_sig::CUSUMCalculator              cusum_;

    // ── Derived / top-level signal calculators ────────────────────────────────
    derived::SignalNormalizer             normalizer_;
    derived::CompositeScoreCalculator     composite_;
    derived::AlphaDecayCalculator         alpha_decay_;
    derived::PnLTracker                   pnl_;

    // ── Infrastructure ────────────────────────────────────────────────────────
    alpha::signal::ShmWriter*             shm_writer_ = nullptr;
};

} // namespace alpha::signal::pipelines
