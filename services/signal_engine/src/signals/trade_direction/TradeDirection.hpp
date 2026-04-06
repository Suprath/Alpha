/**
 * @file TradeDirection.hpp
 * @brief Ultra-low latency Trade Direction classifier (Lee-Ready quote rule).
 *
 * Formula:
 *   d(t) = +1   if P(t) >= P_ask(t-1)   — buyer-initiated (hit the ask)
 *   d(t) = -1   if P(t) <= P_bid(t-1)   — seller-initiated (hit the bid)
 *   d(t) =  0   otherwise               — mid-market / indeterminate
 *
 *   P(t)        = last trade price
 *   P_ask(t-1)  = best ask price on the previous tick
 *   P_bid(t-1)  = best bid price on the previous tick
 *
 * Design goals:
 *   - Zero heap allocation in the hot path
 *   - O(1) update per tick
 *   - Cache-line aligned per-instrument state (no false sharing)
 *   - Fixed-size open-addressing hash table with Fibonacci hashing
 *   - No locks (single-threaded spin loop)
 */

#pragma once

#include <alpha/models/MarketModels.hpp>
#include <cstdint>

namespace alpha::signal::signals::trade_direction {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr uint32_t TD_TABLE_CAPACITY = 4096u;
static constexpr uint32_t TD_TABLE_MASK     = TD_TABLE_CAPACITY - 1u;
static constexpr uint32_t TD_EMPTY_TOKEN    = 0xFFFF'FFFFu;

// ─── Per-instrument state (exactly one 64-byte cache line) ───────────────────

struct alignas(64) TDState {
    uint32_t token;               //  4
    uint32_t tick_count;          //  4
    int32_t  cumulative_direction;//  4 — Σ d(t) since session start
    int32_t  _pad0;               //  4 — alignment
    double   prev_bid_price;      //  8
    double   prev_ask_price;      //  8
    uint8_t  _pad1[32];           // 32 — pad to 64 bytes
};
static_assert(sizeof(TDState) == 64, "TDState must be exactly one cache line");

// ─── Result ──────────────────────────────────────────────────────────────────

struct TDResult {
    uint32_t instrument_token;
    int8_t   direction;           // +1 buy, -1 sell, 0 indeterminate
    int32_t  cumulative_direction;// Σ d(t) since session start
    bool     valid;               // false on the very first tick (no prior quotes)
};

// ─── Hash function ────────────────────────────────────────────────────────────

[[nodiscard]] __attribute__((always_inline))
inline uint32_t td_hash(uint32_t token) noexcept {
    constexpr uint64_t FIBO  = 11400714819323198485ULL; // 2^64 / φ
    constexpr uint32_t SHIFT = 64u - 12u;               // → [0, 4096)
    return static_cast<uint32_t>((static_cast<uint64_t>(token) * FIBO) >> SHIFT);
}

// ─── Trade Direction Calculator ──────────────────────────────────────────────

/**
 * @brief Stateful, per-instrument trade direction classifier.
 *
 * Uses the previous tick's best bid/ask quotes to classify the current
 * trade price. Maintains a running cumulative direction score per instrument.
 *
 * Thread safety: NOT thread-safe — intended for a single-threaded spin loop.
 */
class TradeDirectionCalculator {
public:
    TradeDirectionCalculator() noexcept {
        for (auto& s : table_) s.token = TD_EMPTY_TOKEN;
    }

    /**
     * @brief Classify trade direction for one tick.
     *
     * Hot path: one cache-line read + write per tick (no collision in steady state).
     */
    [[nodiscard]] __attribute__((always_inline))
    TDResult update(const alpha::models::Tick& tick) noexcept {
        TDState& state = find_or_insert(tick.instrument_token);

        TDResult result;
        result.instrument_token = tick.instrument_token;
        result.valid            = (state.tick_count > 0);

        if (__builtin_expect(!result.valid, 0)) {
            // First tick — seed quotes, direction undefined
            state.prev_bid_price = tick.bid_price;
            state.prev_ask_price = tick.ask_price;
            state.tick_count     = 1;
            result.direction           = 0;
            result.cumulative_direction = 0;
            return result;
        }

        // ── Classify ──────────────────────────────────────────────────────────
        const double last_px = tick.last_price;

        int8_t d;
        if (last_px >= state.prev_ask_price) {
            d = +1;  // trade at or above prev ask — buyer initiated
        } else if (last_px <= state.prev_bid_price) {
            d = -1;  // trade at or below prev bid — seller initiated
        } else {
            d =  0;  // inside the spread — indeterminate
        }

        result.direction = d;

        state.cumulative_direction += d;
        result.cumulative_direction = state.cumulative_direction;

        // ── Advance quotes ────────────────────────────────────────────────────
        state.prev_bid_price = tick.bid_price;
        state.prev_ask_price = tick.ask_price;
        state.tick_count++;

        return result;
    }

    /// Reset cumulative direction for all instruments (call at session open/close).
    void reset_cumulative() noexcept {
        for (auto& s : table_) {
            if (s.token != TD_EMPTY_TOKEN) s.cumulative_direction = 0;
        }
    }

    [[nodiscard]] uint32_t size() const noexcept { return size_; }

private:
    __attribute__((always_inline))
    TDState& find_or_insert(uint32_t token) noexcept {
        uint32_t idx = td_hash(token);
        while (true) {
            TDState& slot = table_[idx & TD_TABLE_MASK];
            if (__builtin_expect(slot.token == token, 1))       return slot;
            if (__builtin_expect(slot.token == TD_EMPTY_TOKEN, 0)) {
                slot       = TDState{};
                slot.token = token;
                size_++;
                return slot;
            }
            idx++; // linear probe
        }
    }

    /// 4096 × 64 B = 256 KB — fits entirely in L2 cache.
    TDState  table_[TD_TABLE_CAPACITY];
    uint32_t size_ = 0;
};

} // namespace alpha::signal::signals::trade_direction
