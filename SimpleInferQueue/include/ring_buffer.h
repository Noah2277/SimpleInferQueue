#pragma once
#include <vector>

template <typename T>
class RingBuffer {
public:
    //构造函数：传入容量
    explicit RingBuffer(size_t capacity)
        : buffer(capacity), head(0), tail(0), count(0), capacity(capacity) {}

    bool Push(const T& item){
        if(count == capacity){
            return false;
        }
        buffer[tail] = item;
        tail = (tail + 1) % capacity;
        count++;
        return true;
    }

    bool Pop(T& item){
        if(count == 0){
            return false;
        }
        item = buffer[head];
        head = (head + 1) % capacity;
        count--;
        return true;
    }

    bool Full() const {
        return count == capacity;
    }

    bool Empty() const {
        return count == 0;
    }

    size_t Size() const {
        return count;
    }

private:
    std::vector<T> buffer;
    size_t head;
    size_t tail;
    size_t count;
    size_t capacity;

};