#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "block_queue.h"

// 满/空时 try 接口不阻塞、立即返回失败（迁移自原 examples/test_try.cpp）
TEST(BlockQueueTest, TryPushPopBounds) {
    BlockQueue<int> queue(3);

    // 连续 try_Push 三次，都应成功
    EXPECT_TRUE(queue.try_Push(1));
    EXPECT_TRUE(queue.try_Push(2));
    EXPECT_TRUE(queue.try_Push(3));

    // 第四次：队列已满，应失败
    EXPECT_FALSE(queue.try_Push(4));

    // 连续 try_Pop 三次，都应成功且顺序正确
    int value = 0;
    EXPECT_TRUE(queue.try_Pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.try_Pop(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.try_Pop(value));
    EXPECT_EQ(value, 3);

    // 第四次：队列已空，应失败
    EXPECT_FALSE(queue.try_Pop(value));
}

// 阻塞 Push/Pop 在多线程下不丢失、不重复元素
TEST(BlockQueueTest, MultithreadedPushPop) {
    constexpr int kTotal = 10000;
    BlockQueue<int> queue(64);

    std::atomic<int> sum{0};
    std::thread producer([&] {
        for (int i = 1; i <= kTotal; ++i) {
            queue.Push(i);
        }
    });
    std::thread consumer([&] {
        for (int i = 0; i < kTotal; ++i) {
            sum.fetch_add(queue.Pop());
        }
    });

    producer.join();
    consumer.join();

    // 1..kTotal 的和，说明每个元素恰好被取到一次
    EXPECT_EQ(sum.load(), kTotal * (kTotal + 1) / 2);
}
