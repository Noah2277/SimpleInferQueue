#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "lockfree_queue.h"

// 容量边界：容量为 N 时最多入队 N 个元素（迁移自原 examples/test_lockfree.cpp）
TEST(LockFreeQueueTest, CapacityBound) {
    LockFreeQueue<int> queue(5);

    for (int i = 1; i <= 5; ++i) {
        EXPECT_TRUE(queue.Enqueue(i));
    }
    EXPECT_TRUE(queue.Full());
    EXPECT_FALSE(queue.Enqueue(6)); // 已满，入队失败

    int val = 0;
    for (int expected = 1; expected <= 5; ++expected) {
        EXPECT_TRUE(queue.Dequeue(val));
        EXPECT_EQ(val, expected);
    }
    EXPECT_TRUE(queue.Empty());
    EXPECT_FALSE(queue.Dequeue(val)); // 已空，出队失败
}

// SPSC 并发：单生产者单消费者，10000 个元素严格按 FIFO 顺序出队
TEST(LockFreeQueueTest, SingleProducerSingleConsumerFifo) {
    constexpr int kN = 10000;
    LockFreeQueue<int> queue(1000);

    std::atomic<bool> producer_done{false};
    std::atomic<int> error_count{0};

    std::thread producer([&] {
        for (int i = 0; i < kN; ++i) {
            while (!queue.Enqueue(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true);
    });

    std::thread consumer([&] {
        int expected = 0;
        while (expected < kN) {
            int val = 0;
            if (queue.Dequeue(val)) {
                if (val != expected) {
                    error_count.fetch_add(1);
                }
                ++expected;
            } else if (!producer_done.load()) {
                // 队列暂时为空且生产者未结束，让出 CPU 再试
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(error_count.load(), 0);
}
