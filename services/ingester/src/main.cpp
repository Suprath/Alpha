#include <iostream>
#include <alpha/config/Config.hpp>
#include <alpha/ingester/UpstoxIngester.hpp>
#include <alpha/time/Timestamp.hpp>
#include <memory>
#include <utility>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    try {
        std::cout << "=== Alpha Ingester v0.1.0 ===" << std::endl;
        
        boost::asio::io_context ioc;
        // Keep the IO context alive even if it's idle for now
        auto work_guard = boost::asio::make_work_guard(ioc);
        
        auto ingester = std::make_unique<alpha::ingester::UpstoxIngester>(ioc);

        // 1. Check for token in ENV first (non-interactive/headless mode)
        std::string auth_code = "";
        std::string existing_token = alpha::config::Config::get().get_string("UPSTOX_ACCESS_TOKEN", "");
        
        if (existing_token.empty()) {
            // Only prompt if we're in a TTY (not detached Docker)
            std::string login_url = "https://api.upstox.com/v2/login/authorization/dialog?response_type=code&client_id=";
            login_url += alpha::config::Config::get().get_string("UPSTOX_API_KEY");
            login_url += "&redirect_uri=" + alpha::config::Config::get().get_string("UPSTOX_REDIRECT_URI");

            std::cout << "1. Open this URL in your browser:\n" << login_url << std::endl;
            std::cout << "2. Log in and paste the 'code=' from the redirect URL here: ";
            
            // Note: This will hang or fail in detached Docker if no token is in .env
            if (!(std::cin >> auth_code)) {
                std::cerr << "Failed to read auth_code from stdin. Ensure UPSTOX_ACCESS_TOKEN is set in .env for headless mode." << std::endl;
            }
        }

        if (!existing_token.empty() || ingester->authenticate(auth_code)) {
            std::cout << "[IST " << alpha::time::Timestamp::now_ist_ns() << "] Authentication successful!" << std::endl;
            
            // Dynamic Retrieval based on Environment
            std::string symbols_env = alpha::config::Config::get().get_string("INGEST_SYMBOLS", "");
            if (!symbols_env.empty()) {
                std::string interval = alpha::config::Config::get().get_string("INGEST_INTERVAL", "1minute");
                std::string from = alpha::config::Config::get().get_string("INGEST_FROM", "");
                std::string to = alpha::config::Config::get().get_string("INGEST_TO", "");

                if (!from.empty() && !to.empty()) {
                    auto candles = ingester->fetch_historical(symbols_env, interval, from, to);
                    std::cout << "Fetched " << candles.size() << " candles for " << symbols_env << std::endl;
                }
            }

            ingester->connect_feed();
            if (!symbols_env.empty()) {
                ingester->subscribe({symbols_env});
            }
            
            std::cout << "[IST " << alpha::time::Timestamp::now_ist_ns() << "] Ingester is now monitoring the feed. Press Ctrl+C to terminate." << std::endl;
            ioc.run();
        }

    } catch (std::exception& e) {
        std::cerr << "CRITICAL ERROR: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
