#include <gtest/gtest.h>
#include <alpha/ingester/UpstoxIngester.hpp>
#include "market_data_v3.pb.h"

using namespace com::upstox::marketdata::v3;

/**
 * @brief Verify that OptionGreeks can be correctly serialized and deserialized.
 */
TEST(OptionsDecodeTest, GreeksProtobufCheck) {
    OptionGreeks greeks;
    greeks.set_delta(0.5);
    greeks.set_gamma(0.01);
    greeks.set_theta(-10.5);
    greeks.set_vega(5.2);
    greeks.set_iv(15.5);

    std::string serialized;
    EXPECT_TRUE(greeks.SerializeToString(&serialized));

    OptionGreeks decoded;
    EXPECT_TRUE(decoded.ParseFromString(serialized));
    EXPECT_DOUBLE_EQ(decoded.delta(), 0.5);
    EXPECT_DOUBLE_EQ(decoded.theta(), -10.5);
}

/**
 * @brief Verify MarketDepth decoding.
 */
TEST(OptionsDecodeTest, MarketDepthCheck) {
    MarketDepth depth;
    auto* bid = depth.add_bids();
    bid->set_price(100.5);
    bid->set_quantity(50);
    bid->set_orders(5);

    std::string serialized;
    EXPECT_TRUE(depth.SerializeToString(&serialized));

    MarketDepth decoded;
    EXPECT_TRUE(decoded.ParseFromString(serialized));
    ASSERT_EQ(decoded.bids_size(), 1);
    EXPECT_DOUBLE_EQ(decoded.bids(0).price(), 100.5);
    EXPECT_EQ(decoded.bids(0).quantity(), 50);
}
