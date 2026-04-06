/**
 * @file EntropyCalculator.hpp
 * @brief Ultra-low latency L2 Order Book Entropy signal.
 *
 * Measures the distribution of liquidity across the 5 visible depth levels.
 * High entropy → liquidity spread evenly across levels (deep, resilient book).
 * Low entropy  → liquidity concentrated at one level (thin, fragile book).
 *
 * Combined qty per level (bid + ask):
 *   C_k = Q^bid_k + Q^ask_k        k ∈ {1..5}
 *
 * Level probability:
 *   p_k = C_k / Σ_{j=1}^{5} C_j
 *
 * Shannon entropy (nats):
 *   H(t) = -Σ_{k=1}^{5} p_k · ln(p_k)      (0 · ln 0 ≡ 0)
 *
 * Normalized entropy:
 *   H_hat(t) = H(t) / ln(10)                ∈ [0, 1]
 *
 * Normalization constant ln(10) ≈ 2.302585 is the maximum entropy when
 * treating 5 bid + 5 ask levels as 10 independent uniform contributions.
 *
 * Design goals:
 *   - Zero heap allocation
 *   - Stateless: entropy is computed purely from the current tick's L2 depth
 *   - Fixed 5-iteration unrollable inner loop — no branching on depth
 *   - Uses fast natural log (logf) — replaceable with a bit-trick approximation
 *     if sub-nanosecond precision on this signal is required
 */

#pragma once

#include <alpha/models/MarketModels.hpp>
#include <cmath>    // logf
#include <cstdint>

namespace alpha::signal::signals::entropy {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr uint32_t ENTROPY_DEPTH_LEVELS = 5u;

/// Maximum entropy divisor: ln(10) ≈ 2.302585
/// (5 bid levels + 5 ask levels treated as 10 uniform contributions)
static constexpr float LN_10 = 2.302585092994046f;

// ─── Result ──────────────────────────────────────────────────────────────────

struct EntropyResult {
    uint32_t instrument_token;
    float    H;             // Shannon entropy H(t) in nats  ∈ [0, ln(5) ≈ 1.609]
    float    H_normalized;  // H(t) / ln(10)                ∈ [0, ~0.699]
    bool     valid;         // false when total L2 qty is zero (empty book)
};

// ─── Entropy Calculator ───────────────────────────────────────────────────────

/**
 * @brief Stateless L2 order book entropy engine.
 *
 * No per-instrument state — call compute() on every tick.
 * Thread safety: fully re-entrant (no mutable state).
 */
class EntropyCalculator {
public:
    /**
     * @brief Compute L2 entropy for the current tick's order book snapshot.
     *
     * Hot path: 5 additions + 5 divisions + up to 5 logf calls.
     * logf is typically 5–15 ns on modern x86_64 with -O2.
     */
    [[nodiscard]] __attribute__((always_inline))
    EntropyResult compute(const alpha::models::Tick& tick) const noexcept {
        EntropyResult result;
        result.instrument_token = tick.instrument_token;

        // ── Step 1: combined qty per level and total ──────────────────────────
        float combined[ENTROPY_DEPTH_LEVELS];
        float total = 0.0f;

        // Manually unrolled — compiler will vectorize with -O2 / AVX
        combined[0] = static_cast<float>(tick.bids[0].quantity + tick.asks[0].quantity);
        combined[1] = static_cast<float>(tick.bids[1].quantity + tick.asks[1].quantity);
        combined[2] = static_cast<float>(tick.bids[2].quantity + tick.asks[2].quantity);
        combined[3] = static_cast<float>(tick.bids[3].quantity + tick.asks[3].quantity);
        combined[4] = static_cast<float>(tick.bids[4].quantity + tick.asks[4].quantity);

        total = combined[0] + combined[1] + combined[2]
              + combined[3] + combined[4];

        if (__builtin_expect(total == 0.0f, 0)) {
            result.H            = 0.0f;
            result.H_normalized = 0.0f;
            result.valid        = false;
            return result;
        }

        // ── Step 2: H(t) = -Σ p_k · ln(p_k) ─────────────────────────────────
        // Reformulated to avoid redundant divisions:
        //   H = ln(total) - (1/total) · Σ C_k · ln(C_k)
        //
        // This pulls ln(total) out of the loop and reduces to 5 logf calls
        // on the raw combined quantities (all positive integers when non-zero).
        //
        // Proof:
        //   -p_k · ln(p_k) = -(C_k/T) · (ln(C_k) - ln(T))
        //   Summing: -Σ p_k·ln(p_k) = ln(T)·Σ(C_k/T) - (1/T)·Σ C_k·ln(C_k)
        //                           = ln(T) - (1/T)·Σ C_k·ln(C_k)

        const float ln_total   = logf(total);
        const float inv_total  = 1.0f / total;
        float       sum_c_lnc  = 0.0f;

        // Each branch skips the logf for zero-qty levels (0·ln(0) ≡ 0)
        if (combined[0] > 0.0f) sum_c_lnc += combined[0] * logf(combined[0]);
        if (combined[1] > 0.0f) sum_c_lnc += combined[1] * logf(combined[1]);
        if (combined[2] > 0.0f) sum_c_lnc += combined[2] * logf(combined[2]);
        if (combined[3] > 0.0f) sum_c_lnc += combined[3] * logf(combined[3]);
        if (combined[4] > 0.0f) sum_c_lnc += combined[4] * logf(combined[4]);

        const float H = ln_total - inv_total * sum_c_lnc;

        // ── Step 3: normalize ─────────────────────────────────────────────────
        result.H            = H;
        result.H_normalized = H * (1.0f / LN_10); // multiply is faster than divide
        result.valid        = true;
        return result;
    }
};

} // namespace alpha::signal::signals::entropy
