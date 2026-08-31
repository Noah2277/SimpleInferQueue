#include "lockfree_queue.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    constexpr int N = 1000000;
    LockFreeQueue<int> q(4096);

    std::atomic<bool> producer_done(false);
    std::atomic<int> error_count(0);
    auto start = std::chrono::high_resolution_clock::now();

    // 生产者线程
    std::thread producer([&]() {
        for (int i = 0; i < N; i++) {
            while (!q.Enqueue(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true);
    });

    // 消费者线程
    std::thread consumer([&]() {
        int expected = 0;
        while (expected < N) {
            int val;
            if (q.Dequeue(val)) {
                if (val != expected) {
                    std::cout << "错误：期望 " << expected << ", 实际 " << val << std::endl;
                    error_count.fetch_add(1);
                }
                expected++;
            } else {
                // 队列空，如果有生产者还没结束，稍等再试
                if (!producer_done.load()) {
                    std::this_thread::yield();
                }
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Lockfree time: " << duration << " ms" << std::endl;
    std::cout << "Throughput: " << (N * 1000.0 / duration) << " ops/s" << std::endl;

    return 0;
}
