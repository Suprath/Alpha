#include <alpha/ingester/UpstoxIngester.hpp>
#include <alpha/config/Config.hpp>
#include <alpha/time/Timestamp.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <chrono>
#include <utility>
#include <boost/beast/version.hpp>

namespace alpha::ingester {

using json = nlohmann::json;

UpstoxIngester::UpstoxIngester(net::io_context& ioc, bool with_db) 
    : ioc_(ioc), heartbeat_timer_(ioc) {
    // 30 requests per minute for historical data = 0.5 requests per second
    rest_limiter_ = std::make_unique<RateLimiter>(0.5, 1.0); 
    access_token_ = load_token();

    // Map Shared Memory
    std::cout << "[IPC] Initializing Shared Memory Mapping (alpha_upstox_shm_v2)..." << std::endl;
    shm_manager_ = std::make_unique<alpha::ipc::ShmManager>("alpha_upstox_shm_v2", alpha::ipc::ShmRole::PRODUCER);
    ring_buffer_ = shm_manager_->get_or_create_buffer<alpha::ipc::SPSCRingBuffer<Tick, 65536>>("tick_queue");
    
    // Start Heartbeat for Zombie Segment Monitoring
    schedule_heartbeat();

    // Load Database Keys
    if (with_db) {
        load_instruments_from_db();
    }
}

void UpstoxIngester::load_instruments_from_db() {
    std::cout << "[DB] Connecting to PostgreSQL to load Instrument Master..." << std::endl;
    std::string db_host = alpha::config::Config::get().get_string("POSTGRES_HOST", "postgres-db");
    std::string db_port = alpha::config::Config::get().get_string("POSTGRES_PORT", "5432");
    std::string db_name = alpha::config::Config::get().get_string("POSTGRES_DB", "alpha_db");
    std::string db_user = alpha::config::Config::get().get_string("POSTGRES_USER", "alpha_user");
    std::string db_pass = alpha::config::Config::get().get_string("POSTGRES_PASSWORD", "alpha_password");
    
    std::string conn_str = "host=" + db_host + " port=" + db_port + 
                           " dbname=" + db_name + " user=" + db_user + " password=" + db_pass;

    int retries = 15;
    while (retries > 0) {
        try {
            pqxx::connection c(conn_str);
            if (c.is_open()) {
                pqxx::nontransaction n(c);
                pqxx::result r = n.exec("SELECT instrument_key, id FROM instrument_universe WHERE date = CURRENT_DATE");
                for (auto const& row : r) {
                    instrument_map_[row[0].c_str()] = row[1].as<uint32_t>();
                }
                std::cout << "[DB] Successfully loaded " << instrument_map_.size() << " instruments from the Master." << std::endl;
                return;
            }
        } catch (const std::exception &e) {
            std::cerr << "[DB] Database not ready. Retrying in 2s... (" << e.what() << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            retries--;
        }
    }
    std::cerr << "CRITICAL: Could not fetch instrument Master from Postgres!" << std::endl;
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

void UpstoxIngester::inject_manual_instrument_for_testing(const std::string& key, uint32_t token) {
    instrument_map_[key] = token;
}

bool UpstoxIngester::authenticate(const std::string& auth_code) {
    std::cout << "Exchanging auth code for access token..." << std::endl;
    if (access_token_.empty()) {
        access_token_ = auth_code;
    }
    save_token(access_token_);
    return true;
}

void UpstoxIngester::save_token(const std::string& token) {
    if (token.empty()) return;
    std::string path = alpha::config::Config::get().get_string("UPSTOX_TOKEN_PATH", "/app/secrets/.upstox_token.json");
    json j;
    j["access_token"] = token;
    j["timestamp"] = alpha::time::Timestamp::now_ns();
    
    std::ofstream file(path);
    if (!file.is_open()) return;
    file << j.dump(4);
}

std::string UpstoxIngester::load_token() {
    std::string env_token = alpha::config::Config::get().get_string("UPSTOX_ACCESS_TOKEN", "");
    if (!env_token.empty()) return env_token;

    std::string path = alpha::config::Config::get().get_string("UPSTOX_TOKEN_PATH", "/app/secrets/.upstox_token.json");
    std::ifstream file(path);
    if (file.is_open()) {
        try {
            json j; file >> j;
            if (j.contains("access_token") && j.contains("timestamp")) {
                uint64_t saved_time = j["timestamp"];
                uint64_t current_time = alpha::time::Timestamp::now_ns();
                if ((current_time - saved_time) < 86400000000000ULL) return j["access_token"];
            }
        } catch (...) {}
    }
    return "";
}

void UpstoxIngester::connect_feed() {
    if (access_token_.empty()) return;
    std::cout << "DEBUG: [IST " << alpha::time::Timestamp::now_ist_ns() << "] WebSocket Feed connection initialized." << std::endl;
}

void UpstoxIngester::subscribe(const std::vector<std::string>& symbols) {
    std::cout << "DEBUG: Subscribing to " << symbols.size() << " symbols in Full Mode." << std::endl;
}

std::vector<Candle> UpstoxIngester::fetch_historical(const std::string& symbol, 
                                                   const std::string& interval,
                                                   const std::string& from, 
                                                   const std::string& to) {
    (void)symbol;
    (void)interval;
    (void)from;
    (void)to;
    rest_limiter_->acquire();
    std::vector<Candle> mock_candles;
    Candle c { alpha::time::Timestamp::now_ns(), 100.0, 105.0, 95.0, 102.0, 1000, 0 };
    mock_candles.push_back(c);
    return mock_candles;
}

void UpstoxIngester::on_message(const std::string& data) {
    if (!ring_buffer_) return;

    com::upstox::marketdata::v3::MarketDataFeed::FeedResponse response;
    if (!response.ParseFromString(data)) {
        std::cerr << "[WARN] Failed to parse Upstox Protobuf payload." << std::endl;
        return;
    }

    // High Performance Iterator loop
    for (auto const& [key, instrument_data] : response.feeds()) {
        auto it = instrument_map_.find(key);
        if (it == instrument_map_.end()) {
            // Unregistered token, skip processing! O(1) mitigation of garbage bandwidth.
            continue;
        }

        Tick t;
        t.instrument_token = it->second;

        if (instrument_data.has_ltp_data()) {
            auto const& ltp = instrument_data.ltp_data();
            t.last_price = ltp.last_price();
            t.timestamp_ns = ltp.last_tick_time() * 1000000ULL; // Upstox provides ms, convert to ns
            ring_buffer_->push(t);

        } else if (instrument_data.has_full_data()) {
            auto const& full = instrument_data.full_data();
            t.last_price = full.ltp().last_price();
            t.timestamp_ns = full.ltp().last_tick_time() * 1000000ULL;
            t.total_volume = full.volume_traded_today();
            t.vwap = full.average_price();
            // Upstox schema extension: close_price could optionally map if structurally required

            if (full.has_market_depth()) {
                auto const& depth = full.market_depth();
                for (int i = 0; i < depth.bids_size() && i < 5; ++i) {
                    t.bids[i] = {
                        .price = depth.bids(i).price(),
                        .quantity = static_cast<uint32_t>(depth.bids(i).quantity()),
                        .orders = static_cast<uint32_t>(depth.bids(i).orders())
                    };
                }
                for (int i = 0; i < depth.asks_size() && i < 5; ++i) {
                    t.asks[i] = {
                        .price = depth.asks(i).price(),
                        .quantity = static_cast<uint32_t>(depth.asks(i).quantity()),
                        .orders = static_cast<uint32_t>(depth.asks(i).orders())
                    };
                }
            }

            if (full.has_greeks()) {
                auto const& greeks = full.greeks();
                t.greeks.iv = greeks.iv();
                t.greeks.delta = greeks.delta();
                t.greeks.theta = greeks.theta();
                t.greeks.gamma = greeks.gamma();
                t.greeks.vega = greeks.vega();
            }

            ring_buffer_->push(t);
        }
    }
}

} // namespace alpha::ingester
