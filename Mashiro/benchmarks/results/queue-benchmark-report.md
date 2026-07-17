# Mashiro 并发队列性能评估与优化报告

日期：2026-07-17  
仓库提交：`e47b8e89f3e98a2ed55e0645bc4283e6cf13a0a0`（测试前工作树含本报告对应的未提交改动）

## 1. 结论

本轮共记录 4560 个正式样本：完整基线 1740 个、优化实现 1740 个、同进程 A/B 配对 1080 个。每个样本均验证
消费数量与 `1..N` 校验和，未发现丢失、重复或提前终止。

三项结论具有不同的证据强度：

1. `SpscChannel` 的共享 epoch 是确定的结构性瓶颈。readable/writable 两条方向序列使 4 个配对 case 全部提升，
   加速范围为 2.16-4.94 倍，中位数为 3.15 倍。
2. MPSC 在完整优化矩阵的 12 个 case 中均不低于同场景最佳外部对照，中位比值为 1.43，范围为
   1.01-3.06。其 64 字节槽位隔离占用较大，但现有数据不支持直接移除。
3. `TryPop(T&)` 相对已优化的 `optional<T>` 路径没有稳定吞吐优势。MPSC 配对中位数为 0.98，MPMC 在
   MPSC/MPMC 拓扑下分别为 1.00/1.02。两种 API 因此共享同一个线性化核心，保留语义选择而不复制算法。

## 2. 比较对象与语义边界

| 实现 | 拓扑 | 容量语义 | 说明 |
|---|---:|---|---|
| Mashiro `SpscRingBuffer` | 1P/1C | 编译期固定、严格有界 | wait-free try 路径 |
| Mashiro `SpscChannel` | 1P/1C | 编译期固定、严格有界 | 数据平面加方向性 wait/notify 序列 |
| Mashiro `MpscQueue` | NP/1C | 编译期固定、严格有界 | sequence-cell，槽位按 cache line 隔离 |
| Mashiro `MpmcQueue` | NP/NC | 编译期固定、严格有界 | Vyukov sequence-cell 结构 |
| Rigtorp `SPSCQueue` | 1P/1C | 构造期固定、有一个内部 slack slot | commit `1053918d` |
| moodycamel `ConcurrentQueue` | NP/NC | 预分配 `try_enqueue`，不等同严格固定容量 | v1.0.5，commit `9afb9974` |
| oneTBB `concurrent_bounded_queue` | NP/NC | 运行时有界 | oneTBB 2023.1.0 |
| `std::mutex + std::deque` | NP/NC | 基准适配器严格有界 | 阻塞互斥参考实现 |

`AsyncQueue` 未纳入裸吞吐排名。它包含 P2300 operation state、取消、关闭、backpressure 与 waiter 控制面，
其成本模型不等同队列存储。`SpscChannel` 也单列展示，以免把通知成本误写成 SPSC 存储成本。

## 3. 实验环境

| 项目 | 配置 |
|---|---|
| CPU | Intel Core i7-12700H，14 核、20 逻辑处理器 |
| 内存 | 39.7 GiB |
| 操作系统 | Windows 11 Pro 10.0.26200 |
| 编译器 | COCA clang-p2996，Clang 21，revision `6e67e01c` |
| 标准库与语言 | libc++，`-std=gnu++26 -freflection-latest` |
| 性能配置 | `x64-release`，`-O2 -DNDEBUG` |
| 正确性配置 | `x64-asan`，ASan + UBSan |

工作线程使用 Windows CPU Set API 固定放置。CPU set 先按 `EfficiencyClass` 降序，再按物理核和 SMT sibling
排序；消费者优先取得 CPU set，生产者随后取得。此策略消除了短样本中由 P/E 核迁移引起的主要离散噪声。

## 4. 测量协议

- 容量：1024、16384。
- 载荷：8 B、64 B，载荷携带唯一 `uint64_t id`。
- MPSC：2P/1C、4P/1C、8P/1C。
- MPMC：2P/2C、4P/4C。
- 每个 case：闭环校准、2 次 warmup、15 次正式采样。
- 校准目标：单次正式工作量以 200 ms 为下限，按至少 2 倍几何增长，避免 Windows 调度量子支配快队列。
- 线程同步：工作线程先创建并完成 CPU 绑定，再由 barrier 同时进入计时区间；线程创建、队列构造和销毁不计时。
- 统计量：中位数为主估计，P10-P90 表示样本区间；CSV 同时保留 Push/Pop 失败重试数与对象字节数。

第一轮未固定线程、固定 200K 项的结果出现约 15/30 ms 双峰，属于调度量子伪影，已从正式结果中移除。
完整基线与优化轮之间又出现明显时段漂移，因此代码改动的因果判断以同进程交替 A/B 数据为准。

## 5. 代表性吞吐

以下数据取优化矩阵的 8 B、容量 16384 case，单位为百万次 transfer/s。

| 拓扑 | 实现 | 中位数 | P10 | P90 |
|---|---|---:|---:|---:|
| SPSC 1P/1C | Mashiro SPSC | 144.70 | 109.31 | 177.72 |
| SPSC 1P/1C | Rigtorp SPSC | 126.54 | 108.48 | 209.94 |
| SPSC 1P/1C | Mashiro SPSC channel | 29.41 | 25.29 | 32.77 |
| MPSC 4P/1C | Mashiro MPSC | 8.87 | 6.60 | 9.93 |
| MPSC 4P/1C | mutex deque | 7.67 | 3.59 | 10.57 |
| MPSC 4P/1C | Mashiro MPMC | 6.92 | 6.00 | 7.86 |
| MPSC 4P/1C | moodycamel | 4.00 | 3.23 | 4.98 |
| MPMC 4P/4C | mutex deque | 9.33 | 7.79 | 10.75 |
| MPMC 4P/4C | moodycamel | 6.92 | 6.21 | 8.31 |
| MPMC 4P/4C | Mashiro MPMC | 6.17 | 5.19 | 6.98 |

