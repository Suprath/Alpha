#include <alpha/ingester/UpstoxIngester.hpp>
#include <alpha/config/Config.hpp>
#include <alpha/time/Timestamp.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <chrono>
#include <utility>
#include <iomanip>
#include <sstream>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/core.hpp>
#include <pqxx/pqxx>

namespace alpha::ingester {

using json = nlohmann::json;
namespace http = boost::beast::http;
namespace beast = boost::beast;
using tcp = boost::asio::ip::tcp;

/**
 * @brief Helper to convert Upstox ISO8601 timestamp to nanoseconds.
 */
uint64_t parse_upstox_ts(const std::string& ts_str) {
    if (ts_str.length() < 19) return 0;
    std::tm tm = {};
    std::istringstream ss(ts_str.substr(0, 19));
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;
    // Standard IST to UTC/Epoch logic
    auto tp = std::chrono::system_clock::from_time_t(timegm(&tm));
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

UpstoxIngester::UpstoxIngester(net::io_context& ioc, bool with_db) 
    : ioc_(ioc), ssl_ctx_(ssl::context::tls_client), heartbeat_timer_(ioc), backfill_timer_(ioc) {
    
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_none);

    access_token_ = load_token();

    shm_manager_ = std::make_unique<alpha::ipc::ShmManager>("alpha_upstox_shm_v2", alpha::ipc::ShmRole::PRODUCER);
    ring_buffer_ = shm_manager_->get_or_create_buffer<alpha::ipc::SPSCRingBuffer<Tick, 65536>>("tick_queue");

    if (with_db) {
        load_instruments_from_db();
    }

    std::string qdb_host = alpha::config::Config::get().get_string("QUESTDB_HOST", "questdb");
    int qdb_port = alpha::config::Config::get().get_int("QUESTDB_ILP_PORT", 9009);

    live_feed_ = std::make_shared<UpstoxLiveFeed>(ioc_, ssl_ctx_, ring_buffer_, instrument_map_);
    ops_feed_ = std::make_shared<UpstoxOpsFeed>(ioc_, ssl_ctx_, qdb_host, qdb_port, token_to_symbol_map_);

    backfill_limiter_ = std::make_unique<RateLimiter>(8.0, 2.0);

    schedule_heartbeat();
    process_backfill_queue();
}

bool UpstoxIngester::authenticate(const std::string& code) {
    if (!code.empty()) {
        access_token_ = code;
        save_token(access_token_);
    }
    return !access_token_.empty();
}

void UpstoxIngester::connect_feed() {
    if (access_token_.empty()) return;
    live_feed_->connect(access_token_);
    ops_feed_->connect(access_token_);
}

void UpstoxIngester::subscribe(const std::vector<std::string>& symbols) {
    live_feed_->subscribe(symbols);
}

std::vector<Candle> UpstoxIngester::fetch_historical(const std::string& symbol, const std::string& interval, const std::string& from, const std::string& to) {
    BackfillTask task {symbol, interval, from, to};
    backfill_queue_.push(task);
    return {}; 
}

void UpstoxIngester::on_message([[maybe_unused]] const std::string& data) {
    // Manual injection point for testing
}

void UpstoxIngester::process_backfill_queue() {
    if (backfill_queue_.empty()) {
        backfill_timer_.expires_after(std::chrono::seconds(1));
        backfill_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) process_backfill_queue();
        });
        return;
    }

    backfill_limiter_->acquire();
    BackfillTask task = backfill_queue_.front();
    backfill_queue_.pop();

    fetch_historical_rest(task);

    backfill_timer_.expires_after(std::chrono::milliseconds(200));
    backfill_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) process_backfill_queue();
    });
}

