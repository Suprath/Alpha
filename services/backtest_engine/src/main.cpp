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
#include <cstdlib>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

static std::string get_env(const char* key, const char* def = "") {
    const char* v = std::getenv(key);
    return v ? v : def;
}

// ── Mean-reversion + momentum strategy targeting 10-20% annual returns ──────
//
// Design principles:
//   • Long-only: NSE equity CNC segment — no overnight short selling.
//   • Capital-proportional sizing: allocate ₹1L (~10% of ₹10L capital) per
//     trade so each position is meaningful relative to total equity.
//   • Multi-signal confirmation: require RSI oversold + MACD momentum shift
//     (histogram turning positive) to reduce false entries.
//   • BB refinement: lower-band touch adds conviction for mean-reversion;
//     upper-band touch or RSI overbought triggers clean exit.
//   • Exit discipline: sell on RSI > 65 OR BB %B > 0.88 — lock in gains
//     before momentum reverses, keep avg winner larger than avg loser.
//   • Warmup fallback: VWAP mean-reversion with half-sized allocation until
//     35-bar signal warmup completes (MACD is the slowest indicator).
static alpha::models::OrderIntent signal_strategy(
    const alpha::models::BacktestTick&    tick,
    const alpha::backtest::BacktestClock& /*clock*/,
    const alpha::models::BacktestSignal*  sig)
{
    alpha::models::OrderIntent intent{};
    intent.timestamp_ns     = tick.timestamp_ns;
    intent.instrument_token = tick.instrument_token;
    intent.price            = static_cast<double>(tick.last_price);
    intent.strategy_id      = 1;

    const double price = static_cast<double>(tick.last_price);
    if (price <= 0.0) return intent;

    // Allocate ₹1,00,000 per entry (10% of ₹10L starting capital).
    // Ensures meaningful P&L per trade even for high-priced stocks.
    const int qty = std::max(1, static_cast<int>(100000.0 / price));

    // ── Signal-based logic (active after 35-bar MACD warmup) ──────────────────
    if (sig && sig->valid_rsi && sig->valid_macd) {
        const double rsi  = sig->rsi_14;
        const double hist = sig->macd_histogram;

        // ── LONG ENTRY ─────────────────────────────────────────────────────────
        // Condition: RSI recovering from oversold (<40) AND MACD histogram has
        // turned positive (momentum shifting bullish). Optional BB confirmation:
        // price in lower half of band signals we are still close to value.
        if (rsi < 40.0 && hist > 0.0) {
            const bool bb_ok = !sig->valid_bb || sig->bb_pct_b < 0.5;
            if (bb_ok) {
                intent.side       = 1;
                intent.qty        = qty;
                intent.reason     = 1;   // ENTRY
                intent.confidence = std::min(1.0, 0.40 + (40.0 - rsi) / 40.0 * 0.50);
                return intent;
            }
        }

        // BB lower-band touch (strong mean-reversion setup):
        // price near/below lower band AND not already overbought on RSI.
        if (sig->valid_bb && sig->bb_pct_b < 0.08 && rsi < 55.0) {
            intent.side       = 1;
            intent.qty        = qty;
            intent.reason     = 1;
            intent.confidence = 0.60;
            return intent;
        }

        // ── LONG EXIT ──────────────────────────────────────────────────────────
        // Exit when either RSI signals overbought or price tags the upper BB.
        // Both indicate the mean-reversion move has largely played out.
        const bool rsi_exit = (rsi > 65.0);
        const bool bb_exit  = (sig->valid_bb && sig->bb_pct_b > 0.88);
        if (rsi_exit || bb_exit) {
            intent.side       = -1;
            intent.qty        = qty;
            intent.reason     = 2;   // EXIT
            intent.confidence = rsi_exit
                ? std::min(1.0, 0.40 + (rsi - 65.0) / 35.0 * 0.50)
                : 0.55;
            return intent;
        }

        return intent;  // Signals valid but no actionable setup — hold or flat
    }

    // ── Fallback: VWAP mean-reversion (warmup / no signals loaded) ───────────
    // During warmup use half-sized allocation to limit early-bar exposure.
    const float spread = tick.ask_price - tick.bid_price;
    const bool  liquid = (tick.ask_price > 0.0f && tick.last_price > 0.0f &&
                          spread / tick.last_price < 0.005f);
    if (!liquid) return intent;

    const int fqty = std::max(1, static_cast<int>(50000.0 / price));

    // Buy below VWAP (undervalued intraday), sell above VWAP (take profit)
    if (tick.last_price < tick.vwap * 0.9990f) {
        intent.side = 1;  intent.qty = fqty; intent.reason = 1; intent.confidence = 0.35;
    } else if (tick.last_price > tick.vwap * 1.0010f) {
        intent.side = -1; intent.qty = fqty; intent.reason = 2; intent.confidence = 0.35;
    }
    return intent;
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

        alpha::backtest::SimRunner runner(cfg, signal_strategy);

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

            const double final_eq = cfg.starting_capital + result.total_net_pnl;
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
