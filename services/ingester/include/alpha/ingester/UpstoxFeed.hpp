#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <alpha/models/MarketModels.hpp>
#include <string>
#include <vector>
#include <memory>

namespace alpha::ingester {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

/**
 * @brief Base class for Upstox feeds (Live and Ops).
 * Handles the common SSL/WebSocket handshake and reconnection logic.
 */
class UpstoxFeed : public std::enable_shared_from_this<UpstoxFeed> {
public:
    UpstoxFeed(net::io_context& ioc, ssl::context& ctx);
    virtual ~UpstoxFeed() = default;

    void connect(const std::string& access_token);
    void disconnect();

    virtual void subscribe([[maybe_unused]] const std::vector<std::string>& symbols) {}

protected:
    // Shared SSL/WebSocket boilerplate
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, [[maybe_unused]] tcp::resolver::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void on_read(beast::error_code ec, [[maybe_unused]] std::size_t bytes_transferred);

    // To be implemented by Live/Ops specialized classes
    virtual void handle_message(const std::string& data) = 0;
    virtual std::string get_feed_url() const = 0;

    net::io_context& ioc_;
    ssl::context& ssl_ctx_;
    tcp::resolver resolver_;
    // Fixed: Using net::ssl::stream instead of beast::ssl_stream
    std::unique_ptr<websocket::stream<ssl::stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    std::string access_token_;
    bool is_connected_ = false;
};

} // namespace alpha::ingester
