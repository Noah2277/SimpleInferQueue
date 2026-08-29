# SimpleInferQueue

一个从零构建的 C++ 高性能线程安全任务队列，面向 AI 推理场景设计。支持互斥锁与无锁（Lock-Free）两种并发模型，涵盖环形缓冲区、阻塞队列、Michael-Scott 无锁队列等核心组件，并附带模拟推理服务的完整演示。仅依赖 C++17 标准库，无第三方依赖。

## 项目特性

- **两种并发模型**：互斥锁 + 条件变量，与 CAS 无锁队列，适配不同场景
- **Michael-Scott 无锁队列**：MPMC（多生产者-多消费者），链表 + 哑节点设计
- **完整测试**：单元测试、多线程并发正确性测试、性能基准测试
- **教学友好**：核心算法均配步骤注释，适合学习并发编程与无锁编程

## 项目结构

```
SimpleInferQueue/
├── include/
│   ├── ring_buffer.h          # 环形缓冲区（非线程安全）
│   ├── block_queue.h          # 互斥锁阻塞队列
│   ├── lockfree_queue.h       # CAS无锁队列（原子操作+内存序）
│   └── ms_queue.h             # Michael-Scott 无锁队列（MPMC，链表+哑节点）
├── examples/
│   ├── demo_producer_consumer.cpp  # 阻塞队列多线程演示
│   ├── test_try.cpp                # 非阻塞接口单元测试
│   ├── test_lockfree.cpp           # 无锁队列并发正确性测试
│   ├── test_ms_queue.cpp           # MS队列多线程正确性测试（4生产者×4消费者）
│   ├── bench_blocking.cpp          # 互斥锁队列性能基准测试
│   ├── bench_lockfree.cpp          # 无锁队列性能基准测试
│   └── mock_inference.cpp          # 模拟AI推理服务集成演示
└── CMakeLists.txt
```

## 核心技术点

- **环形缓冲区**: 预分配内存，减少动态分配，提升缓存局部性。
- **互斥锁与条件变量**: `std::mutex` + `std::condition_variable` 实现线程安全的阻塞队列。
- **无锁编程**: 基于 `std::atomic`、CAS、Acquire-Release 内存序，实现生产者-消费者无锁队列。
- **Michael-Scott 无锁队列**: 链表 + 哑节点 + head/tail 双原子指针，CAS + Acquire-Release 内存序实现 MPMC 入队/出队，"互助推进"机制保证无锁下的高效协作。
- **优雅退出**: 通过原子标志位实现生产者和消费者的协调关闭。

## MSQueue 设计要点

- **哑节点**: 队列始终保留一个不含数据的占位节点，head 指向它——"空队列"被化简成普通状态，入队/出队永远不需要处理空指针
- **入队两步走**: 先 CAS 把新节点挂到 `tail->next`（真正入队），再尝试推进 `tail`；推进失败没关系，其他线程会"互助推进"
- **出队**: CAS 推进 `head`，成功者取走值；当 `head == tail` 但队列非空时，先帮忙推进 `tail` 以维持"tail 不落后于 head"的不变量
- **内存回收**: 当前版本不释放被出队的节点（规避无锁内存回收难题），析构函数仅回收仍在队列中的节点

## 构建与运行

### 前置要求

- CMake ≥ 3.10
- 支持 C++17 的编译器（MSVC / GCC / Clang）

### Windows (Visual Studio)

```powershell
cd SimpleInferQueue
cmake -S . -B build
cmake --build build --config Debug
```

### Linux / macOS

```bash
cd SimpleInferQueue
cmake -S . -B build
cmake --build build
```

## 运行测试

### Windows (PowerShell)

```powershell
.\build\Debug\test_try.exe        # 非阻塞接口单元测试
.\build\Debug\test_lockfree.exe   # 无锁队列并发正确性测试
.\build\Debug\test_ms_queue.exe   # MS队列多线程正确性测试（输出 PASS/FAIL）
.\build\Debug\mock_inference.exe  # 模拟AI推理服务演示
```

### Linux / macOS

```bash
./build/test_try
./build/test_lockfree
./build/test_ms_queue
./build/mock_inference
```

## 性能对比

测试环境：单生产者-单消费者，100万入队/出队，队列容量4096

| 实现 | 总耗时 | 吞吐量 |
| :--- | :--- | :--- |
| BlockQueue（互斥锁） | **98** ms | **1020.4万** ops/s |
| LockFreeQueue（无锁） | **45** ms | **2222万** ops/s |

无锁队列吞吐量提升约 **2.18 倍**。

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
