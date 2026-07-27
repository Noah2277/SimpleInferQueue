#pragma once

#include <atomic>
#include <vector>

template <typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity):
        buffer_(capacity+1), head_(0), tail_(0), capacity_(capacity+1) {}

    bool Enqueue(const T& item){
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % capacity_;
        if(next == head_.load(std::memory_order_acquire)){
            return false;
        }
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }
    bool Dequeue(T& item){
        size_t head = head_.load(std::memory_order_relaxed);
        if(head == tail_.load(std::memory_order_acquire)){
            return false;
        }
        item = buffer_[head];
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
    std::vector<T> buffer_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
    size_t capacity_;


};