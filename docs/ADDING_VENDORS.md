# Adding New Data Vendors to Alpha

Alpha uses a plugin-based architecture for market data ingestion. This allows you to add support for new brokers (e.g., Zerodha, Interactive Brokers, Binance) without modifying the core Signal Engine or Trading Logic.

## 1. Implement the `DataIngester` Interface

Create a new class in `services/ingester/src/` (e.g., `KiteIngester.cpp`) that inherits from `alpha::ingester::DataIngester`.

```cpp
#include <alpha/ingester/DataIngester.hpp>

class KiteIngester : public alpha::ingester::DataIngester {
public:
    bool authenticate(const std ::string& code) override {
        // Handle OAuth or API Key validation here
        return true;
    }

    void connect_feed() override {
        // Initialize your WebSocket or TCP stream
    }

    void on_message(const std::string& data) override {
        // 1. Parse the vendor's binary/JSON format
        // 2. Map it to the internal 'alpha::models::Tick' struct
        // 3. Push to the shared memory: ring_buffer_->push(t);
    }
};
```

## 2. Standardize the Output (`Tick` model)

Regardless of the vendor's format, you MUST normalize the data into the `alpha::models::Tick` structure defined in `shared/cpp/include/alpha/models/MarketModels.hpp`.

- **Instrument Tokens**: Use the `instrument_token` (integer) mapped from the PostgreSQL registry for $O(1)$ routing.
- **Timestamps**: All timestamps must be converted to **Nanoseconds (IST)** using `alpha::time::Timestamp::now_ns()`.

## 3. Register in `IngesterFactory`

Add your new vendor to `services/ingester/src/IngesterFactory.cpp`:

```cpp
if (v == "UPSTOX") {
    return std::make_unique<UpstoxIngester>(ioc);
} else if (v == "KITE") {
    return std::make_unique<KiteIngester>(ioc); // Add this
}
```

## 4. Update Environment Configuration

Set the `DATA_VENDOR` variable in your `.env` or Docker Compose file:

```yaml
services:
  ingester:
    environment:
      - DATA_VENDOR=KITE
```

## 5. Verification

Run the unit tests to ensure your new decoder doesn't violate memory alignment rules:
`docker compose run --rm --entrypoint /app/alpha-ingester-test ingester`
