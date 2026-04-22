#include "RedisPublisher.hpp"
#include "alpha_portfolio.pb.h"
#include <iostream>

namespace alpha::market {

RedisPublisher::~RedisPublisher() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisPublisher::connect(const std::string& host, int port) {
    host_ = host;
    port_ = port;
    if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }

    ctx_ = redisConnect(host.c_str(), port);
    if (!ctx_ || ctx_->err) {
        std::cerr << "[RedisPublisher] Connect failed: "
                  << (ctx_ ? ctx_->errstr : "OOM") << "\n";
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        return false;
    }
    std::cout << "[RedisPublisher] Connected to Redis at "
              << host << ":" << port << "\n";
    return true;
}

bool RedisPublisher::try_reconnect() {
    if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
    ctx_ = redisConnect(host_.c_str(), port_);
    if (!ctx_ || ctx_->err) {
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        return false;
    }
    return true;
}

void RedisPublisher::cmd_binary(int argc, const char** argv,
                                const size_t* argvlen) {
    if (!is_connected() && !try_reconnect()) return;

    auto* reply = static_cast<redisReply*>(
        redisCommandArgv(ctx_, argc, argv, argvlen));

    if (!reply) {
        std::cerr << "[RedisPublisher] Command error: " << ctx_->errstr << "\n";
        redisFree(ctx_); ctx_ = nullptr;
        return;
    }
    freeReplyObject(reply);
}

void RedisPublisher::publish_portfolio(
    double   cash,
    double   equity,
    double   realized_pnl,
    double   total_charges,
    int32_t  open_positions,
    int32_t  total_trades,
    double   starting_capital,
    const std::vector<PositionSnapshot>& positions,
    uint64_t ts_ns)
{
    if (!is_connected() && !try_reconnect()) return;

    // Build PortfolioSnapshot proto
    alpha::portfolio::PortfolioSnapshot snap;
    snap.set_cash(cash);
    snap.set_equity(equity);
    snap.set_realized_pnl(realized_pnl);
    snap.set_total_charges(total_charges);
    snap.set_open_positions(open_positions);
    snap.set_total_trades(total_trades);
    snap.set_starting_capital(starting_capital);
    snap.set_timestamp_ns(ts_ns);

    for (const auto& p : positions) {
        auto* pos = snap.add_positions();
        pos->set_token(p.token);
        pos->set_symbol(p.symbol);
        pos->set_qty(p.qty);
        pos->set_avg_cost(p.avg_cost);
        pos->set_realized_pnl(p.realized_pnl);
        pos->set_unrealized_pnl(p.unrealized_pnl);
    }

    std::string bytes;
    if (!snap.SerializeToString(&bytes)) {
        std::cerr << "[RedisPublisher] Failed to serialize PortfolioSnapshot\n";
        return;
    }

    // HSET alpha:portfolio data <binary>  (binary-safe via redisCommandArgv)
    const char* argv[] = {"HSET", "alpha:portfolio", "data", bytes.data()};
    const size_t lens[] = {4, 15, 4, bytes.size()};
    cmd_binary(4, argv, lens);
}

void RedisPublisher::publish_trade(
    uint64_t    trade_id,
    const char* symbol,
    int32_t     side,
    int32_t     qty,
    double      fill_price,
    double      charges,
    double      cash_after,
    double      realized_pnl,
    uint64_t    ts_ns)
{
    if (!is_connected() && !try_reconnect()) return;

    // Build Trade proto
    alpha::portfolio::Trade trade;
    trade.set_trade_id(trade_id);
    trade.set_symbol(symbol);
    trade.set_side(side);
    trade.set_qty(qty);
    trade.set_fill_price(fill_price);
    trade.set_charges(charges);
    trade.set_cash_after(cash_after);
    trade.set_realized_pnl(realized_pnl);
    trade.set_timestamp_ns(ts_ns);

    std::string bytes;
    if (!trade.SerializeToString(&bytes)) {
        std::cerr << "[RedisPublisher] Failed to serialize Trade\n";
        return;
    }

    // XADD alpha:trades MAXLEN ~ 500 * data <binary>
    const char* argv[] = {
        "XADD", "alpha:trades", "MAXLEN", "~", "500", "*", "data", bytes.data()
    };
    const size_t lens[] = {4, 12, 6, 1, 3, 1, 4, bytes.size()};
    cmd_binary(8, argv, lens);
}

} // namespace alpha::market
