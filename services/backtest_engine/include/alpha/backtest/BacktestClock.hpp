#pragma once

#include <cstdint>
#include <atomic>
#include <stdexcept>

namespace alpha {
namespace backtest {

/**
 * BacktestClock — strict monotonic time-frontier for look-ahead prevention.
 *
 * The clock enforces that simulation time only moves forward. Any strategy
 * callback that queries historical data must validate against now() to ensure
 * it cannot access data with timestamp > frontier_ns (look-ahead bias).
 *
 * Thread-safe: uses acquire/release atomics for single-writer / multi-reader.
 *
 * Usage:
 *   BacktestClock clock;
 *   for (const auto& tick : ticks) {
 *       clock.advance(tick.timestamp_ns);
 *       strategy.on_tick(tick, clock);  // strategy must not look past clock.now()
 *   }
 */
class BacktestClock {
public:
    BacktestClock() : frontier_ns_(0) {}

    /**
     * Advance the frontier to ts_ns.
     * @throws std::logic_error if ts_ns < current frontier (non-monotonic tick stream).
     */
    void advance(uint64_t ts_ns) {
        uint64_t current = frontier_ns_.load(std::memory_order_relaxed);
        if (ts_ns < current) [[unlikely]] {
            throw std::logic_error(
                "BacktestClock: non-monotonic tick stream — "
                "timestamp " + std::to_string(ts_ns) +
                " < frontier " + std::to_string(current));
        }
        frontier_ns_.store(ts_ns, std::memory_order_release);
    }

    /** Current frontier (nanoseconds since epoch, IST). */
    uint64_t now() const noexcept {
        return frontier_ns_.load(std::memory_order_acquire);
    }

    /**
     * Returns true if the given timestamp is at or before the current frontier.
     * Strategy code must call this before using any indicator value.
     */
    bool is_visible(uint64_t ts_ns) const noexcept {
        return ts_ns <= frontier_ns_.load(std::memory_order_acquire);
    }

    /** Reset to t=0 (start of a new simulation run). */
    void reset() noexcept {
        frontier_ns_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> frontier_ns_;
};

} // namespace backtest
} // namespace alpha
