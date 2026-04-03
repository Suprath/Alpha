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
    : ioc_(ioc), ssl_ctx_(ssl::context::tlsv12_client), resolver_(ioc), heartbeat_timer_(ioc) {
    // 30 requests per minute for historical data = 0.5 requests per second
    rest_limiter_ = std::make_unique<RateLimiter>(0.5, 1.0); 
    access_token_ = load_token();

    // Configure SSL context
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_none); // In production, use peer verification with proper CA certs

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
                pqxx::result r = n.exec("SELECT instrument_key, id FROM instrument_universe ORDER BY date DESC LIMIT 50000");
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

bool UpstoxIngester::authenticate(const std::string& code) {
    std::string auth_code = code;
    
    // 1. Check if we already have a valid session
    if (!access_token_.empty()) {
        std::cout << "[Upstox] Using cached session token." << std::endl;
        return true;
    }

    // 2. If no code provided, and no token, we may need interactive login
    if (auth_code.empty()) {
        std::string api_key = alpha::config::Config::get().get_string("UPSTOX_API_KEY", "");
        std::string redirect = alpha::config::Config::get().get_string("UPSTOX_REDIRECT_URI", "http://localhost:8080");
        
        if (api_key.empty()) {
            std::cerr << "[Upstox] ERROR: UPSTOX_API_KEY is missing from configuration." << std::endl;
            return false;
        }

        std::string login_url = "https://api.upstox.com/v2/login/authorization/dialog?response_type=code&client_id=" + api_key + "&redirect_uri=" + redirect;
        
        std::cout << "\n=== Upstox Interactive Login ===" << std::endl;
        std::cout << "1. Open this URL: " << login_url << std::endl;
        std::cout << "2. Enter the 'code=' parameter here: ";
        
        if (!(std::cin >> auth_code)) {
            std::cerr << "[Upstox] Failed to read auth_code from stdin." << std::endl;
            return false;
        }
    }

    std::cout << "[Upstox] Exchanging auth code for access token..." << std::endl;
    // In production, this would be a real POST request. 
    // For now, we simulate the exchange.
    access_token_ = auth_code; 
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
    if (access_token_.empty()) {
        std::cerr << "[Upstox] ERROR: Cannot connect to feed without access token." << std::endl;
        return;
    }

    // Market Hours Check
    if (!alpha::time::Timestamp::is_market_session_active()) {
        std::cout << "[Upstox] WARNING: Attempting connection outside market hours (09:15-15:30 IST)." << std::endl;
        // Proceeding anyway but with a warning, as requested for error handling
    }

    std::cout << "[Upstox] Authorizing V3 Market Data Feed..." << std::endl;
    
    // We'll reuse the resolver for the HTTP authorization step
    resolver_.async_resolve("api.upstox.com", "443",
        [this](beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) {
                std::cerr << "[Upstox] Auth Resolve error: " << ec.message() << std::endl;
                return;
            }

            // Create a temporary SSL stream for the authorization request
            auto stream = std::make_shared<beast::ssl_stream<beast::tcp_stream>>(ioc_, ssl_ctx_);
            
            // Set SNI
            if (!SSL_set_tlsext_host_name(stream->native_handle(), "api.upstox.com")) {
                std::cerr << "[Upstox] Auth SNI error" << std::endl;
                return;
            }

            beast::get_lowest_layer(*stream).async_connect(results,
                [this, stream](beast::error_code ec, tcp::resolver::endpoint_type) {
                    if (ec) {
                        std::cerr << "[Upstox] Auth Connect error: " << ec.message() << std::endl;
                        return;
                    }

                    stream->async_handshake(ssl::stream_base::client,
                        [this, stream](beast::error_code ec) {
                            if (ec) {
                                std::cerr << "[Upstox] Auth SSL handshake error: " << ec.message() << std::endl;
                                return;
                            }

                            // Formulate the HTTP GET request
                            auto req = std::make_shared<http::request<http::empty_body>>(http::verb::get, "/v3/feed/market-data-feed/authorize", 11);
                            req->set(http::field::host, "api.upstox.com");
                            req->set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
                            req->set(http::field::authorization, "Bearer " + access_token_);
                            req->set(http::field::accept, "application/json");

                            http::async_write(*stream, *req,
                                [this, stream, req](beast::error_code ec, std::size_t) {
                                    if (ec) {
                                        std::cerr << "[Upstox] Auth Write error: " << ec.message() << std::endl;
                                        return;
                                    }

                                    auto res = std::make_shared<http::response<http::string_body>>();
                                    // Use a temporary buffer for the response
                                    auto buffer = std::make_shared<beast::flat_buffer>();
                                    http::async_read(*stream, *buffer, *res,
                                        [this, stream, res, buffer](beast::error_code ec, std::size_t) {
                                            if (ec) {
                                                std::cerr << "[Upstox] Auth Read error: " << ec.message() << std::endl;
                                                return;
                                            }
                                            this->on_authorize(ec, *res);
                                        });
                                });
                        });
                });
        });
}

