# Yuki Y2 Core Foundation — A3 Design Spec

**Date:** 2026-06-19
**Status:** Approved for implementation planning
**Scope:** Tasks T21–T23 — introspection surface, closure-walking helpers, runtime-maturity items
deferred from A2. Closes the three-slice Y2 foundation (A1 + A2 + A3) for `Yuki::Core`.

**Parent design:** `docs/superpowers/specs/2026-06-17-yuki-object-model-y2-design.md`
**Parent plan:** `docs/superpowers/plans/2026-06-17-yuki-y2-core-plan.md`
**Predecessor slice:** `docs/superpowers/plans/2026-06-18-yuki-y2-core-foundation-a2.md`

---

## 0. Goals

A3 closes the four open items A2 explicitly deferred:

1. **Introspection (T21)** — type-erased queries over `RootObject*`: "what does this closure provide?".
2. **Closure helpers (T22)** — walk from any node to its nucleus, facades, extensions; same-closure predicate.
3. **Runtime maturity (T23, four sub-pieces):**
   - Per-thread epoch-RCU replacing A2's unbounded `RetiredPool`.
   - D16 base-chain `mergedDispatch` flatten at `Install<E>` time.
   - Full `SideTableResolver` and `CodeExtensionSingleton` arm wiring (A2 only wired `InlineFacade`).
   - Nucleus dtor walker that deletes parked `Anno::Eager` extensions (D11 final teardown).

A3 ships no new dispatch semantics beyond what the Y2 parent spec already defines (D14 arm kinds,
D15 four-level cache, D16 base-chain flatten, D11 eager parking). Every section below realizes part
of the parent spec into code.

---

## 1. Architecture Overview

### 1.1 Files added

| File | Purpose |
|------|---------|
| `Yuki/include/Yuki/Core/Introspection.h` | T21 — free fns on `RootObject*`: `IidsOf`, `Provides`, `ProviderClass`, `RoleOf`, `TypeOf` |
| `Yuki/src/Core/Introspection.cpp` | T21 — implementations routing through `MetaDyn()` |
| `Yuki/include/Yuki/Core/Closure.h` | T22 — `Nucleus`, `MaterializedFacades`, `Extensions`, `InClosure`, `WalkClosure` |
| `Yuki/src/Core/Closure.cpp` | T22 — snapshot-at-call walk over `extendedBy`/`implementedBy` |
| `Yuki/include/Yuki/Core/EpochRcu.h` | T23 — `RcuReadGuard`, `RetireSnapshot`, `TryReclaim` |
| `Yuki/src/Core/EpochRcu.cpp` | T23 — global epoch counter, thread slot registry, reclaimer |
| `Yuki/src/Core/Arm.cpp` | T23 — `SideTableResolver` + `CodeExtensionSingleton` trampolines |

### 1.2 Files modified

| File | Change |
|------|--------|
| `Yuki/include/Yuki/Core/YObjectMacro.h` | Adds `virtual const MetaDynamic& MetaDyn() const noexcept` |
| `Yuki/include/Yuki/Core/MetaLinks.h` | Replaces `RetiredPool` with `EpochRetireQueue`; adds `subclassedBy` snapshot |
| `Yuki/include/Yuki/Core/DispatchEntry.h` | Adds `ArmKind` tag + union for resolver/singleton fnPtrs; adds `providerClass` field |
| `Yuki/src/Core/Query.cpp` | Dispatches by `ArmKind` after L2 lookup |
| `Yuki/src/Core/Install.cpp` | Emits correct `ArmKind`; runs D16 subclass flatten; uses epoch retire |
| `Yuki/src/Core/RootObject.cpp` | Nucleus dtor calls `EagerChain::DeleteParkedFor(this)` |
| `Yuki/src/Core/EagerChain.cpp` | Adds `DeleteParkedFor` walker |

### 1.3 What does NOT change

- `RootObject` 2-word layout (D1). `MetaDyn()` adds a vtable slot, not a sizeof slot.
- A2's `Install` writer-mutex discipline. A3 reuses it for subclass flatten lock ordering.
- A1's refcount CAS protocol on `metaWord_`. The external sentinel is untouched.

---

## 2. T21 Introspection Surface

All take `const RootObject*` and route through `node->MetaDyn()` to reach `links` + `core`. Each
function takes one `RcuReadGuard` on entry so the snapshot pointers stay live for the call.

