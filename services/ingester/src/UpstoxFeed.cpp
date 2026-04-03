#include <alpha/ingester/UpstoxFeed.hpp>
#include <iostream>

namespace alpha::ingester {

UpstoxFeed::UpstoxFeed(net::io_context& ioc, ssl::context& ctx)
    : ioc_(ioc), ssl_ctx_(ctx), resolver_(ioc) {}

void UpstoxFeed::connect(const std::string& access_token) {
    access_token_ = access_token;
    resolver_.async_resolve("api.upstox.com", "443", 
        beast::bind_front_handler(&UpstoxFeed::on_resolve, shared_from_this()));
}

void UpstoxFeed::disconnect() {
    if (ws_) ws_->async_close(websocket::close_code::normal, [](beast::error_code) {});
    is_connected_ = false;
}

void UpstoxFeed::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) return;
    ws_ = std::make_unique<websocket::stream<ssl::stream<beast::tcp_stream>>>(ioc_, ssl_ctx_);
    beast::get_lowest_layer(*ws_).async_connect(results,
        beast::bind_front_handler(&UpstoxFeed::on_connect, shared_from_this()));
}

void UpstoxFeed::on_connect(beast::error_code ec, [[maybe_unused]] tcp::resolver::endpoint_type ep) {
    if (ec) return;
    ws_->next_layer().async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&UpstoxFeed::on_ssl_handshake, shared_from_this()));
}

void UpstoxFeed::on_ssl_handshake(beast::error_code ec) {
    if (ec) return;
    ws_->async_handshake("api.upstox.com", get_feed_url() + "?access_token=" + access_token_,
        beast::bind_front_handler(&UpstoxFeed::on_handshake, shared_from_this()));
}

void UpstoxFeed::on_handshake(beast::error_code ec) {
    if (ec) return;
    is_connected_ = true;
    ws_->async_read(buffer_, beast::bind_front_handler(&UpstoxFeed::on_read, shared_from_this()));
}

void UpstoxFeed::on_read(beast::error_code ec, [[maybe_unused]] std::size_t bytes_transferred) {
    if (ec) {
        is_connected_ = false;
        return;
    }
    std::string data = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    handle_message(data);
    ws_->async_read(buffer_, beast::bind_front_handler(&UpstoxFeed::on_read, shared_from_this()));
}

void UpstoxFeed::on_write(beast::error_code ec, std::size_t) {
    if (ec) is_connected_ = false;
}

} // namespace alpha::ingester
