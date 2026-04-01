#include <gtest/gtest.h>
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>

using namespace alpha::models;

/**
 * @brief Verify that a 1-minute candle expands into 4Ticks at the end-of-minute boundary.
 */
TEST(NormalizationTest, CandleToTicksClustered) {
    uint64_t start_ts = 1711996200000000000ULL; // A sample timestamp
    
    Candle c;
    c.timestamp_ns = start_ts;
    c.open = 100.0;
    c.high = 110.0;
    c.low = 90.0;
    c.close = 105.0;
    c.volume = 10000;
    
    auto ticks = c.normalize_to_ticks();
    
    ASSERT_EQ(ticks.size(), 4);
    
    uint64_t expected_end_ts = start_ts + 59999000000ULL;
    
    // Check Clustered Timestamps
    EXPECT_EQ(ticks[0].timestamp_ns, expected_end_ts);
    EXPECT_EQ(ticks[1].timestamp_ns, expected_end_ts);
    EXPECT_EQ(ticks[2].timestamp_ns, expected_end_ts);
    EXPECT_EQ(ticks[3].timestamp_ns, expected_end_ts);
    
    // Check Prices (O, H, L, C)
    EXPECT_DOUBLE_EQ(ticks[0].last_price, 100.0);
    EXPECT_DOUBLE_EQ(ticks[1].last_price, 110.0);
    EXPECT_DOUBLE_EQ(ticks[2].last_price, 90.0);
    EXPECT_DOUBLE_EQ(ticks[3].last_price, 105.0);
}

/**
 * @brief Verify that a 5-minute candle expands into 4Ticks at the end-of-5-minute boundary.
 */
TEST(NormalizationTest, CandleToTicks5MinInterval) {
    uint64_t start_ts = 1711996200000000000ULL;
    Candle c;
    c.timestamp_ns = start_ts;
    c.open = 100.0;
    
    // 5 minutes in nanoseconds
    uint64_t interval_5m = 299999000000ULL; 
    auto ticks = c.normalize_to_ticks(interval_5m);
    
    ASSERT_EQ(ticks.size(), 4);
    uint64_t expected_end_ts = start_ts + interval_5m;
    EXPECT_EQ(ticks[0].timestamp_ns, expected_end_ts);
}
