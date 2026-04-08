/**
 * @file signal_engine_test.cpp
 * @brief Unit tests for all signal_engine calculator modules.
 *
 * Covers mathematical correctness of every formula across:
 *   OFI, TradeDirection, VPIN, Kyle's Lambda, Entropy,
 *   LogReturn, RealizedVolatility, KalmanFilter, CUSUM,
 *   BSGamma, GEX, VRP, SignalNormalizer, CompositeScore,
 *   PnLTracker, AlphaDecay
 */

#include <gtest/gtest.h>
#include <cmath>
#include <array>
#include <limits>

#include <alpha/models/MarketModels.hpp>
#include <core/EnhancedBar.hpp>

#include <signals/ofi/OFICalculator.hpp>
#include <signals/trade_direction/TradeDirection.hpp>
#include <signals/vpin/VPINCalculator.hpp>
#include <signals/kyles_lambda/KylesLambda.hpp>
#include <signals/entropy/EntropyCalculator.hpp>
#include <signals/bar/LogReturn.hpp>
#include <signals/bar/RealizedVolatility.hpp>
#include <signals/bar/KalmanFilter.hpp>
#include <signals/bar/CUSUM.hpp>
#include <signals/options/BSGamma.hpp>
#include <signals/options/OptionsChain.hpp>
#include <signals/options/GEXCalculator.hpp>
#include <signals/options/VRPCalculator.hpp>
#include <signals/derived/SignalBundle.hpp>
#include <signals/derived/SignalNormalizer.hpp>
#include <signals/derived/CompositeScore.hpp>
#include <signals/derived/PnLTracker.hpp>
#include <signals/derived/AlphaDecay.hpp>

// ─── Helpers ─────────────────────────────────────────────────────────────────

static alpha::models::Tick make_tick(uint32_t token,
                                     double bid_px, uint32_t bid_sz,
                                     double ask_px, uint32_t ask_sz,
                                     double last_px  = 0.0,
                                     uint32_t last_qty = 0) {
    alpha::models::Tick t{};
    t.instrument_token = token;
    t.bid_price        = bid_px;
    t.bid_size         = bid_sz;
    t.ask_price        = ask_px;
    t.ask_size         = ask_sz;
    t.last_price       = last_px;
    t.last_quantity    = last_qty;
    return t;
}

static alpha::signal::core::EnhancedBar make_bar(uint32_t token, double close,
                                                  uint64_t buy_vol = 0,
                                                  uint64_t sell_vol = 0,
                                                  uint64_t volume = 0) {
    alpha::signal::core::EnhancedBar b{};
    b.instrument_token = token;
    b.close            = close;
    b.buy_volume       = buy_vol;
    b.sell_volume      = sell_vol;
    b.volume           = volume;
    return b;
}

// ─── BSGamma Tests ────────────────────────────────────────────────────────────

using namespace alpha::signal::signals::options;

TEST(BSGammaTest, PhiAtZeroEqualsInvSqrt2Pi) {
    EXPECT_NEAR(phi(0.0), INV_SQRT_2PI, 1e-15);
}

TEST(BSGammaTest, PhiIsSymmetric) {
    EXPECT_NEAR(phi(1.5), phi(-1.5), 1e-15);
}

TEST(BSGammaTest, PhiDecaysAwayFromZero) {
    EXPECT_GT(phi(0.0), phi(1.0));
    EXPECT_GT(phi(1.0), phi(2.0));
}

TEST(BSGammaTest, D1AtMoneyZeroRate) {
    // At-the-money, r=0: d1 = (0 + sigma^2/2 * T) / (sigma * sqrt(T)) = sigma*sqrt(T)/2
    const double sigma = 0.2, T = 1.0;
    const double expected = sigma * std::sqrt(T) / 2.0;
    EXPECT_NEAR(bs_d1(100.0, 100.0, 0.0, sigma, T), expected, 1e-14);
}

TEST(BSGammaTest, D1IncreasesWithSpot) {
    // Higher S/K → more in-the-money → higher d1
    EXPECT_GT(bs_d1(110.0, 100.0, 0.05, 0.2, 0.25),
              bs_d1(100.0, 100.0, 0.05, 0.2, 0.25));
}

TEST(BSGammaTest, GammaDegenerateInputsReturnZero) {
    EXPECT_EQ(bs_gamma(0.0,   100.0, 0.0, 0.2,  0.25), 0.0); // S=0
    EXPECT_EQ(bs_gamma(100.0, 0.0,   0.0, 0.2,  0.25), 0.0); // K=0
    EXPECT_EQ(bs_gamma(100.0, 100.0, 0.0, 0.0,  0.25), 0.0); // sigma=0
    EXPECT_EQ(bs_gamma(100.0, 100.0, 0.0, 0.2,  0.0),  0.0); // T=0
}

TEST(BSGammaTest, GammaFormulaATM) {
    // Γ = phi(d1) / (S * sigma * sqrt(T))
    const double S = 100.0, K = 100.0, r = 0.05, sigma = 0.2, T = 30.0 / 365.0;
    const double sqrt_T = std::sqrt(T);
    const double d1     = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * sqrt_T);
    const double expected = phi(d1) / (S * sigma * sqrt_T);
    EXPECT_NEAR(bs_gamma(S, K, r, sigma, T), expected, 1e-12);
}

TEST(BSGammaTest, GammaIsPositive) {
    EXPECT_GT(bs_gamma(100.0, 100.0, 0.05, 0.2, 0.25), 0.0);
    EXPECT_GT(bs_gamma(100.0,  90.0, 0.05, 0.3, 0.5),  0.0);
    EXPECT_GT(bs_gamma(100.0, 110.0, 0.05, 0.25, 0.1), 0.0);
}

TEST(BSGammaTest, GammaMaximizedATM) {
    // Gamma is highest at the money and decreases for deep ITM/OTM
    const double S = 100.0, r = 0.0, sigma = 0.2, T = 1.0;
    const double gamma_atm  = bs_gamma(S, 100.0, r, sigma, T);
    const double gamma_deep_itm  = bs_gamma(S, 70.0, r, sigma, T);
    const double gamma_deep_otm  = bs_gamma(S, 130.0, r, sigma, T);
    EXPECT_GT(gamma_atm, gamma_deep_itm);
    EXPECT_GT(gamma_atm, gamma_deep_otm);
}

// ─── OFICalculator Tests ──────────────────────────────────────────────────────

using namespace alpha::signal::signals::ofi;

TEST(OFICalculatorTest, FirstTickIsInvalid) {
    OFICalculator calc;
    auto tick = make_tick(100001, 100.0, 500, 100.5, 300);
    auto res = calc.update(tick);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.ofi, 0);
    EXPECT_EQ(res.cumulative_ofi, 0);
    EXPECT_EQ(res.ofi_normalized, 0.0f);
}

TEST(OFICalculatorTest, BidPriceImproves) {
    // ΔQ_bid = +new_bid_sz when bid price rises
    OFICalculator calc;
    calc.update(make_tick(1, 100.0, 500, 100.5, 300));
    auto res = calc.update(make_tick(1, 100.1, 600, 100.5, 300));
    EXPECT_TRUE(res.valid);
    EXPECT_EQ(res.ofi, 600); // delta_bid=+600, delta_ask=0
}

TEST(OFICalculatorTest, BidPriceSameQtyIncreases) {
    // ΔQ_bid = new_qty - old_qty when bid price unchanged
    OFICalculator calc;
    calc.update(make_tick(2, 100.0, 400, 100.5, 200));
    auto res = calc.update(make_tick(2, 100.0, 600, 100.5, 200));
    EXPECT_EQ(res.ofi, 200); // delta_bid = 600-400=200, delta_ask=0
}

TEST(OFICalculatorTest, BidPriceSameQtyDecreases) {
    OFICalculator calc;
    calc.update(make_tick(3, 100.0, 600, 100.5, 300));
    auto res = calc.update(make_tick(3, 100.0, 200, 100.5, 300));
    EXPECT_EQ(res.ofi, -400); // delta_bid = 200-600=-400, delta_ask=0
}

TEST(OFICalculatorTest, BidPriceRetreats) {
    // ΔQ_bid = -prev_bid_size when bid price falls
    OFICalculator calc;
    calc.update(make_tick(4, 100.0, 500, 100.5, 300));
    auto res = calc.update(make_tick(4, 99.9, 200, 100.5, 300));
    EXPECT_EQ(res.ofi, -500); // delta_bid = -prev(500), delta_ask=0
}

TEST(OFICalculatorTest, AskPriceImproves) {
    // ΔQ_ask = +new_ask_sz when ask price falls → OFI = -delta_ask
    OFICalculator calc;
    calc.update(make_tick(5, 100.0, 300, 100.5, 400));
    auto res = calc.update(make_tick(5, 100.0, 300, 100.4, 500));
    EXPECT_EQ(res.ofi, -500); // delta_bid=0, delta_ask=+500 → OFI = 0-500
}

