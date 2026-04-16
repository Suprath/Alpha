#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include <alpha/models/MarketModels.hpp>
#include "../core/InstrumentInfo.hpp"
#include "../portfolio/PortfolioManager.hpp"
#include "../execution/ExecutionSimulator.hpp"
#include "../redis/RedisPublisher.hpp"

namespace alpha::market {

/**
 * @brief Top-level paper trading engine for Indian markets.
 *
 * Responsibilities:
 *  1. Receive OrderIntent from strategy engine (via SHM)
 *  2. Look up instrument metadata (segment, lot size)
 *  3. Simulate paper execution with realistic slippage
 *  4. Calculate all Indian market charges (STT, GST, exchange, SEBI, stamp, DP)
 *  5. Apply trade to portfolio (positions, cash, margin)
 *  6. Print real-time trade blotter and portfolio summary
 *
 * Supported segments:
 *  - Equity Intraday (MIS) — default for algo signals
 *  - Equity Delivery (CNC)
 *  - Index Futures (NIFTY, BANKNIFTY, FINNIFTY)
 *  - Stock Futures
 *  - Index Options (CE/PE)
 *  - Stock Options
 *  - Currency Futures/Options
 */
class MarketEngine {
public:
    explicit MarketEngine(double starting_capital = PortfolioManager::DEFAULT_STARTING_CAPITAL,
                          const std::string& redis_host = "redis", int redis_port = 6379);

    /**
     * @brief Process an incoming OrderIntent from the strategy engine.
     * @return true if the trade was accepted and simulated.
     */
    bool on_order(const alpha::models::OrderIntent& intent);

    /**
     * @brief Update mark-to-market price for an instrument.
     * Called whenever a new signal/tick arrives for an instrument.
     */
    void update_price(uint32_t token, double current_price);

    /**
     * @brief Trigger EOD square-off of all intraday positions.
     */
    void eod_square_off_all();

    /**
     * @brief Periodic portfolio summary (call at desired intervals).
     */
    void print_portfolio() const { portfolio_.print_summary(); }

    /**
     * @brief Publish current portfolio snapshot to Redis.
     * Called after every trade and periodically from the main loop.
     */
    void publish_portfolio_to_redis();

    const PortfolioManager& portfolio() const { return portfolio_; }

private:
    PortfolioManager    portfolio_;
    InstrumentRegistry& registry_;
    RedisPublisher      redis_publisher_;
    uint64_t            trade_id_counter_{0};

    // Last known price per instrument (for MTM and EOD square-off)
    std::unordered_map<uint32_t, double> last_prices_;

    void log_trade(const Trade& trade) const;
};

} // namespace alpha::market
