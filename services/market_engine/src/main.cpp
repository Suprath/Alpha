/**
 * Alpha Market Engine — v0.1.0
 *
 * Indian market paper trading engine. Reads OrderIntents from shared memory
 * (written by the strategy engine), simulates realistic execution with:
 *   - Bid-ask spread slippage by segment
 *   - Accurate NSE/BSE charge calculations (STT, GST, brokerage, exchange,
 *     SEBI, stamp duty, DP charges)
 *   - Portfolio tracking (positions, cash, P&L, margin)
 *   - Support for equity intraday, delivery, index/stock futures & options
 *
 * SHM Input:  alpha_order_shm_v1 / order_queue  (OrderIntent structs, 4096-cap SPSC)
 * Data Flow:  Strategy Engine → [SHM] → Market Engine → Portfolio State
 *
 * Starting capital: ₹10,00,000 (configurable via STARTING_CAPITAL env var)
 */

#include <iostream>
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <alpha/time/Timestamp.hpp>

#include "shm_reader/ShmReader.hpp"
#include "engine/MarketEngine.hpp"

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

int main() {
    std::cout << std::unitbuf;   // Force line-unbuffered stdout in Docker
    std::cout << "=== Alpha Market Engine v0.1.0 (Indian Markets Paper Trader) ===" << std::endl;

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Configurable starting capital
    const char* capital_env = std::getenv("STARTING_CAPITAL");
    const double starting_capital = capital_env
        ? std::stod(capital_env)
        : alpha::market::PortfolioManager::DEFAULT_STARTING_CAPITAL;

    // Redis config for portfolio publishing
    const char* redis_host_env = std::getenv("REDIS_HOST");
    const char* redis_port_env = std::getenv("REDIS_PORT");
    const std::string redis_host = redis_host_env ? redis_host_env : "redis";
    const int         redis_port = redis_port_env ? std::stoi(redis_port_env) : 6379;

    std::cout << "[Main] Starting capital: " << starting_capital << std::endl;
    std::cout << "[Main] Redis publisher: " << redis_host << ":" << redis_port << std::endl;

    // -- SHM Reader: consumes OrderIntents from strategy engine --
    alpha::market::ShmReader reader("alpha_order_shm_v1", "order_queue");

    // -- Market Engine: paper trading + portfolio --
    alpha::market::MarketEngine engine(starting_capital, redis_host, redis_port);

    std::cout << "[Main] Entering order processing loop..." << std::endl;

    alpha::models::OrderIntent intent{};
    uint64_t last_portfolio_print_ns  = alpha::time::Timestamp::now_ns();
    uint64_t last_redis_publish_ns    = last_portfolio_print_ns;
    constexpr uint64_t PORTFOLIO_PRINT_INTERVAL_NS  = 300ULL * 1'000'000'000ULL; // 5 min
    constexpr uint64_t REDIS_PUBLISH_INTERVAL_NS    =   5ULL * 1'000'000'000ULL; // 5 sec

    while (g_running.load(std::memory_order_relaxed)) {
        if (reader.poll(intent)) {
            engine.on_order(intent);
            // Update MTM price with latest signal price
            engine.update_price(intent.instrument_token, intent.price);
        }

        uint64_t now = alpha::time::Timestamp::now_ns();

        // Periodic portfolio summary to stdout (every 5 minutes)
        if (now - last_portfolio_print_ns >= PORTFOLIO_PRINT_INTERVAL_NS) {
            engine.print_portfolio();
            last_portfolio_print_ns = now;
        }

        // Periodic Redis publish (every 5 seconds — drives TUI portfolio panel)
        if (now - last_redis_publish_ns >= REDIS_PUBLISH_INTERVAL_NS) {
            engine.publish_portfolio_to_redis();
            last_redis_publish_ns = now;
        }

        // EOD check: square off all intraday positions at 15:30 IST
        // The Timestamp utility provides is_market_session_active()
        // At session end, trigger square-off once
        // (simplified: handled externally or via SIGTERM + final summary)
    }

    // Final cleanup
    engine.eod_square_off_all();
    engine.print_portfolio();

    return 0;
}
