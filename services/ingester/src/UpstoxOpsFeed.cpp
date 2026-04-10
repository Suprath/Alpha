#include <alpha/ingester/UpstoxOpsFeed.hpp>
#include <alpha/config/Config.hpp>
#include <alpha/time/Timestamp.hpp>
#include <iostream>

namespace alpha::ingester {

using namespace com::upstox::marketdatafeederv3udapi::rpc::proto;

UpstoxOpsFeed::UpstoxOpsFeed(net::io_context& ioc, ssl::context& ssl_ctx,
                             const std::string& qdb_host, int qdb_ilp_port,
                             const std::unordered_map<uint32_t, std::string>& token_map)
    : UpstoxFeed(ioc, ssl_ctx),
      qdb_host_(qdb_host), qdb_ilp_port_(qdb_ilp_port),
      token_to_symbol_map_(token_map) {

    worker_thread_ = std::thread(&UpstoxOpsFeed::worker_loop, this);
}

UpstoxOpsFeed::~UpstoxOpsFeed() {
    stop_worker_ = true;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void UpstoxOpsFeed::handle_message(const std::string& data) {
    FeedResponse feed;
    if (!feed.ParseFromString(data)) return;

    for (auto const& [key, value] : feed.feeds()) {
        if (!value.has_fullfeed() || !value.fullfeed().has_marketff()) continue;

        auto const& ff        = value.fullfeed();
        auto const& market_ff = ff.marketff();

        // 1. Persist Greeks/IV if available
        if (market_ff.has_optiongreeks() || market_ff.iv() > 0) {
            auto const& g_proto = market_ff.optiongreeks();
            OptionGreeks g;
            g.delta = g_proto.delta();
            g.gamma = g_proto.gamma();
            g.theta = g_proto.theta();
            g.vega  = g_proto.vega();
            g.rho   = g_proto.rho();
            g.iv    = market_ff.iv();
            persist_greeks(key, g, alpha::time::Timestamp::now_ns());
        }

        // 2. Persist Tick
        Tick t;
        t.last_price    = market_ff.ltpc().ltp();
        t.total_volume  = static_cast<uint64_t>(market_ff.vtt());
        t.open_interest = market_ff.oi();

        if (market_ff.has_marketlevel() && market_ff.marketlevel().bidaskquote_size() > 0) {
            const auto& q0 = market_ff.marketlevel().bidaskquote(0);
            t.bid_price = q0.bidp();
            t.bid_size  = static_cast<uint32_t>(q0.bidq());
            t.ask_price = q0.askp();
            t.ask_size  = static_cast<uint32_t>(q0.askq());
        }

        t.timestamp_ns = alpha::time::Timestamp::now_ns();
        persist_tick_by_symbol(key, t);

        // 3. Persist various timeframes
        if (market_ff.has_marketohlc()) {
            for (auto const& ohlc : market_ff.marketohlc().ohlc()) {
                Candle c;
                c.open          = ohlc.open();
                c.high          = ohlc.high();
                c.low           = ohlc.low();
                c.close         = ohlc.close();
                c.volume        = static_cast<uint64_t>(ohlc.vol());
                c.timestamp_ns  = static_cast<uint64_t>(ohlc.ts());
                c.open_interest = market_ff.oi();
                persist_candle(c, key, ohlc.interval());
            }
        }
    }
}

void UpstoxOpsFeed::persist_candle(const Candle& c, const std::string& symbol, const std::string& interval) {
    std::string line = "candles,symbol=" + symbol +
                       ",interval=" + interval +
                       " open=" + std::to_string(c.open) +
                       ",high=" + std::to_string(c.high) +
                       ",low=" + std::to_string(c.low) +
                       ",close=" + std::to_string(c.close) +
                       ",volume=" + std::to_string(c.volume) + "i" +
                       ",open_interest=" + std::to_string(c.open_interest) +
                       " " + std::to_string(c.timestamp_ns) + "\n";
    push_to_queue(line);
}

void UpstoxOpsFeed::persist_tick(const Tick& t) {
    auto it = token_to_symbol_map_.find(t.instrument_token);
    if (it != token_to_symbol_map_.end()) {
        persist_tick_by_symbol(it->second, t);
    }
}

void UpstoxOpsFeed::persist_tick_by_symbol(const std::string& symbol, const Tick& t) {
    std::string line = "ticks,symbol=" + symbol +
                       " price=" + std::to_string(t.last_price) +
                       ",volume=" + std::to_string(t.total_volume) + "i" +
                       ",bid_price=" + std::to_string(t.bid_price) +
                       ",bid_size=" + std::to_string(t.bid_size) + "i" +
                       ",ask_price=" + std::to_string(t.ask_price) +
                       ",ask_size=" + std::to_string(t.ask_size) + "i" +
                       ",open_interest=" + std::to_string(t.open_interest) +
                       " " + std::to_string(t.timestamp_ns) + "\n";
    push_to_queue(line);
}

void UpstoxOpsFeed::persist_greeks(const std::string& symbol, const OptionGreeks& g, uint64_t ts_ns) {
    std::string line = "option_greeks,symbol=" + symbol +
                       " iv=" + std::to_string(g.iv) +
                       ",delta=" + std::to_string(g.delta) +
                       ",theta=" + std::to_string(g.theta) +
                       ",gamma=" + std::to_string(g.gamma) +
                       ",vega=" + std::to_string(g.vega) +
                       ",rho=" + std::to_string(g.rho) +
                       " " + std::to_string(ts_ns) + "\n";
    push_to_queue(line);
}

void UpstoxOpsFeed::push_to_queue(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        write_queue_.push_back(line);
    }
    queue_cv_.notify_one();
}

void UpstoxOpsFeed::worker_loop() {
    std::string batch_buffer;
    const size_t batch_limit = 100;

    while (!stop_worker_) {
        std::deque<std::string> local_queue;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !write_queue_.empty() || stop_worker_; });
            if (stop_worker_ && write_queue_.empty()) break;
            local_queue.swap(write_queue_);
        }

        ensure_qdb_connection();
        if (!qdb_socket_ || !qdb_socket_->is_open()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        batch_buffer.clear();
        size_t count = 0;
        for (const auto& line : local_queue) {
            batch_buffer += line;
            count++;
            if (count >= batch_limit) {
                try {
                    boost::asio::write(*qdb_socket_, boost::asio::buffer(batch_buffer));
                    batch_buffer.clear();
                    count = 0;
                } catch (...) {
                    qdb_socket_.reset();
                    break;
                }
            }
        }

        if (!batch_buffer.empty() && qdb_socket_) {
            try {
                boost::asio::write(*qdb_socket_, boost::asio::buffer(batch_buffer));
            } catch (...) {
                qdb_socket_.reset();
            }
        }
    }
}

void UpstoxOpsFeed::ensure_qdb_connection() {
    if (qdb_socket_ && qdb_socket_->is_open()) return;

    try {
        qdb_socket_ = std::make_unique<tcp::socket>(ioc_);
        tcp::resolver resolver(ioc_);
        auto endpoints = resolver.resolve(qdb_host_, std::to_string(qdb_ilp_port_));
        boost::asio::connect(*qdb_socket_, endpoints);
    } catch (...) {
        qdb_socket_.reset();
    }
}

} // namespace alpha::ingester
