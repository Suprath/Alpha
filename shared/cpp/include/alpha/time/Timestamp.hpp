#pragma once

#include <chrono>
#include <cstdint>

namespace alpha::time {

/**
 * @brief High-precision timestamp utility.
 * Uses std::chrono::high_resolution_clock to provide nanosecond granularity.
 */
class Timestamp {
public:
    /**
     * @return Nanoseconds since epoch.
     */
    static inline uint64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    /**
     * @return Microseconds since epoch.
     */
    static inline uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    /**
     * @brief IST Offset (5 hours 30 minutes in nanoseconds)
     */
    static constexpr uint64_t IST_OFFSET_NS = 19800ULL * 1000000000ULL;

    /**
     * @return Nanoseconds in Indian Standard Time (IST).
     */
    static inline uint64_t now_ist_ns() {
        return now_ns() + IST_OFFSET_NS;
    }

    struct IstTime {
        uint32_t hour;
        uint32_t minute;
        uint32_t second;
        uint32_t millisecond;
        uint32_t day_of_week; // 0=Sun, 1=Mon, ..., 6=Sat
        uint64_t nanosecond;
    };

    /**
     * @brief Extract IST time components from nanoseconds.
     */
    static inline IstTime get_ist(uint64_t ns_ist) {
        time_t total_seconds = static_cast<time_t>(ns_ist / 1000000000ULL);
        struct tm *tm_ptr = gmtime(&total_seconds);
        
        return {
            static_cast<uint32_t>(tm_ptr->tm_hour),
            static_cast<uint32_t>(tm_ptr->tm_min),
            static_cast<uint32_t>(tm_ptr->tm_sec),
            static_cast<uint32_t>((ns_ist / 1000000ULL) % 1000),
            static_cast<uint32_t>(tm_ptr->tm_wday),
            ns_ist % 1000000000ULL
        };
    }

    /**
     * @brief Check if the current time is within Indian Market Hours (09:15 - 15:30 IST).
     */
    static inline bool is_market_session_active() {
        uint64_t ns_ist = now_ist_ns();
        IstTime ist = get_ist(ns_ist);
        
        // NSE/BSE Market Days: Monday (1) to Friday (5)
        if (ist.day_of_week == 0 || ist.day_of_week == 6) return false;
        
        uint32_t minutes_since_midnight = ist.hour * 60 + ist.minute;
        
        // 09:15 = 555 minutes, 15:30 = 930 minutes
        return (minutes_since_midnight >= 555 && minutes_since_midnight <= 930);
    }
};

} // namespace alpha::time
