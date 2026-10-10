#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

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
        int value = 0;
        int taken = 0;
        while (taken < kTotal && queue.Pop(value)) {
            sum.fetch_add(value);
            ++taken;
        }
    });

    producer.join();
    consumer.join();

    // 1..kTotal 的和，说明每个元素恰好被取到一次
    EXPECT_EQ(sum.load(), kTotal * (kTotal + 1) / 2);
}

// Close 能唤醒阻塞中的消费者
// 这是「没有关闭语义就会永久阻塞」那条结论的回归测试：删掉 Close 的实现，这个用例会挂死
TEST(BlockQueueTest, CloseWakesBlockedConsumer) {
    BlockQueue<int> queue(8);
    std::atomic<bool> consumer_exited{false};

    std::thread consumer([&] {
        int value = 0;
        // 队列自始至终没有元素，Pop 会一直阻塞在这里；
        // 能出来只可能是因为 Close() 把谓词里的 closed_ 打开了
        while (queue.Pop(value)) {
        }
        consumer_exited.store(true);
    });

    // 给消费者一点时间真正进入阻塞状态
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(consumer_exited.load()); // 此刻它必须还卡在 Pop 上，而不是走到后面去了

    queue.Close();
    consumer.join(); // 没有 Close 的话，这一行会永远不返回
    EXPECT_TRUE(consumer_exited.load());
}

// 关闭后，队列里剩余的元素仍要能取完，取空之后才返回 false
TEST(BlockQueueTest, PopDrainsRemainingAfterClose) {
    BlockQueue<int> queue(8);
    EXPECT_TRUE(queue.Push(1));
    EXPECT_TRUE(queue.Push(2));
    queue.Close();

    int value = 0;
    EXPECT_TRUE(queue.Pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.Pop(value));
    EXPECT_EQ(value, 2);
    EXPECT_FALSE(queue.Pop(value)); // 已关闭且已取空
}

// 关闭后不再接受新元素
TEST(BlockQueueTest, PushRejectedAfterClose) {
    BlockQueue<int> queue(8);
    EXPECT_FALSE(queue.Closed());
    queue.Close();
    EXPECT_TRUE(queue.Closed());

    EXPECT_FALSE(queue.Push(1));
    EXPECT_FALSE(queue.try_Push(1));
}

// move-only 载荷（持 unique_ptr 的推理请求等）现在可以入队
TEST(BlockQueueTest, SupportsMoveOnlyPayload) {
    BlockQueue<std::unique_ptr<int>> queue(8);
    EXPECT_TRUE(queue.Push(std::make_unique<int>(7)));

    std::unique_ptr<int> out;
    ASSERT_TRUE(queue.Pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
}
