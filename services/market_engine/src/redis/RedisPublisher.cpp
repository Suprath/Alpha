#include "RedisPublisher.hpp"
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <sstream>

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

redisReply* RedisPublisher::cmd(const char* fmt, ...) {
    if (!is_connected() && !try_reconnect()) return nullptr;

    va_list args;
    va_start(args, fmt);
    auto* reply = static_cast<redisReply*>(redisvCommand(ctx_, fmt, args));
    va_end(args);

    if (!reply && ctx_->err) {
        std::cerr << "[RedisPublisher] Command error: " << ctx_->errstr << "\n";
        redisFree(ctx_); ctx_ = nullptr;
        return nullptr;
    }
    return reply;
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

    // Build HSET alpha:portfolio field value [field value ...]
    // We store summary fields + one encoded field per position.
    char cash_s[32], equity_s[32], rpnl_s[32], chg_s[32], sc_s[32], ts_s[32];
    std::snprintf(cash_s,   sizeof(cash_s),   "%.2f", cash);
    std::snprintf(equity_s, sizeof(equity_s), "%.2f", equity);
    std::snprintf(rpnl_s,   sizeof(rpnl_s),   "%.2f", realized_pnl);
    std::snprintf(chg_s,    sizeof(chg_s),    "%.2f", total_charges);
    std::snprintf(sc_s,     sizeof(sc_s),     "%.2f", starting_capital);
    std::snprintf(ts_s,     sizeof(ts_s),     "%llu", (unsigned long long)ts_ns);

    // Core snapshot fields
    auto* r = cmd(
        "HSET alpha:portfolio"
        " cash %s"
        " equity %s"
        " realized_pnl %s"
        " total_charges %s"
        " open_positions %d"
        " total_trades %d"
        " starting_capital %s"
        " ts %s",
        cash_s, equity_s, rpnl_s, chg_s,
        open_positions, total_trades, sc_s, ts_s);
    if (r) freeReplyObject(r);

    // Build pos_tokens comma list + per-position fields
    std::ostringstream tokens_oss;
    for (size_t i = 0; i < positions.size(); ++i) {
        const auto& p = positions[i];
        if (i > 0) tokens_oss << ",";
        tokens_oss << p.token;

        // "SYMBOL,qty,avg_cost,realized_pnl,unrealized_pnl"
        char pos_val[128];
        std::snprintf(pos_val, sizeof(pos_val), "%s,%d,%.4f,%.2f,%.2f",
                      p.symbol, p.qty, p.avg_cost, p.realized_pnl, p.unrealized_pnl);

        char field_name[32];
        std::snprintf(field_name, sizeof(field_name), "pos_%u", p.token);

        auto* pr = cmd("HSET alpha:portfolio %s %s", field_name, pos_val);
        if (pr) freeReplyObject(pr);
    }

    auto* tr = cmd("HSET alpha:portfolio pos_tokens %s",
                   tokens_oss.str().c_str());
    if (tr) freeReplyObject(tr);
}

void RedisPublisher::publish_trade(
    uint64_t trade_id,
    const char* symbol,
    int32_t  side,
    int32_t  qty,
    double   fill_price,
    double   charges,
    double   cash_after,
    double   realized_pnl,
    uint64_t ts_ns)
{
    char fill_s[32], chg_s[32], cash_s[32], rpnl_s[32], ts_s[32];
    std::snprintf(fill_s, sizeof(fill_s), "%.4f", fill_price);
    std::snprintf(chg_s,  sizeof(chg_s),  "%.2f", charges);
    std::snprintf(cash_s, sizeof(cash_s), "%.2f", cash_after);
    std::snprintf(rpnl_s, sizeof(rpnl_s), "%.2f", realized_pnl);
    std::snprintf(ts_s,   sizeof(ts_s),   "%llu", (unsigned long long)ts_ns);

    // XADD alpha:trades MAXLEN ~ 500 * <fields>
    auto* r = cmd(
        "XADD alpha:trades MAXLEN ~ 500 *"
        " id %llu"
        " sym %s"
        " side %d"
        " qty %d"
        " fill %s"
        " charges %s"
        " cash %s"
        " rpnl %s"
        " ts %s",
        (unsigned long long)trade_id,
        symbol, side, qty,
        fill_s, chg_s, cash_s, rpnl_s, ts_s);
    if (r) freeReplyObject(r);
}

} // namespace alpha::market
