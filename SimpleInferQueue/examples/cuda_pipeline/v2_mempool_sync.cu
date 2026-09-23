#include <stdio.h>
#include <cuda_runtime.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include "ms_queue.h"
#include "block_queue.h"

struct DeviceBufferBlock {
    float* d_A;
    float* d_B;
    float* d_C;
    size_t bytes;
    cudaEvent_t done_event;
};

struct InferenceTask {
    int task_id;
    DeviceBufferBlock* block;
    float* h_output;
    int length;
};

__global__ void vectorAdd(const float* A, const float* B, float* C, int N) {
    int i = blockDim.x * blockIdx.x + threadIdx.x;
    if (i < N) {
        C[i] = A[i] + B[i];
    }
}

int main() {
    constexpr int POOL_SIZE = 4;
    constexpr int NUM_TASKS = 100;
    constexpr int LENGTH = 1 << 16;
    size_t bytes = LENGTH * sizeof(float);

    // 队列
    BlockQueue<DeviceBufferBlock*> free_queue(POOL_SIZE);
    MSQueue<InferenceTask> task_queue;
    BlockQueue<InferenceTask> done_queue(POOL_SIZE);

    std::atomic<bool> producer_done(false);

    // 主机结果数组
    std::vector<float*> host_results(NUM_TASKS);
    for (int i = 0; i < NUM_TASKS; ++i) {
        host_results[i] = new float[LENGTH];
    }

    // 预分配缓冲区块池
    for (int i = 0; i < POOL_SIZE; ++i) {
        DeviceBufferBlock* block = new DeviceBufferBlock;
        block->bytes = bytes;
        cudaMalloc(&block->d_A, bytes);
        cudaMalloc(&block->d_B, bytes);
        cudaMalloc(&block->d_C, bytes);
        cudaEventCreate(&block->done_event);
        free_queue.Push(block);
    }

    //开始计时
    auto start = std::chrono::high_resolution_clock::now();

    // 生产者
    std::thread producer([&]() {
        for (int task_id = 0; task_id < NUM_TASKS; ++task_id) {
            DeviceBufferBlock* block = free_queue.Pop();

            float* h_A = new float[LENGTH];
            float* h_B = new float[LENGTH];
            for (int i = 0; i < LENGTH; ++i) {
                h_A[i] = task_id * 1.0f;
                h_B[i] = task_id * 2.0f;
            }

            cudaMemcpy(block->d_A, h_A, bytes, cudaMemcpyHostToDevice);
            cudaMemcpy(block->d_B, h_B, bytes, cudaMemcpyHostToDevice);

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
    });

    // 消费者
    std::thread consumer([&]() {
        int processed = 0;
        while (processed < NUM_TASKS) {
            InferenceTask task;
            if (task_queue.Dequeue(task)) {
                int threadsPerBlock = 256;
                int blocksPerGrid = (task.length + threadsPerBlock - 1) / threadsPerBlock;

                vectorAdd<<<blocksPerGrid, threadsPerBlock>>>(
                    task.block->d_A, task.block->d_B, task.block->d_C, task.length);

                cudaMemcpyAsync(task.h_output, task.block->d_C, bytes,
                                cudaMemcpyDeviceToHost);

                cudaDeviceSynchronize();

                cudaEventRecord(task.block->done_event);

                done_queue.Push(task);
                processed++;
            } else {
                if (!producer_done.load()) {
                    std::this_thread::yield();
                }
            }
        }
    });

    // 回收线程
    std::thread recycler([&]() {
        int recycled = 0;
        while (recycled < NUM_TASKS) {
            InferenceTask task = done_queue.Pop();
            cudaEventSynchronize(task.block->done_event);
            free_queue.Push(task.block);
            recycled++;
        }
    });

    producer.join();
    consumer.join();
    recycler.join();

    // 验证
    bool success = true;
    for (int task_id = 0; task_id < NUM_TASKS; ++task_id) {
        float expected = task_id * 3.0f;
        for (int i = 0; i < LENGTH; ++i) {
            if (fabs(host_results[task_id][i] - expected) > 1e-5) {
                success = false;
                printf("Mismatch at task %d, index %d: got %f, expected %f\n",
                       task_id, i, host_results[task_id][i], expected);
                break;
            }
        }
        if (!success) break;
    }

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    printf("V2 Async single stream: %s\n", success ? "PASS" : "FAIL");
    printf("Time: %.2f ms\n", ms);
    printf("Throughput: %.2f tasks/s\n", NUM_TASKS * 1000.0 / ms);


    
    // 清理
    for (int i = 0; i < NUM_TASKS; ++i) delete[] host_results[i];

    // 块在每次任务完成后都会 Push 回 free_queue（见消费者末尾的 free_queue.Push(task.block)），
    // 所以这里从队列里逐个取回来释放。用 try_Pop 而不是 Pop：
    // Pop() 是阻塞的，万一上面提前 break 有块没回来，阻塞版会永久挂住；
    // 非阻塞版宁可漏释放也不死等
    int freed = 0;
    DeviceBufferBlock* block = nullptr;
    while (free_queue.try_Pop(block)) {
        cudaFree(block->d_A);
        cudaFree(block->d_B);
        cudaFree(block->d_C);
        cudaEventDestroy(block->done_event);
        delete block;
        ++freed;
    }
    if (freed != POOL_SIZE) {
        printf("WARNING: 只回收了 %d / %d 个缓冲块\n", freed, POOL_SIZE);
    }

    return success ? 0 : 1;
}