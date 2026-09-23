#include <stdio.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include <string>
#include <cstdlib>
#include "tensorrt_engine.h"
#include "ms_queue.h"

//MobileNetV2 batch=32的预期规格，只用于核对加载到的引擎（缓冲区按引擎实际大小分配）
constexpr int BATCH = 32;
constexpr int IN_C = 3;
constexpr int IN_H = 224;
constexpr int IN_W = 224;
constexpr int IN_SIZE = BATCH * IN_C * IN_H * IN_W;//4816896
constexpr int OUT_SIZE = BATCH * 1000;//32000

//任务：只存task_id
struct InferenceTask {
    int task_id;
};

//结果：存输出向量
struct InferenceResult {
    int task_id;
    std::vector<float> output;
};

int main(int argc, char** argv) {
    //不设的话 stdout 是块缓冲，线程里崩了前面 printf 全丢，只剩退出码
    setvbuf(stdout, nullptr, _IONBF, 0);

    constexpr int NUM_TASKS = 100;
    constexpr int NUM_CONSUMERS = 3;

    //路径由 CMake 以绝对路径注入，命令行给一个可以覆盖（换 batch 的引擎）
    const std::string engine_path = (argc > 1) ? argv[1] : ENGINE_PATH;
    printf("Engine: %s\n", engine_path.c_str());

    //队列
    MSQueue<InferenceTask> task_queue;
    MSQueue<InferenceResult> result_queue;

    std::atomic<bool> producer_done(false);
    std::atomic<int> total_enqueued(0);
    std::atomic<int> processed_count(0);
    std::atomic<int> engines_ready(0);   //已建好引擎的线程数
    std::atomic<bool> go(false);         //计时已开始，消费者可以开工

    //每线程回填自己的槽位，主线程 join 完再统一打印
    std::vector<double> load_ms(NUM_CONSUMERS, 0.0);
    std::vector<double> infer_ms(NUM_CONSUMERS, 0.0);  //线程内累计的纯推理耗时
    std::vector<int> thread_tasks(NUM_CONSUMERS, 0);

    //生产者
    std::thread producer([&]() {
        for(int i = 0; i < NUM_TASKS; i++) {
            InferenceTask task{i};
            task_queue.Enqueue(task);
            total_enqueued.fetch_add(1);
        }
        producer_done.store(true);
    });

    //多个消费者，每个线程独立创建engine
    std::vector<std::thread> consumers;
    for(int c =0; c < NUM_CONSUMERS; c++) {
        consumers.emplace_back([&, c]() {
            auto load_t0 = std::chrono::high_resolution_clock::now();
            TensorRTEngine engine(engine_path);
            load_ms[c] = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::high_resolution_clock::now() - load_t0).count() / 1000.0;

            engines_ready.fetch_add(1);

            //等主线程按下秒表再开工：少了这道闸，最后建好的线程可能在主线程
            //取 start 之前就抢完一个任务，那个任务就漏在计时外了
            while(!go.load()) {
                std::this_thread::yield();
            }

            //缓冲区按引擎实际大小分配：infer() 按引擎的 output_size_ 拷贝、校验不了
            //调用者缓冲多长，按常量分配的话换个 batch 的引擎就是堆越界
            if(engine.input_size() != static_cast<size_t>(IN_SIZE) ||
               engine.output_size() != static_cast<size_t>(OUT_SIZE)) {
                printf("引擎规格 (%zu, %zu) 与源码常量 (%d, %d) 不符——换了模型记得同步 BATCH\n",
                       engine.input_size(), engine.output_size(), IN_SIZE, OUT_SIZE);
                std::exit(1);
            }

            //每线程自己的输入输出缓冲
            std::vector<float> input(engine.input_size(), 0.5f);
            std::vector<float> output_buffer(engine.output_size());

            int mine = 0;
            while(true) {
                if(producer_done.load() && processed_count.load() >= total_enqueued.load()) {
                    break;
                }

                InferenceTask task;
                if(task_queue.Dequeue(task)) {
                    //执行推理。单独掐表：这个数才是「推理的时间」，下面的 Time 是
                    //整条流水线的墙钟，两者之差就是排队、拷结果、校验这些开销
                    auto infer_t0 = std::chrono::high_resolution_clock::now();
                    engine.infer(input.data(), output_buffer.data());
                    infer_ms[c] += std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::high_resolution_clock::now() - infer_t0).count() / 1000.0;

                    InferenceResult result;
                    result.task_id = task.task_id;
                    result.output = output_buffer;

                    result_queue.Enqueue(result);
                    processed_count.fetch_add(1);
                    mine++;
                } else {
                    std::this_thread::yield();
                }
            }
            thread_tasks[c] = mine;
        });
    }

    //等三个引擎都加载完再开始计时：加载是这份设计的代价，不该算进推理吞吐。
    //v5/v6 的 session 都建在计时之前，这样三者的吞吐才可比
    while(engines_ready.load() < NUM_CONSUMERS) {
        std::this_thread::yield();
    }

    //计时开始
    auto start = std::chrono::high_resolution_clock::now();
    go.store(true);

    //主线程：消费结果并验证
    int verified = 0;
    std::vector<float> first_output;
    int size_mismatch = 0;
    double max_abs_diff = 0.0;
    while(verified < NUM_TASKS) {
        InferenceResult result;
        if(result_queue.Dequeue(result)) {
            if(first_output.empty()) {
                first_output = result.output;
            } else if(result.output.size() != first_output.size()) {
                size_mismatch++;
            } else {
                //100 个任务喂同一份输入，输出必须逐位相同——跨线程串用 context/缓冲/流 只在这露出来
                for(size_t i = 0; i < first_output.size(); ++i) {
                    double d = std::fabs(static_cast<double>(result.output[i])
                                       - static_cast<double>(first_output[i]));
                    if(d > max_abs_diff) max_abs_diff = d;
                }
            }
            verified++;
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();
    for(auto& t : consumers) t.join();

    //计时结束
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    //检查最终状态
    if(processed_count.load() != NUM_TASKS) {
        printf("WARNING: processed %d / %d\n", processed_count.load(), NUM_TASKS);
    }

    //各引擎加载耗时：每线程各反序列化一份引擎，这份代价单独列出，别算进下面的吞吐
    printf("Engines: %d instances (one per consumer thread)\n", NUM_CONSUMERS);
    double infer_total = 0.0;
    for(int c = 0; c < NUM_CONSUMERS; ++c) {
        printf("  [t%d] load %.1f ms, %d tasks, infer %.1f ms (%.2f ms/task)\n",
               c, load_ms[c], thread_tasks[c], infer_ms[c],
               thread_tasks[c] > 0 ? infer_ms[c] / thread_tasks[c] : 0.0);
        infer_total += infer_ms[c];
    }
    //三个线程各自累计后相加：并发跑时它会大于墙钟，比值就是并行的重叠程度
    printf("Infer total: %.1f ms over %d tasks (%.2f ms/task, sum of threads)\n",
           infer_total, NUM_TASKS, infer_total / NUM_TASKS);

    bool success = (processed_count.load() == NUM_TASKS
                    && !first_output.empty()
                    && size_mismatch == 0
                    && max_abs_diff == 0.0);

    printf("Output shape: [%d, 1000]  (%zu floats)\n",
           BATCH, first_output.size());
    printf("Output[:5]: ");
    for (int i = 0; i < 5 && i < (int)first_output.size(); ++i) {
        printf("%.6f ", first_output[i]);
    }
    printf("\n");

    printf("V7 TensorRT native: %s  (%d tasks, size mismatch %d, max |diff| = %.3e)\n",
           success ? "PASS" : "FAIL", NUM_TASKS, size_mismatch, max_abs_diff);
    printf("Time: %.2f ms\n", ms);
    printf("Throughput: %.2f tasks/s\n", NUM_TASKS * 1000.0 / ms);

    return success ? 0 : 1;
}