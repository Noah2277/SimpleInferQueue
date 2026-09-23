#include "tensorrt_engine.h"
#include <fstream>
#include <iostream>
#include <cassert>

//TensortRT的日志接口，必须实现
class TRTLogger : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* msg) noexcept override {
        if(severity == Severity::kERROR) {
            std::cerr << "[TRT ERROR] " << msg << std::endl;
        }
    }
};

//全局唯一logger实例
static TRTLogger g_logger;

TensorRTEngine::TensorRTEngine(const std::string& engine_path) {
    //1、读取engine文件到内存
    std::ifstream file(engine_path, std::ios::binary);
    if(!file.good()) {
        std::cerr << "Failed to open engine file:" << engine_path <<  std::endl;
        exit(1);
    }

file.seekg(0, std::ios::end);
size_t size = file.tellg();
file.seekg(0, std::ios::beg);
std::vector<char> buffer(size);
file.read(buffer.data(), size);
file.close();

//2.创建runtime，它时整个TensorRT的入口对象
runtime_ = nvinfer1::createInferRuntime(g_logger);

//3.反序列化engine，把二进制变成可执行的engine对象
//失败时安静地返回 nullptr（引擎与运行时版本不匹配、文件损坏、加载到错的
//nvinfer_10.dll），下面那行的 -> 就是空指针解引用，只留下一个退出码
engine_ = runtime_->deserializeCudaEngine(buffer.data(), size);
if(!engine_) {
    std::cerr << "Failed to deserialize engine:" << engine_path << std::endl;
    exit(1);
}

//4.创建执行上下文，context是执行推理的现场
context_ = engine_->createExecutionContext();
if(!context_) {
    std::cerr << "Failed to create execution context" << std::endl;
    exit(1);
}

//5.查询输入张量信息
int nb_tensors = engine_->getNbIOTensors();
for(int i =0; i < nb_tensors; i++) {
    const char* name = engine_->getIOTensorName(i);
    auto mode = engine_->getTensorIOMode(name);
    if(mode == nvinfer1::TensorIOMode::kINPUT) {
        input_name_ = name;
    } else if(mode == nvinfer1::TensorIOMode::kOUTPUT) {
        output_name_ = name;
    }
}

if(input_name_.empty() || output_name_.empty()) {
    std::cerr << "Failed to find input or output tensor" << std::endl;
    exit(1);
}

//6.查询张量大小（元素个数）
auto in_dims = engine_->getTensorShape(input_name_.c_str());
auto out_dims = engine_->getTensorShape(output_name_.c_str());

input_size_ = 1;
for(int i = 0; i < in_dims.nbDims; i++) {
    input_size_ *= in_dims.d[i];
}

output_size_ = 1;
for(int i = 0; i < out_dims.nbDims; i++) {
    output_size_ *= out_dims.d[i];
}

//7.分配设备显存，输入输出在缓冲区构造时分配一次，之后每次推理复用
cudaMalloc(&d_input_, input_size_ * sizeof(float));
cudaMalloc(&d_output_, output_size_ * sizeof(float));

//8.创建CUDA流
cudaStreamCreate(&stream_);

}

// infer：执行一次推理
// input:  CPU 端输入数据指针
// output: CPU 端输出缓冲区指针（调用者负责分配足够空间）

void TensorRTEngine::infer(const float* input, float* output) {
    //1.输入数据从CPU异步拷贝到GPU
    cudaMemcpyAsync(d_input_, input, input_size_ * sizeof(float), cudaMemcpyHostToDevice, stream_);

    //2.告诉TensorRT输入输出张量在显存中的地址
    context_->setTensorAddress(input_name_.c_str(), d_input_);
    context_->setTensorAddress(output_name_.c_str(), d_output_);

    //3.执行推理
    context_->enqueueV3(stream_);

    //4.输出从GPU异步拷贝回CPU
    cudaMemcpyAsync(output, d_output_, output_size_ * sizeof(float), cudaMemcpyDeviceToHost, stream_);

    //5.等待流上所有操作完成，保证这一步output里的数据是有效的
    cudaStreamSynchronize(stream_);
}

TensorRTEngine::~TensorRTEngine() {
    if(stream_) cudaStreamDestroy(stream_);
    if(d_input_) cudaFree(d_input_);
    if(d_output_) cudaFree(d_output_);
    if(context_) delete context_;
    if(engine_) delete engine_;
    if(runtime_) delete runtime_;
}