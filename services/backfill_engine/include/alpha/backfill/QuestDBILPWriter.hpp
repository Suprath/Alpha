#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <alpha/models/MarketModels.hpp>
#include <alpha/models/BacktestTick.hpp>

namespace alpha {
namespace backfill {

/**
 * QuestDBILPWriter — batched ILP/TCP writer for QuestDB.
 *
 * InfluxDB Line Protocol format (one line per row):
 *   table,tag1=v1 field1=v1,field2=v2i timestamp_ns\n
 *
 * Batching strategy:
 *   - Buffer up to BATCH_BYTES before a TCP send syscall.
 *   - flush() forces an immediate send of whatever is buffered.
 *   - The destructor calls flush() automatically.
 *
 * Not thread-safe — single-producer assumed. Use one writer per thread.
 *
 * Usage:
 *   QuestDBILPWriter w("questdb", 9009);
 *   w.connect();
 *   w.write_candle(candle, token, "RELIANCE", "1minute");
 *   w.flush();
 */
class QuestDBILPWriter {
public:
    static constexpr size_t BATCH_BYTES    = 65536;  // 64 KB per TCP send
    static constexpr size_t MAX_LINE_BYTES = 512;    // Max chars per ILP line

    QuestDBILPWriter(std::string host, uint16_t port);
    ~QuestDBILPWriter();

    // Non-copyable
    QuestDBILPWriter(const QuestDBILPWriter&)            = delete;
    QuestDBILPWriter& operator=(const QuestDBILPWriter&) = delete;

    /** Open TCP connection to QuestDB ILP endpoint. Throws std::runtime_error on failure. */
    void connect();

    /** Disconnect and close the socket. Safe to call multiple times. */
    void disconnect();

    /**
     * Write a raw OHLCV candle to the candles_{interval} table.
     *
     * Table schema (auto-created by QuestDB on first insert):
     *   timestamp     TIMESTAMP (designated)
     *   instrument_token  INT
     *   symbol        SYMBOL
     *   open, high, low, close  DOUBLE
     *   volume        LONG
     *   open_interest LONG
     */
    void write_candle(const models::Candle& candle,
                      uint32_t              instrument_token,
                      const std::string&    symbol,
                      const std::string&    interval);

    /**
     * Write a pre-computed BacktestTick to the backtest_ticks table.
     *
     * Table schema:
     *   timestamp         TIMESTAMP (designated)
     *   instrument_token  INT
     *   symbol            SYMBOL
     *   last_price        DOUBLE
     *   volume            LONG
     *   bid_price, ask_price, vwap  DOUBLE
     */
    void write_backtest_tick(const models::BacktestTick& tick,
                             const std::string&          symbol);

    /**
     * Write a pre-calc enriched bar to the precalc_bars table.
     *
     * Table schema adds: ofi, kyle_lambda, realized_vol DOUBLE
     */
    void write_precalc_bar(const models::Candle& candle,
                           uint32_t              instrument_token,
                           const std::string&    symbol,
                           const std::string&    interval,
                           double                ofi,
                           double                kyle_lambda,
                           double                vwap,
                           double                realized_vol);

    /** Flush all buffered lines to QuestDB. Called automatically on destruct. */
    void flush();

    uint64_t rows_written()   const noexcept { return rows_written_; }
    uint64_t bytes_sent()     const noexcept { return bytes_sent_; }
    bool     is_connected()   const noexcept { return sock_fd_ >= 0; }

private:
    void append(const char* data, size_t len);
    void flush_internal();
    void send_all(const char* data, size_t len);

    std::string           host_;
    uint16_t              port_;
    int                   sock_fd_{-1};
    std::vector<char>     buf_;
    size_t                buf_used_{0};
    uint64_t              rows_written_{0};
    uint64_t              bytes_sent_{0};
};

} // namespace backfill
} // namespace alpha
