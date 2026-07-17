# Mashiro AsyncQueue 性能评估报告

日期：2026-07-17  
基线提交：8535e9bc102b872054a811e0d669810a7d0e694e，工作树包含本报告对应的未提交 benchmark 改动。

## 0. 优化复测

本节记录性能诊断后的实现改动。后续第 1 至 10 节保留优化前基线及其推理过程；凡涉及裸 `sync_wait`
吞吐，以本节的同协议复测为准。

性能差距由两个不同层次的成本叠加而成。`AsyncQueue` 的 storage try 为 0.929 ns/op，connected sender
ready path 为 40.299 ns/op，两者均不能解释原始端到端吞吐。主要成本来自 stdexec 默认 `sync_wait`：每个
元素分别构造一个 `run_loop`，再由 libc++ `atomic::wait` 执行 spin-then-park。Windows 调度使该终端呈现
约 0.5 us 的 P50 与 13 至 15 ms 的 P99，严格有界队列一旦填满，生产者和消费者便共同受此长尾限制。

实现包含三项改动：

1. `QueueDomain` 仅为四种原始 queue sender 定制 `sync_wait`，以栈上 condition-variable state 直接连接
   operation。经 `then` 等 adaptor 组合后的 sender 回退 `default_domain`，保留 scheduler 与 delegation
   environment 语义。
2. `unstoppable_token` 路径以 concept 在编译期删除 stop-token 查询、callback 注册与启动 CAS；可取消路径
   保留原状态机。connected sender ready path 由约 40.3 ns/op 降至约 37.6 ns/op。
3. benchmark 增加 atomic、spin、condition-variable 三种 terminal 隔离层，以及 implementation-prefix
   过滤。由此可分别测量队列、operation state、内核停车，不再把三者压缩为单一指标。

下表比较相同协议的 Release 数据：25 samples、3 warmups、每个写入样本不少于 300 ms，CPU Set 固定到
不同物理核心。吞吐单位为 million transfers/s。

| Topology | Payload | 优化前 | 优化后 | 倍数 | 优化后 P10-P90 |
|---|---:|---:|---:|---:|---:|
| SPSC | 8 B | 0.214 | 2.421 | 11.32 | 2.074-2.558 |
| SPSC | 64 B | 0.180 | 2.064 | 11.46 | 1.564-2.416 |
| MPSC | 8 B | 0.789 | 2.490 | 3.15 | 1.819-2.990 |
| MPSC | 64 B | 1.064 | 1.933 | 1.82 | 1.528-2.401 |
| MPMC | 8 B | 0.477 | 2.562 | 5.37 | 2.147-2.864 |
| MPMC | 64 B | 0.522 | 2.265 | 4.34 | 1.918-2.456 |

优化后的裸 `sync_wait` 达到严格 condition-variable monitor 中位吞吐的 25%-50%，达到各 case 最佳外部
实现的 21%-48%。数量级缺陷已经消除，剩余差距仍然客观存在。其结构性原因是每个元素仍独立构造
operation state，并独立执行一次阻塞 terminal；callback-composed graph、批量传输与持久 scheduler 才是
异步接口的目标工作模型。忙等虽然在部分 case 达到 2.1-2.8 M transfers/s，但会永久占用核心，不属于产品
优化方案。

复测数据为 `async-sync-wait-optimized-formal.csv`，terminal 隔离数据为
`async-mashiro-optimized-formal.csv`，图表位于 `async-queue-optimized-charts/`。曾评估将生命周期关闭位与
active-producer count 合并为一个原子状态；同参数 ready-path 测量没有收益，该改动已撤回，不进入产品代码。

## 1. 结论

本轮测量必须分为 callback 控制面、线程终端和端到端阻塞传输三个层次，三者不可合并为一个“异步队列性能”指标。

1. AsyncQueue 的直接控制面较快。ready callback delivery 的 P50/P90/P99/P99.9 均约为 0.2 us，取消
   delivery 的对应分位数均约为 0.1 us；abort callback fan-out 从 1 waiter 的 0.1 us 近线性增长至
   16 waiters 的 0.5 us。
2. 线程可见 resume 不是单纯的队列成本。Mashiro receiver 配合 atomic::wait 时，consumer/producer resume
   的 P50 均约为 0.5 us，但 P99 分别达到 13.033 ms 与 15.455 ms；该双峰来自终端等待与 Windows 调度，
   而不是 callback delivery。oneTBB 与 condition-variable 路径的 P50 为 7.2-8.4 us，但 P99 仅为
   35.9-40.6 us。
3. 每次传输都调用 stdexec::sync_wait 的端到端吞吐明显落后于主流阻塞队列。六个 topology/payload case
   中，Mashiro 相对同 case 最佳外部实现的中位吞吐比为 0.022-0.114，尚未达到性能齐平。该结果不能外推为
   callback-composed sender graph 的吞吐，因为此处刻意测量的是每项一次 blocking terminal 的用户路径。

