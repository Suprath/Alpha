#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <alpha/models/MarketModels.hpp>

namespace alpha {
namespace backfill {

/**
 * HistoricalRequest — parameters for one Upstox historical candle fetch.
 */
struct HistoricalRequest {
    std::string instrument_key;  // e.g. "NSE_EQ|INE002A01018" (Reliance)
    std::string interval;        // "1minute" | "5minute" | "30minute" | "day" | "week" | "month"
    std::string from_date;       // "YYYY-MM-DD"
    std::string to_date;         // "YYYY-MM-DD"
};

/**
 * UpstoxHistoricalFeed — fetches OHLCV candle history from Upstox REST API.
 *
 * Endpoint (v2):
 *   GET https://api.upstox.com/v2/historical-candle/{instrument_key}/{interval}/{to_date}/{from_date}
 *   Header: Authorization: Bearer {access_token}
 *
 * Response JSON candle array format per element:
 *   [timestamp_iso, open, high, low, close, volume, open_interest]
 *
 * The caller must supply a SlidingWindowThrottler to comply with rate limits.
 * This class does NOT enforce rate limits internally — that is the caller's job.
 *
 * Usage:
 *   UpstoxHistoricalFeed feed(access_token);
 *   HistoricalRequest req{"NSE_EQ|INE002A01018", "1minute", "2024-01-01", "2024-01-31"};
 *   feed.fetch(req, 408065, "RELIANCE", [](const Candle& c, uint32_t t, const std::string& s) {
 *       // process candle
 *   });
 */
class UpstoxHistoricalFeed {
public:
    using CandleCallback = std::function<void(const models::Candle&,
                                              uint32_t        instrument_token,
                                              const std::string& symbol)>;

    explicit UpstoxHistoricalFeed(std::string access_token);

    /**
     * Fetch all candles for the given request, calling callback for each.
     * Candles are delivered in chronological order (oldest first).
     *
     * Performs a single HTTPS GET — Upstox returns up to ~2000 bars per call.
     * For larger date ranges use fetch_range() which auto-chunks.
     *
     * @throws std::runtime_error on HTTP error or JSON parse failure.
     * @return Number of candles delivered to the callback.
     */
    size_t fetch(const HistoricalRequest& req,
                 uint32_t                 instrument_token,
                 const std::string&       symbol,
                 CandleCallback           callback);

    /**
     * fetch_range — Auto-chunked fetch over an arbitrary date range.
     *
     * Splits [from_date, to_date] into interval-appropriate chunks so each
     * HTTP request stays within the ~2000-bar Upstox per-call limit:
     *   1minute  →  4-day chunks   (375 bars/day × 4 = 1 500)
     *   5minute  → 24-day chunks   ( 75 bars/day × 24 = 1 800)
     *   15minute → 75-day chunks
     *   30minute → 150-day chunks
     *   60minute → 300-day chunks
     *   day/week/month → single call (no chunking)
     *
     * pre_fetch_cb is called before each HTTP request — pass your
     * SlidingWindowThrottler::acquire() lambda here.
     *
     * @return Total candles delivered across all chunks.
     */
    size_t fetch_range(const HistoricalRequest&      req,
                       uint32_t                      instrument_token,
                       const std::string&            symbol,
                       CandleCallback                callback,
                       std::function<void()>         pre_fetch_cb = nullptr);

    /**
     * Returns the recommended chunk size in calendar days for the given interval.
     * Returns 0 for intervals that do not require chunking.
     */
    static int chunk_days_for_interval(const std::string& interval);

private:
    // Performs synchronous HTTPS GET via Boost.Beast. Returns response body.
    std::string do_https_get(const std::string& path);

    // Parses Upstox JSON candle array into Candle structs.
    std::vector<models::Candle> parse_response(const std::string& json_body);

    // Converts Upstox ISO-8601 timestamp ("+05:30" suffix) to nanoseconds since epoch.
    static uint64_t iso8601_to_ns(const std::string& iso);

    std::string access_token_;

    static constexpr const char* API_HOST    = "api.upstox.com";
    static constexpr uint16_t    API_PORT    = 443;
    static constexpr const char* API_VERSION = "/v2";
};

} // namespace backfill
} // namespace alpha
