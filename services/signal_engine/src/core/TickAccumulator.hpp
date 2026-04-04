#pragma once

#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>
#include <unordered_map>
#include <functional>
#include <iostream>

namespace alpha::signal::core {

/**
 * @brief Thread-safe (if called sequentially) accumulator for resampling Ticks into OHLCV Candles.
 * Aligned to 1-minute clock intervals using alpha::time::Timestamp.
 */
class TickAccumulator {
public:
    using BarCallback = std::function<void(const alpha::models::Candle&)>;

    TickAccumulator(BarCallback callback) : callback_(callback) {}

    /**
     * @brief Process a new tick and update the current minute bar.
     * Triggers the callback if a new minute has started.
     */
    void process_tick(const alpha::models::Tick& tick) {
        uint64_t ts_ist = tick.timestamp_ns + alpha::time::Timestamp::IST_OFFSET_NS;
        uint64_t minute_ts_ns = (ts_ist / 60000000000ULL) * 60000000000ULL;
        
        auto& bar = active_bars_[tick.instrument_token];

        // If this is a new minute, close the previous bar
        if (bar.timestamp_ns != 0 && minute_ts_ns > bar.timestamp_ns) {
            callback_(bar);
            // Reset for new minute
            bar = {}; 
        }

        if (bar.timestamp_ns == 0) {
            // New Bar Initialization
            bar.timestamp_ns = minute_ts_ns;
            bar.open = tick.last_price;
            bar.high = tick.last_price;
            bar.low = tick.last_price;
            bar.close = tick.last_price;
            bar.volume = tick.last_quantity;
            bar.open_interest = tick.open_interest;
        } else {
            // Bar Update
            bar.high = std::max(bar.high, tick.last_price);
            bar.low = std::min(bar.low, tick.last_price);
            bar.close = tick.last_price;
            bar.volume += tick.last_quantity;
            bar.open_interest = tick.open_interest;
        }
    }

private:
    BarCallback callback_;
    // token -> current active candle
    std::unordered_map<uint32_t, alpha::models::Candle> active_bars_;
};

} // namespace alpha::signal::core
