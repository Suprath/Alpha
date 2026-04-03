#pragma once

#include <alpha/ingester/UpstoxFeed.hpp>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>
#include <nlohmann/json.hpp>
#include "market_data_v3.pb.h"

namespace alpha::ingester {

using namespace alpha::models;
using json = nlohmann::json;

/**
 * @brief Handles low-latency market data ingestion.
 * Pushes ticks directly to Shared Memory for the Signal Engine.
 */
class UpstoxLiveFeed : public UpstoxFeed {
public:
    UpstoxLiveFeed(net::io_context& ioc, ssl::context& ssl_ctx, 
                  alpha::ipc::SPSCRingBuffer<Tick, 65536>* ring_buffer,
                  const std::unordered_map<std::string, uint32_t>& instrument_map)
        : UpstoxFeed(ioc, ssl_ctx), 
          ring_buffer_(ring_buffer), 
          instrument_map_(instrument_map) {}

    void subscribe(const std::vector<std::string>& symbols) override;

protected:
    void handle_message(const std::string& data) override;
    std::string get_feed_url() const override { return "/v3/market_data/feed"; }

private:
    alpha::ipc::SPSCRingBuffer<Tick, 65536>* ring_buffer_;
    const std::unordered_map<std::string, uint32_t>& instrument_map_;
};

} // namespace alpha::ingester
