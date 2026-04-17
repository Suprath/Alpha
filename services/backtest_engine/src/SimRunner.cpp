#include <alpha/backtest/SimRunner.hpp>

#include <stdexcept>
#include <cmath>
#include <cstring>
#include <iostream>

namespace alpha {
namespace backtest {

SimRunner::SimRunner(SimConfig cfg, StrategyFn strategy)
    : cfg_(cfg)
    , strategy_(std::move(strategy))
    , loader_(LoaderMode::BINARY_MMAP)
    , equity_(cfg.starting_capital)
    , peak_equity_(cfg.starting_capital)
{}

void SimRunner::reset() {
    clock_.reset();
    aggregator_.reset();
    equity_            = cfg_.starting_capital;
    peak_equity_       = cfg_.starting_capital;
    open_qty_          = 0;
    open_entry_price_  = 0.0;
    open_entry_ts_     = 0;
    open_side_         = 0;
    open_instrument_   = 0;
    signals_           = nullptr;
}

BacktestResult SimRunner::run(const std::string& alpha_file_path,
                               const SignalMap&   signals) {
    reset();
    signals_ = signals.empty() ? nullptr : &signals;

    size_t count = 0;
    const models::BacktestTick* ticks = loader_.open_file(alpha_file_path, count);

    std::cout << "[backtest] Loaded " << count << " ticks from " << alpha_file_path << std::endl;
    if (signals_)
        std::cout << "[backtest] " << signals_->size() << " pre-computed signal bars available\n";

    run(ticks, count, signals);

    loader_.close_file();
    return aggregator_.compute();
}

BacktestResult SimRunner::run(const models::BacktestTick* ticks,
                               size_t                      count,
                               const SignalMap&             signals) {
    signals_ = signals.empty() ? nullptr : &signals;
    if (ticks == nullptr || count == 0) return aggregator_.compute();

    const size_t batch = cfg_.batch_size;
    size_t offset = 0;

    // ── Hot loop — iterate in batches for cache locality ──────────────────
    while (offset < count) {
        size_t n = std::min(batch, count - offset);
        process_batch(ticks + offset, n);
        offset += n;
    }

    // Mark any open position as closed at last price
    if (open_qty_ != 0 && count > 0) {
        const auto& last = ticks[count - 1];
        models::OrderIntent exit_intent{};
        exit_intent.timestamp_ns     = last.timestamp_ns;
        exit_intent.instrument_token = last.instrument_token;
        std::strncpy(exit_intent.symbol, "END_OF_DATA", sizeof(exit_intent.symbol));
        exit_intent.price  = static_cast<double>(last.last_price);
        exit_intent.qty    = std::abs(open_qty_);
        exit_intent.side   = -open_side_;  // Close by reversing
        exit_intent.reason = 2;            // EXIT
        execute_order(exit_intent, last);
    }

    return aggregator_.compute();
}

void SimRunner::process_batch(const models::BacktestTick* batch, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const auto& tick = batch[i];

        // Advance time frontier — prevents look-ahead in strategy
        clock_.advance(tick.timestamp_ns);

        // Look up pre-computed signal for this bar (O(1) hash map lookup)
        const models::BacktestSignal* sig = nullptr;
        if (signals_) {
            const uint64_t bar_ts = (tick.timestamp_ns / ONE_MINUTE_NS) * ONE_MINUTE_NS;
            auto it = signals_->find(bar_ts);
            if (it != signals_->end()) sig = &it->second;
        }

        // Evaluate strategy — receives signal (nullptr if unavailable)
        models::OrderIntent intent = strategy_(tick, clock_, sig);

        // Execute if qty > 0 (qty=0 means "no action" from strategy)
        if (intent.qty != 0 && intent.side != 0) {
            execute_order(intent, tick);
        }

        // Update open position P&L
        if (open_qty_ != 0) {
            double mark_pnl = static_cast<double>(open_side_) *
                              static_cast<double>(open_qty_) *
                              (static_cast<double>(tick.last_price) - open_entry_price_);
            double current_equity = cfg_.starting_capital + mark_pnl;

            if (current_equity > peak_equity_)
                peak_equity_ = current_equity;

            // Record equity every 1000 ticks to avoid memory explosion
            if (i % 1000 == 0)
                aggregator_.record_equity(tick.timestamp_ns, current_equity);
        }
    }
}

void SimRunner::execute_order(const models::OrderIntent& intent,
                               const models::BacktestTick& tick) {
    double exec_price = apply_cost_model(static_cast<double>(tick.last_price),
                                         intent.qty, intent.side);

    if (open_qty_ == 0) {
        // ── Open new position ─────────────────────────────────────────────
        open_qty_          = intent.qty * intent.side;
        open_entry_price_  = exec_price;
        open_entry_ts_     = tick.timestamp_ns;
        open_side_         = intent.side;
        open_instrument_   = tick.instrument_token;

    } else if (intent.side == -open_side_ || intent.reason == 2) {
        // ── Close / reverse position ──────────────────────────────────────
        double gross_pnl = static_cast<double>(open_side_) *
                           static_cast<double>(std::abs(open_qty_)) *
                           (exec_price - open_entry_price_);

        // Cost on entry was already applied; apply cost on exit too
        double exit_cost = apply_cost_model(exec_price, std::abs(open_qty_), intent.side)
                         - exec_price;  // cost delta
        double commission = std::abs(exit_cost) * std::abs(open_qty_);
        // Flat brokerage: both legs
        commission += cfg_.brokerage_flat * 2.0;
        commission += commission * cfg_.gst_rate;  // GST on brokerage

        Trade t{};
        t.entry_ts_ns      = open_entry_ts_;
        t.exit_ts_ns       = tick.timestamp_ns;
        t.instrument_token = open_instrument_;
        t.entry_price      = open_entry_price_;
        t.exit_price       = exec_price;
        t.qty              = std::abs(open_qty_);
        t.side             = open_side_;
        t.gross_pnl        = gross_pnl;
        t.commission       = commission;
        t.net_pnl          = gross_pnl - commission;
        aggregator_.record_trade(t);

        equity_ += t.net_pnl;
        if (equity_ > peak_equity_) peak_equity_ = equity_;
        aggregator_.record_equity(tick.timestamp_ns, equity_);

        // Reset position
        open_qty_         = 0;
        open_entry_price_ = 0.0;
        open_entry_ts_    = 0;
        open_side_        = 0;
        open_instrument_  = 0;
    }
}

double SimRunner::apply_cost_model(double price, int32_t qty, int32_t side) const {
    // Slippage: shift price adversely by slippage_bps
    double slip = price * cfg_.slippage_bps / 10000.0;
    double exec_price = (side > 0) ? (price + slip) : (price - slip);

    // STT on sell side (0.1% of turnover)
    if (side < 0) {
        double stt = exec_price * std::abs(qty) * cfg_.stt_rate;
        exec_price -= stt / std::abs(qty);  // Amortize STT into per-share price
    }

    // Exchange transaction charge + SEBI
    double turnover = exec_price * std::abs(qty);
    double exch_charge = turnover * (cfg_.exchange_txn_charge + cfg_.sebi_charge);
    exec_price -= (side > 0 ? 1.0 : -1.0) * exch_charge / std::abs(qty);

    return exec_price;
}

} // namespace backtest
} // namespace alpha
