/**
 * @file SignalBundle.hpp
 * @brief Canonical index and value container for all normalized signals
 *        fed into CompositeScore.
 *
 * The bundle is populated at bar close from the outputs of all upstream
 * calculators and passed to SignalNormalizer then CompositeScore.
 *
 * ── Adding a new signal ───────────────────────────────────────────────────────
 * 1. Write your calculator in signals/<name>/<Name>Calculator.hpp
 * 2. Add an entry to SignalIndex below
 * 3. Increment NUM_SIGNALS by 1  (arrays are pre-sized to SIGNAL_CAPACITY)
 * 4. Add the result field to TickSignalSnapshot (if tick-level) or populate
 *    directly in BarPipeline::on_bar() (if bar-level)
 * 5. Add #include + private member + update() call in TickPipeline/BarPipeline
 * 6. Populate the bundle slot in BarPipeline::on_bar()
 * SignalNormalizer and CompositeScore require NO changes.
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Signal index mapping (k = 0..NUM_SIGNALS-1, w_k from IC calibration):
 *   0  OFI_NORM     — tick-level OFI normalized ∈ [-1, +1]
 *   1  TRADE_DIR    — cumulative trade direction (raw, normalizer handles scale)
 *   2  VPIN         — order toxicity proxy ∈ [0, 1]
 *   3  KYLE_LAMBDA  — price impact per unit OFI (illiquidity)
 *   4  ENTROPY      — L2 depth entropy H_normalized ∈ [0, ~0.70]
 *   5  LOG_RETURN   — bar log return r(t)
 *   6  RV_ANN       — annualized realized volatility σ_ann
 *   7  BAR_OFI      — bar-level OFI = (V^B - V^S) / V ∈ [-1, +1]
 */

#pragma once

#include <cstdint>
#include <array>

namespace alpha::signal::signals::derived {

/// Maximum number of signals the pipeline can ever hold.
/// Arrays are sized to this — never needs to change when adding signals.
static constexpr uint32_t SIGNAL_CAPACITY = 32u;

/// Number of currently active signals. Increment this (and add an enum entry)
/// when adding a new signal. Everything else adapts automatically.
static constexpr uint32_t NUM_SIGNALS = 8u;

static_assert(NUM_SIGNALS <= SIGNAL_CAPACITY,
              "NUM_SIGNALS exceeds SIGNAL_CAPACITY — increase SIGNAL_CAPACITY");

// ── Signal index enum ────────────────────────────────────────────────────────
// Keep entries contiguous and < NUM_SIGNALS.
// Add new signals at the end; never reorder (backtest reproducibility).

enum SignalIndex : uint8_t {
    OFI_NORM    = 0,
    TRADE_DIR   = 1,
    VPIN        = 2,
    KYLE_LAMBDA = 3,
    ENTROPY     = 4,
    LOG_RETURN  = 5,
    RV_ANN      = 6,
    BAR_OFI     = 7,
    // ── Add new signals here ──────────────────────────────────────────────
    // EXAMPLE_SIGNAL = 8,  // and set NUM_SIGNALS = 9 above
};

/// Raw (un-normalized) values of all active signals at bar close.
struct SignalBundle {
    uint32_t instrument_token;
    uint64_t timestamp_ns;
    std::array<double, SIGNAL_CAPACITY> values = {};  // indexed by SignalIndex

    double& operator[](SignalIndex k)       { return values[k]; }
    double  operator[](SignalIndex k) const { return values[k]; }
};

} // namespace alpha::signal::signals::derived
