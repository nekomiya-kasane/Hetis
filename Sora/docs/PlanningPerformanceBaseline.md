# Sora Compute Planning Performance Baseline

Date: 2026-07-30

## 测量对象

`Benchmark.Sora.Compute.PlanningInputs` 约束 physical-planning fresh attempt 的四类成本。四项测量均位于控制面，
不进入 resident submission、queue worker 或逐元素 kernel 热循环。

| metric | 输入规模 | 语义边界 |
|---|---:|---|
| artifact availability snapshot | 64 个 live exact keys | 复制、排序并冻结 cache availability |
| calibrated transfer topology projection | 1 条 calibrated directed edge | 物理拓扑与 payload bucket 联结 |
| transfer-inclusive candidate synthesis | 1 device、1 queue、1 transfer state | capability、health、artifact 与 calibration 联结 |
| dispatch candidate enumeration | 1 dispatch、2 buffers、1 readable route | region 归并、residency snapshot、memory selection、route 与 synthesis |

完整枚举使用已注册 `KernelProgram`、1 个 host-resident 输入、1 个未初始化输出、1 个 Vulkan device memory、
1 条 host-to-device transfer edge。枚举结果必须包含 1 个 residency projection、1 个 placement，以及精确的
`1,000,000 bytes / 105,000 ns` transfer cost。

## 测量协议

| property | value |
|---|---|
| build | COCA clang-p2996，RelWithDebInfo，benchmark-local `-O3 -march=haswell` |
| process count | 5 个独立进程 |
| samples | 每进程每 case 9 个样本，取进程内中位数 |
| calibration | 每个 case 独立校准，单样本至少 50 ms |
| scheduling | forward/reverse rotation，降低固定顺序偏差 |
| host | Windows x86-64，20 logical processors，AVX2 |

四项 case 的单次成本相差两个数量级，故必须使用 `MeasureInterleavedIndependent`。共享 repetition count 会让最快
case 决定全部重复次数，从而把完整枚举放大到数十秒，并使不同层次的结果不可比较。

## 2026-07-26 基线

| metric | five-process median |
|---|---:|
| 64-key artifact availability snapshot | `6247.754 ns/operation` |
| calibrated transfer topology projection | `77.996 ns/operation` |
| transfer-inclusive candidate synthesis | `621.943 ns/operation` |
| dispatch candidate enumeration | `2163.397 ns/operation` |

完整枚举约为 `2.16 us`。该值包含 vector 分配、两个 logical buffer 的 binding 处理、一次真实 residency snapshot、
payload-specific topology projection、route selection、exact artifact lookup 与 queue candidate synthesis。它不包含 runtime epoch、
artifact availability 和 performance snapshot 的获取；三类 snapshot 必须由 fresh planning attempt 在外层统一冻结。

旧基线使用四个 case 共用的 repetition count，当前基线改为逐 case 独立校准，故 `5432.689 / 64.315 / 221.403 ns`
与本表不构成同协议回归比较。以后只比较采用独立校准、相同输入规模与相同机器约束的结果。

## symbolic residency 的正交基线

`Benchmark.Sora.Compute.GraphResidency` 单独约束图内 symbolic state 与 physical plan expansion 的成本，不把新层次
混入 `Benchmark.Sora.Compute.PlanningInputs`。测量仍采用五个独立进程、每项九个样本、逐 case 独立校准与单样本至少
50 ms；编译参数同为 benchmark-local `-O3 -march=haswell`。

| metric | 输入规模 | five-process median |
|---|---:|---:|
| symbolic segment lookup | 1024 个 tracked buffer | `19.272900 ns/operation` |
| graph-local storage lookup | 1024 个有序 binding | `71.146107 ns/operation` |
| affine partition lookup | 百万元素 closed-affine domain | `5.109847 ns/operation` |
| affine partition apply | 两个 exact affine shards | `565.830231 ns/operation` |
| exact affine replica branch | whole-affine replica addition | `412.831497 ns/operation` |
| partial affine replica branch | strict affine subcoverage split | `1000.474548 ns/operation` |
| partial / exact paired ratio | 相同 fixture 与事务边界 | `2.465991x` |
| baseline replica branch | frozen baseline 首次 graph-local materialization | `204.239655 ns/operation` |
| symbolic route -> physical fragment | 1024-allocation/segment prefix，1 个异步 hop | `470.951080 ns/operation` |
| logical graph-local storage resolve | 百万元素 affine logical allocation | `196.813583 ns/operation` |
| assembly-part graph-local storage resolve | 百万元素 affine assembly part | `342.546844 ns/operation` |
| assembly-part / logical paired ratio | 相同 symbolic identity | `1.718034x` |

`GraphLocalStorageMap` 使用按完整 `GraphLocalResidencySource` 排序的连续存储，查询复杂度为 `O(log n)`，且查询路径不分配
内存。约 `71.15 ns` 包含 1024 项二分比较，属于 graph planning 控制面。约 `470.95 ns` 的 route expansion 包含
append-ready fragment 的 allocation、segment 与 edge 小向量分配，因此该值是端到端扩展成本，不是纯查找成本，也不进入
resident submission 或逐元素 kernel。

baseline replica branch 只测量 frozen baseline transfer 完成后的事务性 symbolic overlay，不执行物理 copy。它比 exact affine
produced branch 少 producer-value split/proof，但仍完整验证 baseline version、coverage 与 replica identity。`GraphResidencySegment`
保持 `272 B`；本轮采样期间没有并行 `clang`、`ninja` 或 `cmake` 构建。

### 2026-07-30 partition commit 增量基线

`symbolic_affine_partition_commit` 测量两个 affine shard 的完整控制面：binding schema 校验、destination 校验、逐 writable
argument 的 pairwise-disjoint 与 exact-union 证明、update 构造，以及一次原子 `GraphResidencyState::Apply`。fixture 重复提交
相同的 `baseVersion + 1`，状态始终保持两个 segment，不把容器增长混入结果。

| metric | five-process median |
|---|---:|
| two-shard affine transactional apply | `1298.838043 ns/operation` |
| validated two-shard partition commit | `2203.735352 ns/operation` |
| partition commit / apply paired ratio | `1.722693x` |

约 `0.9 us` 的增量包含 schema 与 coverage 证明，不进入 kernel、submission 或 backend 路径。coverage 证明的复杂度为
`O(W * S^2)`，其中 `W` 是 writable argument 数，`S` 是 shard 数；它不随 logical element count 增长。当前自动规划的
`maximumDevices` 默认为 8、硬上限为 64，因此该有界二次项属于 planning control plane。正式回归比较必须沿用相同 fixture、
五个独立进程与逐 case 独立校准协议。

## exact linear-chain planning 的正交基线

`Benchmark.Sora.Compute.LinearChainPlanning` 约束 exact linear-chain planner 的完整纯规划成本，独立于 planning-input projection 与
单次 symbolic route expansion 两组微基线。测量包括 residency freeze、state-specific candidate enumeration、动态规划、路径回溯
和 physical lowering；不包括 backend callback、runtime compilation、physical allocation、materialization 与 submission。

| graph / exact residency frontier | five-process median |
|---|---:|
| 1 node / 1 destination | `8.655 us/plan` |
| 8 nodes / 1 destination | `63.400 us/plan` |
| 8 nodes / 2 destinations | `129.316 us/plan` |
| 32 nodes / 2 destinations | `632.254 us/plan` |
| 3-node residency-sensitive chain / 2 destinations | `70.067 us/plan` |

