#include <gtest/gtest.h>
#include "AsyncQueue.h"
#include <string>
#include <thread>
#include <atomic>
#include <vector>

TEST(AsyncQueue, PushPop_FIFO_Order) {
    AsyncQueue q;
    q.Push("first");
    q.Push("second");
    q.Push("third");

    std::string out;
    ASSERT_TRUE(q.Pop(out, 200));  EXPECT_EQ(out, "first");
    ASSERT_TRUE(q.Pop(out, 200));  EXPECT_EQ(out, "second");
    ASSERT_TRUE(q.Pop(out, 200));  EXPECT_EQ(out, "third");
}

TEST(AsyncQueue, Pop_EmptyQueue_Timeout) {
    AsyncQueue q;
    std::string out;
    // Queue is empty — Pop must return false within the timeout window
    EXPECT_FALSE(q.Pop(out, 50));
}

TEST(AsyncQueue, Shutdown_PopReturnsFalse) {
    AsyncQueue q;
    q.Shutdown();
    EXPECT_TRUE(q.IsShutdown());

    std::string out;
    EXPECT_FALSE(q.Pop(out, 200));
}

TEST(AsyncQueue, MaxSize_DropsOldest) {
    AsyncQueue q;
    // Fill to capacity then push one extra — the oldest item must be dropped
    for (size_t i = 0; i <= AsyncQueue::kMaxSize; ++i)
        q.Push(std::to_string(i));

    // "0" was evicted; "1" should be the first item available
    std::string out;
    ASSERT_TRUE(q.Pop(out, 200));
    EXPECT_EQ(out, "1");
}

TEST(AsyncQueue, ConcurrentPushPop_AllItemsDelivered) {
    AsyncQueue q;
    constexpr int kItems = 200;
    std::atomic<int> received{0};

    std::thread producer([&]() {
        for (int i = 0; i < kItems; ++i)
            q.Push(std::to_string(i));
    });

    std::thread consumer([&]() {
        std::string out;
        while (received.load(std::memory_order_relaxed) < kItems) {
            if (q.Pop(out, 500))
                received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(received.load(), kItems);
}
