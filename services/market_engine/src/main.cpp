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
    std::cout << "=== Alpha Market Engine v0.1.0 (Indian Markets Paper Trader) ===" << std::endl;

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Configurable starting capital
    const char* capital_env = std::getenv("STARTING_CAPITAL");
    const double starting_capital = capital_env
        ? std::stod(capital_env)
        : alpha::market::PortfolioManager::DEFAULT_STARTING_CAPITAL;

    std::cout << "[Main] Starting capital: " << starting_capital << std::endl;

    // -- SHM Reader: consumes OrderIntents from strategy engine --
    alpha::market::ShmReader reader("alpha_order_shm_v1", "order_queue");
    reader.wait_for_attachment();

    // -- Market Engine: paper trading + portfolio --
    alpha::market::MarketEngine engine(starting_capital);

    std::cout << "[Main] Entering order processing loop..." << std::endl;

    alpha::models::OrderIntent intent{};
    uint64_t last_portfolio_print_ns = alpha::time::Timestamp::now_ns();
    constexpr uint64_t PORTFOLIO_PRINT_INTERVAL_NS = 300ULL * 1'000'000'000ULL; // 5 min

    while (g_running.load(std::memory_order_relaxed)) {
        if (reader.poll(intent)) {
            engine.on_order(intent);
            // Update MTM price with latest signal price
            engine.update_price(intent.instrument_token, intent.price);
        }

        // Periodic portfolio summary (every 5 minutes)
        uint64_t now = alpha::time::Timestamp::now_ns();
        if (now - last_portfolio_print_ns >= PORTFOLIO_PRINT_INTERVAL_NS) {
            engine.print_portfolio();
            last_portfolio_print_ns = now;
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
