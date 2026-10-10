#pragma once

#include <atomic>
#include <optional>
#include <utility>
#include <vector>

// 线程模型：仅单生产者-单消费者（SPSC）安全。
// 多生产者/多消费者场景请使用 MSQueue（见 ms_queue.h）。
//
// 实现方式：单写者/单读者，靠 capacity+1 的哨兵位区分满/空，用 load/store +
// acquire-release 配对保证可见性。这里**没有 compare_exchange**——因为 SPSC 下
// head_ 只有消费者写、tail_ 只有生产者写，不存在竞争写者，不需要 CAS。
// 不要把它当成可扩展的多生产者无锁队列。
//
// T 的约束：只需「可移动构造 + 可移动赋值」，不要求可拷贝或可默认构造
// （槽位是 std::optional<T>，空槽不构造 T）。
template <typename T> class LockFreeQueue {
  public:
    explicit LockFreeQueue(size_t capacity)
        : buffer_(capacity + 1), head_(0), tail_(0), capacity_(capacity + 1) {}

    bool Enqueue(const T &item) { return EnqueueImpl(item); }

    bool Enqueue(T &&item) { return EnqueueImpl(std::move(item)); }

    bool Dequeue(T &item) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        item = std::move(*buffer_[head]);
        // 先析构槽位里的值，再把 head_ 发布出去：必须在 release store 之前完成，
        // 否则生产者可能在消费者还在读这块槽位时就把新值写进来
        buffer_[head].reset();
        head_.store((head + 1) % capacity_, std::memory_order_release);
        return true;
    }

    bool Empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    bool Full() const {
        size_t next = (tail_.load(std::memory_order_acquire) + 1) % capacity_;
        return next == head_.load(std::memory_order_acquire);
    }

  private:
    template <typename U> bool EnqueueImpl(U &&item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % capacity_;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[tail].emplace(std::forward<U>(item));
        tail_.store(next, std::memory_order_release);
        return true;
    }

    std::vector<std::optional<T>> buffer_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
    size_t capacity_;
};
