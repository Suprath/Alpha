#pragma once

#include <utility>
#include <string>
#include <memory>
#include <vector>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <alpha/models/MarketModels.hpp>
#include <alpha/rate_limit/RateLimiter.hpp>
#include "market_data_v3.pb.h"
#include <unordered_map>
#include <pqxx/pqxx>

#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/ingester/DataIngester.hpp>

namespace alpha::ingester {

using namespace alpha::models;
using namespace alpha::rate_limit;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

/**
 * @brief Manages data ingestion from Upstox V3.
 * Handles OAuth, WebSocket streaming, and historical data retrieval.
 */
class UpstoxIngester : public DataIngester {
public:
    UpstoxIngester(net::io_context& ioc, bool with_db = true);

    /**
     * @brief Perform manual OAuth token exchange.
     */
    bool authenticate(const std::string& auth_code = "") override;

    /**
     * @brief Connect to the Upstox V3 WebSocket feed.
     */
    void connect_feed() override;

    /**
     * @brief Subscribe to full market data for a list of symbols.
     */
    void subscribe(const std::vector<std::string>& symbols) override;

    /**
     * @brief Fetch historical candles for a specific timeframe.
     */
    std::vector<Candle> fetch_historical(const std::string& symbol, 
                                        const std::string& interval,
                                        const std::string& from, 
                                        const std::string& to) override;

    // Testing Interface
    void inject_manual_instrument_for_testing(const std::string& key, uint32_t token);
    void on_message(const std::string& data) override;

private:
    void save_token(const std::string& token);
    std::string load_token();
    void schedule_heartbeat();
    void load_instruments_from_db();

    // Async WebSocket handlers
    void on_authorize(beast::error_code ec, http::response<http::string_body> res);
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);

    net::io_context& ioc_;
    ssl::context ssl_ctx_;
    tcp::resolver resolver_;
    std::string access_token_;
    std::string authorized_url_;
    std::vector<std::string> pending_subscriptions_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    std::unique_ptr<RateLimiter> rest_limiter_;
    
    // DB Context
    std::unordered_map<std::string, uint32_t> instrument_map_;

    // IPC Memory Management
    std::unique_ptr<alpha::ipc::ShmManager> shm_manager_;
    alpha::ipc::SPSCRingBuffer<Tick, 65536>* ring_buffer_{nullptr};
    net::steady_timer heartbeat_timer_;
};

} // namespace alpha::ingester
