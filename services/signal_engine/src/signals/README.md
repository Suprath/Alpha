# Signal Engine — Signals

This directory contains all signal calculators. Each signal lives in its own subdirectory and is completely self-contained.

## Directory layout

```
signals/
├── ofi/              Order Flow Imbalance
├── trade_direction/  Tick-level trade direction classifier
├── vpin/             Volume-synchronised Probability of Informed trading
├── kyles_lambda/     Kyle's Lambda price-impact estimator
├── entropy/          L2 depth distribution entropy
├── bar/              Bar-level signals (LogReturn, RealizedVolatility, KalmanFilter, CUSUM)
├── derived/          Aggregation layer (SignalBundle, SignalNormalizer, CompositeScore,
│                     AlphaDecay, PnLTracker)
└── options/          Options-specific signals (BSGamma, GEX, VRP, OptionsChain)
```

## How to add a new signal

### 1 — Write the calculator

Create `signals/<name>/<Name>Calculator.hpp`. Follow the existing pattern:

```cpp
namespace alpha::signal::signals::<name> {

struct <Name>Result {
    uint32_t instrument_token;
    double   value;     // your output
    bool     valid;     // false on first tick (no prior state)
};

class <Name>Calculator {
public:
    // Tick-level signal: takes a Tick
    [[nodiscard]] <Name>Result update(const alpha::models::Tick& tick) noexcept;

    // Bar-level signal: takes an EnhancedBar
    [[nodiscard]] <Name>Result update(const alpha::signal::core::EnhancedBar& bar) noexcept;

    void reset() noexcept;
private:
    // per-instrument state (use a hash table like OFICalculator for tick-level,
    // or std::unordered_map for bar-level — bar frequency, latency doesn't matter)
};

} // namespace
```

Key constraints:
- **No heap allocation in `update()`** — pre-allocate all state in the constructor
- **O(1) per tick** — no loops proportional to history length
- **Not thread-safe by design** — the spin loop is single-threaded

### 2 — Register the signal index

In `derived/SignalBundle.hpp`:

```cpp
// Increment NUM_SIGNALS
static constexpr uint32_t NUM_SIGNALS = 9u;  // was 8

// Add enum entry (always append — never reorder)
enum SignalIndex : uint8_t {
    OFI_NORM    = 0,
    TRADE_DIR   = 1,
    VPIN        = 2,
    KYLE_LAMBDA = 3,
    ENTROPY     = 4,
    LOG_RETURN  = 5,
    RV_ANN      = 6,
    BAR_OFI     = 7,
    MY_SIGNAL   = 8,   // ← new
};
```

`SignalNormalizer` and `CompositeScore` automatically handle the new slot — no changes needed there.

### 3 — (Tick-level only) Add to TickSignalSnapshot

In `core/TickSignalSnapshot.hpp`, add a field to carry the latest value to bar close:

```cpp
double my_signal = 0.0;
```

### 4 — Add to the pipeline

**Tick-level** → edit `pipelines/TickPipeline.hpp`:
```cpp
// include
#include <signals/<name>/<Name>Calculator.hpp>

// call in on_tick(), after existing signals
const auto my = my_calc_.update(tick);

// store in snapshot
if (my.valid) snap.my_signal = my.value;

// private member
alpha::signal::signals::<name>::<Name>Calculator my_calc_;
```

**Bar-level** → edit `pipelines/BarPipeline.hpp`:
```cpp
// include
#include <signals/<name>/<Name>Calculator.hpp>

// call in on_bar()
const auto my = my_calc_.update(bar);

// private member
alpha::signal::signals::<name>::<Name>Calculator my_calc_;
```

### 5 — Populate the bundle

In `BarPipeline::on_bar()`, after the existing bundle assignments:

```cpp
// Tick-level value comes from snap:
bundle[derived::MY_SIGNAL] = static_cast<double>(snap.my_signal);

// Bar-level value comes directly from the result:
bundle[derived::MY_SIGNAL] = my.valid ? my.value : 0.0;
```

That's all. `SignalNormalizer` will start z-scoring the new slot and `CompositeScore` will include it with equal IC weight until you calibrate with `set_ic_weights()`.

---

## Capacity

Arrays are pre-sized to `SIGNAL_CAPACITY = 32` (defined in `derived/SignalBundle.hpp`). You can add up to 32 signals before needing to increase the capacity. Increasing `SIGNAL_CAPACITY` is a one-line change with no behavioral impact.

## IC weight calibration

After adding a signal, run a backtest and measure its Information Coefficient. Pass calibrated values to:

```cpp
std::array<double, SIGNAL_CAPACITY> ic{};
ic[OFI_NORM]    = 0.12;
ic[MY_SIGNAL]   = 0.07;
// ...
composite_.set_ic_weights(ic);
```

Signals with IC ≤ 0 are automatically zeroed out by `CompositeScore`.