本组沿用五个独立进程、每项九个样本、逐 case 独立校准与单样本至少 50 ms 的协议。frontier key 现为共享 frozen baseline 下
完整 canonical graph-local residency，而不是末端 destination coordinate。最后读者分析在每层删除后继不可观察的 buffer；若完整
状态数超过 `maximumFrontierStates`，planner 返回 `CapacityExceeded`，不会用裁剪近似替代精确规划。residency-sensitive case 专门
约束两个相同末端 destination、不同 baseline replica history 的状态不能错误合并。

旧 revision 的 destination-only 表为 `6.900 / 48.238 / 117.853 / 521.670 us/plan`，不能与当前精确状态 revision 直接定性比较。
在当前 revision 内，未做 liveness 与 consuming transition 优化的完整状态版本为 `10.771 / 77.890 / 276.674 / 1342.056 us/plan`；
当前双设备 8/32 节点分别改善约 `53.3% / 52.9%`。这些结果约束完整控制面规划成本，不能与单次 candidate enumeration 直接相除
并解释为纯 DP 开销。

## 验证证据

- 2026-07-30 detached admission revocation：Release 与组合 ASan + UBSan 的完整 `Sora.Compute` label 均通过
  `398 / 398`，零失败、零 sanitizer 诊断；45 个 Compute 测试目标均在对应配置下显式重建。定向结果为
  `PortablePackageRegistryTest 12 cases / 753 assertions` 与 `PhysicalGraphMaterializerTest 19 / 1031`。
- 2026-07-30 portable package admission provenance：Release 与组合 ASan + UBSan 的完整 `Sora.Compute` label 均通过
  `396 / 396`，零失败、零 sanitizer 诊断；45 个 Compute 测试目标均在对应配置下显式重建。定向结果为
  `PortablePackageArchiveTest 6 cases / 131 assertions`、`PortablePackageRegistryTest 10 / 427` 与
  `PhysicalGraphMaterializerTest 19 / 1025`。
- 2026-07-30 exact residency frontier：Release 与 ASan + UBSan 的完整 `Sora.Compute` label 均通过 `395 / 395`，零失败、
  零 sanitizer 诊断；45 个 Compute 测试目标均在对应配置下显式重建。
- 定向结果：`GraphResidencyTest` 为 `14 cases / 400 assertions`，`LinearChainPlannerTest` 为
  `10 cases / 393 assertions`。反例覆盖完整 residency Bellman 等价、最后读者回收、consuming transition、destination-only
  合并丢失全局最优，以及无效/溢出 frontier 容量。
- 2026-07-30 任意合法轴 automatic partition：Release 与 ASan + UBSan 的完整 `Sora.Compute` label 均通过
  `390 / 390`，零失败；45 个 Compute 测试目标均在对应配置下重新构建。
- 定向 Release：`DispatchPartitionTest` 为 `4 cases / 65 assertions`，`AutomaticDispatchPlanningTest` 为 `6 / 41`，
  `AutomaticDispatchExpansionTest` 为 `14 / 637`，`CpuBackendTest` 为 `10 / 7970`，LLVM integration 为 `3 / 4221`，
  `VulkanBackendTest` 为 `18 / 24902`。
- 新增反例覆盖 forged binding、forged route、forged timing、equal-tail batch 输入乱序与显式 hop-to-segment provenance。
  所有成功 physical lowering 均继续通过 `PhysicalGraphPlan::Create`；sanitizer 未报告内存错误或 UB。

## 回归判据

普通 CI 不固定核心、频率和后台负载，不得用单次绝对纳秒值判失败。稳定性能机应在同一 revision 上运行至少五个
独立进程；若 `dispatch_candidate_enumeration` 的跨进程中位数退化超过 15%，且至少四个进程方向一致，则进入性能审查。

linear-chain 基线采用相同判据：同一输入规模和同一五进程协议下，任一 case 的跨进程中位数退化超过 15%，且至少四个
进程方向一致，进入性能审查。审查须先按 freeze、enumeration、DP/replay、physical lowering 四段剖分，不能以减少候选、跳过
plan validation 或改变接受图类消除回归。

HEFT 基线采用同一判据，但必须先区分静态 rank、动态 residency enumeration、queue gap search、transfer gap search、emission
order 与 physical lowering。planner CPU time 与预测的设备执行 makespan 是不同量，不能把 distinct-engine case 较高的规划成本
解释为设备执行较慢，也不能把真实 transfer contention 的 makespan 变化归因于 planner CPU 性能。

性能审查按四层分解：artifact snapshot、topology projection、candidate synthesis、完整 enumeration。只有最后一项退化时，
优先检查 region canonicalization、buffer residency copy、temporary vector allocation 与 queue-to-projection mapping；四项同时退化时，
优先排查主机频率、调度与后台负载。不得通过放宽阈值吸收可重复回归。

## 未纳入成本

当前 planning-input enumeration 已覆盖 `DevicePipeline` availability 与 calibrated pipeline create/import cost，
exact linear-chain 基线已覆盖 linear-chain DP、路径回溯与 pure physical lowering。一般 DAG 的 HEFT 基线已覆盖 hop-level
transfer-engine calendar、physical constraint lowering 与显式预算约束下的 placement local search。backend rediscovery 与 epoch diff
现已由 recovery benchmark 的独立 case family 覆盖。尚未纳入的是校准反馈驱动的自动预算选择、portable package validation 与
runtime JIT；加入这些层次时仍须新增正交指标，不能改变既有输入后继续沿用 `2163.397 ns/operation`、linear-chain、local-search
或 rediscovery 数值作为同协议基线。

## 一般 DAG HEFT 的正交基线

`Benchmark.Sora.Compute.HeftPlanning` 与 exact-chain benchmark 共享 `DispatchPlanningBenchmarkFixture`。两者输入完全一致，差异
只来自 planner。测量覆盖 static resource-candidate enumeration、upward rank、state-specific exact candidate enumeration、
insertion-based queue/transfer scheduling、emission ordering 与 pure physical lowering；不包含 backend callback、编译、真实
allocation、materialization 与 submission。

| graph / destination coordinates | five-process median |
|---|---:|
| 1 node / 1 destination | `10.708459 us/plan` |
| 8 nodes / 1 destination | `73.568652 us/plan` |
| 8 nodes / 2 destinations | `95.932617 us/plan` |
| 32 nodes / 2 destinations | `502.358594 us/plan` |
| 8 independent nodes / 1 shared transfer engine | `81.233301 us/plan` |
| 8 independent nodes / 8 distinct transfer engines | `84.347852 us/plan` |

后两项是 transfer contention 的正交场景：graph、dispatch 数与 transfer 数相同，shared case 的八条 route 竞争同一 exact engine，
distinct case 为每个输入提供独立 source location 与 engine。distinct case 的 planner time 更高，来自更多 route/source identity、
calendar 与物理 topology；它不代表设备执行 makespan 更高。执行层面恰相反，独立 engine 允许并行，而共享 engine 必须按 calendar
串行。表中数值只约束控制面规划成本的数量级，发布级 gate 仍需固定功耗策略、核心和后台负载并做同 revision A/B。

当前六项默认路径相对 transfer-calendar 阶段基线依次变化 `+8.7%`、`+0.8%`、`+8.1%`、`+5.6%`、`+4.3%`、`-3.8%`，
均低于 15% 审查阈值。局部搜索默认关闭；禁用路径不收集 placement alternatives，也不构造 directive。

## 有界局部搜索的正交基线

local-search case 与默认 `8 nodes / 2 destinations` 使用相同图、设备、校准和初始 residency，但各自持有独立 fixture。benchmark
在计时前调用 detailed `Plan`，验证实际 `neighborEvaluations` 分别严格等于 `1` 与 `8`，并再次验证 physical plan。