TEST(OFICalculatorTest, AskPriceSameQtyDecreases) {
    OFICalculator calc;
    calc.update(make_tick(6, 100.0, 300, 100.5, 400));
    auto res = calc.update(make_tick(6, 100.0, 300, 100.5, 200));
    EXPECT_EQ(res.ofi, 200); // delta_ask = 200-400=-200 → OFI = 0-(-200)=200
}

TEST(OFICalculatorTest, AskPriceRetreats) {
    // ΔQ_ask = -prev_ask_size when ask price rises → OFI = +prev_ask_size
    OFICalculator calc;
    calc.update(make_tick(7, 100.0, 300, 100.5, 400));
    auto res = calc.update(make_tick(7, 100.0, 300, 100.6, 200));
    EXPECT_EQ(res.ofi, 400); // delta_ask = -prev(400) → OFI = 0-(-400)=400
}

TEST(OFICalculatorTest, NormalizationBasicFormula) {
    OFICalculator calc;
    calc.update(make_tick(8, 100.0, 200, 100.5, 200));
    // Bid improves: delta_bid=300, delta_ask=0 → OFI=300
    // total_qty = 300 + 200 = 500, normalized = 300/500 = 0.6
    auto res = calc.update(make_tick(8, 100.1, 300, 100.5, 200));
    EXPECT_EQ(res.ofi, 300);
    EXPECT_NEAR(res.ofi_normalized, 0.6f, 1e-5f);
}

TEST(OFICalculatorTest, NormalizationClampedToMinus1) {
    // Bid retreats (-prev_bid_sz) with tiny new qty → |OFI| > total_qty
    OFICalculator calc;
    calc.update(make_tick(9, 100.0, 1000, 100.5, 100));
    // Bid falls: delta_bid = -1000, delta_ask=0 → OFI = -1000
    // total_qty = 50 + 100 = 150; -1000/150 < -1.0 → clamped to -1.0
    auto res = calc.update(make_tick(9, 99.9, 50, 100.5, 100));
    EXPECT_EQ(res.ofi_normalized, -1.0f);
}

TEST(OFICalculatorTest, NormalizationClampedToPlus1) {
    // Ask retreats → OFI = +prev_ask_sz with tiny bid qty
    OFICalculator calc;
    calc.update(make_tick(10, 100.0, 50, 100.5, 1000));
    // Ask rises: delta_ask = -1000 → OFI = +1000; total_qty = 50+100 = 150
    auto res = calc.update(make_tick(10, 100.0, 50, 100.6, 100));
    EXPECT_EQ(res.ofi_normalized, 1.0f);
}

TEST(OFICalculatorTest, CumulativeOFIAccumulates) {
    OFICalculator calc;
    const uint32_t token = 11;
    calc.update(make_tick(token, 100.0, 200, 100.5, 200));

    // Tick 2: bid improves → OFI = +300
    calc.update(make_tick(token, 100.1, 300, 100.5, 200));
    // Tick 3: ask retreats → OFI = +200 (prev ask was 200)
    auto res = calc.update(make_tick(token, 100.1, 300, 100.6, 150));
    EXPECT_EQ(res.cumulative_ofi, 300 + 200);  // 500
}

TEST(OFICalculatorTest, ResetCumulativeZeroesAccumulator) {
    OFICalculator calc;
    calc.update(make_tick(12, 100.0, 200, 100.5, 200));
    calc.update(make_tick(12, 100.1, 300, 100.5, 200)); // ofi=+300
    calc.reset_cumulative();
    auto res = calc.update(make_tick(12, 100.2, 400, 100.5, 200)); // ofi=+400
    EXPECT_EQ(res.cumulative_ofi, 400);  // only counts post-reset
}

TEST(OFICalculatorTest, ZeroTotalQtyNormalizationIsZero) {
    OFICalculator calc;
    calc.update(make_tick(13, 100.0, 100, 100.5, 100));
    // Zero bid and ask qty
    auto res = calc.update(make_tick(13, 100.0, 0, 100.5, 0));
    EXPECT_EQ(res.ofi_normalized, 0.0f);
}

// ─── TradeDirectionCalculator Tests ──────────────────────────────────────────

using namespace alpha::signal::signals::trade_direction;

TEST(TradeDirectionTest, FirstTickIsInvalid) {
    TradeDirectionCalculator calc;
    auto tick = make_tick(20001, 100.0, 500, 100.5, 300, 100.0);
    auto res = calc.update(tick);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.direction, 0);
    EXPECT_EQ(res.cumulative_direction, 0);
}

TEST(TradeDirectionTest, TradeAtAskIsBuy) {
    // last_price >= prev_ask_price → +1
    TradeDirectionCalculator calc;
    calc.update(make_tick(21, 100.0, 500, 100.5, 300, 100.25));
    // Tick 2: last_price = 100.5 = prev_ask → buy
    auto res = calc.update(make_tick(21, 100.1, 500, 100.6, 300, 100.5));
    EXPECT_TRUE(res.valid);
    EXPECT_EQ(res.direction, +1);
}

TEST(TradeDirectionTest, TradeAboveAskIsBuy) {
    TradeDirectionCalculator calc;
    calc.update(make_tick(22, 100.0, 500, 100.5, 300, 100.25));
    auto res = calc.update(make_tick(22, 100.1, 500, 100.6, 300, 100.7));
    EXPECT_EQ(res.direction, +1);
}

TEST(TradeDirectionTest, TradeAtBidIsSell) {
    // last_price <= prev_bid_price → -1
    TradeDirectionCalculator calc;
    calc.update(make_tick(23, 100.0, 500, 100.5, 300, 100.25));
    auto res = calc.update(make_tick(23, 99.9, 400, 100.4, 300, 100.0));
    EXPECT_EQ(res.direction, -1);
}

TEST(TradeDirectionTest, TradeBelowBidIsSell) {
    TradeDirectionCalculator calc;
    calc.update(make_tick(24, 100.0, 500, 100.5, 300, 100.25));
    auto res = calc.update(make_tick(24, 99.9, 400, 100.4, 300, 99.8));
    EXPECT_EQ(res.direction, -1);
}

TEST(TradeDirectionTest, TradeInsideSpreadIsIndeterminate) {
    // prev_bid < last_price < prev_ask → 0
    TradeDirectionCalculator calc;
    calc.update(make_tick(25, 100.0, 500, 100.5, 300, 100.25));
    auto res = calc.update(make_tick(25, 99.9, 400, 100.4, 300, 100.25));
    EXPECT_EQ(res.direction, 0);
}

TEST(TradeDirectionTest, CumulativeDirectionAccumulates) {
    TradeDirectionCalculator calc;
    const uint32_t token = 26;
    calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.25));
    calc.update(make_tick(token, 100.1, 500, 100.6, 300, 100.5));  // buy  → +1
    calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.0));  // sell → -1
    auto res = calc.update(make_tick(token, 100.1, 500, 100.6, 300, 100.6)); // buy → +1
    EXPECT_EQ(res.cumulative_direction, 1);  // +1 - 1 + 1 = 1
}

TEST(TradeDirectionTest, MultipleInstrumentsAreIndependent) {
    TradeDirectionCalculator calc;
    // Instrument A: seeded
    calc.update(make_tick(31, 100.0, 500, 100.5, 300, 100.25));
    // Instrument B: seeded
    calc.update(make_tick(32, 200.0, 100, 200.5, 100, 200.25));

    // A: trade at ask → +1
    auto resA = calc.update(make_tick(31, 100.1, 500, 100.6, 300, 100.5));
    // B: trade at bid → -1
    auto resB = calc.update(make_tick(32, 199.9, 100, 200.4, 100, 200.0));

    EXPECT_EQ(resA.direction, +1);
    EXPECT_EQ(resB.direction, -1);
    EXPECT_EQ(resA.cumulative_direction, 1);
    EXPECT_EQ(resB.cumulative_direction, -1);
}

// ─── VPINCalculator Tests ─────────────────────────────────────────────────────

using namespace alpha::signal::signals::vpin;

TEST(VPINCalculatorTest, NotValidBeforeFirstBucketCloses) {
    VPINCalculator calc;
    const uint32_t token = 40001;
    calc.set_adv(token, 500); // bucket_size = 500/50 = 10

    // Send 9 units of buy volume — bucket not yet full
    auto tick = make_tick(token, 100.0, 500, 100.5, 300, 100.25, 9);
    auto res = calc.update(tick, +1);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.vpin, 0.0f);
    EXPECT_EQ(res.buckets_completed, 0u);
}

