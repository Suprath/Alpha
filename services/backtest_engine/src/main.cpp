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

// ── Example signal-aware strategy ────────────────────────────────────────────
//
// Uses pre-computed BacktestSignal (RSI + MACD) when available.
// Falls back to VWAP deviation when signals are not yet warmed up.
// In production, swap this lambda for a call into strategy_engine batch mode.
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

    // ── Signal-based logic (preferred when warmed up) ─────────────────────────
    if (sig && sig->valid_rsi && sig->valid_macd) {
        // RSI oversold + MACD bullish crossover → long entry
        if (sig->rsi_14 < 35.0 && sig->macd_histogram > 0.0 && sig->macd_line < 0.0) {
            intent.side       = 1;
            intent.qty        = 1;
            intent.reason     = 1;   // ENTRY
            intent.confidence = std::min(1.0, (35.0 - sig->rsi_14) / 35.0 + 0.3);
            return intent;
        }
        // RSI overbought + MACD bearish crossover → short/exit
        if (sig->rsi_14 > 65.0 && sig->macd_histogram < 0.0 && sig->macd_line > 0.0) {
            intent.side       = -1;
            intent.qty        = 1;
            intent.reason     = 2;   // EXIT
            intent.confidence = std::min(1.0, (sig->rsi_14 - 65.0) / 35.0 + 0.3);
            return intent;
        }
        // Bollinger band mean-reversion (when BB valid too)
        if (sig->valid_bb) {
            if (sig->bb_pct_b < 0.05) {          // Touching lower band
                intent.side = 1; intent.qty = 1; intent.reason = 1;
                intent.confidence = 0.5;
                return intent;
            }
            if (sig->bb_pct_b > 0.95) {          // Touching upper band
                intent.side = -1; intent.qty = 1; intent.reason = 2;
                intent.confidence = 0.5;
                return intent;
            }
        }
        return intent;  // Signals valid but no actionable setup — no trade
    }

    // ── Fallback: VWAP deviation (warmup period or no signals loaded) ─────────
    const float spread = tick.ask_price - tick.bid_price;
    const bool  liquid = (tick.ask_price > 0.0f && tick.last_price > 0.0f &&
                          spread / tick.last_price < 0.005f);
    if (!liquid) return intent;

    if (tick.last_price > tick.vwap * 1.0005f) {
        intent.side = 1; intent.qty = 1; intent.reason = 1; intent.confidence = 0.4;
    } else if (tick.last_price < tick.vwap * 0.9995f) {
        intent.side = -1; intent.qty = 1; intent.reason = 2; intent.confidence = 0.4;
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
