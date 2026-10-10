#include "block_queue.h"
#include "ms_queue.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <onnxruntime_cxx_api.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>

// 模型规格：与 models/export_mobilenet_batch64.py 的导出规格一一对应，改一边必须改另一边。
// 这组常量写死的是 [64,3,224,224] -> [64,1000]，喂错模型会在 session.Run() 抛形状不匹配异常
constexpr int BATCH = 64;
constexpr int IN_C = 3;
constexpr int IN_H = 224;
constexpr int IN_W = 224;
constexpr int NUM_CLASSES = 1000;
constexpr int IN_SIZE = BATCH * IN_C * IN_H * IN_W; // 64*3*224*224 = 9633792
constexpr int OUT_SIZE = BATCH * NUM_CLASSES;       // 64*1000       = 64000

// 任务结构体:只存task_id其他数据由任务共享
struct InferenceTask {
    int task_id;
};

// 结果结构体：存输出向量
struct InferenceResult {
    int task_id;
    std::vector<float> output; // 推理输出
    std::vector<int64_t> shape; // 输出张量的真实形状（如 {64,1000}），供校验和打印
};

int main(int argc, char **argv) {
    // 解析命令行参数
    bool use_cuda = true;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--cpu")
            use_cuda = false;
        if (std::string(argv[i]) == "--gpu")
            use_cuda = true;
    }

    constexpr int NUM_TASKS = 100;
    constexpr int NUM_CONSUMERS = 3;

    // 初始化ONNX Runtime
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "v5_inference");
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (use_cuda) {
        // 探测 CUDA EP 是否可用：取决于 ORT 能否在 exe 同目录加载到
        bool cuda_available = false;
        for (const auto &p : Ort::GetAvailableProviders()) {
            printf("  provider: %s\n", p.c_str());
            if (p == "CUDAExecutionProvider")
                cuda_available = true;
        }

        // 一个 EP 都不 Append 时，ORT 用的是内置 CPU EP（c_api.h:1457），
        // 所以下面这个分支不是「选 CUDA 还是 CPU」，而是「要不要挂 CUDA」
        if (cuda_available) {
            // OrtCUDAProviderOptions 带 C++ 构造函数（c_api.h:641），
            OrtCUDAProviderOptions cuda_options{};
            cuda_options.device_id = 0;
            session_options.AppendExecutionProvider_CUDA(cuda_options);
            printf("Backend: CUDA\n");
        } else {
            printf("CUDA is not available, falling back to CPU\n");
        }
    } else {
        printf("Backend: CPU\n");
    }

    // 加载模型
    std::filesystem::path model_path{MODEL_PATH};
    printf("Model: %s\n", model_path.string().c_str());
    Ort::Session session(env, model_path.c_str(), session_options);

    // 获取输出名称
    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name = session.GetInputNameAllocated(0, allocator);
    auto output_name = session.GetOutputNameAllocated(0, allocator);

    // 保存为std::,避免AllocatedStringPtr 生命周期问题
    std::string input_name_str = input_name.get();
    std::string output_name_str = output_name.get();

    // 准备数据共享
    // 所有任务用同一份输入，方便验证
    std::vector<float> shared_input(IN_SIZE, 0.5f);
    std::vector<int64_t> input_shape = {BATCH, IN_C, IN_H, IN_W};
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // 队列
    MSQueue<InferenceTask> task_queue;
    MSQueue<InferenceResult> result_queue;

    std::atomic<bool> producer_done(false);
    std::atomic<int> total_enqueue(0);
    std::atomic<int> processed_count(0);

    // 计时开始
    auto start = std::chrono::high_resolution_clock::now();

    // 生产者线程
    std::thread producer([&]() {
        for (int i = 0; i < NUM_TASKS; i++) {
            InferenceTask task{i};
            task_queue.Enqueue(task);
            total_enqueue.fetch_add(1);
        }
        producer_done.store(true);
    });

    // 多个消费者线程
    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; c++) {
        consumers.emplace_back([&, c]() {
            while (true) {
                if (producer_done.load() && processed_count.load() >= total_enqueue.load()) {
                    break;
                }

                InferenceTask task;
                if (task_queue.Dequeue(task)) {
                    // 准备输入张量
                    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
                        memory_info, shared_input.data(), shared_input.size(), input_shape.data(),
                        input_shape.size());

                    // 运行推理
                    const char *input_names[] = {input_name_str.c_str()};
                    const char *output_names[] = {output_name_str.c_str()};

                    auto outoput_tensors = session.Run(Ort::RunOptions{nullptr}, input_names,
                                                       &input_tensor, 1, output_names, 1);

                    // 取出输出。形状要从张量本身读，不能拿源码里那几个常量去凑：
                    // 常量是「我以为的规格」，GetShape() 才是「ORT 实际算出来的」
                    auto shape_info = outoput_tensors[0].GetTensorTypeAndShapeInfo();
                    float *output_data = outoput_tensors[0].GetTensorMutableData<float>();
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

    // 主线程：验证结果
    int verified = 0;
    std::vector<float> first_output;
    std::vector<int64_t> first_shape;
    while (verified < NUM_TASKS) {
        InferenceResult result;
        if (result_queue.Dequeue(result)) {
            if (first_output.empty()) {
                first_output = result.output;
                first_shape = result.shape;
            }
            verified++;
        } else {
            std::this_thread::yield();
        }
    }

    // 等待线程结束
    producer.join();
    for (auto &t : consumers)
        t.join();

    // 计时结束
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    // 此时线程全部停止，可以进行任务检查
    if (processed_count.load() != NUM_TASKS) {
        printf("WARNING: processed %d / %d task\n", processed_count.load(), NUM_TASKS);
    }

    // 汇总输出
    bool success = (processed_count.load() == NUM_TASKS && first_output.size() == OUT_SIZE);

    // 打印前5个输出，供和 Python 端对比
    printf("Output shape: [");
    for (size_t i = 0; i < first_shape.size(); ++i) {
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
