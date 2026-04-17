/**
 * @file backtest_engine_test.cpp
 * @brief Unit tests for backtest_engine modules.
 *
 * Covers:
 *   ResultAggregator — P&L aggregation, drawdown, Sharpe/Sortino, trade stats
 *   SimRunner        — hot-loop, strategy callback, cost model, round-trip PnL
 */

#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>

#include <alpha/backtest/ResultAggregator.hpp>
#include <alpha/backtest/SimRunner.hpp>
#include <alpha/models/BacktestTick.hpp>
#include <alpha/models/BacktestSignal.hpp>

using namespace alpha::backtest;
using namespace alpha::models;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static Trade make_trade(double gross, double commission, double net) {
    Trade t{};
    t.gross_pnl  = gross;
    t.commission = commission;
    t.net_pnl    = net;
    return t;
}

static BacktestTick make_bt_tick(uint32_t token, float price,
                                  uint64_t ts_ns = 0, uint32_t vol = 100) {
    BacktestTick t{};
    t.instrument_token = token;
    t.last_price       = price;
    t.bid_price        = price - 0.05f;
    t.ask_price        = price + 0.05f;
    t.vwap             = price;
    t.volume           = vol;
    t.timestamp_ns     = ts_ns;
    return t;
}

// ─── ResultAggregator Tests ───────────────────────────────────────────────────

TEST(ResultAggregatorTest, EmptyReturnsZeroResult) {
    ResultAggregator agg;
    auto r = agg.compute();
    EXPECT_EQ(r.total_trades, 0u);
    EXPECT_EQ(r.total_net_pnl, 0.0);
    EXPECT_EQ(r.total_gross_pnl, 0.0);
    EXPECT_EQ(r.win_rate, 0.0);
    EXPECT_EQ(r.max_drawdown, 0.0);
}

TEST(ResultAggregatorTest, SingleWinningTrade) {
    ResultAggregator agg;
    agg.record_trade(make_trade(200.0, 10.0, 190.0));
    auto r = agg.compute();
    EXPECT_EQ(r.total_trades, 1u);
    EXPECT_EQ(r.winning_trades, 1u);
    EXPECT_EQ(r.losing_trades, 0u);
    EXPECT_NEAR(r.total_net_pnl, 190.0, 1e-10);
    EXPECT_NEAR(r.total_gross_pnl, 200.0, 1e-10);
    EXPECT_NEAR(r.total_commission, 10.0, 1e-10);
    EXPECT_NEAR(r.win_rate, 1.0, 1e-10);
    EXPECT_NEAR(r.avg_win, 190.0, 1e-10);
    EXPECT_NEAR(r.largest_win, 190.0, 1e-10);
}

TEST(ResultAggregatorTest, SingleLosingTrade) {
    ResultAggregator agg;
    agg.record_trade(make_trade(-150.0, 10.0, -160.0));
    auto r = agg.compute();
    EXPECT_EQ(r.total_trades, 1u);
    EXPECT_EQ(r.winning_trades, 0u);
    EXPECT_EQ(r.losing_trades, 1u);
    EXPECT_NEAR(r.total_net_pnl, -160.0, 1e-10);
    EXPECT_NEAR(r.win_rate, 0.0, 1e-10);
    EXPECT_NEAR(r.largest_loss, -160.0, 1e-10);
}

TEST(ResultAggregatorTest, WinRateFormula) {
    ResultAggregator agg;
    agg.record_trade(make_trade( 100.0, 5.0,  95.0));  // win
    agg.record_trade(make_trade(-100.0, 5.0, -105.0)); // loss
    agg.record_trade(make_trade( 200.0, 5.0,  195.0)); // win
    agg.record_trade(make_trade(-50.0,  5.0,  -55.0)); // loss
    auto r = agg.compute();
    EXPECT_EQ(r.total_trades, 4u);
    EXPECT_EQ(r.winning_trades, 2u);
    EXPECT_EQ(r.losing_trades, 2u);
    EXPECT_NEAR(r.win_rate, 0.5, 1e-10);
}

