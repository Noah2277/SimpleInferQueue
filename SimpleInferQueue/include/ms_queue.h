#pragma once

#include <atomic>
#include <optional>
#include <utility>

// 线程模型：多生产者-多消费者（MPMC）安全，无界队列（可无限入队）。
// 注意：被出队的节点暂不回收内存（规避无锁内存回收难题），仅析构时回收仍在队列中的节点。
//
// T 的约束：只需「可移动构造 + 可移动赋值」，不要求可拷贝或可默认构造
// （节点的 value 是 std::optional<T>，哑节点的槽位为空、不构造 T）。
template <typename T> class MSQueue {
  public:
    MSQueue() {
        // 创建哑节点(dummy node)。哑节点不承载数据，其 value 为空 optional，
        // 因此这里不需要 T 可默认构造
        Node *dummy = new Node();
        // 初始时head和tail都指向哑节点
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    // 析构时假设没有其他线程在并发访问队列
    ~MSQueue() {
        Node *node = head_.load(std::memory_order_relaxed);
        while (node != nullptr) {
            Node *next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    // 禁止拷贝构造和赋值
    MSQueue(const MSQueue &) = delete;
    MSQueue &operator=(const MSQueue &) = delete;

    // 入队（多生产者安全）
    void Enqueue(const T &value) { EnqueueImpl(value); }

    void Enqueue(T &&value) { EnqueueImpl(std::move(value)); }

    // 出队（多消费者安全），成功返回true并将值写入result，失败返回false
    bool Dequeue(T &result) {
        while (true) {
            Node *head = head_.load(std::memory_order_acquire); // 1、读取当前节点头（哑节点）
            Node *tail = tail_.load(std::memory_order_acquire); // 2、读取当前尾节点
            Node *next = head->next.load(std::memory_order_acquire); // 3、读取头节点的下一个节点

            if (next == nullptr) {
                return false; // 4、队列为空（只剩哑节点），出队失败
            }

            if (head == tail) {
                // 5、tail落后了（有人挂了节点但还没推进tail），先帮忙推进tail
                //    （保持"tail不落在head后面"的不变量，这是经典MS算法的必要步骤）
                tail_.compare_exchange_weak(tail, next, std::memory_order_release,
                                            std::memory_order_acquire);
                // 推进完回到循环开头重试
            } else {
                // 6、尝试将head_从当前head推进到next
                if (head_.compare_exchange_weak(head, next, std::memory_order_release,
                                                std::memory_order_acquire)) {
                    // 7、成功推进，取出值。
                    //    用 move 而非拷贝：move-only 类型的唯一选择；对可拷贝类型也少一次拷贝。
                    //    注意：赋值若抛异常，元素已离开队列却未交付（详见下方 ABA 警告后的说明）
                    result = std::move(*next->value);
                    // ⚠️ 这里不释放旧 head 节点。这个「不回收」既是性能取舍，也是当前
                    //    实现没有 ABA 问题的唯一原因：节点永不归还给分配器，CAS 比较的指针
                    //    就不可能「被复用后指向另一个对象」。
                    //    若将来引入任何形式的内存回收（hazard pointer / epoch / free list），
                    //    必须同时引入带标记的指针（tagged pointer）或 hazard 保护，
                    //    否则 ABA 会立刻以 use-after-free 的形式出现。
                    return true;
                }
                // 8、CAS失败（被其他消费者抢先），回到开头重试
            }
        }
    }

    // 判断队列是否为空
    bool Empty() const {
        Node *head = head_.load(std::memory_order_acquire);
        return head->next.load(std::memory_order_acquire) == nullptr;
    }

  private:
    // 链表节点
    struct Node {
        std::optional<T> value;
        std::atomic<Node *> next;

        // 哑节点专用：不构造 T
        Node() : value(std::nullopt), next(nullptr) {}

        explicit Node(const T &v) : value(v), next(nullptr) {}

        explicit Node(T &&v) : value(std::move(v)), next(nullptr) {}
    };

    template <typename U> void EnqueueImpl(U &&value) {
        Node *node = new Node(std::forward<U>(value)); // 1、创建新节点

        while (true) {
            Node *tail = tail_.load(std::memory_order_acquire); // 2、读取当前尾节点
            Node *next = tail->next.load(std::memory_order_acquire); // 3、读取尾节点的下一个节点

            if (next == nullptr) {
                // 4、尾节点后面是空的，尝试把新节点挂上去
                Node *expected = nullptr;
                if (tail->next.compare_exchange_weak(expected, node, std::memory_order_release,
                                                     std::memory_order_acquire)) {
                    // 5、挂载成功！尝试推进tail_指向新节点（失败也没关系，别人会帮忙）
                    tail_.compare_exchange_weak(tail, node, std::memory_order_release,
                                                std::memory_order_acquire);
                    return; // 入队成功
                }
                // 6、挂载失败（被别人抢先了），回到循环开头重试
            } else {
                // 7、tail不是真正的尾节点（有人挂了节点但还没更新tail），帮忙推进tail
                tail_.compare_exchange_weak(tail, next, std::memory_order_release,
                                            std::memory_order_acquire);
                // 8、继续循环，读取新的tail
            }
        }
    }

    std::atomic<Node *> head_; // 指向队列头的指针（哑节点）
    std::atomic<Node *> tail_; // 指向队列尾的指针（最后一个节点）
};
