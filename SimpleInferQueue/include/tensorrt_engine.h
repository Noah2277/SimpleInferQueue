#pragma once
#include <string>
#include <vector>
#include <NvInfer.h>
#include <cuda_runtime.h>

class TensorRTEngine {
public:
    TensorRTEngine(const std::string& engine_path);
    ~TensorRTEngine();

    //执行一次推理
    //input:输入数据（CPU端）
    //output:输出缓冲区（CPU端，调用者分配）
    void infer(const float* input, float* output);

    //查询输入大小
    size_t input_size() const { return input_size_; }
    size_t output_size() const { return output_size_; }

private:
    nvinfer1::IRuntime* runtime_ = nullptr;
    nvinfer1::ICudaEngine* engine_ = nullptr;
    nvinfer1::IExecutionContext* context_ = nullptr;

    void* d_input_ = nullptr;
    void* d_output_ = nullptr;
    cudaStream_t stream_ = nullptr;

    std::string input_name_;
    std::string output_name_;
    size_t input_size_ = 0;
    size_t output_size_ = 0;
};