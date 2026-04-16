#include "MarketEngine.hpp"
#include <iostream>
#include <iomanip>
#include <alpha/time/Timestamp.hpp>

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

MarketEngine::MarketEngine(double starting_capital,
                           const std::string& redis_host, int redis_port)
    : portfolio_(starting_capital)
    , registry_(InstrumentRegistry::instance())
{
    // Connect Redis publisher — non-fatal if Redis is unavailable
    redis_publisher_.connect(redis_host, redis_port);
}

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
    publish_portfolio_to_redis();
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

void MarketEngine::publish_portfolio_to_redis() {
    if (!redis_publisher_.is_connected()) return;

    const auto snap = portfolio_.snapshot();
    const auto& hist = portfolio_.trade_history();
    const uint64_t ts = alpha::time::Timestamp::now_ns();

    // Collect open positions
    std::vector<RedisPublisher::PositionSnapshot> pos_snaps;
    for (const auto& [token, pos] : portfolio_.positions()) {
        if (pos.is_flat()) continue;
        double last_p = last_prices_.count(token) ? last_prices_.at(token) : pos.avg_cost;
        pos_snaps.push_back({
            token,
            pos.symbol,
            pos.qty,
            pos.avg_cost,
            pos.realized_pnl,
            pos.unrealized_pnl(last_p),
        });
    }

    redis_publisher_.publish_portfolio(
        snap.cash,
        snap.total_equity,
        snap.net_realized_pnl,
        snap.total_charges_paid,
        snap.open_positions,
        snap.total_trades,
        PortfolioManager::DEFAULT_STARTING_CAPITAL,
        pos_snaps,
        ts);

    // Publish the most recent trade to the stream
    if (!hist.empty()) {
        const auto& t = hist.back();
        const auto& pos = portfolio_.position(t.instrument_token);
        redis_publisher_.publish_trade(
            t.trade_id, t.symbol, t.side, t.qty,
            t.fill_price, t.charges.total,
            snap.cash, pos.realized_pnl,
            t.timestamp_ns);
    }
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
