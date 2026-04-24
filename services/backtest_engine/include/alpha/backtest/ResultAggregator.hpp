#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <string>
#include <limits>

namespace alpha {
namespace backtest {

/**
 * Trade — a completed round-trip (entry + exit).
 */
struct Trade {
    uint64_t    entry_ts_ns;
    uint64_t    exit_ts_ns;
    uint32_t    instrument_token;
    double      entry_price;
    double      exit_price;
    int32_t     qty;          // +N = long, −N = short
    int32_t     side;         // 1 = BUY (long), −1 = SELL (short)
    double      gross_pnl;    // Before costs
    double      commission;   // Brokerage + STT + other charges
    double      net_pnl;      // gross_pnl − commission
};

/**
 * BacktestResult — summary performance metrics for one simulation run.
 */
struct BacktestResult {
    // P&L
    double total_net_pnl{0.0};
    double total_gross_pnl{0.0};
    double total_commission{0.0};

    // Drawdown
    double max_drawdown{0.0};        // Absolute peak-to-trough loss (₹)
    double max_drawdown_pct{0.0};    // As % of peak equity
    double calmar_ratio{0.0};        // CAGR / max_drawdown_pct

    // Risk-adjusted returns
    double sharpe_ratio{0.0};        // Annualized (daily returns, rf=0)
    double sortino_ratio{0.0};       // Downside std-dev only

    // Trade statistics
    uint64_t total_trades{0};
    uint64_t winning_trades{0};
    uint64_t losing_trades{0};
    double   win_rate{0.0};          // winning / total
    double   profit_factor{0.0};     // gross_profit / gross_loss
    double   avg_trade_pnl{0.0};
    double   avg_win{0.0};
    double   avg_loss{0.0};
    double   largest_win{0.0};
    double   largest_loss{0.0};
    double   expectancy{0.0};        // win_rate × avg_win − loss_rate × |avg_loss|

    // True final portfolio equity (starting_capital + all realized PnL after forced close)
    double   final_equity{0.0};

    void print() const;
};

/**
 * ResultAggregator — single-pass O(N) metrics engine.
 *
 * Designed for instant aggregation: all metrics are computed in one pass
 * over the trade list and equity curve after the hot-loop completes.
 *
 * Usage:
 *   ResultAggregator agg;
 *   agg.record_trade(trade);                // called per completed trade
 *   agg.record_equity(ts_ns, equity);       // called per tick (or per trade)
 *   BacktestResult res = agg.compute();
 */
class ResultAggregator {
public:
    ResultAggregator() = default;

    void record_trade(Trade t);
    void record_equity(uint64_t ts_ns, double equity);

    /**
     * Compute and return final performance statistics.
     * Requires at least one equity point to be recorded.
     */
    BacktestResult compute() const;

    void reset();

private:
    std::vector<Trade>                          trades_;
    std::vector<std::pair<uint64_t, double>>    equity_curve_;
};

} // namespace backtest
} // namespace alpha
