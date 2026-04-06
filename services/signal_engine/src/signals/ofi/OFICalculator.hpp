/**
 * @file OFICalculator.hpp
 * @brief Ultra-low latency Order Flow Imbalance (OFI) signal calculator.
 *
 * OFI(t) = ΔQ_bid(t) - ΔQ_ask(t)
 *
 * ΔQ_bid:
 *   +Q_bid(t)               if P_bid(t) >  P_bid(t-1)   — bid price improved (new level)
 *   Q_bid(t) - Q_bid(t-1)   if P_bid(t) == P_bid(t-1)   — same level, qty change
 *   -Q_bid(t-1)             if P_bid(t) <  P_bid(t-1)   — bid retreated (full removal)
 *
 * ΔQ_ask (symmetric — ask improves when price falls):
 *   +Q_ask(t)               if P_ask(t) <  P_ask(t-1)   — ask price improved (new level)
 *   Q_ask(t) - Q_ask(t-1)   if P_ask(t) == P_ask(t-1)   — same level, qty change
 *   -Q_ask(t-1)             if P_ask(t) >  P_ask(t-1)   — ask retreated (full removal)
 *
 * Design goals:
 *   - Zero heap allocation in the hot path
 *   - O(1) update per tick
 *   - Cache-line aligned per-instrument state (no false sharing)
 *   - Fixed-size open-addressing hash table with Fibonacci hashing
 *   - No locks (single-threaded spin loop)
 *
 * Note on floating-point price equality:
 *   NSE prices arrive as discrete multiples of the tick size (0.05 INR).
 *   Upstox transmits them as IEEE-754 doubles without intermediate arithmetic,
 *   so direct == comparison is safe for detecting an unchanged best price.
 */

#pragma once

#include <alpha/models/MarketModels.hpp>
#include <cstdint>

namespace alpha::signal::signals::ofi {

// ─── Constants ───────────────────────────────────────────────────────────────

/// Hash table capacity — must be a power of 2.
/// 4096 slots → <40% load factor for ~1500 NSE instruments.
static constexpr uint32_t OFI_TABLE_CAPACITY = 4096u;
static constexpr uint32_t OFI_TABLE_MASK     = OFI_TABLE_CAPACITY - 1u;

/// Sentinel for an empty slot.
static constexpr uint32_t OFI_EMPTY_TOKEN = 0xFFFF'FFFFu;

// ─── Per-instrument state (exactly one 64-byte cache line) ───────────────────

struct alignas(64) OFIState {
    uint32_t token;             //  4 — instrument token (hash key)
    uint32_t prev_bid_size;     //  4
    uint32_t prev_ask_size;     //  4
    uint32_t tick_count;        //  4
    double   prev_bid_price;    //  8
    double   prev_ask_price;    //  8
    int64_t  cumulative_ofi;    //  8 — Σ OFI(t) since session start
    uint8_t  _pad[24];          // 24 — pad to 64 bytes
};
static_assert(sizeof(OFIState) == 64, "OFIState must be exactly one cache line");

// ─── Result ──────────────────────────────────────────────────────────────────

struct OFIResult {
    uint32_t instrument_token;
    int32_t  ofi;             // OFI(t) = ΔQ_bid - ΔQ_ask  (this tick)
    int64_t  cumulative_ofi;  // Σ OFI since session start
    float    ofi_normalized;  // OFI(t) / (Q_bid(t) + Q_ask(t)), clamped to [-1, +1]
    bool     valid;           // false on the very first tick (no prior state to diff)
};

// ─── Hash function ────────────────────────────────────────────────────────────

/// Fibonacci (multiply-shift) hash: distributes sparse NSE tokens uniformly.
[[nodiscard]] __attribute__((always_inline))
inline uint32_t ofi_hash(uint32_t token) noexcept {
    // 2^64 / φ ≈ 11400714819323198485  (golden-ratio multiplier)
    constexpr uint64_t FIBO  = 11400714819323198485ULL;
    constexpr uint32_t SHIFT = 64u - 12u; // keep top 12 bits → [0, 4096)
    return static_cast<uint32_t>((static_cast<uint64_t>(token) * FIBO) >> SHIFT);
}

// ─── OFI Calculator ──────────────────────────────────────────────────────────

/**
 * @brief Stateful, per-instrument OFI engine.
 *
 * Fixed open-addressing hash table of OFIState entries.
 * Hot path: one cache-line read + write per tick (no collision in steady state).
 *
 * Thread safety: NOT thread-safe — call from a single spin-loop thread only.
 */
class OFICalculator {
public:
    OFICalculator() noexcept {
        for (auto& s : table_) s.token = OFI_EMPTY_TOKEN;
    }

