/**
 * @file VPINCalculator.hpp
 * @brief Ultra-low latency VPIN (Volume-synchronized Probability of Informed Trading).
 *
 * Bucket size:
 *   σ_v = ADV / n_buckets        (default n_buckets = 50)
 *
 * Per-bucket accumulators (fill until total volume reaches σ_v):
 *   V^B_τ = Σ_{t∈τ} v(t) · 1[d(t) > 0]   — buyer-initiated volume
 *   V^S_τ = Σ_{t∈τ} v(t) · 1[d(t) < 0]   — seller-initiated volume
 *
 * Rolling VPIN over the last N_BUCKETS completed buckets:
 *   VPIN = Σ_{τ=1}^{n} |V^B_τ - V^S_τ| / (n · σ_v)   ∈ [0, 1]
 *
 * The running sum of imbalances is maintained so VPIN updates in O(1) per
 * bucket close — no re-scan of the window.
 *
 * Bucket-overflow handling:
 *   When a single tick's volume spans a bucket boundary, the volume is split
 *   proportionally between the current bucket and subsequent ones. This is
 *   handled with a small carry loop (O(1) amortized — very rare in practice).
 *
 * Design goals:
 *   - Zero heap allocation in the hot path
 *   - O(1) amortized update per tick
 *   - Hot-path fields packed into cache line 0 of VPINState
 *   - Fixed-size open-addressing hash table with Fibonacci hashing
 *   - No locks (single-threaded spin loop)
 */

#pragma once

#include <alpha/models/MarketModels.hpp>
#include <cstdint>

namespace alpha::signal::signals::vpin {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr uint32_t VPIN_N_BUCKETS      = 50u;
static constexpr uint64_t VPIN_DEFAULT_ADV     = 10'000'000ULL; // fallback ADV in shares
static constexpr uint64_t VPIN_DEFAULT_BUCKET  = VPIN_DEFAULT_ADV / VPIN_N_BUCKETS;

/// Hash table: 512 slots × 512 bytes = 256 KB — fits in L2 cache.
static constexpr uint32_t VPIN_TABLE_CAPACITY = 512u;
static constexpr uint32_t VPIN_TABLE_MASK     = VPIN_TABLE_CAPACITY - 1u;
static constexpr uint32_t VPIN_EMPTY_TOKEN    = 0xFFFF'FFFFu;

// ─── Per-instrument state ─────────────────────────────────────────────────────
//
// Layout (512 bytes = 8 cache lines):
//   Cache line 0  — hot-path scalar fields (read/written on every tick)
//   Cache lines 1–7 — ring buffer of 50 bucket imbalances (read/written on bucket close)

struct alignas(64) VPINState {
    // ── Cache line 0 (hot path, 64 bytes) ────────────────────────────────────
    uint32_t token;             //  4
    uint32_t bucket_count;      //  4  — total completed buckets (ever)
    uint64_t bucket_size;       //  8  — σ_v = ADV / N_BUCKETS
    uint64_t cur_bucket_vol;    //  8  — total vol accumulated in current bucket
    uint64_t cur_buy_vol;       //  8  — V^B accumulator for current bucket
    uint64_t cur_sell_vol;      //  8  — V^S accumulator for current bucket
    uint64_t sum_imbalances;    //  8  — rolling Σ|V^B_τ - V^S_τ| over window
    uint32_t window_head;       //  4  — next write index in the ring buffer
    float    vpin;              //  4  — latest VPIN ∈ [0, 1]
    // = 60 bytes; pad to 64
    uint8_t  _pad0[4];          //  4

