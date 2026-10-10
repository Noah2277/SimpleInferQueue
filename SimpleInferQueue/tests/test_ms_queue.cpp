#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "ms_queue.h"

// 基础 FIFO：单线程入队出队顺序保持
TEST(MSQueueTest, BasicFifo) {
    MSQueue<int> q;
    for (int i = 1; i <= 5; ++i) {
        q.Enqueue(i);
    }
    EXPECT_FALSE(q.Empty());

    int v = 0;
    for (int expected = 1; expected <= 5; ++expected) {
        ASSERT_TRUE(q.Dequeue(v));
        EXPECT_EQ(v, expected);
    }
    EXPECT_TRUE(q.Empty());
    EXPECT_FALSE(q.Dequeue(v)); // 队列已空，出队失败
}

// MPCP 并发：4 生产者 × 4 消费者，4000 个元素不重不漏（迁移自原 examples/test_ms_queue.cpp）
TEST(MSQueueTest, MultiProducerMultiConsumer) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kItemsPerProducer = 1000;
    constexpr int kTotal = kProducers * kItemsPerProducer;

    MSQueue<int> q;
    std::atomic<int> produced_count{0};
    std::atomic<int> consumed_count{0};

    // 用于验证不重复、不遗漏
    std::mutex mtx;
    std::set<int> consumed_values;

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                q.Enqueue(p * kItemsPerProducer + i); // 每个生产者生成独立区间的值
                produced_count.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (consumed_count.load() < kTotal) {
                int val = 0;
                if (q.Dequeue(val)) {
                    std::lock_guard<std::mutex> lock(mtx);
                    consumed_values.insert(val);
                    consumed_count.fetch_add(1);
                } else {
                    // 队列暂时为空（生产者可能还没生产完），让出 CPU 再试
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto &t : producers) {
        t.join();
    }
    for (auto &t : consumers) {
        t.join();
    }

    // 不重不漏：元素总数正确，且覆盖 0..kTotal-1 的全部值
    EXPECT_EQ(consumed_values.size(), static_cast<size_t>(kTotal));
    for (int i = 0; i < kTotal; ++i) {
        EXPECT_NE(consumed_values.find(i), consumed_values.end());
    }
}

// move-only 类型（不可拷贝、不可默认构造）也能装
// 哑节点现在用空 optional 构造，不再需要 T{}
TEST(MSQueueTest, SupportsMoveOnlyPayload) {
    MSQueue<std::unique_ptr<int>> q;
    q.Enqueue(std::make_unique<int>(1));
    q.Enqueue(std::make_unique<int>(2));

    std::unique_ptr<int> out;
    ASSERT_TRUE(q.Dequeue(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 1);

    ASSERT_TRUE(q.Dequeue(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 2);

    EXPECT_TRUE(q.Empty());
    EXPECT_FALSE(q.Dequeue(out)); // 队列已空
}
