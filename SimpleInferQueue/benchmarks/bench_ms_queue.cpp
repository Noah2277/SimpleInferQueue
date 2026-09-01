// MSQueue 基准：Michael-Scott 无锁队列（MPMC 安全，无界）
//
// 线程分工：Threads(8)，thread_index < 4 的线程生产，其余线程消费。
#include "ms_queue.h"
#include <benchmark/benchmark.h>

#include <thread>

static void BM_MSQueue_MPMC_4P4C(benchmark::State &state) {
    static MSQueue<int> q;

    if (state.thread_index() < 4) {
        // 生产者：无界队列永不阻塞
        for (auto _ : state) {
            q.Enqueue(1);
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
// Iterations 固定迭代数：MSQueue 被出队的节点不回收内存（见 ms_queue.h 契约），
// 迭代数太多内存会持续上涨，必须设上限；UseRealTime：消费者有 yield 等待。
BENCHMARK(BM_MSQueue_MPMC_4P4C)->Threads(8)->Iterations(1000000)->UseRealTime();