TEST(VPINCalculatorTest, ValidAfterFirstBucketFills) {
    VPINCalculator calc;
    const uint32_t token = 40002;
    calc.set_adv(token, 500); // bucket_size = 10

    // Fill one bucket with all buy volume (10 units total)
    auto tick = make_tick(token, 100.0, 500, 100.5, 300, 100.25, 10);
    auto res = calc.update(tick, +1);
    EXPECT_TRUE(res.valid);
    EXPECT_EQ(res.buckets_completed, 1u);
    EXPECT_TRUE(res.bucket_closed);
}

TEST(VPINCalculatorTest, AllBuyVolumeVPINIsOne) {
    // All volume buyer-initiated → imbalance = bucket_size → VPIN = 1.0
    VPINCalculator calc;
    const uint32_t token = 40003;
    calc.set_adv(token, 500);
    auto tick = make_tick(token, 100.0, 500, 100.5, 300, 100.25, 10);
    auto res = calc.update(tick, +1); // 10 buy, 0 sell → imbalance = 10
    // VPIN = |10 - 0| / (1 * 10) = 1.0
    EXPECT_NEAR(res.vpin, 1.0f, 1e-5f);
}

TEST(VPINCalculatorTest, AllSellVolumeVPINIsOne) {
    VPINCalculator calc;
    const uint32_t token = 40004;
    calc.set_adv(token, 500);
    auto tick = make_tick(token, 100.0, 500, 100.5, 300, 100.25, 10);
    auto res = calc.update(tick, -1);
    EXPECT_NEAR(res.vpin, 1.0f, 1e-5f);
}

TEST(VPINCalculatorTest, BalancedBucketVPINIsZero) {
    // 5 buy + 5 sell fills bucket, then 1 extra tick triggers close
    // imbalance = |5-5| = 0 → VPIN = 0/(1*10) = 0
    VPINCalculator calc;
    const uint32_t token = 40005;
    calc.set_adv(token, 500); // bucket_size = 10

    calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.25, 5), +1); // buy 5
    calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.25, 5), -1); // sell 5 → bucket full
    // Send 1 more unit to overflow and close the full bucket
    auto res = calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.25, 1), +1);

    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.vpin, 0.0f, 1e-5f); // |5-5| / (1*10) = 0
}

TEST(VPINCalculatorTest, DefaultBucketSizeWithoutSetAdv) {
    // Without set_adv(), bucket_size = VPIN_DEFAULT_ADV / VPIN_N_BUCKETS = 200000
    VPINCalculator calc;
    const uint32_t token = 40006;
    // Sending less than 200000 → no bucket close
    auto tick = make_tick(token, 100.0, 500, 100.5, 300, 100.25, 100000);
    auto res = calc.update(tick, +1);
    EXPECT_FALSE(res.valid);
}

TEST(VPINCalculatorTest, BucketCountIncrements) {
    VPINCalculator calc;
    const uint32_t token = 40007;
    calc.set_adv(token, 500);

    calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.25, 10), +1); // 1 bucket
    calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.25, 10), +1); // 2 buckets
    auto res = calc.update(make_tick(token, 100.0, 500, 100.5, 300, 100.25, 10), +1); // 3
    EXPECT_EQ(res.buckets_completed, 3u);
}

TEST(VPINCalculatorTest, ZeroVolumeTickIsNoOp) {
    VPINCalculator calc;
    const uint32_t token = 40008;
    calc.set_adv(token, 500);
    auto tick = make_tick(token, 100.0, 500, 100.5, 300, 100.25, 0);
    auto res = calc.update(tick, +1);
    EXPECT_FALSE(res.valid);
    EXPECT_FALSE(res.bucket_closed);
}

// ─── KylesLambdaCalculator Tests ─────────────────────────────────────────────

using namespace alpha::signal::signals::kyles_lambda;

TEST(KylesLambdaTest, FirstTickIsInvalid) {
    KylesLambdaCalculator calc;
    auto tick = make_tick(50001, 100.0, 500, 100.5, 300, 100.0, 100);
    auto res = calc.update(tick, 0);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.ticks_in_window, 0u);
}

TEST(KylesLambdaTest, NotValidUntilWindowFull) {
    KylesLambdaCalculator calc;
    const uint32_t token = 50002;
    auto tick = make_tick(token, 100.0, 500, 100.5, 300, 100.0, 100);
    calc.update(tick, 100);  // tick 1: seeds prev_price

    // Ticks 2–20: 19 data points, still not full (need 20)
    for (int i = 1; i < 20; ++i) {
        auto t = make_tick(token, 100.0, 500, 100.5, 300, 100.0 + i * 0.01, 100);
        auto res = calc.update(t, 100 + i);
        EXPECT_FALSE(res.valid) << "Should be invalid at tick " << (i + 1);
    }

    // Tick 21: window is now full (20 data points from ticks 2–21)
    auto t21 = make_tick(token, 100.0, 500, 100.5, 300, 100.21, 100);
    auto res = calc.update(t21, 200);
    EXPECT_TRUE(res.valid);
    EXPECT_EQ(res.ticks_in_window, KYLE_WINDOW);
}

TEST(KylesLambdaTest, LambdaFormula) {
    // Feed constant ΔP = 0.1 and constant ΔOFI = 10 for a full window
    // λ = Σ(ΔP * ΔOFI) / Σ(ΔOFI²) = 20*(0.1*10) / 20*(100) = 2/2000 = 0.001
    KylesLambdaCalculator calc;
    const uint32_t token = 50003;
    double price = 100.0;
    int32_t ofi_val = 10;
    auto tick0 = make_tick(token, 100.0, 500, 100.5, 300, price, 100);
    calc.update(tick0, ofi_val);  // seed

    for (uint32_t i = 0; i < KYLE_WINDOW; ++i) {
        price += 0.1;
        auto t = make_tick(token, 100.0, 500, 100.5, 300, price, 100);
        calc.update(t, ofi_val);
    }

    // Now valid: λ = (KYLE_WINDOW * ΔP * ΔOFI) / (KYLE_WINDOW * ΔOFI²)
    //              = (20 * 0.1 * 10) / (20 * 100) = 20 / 2000 = 0.01
    auto t_final = make_tick(token, 100.0, 500, 100.5, 300, price + 0.1, 100);
    auto res = calc.update(t_final, ofi_val);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.lambda, 0.01, 1e-10);
}

TEST(KylesLambdaTest, ZeroOFIVariationLambdaIsZero) {
    // ΔOFI = 0 → S_dofi2 = 0 → lambda = 0
    KylesLambdaCalculator calc;
    const uint32_t token = 50004;
    double price = 100.0;
    auto tick0 = make_tick(token, 100.0, 500, 100.5, 300, price, 100);
    calc.update(tick0, 0);

    for (uint32_t i = 0; i < KYLE_WINDOW + 1; ++i) {
        price += 0.1;
        auto t = make_tick(token, 100.0, 500, 100.5, 300, price, 100);
        calc.update(t, 0);
    }
    auto final_tick = make_tick(token, 100.0, 500, 100.5, 300, price + 0.1, 100);
    auto res = calc.update(final_tick, 0);
    EXPECT_EQ(res.lambda, 0.0);
}

TEST(KylesLambdaTest, SumsAreCorrect) {
    // Verify sum_dp_dofi and sum_dofi_sq
    KylesLambdaCalculator calc;
    const uint32_t token = 50005;
    // Seed
    auto tick0 = make_tick(token, 100.0, 500, 100.5, 300, 100.0, 100);
    calc.update(tick0, 5);

    // One data point: ΔP = 1.0, ΔOFI = 5
    auto tick1 = make_tick(token, 100.0, 500, 100.5, 300, 101.0, 100);
    auto res = calc.update(tick1, 5);
    EXPECT_NEAR(res.sum_dp_dofi, 1.0 * 5.0, 1e-12);   // 5.0
    EXPECT_NEAR(res.sum_dofi_sq, 5.0 * 5.0, 1e-12);   // 25.0
    EXPECT_EQ(res.ticks_in_window, 1u);
}

// ─── EntropyCalculator Tests ──────────────────────────────────────────────────

using namespace alpha::signal::signals::entropy;

TEST(EntropyTest, EmptyBookIsInvalid) {
    EntropyCalculator calc;
    alpha::models::Tick tick{};
    tick.instrument_token = 60001;
    // All levels zero
    auto res = calc.compute(tick);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.H, 0.0f);
    EXPECT_EQ(res.H_normalized, 0.0f);
}

TEST(EntropyTest, SingleLevelHasZeroEntropy) {
    // All weight in one level → H = 0
    EntropyCalculator calc;
    alpha::models::Tick tick{};
    tick.instrument_token = 60002;
    tick.bids[0].quantity = 1000;
    // All other levels = 0
    auto res = calc.compute(tick);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.H, 0.0f, 1e-5f);
    EXPECT_NEAR(res.H_normalized, 0.0f, 1e-5f);
}

