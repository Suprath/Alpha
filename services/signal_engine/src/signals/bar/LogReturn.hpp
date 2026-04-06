/**
 * @file LogReturn.hpp
 * @brief Bar-level Log Return signal.
 *
 * Formula:
 *   r(t) = ln( C_t / C_{t-1} )
 *
 *   C_t   = current bar close price
 *   C_{t-1} = previous bar close price
 *
 * Properties:
 *   - Continuously compounded return — additive across bars (sum = total return)
 *   - Symmetric: equal magnitude up/down moves give equal |r|
 *   - Approximately equal to simple return for small moves
 *   - r ≈ 0 for unchanged price, r > 0 up-bar, r < 0 down-bar
 *
 * Frequency: called once per bar close (1 minute) — std::unordered_map is fine,
 * this is not the microsecond hot path.
 */

#pragma once

#include <core/EnhancedBar.hpp>
#include <cmath>         // std::log
#include <cstdint>
#include <unordered_map>

namespace alpha::signal::signals::bar {

// ─── Result ──────────────────────────────────────────────────────────────────

struct LogReturnResult {
    uint32_t instrument_token;
    double   r;       // ln(C_t / C_{t-1})  in nats
    bool     valid;   // false on the first bar (no C_{t-1} available)
};

// ─── Log Return Calculator ────────────────────────────────────────────────────

/**
 * @brief Stateful per-instrument log return engine.
 *
 * Stores the previous close per instrument in an unordered_map.
 * Call update() once per bar close from BarPipeline::on_bar().
 */
class LogReturnCalculator {
public:
    /**
     * @brief Compute r(t) = ln(C_t / C_{t-1}) for the given bar.
     */
    [[nodiscard]]
    LogReturnResult update(const alpha::signal::core::EnhancedBar& bar) {
        LogReturnResult result;
        result.instrument_token = bar.instrument_token;

        const double close = bar.close;
        auto it = prev_close_.find(bar.instrument_token);

        if (it == prev_close_.end()) {
            // First bar — seed previous close, no return to compute yet
            prev_close_.emplace(bar.instrument_token, close);
            result.r     = 0.0;
            result.valid = false;
            return result;
        }

        const double prev = it->second;

        if (__builtin_expect(prev <= 0.0 || close <= 0.0, 0)) {
            // Guard against invalid prices — advance state, skip computation
            it->second   = close;
            result.r     = 0.0;
            result.valid = false;
            return result;
        }

        result.r     = std::log(close / prev);
        result.valid = true;

        it->second = close; // advance state
        return result;
    }

    /// Reset all stored closes (call at session open).
    void reset() noexcept { prev_close_.clear(); }

private:
    std::unordered_map<uint32_t, double> prev_close_;
};

} // namespace alpha::signal::signals::bar
