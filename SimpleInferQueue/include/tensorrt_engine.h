#pragma once

#include <NvInfer.h>
#include <cuda_runtime.h>
#include <memory>
#include <string>

// ============================ TensorRTEngine ============================
// 引擎本身：持有 IRuntime + ICudaEngine，负责反序列化与元信息查询。
//
// 线程模型：构造完成后**只读**，可以被多个线程共享——这是 TensorRT 官方推荐的用法
// （引擎持有权重，复制它既费内存又拿不到吞吐）。
//
// 因此这里把拷贝和移动**全部显式删除**：类里握着 runtime/engine 两个裸句柄，
// 若允许隐式拷贝，两个对象析构时会对同一批句柄重复释放。注意只声明析构函数
// 并不足以禁止拷贝——它只抑制隐式 move，**隐式 copy 仍然会被生成**，
// 所以下面四行的 = delete 必须写全。
//
// 失败策略：一律抛 std::runtime_error。引擎路径写错、nvinfer DLL 版本不匹配
// 这类问题应该由调用方决定怎么处理（降级、换模型、报错退出），
// 库代码自己 exit 会连带杀掉整个进程，无法恢复也无法单测。
class TensorRTEngine {
  public:
    explicit TensorRTEngine(const std::string &engine_path);
    ~TensorRTEngine();

    TensorRTEngine(const TensorRTEngine &) = delete;
    TensorRTEngine &operator=(const TensorRTEngine &) = delete;
    TensorRTEngine(TensorRTEngine &&) = delete;
    TensorRTEngine &operator=(TensorRTEngine &&) = delete;

    nvinfer1::ICudaEngine *handle() const { return engine_; }

    const std::string &input_name() const { return input_name_; }
    const std::string &output_name() const { return output_name_; }

    // 元素个数（不是字节数）
    size_t input_size() const { return input_size_; }
    size_t output_size() const { return output_size_; }

  private:
    void release() noexcept;

    nvinfer1::IRuntime *runtime_ = nullptr;
    nvinfer1::ICudaEngine *engine_ = nullptr;

    std::string input_name_;
    std::string output_name_;
    size_t input_size_ = 0;
    size_t output_size_ = 0;
};

// =========================== TensorRTSession ============================
// 一次推理的「现场」：IExecutionContext + CUDA 流 + 本线程独占的输入/输出显存。
//
// 线程模型：**每个线程一个，不可共享**。IExecutionContext 不是线程安全的，
// 这里的 d_input_/d_output_/stream_ 也都是独占资源。多个线程共用一个 Session
// 会产生数据竞争（输出互相覆盖、流被并发使用）。
//
// 典型用法：
//     auto engine = std::make_shared<TensorRTEngine>(path);   // 一次
//     // 每个工作线程里各建一个：
//     TensorRTSession session(engine);
//     session.infer(input, output);
class TensorRTSession {
  public:
    explicit TensorRTSession(std::shared_ptr<TensorRTEngine> engine);
    ~TensorRTSession();

    TensorRTSession(const TensorRTSession &) = delete;
    TensorRTSession &operator=(const TensorRTSession &) = delete;
    TensorRTSession(TensorRTSession &&) = delete;
    TensorRTSession &operator=(TensorRTSession &&) = delete;

    size_t input_size() const { return engine_->input_size(); }
    size_t output_size() const { return engine_->output_size(); }

    // 执行一次推理（同步）。input/output 是 CPU 端缓冲区，
    // 长度须分别不小于 input_size()/output_size()。返回时 output 已填好。
    // 失败抛 std::runtime_error——不再静默产出上一次的陈旧数据。
    void infer(const float *input, float *output);

  private:
    void release() noexcept;

    std::shared_ptr<TensorRTEngine> engine_;
    nvinfer1::IExecutionContext *context_ = nullptr;
    void *d_input_ = nullptr;
    void *d_output_ = nullptr;
    cudaStream_t stream_ = nullptr;
};
