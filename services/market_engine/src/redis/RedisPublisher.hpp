#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <cstdint>
#include <vector>

namespace alpha::market {

/**
 * RedisPublisher — fire-and-forget Redis sink for portfolio + trade events.
 *
 * Publishes to two keys:
 *   HSET alpha:portfolio    — live snapshot (cash, equity, P&L, positions)
 *   XADD alpha:trades       — append-only trade blotter stream
 *
 * Non-blocking: any Redis failure silently logs to stderr and is skipped.
 * The market engine main loop is never stalled.
 */
class RedisPublisher {
public:
    RedisPublisher() = default;
    ~RedisPublisher();

    // Non-copyable
    RedisPublisher(const RedisPublisher&) = delete;
    RedisPublisher& operator=(const RedisPublisher&) = delete;

    /** Connect to Redis. Returns false if unreachable (publisher becomes a no-op). */
    bool connect(const std::string& host = "redis", int port = 6379);

    bool is_connected() const { return ctx_ && ctx_->err == 0; }

    /**
     * Publish a full portfolio snapshot to HSET alpha:portfolio.
     * Also serializes open positions into pos_tokens + pos_<token> fields.
     */
    struct PositionSnapshot {
        uint32_t token;
        const char* symbol;
        int32_t  qty;
        double   avg_cost;
        double   realized_pnl;
        double   unrealized_pnl;
    };

    void publish_portfolio(
        double   cash,
        double   equity,
        double   realized_pnl,
        double   total_charges,
        int32_t  open_positions,
        int32_t  total_trades,
        double   starting_capital,
        const std::vector<PositionSnapshot>& positions,
        uint64_t ts_ns);

    /**
     * Append a fill to the XADD alpha:trades stream (maxlen=500).
     */
    void publish_trade(
        uint64_t trade_id,
        const char* symbol,
        int32_t  side,
        int32_t  qty,
        double   fill_price,
        double   charges,
        double   cash_after,
        double   realized_pnl,
        uint64_t ts_ns);

private:
    redisContext* ctx_{nullptr};
    std::string   host_;
    int           port_{6379};

    /** Try to reconnect once; returns true on success. */
    bool try_reconnect();

    /** Execute a command; reconnects once on failure. Returns reply or nullptr. */
    redisReply* cmd(const char* fmt, ...);
};

} // namespace alpha::market
