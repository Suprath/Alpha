#include <gtest/gtest.h>
#include <alpha/time/Timestamp.hpp>

/**
 * @brief Verify that the IST offset is exactly 5 hours and 30 minutes.
 */
TEST(TimeSyncTest, IstOffsetAccuracy) {
    uint64_t utc_ns = alpha::time::Timestamp::now_ns();
    uint64_t ist_ns = alpha::time::Timestamp::now_ist_ns();
    
    // The difference should be exactly the defined IST offset
    EXPECT_EQ(ist_ns - utc_ns, alpha::time::Timestamp::IST_OFFSET_NS);
}

/**
 * @brief Verify component extraction from IST nanoseconds.
 */
TEST(TimeSyncTest, IstComponentExtraction) {
    // 0 ns IST = 00:00:00.000 UTC on 1970-01-01
    auto ist = alpha::time::Timestamp::get_ist(0);
    EXPECT_EQ(ist.hour, 0);
    EXPECT_EQ(ist.minute, 0);
    EXPECT_EQ(ist.second, 0);
    EXPECT_EQ(ist.millisecond, 0);
    EXPECT_EQ(ist.nanosecond, 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
