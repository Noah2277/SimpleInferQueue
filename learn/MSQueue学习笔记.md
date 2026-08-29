# MSQueue 学习笔记

> 从零实现 Michael-Scott 无锁队列的完整历程：学习时间线、踩坑清单、知识点精华。

## 一、学习历程

### 迭代 1：骨架

- 创建 `ms_queue.h`：类结构 + 哑节点初始化 + `Enqueue`/`Dequeue`/`Empty` 空实现
- 创建 `test_ms_queue.cpp`：只验证「能编译 + 能构造」
- 目标：先把编译链路跑通

### 迭代 2：补全实现

- 按经典 MS 算法填了 `Enqueue`（两步入队 + 互助推进）和 `Dequeue`（CAS 推进 head）
- 结果：**4 处内存序拼写错误 + 2 处逻辑错误**，保存后编译失败
- 修复：拼写、`next` 未声明、空判断改为 `next == nullptr`、补 `head == tail` 互助步骤、补析构函数

### 迭代 3：单线程功能测试

- 入队 1~5 → 按序出队 → 空队列出队返回 false，全部正确
- 意义：验证基本逻辑，但**测不出并发问题**

### 迭代 4：MPCP 并发压测

- 4 生产者 × 4 消费者 × 1000 元素，验证「不多不少不重复」
- 踩坑：验证循环写反导致永远 FAIL（见踩坑 5）
- 最终：g++ 连跑 3 次 + MSVC 全部 PASS

### 迭代 5：文档化

- 提炼设计要点写入 README（哑节点、互助推进、内存序）
- 沉淀本笔记

---

## 二、踩坑清单

### 代码坑

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | 编译报 `memory_order_accquire` 不是 std 成员 | 拼写错误：`accquire`/`acq_acquire`/`prder_release`/`oreder_acquire` 共 4 处 | 统一改成正确的 `acquire`/`release` |
| 2 | `Dequeue` 里 `head == nullptr` 判空永不成立 | 哑节点永远存在，head 不可能为空 | 改为 `next = head->next.load(); next == nullptr` 才是真空 |
| 3 | `Dequeue`/`Empty` 里用未声明的 `next` | 忘了加载 `head->next` | 先 load 再判断 |
| 4 | 缺「head == tail 时帮忙推进 tail」步骤 | 简化实现省略了经典算法的互助分支 | 补上，维持「tail 不落后于 head」不变量 |
| 5 | 压测永远输出 FAIL | 验证循环里无条件 `success = false; break;` | 改成 `find(i) == end()` 才置失败 |
| 6 | `set.find()` 编译不过 | `find()` 必须传参数 | `find(i)` |
| 7 | 内存泄漏 | 无析构函数；出队节点被丢弃不释放 | 加析构（仅能回收仍在队列中的节点；出队节点需 Hazard Pointer/EBR 才能彻底解决，当前版本接受此泄漏） |
| 8 | 参数名 `reslut` | 拼写 | `result` |

### 编译环境坑

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 9 | `cd build` 报路径不存在 | build 在 `SimpleInferQueue/SimpleInferQueue/build`，少一层 | 注意工程目录层级 |
| 10 | CMake 报「CMakeCache 目录不同」+「找不到 VS Community」 | 项目从旧路径 `d:/c++项目/初始/...` 移过来，缓存里还记着旧路径和旧 VS 实例 | 删掉 build 目录重新 `cmake -S . -B build`，让 CMake 自动探测当前 VS |
| 11 | 编译「成功」但代码明明有错 | IDE 缓冲区未保存 ≠ 磁盘文件；MSBuild 增量编译被 mtime 骗过（磁盘旧版本编译通过） | 保存后再编译；必要时 `touch` 头文件或删 .obj 强制重编 |
| 12 | 中文注释乱码/报错（MSVC） | cl 默认按 GBK 解析源码 | CMakeLists 加 `/utf-8`（g++ 默认 UTF-8 无需处理） |