TEST(EntropyTest, UniformDistributionMaximizesEntropy) {
    // Equal qty at each of 5 levels (bid side only): H = ln(5)
    EntropyCalculator calc;
    alpha::models::Tick tick{};
    tick.instrument_token = 60003;
    for (int i = 0; i < 5; ++i) tick.bids[i].quantity = 100;
    auto res = calc.compute(tick);
    EXPECT_TRUE(res.valid);
    const float expected_H = std::log(5.0f);
    EXPECT_NEAR(res.H, expected_H, 1e-4f);
}

TEST(EntropyTest, HNormalizedIsDividedByLn10) {
    EntropyCalculator calc;
    alpha::models::Tick tick{};
    tick.instrument_token = 60004;
    for (int i = 0; i < 5; ++i) tick.bids[i].quantity = 100;
    auto res = calc.compute(tick);
    EXPECT_NEAR(res.H_normalized, res.H / LN_10, 1e-5f);
}

TEST(EntropyTest, HNormalizedInValidRange) {
    EntropyCalculator calc;
    alpha::models::Tick tick{};
    tick.instrument_token = 60005;
    tick.bids[0].quantity = 500;
    tick.bids[1].quantity = 300;
    tick.bids[2].quantity = 100;
    tick.asks[0].quantity = 200;
    tick.asks[1].quantity = 100;
    auto res = calc.compute(tick);
    EXPECT_TRUE(res.valid);
    EXPECT_GE(res.H_normalized, 0.0f);
    EXPECT_LE(res.H_normalized, 1.0f); // ln(5)/ln(10) < 1
}

TEST(EntropyTest, CombinesBidAndAskQuantities) {
    // C_k = bid_k + ask_k
    EntropyCalculator calc;
    alpha::models::Tick tick{};
    tick.instrument_token = 60006;
    // Level 0: bid=50, ask=50 → C_0=100
    // Levels 1–4: all zero
    tick.bids[0].quantity = 50;
    tick.asks[0].quantity = 50;
    auto res = calc.compute(tick);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.H, 0.0f, 1e-5f); // single effective level
}

TEST(EntropyTest, TwoEqualLevelsEntropy) {
    // Two levels of equal weight: H = ln(2)
    EntropyCalculator calc;
    alpha::models::Tick tick{};
    tick.instrument_token = 60007;
    tick.bids[0].quantity = 100;
    tick.bids[1].quantity = 100;
    auto res = calc.compute(tick);
    EXPECT_NEAR(res.H, std::log(2.0f), 1e-5f);
}

// ─── LogReturnCalculator Tests ────────────────────────────────────────────────

using namespace alpha::signal::signals::bar;

TEST(LogReturnTest, FirstBarIsInvalid) {
    LogReturnCalculator calc;
    auto bar = make_bar(70001, 100.0);
    auto res = calc.update(bar);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.r, 0.0);
}

TEST(LogReturnTest, SecondBarComputesLogReturn) {
    LogReturnCalculator calc;
    calc.update(make_bar(71, 100.0));
    auto res = calc.update(make_bar(71, 110.0));
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.r, std::log(110.0 / 100.0), 1e-12);
}

TEST(LogReturnTest, UpBarPositiveReturn) {
    LogReturnCalculator calc;
    calc.update(make_bar(72, 100.0));
    auto res = calc.update(make_bar(72, 105.0));
    EXPECT_GT(res.r, 0.0);
}

TEST(LogReturnTest, DownBarNegativeReturn) {
    LogReturnCalculator calc;
    calc.update(make_bar(73, 100.0));
    auto res = calc.update(make_bar(73, 95.0));
    EXPECT_LT(res.r, 0.0);
}

TEST(LogReturnTest, UnchangedPriceZeroReturn) {
    LogReturnCalculator calc;
    calc.update(make_bar(74, 100.0));
    auto res = calc.update(make_bar(74, 100.0));
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.r, 0.0, 1e-15);
}

TEST(LogReturnTest, ZeroPriceIsInvalid) {
    LogReturnCalculator calc;
    calc.update(make_bar(75, 100.0));
    auto res = calc.update(make_bar(75, 0.0));
    EXPECT_FALSE(res.valid);
}

TEST(LogReturnTest, NegativePriceIsInvalid) {
    LogReturnCalculator calc;
    calc.update(make_bar(76, 100.0));
    auto res = calc.update(make_bar(76, -1.0));
    EXPECT_FALSE(res.valid);
}

TEST(LogReturnTest, ReturnsAreAdditive) {
    // ln(C2/C0) = ln(C1/C0) + ln(C2/C1)
    LogReturnCalculator calc;
    calc.update(make_bar(77, 100.0));
    auto r1 = calc.update(make_bar(77, 110.0));
    auto r2 = calc.update(make_bar(77, 121.0));
    EXPECT_NEAR(r1.r + r2.r, std::log(121.0 / 100.0), 1e-12);
}

TEST(LogReturnTest, ResetClearsState) {
    LogReturnCalculator calc;
    calc.update(make_bar(78, 100.0));
    calc.reset();
    // After reset, next bar is treated as first bar again
    auto res = calc.update(make_bar(78, 110.0));
    EXPECT_FALSE(res.valid);
}

TEST(LogReturnTest, MultipleInstrumentsAreIndependent) {
    LogReturnCalculator calc;
    calc.update(make_bar(79, 100.0));
    calc.update(make_bar(80, 200.0));
    auto resA = calc.update(make_bar(79, 110.0));
    auto resB = calc.update(make_bar(80, 180.0));
    EXPECT_NEAR(resA.r, std::log(110.0 / 100.0), 1e-12);
    EXPECT_NEAR(resB.r, std::log(180.0 / 200.0), 1e-12);
}

// ─── RealizedVolatilityCalculator Tests ───────────────────────────────────────

TEST(RealizedVolatilityTest, SingleReturnIsInvalid) {
    RealizedVolatilityCalculator calc;
    auto res = calc.update(100001, 0.01);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.variance, 0.0);
}

TEST(RealizedVolatilityTest, TwoReturnsBecomeValid) {
    RealizedVolatilityCalculator calc;
    calc.update(100002, 0.01);
    auto res = calc.update(100002, 0.02);
    EXPECT_TRUE(res.valid);
    EXPECT_EQ(res.n, 2u);
}

TEST(RealizedVolatilityTest, TwoEqualReturnsZeroVariance) {
    RealizedVolatilityCalculator calc;
    calc.update(100003, 0.01);
    auto res = calc.update(100003, 0.01);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.variance, 0.0, 1e-15);
    EXPECT_NEAR(res.volatility, 0.0, 1e-15);
}

TEST(RealizedVolatilityTest, SampleVarianceFormula) {
    // Two returns: 0.01 and 0.03; mean=0.02; var = ((0.01-0.02)^2 + (0.03-0.02)^2) / (2-1)
    // = (0.0001 + 0.0001) / 1 = 0.0002
    RealizedVolatilityCalculator calc;
    calc.update(100004, 0.01);
    auto res = calc.update(100004, 0.03);
    EXPECT_NEAR(res.variance, 0.0002, 1e-12);
    EXPECT_NEAR(res.volatility, std::sqrt(0.0002), 1e-12);
}

TEST(RealizedVolatilityTest, AnnualizationFactor) {
    RealizedVolatilityCalculator calc;
    calc.update(100005, 0.01);
    auto res = calc.update(100005, 0.03);
    EXPECT_NEAR(res.sigma_ann, res.volatility * RV_ANNUALIZATION, 1e-12);
}

TEST(RealizedVolatilityTest, WindowSizeIsRVWindow) {
    RealizedVolatilityCalculator calc;
    for (uint32_t i = 0; i < RV_WINDOW + 5; ++i) {
        auto res = calc.update(100006, 0.01 * (i % 3 == 0 ? 1 : -1));
        if (i >= RV_WINDOW - 1) {
            EXPECT_EQ(res.n, RV_WINDOW) << "Window should cap at " << RV_WINDOW;
        }
    }
}

TEST(RealizedVolatilityTest, RollingEviction) {
    // Feed constant returns of 0.01 to fill the window, then a different value
    // and verify variance changes when old values are evicted
    RealizedVolatilityCalculator calc;
    const uint32_t token = 100007;
    for (uint32_t i = 0; i < RV_WINDOW; ++i) calc.update(token, 0.01);

    // Window is full of 0.01 → variance = 0
    auto res1 = calc.update(token, 0.01);
    EXPECT_NEAR(res1.variance, 0.0, 1e-12);

    // Now feed a very different value — one old 0.01 is evicted
    auto res2 = calc.update(token, 0.05);
    EXPECT_GT(res2.variance, 0.0); // variance should now be non-zero
}

