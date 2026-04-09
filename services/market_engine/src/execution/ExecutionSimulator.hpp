#pragma once

#include <cstdint>
#include <alpha/models/MarketModels.hpp>
#include "../core/InstrumentInfo.hpp"
#include "../core/Trade.hpp"

namespace alpha::market {

/**
 * @brief Slippage rates by segment (as fraction of price).
 *
 * Models market impact of a market order hitting the spread.
 * HFT-grade: equity 1-tick slippage, options wider spread.
 */
struct SlippageModel {
    static double slippage_fraction(Segment seg) {
        switch (seg) {
        case Segment::EQUITY_INTRADAY:
        case Segment::EQUITY_DELIVERY: return 0.0001; // 0.01% — 1 tick on liquid NSE equity
        case Segment::INDEX_FUTURES:
        case Segment::STOCK_FUTURES:   return 0.00005; // 0.005% — tight futures spread
        case Segment::INDEX_OPTIONS:
        case Segment::STOCK_OPTIONS:   return 0.0005;  // 0.05% — wider options spread
        case Segment::CURRENCY_FUT:    return 0.00005;
        default:                       return 0.0001;
        }
    }
};

/**
 * @brief Simulates paper-trade execution of an OrderIntent.
 *
 * Applies realistic slippage, calculates all Indian market charges,
 * and returns a filled Trade record.
 *
 * Fill model: market order fills immediately at signal_price ± slippage.
 *   BUY  → fill = signal_price × (1 + slippage)  [adverse)
 *   SELL → fill = signal_price × (1 - slippage)  (adverse)
 */
class ExecutionSimulator {
public:
    /**
     * @brief Simulate execution of an order intent.
     * @param intent     The order decision from the strategy engine.
     * @param info       Instrument metadata (segment, lot_size, etc.).
     * @param trade_id   Monotonically increasing trade identifier.
     * @return           Filled Trade with all charges computed.
     */
    static Trade simulate(
        const alpha::models::OrderIntent& intent,
        const InstrumentInfo& info,
        uint64_t trade_id);
};

} // namespace alpha::market
