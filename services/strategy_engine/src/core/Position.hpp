#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace alpha::strategy {

/**
 * @brief Tracks the live position state for a single instrument.
 */
/**
 * @brief Cost model for transaction fee estimation (NSE/Zerodha style).
 */
struct CostModel {
    double brokerage_flat   = 20.0;    // ₹20 per order
    double brokerage_pct    = 0.0003;  // 0.03% cap
    double slippage_ticks   = 2.0;     // ticks of slippage
    double tick_size        = 0.05;    // ₹0.05 for NSE
    double stt_rate         = 0.00025; // 0.025% on sell side
    double gst_rate         = 0.18;    // 18% on brokerage
};

/**
 * @brief Tracks the live position state for a single instrument.
 */
struct Position {
    int32_t  qty{0};            // Net position: + long, - short, 0 flat
    double   avg_entry{0.0};    // VWAP entry price
    double   realized_pnl{0.0}; // Cumulative realized P&L (INR)
    uint64_t opened_at_ns{0};   // Timestamp when last opened/reversed
    double   total_costs{0.0};  // Cumulative transaction costs (INR)

    bool is_flat()  const { return qty == 0; }
    bool is_long()  const { return qty > 0; }
    bool is_short() const { return qty < 0; }

    double unrealized_pnl(double current_price) const {
        return qty * (current_price - avg_entry);
    }

    /**
     * @brief Apply a fill to update position state.
     * @param fill_qty  Signed: + for buy fill, - for sell fill.
     * @param fill_price Execution price.
     * @param ts_ns     Fill timestamp in nanoseconds.
     * @param cm        Cost model to apply.
     */
    void apply_fill(int32_t fill_qty, double fill_price, uint64_t ts_ns, const CostModel& cm = CostModel{}) {
        // 1. Calculate transaction costs for this leg
        double turnover = fill_price * std::abs(fill_qty);
        double slip_val = cm.slippage_ticks * cm.tick_size * std::abs(fill_qty);
        double brk_val  = std::min(cm.brokerage_flat, turnover * cm.brokerage_pct);
        double stt_val  = (fill_qty < 0) ? (turnover * cm.stt_rate) : 0.0; // STT on sell only
        double cost     = slip_val + (brk_val * (1.0 + cm.gst_rate)) + stt_val;

        total_costs += cost;
        realized_pnl -= cost; // Deduct immediately

        if (qty == 0) {
            // Fresh open
            avg_entry    = fill_price;
            opened_at_ns = ts_ns;
        } else if ((qty > 0 && fill_qty > 0) || (qty < 0 && fill_qty < 0)) {
            // Scaling in same direction — recalculate VWAP
            double total_cost = avg_entry * std::abs(qty) + fill_price * std::abs(fill_qty);
            avg_entry = total_cost / (std::abs(qty) + std::abs(fill_qty));
        } else {
            // Reducing or reversing
            int32_t closing = std::min(std::abs(fill_qty), std::abs(qty));
            double pnl_per_unit = (fill_price - avg_entry) * (qty > 0 ? 1.0 : -1.0);
            realized_pnl += closing * pnl_per_unit;

            int32_t new_qty = qty + fill_qty;
            if (new_qty != 0 && ((qty > 0 && new_qty < 0) || (qty < 0 && new_qty > 0))) {
                // Reversal: remaining units open a new position at fill_price
                avg_entry    = fill_price;
                opened_at_ns = ts_ns;
            }
        }
        qty = qty + fill_qty;
    }
};

} // namespace alpha::strategy
