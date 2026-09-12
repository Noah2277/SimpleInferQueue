#include <stdio.h>
#include <cuda_runtime.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include "ms_queue.h"

// 任务结构体
struct InferenceTask {
    int task_id;
    float* d_inputA;
    float* d_inputB;
    float* d_output;
    float* h_output;   // 主机结果缓冲区
    int length;
};

// CUDA 核函数：向量加法
__global__ void vectorAdd(const float* A, const float* B, float* C, int N) {
    int i = blockDim.x * blockIdx.x + threadIdx.x;
    if (i < N) {
        C[i] = A[i] + B[i];
    }
}

int main() {
    constexpr int NUM_TASKS = 100;
    constexpr int LENGTH = 1 << 16;  // 65536
    size_t bytes = LENGTH * sizeof(float);

    // 队列
    MSQueue<InferenceTask> queue;
    std::atomic<bool> producer_done(false);

    // 预分配主机结果数组
    std::vector<float*> host_results(NUM_TASKS);
    for (int i = 0; i < NUM_TASKS; ++i) {
        host_results[i] = new float[LENGTH];
    }

    // 用于收集所有设备缓冲区指针，最后统一释放
    std::vector<float*> device_ptrs;  // 只有生产者添加，主线程最后释放

    //计时开始
    auto start = std::chrono::high_resolution_clock::now();

    // 生产者线程
    std::thread producer([&]() {
        for (int task_id = 0; task_id < NUM_TASKS; ++task_id) {
            // 主机准备数据
            float* h_A = new float[LENGTH];
            float* h_B = new float[LENGTH];
            for (int i = 0; i < LENGTH; ++i) {
                h_A[i] = task_id * 1.0f;
                h_B[i] = task_id * 2.0f;
            }

            // 为每个任务独立分配设备内存
            float *d_A, *d_B, *d_C;
            cudaMalloc(&d_A, bytes);
            cudaMalloc(&d_B, bytes);
            cudaMalloc(&d_C, bytes);

            // 拷贝输入数据到设备
            cudaMemcpy(d_A, h_A, bytes, cudaMemcpyHostToDevice);
            cudaMemcpy(d_B, h_B, bytes, cudaMemcpyHostToDevice);

            // 收集设备指针，便于最后统一释放
            device_ptrs.push_back(d_A);
            device_ptrs.push_back(d_B);
            device_ptrs.push_back(d_C);

            // 构造任务
            InferenceTask task;
            task.task_id = task_id;
            task.d_inputA = d_A;
            task.d_inputB = d_B;
            task.d_output = d_C;
            task.h_output = host_results[task_id];
            task.length = LENGTH;

            queue.Enqueue(task);

            delete[] h_A;
            delete[] h_B;
        }
        producer_done.store(true);
    });

    // 消费者线程
    std::thread consumer([&]() {
        int processed = 0;
        while (processed < NUM_TASKS) {
            InferenceTask task;
            if (queue.Dequeue(task)) {
                int threadsPerBlock = 256;
                int blocksPerGrid = (task.length + threadsPerBlock - 1) / threadsPerBlock;

                // 在默认流上启动核函数
                vectorAdd<<<blocksPerGrid, threadsPerBlock>>>(
                    task.d_inputA, task.d_inputB, task.d_output, task.length);

                // 异步拷贝结果到主机
                cudaMemcpyAsync(task.h_output, task.d_output, bytes,
                                cudaMemcpyDeviceToHost);
                cudaDeviceSynchronize();   // 同步基线：每个任务都等 GPU 算完
                processed++;
            } else {
                if (!producer_done.load()) {
                    std::this_thread::yield();
                }
            }
        }
    });

    producer.join();
    consumer.join();

    // 等待所有 GPU 工作（核函数和异步拷贝）完成
    cudaDeviceSynchronize();

    //计时结束
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    // 验证所有任务结果
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

    printf("V1 Sync baseline: %s\n", success ? "PASS" : "FAIL");
    printf("Time: %.2f ms\n", ms);
    printf("Throughput: %.2f tasks/s\n", NUM_TASKS * 1000.0 / ms);

    // 清理所有设备内存
    for (float* ptr : device_ptrs) {
        cudaFree(ptr);
    }

    // 清理主机结果数组
    for (int i = 0; i < NUM_TASKS; ++i) {
        delete[] host_results[i];
    }

    return success ? 0 : 1;
}