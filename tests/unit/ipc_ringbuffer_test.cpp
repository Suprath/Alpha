#include <gtest/gtest.h>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>
#include <thread>
#include <vector>
#include <cstddef>

using namespace alpha::ipc;
using namespace alpha::models;

/**
 * @brief Initialize the Ring Buffer globally to test multi-threaded access without heap overhead.
 */
SPSCRingBuffer<Tick, 1024> test_buffer;

/**
 * @brief Test basic SPSC push and pop logic
 */
TEST(RingBufferTest, BasicPushPop) {
    Tick t;
    t.instrument_token = 12345;
    t.last_price = 100.5;

    test_buffer.push(t);

    Tick popped;
    EXPECT_TRUE(test_buffer.pop(popped));
    EXPECT_EQ(popped.instrument_token, 12345);
    EXPECT_DOUBLE_EQ(popped.last_price, 100.5);

    // Buffer should now be empty
    EXPECT_FALSE(test_buffer.pop(popped));
}

/**
 * @brief Test Drop-Oldest logic
 */
TEST(RingBufferTest, DropOldestOverflow) {
    SPSCRingBuffer<uint32_t, 4> drop_buffer;
    
    // Push 5 items into a capacity 4 buffer
    drop_buffer.push(1);
    drop_buffer.push(2);
    drop_buffer.push(3);
    drop_buffer.push(4);
    drop_buffer.push(5); // This should drop 1
    
    EXPECT_EQ(drop_buffer.overflow_counter.load(), 1);
    
    uint32_t val;
    EXPECT_TRUE(drop_buffer.pop(val));
    EXPECT_EQ(val, 2); // 1 was dropped, tail moved to 2
    EXPECT_TRUE(drop_buffer.pop(val));
    EXPECT_EQ(val, 3);
}

/**
 * @brief Verify Memory Alignment guarantees false-sharing prevention
 */
TEST(RingBufferTest, MemoryAlignment) {
    using MyBuffer = SPSCRingBuffer<Tick, 1024>;
    
    EXPECT_EQ(offsetof(MyBuffer, write_idx) % CACHE_LINE_SIZE, 0);
    EXPECT_EQ(offsetof(MyBuffer, read_idx) % CACHE_LINE_SIZE, 0);
    
    size_t write_offset = offsetof(MyBuffer, write_idx);
    size_t read_offset = offsetof(MyBuffer, read_idx);
    
    EXPECT_GE(std::abs(static_cast<std::ptrdiff_t>(write_offset - read_offset)), CACHE_LINE_SIZE);
}

/**
 * @brief Multi-threaded stress test simulating Producer -> Consumer
 */
TEST(RingBufferTest, MultiThreadedStress) {
    SPSCRingBuffer<uint32_t, 1024> stress_buffer;
    
    std::atomic<uint32_t> numbers_popped{0};
    std::atomic<bool> producer_done{false};
    
    std::thread producer([&]() {
        for (uint32_t i = 1; i <= 20000; i++) {
            stress_buffer.push(i);
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        uint32_t val;
        while (true) {
            bool has_val = stress_buffer.pop(val);
            if (has_val) {
                numbers_popped++;
            } else {
                if (producer_done.load(std::memory_order_acquire)) {
                    // Double-check pop in case a push sneaked in before done flag propagating
                    if (!stress_buffer.pop(val)) {
                        break;
                    }
                    numbers_popped++;
                }
            }
        }
    });

    producer.join();
    consumer.join();

    // Since Drop-Oldest allows the producer to skip past the consumer, 
    // we just guarantee we didn't deadlock, and we processed SOME elements.
    EXPECT_GT(numbers_popped.load(), 0);
    EXPECT_LE(numbers_popped.load(), 20000);
}
