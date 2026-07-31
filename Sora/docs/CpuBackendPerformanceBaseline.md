# CPU Backend Performance Baseline

## 1. 基线用途

本基线回答三个彼此独立的问题：内建 scalar/SIMD provider 的执行宽度收益，AoS/SoA layout 对 Vec3 全分量加法的影响，
以及 LLVM native provider 相对解释执行路径的 steady-state 收益。三者不得合并为一个“SIMD 加速比”，因为 LLVM 数据同时
包含消除逐元素解释、循环优化和自动向量化的收益。

## 2. 实验环境

| 项目 | 值 |
|---|---|
| 日期 | 2026-07-24 |
| 构建 | `x64-release`，COCA clang-p2996，C++26 |
| 操作系统 | Windows x86-64 |
| logical processors | 20 |
| maximum fixed vector width | 256 bit |
| architecture id | `10022672816754571385` |
| native provider | dynamically loaded LLVM-C 22 / ORC LLJIT |
| benchmark | `Benchmark.Sora.Compute.CpuBackend` |

architecture id 是本机 fingerprint 的摘要，不是跨版本稳定的 CPU 型号编号。硬件、OS vector state、LLVM identity 或
lowering schema 改变后，原数据只能作为历史样本，不能继续充当同一性能总体的观测值。

## 3. 测量协议

每个 case 先执行三次 warmup，再独立校准 repetition count，使单个 sample 至少运行 40 ms。正式阶段采集七轮样本；每轮
旋转 case 起点，并交替使用正序和逆序，最终报告 median。ratio 按同轮归一化结果计算后取中位数，因此比两个独立 median
相除更能抵抗频率和系统负载漂移。

SAXPY 六条路径消费相同输入和同一 `KernelProgram`。`KernelWorkload` 构造阶段完成 program compilation、LLVM object
emission 与 ORC materialization；计时阶段只包含 queue submit、native/interpreter dispatch 和 terminal wait，不包含 JIT
冷启动、输入初始化、host/device transfer 或结果验证。测试前单独验证数值结果，计时后还检查 path statistics：LLVM case
必须增加 `nativeWorkItems`，scalar/SIMD case 必须进入各自声明的路径。

该定义是 steady-state dispatch 基线。cold JIT、disk import、跨设备 transfer 和 graph placement 需要独立 benchmark，
不能从本表外推。

## 4. SAXPY steady-state

kernel 为 `output[i] = fma(alpha, x[i], y[i])`，每个元素读取两个 `float` 并写入一个 `float`。

| elements | SIMD fallback (ns/element) | LLVM JIT (ns/element) | LLVM over SIMD fallback |
|---:|---:|---:|---:|
| 4,096 | 34.68 | 2.420 | 14.71x |
| 32,768 | 26.86 | 0.877 | 30.40x |
| 131,072 | 23.11 | 0.776 | 32.19x |
| 262,144 | 22.00 | 0.667 | 32.27x |
| 524,288 | 22.99 | 0.574 | 39.87x |
| 1,048,576 | 21.29 | 0.718 | 29.66x |

131K 至 1M 的 LLVM 结果对应约 15 至 21 GB/s 的三流有效带宽，数量级与单 socket 内存层次相容。30x 至 40x 的比值不表示
LLVM 自动向量化比手写 SIMD 宽 30 倍；内建 fallback 仍逐元素解释 `KernelProgram`，其 opcode dispatch、binding lookup 与
控制流开销主导总时间。LLVM provider 把这些工作移到编译阶段，热循环只保留已优化机器码。

## 5. Vec3 layout 与 SIMD

Vec3 kernel 对两个数组执行全分量加法并写回全部三个分量，元素数为 262,144。

| layout/path | median (ns/element) | paired relation |
|---|---:|---:|
| AoS scalar | 97.54 | baseline |
| AoS SIMD | 48.31 | scalar/SIMD = 2.11x |
| SoA scalar | 91.24 | layout comparison only |
| SoA SIMD | 44.68 | scalar/SIMD = 2.04x |

同轮 paired ratio 中，SoA SIMD 相对 AoS scalar 为 2.14x，SoA SIMD 相对 AoS SIMD 为 1.008x。后一个比值接近 1，说明
当前 kernel 的主要收益来自执行宽度，而非 layout。AoS/SoA 都连续读取全部分量，既没有分量裁剪，也没有跨 kernel SoA
residency；在这个访问模型中声称 SoA 应额外获得约 3x 缺乏依据。

## 6. 回归判定

