#include <stdio.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include <string>
#include <filesystem>
#include <onnxruntime_cxx_api.h>
#include "ms_queue.h"
#include "block_queue.h"



//模型规格：对应 models/export_mobilenet_batch32.py 的导出规格 [32,3,224,224] -> [32,1000]
//CMake 会读这里的 BATCH 值去配 models/mobilenet_batch<BATCH>.onnx，改这个数模型自动跟着换
constexpr int BATCH = 32;
constexpr int IN_C = 3;
constexpr int IN_H = 224;
constexpr int IN_W = 224;
constexpr int NUM_CLASSES = 1000;
constexpr int IN_SIZE = BATCH * IN_C * IN_H * IN_W; //32*3*224*224 = 4816896
constexpr int OUT_SIZE = BATCH * NUM_CLASSES;       //32*1000       = 32000

//任务结构体:只存task_id其他数据由任务共享
struct InferenceTask {
    int task_id;
};

//结果结构体：存输出向量
struct InferenceResult {
    int task_id;
    std::vector<float> output;  //推理输出
    std::vector<int64_t> shape; //输出张量的真实形状（如 {32,1000}），供校验和打印
};

int main(int argc, char** argv) {
    //解析命令行参数
    bool use_cuda = true;
    for(int i = 1; i < argc; i++) {
        if(std::string(argv[i]) == "--cpu") use_cuda = false;
        if(std::string(argv[i]) == "--gpu") use_cuda = true;
    }

    constexpr int NUM_TASKS = 100;
    constexpr int NUM_CONSUMERS = 3;

    //初始化ONNX Runtime
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "v6_trt_inference");
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if(use_cuda) {
        //探测 ORT 能加载到哪些 provider，逐个打印出来
        bool trt_available  = false;
        bool cuda_available = false;
        for(const auto& p : Ort::GetAvailableProviders()) {
            printf("  provider: %s\n", p.c_str());
            if(p == "TensorrtExecutionProvider") trt_available  = true;
            if(p == "CUDAExecutionProvider")     cuda_available = true;
        }

        //挂 TensorRT EP（优先级更高，先挂）
        if(trt_available) {
            OrtTensorRTProviderOptions trt_options{};
            trt_options.device_id = 0;
            trt_options.trt_max_partition_iterations = 1000;
            trt_options.trt_min_subgraph_size = 1;
            trt_options.trt_fp16_enable = 1;                    //FP16 推理
            trt_options.trt_engine_cache_enable = 1;            //开启引擎缓存
            trt_options.trt_engine_cache_path = TRT_CACHE_PATH; //CMake 注入的绝对路径
            session_options.AppendExecutionProvider_TensorRT(trt_options);
        }

        //挂 CUDA EP 作为回退（TRT 分不掉的子图交给它）
        if(cuda_available) {
            OrtCUDAProviderOptions cuda_options{};
            cuda_options.device_id = 0;
            session_options.AppendExecutionProvider_CUDA(cuda_options);
        }

        //打印最终生效的后端
        if(trt_available && cuda_available) printf("Backend: TensorRT + CUDA\n");
        else if(trt_available)              printf("Backend: TensorRT\n");
        else if(cuda_available)             printf("Backend: CUDA\n");
        else                                printf("没有 GPU provider，回退 CPU\n");
    } else {
        printf("Backend: CPU\n");
    }


    //加载模型
    std::filesystem::path model_path{MODEL_PATH};
    printf("Model: %s\n", model_path.string().c_str());
    Ort::Session session(env, model_path.c_str(), session_options);

    //获取输入/输出名称，存成std::string避免AllocatedStringPtr生命周期问题
    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name = session.GetInputNameAllocated(0, allocator);
    auto output_name = session.GetOutputNameAllocated(0, allocator);
    std::string input_name_str = input_name.get();
    std::string output_name_str = output_name.get();

    //准备数据共享：所有任务用同一份输入，方便验证
    std::vector<float> shared_input(IN_SIZE, 0.5f);
    std::vector<int64_t> input_shape = {BATCH, IN_C, IN_H, IN_W};
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    //队列
    MSQueue<InferenceTask> task_queue;
    MSQueue<InferenceResult> result_queue;

    std::atomic<bool> producer_done(false);
    std::atomic<int> total_enqueue(0);
    std::atomic<int> processed_count(0);

    //计时开始
    auto start = std::chrono::high_resolution_clock::now();

    //生产者线程
    std::thread producer([&]() {
        for(int i = 0; i < NUM_TASKS; i++) {
            InferenceTask task{i};
            task_queue.Enqueue(task);
            total_enqueue.fetch_add(1);
        }
        producer_done.store(true);
    });

    //多个消费者线程
    std::vector<std::thread> consumers;
    for(int c = 0; c < NUM_CONSUMERS; c++) {
         consumers.emplace_back([&, c]() {
            while(true) {
                if(producer_done.load() && processed_count.load() >= total_enqueue.load()) {
                    break;
                }

                InferenceTask task;
                if(task_queue.Dequeue(task)) {
                    //准备输入张量
                    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, shared_input.data(), shared_input.size(), input_shape.data(), input_shape.size());

                    //运行推理
                    const char* input_names[] = {input_name_str.c_str()};
                    const char* output_names[] = {output_name_str.c_str()};

                    auto outoput_tensors = session.Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);

                    //取出输出张量（形状从张量本身读，不靠源码里那几个常量推）
                    auto shape_info = outoput_tensors[0].GetTensorTypeAndShapeInfo();
                    float* output_data = outoput_tensors[0].GetTensorMutableData<float>();
                    size_t output_count = shape_info.GetElementCount();

                    InferenceResult result;
                    result.task_id = task.task_id;
                    result.output.assign(output_data, output_data + output_count);
                    result.shape = shape_info.GetShape();
                    result_queue.Enqueue(result);
                    processed_count.fetch_add(1);
                } else {
                    std::this_thread::yield();
                }
            }
         });
    }

    //主线程：验证结果
    int verified = 0;
    std::vector<float> first_output;
    std::vector<int64_t> first_shape;
    while(verified < NUM_TASKS) {
        InferenceResult result;
        if(result_queue.Dequeue(result)) {
           if(first_output.empty()) {
            first_output = result.output;
            first_shape = result.shape;
           }
            verified++;
        } else {
            std::this_thread::yield();
        }
    }

    //等待线程结束
    producer.join();
    for (auto& t : consumers) t.join();


    //计时结束
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

     //此时线程全部停止，可以进行任务检查
    if(processed_count.load() != NUM_TASKS) {
        printf("WARNING: processed %d / %d task\n", processed_count.load(), NUM_TASKS);
    }


    //汇总输出
    bool success = (processed_count.load() == NUM_TASKS && first_output.size() == OUT_SIZE);

    //打印输出形状和前5个值，供和 Python 端对比
    printf("Output shape: [");
    for(size_t i = 0; i < first_shape.size(); ++i) {
        printf("%s%lld", i ? ", " : "", static_cast<long long>(first_shape[i]));
    }
    printf("]  (%zu floats)\n", first_output.size());
    printf("Output[:5]: ");
    for (int i = 0; i < 5 && i < (int)first_output.size(); ++i) {
        printf("%.6f ", first_output[i]);
    }
    printf("\n");

    printf("VS ONNX Runtime inference: %s\n", success ? "PASS" : "FAIL");
    printf("Time: %.2f ms\n", ms);
    printf("Throughput: %.2f tasks/s\n", NUM_TASKS * 1000.0 / ms);

    return success ? 0 : 1;
}


