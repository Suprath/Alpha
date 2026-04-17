#pragma once

#include <cstdint>
#include <unordered_map>
#include "../../core/EnhancedBar.hpp"

namespace alpha::signal::bar_sig {

struct VWAPDevState {
    double   pv_sum  = 0.0;  // Σ(vwap × volume) — price-volume cumulative
    double   vol_sum = 0.0;  // Σ volume
};

struct VWAPDevResult {
    uint32_t instrument_token;
    double   session_vwap = 0.0;
    double   deviation    = 0.0;  // (close − session_vwap) / session_vwap
    bool     valid        = false;
};

/**
 * VWAPDeviationCalculator — Session-accumulated VWAP and normalized deviation.
 *
 * Accumulates from first bar each session. Call reset_session() at EOD.
 * Uses bar.vwap × bar.volume for price-volume accumulation (consistent with
 * how candles store bar-level VWAP in QuestDB).
 */
class VWAPDeviationCalculator {
public:
    [[nodiscard]] VWAPDevResult update(const core::EnhancedBar& bar) {
        VWAPDevResult res;
        res.instrument_token = bar.instrument_token;

        if (bar.volume == 0) return res;

        auto& s = state_[bar.instrument_token];
        s.pv_sum  += bar.vwap * static_cast<double>(bar.volume);
        s.vol_sum += static_cast<double>(bar.volume);

        if (s.vol_sum < 1.0) return res;

        res.session_vwap = s.pv_sum / s.vol_sum;

        if (res.session_vwap > 1e-6) {
            res.deviation = (bar.close - res.session_vwap) / res.session_vwap;
            res.valid     = true;
        }

        return res;
    }

    // Call at start of each new trading session (09:15 IST)
    void reset_session() noexcept { state_.clear(); }
    void reset() noexcept { state_.clear(); }

private:
    std::unordered_map<uint32_t, VWAPDevState> state_;
};

} // namespace alpha::signal::bar_sig