TEST(RealizedVolatilityTest, MultipleInstrumentsAreIndependent) {
    RealizedVolatilityCalculator calc;
    calc.update(100008, 0.01);
    calc.update(100009, 0.05);
    auto resA = calc.update(100008, 0.03);
    auto resB = calc.update(100009, 0.07);
    // A: returns 0.01, 0.03 → var = 0.0002
    EXPECT_NEAR(resA.variance, 0.0002, 1e-12);
    // B: returns 0.05, 0.07 → var = 0.0002
    EXPECT_NEAR(resB.variance, 0.0002, 1e-12);
}

// ─── KalmanFilterCalculator Tests ─────────────────────────────────────────────

TEST(KalmanFilterTest, FirstBarIsInvalid) {
    KalmanFilterCalculator calc;
    auto res = calc.update(110001, 0.01);
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.K, 0.0);
    EXPECT_EQ(res.innovation, 0.0);
}

TEST(KalmanFilterTest, FirstBarSeedsAlphaHat) {
    KalmanFilterCalculator calc;
    auto res = calc.update(110002, 0.015);
    EXPECT_NEAR(res.alpha_hat, 0.015, 1e-15);
    EXPECT_NEAR(res.alpha_predict, 0.015, 1e-15);
}

TEST(KalmanFilterTest, SecondBarPredictAndUpdate) {
    KalmanFilterCalculator calc;
    const double r1 = 0.01, r2 = 0.02;
    const double Q = KF_DEFAULT_Q, R = KF_DEFAULT_R;
    calc.update(110003, r1);

    // Predict: alpha_predict = r1, P_predict = P_init + Q
    const double P_predict = KF_DEFAULT_P_INIT + Q;
    const double innovation = r2 - r1;
    const double K = P_predict / (P_predict + R);
    const double alpha_hat_expected = r1 + K * innovation;
    const double P_expected = (1.0 - K) * P_predict;

    auto res = calc.update(110003, r2);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.alpha_predict, r1, 1e-15);
    EXPECT_NEAR(res.P_predict, P_predict, 1e-15);
    EXPECT_NEAR(res.innovation, innovation, 1e-15);
    EXPECT_NEAR(res.K, K, 1e-12);
    EXPECT_NEAR(res.alpha_hat, alpha_hat_expected, 1e-12);
    EXPECT_NEAR(res.P, P_expected, 1e-12);
}

TEST(KalmanFilterTest, KalmanGainInUnitInterval) {
    KalmanFilterCalculator calc;
    calc.update(110004, 0.01);
    auto res = calc.update(110004, 0.02);
    EXPECT_GT(res.K, 0.0);
    EXPECT_LT(res.K, 1.0);
}

TEST(KalmanFilterTest, CovarianceConverges) {
    // With many bars, P should converge to a small steady-state value
    KalmanFilterCalculator calc;
    const uint32_t token = 110005;
    double prev_P = KF_DEFAULT_P_INIT;
    for (int i = 0; i < 100; ++i) {
        auto res = calc.update(token, 0.01 * (i % 2 == 0 ? 1 : -1));
        if (res.valid) {
            EXPECT_LT(res.P, prev_P + 1e-4); // P should not grow unboundedly
            prev_P = res.P;
        }
    }
    // Final P should be much smaller than initial P_init = 1.0
    EXPECT_LT(prev_P, 0.01);
}

TEST(KalmanFilterTest, CustomNoiseParameters) {
    KalmanFilterCalculator calc;
    const uint32_t token = 110006;
    const double Q = 1e-3, R = 1e-2;
    calc.set_noise(token, Q, R);
    calc.update(token, 0.01);

    const double P_predict = KF_DEFAULT_P_INIT + Q;
    const double K_expected = P_predict / (P_predict + R);
    auto res = calc.update(token, 0.02);
    EXPECT_NEAR(res.K, K_expected, 1e-12);
}

TEST(KalmanFilterTest, ConstantReturnAlphaConverges) {
    // Feed a constant return — alpha_hat should converge to that value
    KalmanFilterCalculator calc;
    const uint32_t token = 110007;
    const double r = 0.005;
    for (int i = 0; i < 50; ++i) calc.update(token, r);
    auto res = calc.update(token, r);
    EXPECT_NEAR(res.alpha_hat, r, 1e-6);
}

// ─── CUSUMCalculator Tests ────────────────────────────────────────────────────

TEST(CUSUMTest, ZeroVarianceIsInvalid) {
    CUSUMCalculator calc;
    auto res = calc.update(120001, 0.01, 0.005, 0.0); // variance=0
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.threshold, std::numeric_limits<double>::infinity());
}

TEST(CUSUMTest, ZeroAlphaHatIsInvalid) {
    CUSUMCalculator calc;
    auto res = calc.update(120002, 0.01, 0.0, 0.0001); // alpha_hat=0
    EXPECT_FALSE(res.valid);
}

TEST(CUSUMTest, CPlusIncreasesWithPositiveScore) {
    // r > 0, alpha_hat > 0 → score > drift_penalty → C+ grows
    CUSUMCalculator calc;
    const uint32_t token = 120003;
    // r=0.01, alpha=0.005, var=0.0001
    // score = 0.005 * 0.01 / 0.0001 = 0.5
    // drift_penalty = 0.005^2 / (2 * 0.0001) = 0.125
    // C+ increment = 0.5 - 0.125 = 0.375
    auto res = calc.update(token, 0.01, 0.005, 0.0001);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.C_plus, 0.375, 1e-10);
    EXPECT_NEAR(res.C_minus, 0.0, 1e-10); // clamped to 0
}

TEST(CUSUMTest, CMinusIncreasesWithNegativeScore) {
    // Negative alpha or negative r → C- grows
    // r=-0.01, alpha=0.005, var=0.0001
    // score = 0.005 * (-0.01) / 0.0001 = -0.5
    // C- increment = 0 - (-0.5) - 0.125 = 0.375
    CUSUMCalculator calc;
    auto res = calc.update(120004, -0.01, 0.005, 0.0001);
    EXPECT_NEAR(res.C_minus, 0.375, 1e-10);
    EXPECT_NEAR(res.C_plus, 0.0, 1e-10);
}

TEST(CUSUMTest, ThresholdFormula) {
    // h* = ln(|alpha| / c) - lambda * variance / alpha^2
    // Default c=1e-4, lambda=0.5
    // alpha=0.005, var=0.0001
    // h* = ln(0.005/1e-4) - 0.5 * 0.0001 / 0.000025 = ln(50) - 2.0
    CUSUMCalculator calc;
    auto res = calc.update(120005, 0.01, 0.005, 0.0001);
    const double expected_threshold = std::log(0.005 / 1e-4) - 0.5 * 0.0001 / (0.005 * 0.005);
    EXPECT_NEAR(res.threshold, expected_threshold, 1e-10);
}

TEST(CUSUMTest, AlarmFiresAndResetsC) {
    // Feed the same positive signal until C+ exceeds threshold
    CUSUMCalculator calc;
    const uint32_t token = 120006;
    // Each step: C+ += 0.375, threshold ≈ 1.912
    // After 6 steps: C+ ≈ 2.25 > 1.912 → alarm fires
    bool alarm_fired = false;
    for (int i = 0; i < 10; ++i) {
        auto res = calc.update(token, 0.01, 0.005, 0.0001);
        if (res.fired_up) {
            alarm_fired = true;
            // After alarm, C+ resets to 0
            EXPECT_EQ(res.C_plus, 0.0);
            break;
        }
    }
    EXPECT_TRUE(alarm_fired);
}

TEST(CUSUMTest, PageHinkleyRestartAfterAlarm) {
    CUSUMCalculator calc;
    const uint32_t token = 120007;
    // Drive to alarm
    for (int i = 0; i < 10; ++i) {
        auto res = calc.update(token, 0.01, 0.005, 0.0001);
        if (res.fired) break;
    }
    // After alarm (and reset), C+ should restart from 0
    auto res = calc.update(token, 0.01, 0.005, 0.0001);
    EXPECT_LE(res.C_plus, 0.4); // fresh start → only one increment
}

TEST(CUSUMTest, CustomParams) {
    CUSUMCalculator calc;
    const uint32_t token = 120008;
    calc.set_params(token, 1e-3, 1.0); // larger c, different lambda
    auto res = calc.update(token, 0.01, 0.005, 0.0001);
    EXPECT_TRUE(res.valid);
    // Threshold: ln(0.005/1e-3) - 1.0 * 0.0001 / 0.000025 = ln(5) - 4.0
    const double expected = std::log(0.005 / 1e-3) - 1.0 * 0.0001 / (0.005 * 0.005);
    EXPECT_NEAR(res.threshold, expected, 1e-10);
}

