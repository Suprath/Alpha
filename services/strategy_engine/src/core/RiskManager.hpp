#pragma once

#include <cstdint>
#include <ctime>
#include <cmath>

namespace alpha::strategy {

struct RiskParams {
    int32_t max_units_per_instrument = 100;  // Max abs position per instrument
    double  daily_loss_limit         = -50000.0; // INR; stop trading if breached
    int     market_open_hhmm         = 915;  // 09:15 IST
    int     market_close_hhmm        = 1530; // 15:30 IST
};

/**
 * @brief Pre-trade risk checks for the strategy engine.
 *
 * All limits are intentionally conservative for a single-lot NSE equity strategy.
 */
class RiskManager {
public:
    explicit RiskManager(RiskParams p = {}) : params_(p) {}

    /**
     * @brief Check whether a proposed order is within risk limits.
     * @param desired_qty     Absolute target position size.
     * @param cumulative_pnl  Sum of realized P&L across all instruments (INR).
     * @return true if the order is permitted.
     */
    bool allow(int32_t desired_qty, double cumulative_pnl) const {
        if (!is_market_hours()) return false;
        if (cumulative_pnl <= params_.daily_loss_limit) return false;
        if (std::abs(desired_qty) > params_.max_units_per_instrument) return false;
        return true;
    }

    bool is_market_hours() const {
        std::time_t t = std::time(nullptr);
        std::tm* local = std::localtime(&t);
        int hhmm = local->tm_hour * 100 + local->tm_min;
        return hhmm >= params_.market_open_hhmm && hhmm < params_.market_close_hhmm;
    }

    const RiskParams& params() const { return params_; }

private:
    RiskParams params_;
};

} // namespace alpha::strategy
