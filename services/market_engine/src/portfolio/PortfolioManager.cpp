#include "PortfolioManager.hpp"
#include "../core/MarginModel.hpp"
#include <iostream>
#include <iomanip>
#include <stdexcept>

namespace alpha::market {

static const Position FLAT_POSITION{};

PortfolioManager::PortfolioManager(double starting_capital)
    : starting_capital_(starting_capital), cash_(starting_capital), margin_used_(0.0) {}

bool PortfolioManager::apply_trade(const Trade& trade) {
    // Compute margin required for this order
    auto margin = compute_margin(trade.segment, trade.side,
                                 trade.qty, trade.fill_price, trade.lot_size);

    // For opening/scaling trades, check we have enough cash + free margin
    double cash_needed = 0.0;
    if (trade.side == 1) { // BUY
        // Delivery: full cash; intraday/F&O: margin only
        if (trade.segment == Segment::EQUITY_DELIVERY) {
            cash_needed = trade.turnover + trade.charges.total;
        } else {
            cash_needed = margin.required_margin + trade.charges.total;
        }
    } else { // SELL (short or close)
        // For sells that open/extend short: need margin
        Position& pos = positions_[trade.instrument_token];
        if (pos.qty >= 0) {
            // Selling beyond current long → short position being opened
            cash_needed = margin.required_margin + trade.charges.total;
        } else {
            // Closing an existing short: release margin, but pay charges
            cash_needed = trade.charges.total;
        }
    }

    if (cash_needed > cash_ + margin_used_) {
        // Insufficient funds — in paper trading, warn but allow (flag only)
        std::cout << "[Portfolio] WARNING: Insufficient funds for trade on "
                  << trade.symbol << " (need ₹" << std::fixed << std::setprecision(2)
                  << cash_needed << ", available ₹" << cash_ << ")\n";
    }

    // Apply fill to position
    Position& pos = positions_[trade.instrument_token];
    if (pos.qty == 0) {
        // Initializing a new position slot
        pos.instrument_token = trade.instrument_token;
        std::strncpy(pos.symbol, trade.symbol, sizeof(pos.symbol) - 1);
        pos.segment  = trade.segment;
        pos.lot_size = trade.lot_size;
    }

    const int32_t signed_qty = trade.side * trade.qty;
    pos.apply_fill(signed_qty, trade.fill_price, trade.charges.total, trade.timestamp_ns);

    // Update cash: BUY costs money, SELL returns money (net of charges)
    cash_ -= trade.net_amount;

    // Update margin
    // Release existing margin for this instrument, re-compute for new position
    // (simplified: track per-position margin separately)
    // TODO: precise margin release on partial close — approximate here
    auto new_margin = compute_margin(trade.segment, 1,
                                     std::abs(pos.qty), trade.fill_price, trade.lot_size);
    // Rough margin update: recalculate from scratch based on new position size
    if (pos.is_flat()) {
        // Released — no margin needed
    }

    trades_.push_back(trade);
    ++trade_count_;

    return true;
}

void PortfolioManager::mark_to_market(uint32_t token, double current_price) {
    auto it = positions_.find(token);
    if (it != positions_.end() && !it->second.is_flat()) {
        // MTM is computed on-demand via Position::unrealized_pnl()
        (void)current_price; // stored externally when needed
    }
}

const Position& PortfolioManager::position(uint32_t token) const {
    auto it = positions_.find(token);
    if (it != positions_.end()) return it->second;
    return FLAT_POSITION;
}

PortfolioSnapshot PortfolioManager::snapshot() const {
    PortfolioSnapshot s{};
    s.cash         = cash_;
    s.margin_used  = margin_used_;
    s.total_trades = trade_count_;

    for (const auto& [token, pos] : positions_) {
        if (!pos.is_flat()) ++s.open_positions;
        s.gross_realized_pnl  += pos.realized_pnl;
        s.total_charges_paid  += pos.total_charges;
    }
    s.net_realized_pnl = s.gross_realized_pnl;
    s.total_equity     = cash_ + s.gross_realized_pnl;

    return s;
}

void PortfolioManager::eod_square_off(uint32_t token, double last_price) {
    auto it = positions_.find(token);
    if (it == positions_.end() || it->second.is_flat()) return;

    Position& pos = it->second;
    if (pos.segment == Segment::EQUITY_INTRADAY) {
        // Auto square-off intraday position at last price
        double pnl = pos.unrealized_pnl(last_price);
        pos.realized_pnl += pnl;
        cash_ += pnl;
        pos.qty          = 0;
        pos.avg_cost     = 0.0;
        pos.opened_at_ns = 0;
        std::cout << "[Portfolio] EOD square-off " << pos.symbol
                  << " P&L: ₹" << std::fixed << std::setprecision(2) << pnl << "\n";
    }
}

void PortfolioManager::print_summary() const {
    auto s = snapshot();
    std::cout << "\n========== Portfolio Summary ==========\n"
              << std::fixed << std::setprecision(2)
              << "  Cash available   : ₹" << s.cash << "\n"
              << "  Realized P&L     : ₹" << s.net_realized_pnl << "\n"
              << "  Total charges    : ₹" << s.total_charges_paid << "\n"
              << "  Net equity (MTM) : ₹" << s.total_equity << "\n"
              << "  Total trades     : "  << s.total_trades << "\n"
              << "  Open positions   : "  << s.open_positions << "\n"
              << "=======================================\n";
}

void PortfolioManager::update_margin(uint32_t /*token*/, double delta_margin) {
    margin_used_ += delta_margin;
    cash_        -= delta_margin;
    if (margin_used_ < 0.0) margin_used_ = 0.0;
}

} // namespace alpha::market