// ─── GEXCalculator Tests ──────────────────────────────────────────────────────

TEST(GEXTest, EmptyChainIsInvalid) {
    GEXCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 130001;
    chain.spot = 100.0; chain.T = 0.1; chain.r = 0.05; chain.lot_size = 50;
    auto res = calc.compute(chain);
    EXPECT_FALSE(res.valid);
}

TEST(GEXTest, ZeroSpotIsInvalid) {
    GEXCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 130002;
    chain.spot = 0.0; chain.T = 0.1; chain.r = 0.05; chain.lot_size = 50;
    chain.strikes.push_back({100.0, 0.2, 0.2, 1000, 1000});
    auto res = calc.compute(chain);
    EXPECT_FALSE(res.valid);
}

TEST(GEXTest, ZeroTIsInvalid) {
    GEXCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 130003;
    chain.spot = 100.0; chain.T = 0.0; chain.r = 0.05; chain.lot_size = 50;
    chain.strikes.push_back({100.0, 0.2, 0.2, 1000, 1000});
    auto res = calc.compute(chain);
    EXPECT_FALSE(res.valid);
}

TEST(GEXTest, SingleStrikeCallsOnlyPositiveGEX) {
    // GEX = (Σ OI_call * gamma_call - Σ OI_put * gamma_put) * L
    // With only call OI → GEX > 0
    GEXCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 130004;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.05; chain.lot_size = 50;
    chain.strikes.push_back({100.0, 0.2, 0.2, 1000, 0}); // only call OI
    auto res = calc.compute(chain);
    EXPECT_TRUE(res.valid);
    EXPECT_GT(res.gex, 0.0);
    EXPECT_EQ(res.gex_put, 0.0);
    EXPECT_GT(res.gex_call, 0.0);
}

TEST(GEXTest, SingleStrikePutsOnlyNegativeGEX) {
    GEXCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 130005;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.05; chain.lot_size = 50;
    chain.strikes.push_back({100.0, 0.2, 0.2, 0, 1000}); // only put OI
    auto res = calc.compute(chain);
    EXPECT_TRUE(res.valid);
    EXPECT_LT(res.gex, 0.0);
    EXPECT_EQ(res.gex_call, 0.0);
    EXPECT_GT(res.gex_put, 0.0);
}

TEST(GEXTest, GEXFormula) {
    // GEX = (OI_call * gamma_call - OI_put * gamma_put) * L
    GEXCalculator calc;
    const double S = 100.0, K = 100.0, r = 0.0, sigma = 0.2, T = 1.0;
    const uint64_t oi_call = 500, oi_put = 300;
    const uint32_t lot = 75;

    OptionsChain chain;
    chain.instrument_token = 130006;
    chain.spot = S; chain.T = T; chain.r = r; chain.lot_size = lot;
    chain.strikes.push_back({K, sigma, sigma, oi_call, oi_put});

    auto res = calc.compute(chain);
    const double gamma = bs_gamma(S, K, r, sigma, T);
    const double expected_gex = (oi_call * gamma - oi_put * gamma) * lot;
    EXPECT_NEAR(res.gex, expected_gex, 1e-10);
}

TEST(GEXTest, MeanReversionRegimeWhenAboveTheta) {
    GEXCalculator calc;
    const uint32_t token = 130007;
    calc.set_thresholds(token, 100.0, -100.0); // low threshold

    OptionsChain chain;
    chain.instrument_token = token;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 1000;
    chain.strikes.push_back({100.0, 0.2, 0.2, 10000, 0}); // large call OI → high GEX

    auto res = calc.compute(chain);
    EXPECT_EQ(res.regime, GEXRegime::MeanReversion);
    EXPECT_GT(res.gex, res.theta_pos);
}

TEST(GEXTest, MomentumRegimeWhenBelowTheta) {
    GEXCalculator calc;
    const uint32_t token = 130008;
    calc.set_thresholds(token, 100.0, -100.0);

    OptionsChain chain;
    chain.instrument_token = token;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 1000;
    chain.strikes.push_back({100.0, 0.2, 0.2, 0, 10000}); // large put OI → negative GEX

    auto res = calc.compute(chain);
    EXPECT_EQ(res.regime, GEXRegime::Momentum);
    EXPECT_LT(res.gex, res.theta_neg);
}

TEST(GEXTest, NeutralRegimeBetweenThresholds) {
    GEXCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 130009;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 1;
    chain.strikes.push_back({100.0, 0.2, 0.2, 1, 1}); // tiny OI → tiny GEX

    // Default thresholds = ±1e6; tiny GEX → neutral
    auto res = calc.compute(chain);
    EXPECT_EQ(res.regime, GEXRegime::Neutral);
}

// ─── VRPCalculator Tests ──────────────────────────────────────────────────────

TEST(VRPTest, EmptyChainIsInvalid) {
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140001;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.05; chain.lot_size = 50;
    auto res = calc.compute(chain, 0.04);
    EXPECT_FALSE(res.valid);
}

TEST(VRPTest, ZeroSigmaAnnSqIsInvalid) {
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140002;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.05; chain.lot_size = 50;
    chain.strikes.push_back({100.0, 0.2, 0.2, 1000, 1000});
    auto res = calc.compute(chain, 0.0);
    EXPECT_FALSE(res.valid);
}

TEST(VRPTest, ATMStrikeExactMatch) {
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140003;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.05; chain.lot_size = 50;
    chain.strikes.push_back({90.0,  0.25, 0.25, 1000, 1000});
    chain.strikes.push_back({100.0, 0.20, 0.20, 1000, 1000}); // ATM
    chain.strikes.push_back({110.0, 0.22, 0.22, 1000, 1000});
    auto res = calc.compute(chain, 0.04);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.K_atm, 100.0, 1e-12);
    EXPECT_NEAR(res.iv_atm, 0.20, 1e-12);
}

TEST(VRPTest, ATMStrikeNearestWins) {
    // spot=102 → closest strike is 100 (dist=2) vs 105 (dist=3)
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140004;
    chain.spot = 102.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 50;
    chain.strikes.push_back({100.0, 0.20, 0.20, 1000, 1000});
    chain.strikes.push_back({105.0, 0.22, 0.22, 1000, 1000});
    auto res = calc.compute(chain, 0.04);
    EXPECT_NEAR(res.K_atm, 100.0, 1e-12);
}

TEST(VRPTest, VRPFormula) {
    // VRP = iv_atm^2 - sigma_ann_sq
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140005;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 50;
    const double iv = 0.20;
    const double sigma_ann_sq = 0.03;
    chain.strikes.push_back({100.0, iv, iv, 1000, 1000});
    auto res = calc.compute(chain, sigma_ann_sq);
    EXPECT_TRUE(res.valid);
    EXPECT_NEAR(res.iv_atm, iv, 1e-12);
    EXPECT_NEAR(res.iv_atm_sq, iv * iv, 1e-12);
    EXPECT_NEAR(res.vrp, iv * iv - sigma_ann_sq, 1e-12);
}

TEST(VRPTest, VRPNormalizedFormula) {
    // VRP_normalized = VRP / sigma_ann_sq
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140006;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 50;
    const double iv = 0.25, sigma_ann_sq = 0.04;
    chain.strikes.push_back({100.0, iv, iv, 1000, 1000});
    auto res = calc.compute(chain, sigma_ann_sq);
    EXPECT_NEAR(res.vrp_normalized, res.vrp / sigma_ann_sq, 1e-12);
}

TEST(VRPTest, ATMIVMidOfCallAndPut) {
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140007;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 50;
    const double iv_call = 0.20, iv_put = 0.22;
    chain.strikes.push_back({100.0, iv_call, iv_put, 1000, 1000});
    auto res = calc.compute(chain, 0.04);
    EXPECT_NEAR(res.iv_atm, 0.5 * (iv_call + iv_put), 1e-12);
}

TEST(VRPTest, PositiveVRPOptionsAreRich) {
    VRPCalculator calc;
    OptionsChain chain;
    chain.instrument_token = 140008;
    chain.spot = 100.0; chain.T = 0.25; chain.r = 0.0; chain.lot_size = 50;
    chain.strikes.push_back({100.0, 0.25, 0.25, 1000, 1000}); // iv=0.25, iv_sq=0.0625
    auto res = calc.compute(chain, 0.04);  // sigma_ann_sq=0.04
    EXPECT_GT(res.vrp, 0.0); // 0.0625 - 0.04 > 0
}

// ─── SignalNormalizer Tests ───────────────────────────────────────────────────

using namespace alpha::signal::signals::derived;