```cpp
namespace Yuki {
    std::span<const Iid> IidsOf(const RootObject* node) noexcept;
    [[nodiscard]] bool   Provides(const RootObject* node, Iid iid) noexcept;
    const MetaCore*      ProviderClass(const RootObject* node, Iid iid) noexcept;
    ClassType            RoleOf(const RootObject* node, Iid iid) noexcept;
    const MetaCore*      TypeOf(const RootObject* node) noexcept;   // == node->MetaDyn().core
}
```

**Three rules:**

1. *Closure view, not static view.* `IidsOf` enumerates the live `mergedDispatch`; extensions
   installed at runtime appear; `Install<E>` calls observed after the read-guard takes its snapshot
   do not.
2. *Read-only.* Nothing in this surface mutates `MetaLinks`. RCU guard is acquire + release only.
3. *No vtable bloat.* Free functions; `RootObject` itself gains nothing beyond `MetaDyn()`.

**Important tiebreak:** when two providers register the same iid and one is marked `Important`
(D7.3), the Important one wins `mergedDispatch` placement at Install time; introspection therefore
returns the Important provider for `ProviderClass`/`RoleOf`. The displaced provider is not visible
to introspection (it never reached `mergedDispatch`).

---

## 3. T22 Closure-Walking Helpers

Snapshot-at-call semantics across the board. Callbacks are templated; no `std::function_ref`.

```cpp
namespace Yuki {
    RootObject* Nucleus(RootObject* node) noexcept;
    std::span<RootObject* const> MaterializedFacades(RootObject* nucleus) noexcept;
    std::span<RootObject* const> Extensions(RootObject* nucleus) noexcept;
    [[nodiscard]] bool InClosure(RootObject* a, RootObject* b) noexcept;

    template<class F>
    void WalkClosure(RootObject* nucleus, F&& fn)
        noexcept(std::is_nothrow_invocable_v<F&, RootObject*>);
}
```

**Two semantic clarifications:**

1. **InlineFacade vs MaterializedFacade.** Inline facades live in the impl's frame and have no
   separate `RootObject` — they cannot appear in any walk result. Only `SideTableResolver` arms
   produce materialized facades. `MaterializedFacades` is honest about this.
2. **Eager extension visibility.** Parked `Anno::Eager` extensions (refcount=0, owned only by the
   chain pointer per D11) ARE visited by `WalkClosure` and ARE in `Extensions(nucleus)`. Callers
   may observe a refcount-0 node; storage is safe because the chain owns it and the
   `RcuReadGuard` keeps the snapshot alive.

**Up-pointer storage for `Nucleus`:** facades and extensions store a per-instance
`RootObject* upstream_` slot, written at construction by `MaterializeIntoImpl` (facades) or
`MakeOwned<E>(extendee)` / eager-park hook (extensions). Impls store nothing — `Nucleus(impl)`
returns `impl`. Cost: one pointer per facade/extension instance; nothing on Impls.

---

## 4. T23 Epoch-RCU Mechanism

Replaces A2's unbounded `RetiredPool`. Three pieces: per-thread epoch counter, global epoch +
retire queue, reclaimer.

```cpp
namespace Yuki {
    class [[nodiscard]] RcuReadGuard {
      public:
        RcuReadGuard() noexcept;
        ~RcuReadGuard() noexcept;
        RcuReadGuard(const RcuReadGuard&) = delete;
        RcuReadGuard& operator=(const RcuReadGuard&) = delete;
      private:
        bool wasOuter_;
    };
    void RetireSnapshot(void* ptr, void (*deleter)(void*)) noexcept;
    size_t TryReclaim() noexcept;
}
```

### 4.1 Internal state

- `std::atomic<uint64_t> gGlobalEpoch{1};` — `0` is reserved for "quiescent".
- `std::array<ThreadSlot, 64> gSlots;` — fixed-size open-addressed slot table. Threads claim a
  slot on first guard via CAS on `occupied`. 64 slots covers test workloads; size raises with
  Yuki workload growth.
- `thread_local ThreadSlot* tlSlot;` — cached pointer to claimed slot.
- `std::mutex gRetireMu;` and `std::vector<Retired> gRetired;` — retire queue.

### 4.2 Reader path (`RcuReadGuard` ctor)

1. If `tlSlot == nullptr`, scan `gSlots` linearly for the first slot whose `occupied` CASes
   false→true; cache it in `tlSlot`. If all 64 slots are occupied, `kDebug` asserts and the
   release build conservatively treats the reader as "always active" (epoch never advances) —
   safe but starves the reclaimer until a thread exits and frees its slot.
