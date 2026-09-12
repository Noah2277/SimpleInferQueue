#include <stdio.h>
#include <cuda_runtime.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include "ms_queue.h"
#include "block_queue.h"

// CPU-GPU 流水线原型
// 用 MSQueue 连接 CPU 生产者和 GPU 计算


//设置缓冲区块：一组可复用的显存
struct DeviceBufferBlock{
    float* d_A;
    float* d_B;
    float* d_C;
    size_t bytes;
    cudaEvent_t done_enent;  //标记GPU何时用完这个块
};

//任务结构体：描述一个加法向量
struct InferenceTask
{
    int task_id; //任务唯一编号，用于验证结果
    DeviceBufferBlock* block; //指向它使用的缓冲区块
    float* h_output; //主机结果缓冲器
    int length;
};

//结果结构体
struct InferenceResult {
    int task_id;
    float* h_output;
    int length;
    cudaEvent_t copy_done; //拷贝完成事件
};

//CUDA核函数：向量加法 C = A + B
__global__ void vectorAdd(const float* A, const float* B, float* C, int N) {
    int i = blockDim.x * blockIdx.x + threadIdx.x;
    if(i < N) {
        C[i] = A[i] + B[i];
    }
}

int main() {
    constexpr int POOL_SIZE = 4; //池中缓冲区块数量
    constexpr int NUM_TASK = 100;
    constexpr int NUM_CONSUMERS = 3; //多个消费者线程
    constexpr int LENGTH = 1 << 16;
    size_t bytes = LENGTH * sizeof(float);

    //队列
    BlockQueue<DeviceBufferBlock*> free_queue(POOL_SIZE);//空闲队列：存放可用的DeviceBufferBlock*
    MSQueue<InferenceTask> task_queue;//任务队列
    BlockQueue<InferenceTask> done_queue(POOL_SIZE);//完成队列：存放已完成的任务，等待回收
    MSQueue<InferenceResult> result_queue; //结果队列

    std::atomic<bool> producer_done(false);
    std::atomic<int> processed_count(0); //所有消费者共享已处理计数
    std::atomic<int> reslut_processed(0); //输出线程已处理的结果数
    std::atomic<int> error_count(0); //验证失败计数

    

    //主机结果数组
    std::vector<float*> host_results(NUM_TASK);
    for(int i = 0; i< NUM_TASK; i++) {
        host_results[i] = new float[LENGTH];
    }

    //跟踪所有缓冲区块，用于最终释放
    std::vector<DeviceBufferBlock*> all_blocks;

    //预分配POOL_SIZE个缓冲区域，放入空闲队列
    for(int i = 0; i < POOL_SIZE; i++) {
        DeviceBufferBlock* block = new DeviceBufferBlock;
        block->bytes = bytes;
        cudaMalloc(&block->d_A, bytes);
        cudaMalloc(&block->d_B, bytes);
        cudaMalloc(&block->d_C, bytes);
        cudaEventCreate(&block->done_enent); 
        free_queue.Push(block);
        all_blocks.push_back(block);
    }

    // 预分配结束后，生产者线程创建前
    auto start = std::chrono::high_resolution_clock::now();

    //生产者线程
    std::thread producer([&]() {
        for(int task_id = 0; task_id < NUM_TASK; task_id++) {
            //从池里取出一个空闲块
            DeviceBufferBlock * block = free_queue.Pop();

            //主机准备数据
            float* h_A = new float[LENGTH];
            float* h_B = new float[LENGTH];
            for(int i = 0; i < LENGTH; i++) {
                h_A[i] = task_id * 1.0f;
                h_B[i] = task_id * 2.0f;
            }

            //拷贝到显存
            cudaMemcpy(block->d_A, h_A, bytes, cudaMemcpyHostToDevice);
            cudaMemcpy(block->d_B, h_B, bytes, cudaMemcpyHostToDevice);
           
            //构造任务
            InferenceTask task;
            task.task_id = task_id;
            task.block = block;
            task.h_output = host_results[task_id];
            task.length = LENGTH;

            task_queue.Enqueue(task);

            //释放主机临时数据
            delete[] h_A;
            delete[] h_B;
        }
        producer_done.store(true);
    });

    //消费者线程(多个)
    std::vector<std::thread> consumers;
    for(int c = 0; c < NUM_CONSUMERS; c++) {
        consumers.emplace_back([&, c]() {
            //每个消费者创建自己的流
            cudaStream_t stream;
            cudaStreamCreate(&stream);

            while(true) {
                //检查所有任务是否已被处理
                if(processed_count.load() >= NUM_TASK) {
                    break;
                }

                InferenceTask task;
                if(task_queue.Dequeue(task)) {
                    int threadsPerBlock = 256;
                    int blockPerGrid = (task.length + threadsPerBlock - 1) / threadsPerBlock;

                    //在自己的流上启动核函数
                    vectorAdd<<<blockPerGrid, threadsPerBlock, 0, stream>>>(task.block->d_A, task.block->d_B, task.block->d_C, task.length);

                    //异步拷贝结果（在自己的流上
                    cudaMemcpyAsync(task.h_output, task.block->d_C, bytes, cudaMemcpyDeviceToHost, stream);

                    //在自己的流上记录块完成事件（回收线程据此知道 GPU 何时用完块）
                    cudaEventRecord(task.block->done_enent, stream);

                    //为本次结果创建专属完成事件（块事件会被后续任务重复记录，
                    //不能拿它来等本任务的拷贝完成）
                    cudaEvent_t copy_done;
                    cudaEventCreate(&copy_done);
                    cudaEventRecord(copy_done, stream);

                    //构造结果并送入结果队列
                    InferenceResult result;
                    result.task_id = task.task_id;
                    result.h_output = task.h_output;
                    result.length = task.length;
                    result.copy_done = copy_done;
                    result_queue.Enqueue(result);

                    //放入完成队列
                    done_queue.Push(task);

                    int count = processed_count.fetch_add(1) + 1;
                    
                } else {
                    //队列空，等待生产者
                    if(!producer_done.load()) {
                        std::this_thread::yield();
                    } else {
                        //生产者已结束，但如果还没有处理完所有任务，短暂退出
                        std::this_thread::yield();
                    }
                }
            }
            cudaStreamDestroy(stream);
        });
    } 
    
    //输出线程
    std::thread outputter([&]() {
        while(reslut_processed.load() < NUM_TASK) {
            InferenceResult result;
            if(result_queue.Dequeue(result)) {
                //等待拷贝完成
                cudaEventSynchronize(result.copy_done);
                cudaEventDestroy(result.copy_done);

                //验证结果
                float expected = result.task_id * 3.0f;
                bool ok = true;
                for(int i = 0; i < result.length; i++) {
                    if(fabs(result.h_output[i] - expected) > 1e-5) {
                        ok = false;
                        
                        break;
                    }
                }
                if(!ok) {
                    error_count.fetch_add(1);
                }

                int count = reslut_processed.fetch_add(1) + 1;
                
            } else {
                std::this_thread::yield();
            }
        }
    });

    //回收线程
    std::thread recycler([&] () {
        int recycled = 0;
        while(recycled < NUM_TASK) {
            InferenceTask task = done_queue.Pop(); //阻塞等待完成任务

            //等待GPU用完这个块
            cudaEventSynchronize(task.block->done_enent);

            //把块还回空闲队列
            free_queue.Push(task.block);

            recycled++;
        }
    });

    //等待线程结束
    producer.join();
   for(auto& t : consumers) t.join();
    recycler.join();
    outputter.join();

    // 所有线程 join 之后（producer、consumers、outputter、recycler）
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    //消费者用的是 cudaMemcpyAsync（异步），join 只代表线程函数返回，
    //拷贝可能还在进行——验证前必须等所有流上的工作全部完成
    cudaDeviceSynchronize();

    //验证所有任务结果
    bool success = true;
    for(int task_id = 0; task_id < NUM_TASK; task_id++) {
        float expected = task_id * 3.0f;
        for(int i = 0; i < LENGTH; i++) {
            if(fabs(host_results[task_id][i] - expected) > 1e-5) {
                success = false;
                break;
            }
        }
        if(!success) break;
    }

    printf("V4 Bidirectional pipeline: %s\n", success ? "PASS" : "FAIL");
    printf("Time: %.2f ms\n", ms);
    printf("Throughput: %.2f tasks/s\n", NUM_TASK * 1000.0 / ms);

    //清理
    for(int i = 0; i < NUM_TASK; i++) delete[] host_results[i];
    for(auto* block : all_blocks) {
       cudaFree(block->d_A);
       cudaFree(block->d_B);
       cudaFree(block->d_C);
       cudaEventDestroy(block->done_enent);
       delete block;
    }
    

    return success ? 0 : 1;
}
