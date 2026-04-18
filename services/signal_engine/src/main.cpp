#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include <stdexcept>
#include "shm_reader/ShmReader.hpp"
#include "shm_writer/ShmWriter.hpp"
#include "core/QuestDBClient.hpp"
#include "core/EnhancedBar.hpp"
#include "pipelines/TickPipeline.hpp"
#include "pipelines/BarPipeline.hpp"
#include "pipelines/BacktestBarPipeline.hpp"
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>
#include <pqxx/pqxx>

using namespace alpha::signal;
using namespace alpha::signal::core;
using namespace alpha::signal::pipelines;
using namespace alpha::models;

static std::string get_env(const char* key, const char* def = "") {
    const char* v = std::getenv(key);
    return v ? v : def;
}

// ── Backtest batch mode ────────────────────────────────────────────────────────
// Reads OHLCV candles from QuestDB, computes OHLCV-only signals, writes
// one BacktestSignal row per bar back to QuestDB via ILP.
static int run_backtest_batch() {
    const std::string qdb_host   = get_env("QUESTDB_HOST", "questdb");
    const std::string symbol     = get_env("BACKTEST_SYMBOL", "");
    const uint32_t    token      = static_cast<uint32_t>(
                                       std::stoul(get_env("BACKTEST_INSTRUMENT_TOKEN", "0")));
    const uint64_t    start_ns   = std::stoull(get_env("BACKTEST_START_NS", "0"));
    const uint64_t    end_ns     = std::stoull(get_env("BACKTEST_END_NS", "0"));

    if (symbol.empty() || token == 0 || start_ns == 0 || end_ns == 0) {
        std::cerr << "[backtest-batch] Missing env: BACKTEST_SYMBOL, BACKTEST_INSTRUMENT_TOKEN, "
                     "BACKTEST_START_NS, BACKTEST_END_NS\n";
        return 1;
    }

    std::cout << "[backtest-batch] Starting for symbol=" << symbol
              << " token=" << token << std::endl;

    // ILP connection for writing signals
    QuestDBClient qdb(qdb_host, 9009);
    if (!qdb.connect()) {
        std::cerr << "[backtest-batch] Cannot connect to QuestDB ILP\n";
        return 1;
    }

    // Postgres wire for reading candles
    const std::string pg_dsn = "host=" + qdb_host +
                                " port=8812 dbname=qdb user=admin password=quest";
    pqxx::connection conn(pg_dsn);
    pqxx::work txn(conn);

    const uint64_t start_us = start_ns / 1000ULL;
    const uint64_t end_us   = end_ns   / 1000ULL;

    // Use typical price (H+L+C)/3 as vwap proxy — historical candles from
    // historical_feed.py do not include a vwap column (only live signal-engine
    // bars do).  This avoids a NULL/column-missing crash on backtest-only setups.
    const std::string sql =
        "SELECT cast(timestamp as long) as ts_us, open, high, low, close, volume,"
        " (high+low+close)/3.0 as vwap "
        "FROM candles "
        "WHERE symbol = " + txn.quote(symbol) +
        "  AND timestamp >= cast(" + std::to_string(start_us) + " as timestamp)"
        "  AND timestamp <= cast(" + std::to_string(end_us)   + " as timestamp)"
        " ORDER BY timestamp";

    const auto result = txn.exec(sql);
    std::cout << "[backtest-batch] Loaded " << result.size() << " candles\n";

    BacktestBarPipeline pipeline(qdb);

    for (const auto& row : result) {
        EnhancedBar bar{};
        bar.timestamp_ns     = row[0].as<uint64_t>() * 1000ULL;  // μs → ns
        bar.instrument_token = token;
        bar.open             = row[1].as<double>();
        bar.high             = row[2].as<double>();
        bar.low              = row[3].as<double>();
        bar.close            = row[4].as<double>();
        bar.volume           = row[5].as<uint64_t>();
        bar.vwap             = row[6].as<double>();
        // Orderflow fields unavailable from OHLCV — leave at 0
        bar.buy_volume  = 0;
        bar.sell_volume = 0;
        bar.bar_ofi     = 0.0f;

        pipeline.on_bar(bar);
    }

    std::cout << "[backtest-batch] Done. " << pipeline.bars_processed()
              << " signals written for " << symbol << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    // ── Batch mode: signal_engine --backtest-batch ─────────────────────────────
    if (argc > 1 && std::string(argv[1]) == "--backtest-batch") {
        try {
            return run_backtest_batch();
        } catch (const std::exception& e) {
            std::cerr << "[backtest-batch] ERROR: " << e.what() << std::endl;
            return 1;
        }
    }

    // ── Live mode (original) ────────────────────────────────────────────────────
    std::cout << "=== Alpha Signal Engine v0.5.0 (Derived Pipeline) ===" << std::endl;

    // 1. Initialize infrastructure
    ShmReader reader("alpha_upstox_shm_v2", "tick_queue");
    ShmWriter writer("alpha_signal_shm_v1", "signal_queue");
    QuestDBClient qdb("questdb", 9009);

    // 2. Pipelines
    // TickPipeline owns: OFI, TradeDirection, VPIN, Kyle's Lambda, Entropy, BarAccumulator.
    // BarPipeline owns: bar-level analytics (Kalman, CUSUM, etc.) — runs on bar close.
    TickPipeline tick_pipeline;
    BarPipeline  bar_pipeline;

    // 3. Wire bar-close callback into TickPipeline.
    //    Fires once per minute per instrument (infrequent — std::function overhead is fine).
    //    Captures tick_pipeline by ref to read the latest tick signal snapshot at bar close.
    tick_pipeline.set_bar_callback([&](const EnhancedBar& bar) {
        const std::string symbol = "TOKEN_" + std::to_string(bar.instrument_token);

        // Persist to QuestDB (OHLCV + VWAP + signed vol + OFI_bar)
        qdb.write_enhanced_bar(bar, symbol);

        // Snapshot of tick-level signals at this bar close — feeds the SignalBundle
        const auto snap = tick_pipeline.get_snapshot(bar.instrument_token);

        // Bar-level signal processing (Kalman, CUSUM, CompositeScore, AlphaDecay, PnL)
        bar_pipeline.on_bar(bar, snap);
    });

    // 4. Initialize connections
    try {
        reader.wait_for_attachment();
        writer.initialize();
        qdb.connect();
    } catch (const std::exception& e) {
        std::cerr << "[CORE] Initialization failed: " << e.what() << std::endl;
        return 1;
    }

    // 5. Wire ShmWriter into pipelines so signals are published to shared memory.
    tick_pipeline.set_shm_writer(&writer);
    bar_pipeline.set_shm_writer(&writer);

    std::cout << "[CORE] Unified Pipeline Started." << std::endl;

    Tick t;
    uint32_t ticks_processed = 0;

    // 6. O(1) Spin Loop
    //    on_tick() runs all tick signals AND feeds the bar accumulator.
    //    Bar close fires the callback above automatically.
    while (true) {
        if (reader.poll(t)) {
            tick_pipeline.on_tick(t);
            ticks_processed++;

            if (ticks_processed % 1'000'000 == 0) {
                std::cout << "[CORE] Velocity Check: " << ticks_processed
                          << " ticks processed." << std::endl;
            }
        }
    }

    return 0;
}
