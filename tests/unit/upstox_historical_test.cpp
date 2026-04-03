#include <gtest/gtest.h>
#include <alpha/ingester/UpstoxIngester.hpp>
#include <boost/asio.hpp>
#include <thread>
#include <chrono>

namespace alpha::ingester {

// We need to declare the helper here or make it public/friend in UpstoxIngester.
// Since it's a helper in the .cpp, let's just test the logic directly or move it to a utility.
// For now, I'll assume we want to test the parsing logic.
uint64_t parse_upstox_ts_test(const std::string& ts_str) {
    std::tm tm = {};
    std::istringstream ss(ts_str.substr(0, 19));
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;
    // tm_isdst = -1 means mktime should determine if DST is in effect.
    tm.tm_isdst = -1;
    auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

class UpstoxHistoricalTest : public ::testing::Test {
protected:
    boost::asio::io_context ioc;
};

TEST_F(UpstoxHistoricalTest, TimestampParsing) {
    std::string ts = "2023-11-01T09:15:00+05:30";
    uint64_t ns = parse_upstox_ts_test(ts);
    EXPECT_GT(ns, 0);
    
    // Check if it's roughly the correct year (2023)
    // 2023-01-01 is ~1672531200 seconds
    EXPECT_GT(ns, 1600000000000000000ULL);
}

TEST_F(UpstoxHistoricalTest, QueueTaskIntegration) {
    auto ingester = std::make_shared<UpstoxIngester>(ioc, false);
    ingester->authenticate("mock_token");
    ingester->fetch_historical("NSE_EQ|INE002A01018", "1minute", "2023-11-01", "2023-11-02");
    
    // The test succeeds if it doesn't crash during initialization and queuing.
}

} // namespace alpha::ingester
