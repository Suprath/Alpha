#pragma once

#include <string>
#include <iostream>
#include <sstream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <alpha/models/MarketModels.hpp>

namespace alpha::signal::core {

/**
 * @brief High-performance QuestDB InfluxDB Line Protocol (ILP) client over TCP.
 */
class QuestDBClient {
public:
    QuestDBClient(const std::string& host = "questdb", int port = 9009)
        : host_(host), port_(port), sock_(-1) {}

    ~QuestDBClient() {
        if (sock_ != -1) ::close(sock_);
    }

    bool connect() {
        struct hostent* server = gethostbyname(host_.c_str());
        if (server == nullptr) {
            std::cerr << "[QuestDB] DNS lookup failed for " << host_ << std::endl;
            return false;
        }

        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;

        struct sockaddr_in serv_addr;
        std::memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
        serv_addr.sin_port = htons(port_);

        if (::connect(sock_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cerr << "[QuestDB] Connection failed to " << host_ << ":" << port_ << std::endl;
            return false;
        }
        
        std::cout << "[QuestDB] Connected to ILP port at " << host_ << ":" << port_ << std::endl;
        return true;
    }

    /**
     * @brief Send a Candle to QuestDB using ILP.
     */
    void write_candle(const alpha::models::Candle& candle, const std::string& symbol, const std::string& interval = "1m") {
        if (sock_ == -1) return;

        // Line Protocol Format:
        // candles,symbol=RELIANCE,interval=1m open=1.0,high=2.0,low=0.5,close=1.5,volume=100i,open_interest=0.0 1670000000000000000
        std::stringstream ss;
        ss << "candles,symbol=" << symbol << ",interval=" << interval << " "
           << "open=" << candle.open << ","
           << "high=" << candle.high << ","
           << "low=" << candle.low << ","
           << "close=" << candle.close << ","
           << "volume=" << candle.volume << "i,"
           << "open_interest=" << candle.open_interest << " "
           << candle.timestamp_ns << "\n";

        std::string payload = ss.str();
        ssize_t sent = send(sock_, payload.c_str(), payload.size(), 0);
        if (sent < 0) {
            std::cerr << "[QuestDB] Failed to send ILP data." << std::endl;
            ::close(sock_);
            sock_ = -1; // Trigger reconnect on next call if needed (or handle elsewhere)
        }
    }

private:
    std::string host_;
    int port_;
    int sock_;
};

} // namespace alpha::signal::core
