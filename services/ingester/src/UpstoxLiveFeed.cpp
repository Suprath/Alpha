#include <alpha/ingester/UpstoxLiveFeed.hpp>
#include <alpha/config/Config.hpp>
#include <alpha/time/Timestamp.hpp>
#include <iostream>

namespace alpha::ingester {

void UpstoxLiveFeed::subscribe(const std::vector<std::string>& symbols) {
    if (!is_connected_) return;
    
    json sub;
    sub["guid"] = "live-feed-guid";
    sub["method"] = "sub";
    sub["data"]["instrumentKeys"] = symbols;
    sub["data"]["mode"] = "full"; // Get full L1 + Depth

    auto msg = sub.dump();
    ws_->async_write(net::buffer(msg), 
        beast::bind_front_handler(&UpstoxLiveFeed::on_write, shared_from_this()));
}

void UpstoxLiveFeed::handle_message(const std::string& data) {
    Upstox::MarketDataFeed feed;
    if (!feed.ParseFromString(data)) return;

    auto now = alpha::time::Timestamp::now_ns();

    for (auto const& [key, value] : feed.feeds()) {
        auto it = instrument_map_.find(key);
        if (it == instrument_map_.end()) continue;

        Tick tick {};
        tick.instrument_token = it->second;
        tick.timestamp_ns = now;
        
        if (value.has_ff()) {
            auto const& market_ff = value.ff().marketff();
            tick.last_price = market_ff.ltp();
            tick.total_volume = market_ff.vtt();
            tick.open_interest = market_ff.oi();
            
            // L1 Bid/Ask
            if (market_ff.bids_size() > 0) {
                tick.bid_price = market_ff.bids(0).price();
                tick.bid_size = market_ff.bids(0).quantity();
            }
            if (market_ff.asks_size() > 0) {
                tick.ask_price = market_ff.asks(0).price();
                tick.ask_size = market_ff.asks(0).quantity();
            }

            // Greeks (If available in full feed)
            if (market_ff.has_optiongreeks()) {
                auto const& g = market_ff.optiongreeks();
                tick.greeks.delta = g.delta();
                tick.greeks.gamma = g.gamma();
                tick.greeks.theta = g.theta();
                tick.greeks.vega = g.vega();
                tick.greeks.rho = g.rho();
                tick.greeks.iv = market_ff.iv();
            }
        }

        // Push to Ring Buffer
        if (ring_buffer_) {
            ring_buffer_->push(tick);
        }
    }
}

} // namespace alpha::ingester
