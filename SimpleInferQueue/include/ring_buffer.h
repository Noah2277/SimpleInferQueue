#pragma once
#include <optional>
#include <utility>
#include <vector>

// 线程模型：非线程安全，仅限单线程使用。
// 多线程场景请使用 BlockQueue（有锁）或 LockFreeQueue / MSQueue（无锁）。
//
// T 的约束：只需「可移动构造 + 可移动赋值」。不要求可拷贝，也不要求可默认构造
// ——槽位用 std::optional<T> 存储，空槽不构造 T，因此 unique_ptr 这类 move-only
// 任务也能装进来。
//
// 异常安全契约（重要，改动前务必读）：
// Push/Pop 一律「先完成元素的构造/赋值，再改 head/tail/count」。这个顺序保证了
// 赋值抛异常时队列状态不变，因此上层的 BlockQueue 不通知也不会留下丢失唤醒。
// 千万不要把索引更新挪到赋值之前——那样一旦抛异常就会出现「计数变了但元素没进去」，
// BlockQueue 会永久丢失唤醒而死锁。
template <typename T> class RingBuffer {
  public:
    // 构造函数：传入容量
    explicit RingBuffer(size_t capacity)
        : buffer(capacity), head(0), tail(0), count(0), capacity(capacity) {}

    bool Push(const T &item) { return PushImpl(item); }

    bool Push(T &&item) { return PushImpl(std::move(item)); }

    bool Pop(T &item) {
        if (count == 0) {
            return false;
        }
        item = std::move(*buffer[head]);
        buffer[head].reset();
        head = (head + 1) % capacity;
        count--;
        return true;
    }

    bool Full() const { return count == capacity; }

    bool Empty() const { return count == 0; }

    size_t Size() const { return count; }

  private:
    template <typename U> bool PushImpl(U &&item) {
        if (count == capacity) {
            return false;
        }
        buffer[tail].emplace(std::forward<U>(item));
        tail = (tail + 1) % capacity;
        count++;
        return true;
    }

    std::vector<std::optional<T>> buffer;
    size_t head;
    size_t tail;
    size_t count;
    size_t capacity;
};
