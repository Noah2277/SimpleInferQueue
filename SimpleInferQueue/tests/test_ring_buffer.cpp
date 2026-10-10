#include <gtest/gtest.h>

#include <memory>

#include "ring_buffer.h"

// 基础 FIFO：先入先出，元素顺序不变
TEST(RingBufferTest, PushPopFifo) {
    RingBuffer<int> buf(4);
    ASSERT_TRUE(buf.Push(1));
    ASSERT_TRUE(buf.Push(2));
    ASSERT_TRUE(buf.Push(3));

    int v = 0;
    ASSERT_TRUE(buf.Pop(v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(buf.Pop(v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(buf.Pop(v));
    EXPECT_EQ(v, 3);
}

// 满/空边界：容量耗尽时 Push 失败，取空时 Pop 失败
TEST(RingBufferTest, FullAndEmptyBoundary) {
    RingBuffer<int> buf(2);
    EXPECT_TRUE(buf.Empty());

    EXPECT_TRUE(buf.Push(1));
    EXPECT_TRUE(buf.Push(2));
    EXPECT_TRUE(buf.Full());
    EXPECT_FALSE(buf.Push(3)); // 已满，插入失败

    int v = 0;
    EXPECT_TRUE(buf.Pop(v));
    EXPECT_FALSE(buf.Full());
    EXPECT_TRUE(buf.Pop(v));
    EXPECT_TRUE(buf.Empty());
    EXPECT_FALSE(buf.Pop(v)); // 已空，取出失败
}

// Size 计数随 Push/Pop 增减
TEST(RingBufferTest, SizeCounting) {
    RingBuffer<int> buf(4);
    EXPECT_EQ(buf.Size(), 0u);

    buf.Push(1);
    buf.Push(2);
    EXPECT_EQ(buf.Size(), 2u);

    int v = 0;
    buf.Pop(v);
    EXPECT_EQ(buf.Size(), 1u);
}

// 环形回绕：head/tail 越过数组末尾回到开头，数据依然正确
TEST(RingBufferTest, WrapAround) {
    RingBuffer<int> buf(3);
    int v = 0;

    // 反复 Push/Pop 迫使指针多次回绕
    for (int round = 0; round < 10; ++round) {
        ASSERT_TRUE(buf.Push(round));
        ASSERT_TRUE(buf.Pop(v));
        EXPECT_EQ(v, round);
    }
    EXPECT_TRUE(buf.Empty());

    // 回绕后 FIFO 顺序仍然保持
    ASSERT_TRUE(buf.Push(1));
    ASSERT_TRUE(buf.Push(2));
    ASSERT_TRUE(buf.Push(3));
    EXPECT_TRUE(buf.Full());
    for (int expected : {1, 2, 3}) {
        ASSERT_TRUE(buf.Pop(v));
        EXPECT_EQ(v, expected);
    }
}

// move-only 类型（不可拷贝、不可默认构造）也能装
// 槽位是 std::optional<T>，空槽不构造 T，因此不再要求 T 可默认构造
TEST(RingBufferTest, SupportsMoveOnlyPayload) {
    RingBuffer<std::unique_ptr<int>> buf(2);

    EXPECT_TRUE(buf.Push(std::make_unique<int>(1)));
    EXPECT_TRUE(buf.Push(std::make_unique<int>(2)));
    EXPECT_FALSE(buf.Push(std::make_unique<int>(3))); // 已满

    std::unique_ptr<int> out;
    ASSERT_TRUE(buf.Pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 1);

    ASSERT_TRUE(buf.Pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 2);

    EXPECT_FALSE(buf.Pop(out)); // 已空
}

// 空槽在 Pop 之后必须被真正重置，而不是留一份副本在里面
// （槽位复用若忘了 reset，取走的对象的生命周期会被队列莫名延长）
TEST(RingBufferTest, SlotReleasedAfterPop) {
    RingBuffer<std::shared_ptr<int>> buf(2);
    std::weak_ptr<int> weak;

    auto owner = std::make_shared<int>(42);
    weak = owner;
    ASSERT_TRUE(buf.Push(std::move(owner)));
    EXPECT_FALSE(weak.expired()); // 队列持有一份

    std::shared_ptr<int> out;
    ASSERT_TRUE(buf.Pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 42);

    out.reset();
    // 槽位若没被 reset，队列里还留着一份 shared_ptr，weak 就不会过期
    EXPECT_TRUE(weak.expired());
}
