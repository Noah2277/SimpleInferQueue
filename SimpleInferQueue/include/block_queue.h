#pragma once

#include "ring_buffer.h"
#include <condition_variable>
#include <mutex>

// 线程模型：多生产者-多消费者（MPMC）安全。
// Push/Pop 在队列满/空时阻塞等待；try_Push/try_Pop 非阻塞，立即返回成败。
template <typename T> class BlockQueue {
  public:
    BlockQueue(size_t max_size = 100) : buffer_(max_size) {}

    void Push(const T &item) {
        std::unique_lock<std::mutex> lock(mtx);
        cv_not_full_.wait(lock, [this]() { return !buffer_.Full(); });
        buffer_.Push(item);
        cv_not_empty_.notify_one();
    }

    T Pop() {
        std::unique_lock<std::mutex> lock(mtx);
        cv_not_empty_.wait(lock, [this]() { return !buffer_.Empty(); });
        T item;
        buffer_.Pop(item);
        cv_not_full_.notify_all();
        return item;
    }

    // 非阻塞尝试
    bool try_Push(const T &item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!buffer_.Full()) {
            buffer_.Push(item);
            cv_not_empty_.notify_all();
            return true;
        }
        return false;
    }

    bool try_Pop(T &item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (buffer_.Pop(item)) {
            cv_not_full_.notify_all();
            return true;
        }
        return false;
    }

  private:
    RingBuffer<T> buffer_;
    std::mutex mtx;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
};