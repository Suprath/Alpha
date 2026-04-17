#pragma once

#include <cstdint>
#include <cmath>
#include <unordered_map>
#include "../../core/EnhancedBar.hpp"

namespace alpha::signal::bar_sig {

static constexpr uint32_t BB_PERIOD = 20u;
static constexpr double   BB_MULT   = 2.0;

struct BBState {
    double   window[BB_PERIOD] = {};
    double   sum    = 0.0;
    double   sum_sq = 0.0;
    uint32_t head   = 0;
    uint32_t count  = 0;
};

struct BBResult {
    uint32_t instrument_token;
    double   sma       = 0.0;
    double   upper     = 0.0;
    double   lower     = 0.0;
    double   pct_b     = 0.0;   // (close − lower) / (upper − lower) ∈ [0, 1]
    double   bandwidth = 0.0;   // (upper − lower) / sma
    bool     valid     = false;
};

/**
 * BollingerBandsCalculator — SMA(20) ± 2σ, O(1) rolling variance.
 *
 * Uses Welford-style online variance via sum and sum_sq over a fixed ring buffer.
 * Population variance (Bollinger convention): σ² = (sum_sq - sum²/N) / N
 */
class BollingerBandsCalculator {
public:
    [[nodiscard]] BBResult update(const core::EnhancedBar& bar) {
        BBResult res;
        res.instrument_token = bar.instrument_token;

        auto& s = state_[bar.instrument_token];
        const double close = bar.close;

        // Evict oldest value if window is full
        if (s.count == BB_PERIOD) {
            const double old = s.window[s.head];
            s.sum    -= old;
            s.sum_sq -= old * old;
        } else {
            ++s.count;
        }

        // Insert new value
        s.window[s.head] = close;
        s.head = (s.head + 1) % BB_PERIOD;
        s.sum    += close;
        s.sum_sq += close * close;

        if (s.count < BB_PERIOD) return res;

        const double n    = static_cast<double>(BB_PERIOD);
        const double sma  = s.sum / n;
        // Population variance: σ² = (Σx² - (Σx)²/N) / N
        const double var  = (s.sum_sq - (s.sum * s.sum) / n) / n;
        const double stddev = std::sqrt(std::max(var, 0.0));

        res.sma       = sma;
        res.upper     = sma + BB_MULT * stddev;
        res.lower     = sma - BB_MULT * stddev;
        res.bandwidth = (res.upper - res.lower) / (sma > 1e-10 ? sma : 1e-10);

        const double band = res.upper - res.lower;
        res.pct_b = (band > 1e-10)
            ? (close - res.lower) / band
            : 0.5;

        res.valid = true;
        return res;
    }

    void reset() noexcept { state_.clear(); }

private:
    std::unordered_map<uint32_t, BBState> state_;
};

} // namespace alpha::signal::bar_sig
