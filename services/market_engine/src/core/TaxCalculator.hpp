#pragma once

#include <cmath>
#include "ChargeSchedule.hpp"

namespace alpha::market {

/**
 * @brief Itemized breakdown of all charges for a single trade.
 * All values in INR.
 */
struct TaxBreakdown {
    double brokerage;
    double stt;
    double exchange_charges;
    double sebi_charges;
    double gst;              // 18% on (brokerage + exchange + SEBI)
    double stamp_duty;       // Only on buy side
    double dp_charges;       // Only on delivery sell
    double total;
};

/**
 * @brief Computes all Indian market charges for a single order.
 *
 * @param seg        Instrument segment (determines which schedule to use)
 * @param side       1 = BUY, -1 = SELL
 * @param qty        Absolute quantity traded
 * @param price      Execution price
 * @param lot_size   Contract lot size (1 for equity)
 *
 * Turnover = qty × price × lot_size
 * For options this is the PREMIUM turnover (not underlying value).
 */
inline TaxBreakdown calculate_charges(
    Segment seg,
    int32_t side,
    double  qty,
    double  price,
    double  lot_size = 1.0)
{
    const ChargeSchedule& cs = get_charge_schedule(seg);
    const double turnover = std::abs(qty) * price * lot_size;

    TaxBreakdown tb{};

    // --- Brokerage ---
    if (cs.brokerage_flat_inr > 0.0 || cs.brokerage_pct > 0.0) {
        double flat = cs.brokerage_flat_inr;
        double pct  = cs.brokerage_pct > 0.0 ? cs.brokerage_pct * turnover : flat;
        tb.brokerage = cs.use_lower_brokerage ? std::min(flat, pct) : flat;
        if (cs.brokerage_pct > 0.0 && cs.brokerage_flat_inr == 0.0)
            tb.brokerage = pct; // Delivery: 0 flat, 0 pct → 0 brokerage
    }

    // --- STT (Securities Transaction Tax) ---
    if (side == 1) { // BUY
        tb.stt = cs.stt_buy_pct * turnover;
    } else {         // SELL
        tb.stt = cs.stt_sell_pct * turnover;
    }

    // --- Exchange Transaction Charge ---
    // For options: applied on premium turnover (already in `turnover`)
    // For futures/equity: applied on contract/trade value (same)
    tb.exchange_charges = cs.exchange_charge_pct * turnover;

    // --- SEBI Turnover Fee ---
    tb.sebi_charges = cs.sebi_charge_pct * turnover;

    // --- GST (18% on brokerage + exchange + SEBI) ---
    tb.gst = cs.gst_pct * (tb.brokerage + tb.exchange_charges + tb.sebi_charges);

    // --- Stamp Duty (only on buy side) ---
    if (side == 1) {
        tb.stamp_duty = cs.stamp_duty_buy_pct * turnover;
        // Cap at ₹1500 per instrument per day per client (SEBI stamp duty cap)
        if (tb.stamp_duty > 1500.0) tb.stamp_duty = 1500.0;
    }

    // --- DP Charges (delivery sell only) ---
    if (side == -1 && seg == Segment::EQUITY_DELIVERY) {
        tb.dp_charges = cs.dp_charge_inr;
    }

    tb.total = tb.brokerage + tb.stt + tb.exchange_charges +
               tb.sebi_charges + tb.gst + tb.stamp_duty + tb.dp_charges;

    return tb;
}

} // namespace alpha::market
