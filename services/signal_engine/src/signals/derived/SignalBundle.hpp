/**
 * @file SignalBundle.hpp
 * @brief Canonical index and value container for the 8 normalized signals
 *        fed into CompositeScore.
 *
 * The bundle is populated at bar close from the outputs of all upstream
 * calculators and passed to SignalNormalizer then CompositeScore.
 *
 * Signal index mapping (k = 0..7, w_k from IC calibration):
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

static constexpr uint32_t NUM_SIGNALS = 8u;

enum SignalIndex : uint8_t {
    OFI_NORM    = 0,
    TRADE_DIR   = 1,
    VPIN        = 2,
    KYLE_LAMBDA = 3,
    ENTROPY     = 4,
    LOG_RETURN  = 5,
    RV_ANN      = 6,
    BAR_OFI     = 7,
};

/// Raw (un-normalized) values of all 8 signals at bar close.
struct SignalBundle {
    uint32_t instrument_token;
    uint64_t timestamp_ns;
    std::array<double, NUM_SIGNALS> values = {};  // indexed by SignalIndex

    double& operator[](SignalIndex k)       { return values[k]; }
    double  operator[](SignalIndex k) const { return values[k]; }
};

} // namespace alpha::signal::signals::derived
