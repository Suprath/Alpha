#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <cstddef>
#include <iostream>

#include <boost/interprocess/managed_shared_memory.hpp>
#include <alpha/models/MarketModels.hpp>

// Hardware L1/L2 cache line size for x86_64 and ARM64
#define CACHE_LINE_SIZE 64

namespace alpha::ipc {

/**
 * @brief Zero-Allocation Lock-Free SPSC Ring Buffer for Shared Memory IPC.
 * Built strictly for High Frequency Trading (HFT).
 * 
 * CORE ARCHITECTURAL RULE (False Sharing Prevention):
 * Every variable written by different threads/processes is strictly padded out to its 
 * own dedicated 64-byte hardware cache line using alignas(). This guarantees absolute 
 * O(1) memory mapping without inter-core cache invalidation storms.
 */
template <typename T, size_t Capacity>
struct SPSCRingBuffer {
    // Capacity implicitly MUST be a Power of 2 for fast modulus. Let's enforce it at compile time.
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of 2");

    // --- PRODUCER CACHE LINE ---
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_idx{0};
    
    // Heartbeat written by producer every 100ms. If stale on restart, it indicates a zombie segment.
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> heartbeat_ts_ns{0};

    // Overflow metric
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> overflow_counter{0};

    // --- CONSUMER CACHE LINE ---
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_idx{0};

    // --- PAYLOAD DATA ---
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer;

    /**
     * @brief O(1) Producer Push (Lock-Free)
     * Pushes a new struct. If full, exhibits Drop-Oldest behavior by forcefully evicting the tail.
     */
    inline void push(const T& item) {
        size_t current_write = write_idx.load(std::memory_order_relaxed);
        size_t current_read = read_idx.load(std::memory_order_acquire);

        // Bitwise AND for ultra-fast Power of 2 modulus
        size_t mask = Capacity - 1;

        if (current_write - current_read >= Capacity) {
            // Buffer Full. Priority: DO NOT BLOCK. Drop oldest.
            // Producer forces the consumer's read cursor instantly forward. 
            // Note: This temporarily breaks SPSC write isolation, but only during critical overflows.
            read_idx.fetch_add(1, std::memory_order_release);
            overflow_counter.fetch_add(1, std::memory_order_relaxed);
        }

        buffer[current_write & mask] = item;
        write_idx.store(current_write + 1, std::memory_order_release);
    }

    /**
     * @brief O(1) Consumer Pop (Lock-Free)
     */
    inline bool pop(T& item) {
        size_t current_read = read_idx.load(std::memory_order_relaxed);
        size_t current_write = write_idx.load(std::memory_order_acquire);

        if (current_write == current_read) {
            return false;
        }

        size_t mask = Capacity - 1;
        item = buffer[current_read & mask];
        read_idx.store(current_read + 1, std::memory_order_release);
        return true;
    }
};

} // namespace alpha::ipc
