#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <cstdint>
#include <iostream>
#include <cstring>

// Generated from shared/proto/alpha_backtest.proto at build time
#include "alpha_backtest.pb.h"

#include <alpha/backtest/ResultAggregator.hpp>

namespace alpha {
namespace backtest {

/**
 * BacktestRedisPublisher — publishes backtest status + results to Redis.
 *
 * Redis key schema:
 *   alpha:backtest:status   HSET field "data" → BacktestStatus proto (updated during run)
 *   alpha:backtest:result   HSET field "data" → BacktestResultProto (written on completion)
 *   alpha:backtest:results  XADD MAXLEN 20    → stream of completed run results (history)
 *
 * Non-blocking: failures are logged and skipped — never stall the hot loop.
 */
class BacktestRedisPublisher {
public:
    BacktestRedisPublisher() = default;
    ~BacktestRedisPublisher() {
        if (ctx_) redisFree(ctx_);
    }

    bool connect(const std::string& host = "redis", int port = 6379) {
        host_ = host;
        port_ = port;
        ctx_ = redisConnect(host.c_str(), port);
        if (!ctx_ || ctx_->err) {
            std::cerr << "[BacktestRedis] Cannot connect to " << host << ":" << port;
            if (ctx_) std::cerr << " — " << ctx_->errstr;
            std::cerr << std::endl;
            return false;
        }
        std::cout << "[BacktestRedis] Connected to " << host << ":" << port << std::endl;
        return true;
    }

    bool is_connected() const { return ctx_ && ctx_->err == 0; }

    void publish_status(
        alpha::backtest::BacktestRunState state,
        uint32_t  instrument_token,
        uint64_t  ticks_processed,
        uint64_t  total_ticks,
        const std::string& symbol   = "",
        const std::string& message  = "")
    {
        if (!ctx_) return;

        ::alpha::backtest::BacktestStatus s;
        s.set_state(state);
        s.set_instrument_token(instrument_token);
        s.set_ticks_processed(ticks_processed);
        s.set_total_ticks(total_ticks);
        s.set_progress_pct(total_ticks > 0
            ? (static_cast<double>(ticks_processed) / total_ticks) * 100.0
            : 0.0);
        s.set_timestamp_ns(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        s.set_symbol(symbol);
        s.set_message(message);

        const std::string payload = s.SerializeAsString();
        hset_binary("alpha:backtest:status", "data", payload);
    }

    void publish_result(
        const BacktestResult& result,
        uint32_t  instrument_token,
        const std::string& symbol,
        double    starting_capital,
        double    final_equity)
    {
        if (!ctx_) return;

        ::alpha::backtest::BacktestResultProto r;
        r.set_total_net_pnl(result.total_net_pnl);
        r.set_total_gross_pnl(result.total_gross_pnl);
        r.set_total_commission(result.total_commission);
        r.set_starting_capital(starting_capital);
        r.set_final_equity(final_equity);
        r.set_total_return_pct(starting_capital > 0
            ? (final_equity - starting_capital) / starting_capital * 100.0
            : 0.0);
        r.set_sharpe_ratio(result.sharpe_ratio);
        r.set_sortino_ratio(result.sortino_ratio);
        r.set_calmar_ratio(result.calmar_ratio);
        r.set_max_drawdown(result.max_drawdown);
        r.set_max_drawdown_pct(result.max_drawdown_pct);
        r.set_total_trades(result.total_trades);
        r.set_winning_trades(result.winning_trades);
        r.set_losing_trades(result.losing_trades);
        r.set_win_rate(result.win_rate);
        r.set_profit_factor(result.profit_factor);
        r.set_avg_trade_pnl(result.avg_trade_pnl);
        r.set_avg_win(result.avg_win);
        r.set_avg_loss(result.avg_loss);
        r.set_largest_win(result.largest_win);
        r.set_largest_loss(result.largest_loss);
        r.set_expectancy(result.expectancy);
        r.set_instrument_token(instrument_token);
        r.set_symbol(symbol);
        r.set_run_timestamp_ns(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));

        const std::string payload = r.SerializeAsString();

        // Write latest result
        hset_binary("alpha:backtest:result", "data", payload);

        // Append to history stream (capped at 20 entries)
        xadd_binary("alpha:backtest:results", payload);
    }

private:
    redisContext* ctx_  = nullptr;
    std::string   host_;
    int           port_ = 6379;

    void hset_binary(const std::string& key, const std::string& field,
                     const std::string& value) {
        const char*  argv[4]    = { "HSET", key.c_str(), field.c_str(), value.c_str() };
        const size_t argvlen[4] = { 4, key.size(), field.size(), value.size() };
        redisReply* reply = static_cast<redisReply*>(
            redisCommandArgv(ctx_, 4, argv, argvlen));
        if (reply) freeReplyObject(reply);
    }

    void xadd_binary(const std::string& key, const std::string& value) {
        // XADD <key> MAXLEN ~ 20 * data <value>
        const char*  argv[7]    = { "XADD", key.c_str(), "MAXLEN", "~", "20", "data", value.c_str() };
        const size_t argvlen[7] = { 4, key.size(), 6, 1, 2, 4, value.size() };
        redisReply* reply = static_cast<redisReply*>(
            redisCommandArgv(ctx_, 7, argv, argvlen));
        if (reply) freeReplyObject(reply);
    }
};

} // namespace backtest
} // namespace alpha