TEST(SignalNormalizerTest, FirstObservationPassthrough) {
    SignalNormalizer norm;
    SignalBundle bundle;
    bundle.instrument_token = 150001;
    bundle.timestamp_ns = 0;
    bundle.values.fill(2.0);

    auto res = norm.update(bundle);
    EXPECT_EQ(res.n, 1u);
    for (uint32_t k = 0; k < NUM_SIGNALS; ++k) {
        EXPECT_FALSE(res.valid[k]);
        EXPECT_NEAR(res.z[k], 2.0, 1e-15); // passthrough
    }
}

TEST(SignalNormalizerTest, TwoEqualValuesZeroZ) {
    SignalNormalizer norm;
    SignalBundle b;
    b.instrument_token = 150002;
    b.timestamp_ns = 0;
    b.values.fill(3.0);

    norm.update(b);
    auto res = norm.update(b); // second observation, same value
    for (uint32_t k = 0; k < NUM_SIGNALS; ++k) {
        EXPECT_TRUE(res.valid[k]);
        // Both values = 3.0 → mu=3.0, sigma=0 → z=0
        EXPECT_NEAR(res.z[k], 0.0, 1e-15);
    }
}

TEST(SignalNormalizerTest, ZScoreFormula) {
    // With two values x1=0 and x2=2: mu=1, var=((0-1)^2+(2-1)^2)/(2-1)=2, sigma=sqrt(2)
    // z for x2=2: (2-1)/sqrt(2) = 1/sqrt(2) ≈ 0.7071
    SignalNormalizer norm;
    const uint32_t token = 150003;

    SignalBundle b1, b2;
    b1.instrument_token = b2.instrument_token = token;
    b1.timestamp_ns = b2.timestamp_ns = 0;
    b1.values.fill(0.0);
    b2.values.fill(2.0);

    norm.update(b1);
    auto res = norm.update(b2);

    const double expected_z = (2.0 - 1.0) / std::sqrt(2.0);
    for (uint32_t k = 0; k < NUM_SIGNALS; ++k) {
        EXPECT_TRUE(res.valid[k]);
        EXPECT_NEAR(res.z[k], expected_z, 1e-12);
    }
}

TEST(SignalNormalizerTest, NCountIncrementsCorrectly) {
    SignalNormalizer norm;
    SignalBundle b;
    b.instrument_token = 150004;
    b.timestamp_ns = 0;
    b.values.fill(1.0);

    for (uint32_t i = 1; i <= 5; ++i) {
        auto res = norm.update(b);
        EXPECT_EQ(res.n, i);
    }
}

TEST(SignalNormalizerTest, MultipleInstrumentsAreIndependent) {
    SignalNormalizer norm;
    SignalBundle bA, bB;
    bA.instrument_token = 150005;
    bB.instrument_token = 150006;
    bA.timestamp_ns = bB.timestamp_ns = 0;
    bA.values.fill(1.0);
    bB.values.fill(10.0);

    norm.update(bA);
    norm.update(bB);
    auto resA = norm.update(bA);
    auto resB = norm.update(bB);

    // A: two equal values → z=0
    EXPECT_NEAR(resA.z[0], 0.0, 1e-15);
    // B: two equal values → z=0
    EXPECT_NEAR(resB.z[0], 0.0, 1e-15);
    // But mu should differ
    EXPECT_NEAR(resA.mu[0], 1.0, 1e-12);
    EXPECT_NEAR(resB.mu[0], 10.0, 1e-12);
}

// ─── CompositeScoreCalculator Tests ──────────────────────────────────────────

TEST(CompositeScoreTest, DefaultEqualWeights) {
    CompositeScoreCalculator calc;
    // Default: equal IC → w_k = 1/8 for each of 8 signals
    NormalizedBundle z;
    z.instrument_token = 160001;
    z.timestamp_ns = 0;
    z.z.fill(1.0);    // all z=1.0
    z.mu.fill(0.0);
    z.sigma.fill(1.0);
    z.valid.fill(true);
    z.n = 2;

    auto res = calc.compute(z);
    // Score = Σ (1/8) * 1.0 = 1.0
    EXPECT_NEAR(res.score, 1.0, 1e-12);
}

TEST(CompositeScoreTest, ScoreFormula) {
    CompositeScoreCalculator calc;
    // Set IC weights manually: IC_k = k+1 (positive, unequal)
    std::array<double, NUM_SIGNALS> ic;
    for (uint32_t k = 0; k < NUM_SIGNALS; ++k) ic[k] = static_cast<double>(k + 1);
    calc.set_ic_weights(ic);

    NormalizedBundle z;
    z.instrument_token = 160002;
    z.timestamp_ns = 0;
    z.valid.fill(true);
    z.n = 2;
    z.z.fill(0.0);
    z.z[0] = 1.0; // only first signal is non-zero

    // weight[0] = IC[0] / Σ IC = 1 / (1+2+3+4+5+6+7+8) = 1/36
    const double sum_ic = 1+2+3+4+5+6+7+8; // 36
    const double w0 = 1.0 / sum_ic;

    auto res = calc.compute(z);
    EXPECT_NEAR(res.score, w0 * 1.0, 1e-12);
    EXPECT_NEAR(res.weights[0], w0, 1e-12);
}

TEST(CompositeScoreTest, NegativeICIsZeroed) {
    CompositeScoreCalculator calc;
    std::array<double, NUM_SIGNALS> ic;
    ic.fill(1.0);
    ic[0] = -5.0; // negative → should be zeroed
    calc.set_ic_weights(ic);

    // Only 7 signals remain with IC=1.0 → w_k = 1/7
    NormalizedBundle z;
    z.instrument_token = 160003;
    z.timestamp_ns = 0;
    z.valid.fill(true); z.n = 2;
    z.z.fill(0.0);
    z.z[1] = 1.0; // signal 1 (IC=1.0, weight=1/7)

    auto res = calc.compute(z);
    EXPECT_NEAR(res.weights[0], 0.0, 1e-12); // zeroed IC
    EXPECT_NEAR(res.score, 1.0 / 7.0, 1e-12);
}

TEST(CompositeScoreTest, PWinIsSigmoidOfScore) {
    CompositeScoreCalculator calc;
    NormalizedBundle z;
    z.instrument_token = 160004;
    z.timestamp_ns = 0;
    z.valid.fill(true); z.n = 2;
    z.z.fill(1.0); // score = 1.0 with equal weights

    auto res = calc.compute(z);
    const double expected_p = 1.0 / (1.0 + std::exp(-res.score));
    EXPECT_NEAR(res.p_win, expected_p, 1e-12);
}

TEST(CompositeScoreTest, KellyFullFormula) {
    CompositeScoreCalculator calc;
    NormalizedBundle z;
    z.instrument_token = 160005;
    z.timestamp_ns = 0;
    z.valid.fill(true); z.n = 2;
    z.z.fill(0.0); // score = 0 → p = 0.5

    auto res = calc.compute(z);
    // p = 0.5, b = 1.0: f* = (0.5*(1+1) - 1) / 1 = 0
    EXPECT_NEAR(res.kelly_full, 0.0, 1e-12);
    EXPECT_NEAR(res.kelly_half, 0.0, 1e-12);
}

TEST(CompositeScoreTest, KellyHalfIsClampedToUnitInterval) {
    CompositeScoreCalculator calc;
    NormalizedBundle z;
    z.instrument_token = 160006;
    z.timestamp_ns = 0;
    z.valid.fill(true); z.n = 2;
    z.z.fill(100.0); // extreme score → p ≈ 1 → f* ≈ 1 → f=0.5

    auto res = calc.compute(z);
    EXPECT_GE(res.kelly_half, 0.0);
    EXPECT_LE(res.kelly_half, 1.0);
}

TEST(CompositeScoreTest, NegativeKellyClampedToZero) {
    CompositeScoreCalculator calc;
    NormalizedBundle z;
    z.instrument_token = 160007;
    z.timestamp_ns = 0;
    z.valid.fill(true); z.n = 2;
    z.z.fill(-100.0); // extreme negative score → p ≈ 0 → f* very negative

    auto res = calc.compute(z);
    EXPECT_EQ(res.kelly_half, 0.0);
}

TEST(CompositeScoreTest, QualityCountsActiveSignals) {
    CompositeScoreCalculator calc;
    NormalizedBundle z;
    z.instrument_token = 160008;
    z.timestamp_ns = 0;
    z.valid.fill(true); z.n = 2;
    z.z.fill(0.0);
    // Set 4 signals to |z| > 1
    z.z[0] = 2.0; z.z[1] = -1.5; z.z[2] = 3.0; z.z[3] = -2.0;

    auto res = calc.compute(z);
    // 4 out of 8 signals active → quality = 4/8 * 100 = 50.0
    EXPECT_NEAR(res.quality, 50.0, 1e-12);
}

