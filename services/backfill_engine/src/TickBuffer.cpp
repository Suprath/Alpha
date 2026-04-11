#include <alpha/backfill/TickBuffer.hpp>

#include <cmath>
#include <stdexcept>

namespace alpha {
namespace backfill {

TickBuffer::TickBuffer(size_t rolling_window, double bars_per_year)
    : rolling_window_(rolling_window)
    , bars_per_year_(bars_per_year)
{
    window_.resize(rolling_window_);
}

void TickBuffer::reset() {
    window_head_     = 0;
    window_full_     = false;
    cumul_pv_        = 0.0;
    cumul_v_         = 0.0;
    bars_processed_  = 0;
}

void TickBuffer::push(const models::Candle& candle,
                      uint32_t              instrument_token,
                      const std::string&    symbol,
                      const std::string&    interval,
                      BarCallback           bar_cb,
                      TickCallback          tick_cb) {
    // ── 1. Update circular rolling window ────────────────────────────────
    window_[window_head_] = candle;
    window_head_ = (window_head_ + 1) % rolling_window_;
    if (!window_full_ && window_head_ == 0) window_full_ = true;

    // ── 2. Update cumulative VWAP accumulators ────────────────────────────
    cumul_pv_ += candle.close * static_cast<double>(candle.volume);
    cumul_v_  += static_cast<double>(candle.volume);

    double current_vwap = (cumul_v_ > 0.0) ? (cumul_pv_ / cumul_v_) : candle.close;

    // ── 3. Compute rolling signals ────────────────────────────────────────
    double ofi         = compute_ofi();
    double kyle_lambda = compute_kyle_lambda();
    double rvol        = compute_realized_vol();

    ++bars_processed_;

    // ── 4. Emit PreCalcBar ────────────────────────────────────────────────
    if (bar_cb) {
        PreCalcBar bar;
        bar.candle           = candle;
        bar.instrument_token = instrument_token;
        bar.symbol           = symbol;
        bar.interval         = interval;
        bar.ofi              = ofi;
        bar.kyle_lambda      = kyle_lambda;
        bar.vwap             = current_vwap;
        bar.realized_vol     = rvol;
        bar_cb(bar);
    }

    // ── 5. Normalize to BacktestTicks (OHLC synthetic ticks) ─────────────
    if (tick_cb) {
        auto ticks = candle.normalize_to_ticks();
        for (auto& raw_tick : ticks) {
            models::BacktestTick bt{};
            bt.timestamp_ns      = raw_tick.timestamp_ns;
            bt.instrument_token  = instrument_token;
            bt.last_price        = static_cast<float>(raw_tick.last_price);
            bt.volume            = static_cast<uint32_t>(candle.volume / 4);
            bt.bid_price         = static_cast<float>(raw_tick.last_price * 0.9999);
            bt.ask_price         = static_cast<float>(raw_tick.last_price * 1.0001);
            bt.vwap              = static_cast<float>(current_vwap);
            tick_cb(bt, symbol);
        }
    }
}

// ── Rolling signal computations ───────────────────────────────────────────

double TickBuffer::compute_ofi() const {
    // OFI proxy: Σ(volume × sign(close − open)) over rolling window
    size_t count = window_full_ ? rolling_window_ : window_head_;
    double ofi = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const auto& c = window_[i];
        double sign = (c.close >= c.open) ? 1.0 : -1.0;
        ofi += sign * static_cast<double>(c.volume);
    }
    return ofi;
}

double TickBuffer::compute_kyle_lambda() const {
    // Kyle's λ ≈ Σ|Δclose| / Σvolume over window (₹ per share per 100sh traded)
    size_t count = window_full_ ? rolling_window_ : window_head_;
    if (count < 2) return 0.0;

    double sum_dp = 0.0;
    double sum_dv = 0.0;

    // Walk in insertion order: head_ points to the oldest slot when full
    for (size_t i = 1; i < count; ++i) {
        size_t prev = (window_head_ + i - 1) % rolling_window_;
        size_t curr = (window_head_ + i)     % rolling_window_;
        double dp = std::abs(window_[curr].close - window_[prev].close);
        double dv = static_cast<double>(window_[curr].volume);
        sum_dp += dp;
        sum_dv += dv;
    }

    return (sum_dv > 0.0) ? (sum_dp / sum_dv * 100.0) : 0.0;
}

double TickBuffer::compute_realized_vol() const {
    // Annualized realized vol: std-dev of log-returns × √(bars_per_year)
    size_t count = window_full_ ? rolling_window_ : window_head_;
    if (count < 2) return 0.0;

    std::vector<double> log_rets;
    log_rets.reserve(count - 1);

    for (size_t i = 1; i < count; ++i) {
        size_t prev = (window_head_ + i - 1) % rolling_window_;
        size_t curr = (window_head_ + i)     % rolling_window_;
        if (window_[prev].close > 0.0 && window_[curr].close > 0.0)
            log_rets.push_back(std::log(window_[curr].close / window_[prev].close));
    }

    if (log_rets.empty()) return 0.0;

    double mean = 0.0;
    for (double r : log_rets) mean += r;
    mean /= static_cast<double>(log_rets.size());

    double var = 0.0;
    for (double r : log_rets) var += (r - mean) * (r - mean);
    var /= static_cast<double>(log_rets.size());

    return std::sqrt(var * bars_per_year_);
}

} // namespace backfill
} // namespace alpha