    // ── Cache lines 1–7 (ring buffer, 448 bytes) ──────────────────────────────
    uint64_t imbalances[VPIN_N_BUCKETS]; // 50 × 8 = 400 bytes
    uint8_t  _pad1[48];         // 48  → total 512 bytes
};
static_assert(sizeof(VPINState) == 512, "VPINState must be 512 bytes (8 cache lines)");

// ─── Result ──────────────────────────────────────────────────────────────────

struct VPINResult {
    uint32_t instrument_token;
    float    vpin;              // VPIN ∈ [0, 1]  (0 until first bucket closes)
    uint32_t buckets_completed; // total completed buckets since session start
    bool     bucket_closed;     // true if this tick triggered a bucket close
    bool     valid;             // false until at least 1 bucket is complete
};

// ─── Hash function ────────────────────────────────────────────────────────────

[[nodiscard]] __attribute__((always_inline))
inline uint32_t vpin_hash(uint32_t token) noexcept {
    constexpr uint64_t FIBO  = 11400714819323198485ULL; // 2^64 / φ
    constexpr uint32_t SHIFT = 64u - 9u;                // → [0, 512)
    return static_cast<uint32_t>((static_cast<uint64_t>(token) * FIBO) >> SHIFT);
}

// ─── VPIN Calculator ─────────────────────────────────────────────────────────

/**
 * @brief Stateful, per-instrument VPIN engine.
 *
 * Call set_adv() before the session starts for each tracked instrument.
 * Call update() on every tick, passing the trade direction from TradeDirectionCalculator.
 *
 * Thread safety: NOT thread-safe — single-threaded spin loop only.
 */
class VPINCalculator {
public:
    VPINCalculator() noexcept {
        for (auto& s : table_) s.token = VPIN_EMPTY_TOKEN;
    }

    /**
     * @brief Configure the ADV (average daily volume) for an instrument.
     *        Must be called before the first update() for that instrument.
     *        bucket_size = adv / N_BUCKETS.
     */
    void set_adv(uint32_t token, uint64_t adv) noexcept {
        VPINState& state = find_or_insert(token);
        state.bucket_size = (adv > 0u) ? (adv / VPIN_N_BUCKETS) : VPIN_DEFAULT_BUCKET;
    }

    /**
     * @brief Process one tick.
     *
     * @param tick      Current market tick (v(t) = tick.last_quantity)
     * @param direction Trade direction: +1 buy, -1 sell, 0 indeterminate
     *                  (from TradeDirectionCalculator::update().direction)
     */
    [[nodiscard]] __attribute__((always_inline))
    VPINResult update(const alpha::models::Tick& tick, int8_t direction) noexcept {
        VPINState& state = find_or_insert(tick.instrument_token);

        VPINResult result;
        result.instrument_token = tick.instrument_token;
        result.bucket_closed    = false;

        const uint64_t vol = static_cast<uint64_t>(tick.last_quantity);

        if (__builtin_expect(vol == 0u, 0)) {
            result.vpin              = state.vpin;
            result.buckets_completed = state.bucket_count;
            result.valid             = (state.bucket_count > 0u);
            return result;
        }

        // ── Accumulate volume, handling bucket-boundary overflow ──────────────
        uint64_t buy_carry  = (direction > 0) ? vol : 0u;
        uint64_t sell_carry = (direction < 0) ? vol : 0u;
        uint64_t vol_carry  = vol;

        while (vol_carry > 0u) {
            const uint64_t capacity = state.bucket_size - state.cur_bucket_vol;

            if (vol_carry < capacity) {
                // Fits entirely in the current bucket without filling it — common case
                state.cur_bucket_vol += vol_carry;
                state.cur_buy_vol    += buy_carry;
                state.cur_sell_vol   += sell_carry;
                vol_carry   = 0u;
                buy_carry   = 0u;
                sell_carry  = 0u;
            } else {
                // Tick overflows current bucket — fill it to the brim
                // Split buy/sell volume proportionally to the overflow fraction
                const uint64_t bv = (buy_carry  * capacity) / vol_carry;
                const uint64_t sv = (sell_carry * capacity) / vol_carry;

                state.cur_bucket_vol += capacity;
                state.cur_buy_vol    += bv;
                state.cur_sell_vol   += sv;

                // Close the full bucket
                close_bucket(state);
                result.bucket_closed = true;

                // Carry the remainder forward into the next bucket
                vol_carry  -= capacity;
                buy_carry  -= bv;
                sell_carry -= sv;
            }
        }

        result.vpin              = state.vpin;
        result.buckets_completed = state.bucket_count;
        result.valid             = (state.bucket_count > 0u);
        return result;
    }