MPMC 的结论不能简化为 lock-free 必然优于 mutex。i7-12700H、4P/4C、短临界区和 Windows 调度组合下，
`mutex deque` 在代表 case 中领先。Mashiro MPMC 相对每个 case 最佳外部对照的中位比值为 0.95，范围为
0.66-1.32。该结果要求后续优化聚焦全局 enqueue/dequeue ticket 的争用，而非继续压缩普通指令数。

## 6. `SpscChannel` 配对结果

| 载荷 | 容量 | 双 epoch | 共享 epoch | 加速比 |
|---:|---:|---:|---:|---:|
| 8 B | 1024 | 6.106 | 2.826 | 2.161 |
| 8 B | 16384 | 9.988 | 3.676 | 2.717 |
| 64 B | 1024 | 10.650 | 2.155 | 4.942 |
| 64 B | 16384 | 20.816 | 5.822 | 3.576 |

旧结构要求生产者和消费者对同一 epoch 执行 RMW，每次成功 Push/Pop 都争用同一 cache line。新结构把
readable epoch 交给生产方向，把 writable epoch 交给消费方向；外部 predicate change 仍同时推进两条序列。
队列保持唯一真值来源，epoch 只承担唤醒，不参与数据可见性判定。

## 7. 存储成本

容量 16384 时，8 B 载荷的 MPSC 对象为 1,048,704 B，MPMC 为 262,272 B；64 B 载荷时分别为
2,097,280 B 与 1,179,776 B。MPSC 每槽 cache-line 隔离使 8 B case 约占 MPMC 的 4 倍。现有 MPSC 相对
同拓扑 MPMC 的性能并非在所有 case 中单调领先，且两者还相差 consumer CAS，故本轮不把差异单独归因于
slot alignment。保留现状，并把布局成本作为显式图表输出，优于在证据不足时压缩槽位。

双 epoch 使 `SpscChannel` 相对 `SpscRingBuffer` 增加 128 B。容量 16384 时，该固定成本分别占 8 B/64 B
对象的约 0.10%/0.01%。

## 8. 正确性验证

| 测试 | ASan/UBSan 断言数 | 结果 |
|---|---:|---|
| `Test.Core.SpscRingBufferTest` | 425,859 | 通过 |
| `Test.Core.SpscChannelTest` | 12 | 通过 |
| `Test.Core.MpscQueueTest` | 6,214 | 通过 |
| `Test.Core.MpmcQueueTest` | 40,844 | 通过 |
| `Test.Async.AsyncQueueTest` | 6,130 | 通过 |

新增 MPMC 测试覆盖输出参数、满队列回绕、move-only optional 路径，以及 4P/4C 下 40,000 个值的逐项唯一消费。

## 9. 图表与数据

- [吞吐总览](queue-throughput-overview.png)
- [MPSC/MPMC 扩展性](queue-scaling.png)
- [同进程 A/B 倍率](queue-paired-speedup.png)
- [样本变异箱线图](queue-sample-variability.png)
- [固定容量对象占用](queue-storage-footprint.png)
- [跨轮原始倍率](queue-optimization-speedup.png)，仅用于观察时段漂移，不作为因果证据
- [完整汇总 CSV](queue-summary.csv)
- 原始数据：`queue-baseline-pinned.csv`、`queue-optimized-pinned.csv`、`queue-paired-pinned.csv`

每张图同时提供 SVG。所有吞吐数字只适用于本机、当前工具链和当前线程放置；结论可迁移的部分是算法关系、
通知面争用机制与测量方法，不是绝对 Mops/s。

## 10. 复现

```powershell
python T:\toolchains\coca-toolchain-p2996\setup.py exec --shell powershell
cmake --build G:/Teaching/Vulkan/build/x64-release --target Benchmark.Mashiro.Queues --

G:/Teaching/Vulkan/build/x64-release/bin/Benchmark.Mashiro.Queues.exe `
  --samples 15 --warmups 2 --items 200000 --minimum-ms 200 --label optimized `
  --output G:/Teaching/Vulkan/Mashiro/benchmarks/results/queue-optimized-pinned.csv

G:/Teaching/Vulkan/build/x64-release/bin/Benchmark.Mashiro.Queues.exe `
  --paired --samples 15 --warmups 2 --items 200000 --minimum-ms 200 --label paired `
  --output G:/Teaching/Vulkan/Mashiro/benchmarks/results/queue-paired-pinned.csv

py -3 G:/Teaching/Vulkan/Mashiro/benchmarks/plot_queue_benchmarks.py `
  G:/Teaching/Vulkan/Mashiro/benchmarks/results/queue-baseline-pinned.csv `
  G:/Teaching/Vulkan/Mashiro/benchmarks/results/queue-optimized-pinned.csv `
  G:/Teaching/Vulkan/Mashiro/benchmarks/results/queue-paired-pinned.csv `
  --output G:/Teaching/Vulkan/Mashiro/benchmarks/results
```
