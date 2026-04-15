/**
 * Alpha Strategy Engine — v0.2.0
 *
 * Reads trading signals from shared memory (written by the signal engine),
 * applies position management and risk checks, and publishes order decisions
 * to shared memory for the market engine to consume.
 *
 * SHM Input:  alpha_signal_shm_v1 / signal_queue   (Signal structs)
 * SHM Output: alpha_order_shm_v1  / order_queue    (OrderIntent structs)
 * Data Flow:  Signal Engine → [SHM] → Strategy Engine → [SHM] → Market Engine
 */

#include <iostream>
#include <csignal>
#include <atomic>
#include <cstdlib>

#include "shm_reader/ShmReader.hpp"
#include "shm_writer/OrderShmWriter.hpp"
#include "strategy/StrategyEngine.hpp"

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

int main() {
    std::cout << std::unitbuf;   // Force line-unbuffered stdout in Docker
    std::cout << "=== Alpha Strategy Engine v0.2.0 ===" << std::endl;

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // -- Risk configuration (TEST_MODE bypasses market-hours gate for out-of-hours testing) --
    const char* test_mode_env = std::getenv("TEST_MODE");
    const bool  test_mode     = test_mode_env && std::string(test_mode_env) == "1";

    alpha::strategy::RiskParams risk_params;
    risk_params.bypass_market_hours = test_mode;

    if (test_mode) {
        std::cout << "[Main] TEST_MODE=1: market-hours check bypassed.\n";
    }

    // -- SHM Reader: consumes Signal structs from signal engine --
    alpha::strategy::ShmReader reader("alpha_signal_shm_v1", "signal_queue");
    reader.wait_for_attachment();

    // -- SHM Writer: publishes OrderIntents to market engine --
    alpha::strategy::OrderShmWriter order_writer("alpha_order_shm_v1", "order_queue");
    order_writer.initialize();

    // -- Strategy Engine: position management + risk --
    alpha::strategy::StrategyEngine engine(risk_params);

    std::cout << "[Main] Entering signal processing loop..." << std::endl;

    alpha::models::Signal       sig{};
    alpha::strategy::OrderIntent intent{};

    while (g_running.load(std::memory_order_relaxed)) {
        if (reader.poll(sig)) {
            if (engine.on_signal(sig, intent)) {
                order_writer.publish(intent);
            }
        }
        // Busy-spin: dedicated core
    }

    std::cout << "[Main] Shutting down. Final P&L: "
              << engine.total_realized_pnl() << " INR\n";

    return 0;
}
