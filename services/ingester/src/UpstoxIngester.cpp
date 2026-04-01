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

UpstoxIngester::UpstoxIngester(net::io_context& ioc) 
    : ioc_(ioc) {
    // 30 requests per minute for historical data = 0.5 requests per second
    rest_limiter_ = std::make_unique<RateLimiter>(0.5, 1.0); 
    access_token_ = load_token();
}

bool UpstoxIngester::authenticate(const std::string& auth_code) {
    std::cout << "Exchanging auth code for access token..." << std::endl;
    // Manual code-to-token logic (In production, this is a POST request to Upstox)
    // For now, we'll use the token if it's already in .env or the CACHE
    if (access_token_.empty()) {
        access_token_ = auth_code; // If code is provided, use it
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
    if (!file.is_open()) {
        std::cerr << "CRITICAL ERROR: Failed to write to token path " << path << std::endl;
        return;
    }
    file << j.dump(4);
}

std::string UpstoxIngester::load_token() {
    // Priority: 1. ENV variable 2. JSON cache
    std::string env_token = alpha::config::Config::get().get_string("UPSTOX_ACCESS_TOKEN", "");
    if (!env_token.empty()) return env_token;

    std::string path = alpha::config::Config::get().get_string("UPSTOX_TOKEN_PATH", "/app/secrets/.upstox_token.json");
    std::ifstream file(path);
    if (file.is_open()) {
        try {
            json j;
            file >> j;
            if (j.contains("access_token") && j.contains("timestamp")) {
                uint64_t saved_time = j["timestamp"];
                uint64_t current_time = alpha::time::Timestamp::now_ns();
                // Upstox tokens expire after 1 day (86400 seconds) = 86400000000000 ns
                if ((current_time - saved_time) < 86400000000000ULL) {
                    return j["access_token"];
                } else {
                    std::cout << "[IST " << alpha::time::Timestamp::now_ist_ns() << "] Cached token expired." << std::endl;
                }
            }
        } catch (...) {
            // Ignore parse errors, just return empty and act as if no token exists
        }
    }
    return "";
}

void UpstoxIngester::connect_feed() {
    if (access_token_.empty()) return;
    std::cout << "DEBUG: [IST " << alpha::time::Timestamp::now_ist_ns() << "] WebSocket Feed connection initialized." << std::endl;
}

void UpstoxIngester::subscribe(const std::vector<std::string>& symbols) {
    (void)symbols;
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
    // ENFORCE RATE LIMIT
    rest_limiter_->acquire();
    
    std::cout << "DEBUG: [IST " << alpha::time::Timestamp::now_ist_ns() 
              << "] Fetching " << interval << " for " << symbol << "..." << std::endl;
    
    // In this stage, we are just verifying the logic. 
    // We'll perform the real HTTP request in the next step when we verify connection.
    std::vector<Candle> mock_candles;
    Candle c { alpha::time::Timestamp::now_ns(), 100.0, 105.0, 95.0, 102.0, 1000, 0 };
    mock_candles.push_back(c);
    
    return mock_candles;
}

} // namespace alpha::ingester
