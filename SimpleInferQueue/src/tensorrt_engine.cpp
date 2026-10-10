#include "tensorrt_engine.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// TensorRT 的日志接口，必须实现
class TRTLogger : public nvinfer1::ILogger {
  public:
    void log(Severity severity, const char *msg) noexcept override {
        if (severity == Severity::kERROR) {
            std::cerr << "[TRT ERROR] " << msg << std::endl;
        }
    }
};

// 全局唯一 logger 实例
TRTLogger g_logger;

// 统一失败出口：库代码不调 exit()，交给调用方决定怎么处理
[[noreturn]] void fail(const std::string &what) { throw std::runtime_error(what); }

void checkCuda(cudaError_t err, const char *what) {
    if (err != cudaSuccess) {
        fail(std::string(what) + " 失败: " + cudaGetErrorString(err));
    }
}

// 动态 shape 的维度会是 -1；负值乘进 size_t 会绕成极大的数，
// 最后以「cudaMalloc 显存不足」这种看不懂的形式暴露出来，所以提前拒绝
bool isStaticShape(const nvinfer1::Dims &d) {
    for (int i = 0; i < d.nbDims; ++i) {
        if (d.d[i] < 0) {
            return false;
        }
    }
    return true;
}

size_t elementCount(const nvinfer1::Dims &d) {
    size_t n = 1;
    for (int i = 0; i < d.nbDims; ++i) {
        n *= static_cast<size_t>(d.d[i]);
    }
    return n;
}

} // namespace

// ============================ TensorRTEngine ============================

TensorRTEngine::TensorRTEngine(const std::string &engine_path) {
    // 构造函数抛异常时析构函数**不会**执行，所以中途失败必须自己把已取得的
    // 句柄释放掉，否则就是泄漏。下面整段工作都包在 try 里
    try {
        // 1、读取 engine 文件到内存
        std::ifstream file(engine_path, std::ios::binary);
        if (!file.good()) {
            fail("打不开 engine 文件: " + engine_path);
        }

        file.seekg(0, std::ios::end);
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            fail("engine 文件为空或不可读: " + engine_path);
        }
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(static_cast<size_t>(size));
        if (!file.read(buffer.data(), size)) {
            fail("读取 engine 文件失败: " + engine_path);
        }

        // 2、创建 runtime，它是整个 TensorRT 的入口对象
        runtime_ = nvinfer1::createInferRuntime(g_logger);
        if (!runtime_) {
            fail("createInferRuntime 返回空——通常是 nvinfer 的 DLL 没能加载，"
                 "或 TRT 版本与头文件不匹配");
        }

        // 3、反序列化 engine，把二进制变成可执行的 engine 对象
        // 失败会返回 nullptr（引擎与运行时版本不匹配、文件损坏、加载到错的
        // nvinfer_10.dll），必须判空后再用
        engine_ = runtime_->deserializeCudaEngine(buffer.data(), static_cast<size_t>(size));
        if (!engine_) {
            fail("反序列化 engine 失败: " + engine_path +
                 "（引擎与当前 TensorRT 版本不匹配，或加载到了错误的 nvinfer DLL）");
        }

        // 4、查询 I/O 张量。
        // 本封装只支持单输入单输出：旧写法是无条件覆盖赋值，遇到多输入时
        // 会静默只留下最后遍历到的那个，第一个被丢掉且事后查不出来
        int nb_inputs = 0;
        int nb_outputs = 0;
        const int nb_tensors = engine_->getNbIOTensors();
        for (int i = 0; i < nb_tensors; i++) {
            const char *name = engine_->getIOTensorName(i);
            const auto mode = engine_->getTensorIOMode(name);
            if (mode == nvinfer1::TensorIOMode::kINPUT) {
                if (nb_inputs == 0) {
                    input_name_ = name;
                }
                nb_inputs++;
            } else if (mode == nvinfer1::TensorIOMode::kOUTPUT) {
                if (nb_outputs == 0) {
                    output_name_ = name;
                }
                nb_outputs++;
            }
        }

        if (nb_inputs != 1 || nb_outputs != 1) {
            fail("只支持单输入单输出的引擎，当前引擎有 " + std::to_string(nb_inputs) + " 个输入、" +
                 std::to_string(nb_outputs) + " 个输出");
        }

        // 5、校验 I/O 数据类型。
        // infer() 的接口是 float*，只对 FP32 binding 成立。TensorRT 用 --fp16 构建时
        // 权重走 FP16、I/O binding 通常仍是 FP32，但这不是保证：若 binding 真是 FP16，
        // 按 sizeof(float) 算出来的字节数就是错的，更糟的是把 FP32 位模式当 FP16 解释，
        // 数值会静默出错。这里明确失败，而不是默认「应该是 FP32 吧」
        for (const auto &kv : {std::make_pair(input_name_.c_str(), "输入"),
                               std::make_pair(output_name_.c_str(), "输出")}) {
            const auto dtype = engine_->getTensorDataType(kv.first);
            if (dtype != nvinfer1::DataType::kFLOAT) {
                fail(std::string(kv.second) + "张量 \"" + kv.first +
                     "\" 的数据类型不是 FP32（dtype=" + std::to_string(static_cast<int>(dtype)) +
                     "），本封装的 infer() 只接受 float*");
            }
        }

        // 6、查询张量形状（元素个数）
        const auto in_dims = engine_->getTensorShape(input_name_.c_str());
        const auto out_dims = engine_->getTensorShape(output_name_.c_str());

        if (!isStaticShape(in_dims) || !isStaticShape(out_dims)) {
            fail("引擎含动态维度（-1），本封装只支持全静态 shape 的引擎");
        }

        input_size_ = elementCount(in_dims);
        output_size_ = elementCount(out_dims);
    } catch (...) {
        release();
        throw;
    }
}