2. If `tlSlot->epoch.load(relaxed) != 0` → nested guard, `wasOuter_=false`, return.
3. Else: `e = gGlobalEpoch.load(acquire)`; `tlSlot->epoch.store(e, release)`; `wasOuter_=true`.

Reader exit (`~RcuReadGuard`): if `wasOuter_`, `tlSlot->epoch.store(0, release)`.

### 4.3 Writer path (`Install<E>`, after publishing new mergedDispatch)

1. `uint64_t stamp = gGlobalEpoch.fetch_add(1, acq_rel);`
2. Lock `gRetireMu`, push `{oldSnapshot, deleter, stamp}`, unlock.
3. Opportunistically call `TryReclaim()`.

### 4.4 Reclaimer (`TryReclaim`)

1. Compute `safe = min over occupied slots of (epoch ? epoch : UINT64_MAX)` — min of all
   non-quiescent reader epochs.
2. Lock `gRetireMu`; for each retired item with `epoch < safe`, call deleter; erase. Unlock.
3. Return freed count.

### 4.5 Correctness sketch

A reader publishing `epoch = E` may hold pointers retired at epochs `≤ E−1`, never any retired at
`≥ E`. A writer at retire stamp `S` wrote `S = gGlobalEpoch.fetch_add(1)`. Any reader started
before that fetch sees `epoch ≤ S−1` and is dangerous; any reader started after sees `epoch ≥ S`
and has already observed the new pointer via `mergedDispatch.load(acquire)` (release-store
happens-before retire). `safe = min(reader.epoch)` ensures dangerous readers block reclaim of
their epoch's retirees.

### 4.6 Degenerate case mitigation

If a thread never calls `TryReclaim`, retirees accumulate. Mitigation: every `Install<E>` writer
calls `TryReclaim` (already holds writer mutex; cheap when queue is empty). Shutdown calls a
final `TryReclaim` after forcing all slots to quiescent.

---

## 5. T23 D16 Base-Chain `mergedDispatch` Flatten

When `Install<E>` registers `E` against base `B`, every Impl whose inheritance chain includes `B`
must observe `E`'s entries in *its* `mergedDispatch`.

### 5.1 New reverse edge: `MetaLinks::subclassedBy`

```cpp
std::atomic<const SubclassSnapshot*> subclassedBy;

struct SubclassSnapshot {
    size_t          count;
    const MetaCore* data[];   // flexible array, sorted by Iid for stable diff
};
```

Holds `const MetaCore*` (class-level identity), not `RootObject*`.

### 5.2 Population — at class-static-init, NOT Install time

`Y_OBJECT` for subclass `D` already runs `Detail::MakeMetaCoreFor<D>()` at constexpr. A2 paired
that with a per-class static initializer. T23 extends it to walk `D`'s `kMetaCore.extendsInfo`
and CAS-append `&D::kMetaCore` onto each base's `MetaLinks::subclassedBy`. Uses the per-metaclass
writer mutex (A2 invariant). Cross-TU order is fine: insert is monotonic and idempotent.

### 5.3 `Install<E>` flatten algorithm

```
Install<E> on nucleus B:
  Acquire writer mutex on B
  snap_old = B.mergedDispatch.load(acquire)
  snap_new = MergeDispatch(snap_old, EntriesFromE())
  B.mergedDispatch.store(snap_new, release)
  RetireSnapshot(snap_old, &DeleteDispatchSnapshot)

  // D16 addition:
  sub_snap = B.subclassedBy.load(acquire)
  for each D in sub_snap (in iid order):
      Acquire writer mutex on D
      d_old = D.mergedDispatch.load(acquire)
      d_new = MergeDispatch(d_old, EntriesFromE())
      D.mergedDispatch.store(d_new, release)
      RetireSnapshot(d_old, &DeleteDispatchSnapshot)
      Release writer mutex on D

  BroadcastInvalidation over B.extendedBy
  BroadcastInvalidation over sub_snap
  Release writer mutex on B
```

### 5.4 Lock ordering — deadlock prevention

Writer mutexes are acquired in `subclassedBy` order (iid-sorted, deterministic). `Install<E>` on
`B` holds `B`'s mutex first, then walks subclasses in iid order, releasing each before moving to
the next. Two concurrent `Install<E>` on different bases sharing a subclass acquire that subclass
in the same iid order, so they serialize without cycles.

### 5.5 Memory cost

One `SubclassSnapshot*` slot per `MetaLinks` (~8 B). Plus N pointers per snapshot for a base with
N concrete subclasses. Realistic graphs have small N. Snapshots are RCU-retired via Section 4.