因此，AsyncQueue 的 P2300 callback 核心没有显示出高固定成本；当前主要问题是“每项一个 operation state +
每项一个 blocking terminal”的使用模式，以及终端等待策略的长尾。优化方向应首先改变组合与批处理粒度，
而不是继续压缩 callback 分派的普通指令数。

## 2. 比较对象与语义边界

| 实现 | 容量语义 | 等待语义 | 关闭/取消语义 | 本报告中的角色 |
|---|---|---|---|---|
| Mashiro AsyncQueue | 编译期固定、严格有界 | P2300 sender；可 callback 或 terminal wait | close、abort、stop token | 被测对象 |
| oneTBB concurrent_bounded_queue | 运行时严格有界 | 阻塞 push/pop | abort，无 P2300 stop token | 阻塞与 backpressure 对照 |
| moodycamel BlockingConcurrentQueue 1.0.5 | 初始容量为分配提示，不是严格上限 | semaphore dequeue | 无统一 close/cancel | 吞吐与 consumer wake 对照 |
| mutex + condition_variable | 编译期严格有界 | monitor blocking | benchmark 定义 close/abort | 透明算法基线 |

moodycamel 不参与 producer backpressure 与 lifecycle fan-out 比较，因为它不提供同构语义。oneTBB 与 monitor
的 thread-return fan-out 包含操作系统唤醒和线程重新调度；Mashiro 的 callback fan-out 另行测量。

## 3. 实验协议

| 项目 | 配置 |
|---|---|
| CPU | Intel Core i7-12700H，14 核、20 逻辑处理器 |
| OS | Windows 10.0.26200.0 |
| 工具链 | COCA clang-p2996，LLVM 21，fingerprint af4c885885a5 |
| 构建 | x64-release，-O2 -DNDEBUG -std=gnu++26 -freflection-latest |
| 正确性 | x64-asan，ASan + UBSan quick matrix |
| 线程放置 | Windows CPU Set，优先性能核、不同物理核，再使用 SMT sibling |
| throughput | 25 samples，3 warmups，每条写入记录实测不少于 300 ms |
| latency | 每个 case 25 samples，每个 sample 10,000 observations |
| park 确认 | 独立 CPU 上 20 us pause-spin，不使用受 Windows sleep quantum 影响的 sleep_for |
| 统计量 | median 为主估计；P10-P90 表示 sample 间区间；latency 另保存逐 observation 原始数据 |

吞吐采用逐样本自适应 workload。若一次观测短于 300 ms，则该观测被丢弃并扩大 workload 重测；写入 CSV 的
600 条 throughput 记录全部满足下限，最短 306.174 ms。每个 sample 验证 1..N 的消费数量与校验和。

## 4. Ready Path

单位为 million operations/s；一次 push 和一次 pop 分别计为一个 operation。

| 层次 | P10 | Median | P90 | Median ns/op |
|---|---:|---:|---:|---:|
| Mashiro SPSC storage try | 959.067 | 1076.410 | 1146.644 | 0.929 |
| AsyncQueue::TryPush/TryPop | 97.732 | 109.252 | 115.487 | 9.153 |
| connected sender start | 23.660 | 24.814 | 25.510 | 40.299 |

storage 测量在每次 push/pop 后包含零指令 compiler barrier，防止同线程循环被证明等价后折叠。sender 路径不含
线程等待和 scheduler，只测 sender 构造、connect/start、operation state 与 inline receiver completion。

## 5. 端到端吞吐

单位为 million transfers/s。Mashiro 路径每次 push/pop 均通过 stdexec::sync_wait；外部实现使用各自阻塞 API。

| Topology | Payload | Mashiro | oneTBB | moodycamel | Monitor | Mashiro / best external |
|---|---:|---:|---:|---:|---:|---:|
| SPSC | 8 B | 0.214 | 4.632 | 9.889 | 8.392 | 0.022 |
| SPSC | 64 B | 0.180 | 4.862 | 8.069 | 8.339 | 0.022 |
| MPSC | 8 B | 0.789 | 1.231 | 10.609 | 5.288 | 0.074 |
| MPSC | 64 B | 1.064 | 1.445 | 9.340 | 4.853 | 0.114 |
| MPMC | 8 B | 0.477 | 0.141 | 5.391 | 5.146 | 0.089 |
| MPMC | 64 B | 0.522 | 0.250 | 5.153 | 5.034 | 0.101 |

oneTBB MPMC 出现显著长尾，故其中位吞吐低且 P10 接近零；该现象不会改变 Mashiro 相对 moodycamel/monitor
仍明显落后的结论。moodycamel 的容量语义较弱，因此其吞吐是工程参考，不是严格有界算法的等价证明。

