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
