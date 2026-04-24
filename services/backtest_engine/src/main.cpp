#include <alpha/backtest/SimRunner.hpp>
#include <alpha/backtest/TickLoader.hpp>
#include <alpha/backtest/BacktestClock.hpp>
#include <alpha/backtest/ResultAggregator.hpp>
#include <alpha/models/BacktestTick.hpp>
#include <alpha/models/MarketModels.hpp>
#include "BacktestRedisPublisher.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

static std::string get_env(const char* key, const char* def = "") {
    const char* v = std::getenv(key);
    return v ? v : def;
}

// ── Strategy factory — stateful mean-reversion + momentum ───────────────────
//
// Returns a lambda that captures per-instrument position state.  Must be
// called once per instrument (inside the file loop) so state resets cleanly.
//
// Design:
//   • Long-only, single position at a time.
//   • Entry: RSI(14) < 30 (deep oversold) AND MACD histogram just turned
//     positive (momentum inflecting bullish) AND BB %B < 0.25 (near lower
//     band).  Three-way confirmation keeps false-entry rate low.
//   • Exit: RSI > 70 OR BB %B > 0.92 OR hard stop-loss -1.5%.
//   • Min hold: 15 bars (~15 min) — prevents exit on first noise spike.
//   • Max hold: 120 bars (~2 h) — forces close if trade doesn't play out.
//   • Cooldown: 20 bars after any exit before next entry — avoids whipsaws.
//   • No warmup fallback: VWAP ±0.1% triggered on every bar.  Silence the
//     strategy during the 35-bar MACD warmup; better than thrashing.
//   • Sizing: ₹1L per trade (~10% of ₹10L capital) for meaningful P&L.
static auto make_strategy() {
    // Per-instrument mutable state (reset each time make_strategy() is called)
    struct State {
        bool    in_position   = false;
        double  entry_px      = 0.0;
        int32_t entry_qty     = 0;
        int32_t position_bars = 0;   // bars held so far
        int32_t cooldown_bars = 0;   // bars until next entry allowed
    };

    auto state = std::make_shared<State>();

    return [state](
        const alpha::models::BacktestTick&    tick,
        const alpha::backtest::BacktestClock& /*clock*/,
        const alpha::models::BacktestSignal*  sig
    ) -> alpha::models::OrderIntent {

        alpha::models::OrderIntent intent{};
        intent.timestamp_ns     = tick.timestamp_ns;
        intent.instrument_token = tick.instrument_token;
        intent.price            = static_cast<double>(tick.last_price);
        intent.strategy_id      = 1;

        const double price = static_cast<double>(tick.last_price);
        if (price <= 0.0) return intent;

        // ₹1L per entry — 10% of ₹10L capital, meaningful per-trade P&L
        const int qty = std::max(1, static_cast<int>(100000.0 / price));

        auto emit_exit = [&](int32_t cooldown) -> alpha::models::OrderIntent {
            intent.side           = -1;
            intent.qty            = state->entry_qty; // Use stored entry qty
            intent.reason         = 2;   // EXIT
            intent.confidence     = 0.80;
            state->in_position    = false;
            state->entry_px       = 0.0;
            state->entry_qty      = 0;
            state->position_bars  = 0;
            state->cooldown_bars  = cooldown;
            return intent;
        };

        // ── In-position management ─────────────────────────────────────────────
        if (state->in_position) {
            ++state->position_bars;

            // Hard stop-loss: -1.5% from entry
            if (price < state->entry_px * 0.985) {
                return emit_exit(30);   // 30-bar cooldown after a stop
            }

            // Signal-based exit — only after min-hold (30 bars) to avoid noise
            if (state->position_bars >= 30 && sig && sig->valid_rsi) {
                const bool rsi_exit = (sig->rsi_14 > 75.0);
                const bool bb_exit  = (sig->valid_bb && sig->bb_pct_b > 0.95);
                if (rsi_exit || bb_exit) {
                    return emit_exit(20);
                }
            }

            // Time-based exit: close after 2 h (120 bars) regardless
            if (state->position_bars >= 120) {
                return emit_exit(20);
            }

            return intent;  // hold
        }

        // ── Cooldown between trades ────────────────────────────────────────────
        if (state->cooldown_bars > 0) {
            --state->cooldown_bars;
            return intent;
        }

        // ── Entry — requires full signal set (past MACD warmup) ───────────────
        // Silence strategy during the first 35-bar warmup rather than falling
        // back to the noisy VWAP ±0.1% threshold.
        if (!sig || !sig->valid_rsi || !sig->valid_macd) return intent;

        const double rsi  = sig->rsi_14;
        const double hist = sig->macd_histogram;

        // Primary setup: deep oversold + momentum inflecting + near lower BB
        // RSI < 25: stock is genuinely oversold
        // hist > 0: MACD histogram turned positive — buying pressure growing
        // bb_pct_b < 0.15: price in the extreme lower quarter/edge of band
        // volume > 100: ensure liquidity
        if (rsi < 25.0 && hist > 0.0 && tick.volume > 100) {
            const bool bb_ok = !sig->valid_bb || sig->bb_pct_b < 0.15;
            if (bb_ok) {
                intent.side          = 1;
                intent.qty           = qty;
                intent.reason        = 1;   // ENTRY
                intent.confidence    = std::min(1.0, 0.50 + (30.0 - rsi) / 30.0 * 0.40);
                state->in_position   = true;
                state->entry_px      = price;
                state->entry_qty     = qty;
                state->position_bars = 0;
                state->cooldown_bars = 0;
                return intent;
            }
        }

        // Secondary: BB lower-band pierce — strong mean-reversion signal
        // bb_pct_b < 0.02: price at or below 2-sigma lower band edge
        // rsi < 40: not overbought; hist > -0.2: MACD not in free-fall
        if (sig->valid_bb && sig->bb_pct_b < 0.02 && rsi < 40.0 && hist > -0.2 && tick.volume > 100) {
            intent.side          = 1;
            intent.qty           = qty;
            intent.reason        = 1;
            intent.confidence    = 0.65;
            state->in_position   = true;
            state->entry_px      = price;
            state->entry_qty     = qty;
            state->position_bars = 0;
            state->cooldown_bars = 0;
            return intent;
        }

        return intent;  // no actionable setup
    };
}