    /// Reset VPIN state for all instruments (call at session open).
    void reset() noexcept {
        for (auto& s : table_) {
            if (s.token == VPIN_EMPTY_TOKEN) continue;
            const uint64_t bs     = s.bucket_size; // preserve configured bucket size
            const uint32_t tok    = s.token;
            s                     = VPINState{};
            s.token               = tok;
            s.bucket_size         = bs;
        }
    }

    [[nodiscard]] uint32_t size() const noexcept { return size_; }

private:
    // ── Bucket close — called only when a bucket fills ────────────────────────

    __attribute__((noinline)) // keep hot path lean; bucket close is rare
    void close_bucket(VPINState& state) noexcept {
        // Imbalance for this bucket: |V^B - V^S|
        const uint64_t imbalance = (state.cur_buy_vol >= state.cur_sell_vol)
            ? (state.cur_buy_vol  - state.cur_sell_vol)
            : (state.cur_sell_vol - state.cur_buy_vol);

        // ── Update rolling window ─────────────────────────────────────────────
        // VPIN_N_BUCKETS = 50 is not a power of 2 — use modulo, not bitmask.
        const uint32_t slot = state.window_head % VPIN_N_BUCKETS;

        // Subtract the value being evicted from the running sum
        state.sum_imbalances -= state.imbalances[slot];

        // Write new imbalance into the ring
        state.imbalances[slot] = imbalance;
        state.sum_imbalances  += imbalance;

        // Advance head
        state.window_head = (slot + 1u) % VPIN_N_BUCKETS;
        state.bucket_count++;

        // ── Recompute VPIN ────────────────────────────────────────────────────
        // VPIN = Σ|V^B_τ - V^S_τ| / (n · σ_v)
        // where n = min(bucket_count, N_BUCKETS)
        const uint32_t n = (state.bucket_count < VPIN_N_BUCKETS)
                         ? state.bucket_count
                         : VPIN_N_BUCKETS;

        const uint64_t denom = static_cast<uint64_t>(n) * state.bucket_size;

        state.vpin = (denom > 0u)
            ? static_cast<float>(state.sum_imbalances) / static_cast<float>(denom)
            : 0.0f;

        // Clamp to [0, 1] (should be natural, but guard against rounding)
        if (state.vpin > 1.0f) state.vpin = 1.0f;

        // ── Reset current-bucket accumulators ─────────────────────────────────
        state.cur_bucket_vol = 0u;
        state.cur_buy_vol    = 0u;
        state.cur_sell_vol   = 0u;
    }

    // ── Open-addressing hash table ────────────────────────────────────────────

    __attribute__((always_inline))
    VPINState& find_or_insert(uint32_t token) noexcept {
        uint32_t idx = vpin_hash(token);
        while (true) {
            VPINState& slot = table_[idx & VPIN_TABLE_MASK];
            if (__builtin_expect(slot.token == token, 1))        return slot;
            if (__builtin_expect(slot.token == VPIN_EMPTY_TOKEN, 0)) {
                slot             = VPINState{};
                slot.token       = token;
                slot.bucket_size = VPIN_DEFAULT_BUCKET;
                size_++;
                return slot;
            }
            idx++; // linear probe
        }
    }

    // ── Storage ───────────────────────────────────────────────────────────────

    /// 512 × 512 B = 256 KB — fits in L2 cache.
    VPINState table_[VPIN_TABLE_CAPACITY];
    uint32_t  size_ = 0;
};

} // namespace alpha::signal::signals::vpin