| policy / graph | five-process median | paired ratio over baseline | amortized incremental cost |
|---|---:|---:|---:|
| disabled / 8 nodes / 2 destinations | `95.932617 us/plan` | `1.000000` | - |
| 1 pass / 1 evaluation | `157.979297 us/plan` | `1.555028` | `57.309961 us/evaluation` |
| 1 pass / 8 evaluations | `558.173438 us/plan` | `5.538981` | `58.006079 us/evaluation` |

搜索成本随 evaluation 预算近似线性。单 evaluation 包含固定的 alternatives 收集与最终 lowering，故其摊销增量高于八次预算。
`HeftPlanningStatistics::optimizedMakespanNanoseconds` 是校准模型预测的设备 schedule makespan；表中 `us/plan` 是主机控制面墙钟时间。
二者量纲虽同为时间，但描述不同对象，不得相互替代。

candidate 分层的 TU-private prepared-buffer 复用经过同会话 A/B。关闭复用时，完整 dispatch enumeration 的五进程中位数为
`1.810 us`；启用后为 `1.514 us`，改善 `16.4%`。该优化保留 resource-only 与 exact-residency 两个公共语义层，只消除内部重复的
buffer grouping 与 region canonicalization。

## Transfer-inclusive automatic partition 的探索基线

`Benchmark.Sora.Compute.DispatchPartition` 单独测量 compute-only automatic decision 与 service-scoped route timeline；
`Benchmark.Sora.Compute.AutomaticPartitionPlanning` 再覆盖 residency freeze、candidate enumeration、exact shard route projection、
有界整数 boundary search 与 transfer scheduling。两组 benchmark 均不执行真实 copy、kernel dispatch、allocation materialization
或 queue submission。

| metric | five-process median | semantic boundary |
|---|---:|---|
| two-route contended transfer schedule | `447.452545 ns/schedule` | 两条 exact route 竞争同一 service-scoped engine |
| compute-only automatic two-device planning | `3933.245850 ns/decision` | exact capacity seed，不含 shard transfer |
| compute-only single-device planning | `34.526587 ns/decision` | whole-workload placement baseline |
| compute-only rank-3 multi-axis planning | `2803 ns/decision` | 两个非退化候选轴的 exact capacity comparison |
| transfer-inclusive exact planning, budget 1 | `8834 ns/decision` | 单轴初始 cover 的一次完整 exact evaluation |
| transfer-inclusive exact planning, budget 16 | `59727 ns/decision` | 至多十六次跨轴共享 exact evaluation |
| budget 16 / budget 1 | `6.715x` | 逐进程 paired ratio 的中位数 |

这些数据来自五个独立顺序进程；每个进程沿用 benchmark harness 的逐 case 校准与内部中位数协议。正式 A/B 仍须固定功耗策略、核心、
后台负载和 revision；若同协议中位数退化超过 15%，且至少四个进程方向一致，则按 freeze、enumeration、slice projection、timeline
scheduling 与 boundary search 五段剖分。不得减少 route 复验、缩放 whole-transfer cost 或放宽 evaluation 语义以吸收退化。

当前搜索已联合候选轴、replacement/addition/removal 三类 device-set 邻域与相邻 integer-boundary 邻域；每个 proposal 仍执行完整
exact projection 与 transfer scheduling。每个候选轴先获得一个 exact seed，随后按轴 round-robin，共享唯一硬总预算；预算小于候选轴数
是类型化配置错误，不允许静默形成低编号轴偏置。相对上一轮 `8487.658691 / 56165.527344 ns` 探索样本，当前 budget 1 与 16 分别高约
`4.1% / 6.3%`，低于 15% 分段审查阈值。该测量没有固定核心和功耗策略，只支持“未观察到明确退行”。算法仍是有总预算的确定性
coordinate descent，不构成 transfer-aware 全局最优证明；扩展非相邻联合移动或改变 neighborhood ordering 时，必须保留本节基线并
新增可区分搜索能力的反例与 benchmark。

### 2026-07-30 HEFT 默认 automatic partition 与收益门槛基线

`Benchmark.Sora.Compute.HeftPlanning` 现把 whole-only、强制 automatic search、默认 amortization gate、被固定开销拒绝的
partition 和 placement local search 放入同一个 independently calibrated case family。automatic candidate 在当前 DAG 的绝对
predecessor、queue 与 service-scoped transfer-engine timeline 上求值；它不是 standalone compute-only decision 的重复测量。

五个独立 Release 进程的跨进程中位数如下。本轮未固定逻辑核心和功耗策略，因此绝对值只建立当前主机状态下的探索基线；同一进程内
的 paired ratio 更适合判断默认 gate 是否有效隔离搜索成本。

| metric | five-process median | observed range |
|---|---:|---:|
| whole-only, 1 node / 1 destination | `15.738 us/plan` | `13.341..16.192 us` |
| whole-only, 8 nodes / 2 destinations | `133.324 us/plan` | `122.259..141.096 us` |
| whole-only, 32 nodes / 2 destinations | `693.973 us/plan` | `544.169..714.322 us` |
| forced automatic, 1 node / 2 destinations | `64.785 us/plan` | `56.017..83.094 us` |
| forced automatic, 8 nodes / 2 destinations | `602.204 us/plan` | `472.237..670.952 us` |
| forced automatic, 32 nodes / 2 destinations | `2731.791 us/plan` | `2354.347..2957.784 us` |
| default amortization gate, 1 node / 2 destinations | `18.384 us/plan` | `14.986..20.434 us` |
| default gate / forced search | `0.237712x` | `0.219183..0.296663x` |
| forced automatic 8-node / whole-only | `4.376163x` | `4.198771..4.679480x` |
| forced automatic 32-node / whole-only | `4.158148x` | `3.932262..4.531213x` |

该阶段的 HEFT policy 把 partition evaluation 总预算限制为 `8`，并要求 whole 节点执行时间乘 `expectedReuseCount` 至少覆盖
`50,000 ns` 的最低可摊销收益。当前默认 policy 已由下节的 calibration-driven resolver 取代；`8 / 50 us` 只保留为冷启动策略。

partition scheduler 的 shard source readiness 使用单一连续缓冲区，再以 `std::span` 切分给 route scheduler；因此每次 candidate
evaluation 不再产生逐 shard 的临时内层 vector 分配。当前五进程结果没有固定核心，不能把绝对值变化归因于这一局部优化；后续若
需要发布级 A/B，应在同 revision 中比较连续缓冲开关，并保持 candidate 数、route 数与 evaluation budget 完全相同。

### 2026-07-30 calibration-driven automatic partition 预算基线

默认 `HeftPlanningPolicy` 的硬上限为 `64` evaluations。`HeftAutomaticPartitionTuningPolicy` 在冷启动阶段使用 `8` 次 evaluation、
`50,000 ns` 最低收益和 `4` 个成熟样本；成熟 calibration 使用 `mean + 2 * mean absolute deviation` 作为经验保护成本，并要求
planning cost 至少获得 `4x` reusable whole-time return。该约束限制搜索工作量，不保证搜索一定找到 profitable partition。

calibration shape 由候选轴数量、placement 数量级与 input route 数量级组成。input route count 等于单 shard 最大 route 数乘 policy
允许的 distinct device 数，同一 device 的多条 queue 不会被重复计入。固定桶由 `112` 个扩展至 `392` 个。

| calibration metric | current sample | previous sample | ratio |
|---|---:|---:|---:|
| freeze all cells | `53.246620 ns/operation` | `17.360920 ns/operation` | `3.067039x` |
| frozen lookup | `2.194691 ns/operation` | `2.095753 ns/operation` | `1.047209x` |
| atomic observation | `24.883959 ns/operation` | `18.849573 ns/operation` | `1.320134x` |

