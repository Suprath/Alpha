#include <alpha/ingester/UpstoxLiveFeed.hpp>
#include <alpha/config/Config.hpp>
#include <alpha/time/Timestamp.hpp>
#include <iostream>

namespace alpha::ingester {

using namespace com::upstox::marketdatafeederv3udapi::rpc::proto;

void UpstoxLiveFeed::subscribe(const std::vector<std::string>& symbols) {
    if (symbols.empty()) return;
    if (!is_connected_) {
        // WebSocket not ready yet — queue for when on_connected() fires
        pending_symbols_ = symbols;
        return;
    }

    json sub;
    sub["guid"] = "live-feed-guid";
    sub["method"] = "sub";
    sub["data"]["instrumentKeys"] = symbols;
    sub["data"]["mode"] = "full"; // Get full L1 + Depth

    sub_msg_ = sub.dump();
    std::cout << "[Feed] Subscribing to " << symbols.size() << " instruments" << std::endl;
    ws_->async_write(net::buffer(sub_msg_),
        beast::bind_front_handler(&UpstoxLiveFeed::on_write, shared_from_this()));
}

void UpstoxLiveFeed::on_connected() {
    if (!pending_symbols_.empty()) {
        subscribe(pending_symbols_);
        pending_symbols_.clear();
    }
}

void UpstoxLiveFeed::handle_message(const std::string& data) {
    FeedResponse feed;
    if (!feed.ParseFromString(data)) {
        std::cout << "[Feed] Received non-protobuf message (" << data.size() << " bytes): " << data.substr(0, 120) << std::endl;
        return;
    }

    if (feed.feeds().empty()) {
        if (feed.type() == 2) {
            // market_info message — market status update (normal when market is closed)
            static bool logged_once = false;
            if (!logged_once) {
                std::cout << "[Feed] Market info received (" << feed.marketinfo().segmentstatus_size()
                          << " segments). Market may be closed." << std::endl;
                logged_once = true;
            }
        }
        return;
    }

    auto now = alpha::time::Timestamp::now_ns();

    for (auto const& [key, value] : feed.feeds()) {
        auto it = instrument_map_.find(key);
        if (it == instrument_map_.end()) continue;

        Tick tick {};
        tick.instrument_token = it->second;
        tick.timestamp_ns = now;

        if (value.has_fullfeed()) {
            auto const& ff = value.fullfeed();
            if (ff.has_marketff()) {
                auto const& market_ff = ff.marketff();
                tick.last_price   = market_ff.ltpc().ltp();
                tick.total_volume = static_cast<uint64_t>(market_ff.vtt());
                tick.open_interest = market_ff.oi();

                if (market_ff.has_marketlevel() && market_ff.marketlevel().bidaskquote_size() > 0) {
                    const auto& q0 = market_ff.marketlevel().bidaskquote(0);
                    tick.bid_price = q0.bidp();
                    tick.bid_size  = static_cast<uint32_t>(q0.bidq());
                    tick.ask_price = q0.askp();
                    tick.ask_size  = static_cast<uint32_t>(q0.askq());
                }
                if (market_ff.has_optiongreeks()) {
                    auto const& g = market_ff.optiongreeks();
                    tick.greeks.delta = g.delta();
                    tick.greeks.gamma = g.gamma();
                    tick.greeks.theta = g.theta();
                    tick.greeks.vega  = g.vega();
                    tick.greeks.rho   = g.rho();
                    tick.greeks.iv    = market_ff.iv();
                }
            } else if (ff.has_indexff()) {
                // Index instruments (NIFTY, BANKNIFTY) use IndexFullFeed
                tick.last_price = ff.indexff().ltpc().ltp();
            }
        }

        // Push to Ring Buffer
        if (ring_buffer_) {
            ring_buffer_->push(tick);
        }
    }
}

} // namespace alpha::ingester
