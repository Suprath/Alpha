#include <gtest/gtest.h>
#include <alpha/time/Timestamp.hpp>
#include <alpha/concurrency/SpinLock.hpp>

/**
 * @brief Test standard timestamping in Alpha.
 */
TEST(IngesterTest, TimestampMonotonic) {
    auto t1 = alpha::time::Timestamp::now_ns();
    auto t2 = alpha::time::Timestamp::now_ns();
    EXPECT_GE(t2, t1);
}

/**
 * @brief Test basic spinlock functionality.
 */
TEST(IngesterTest, SpinLockFunctional) {
    alpha::concurrency::SpinLock lock;
    EXPECT_TRUE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock());
    lock.unlock();
    EXPECT_TRUE(lock.try_lock());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
