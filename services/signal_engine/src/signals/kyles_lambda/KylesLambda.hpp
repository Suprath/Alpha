/**
 * @file KylesLambda.hpp
 * @brief Ultra-low latency Kyle's Lambda — rolling OLS price-impact estimator.
 *
 * Kyle's Lambda measures market illiquidity: how much price moves per unit
 * of signed order flow imbalance (OFI). A higher λ means less liquidity.
 *
 * Rolling OLS over the last KYLE_WINDOW ticks (default: 20):
 *
 *   ΔP(t)    = P(t) − P(t−1)          (last trade price change)
 *   ΔOFI(t)  = OFI(t)                 (from OFICalculator, this tick)
 *
 *   Numerator   (recursive):  S_dp_dofi(t) = S_dp_dofi(t−1) + ΔP(t)·ΔOFI(t)
 *   Denominator (recursive):  S_dofi2(t)   = S_dofi2(t−1)   + (ΔOFI(t))²
 *
 *   λ̂(t) = S_dp_dofi(t) / S_dofi2(t)
 *
 * Rolling subtraction is O(1): the ring buffer stores each tick's contribution
 * so the oldest entry can be evicted without re-scanning the window.
 *
 * Design goals:
 *   - Zero heap allocation in the hot path
 *   - O(1) update per tick
 *   - Hot-path scalars packed into cache line 0 of KylesLambdaState
 *   - Fixed-size open-addressing hash table with Fibonacci hashing
 *   - No locks (single-threaded spin loop)
 */

#pragma once

#include <alpha/models/MarketModels.hpp>
#include <cstdint>

namespace alpha::signal::signals::kyles_lambda {

// ─── Constants ───────────────────────────────────────────────────────────────

/// Rolling OLS window length (standard: 20 ticks).
static constexpr uint32_t KYLE_WINDOW = 20u;

/// Hash table: 512 slots × 384 bytes = 192 KB — fits in L2 cache.
static constexpr uint32_t KYLE_TABLE_CAPACITY = 512u;
static constexpr uint32_t KYLE_TABLE_MASK     = KYLE_TABLE_CAPACITY - 1u;
static constexpr uint32_t KYLE_EMPTY_TOKEN    = 0xFFFF'FFFFu;

// ─── Ring-buffer entry ────────────────────────────────────────────────────────

/// One tick's contribution to the rolling OLS sums.
/// Stored so the oldest entry can be evicted in O(1).
struct KylesLambdaEntry {
    double dp_dofi; //  8  — ΔP(t) · ΔOFI(t)  (numerator contribution)
    double dofi_sq; //  8  — (ΔOFI(t))²        (denominator contribution)
};
static_assert(sizeof(KylesLambdaEntry) == 16);

// ─── Per-instrument state (384 bytes = 6 cache lines) ────────────────────────
//
// Cache line 0 (64 B): all hot-path scalars — read/written on every tick.
// Cache lines 1–5 (320 B): ring buffer — read only on eviction (every KYLE_WINDOW ticks).

struct alignas(64) KylesLambdaState {
    // ── Cache line 0 (hot path) ───────────────────────────────────────────────
    uint32_t token;         //  4
    uint32_t count;         //  4  — total ticks processed for this instrument
    uint32_t head;          //  4  — next write position in the ring buffer
    uint32_t _pad0;         //  4
    double   prev_price;    //  8  — P(t−1), to compute ΔP(t)
    double   sum_dp_dofi;   //  8  — S_dp_dofi: rolling Σ ΔP·ΔOFI
    double   sum_dofi_sq;   //  8  — S_dofi2:   rolling Σ (ΔOFI)²
    double   lambda;        //  8  — λ̂(t): current Kyle's Lambda
    uint8_t  _pad1[16];     // 16  — pad to 64 bytes
    // = 64 bytes ✓

    // ── Cache lines 1–5 (ring buffer) ────────────────────────────────────────
    KylesLambdaEntry window[KYLE_WINDOW]; // 20 × 16 = 320 bytes
    // Total: 64 + 320 = 384 bytes ✓
};
static_assert(sizeof(KylesLambdaState) == 384,
              "KylesLambdaState must be 384 bytes (6 cache lines)");

// ─── Result ──────────────────────────────────────────────────────────────────

struct KylesLambdaResult {
    uint32_t instrument_token;
    double   lambda;        // λ̂(t): price impact per unit OFI (price units / qty)
    double   sum_dp_dofi;   // S_dp_dofi — numerator (useful for debugging)
    double   sum_dofi_sq;   // S_dofi2   — denominator
    uint32_t ticks_in_window; // number of valid ticks contributing (up to KYLE_WINDOW)
    bool     valid;         // false until window is full (KYLE_WINDOW ticks)
};

// ─── Hash function ────────────────────────────────────────────────────────────

[[nodiscard]] __attribute__((always_inline))
inline uint32_t kyle_hash(uint32_t token) noexcept {
    constexpr uint64_t FIBO  = 11400714819323198485ULL; // 2^64 / φ
    constexpr uint32_t SHIFT = 64u - 9u;                // → [0, 512)
    return static_cast<uint32_t>((static_cast<uint64_t>(token) * FIBO) >> SHIFT);
}

// ─── Kyle's Lambda Calculator ─────────────────────────────────────────────────

/**
 * @brief Stateful, per-instrument Kyle's Lambda engine.
 *
 * Call update() on every tick, passing the current OFI value from OFICalculator.
 * λ is recomputed on every tick and is valid once KYLE_WINDOW ticks have elapsed.
 *
 * Thread safety: NOT thread-safe — single-threaded spin loop only.
 */
class KylesLambdaCalculator {
public:
    KylesLambdaCalculator() noexcept {
        for (auto& s : table_) s.token = KYLE_EMPTY_TOKEN;
    }