void UpstoxIngester::on_authorize(beast::error_code ec, http::response<http::string_body> res) {
    if (ec) return;

    if (res.result() != http::status::ok) {
        std::cerr << "[Upstox] Authorization Failed: " << res.result_int() << " " << res.body() << std::endl;
        
        if (alpha::time::Timestamp::is_market_session_active()) {
            std::cerr << "[Upstox] CRITICAL: Authorization failed during market hours! Check token validity." << std::endl;
        }
        return;
    }

    try {
        auto data = json::parse(res.body());
        if (data["status"] == "success") {
            authorized_url_ = data["data"]["authorized_redirect_uri"];
            std::cout << "[Upstox] Authorization Successful. Redirecting to WSS..." << std::endl;

            // Parse the WSS URL to get host and path
            // Format: wss://wsfeeder-api.upstox.com/market-data-feeder/v3/upstox-developer-api/feeds?requestId=...&code=...
            std::string url = authorized_url_;
            std::string protocol = "wss://";
            size_t host_start = url.find(protocol) + protocol.length();
            size_t path_start = url.find("/", host_start);
            std::string host = url.substr(host_start, path_start - host_start);

            resolver_.async_resolve(host, "443",
                beast::bind_front_handler(&UpstoxIngester::on_resolve, this));
        } else {
            std::cerr << "[Upstox] Auth error in response body: " << res.body() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Upstox] JSON Parse error during auth: " << e.what() << std::endl;
    }
}

void UpstoxIngester::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "[Upstox] Resolve error: " << ec.message() << std::endl;
        return;
    }

    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(ioc_, ssl_ctx_);
    
    // Set suggested timeout settings for the websocket
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));

    // Make the connection on the IP address we get from a lookup
    beast::get_lowest_layer(*ws_).async_connect(results,
        beast::bind_front_handler(&UpstoxIngester::on_connect, this));
}

void UpstoxIngester::on_connect(beast::error_code ec, tcp::resolver::endpoint_type ep) {
    boost::ignore_unused(ep);
    if (ec) {
        std::cerr << "[Upstox] Connect error: " << ec.message() << std::endl;
        return;
    }

    // Set a timeout on the operation
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));

    // Set SNI Hostname (Mandatory for Cloudfront/modern TLS servers)
    // Extract host from authorized_url_
    std::string url = authorized_url_;
    std::string protocol = "wss://";
    size_t host_start = url.find(protocol) + protocol.length();
    size_t path_start = url.find("/", host_start);
    std::string host = url.substr(host_start, path_start - host_start);

    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        std::cerr << "[Upstox] SNI set host name error: " << ec.message() << std::endl;
        return;
    }

    // Perform the SSL handshake
    ws_->next_layer().async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&UpstoxIngester::on_ssl_handshake, this));
}

void UpstoxIngester::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[Upstox] SSL handshake error: " << ec.message() << std::endl;
        return;
    }

    // Turn off the timeout on the tcp_stream, because the websocket stream has its own timeout system.
    beast::get_lowest_layer(*ws_).expires_never();

    // Set suggested timeout settings for the websocket
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

    // Add Authorization header for WebSocket upgrade
    ws_->set_option(websocket::stream_base::decorator(
        [token = access_token_](websocket::request_type& req)
        {
            req.set(http::field::authorization, "Bearer " + token);
            req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " alpha-ingester");
            req.set(http::field::origin, "https://api.upstox.com");
        }));

    // Perform the websocket handshake
    // Extract host and target path from authorized_url_
    std::string url = authorized_url_;
    std::string protocol = "wss://";
    size_t host_start = url.find(protocol) + protocol.length();
    size_t path_start = url.find("/", host_start);
    std::string host = url.substr(host_start, path_start - host_start);
    std::string target = url.substr(path_start);

    ws_->async_handshake(host, target,
        beast::bind_front_handler(&UpstoxIngester::on_handshake, this));
}

