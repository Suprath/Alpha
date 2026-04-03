#pragma once

#include <utility>
#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <unordered_map>
#include <boost/asio.hpp>
#include <alpha/models/MarketModels.hpp>
#include <alpha/rate_limit/RateLimiter.hpp>
#include <alpha/ingester/DataIngester.hpp>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>

#include <alpha/ingester/UpstoxLiveFeed.hpp>
#include <alpha/ingester/UpstoxOpsFeed.hpp>

namespace alpha::ingester {

using namespace alpha::models;
using namespace alpha::rate_limit;
namespace net = boost::asio;

/**
 * @brief Orchestrator for Upstox V3 Ingestion.
 * Manages the high-priority Live feed and the low-priority Ops feed.
 */
class UpstoxIngester : public DataIngester {
public:
    UpstoxIngester(net::io_context& ioc, bool with_db = true);
    ~UpstoxIngester() override = default;

    bool authenticate(const std::string& auth_code = "") override;
    void connect_feed() override;
    void subscribe(const std::vector<std::string>& symbols) override;
    std::vector<Candle> fetch_historical(const std::string& symbol, 
                                        const std::string& interval,
                                        const std::string& from, 
                                        const std::string& to) override;
    void on_message(const std::string& data) override;

private:
    struct BackfillTask {
        std::string symbol;
        std::string interval;
        std::string from;
        std::string to;
    };

    void save_token(const std::string& token);
    std::string load_token();
    void schedule_heartbeat();
    void load_instruments_from_db();
    void process_backfill_queue();
    void fetch_historical_rest(const BackfillTask& task);

    net::io_context& ioc_;
    ssl::context ssl_ctx_;
    std::string access_token_;
    
    // Decoupled Feeds
    std::shared_ptr<UpstoxLiveFeed> live_feed_;
    std::shared_ptr<UpstoxOpsFeed> ops_feed_;

    std::unique_ptr<RateLimiter> backfill_limiter_;
    std::queue<BackfillTask> backfill_queue_;

    // Context / Mapping
    std::unordered_map<std::string, uint32_t> instrument_map_;
    std::unordered_map<uint32_t, std::string> token_to_symbol_map_;

    // IPC & Monitoring
    std::unique_ptr<alpha::ipc::ShmManager> shm_manager_;
    alpha::ipc::SPSCRingBuffer<Tick, 65536>* ring_buffer_{nullptr};
    net::steady_timer heartbeat_timer_;
    net::steady_timer backfill_timer_;
};

} // namespace alpha::ingester