void TensorRTEngine::release() noexcept {
    // 逆创建顺序释放
    if (engine_) {
        delete engine_;
        engine_ = nullptr;
    }
    if (runtime_) {
        delete runtime_;
        runtime_ = nullptr;
    }
}

TensorRTEngine::~TensorRTEngine() { release(); }

// =========================== TensorRTSession ============================

TensorRTSession::TensorRTSession(std::shared_ptr<TensorRTEngine> engine)
    : engine_(std::move(engine)) {
    if (!engine_) {
        fail("TensorRTSession 收到空的 engine");
    }

    try {
        // 1、创建执行上下文，context 是执行推理的现场
        context_ = engine_->handle()->createExecutionContext();
        if (!context_) {
            fail("createExecutionContext 返回空");
        }

        const size_t in_bytes = engine_->input_size() * sizeof(float);
        const size_t out_bytes = engine_->output_size() * sizeof(float);

        // 2、分配设备显存。缓冲区归本 Session 独占，构造时分配一次、
        // 之后每次推理复用，不需要按推理次数反复分配
        checkCuda(cudaMalloc(&d_input_, in_bytes), "cudaMalloc(输入)");
        checkCuda(cudaMalloc(&d_output_, out_bytes), "cudaMalloc(输出)");

        // 3、创建本 Session 专属的 CUDA 流
        checkCuda(cudaStreamCreate(&stream_), "cudaStreamCreate");

        // 4、绑定张量地址。缓冲区在本 Session 生命周期内不变，
        // 所以绑定一次就够——放在这里可以省掉每次推理的两次驱动调用
        if (!context_->setTensorAddress(engine_->input_name().c_str(), d_input_)) {
            fail("setTensorAddress(输入) 失败");
        }
        if (!context_->setTensorAddress(engine_->output_name().c_str(), d_output_)) {
            fail("setTensorAddress(输出) 失败");
        }
    } catch (...) {
        release();
        throw;
    }
}

void TensorRTSession::release() noexcept {
    if (stream_) {
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }
    if (d_input_) {
        cudaFree(d_input_);
        d_input_ = nullptr;
    }
    if (d_output_) {
        cudaFree(d_output_);
        d_output_ = nullptr;
    }
    if (context_) {
        delete context_;
        context_ = nullptr;
    }
}

TensorRTSession::~TensorRTSession() { release(); }

void TensorRTSession::infer(const float *input, float *output) {
    const size_t in_bytes = engine_->input_size() * sizeof(float);
    const size_t out_bytes = engine_->output_size() * sizeof(float);

    // 1、输入数据从 CPU 异步拷贝到 GPU
    checkCuda(cudaMemcpyAsync(d_input_, input, in_bytes, cudaMemcpyHostToDevice, stream_),
              "cudaMemcpyAsync(输入 H2D)");

    // 2、执行推理。enqueueV3 返回 bool，失败时（binding 未设、shape 不匹配、
    // kernel 启动失败）必须显式报错——否则 output 里留下的是上一次的数据，
    // 调用方完全无从知道这次推理失败了
    if (!context_->enqueueV3(stream_)) {
        fail("enqueueV3 执行失败");
    }

    // 3、输出从 GPU 异步拷贝回 CPU
    checkCuda(cudaMemcpyAsync(output, d_output_, out_bytes, cudaMemcpyDeviceToHost, stream_),
              "cudaMemcpyAsync(输出 D2H)");

    // 4、等待流上所有操作完成，保证这一步 output 里的数据是有效的
    checkCuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize");
}
