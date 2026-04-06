/**
 * @file PnLTracker.hpp
 * @brief Per-instrument realized and unrealized P&L tracker.
 *
 * Realized P&L:
 *   Π(t) = Q · (P(t) - P_entry) · direction
 *
 *   Q         = position size (shares / lots)
 *   P(t)      = current mark price
 *   P_entry   = entry price at open
 *   direction = +1 (long) or -1 (short)
 *
 * Unrealized P&L (includes transaction costs):
 *   uPnL(t) = Π(t) - costs
 *
 *   costs = brokerage + STT + slippage  (per-trade, set at open)
 *
 * NSE default cost model (configurable):
 *   brokerage = flat ₹20 per order (Zerodha model)
 *   STT       = 0.025% of turnover (sell-side equity delivery)
 *   slippage  = N × tick_size (tick_size = ₹0.05 for equities)
 *
 * Usage:
 *   open_position()  — at signal fire
 *   update()         — every bar (mark-to-market)
 *   close_position() — at exit signal or end of session
 */

#pragma once

#include <cstdint>
#include <unordered_map>
#include <limits>

namespace alpha::signal::signals::derived {

// ─── Cost model ───────────────────────────────────────────────────────────────

struct CostModel {
    double brokerage    = 20.0;   // ₹ flat per order (entry or exit)
    double stt_rate     = 0.00025;// 0.025% of turnover (sell side)
    double tick_size    = 0.05;   // ₹0.05 for NSE equities
    double slippage_ticks = 2.0;  // estimated slippage in ticks
};

// ─── Position state ───────────────────────────────────────────────────────────

struct Position {
    double   entry_price  = 0.0;
    int64_t  quantity     = 0;    // positive = long, negative = short
    int8_t   direction    = 0;    // +1 long, -1 short
    double   entry_costs  = 0.0;  // costs paid at open
    bool     is_open      = false;
};

// ─── Result ──────────────────────────────────────────────────────────────────

struct PnLResult {
    uint32_t instrument_token;
    double   gross_pnl;     // Π(t) = Q · (P - P_entry) · direction
    double   costs;         // total costs (entry + estimated exit)
    double   unrealized_pnl;// Π(t) - costs
    bool     is_open;       // position is active
};

// ─── P&L Tracker ─────────────────────────────────────────────────────────────

class PnLTracker {
public:
    explicit PnLTracker(CostModel costs = CostModel{}) : cost_model_(costs) {}

    /**
     * @brief Open a new position for an instrument.
     *        Computes and records entry costs.
     *
     * @param token      Instrument token
     * @param entry_px   Execution price (after slippage)
     * @param quantity   Number of shares/lots (must be > 0)
     * @param direction  +1 long, -1 short
     */
    void open_position(uint32_t token, double entry_px,
                       uint64_t quantity, int8_t direction) {
        Position& pos     = positions_[token];
        pos.entry_price   = entry_px;
        pos.quantity      = static_cast<int64_t>(quantity);
        pos.direction     = direction;
        pos.is_open       = true;

        // Entry costs: brokerage + slippage
        const double turnover     = entry_px * static_cast<double>(quantity);
        const double slippage_val = cost_model_.slippage_ticks
                                  * cost_model_.tick_size
                                  * static_cast<double>(quantity);
        pos.entry_costs = cost_model_.brokerage + slippage_val;
        (void)turnover; // STT only on exit (sell side)
    }

    /**
     * @brief Mark-to-market the position at current price.
     *        Call every bar while position is open.
     */
    [[nodiscard]]
    PnLResult update(uint32_t token, double current_price) const {
        PnLResult result;
        result.instrument_token = token;

        const auto it = positions_.find(token);
        if (it == positions_.end() || !it->second.is_open) {
            result.gross_pnl      = 0.0;
            result.costs          = 0.0;
            result.unrealized_pnl = 0.0;
            result.is_open        = false;
            return result;
        }

        const Position& pos = it->second;
        const double    Q   = static_cast<double>(pos.quantity);

        // ── Realized gross P&L ────────────────────────────────────────────────
        // Π(t) = Q · (P(t) - P_entry) · direction
        const double gross = Q * (current_price - pos.entry_price)
                           * static_cast<double>(pos.direction);

        // ── Estimated exit costs (STT on sell + brokerage + slippage) ────────
        const double exit_turnover  = current_price * Q;
        const double exit_stt       = (pos.direction == +1)
            ? exit_turnover * cost_model_.stt_rate  // long → sell to exit
            : 0.0;                                   // short → buy to exit (no STT)
        const double exit_slippage  = cost_model_.slippage_ticks
                                    * cost_model_.tick_size * Q;
        const double total_costs    = pos.entry_costs
                                    + cost_model_.brokerage
                                    + exit_stt
                                    + exit_slippage;

        result.gross_pnl      = gross;
        result.costs          = total_costs;
        result.unrealized_pnl = gross - total_costs;  // uPnL = Π - costs
        result.is_open        = true;
        return result;
    }

    /**
     * @brief Close the position — finalizes P&L and clears state.
     * @return Final PnLResult at the given exit price.
     */
    PnLResult close_position(uint32_t token, double exit_price) {
        PnLResult result = update(token, exit_price);
        if (auto it = positions_.find(token); it != positions_.end()) {
            it->second.is_open = false;
        }
        return result;
    }

    bool has_open_position(uint32_t token) const {
        const auto it = positions_.find(token);
        return (it != positions_.end()) && it->second.is_open;
    }

    void reset() noexcept { positions_.clear(); }

private:
    std::unordered_map<uint32_t, Position> positions_;
    CostModel                              cost_model_;
};

} // namespace alpha::signal::signals::derived
