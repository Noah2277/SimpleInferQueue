// LockFreeQueue 基准：CAS 无锁队列（仅 SPSC 安全，见 lockfree_queue.h 的线程模型契约）
//
// 线程分工：Threads(2)，线程 0 生产、线程 1 消费。
#include "lockfree_queue.h"
#include <benchmark/benchmark.h>

#include <thread>

static void BM_LockFreeQueue_SPSC(benchmark::State &state) {
    static LockFreeQueue<int> q(4096); // 构造函数内部会 +1 作为哨兵位

    if (state.thread_index() == 0) {
        // 生产者：队列满时自旋让出
        for (auto _ : state) {
            while (!q.Enqueue(1)) {
                std::this_thread::yield();
            }
        }
    } else {
        // 消费者：队列空时自旋让出
        for (auto _ : state) {
            int v;
            while (!q.Dequeue(v)) {
                std::this_thread::yield();
            }
        }
    }
}
BENCHMARK(BM_LockFreeQueue_SPSC)->Threads(2)->UseRealTime();
