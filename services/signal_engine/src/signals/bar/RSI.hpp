#pragma once

#include <cstdint>
#include <unordered_map>
#include "../../core/EnhancedBar.hpp"

namespace alpha::signal::bar_sig {

static constexpr uint32_t RSI_PERIOD = 14u;

struct RSIState {
    double   avg_gain  = 0.0;
    double   avg_loss  = 0.0;
    double   prev_close = 0.0;
    uint32_t count     = 0;
    // Accumulator for the initial simple average seed
    double   sum_gain  = 0.0;
    double   sum_loss  = 0.0;
};

struct RSIResult {
    uint32_t instrument_token;
    double   rsi   = 0.0;   // Wilder's RSI ∈ [0, 100]
    bool     valid = false;  // true after RSI_PERIOD bars
};

/**
 * RSICalculator — Wilder's Smoothed RSI(14), stateful per instrument.
 *
 * Seeding: bars 1..RSI_PERIOD accumulate simple average of gains/losses.
 * Smoothing: bars > RSI_PERIOD apply Wilder's EMA:
 *   avg_gain = (prev_avg_gain * (N-1) + gain) / N
 */
class RSICalculator {
public:
    [[nodiscard]] RSIResult update(const core::EnhancedBar& bar) {
        RSIResult res;
        res.instrument_token = bar.instrument_token;

        auto& s = state_[bar.instrument_token];
        const double close = bar.close;

        if (s.count == 0) {
            s.prev_close = close;
            ++s.count;
            return res;  // Need at least 2 bars for a change
        }

        const double change = close - s.prev_close;
        const double gain   = (change > 0.0) ? change : 0.0;
        const double loss   = (change < 0.0) ? -change : 0.0;
        s.prev_close = close;
        ++s.count;

        if (s.count <= RSI_PERIOD) {
            // Accumulate for seed
            s.sum_gain += gain;
            s.sum_loss += loss;

            if (s.count == RSI_PERIOD) {
                // Seed the smoothed averages
                s.avg_gain = s.sum_gain / static_cast<double>(RSI_PERIOD - 1);
                s.avg_loss = s.sum_loss / static_cast<double>(RSI_PERIOD - 1);
                res.valid  = true;
            }
        } else {
            // Wilder's smoothing
            constexpr double alpha = 1.0 / static_cast<double>(RSI_PERIOD);
            s.avg_gain = s.avg_gain * (1.0 - alpha) + gain * alpha;
            s.avg_loss = s.avg_loss * (1.0 - alpha) + loss * alpha;
            res.valid  = true;
        }

        if (res.valid) {
            if (s.avg_loss < 1e-10) {
                res.rsi = 100.0;
            } else {
                const double rs = s.avg_gain / s.avg_loss;
                res.rsi = 100.0 - (100.0 / (1.0 + rs));
            }
        }

        return res;
    }

    void reset() noexcept { state_.clear(); }

private:
    std::unordered_map<uint32_t, RSIState> state_;
};

} // namespace alpha::signal::bar_sig