    /**
     * @brief Process one tick and return the updated Kyle's Lambda.
     *
     * @param tick  Current market tick  (P(t) = tick.last_price)
     * @param ofi   ΔOFI(t) from OFICalculator::update().ofi  (int32_t)
     */
    [[nodiscard]] __attribute__((always_inline))
    KylesLambdaResult update(const alpha::models::Tick& tick,
                             int32_t                    ofi) noexcept {
        KylesLambdaState& state = find_or_insert(tick.instrument_token);

        KylesLambdaResult result;
        result.instrument_token = tick.instrument_token;

        const double price = tick.last_price;

        if (__builtin_expect(state.count == 0u, 0)) {
            // First tick — seed prev_price, nothing to compute yet
            state.prev_price = price;
            state.count      = 1u;
            result.lambda        = 0.0;
            result.sum_dp_dofi   = 0.0;
            result.sum_dofi_sq   = 0.0;
            result.ticks_in_window = 0u;
            result.valid         = false;
            return result;
        }

        // ── Compute this tick's contributions ─────────────────────────────────
        const double d_price = price - state.prev_price;       // ΔP(t)
        const double d_ofi   = static_cast<double>(ofi);       // ΔOFI(t)

        const double dp_dofi = d_price * d_ofi;                // ΔP · ΔOFI
        const double dofi_sq = d_ofi   * d_ofi;                // (ΔOFI)²

        // ── Rolling window eviction (O(1)) ────────────────────────────────────
        const uint32_t slot = state.head % KYLE_WINDOW;

        if (state.count > KYLE_WINDOW) {
            // Window is full — subtract the entry being overwritten
            state.sum_dp_dofi -= state.window[slot].dp_dofi;
            state.sum_dofi_sq -= state.window[slot].dofi_sq;
        }

        // ── Write new entry and add to running sums ───────────────────────────
        state.window[slot].dp_dofi  = dp_dofi;
        state.window[slot].dofi_sq  = dofi_sq;

        state.sum_dp_dofi += dp_dofi;
        state.sum_dofi_sq += dofi_sq;

        // ── Advance state ─────────────────────────────────────────────────────
        state.head       = slot + 1u;  // next write position (unbounded; mod on read)
        state.prev_price = price;
        state.count++;

        // ── Recompute λ̂(t) = S_dp_dofi / S_dofi2 ────────────────────────────
        if (__builtin_expect(state.sum_dofi_sq != 0.0, 1)) {
            state.lambda = state.sum_dp_dofi / state.sum_dofi_sq;
        } else {
            // Zero denominator: no OFI variation in the window — market is frozen
            state.lambda = 0.0;
        }

        const uint32_t n = (state.count - 1u < KYLE_WINDOW)
                         ? (state.count - 1u)
                         : KYLE_WINDOW;

        result.lambda          = state.lambda;
        result.sum_dp_dofi     = state.sum_dp_dofi;
        result.sum_dofi_sq     = state.sum_dofi_sq;
        result.ticks_in_window = n;
        result.valid           = (n >= KYLE_WINDOW);  // full window required

        return result;
    }

    /// Reset all state (call at session open/close).
    void reset() noexcept {
        for (auto& s : table_) {
            if (s.token == KYLE_EMPTY_TOKEN) continue;
            const uint32_t tok = s.token;
            s       = KylesLambdaState{};
            s.token = tok;
        }
    }

    [[nodiscard]] uint32_t size() const noexcept { return size_; }

private:
    __attribute__((always_inline))
    KylesLambdaState& find_or_insert(uint32_t token) noexcept {
        uint32_t idx = kyle_hash(token);
        while (true) {
            KylesLambdaState& slot = table_[idx & KYLE_TABLE_MASK];
            if (__builtin_expect(slot.token == token, 1))         return slot;
            if (__builtin_expect(slot.token == KYLE_EMPTY_TOKEN, 0)) {
                slot       = KylesLambdaState{};
                slot.token = token;
                size_++;
                return slot;
            }
            idx++; // linear probe
        }
    }

    /// 512 × 384 B = 192 KB — fits in L2 cache.
    KylesLambdaState table_[KYLE_TABLE_CAPACITY];
    uint32_t         size_ = 0;
};

} // namespace alpha::signal::signals::kyles_lambda
