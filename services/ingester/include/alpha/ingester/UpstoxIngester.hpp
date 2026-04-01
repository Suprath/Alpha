#pragma once

#include <string>
#include <memory>
#include <vector>
#include <utility>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <alpha/models/MarketModels.hpp>
#include <alpha/rate_limit/RateLimiter.hpp>
#include "market_data_v3.pb.h"

namespace alpha::ingester {

using namespace alpha::models;
using namespace alpha::rate_limit;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

/**
 * @brief Manages data ingestion from Upstox V3.
 * Handles OAuth, WebSocket streaming, and historical data retrieval.
 */
class UpstoxIngester {
public:
    UpstoxIngester(net::io_context& ioc);

    /**
     * @brief Perform manual OAuth token exchange.
     */
    bool authenticate(const std::string& auth_code);

    /**
     * @brief Connect to the Upstox V3 WebSocket feed.
     */
    void connect_feed();

    /**
     * @brief Subscribe to full market data for a list of symbols.
     */
    void subscribe(const std::vector<std::string>& symbols);

    /**
     * @brief Fetch historical candles for a specific timeframe.
     */
    std::vector<Candle> fetch_historical(const std::string& symbol, 
                                        const std::string& interval,
                                        const std::string& from, 
                                        const std::string& to);

private:
    void on_message(const std::string& data);
    void save_token(const std::string& token);
    std::string load_token();

    net::io_context& ioc_;
    std::string access_token_;
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws_;
    std::unique_ptr<RateLimiter> rest_limiter_;
};

} // namespace alpha::ingester
