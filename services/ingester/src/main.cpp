#include <iostream>
#include <alpha/config/Config.hpp>
#include <alpha/ingester/IngesterFactory.hpp>
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
        
        // 1. Resolve Vendor and Create Ingester
        std::string vendor = alpha::config::Config::get().get_string("DATA_VENDOR", "UPSTOX");
        auto ingester = alpha::ingester::IngesterFactory::create(ioc, vendor);

        // 2. Universal Authentication Flow (Interactive logic is now in subclasses)
        if (ingester->authenticate()) {
            std::cout << "[IST " << alpha::time::Timestamp::now_ist_ns() << "] " << vendor << " authentication successful!" << std::endl;
            
            // 3. Optional Historical Data Backfill
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

            // 4. Connect to Real-time Stream
            ingester->connect_feed();

            // 5. Subscribe to instruments
            //    Priority: INGEST_SYMBOLS env var → all instruments loaded from DB
            std::vector<std::string> symbols;
            if (!symbols_env.empty()) {
                std::cout << "[INFO] INGEST_SYMBOLS: " << symbols_env << std::endl;
                std::stringstream ss(symbols_env);
                std::string s;
                while (std::getline(ss, s, ',')) {
                    if (!s.empty()) symbols.push_back(s);
                }
            } else {
                // Auto-subscribe to all instruments loaded from DB
                auto* upstox = dynamic_cast<alpha::ingester::UpstoxIngester*>(ingester.get());
                if (upstox) {
                    symbols = upstox->all_instrument_keys();
                    std::cout << "[INFO] Auto-subscribing to " << symbols.size()
                              << " instruments from DB." << std::endl;
                }
            }
            if (!symbols.empty()) {
                ingester->subscribe(symbols);
            } else {
                std::cout << "[WARN] No instruments to subscribe to. "
                          << "Set INGEST_SYMBOLS or run instrument_loader first." << std::endl;
            }
            
            std::cout << "[IST " << alpha::time::Timestamp::now_ist_ns() << "] Monitoring " << vendor << " feed. Press Ctrl+C to terminate." << std::endl;
            ioc.run();
        }

    } catch (std::exception& e) {
        std::cerr << "CRITICAL ERROR: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