### Git 坑

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 13 | 新文件显示 U（未跟踪） | git 不自动管理新文件 | `git add` 后才被跟踪 |
| 14 | push 被拒（rejected, fetch first） | 远程有本地没有的提交（网页上传） | `git pull` 合并后再 push |
| 15 | 忽略规则不生效 | `.gitignore` 只对**未跟踪**文件生效；子目录的 .gitignore 管不到上级目录 | 已跟踪文件先 `git rm --cached`；根目录需要单独建 .gitignore |
| 16 | 想彻底撤销提交 | 普通 revert 会留痕迹 | `git reset --hard 目标提交` + `git push --force`；**操作前务必建备份分支** |
| 17 | 仓库里出现 zip | 网页上传源码快照 | 不建议提交二进制快照（GitHub 自带 Download ZIP），加 `*.zip` 忽略 |

---

## 三、知识点精华

### 1. 哑节点（哨兵节点）

- 队列永远保留一个不含有效数据的占位节点，`head_` 永远指向它
- 作用：消灭「空队列」特殊状态 → 入队只碰 tail、出队只碰 head，任何时刻指针都不悬空
- 空判断：`head->next == nullptr`
- 出队后：被越过的节点成为新哨兵，旧哨兵被丢弃

### 2. Michael-Scott 算法核心

- 结构：单向链表 + `head_`/`tail_` 两个原子指针
- 入队两步：① CAS 挂链到 `tail->next`（**关键**，唯一赢家）② CAS 推进 `tail_`（**非关键**，可失败可代劳）
- 出队一步：CAS 推进 `head_`，成功者取走 `next->value`
- 不变量：head 沿 next 可达 tail；tail 永不落后到 head 后面
- **互助推进**：任何线程发现 tail 不是真尾就帮忙推进——无锁世界里没有「等待」，只有「帮别人完成任务」。结果唯一、允许失败、幂等，所以谁帮都行。这是保证 lock-free（系统不因单个线程挂起而停滞）的关键

### 3. 内存序与 CAS

- `compare_exchange_weak(expected, desired, success_order, failure_order)`
- 成功用 `release`（发布写入），失败用 `acquire`（同步他人写入）
- 入队写数据 → release 发布指针；出队 acquire 读到指针 → 安全读数据，构成 happens-before 链
- weak 允许伪失败，配合循环重试，性能更好

### 4. 无锁的本质

- 不用 mutex，靠 CAS + 重试 + 互助，任何时刻至少一个线程能前进
- 线程随时被抢占，但系统不停滞——这是 lock-free 的定义性要求

### 5. 为什么 MPCP / 为什么链表

- **MPCP**：AI 推理场景两端天然并发（多请求线程 + 多 GPU worker）；MPMC 能退化到任何场景，反过来不行
- **链表 vs 环形数组**：链表无界（生产者永不等待）、节点挂上后永不变（CAS 只改一个指针）、竞争点分离；代价是每次入队 new、缓存不友好、回收难。固定容量极限性能用环形缓冲，通用任务队列用链表
- **SPSC 的两个游标打架**：head/tail 挤在同一缓存行 → 假共享（false sharing），两端互相失效缓存；MSQueue 入队只碰 tail_、出队只碰 head_，无公共计数器

### 6. MPCP 测试设计

- **不相交值区间**：生产者 p 生成 `[p*N, (p+1)*N)`，每个值全局唯一 → 正确答案已知，并发正确性化简为集合相等
- **原子计数**：`produced_count`/`consumed_count` 控制消费者退出协议（空队列时区分「生产者没干完→yield」和「干完了→继续等别人」）
- **锁只保护验证数据**：mutex 只包 set 插入，不包队列操作——否则测的是 mutex 不是队列
- **验证**：`size == TOTAL` 捕获重复 + 遍历 `find(i)` 捕获丢失
- 并发度必须 ≥ 实现的并发度，测试才有意义

---

## 四、一句话总结

> 哑节点消灭空队列，两个 CAS 定输赢，互助推进保进度——这就是 Michael-Scott 无锁队列在「谁都能投、谁都能取」场景下永不阻塞的全部秘密。
