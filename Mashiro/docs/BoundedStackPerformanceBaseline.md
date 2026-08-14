# BoundedStack Performance Baseline

## Scope

This baseline covers `Mashiro::BoundedStack<T, Capacity, Topology>` against the Windows x64 `SLIST` primitive.
Both implementations are allocation-free after construction and use one ready collection plus one bounded free-node
collection. The comparison measures completed value transfers, validates the count and identity checksum, and reports
paired throughput ratios rather than ratios of independently aggregated samples.

The topology is selected at translation time:

- Local uses a direct array-backed stack.
- SPSC uses a producer-to-consumer index ring, a consumer-local LIFO chain, and a return index ring. Its push and pop
  paths contain no CAS and have wait-free bounded loops.
- MPSC moves each visible ready snapshot into its sole consumer's local LIFO chain. Every pop checks the shared head
  first, so newer publications remain ahead of older local values while most cached pops avoid a ready-head CAS.
- SPMC moves returned free-node snapshots into its sole producer's local free chain, amortizing free-head CAS without
  changing value order.
- MPSC, SPMC, and MPMC retain tagged indexed Treiber heads wherever multiple endpoints can mutate one collection and
  provide lock-free progress.

## Environment

| Property | Value |
|---|---|
| Date | 2026-08-14 |
| CPU | Intel Core i7-12700H, 14 cores, 20 logical processors |
| OS | Windows 11 Pro 10.0.26200 |
| Toolchain | COCA clang-p2996, LLVM 21, libc++, `-std=gnu++26 -freflection-latest` |
| Build | `x64-release`, `-O2 -DNDEBUG` |
| Placement | Windows CPU sets, high-priority benchmark process |
| Capacity | 1,024 values |
| Payloads | 8 bytes and 64 bytes |
| Samples | Two runs, 9 paired samples per run, 2 warmups, at least 400 ms per measured sample |
| Reference | Windows `InterlockedPushEntrySList` / `InterlockedPopEntrySList` bounded by 1,024 nodes |

Each pair alternates which implementation runs first. Scenario order is shuffled, and both implementations in a pair
transfer the same calibrated item count.

## Results

The table combines 18 paired ratios per scenario. `Median gain` is the median of
`Mashiro throughput / SLIST throughput - 1`; `Wins` counts positive paired ratios.

| Topology | Payload | Median gain | Wins |
|---|---:|---:|---:|
| SPSC | 8 B | +338.19% | 18 / 18 |
| SPSC | 64 B | +115.19% | 18 / 18 |
| MPSC | 8 B | +15.04% | 17 / 18 |
| MPSC | 64 B | +40.23% | 17 / 18 |
| SPMC | 8 B | +19.65% | 18 / 18 |
| SPMC | 64 B | +15.51% | 18 / 18 |
| MPMC | 8 B | +13.60% | 15 / 18 |
| MPMC | 64 B | +28.07% | 16 / 18 |

The SPSC assembly probes `MashiroSpscStackPush` and `MashiroSpscStackPop` contain no `lock` or `cmpxchg`
instructions. Multi-endpoint probes retain the tagged CAS loops required by their endpoint cardinalities.

## Reproduction

```powershell
python T:\toolchains\coca-toolchain-p2996\setup.py env --shell powershell | Invoke-Expression
.\build\x64-release\bin\Benchmark.Mashiro.Queues.exe `
  --stacks --samples 9 --warmups 2 --items 100000 --minimum-ms 400 `
  --output build/x64-release/stack-bounded-baseline.csv --label bounded-stack-baseline
```

Run the command twice before comparing a change. Preserve all paired samples; do not select only the faster run.
Results are machine- and OS-build-specific and establish a local regression baseline, not a universal throughput
constant.
