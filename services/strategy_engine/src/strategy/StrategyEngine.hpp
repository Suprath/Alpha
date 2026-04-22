#pragma once

#include <unordered_map>
#include <alpha/models/MarketModels.hpp>
#include "../core/Position.hpp"
#include "../core/OrderIntent.hpp"
#include "../core/RiskManager.hpp"

namespace alpha::strategy {

/**
 * @brief Core trading decision engine.
 *
 * Receives Signal structs from the signal engine (via shared memory),
 * maintains per-instrument position state, applies risk checks, and
 * generates OrderIntents.
 *
 * Position sizing: desired_qty = action * round(MAX_UNITS * confidence)
 * Orders are generated for the delta between desired and current position.
 */
class StrategyEngine {
public:
    static constexpr int32_t MAX_UNITS = 100; // One NSE lot
    static constexpr int32_t MIN_TRADE_QTY = 10; // Don't trade for less than 10 shares
    static constexpr double  HYSTERESIS_THRESHOLD = 0.05; // 5% capacity change required

    explicit StrategyEngine(RiskParams risk_params = {});

    /**
     * @brief Process an incoming signal and generate an order if warranted.
     * @return true if an OrderIntent was generated (written to out).
     */
    bool on_signal(const alpha::models::Signal& sig, OrderIntent& out);

    /**
     * @brief Total realized P&L across all instruments (for risk checks).
     */
    double total_realized_pnl() const;

    const std::unordered_map<uint32_t, Position>& positions() const { return positions_; }

private:
    std::unordered_map<uint32_t, Position> positions_;
    RiskManager risk_;

    OrderReason classify_reason(int32_t current_qty, int32_t desired_qty) const;
    void log_intent(const OrderIntent& intent, const Position& pos) const;
};

} // namespace alpha::strategy
