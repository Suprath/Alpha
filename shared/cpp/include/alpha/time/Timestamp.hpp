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
        uint64_t nanosecond;
    };

    /**
     * @brief Extract IST time components from nanoseconds.
     */
    static inline IstTime get_ist(uint64_t ns_ist) {
        uint64_t total_seconds = ns_ist / 1000000000ULL;
        return {
            static_cast<uint32_t>((total_seconds / 3600) % 24),
            static_cast<uint32_t>((total_seconds / 60) % 60),
            static_cast<uint32_t>(total_seconds % 60),
            static_cast<uint32_t>((ns_ist / 1000000ULL) % 1000),
            ns_ist % 1000000000ULL
        };
    }
};

} // namespace alpha::time
