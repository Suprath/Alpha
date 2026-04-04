#include <iostream>
#include <thread>
#include <chrono>
#include "shm_reader/ShmReader.hpp"
#include "shm_writer/ShmWriter.hpp"
#include "core/TickAccumulator.hpp"
#include "core/QuestDBClient.hpp"
#include "pipelines/TickPipeline.hpp"
#include "pipelines/BarPipeline.hpp"
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>

using namespace alpha::signal;
using namespace alpha::signal::core;
using namespace alpha::signal::pipelines;
using namespace alpha::models;

int main() {
    std::cout << "=== Alpha Signal Engine v0.3.0 (Dual Pipeline) ===" << std::endl;

    // 1. Initialize Components
    ShmReader reader("alpha_upstox_shm_v2", "tick_queue");
    ShmWriter writer("alpha_signal_shm_v1", "signal_queue");
    
    // Core Infrastructure
    QuestDBClient qdb("questdb", 9009);
    
    // Analytic Pipelines
    TickPipeline tick_pipeline;
    BarPipeline bar_pipeline;
    
    // OHLCV Aggregator (Bridge between Pipeline 1 and 2)
    TickAccumulator aggregator([&](const Candle& bar) {
        // --- ON BAR CLOSE ---
        std::string symbol = "TOKEN_" + std::to_string(bar.timestamp_ns); // Placeholder
        qdb.write_candle(bar, symbol);
        bar_pipeline.on_bar(bar);
        std::cout << "[BAR] Closed 1m Bar: " << symbol << " @ " << bar.close << std::endl;
    });

    try {
        reader.wait_for_attachment();
        writer.initialize();
        qdb.connect();
    } catch (const std::exception& e) {
        std::cerr << "[CORE] Initialization failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[CORE] Dual-Pipeline Loop Started (Tick + Bar)." << std::endl;

    Tick t;
    uint32_t ticks_processed = 0;

    // Constant-time O(1) Spin Loop
    while (true) {
        if (reader.poll(t)) {
            ticks_processed++;

            // Pipeline 1: Microsecond Tick Logic
            tick_pipeline.on_tick(t);
            
            // Bridge: Accumulate into 1m Bars
            aggregator.process_tick(t);

            // Simple status log every 1M ticks
            if (ticks_processed % 1000000 == 0) {
                std::cout << "[CORE] Velocity Check: " << ticks_processed << " ticks total." << std::endl;
            }
        }
    }

    return 0;
}