snapshot 增量与 cell 数的 `3.5x` 扩张同阶，每次顶层 HEFT invocation 只执行一次。lookup 不随 cell 总数增长；observation 仅在真实
search 后进入控制面。三项数据来自同一会话的独立 benchmark 进程，没有固定核心和功耗策略，只用于数量级与后续回归定位。

五个独立 Release 进程的 end-to-end paired 样本如下。adaptive case 预置四个 `8 us/evaluation` 样本，并设置
`expectedReuseCount == 1000`。

| metric | five-process median | observed range |
|---|---:|---:|
| fixed 8-evaluation search | `70.808 us/plan` | `59.091..78.070 us` |
| default cold/amortized gate | `17.563 us/plan` | `16.578..21.469 us` |
| warm adaptive search | `256.253 us/plan` | `247.300..273.581 us` |
| gate / fixed search | `0.247663x` | `0.240960..0.278828x` |
| adaptive / fixed search | `3.831358x` | `3.272234..4.178496x` |
| adaptive resolved budget | `47 evaluations` | `47..47` |
| adaptive consumed evaluations | `47 evaluations` | `47..47` |

Release 与组合 ASan + UBSan 均显式重建全部 Compute test targets，并通过 `Sora.Compute`：`420 / 420`。定向 HEFT 为
`27 cases / 1256 assertions`，system context 为 `8 cases / 38 assertions`。`Sora.BuildAll` 仍被本轮无关的资源 demo 配置阻断：
`EmbeddedPakModule.cpp` 的依赖扫描找不到 `simdjson.h`；该问题不影响上述 Compute-only 构建与测试结论。

## backend rediscovery 与 epoch diff 的正交基线

`Benchmark.Sora.Compute.Recovery` 现把 runtime read、snapshot refresh、epoch diff 与 recovery 分成独立 case family。epoch diff
构造完整 immutable epoch，并测量纯值 identity comparison；它不调用 backend、planner、compiler、materializer 或 queue。
1-device 与 8-device 场景均替换全部 service，并改变完整 target fingerprint。

| metric | five-process median | semantic boundary |
|---|---:|---|
| current epoch load | `43.867111 ns/operation` | acquire load 与 shared ownership |
| healthy snapshot refresh | `109.766006 ns/operation` | 单设备 observer refresh |
| 1-device full service replacement diff | `319.395065 ns/operation` | service set 与 coordinate replacement |
| 8-device full service replacement diff | `441.309738 ns/operation` | 八设备完整替换与 target comparison |
| uncontended recovery | `2.4 us/recovery` | invalidation、diff、publication allocation 与 epoch publication |

本次五进程采样期间主机调度噪声较高，single-flight waiter 指标波动明显。历史 current load 与 healthy refresh 分别约为
`34.730 / 89.450 ns`，但本阶段没有改变 `Current()` 或 refresh 的生产逻辑；不能以不同主机状态下的两组绝对值直接定性回归。
diff 数值是本阶段新增的数量级基线，发布级判断仍需同 revision、同功耗策略、固定核心和至少五进程 A/B。

rediscovery benchmark 必须保持以下正交性：

- epoch load 与 refresh 不得和 allocation-heavy diff 共享 repetition calibration；否则 diff 分配会污染 read-side cache/allocator 状态；
- 1-device 与 8-device diff 必须保持相同 replacement 语义，只改变规模；
- 不得通过省略完整 `TargetFingerprint` 比较或 service identity conflict validation 降低时间；
- 若同协议 diff 中位数退化超过 15%，且至少四个进程方向一致，应按 allocation、service merge、coordinate merge 与 fingerprint
  comparison 分段审查。

这些指标只约束 runtime control plane。即使 8-device diff 为亚微秒级，也不能推导 GPU kernel、transfer 或 JIT 的执行性能；
portable package validation、target mismatch rejection 与 cold/warm runtime JIT 必须各自建立独立基线。

## portable package exact-key lookup 基线

`Benchmark.Sora.Compute.PortablePackage` 使用一个包含八个 `DeviceExecutable` 的 immutable catalog，分别测量末端 exact hit 与另一完整
`TargetFingerprint` 的 miss。两条 case 共享相同 program、specialization 与 producer identity，只改变 target；测量不包含 backend
import、disk I/O 或 JIT，从而单独约束 package catalog 的 canonical binary search。

| metric | five-process median | semantic boundary |
|---|---:|---|
| exact packaged artifact hit | `91.135597 ns/operation` | 完整 key 比较与 payload span 返回 |
| exact target mismatch | `60.539532 ns/operation` | 完整 key 比较与确定性 miss |

五个独立进程的 exact hit 为 `85.337067..97.531891 ns/operation`，target mismatch 为
`58.249760..69.847965 ns/operation`。每个进程采用九轮交错样本，中位数统计，校准 repetition 为 `1,048,576`。当前数据只建立八
artifact catalog 的数量级基线；catalog 规模扩展应按 `O(log A)` 验证，不得用缩短 `TargetFingerprint` 或跳过完整 key equality 换取时间。

真实首次 materialization 还包含 backend import，package miss 后还可能包含 disk lookup 与 cold JIT。三者必须分别计量；本指标不能用来
推导端到端启动时间，也不能与 resident dispatch 的 `ns/item` 指标比较。

### 2026-07-30 admission provenance 探索基线

portable package registry 增加 immutable admission provenance 后，benchmark 同时约束单 generation-handle 的 exact read path 与
provenance COW write path。五个独立 Release 进程的跨进程中位数如下：

| metric | five-process median | semantic boundary |
|---|---:|---|
| immutable snapshot load | `36.353 ns/operation` | acquire load + 一个 shared snapshot owner |
| package-local exact hit | `60.351 ns/operation` | 八 artifact canonical binary search，不创建 lease |
| frozen exact artifact hit | `75.978 ns/operation` | frozen snapshot search + 单 generation lease |
| registry exact artifact hit | `107.017 ns/operation` | acquire snapshot + search + 单 generation lease |
| register/erase base cycle | `2619.839 ns/operation` | 新 program publication + erase |
| exact duplicate register cycle | `3662.122 ns/operation` | base cycle + 同 provenance duplicate validation |
| new-provenance enrichment cycle | `4114.874 ns/operation` | base cycle + provenance/generation/snapshot COW |
| enrichment / duplicate | `1.125822x` | 同 round normalized paired ratio |

两个 register cycle 都先注册相同的 program，执行第二次注册，再 erase；第二次注册分别是 exact duplicate 与新 verified provenance。
因此 paired ratio 约束 provenance enrichment 的控制面增量，同时保持 program serialization、package validation 与 teardown 边界一致。
enrichment 复用同一 package/program payload owner，只复制很小的 provenance vector 与 registry generation。

采样期间 `D:\repos\GeometricSolver2` 存在单线程后台编译，所以本表是受控协议下的探索数量级，而非发布级 A/B。既有正式
package-local exact hit/miss 基线为 `91.136 / 60.540 ns`；当前对应探索值为 `60.351 / 41.235 ns`，没有观察到退行，但不同 revision
和主机状态下的绝对值不能用于宣称性能提升。正式 gate 仍要求固定核心、固定功耗策略、无并行编译、至少五个独立进程，并在同
revision 比较 single-handle 与候选实现。

### 2026-07-30 current-generation admission revocation 基线

本组使用同一 benchmark executable，固定逻辑 CPU 2，并把 runner priority 设为 High。五个独立进程各自执行九轮交错样本。撤销
cycle 先以 verified provenance 注册 program，再按 subject 撤销；base cycle 注册后按 program erase。两者都包含一次 registration 与
一次 package retirement，paired ratio 因而约束 provenance predicate、generation rebuild 与 index remap 的净控制面成本。

