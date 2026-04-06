#include <iostream>
#include <thread>
#include <chrono>
#include "shm_reader/ShmReader.hpp"
#include "shm_writer/ShmWriter.hpp"
#include "core/QuestDBClient.hpp"
#include "core/EnhancedBar.hpp"
#include "pipelines/TickPipeline.hpp"
#include "pipelines/BarPipeline.hpp"
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>

using namespace alpha::signal;
using namespace alpha::signal::core;
using namespace alpha::signal::pipelines;
using namespace alpha::models;

int main() {
    std::cout << "=== Alpha Signal Engine v0.4.0 (Unified Pipeline) ===" << std::endl;

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
    tick_pipeline.set_bar_callback([&](const EnhancedBar& bar) {
        const std::string symbol = "TOKEN_" + std::to_string(bar.instrument_token);

        // Persist to QuestDB (OHLCV + VWAP + signed vol + OFI_bar)
        qdb.write_enhanced_bar(bar, symbol);

        // Bar-level signal processing (Kalman, CUSUM, ...)
        bar_pipeline.on_bar(bar);

        std::cout << "[BAR] " << symbol
                  << "  C="       << bar.close
                  << "  V="       << bar.volume
                  << "  VWAP="    << bar.vwap
                  << "  OFI_bar=" << bar.bar_ofi
                  << "  N="       << bar.tick_count
                  << std::endl;
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

    std::cout << "[CORE] Unified Pipeline Started." << std::endl;

    Tick t;
    uint32_t ticks_processed = 0;

    // 5. O(1) Spin Loop
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
