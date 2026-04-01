#include <iostream>
#include <thread>
#include <chrono>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>

using namespace alpha::ipc;
using namespace alpha::models;

int main() {
    std::cout << "=== Alpha Signal Engine v0.1.0 ===" << std::endl;
    std::cout << "[IPC] Attempting to attach to Upstox Ingester stream..." << std::endl;

    ShmManager upstox_shm("alpha_upstox_shm_v2", ShmRole::CONSUMER);

    // Wait and attach loop if Ingester isn't ready
    SPSCRingBuffer<Tick, 65536>* ring_buffer = nullptr;
    while (!ring_buffer) {
        try {
            ring_buffer = upstox_shm.get_or_create_buffer<SPSCRingBuffer<Tick, 65536>>("tick_queue");
            std::cout << "[IPC] Successfully attached to alpha_upstox_shm_v2/tick_queue." << std::endl;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    std::cout << "[CORE] Spinning Engine Loop Started." << std::endl;

    Tick t;
    uint32_t ticks_processed = 0;
    uint64_t start_time = alpha::time::Timestamp::now_ns();

    // Constant-time O(1) Spin Loop (100% CPU on 1 Core in production)
    while (true) {
        if (ring_buffer->pop(t)) {
            ticks_processed++;
            // Log every tick for testing
            std::cout << "[SIGNAL] Processed " << ticks_processed << " ticks. "
                      << "Last Price (Token " << t.instrument_token << "): " 
                      << t.last_price << std::endl;
            // Check overflow metrics
            uint64_t drops = ring_buffer->overflow_counter.load(std::memory_order_relaxed);
            if (drops > 0) {
                // Warning! The Engine is too slow!
                std::cout << "[WARN] Ingester Dropped " << drops << " ticks due to Engine Lag!" << std::endl;
            }
        }
    }

    return 0;
}