正确性门禁先于时序门禁：LLVM case 必须产生精确结果且只增加 `nativeWorkItems`；任何 scalar/SIMD fallback 都属于路径
回归。性能门禁分为本机硬件基线和 portable 趋势基线。

| 条件 | 本机告警阈值 | 解释 |
|---|---:|---|
| LLVM，4,096 elements | speedup < 10x | 捕获 JIT 路径退化或解释器误入 |
| LLVM，32,768 至 1,048,576 elements | speedup < 20x | 低于现有 29.66x 至 39.87x 区间并保留噪声余量 |
| LLVM，131,072 至 1,048,576 elements | > 1.0 ns/element | 仅适用于相同 host、构建与电源策略 |
| Vec3 AoS/SoA SIMD | paired speedup < 1.8x | 捕获 fallback SIMD 失效，不约束 layout 优劣 |

普通 CI 若不固定 host、频率、核心亲和性和系统负载，只报告七样本 median、paired ratio 与历史分位数，不以绝对
`ns/element` 判失败。稳定性能机应对同一 revision 运行至少三个独立进程；若新 median 相对基线退化超过 15%，且三个进程
方向一致，则进入性能审查。阈值变化必须附带 benchmark 输出、CPU metadata 和实现机制说明，不能通过改宽阈值吸收回归。

## 7. 后续基线

异构执行完成前还需建立四组正交测量：cold compile 与 warm object import、resident GPU dispatch、transfer-inclusive graph、
device-loss 后的 cache/pipeline 恢复。placement planner 只能消费这些实测 cost model；不能以元素数量或设备类别硬编码
CPU/GPU 分界。

## 8. Backend ABI v4 回归样本

独立 `DevicePipeline` stage 与 Backend ABI v4 合入后，重新构建 Release benchmark，并在同一 host 上执行三个独立进程。
该样本用于验证 ABI/cache 边界未进入 CPU steady-state 热路径，不替代第 4、5 节的历史基线。

| 指标 | 三进程中位数 |
|---|---:|
| 1M scalar serial | `40.98 ns/element` |
| 1M SIMD serial | `19.87 ns/element` |
| 1M adaptive | `14.92 ns/element` |
| adaptive / SIMD serial paired speedup | `1.301x` |
| 1M LLVM JIT | `0.788 ns/element` |
| LLVM JIT / SIMD fallback paired speedup | `25.56x` |
| Vec3 AoS scalar / SIMD | `78.56 / 34.73 ns/element` |
| Vec3 SoA scalar / SIMD | `80.27 / 35.16 ns/element` |
| Vec3 AoS / SoA SIMD speedup | `2.247x / 2.264x` |
| Vec3 SoA SIMD / AoS SIMD | `1.008x` |
| pipeline round-trip / dispatch | `1.004` |

三个进程的 1M adaptive 为 `14.92 / 14.88 / 15.47 ns/element`，没有一致方向的 15% 退行。pipeline paired ratio 的
三个样本为 `0.808 / 1.004 / 1.010`；首个样本由 dispatch 单侧升至 `73.75 ns/element` 造成，三进程中位数仍接近 1。结论与
原基线一致：当前波动主要来自线程调度与 host 状态，stage identity、pipeline service 和持久化协调没有进入逐元素热循环。

## 9. Physical materialization pipeline 回归样本

`BackendPipelineFactory` 注册与 materializer 自动 `DeviceExecutable -> DevicePipeline` 解析完成后，重新构建 Release benchmark，
执行三个独立进程。该变更位于 graph materialization 冷路径；steady-state benchmark 用于排除目录与 schema identity 误入 dispatch
热循环。

| 指标 | 三进程中位数 | ABI v4 样本 | 判定 |
|---|---:|---:|---|
| 1M adaptive | `14.79 ns/element` | `14.92 ns/element` | 无一致退行 |
| adaptive / SIMD serial paired speedup | `1.292x` | `1.301x` | 结构一致 |
| Vec3 AoS SIMD speedup | `2.259x` | `2.247x` | 通过 `1.8x` 门槛 |
| Vec3 SoA SIMD speedup | `2.203x` | `2.264x` | 通过 `1.8x` 门槛 |
| pipeline round-trip / dispatch | `1.003` | `1.004` | 未进入热路径 |

三个进程的 1M adaptive 为 `15.08 / 14.79 / 14.70 ns/element`；pipeline ratio 为
`0.991 / 1.005 / 1.003`。所有 paired 性能门槛均通过，且不存在三个进程同向超过 15% 的退行。
