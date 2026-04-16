#pragma once

#include <unordered_map>
#include <vector>
#include <cstdint>
#include "Position.hpp"
#include "../core/Trade.hpp"

namespace alpha::market {

/**
 * @brief Aggregate portfolio state — cash, positions, P&L, margin.
 */
struct PortfolioSnapshot {
    double cash;                   // Available cash (unrestricted)
    double margin_used;            // Margin currently blocked by open positions
    double total_equity;           // cash + unrealized_pnl (MTM net asset value)
    double gross_realized_pnl;     // Sum of realized_pnl across all positions
    double total_charges_paid;     // Cumulative charges across all trades
    double net_realized_pnl;       // gross_realized_pnl (charges already deducted in Position)
    int32_t open_positions;        // Count of non-flat positions
    int32_t total_trades;          // Lifetime trade count
};

/**
 * @brief Manages portfolio state: cash, positions, margin, and trade history.
 *
 * Thread-safety: single-threaded (called from the market engine spin loop).
 */
class PortfolioManager {
public:
    static constexpr double DEFAULT_STARTING_CAPITAL = 10'00'000.0; // ₹10,00,000

    explicit PortfolioManager(double starting_capital = DEFAULT_STARTING_CAPITAL);

    /**
     * @brief Apply a completed paper trade to the portfolio.
     * Updates position, cash balance, and margin.
     * @return false if insufficient cash/margin (trade rejected).
     */
    bool apply_trade(const Trade& trade);

    /**
     * @brief Mark all open positions to current market price (MTM).
     */
    void mark_to_market(uint32_t token, double current_price);

    /**
     * @brief Get or create a position for an instrument.
     */
    const Position& position(uint32_t token) const;

    /**
     * @brief Snapshot of current portfolio state.
     */
    PortfolioSnapshot snapshot() const;

    /**
     * @brief Full trade history (all fills).
     */
    const std::vector<Trade>& trade_history() const { return trades_; }

    double available_cash()  const { return cash_; }
    double margin_used()     const { return margin_used_; }

    /** Read-only access to all positions (for Redis publishing). */
    const std::unordered_map<uint32_t, Position>& positions() const { return positions_; }

    /**
     * @brief EOD reset: realize all intraday positions at last price, clear margin.
     * Call this at 15:30 IST for intraday positions.
     */
    void eod_square_off(uint32_t token, double last_price);

    /**
     * @brief Print portfolio summary to stdout.
     */
    void print_summary() const;

private:
    double starting_capital_;
    double cash_;
    double margin_used_;
    std::unordered_map<uint32_t, Position> positions_;
    std::vector<Trade> trades_;
    int32_t trade_count_{0};

    void update_margin(uint32_t token, double delta_margin);
};

} // namespace alpha::market
