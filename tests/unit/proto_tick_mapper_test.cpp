#include <gtest/gtest.h>
#include "ProtoMapper.hpp"
#include <alpha/models/MarketModels.hpp>

using namespace alpha::ingester;
using namespace alpha::models;

// ── Scalar fields ─────────────────────────────────────────────────────────────

TEST(ProtoTickMapper, ScalarFields) {
    alpha::feed::Tick p;
    p.set_token(42);
    p.set_timestamp_ns(1711996200000000000ULL);
    p.set_last_price(2345.50);
    p.set_volume(500000);
    p.set_open_interest(12500.0);
    p.set_bid_price(2345.00);
    p.set_bid_size(200);
    p.set_ask_price(2346.00);
    p.set_ask_size(150);

    Tick t = proto_to_tick(p);

    EXPECT_EQ(t.instrument_token,    42u);
    EXPECT_EQ(t.timestamp_ns,        1711996200000000000ULL);
    EXPECT_DOUBLE_EQ(t.last_price,   2345.50);
    EXPECT_EQ(t.total_volume,        500000ULL);
    EXPECT_DOUBLE_EQ(t.open_interest, 12500.0);
    EXPECT_DOUBLE_EQ(t.bid_price,    2345.00);
    EXPECT_EQ(t.bid_size,            200u);
    EXPECT_DOUBLE_EQ(t.ask_price,    2346.00);
    EXPECT_EQ(t.ask_size,            150u);
}

// ── L2 depth — full 5 levels ──────────────────────────────────────────────────

TEST(ProtoTickMapper, FiveDepthLevels) {
    alpha::feed::Tick p;
    for (int i = 0; i < 5; ++i) {
        auto* bid = p.add_bids();
        bid->set_price(100.0 - i);
        bid->set_quantity(static_cast<uint32_t>(100 + i * 10));
        bid->set_orders(static_cast<uint32_t>(i + 1));

        auto* ask = p.add_asks();
        ask->set_price(101.0 + i);
        ask->set_quantity(static_cast<uint32_t>(200 + i * 10));
        ask->set_orders(static_cast<uint32_t>(i + 2));
    }

    Tick t = proto_to_tick(p);

    for (int i = 0; i < 5; ++i) {
        EXPECT_DOUBLE_EQ(t.bids[i].price,    100.0 - i)   << "bid price at level " << i;
        EXPECT_EQ(t.bids[i].quantity,        static_cast<uint32_t>(100 + i * 10)) << "bid qty at level " << i;
        EXPECT_EQ(t.bids[i].orders,          static_cast<uint32_t>(i + 1))        << "bid orders at level " << i;
        EXPECT_DOUBLE_EQ(t.asks[i].price,    101.0 + i)   << "ask price at level " << i;
        EXPECT_EQ(t.asks[i].quantity,        static_cast<uint32_t>(200 + i * 10)) << "ask qty at level " << i;
        EXPECT_EQ(t.asks[i].orders,          static_cast<uint32_t>(i + 2))        << "ask orders at level " << i;
    }
}

// ── L2 depth — partial fill ───────────────────────────────────────────────────

