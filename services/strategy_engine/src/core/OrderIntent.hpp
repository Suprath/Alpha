#pragma once

#include <cstdint>

namespace alpha::strategy {

enum class OrderSide : int32_t { BUY = 1, SELL = -1 };

enum class OrderReason : int32_t {
    ENTRY     = 1,  // Opening a new position
    EXIT      = 2,  // Closing an existing position
    SCALE_IN  = 3,  // Adding to an existing position in the same direction
    SCALE_OUT = 4,  // Partially reducing a position
    REVERSE   = 5,  // Flipping direction (close + open opposite)
};

/**
 * @brief Trading decision emitted by the StrategyEngine.
 * POD struct — ready for future shared memory publishing.
 */
struct OrderIntent {
    uint64_t    timestamp_ns;
    uint32_t    instrument_token;
    char        symbol[32];
    double      price;        // Reference price at signal time
    int32_t     qty;          // Absolute units to trade
    OrderSide   side;         // BUY or SELL
    OrderReason reason;
    double      confidence;   // Kelly fraction from signal
    uint32_t    strategy_id;
};

} // namespace alpha::strategy
