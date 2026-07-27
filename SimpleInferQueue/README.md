# SimpleInferQueue

一个从零构建的C++高性能线程安全任务队列，专为AI推理所设计。支持互斥锁和无锁两种并发模型，并提供完整的模拟推理演示

## 项目结构

SimpleInferQueue/
├── include/
│   ├── ring_buffer.h          # 环形缓冲区（非线程安全）
│   ├── block_queue.h          # 互斥锁阻塞队列
│   └── lockfree_queue.h       # CAS无锁队列（原子操作+内存序）
├── examples/
│   ├── demo_producer_consumer.cpp  # 阻塞队列多线程演示
│   ├── test_try.cpp                # 非阻塞接口单元测试
│   ├── test_lockfree.cpp          # 无锁队列并发正确性测试
│   ├── bench_blocking.cpp         # 互斥锁队列性能基准测试
│   ├── bench_lockfree.cpp         # 无锁队列性能基准测试
│   └── mock_inference.cpp         # 模拟AI推理服务集成演示
├── CMakeLists.txt
└── README.md

## 核心技术点

- **环形缓冲区**: 预分配内存，减少动态分配，提升缓存局部性。
- **互斥锁与条件变量**: `std::mutex` + `std::condition_variable` 实现线程安全的阻塞队列。
- **无锁编程**: 基于`std::atomic`、CAS、Acquire-Release 内存序，实现生产者-消费者无锁队列。
- **优雅退出**: 通过原子标志位实现生产者和消费者的协调关闭。

## 构建与运行

cd SimpleInferQueue
mkdir build && cd build
cmake .. 
cmake --build .

## 运行模拟AI推理服务

#### Windows(PowerShell)
.\Debug\test_try.exe
.\Debug\test_lockfree.exe
.\Debug\mock_inference.exe

### Linux / macOS
./test_try
./test_lockfree
./mock_inference


## 性能对比

测试环境：单生产者-单消费者，100万入队/出队，队列容量4096

| 实现 | 总耗时 | 吞吐量 |
| :--- | :--- | :---|
|BlockQueue(互斥锁) | **98** ms | **1020.4万** ops/s |
| LockFreeQueue(无锁) | **45** ms | **2222万** ops/s |

无锁队列吞吐量提升约 **2.18 倍**。


## 模拟推理服务器运行效果

Processing request ID: 1208, Path: image/1208.jpg
Processing request ID: 1501, Path: image/1501.jpg
Processing request ID: 4397, Path: image/4397.jpg
Processing request ID: 7323, Path: image/7323.jpg
Processing request ID: 6404, Path: image/6404.jpg
Processing request ID: 3611, Path: image/3611.jpg
Processing request ID: 2999, Path: image/2999.jpg
Processing request ID: 6698, Path: image/6698.jpg
Processing request ID: 2264, Path: image/2264.jpg
Processing request ID: 9054, Path: image/9054.jpg
Processing request ID: 4857, Path: image/4857.jpg
Processing request ID: 3934, Path: image/3934.jpg
Processing request ID: 2927, Path: image/2927.jpg
Processing request ID: 7602, Path: image/7602.jpg
Processing request ID: 2405, Path: image/2405.jpg
Processing request ID: 9701, Path: image/9701.jpg
Processing request ID: 7314, Path: image/7314.jpg
Processing request ID: 4385, Path: image/4385.jpg
Processing request ID: 5260, Path: image/5260.jpg
Processing request ID: 5110, Path: image/5110.jpg

## 关于本项目
本项目是作者从零开始，逐步学习并发编程、无锁编程和工程构建的实践成果。从有锁到无锁，从基础组件到完整服务，每一步都经过测试验证。