#include <alpha/ingester/UpstoxFeed.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <iostream>
#include <regex>

namespace alpha::ingester {

using json = nlohmann::json;

UpstoxFeed::UpstoxFeed(net::io_context& ioc, ssl::context& ctx)
    : ioc_(ioc), ssl_ctx_(ctx), resolver_(ioc) {}

// ── Public API ───────────────────────────────────────────────────────────────

void UpstoxFeed::connect(const std::string& access_token) {
    access_token_ = access_token;
    // Phase 1: resolve api.upstox.com to call the authorize endpoint
    resolver_.async_resolve("api.upstox.com", "443",
        beast::bind_front_handler(&UpstoxFeed::on_auth_resolve, shared_from_this()));
}

void UpstoxFeed::disconnect() {
    if (ws_) ws_->async_close(websocket::close_code::normal, [](beast::error_code) {});
    is_connected_ = false;
}

// ── Phase 1: HTTPS authorize ─────────────────────────────────────────────────

void UpstoxFeed::on_auth_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) { std::cerr << "[Feed] Auth DNS resolve: " << ec.message() << std::endl; return; }
    auth_stream_ = std::make_unique<ssl::stream<beast::tcp_stream>>(ioc_, ssl_ctx_);
    if (!SSL_set_tlsext_host_name(auth_stream_->native_handle(), "api.upstox.com")) {
        std::cerr << "[Feed] Auth SNI failed" << std::endl; return;
    }
    beast::get_lowest_layer(*auth_stream_).async_connect(results,
        beast::bind_front_handler(&UpstoxFeed::on_auth_connect, shared_from_this()));
}

void UpstoxFeed::on_auth_connect(beast::error_code ec, tcp::resolver::endpoint_type) {
    if (ec) { std::cerr << "[Feed] Auth TCP connect: " << ec.message() << std::endl; return; }
    auth_stream_->async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&UpstoxFeed::on_auth_ssl, shared_from_this()));
}

void UpstoxFeed::on_auth_ssl(beast::error_code ec) {
    if (ec) { std::cerr << "[Feed] Auth SSL: " << ec.message() << std::endl; return; }
    auth_req_ = {};
    auth_req_.method(http::verb::get);
    auth_req_.target("/v3/feed/market-data-feed/authorize");
    auth_req_.version(11);
    auth_req_.set(http::field::host, "api.upstox.com");
    auth_req_.set(http::field::authorization, "Bearer " + access_token_);
    auth_req_.set("Api-Version", "2.0");
    auth_req_.set(http::field::accept, "application/json");
    http::async_write(*auth_stream_, auth_req_,
        beast::bind_front_handler(&UpstoxFeed::on_auth_write, shared_from_this()));
}

void UpstoxFeed::on_auth_write(beast::error_code ec, std::size_t) {
    if (ec) { std::cerr << "[Feed] Auth write: " << ec.message() << std::endl; return; }
    auth_res_ = {};
    http::async_read(*auth_stream_, auth_buf_, auth_res_,
        beast::bind_front_handler(&UpstoxFeed::on_auth_read, shared_from_this()));
}

void UpstoxFeed::on_auth_read(beast::error_code ec, std::size_t) {
    if (ec) { std::cerr << "[Feed] Auth read: " << ec.message() << std::endl; return; }
    if (auth_res_.result() != http::status::ok) {
        std::cerr << "[Feed] Auth HTTP " << auth_res_.result_int()
                  << ": " << auth_res_.body() << std::endl;
        return;
    }
    // Parse authorized_redirect_uri from JSON
    std::string ws_uri;
    try {
        auto j = json::parse(auth_res_.body());
        ws_uri = j["data"]["authorized_redirect_uri"].get<std::string>();
    } catch (...) {
        std::cerr << "[Feed] Auth JSON parse failed: " << auth_res_.body() << std::endl;
        return;
    }
    // Parse: wss://wsfeeder-api.upstox.com/market-data-feeder/...?...
    // Extract host and path from the wss:// URI
    static const std::regex uri_re(R"(wss://([^/]+)(/.+))");
    std::smatch m;
    if (!std::regex_match(ws_uri, m, uri_re)) {
        std::cerr << "[Feed] Cannot parse WS URI: " << ws_uri << std::endl; return;
    }
    ws_host_ = m[1].str();
    ws_path_ = m[2].str();
    std::cout << "[Feed] Auth OK → connecting to " << ws_host_ << ws_path_.substr(0, 60) << "..." << std::endl;
    auth_stream_.reset(); // release the auth connection
    // Phase 2: resolve the WS host
    resolver_.async_resolve(ws_host_, "443",
        beast::bind_front_handler(&UpstoxFeed::on_ws_resolve, shared_from_this()));
}

// ── Phase 2: WebSocket connect ────────────────────────────────────────────────

void UpstoxFeed::on_ws_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) { std::cerr << "[Feed] WS DNS resolve: " << ec.message() << std::endl; return; }
    ws_ = std::make_unique<websocket::stream<ssl::stream<beast::tcp_stream>>>(ioc_, ssl_ctx_);
    // Set SNI on the SSL layer inside the WebSocket stream
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), ws_host_.c_str())) {
        std::cerr << "[Feed] WS SNI failed" << std::endl; return;
    }
    beast::get_lowest_layer(*ws_).async_connect(results,
        beast::bind_front_handler(&UpstoxFeed::on_ws_connect, shared_from_this()));
}

void UpstoxFeed::on_ws_connect(beast::error_code ec, tcp::resolver::endpoint_type) {
    if (ec) { std::cerr << "[Feed] WS TCP connect: " << ec.message() << std::endl; return; }
    ws_->next_layer().async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&UpstoxFeed::on_ws_ssl, shared_from_this()));
}

void UpstoxFeed::on_ws_ssl(beast::error_code ec) {
    if (ec) { std::cerr << "[Feed] WS SSL: " << ec.message() << std::endl; return; }
    // No Authorization header needed — auth token is embedded in ws_path_ query string
    ws_->async_handshake(ws_host_, ws_path_,
        beast::bind_front_handler(&UpstoxFeed::on_ws_handshake, shared_from_this()));
}

void UpstoxFeed::on_ws_handshake(beast::error_code ec) {
    if (ec) { std::cerr << "[Feed] WS handshake: " << ec.message() << std::endl; return; }
    is_connected_ = true;
    std::cout << "[Feed] WebSocket connected to " << ws_host_ << std::endl;
    on_connected();
    ws_->async_read(buffer_, beast::bind_front_handler(&UpstoxFeed::on_read, shared_from_this()));
}

void UpstoxFeed::on_read(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        std::cerr << "[Feed] Read error: " << ec.message() << std::endl;
        is_connected_ = false;
        return;
    }
    std::string data = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    handle_message(data);
    ws_->async_read(buffer_, beast::bind_front_handler(&UpstoxFeed::on_read, shared_from_this()));
}

void UpstoxFeed::on_write(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        std::cerr << "[Feed] Write error: " << ec.message() << std::endl;
        is_connected_ = false;
    }
}

} // namespace alpha::ingester
