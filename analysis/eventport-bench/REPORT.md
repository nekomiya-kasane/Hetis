# EventPort Access Benchmark

## Scope

This benchmark compares four ways to obtain and call an EventPort without adding files or targets under `Sora/`:

1. `kernel_extension`: `Sora::Kernel::EventPort` as a `BaseUnknown` `DataExtension`.
2. `com_adaptor_extension`: `Sora::Experimental::EventPort` wrapped by `ComAdaptor<EventPort>` as a `DataExtension`.
3. `plain_inline_member`: plain non-`ComPtr` object with an in-class `Sora::Experimental::EventPort events` member.
4. `plain_external_unordered_map`: plain non-`ComPtr` object resolved by ADL `EventPortOf(object)` through
   `std::unordered_map<object*, EventPort>`.

The benchmark measures:

- `port_lookup`: only obtain the port. The lookup test rotates over 1024 objects to avoid constant-folding a single
  object/member address.
- `lookup_plus_emit`: obtain the port and synchronously emit one small event payload with 0, 1, 8, and 64 listeners.

## Build And Run

Profile timing:

```powershell
python T:\toolchains\coca-toolchain-p2996\setup.py env --shell powershell | Invoke-Expression
cmake -S analysis/eventport-bench -B build/analysis-eventport-x64-profile -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=T:/toolchains/coca-toolchain-p2996/cmake/toolchain.cmake `
  -DCMAKE_MAKE_PROGRAM=T:/toolchains/coca-toolchain-p2996/tools/ninja/ninja.exe `
  -DCOCA_TARGET_PROFILE=win-x64-clang -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG -fno-omit-frame-pointer -fno-optimize-sibling-calls" `
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG -fno-omit-frame-pointer -fno-optimize-sibling-calls"
cmake --build build/analysis-eventport-x64-profile --target Sora.EventPortBench --parallel
.\build\analysis-eventport-x64-profile\bin\Sora.EventPortBench.exe `
  analysis\eventport-bench\results\eventport-bench-x64-profile.csv
```

ASan/UBSan smoke:

```powershell
.\build\analysis-eventport-x64-asan\bin\Sora.EventPortBench.exe `
  analysis\eventport-bench\results\eventport-bench-x64-asan-smoke.csv 100
```

The optional numeric argument is a scale divisor. `100` runs 1/100 iterations and is intended for sanitizer smoke only.

## Profile Result

COCA clang-p2996, `RelWithDebInfo`, `-O2`, `-DNDEBUG`, frame pointers enabled.

| scenario | path | listeners | iterations | best ns/op | median ns/op |
|---|---:|---:|---:|---:|---:|
| port_lookup | kernel_extension | 0 | 500000 | 170.639 | 178.462 |
| port_lookup | com_adaptor_extension | 0 | 500000 | 158.407 | 175.517 |
| port_lookup | plain_inline_member | 0 | 500000 | 0.635 | 0.648 |
| port_lookup | plain_external_unordered_map | 0 | 500000 | 11.400 | 13.386 |
| lookup_plus_emit | kernel_extension | 0 | 100000 | 265.681 | 273.535 |
| lookup_plus_emit | com_adaptor_extension | 0 | 100000 | 251.096 | 266.901 |
| lookup_plus_emit | plain_inline_member | 0 | 100000 | 117.284 | 124.521 |
| lookup_plus_emit | plain_external_unordered_map | 0 | 100000 | 123.633 | 130.907 |
| lookup_plus_emit | kernel_extension | 1 | 50000 | 341.050 | 371.634 |
| lookup_plus_emit | com_adaptor_extension | 1 | 50000 | 265.220 | 269.264 |
| lookup_plus_emit | plain_inline_member | 1 | 50000 | 114.568 | 122.000 |
| lookup_plus_emit | plain_external_unordered_map | 1 | 50000 | 116.234 | 131.816 |
| lookup_plus_emit | kernel_extension | 8 | 15000 | 954.687 | 1130.900 |
| lookup_plus_emit | com_adaptor_extension | 8 | 15000 | 314.273 | 344.240 |
| lookup_plus_emit | plain_inline_member | 8 | 15000 | 142.933 | 146.200 |
| lookup_plus_emit | plain_external_unordered_map | 8 | 15000 | 139.667 | 157.980 |
| lookup_plus_emit | kernel_extension | 64 | 4000 | 5578.250 | 5812.975 |
| lookup_plus_emit | com_adaptor_extension | 64 | 4000 | 625.100 | 746.850 |
| lookup_plus_emit | plain_inline_member | 64 | 4000 | 402.850 | 404.300 |
| lookup_plus_emit | plain_external_unordered_map | 64 | 4000 | 394.850 | 460.250 |

Raw CSV: `analysis/eventport-bench/results/eventport-bench-x64-profile.csv`.

## Interpretation

`plain_inline_member` is the lower bound: the port is at a fixed offset in the object, so lookup is just address
calculation plus table iteration overhead. `plain_external_unordered_map` adds a hash lookup and lands around 10-13 ns
for lookup-only, but the difference mostly disappears once `Emit` itself dominates.

Both extension routes are roughly two orders of magnitude slower for lookup-only because they go through
`QueryInterfaceRaw`: nucleus discovery, metaclass/provider lookup, extension-node snapshot, lazy-extension cache lookup,
and locking in the closure state. This is the price of open object-model extensibility.

`kernel_extension` becomes much slower as listener count grows because its dispatch path uses `BaseUnknown` weak-retainer
checks and Kernel event context construction. The experimental `ComAdaptor<EventPort>` route still pays QI for lookup,
but its actual `Experimental::EventPort` dispatch path is much lighter, so at 64 listeners it is about an order of
magnitude faster than the Kernel EventPort path in this benchmark.

Architecturally, the result is coherent:

- Use in-class `EventPort` for hot, plain C++ objects when the event capability is intrinsic to the type.
- Use external `unordered_map` only when the object layout must not change; lookup is acceptable but still not free.
- Use `ComAdaptor<EventPort>` when a `BaseUnknown` object needs dynamic extension semantics but the lighter experimental
  event dispatch model is desired.
- Use Kernel `EventPort` when integration with the full Kernel object model, weak closure ownership, and Kernel event
  semantics matter more than raw dispatch cost.

## Verification

- `x64-profile` benchmark build and full run succeeded.
- `x64-asan` benchmark build succeeded.
- `x64-asan` smoke run with `scale_divisor=100` succeeded without sanitizer diagnostics.
