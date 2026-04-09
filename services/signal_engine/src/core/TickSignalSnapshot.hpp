/**
 * @file TickSignalSnapshot.hpp
 * @brief Latest tick-level signal values per instrument, captured at bar close.
 *
 * Updated on every tick inside TickPipeline and consumed by BarPipeline at
 * bar close to populate the 8-signal SignalBundle for the derived pipeline.
 *
 * All fields hold the most recent valid value. Zero-initialized — BarPipeline
 * checks `valid` before using tick values in the bundle.
 */

#pragma once

#include <cstdint>

namespace alpha::signal::core {

struct TickSignalSnapshot {
    uint32_t instrument_token   = 0;
    float    ofi_normalized     = 0.0f; // OFI(t) / depth ∈ [-1, +1]
    int32_t  cum_direction      = 0;    // Σ d(t) since session start
    float    vpin               = 0.0f; // order toxicity ∈ [0, 1]
    double   kyle_lambda        = 0.0;  // price impact per unit OFI
    float    entropy_norm       = 0.0f; // H_normalized ∈ [0, ~0.699]
    bool     valid              = false;// true once at least one tick processed
};

} // namespace alpha::signal::core
