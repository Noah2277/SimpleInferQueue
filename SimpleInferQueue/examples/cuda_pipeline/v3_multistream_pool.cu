#include <stdio.h>
#include <cuda_runtime.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include "ms_queue.h"
#include "block_queue.h"

// 设备缓冲区块
struct DeviceBufferBlock {
    float* d_A;
    float* d_B;
    float* d_C;
    size_t bytes;
    cudaEvent_t done_event;
};

// 任务结构体
struct InferenceTask {
    int task_id;
    DeviceBufferBlock* block;
    float* h_output;
    int length;
};

// CUDA 核函数
__global__ void vectorAdd(const float* A, const float* B, float* C, int N) {
    int i = blockDim.x * blockIdx.x + threadIdx.x;
    if (i < N) {
        C[i] = A[i] + B[i];
    }
}

int main() {
    constexpr int POOL_SIZE = 4;
    constexpr int NUM_TASKS = 100;
    constexpr int NUM_CONSUMERS = 1;  // 多个消费者线程
    constexpr int LENGTH = 1 << 16;
    size_t bytes = LENGTH * sizeof(float);

    // 队列
    BlockQueue<DeviceBufferBlock*> free_queue(POOL_SIZE);
    MSQueue<InferenceTask> task_queue;      // MPMC，支持多消费者
    BlockQueue<InferenceTask> done_queue(POOL_SIZE);

    std::atomic<bool> producer_done(false);
    std::atomic<int> processed_count(0);   // 所有消费者共享的已处理计数

    // 主机结果数组
    std::vector<float*> host_results(NUM_TASKS);
    for (int i = 0; i < NUM_TASKS; ++i) {
        host_results[i] = new float[LENGTH];
    }

    // 跟踪所有缓冲区块，用于最终释放
    std::vector<DeviceBufferBlock*> all_blocks;

    // 预分配缓冲区块池
    for (int i = 0; i < POOL_SIZE; ++i) {
        DeviceBufferBlock* block = new DeviceBufferBlock;
        block->bytes = bytes;
        cudaMalloc(&block->d_A, bytes);
        cudaMalloc(&block->d_B, bytes);
        cudaMalloc(&block->d_C, bytes);
        cudaEventCreate(&block->done_event);
        free_queue.Push(block);
        all_blocks.push_back(block);
    }

    // 为生产者创建专用流，避免默认流与其他显式流同步
    cudaStream_t producer_stream;
    cudaStreamCreate(&producer_stream);

    //开始计时
    auto start = std::chrono::high_resolution_clock::now();

    // ============ 生产者线程（不变） ============
    std::thread producer([&]() {
        for (int task_id = 0; task_id < NUM_TASKS; ++task_id) {
            DeviceBufferBlock* block = free_queue.Pop();

            float* h_A = new float[LENGTH];
            float* h_B = new float[LENGTH];
            for (int i = 0; i < LENGTH; ++i) {
                h_A[i] = task_id * 1.0f;
                h_B[i] = task_id * 2.0f;
            }

            cudaMemcpyAsync(block->d_A, h_A, bytes, cudaMemcpyHostToDevice, producer_stream);
            cudaMemcpyAsync(block->d_B, h_B, bytes, cudaMemcpyHostToDevice, producer_stream);
            cudaStreamSynchronize(producer_stream);  // 等拷贝完成才能入队，否则数据可能还没到

            InferenceTask task;
            task.task_id = task_id;
            task.block = block;
            task.h_output = host_results[task_id];
            task.length = LENGTH;

            task_queue.Enqueue(task);

            delete[] h_A;
            delete[] h_B;
        }
        producer_done.store(true);
        printf("Producer: done, enqueued %d tasks\n", NUM_TASKS);
    });

    // ============ 多个消费者线程 ============
    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&, c]() {
            // 每个消费者创建自己的流
            cudaStream_t stream;
            cudaStreamCreate(&stream);

            while (true) {
                // 检查是否所有任务都已被处理
                if (processed_count.load() >= NUM_TASKS) {
                    break;
                }

                InferenceTask task;
                if (task_queue.Dequeue(task)) {
                    int threadsPerBlock = 256;
                    int blocksPerGrid = (task.length + threadsPerBlock - 1) / threadsPerBlock;

                    // 在自己的流上启动核函数
                    vectorAdd<<<blocksPerGrid, threadsPerBlock, 0, stream>>>(
                        task.block->d_A, task.block->d_B, task.block->d_C, task.length);

                    // 异步拷贝结果（也在自己的流上）
                    cudaMemcpyAsync(task.h_output, task.block->d_C, bytes,
                                    cudaMemcpyDeviceToHost, stream);

                    // 在自己的流上记录完成事件
                    cudaEventRecord(task.block->done_event, stream);

                    // 放入完成队列
                    done_queue.Push(task);

                    int count = processed_count.fetch_add(1) + 1;
                } else {
                    // 队列空，等待生产者
                    if (!producer_done.load()) {
                        std::this_thread::yield();
                    } else {
                        // 生产者已结束，但如果还没处理完所有任务，短暂让出
                        std::this_thread::yield();
                    }
                }
            }

            cudaStreamDestroy(stream);
        });
    }

    // ============ 回收线程（不变） ============
    std::thread recycler([&]() {
        int recycled = 0;
        while (recycled < NUM_TASKS) {
            InferenceTask task = done_queue.Pop();
            cudaEventSynchronize(task.block->done_event);
            free_queue.Push(task.block);
            recycled++;
            
        }
    });

    // ============ 等待所有线程结束 ============
    producer.join();
    for (auto& t : consumers) t.join();
    recycler.join();

    cudaStreamDestroy(producer_stream);

    //结束计时
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    // ============ 验证结果 ============
    bool success = true;
    for (int task_id = 0; task_id < NUM_TASKS; ++task_id) {
        float expected = task_id * 3.0f;
        for (int i = 0; i < LENGTH; ++i) {
            if (fabs(host_results[task_id][i] - expected) > 1e-5) {
                success = false;
                
                break;
            }
        }
        if (!success) break;
    }

    printf("V3 Multi-stream + pool: %s\n", success ? "PASS" : "FAIL");
    printf("Time: %.2f ms\n", ms);
    printf("Throughput: %.2f tasks/s\n", NUM_TASKS * 1000.0 / ms);

    // ============ 清理 ============

    for (int i = 0; i < NUM_TASKS; ++i) {
        delete[] host_results[i];
    }

    for (auto* block : all_blocks) {
        cudaFree(block->d_A);
        cudaFree(block->d_B);
        cudaFree(block->d_C);
        cudaEventDestroy(block->done_event);
        delete block;
    }

    return success ? 0 : 1;
}