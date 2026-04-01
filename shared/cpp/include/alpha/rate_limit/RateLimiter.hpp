#pragma once

#include <chrono>
#include <mutex>
#include <thread>
#include <algorithm>

namespace alpha::rate_limit {

/**
 * @brief Token Bucket Rate Limiter.
 * Prevents "Too Many Requests" (429) errors from external APIs.
 */
class RateLimiter {
public:
    /**
     * @param tokens_per_second Sustained rate of requests.
     * @param burst_size Maximum number of tokens the bucket can hold.
     */
    RateLimiter(double tokens_per_second, double burst_size)
        : rate_(tokens_per_second),
          burst_size_(burst_size),
          tokens_(burst_size),
          last_update_(std::chrono::steady_clock::now()) {}

    /**
     * @brief Acquire 1 token. Blocks until a token is available.
     */
    void acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        while (tokens_ < 1.0) {
            update_tokens();
            if (tokens_ < 1.0) {
                // Wait for the next token to be available
                auto wait_ms = static_cast<uint64_t>((1.0 - tokens_) / rate_ * 1000.0);
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                lock.lock();
                update_tokens();
            }
        }
        
        tokens_ -= 1.0;
    }

    /**
     * @brief Non-blocking attempt to acquire 1 token.
     */
    bool try_acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        update_tokens();
        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            return true;
        }
        return false;
    }

private:
    void update_tokens() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_update_).count();
        
        tokens_ = std::min(burst_size_, tokens_ + elapsed * rate_);
        last_update_ = now;
    }

    double rate_;
    double burst_size_;
    double tokens_;
    std::chrono::steady_clock::time_point last_update_;
    std::mutex mutex_;
};

} // namespace alpha::rate_limit
