#pragma once

#include <cstdint>
#include <alpha/models/MarketModels.hpp>
#include <alpha/models/BacktestTick.hpp>
#include "ResultAggregator.hpp"

// Forward declarations for market_engine types (resolved via CMake include path)
namespace alpha::market {
    class PortfolioManager;
    struct PortfolioSnapshot;
    struct Trade;
}

namespace alpha {
namespace backtest {

/**
 * BacktestPortfolio — wraps market_engine's PortfolioManager for backtest simulation.
 *
 * Mirrors the live market_engine exactly:
 *   - Full TaxBreakdown (brokerage, STT, GST, exchange, SEBI, stamp duty, DP)
 *   - Segment-aware (EQUITY_INTRADAY, FUTURES, OPTIONS, etc.)
 *   - Multi-instrument positions via InstrumentRegistry
 *   - ExecutionSimulator for realistic slippage
 *
 * Called by SimRunner in place of the old flat open_qty_/open_entry_price_ state.
 */
class BacktestPortfolio {
public:
    explicit BacktestPortfolio(double starting_capital = 1'000'000.0);
    ~BacktestPortfolio();

    /**
     * Apply an OrderIntent to the portfolio — simulates a fill with full charges.
     * Records the resulting trade in ResultAggregator for metrics.
     * @return false if trade was rejected (insufficient margin/cash).
     */
    bool apply_order(
        const models::OrderIntent&  intent,
        const models::BacktestTick& tick,
        ResultAggregator&           aggregator);

    /**
     * MTM all open positions at current tick price.
     * Records an equity curve sample in aggregator every N calls.
     */
    void mark_to_market(const models::BacktestTick& tick,
                        ResultAggregator&             aggregator);

    double total_equity()     const;
    double available_cash()   const;
    int32_t open_positions()  const;
    int32_t total_trades()    const;

    /** Print portfolio summary (delegates to PortfolioManager::print_summary). */
    void print_summary() const;

private:
    market::PortfolioManager* pm_;   // Heap-allocated to avoid including full header
    uint64_t trade_id_seq_     = 0;
    uint64_t mtm_call_count_   = 0;
    static constexpr uint64_t MTM_RECORD_INTERVAL = 100;  // Record equity every 100 ticks
};

} // namespace backtest
} // namespace alpha
