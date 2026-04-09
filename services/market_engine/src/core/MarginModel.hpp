#pragma once

#include <cmath>
#include "InstrumentInfo.hpp"

namespace alpha::market {

/**
 * @brief Simplified SPAN-style margin model for NSE segments.
 *
 * Returns the margin required (in INR) to hold/open a position.
 * These are conservative approximations — actual SPAN margin changes daily.
 *
 * Margin ratios (approximate as of 2024):
 *   Equity Intraday  : 20%  (5x intraday leverage allowed by SEBI)
 *   Equity Delivery  : 100% (full value required)
 *   Index Futures    : 12%  (SPAN ~8% + exposure ~4%)
 *   Stock Futures    : 15%  (SPAN ~10% + exposure ~5%)
 *   Options Buy      : 100% of premium (no extra margin)
 *   Options Sell     : 12–15% of contract value (like futures)
 *   Currency Futures : 2%   (high liquidity, lower margin)
 */
struct MarginResult {
    double required_margin; // INR to block
    double leverage;        // For display: how much exposure per rupee of margin
};

inline MarginResult compute_margin(
    Segment seg,
    int32_t side,     // 1=BUY, -1=SELL (options sell has higher margin)
    double  qty,
    double  price,
    double  lot_size  = 1.0)
{
    const double notional = std::abs(qty) * price * lot_size; // Contract value

    switch (seg) {
    case Segment::EQUITY_DELIVERY:
        return { notional, 1.0 };

    case Segment::EQUITY_INTRADAY:
        // SEBI mandates minimum 20% margin for intraday equity; no peak-margin violation
        return { notional * 0.20, 5.0 };

    case Segment::INDEX_FUTURES:
        return { notional * 0.12, 8.33 };

    case Segment::STOCK_FUTURES:
        return { notional * 0.15, 6.67 };

    case Segment::INDEX_OPTIONS:
        if (side == 1) {
            // Long option: pay full premium, no additional margin
            return { notional, 1.0 };
        } else {
            // Short option: SPAN margin based on underlying notional
            // Approximate: 12% of underlying (not premium) value
            return { notional * 12.0, 1.0 }; // notional is premium here, ×12 approximates underlying %
        }

    case Segment::STOCK_OPTIONS:
        if (side == 1) {
            return { notional, 1.0 };
        } else {
            return { notional * 15.0, 1.0 };
        }

    case Segment::CURRENCY_FUT:
        return { notional * 0.02, 50.0 };

    case Segment::CURRENCY_OPT:
        if (side == 1) return { notional, 1.0 };
        return { notional * 0.03, 1.0 };

    case Segment::COMMODITY_FUT:
        return { notional * 0.10, 10.0 };

    default:
        return { notional * 0.20, 5.0 };
    }
}

} // namespace alpha::market
