#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace alpha::strategy {

/**
 * @brief Tracks the live position state for a single instrument.
 */
struct Position {
    int32_t  qty{0};            // Net position: + long, - short, 0 flat
    double   avg_entry{0.0};    // VWAP entry price
    double   realized_pnl{0.0}; // Cumulative realized P&L (INR)
    uint64_t opened_at_ns{0};   // Timestamp when last opened/reversed

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
     */
    void apply_fill(int32_t fill_qty, double fill_price, uint64_t ts_ns) {
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
