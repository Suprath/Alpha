#include "StrategyEngine.hpp"
#include <alpha/time/Timestamp.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>

namespace alpha::strategy {

StrategyEngine::StrategyEngine(RiskParams risk_params)
    : risk_(risk_params) {}

bool StrategyEngine::on_signal(const alpha::models::Signal& sig, OrderIntent& out) {
    // Ignore neutral signals (shouldn't reach here, but guard anyway)
    if (sig.action == 0) return false;

    // Compute desired position based on Kelly-sized confidence
    int32_t desired_qty = sig.action * static_cast<int32_t>(std::round(MAX_UNITS * sig.confidence));

    // Cap at max units
    if (desired_qty >  MAX_UNITS) desired_qty =  MAX_UNITS;
    if (desired_qty < -MAX_UNITS) desired_qty = -MAX_UNITS;

    Position& pos = positions_[sig.instrument_token];
    int32_t   current_qty = pos.qty;
    int32_t   delta = desired_qty - current_qty;

    if (delta == 0) return false; // No change needed

    // Pre-trade risk check
    if (!risk_.allow(desired_qty, total_realized_pnl())) {
        std::cout << "[StrategyEngine] RISK BLOCKED token=" << sig.instrument_token
                  << " desired=" << desired_qty << " pnl=" << total_realized_pnl() << "\n";
        return false;
    }

    // Build OrderIntent
    out.timestamp_ns     = alpha::time::Timestamp::now_ns();
    out.instrument_token = sig.instrument_token;
    out.price            = sig.price;
    out.qty              = std::abs(delta);
    out.side             = (delta > 0) ? OrderSide::BUY : OrderSide::SELL;
    out.reason           = classify_reason(current_qty, desired_qty);
    out.confidence       = sig.confidence;
    out.strategy_id      = sig.strategy_id;
    std::strncpy(out.symbol, sig.symbol, sizeof(out.symbol) - 1);
    out.symbol[sizeof(out.symbol) - 1] = '\0';

    // Simulate fill (assume market order fills at signal price)
    pos.apply_fill(delta, sig.price, out.timestamp_ns);

    log_intent(out, pos);
    return true;
}

double StrategyEngine::total_realized_pnl() const {
    double total = 0.0;
    for (const auto& [token, pos] : positions_) {
        total += pos.realized_pnl;
    }
    return total;
}

OrderReason StrategyEngine::classify_reason(int32_t current_qty, int32_t desired_qty) const {
    if (current_qty == 0) return OrderReason::ENTRY;
    if (desired_qty == 0) return OrderReason::EXIT;

    bool same_dir = (current_qty > 0 && desired_qty > 0) || (current_qty < 0 && desired_qty < 0);
    if (same_dir) {
        return (std::abs(desired_qty) > std::abs(current_qty))
            ? OrderReason::SCALE_IN
            : OrderReason::SCALE_OUT;
    }
    return OrderReason::REVERSE;
}

void StrategyEngine::log_intent(const OrderIntent& intent, const Position& pos) const {
    const char* side_str   = (intent.side == OrderSide::BUY) ? "BUY " : "SELL";
    const char* reason_str = "UNKNOWN";
    switch (intent.reason) {
        case OrderReason::ENTRY:     reason_str = "ENTRY    "; break;
        case OrderReason::EXIT:      reason_str = "EXIT     "; break;
        case OrderReason::SCALE_IN:  reason_str = "SCALE_IN "; break;
        case OrderReason::SCALE_OUT: reason_str = "SCALE_OUT"; break;
        case OrderReason::REVERSE:   reason_str = "REVERSE  "; break;
    }

    std::cout << std::fixed << std::setprecision(2)
              << "[ORDER] " << reason_str
              << " | " << side_str
              << " " << std::setw(4) << intent.qty << " x " << intent.symbol
              << " @ " << intent.price
              << " | net_pos=" << pos.qty
              << " | kelly=" << std::setprecision(3) << intent.confidence
              << " | rpnl=" << std::setprecision(2) << pos.realized_pnl
              << " | total_pnl=" << total_realized_pnl()
              << "\n";
}

} // namespace alpha::strategy