TEST(ResultAggregatorTest, ProfitFactorGrossOverGrossLoss) {
    ResultAggregator agg;
    // gross_profit = 300+200 = 500, gross_loss = |(-100)|+|(-50)| = 150
    agg.record_trade(make_trade(300.0, 0.0,  300.0));
    agg.record_trade(make_trade(200.0, 0.0,  200.0));
    agg.record_trade(make_trade(-100.0, 0.0, -100.0));
    agg.record_trade(make_trade(-50.0,  0.0,  -50.0));
    auto r = agg.compute();
    EXPECT_NEAR(r.profit_factor, 500.0 / 150.0, 1e-10);
}

TEST(ResultAggregatorTest, ProfitFactorInfiniteWhenNoLosses) {
    ResultAggregator agg;
    agg.record_trade(make_trade(500.0, 0.0, 500.0));
    auto r = agg.compute();
    EXPECT_TRUE(std::isinf(r.profit_factor));
}

TEST(ResultAggregatorTest, AvgTradeAndAvgWinLoss) {
    ResultAggregator agg;
    agg.record_trade(make_trade(100.0, 0.0,  100.0));  // win
    agg.record_trade(make_trade(-60.0, 0.0,  -60.0));  // loss
    auto r = agg.compute();
    EXPECT_NEAR(r.avg_trade_pnl, (100.0 + (-60.0)) / 2.0, 1e-10);
    EXPECT_NEAR(r.avg_win,  100.0, 1e-10);
    EXPECT_NEAR(r.avg_loss,  -60.0, 1e-10);
}

TEST(ResultAggregatorTest, ExpectancyFormula) {
    // expectancy = win_rate * avg_win + (1 - win_rate) * avg_loss
    // avg_loss is stored negative in BacktestResult
    ResultAggregator agg;
    agg.record_trade(make_trade(100.0, 0.0,  100.0));
    agg.record_trade(make_trade(-40.0, 0.0,  -40.0));
    auto r = agg.compute();
    double expected = r.win_rate * r.avg_win + (1.0 - r.win_rate) * r.avg_loss;
    EXPECT_NEAR(r.expectancy, expected, 1e-10);
}

TEST(ResultAggregatorTest, MaxDrawdownFromEquityCurve) {
    ResultAggregator agg;
    agg.record_trade(make_trade(0.0, 0.0, 0.0));  // need at least one trade

    // Equity: 1000 → 1200 → 900 → 1100
    // Peak after 1200 is 1200; trough at 900 → drawdown = 300
    agg.record_equity(1000, 1000.0);
    agg.record_equity(2000, 1200.0);
    agg.record_equity(3000,  900.0);
    agg.record_equity(4000, 1100.0);
    auto r = agg.compute();
    EXPECT_NEAR(r.max_drawdown, 300.0, 1e-6);
    EXPECT_NEAR(r.max_drawdown_pct, 300.0 / 1200.0 * 100.0, 1e-6);
}

TEST(ResultAggregatorTest, NoDrawdownOnMonotonicallyRisingEquity) {
    ResultAggregator agg;
    agg.record_trade(make_trade(0.0, 0.0, 0.0));
    for (uint64_t i = 0; i < 10; ++i)
        agg.record_equity(i * 1000, 1000.0 + i * 100.0);
    auto r = agg.compute();
    EXPECT_NEAR(r.max_drawdown, 0.0, 1e-6);
}

TEST(ResultAggregatorTest, ResetClearsAllState) {
    ResultAggregator agg;
    agg.record_trade(make_trade(500.0, 0.0, 500.0));
    agg.record_equity(0, 1500.0);
    agg.reset();
    auto r = agg.compute();
    EXPECT_EQ(r.total_trades, 0u);
    EXPECT_EQ(r.total_net_pnl, 0.0);
}

