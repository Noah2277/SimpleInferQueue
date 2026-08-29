# BlockQueue 与 LockFreeQueue 学习笔记

> 从互斥锁阻塞队列到无锁环形队列的完整学习历程：学习时间线、踩坑清单、知识点精华。

## 一、学习历程

### 阶段 1：阻塞队列（mutex + condition_variable）

- 理解为什么同时需要 mutex 和 condition_variable：一个解决「同时改」，一个解决「何时睡 / 何时醒」
- 梳理 `Push`/`Pop` 核心逻辑：加锁 → 谓词等待 → 操作 → 精准通知
- 搞清楚 `notify_one` 与 `notify_all` 的选择依据

### 阶段 2：环形缓冲区

- 理解为什么弃用 `std::queue`：有界背压、热路径零分配、缓存局部性
- 掌握环形缓冲区原理：head/tail 取模绕圈、count 计数区分满空
- 逐行追踪 `tail = (tail + 1) % capacity` 的回卷轨迹，对比 `%` 与 `if` 两种写法

### 阶段 3：无锁队列

- 从 benchmark 数据出发（无锁约为有锁 2.18 倍），理解无锁为什么快
- 学习 atomic 与 CAS 的基本概念
- 深入 Acquire-Release 内存序的「发布-订阅」语义
- 关键发现：本项目 SPSC 无锁队列**不需要 CAS**，仅 load/store 即可

### 阶段 4：运行与收尾

- 学会编译运行 benchmark（踩了 `cmake --build` 目录的坑）
- 理解优雅退出：原子标志 + 双条件判断 + join

---

## 二、踩坑清单

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | `cmake --build .` 报「not a CMake build directory (missing CMakeCache.txt)」 | 在源码目录（只有 CMakeLists.txt）执行了 build | 先 `cd build`，或直接 `cmake --build <build目录完整路径>`；判断标准：有 CMakeCache.txt 的目录才是 build 目录 |
| 2 | `cd build` 报路径不存在 | 仓库两层嵌套：`SimpleInferQueue/SimpleInferQueue/build`，当前在外层（同 [[MSQueue学习笔记]] 坑 9） | 先 `cd SimpleInferQueue` 再 `cd build`，或直接用完整路径 |
| 3 | 已在 build 目录又执行 `cd SimpleInferQueue\build` 报错 | 报错信息拼出的完整路径就是当前目录——人已经在 build 里，又往深处钻 | 先看报错里的完整路径定位自己在哪；进了 build 直接 `cmake --build .`，不再 cd |

---

## 三、知识点精华

### 1. mutex 与 condition_variable 的分工

- **mutex** 解决「同时改」：临界区互斥，防数据竞争
- **condition_variable** 解决「没活干时怎么睡、有活时怎么被叫醒」：避免忙等空转烧 CPU
- `wait(lock, 谓词)` 做三件事：① 原子地释放锁并睡眠（防 lost wakeup）② 被唤醒后自动重新加锁 ③ 醒来检查谓词，不满足继续睡（防虚假唤醒）
- **两个 cv 精准唤醒**：`cv_not_full_` 服务生产者，`cv_not_empty_` 服务消费者，避免吵醒无关线程空跑
- **notify_one vs notify_all**：生产一个元素只喂饱一个消费者 → `notify_one`；消费一个元素只空出一个位置、但多个生产者都在等 → `notify_all`

### 2. BlockQueue Push/Pop 核心逻辑

- `Push`：加锁 → `!Full()` 谓词等待 → `buffer_.Push` → `notify_one(cv_not_empty_)`
- `Pop`：加锁 → `!Empty()` 谓词等待 → `buffer_.Pop` → `notify_all(cv_not_full_)`
- `try_Push`/`try_Pop`：`lock_guard` + 条件判断，非阻塞尝试
- 分层设计：`RingBuffer` 管数据、`BlockQueue` 管同步，两个关注点解耦

### 3. 为什么弃用 std::queue（deque）

- **无界** → 无法做背压限流；阻塞队列的「满即阻塞」语义必须有固定容量
- **动态分配**：deque 按块 new/delete，推理热路径上的分配毛刺不可接受
- **缓存局部性差**：块指针数组 + 散落的块；环形缓冲是整块连续内存
- **接口表达力差**：`push` 无返回值，表达不出「满了推不进去」

