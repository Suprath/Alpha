#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include <cstddef>
#include <alpha/models/BacktestTick.hpp>
#include <alpha/models/MarketModels.hpp>
#include "BacktestClock.hpp"
#include "TickLoader.hpp"
#include "ResultAggregator.hpp"

namespace alpha {
namespace backtest {

/**
 * SimConfig — parameters for a single backtest run.
 */
struct SimConfig {
    double   starting_capital{1'000'000.0}; // ₹10,00,000 default
    double   brokerage_flat{20.0};          // ₹20 flat per order (Zerodha-style)
    double   stt_rate{0.001};               // 0.1% STT on sell side
    double   exchange_txn_charge{0.0000345};// NSE transaction charge
    double   gst_rate{0.18};                // 18% GST on brokerage
    double   sebi_charge{0.000001};         // ₹10/crore SEBI turnover fee
    double   slippage_bps{2.0};             // 2 bps market impact/slippage
    size_t   batch_size{1024};              // Ticks per hot-loop iteration
};

/**
 * SimRunner — hot-loop orchestrator for backtesting.
 *
 * ── Hot-loop design ────────────────────────────────────────────────────────
 *   for each batch of 1024 BacktestTicks:
 *     1. Advance BacktestClock to batch[last].timestamp_ns
 *     2. For each tick in batch:
 *        a. Call strategy_fn(tick, clock) → OrderIntent
 *        b. If intent.action != 0: execute trade with cost model
 *        c. Update open position P&L
 *     3. Record equity point in ResultAggregator
 *
 * ── Performance targets ────────────────────────────────────────────────────
 *   - Zero heap allocation in hot path (all state pre-allocated)
 *   - strategy_fn must be a tight lambda (inline-able)
 *   - TickLoader in BINARY_MMAP mode: tick array from mmap — pointer arithmetic only
 *   - Target: 200M ticks/sec on modern NVMe + SIMD-friendly batch size
 *
 * ── Cost model (Indian equity / F&O) ──────────────────────────────────────
 *   Total cost = brokerage + STT + exchange_txn + SEBI + GST on brokerage + slippage
 *   Applied at order execution time against reference tick price.
 */
class SimRunner {
public:
    /**
     * Strategy function signature.
     * Receives the current tick and a read-only clock.
     * Returns an OrderIntent (action=0 means no trade).
     * Must NOT read any market data with timestamp > clock.now() (look-ahead).
     */
    using StrategyFn = std::function<
        models::OrderIntent(const models::BacktestTick& tick,
                            const BacktestClock&        clock)>;

    SimRunner(SimConfig cfg, StrategyFn strategy);

    /**
     * Run backtest on a binary .alpha file (BINARY_MMAP path).
     * @return Final performance statistics.
     */
    BacktestResult run(const std::string& alpha_file_path);

    /**
     * Run backtest on an in-memory tick array (QUESTDB_SQL or test path).
     * @param ticks   Pointer to contiguous BacktestTick array.
     * @param count   Number of ticks.
     */
    BacktestResult run(const models::BacktestTick* ticks, size_t count);

    /** Reset all state for a fresh simulation run on the same SimRunner. */
    void reset();

private:
    void process_batch(const models::BacktestTick* batch, size_t n);
    void execute_order(const models::OrderIntent& intent,
                       const models::BacktestTick& tick);
    double apply_cost_model(double price, int32_t qty, int32_t side) const;

    SimConfig        cfg_;
    StrategyFn       strategy_;
    BacktestClock    clock_;
    ResultAggregator aggregator_;
    TickLoader       loader_;

    // Position tracking (per instrument — simplified: single position)
    double   equity_;
    double   peak_equity_;
    int32_t  open_qty_{0};
    double   open_entry_price_{0.0};
    uint64_t open_entry_ts_{0};
    int32_t  open_side_{0};
    uint32_t open_instrument_{0};
};

} // namespace backtest
} // namespace alpha
