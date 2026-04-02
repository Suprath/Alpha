#pragma once

#include <string>
#include <vector>
#include <alpha/models/MarketModels.hpp>

namespace alpha::ingester {

/**
 * @brief Abstract Base Class for all Market Data Ingesters.
 * Each vendor (Upstox, Zerodha, Binance) must implement this interface.
 */
class DataIngester {
public:
    virtual ~DataIngester() = default;

    /**
     * @brief Authenticate with the vendor's API.
     * @param code Optional authentication code (e.g. OAuth code).
     * @return true if authentication succeeded or a valid session exists.
     */
    virtual bool authenticate(const std::string& code = "") = 0;

    /**
     * @brief Establish the primary data feed connection (WebSocket/TCP).
     */
    virtual void connect_feed() = 0;

    /**
     * @brief Subscribe to specific market instruments.
     * @param symbols Vector of vendor-specific instrument keys.
     */
    virtual void subscribe(const std::vector<std::string>& symbols) = 0;

    /**
     * @brief Fetch historical candle data.
     */
    virtual std::vector<alpha::models::Candle> fetch_historical(
        const std::string& symbol, 
        const std::string& interval,
        const std::string& from, 
        const std::string& to) = 0;

    /**
     * @brief Test-only: Inject a message manually (exposed for GTest).
     */
    virtual void on_message(const std::string& data) = 0;
};

} // namespace alpha::ingester
