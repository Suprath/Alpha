#include <alpha/backtest/BacktestPortfolio.hpp>

// market_engine portfolio — included via CMake cross-service path
#include <portfolio/PortfolioManager.hpp>
#include <execution/ExecutionSimulator.hpp>
#include <core/InstrumentInfo.hpp>
#include <core/Trade.hpp>

#include <cstring>
#include <iostream>

namespace alpha {
namespace backtest {

BacktestPortfolio::BacktestPortfolio(double starting_capital)
    : pm_(new market::PortfolioManager(starting_capital))
{}

BacktestPortfolio::~BacktestPortfolio() {
    delete pm_;
}

bool BacktestPortfolio::apply_order(
    const models::OrderIntent&  intent,
    const models::BacktestTick& tick,
    ResultAggregator&           aggregator)
{
    if (intent.qty == 0 || intent.side == 0) return false;

    // Build InstrumentInfo via registry (auto-registers on first sight)
    auto& registry = market::InstrumentRegistry::instance();
    const auto& info = registry.get_or_register(
        intent.instrument_token, intent.symbol);

    // Simulate fill with realistic slippage + full charges
    ++trade_id_seq_;
    market::Trade mt = market::ExecutionSimulator::simulate(intent, info, trade_id_seq_);

    // Override timestamp with backtest tick time (not wall clock)
    mt.timestamp_ns = tick.timestamp_ns;

    // Track realized PnL change for this trade
    const double pnl_before = pm_->snapshot().gross_realized_pnl;

    // Apply to portfolio
    const bool accepted = pm_->apply_trade(mt);
    if (!accepted) return false;

    const double pnl_after = pm_->snapshot().gross_realized_pnl;
    const double net_pnl = pnl_after - pnl_before;

    // Only record a trade when a position is closed or reduced (net_pnl != 0.0)
    // or if the reason was specifically an exit/reverse.
    if (net_pnl != 0.0 || intent.reason == 2 || intent.reason == 5) {
        backtest::Trade bt{};
        bt.entry_ts_ns      = tick.timestamp_ns;
        bt.exit_ts_ns       = tick.timestamp_ns;
        bt.instrument_token = intent.instrument_token;
        bt.entry_price      = mt.signal_price;
        bt.exit_price       = mt.fill_price;
        bt.qty              = static_cast<int32_t>(mt.qty);
        bt.side             = mt.side;
        bt.gross_pnl        = net_pnl + mt.charges.total;
        bt.commission       = mt.charges.total;
        bt.net_pnl          = net_pnl;
        aggregator.record_trade(bt);
    }

    return true;
}

void BacktestPortfolio::mark_to_market(
    const models::BacktestTick& tick,
    ResultAggregator&             aggregator)
{
    pm_->mark_to_market(tick.instrument_token,
                        static_cast<double>(tick.last_price));

    // Record equity curve point at configured interval
    ++mtm_call_count_;
    if (mtm_call_count_ % MTM_RECORD_INTERVAL == 0) {
        const auto snap = pm_->snapshot();
        aggregator.record_equity(tick.timestamp_ns, snap.total_equity);
    }
}

double BacktestPortfolio::total_equity() const {
    return pm_->snapshot().total_equity;
}

double BacktestPortfolio::available_cash() const {
    return pm_->available_cash();
}

int32_t BacktestPortfolio::open_positions() const {
    return pm_->snapshot().open_positions;
}

int32_t BacktestPortfolio::total_trades() const {
    return pm_->snapshot().total_trades;
}

void BacktestPortfolio::print_summary() const {
    pm_->print_summary();
}

} // namespace backtest
} // namespace alpha