## 6. Latency

下表取 25 个 sample 各自分位数的中位数。

| Case | Implementation | P50 | P90 | P99 | P99.9 |
|---|---|---:|---:|---:|---:|
| consumer resume | Mashiro + atomic::wait | 0.5 us | 0.9 us | 13.033 ms | 16.158 ms |
| consumer resume | oneTBB | 8.4 us | 12.8 us | 40.6 us | 95.0 us |
| consumer resume | moodycamel | 6.9 us | 20.5 us | 580.0 us | 2.539 ms |
| consumer resume | monitor | 7.7 us | 12.1 us | 362.4 us | 2.191 ms |
| producer resume | Mashiro + atomic::wait | 0.5 us | 0.7 us | 15.455 ms | 16.028 ms |
| producer resume | oneTBB | 8.3 us | 11.7 us | 36.9 us | 92.2 us |
| producer resume | monitor | 7.2 us | 9.4 us | 35.9 us | 91.8 us |
| callback delivery | Mashiro receiver | 0.2 us | 0.2 us | 0.2 us | 0.2 us |
| cancellation | Mashiro receiver | 0.1 us | 0.1 us | 0.1 us | 0.1 us |

Mashiro thread resume 的 P50 较低，是因为 libc++ atomic::wait 在短等待中先自旋；一旦进入内核等待，尾部转而
受 Windows 调度量子支配。该分布不是普通单峰延迟，故只汇报平均值会产生误导。

## 7. Fan-out

| Waiters | 1 | 2 | 4 | 8 | 16 |
|---:|---:|---:|---:|---:|---:|
| Abort-to-all-callback | 0.1 us | 0.1 us | 0.2 us | 0.3 us | 0.5 us |

callback fan-out 在当前分辨率下近似线性，16 waiter 仍小于 1 us。thread-return fan-out 则在 2-16 waiter 时
普遍落入约 15.1-31.1 ms 区间，主要反映 Windows 线程重新调度；其图表保留用于展示系统层成本，不用于评价
callback 算法复杂度。

## 8. 存储占用

以 Payload<8>、capacity 1024 的 Mashiro SPSC AsyncQueue 为例：

| 对象 | Bytes |
|---|---:|
| Queue（含 1024 个 inline slots） | 8448 |
| Producer port | 8 |
| Consumer port | 8 |
| Push sender | 16 |
| Pop sender | 8 |
| Connected push operation | 120 |
| Connected pop operation | 200 |

外部队列对象尺寸不可直接与 8448 B 比较，因为 oneTBB、moodycamel 和 monitor 把主要 storage 动态分配；图表
仅披露对象本体，不把它解释为完整内存占用。

## 9. 优化方向

按证据强度排序：

1. 增加 sender-native batch push/pop 与长生命周期 drain sender，使一次 terminal 或一次调度摊销多个元素。
   当前每元素 120/200 B operation state 的反复构造和 sync_wait 是吞吐主瓶颈。
2. 增加 callback-composed throughput benchmark，使用 counting scope/run loop 驱动持续 sender graph，
   与 blocking-per-item 路径分开评价。
3. 研究终端等待策略。当前 atomic::wait 的 spin-then-park 产生 0.5 us P50 与约 15 ms P99 的双峰；可评估
   可配置 spin budget、waiter-aware terminal 或由 scheduler 承接 completion，但不得把此策略塞入
   AsyncQueue 数据平面。
4. 保留 callback 控制面现状。0.1-0.2 us completion 与 16 waiter 0.5 us fan-out 没有显示出优先优化价值。
5. 补充稳态批处理、公平性和 executor hand-off 数据，再决定是否需要改变 intrusive waiter list 或锁粒度。

## 10. 结果文件

- async-ready-formal.csv：75 条 ready-path sample。
- async-throughput-formal.csv：600 条满足 300 ms 下限的 throughput sample。
- async-latency-formal.csv：225 条 latency sample 分位数。
- async-latency-formal-raw.csv：2,250,000 条逐 observation latency。
- async-fanout-formal.csv：500 条 callback/thread fan-out sample。
- async-size-formal.csv：对象尺寸。
- async-queue-formal-charts/：11 组 PNG/SVG 图表。

复现绘图：

    python Mashiro/benchmarks/plot_async_queue_benchmarks.py \
      Mashiro/benchmarks/results/async-size-formal.csv \
      Mashiro/benchmarks/results/async-ready-formal.csv \
      Mashiro/benchmarks/results/async-throughput-formal.csv \
      Mashiro/benchmarks/results/async-latency-formal.csv \
      Mashiro/benchmarks/results/async-fanout-formal.csv \
      --raw Mashiro/benchmarks/results/async-latency-formal-raw.csv \
      --output Mashiro/benchmarks/results/async-queue-formal-charts
