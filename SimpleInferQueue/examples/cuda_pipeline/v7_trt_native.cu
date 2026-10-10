#include "ms_queue.h"
#include "tensorrt_engine.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// MobileNetV2 batch=32的预期规格，只用于核对加载到的引擎（缓冲区按引擎实际大小分配）
constexpr int BATCH = 32;
constexpr int IN_C = 3;
constexpr int IN_H = 224;
constexpr int IN_W = 224;
constexpr int IN_SIZE = BATCH * IN_C * IN_H * IN_W; // 4816896
constexpr int OUT_SIZE = BATCH * 1000;              // 32000

// 任务：只存task_id
struct InferenceTask {
    int task_id;
};

// 结果：存输出向量
struct InferenceResult {
    int task_id;
    std::vector<float> output;
};

int main(int argc, char **argv) {
    // 不设的话 stdout 是块缓冲，线程里崩了前面 printf 全丢，只剩退出码
    setvbuf(stdout, nullptr, _IONBF, 0);

    constexpr int NUM_TASKS = 100;
    constexpr int NUM_CONSUMERS_DEFAULT = 3;

    // 路径由 CMake 以绝对路径注入，命令行给一个可以覆盖（换 batch 的引擎）。
    // 空串视同没给：否则想「只改线程数、用默认引擎」就得把整条绝对路径抄一遍
    const std::string engine_path = (argc > 1 && argv[1][0] != '\0') ? argv[1] : ENGINE_PATH;

    // 消费者线程数也可以用命令行覆盖（argv[2]）。
    // 有了它才能干净地回答「加线程有没有换来吞吐」：同一份二进制、同一套开销，
    // 只改线程数跑 1 vs 3。否则拿本程序的三线程墙钟去比别处的串行推理，
    // 比的其实是两份不同的开销
    int num_consumers = NUM_CONSUMERS_DEFAULT;
    if (argc > 2) {
        num_consumers = std::atoi(argv[2]);
        if (num_consumers < 1) {
            printf("argv[2] 是消费者线程数，需 >= 1\n");
            return 1;
        }
    }

    printf("Engine: %s\n", engine_path.c_str());
    printf("Consumers: %d\n", num_consumers);

    // 引擎只反序列化一次，之后被所有消费者线程共享——这是 TensorRT 官方推荐的用法。
    // 引擎持有的是权重（只读），复制它既费显存又拿不到吞吐；一次推理里真正
    // 需要每线程独占的是 context、流和显存缓冲，那些在 Session 里。
    // 失败这里会抛异常而不是杀进程，给调用方留下降级/换模型的余地
    std::shared_ptr<TensorRTEngine> engine;
    try {
        engine = std::make_shared<TensorRTEngine>(engine_path);
    } catch (const std::exception &e) {
        printf("建引擎失败: %s\n", e.what());
        return 1;
    }

    // 核对引擎规格与源码常量是否一致。放在主线程做一次就够了
    if (engine->input_size() != static_cast<size_t>(IN_SIZE) ||
        engine->output_size() != static_cast<size_t>(OUT_SIZE)) {
        printf("引擎规格 (%zu, %zu) 与源码常量 (%d, %d) 不符——换了模型记得同步 BATCH\n",
               engine->input_size(), engine->output_size(), IN_SIZE, OUT_SIZE);
        return 1;
    }

    printf("Engine loaded: input %zu floats, output %zu floats\n", engine->input_size(),
           engine->output_size());

    // 队列
    MSQueue<InferenceTask> task_queue;
    MSQueue<InferenceResult> result_queue;

    std::atomic<bool> producer_done(false);
    std::atomic<int> total_enqueued(0);
    std::atomic<int> processed_count(0);
    std::atomic<int> sessions_ready(0); // 已建好 Session 的线程数
    std::atomic<bool> go(false);        // 计时已开始，消费者可以开工

    // 每线程回填自己的槽位，主线程 join 完再统一打印
    std::vector<double> setup_ms(num_consumers, 0.0);
    std::vector<double> infer_ms(num_consumers, 0.0); // 线程内累计的纯推理耗时
    std::vector<int> thread_tasks(num_consumers, 0);

    // 生产者
    std::thread producer([&]() {
        for (int i = 0; i < NUM_TASKS; i++) {
            InferenceTask task{i};
            task_queue.Enqueue(task);
            total_enqueued.fetch_add(1);
        }
        producer_done.store(true);
    });

    // 多个消费者：共享同一个引擎，各自建一个 Session（context + 流 + 显存）
    std::vector<std::thread> consumers;
    for (int c = 0; c < num_consumers; c++) {
        consumers.emplace_back([&, c]() {
            auto setup_t0 = std::chrono::high_resolution_clock::now();
            TensorRTSession session(engine);
            setup_ms[c] = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::high_resolution_clock::now() - setup_t0)
                              .count() /
                          1000.0;

            sessions_ready.fetch_add(1);

            // 等主线程按下秒表再开工：少了这道闸，最后建好的线程可能在主线程
            // 取 start 之前就抢完一个任务，那个任务就漏在计时外了
            while (!go.load()) {
                std::this_thread::yield();
            }

            // 每线程自己的输入输出缓冲（CPU 端），Session 内部还有对应的显存
            std::vector<float> input(session.input_size(), 0.5f);
            std::vector<float> output_buffer(session.output_size());

            int mine = 0;
            while (true) {
                if (producer_done.load() && processed_count.load() >= total_enqueued.load()) {
                    break;
                }

                InferenceTask task;
                if (task_queue.Dequeue(task)) {
                    // 执行推理。单独掐表：这个数才是「推理的时间」，下面的 Time 是
                    // 整条流水线的墙钟，两者之差就是排队、拷结果、校验这些开销
                    auto infer_t0 = std::chrono::high_resolution_clock::now();
                    session.infer(input.data(), output_buffer.data());
                    infer_ms[c] += std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::high_resolution_clock::now() - infer_t0)
                                       .count() /
                                   1000.0;

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

    // 等所有 Session 都建好再开始计时：建 context/显存是这份设计的固定代价，
    // 不该算进推理吞吐。v5/v6 的 session 也都建在计时之前，这样三者才可比
    while (sessions_ready.load() < num_consumers) {
        std::this_thread::yield();
    }

    // 计时开始
    auto start = std::chrono::high_resolution_clock::now();
    go.store(true);

    // 主线程：消费结果并验证
    int verified = 0;
    std::vector<float> first_output;
    int size_mismatch = 0;
    double max_abs_diff = 0.0;
    while (verified < NUM_TASKS) {
        InferenceResult result;
        if (result_queue.Dequeue(result)) {
            if (first_output.empty()) {
                first_output = result.output;
            } else if (result.output.size() != first_output.size()) {
                size_mismatch++;
            } else {
                // 100 个任务喂同一份输入，输出必须逐位相同——跨线程串用 context/缓冲/流 只在这露出来
                for (size_t i = 0; i < first_output.size(); ++i) {
                    double d = std::fabs(static_cast<double>(result.output[i]) -
                                         static_cast<double>(first_output[i]));
                    if (d > max_abs_diff)
                        max_abs_diff = d;
                }
            }
            verified++;
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();
    for (auto &t : consumers)
        t.join();

    // 计时结束
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    // 检查最终状态
    if (processed_count.load() != NUM_TASKS) {
        printf("WARNING: processed %d / %d\n", processed_count.load(), NUM_TASKS);
    }

    // 一份引擎 + N 个 Session：引擎的加载代价只在主线程发生过一次
    printf("Engine: 1 instance shared by %d sessions\n", num_consumers);
    double infer_total = 0.0;
    for (int c = 0; c < num_consumers; ++c) {
        printf("  [t%d] session %.2f ms, %d tasks, infer %.1f ms (%.2f ms/task)\n", c, setup_ms[c],
               thread_tasks[c], infer_ms[c],
               thread_tasks[c] > 0 ? infer_ms[c] / thread_tasks[c] : 0.0);
        infer_total += infer_ms[c];
    }
    // 三个线程各自累计后相加：并发跑时它会大于墙钟，比值就是并行的重叠程度
    printf("Infer total: %.1f ms over %d tasks (%.2f ms/task, sum of threads)\n", infer_total,
           NUM_TASKS, infer_total / NUM_TASKS);

    bool success = (processed_count.load() == NUM_TASKS && !first_output.empty() &&
                    size_mismatch == 0 && max_abs_diff == 0.0);

    printf("Output shape: [%d, 1000]  (%zu floats)\n", BATCH, first_output.size());
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
