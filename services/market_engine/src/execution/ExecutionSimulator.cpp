#include "ExecutionSimulator.hpp"
#include "../core/TaxCalculator.hpp"
#include <alpha/time/Timestamp.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace alpha::market {

Trade ExecutionSimulator::simulate(
    const alpha::models::OrderIntent& intent,
    const InstrumentInfo& info,
    uint64_t trade_id)
{
    Trade t{};
    t.trade_id         = trade_id;
    t.timestamp_ns     = alpha::time::Timestamp::now_ns();
    t.instrument_token = intent.instrument_token;
    t.segment          = info.segment;
    t.qty              = std::abs(intent.qty);
    t.side             = intent.side;
    t.lot_size         = info.lot_size;
    t.signal_price     = intent.price;
    t.confidence       = intent.confidence;
    t.strategy_id      = intent.strategy_id;
    t.order_reason     = intent.reason;
    std::snprintf(t.symbol, sizeof(t.symbol), "%s", intent.symbol);

    // Apply slippage: buyer pays more, seller receives less
    const double slip = SlippageModel::slippage_fraction(info.segment);
    if (intent.side == 1) { // BUY
        t.fill_price = intent.price * (1.0 + slip);
    } else {                 // SELL
        t.fill_price = intent.price * (1.0 - slip);
    }

    // Turnover = qty × fill_price × lot_size (premium turnover for options)
    t.turnover = t.qty * t.fill_price * info.lot_size;

    // Calculate all Indian market charges
    t.charges = calculate_charges(info.segment, intent.side,
                                  t.qty, t.fill_price, info.lot_size);

    // Net cash impact:
    //   BUY  → outflow = turnover + charges (negative on cash)
    //   SELL → inflow  = turnover - charges (positive on cash)
    t.net_amount = (intent.side == 1)
        ?  (t.turnover + t.charges.total)   // BUY: cash goes out
        : -(t.turnover - t.charges.total);  // SELL: cash comes in (negative of outflow)

    return t;
}

} // namespace alpha::market