---

## 6. T23 Arm Wiring — SideTableResolver + CodeExtensionSingleton

A2 wired only `InlineFacade`. A3 wires the other two D14 arms.

### 6.1 Updated `DispatchEntry`

```cpp
enum class ArmKind : uint8_t {
    InlineFacade           = 0,
    SideTableResolver      = 1,
    CodeExtensionSingleton = 2,
};

struct DispatchEntry {
    Iid              iid;
    ArmKind          kind;
    uint8_t          important : 1;
    const MetaCore*  providerClass;   // for T21 introspection
    union {
        uint32_t     inlineFacadeOff;            // InlineFacade
        RootObject* (*resolver)(RootObject*);    // SideTableResolver
        RootObject* (*singleton)();              // CodeExtensionSingleton
    } u;
};
```

Iid-sorted in `mergedDispatch`; binary search by iid; Important bit tiebreak (D7.3).

### 6.2 Query dispatch (post-L2 lookup)

```cpp
template<class I, class T>
ComPtr<I> Query(T* node) noexcept {
    if constexpr (Detail::IsBoaProvider<T, I>())
        return ComPtr<I>::Adopt(static_cast<I*>(node));

    RcuReadGuard g;
    const DispatchEntry* e = QueryDynamicRaw(node->MetaDyn().links, IidOf<I>());
    if (!e) return {};

    switch (e->kind) {
      case ArmKind::InlineFacade:
          if constexpr (std::is_base_of_v<I, T>) {
              auto* facade = reinterpret_cast<I*>(
                  reinterpret_cast<std::byte*>(node) + e->u.inlineFacadeOff);
              Acquire(facade);
              return ComPtr<I>::Adopt(facade);
          }
          return {};
      case ArmKind::SideTableResolver: {
          RootObject* mat = e->u.resolver(node);
          if (!mat) return {};
          return ComPtr<I>::Adopt(static_cast<I*>(mat));
      }
      case ArmKind::CodeExtensionSingleton: {
          RootObject* s = e->u.singleton();
          Acquire(s);   // no-op when payload carries external sentinel
          return ComPtr<I>::Adopt(static_cast<I*>(s));
      }
    }
    return {};
}
```

### 6.3 Registrant API

```cpp
namespace Yuki {
    template<class Impl, class I, RootObject* (*Resolver)(RootObject*)>
    void RegisterSideTable() noexcept;

    template<class Impl, class I, RootObject* (*Singleton)()>
    void RegisterCodeExt() noexcept;
}
```

Both go through A1's `Registry::Install` path. D7.2 cross-module seal checks apply uniformly
across all three arm kinds.

### 6.4 Lifetime contracts

| Arm | Returns | Caller responsibility |
|-----|---------|-----------------------|
| `InlineFacade` | Borrowed ptr into Impl frame | `ComPtr` ctor `Acquire`s; facade dtor `Release(impl)` (A2 wiring) |
| `SideTableResolver` | +1 reference to heap facade, OR `nullptr` if resolver chose not to materialize | If `nullptr`, `Query` returns empty `ComPtr<I>` (same shape as Provides==false); resolvers MUST NOT return a 0-refcount pointer |
| `CodeExtensionSingleton` | Borrowed ptr to external-lifetime singleton | Acquire/Release are payload-sentinel no-ops; dtor never fires |

Function-pointer arms keep `DispatchEntry` POD and inline-storable in rodata snapshots; no
per-entry heap; no virtual call on the Query hot path.

---

## 7. T23 Nucleus Dtor Walker

D11: final teardown deletes parked Eager extensions when the nucleus dies.

### 7.1 Hook in `~RootObject`

```cpp
RootObject::~RootObject() noexcept {
    if (TypeDynamic() == ClassType::Implementation)
        Detail::EagerChain::DeleteParkedFor(this);
}
```

### 7.2 Walker

```cpp
void EagerChain::DeleteParkedFor(RootObject* nucleus) noexcept {
    MetaLinks* links = nucleus->MetaDyn().links;
    std::lock_guard<std::mutex> g(links->writerMu);

    const EagerSetSnapshot* snap = links->eagerSet.load(std::memory_order_acquire);
    if (!snap) return;

    for (size_t i = 0; i < snap->count; ++i) {
        RootObject* parked = snap->parked[i];
        if constexpr (kDebug) {
            auto payload = parked->PayloadRelaxed();
            assert(payload.refcount() == 0 &&
                   "eager extension parked with non-zero refcount at nucleus dtor");
        }
        delete parked;
    }

    links->eagerSet.store(nullptr, std::memory_order_release);
    RetireSnapshot(const_cast<EagerSetSnapshot*>(snap), &Detail::DeleteEagerSnapshot);
}
```