// ─── PnLTracker Tests ─────────────────────────────────────────────────────────

TEST(PnLTrackerTest, NoPositionReturnsZero) {
    PnLTracker tracker;
    auto res = tracker.update(170001, 100.0);
    EXPECT_FALSE(res.is_open);
    EXPECT_EQ(res.gross_pnl, 0.0);
    EXPECT_EQ(res.unrealized_pnl, 0.0);
}

TEST(PnLTrackerTest, OpenLongPositionCosts) {
    PnLTracker tracker;
    // entry_costs = brokerage + slippage = 20 + 2*0.05*100 = 30
    tracker.open_position(170002, 100.0, 100, +1);
    EXPECT_TRUE(tracker.has_open_position(170002));
    auto res = tracker.update(170002, 100.0); // no price change
    EXPECT_NEAR(res.gross_pnl, 0.0, 1e-12);
    EXPECT_TRUE(res.is_open);
}

TEST(PnLTrackerTest, GrossPnLForLongPosition) {
    PnLTracker tracker;
    // Q=100, entry=100.0, direction=+1
    tracker.open_position(170003, 100.0, 100, +1);
    auto res = tracker.update(170003, 105.0);
    // gross = 100 * (105 - 100) * 1 = 500
    EXPECT_NEAR(res.gross_pnl, 500.0, 1e-10);
}

TEST(PnLTrackerTest, GrossPnLForShortPosition) {
    PnLTracker tracker;
    // Q=100, entry=100.0, direction=-1
    tracker.open_position(170004, 100.0, 100, -1);
    auto res = tracker.update(170004, 95.0);
    // gross = 100 * (95 - 100) * (-1) = 500
    EXPECT_NEAR(res.gross_pnl, 500.0, 1e-10);
}

TEST(PnLTrackerTest, UnrealizedPnLIncludesAllCosts) {
    CostModel cm;
    cm.brokerage = 20.0; cm.stt_rate = 0.00025; cm.tick_size = 0.05; cm.slippage_ticks = 2.0;
    PnLTracker tracker(cm);

    const double entry = 100.0, qty = 100.0;
    tracker.open_position(170005, entry, static_cast<uint64_t>(qty), +1);

    const double current = 105.0;
    auto res = tracker.update(170005, current);

    const double gross = qty * (current - entry); // 500
    // Entry costs: brokerage + slippage = 20 + 2*0.05*100 = 30
    const double entry_costs = 20.0 + 2.0 * 0.05 * qty;
    // Exit costs (long→sell): brokerage + STT + slippage
    const double exit_stt = current * qty * 0.00025; // 105*100*0.00025 = 2.625
    const double exit_slip = 2.0 * 0.05 * qty;       // 10
    const double total_costs = entry_costs + 20.0 + exit_stt + exit_slip;
    EXPECT_NEAR(res.costs, total_costs, 1e-8);
    EXPECT_NEAR(res.unrealized_pnl, gross - total_costs, 1e-8);
}

TEST(PnLTrackerTest, ShortPositionNoExitSTT) {
    // Short position: close by buying → no STT (buy side)
    CostModel cm;
    PnLTracker tracker(cm);
    tracker.open_position(170006, 100.0, 100, -1);
    auto res = tracker.update(170006, 95.0);
    // exit_stt = 0 for short positions
    const double entry_costs = 20.0 + 2.0 * 0.05 * 100; // 30
    const double exit_costs = 20.0 + 0.0 + 2.0 * 0.05 * 100; // 30
    const double total_costs = entry_costs + exit_costs; // 60
    EXPECT_NEAR(res.costs, total_costs, 1e-8);
}

TEST(PnLTrackerTest, ClosePositionSetsIsOpenFalse) {
    PnLTracker tracker;
    tracker.open_position(170007, 100.0, 100, +1);
    tracker.close_position(170007, 105.0);
    EXPECT_FALSE(tracker.has_open_position(170007));
}

TEST(PnLTrackerTest, ClosePositionReturnsCorrectPnL) {
    PnLTracker tracker;
    tracker.open_position(170008, 100.0, 100, +1);
    auto res = tracker.close_position(170008, 105.0);
    EXPECT_NEAR(res.gross_pnl, 500.0, 1e-10);
    EXPECT_FALSE(res.is_open); // position is now closed
}

// ─── AlphaDecayCalculator Tests ───────────────────────────────────────────────

TEST(AlphaDecayTest, DefaultLambdaFormula) {
    // lambda = 0.10 + 0.01*N + 0*V + 5.0*sigma^2
    // N=0, V=0, sigma^2=0 → lambda = 0.10
    AlphaDecayCalculator calc;
    auto res = calc.project(180001, 0.01, 0, 0, 0.0);
    EXPECT_NEAR(res.lambda_hat, 0.10, 1e-12);
}

TEST(AlphaDecayTest, LambdaWithAnalystCoverage) {
    // N=10, V=0, sigma^2=0 → lambda = 0.10 + 0.01*10 = 0.20
    AlphaDecayCalculator calc;
    auto res = calc.project(180002, 0.01, 10, 0, 0.0);
    EXPECT_NEAR(res.lambda_hat, 0.20, 1e-12);
}

TEST(AlphaDecayTest, LambdaWithVariance) {
    // N=0, V=0, sigma^2=0.0002 → lambda = 0.10 + 5.0*0.0002 = 0.101
    AlphaDecayCalculator calc;
    auto res = calc.project(180003, 0.01, 0, 0, 0.0002);
    EXPECT_NEAR(res.lambda_hat, 0.101, 1e-12);
}

TEST(AlphaDecayTest, LambdaWithVolume) {
    // Volume is scaled by 1e-6; lambda_2 = 0 by default → no contribution
    AlphaDecayCalculator calc;
    auto res = calc.project(180004, 0.01, 0, 5000000, 0.0); // 5M shares = 5.0 in millions
    EXPECT_NEAR(res.lambda_hat, 0.10, 1e-12); // lambda_2=0 → no effect
}

TEST(AlphaDecayTest, AlphaDecayFormula) {
    // alpha_decayed = alpha_hat * exp(-lambda * dt)
    AlphaDecayCalculator calc;
    const double alpha = 0.01, lambda = 0.10, dt = ALPHA_DECAY_DT;
    auto res = calc.project(180005, alpha, 0, 0, 0.0, dt);
    const double expected = alpha * std::exp(-lambda * dt);
    EXPECT_NEAR(res.alpha_decayed, expected, 1e-15);
}

TEST(AlphaDecayTest, HalfLifeFormula) {
    // half_life = ln(2) / (lambda * dt)
    AlphaDecayCalculator calc;
    const double lambda = 0.10, dt = ALPHA_DECAY_DT;
    auto res = calc.project(180006, 0.01, 0, 0, 0.0, dt);
    const double expected_hl = std::log(2.0) / (lambda * dt);
    EXPECT_NEAR(res.half_life_bars, expected_hl, 1.0); // large value, relative tolerance
}

TEST(AlphaDecayTest, ZeroLambdaInfiniteHalfLife) {
    // If lambda * dt = 0, half_life = infinity
    AlphaDecayCalculator calc;
    // Set custom coefficients with all zeros to get lambda=0
    DecayCoefficients all_zero{0.0, 0.0, 0.0, 0.0};
    calc.set_coefficients(180007, all_zero);
    auto res = calc.project(180007, 0.01, 0, 0, 0.0);
    EXPECT_EQ(res.lambda_hat, 0.0);
    EXPECT_EQ(res.half_life_bars, std::numeric_limits<double>::infinity());
}

TEST(AlphaDecayTest, AlphaDecayedLessThanOriginal) {
    // Positive lambda → decay factor < 1 → |alpha_decayed| < |alpha_hat|
    AlphaDecayCalculator calc;
    auto res = calc.project(180008, 0.01, 0, 0, 0.0);
    EXPECT_LT(std::abs(res.alpha_decayed), 0.01);
}

TEST(AlphaDecayTest, CustomCoefficients) {
    AlphaDecayCalculator calc;
    DecayCoefficients coeff{0.05, 0.02, 0.001, 2.0};
    calc.set_coefficients(180009, coeff);
    // N=5, V=1e6, sigma^2=0.01
    // lambda = 0.05 + 0.02*5 + 0.001*(1.0) + 2.0*0.01 = 0.05 + 0.10 + 0.001 + 0.02 = 0.171
    auto res = calc.project(180009, 0.01, 5, 1000000, 0.01);
    const double expected_lambda = 0.05 + 0.02 * 5 + 0.001 * 1.0 + 2.0 * 0.01;
    EXPECT_NEAR(res.lambda_hat, expected_lambda, 1e-12);
}