// ─── SimRunner Tests ──────────────────────────────────────────────────────────

// Strategy that never trades
static OrderIntent no_trade_strategy(const BacktestTick&,
                                     const BacktestClock&,
                                     const BacktestSignal*) {
    return {};  // qty=0 → no action
}

TEST(SimRunnerTest, EmptyTickListReturnsZeroResult) {
    SimConfig cfg{};
    SimRunner runner(cfg, no_trade_strategy);
    auto r = runner.run(nullptr, 0);
    EXPECT_EQ(r.total_trades, 0u);
    EXPECT_EQ(r.total_net_pnl, 0.0);
}

TEST(SimRunnerTest, NoTradeStrategyProducesNoTrades) {
    SimConfig cfg{};
    cfg.slippage_bps = 0.0;
    SimRunner runner(cfg, no_trade_strategy);

    std::vector<BacktestTick> ticks;
    for (uint32_t i = 0; i < 10; ++i)
        ticks.push_back(make_bt_tick(100001, 500.0f + i, i * 1'000'000'000ULL));

    auto r = runner.run(ticks.data(), ticks.size());
    EXPECT_EQ(r.total_trades, 0u);
}

TEST(SimRunnerTest, StrategyReceivesCorrectTickAndSignal) {
    // Verify the strategy callback receives the same tick that was passed in
    uint32_t received_token = 0;
    bool     signal_was_null = false;

    auto spy_strategy = [&](const BacktestTick& tick,
                             const BacktestClock&,
                             const BacktestSignal* sig) -> OrderIntent {
        received_token   = tick.instrument_token;
        signal_was_null  = (sig == nullptr);
        return {};
    };

    SimConfig cfg{};
    SimRunner runner(cfg, spy_strategy);

    BacktestTick tick = make_bt_tick(999888, 1234.5f, 1'000'000'000ULL);
    runner.run(&tick, 1);

    EXPECT_EQ(received_token, 999888u);
    EXPECT_TRUE(signal_was_null);  // No signals provided → nullptr
}

TEST(SimRunnerTest, BuySellRoundTripProducesOneTrade) {
    // Tick 0: strategy returns BUY
    // Tick 1: strategy returns SELL → closes position → one trade recorded
    int call_count = 0;
    auto bt_strategy = [&](const BacktestTick& tick,
                            const BacktestClock&,
                            const BacktestSignal*) -> OrderIntent {
        OrderIntent intent{};
        intent.instrument_token = tick.instrument_token;
        intent.price            = static_cast<double>(tick.last_price);
        intent.qty              = 10;
        intent.timestamp_ns     = tick.timestamp_ns;
        std::strncpy(intent.symbol, "NIFTY", sizeof(intent.symbol));

        if (call_count == 0)       intent.side = 1;   // BUY on first tick
        else if (call_count == 1)  intent.side = -1;  // SELL on second tick
        else                       intent.side = 0;   // No action after

        ++call_count;
        return intent;
    };

    SimConfig cfg{};
    cfg.starting_capital = 1'000'000.0;
    cfg.slippage_bps     = 0.0;  // Zero slippage for deterministic PnL
    cfg.brokerage_flat   = 0.0;
    cfg.stt_rate         = 0.0;
    cfg.exchange_txn_charge = 0.0;
    cfg.sebi_charge      = 0.0;
    cfg.gst_rate         = 0.0;

    SimRunner runner(cfg, bt_strategy);

    std::vector<BacktestTick> ticks = {
        make_bt_tick(100002, 100.0f, 0),
        make_bt_tick(100002, 110.0f, 60'000'000'000ULL),  // +10 points = +100 gross
        make_bt_tick(100002, 110.0f, 120'000'000'000ULL),
    };

    auto r = runner.run(ticks.data(), ticks.size());
    EXPECT_EQ(r.total_trades, 1u);
    // With zero costs: net_pnl = (110 - 100) * 10 = 100
    EXPECT_NEAR(r.total_net_pnl, 100.0, 1.0);
}

TEST(SimRunnerTest, PositiveNetPnlForProfitableLong) {
    // Buy at 500, sell at 520 → gross = 20 * qty; net = gross − costs
    int tick_idx = 0;
    auto trend_strategy = [&](const BacktestTick& tick,
                               const BacktestClock&,
                               const BacktestSignal*) -> OrderIntent {
        OrderIntent intent{};
        intent.instrument_token = tick.instrument_token;
        intent.price            = static_cast<double>(tick.last_price);
        intent.qty              = 5;
        intent.timestamp_ns     = tick.timestamp_ns;
        std::strncpy(intent.symbol, "TEST", sizeof(intent.symbol));

        if (tick_idx == 0)      intent.side = 1;   // BUY
        else if (tick_idx == 1) intent.side = -1;  // SELL
        else                    intent.side = 0;

        ++tick_idx;
        return intent;
    };

    SimConfig cfg{};
    cfg.slippage_bps = 0.0;
    cfg.brokerage_flat = 0.0;
    cfg.stt_rate = 0.0;
    cfg.exchange_txn_charge = 0.0;
    cfg.sebi_charge = 0.0;
    cfg.gst_rate = 0.0;

    SimRunner runner(cfg, trend_strategy);

    std::vector<BacktestTick> ticks = {
        make_bt_tick(300001, 500.0f, 0),
        make_bt_tick(300001, 520.0f, 60'000'000'000ULL),
        make_bt_tick(300001, 520.0f, 120'000'000'000ULL),
    };

    auto r = runner.run(ticks.data(), ticks.size());
    EXPECT_EQ(r.total_trades, 1u);
    EXPECT_NEAR(r.total_net_pnl, 100.0, 1.0);  // (520-500)*5 = 100
    EXPECT_GT(r.win_rate, 0.0);
}

TEST(SimRunnerTest, ResetAllowsSecondRun) {
    int fired = 0;
    auto counting_strategy = [&](const BacktestTick&,
                                  const BacktestClock&,
                                  const BacktestSignal*) -> OrderIntent {
        ++fired;
        return {};
    };

    SimConfig cfg{};
    SimRunner runner(cfg, counting_strategy);

    BacktestTick tick = make_bt_tick(400001, 200.0f, 0);
    runner.run(&tick, 1);
    int after_first = fired;

    runner.reset();
    runner.run(&tick, 1);

    EXPECT_EQ(after_first, 1);
    EXPECT_EQ(fired, 2);  // Strategy called again after reset
}

TEST(SimRunnerTest, SignalMapPassedToStrategy) {
    bool signal_received = false;
    double received_rsi  = 0.0;

    auto signal_strategy = [&](const BacktestTick&,
                                const BacktestClock&,
                                const BacktestSignal* sig) -> OrderIntent {
        if (sig != nullptr) {
            signal_received = true;
            received_rsi    = sig->rsi_14;
        }
        return {};
    };

    SimConfig cfg{};
    SimRunner runner(cfg, signal_strategy);

    constexpr uint64_t ONE_MINUTE_NS = 60'000'000'000ULL;
    BacktestTick tick = make_bt_tick(500001, 300.0f, ONE_MINUTE_NS);

    BacktestSignal sig{};
    sig.timestamp_ns     = ONE_MINUTE_NS;
    sig.instrument_token = 500001;
    sig.rsi_14           = 65.5;
    sig.valid_rsi        = true;

    // Key must match (tick.timestamp_ns / ONE_MINUTE_NS) * ONE_MINUTE_NS
    SignalMap smap;
    smap[ONE_MINUTE_NS] = sig;

    runner.run(&tick, 1, smap);

    EXPECT_TRUE(signal_received);
    EXPECT_NEAR(received_rsi, 65.5, 1e-10);
}
