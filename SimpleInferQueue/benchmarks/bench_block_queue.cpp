// BlockQueue 基准：互斥锁 + 条件变量实现（MPMC 安全）
//
// 线程分工：Google Benchmark 用 Threads(2) 同时启动 2 个线程执行同一段基准代码，
// 通过 state.thread_index() 区分角色——线程 0 生产、线程 1 消费。
#include "block_queue.h"
#include <benchmark/benchmark.h>

#include <thread>

static void BM_BlockQueue_SPSC(benchmark::State &state) {
    static BlockQueue<int> q(4096); // static：跨迭代复用同一个队列，避免反复重建

    if (state.thread_index() == 0) {
        // 生产者：队列满时自旋让出
        for (auto _ : state) {
            while (!q.try_Push(1)) {
                std::this_thread::yield();
            }
        }
    } else {
        // 消费者：队列空时自旋让出
        for (auto _ : state) {
            int v;
            while (!q.try_Pop(v)) {
                std::this_thread::yield();
            }
        }
    }
}
// UseRealTime：基准里有 yield 等待（消耗墙钟时间而非 CPU 时间），
// 默认的 CPU 计时会把这段等待漏掉、数字虚高，必须切到墙钟。
BENCHMARK(BM_BlockQueue_SPSC)->Threads(2)->UseRealTime();

// 整个程序只允许一个 BENCHMARK_MAIN，放在本文件，其余基准文件不要写
BENCHMARK_MAIN();
