#include <alpha/backfill/UpstoxHistoricalFeed.hpp>

#include <stdexcept>
#include <sstream>
#include <chrono>
#include <ctime>
#include <cstring>
#include <iostream>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = boost::asio::ssl;
using tcp       = net::ip::tcp;
using json      = nlohmann::json;

namespace alpha {
namespace backfill {

UpstoxHistoricalFeed::UpstoxHistoricalFeed(std::string access_token)
    : access_token_(std::move(access_token))
{}

size_t UpstoxHistoricalFeed::fetch(const HistoricalRequest& req,
                                    uint32_t                 instrument_token,
                                    const std::string&       symbol,
                                    CandleCallback           callback) {
    // Build path: /v2/historical-candle/{instrument_key}/{interval}/{to_date}/{from_date}
    std::string path = std::string(API_VERSION) + "/historical-candle/"
                     + req.instrument_key + "/"
                     + req.interval + "/"
                     + req.to_date + "/"
                     + req.from_date;

    std::string body = do_https_get(path);
    auto candles = parse_response(body);

    for (const auto& c : candles)
        callback(c, instrument_token, symbol);

    return candles.size();
}

std::string UpstoxHistoricalFeed::do_https_get(const std::string& path) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    // SNI hostname
    if (!SSL_set_tlsext_host_name(stream.native_handle(), API_HOST))
        throw std::runtime_error("UpstoxHistoricalFeed: SSL_set_tlsext_host_name failed");

    auto const results = resolver.resolve(API_HOST, "443");
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::empty_body> req{http::verb::get, path, 11};
    req.set(http::field::host, API_HOST);
    req.set(http::field::user_agent, "AlphaHFT/1.0");
    req.set(http::field::accept, "application/json");
    req.set(http::field::authorization, "Bearer " + access_token_);

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    if (res.result() != http::status::ok) {
        throw std::runtime_error(
            "UpstoxHistoricalFeed: HTTP " + std::to_string(res.result_int())
            + " for path: " + path
            + "\nBody: " + res.body().substr(0, 256));
    }

    beast::error_code ec;
    stream.shutdown(ec);
    // Ignore shutdown errors (common with HTTP/1.1 connections)

    return res.body();
}

std::vector<models::Candle> UpstoxHistoricalFeed::parse_response(const std::string& body) {
    // Upstox response:
    // {
    //   "status": "success",
    //   "data": {
    //     "candles": [
    //       ["2024-01-01T09:15:00+05:30", open, high, low, close, volume, oi],
    //       ...
    //     ]
    //   }
    // }

    std::vector<models::Candle> out;

    try {
        auto j = json::parse(body);

        if (j.value("status", "") != "success")
            throw std::runtime_error("UpstoxHistoricalFeed: API returned non-success status");

        const auto& candles_arr = j.at("data").at("candles");
        out.reserve(candles_arr.size());

        for (const auto& row : candles_arr) {
            if (!row.is_array() || row.size() < 6) continue;

            models::Candle c{};
            c.timestamp_ns  = iso8601_to_ns(row[0].get<std::string>());
            c.open          = row[1].get<double>();
            c.high          = row[2].get<double>();
            c.low           = row[3].get<double>();
            c.close         = row[4].get<double>();
            c.volume        = row[5].get<uint64_t>();
            c.open_interest = (row.size() > 6) ? row[6].get<int32_t>() : 0;
            out.push_back(c);
        }

        // Upstox returns newest-first; reverse to chronological order
        std::reverse(out.begin(), out.end());

    } catch (const json::exception& e) {
        throw std::runtime_error(
            std::string("UpstoxHistoricalFeed: JSON parse error: ") + e.what());
    }

    return out;
}

uint64_t UpstoxHistoricalFeed::iso8601_to_ns(const std::string& iso) {
    // Format: "2024-01-01T09:15:00+05:30" or "2024-01-01T09:15:00+0530"
    // We parse as UTC by offsetting for +05:30 manually.
    struct tm t{};
    int tz_h = 5, tz_m = 30;

    // sscanf: "2024-01-01T09:15:00+05:30"
    int parsed = std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d+%d:%d",
        &t.tm_year, &t.tm_mon, &t.tm_mday,
        &t.tm_hour, &t.tm_min, &t.tm_sec,
        &tz_h, &tz_m);

    if (parsed < 6) {
        // Try without timezone
        std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
            &t.tm_year, &t.tm_mon, &t.tm_mday,
            &t.tm_hour, &t.tm_min, &t.tm_sec);
    }

    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    t.tm_isdst = 0;

    // Convert local IST time to UTC epoch
    time_t epoch_s = timegm(&t);
    epoch_s -= (tz_h * 3600 + tz_m * 60);  // subtract IST offset to get UTC

    return static_cast<uint64_t>(epoch_s) * 1'000'000'000ULL;
}

// ── fetch_range (auto-chunked) ────────────────────────────────────────────────

int UpstoxHistoricalFeed::chunk_days_for_interval(const std::string& interval) {
    // Stay under ~1900 bars per request.
    // NSE intraday session = 375 min (09:15–15:30).
    if (interval == "1minute")                       return 4;   // 375×4 = 1 500
    if (interval == "5minute")                       return 24;  //  75×24 = 1 800
    if (interval == "15minute")                      return 75;  //  25×75 = 1 875
    if (interval == "30minute")                      return 150; //  12×150 = 1 800
    if (interval == "60minute" || interval == "1hour") return 300; // 6×300 = 1 800
    return 0;  // day / week / month — single call is fine
}

size_t UpstoxHistoricalFeed::fetch_range(const HistoricalRequest& req,
                                          uint32_t                 instrument_token,
                                          const std::string&       symbol,
                                          CandleCallback           callback,
                                          std::function<void()>    pre_fetch_cb) {
    const int chunk_days = chunk_days_for_interval(req.interval);

    // No chunking needed — single call
    if (chunk_days == 0) {
        if (pre_fetch_cb) pre_fetch_cb();
        return fetch(req, instrument_token, symbol, std::move(callback));
    }

    // Parse from/to dates into time_t (UTC midnight)
    auto parse_date = [](const std::string& s) -> time_t {
        struct tm t{};
        std::sscanf(s.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
        t.tm_year -= 1900;
        t.tm_mon  -= 1;
        return timegm(&t);
    };

    auto format_date = [](time_t t, char* buf, size_t n) {
        struct tm ft;
        gmtime_r(&t, &ft);
        std::strftime(buf, n, "%Y-%m-%d", &ft);
    };

    const time_t t_from = parse_date(req.from_date);
    const time_t t_to   = parse_date(req.to_date);

    size_t  total  = 0;
    time_t  cursor = t_from;

    while (cursor <= t_to) {
        const time_t chunk_end =
            std::min(cursor + static_cast<time_t>(chunk_days - 1) * 86400LL, t_to);

        char from_buf[12], to_buf[12];
        format_date(cursor,    from_buf, sizeof(from_buf));
        format_date(chunk_end, to_buf,   sizeof(to_buf));

        HistoricalRequest chunk_req = req;
        chunk_req.from_date = from_buf;
        chunk_req.to_date   = to_buf;

        if (pre_fetch_cb) pre_fetch_cb();
        total += fetch(chunk_req, instrument_token, symbol, callback);

        cursor = chunk_end + 86400LL;
    }

    return total;
}

} // namespace backfill
} // namespace alpha
