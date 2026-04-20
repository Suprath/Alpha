#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include "../core/InstrumentInfo.hpp"

namespace alpha::market {

/**
 * @brief Live position state for a single instrument.
 *
 * avg_cost is the VWAP of all fills including brokerage (true cost basis).
 * realized_pnl excludes capital gains tax — that is computed separately at EOD.
 */
struct Position {
    uint32_t instrument_token{0};
    char     symbol[32]{};
    Segment  segment{Segment::EQUITY_INTRADAY};
    double   lot_size{1.0};

    int32_t  qty{0};            // Net position: + long, - short, 0 flat
    double   avg_cost{0.0};     // VWAP including charges (true cost basis)
    double   realized_pnl{0.0}; // Cumulative realized P&L (before capital gains tax)
    double   total_charges{0.0};// All charges paid on this instrument so far
    uint64_t opened_at_ns{0};

    bool is_flat()  const { return qty == 0; }
    bool is_long()  const { return qty > 0; }
    bool is_short() const { return qty < 0; }

    /**
     * @brief Mark-to-market unrealized P&L at current price.
     */
    double unrealized_pnl(double current_price) const {
        return qty * (current_price - avg_cost) * lot_size;
    }

    /**
     * @brief Apply a paper fill to update position state.
     *
     * @param fill_qty   Signed quantity: + for buy, - for sell
     * @param fill_price Execution price (after slippage)
     * @param charges    Total transaction charges for this fill
     * @param ts_ns      Fill timestamp
     */
    void apply_fill(int32_t fill_qty, double fill_price, double charges, uint64_t ts_ns) {
        total_charges += charges;

        if (qty == 0) {
            // Opening a new position: include charges in avg_cost (true cost basis)
            avg_cost     = fill_price + (charges / std::abs(fill_qty) / lot_size);
            opened_at_ns = ts_ns;
        } else if ((qty > 0 && fill_qty > 0) || (qty < 0 && fill_qty < 0)) {
            // Scaling in same direction — recalculate VWAP with charges
            double current_total_cost = avg_cost * std::abs(qty);
            double fill_total_cost    = (fill_price * std::abs(fill_qty) * lot_size + charges) / lot_size;
            avg_cost = (current_total_cost + fill_total_cost) / (std::abs(qty) + std::abs(fill_qty));
        } else {
            // Reducing or reversing
            int32_t closing = std::min(std::abs(fill_qty), std::abs(qty));
            
            // realized_pnl = (exit_price - avg_cost) * qty_closed - closing_charges
            // Since charges are per-fill, we subtract the full fill charges for the closing leg
            double  pnl_per_unit = (fill_price - avg_cost) * lot_size * (qty > 0 ? 1.0 : -1.0);
            realized_pnl += (closing * pnl_per_unit) - charges;

            int32_t new_qty = qty + fill_qty;
            if (new_qty != 0 && ((qty > 0 && new_qty < 0) || (qty < 0 && new_qty > 0))) {
                // Reversal: new position starts at fill_price + its share of charges
                int32_t net_new_qty = std::abs(new_qty);
                double  pro_rata_charges = (static_cast<double>(net_new_qty) / std::abs(fill_qty)) * charges;
                avg_cost     = fill_price + (pro_rata_charges / net_new_qty / lot_size);
                opened_at_ns = ts_ns;
            }
        }
        qty = qty + fill_qty;
        if (qty == 0) {
            avg_cost     = 0.0;
            opened_at_ns = 0;
        }
    }
};

} // namespace alpha::market
