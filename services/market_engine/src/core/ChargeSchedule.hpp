#pragma once

#include "InstrumentInfo.hpp"

namespace alpha::market {

/**
 * @brief Charge rates for a single market segment.
 *
 * All rates are expressed as decimals (e.g., 0.1% = 0.001).
 * Sources: SEBI/NSE/BSE circulars, Zerodha brokerage calculator (FY 2024-25).
 *
 * Key Indian market charges:
 *  - Brokerage      : ₹20 flat or % of turnover (whichever is applicable)
 *  - STT            : Securities Transaction Tax (buy/sell side depends on segment)
 *  - Exchange charge: NSE/BSE transaction charge on turnover
 *  - SEBI charge    : ₹10 per crore = 0.0001% of turnover
 *  - GST            : 18% on (brokerage + exchange charge + SEBI charge)
 *  - Stamp duty     : State stamp duty on buy side only (unified rate since 2020)
 *  - DP charge      : Depository participant charge (delivery sell only)
 */
struct ChargeSchedule {
    double brokerage_flat_inr;    // Flat brokerage per executed order (₹20)
    double brokerage_pct;         // Percentage brokerage (alt to flat)
    bool   use_lower_brokerage;   // If true: min(flat, pct × turnover)

    double stt_buy_pct;           // STT on buy-side turnover
    double stt_sell_pct;          // STT on sell-side turnover

    double exchange_charge_pct;   // NSE transaction charge on total turnover
    double sebi_charge_pct;       // SEBI turnover fee (0.0001% = ₹10/crore)
    double gst_pct;               // GST on (brokerage + exchange + SEBI) = 18%

    double stamp_duty_buy_pct;    // Stamp duty on buy-side value only
    double dp_charge_inr;         // DP charge per sell transaction (delivery only)
};

/**
 * @brief Standard charge schedules for each Indian market segment (FY 2024-25).
 */
inline ChargeSchedule get_charge_schedule(Segment seg) {
    switch (seg) {
    case Segment::EQUITY_DELIVERY:
        // Zero brokerage (Zerodha-style); STT 0.1% both sides; stamp 0.015% buy
        return { 0.0, 0.0, false,
                 0.001,   0.001,     // STT buy + sell
                 0.0000297, 0.000001, // exchange + SEBI
                 0.18,
                 0.00015, 15.93 };   // stamp 0.015% buy; DP ₹13.5+GST=₹15.93

    case Segment::EQUITY_INTRADAY:
        // ₹20 or 0.03% lower; STT 0.025% sell only; stamp 0.003% buy
        return { 20.0, 0.0003, true,
                 0.0,    0.00025,    // STT: 0 buy, 0.025% sell
                 0.0000297, 0.000001,
                 0.18,
                 0.00003, 0.0 };    // stamp 0.003% buy; no DP

    case Segment::INDEX_FUTURES:
    case Segment::STOCK_FUTURES:
        // ₹20 flat; STT 0.0125% sell on contract value; stamp 0.002% buy
        return { 20.0, 0.0, false,
                 0.0,    0.0001255,  // STT: 0 buy, 0.0125% sell
                 0.0000173, 0.000001, // NSE F&O exchange charge + SEBI
                 0.18,
                 0.00002, 0.0 };    // stamp 0.002% buy; no DP

    case Segment::INDEX_OPTIONS:
    case Segment::STOCK_OPTIONS:
        // ₹20 flat; STT 0.0625% sell on premium; exchange 0.0325% on premium turnover
        return { 20.0, 0.0, false,
                 0.0,    0.000625,   // STT: 0 buy, 0.0625% sell on premium
                 0.000325, 0.000001, // NSE options exchange charge (0.0325%) + SEBI
                 0.18,
                 0.00003, 0.0 };    // stamp 0.003% buy; no DP

    case Segment::CURRENCY_FUT:
        // ₹20 flat; no STT for currency; exchange 0.00035%
        return { 20.0, 0.0, false,
                 0.0, 0.0,
                 0.0000035, 0.000001,
                 0.18,
                 0.000001, 0.0 };

    case Segment::CURRENCY_OPT:
        return { 20.0, 0.0, false,
                 0.0, 0.0,
                 0.000313, 0.000001,
                 0.18,
                 0.000001, 0.0 };

    case Segment::COMMODITY_FUT:
        // MCX: ₹20 flat; CTT 0.01% sell (non-agri); exchange 0.0026%
        return { 20.0, 0.0, false,
                 0.0, 0.0001,
                 0.000026, 0.000001,
                 0.18,
                 0.00002, 0.0 };

    default:
        return get_charge_schedule(Segment::EQUITY_INTRADAY);
    }
}

} // namespace alpha::market
