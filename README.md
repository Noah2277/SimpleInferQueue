# SimpleInferQueue

![CI](https://github.com/Noah2277/SimpleInferQueue/actions/workflows/ci.yml/badge.svg)

一个从零构建的 C++ 高性能线程安全任务队列，面向 AI 推理场景设计。支持互斥锁与无锁（Lock-Free）两种并发模型，涵盖环形缓冲区、阻塞队列、Michael-Scott 无锁队列等核心组件，并附带模拟推理服务演示，以及 7 个逐步演进的推理流水线示例（4 个 CUDA 基础版本 → ONNX Runtime 真实模型 → TensorRT 两条路径）。运行时仅依赖 C++17 标准库；测试使用 GoogleTest（构建时通过 FetchContent 自动拉取）。

## 项目特性

- **两种并发模型**：互斥锁 + 条件变量（`BlockQueue`），与无锁队列（`LockFreeQueue` / `MSQueue`），适配不同场景
- **Michael-Scott 无锁队列**：MPMC（多生产者-多消费者），链表 + 哑节点设计
- **完整测试与基准**：GoogleTest 单元测试 + Google Benchmark 性能基准；CI 三平台（Linux / Windows / macOS）+ ThreadSanitizer 数据竞争检测 + clang-format 全树自动验证 + `find_package` 下游消费回归
- **GPU 流水线示例**：4 个 CUDA 演进版本（同步基线 → 缓冲池 → 多流 → 双向流水线），附本机实测性能对照
- **真实模型推理**：v5 把模拟负载换成 ONNX Runtime + MobileNetV2，可切换 CPU / CUDA 执行提供程序、对比不同 batch，附实测数据与取舍分析
- **TensorRT 两条路径**：v6 走 ONNX Runtime + TensorRT EP，v7 用裸 TensorRT API 自行封装（引擎共享 + 每线程 Session），附「封装零开销」的对照测量与数据流分析
- **教学友好**：核心算法均配步骤注释，适合学习并发编程与无锁编程
- **工程规范**：clang-format 统一代码风格，install 规则支持 `find_package` 下游消费
- **开源协议**：MIT License，可自由使用、修改与分发

## 快速上手

```cpp
#include "ms_queue.h"

MSQueue<int> q;
q.Enqueue(42);

int v = 0;
q.Dequeue(v);  // v == 42
```

完整构建/安装方法见下文「构建与运行」「安装与下游使用」。

## 组件与适用场景

线程模型是各组件最关键的契约，**使用前务必确认**：

| 组件 | 线程模型 | 生产者 | 消费者 | 阻塞 | 容量 | 说明 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `RingBuffer` | 非线程安全 | - | - | 否 | 固定 | 基础环形缓冲区，多线程下需自行加锁 |
| `BlockQueue` | 互斥锁 + 条件变量 | 多 | 多 | `Push`/`Pop` 阻塞，`try_*` 非阻塞 | 固定 | 通用阻塞队列；`Close()` 可唤醒所有阻塞线程，`Pop` 返回 `false` 即「已关闭且已取空」 |
| `LockFreeQueue` | 原子 load/store 无锁 | **单** | **单** | 否 | 固定 | **仅 SPSC 安全**；环形缓冲 + acquire-release 内存序，**不含 CAS**。多生产者/多消费者请用 `MSQueue` |
| `MSQueue` | Michael-Scott 无锁（CAS） | 多 | 多 | 否 | 无界 | MPMC；**内存随处理量单调增长**，不适合长时间运行的服务（见下文设计要点） |

## 项目结构

```
SimpleInferQueue/
├── include/
│   ├── ring_buffer.h                    # 环形缓冲区（非线程安全）
│   ├── block_queue.h                    # 互斥锁阻塞队列
│   ├── lockfree_queue.h                 # SPSC 无锁环形队列（原子 load/store + acquire-release，无 CAS）
│   ├── ms_queue.h                       # Michael-Scott 无锁队列（MPMC，链表+哑节点，CAS）
│   └── tensorrt_engine.h                # TensorRT 封装：TensorRTEngine（引擎，可共享）+ TensorRTSession（每线程，可选构建，v7 使用）
├── src/
│   └── tensorrt_engine.cpp              # 上述封装的实现（可选构建）
├── benchmarks/
│   ├── bench_block_queue.cpp            # BlockQueue SPSC 吞吐基准（Google Benchmark）
│   ├── bench_lockfree_queue.cpp         # LockFreeQueue SPSC 吞吐基准
│   └── bench_ms_queue.cpp               # MSQueue 4P×4C MPMC 吞吐基准
├── examples/
│   ├── demo_producer_consumer.cpp       # 阻塞队列多线程演示
│   ├── mock_inference.cpp               # 模拟AI推理服务集成演示
│   └── cuda_pipeline/                   # CUDA GPU 流水线示例（可选构建）
│       ├── v1_sync_baseline.cu          # v1 同步基线（默认流 + 同步拷贝，无池）
│       ├── v2_mempool_sync.cu           # v2 显存缓冲池 + 回收线程（单消费者同步）
│       ├── v3_multistream_pool.cu       # v3 多流全异步 + 缓冲池
│       ├── v4_bidirectional.cu          # v4 双向流水线（生产/消费/输出/回收）
│       ├── v5_onnx_inference.cu         # v5 真实模型推理（ONNX Runtime + MobileNetV2，可选构建）
│       ├── v6_trt_inference.cu          # v6 ONNX Runtime + TensorRT EP（可选构建）
│       └── v7_trt_native.cu             # v7 裸 TensorRT API 自封装（读 models/*.engine，可选构建）
├── models/                              # ONNX 模型导出脚本（Python/PyTorch；产出的 *.onnx 不入库）
│   ├── export_mobilenet.py              # MobileNetV2，batch = 1
│   ├── export_mobilenet_batch32.py      # MobileNetV2，batch = 32
│   ├── export_mobilenet_batch64.py      # MobileNetV2，batch = 64
│   └── export_model.py                  # 4→2 定值权重小模型（供数值验证，期望输出可手算）
├── tests/
│   ├── test_ring_buffer.cpp             # 环形缓冲区单元测试（FIFO/边界/回绕）
│   ├── test_block_queue.cpp             # 阻塞队列测试（try 接口 + 多线程）
│   ├── test_lockfree_queue.cpp          # 无锁队列测试（容量边界 + SPSC）
│   └── test_ms_queue.cpp                # MS队列测试（FIFO + 4P×4C 不重不漏）
├── cmake/
│   └── SimpleInferQueueConfig.cmake.in  # 包配置模板（供 find_package 使用）
├── .github/workflows/ci.yml             # 三平台 CI + TSAN 数据竞争检测 + clang-format 检查
├── .clang-format
├── LICENSE                              # MIT License
├── CMakePresets.json                    # 跨机器可复用的 configure/build/test 预设
└── CMakeLists.txt
```

## 核心技术点

- **环形缓冲区**: 预分配内存，减少动态分配，提升缓存局部性。
- **互斥锁与条件变量**: `std::mutex` + `std::condition_variable` 实现线程安全的阻塞队列。
- **无锁编程**: 基于 `std::atomic` 与 Acquire-Release 内存序实现生产者-消费者无锁队列。两条路子不同——`LockFreeQueue` 是 SPSC 环，靠原子 load/store 就够，**一次 CAS 都不用**；`MSQueue` 是 MPMC 链表，才是真正的 CAS 主场。
- **Michael-Scott 无锁队列**: 链表 + 哑节点 + head/tail 双原子指针，CAS + Acquire-Release 内存序实现 MPMC 入队/出队，"互助推进"机制保证无锁下的高效协作。
- **优雅退出**: `BlockQueue::Close()` 唤醒所有阻塞中的线程，`Pop` 返回 `false` 表示「已关闭且已取空」——消费者不必靠「固定循环 N 次」或原子标志位猜时机。

## MSQueue 设计要点

- **哑节点**: 队列始终保留一个不含数据的占位节点，head 指向它——"空队列"被化简成普通状态，入队/出队永远不需要处理空指针
- **入队两步走**: 先 CAS 把新节点挂到 `tail->next`（真正入队），再尝试推进 `tail`；推进失败没关系，其他线程会"互助推进"
- **出队**: CAS 推进 `head`，成功者取走值；当 `head == tail` 但队列非空时，先帮忙推进 `tail` 以维持"tail 不落后于 head"的不变量
- **内存回收**: 当前版本不释放被出队的节点（规避无锁内存回收难题），析构函数仅回收仍在队列中的节点。**代价要说清楚：内存随处理量单调增长，与队列当前长度无关**——跑 100 万个任务就欠下 100 万个节点，即使队列早已空了。真要解决得上 hazard pointer / epoch-based reclamation 或换有界 MPMC 环（Vyukov），因此**本组件不适合长时间运行的服务**

## 构建与运行

### 前置要求

- CMake ≥ 3.16
- 支持 C++17 的编译器（MSVC / GCC / Clang）
- CUDA Toolkit（可选，仅构建 `examples/cuda_pipeline/` 的 GPU 流水线示例时需要；未安装时 CMake 自动跳过，不影响其余部分）
- ONNX Runtime（可选，构建 `v5_onnx_inference.cu` / `v6_trt_inference.cu` 时需要）。查找顺序：`ONNXRUNTIME_ROOT` 环境变量 → `third_party/onnxruntime` → 都没命中则这两个示例自动跳过，不影响其余部分；也可用 `-DONNXRUNTIME_ROOT=<ORT 解压根目录>` 直接指定
- TensorRT（可选，构建 `v6_trt_inference.cu` / `v7_trt_native.cu` 时需要；未命中时这两个示例自动跳过）。查找顺序：`TENSORRT_ROOT` 环境变量 → `-DTENSORRT_ROOT=<TRT 解压根目录>`。**两条路径的依赖性质不同**：v6 走 ORT 的 TRT EP，编译期完全不需要 TRT（只要运行期能加载 `nvinfer_10.dll`）；v7 直接调原生 API，`NvInfer.h` 和 `nvinfer_10.lib` 都是**编译期硬依赖**
- 首次构建会通过 FetchContent 从 GitHub 下载 GoogleTest 与 Google Benchmark 源码（仅测试/基准需要，可分别用 `-DSIMPLEINFERQUEUE_BUILD_TESTS=OFF` / `-DSIMPLEINFERQUEUE_BUILD_BENCHMARKS=OFF` 关闭；CUDA 示例可用 `-DSIMPLEINFERQUEUE_BUILD_CUDA_EXAMPLES=OFF` 关闭）

### 构建

```bash
# 在仓库根目录执行（Linux / macOS / Windows PowerShell 通用）
cmake -S SimpleInferQueue -B build
cmake --build build --config Release
```

> **Windows 注意**：项目/构建路径若含非 ASCII 字符（如中文），gtest 的测试发现（`gtest_discover_tests`）会因 MSVC CRT 与 CMake 的编码不一致而失败。请把构建目录放到纯 ASCII 路径：
>
> ```powershell
> cmake -S SimpleInferQueue -B D:\build\SimpleInferQueue
> cmake --build D:\build\SimpleInferQueue --config Release
> ctest --test-dir D:\build\SimpleInferQueue -C Release --output-on-failure
> ```

> **离线/网络不稳时**：GoogleTest 首次下载失败，可预先下载源码后通过 FetchContent 本地源覆盖，跳过网络下载：
>
> ```powershell
> cmake -S SimpleInferQueue -B D:\build\SimpleInferQueue `
>       -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=D:\deps\googletest-1.16.0
> ```

### 运行测试

```bash
ctest --test-dir build -C Release --output-on-failure
```

构建出的示例程序（`build/Release/` 或 `build/` 下）：

```bash
./demo_pc          # 阻塞队列多线程演示
./mock_inference   # 模拟AI推理服务演示
./siq_benchmarks   # 性能基准（Google Benchmark）

# CUDA 流水线示例（仅在检测到 CUDA Toolkit 时构建，详见下文「CUDA 流水线示例」）
./v1_sync_baseline  ./v2_mempool_sync  ./v3_multistream_pool  ./v4_bidirectional

# v5 真实模型推理（还需 ONNX Runtime，详见下文「v5：接入 ONNX Runtime 的真实模型推理」）
./v5_onnx_inference --gpu     # 默认走 CUDA；--cpu 切换到 CPU 执行提供程序

# v6/v7 走 TensorRT（详见下文「v6 / v7：接 TensorRT 的两条路」）
./v6_trt_inference --gpu --consumers=3   # ONNX Runtime + TensorRT EP；--consumers 可改消费者线程数
./v7_trt_native "" 3                     # 裸 TensorRT API；argv[1]=引擎路径（空串用默认），argv[2]=消费者线程数
                                         # 需先用 trtexec 导出 models/*.engine
```

## 安装与下游使用

```bash
# 安装到指定前缀
cmake --install build --prefix /path/to/install
```

下游 CMake 工程：

```cmake
find_package(SimpleInferQueue CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE SimpleInferQueue::SimpleInferQueue)
```

头文件直接 `#include "ms_queue.h"` 等即可使用。

**这条链路有 CI 守着**：`tests/consumer/` 是一个独立的最小下游工程，CI 的 `install-consumer`
job 会先把库装到临时 prefix，再用它 `find_package` 回来、编译、并真的跑一遍。本地手动复现：

```bash
cmake -S SimpleInferQueue -B build-install -DCMAKE_INSTALL_PREFIX=/tmp/siq-prefix
cmake --build build-install && cmake --install build-install

cmake -S SimpleInferQueue/tests/consumer -B build-consumer \
  -DCMAKE_PREFIX_PATH=/tmp/siq-prefix
cmake --build build-consumer && ./build-consumer/siq_consumer   # 应输出 consumer ok: 42
```

> **为什么值得专门为它加一个 job**：包导出坏掉（比如 `INTERFACE_INCLUDE_DIRECTORIES` 被
> 静默丢弃）这种问题，**库自己构建得再好也照样全绿**——只有真的「装一遍、再消费一遍」才会
> 暴露。这个 bug 在本仓库里活了很久，根因就是当时没有任何自动化测试覆盖这条链路。

## 代码格式

项目使用 clang-format（基于 LLVM 风格，4 空格缩进），配置见仓库根目录 `.clang-format`。CI 会检查格式，违规会导致构建失败。

**CI 扫描整个源码树**：`include/` / `src/` / `examples/` / `tests/` / `benchmarks/` 下的 `*.h`、`*.cpp`、`*.cu` 全在内。`src/` 与 `*.cu` 曾经是盲区——`src/tensorrt_engine.cpp` 当初有 108 处违规却一直没人发现，只因 `find` 没扫它，所以现在一并纳入。

```bash
# 格式化全部源码（版本需与 CI 一致：18.1.8）
find SimpleInferQueue/include SimpleInferQueue/src SimpleInferQueue/examples \
     SimpleInferQueue/tests SimpleInferQueue/benchmarks \
     \( -name '*.h' -o -name '*.cpp' -o -name '*.cu' \) \
  | xargs clang-format -i

# 本地检查是否合规
clang-format --dry-run --Werror <file>
```

> **注意 `tests/consumer/main.cpp` 是纯 ASCII 的**，与仓库其余部分不同——这是有意的，原因见该目录的 `CMakeLists.txt`：下游工程必须能用默认编译设置消费本库，不能要求它也加 `/utf-8`。

## 性能基准

基准使用 [Google Benchmark](https://github.com/google/benchmark)（构建时通过 FetchContent 自动拉取 v1.9.5），全部基准编译为一个可执行文件：

```bash
./siq_benchmarks                                         # 运行全部基准
./siq_benchmarks --benchmark_filter=MSQueue              # 只跑名称匹配的基准
./siq_benchmarks --benchmark_out=result.json --benchmark_out_format=json
# 导出 JSON 后可用官方 compare.py 对比改动前后的结果，量化每次优化的收益
```

> **CI 只编译基准、不运行**：共享 runner 上计时噪音大，跑出来的数字没有参考价值。CI 保证基准代码「能编译、不腐烂」，真实数据请在本机测量。

本机测试结果（Windows / MSVC Release / 24 核，队列容量 4096）。**每种配置取 12~16 组样本的中位数**——单次运行的热漂移能到 5%，只跑一次不足以支撑后面那些结论：

| 实现 | 场景 | 单操作耗时 | 吞吐量 |
| :--- | :--- | :--- | :--- |
| BlockQueue（互斥锁） | 1P×1C | **46.0** ns | **2176万** ops/s |
| LockFreeQueue（无锁） | 1P×1C | **19.5** ns | **5141万** ops/s |
| MSQueue（Michael-Scott 无锁） | 4P×4C | **740** ns | **135万** ops/s |

- SPSC 场景：无锁队列吞吐约为有锁队列的 **2.36 倍**
- MSQueue 单操作开销远高于 SPSC——**8 个线程竞争同一队列时 CAS 反复失败重试，这正是「无锁不等于免费」**：锁的开销换成了缓存行乒乓与 CAS 竞争开销，且出队节点暂不回收、内存持续占用
- **`BlockQueue` 比上一版慢了约 16%，这是为「支持 move-only 类型 + close 语义」付的账**。四个容器都改成用 `std::optional<T>` 槽位，`Pop` 变成「移动出来 + `reset()` 槽位」，`try_Push`/`try_Pop` 的谓词里还多一个 `closed_` 判断。这份簿记开销**与 `sizeof(T)` 无关**，而本基准的 `T` 就是 `int`，所以占比最大。分开量过（同一台机器、先 stash 回旧实现再测）：`BlockQueue` 39.8 → 46.0 ns，两组样本区间 **37.9~42.1 对 45.0~47.6 完全不重叠，是真回归**；`LockFreeQueue` 与 `MSQueue` 的区间大幅重叠，**看不出确定变化**。换成真实任务类型（`vector`、结构体）时这份固定开销的占比会小得多。
- 上面三行是**重新测过**的：容器内部改完，旧数字就作废了——顺带说明基准表为什么必须标明测量时间，否则读者无从判断它对应哪一版代码。

## CUDA 流水线示例：从同步到双向流水线

`examples/cuda_pipeline/` 下的 v1~v4 是 4 个逐步演进的 CPU-GPU 流水线版本，展示推理场景中从「能用」到「生产级」的优化路径。构建时 CMake 自动探测 CUDA Toolkit：本机安装则编译，未安装（如 CI 的无 GPU runner）则静默跳过。

> v1~v4 用向量加法**模拟**推理负载，重点在流水线编排；接真实模型的是 v5，见下文「v5：接入 ONNX Runtime 的真实模型推理」。

测试环境：RTX 5060 Laptop GPU / CUDA 12.8 / Windows MSVC Release / 100 个任务 × 65536 元素向量加法 / 缓冲池 4 块。

| 版本 | 设计 | 总耗时 | 吞吐量 | 相对 v1 |
| :--- | :--- | :--- | :--- | :--- |
| v1 同步基线 | 默认流 + 同步拷贝，一次一个任务 | 134.86 ms | 741.5 tasks/s | 1.00× |
| v2 缓冲池 | 显存池复用 + 回收线程（单消费者，同步拷贝） | 34.27 ms | 2918.3 tasks/s | **3.94×** |
| v3 多流全异步 | 生产者/消费者专用流 + 全异步拷贝 | 19.02 ms | 5259.0 tasks/s | **7.09×** |
| v4 双向流水线 | 3 消费者 + 输出线程 + 回收线程，双重背压 | 27.85 ms | 3591.1 tasks/s | **4.84×** |

### 演进分析

- **v1 → v2（3.94×）**：缓冲池复用显存，消除每个任务的 `cudaMalloc`/`cudaFree`；回收线程把「等 GPU 用完块」的同步移出主路径
- **v2 → v3（1.80×）**：生产者、消费者各自专用流，绕开 legacy 默认流「一条流阻塞所有流」的全局同步；拷贝全异步化，CPU 不再空等 GPU
- **v3 → v4（吞吐量下降约 32%）**：新增输出线程与逐任务事件同步、有界队列双重背压（池空则生产者阻塞、回收慢则消费者阻塞）——显存占用有上界、结果顺序有保证。**性能不是唯一目标**：v4 用约三成峰值吞吐换来了生产级的资源可控性，这正是推理服务（如 vLLM、TensorRT 流水线）的真实取舍；追求极限吞吐选 v3 形态，追求稳健可控选 v4 形态

### 构建与运行

```bash
# CMake 自动探测 CUDA，examples/cuda_pipeline/ 下每个 .cu 生成一个可执行目标
cmake -S SimpleInferQueue -B build
cmake --build build --config Release --target v4_bidirectional   # 换成其他目标名即可编译对应版本
```

> **Windows 注意**：与前文「构建」一节相同——项目路径含中文时请改用纯 ASCII 构建目录（如 `D:\build\SimpleInferQueue`），否则 gtest 测试发现会失败。

构建产物（Windows 多配置生成器位于 `Release/` 子目录）：

```bash
./v1_sync_baseline    # v1 同步基线
./v2_mempool_sync     # v2 缓冲池
./v3_multistream_pool # v3 多流全异步
./v4_bidirectional    # v4 双向流水线
```

四个版本输出格式一致（PASS + 耗时 + 吞吐），可直接对比。

## v5：接入 ONNX Runtime 的真实模型推理

v1~v4 的「推理」是向量加法。v5 把这一层换成**真实模型**：ONNX Runtime + MobileNetV2，输入 `[B, 3, 224, 224]`、输出 `[B, 1000]`。结构上仍是「生产者投递 → `MSQueue` → 3 个消费者并发 `session.Run()`」，变的是负载终于真的在算卷积了。

模型需要先用 `models/` 下的脚本从 PyTorch 导出（`*.onnx` 是二进制、不入库）：

```bash
pip install torch torchvision onnx
python models/export_mobilenet_batch64.py     # 生成 models/mobilenet_batch64.onnx
```

### 构建与运行

```bash
cmake -S SimpleInferQueue -B build -DONNXRUNTIME_ROOT=<ORT 解压根目录>
cmake --build build --config Release --target v5_onnx_inference
./v5_onnx_inference --gpu      # 或 --cpu
```

### 三个容易踩的坑

- **模型的 batch 必须和源码顶部的 `BATCH` 常量一致**。`BATCH` / `IN_C` / `IN_H` / `IN_W` / `OUT_SIZE` 这组常量与 `models/export_mobilenet_batch*.py` 一一对应，改一边必须改另一边。喂错模型时，`session.Run()` 抛出的形状异常发生在消费者线程里、没人接，会直接 `terminate` 掉整个进程——而 stdout 是块缓冲，`abort` 不走正常退出流程，**连崩溃前已经执行到的日志都一个字看不到**，表现为「静默退出、零输出」。排查这种问题时，先把 stdout 重定向关掉看能不能拿到输出。
- **不要只配 `PATH` 找 ONNX Runtime**。Windows 的 DLL 搜索顺序里 `System32` 优先于 `PATH`，系统里如果装着别的 ONNX Runtime 版本，会被静默加载、然后崩在 API 版本不匹配上。CMake 的 `POST_BUILD` 会把 ORT 的 `.dll` 复制到 exe 同目录——exe 自身目录排在 `System32` 之前，这样才稳。
- **模型路径由 CMake 以绝对路径注入**，不依赖当前工作目录；exe 在 `build/Release/` 下也能直接跑。

### 实测：batch 决定 CPU 和 GPU 谁赢

测试环境：RTX 5060 Laptop / CUDA 12.8 / Windows MSVC Release / ONNX Runtime 1.30.0 / 100 个 task / 3 个消费者线程。

| 模型 batch | 1 个 task | CPU 每张图 | GPU 每张图 |
| :--- | :--- | :--- | :--- |
| 1 | 1 张 | 2.18 ms（458 img/s） | 6.69 ms（150 img/s） |
| 32 | 32 张 | 3.52 ms（284 img/s） | 0.786 ms（1273 img/s） |
| 64 | 64 张 | 3.23 ms（310 img/s） | **0.711 ms（1407 img/s）** |

**必须先换算成「每张图」**：`tasks/s` 在不同 batch 之间不可比（1 个 task 分别是 1 / 32 / 64 张图），直接比会得出完全相反的结论。

- **batch=1 时 CPU 反超 GPU 约 3 倍**。固定开销（H2D 拷贝、kernel launch、同步）远大于 MobileNet 本身的计算量，GPU 大部分时间在等，不在算。
- **batch=32 时 GPU 反超约 4.5 倍**。固定开销摊薄到 32 张图上，这才是 GPU 该有的样子。
- **batch 加到 64，收益明显放缓**（每张图只再降约 10%）。说明 32 之后已经进入算力瓶颈，**再加 batch 不是那个杠杆了**。
- **CPU 恰好相反，batch=1 才是最优**。批处理不会让 CPU 的 SIMD 宽度变宽，却让工作集和中间激活值暴涨、冲垮缓存，反而把并行度吃掉了。
- 3 个消费者线程在 GPU 侧共享同一条 CUDA 流，计算实际被串行化——**GPU 真正吃到的是 batch，不是线程数**。想继续榨 GPU，方向是多流，不是加线程。

> **CPU 和 GPU 的输出不是逐位相同的，这不是 bug。** cuDNN 会按 batch 大小选不同的卷积算法，且默认启用 TF32，尾数精度低于 FP32。实测同一张图，CPU 在 batch 1 / 32 / 64 下输出**逐位一致、完全可复现**，而 GPU 三个 batch **各不相同**。做结果校验时请按 FP32 的容差比，不要期待逐位相等。

> **v5 打印的 `PASS` 只检查「任务数跑满 + 输出维度正确」，不校验数值是否算对。** 想做数值校验，可参考 `models/export_model.py` 的做法：用一组写死的正整数权重、让 ReLU 不触发，期望输出就能手算 `Wx+b`，与推理引擎独立对账——注意 v5 顶部的常量是按 MobileNet 写死的，换模型要同步改那组常量。

## v6 / v7：接 TensorRT 的两条路

v5 末尾的结论是「想继续榨 GPU，方向是多流，不是加线程」。v6 和 v7 是这条线上的两个分叉：

- **v6**：ONNX Runtime 挂 **TensorRT EP**（`trt_fp16_enable=1`）。TRT 在运行时把子图编译成 fp16 引擎并缓存到磁盘，模型仍是 `.onnx`，切图、回退、显存管理都交给 ORT。
- **v7**：绕开 ORT，用**裸 TensorRT API** 自己封装。模型是 `.engine`，封装分两层：`TensorRTEngine` 持有 runtime + engine，**反序列化一次、所有线程共享**；`TensorRTSession` 持有 execution context + 显存 + CUDA 流，**每线程一个**——这正是 TensorRT 官方推荐的用法（`IExecutionContext` 不是线程安全的）。

### 构建与运行

`.engine` 是派生产物、不入库，v7 需要先用 `trtexec` 从 `.onnx` 导出：

```bash
trtexec --onnx=models/mobilenet_batch32.onnx --fp16 \
        --saveEngine=models/mobilenet_batch32_fp16.engine

cmake --build build --config Release --target v6_trt_inference v7_trt_native
```

运行命令见前文「运行测试」一节。**两个示例的消费者线程数都能从命令行改**，这是量「加线程有没有换来吞吐」的正确姿势——同一份二进制，只改线程数：

```bash
./v7_trt_native "" 3          # argv[1]=引擎路径（空串=用 CMake 注入的默认值），argv[2]=消费者线程数
./v6_trt_inference --gpu --consumers=1
```

### 实测：封装零开销，但加线程只有 v6 受益

RTX 5060 Laptop / CUDA 13.2 / TensorRT 10.16.1.11 / MSVC Release / **两者都是 batch 32 fp16、100 个任务**。每格是 5~9 组样本的中位数；**同行三列来自同一份二进制、同一套开销，只有线程数不同**——这样才排除掉「拿本程序的多线程墙钟去比别处的串行基线」这种量法：

| | 单次推理 | 1 消费者 | 3 消费者 | 3 / 1 |
| :--- | :--- | :--- | :--- | :--- |
| v6 ORT + TRT EP | 4.62 ms | 461.6 ms | **425.7 ms** | **1.08×** |
| v7 裸 TensorRT | 4.52 ms | 451.6 ms | 462.3 ms | 0.98× |

**看「单次推理」列**：v7 4.52 ms 对 v6 4.62 ms —— 自己封装的 `TensorRTEngine` + `TensorRTSession` 没引入额外开销，run-to-run 之间符号还会翻，本来就在噪声里。

**看右三列**：v6 加到三个线程快 8%（两组样本区间完全不重叠：453~470 ms 对 412~429 ms，是实打实的收益）；**v7 加了等于白加**（451.6 → 462.3 ms，两组样本区间大幅重叠，差异在噪声内）。

> **这个结论推翻了本项目早先的一个说法。** 上一版 README 把 v7 的并发劣势归因于「`TensorRTEngine` 把 runtime / engine / context 捆成一个对象，每线程一个实例等于把引擎复制了三份」——**这个归因是错的**。v7 已按 TensorRT 官方推荐的「engine 共享、context 每线程一个」重构（`TensorRTEngine` 反序列化一次全局共享，`TensorRTSession` 每线程一个），引擎份数从 3 降到 1，**并发表现没有任何变化**。归因错在哪，见下面两段。

**真正的差别目前没有定论。** 单线程时两者几乎同速（4.52 / 4.62 ms），说明瓶颈是同一块 GPU，差别只出现在并发调度上。至少有两个候选原因，本项目的实验**分不开它们**：

1. **两者跑的根本不是同一份引擎**。v6 的引擎由 ORT 的 TRT EP 在运行时编译，v7 的是 `trtexec --fp16` 提前导出——tactic 选择、kernel 实现都可能不同，8% 完全可能是引擎质量差而非调度差。
2. **两者的并发模型不同**。v6 三线程共用一个 `Ort::Session`，context 池由 ORT 内部管；v7 三个 Session 各自持有独立的 context、显存和 CUDA 流，三条流一起往同一批已经吃满的 SM 上灌。

要分开这两者，需要一个让两条路径加载**同一个 `.engine` 文件**的对照实验——v7 能指定引擎路径，v6 不能（它必须从 `.onnx` 现场编译）。**所以这里只报事实，不报机制。**

> **两个容易量错的地方。** 一是**别拿 v6 和 v7 互比**，各自和串行基线比才有意义。二是**别在 `infer()` 内部掐表算「重叠度」**：把三个线程各自累计的推理时间加起来除以墙钟，v7 三线程量出来是 **2.9**（各线程累计 441/442/443 ms，墙钟 456 ms），看着「并行得挺好」——但那段计时**包含等 GPU 的时间**。同一组数据换个读法就露馅了：三个线程各只做了 1/3 的任务，单任务耗时却从 4.54 ms 涨到 **12.6 ms**（约 2.8×）。每个任务都在排队等另外两个，GPU 是**串着**处理它们的，那个 2.9 倍的比值正是「三个线程分同一条时间线」的算术后果，不是并行产出。

### 数据流还有哪些没吃到的性能

v7 每个任务的时间线是严格串行的，每一段都有硬件在空转：

```
[H2D 1.35ms] → [kernel ~3.1ms] → [D2H 0.04ms] → cudaStreamSynchronize
```

H2D / D2H 是单独用一段只做拷贝的 CUDA 程序量出来的（18.38 MB，100 次取平均）；kernel 那一段是 `infer()` 单次总耗时 4.52 ms 减去两个拷贝后的余量——**注意它同时包含了 kernel 和驱动调用、显存访问的竞争，不是纯粹的 GPU 计算时间**。

| 方向 | 原理 | 理论上限 |
| :--- | :--- | :--- |
| **双缓冲流水** | 拷贝引擎（DMA）和计算单元（SM）是**两块独立硬件**，串行用等于各浪费一半；而流内有序、流间无序，重叠只能跨流 | `1.35 + 3.1` → `max(1.35, 3.1)`，**快约 30%** |
| **放粗同步粒度** | 每任务一次 `cudaStreamSynchronize`，等于「用了异步 API 却立刻同步」——退化成同步 API，还白付驱动调用 | 线程在 D2H 和等待期间不再空转 |
| **CUDA Graph** | 每任务 4 次驱动调用（`enqueueV3` + 拷贝×2 + `cudaStreamSynchronize`；`setTensorAddress`×2 已经挪到 Session 构造里，每 Session 只调一次，不在关键路径上），100 任务 × 3 线程 = 1200 次；录一次重放即可 | 上千次塌缩成「每 Session 一次」 |

后两层省的都只是 **CPU 时间**，而现在 CPU 不是瓶颈（线程大部分时间堵在 `cudaStreamSynchronize` 里）；双缓冲是唯一动 GPU 时序的，但 SM 已经饱和，腾出的 DMA 换不成吞吐——**上一节的实测正是这句话的证据**：三条流都没能换来吞吐，再加一条只会更难。

**真正还能加吞吐的在别处**：用 **pinned 内存**接住每任务那 18.38 MB 的 H2D（实测单次拷贝 **1.35 → 0.72 ms**，省 0.63 ms，按单次推理 4.52 ms 算整次快 **约 17%**）；若上游本来就在显存里（预处理、上一个模型），直接把设备指针交给 `setTensorAddress`，这次拷贝**整个消失**——那才是把 1.35 ms 从关键路径上彻底拿掉。

## 模拟推理服务器运行效果

```
Processing request ID: 1208, Path: image/1208.jpg
Processing request ID: 1501, Path: image/1501.jpg
Processing request ID: 4397, Path: image/4397.jpg
Processing request ID: 7323, Path: image/7323.jpg
...（多请求并发处理，完整输出请运行 mock_inference 查看）
```

## 关于本项目

本项目是作者从零开始学习并发编程、无锁编程与工程构建的实践成果：从有锁到无锁，从基础组件到完整服务，每一步都经过测试验证。

## 知乎账号
https://www.zhihu.com/people/94-16-90-75