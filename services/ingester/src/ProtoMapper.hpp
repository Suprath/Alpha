#pragma once

/**
 * ProtoMapper — alpha::feed::Tick proto → alpha::models::Tick struct
 * ─────────────────────────────────────────────────────────────────────────────
 * Single responsibility: map the transport representation (protobuf) to the
 * hot-path in-memory representation (POD struct for SHM ring buffer).
 *
 * Kept as an inline header so it can be included by both main.cpp and the
 * unit test binary without needing a separate translation unit or CMake target.
 */

#include "alpha_tick.pb.h"
#include <alpha/models/MarketModels.hpp>
#include <algorithm>

namespace alpha::ingester {

inline alpha::models::Tick proto_to_tick(const alpha::feed::Tick& p) {
    using namespace alpha::models;

    Tick t{};
    t.timestamp_ns     = p.timestamp_ns();
    t.instrument_token = p.token();
    t.last_price       = p.last_price();
    t.total_volume     = p.volume();
    t.open_interest    = p.open_interest();
    t.bid_price        = p.bid_price();
    t.bid_size         = p.bid_size();
    t.ask_price        = p.ask_price();
    t.ask_size         = p.ask_size();

    const int bid_depth = std::min(5, p.bids_size());
    for (int i = 0; i < bid_depth; ++i) {
        t.bids[i].price    = p.bids(i).price();
        t.bids[i].quantity = p.bids(i).quantity();
        t.bids[i].orders   = p.bids(i).orders();
    }
    const int ask_depth = std::min(5, p.asks_size());
    for (int i = 0; i < ask_depth; ++i) {
        t.asks[i].price    = p.asks(i).price();
        t.asks[i].quantity = p.asks(i).quantity();
        t.asks[i].orders   = p.asks(i).orders();
    }

    // greeks() always returns a valid (possibly zero-filled) message
    t.greeks.delta = p.greeks().delta();
    t.greeks.gamma = p.greeks().gamma();
    t.greeks.theta = p.greeks().theta();
    t.greeks.vega  = p.greeks().vega();
    t.greeks.rho   = p.greeks().rho();
    t.greeks.iv    = p.greeks().iv();

    return t;
}

} // namespace alpha::ingester
