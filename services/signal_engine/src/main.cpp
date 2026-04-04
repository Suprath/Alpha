#include <iostream>
#include <thread>
#include <chrono>
#include "shm_reader/ShmReader.hpp"
#include "shm_writer/ShmWriter.hpp"
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>

using namespace alpha::signal;
using namespace alpha::models;

int main() {
    std::cout << "=== Alpha Signal Engine v0.2.0 (Modular) ===" << std::endl;

    // 1. Initialize SHM Components
    // Reader attaches to Ingester output
    ShmReader reader("alpha_upstox_shm_v2", "tick_queue");
    
    // Writer creates Signal Engine output
    ShmWriter writer("alpha_signal_shm_v1", "signal_queue");

    try {
        reader.wait_for_attachment();
        writer.initialize();
    } catch (const std::exception& e) {
        std::cerr << "[CORE] Initialization failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[CORE] Spinning Engine Loop Started (Lock-Free)." << std::endl;

    Tick t;
    uint32_t ticks_processed = 0;

    // Constant-time O(1) Spin Loop
    while (true) {
        if (reader.poll(t)) {
            ticks_processed++;

            // --- Calculation Placeholder ---
            // In the future, this is where we call strategy.on_tick(t)
            
            // Example: Generate a dummy signal every 10,000 ticks
            if (ticks_processed % 10000 == 0) {
                Signal sig {};
                sig.timestamp_ns = alpha::time::Timestamp::now_ns();
                sig.instrument_token = t.instrument_token;
                sig.price = t.last_price;
                sig.confidence = 0.85;
                sig.action = 1; // Buy
                sig.strategy_id = 101;
                
                writer.publish(sig);
                
                std::cout << "[SIGNAL] Published Signal for Token " << t.instrument_token 
                          << " @ " << t.last_price << std::endl;
            }

            // Simple status log
            if (ticks_processed % 100000 == 0) {
                std::cout << "[CORE] Processed " << ticks_processed << " ticks total." << std::endl;
            }
        }
    }

    return 0;
}
