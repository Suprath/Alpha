/**
 * @file OptionsChain.hpp
 * @brief Options chain data structures consumed by GEXCalculator and VRPCalculator.
 *
 * Populated externally (e.g., from NSE option chain REST API, updated every bar
 * or every few seconds). Not part of the real-time tick stream.
 *
 * One OptionStrike holds the data for a single (instrument, expiry, strike) tuple.
 * OptionsChain aggregates all strikes for a single instrument + expiry.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace alpha::signal::signals::options {

// ─── Single strike ────────────────────────────────────────────────────────────

struct OptionStrike {
    double   K;         // strike price (INR)
    double   iv_call;   // implied vol — call (annualized, e.g. 0.18 = 18%)
    double   iv_put;    // implied vol — put  (annualized)
    uint64_t oi_call;   // open interest — calls (in contracts)
    uint64_t oi_put;    // open interest — puts  (in contracts)
};

// ─── Full chain for one instrument + expiry ───────────────────────────────────

struct OptionsChain {
    uint32_t instrument_token; // NSE underlying token (e.g. NIFTY 50)
    double   spot;             // S(t) — current spot price
    double   T;                // time to expiry in years (e.g. 7/365.0)
    double   r;                // risk-free rate, annualized (e.g. 0.065 = 6.5%)
    uint32_t lot_size;         // L — NSE lot size (NIFTY=50, BANKNIFTY=15, etc.)

    std::vector<OptionStrike> strikes; // all available strikes, any order
};

} // namespace alpha::signal::signals::options
