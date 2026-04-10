#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
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

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;

/**
 * @brief Base class for Upstox feeds (Live and Ops).
 *
 * Connection flow (Upstox v3):
 *   1. HTTPS GET api.upstox.com/v3/feed/market-data-feed/authorize  → authorized_redirect_uri
 *   2. WebSocket connect to wsfeeder-api.upstox.com  (URI from step 1, no auth header)
 */
class UpstoxFeed : public std::enable_shared_from_this<UpstoxFeed> {
public:
    UpstoxFeed(net::io_context& ioc, ssl::context& ctx);
    virtual ~UpstoxFeed() = default;

    void connect(const std::string& access_token);
    void disconnect();

    virtual void subscribe([[maybe_unused]] const std::vector<std::string>& symbols) {}

    // Public so the owning service (and tests) can dispatch raw frames directly
    virtual void handle_message(const std::string& data) = 0;

protected:
    // Called once the WebSocket handshake completes — override to auto-subscribe
    virtual void on_connected() {}
    virtual std::string get_feed_url() const = 0;   // unused after v3 auth flow, kept for compat

    // ── Phase 1: HTTP authorize ──────────────────────────────────────────────
    void on_auth_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_auth_connect(beast::error_code ec, tcp::resolver::endpoint_type);
    void on_auth_ssl   (beast::error_code ec);
    void on_auth_write (beast::error_code ec, std::size_t);
    void on_auth_read  (beast::error_code ec, std::size_t);

    // ── Phase 2: WebSocket connect ───────────────────────────────────────────
    void on_ws_resolve  (beast::error_code ec, tcp::resolver::results_type results);
    void on_ws_connect  (beast::error_code ec, tcp::resolver::endpoint_type);
    void on_ws_ssl      (beast::error_code ec);
    void on_ws_handshake(beast::error_code ec);
    void on_write       (beast::error_code ec, std::size_t);
    void on_read        (beast::error_code ec, std::size_t);

    net::io_context& ioc_;
    ssl::context&    ssl_ctx_;
    std::string      access_token_;
    bool             is_connected_ = false;

    // Phase-1 HTTP objects (reused for the authorize REST call)
    tcp::resolver                                         resolver_;
    std::unique_ptr<ssl::stream<beast::tcp_stream>>       auth_stream_;
    beast::flat_buffer                                    auth_buf_;
    http::request<http::empty_body>                       auth_req_;
    http::response<http::string_body>                     auth_res_;

    // Phase-2 WebSocket objects
    std::string                                                     ws_host_;
    std::string                                                     ws_path_;
    std::unique_ptr<websocket::stream<ssl::stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer                                              buffer_;
};

} // namespace alpha::ingester
