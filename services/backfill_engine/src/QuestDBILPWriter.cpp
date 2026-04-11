#include <alpha/backfill/QuestDBILPWriter.hpp>

#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <cstdio>

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

namespace alpha {
namespace backfill {

QuestDBILPWriter::QuestDBILPWriter(std::string host, uint16_t port)
    : host_(std::move(host))
    , port_(port)
{
    buf_.resize(BATCH_BYTES + MAX_LINE_BYTES);
}

QuestDBILPWriter::~QuestDBILPWriter() {
    if (sock_fd_ >= 0) {
        try { flush(); } catch (...) {}
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
}

void QuestDBILPWriter::connect() {
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port_);
    struct addrinfo* res = nullptr;

    int rc = ::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0)
        throw std::runtime_error("QuestDBILPWriter: getaddrinfo failed: " + std::string(gai_strerror(rc)));

    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            sock_fd_ = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(res);

    if (sock_fd_ < 0)
        throw std::runtime_error("QuestDBILPWriter: cannot connect to " + host_ + ":" + port_str);
}

void QuestDBILPWriter::disconnect() {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
}

// ── ILP line builders ─────────────────────────────────────────────────────

void QuestDBILPWriter::write_candle(const models::Candle& c,
                                    uint32_t              token,
                                    const std::string&    symbol,
                                    const std::string&    interval) {
    // Table name: candles_1minute, candles_5minute, candles_1day, etc.
    std::string table = "candles_" + interval;

    char line[MAX_LINE_BYTES];
    int n = std::snprintf(line, sizeof(line),
        "%s,symbol=%s,token=%u "
        "open=%.4f,high=%.4f,low=%.4f,close=%.4f,volume=%llui,open_interest=%di "
        "%llu\n",
        table.c_str(), symbol.c_str(), token,
        c.open, c.high, c.low, c.close,
        static_cast<unsigned long long>(c.volume),
        c.open_interest,
        static_cast<unsigned long long>(c.timestamp_ns));

    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line))
        throw std::runtime_error("QuestDBILPWriter: ILP line overflow for candle");

    append(line, static_cast<size_t>(n));
    ++rows_written_;
}

void QuestDBILPWriter::write_backtest_tick(const models::BacktestTick& tick,
                                           const std::string&          symbol) {
    char line[MAX_LINE_BYTES];
    int n = std::snprintf(line, sizeof(line),
        "backtest_ticks,symbol=%s,token=%u "
        "last_price=%.4f,volume=%ui,bid=%.4f,ask=%.4f,vwap=%.4f "
        "%llu\n",
        symbol.c_str(), tick.instrument_token,
        static_cast<double>(tick.last_price),
        tick.volume,
        static_cast<double>(tick.bid_price),
        static_cast<double>(tick.ask_price),
        static_cast<double>(tick.vwap),
        static_cast<unsigned long long>(tick.timestamp_ns));

    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line))
        throw std::runtime_error("QuestDBILPWriter: ILP line overflow for BacktestTick");

    append(line, static_cast<size_t>(n));
    ++rows_written_;
}

void QuestDBILPWriter::write_precalc_bar(const models::Candle& c,
                                         uint32_t              token,
                                         const std::string&    symbol,
                                         const std::string&    interval,
                                         double                ofi,
                                         double                kyle_lambda,
                                         double                vwap,
                                         double                realized_vol) {
    char line[MAX_LINE_BYTES];
    int n = std::snprintf(line, sizeof(line),
        "precalc_bars,symbol=%s,token=%u,interval=%s "
        "open=%.4f,high=%.4f,low=%.4f,close=%.4f,"
        "volume=%llui,ofi=%.4f,kyle_lambda=%.6f,vwap=%.4f,realized_vol=%.6f "
        "%llu\n",
        symbol.c_str(), token, interval.c_str(),
        c.open, c.high, c.low, c.close,
        static_cast<unsigned long long>(c.volume),
        ofi, kyle_lambda, vwap, realized_vol,
        static_cast<unsigned long long>(c.timestamp_ns));

    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line))
        throw std::runtime_error("QuestDBILPWriter: ILP line overflow for precalc_bar");

    append(line, static_cast<size_t>(n));
    ++rows_written_;
}

// ── Internal buffer management ────────────────────────────────────────────

void QuestDBILPWriter::append(const char* data, size_t len) {
    if (buf_used_ + len > BATCH_BYTES)
        flush_internal();

    std::memcpy(buf_.data() + buf_used_, data, len);
    buf_used_ += len;
}

void QuestDBILPWriter::flush() {
    if (buf_used_ > 0) flush_internal();
}

void QuestDBILPWriter::flush_internal() {
    if (buf_used_ == 0) return;
    if (sock_fd_ < 0)
        throw std::runtime_error("QuestDBILPWriter: flush() called but not connected");
    send_all(buf_.data(), buf_used_);
    bytes_sent_ += buf_used_;
    buf_used_ = 0;
}

void QuestDBILPWriter::send_all(const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock_fd_, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(
                std::string("QuestDBILPWriter: send() failed: ") + std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

} // namespace backfill
} // namespace alpha