void UpstoxIngester::on_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[Upstox] WebSocket handshake error: " << ec.message() << std::endl;
        return;
    }

    std::cout << "[Upstox] WebSocket Handshake Successful. Connection established." << std::endl;
    
    // Set binary mode for Protobuf ingestion
    ws_->binary(true);

    // Initial read
    ws_->async_read(buffer_,
        beast::bind_front_handler(&UpstoxIngester::on_read, this));

    // Handle pending subscriptions
    if (!pending_subscriptions_.empty()) {
        std::cout << "[Upstox] Applying " << pending_subscriptions_.size() << " pending subscriptions." << std::endl;
        subscribe(pending_subscriptions_);
        pending_subscriptions_.clear();
    }
}

void UpstoxIngester::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        std::cerr << "[Upstox] Read error: " << ec.message() << std::endl;
        return;
    }

    // Process the message (Protobuf Decoder)
    std::string message = beast::buffers_to_string(buffer_.data());
    on_message(message);

    // Clear the buffer
    buffer_.consume(buffer_.size());

    // Queue another read
    ws_->async_read(buffer_,
        beast::bind_front_handler(&UpstoxIngester::on_read, this));
}

void UpstoxIngester::subscribe(const std::vector<std::string>& symbols) {
    std::cout << "[DEBUG] subscribe() called with " << symbols.size() << " symbols." << std::endl;
    if (symbols.empty()) return;

    if (!ws_ || !ws_->is_open()) {
        std::cout << "[Upstox] Caching " << symbols.size() << " symbols for async subscription." << std::endl;
        pending_subscriptions_ = symbols;
        return;
    }

    json j;
    j["guid"] = "alpha_hft_session_" + std::to_string(alpha::time::Timestamp::now_ns());
    j["method"] = "sub";
    j["data"] = {
        {"mode", "full"},
        {"instrumentKeys", symbols}
    };

    std::string frame = j.dump();
    std::cout << "[Upstox] Sending V3 subscription frame for " << symbols.size() << " symbols." << std::endl;
    
    ws_->binary(true);
    ws_->async_write(net::buffer(frame), [this](beast::error_code ec, std::size_t bytes) {
        boost::ignore_unused(bytes);
        if (ec) std::cerr << "[Upstox] Subscription write error: " << ec.message() << std::endl;
    });
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
    if (!ring_buffer_ || data.empty()) return;

    com::upstox::marketdata::v3::MarketDataFeed::FeedResponse response;
    bool parsed = false;

    if (response.ParseFromString(data)) {
        parsed = true;
    } else if (data.size() > 1 && response.ParseFromArray(data.data() + 1, data.size() - 1)) {
        parsed = true;
    }

    if (!parsed) {
        std::cerr << "[Upstox] CRITICAL: Protobuf parse failure. Size: " << data.size() << std::endl;
        return;
    }

    for (auto const& [key, instrument_data] : response.feeds()) {
        auto it = instrument_map_.find(key);
        if (it == instrument_map_.end()) continue;

        Tick t;
        t.instrument_token = it->second;
        t.last_price = 0.0;

        if (instrument_data.has_ltp_data()) {
            t.last_price = instrument_data.ltp_data().last_price();
            t.timestamp_ns = instrument_data.ltp_data().last_tick_time() * 1000000ULL;
        } else if (instrument_data.has_full_data()) {
            auto const& full = instrument_data.full_data();
            if (full.has_ltp()) {
                t.last_price = full.ltp().last_price();
                t.timestamp_ns = full.ltp().last_tick_time() * 1000000ULL;
            }
            t.total_volume = full.volume_traded_today();
            t.vwap = full.average_price();
            
            // Populate Market Depth
            if (full.has_market_depth()) {
                auto const& depth = full.market_depth();
                for (int i = 0; i < depth.bids_size() && i < 5; ++i) {
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