### 4. 环形缓冲区原理

- 核心：固定数组 + 两个会绕圈的指针 `head`（读）/ `tail`（写）+ 取模回卷
- `tail = (tail + 1) % capacity`：走到底自动绕回 0，越界不可能发生
- **Pop 不搬移元素**：head 越过旧槽，将来被新数据覆盖——O(1) 零移动
- **满空判定两种方案**：
  - 方案 A（牺牲一格）：`(tail+1) % cap == head` 即满，浪费一个槽
  - 方案 B（count 计数）：`count == 0` 空、`count == capacity` 满——本项目 RingBuffer 用此方案
- **head == tail 二义性**：位置重合无法区分满/空，必须引入额外信息（count 或空槽）
- `%` 写法与 `if (tail == capacity) tail = 0` 等价，前者更简洁且天然防越界；2 的幂容量会被编译器优化成位运算
- 注意：LockFreeQueue 用的是**牺牲一格**方案（`capacity_ = capacity + 1`），与 RingBuffer 的 count 方案形成对比

### 5. 无锁为什么比有锁快（benchmark 约 2.18 倍）

- **mutex 的成本**：lock/unlock 是原子 RMW（独占缓存行）、锁状态缓存行在两个核间弹跳、竞争时陷内核（futex）
- **无锁关键路径**：SPSC 下只有 load/store，**连 CAS 都不用**；纯用户态
- **内存序精打细算**：relaxed/acquire/release 按需取用 vs mutex 隐含的全量屏障
- **head_/tail_ 分离**：生产者只写 tail、消费者只写 head，每个核只改自己的缓存行
- **代价（快的适用条件）**：SPSC + 不阻塞；满/空时无锁版只返回 false，阻塞语义要调用方自己补；多生产者需 CAS 竞争重试；正确性全靠内存序，写错极难调试
- **选型结论**：热路径、短临界区、SPSC → 无锁；逻辑复杂、多生产者、需要阻塞 → 加锁

### 6. atomic 与 CAS 基本概念

- **atomic**：让「读-改-写」不可分割（`count++` 的中间态对他人不可见）+ 通过内存序控制跨线程可见性
- **CAS**：原子地「如果内存值 == expected，就改成 desired」；硬件指令（x86 的 cmpxchg）
- **乐观重试模式**：先读 → 计算 → CAS 提交 → 失败重来，这是多生产者抢位置的经典写法
- **SPSC 只需 load/store**：tail 只有生产者写、head 只有消费者写，没有竞争就没有 CAS 的必要；CAS 是多个线程抢同一变量时才需要的武器

### 7. Acquire-Release 内存序

- **解决的问题**：编译器/CPU 指令重排破坏「先写数据、后发信号」的顺序（relaxed 下消费者可能看到新 tail 却读到未写入的数据）
- **release（store 端）**：我之前的所有写入，任何人看到新值就一定能看到——「数据先落地，再公布」
- **acquire（load 端）**：我读到新值，说明发布端此前的写全部可见——「看到公布，就知道数据在」
- **配对成环**：release-store + acquire-load 作用于同一原子变量构成 happens-before。项目里两条对称环：生产→消费（release tail / acquire tail）、消费→生产（release head / acquire head，保证槽位已空可覆盖）
- **三档选型**：自己独占的变量用 relaxed（如 Enqueue 读自己的 tail）；跨线程传递数据用 acquire/release；seq_cst 最省心但最慢

### 8. 优雅退出（mock_inference 模式）

- **原子标志** `producer_done`：生产者完工后 `store(true)`，跨线程可见
- **消费者双条件退出**：`producer_done.load() && queue.Empty()`——只判 done 会丢存量任务；只判空会误退（生产中队列短暂为空是常态）
- **join 收尾**：主线程等所有线程正常走完，不杀在途任务
- **yield 配套**：无锁队列不阻塞，空转时 `std::this_thread::yield()` 让出 CPU
- 对比：demo 里生产者/消费者都是固定循环次数，线程自然结束，不需要标志位

---

## 四、一句话总结

> 锁解决「同时改」、条件变量解决「何时睡」、环形缓冲用取模绕圈换零分配、无锁用内存序换掉内核仲裁——从有锁到无锁，每一层都在用更便宜的机制做同一件事：正确地搬运任务。
