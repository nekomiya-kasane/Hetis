# Sora Math Performance Baseline

## 1. Purpose

This document defines correctness, code-generation, and timing sentinels for the canonical Sora mathematical value
system. A timing result is invalid unless numerical validation passes first. SIMD speedup is always qualified by its
kernel, storage layout, and baseline execution policy.

The baseline has two independent executables:

- `Benchmark.Sora.Math.Value` covers a memory-sensitive `Mat3f * Vec3f + bias` kernel, Eigen interoperability, and
  already-resolved runtime dispatch.
- `Benchmark.Sora.Core.SOA` covers an eight-field `fma + exp + sin` particle kernel lifted through reflection-generated
  SoA storage.

Absolute times from these different kernels must not be compared.

## 2. Execution Policies

The metric names distinguish four execution policies:

| Policy | Meaning |
|---|---|
| `aos_sample_loop` | AoS traversal with cross-sample LoopVectorizer and interleaving disabled |
| `soa_sample_loop` | SoA traversal with cross-sample LoopVectorizer and interleaving disabled |
| `soa_auto` / `soa_adapted` | Compiler-vectorized SoA traversal over the ordinary scalar source operation |
| `soa_explicit_simd` | `TransformBatch` over Sora's preferred explicit SIMD carrier with unroll 4 |
| `soa_manual_simd` / `soa_manual` | Hand-written SIMD or vectorizable SoA oracle independent of the abstraction |

`sample_loop` means that independent samples are not widened by the LoopVectorizer. It does not promise an absence of
all vector instructions: LLVM's SLP Vectorizer may still pack independent components inside one mathematical value.

All `*_speedup_over_*` metrics are paired `baseline_time / optimized_time` ratios. The
`*_manual_paired_ratio` metrics are `abstraction_time / manual_oracle_time`; their ideal value is one.

## 3. Fixed Build Configuration

Official measurements use:

```text
toolchain: COCA clang-p2996, LLVM/Clang 21, libc++
language:  -std=gnu++26 -freflection-latest
compile:   -O3 -march=haswell -fno-math-errno -ffp-contract=off
dataset:   2^18 elements
samples:   11 per process, median
duration:  common power-of-two repetition count calibrated so every case is at least 100 ms per sample
ordering:  alternating forward/reverse rotation; ratios use same-sample pairs before taking the median
```

Run at least five fresh processes. Record CPU, OS, power plan, affinity, toolchain revision, source revision, and
machine load. On hybrid CPUs, pin the parent process to a verified performance core; child benchmarks inherit the
affinity. Logical CPU numbering is machine-specific and must not be copied blindly.

Example for the baseline machine, where logical CPU 2 is a P-core:

```powershell
$current = [System.Diagnostics.Process]::GetCurrentProcess()
$current.ProcessorAffinity = [System.IntPtr]4
$current.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::High
1..5 | ForEach-Object { .\build\x64-release\bin\Benchmark.Sora.Math.Value.exe }
1..5 | ForEach-Object { .\build\x64-release\bin\Benchmark.Sora.Core.SOA.exe }
```

## 4. Version 2 Baseline

Recorded on 2026-07-21 after introducing adaptive interleaved measurement and explicit execution-policy names.

```text
CPU:        12th Gen Intel Core i7-12700H, 14 cores / 20 logical processors
affinity:   logical CPU 2, verified P-core on this machine
OS:         Windows 11 Pro 10.0.26200
build:      RelWithDebInfo plus benchmark-local O3/Haswell flags
processes:  5
```

### 4.1 Value And Dispatch Times

| Metric | Median ns/element | Relative MAD |
|---|---:|---:|
| Vec3f AoS sample loop | 1.428718 | 0.40% |
| Vec3f SoA sample loop | 1.616149 | 1.21% |
| Vec3f SoA auto-vectorized | 0.731624 | 2.01% |
| Vec3f SoA explicit SIMD | 0.701270 | 1.96% |
| Vec3f SoA manual SIMD | 0.686139 | 1.07% |
| Eigen Vec3f AoS sample loop | 4.054724 | 0.41% |
| Direct static kernel | 0.478171 | 0.15% |
| Resolved static kernel | 0.477604 | 0.09% |
| Cached JIT executable | 0.477846 | 0.33% |

### 4.2 Value Paired Ratios