TEST(ProtoTickMapper, PartialDepthZeroFillsRemainder) {
    // Only 2 levels provided — levels 2-4 must remain zero-initialised
    alpha::feed::Tick p;
    p.add_bids()->set_price(100.0);
    p.add_bids()->set_price(99.5);
    p.add_asks()->set_price(100.5);
    p.add_asks()->set_price(101.0);

    Tick t = proto_to_tick(p);

    EXPECT_DOUBLE_EQ(t.bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(t.bids[1].price, 99.5);
    EXPECT_DOUBLE_EQ(t.bids[2].price, 0.0);   // unfilled — must be zero
    EXPECT_DOUBLE_EQ(t.bids[3].price, 0.0);
    EXPECT_DOUBLE_EQ(t.bids[4].price, 0.0);
    EXPECT_EQ(t.bids[2].quantity,     0u);
}

// ── L2 depth — excess levels silently truncated ───────────────────────────────

TEST(ProtoTickMapper, ExcessDepthTruncatedToFive) {
    // Proto carries 7 levels; C++ struct only holds 5 — must not crash or OOB
    alpha::feed::Tick p;
    for (int i = 0; i < 7; ++i) {
        p.add_bids()->set_price(100.0 - i);
        p.add_asks()->set_price(101.0 + i);
    }

    Tick t = proto_to_tick(p);

    EXPECT_DOUBLE_EQ(t.bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(t.bids[4].price, 96.0);   // level 5 (price=95) dropped
}

// ── Option greeks ─────────────────────────────────────────────────────────────

TEST(ProtoTickMapper, OptionGreeksAllFields) {
    alpha::feed::Tick p;
    p.mutable_greeks()->set_delta(0.65);
    p.mutable_greeks()->set_gamma(0.02);
    p.mutable_greeks()->set_theta(-15.5);
    p.mutable_greeks()->set_vega(8.3);
    p.mutable_greeks()->set_rho(0.5);
    p.mutable_greeks()->set_iv(18.5);

    Tick t = proto_to_tick(p);

    EXPECT_DOUBLE_EQ(t.greeks.delta, 0.65);
    EXPECT_DOUBLE_EQ(t.greeks.gamma, 0.02);
    EXPECT_DOUBLE_EQ(t.greeks.theta, -15.5);
    EXPECT_DOUBLE_EQ(t.greeks.vega,  8.3);
    EXPECT_DOUBLE_EQ(t.greeks.rho,   0.5);
    EXPECT_DOUBLE_EQ(t.greeks.iv,    18.5);
}

// ── Index tick — no depth, no greeks ─────────────────────────────────────────

TEST(ProtoTickMapper, IndexTickNoDepthNoGreeks) {
    alpha::feed::Tick p;
    p.set_token(999);
    p.set_last_price(22500.0);
    p.set_prev_close(22300.0);
    // no bids/asks, no greeks set

    Tick t = proto_to_tick(p);

    EXPECT_EQ(t.instrument_token,    999u);
    EXPECT_DOUBLE_EQ(t.last_price,   22500.0);
    // depth slots must be zero-initialised
    EXPECT_DOUBLE_EQ(t.bids[0].price, 0.0);
    EXPECT_EQ(t.bids[0].quantity,     0u);
    // greeks must be zero-initialised
    EXPECT_DOUBLE_EQ(t.greeks.delta,  0.0);
    EXPECT_DOUBLE_EQ(t.greeks.iv,     0.0);
}

// ── Round-trip via serialise → parse → map ────────────────────────────────────

TEST(ProtoTickMapper, RoundTripSerialiseDeserialise) {
    // Build a fully populated tick, serialise to bytes, parse back, map to struct
    alpha::feed::Tick original;
    original.set_token(77);
    original.set_timestamp_ns(1712000000000000000ULL);
    original.set_last_price(500.25);
    original.set_volume(100000);
    original.mutable_greeks()->set_delta(0.42);
    original.mutable_greeks()->set_iv(22.1);
    auto* b = original.add_bids();
    b->set_price(500.00); b->set_quantity(300);
    auto* a = original.add_asks();
    a->set_price(500.50); a->set_quantity(250);

    // Serialise → deserialise (simulates the Redis wire path)
    std::string bytes;
    ASSERT_TRUE(original.SerializeToString(&bytes));

    alpha::feed::Tick parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));

    Tick t = proto_to_tick(parsed);

    EXPECT_EQ(t.instrument_token,    77u);
    EXPECT_EQ(t.timestamp_ns,        1712000000000000000ULL);
    EXPECT_DOUBLE_EQ(t.last_price,   500.25);
    EXPECT_EQ(t.total_volume,        100000ULL);
    EXPECT_DOUBLE_EQ(t.greeks.delta, 0.42);
    EXPECT_DOUBLE_EQ(t.greeks.iv,    22.1);
    EXPECT_DOUBLE_EQ(t.bids[0].price, 500.00);
    EXPECT_EQ(t.bids[0].quantity,     300u);
    EXPECT_DOUBLE_EQ(t.asks[0].price, 500.50);
    EXPECT_EQ(t.asks[0].quantity,     250u);
}