void UpstoxIngester::fetch_historical_rest(const BackfillTask& task) {
    auto target = "/v2/historical-candle/" + task.symbol + "/" + task.interval + "/" + task.to + "/" + task.from;
    
    struct RestSession : public std::enable_shared_from_this<RestSession> {
        net::io_context& ioc;
        ssl::context& ssl_ctx;
        tcp::resolver resolver;
        ssl::stream<beast::tcp_stream> stream;
        http::request<http::string_body> req;
        http::response<http::string_body> res;
        std::string symbol;
        std::string interval;
        std::shared_ptr<UpstoxOpsFeed> ops_feed;
        beast::flat_buffer buffer;

        RestSession(net::io_context& i, ssl::context& s, const std::string& t, 
                   const std::string& tok, const std::string& sym, 
                   const std::string& inter, std::shared_ptr<UpstoxOpsFeed> ops)
            : ioc(i), ssl_ctx(s), resolver(i), stream(i, s), 
              symbol(sym), interval(inter), ops_feed(ops) {
            req.version(11);
            req.method(http::verb::get);
            req.target(t);
            req.set(http::field::host, "api.upstox.com");
            req.set(http::field::authorization, "Bearer " + tok);
            req.set(http::field::accept, "application/json");
        }

        void run() {
            resolver.async_resolve("api.upstox.com", "443", 
                beast::bind_front_handler(&RestSession::on_resolve, shared_from_this()));
        }

        void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) return;
            beast::get_lowest_layer(stream).async_connect(results,
                beast::bind_front_handler(&RestSession::on_connect, shared_from_this()));
        }

        void on_connect(beast::error_code ec, tcp::resolver::endpoint_type) {
            if (ec) return;
            stream.async_handshake(ssl::stream_base::client,
                beast::bind_front_handler(&RestSession::on_handshake, shared_from_this()));
        }

        void on_handshake(beast::error_code ec) {
            if (ec) return;
            http::async_write(stream, req, beast::bind_front_handler(&RestSession::on_write, shared_from_this()));
        }

        void on_write(beast::error_code ec, std::size_t) {
            if (ec) return;
            http::async_read(stream, buffer, res, beast::bind_front_handler(&RestSession::on_read, shared_from_this()));
        }

        void on_read(beast::error_code ec, std::size_t) {
            if (ec) return;
            if (res.result() == http::status::ok) {
                try {
                    auto j = json::parse(res.body());
                    if (j["status"] == "success") {
                        auto candles_data = j["data"]["candles"];
                        for (auto const& item : candles_data) {
                            Candle c;
                            c.timestamp_ns = parse_upstox_ts(item[0].get<std::string>());
                            c.open = item[1].get<double>();
                            c.high = item[2].get<double>();
                            c.low = item[3].get<double>();
                            c.close = item[4].get<double>();
                            c.volume = item[5].get<uint64_t>();
                            if (item.size() > 6) {
                                c.open_interest = item[6].get<double>();
                            }
                            ops_feed->persist_candle(c, symbol, interval);
                        }
                    }
                } catch (...) {}
            }
        }
    };

    auto session = std::make_shared<RestSession>(ioc_, ssl_ctx_, target, access_token_, task.symbol, task.interval, ops_feed_);
    session->run();
}

void UpstoxIngester::load_instruments_from_db() {
    auto& cfg = alpha::config::Config::get();
    std::string conn_str = "host=" + cfg.get_string("POSTGRES_HOST", "postgres-db") + 
                          " port=" + cfg.get_string("POSTGRES_PORT", "5432") + 
                          " dbname=" + cfg.get_string("POSTGRES_DB", "alpha_db") + 
                          " user=" + cfg.get_string("POSTGRES_USER", "alpha_user") + 
                          " password=" + cfg.get_string("POSTGRES_PASSWORD", "alpha_password");
    try {
        pqxx::connection c(conn_str);
        pqxx::work w(c);
        pqxx::result r = w.exec("SELECT instrument_key, id FROM instrument_universe");
        for (auto const& row : r) {
            std::string key = row["instrument_key"].as<std::string>();
            uint32_t id = row["id"].as<uint32_t>();
            instrument_map_[key] = id;
            token_to_symbol_map_[id] = key;
        }
    } catch (...) {}
}

void UpstoxIngester::save_token(const std::string& token) {
    std::ofstream file("/app/secrets/.upstox_token.json");
    json j; j["access_token"] = token; j["timestamp"] = alpha::time::Timestamp::now_ns();
    file << j.dump(4);
}

std::string UpstoxIngester::load_token() {
    std::ifstream file("/app/secrets/.upstox_token.json");
    if (!file.is_open()) return "";
    try { json j; file >> j; return j["access_token"]; } catch (...) { return ""; }
}

void UpstoxIngester::schedule_heartbeat() {
    heartbeat_timer_.expires_after(std::chrono::milliseconds(100));
    heartbeat_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec && ring_buffer_) {
            ring_buffer_->heartbeat_ts_ns.store(alpha::time::Timestamp::now_ns(), std::memory_order_relaxed);
            schedule_heartbeat();
        }
    });
}

} // namespace alpha::ingester
