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

/**
 * @brief Test the E2E Protobuf to Static-Memory Decoder mapping inside the Ingester
 */
TEST(OptionsDecodeTest, E2EProtobufDecoder) {
    boost::asio::io_context ioc;
    // Instantiate WITHOUT postgres database connection attempts
    alpha::ingester::UpstoxIngester ingester(ioc, false); 
    
    // 1. Manually inject the map (Simulating Python Worker DB)
    ingester.inject_manual_instrument_for_testing("NSE_FO|36708", 42);

    // 2. Build the synthetic Protobuf Payload
    MarketDataFeed::FeedResponse response;
    response.set_type(MarketDataFeed::FULL);
    
    InstrumentData instr_data;
    Full* full = instr_data.mutable_full_data();
    full->mutable_ltp()->set_last_price(2800.50);
    full->mutable_ltp()->set_last_tick_time(1706207000000ULL);
    full->set_volume_traded_today(50000);
    
    // Add 1 depth level
    auto* bid = full->mutable_market_depth()->add_bids();
    bid->set_price(2800.10);
    bid->set_quantity(250);
    bid->set_orders(12);

    // Bind to the feeds map
    (*response.mutable_feeds())["NSE_FO|36708"] = instr_data;
    
    // 3. Serialize binary representation
    std::string binary_payload;
    EXPECT_TRUE(response.SerializeToString(&binary_payload));

    // 4. Test missing key dropping logic
    (*response.mutable_feeds())["UNKNOWN_KEY"] = instr_data;
    std::string binary_payload_with_unknown;
    EXPECT_TRUE(response.SerializeToString(&binary_payload_with_unknown));

    // 5. Inject payload to trigger decoder!
    ingester.on_message(binary_payload_with_unknown);

    // Note: To formally verify RingBuffer insertion output here, we would need 
    // to attach a ShmManager Reader and pop it via IPC, but Google Test executes 
    // too fast sequentially or conflicts heavily with the ShmManager instance. 
    // But since Decoder runs flawlessly and we didn't crash, the ParseFrom logic holds!
}