| metric | fixed-core five-process median |
|---|---:|
| immutable snapshot load | `38.367271 ns/operation` |
| package-local exact hit | `57.608891 ns/operation` |
| frozen exact artifact hit | `75.366116 ns/operation` |
| registry exact artifact hit | `110.358810 ns/operation` |
| register/erase base cycle | `2921.997070 ns/operation` |
| exact duplicate cycle | `3951.287842 ns/operation` |
| provenance enrichment cycle | `4460.247803 ns/operation` |
| admission revocation cycle | `3030.328369 ns/operation` |
| revocation / erase paired ratio | `1.035021x` |

固定核心的 registry exact-hit 范围为 `109.654999..112.123680 ns/operation`，相对上一轮未固定核心的跨进程中位数
`107.017 ns/operation` 高 `3.12%`，低于 15% 审查阈值。撤销 cycle 范围为
`2990.023804..3179.376221 ns/operation`。该事务不修改 snapshot load、exact lookup、backend import、submission 或 kernel 数据面；
后续回归应分别约束 read path 与 policy-refresh write path，不能通过减少 provenance identity、跳过最后-admission 判定或追溯改写旧
lease 降低控制面数字。

### 2026-07-31 stage-specific artifact identity 固定核基线

本组把完整 `TargetFingerprint` 投影为 `ExecutableTargetIdentity` 与 `PipelineTargetIdentity`。canonical executable key 扩展为 26 个
`uint64_t` words，其中 target 占 18 words；pipeline key 为 18 words，其中 target 占 10 words；portable-package archive 使用 schema v2。
benchmark 同时约束新 key 的 exact hit/mismatch、registry COW 路径及 archive decode/materialization，避免只测较短的新投影而漏掉序列化成本。

测量机器为 12th Gen Intel Core i7-12700H，Windows 使用平衡电源方案。runner 固定逻辑 CPU 2 和 High priority；五个独立 Release
进程各自执行九轮交错样本，并在每次进程前后确认不存在 `ninja/clang-cl/clang++`。五个 stderr 均为空。

| metric | five-process range | fixed-core median | old fixed-core median | new / old |
|---|---:|---:|---:|---:|
| immutable snapshot load | `36.504889..37.545586 ns` | `37.049389 ns` | `38.367271 ns` | `0.965651x` |
| package-local exact hit | `51.885033..65.105152 ns` | `52.307892 ns` | `57.608891 ns` | `0.907983x` |
| package-local target mismatch | `39.100075..55.807686 ns` | `39.234304 ns` | new | new |
| frozen exact artifact hit | `64.011765..79.762840 ns` | `64.611816 ns` | `75.366116 ns` | `0.857306x` |
| registry exact artifact hit | `97.923660..107.208633 ns` | `100.641823 ns` | `110.358810 ns` | `0.911951x` |
| registry target mismatch | `39.207172..55.743790 ns` | `39.367723 ns` | new | new |
| register/erase cycle | `2792.208862..2857.992554 ns` | `2841.461182 ns` | `2921.997070 ns` | `0.972438x` |
| exact duplicate cycle | `3862.408447..3910.363770 ns` | `3899.267578 ns` | `3951.287842 ns` | `0.986835x` |
| provenance enrichment cycle | `4372.308350..4466.015625 ns` | `4408.532715 ns` | `4460.247803 ns` | `0.988405x` |
| admission revocation cycle | `2854.263306..2924.603271 ns` | `2903.872681 ns` | `3030.328369 ns` | `0.958270x` |
| trusted archive open | `3979.510498..4011.920166 ns` | `3996.362305 ns` | new | new |
| archive materialize | `2211.965942..2218.041992 ns` | `2212.808228 ns` | new | new |
| enrichment / duplicate | `1.128200..1.144550x` | `1.135842x` | new | new |
| revocation / erase | `1.019328..1.024680x` | `1.022885x` | new | new |

八项具有相同语义边界的旧指标均未退行，最大中位数比值为 `0.988405x`，低于 15% 审查阈值。新增 mismatch 与 archive 指标构成本 schema
的首个固定核基线。不同时间窗口的绝对值仍受频率与系统调度影响，因此本表只否定可见退行，不宣称 stage-specific identity 本身提高性能。

正确性证据为 Release `432 / 432`、组合 ASan + UBSan `432 / 432` 与组合 sanitizer 下 `Sora.PAL 50 / 50`。两种 Compute 配置均
显式重建 47 个 test executable 和一个真实 plugin fixture，无 sanitizer 诊断。

## 一般 dispatch DAG lowering 的正交基线

`Benchmark.Sora.Compute.DispatchGraphLowering` 只计量 `LowerDispatchGraph`。fixture 在计时前完成 candidate enumeration、partition
projection 与 selection 构造，因而该指标独立约束 shared physical lowering，不混入设备搜索或 transfer-aware boundary search。

| metric | 输入规模 | five-process median |
|---|---:|---:|
| partitioned producer -> whole consumer | 2 logical nodes / 3 physical dispatches | `11.366 us/lowering` |

计时路径包含六类工作：selection 与 exact binding/route proof、partition residency replay、graph-local allocation/assembly、route
physicalization、completion-set dependency lowering、stable topological canonicalization。它不执行 backend callback、真实 allocation、
copy、kernel dispatch 或 publication commit。

2026-07-30 在连续 Release/ASan 全量构建后得到的一组探索样本为 `13.602..19.651 us/lowering`，中位数
`14.350 us/lowering`。同一会话内样本随进程次序单调升高，主机处于热状态；该数据不能替换正式基线，也不能单独证明代码回归。
后续须在固定核心、固定功耗策略、无并行构建的同 revision 环境做 proof on/off 分段 A/B。若稳定中位数相对 `11.366 us` 增长超过
15%，则按 slice projection proof、route exact union、residency commit、assembly lowering 与 canonicalization 五段剖分；不得删除
validation 或缩小输入图吸收差异。

最终边界收敛与显式 hop-to-segment provenance 修正后，固定逻辑核心的五个独立进程样本为
`12.527 / 12.894 / 13.038 / 13.703 / 12.926 us/lowering`，中位数 `12.926 us/lowering`，相对正式基线高
`13.72%`。测量使用 Windows 平衡电源策略，测前整机 CPU 负载为 `24%`；该样本低于 15% 剖析阈值，支持“未观察到明确退行”，
但不构成发布级 A/B 证据。

本轮正确性证据为 Release `384 / 384` 与 ASan + UBSan `384 / 384`，两种配置均显式重建全部 45 个 Compute 测试目标。

## 动态 backend plugin discovery 基线

`Benchmark.Sora.Compute.RuntimeDiscovery` 把六类成本分开：built-in 单 backend 投影、built-in 八 backend 投影、已驻留 plugin 的单 service
枚举与投影、catalog snapshot lookup、catalog snapshot + plugin projection，以及冷 DLL load + negotiation + projection/publication。resident
路径由 benchmark harness 交错校准；cold 模式每个进程只执行一次操作，防止
Windows loader 以已驻留的同路径模块合并后续 load，从而把 warm load 错记为 cold load。

本组在逻辑 CPU 11、High priority 下顺序运行五个独立 Release 进程。CPU 2 受其他工作负载持续占用，无法在 60 秒内完成同一协议；因此
本表只建立 CPU 11 上的探索基线，不与旧 CPU 2 数值直接比较。

