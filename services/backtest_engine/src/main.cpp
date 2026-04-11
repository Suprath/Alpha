#include <alpha/backtest/SimRunner.hpp>
#include <alpha/backtest/TickLoader.hpp>
#include <alpha/backtest/BacktestClock.hpp>
#include <alpha/backtest/ResultAggregator.hpp>
#include <alpha/models/BacktestTick.hpp>
#include <alpha/models/MarketModels.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

static std::string get_env(const char* key, const char* def = "") {
    const char* v = std::getenv(key);
    return v ? v : def;
}

// ── Example momentum strategy (replace with real strategy engine hook) ────
//
// This stub demonstrates the strategy callback signature.
// In production, this calls into services/strategy_engine in Backtest_Mode.
//
// Simple logic: buy when price > vwap, sell when price < vwap.
static alpha::models::OrderIntent momentum_strategy(
    const alpha::models::BacktestTick& tick,
    const alpha::backtest::BacktestClock& /*clock*/)
{
    alpha::models::OrderIntent intent{};
    intent.timestamp_ns     = tick.timestamp_ns;
    intent.instrument_token = tick.instrument_token;
    intent.price            = static_cast<double>(tick.last_price);
    intent.strategy_id      = 1;

    float spread = tick.ask_price - tick.bid_price;

    // Only trade if spread is reasonable (< 50 bps)
    bool liquid = (tick.ask_price > 0.0f && spread / tick.last_price < 0.005f);

    if (!liquid) {
        intent.action = 0;
        return intent;
    }

    if (tick.last_price > tick.vwap * 1.0005f) {
        // Price above VWAP — long signal
        intent.action     = 1;
        intent.side       = 1;
        intent.qty        = 1;
        intent.reason     = 1;   // ENTRY
        intent.confidence = 0.6;
    } else if (tick.last_price < tick.vwap * 0.9995f) {
        // Price below VWAP — short/exit signal
        intent.action     = -1;
        intent.side       = -1;
        intent.qty        = 1;
        intent.reason     = 2;   // EXIT
        intent.confidence = 0.6;
    } else {
        intent.action = 0;
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
        return 1;
    }

    std::cout << "[backtest] Found " << files.size() << " .alpha files." << std::endl;

    // Run backtest on each file (one instrument per file)
    for (const auto& file : files) {
        std::cout << "[backtest] Running: " << fs::path(file).filename().string() << std::endl;

        alpha::backtest::SimRunner runner(cfg, momentum_strategy);

        try {
            alpha::backtest::BacktestResult result = runner.run(file);
            result.print();
        } catch (const std::exception& e) {
            std::cerr << "[backtest] ERROR on " << file << ": " << e.what() << std::endl;
        }
    }

    return 0;
}
