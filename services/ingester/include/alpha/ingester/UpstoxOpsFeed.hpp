#pragma once

#include <alpha/ingester/UpstoxFeed.hpp>
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>
#include <nlohmann/json.hpp>
#include "market_data_v3.pb.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace alpha::ingester {

using namespace alpha::models;
using json = nlohmann::json;

/**
 * @brief Handles background operational tasks.
 * Persists data to QuestDB via ILP and manages historical backfill responses.
 */
class UpstoxOpsFeed : public UpstoxFeed {
public:
    UpstoxOpsFeed(net::io_context& ioc, ssl::context& ssl_ctx,
                 const std::string& qdb_host, int qdb_ilp_port,
                 const std::unordered_map<uint32_t, std::string>& token_map);

    ~UpstoxOpsFeed();

    void persist_candle(const Candle& c, const std::string& symbol, const std::string& interval);
    void persist_tick(const Tick& t);
    void persist_tick_by_symbol(const std::string& symbol, const Tick& t);
    void persist_greeks(const std::string& symbol, const OptionGreeks& g, uint64_t ts_ns);

protected:
    void handle_message(const std::string& data) override;
    std::string get_feed_url() const override { return "/v3/feed/market-data-feed"; }

private:
    void ensure_qdb_connection();
    void worker_loop();
    void push_to_queue(const std::string& line);

    std::string qdb_host_;
    int qdb_ilp_port_;
    std::unique_ptr<tcp::socket> qdb_socket_;
    const std::unordered_map<uint32_t, std::string>& token_to_symbol_map_;

    // Async Queue for QuestDB
    std::deque<std::string> write_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::atomic<bool> stop_worker_{false};
};

} // namespace alpha::ingester
