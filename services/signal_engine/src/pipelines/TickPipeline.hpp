#pragma once

#include <alpha/models/MarketModels.hpp>
#include <iostream>

namespace alpha::signal::pipelines {

/**
 * @brief Logic for microsecond-level tick processing.
 * Focus: High-frequency signals like OFI, VPIN.
 */
class TickPipeline {
public:
    void on_tick(const alpha::models::Tick& tick) {
        // --- OFI Placeholder ---
        // Implementation of Order Flow Imbalance logic
        // ...
        
        // --- Signal Logic ---
        // if ofi > threshold -> publish micro-signal to SHM
    }
};

} // namespace alpha::signal::pipelines