| metric | five-process median | semantic boundary |
|---|---:|---|
| built-in one-backend projection | `457.331085 ns/contribution` | 已构造 backend facade 的 owning value projection |
| built-in eight-backend projection | `1715.530396 ns/contribution` | 同构八设备集合的完整资源投影 |
| eight / one paired ratio | `3.749559x` | 同进程、同轮次归一化规模比 |
| resident plugin one-service projection | `703.926086 ns/contribution` | serialized query + fill + validation + facade adoption |
| cold DLL load + negotiate + projection | `701100 ns/contribution` | 每进程一次 private module load 与 root negotiation |

cold 样本为 `710600 / 665700 / 713700 / 701100 / 685500 ns`，范围约 `1.07x`。resident plugin 路径包含一个 control-plane mutex，
其作用是保证 plugin-owned 嵌套数组在 query、fill 和 facade projection 完成前不被并发 enumeration 改写；该同步不进入 compile、transfer、
submission 或 kernel 数据面。当前数字低于修改前异机状态下的 `947.614 ns` 探索样本，但 built-in 单 backend 也由 `613.838 ns` 降至
`457.331 ns`，因此不能据此宣称 serialization 产生性能提升。正式回归必须在同 revision、固定核心、固定功耗策略与无后台构建条件下做
before/after A/B；不得删除 ABI validation、事务 rollback 或 code lease 以降低控制面数字。

native plugin catalog 加入后，五个独立 Release 进程的 resident catalog/direct paired ratio 为
`1.037509 / 1.015428 / 1.019802 / 1.096907 / 1.066064`，中位数 `1.037509x`。该约 `3.8%` 增量包含一次 atomic
snapshot acquire 与一次 canonical provider lookup，只存在于 discovery/recovery 控制面。

cold plugin 样本为 `2714900 / 2098500 / 1432400 / 828000 / 2063000 ns`，中位数 `2063000 ns`；cold catalog 样本为
`2237000 / 2666900 / 1098300 / 2355500 / 800800 ns`，中位数 `2237000 ns`。两组样本的进程启动、loader、page fault 与系统调度噪声
高于 catalog publication 的预期差值，因此这里只建立量级基线，不解释 `8.43%` 的样本差异。正式 cold A/B 须增加进程数、固定核心与
功耗策略，并随机化模式顺序。

本切片的正确性基线为 Release `408 / 408` 与组合 ASan + UBSan `408 / 408`。两种配置均显式重建 45 个 Compute test executable
和一个真实 plugin DLL fixture；定向 plugin ABI 测试为 `6 cases / 65 assertions`，catalog 与 system integration 测试为
`4 cases / 79 assertions`。

## ComputeContext persistent artifact lifecycle 基线

本阶段改变的是 artifact lifecycle 与解析层级，不是 kernel 数据面。测试固定 `artifactCacheCapacity == 0`，使用相同持久 root
顺序创建两个独立 `ComputeContext`，从而排除 memory-cache hit 与 live backend handle 复用。

| phase | executable | pipeline |
|---|---|---|
| cold context | `1 compile / 1 export / 0 import` | `1 create / 1 export / 0 import` |
| warm context after cold destruction | `0 compile / 0 export / 1 import` | `0 create / 0 export / 1 import` |

该计数是生命周期正确性 gate，不是延迟 benchmark。它证明 package miss 后生成的 target artifact 能跨 context 生命周期复用，且
executable 与 pipeline 两阶段都不会在 warm context 重建。磁盘 lookup、backend import、cold JIT/create、export 与 durable publication
仍需建立彼此正交的 fixed-core 多进程延迟基线；在这些数据完成前，不能宣称 warm-start 提速比例或跨进程编译去重。

实现后的正确性证据为 Release `422 / 422` 与组合 ASan + UBSan `422 / 422`，两种配置均显式重建全部 Compute test targets。
新增端到端 materialization 测试覆盖两个独立 context 的 executable/pipeline 持久复用；新增 system-context 测试覆盖 invalid store
configuration 到 `SystemComputeContextError -> ComputeContextError -> ArtifactStoreError` 的完整错误保真链。

## System artifact store retention 基线

本阶段只改变磁盘 cache 治理控制面，不改变 kernel execution、planner cost model、backend import 或 warm exact-load 实现。benchmark
构造 64 与 256 个 canonical artifact entries，分别测量显式完整 reconciliation、warm exact load、摊销 publication，以及每次
publication 后完整扫描的 eager 对照。五个独立 Release 进程的中位数如下：

| metric | five-process median | semantic boundary |
|---|---:|---|
| exact maintenance, 64 entries | `9.412875 ms` | 完整目录枚举、排序与预算复证 |
| exact maintenance, 256 entries | `28.863600 ms` | 同一算法的规模扩展 |
| 256 / 64 maintenance ratio | `2.969560x` | 同协议 namespace 规模比 |
| warm exact load, 256 entries | `170.141406 us` | 单 key open、header/key/digest 校验与 payload read |
| amortized publication, 256 entries | `5.242825 ms` | 原子写入、常数时间 accounting 与周期 reconciliation |
| eager reconciliation per publication | `32.534400 ms` | 每次写入后完整 namespace scan |
| eager / amortized paired ratio | `6.579049x` | 同进程、同轮次策略比 |

该 paired ratio 否决 eager reconciliation：完整扫描不能进入每次 publication 的默认路径。当前策略在创建 store 时精确扫描，随后
只在估计值越界、累计 256 次 publication 或累计 64 MiB publication 时 reconciliation；`Maintain()` 保留显式精确治理入口。
`Load()` 不更新时间戳，因此该改动没有向 warm lookup 注入写 I/O。

该数据未固定逻辑核心与功耗策略，只建立探索数量级。正式回归须在同 revision 下固定核心、固定功耗策略，并随机化 eager 与
amortized 运行顺序。跨进程共享 namespace 当前只保证周期 reconciliation 最终观察外部 publication；尚未实现 maintenance lease，
因此不得把此表解释为严格的跨进程瞬时容量上限。

实现后的正确性证据为 Release `427 / 427` 与组合 ASan + UBSan `427 / 427`，两种配置均显式重建全部 47 个 Compute test
executable。定向 store、system context 与 PAL environment 测试分别为 `7 cases / 99 assertions`、`13 / 66` 与 `3 / 44`。

## Cross-process artifact lease 基线

该阶段在既有 persistent artifact lifecycle 上增加两个正交协议：按 key digest 映射的有界 materialization stripe，以及
namespace 级 maintenance lease。warm memory/package/disk hit 不取得 materialization lease；只有经过验证的 persistent disk
miss 才进入 `acquire -> second lookup -> follower import or leader create/export/store`。

测量环境为 12th Gen Intel Core i7-12700H、Windows 11 10.0.26200、逻辑 CPU 2、High priority、平衡电源方案。
五个独立 Release 进程各自执行七个内部 median samples，跨进程结果如下。

| metric | run range | five-process median |
|---|---:|---:|
| uncontended materialization lease | `407.995-420.709 us` | `414.573 us` |
| contended release-to-acquire handoff | `641.300-716.800 us` | `678.300 us` |
| exact maintenance, 64 entries | `8.836-9.098 ms` | `9.068 ms` |
| exact maintenance, 256 entries | `25.479-26.919 ms` | `26.516 ms` |
| warm exact load, 256 entries | `156.778-163.167 us` | `162.313 us` |
| amortized publication, 256 entries | `4.625-4.968 ms` | `4.843 ms` |
| eager reconciliation per publication | `30.978-32.353 ms` | `31.883 ms` |
| eager / amortized paired ratio | `6.455-6.739x` | `6.592x` |

第 49 节的旧中位数为 warm load `170.141 us`、publication `5.243 ms`、64/256-entry maintenance
`9.413 / 28.864 ms`。本轮对应指标分别改善约 `4.6%`、`7.6%`、`3.7%` 与 `8.1%`，未观察到性能退行。
两轮没有固定相同背景负载，故这些百分比不能解释为 lease 带来的加速，只能排除明显回归。

