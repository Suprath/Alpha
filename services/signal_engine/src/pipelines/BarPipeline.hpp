#pragma once

#include <alpha/models/MarketModels.hpp>
#include <iostream>

namespace alpha::signal::pipelines {

/**
 * @brief Logic for bar-level macro signal analysis.
 * Run every 1-minute on bar-close.
 * Focus: Kalman Filter, CUSUM Regime Switching.
 */
class BarPipeline {
public:
    void on_bar(const alpha::models::Candle& bar) {
        // --- Kalman/CUSUM Placeholder ---
        // Implementation of bar-level math
        // ...
        
        // --- Persistence ---
        // The TickAccumulator handles QuestDB writing, but the pipeline 
        // can also use the data here for internal state update.
    }
};

} // namespace alpha::signal::pipelines
