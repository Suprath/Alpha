#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdint>

namespace alpha {
namespace backfill {

/**
 * SlidingWindowThrottler — exact sliding-window API rate limiter.
 *
 * More precise than a token bucket for strict compliance because it tracks
 * actual request timestamps. The bucket can refill faster than intended
 * if requests cluster at window boundaries; the sliding window cannot.
 *
 * Upstox free tier (historical API): ~100 requests / 60 seconds.
 * Configure: SlidingWindowThrottler throttler(100, 60'000'000'000ULL);
 *
 * Thread-safe: acquire() and current_count() may be called concurrently.
 */
class SlidingWindowThrottler {
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Nanos     = std::chrono::nanoseconds;

    /**
     * @param max_requests  Max requests permitted within the window.
     * @param window_ns     Sliding window duration in nanoseconds.
     */
    SlidingWindowThrottler(uint32_t max_requests, uint64_t window_ns)
        : max_requests_(max_requests)
        , window_ns_(window_ns)
    {}

    /**
     * Block until the next request can be issued without exceeding the rate.
     * Wakes up as soon as the oldest in-window timestamp expires.
     */
    void acquire() {
        std::unique_lock<std::mutex> lk(mutex_);
        evict_expired_locked();

        while (timestamps_.size() >= max_requests_) {
            // Sleep until the oldest request falls out of the window
            TimePoint wake_at = timestamps_.front() + Nanos(static_cast<int64_t>(window_ns_));
            lk.unlock();
            std::this_thread::sleep_until(wake_at);
            lk.lock();
            evict_expired_locked();
        }

        timestamps_.push_back(Clock::now());
    }

    /**
     * Non-blocking attempt. Returns true and records the request if under limit.
     * Returns false immediately if the rate would be exceeded.
     */
    bool try_acquire() {
        std::lock_guard<std::mutex> lk(mutex_);
        evict_expired_locked();
        if (timestamps_.size() >= max_requests_) return false;
        timestamps_.push_back(Clock::now());
        return true;
    }

    /** Requests currently in the window (snapshot — may be stale immediately). */
    uint32_t current_count() {
        std::lock_guard<std::mutex> lk(mutex_);
        evict_expired_locked();
        return static_cast<uint32_t>(timestamps_.size());
    }

    /** Remaining capacity before the rate limit is hit. */
    uint32_t remaining() {
        std::lock_guard<std::mutex> lk(mutex_);
        evict_expired_locked();
        uint32_t used = static_cast<uint32_t>(timestamps_.size());
        return (used < max_requests_) ? (max_requests_ - used) : 0;
    }

private:
    void evict_expired_locked() {
        auto cutoff = Clock::now() - Nanos(static_cast<int64_t>(window_ns_));
        while (!timestamps_.empty() && timestamps_.front() < cutoff)
            timestamps_.pop_front();
    }

    uint32_t                max_requests_;
    uint64_t                window_ns_;
    std::deque<TimePoint>   timestamps_;
    std::mutex              mutex_;
};

} // namespace backfill
} // namespace alpha
