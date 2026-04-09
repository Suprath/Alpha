#include "MarketEngine.hpp"
#include <iostream>
#include <iomanip>

namespace alpha::market {

static const char* segment_str(Segment seg) {
    switch (seg) {
    case Segment::EQUITY_DELIVERY:  return "EQ-DLVRY";
    case Segment::EQUITY_INTRADAY:  return "EQ-INTRA";
    case Segment::INDEX_FUTURES:    return "IDX-FUT ";
    case Segment::STOCK_FUTURES:    return "STK-FUT ";
    case Segment::INDEX_OPTIONS:    return "IDX-OPT ";
    case Segment::STOCK_OPTIONS:    return "STK-OPT ";
    case Segment::CURRENCY_FUT:     return "CCY-FUT ";
    case Segment::CURRENCY_OPT:     return "CCY-OPT ";
    case Segment::COMMODITY_FUT:    return "COM-FUT ";
    default:                        return "UNKNOWN ";
    }
}

static const char* reason_str(int32_t reason) {
    switch (reason) {
    case 1: return "ENTRY    ";
    case 2: return "EXIT     ";
    case 3: return "SCALE_IN ";
    case 4: return "SCALE_OUT";
    case 5: return "REVERSE  ";
    default: return "?        ";
    }
}

MarketEngine::MarketEngine(double starting_capital)
    : portfolio_(starting_capital)
    , registry_(InstrumentRegistry::instance()) {}

bool MarketEngine::on_order(const alpha::models::OrderIntent& intent) {
    // Resolve instrument metadata
    const InstrumentInfo& info = registry_.get_or_register(
        intent.instrument_token, intent.symbol);

    // Simulate paper execution
    Trade trade = ExecutionSimulator::simulate(intent, info, ++trade_id_counter_);

    // Apply to portfolio
    bool accepted = portfolio_.apply_trade(trade);

    // Track last price for MTM
    last_prices_[intent.instrument_token] = intent.price;

    log_trade(trade);
    return accepted;
}

void MarketEngine::update_price(uint32_t token, double current_price) {
    last_prices_[token] = current_price;
    portfolio_.mark_to_market(token, current_price);
}

void MarketEngine::eod_square_off_all() {
    std::cout << "[MarketEngine] EOD square-off triggered.\n";
    for (const auto& [token, price] : last_prices_) {
        portfolio_.eod_square_off(token, price);
    }
    print_portfolio();
}

void MarketEngine::log_trade(const Trade& trade) const {
    const auto& snap = portfolio_.snapshot();
    const char* side_s = (trade.side == 1) ? "BUY " : "SELL";

    std::cout << std::fixed << std::setprecision(2)
              << "[TRADE #" << std::setw(5) << trade.trade_id << "] "
              << reason_str(trade.order_reason) << " | "
              << segment_str(trade.segment) << " | "
              << side_s << " "
              << std::setw(5) << trade.qty << " x "
              << std::left << std::setw(20) << trade.symbol << std::right
              << " @ ₹"  << trade.fill_price
              << " (slip ₹" << std::setprecision(4) << (trade.fill_price - trade.signal_price) << ")"
              << "\n"
              << "           Charges: brokerage=₹"  << std::setprecision(2) << trade.charges.brokerage
              << " stt=₹"          << trade.charges.stt
              << " exch=₹"         << trade.charges.exchange_charges
              << " sebi=₹"         << trade.charges.sebi_charges
              << " gst=₹"          << trade.charges.gst
              << " stamp=₹"        << trade.charges.stamp_duty
              << " dp=₹"           << trade.charges.dp_charges
              << " | TOTAL=₹"      << trade.charges.total
              << "\n"
              << "           Net P&L: realized=₹" << portfolio_.position(trade.instrument_token).realized_pnl
              << " | cash=₹" << snap.cash
              << " | kelly=" << std::setprecision(3) << trade.confidence
              << "\n";
}

} // namespace alpha::market