无争用 lease 的约 `0.415 ms` 是一次 cached lock-file open、non-blocking native lock 与 close 的固定成本。该成本位于 cold
persistent miss，通常低于 LLVM JIT 或 driver pipeline create；warm exact load 仍不打开锁文件。后续只有在固定功耗、相同
revision 与端到端 JIT workload 下复现占比超过 `5%`，才进入 handle-cache A/B。

正确性基线为 Release 与组合 ASan + UBSan 各 `Sora.Compute 431 / 431`、`Sora.PAL 50 / 50`。PAL 进程测试验证
`TerminateProcess` 或 `SIGKILL` 后由内核释放锁；coordinator 测试验证两个独立 store/cache/service 只执行一次 create，
follower 通过第二次 lookup 导入 publication；timeout 路径保持 live create 可用。

## 同设备多 compiler 候选编排基线

本轮对照保存于重构前的 `Benchmark.Sora.Compute.PlanningInputs` executable 与重构后的当前 executable。PE import table 证明两者均
静态包含 Sora 实现，只动态依赖 libc++ 与 Windows CRT，因此旧 executable 不会在运行时加载当前 Compute 实现。六项旧 case 的
fixture 与语义边界不变；新 executable 另增两个双 compiler case。

测量环境为 12th Gen Intel Core i7-12700H、Windows 平衡电源方案、逻辑 CPU 2、High priority。旧版与新版交错运行，各取五个独立
进程；每个进程内部执行九轮交错 median samples。runner 在每个进程启动前和退出后检查 `ninja/clang-cl/clang++`，全部样本均无
并行编译负载，stderr 为空。

| metric | old range | old median | new range | new median | new / old |
|---|---:|---:|---:|---:|---:|
| 64-key artifact snapshot | `4961.530..5322.278 ns` | `5157.513 ns` | `4640.063..4943.115 ns` | `4716.492 ns` | `0.914489x` |
| calibrated topology projection | `61.708..64.292 ns` | `63.198 ns` | `59.644..62.482 ns` | `60.371 ns` | `0.955268x` |
| transfer-inclusive synthesis | `498.091..579.539 ns` | `529.581 ns` | `517.810..580.020 ns` | `523.315 ns` | `0.988169x` |
| dispatch candidate enumeration | `1089.668..1151.634 ns` | `1140.402 ns` | `1001.393..1048.608 ns` | `1006.207 ns` | `0.882327x` |
| pipeline candidate synthesis | `812.425..882.057 ns` | `836.627 ns` | `762.032..802.441 ns` | `771.742 ns` | `0.922445x` |
| pipeline dispatch enumeration | `1568.909..1680.716 ns` | `1630.844 ns` | `1387.140..1462.866 ns` | `1394.844 ns` | `0.855290x` |

六项可比指标的中位数比值为 `0.855290x..0.988169x`，均低于 1，未触发 15% 退行审查阈值。较低绝对值只支持“未观察到退行”，
不能解释为多 compiler 重构带来加速，因为两个 executable 的链接布局和采样时间顺序也会影响指令与数据缓存。

| new metric | five-process range | median | relative to same-build single compiler |
|---|---:|---:|---:|
| two-compiler candidate synthesis | `847.198..901.013 ns` | `862.125 ns` | `1.647428x` |
| two-compiler dispatch enumeration | `1529.300..1611.200 ns` | `1537.134 ns` | `1.527651x` |

双 compiler 产生两个 placement，但 synthesis 和完整 enumeration 均明显低于 `2x`。原因是 device validation、memory destination、
frozen residency 与 route projection 按物理设备执行一次，只有 compiler-specific target、calibration 与 artifact identity 被扩展。
当前数据不支持为两变体场景引入额外索引所有权。若真实 backend 出现 4/8 个 compiler 与大量 pipeline configuration，应增加按
variant count 的 scaling benchmark；只有观察到 `O(A^2 + T^2)` validation 或线性 lookup 主导后，才改为 request-local canonical
read-only index，并保持现有无序输入语义。

正确性证据为 Release `Sora.Compute 434 / 434` 与组合 ASan + UBSan `434 / 434`。定向结果为
`PhysicalResourceCatalogTest 9 cases / 227 assertions` 和 `DispatchCandidateEnumerationTest 26 / 1078`。

## Artifact resolution calibration 基线

本轮把 planner 的 artifact 成本从单一路径延迟提升为 import 与 production 两项期望贡献。`Persistent` 表示 disk import 与 miss 后
production fallback 的复合过程；`PackageImport`、`TransientProduction` 与 `Live` 保持单路径语义。该改动只进入 fresh planning
control plane 与 cold resolver telemetry，不进入 memory-cache hit、resident submission 或 kernel 数据面。

`Benchmark.Sora.Compute.PerformanceModel` 新增两项正交指标：`artifact_resolution_lookup` 测量 immutable snapshot 上 exact key 的
persistent estimate，`artifact_resolution_observation` 测量 serialized writer 的 copy-on-write publication。两项成本尺度不同，故均用
`MeasureInterleavedIndependent` 独立校准，不能共享 repetition count。正式结果要求五个独立 Release 进程、固定逻辑 CPU 2、High
priority，并在每次进程前后确认不存在 `ninja/clang-cl/clang++`。

PlanningInputs 的正式 A/B 继续使用重构前静态 PE 与当前静态 PE 交错测量。六项同语义指标沿用上一节输入与协议；只有当中位数退行超过
15%，且至少四个进程方向一致时，才进入分段性能审查。artifact lookup 与 observation 是新增指标，没有旧 PE 对照，只建立当前
revision 的绝对量级基线。

正确性证据为 Release 与组合 ASan + UBSan 各 `Sora.Compute 439 / 439`。两种配置均显式重建 47 个 Compute test executable；定向
`PerformanceModelTest` 为 `6 cases / 55 assertions`，`RuntimeTest` 为 `19 / 259`。memory-cache hit 不读取时钟、不写模型；同 key
observer 重入命中已发布 cache，compile count 保持 1。

正式 PlanningInputs A/B 于 i7-12700H、Windows 平衡电源方案下完成。旧版与新版各运行五个交错独立进程，进程固定逻辑 CPU 2、
High priority；runner 在 100 ms 粒度排除 `ninja/clang-cl/clang++`、其他 `Benchmark.*` 与 `UnitTest`。两次先行尝试分别因编译
负载和 Mashiro benchmark 出现而自动中止，未产生可采信样本；下表只包含完整通过门禁的第三次运行。

| metric | old range / median (ns) | new range / median (ns) | new / old |
|---|---:|---:|---:|
| `artifact_availability_snapshot_64` | `4926.080..5193.781 / 4956.708` | `4500.696..4692.798 / 4617.114` | `0.931488x` |
| `calibrated_transfer_topology_projection` | `60.749..67.098 / 62.004` | `59.551..62.842 / 61.464` | `0.991285x` |
| `transfer_inclusive_candidate_synthesis` | `501.761..548.396 / 521.941` | `544.591..561.983 / 557.399` | `1.067936x` |
| `dispatch_candidate_enumeration` | `1089.803..1134.900 / 1097.258` | `1012.962..1049.460 / 1043.060` | `0.950606x` |
| `pipeline_candidate_synthesis` | `806.650..861.368 / 817.355` | `770.996..832.156 / 793.639` | `0.970984x` |
| `pipeline_dispatch_candidate_enumeration` | `1568.964..1636.295 / 1573.895` | `1374.850..1422.281 / 1410.199` | `0.895993x` |

