/**
 * @file CompositeScore.hpp
 * @brief IC-weighted composite score, signal quality, and Kelly position sizing.
 *
 * Composite Score:
 *   Score(t) = Σ_{k=0}^{NUM_SIGNALS-1} w_k · ŝ_k(t)
 *   w_k      = IC_k / Σ_j IC_j          (Information Coefficient weights)
 *   ŝ_k      = normalized signal from SignalNormalizer
 *
 * Signal Quality:
 *   Q(t) = 100 · (Σ_k 1[|ŝ_k(t)| > 1.0]) / K     ∈ [0, 100]
 *   Fraction of signals with |z| > 1 std dev — measures signal agreement.
 *
 * Kelly Fraction:
 *   p    = sigmoid(Score) = 1 / (1 + e^{-Score})   ∈ (0, 1)
 *   f*   = (p(b+1) - 1) / b                         (full Kelly)
 *   f    = f* / 2                                    (half Kelly — practical)
 *
 *   b    = win/loss ratio (default 1.0 — symmetric payoff)
 *   f is clamped to [0, 1]: negative → no position, >1 → capped at 1.
 *
 * IC weights are set once from backtesting via set_ic_weights().
 * Default: equal weights (1/8 each) until calibrated.
 */

#pragma once

#include "SignalBundle.hpp"
#include "SignalNormalizer.hpp"
#include <cmath>    // std::exp, std::abs
#include <array>

namespace alpha::signal::signals::derived {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr double QUALITY_THRESHOLD = 1.0;  // |z| > 1σ counts as "active"
static constexpr double DEFAULT_WIN_LOSS  = 1.0;  // symmetric payoff ratio b

// ─── Result ──────────────────────────────────────────────────────────────────

struct CompositeScoreResult {
    uint32_t instrument_token;
    double   score;          // Score(t) = Σ w_k · ŝ_k
    double   quality;        // Q(t) ∈ [0, 100] — fraction of active signals × 100
    double   p_win;          // p = sigmoid(Score) ∈ (0, 1)
    double   kelly_full;     // f* = (p(b+1) - 1) / b
    double   kelly_half;     // f  = f* / 2, clamped to [0, 1]
    std::array<double, SIGNAL_CAPACITY> weights; // w_k used this computation
    bool     valid;          // false until all signals are valid
};

// ─── Composite Score Calculator ───────────────────────────────────────────────

class CompositeScoreCalculator {
public:
    CompositeScoreCalculator() {
        // Default: equal IC weights (1/K each)
        ic_.fill(1.0);
        recompute_weights();
    }

    /**
     * @brief Set IC values from historical backtest results.
     *        Weights are recomputed immediately as w_k = IC_k / Σ IC_j.
     *        Negative ICs are zeroed (only positive predictive power counts).
     */
    void set_ic_weights(const std::array<double, SIGNAL_CAPACITY>& ic_values) {
        ic_ = ic_values;
        recompute_weights();
    }

    /**
     * @brief Override the win/loss payoff ratio b.
     *        Default = 1.0 (symmetric). Set higher for asymmetric strategies.
     */
    void set_win_loss_ratio(double b) {
        b_ = (b > 0.0) ? b : DEFAULT_WIN_LOSS;
    }

    /**
     * @brief Compute composite score, quality, and Kelly from normalized signals.
     *
     * @param z       NormalizedBundle from SignalNormalizer
     */
    [[nodiscard]]
    CompositeScoreResult compute(const NormalizedBundle& z) const {
        CompositeScoreResult result;
        result.instrument_token = z.instrument_token;
        result.weights          = weights_;

        // ── Composite Score: Score = Σ w_k · ŝ_k ────────────────────────────
        double score        = 0.0;
        uint32_t active     = 0;
        bool     all_valid  = true;

        for (uint32_t k = 0; k < NUM_SIGNALS; ++k) {
            score += weights_[k] * z.z[k];
            if (!z.valid[k]) all_valid = false;
            if (std::abs(z.z[k]) > QUALITY_THRESHOLD) active++;
        }

        result.score   = score;
        result.valid   = all_valid;

        // ── Signal Quality: Q = 100 · active / K ─────────────────────────────
        result.quality = 100.0 * static_cast<double>(active)
                               / static_cast<double>(NUM_SIGNALS);

        // ── Kelly Fraction ────────────────────────────────────────────────────
        // p = sigmoid(Score)
        const double p = 1.0 / (1.0 + std::exp(-score));

        // f* = (p(b+1) - 1) / b
        const double kelly_full = (p * (b_ + 1.0) - 1.0) / b_;

        // f = f*/2, clamped to [0, 1]
        double kelly_half = kelly_full * 0.5;
        if (kelly_half < 0.0) kelly_half = 0.0;
        if (kelly_half > 1.0) kelly_half = 1.0;

        result.p_win       = p;
        result.kelly_full  = kelly_full;
        result.kelly_half  = kelly_half;

        return result;
    }

private:
    void recompute_weights() {
        // Zero negative ICs — only reward predictive signals
        double sum = 0.0;
        for (uint32_t k = 0; k < NUM_SIGNALS; ++k) {
            if (ic_[k] < 0.0) ic_[k] = 0.0;
            sum += ic_[k];
        }
        weights_.fill(0.0); // clear all including unused capacity slots
        if (sum > 0.0) {
            for (uint32_t k = 0; k < NUM_SIGNALS; ++k) weights_[k] = ic_[k] / sum;
        } else {
            // fallback: equal weights for active signals only
            for (uint32_t k = 0; k < NUM_SIGNALS; ++k) weights_[k] = 1.0 / NUM_SIGNALS;
        }
    }

    std::array<double, SIGNAL_CAPACITY> ic_      = {};
    std::array<double, SIGNAL_CAPACITY> weights_ = {};
    double                          b_       = DEFAULT_WIN_LOSS;
};

} // namespace alpha::signal::signals::derived
