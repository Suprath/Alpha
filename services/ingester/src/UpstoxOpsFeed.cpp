#include <alpha/ingester/UpstoxOpsFeed.hpp>
#include <alpha/config/Config.hpp>
#include <alpha/time/Timestamp.hpp>
#include <iostream>

namespace alpha::ingester {

void UpstoxOpsFeed::handle_message(const std::string& data) {
    Upstox::MarketDataFeed feed;
    if (!feed.ParseFromString(data)) return;

    for (auto const& [key, value] : feed.feeds()) {
        if (value.has_ff()) {
            auto const& ff = value.ff();
            auto const& market_ff = ff.marketff();
            
            // 1. Persist Greeks/IV if available
            if (market_ff.has_optiongreeks() || market_ff.iv() > 0) {
                auto const& g_proto = market_ff.optiongreeks();
                OptionGreeks g;
                g.delta = g_proto.delta();
                g.gamma = g_proto.gamma();
                g.theta = g_proto.theta();
                g.vega = g_proto.vega();
                g.rho = g_proto.rho();
                g.iv = market_ff.iv();
                persist_greeks(key, g, alpha::time::Timestamp::now_ns());
            }

            // 2. Persist Tick with OI and L1
            Tick t;
            t.last_price = market_ff.ltp();
            t.total_volume = market_ff.vtt();
            t.open_interest = market_ff.oi();
            
            if (market_ff.has_bids() && market_ff.bids().size() > 0) {
                t.bid_price = market_ff.bids(0).price();
                t.bid_size = market_ff.bids(0).quantity();
            }
            if (market_ff.has_asks() && market_ff.asks().size() > 0) {
                t.ask_price = market_ff.asks(0).price();
                t.ask_size = market_ff.asks(0).quantity();
            }
            
            t.timestamp_ns = alpha::time::Timestamp::now_ns();
            persist_tick_by_symbol(key, t);

            // 3. Persist various timeframes if present in the full feed
            for (auto const& ohlc : market_ff.ohlc()) {
                Candle c;
                c.open = ohlc.open();
                c.high = ohlc.high();
                c.low = ohlc.low();
                c.close = ohlc.close();
                c.volume = ohlc.vol();
                c.timestamp_ns = ohlc.ts();
                c.open_interest = market_ff.oi();
                persist_candle(c, key, ohlc.interval());
            }
        }
    }
}

void UpstoxOpsFeed::persist_candle(const Candle& c, const std::string& symbol, const std::string& interval) {
    ensure_qdb_connection();
    if (!qdb_socket_) return;

    std::string line = "candles,symbol=" + symbol + 
                       ",interval=" + interval +
                       " open=" + std::to_string(c.open) +
                       ",high=" + std::to_string(c.high) +
                       ",low=" + std::to_string(c.low) +
                       ",close=" + std::to_string(c.close) +
                       ",volume=" + std::to_string(c.volume) + "u" +
                       ",open_interest=" + std::to_string(c.open_interest) +
                       " " + std::to_string(c.timestamp_ns) + "\n";

    try {
        boost::asio::write(*qdb_socket_, boost::asio::buffer(line));
    } catch (...) { qdb_socket_.reset(); }
}

void UpstoxOpsFeed::persist_tick(const Tick& t) {
    auto it = token_to_symbol_map_.find(t.instrument_token);
    if (it != token_to_symbol_map_.end()) {
        persist_tick_by_symbol(it->second, t);
    }
}

void UpstoxOpsFeed::persist_tick_by_symbol(const std::string& symbol, const Tick& t) {
    ensure_qdb_connection();
    if (!qdb_socket_) return;

    std::string line = "ticks,symbol=" + symbol + 
                       " price=" + std::to_string(t.last_price) +
                       ",volume=" + std::to_string(t.total_volume) + "u" +
                       ",bid_price=" + std::to_string(t.bid_price) +
                       ",bid_size=" + std::to_string(t.bid_size) + "u" +
                       ",ask_price=" + std::to_string(t.ask_price) +
                       ",ask_size=" + std::to_string(t.ask_size) + "u" +
                       ",open_interest=" + std::to_string(t.open_interest) +
                       " " + std::to_string(t.timestamp_ns) + "\n";
    try {
        boost::asio::write(*qdb_socket_, boost::asio::buffer(line));
    } catch (...) { qdb_socket_.reset(); }
}

void UpstoxOpsFeed::persist_greeks(const std::string& symbol, const OptionGreeks& g, uint64_t ts_ns) {
    ensure_qdb_connection();
    if (!qdb_socket_) return;

    std::string line = "option_greeks,symbol=" + symbol + 
                       " iv=" + std::to_string(g.iv) +
                       ",delta=" + std::to_string(g.delta) +
                       ",theta=" + std::to_string(g.theta) +
                       ",gamma=" + std::to_string(g.gamma) +
                       ",vega=" + std::to_string(g.vega) +
                       ",rho=" + std::to_string(g.rho) +
                       " " + std::to_string(ts_ns) + "\n";
    try {
        boost::asio::write(*qdb_socket_, boost::asio::buffer(line));
    } catch (...) { qdb_socket_.reset(); }
}

void UpstoxOpsFeed::ensure_qdb_connection() {
    if (qdb_socket_ && qdb_socket_->is_open()) return;

    try {
        qdb_socket_ = std::make_unique<tcp::socket>(ioc_);
        tcp::resolver resolver(ioc_);
        auto endpoints = resolver.resolve(qdb_host_, std::to_string(qdb_ilp_port_));
        boost::asio::connect(*qdb_socket_, endpoints);
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect to QuestDB: " << e.what() << std::endl;
        qdb_socket_.reset();
    }
}

} // namespace alpha::ingester