### 7.3 Correctness notes

- **No in-flight Query against this nucleus's mergedDispatch.** Refcount just hit 0 so any
  observing `ComPtr` is gone. `RcuReadGuard` protects the snapshot, not the nucleus itself; D8
  refcount governs nucleus lifetime.
- **Why still take `writerMu`.** A concurrent `Install<E>(OtherNucleus)` may flatten through our
  metaclass via D16; mutex serializes cleanly.
- **Live (refcount > 0) eager extensions never reach this path.** D8 says
  `refcount(ext) >= refcount(extendee)` whenever extendee > 0. A live eager ext holds the
  deferred `Acquire(extendee)` (D11), so nucleus can't reach 0 while any live ext exists. Only
  parked ones (refcount=0) survive to here.

---

## 8. Testing Strategy

Eleven Catch2 test files via `add_yuki_test(Core <Name>)`, all under `build/x64-asan`.

### 8.1 Per-task unit tests

| File | Coverage |
|------|----------|
| `Core/IntrospectionTest.cpp` | `IidsOf` order; `Provides` true/false; `ProviderClass`/`RoleOf` agreement; `TypeOf == MetaDyn().core`; `Important` tiebreak |
| `Core/ClosureTest.cpp` | `Nucleus(impl/facade/ext)` one-hop; `MaterializedFacades` excludes inline; `Extensions` includes parked Eager; `InClosure` symmetric/reflexive; `WalkClosure` order; snapshot semantics |
| `Core/EpochRcuTest.cpp` | Guard claim/release; nested no-op; retire/reclaim happy path; reader blocks reclaim; reader exit unblocks; 4-reader + 1-retirer stress for 1s, ASan-clean, queue drains |
| `Core/D16FlattenTest.cpp` | Register `E` on base `B`; subclass `D.mergedDispatch` now contains E; cacheEpoch bumped; old snapshot retired; deadlock smoke: two threads installing on two bases sharing one subclass |
| `Core/ArmKindsTest.cpp` | `RegisterSideTable` — materializes heap facade refcount=1, ComPtr release deletes, second Query allocates fresh; `RegisterCodeExt` — same pointer across calls, Acquire/Release no-ops, outlives every nucleus |
| `Core/NucleusDtorWalkerTest.cpp` | Install eager ext, never user-acquire, drop nucleus ComPtr → ASan confirms ext freed; live eager ext keeps nucleus alive (D8); kDebug assert fires on parked-but-refcount>0 |

### 8.2 Integration test

| File | Coverage |
|------|----------|
| `Core/A3IntegrationTest.cpp` | Three-class hierarchy `Base`, `MidImpl : Base`, `Leaf : MidImpl` with one inline iface, one side-table iface, one code-ext on Base; `Query<I>(leaf)` per arm; `WalkClosure(leaf)` enumerates; `IidsOf(leaf)` matches union; 4 Query threads + 1 Install thread, ASan-clean |

### 8.3 Regression guard

A1 tests: `RootObjectTest`, `RefcountTest`, `RegistryTest`.
A2 tests: `DispatchEntryTest`, `MetaLinksTest`, `InstallTest`, `QueryTest`, `EagerChainTest`.
All continue to pass under the new `MetaDyn()` vtable slot.

### 8.4 Explicit non-coverage

- No multi-process / IPC tests (Plan B / P2-D1 territory).
- No `malloc` fault-injection (Yuki treats OOM as process-fatal).
- No long-running burn-in beyond the 1s `EpochRcuTest` stress.

---

## 9. Out of Scope

- Plan B / P2-D1…D4 (cross-DLL manifest, lazy realize, closure serialization).
- Hot DLL unloading (parent spec section 7).
- ARM64 weaker-memory-model retest (Yuki targets x86-64 TSO for foundation slices).
- Migration of A2's known nits NOT covered here: A2's L0 shortcut path through `IsBoaProvider`
  rather than a dedicated `L0Shortcut` symbol (cosmetic; left for a future polish).

## 10. Open Items

None blocking implementation. Two recorded for future slices:

- `gSlots` size of 64 — raise when a Yuki workload exceeds it; no ABI break (internal).
- `Anno::Pickled` field walker for P2-D4 closure serialization — needs `IidsOf` + `WalkClosure`
  from A3 as prerequisites; lands in Plan B.