// ── Alpha Strategy factory — aligns with live StrategyEngine.cpp logic ───────
//
// Uses the pre-computed composite_score and kelly_fraction from BacktestSignal.
// Implements the same hysteresis and minimum trade size logic as the live bot.
static auto make_alpha_strategy() {
    struct State {
        int32_t current_qty   = 0;
        double  last_kelly    = 0.0;
    };
    auto state = std::make_shared<State>();

    const int32_t MAX_UNITS = 100;
    const int32_t MIN_TRADE = 10;
    const double  HYST      = 0.05;

    return [state, MAX_UNITS, MIN_TRADE, HYST](
        const alpha::models::BacktestTick&    tick,
        const alpha::backtest::BacktestClock& /*clock*/,
        const alpha::models::BacktestSignal*  sig
    ) -> alpha::models::OrderIntent {

        alpha::models::OrderIntent intent{};
        intent.timestamp_ns     = tick.timestamp_ns;
        intent.instrument_token = tick.instrument_token;
        intent.price            = static_cast<double>(tick.last_price);
        intent.strategy_id      = 4; // CompositeScore

        if (!sig || !sig->valid_composite) return intent;

        const int32_t action      = (sig->composite_score > 0.0) ? 1 : -1;
        const int32_t desired_qty = action * static_cast<int32_t>(std::round(MAX_UNITS * sig->kelly_fraction));
        const int32_t delta       = desired_qty - state->current_qty;

        if (delta == 0) return intent;

        // ── Hysteresis & Min Trade Size (Sync with StrategyEngine.cpp) ────────
        bool is_flipping = (state->current_qty > 0 && desired_qty < 0) || 
                           (state->current_qty < 0 && desired_qty > 0);
        bool significant = std::abs(delta) >= MIN_TRADE || 
                           std::abs(sig->kelly_fraction - state->last_kelly) >= HYST;

        if (is_flipping || significant) {
            intent.qty        = std::abs(delta);
            intent.side       = (delta > 0) ? 1 : -1;
            intent.reason     = (state->current_qty == 0) ? 1 : (is_flipping ? 3 : 4); // ENTRY, REVERSE, SCALE
            intent.confidence = sig->kelly_fraction;
            
            state->current_qty += delta;
            state->last_kelly   = sig->kelly_fraction;
            return intent;
        }

        return intent;
    };
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const std::string data_dir   = get_env("BACKTEST_DATA_DIR", "/data/backtest");
    const std::string qdb_host   = get_env("QUESTDB_HOST", "questdb");
    const std::string qdb_pg_dsn = "host=" + qdb_host + " port=8812 dbname=qdb user=admin password=quest";

    // ── Mode: export or run ──────────────────────────────────────────────
    std::string mode = (argc > 1) ? argv[1] : "run";

    if (mode == "export") {
        // Export QuestDB data to .alpha binary files for fast mmap replay
        if (argc < 6) {
            std::cerr << "Usage: backtest export <instrument_token> <start_ns> <end_ns> <output_dir>\n";
            return 1;
        }
        uint32_t token    = static_cast<uint32_t>(std::stoul(argv[2]));
        uint64_t start_ns = std::stoull(argv[3]);
        uint64_t end_ns   = std::stoull(argv[4]);
        std::string outdir = argv[5];

        alpha::backtest::TickLoader loader(alpha::backtest::LoaderMode::QUESTDB_SQL);
        std::string outpath = outdir + "/" + std::to_string(token) + ".alpha";

        std::cout << "[backtest] Exporting token=" << token
                  << " to " << outpath << " ..." << std::flush;
        size_t n = loader.export_to_alpha_file(qdb_pg_dsn, token, start_ns, end_ns, outpath);
        std::cout << " " << n << " ticks." << std::endl;
        return 0;
    }

    // ── Redis publisher (non-blocking — failures are logged, not fatal) ──────
    const std::string redis_host = get_env("REDIS_HOST", "redis");
    const int         redis_port = std::stoi(get_env("REDIS_PORT", "6379"));
    alpha::backtest::BacktestRedisPublisher publisher;
    publisher.connect(redis_host, redis_port);

    // ── Mode: run backtest on all .alpha files in data_dir ────────────────
    alpha::backtest::SimConfig cfg;
    cfg.starting_capital  = std::stod(get_env("STARTING_CAPITAL", "1000000"));
    cfg.brokerage_flat    = std::stod(get_env("BROKERAGE_FLAT", "20"));
    cfg.slippage_bps      = std::stod(get_env("SLIPPAGE_BPS", "2"));
    cfg.batch_size        = static_cast<size_t>(std::stoul(get_env("BATCH_SIZE", "1024")));

    std::cout << "[backtest] Starting simulation. capital=₹" << cfg.starting_capital
              << " brokerage=₹" << cfg.brokerage_flat
              << " slippage=" << cfg.slippage_bps << "bps"
              << " batch=" << cfg.batch_size << std::endl;

    // Collect .alpha files
    std::vector<std::string> files;
    if (fs::exists(data_dir)) {
        for (const auto& entry : fs::directory_iterator(data_dir)) {
            if (entry.path().extension() == ".alpha")
                files.push_back(entry.path().string());
        }
    }

    if (files.empty()) {
        std::cerr << "[backtest] No .alpha files found in " << data_dir
                  << ". Run with 'export' mode first." << std::endl;
        publisher.publish_status(
            alpha::backtest::BacktestRunState::BACKTEST_ERROR, 0, 0, 0, "", "No .alpha files found");
        return 1;
    }

    std::cout << "[backtest] Found " << files.size() << " .alpha files." << std::endl;

    // Run backtest on each file (one instrument per file)
    for (const auto& file : files) {
        const std::string filename = fs::path(file).filename().string();
        const std::string stem     = fs::path(file).stem().string();
        std::cout << "[backtest] Running: " << filename << std::endl;

        uint32_t token = 0;
        try { token = static_cast<uint32_t>(std::stoul(stem)); } catch (...) {}
        const std::string symbol = "TOKEN_" + stem;

        publisher.publish_status(
            alpha::backtest::BacktestRunState::BACKTEST_LOADING,
            token, 0, 0, symbol, "Loading ticks and signals");

        // Select strategy: standard RSI/MACD or the live-aligned Alpha strategy
        alpha::backtest::SimRunner::StrategyFn strategy_fn;
        if (get_env("USE_ALPHA_STRATEGY") == "1") {
            strategy_fn = make_alpha_strategy();
        } else {
            strategy_fn = make_strategy();
        }

        alpha::backtest::SimRunner runner(cfg, std::move(strategy_fn));

        try {
            // Load pre-computed signals from QuestDB (graceful fallback if absent)
            alpha::backtest::SignalMap signals;
            try {
                if (token > 0) {
                    alpha::backtest::TickLoader sig_loader(alpha::backtest::LoaderMode::QUESTDB_SQL);
                    signals = sig_loader.load_signals_as_map(qdb_pg_dsn, token, 0, UINT64_MAX);
                    std::cout << "[backtest] Loaded " << signals.size()
                              << " signal bars for token " << token << std::endl;
                }
            } catch (...) {
                std::cout << "[backtest] No pre-computed signals — running without signals\n";
            }

            publisher.publish_status(
                alpha::backtest::BacktestRunState::BACKTEST_RUNNING,
                token, 0, 0, symbol, "Simulation running");

            alpha::backtest::BacktestResult result = runner.run(file, signals);
            result.print();

            const double final_eq = result.final_equity;
            publisher.publish_result(result, token, symbol, cfg.starting_capital, final_eq);
            publisher.publish_status(
                alpha::backtest::BacktestRunState::BACKTEST_DONE,
                token, 0, 0, symbol, "Completed");

        } catch (const std::exception& e) {
            std::cerr << "[backtest] ERROR on " << file << ": " << e.what() << std::endl;
            publisher.publish_status(
                alpha::backtest::BacktestRunState::BACKTEST_ERROR,
                token, 0, 0, symbol, e.what());
        }
    }

    return 0;
}