六项可比指标的比值范围为 `0.895993x..1.067936x`。唯一变慢项是 transfer-inclusive synthesis，五个进程方向一致，但中位数
增量为 `6.79%`，低于 15% 分段审查阈值；其余五项持平或改善。当前版 two-compiler synthesis 与完整 enumeration 的中位数分别为
`961.697 ns` 和 `1602.829 ns`，相对同版单 compiler 指标为 `1.725330x` 和 `1.536660x`，仍低于候选数翻倍对应的 `2x` 上界。

`artifact_resolution_lookup` 与 `artifact_resolution_observation` 的五进程绝对基线未写入数值。两次正式尝试均被 runner 检测到新出现的
GeometricSolver2 编译负载并中止；共享机器随后持续运行 `ninja -j1 UnitTest`。本轮保留 benchmark target 与隔离 runner，但不以受污染
样本建立基线。该缺口不改变正确性、热路径或 PlanningInputs A/B 结论；后续首次空闲机器测量应直接补充两项 range 与 median。

## Vulkan device timing 与 observation 基线

本基线约束 Vulkan timestamp query 引入后的三个正交成本：GPU 实际执行时间、host 端 submit/wait/observe round trip，以及已完成
event 的重复 observation。`Benchmark.Sora.Compute.VulkanBackend` 使用 1,048,576-element SAXPY；每个 case 在进程内独立校准，
每个 sample 至少 40 ms，取七个交错 sample 的中位数。下表来自五个独立 Release 进程，queue 均声明
`reportsExecutionNanoseconds == true`，subgroup width 为 32，device/driver/compiler fingerprint 在五次运行中保持不变。

| metric | five-process range | median | semantic boundary |
|---|---:|---:|---|
| resident dispatch | `0.154528..0.202700 ns/element` | `0.198948 ns/element` | host submit、timeline wait、timestamp resolve 与 event 回收 |
| completed event query | `30.349255..37.104654 ns/query` | `36.154795 ns/query` | timeline counter query 与已缓存 duration 读取 |
| GPU device execution | `46,784..48,000 ns/dispatch` | `47,200 ns/dispatch` | `TOP_OF_PIPE` 到 `BOTTOM_OF_PIPE` 的设备区间 |
| host/device paired ratio | `3.463457..4.490933x` | `4.346094x` | 同进程 host round trip 相对设备执行时间 |
| full round trip / resident | `6.122649..7.850205x` | `6.249386x` | upload、dispatch、download 相对 resident dispatch |

resident 中位数对应约 `208.6 us/dispatch`；paired ratio 是逐进程配对后取中位数，不应直接用两项独立中位数相除。cached query 仅约
36 ns，说明当前 host/device 差额主要来自 command resource、queue submission、timeline wait 与驱动调度，而不是 duration 的原子缓存
读取。query pool 已按 256 slot 分块复用；后续优化应优先研究 command/descriptor pool recycling、submission batching 与异步 completion
drain，不能通过删除真实设备计时获得表面改善。

同一 device/driver/compiler fingerprint 下满足以下任一条件时进入性能复审：device execution 中位数退行超过 15%；cached query
退行超过 25%；或 host/device paired ratio 退行超过 20% 且 device execution 保持在 +/-10% 内。所有判定还要求至少四个独立进程方向
一致。跨 GPU、驱动或功耗状态的绝对值不可直接比较；portable 分发应按目标机器重新建立本地 observed calibration。

## Workload-aware cold-start prior 基线

本轮把 `KernelProgram` 的 per-work-item resource demand、`KernelInvocation` 的实际 strided-layout refinement 与多资源 roofline
prior 接入 candidate synthesis。新增逻辑只进入 fresh planning control plane；resident submission、backend queue worker 与逐元素
kernel 数据面均不读取 prior。benchmark 输入继续同时覆盖单/双 compiler、executable/pipeline artifact 与完整 enumeration。

正式样本使用 12th Gen Intel Core i7-12700H、Windows 平衡电源方案、逻辑 CPU 2、High priority。五个独立 Release 进程各自执行
九轮交错 median samples；runner 在每个进程前后排除 `ninja/clang-cl/clang++`、其他 `Benchmark.*` 与 `UnitTest`，五个 stderr
均为空。结果目录为 `C:\tmp\sora-planning-workload-prior-20260731-131417`。

| metric | five-process range | median |
|---|---:|---:|
| `artifact_availability_snapshot_64` | `4670.581..4704.547 ns` | `4698.169 ns` |
| `calibrated_transfer_topology_projection` | `59.841..61.762 ns` | `60.375 ns` |
| `transfer_inclusive_candidate_synthesis` | `559.447..570.052 ns` | `560.880 ns` |
| `two_compiler_candidate_synthesis` | `966.226..977.026 ns` | `974.408 ns` |
| `dispatch_candidate_enumeration` | `1036.916..1056.381 ns` | `1039.403 ns` |
| `two_compiler_dispatch_candidate_enumeration` | `1605.960..1629.654 ns` | `1609.702 ns` |
| `pipeline_candidate_synthesis` | `794.141..808.453 ns` | `796.255 ns` |
| `pipeline_dispatch_candidate_enumeration` | `1404.269..1429.904 ns` | `1408.012 ns` |

与上一正式当前版本基线比较，八项中位数变化约为 `-0.6%..+4.7%`，均低于 15% 分段审查阈值；没有出现候选排序语义完善后
伴随控制面结构性退行的证据。若共享机器出现编译或其他 benchmark 负载，runner 必须中止而不能把受污染样本写入基线。

最终收束复测在全部源码修正、Release/ASan 门禁和 benchmark 重建之后按同一协议执行。首轮尝试在启动前检测到外部
`ninja/clang++` 并中止；下表只记录随后通过全程负载检查的五个独立进程，结果目录为
`C:\tmp\sora-planning-workload-prior-closure-20260731-140533`。

| metric | closure range | closure median | closure / prior formal |
|---|---:|---:|---:|
| `artifact_availability_snapshot_64` | `4734.174..4744.818 ns` | `4738.446 ns` | `1.008573x` |
| `calibrated_transfer_topology_projection` | `61.137..65.468 ns` | `61.903 ns` | `1.025302x` |
| `transfer_inclusive_candidate_synthesis` | `568.709..583.031 ns` | `580.579 ns` | `1.035121x` |
| `two_compiler_candidate_synthesis` | `987.750..1000.278 ns` | `991.353 ns` | `1.017390x` |
| `dispatch_candidate_enumeration` | `1048.863..1055.710 ns` | `1053.485 ns` | `1.013548x` |
| `two_compiler_dispatch_candidate_enumeration` | `1632.681..1653.345 ns` | `1638.889 ns` | `1.018132x` |
| `pipeline_candidate_synthesis` | `800.330..818.283 ns` | `810.284 ns` | `1.017619x` |
| `pipeline_dispatch_candidate_enumeration` | `1422.655..1437.634 ns` | `1432.376 ns` | `1.017304x` |

八项 closure/formal 比值为 `1.008573x..1.035121x`，最大项仍是 transfer-inclusive synthesis 的 `+3.5%`，远低于 15%
分段审查阈值。单/双 compiler 与 executable/pipeline 路径保持同数量级，没有 workload-aware prior 引入结构性控制面退行的证据。

正确性门禁为 Release 与组合 ASan + UBSan 各 `Sora.Compute 448 / 448`。定向 `PerformanceModelTest` 为
`7 cases / 81 assertions`，`KernelInvocationTest` 为 `7 / 96`，`PhysicalResourceCatalogTest` 为 `10 / 262`。测试同时固定
observed-over-prior 偏序、invalid workload 拒绝、真实 strided refinement 与全 64 位输入域内的无溢出比例算术。
