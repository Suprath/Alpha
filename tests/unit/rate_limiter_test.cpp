#include <gtest/gtest.h>
#include <alpha/rate_limit/RateLimiter.hpp>
#include <alpha/time/Timestamp.hpp>
#include <chrono>

using namespace alpha::rate_limit;

/**
 * @brief Test that the RateLimiter correctly throttles requests.
 */
TEST(RateLimiterTest, ThrottlingCheck) {
    // 10 tokens per second, burst of 1
    RateLimiter limiter(10.0, 1.0);
    
    auto start = alpha::time::Timestamp::now_ns();
    
    // First acquisition should be instant
    limiter.acquire();
    
    // Second acquisition should take ~100ms
    limiter.acquire();
    
    auto end = alpha::time::Timestamp::now_ns();
    auto duration_ms = (end - start) / 1000000ULL;
    
    EXPECT_GE(duration_ms, 90); // Should be roughly 100ms
}

/**
 * @brief Test non-blocking try_acquire.
 */
TEST(RateLimiterTest, TryAcquire) {
    // 1 token per second, burst of 1
    RateLimiter limiter(1.0, 1.0);
    
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_FALSE(limiter.try_acquire()); // Bucket is empty
}