| Metric | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Median |
|---|---:|---:|---:|---:|---:|---:|
| SoA sample-loop speedup over AoS | 0.771 | 0.882 | 0.887 | 0.883 | 0.877 | 0.882 |
| SoA auto speedup over sample loop | 2.179 | 2.229 | 2.210 | 2.256 | 2.246 | 2.229 |
| Explicit SIMD speedup over sample loop | 2.311 | 2.333 | 2.303 | 2.333 | 2.340 | 2.333 |
| Explicit SIMD speedup over AoS | 1.770 | 2.050 | 2.040 | 2.067 | 2.039 | 2.040 |
| Explicit SIMD speedup over SoA auto | 1.050 | 1.051 | 1.054 | 1.040 | 1.043 | 1.050 |
| Explicit/manual paired time ratio | 0.983 | 1.017 | 1.007 | 1.020 | 1.008 | 1.008 |
| Resolved static/direct time ratio | 1.000 | 1.000 | 1.008 | 0.994 | 0.998 | 1.000 |
| Cached JIT/direct time ratio | 1.004 | 0.988 | 0.996 | 0.999 | 0.999 | 0.999 |
| Portable direct/compiled direct time ratio | 0.980 | 1.000 | 1.006 | 0.996 | 0.993 | 0.996 |

The affine kernel is sufficiently light that its auto-vectorized and explicit SIMD paths occupy the same throughput
tier. The explicit path is only about 5% faster than auto-vectorized SoA, while both are more than twice as fast as the
cross-sample scalar policy. This is expected and is not a SIMD regression.

The portable-direct comparison is a separate two-case paired experiment. It compares the direct evaluator returned by
`Compile<2>` with the direct evaluator carried by `CompilePortable<float, 2>`; neither invokes the runtime planner or
reads Kernel IR. Their element loops have identical instructions, and the five-process median ratio is `0.995942`.

### 4.3 Reflection-Lifted Particle Kernel

| Metric | Median ns/element | Relative MAD |
|---|---:|---:|
| Particle AoS sample loop | 7.910514 | 0.57% |
| Particle SoA sample loop | 11.035609 | 0.59% |
| Particle SoA adapted | 3.854623 | 3.08% |
| Particle SoA manual | 3.757837 | 5.84% |

| Paired metric | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Median |
|---|---:|---:|---:|---:|---:|---:|
| Adapted speedup over AoS | 2.042 | 1.623 | 2.218 | 2.105 | 2.126 | 2.105 |
| Adapted speedup over sample loop | 2.888 | 2.199 | 3.132 | 2.976 | 2.928 | 2.928 |
| Adapted/manual paired time ratio | 0.984 | 1.332 | 1.023 | 1.049 | 0.978 | 1.023 |

This compute-heavy kernel is the source of the historically observed approximately `3x` SIMD speedup. That ratio is
against the SoA sample-loop policy, not against AoS and not against an already auto-vectorized SoA loop. Occasional
runs show scheduler or thermal-frequency drift, so paired ratios are more informative than isolated absolute medians.

## 5. Code-Generation Gates

Release object-code inspection on the baseline build established:

- `Value.SoA.SampleLoop`: zero YMM operands and zero packed FMA instructions.
- `Value.SoA.Auto`, `Value.SoA.Explicit`, and `Value.SoA.Manual`: YMM arithmetic and packed FMA, with no calls.
- `Core.SoA.Adapted` and `Core.SoA.Manual`: identical counts of 60 YMM operands, four packed FMA instructions, three
  packed multiplies, and four direct SVML calls; neither contains an indirect call.
- `Compile<2>` and `CompilePortable<float, 2>` direct scale loops: instruction-for-instruction identical scalar loops;
  the portable module is not observed by the hot path.

ASan and UBSan executions of both complete benchmarks produced no diagnostics. Sanitizer timings are invalid for
performance comparison because instrumentation density differs across abstraction boundaries.

## 6. Regression Decision Rule

A performance regression is actionable only when all conditions hold:

1. Numerical checks and checksums pass.
2. Build flags, affinity, power policy, and machine identity match the baseline.
3. At least five fresh processes reproduce the result.
4. The cross-process median worsens by more than `max(5%, 3 * baseline_relative_MAD)`.
5. The result reproduces in a second independent five-process set.

Primary architectural gates:

- explicit/manual and adapted/manual paired ratios must remain statistically centered on one;
- portable-direct/compiled-direct paired ratio must remain statistically centered on one;
- runtime dispatch must remain a once-per-batch cost, never an element-loop operation;
- the sample-loop, auto-vectorized, and explicit SIMD code-generation classifications above must remain distinct;
- `maximum_relative_error <= 2.0e-6`;
- sanitizer runs must remain diagnostic-free.

## 7. Historical Version 1 Data

The 2026-07-20 Value benchmark reported medians of `1.274395 ns/element` for AoS, `0.732517` for a path labelled SoA
scalar, and `0.709248` for explicit SIMD, producing `1.796827x`. Disassembly later proved that the labelled SoA scalar
path was already AVX2-vectorized, and each timing sample was shorter than one millisecond. Those values are retained as
historical observations but are superseded by version 2 and must not be used as CI thresholds.

An earlier complex particle benchmark reported `2.56x-3.11x` SIMD speedup over scalar SoA. Version 2 recovers the same
approximately `3x` central result under an explicit sample-loop policy and a materially more stable measurement method.
