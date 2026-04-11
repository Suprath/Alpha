#include <alpha/backtest/ResultAggregator.hpp>

#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace alpha {
namespace backtest {

void ResultAggregator::record_trade(Trade t) {
    trades_.push_back(std::move(t));
}

void ResultAggregator::record_equity(uint64_t ts_ns, double equity) {
    equity_curve_.emplace_back(ts_ns, equity);
}

void ResultAggregator::reset() {
    trades_.clear();
    equity_curve_.clear();
}

BacktestResult ResultAggregator::compute() const {
    BacktestResult r{};

    if (trades_.empty()) return r;

    // ── Trade statistics ───────────────────────────────────────────────────
    r.total_trades = trades_.size();

    double gross_profit = 0.0;
    double gross_loss   = 0.0;

    for (const auto& t : trades_) {
        r.total_net_pnl    += t.net_pnl;
        r.total_gross_pnl  += t.gross_pnl;
        r.total_commission += t.commission;

        if (t.net_pnl > 0.0) {
            ++r.winning_trades;
            gross_profit += t.net_pnl;
            r.largest_win = std::max(r.largest_win, t.net_pnl);
        } else if (t.net_pnl < 0.0) {
            ++r.losing_trades;
            gross_loss += std::abs(t.net_pnl);
            r.largest_loss = std::min(r.largest_loss, t.net_pnl);
        }
    }

    r.win_rate    = (r.total_trades > 0) ? (static_cast<double>(r.winning_trades) / r.total_trades) : 0.0;
    r.avg_trade_pnl = r.total_net_pnl / static_cast<double>(r.total_trades);
    r.avg_win  = (r.winning_trades > 0) ? (gross_profit / r.winning_trades) : 0.0;
    r.avg_loss = (r.losing_trades  > 0) ? (-gross_loss  / r.losing_trades)  : 0.0;
    r.profit_factor = (gross_loss > 0.0) ? (gross_profit / gross_loss) : std::numeric_limits<double>::infinity();
    r.expectancy = r.win_rate * r.avg_win + (1.0 - r.win_rate) * r.avg_loss;  // avg_loss is negative

    // ── Drawdown from equity curve ─────────────────────────────────────────
    if (!equity_curve_.empty()) {
        double peak = equity_curve_[0].second;
        for (const auto& [ts, eq] : equity_curve_) {
            if (eq > peak) peak = eq;
            double dd = peak - eq;
            if (dd > r.max_drawdown) {
                r.max_drawdown = dd;
                r.max_drawdown_pct = (peak > 0.0) ? (dd / peak * 100.0) : 0.0;
            }
        }
    }

    // ── Daily returns for Sharpe / Sortino ────────────────────────────────
    // Bucket equity curve into daily returns
    if (equity_curve_.size() >= 2) {
        constexpr uint64_t NS_PER_DAY = 86'400'000'000'000ULL;

        std::vector<double> daily_returns;
        uint64_t bucket_start = equity_curve_[0].first;
        double   bucket_eq    = equity_curve_[0].second;
        double   prev_day_eq  = equity_curve_[0].second;

        for (const auto& [ts, eq] : equity_curve_) {
            if (ts - bucket_start >= NS_PER_DAY) {
                if (prev_day_eq > 0.0)
                    daily_returns.push_back((bucket_eq - prev_day_eq) / prev_day_eq);
                prev_day_eq  = bucket_eq;
                bucket_start = ts;
            }
            bucket_eq = eq;
        }

        if (!daily_returns.empty()) {
            double mean = std::accumulate(daily_returns.begin(), daily_returns.end(), 0.0)
                        / static_cast<double>(daily_returns.size());

            double variance = 0.0;
            double downside_variance = 0.0;
            for (double r_i : daily_returns) {
                variance += (r_i - mean) * (r_i - mean);
                if (r_i < 0.0) downside_variance += r_i * r_i;
            }
            variance         /= static_cast<double>(daily_returns.size());
            downside_variance /= static_cast<double>(daily_returns.size());

            double std_dev         = std::sqrt(variance);
            double downside_std    = std::sqrt(downside_variance);
            constexpr double SQRT252 = 15.8745;  // √252

            r.sharpe_ratio  = (std_dev > 0.0)      ? (mean / std_dev * SQRT252) : 0.0;
            r.sortino_ratio = (downside_std > 0.0)  ? (mean / downside_std * SQRT252) : 0.0;
        }
    }

    return r;
}

void BacktestResult::print() const {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n══════════════════ Backtest Results ══════════════════\n";
    std::cout << "  Net P&L          : ₹" << total_net_pnl << "\n";
    std::cout << "  Gross P&L        : ₹" << total_gross_pnl << "\n";
    std::cout << "  Total Commission : ₹" << total_commission << "\n";
    std::cout << "  ───────────────────────────────────────────────────\n";
    std::cout << "  Total Trades     : " << total_trades << "\n";
    std::cout << "  Win Rate         : " << (win_rate * 100.0) << "%\n";
    std::cout << "  Profit Factor    : " << profit_factor << "\n";
    std::cout << "  Avg Trade P&L    : ₹" << avg_trade_pnl << "\n";
    std::cout << "  Avg Win          : ₹" << avg_win << "\n";
    std::cout << "  Avg Loss         : ₹" << avg_loss << "\n";
    std::cout << "  Largest Win      : ₹" << largest_win << "\n";
    std::cout << "  Largest Loss     : ₹" << largest_loss << "\n";
    std::cout << "  Expectancy       : ₹" << expectancy << "\n";
    std::cout << "  ───────────────────────────────────────────────────\n";
    std::cout << "  Max Drawdown     : ₹" << max_drawdown
              << " (" << max_drawdown_pct << "%)\n";
    std::cout << "  Sharpe Ratio     : " << sharpe_ratio << "\n";
    std::cout << "  Sortino Ratio    : " << sortino_ratio << "\n";
    std::cout << "══════════════════════════════════════════════════════\n\n";
}

} // namespace backtest
} // namespace alpha
