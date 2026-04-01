#pragma once

#include <atomic>

namespace alpha::concurrency {

/**
 * @brief Ultra-low latency CAS-based spinlock.
 * NO context switching or kernel calls in the hot path.
 */
class SpinLock {
public:
    SpinLock() = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // Busy wait - we don't want to yield the CPU core in HFT.
            // In a real HFT engine, we might use __builtin_ia32_pause() here.
#if defined(__x86_64__) || defined(_M_X64)
            __asm__ __volatile__("pause" ::: "memory");
#endif
        }
    }

    bool try_lock() {
        return !flag_.test_and_set(std::memory_order_acquire);
    }

    void unlock() {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

} // namespace alpha::concurrency
