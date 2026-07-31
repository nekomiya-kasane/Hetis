# Sora Compute Graph Performance Baseline

Date: 2026-07-21

## Scope

This baseline measures runtime logical-graph hazard analysis. It does not measure kernel execution, device transfer,
JIT compilation, or queue submission.

The workload contains `N` disjoint four-byte writers to one logical buffer, followed by one reader covering the
whole buffer and one whole-buffer writer. It simultaneously exercises:

- `N` interval boundaries without pairwise conflicts among the initial writers;
- an unavoidable `N`-edge reader fan-in;
- frontier compression from all reader-covered intervals to one final dependency;
- allocation, interval split/update/merge, edge sorting, and result ownership.

The canonical output contains exactly `N + 1` edges. A dense all-node-pairs implementation would perform quadratic
conflict checks even though the required dependency count is linear.

## Environment

| Property | Value |
|---|---|
| CPU | 12th Gen Intel Core i7-12700H, 14 cores / 20 logical processors |
| Affinity | logical CPU 0 |
| OS | Windows 11 Pro 10.0.26200, x64 |
| Toolchain | COCA clang-p2996, Clang 21.0.0git, libc++ |
| Language | `-std=gnu++26 -freflection-latest` |
| Build | `build/x64-release`, RelWithDebInfo + IPO |
| Benchmark flags | `-O3 -march=haswell` |
| Samples | 5 pinned processes; each uses 9 median samples and adaptive minimum 50 ms per sample |

Build and run:

```powershell
cmake --build build/x64-release --target Benchmark.Sora.Compute.Graph --parallel
build\x64-release\bin\Benchmark.Sora.Compute.Graph.exe
```

The recorded run was externally pinned to logical CPU 0.

## Baseline

| Metric | Five-process median |
|---|---:|
| 1K intervals | 721.768397 ns/access |
| 4K intervals | 783.340959 ns/access |
| 16K intervals | 944.266447 ns/access |
| 4K / 1K normalized cost | 1.102553 |
| 16K / 4K normalized cost | 1.185149 |

A fourfold boundary increase raises unit cost by about 10-19%, consistent with ordered interval lookup plus emitted
edges. It does not produce the approximately fourfold unit-cost increase expected from pairwise quadratic scanning.

## Regression Policy

Performance comparisons must use the same machine, affinity, build profile, and workload. Record both absolute
`ns/access` and normalized growth ratios.

Investigate when either condition is repeatable across at least three process runs:

- same-size median `ns/access` regresses by more than 15%;
- either fourfold normalized growth ratio exceeds 1.5.

Do not turn these machine-specific values into cross-machine unit-test assertions. Correctness tests separately prove
the exact edge frontier at 4096 intervals under ASan and UBSan.

## Optimization Direction

The current ordered interval index uses node-based `std::map` storage and immutable shared hazard states. If graph
planning becomes material in end-to-end traces, the next optimization candidate is a contiguous arena-backed ordered
interval tree with state IDs. Any replacement must first beat this baseline while preserving partial-region,
reader-fan-out, no-self-edge, OOM, and deterministic-edge-order tests.
