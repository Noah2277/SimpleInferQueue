#pragma once

#include "ring_buffer.h"
#include <condition_variable>
#include <mutex>
#include <utility>

// 线程模型：多生产者-多消费者（MPMC）安全。
// Push/Pop 在队列满/空时阻塞等待；try_Push/try_Pop 非阻塞，立即返回成败。
//
// T 的约束：只需「可移动构造 + 可移动赋值」，不要求可拷贝或可默认构造。
//
// 关闭语义（Close）：
//   生产结束后调用 Close()，阻塞中的 Push/Pop 会被唤醒并返回 false，消费者得以退出循环。
//   关闭后：Push/try_Push 一律返回 false；Pop/try_Pop 会把队列里剩余的元素取完，取空后才返回
//   false。 也就是说 Pop 返回 false **当且仅当「已关闭且已取空」**，调用方据此就能区分
//   「暂时没数据」（阻塞中，不会返回）与「再也不会有数据了」。
//
//   用法：
//       // 生产者全部结束后
//       queue.Close();
//       for (auto &t : consumers) t.join();
template <typename T> class BlockQueue {
  public:
    BlockQueue(size_t max_size = 100) : buffer_(max_size) {}

    // 析构时唤醒可能仍在等待的线程。注意这**不能替代**正确用法：
    // 标准前置条件仍然要求调用方在销毁队列之前先 join 掉所有线程。
    // 若真有线程在析构进行中才被唤醒，它访问的是正在销毁的对象——那已经是 UB，
    // 这里的 Close() 只是给「忘了 Close 就析构」的情形留一条退路。
    ~BlockQueue() { Close(); }

    // 返回 false 表示队列已关闭，元素没有被放入
    bool Push(const T &item) {
        std::unique_lock<std::mutex> lock(mtx);
        cv_not_full_.wait(lock, [this]() { return closed_ || !buffer_.Full(); });
        if (closed_) {
            return false;
        }
        buffer_.Push(item);
        cv_not_empty_.notify_one();
        return true;
    }

    bool Push(T &&item) {
        std::unique_lock<std::mutex> lock(mtx);
        cv_not_full_.wait(lock, [this]() { return closed_ || !buffer_.Full(); });
        if (closed_) {
            return false;
        }
        buffer_.Push(std::move(item));
        cv_not_empty_.notify_one();
        return true;
    }

    // 返回 false 表示「已关闭且已取空」（见文件头的关闭语义说明）
    bool Pop(T &item) {
        std::unique_lock<std::mutex> lock(mtx);
        cv_not_empty_.wait(lock, [this]() { return closed_ || !buffer_.Empty(); });
        if (!buffer_.Pop(item)) {
            return false; // 走到这里只可能是 closed_ && Empty()
        }
        cv_not_full_.notify_one();
        return true;
    }

    // 非阻塞尝试
    bool try_Push(const T &item) { return try_PushImpl(item); }

    bool try_Push(T &&item) { return try_PushImpl(std::move(item)); }

    bool try_Pop(T &item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (buffer_.Pop(item)) {
            cv_not_full_.notify_one();
            return true;
        }
        return false;
    }

    // 关闭队列：唤醒所有等待者，此后不再接受新元素
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            closed_ = true;
        }
        // close 是唯一需要 notify_all 的路径：它改变了所有等待者都在等的那个条件，
        // 而 Push/Pop/try_* 每次只改变一个元素的占用状态，notify_one 即可，用 all 会造成惊群
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    bool Closed() const {
        std::lock_guard<std::mutex> lock(mtx);
        return closed_;
    }

  private:
    template <typename U> bool try_PushImpl(U &&item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!closed_ && !buffer_.Full()) {
            buffer_.Push(std::forward<U>(item));
            cv_not_empty_.notify_one();
            return true;
        }
        return false;
    }

    RingBuffer<T> buffer_;
    mutable std::mutex mtx;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    bool closed_ = false;
};