    /**
     * @brief Process one tick and return OFI for its instrument.
     */
    [[nodiscard]] __attribute__((always_inline))
    OFIResult update(const alpha::models::Tick& tick) noexcept {
        OFIState& state = find_or_insert(tick.instrument_token);

        const uint32_t bid_sz = tick.bid_size;
        const uint32_t ask_sz = tick.ask_size;
        const double   bid_px = tick.bid_price;
        const double   ask_px = tick.ask_price;

        OFIResult result;
        result.instrument_token = tick.instrument_token;
        result.valid            = (state.tick_count > 0);

        if (__builtin_expect(!result.valid, 0)) {
            // First tick — seed state, nothing to diff
            state.prev_bid_size  = bid_sz;
            state.prev_ask_size  = ask_sz;
            state.prev_bid_price = bid_px;
            state.prev_ask_price = ask_px;
            state.tick_count     = 1;
            result.ofi            = 0;
            result.cumulative_ofi = 0;
            result.ofi_normalized = 0.0f;
            return result;
        }

        // ── ΔQ_bid ────────────────────────────────────────────────────────────
        int32_t delta_bid;
        if (bid_px > state.prev_bid_price) {
            // Bid price improved — treat entire new qty as fresh buying pressure
            delta_bid = static_cast<int32_t>(bid_sz);
        } else if (bid_px == state.prev_bid_price) {
            // Same price level — net qty change at this level
            delta_bid = static_cast<int32_t>(bid_sz)
                      - static_cast<int32_t>(state.prev_bid_size);
        } else {
            // Bid price fell — previous level fully removed
            delta_bid = -static_cast<int32_t>(state.prev_bid_size);
        }

        // ── ΔQ_ask (symmetric: ask improves when price falls) ─────────────────
        int32_t delta_ask;
        if (ask_px < state.prev_ask_price) {
            // Ask price improved — treat entire new qty as fresh selling pressure
            delta_ask = static_cast<int32_t>(ask_sz);
        } else if (ask_px == state.prev_ask_price) {
            // Same price level — net qty change
            delta_ask = static_cast<int32_t>(ask_sz)
                      - static_cast<int32_t>(state.prev_ask_size);
        } else {
            // Ask price rose — previous level fully removed
            delta_ask = -static_cast<int32_t>(state.prev_ask_size);
        }

        // ── OFI ───────────────────────────────────────────────────────────────
        result.ofi = delta_bid - delta_ask;

        state.cumulative_ofi += result.ofi;
        result.cumulative_ofi = state.cumulative_ofi;

        // ── Normalized OFI: OFI(t) / (Q_bid(t) + Q_ask(t)), bounded [-1, +1] ─
        // Denominator uses current best bid/ask qty.
        // Clamp is required: when a price level is fully removed, |OFI| can
        // exceed Q_bid(t) + Q_ask(t) (e.g. large prev level removed, small new qty).
        const uint32_t total_qty = bid_sz + ask_sz;
        if (__builtin_expect(total_qty > 0u, 1)) {
            float n = static_cast<float>(result.ofi) / static_cast<float>(total_qty);
            // Branchless clamp to [-1, +1]
            if (n >  1.0f) n =  1.0f;
            if (n < -1.0f) n = -1.0f;
            result.ofi_normalized = n;
        } else {
            result.ofi_normalized = 0.0f; // undefined — no liquidity on either side
        }

        // ── Advance state ─────────────────────────────────────────────────────
        state.prev_bid_size  = bid_sz;
        state.prev_ask_size  = ask_sz;
        state.prev_bid_price = bid_px;
        state.prev_ask_price = ask_px;
        state.tick_count++;

        return result;
    }

    /// Reset all cumulative accumulators (call at session open/close).
    /// O(TABLE_CAPACITY) — never called in the hot path.
    void reset_cumulative() noexcept {
        for (auto& s : table_) {
            if (s.token != OFI_EMPTY_TOKEN) s.cumulative_ofi = 0;
        }
    }

    [[nodiscard]] uint32_t size() const noexcept { return size_; }

private:
    __attribute__((always_inline))
    OFIState& find_or_insert(uint32_t token) noexcept {
        uint32_t idx = ofi_hash(token);
        while (true) {
            OFIState& slot = table_[idx & OFI_TABLE_MASK];
            if (__builtin_expect(slot.token == token, 1))       return slot;
            if (__builtin_expect(slot.token == OFI_EMPTY_TOKEN, 0)) {
                slot       = OFIState{};
                slot.token = token;
                size_++;
                return slot;
            }
            idx++; // linear probe
        }
    }

    /// 4096 × 64 B = 256 KB — fits entirely in L2 cache.
    OFIState table_[OFI_TABLE_CAPACITY];
    uint32_t size_ = 0;
};

} // namespace alpha::signal::signals::ofi
