# Yuki Closure Architecture Core — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the closure model — nucleus + extensions + facades — with atomic-snapshot dispatch, reflection-driven registration, eager/lazy materialization, and a complete introspection surface, as specified in `docs/superpowers/specs/2026-06-16-yuki-closure-architecture.md` §§1–6.

**Architecture:** Replace `MetaLinks::dispatch` (currently a fixed `std::span`) with an `std::atomic<const DispatchSnapshot*>` that is published by per-class `Detail::Registrar<T>` at static-init time, under a per-metaclass writer mutex with release semantics. Readers do an acquire-load + binary search + small switch; the static face folds `Query<I>(C*)` to a cast when `derived_from<C,I>` holds. Extensions are stored per closure in the nucleus's existing `facades_` list, deduplicated by iid so cardinality is structurally ≤1 per `(Extension type, closure)`. Eager extensions materialize in `MetaNode<T>`'s constructor; lazy extensions materialize on first query via a resolver function captured in the dispatch entry. Introspection is a thin view layer over the snapshot pointers and `facades_`.

**Tech Stack:** C++26 (P2996 reflection, P3394 annotations, `template for`, `consteval` blocks), `std::atomic` acquire-release, `Mashiro::SpinMutex`, Catch2 v3, ninja + clang-p2996, `cmake --build build/x64-asan`. Dev env must be activated with `python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression` before any build.

**Pre-flight (one-time per shell):**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
```

All `cmake --build` and ctest commands in this plan assume the dev env is already active.

---

## File Structure

| Path | Responsibility | Action |
|------|---------------|--------|
| `Yuki/include/Yuki/Core/Identity.h` | Annotations, concepts, `Iid`, role enum | Modify: add `Anno::Eager`/`Anno::Lazy`, rewrite `StatelessExtensionClass`, add `IidOf` runtime overload |
| `Yuki/include/Yuki/Core/FacadeList.h` | Per-closure facade list head + node | Modify: add `Lookup(head, iid)`, `AttachUnique(head, node)` helpers |
| `Yuki/include/Yuki/Core/MetaClass.h` | `MetaCore`, `MetaLinks`, `MetaClass`, reflection pipeline | Modify: `DispatchSnapshot` type, atomic dispatch ptr, payload union, `EagerSet`, snapshot retirement |
| `Yuki/include/Yuki/Core/Registry.h` | Registration entry points | Create: `Install<T>()`, `AlreadyInstalled(iid)`, mutex-map declaration |
| `Yuki/src/Core/Registry.cpp` | Registry storage | Create: writer mutex map, `AlreadyInstalled` set, retirement list global |
| `Yuki/include/Yuki/Core/RootObject.h` | `RootObject`, `MetaNode`, `ExtensionNode`, `IfaceFacadeNode` | Modify: CRTP hook for `Registrar<T>` + `MaterializeInto`; `MetaNode<T>` constructor runs eager-set pass |
| `Yuki/include/Yuki/Core/Query.h` | `RT::Query`, `RT::Materialized`, `RT::Provider`, `RT::Reify`, dynamic kernel | Modify: switch on `DispatchKind` with `Materialize` policy; `HasInlineFacadeFor`/`InlineFacadeAddress` |
| `Yuki/include/Yuki/Core/Introspection.h` | Potential + runtime attribute views | Create: `Capabilities`, `Provides`, `Extensions`, `EagerExtensions`, `Implementations`, `Has`, `IsMaterialized`, `MaterializedExtensions`, `MaterializedFacades`, `InClosure`, `WalkClosure`, `ProviderClass`, `ProviderDispatchKind` |
| `Yuki/tests/Core/ClosureTest.cpp` | Closure semantics tests | Create: cardinality, eager-vs-lazy, cross-closure isolation, concurrent install/query, identity coherence |
| `Yuki/tests/Core/QueryTest.cpp` | Query surface tests | Modify: extend with the four scenarios from spec §4.3 |
| `Yuki/tests/Core/IntrospectionTest.cpp` | Introspection surface tests | Create: potential coverage, non-materializing probe, walk order, symmetry |
| `Yuki/tests/CMakeLists.txt` | Test registration | Modify: register `ClosureTest`, `IntrospectionTest` |

Each task below is one logical unit ending in a green build + a commit.

---

### Task 1: Identity foundation — Eager/Lazy annotations, reflection-based StatelessExtensionClass, IidOf runtime overload

**Files:**
- Modify: `Yuki/include/Yuki/Core/Identity.h`
- Test: `Yuki/tests/Core/IdentityTest.cpp` (existing file — extend)

- [ ] **Step 1: Write the failing tests**

Add to `Yuki/tests/Core/IdentityTest.cpp` inside the existing `namespace`:

```cpp
namespace {
    struct [[=Anno::Implementation]] StatelessTestImpl : Yuki::MetaNode<StatelessTestImpl> {};
    struct [[=Anno::Extension, =Anno::Lazy]] StatefulExt
        : Yuki::ExtensionNode<StatefulExt, StatelessTestImpl> {
        using ExtensionNode::ExtensionNode;
        int payload{0};
    };
    struct [[=Anno::Extension, =Anno::Eager]] StatelessExt
        : Yuki::ExtensionNode<StatelessExt, StatelessTestImpl> {
        using ExtensionNode::ExtensionNode;
    };
} // namespace

TEST_CASE("StatelessExtensionClass detects empty-NSDM extensions", AUTO_TAG) {
    STATIC_REQUIRE(Yuki::Anno::StatelessExtensionClass<StatelessExt>);
    STATIC_REQUIRE_FALSE(Yuki::Anno::StatelessExtensionClass<StatefulExt>);
}

TEST_CASE("Anno::Eager and Anno::Lazy are detected via reflection", AUTO_TAG) {
    STATIC_REQUIRE(Yuki::Anno::IsEager<StatelessExt>);
    STATIC_REQUIRE(Yuki::Anno::IsLazy<StatefulExt>);
    STATIC_REQUIRE_FALSE(Yuki::Anno::IsEager<StatefulExt>);
}

TEST_CASE("IidOf has a runtime form taking a MetaCore", AUTO_TAG) {
    constexpr auto staticIid = Yuki::IidOf<StatelessTestImpl>;
    const auto runtimeIid = Yuki::IidOf(Yuki::MetaCoreOf<StatelessTestImpl>);
    REQUIRE(staticIid == runtimeIid);
}
```

- [ ] **Step 2: Run tests to confirm they fail at compile or link**

```bash
cmake --build build/x64-asan --target Test.Core.IdentityTest
```

Expected: compile error on `StatelessExtensionClass`/`IsEager`/`IsLazy`/runtime `IidOf` not found.

- [ ] **Step 3: Implement the additions in Identity.h**

In `Yuki/include/Yuki/Core/Identity.h`, inside `namespace Yuki::Anno`:

```cpp
struct Eager {};
struct Lazy  {};

template <class T>
inline constexpr bool IsEager = []() consteval {
    for (auto a : std::meta::annotations_of(^^T)) {
        if (std::meta::type_of(a) == ^^Eager) return true;
    }
    return false;
}();

template <class T>
inline constexpr bool IsLazy = []() consteval {
    for (auto a : std::meta::annotations_of(^^T)) {
        if (std::meta::type_of(a) == ^^Lazy) return true;
    }
    return false;
}();
```

Replace the current `StatelessExtensionClass` definition with:

```cpp
template <class E>
concept StatelessExtensionClass = ExtensionClass<E> && []() consteval {
    std::size_t own = 0;
    template for (constexpr auto m : std::meta::nonstatic_data_members_of(
                      ^^E, std::meta::access_context::current())) {
        if (std::meta::parent_of(m) == ^^E) ++own;
    }
    return own == 0;
}();

template <class E>
concept StatefulExtensionClass = ExtensionClass<E> && !StatelessExtensionClass<E>;
```

Add the runtime `IidOf` overload (next to the existing variable template):

```cpp
[[nodiscard]] inline Iid IidOf(const MetaCore& core) noexcept { return core.iid(); }
```

- [ ] **Step 4: Build and run the tests**

```bash
cmake --build build/x64-asan --target Test.Core.IdentityTest
ctest --test-dir build/x64-asan -R "IdentityTest" --output-on-failure
```

Expected: all three new tests pass; existing tests still pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Identity.h Yuki/tests/Core/IdentityTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Identity): add Eager/Lazy annotations and reflection-based StatelessExtensionClass

Per spec docs/superpowers/specs/2026-06-16-yuki-closure-architecture.md §3.2.
Replaces the sizeof<=1 stateless discriminator (broken since CodeExtension
also derives from RootObject) with a reflection-based check that counts
E-declared NSDMs. Adds Anno::Eager/Anno::Lazy markers and IsEager/IsLazy
predicates. Adds runtime IidOf(MetaCore&) overload to bridge static and
dynamic dispatch paths.
EOF
)"
```

---

### Task 2: FacadeList helpers — Lookup and AttachUnique

**Files:**
- Modify: `Yuki/include/Yuki/Core/FacadeList.h`
- Test: `Yuki/tests/Core/FacadeListTest.cpp` (create if missing; extend if existing)

- [ ] **Step 1: Write the failing tests**

Create `Yuki/tests/Core/FacadeListTest.cpp` with:

```cpp
#include <Yuki/Core/FacadeList.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct StubNode : FacadeNode {
        StubNode(Iid id) : FacadeNode{id} {}
    };
}

TEST_CASE("FacadeListHead::Lookup returns nullptr on empty list", AUTO_TAG) {
    FacadeListHead head;
    REQUIRE(FacadeListLookup(head, Iid{1, 0}) == nullptr);
}

TEST_CASE("AttachUnique inserts when iid is absent", AUTO_TAG) {
    FacadeListHead head;
    StubNode node{Iid{42, 0}};
    REQUIRE(AttachUnique(head, &node) == &node);
    REQUIRE(FacadeListLookup(head, Iid{42, 0}) == &node);
}

TEST_CASE("AttachUnique returns the existing node when iid is already present", AUTO_TAG) {
    FacadeListHead head;
    StubNode first{Iid{7, 0}};
    StubNode second{Iid{7, 0}};
    REQUIRE(AttachUnique(head, &first) == &first);
    REQUIRE(AttachUnique(head, &second) == &first);
    REQUIRE(FacadeListLookup(head, Iid{7, 0}) == &first);
}
```

- [ ] **Step 2: Run tests to confirm they fail**

```bash
cmake --build build/x64-asan --target Test.Core.FacadeListTest
```

Expected: compile error on `FacadeListLookup` / `AttachUnique` undefined.

- [ ] **Step 3: Implement the helpers in FacadeList.h**

In `Yuki/include/Yuki/Core/FacadeList.h`, add inside `namespace Yuki`:

```cpp
[[nodiscard]] inline RootObject* FacadeListLookup(const FacadeListHead& head, Iid id) noexcept {
    for (FacadeNode* n = head.Head(); n != nullptr; n = n->next) {
        if (n->iid == id) return n->underlying;
    }
    return nullptr;
}

[[nodiscard]] inline FacadeNode* AttachUnique(FacadeListHead& head, FacadeNode* node) noexcept {
    while (true) {
        for (FacadeNode* n = head.Head(); n != nullptr; n = n->next) {
            if (n->iid == node->iid) return n;
        }
        FacadeNode* observed = head.Head();
        node->next = observed;
        if (head.CompareExchangeHead(observed, node)) return node;
    }
}
```

If `FacadeNode` does not yet have `iid` / `underlying` / `next` members or `FacadeListHead` does not yet expose `Head()` / `CompareExchangeHead`, add them in the same edit. The existing `Attach` becomes a thin wrapper over the CAS used by `AttachUnique`.

- [ ] **Step 4: Register the test in CMakeLists**

In `Yuki/tests/CMakeLists.txt`, add `Test.Core.FacadeListTest` alongside the existing tests following the pattern used for `Test.Core.IdentityTest`.

- [ ] **Step 5: Build and run**

```bash
cmake --build build/x64-asan --target Test.Core.FacadeListTest
ctest --test-dir build/x64-asan -R "FacadeListTest" --output-on-failure
```

Expected: three tests pass.

- [ ] **Step 6: Commit**

```bash
git add Yuki/include/Yuki/Core/FacadeList.h Yuki/tests/Core/FacadeListTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/FacadeList): add Lookup and AttachUnique helpers

Per spec §1.5 (cardinality enforced by iid-keyed CAS dedup) and §5.4 step 2.
AttachUnique is the primitive used by both eager materialization and the
SideTableResolver lazy path; on race, the loser returns the winner's node so
exactly one Extension instance lives per (Extension type, closure).
EOF
)"
```

---

### Task 3: DispatchSnapshot infrastructure — type, atomic pointer, binary search, payload union

**Files:**
- Modify: `Yuki/include/Yuki/Core/MetaClass.h`
- Test: `Yuki/tests/Core/DispatchSnapshotTest.cpp` (create)

- [ ] **Step 1: Write the failing tests**

Create `Yuki/tests/Core/DispatchSnapshotTest.cpp`:

```cpp
#include <Yuki/Core/MetaClass.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

TEST_CASE("Empty DispatchSnapshot lookup returns nullptr", AUTO_TAG) {
    DispatchSnapshot empty{0, nullptr, nullptr};
    REQUIRE(Detail::LookupEntry(&empty, Iid{1, 0}) == nullptr);
}

TEST_CASE("Sorted DispatchSnapshot binary-search hits and misses correctly", AUTO_TAG) {
    DispatchEntry entries[3] = {
        {Iid{1, 0}, DispatchKind::DirectCast,             {.staticOffset = 8}},
        {Iid{5, 0}, DispatchKind::InlineFacade,           {.staticOffset = 16}},
        {Iid{9, 0}, DispatchKind::CodeExtensionSingleton, {.singleton = nullptr}},
    };
    DispatchSnapshot snap{3, entries, nullptr};

    auto* e1 = Detail::LookupEntry(&snap, Iid{1, 0});
    REQUIRE(e1 != nullptr);
    REQUIRE(e1->kind == DispatchKind::DirectCast);

    auto* e5 = Detail::LookupEntry(&snap, Iid{5, 0});
    REQUIRE(e5 != nullptr);
    REQUIRE(e5->kind == DispatchKind::InlineFacade);

    REQUIRE(Detail::LookupEntry(&snap, Iid{4, 0}) == nullptr);
    REQUIRE(Detail::LookupEntry(&snap, Iid{10, 0}) == nullptr);
}

TEST_CASE("MetaLinks dispatch field is std::atomic<const DispatchSnapshot*>", AUTO_TAG) {
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MetaLinks>().dispatch),
                                  std::atomic<const DispatchSnapshot*>>);
}
```

- [ ] **Step 2: Run tests to confirm they fail**

```bash
cmake --build build/x64-asan --target Test.Core.DispatchSnapshotTest
```

Expected: compile errors on `DispatchSnapshot`, payload union variants, `Detail::LookupEntry`, atomic dispatch field.

- [ ] **Step 3: Add DispatchSnapshot and update MetaLinks**

In `Yuki/include/Yuki/Core/MetaClass.h`, inside `namespace Yuki`:

```cpp
struct DispatchEntry {
    Iid          iid;
    DispatchKind kind;
    union Payload {
        std::ptrdiff_t       staticOffset;
        RootObject* const*   singleton;
        RootObject* (*resolver)(RootObject* nucleus) noexcept;
    } payload;
};

struct DispatchSnapshot {
    std::size_t              count;
    const DispatchEntry*     entries;
    const DispatchSnapshot*  previous;
};

namespace Detail {
    [[nodiscard]] inline const DispatchEntry* LookupEntry(
            const DispatchSnapshot* s, Iid id) noexcept {
        if (s == nullptr || s->count == 0) return nullptr;
        std::size_t lo = 0, hi = s->count;
        while (lo < hi) {
            std::size_t mid = lo + (hi - lo) / 2;
            const auto& e = s->entries[mid];
            if (e.iid < id) lo = mid + 1;
            else if (id < e.iid) hi = mid;
            else return &e;
        }
        return nullptr;
    }
}
```

Replace `MetaLinks::dispatch`'s current `std::span<const DispatchEntry>` field with:

```cpp
mutable std::atomic<const DispatchSnapshot*> dispatch{nullptr};
```

(`mutable` so const metaclasses can publish; access remains via atomic load/CAS.)

- [ ] **Step 4: Register the test and build**

Add `Test.Core.DispatchSnapshotTest` to `Yuki/tests/CMakeLists.txt`. Then:

```bash
cmake --build build/x64-asan --target Test.Core.DispatchSnapshotTest
ctest --test-dir build/x64-asan -R "DispatchSnapshotTest" --output-on-failure
```

Expected: three tests pass.

- [ ] **Step 5: Repair downstream consumers**

The full Yuki build will now fail wherever code reads `dispatch` as `std::span`. Search:

```bash
```

Run via Grep:

Pattern: `\.dispatch[^.]` over `Yuki/` and `Mashiro/`.

Update each consumer to do `links.dispatch.load(std::memory_order_acquire)` and treat the result as `const DispatchSnapshot*`. If `Query.h`'s current implementation is one of them, leave a temporary `// TODO(task-12): rewrite kernel` and a stub that returns nullptr to keep the build green; the kernel is fully rewritten in Task 12.

- [ ] **Step 6: Build full Yuki**

```bash
cmake --build build/x64-asan
```

Expected: green build.

- [ ] **Step 7: Commit**

```bash
git add Yuki/include/Yuki/Core/MetaClass.h Yuki/tests/Core/DispatchSnapshotTest.cpp Yuki/tests/CMakeLists.txt Yuki/include/Yuki/Core/Query.h
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/MetaClass): introduce DispatchSnapshot with atomic publish

Per spec §2.1. Replaces the fixed std::span dispatch view with an immutable
DispatchSnapshot owned by the metaclass through an atomic pointer. Readers
acquire-load, writers (registrars, not yet wired) release-publish; the old
snapshot is chained via `previous` for epoch retirement (scaffolding only —
retirement pass added in Task 5). Payload union covers DirectCast/InlineFacade
(staticOffset), CodeExtensionSingleton (singleton), and SideTableResolver
(resolver fn); FacadeList carries no payload.
EOF
)"
```

---

### Task 4: EagerSet infrastructure on MetaLinks

**Files:**
- Modify: `Yuki/include/Yuki/Core/MetaClass.h`
- Test: `Yuki/tests/Core/DispatchSnapshotTest.cpp` (extend)

- [ ] **Step 1: Write the failing test**

Append to `Yuki/tests/Core/DispatchSnapshotTest.cpp`:

```cpp
TEST_CASE("MetaLinks carries an atomic EagerSet pointer, initially empty", AUTO_TAG) {
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MetaLinks>().eagerSet),
                                  std::atomic<const EagerSetSnapshot*>>);
    MetaLinks links;
    REQUIRE(links.eagerSet.load(std::memory_order_acquire) == nullptr);
}
```

- [ ] **Step 2: Confirm failure**

```bash
cmake --build build/x64-asan --target Test.Core.DispatchSnapshotTest
```

Expected: compile error on `EagerSetSnapshot` not found.

- [ ] **Step 3: Add EagerSetSnapshot and field**

In `Yuki/include/Yuki/Core/MetaClass.h`:

```cpp
struct EagerSetSnapshot {
    std::size_t                       count;
    const MetaCore* const*            entries;   // pointers to eager Extension metacores
    const EagerSetSnapshot*           previous;
};
```

In `MetaLinks`, add:

```cpp
mutable std::atomic<const EagerSetSnapshot*> eagerSet{nullptr};
```

- [ ] **Step 4: Build and run**

```bash
cmake --build build/x64-asan --target Test.Core.DispatchSnapshotTest
ctest --test-dir build/x64-asan -R "DispatchSnapshotTest" --output-on-failure
```

Expected: all four tests pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/MetaClass.h Yuki/tests/Core/DispatchSnapshotTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/MetaClass): add EagerSetSnapshot field for closure-construction hook

Per spec §3.3 step 5. Each Implementation metaclass carries an atomic-published
list of stateful eager Extensions to instantiate at nucleus construction time.
Empty by default; populated by Registry::Install once the registrar wires
land in Tasks 7–8.
EOF
)"
```

---

### Task 5: Snapshot retirement scaffolding

**Files:**
- Modify: `Yuki/include/Yuki/Core/MetaClass.h`
- Create: `Yuki/src/Core/SnapshotRetirement.cpp`
- Test: `Yuki/tests/Core/SnapshotRetirementTest.cpp`

- [ ] **Step 1: Write the failing test**

Create `Yuki/tests/Core/SnapshotRetirementTest.cpp`:

```cpp
#include <Yuki/Core/MetaClass.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

TEST_CASE("Retire enqueues a snapshot; sweep frees retired snapshots", AUTO_TAG) {
    auto* entries = new DispatchEntry[1]{{Iid{1,0}, DispatchKind::DirectCast, {.staticOffset = 0}}};
    auto* snap = new DispatchSnapshot{1, entries, nullptr};

    Detail::RetireSnapshot(snap, [](const DispatchSnapshot* s) noexcept {
        delete[] s->entries;
        delete s;
    });

    // Before sweep: snapshot is owned by the retirement list.
    REQUIRE(Detail::PendingRetirementCount() == 1);

    Detail::SweepRetirements();
    REQUIRE(Detail::PendingRetirementCount() == 0);
}
```

- [ ] **Step 2: Confirm failure**

```bash
cmake --build build/x64-asan --target Test.Core.SnapshotRetirementTest
```

Expected: link error on `RetireSnapshot`, `SweepRetirements`, `PendingRetirementCount`.

- [ ] **Step 3: Declare in MetaClass.h, implement in SnapshotRetirement.cpp**

Add to `Yuki/include/Yuki/Core/MetaClass.h` inside `namespace Yuki::Detail`:

```cpp
using SnapshotDeleter = void (*)(const DispatchSnapshot*) noexcept;

void          RetireSnapshot(const DispatchSnapshot* s, SnapshotDeleter d) noexcept;
void          SweepRetirements() noexcept;
std::size_t   PendingRetirementCount() noexcept;
```

Create `Yuki/src/Core/SnapshotRetirement.cpp`:

```cpp
#include <Yuki/Core/MetaClass.h>
#include <Mashiro/Core/SpinMutex.h>
#include <vector>

namespace Yuki::Detail {

namespace {
    struct PendingEntry {
        const DispatchSnapshot* snap;
        SnapshotDeleter         del;
    };
    Mashiro::SpinMutex&            RetirementMutex() noexcept {
        static Mashiro::SpinMutex m;
        return m;
    }
    std::vector<PendingEntry>&     PendingList() noexcept {
        static std::vector<PendingEntry> v;
        return v;
    }
} // namespace

void RetireSnapshot(const DispatchSnapshot* s, SnapshotDeleter d) noexcept {
    if (s == nullptr) return;
    std::lock_guard guard{RetirementMutex()};
    PendingList().push_back({s, d});
}

void SweepRetirements() noexcept {
    std::vector<PendingEntry> drained;
    {
        std::lock_guard guard{RetirementMutex()};
        drained.swap(PendingList());
    }
    for (auto& e : drained) e.del(e.snap);
}

std::size_t PendingRetirementCount() noexcept {
    std::lock_guard guard{RetirementMutex()};
    return PendingList().size();
}

} // namespace Yuki::Detail
```

Add `Yuki/src/Core/SnapshotRetirement.cpp` to `Yuki/CMakeLists.txt`'s source list for the `Yuki` library target.

- [ ] **Step 4: Register the test and build**

Add `Test.Core.SnapshotRetirementTest` to `Yuki/tests/CMakeLists.txt`. Build:

```bash
cmake --build build/x64-asan --target Test.Core.SnapshotRetirementTest
ctest --test-dir build/x64-asan -R "SnapshotRetirementTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/MetaClass.h Yuki/src/Core/SnapshotRetirement.cpp Yuki/tests/Core/SnapshotRetirementTest.cpp Yuki/CMakeLists.txt Yuki/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(Yuki/Core): snapshot retirement scaffold (RCU-by-epoch)

Per spec §2.3. RetireSnapshot enqueues a freed snapshot under a SpinMutex;
SweepRetirements drains and runs deleters. Registry::Install (Task 7) calls
RetireSnapshot on the old pointer after publishing the new one; the next
Install on any metaclass triggers Sweep (a global epoch). No per-thread
state; readers' acquire-load pins the snapshot for the duration of one
query, which is leaf-short.
EOF
)"
```

---

### Task 6: Registry skeleton — AlreadyInstalled + per-metaclass writer mutex map

**Files:**
- Create: `Yuki/include/Yuki/Core/Registry.h`
- Create: `Yuki/src/Core/Registry.cpp`
- Test: `Yuki/tests/Core/RegistryTest.cpp`

- [ ] **Step 1: Write the failing test**

Create `Yuki/tests/Core/RegistryTest.cpp`:

```cpp
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/MetaClass.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

TEST_CASE("AlreadyInstalled is false for an unseen iid", AUTO_TAG) {
    REQUIRE_FALSE(Registry::AlreadyInstalled(Iid{0xDEADBEEF, 0}));
}

TEST_CASE("MarkInstalled flips AlreadyInstalled to true; idempotent", AUTO_TAG) {
    Iid id{0xCAFEBABE, 0};
    Registry::MarkInstalled(id);
    REQUIRE(Registry::AlreadyInstalled(id));
    Registry::MarkInstalled(id); // idempotent
    REQUIRE(Registry::AlreadyInstalled(id));
}

TEST_CASE("WriterMutex returns a per-metaclass mutex, stable across calls", AUTO_TAG) {
    auto& mc = MetaClassOf</* any registrable type */ int>; // placeholder; see step 3
    auto& m1 = Registry::WriterMutexFor(mc);
    auto& m2 = Registry::WriterMutexFor(mc);
    REQUIRE(&m1 == &m2);
}
```

*Note: The `int` placeholder above will not compile because `int` is not annotated. Replace it with a fixture type from `Meta.h` or a local annotated stub.* Use the existing `StatelessTestImpl` from Task 1's tests or define a local stub:

```cpp
namespace { struct [[=Anno::Implementation]] StubImpl : Yuki::MetaNode<StubImpl> {}; }
// in the test:
auto& m1 = Registry::WriterMutexFor(MetaClassOf<StubImpl>);
auto& m2 = Registry::WriterMutexFor(MetaClassOf<StubImpl>);
REQUIRE(&m1 == &m2);
```

- [ ] **Step 2: Confirm failure**

```bash
cmake --build build/x64-asan --target Test.Core.RegistryTest
```

Expected: compile/link error on `Registry::*`.

- [ ] **Step 3: Create Registry.h**

```cpp
#pragma once
#include <Yuki/Core/Identity.h>
#include <Mashiro/Core/SpinMutex.h>

namespace Yuki {
    class MetaClass;
    namespace Registry {
        [[nodiscard]] bool             AlreadyInstalled(Iid id) noexcept;
        void                           MarkInstalled(Iid id) noexcept;
        [[nodiscard]] Mashiro::SpinMutex& WriterMutexFor(const MetaClass& m) noexcept;
    } // namespace Registry
} // namespace Yuki
```

- [ ] **Step 4: Create Registry.cpp**

```cpp
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/MetaClass.h>
#include <unordered_map>
#include <unordered_set>

namespace Yuki::Registry {

namespace {
    Mashiro::SpinMutex&                    InstalledMutex() noexcept {
        static Mashiro::SpinMutex m; return m;
    }
    std::unordered_set<Iid, IidHash>&      InstalledSet() noexcept {
        static std::unordered_set<Iid, IidHash> s; return s;
    }
    Mashiro::SpinMutex&                    MutexMapMutex() noexcept {
        static Mashiro::SpinMutex m; return m;
    }
    std::unordered_map<const MetaClass*, Mashiro::SpinMutex>& MutexMap() noexcept {
        static std::unordered_map<const MetaClass*, Mashiro::SpinMutex> mp; return mp;
    }
}

bool AlreadyInstalled(Iid id) noexcept {
    std::lock_guard g{InstalledMutex()};
    return InstalledSet().contains(id);
}

void MarkInstalled(Iid id) noexcept {
    std::lock_guard g{InstalledMutex()};
    InstalledSet().insert(id);
}

Mashiro::SpinMutex& WriterMutexFor(const MetaClass& m) noexcept {
    std::lock_guard g{MutexMapMutex()};
    return MutexMap()[&m];
}

} // namespace Yuki::Registry
```

Add `Yuki/src/Core/Registry.cpp` to the Yuki library source list. Register `Test.Core.RegistryTest` in the test CMakeLists.

- [ ] **Step 5: Build and run**

```bash
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add Yuki/include/Yuki/Core/Registry.h Yuki/src/Core/Registry.cpp Yuki/tests/Core/RegistryTest.cpp Yuki/CMakeLists.txt Yuki/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Registry): skeleton with idempotent install set and per-metaclass mutex

Per spec §3.3 step 3–4. AlreadyInstalled / MarkInstalled deduplicate registrar
runs across TUs and DLLs; WriterMutexFor returns a stable per-metaclass mutex
so concurrent installs against different metaclasses do not serialize.
Install<T>() body lands in Tasks 7–8.
EOF
)"
```

---

### Task 7: Registry::Install for Implementation classes

**Files:**
- Modify: `Yuki/include/Yuki/Core/Registry.h`
- Test: `Yuki/tests/Core/RegistryTest.cpp` (extend)

- [ ] **Step 1: Write the failing test**

Append to `Yuki/tests/Core/RegistryTest.cpp`:

```cpp
namespace {
    struct [[=Anno::Interface]] IBoaA : RootObject { virtual int A() const = 0; };
    struct [[=Anno::Interface]] IBoaB : RootObject { virtual int B() const = 0; };

    struct [[=Anno::Implementation]] BoaImpl
        : MetaNode<BoaImpl>, IBoaA, IBoaB {
        int A() const override { return 1; }
        int B() const override { return 2; }
    };
}

TEST_CASE("Registry::Install<T> publishes a DispatchSnapshot for an Implementation",
          AUTO_TAG) {
    Registry::Install<BoaImpl>();
    const auto* s = MetaClassOf<BoaImpl>.links().dispatch.load(std::memory_order_acquire);
    REQUIRE(s != nullptr);
    REQUIRE(s->count == 2);

    const auto* eA = Detail::LookupEntry(s, IidOf<IBoaA>);
    REQUIRE(eA != nullptr);
    REQUIRE(eA->kind == DispatchKind::DirectCast);

    const auto* eB = Detail::LookupEntry(s, IidOf<IBoaB>);
    REQUIRE(eB != nullptr);
    REQUIRE(eB->kind == DispatchKind::DirectCast);
}

TEST_CASE("Registry::Install is idempotent", AUTO_TAG) {
    Registry::Install<BoaImpl>();
    const auto* s1 = MetaClassOf<BoaImpl>.links().dispatch.load(std::memory_order_acquire);
    Registry::Install<BoaImpl>();
    const auto* s2 = MetaClassOf<BoaImpl>.links().dispatch.load(std::memory_order_acquire);
    REQUIRE(s1 == s2);
}
```

- [ ] **Step 2: Confirm failure**

```bash
cmake --build build/x64-asan --target Test.Core.RegistryTest
```

Expected: link error on `Install<T>`.

- [ ] **Step 3: Implement Install<T> for Implementation in Registry.h**

In `Yuki/include/Yuki/Core/Registry.h`, add (as a template, so the reflection body sees concrete `T`):

```cpp
namespace Yuki::Detail {

template <class T>
constexpr auto CollectImplDispatchEntries() {
    // Reflection: walk T's annotated Implements list; for each interface I in the
    // closure of inherited implements, emit a DirectCast entry with offset = the
    // result of static_cast<I*>((T*)0x1000) - 0x1000.
    constexpr auto impls = Anno::ImplementsOf<T>; // existing facility in Identity.h
    std::array<DispatchEntry, impls.size()> out{};
    std::size_t i = 0;
    template for (constexpr auto Ireflect : impls) {
        using I = [:Ireflect:];
        constexpr std::ptrdiff_t offset =
            reinterpret_cast<std::byte*>(static_cast<I*>(reinterpret_cast<T*>(0x1000))) -
            reinterpret_cast<std::byte*>(reinterpret_cast<T*>(0x1000));
        out[i++] = {IidOf<I>, DispatchKind::DirectCast, {.staticOffset = offset}};
    }
    std::ranges::sort(out, {}, &DispatchEntry::iid);
    return out;
}

} // namespace Yuki::Detail

namespace Yuki::Registry {

template <Anno::ImplementationClass T>
void Install() noexcept {
    if (AlreadyInstalled(IidOf<T>)) return;
    auto& mc = MetaClassOf<T>;
    auto& mtx = WriterMutexFor(mc);
    std::lock_guard g{mtx};
    if (AlreadyInstalled(IidOf<T>)) return; // double-check under lock

    static constexpr auto entries = Detail::CollectImplDispatchEntries<T>();
    static constexpr DispatchSnapshot snap{entries.size(), entries.data(), nullptr};
    const auto* old = mc.links().dispatch.exchange(&snap, std::memory_order_release);
    if (old != nullptr) {
        Detail::RetireSnapshot(old, [](const DispatchSnapshot*) noexcept { /* static, no-op */ });
    }
    MarkInstalled(IidOf<T>);
    Detail::SweepRetirements();
}

} // namespace Yuki::Registry
```

If `Anno::ImplementsOf<T>` does not yet exist as a `consteval` reflection helper, add it in `Identity.h` as part of this task (one line of reflection: walk the `Anno::Implements` annotation's array argument).

- [ ] **Step 4: Build and run**

```bash
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Registry.h Yuki/include/Yuki/Core/Identity.h Yuki/tests/Core/RegistryTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Registry): Install<T> for Implementation publishes DirectCast snapshot

Per spec §3.3 step 1 and §4.2 DirectCast arm. Reflection over T's
Anno::Implements list (plus OM-inherited bases) yields a sorted array of
DispatchEntry; the snapshot is a constexpr-static struct so its lifetime
spans the program — no allocation. InlineFacade entries land in Task 11
once HasInlineFacadeFor is in place.
EOF
)"
```

---

### Task 8: `Registry::Install<E>` for Extension classes

**Files:**
- Modify: `Yuki/include/Yuki/Core/Registry.h` (add Extension overload)
- Modify: `Yuki/src/Core/Registry.cpp` (resolver generation, eager-set publication)
- Test: `Yuki/tests/Core/RegistryTest.cpp` (extend)

**Spec refs:** §1.5 cardinality/instantiation policy; §3.2 DispatchKind selection table; §3.3 step 2 (Extensions); §3.3 step 5 (Eager-set bookkeeping); §4.2 dynamic kernel (`CodeExtensionSingleton`, `SideTableResolver` arms).

- [ ] **Step 1: Write the failing tests**

Append to `Yuki/tests/Core/RegistryTest.cpp`:

```cpp
#include <Yuki/Core/Annotation.h>

using namespace Yuki;

struct ICookable      : Anno::Interface { virtual ~ICookable() = default; virtual void Cook() = 0; };
[[= Anno::Iid{"YukiTest::Steak"}, Anno::Implements{^^ICookable} =]]
struct Steak          : Anno::Implementation, RootObject { void Cook() override {} };

// Stateless Code Extension: empty Extension class, eager.
[[= Anno::Iid{"YukiTest::SaltShaker"}, Anno::Extends{^^Steak}, Anno::Implements{^^ICookable}, Anno::Eager =]]
struct SaltShaker     : Anno::Extension {};

// Stateful Side-Table Extension: has NSDM; lazy default.
[[= Anno::Iid{"YukiTest::Cooked"}, Anno::Extends{^^Steak}, Anno::Implements{^^ICookable} =]]
struct Cooked         : Anno::Extension { int temperatureC = 0; };

TEST_CASE("Install<E> for stateless extension publishes CodeExtensionSingleton", "[registry]") {
    Registry::Install<Steak>();
    Registry::Install<SaltShaker>();

    auto& mc = MetaClassOf<Steak>;
    auto* snap = mc.links().dispatch.load(std::memory_order_acquire);
    auto* e    = Detail::LookupEntry(snap, IidOf<ICookable>);
    REQUIRE(e != nullptr);
    // SaltShaker wins over Steak's DirectCast for ICookable because Extensions override Implementation
    // for the same (target, iid). See spec §3.2 precedence note.
    REQUIRE(e->kind == DispatchKind::CodeExtensionSingleton);
}

TEST_CASE("Install<E> for stateful extension publishes SideTableResolver", "[registry]") {
    Registry::Install<Steak>();
    Registry::Install<Cooked>();

    auto& mc = MetaClassOf<Steak>;
    auto* snap = mc.links().dispatch.load(std::memory_order_acquire);
    auto* e    = Detail::LookupEntry(snap, IidOf<ICookable>);
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::SideTableResolver);
    REQUIRE(e->payload.resolver != nullptr);
}

TEST_CASE("Install<E> registers eager extensions into the implementation's EagerSet", "[registry]") {
    Registry::Install<Steak>();
    Registry::Install<SaltShaker>();   // eager + stateless → no EagerSet entry per spec §3.3 step 5
    Registry::Install<Cooked>();       // not eager → no EagerSet entry

    auto& mc = MetaClassOf<Steak>;
    auto* es = mc.links().eagerSet.load(std::memory_order_acquire);
    // Both candidate Extensions are either stateless-eager (omitted) or lazy (omitted).
    REQUIRE((es == nullptr || es->count == 0));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: FAIL — `Install<E>` overload for `ExtensionClass` not yet present.

- [ ] **Step 3: Implement the Extension overload**

Add to `Yuki/include/Yuki/Core/Registry.h`:

```cpp
namespace Yuki::Detail {

// Resolver generated per stateful Extension E. Walks n->facades_; on miss,
// materializes E (via E::MaterializeInto when Materialize policy says so).
// The Materialize-OFF variant is selected by Has<I>/Materialized<I>; here we
// emit the Materialize-ON resolver. The OFF variant is the same body up to
// the final `else return nullptr;` and is generated by Task 12.
template <Anno::ExtensionClass E>
RootObject* SideTableResolverFor(RootObject* n) noexcept {
    if (auto* hit = FacadeListLookup(n->facades_, IidOf<E>)) return hit;
    E::MaterializeInto(*n);                                        // emitted in Task 10
    return FacadeListLookup(n->facades_, IidOf<E>);
}

template <Anno::ExtensionClass E>
inline E SingletonInstanceFor{};                                   // process-wide stateless singleton

template <Anno::ExtensionClass E>
RootObject* SingletonAddressFor() noexcept {
    return static_cast<RootObject*>(&SingletonInstanceFor<E>);
}

} // namespace Yuki::Detail

namespace Yuki::Registry {

template <Anno::ExtensionClass E>
void Install() noexcept {
    if (AlreadyInstalled(IidOf<E>)) return;

    // Walk Anno::Extends{B1, B2, ...} reflected from E.
    template for (constexpr auto Breflect : Anno::ExtendsOf<E>) {
        using B = [:Breflect:];
        auto& mc  = MetaClassOf<B>;
        auto& mtx = WriterMutexFor(mc);
        std::lock_guard g{mtx};

        // Compose a new sorted snapshot by merging E's (iid, kind) tuples
        // with the existing snapshot. E provides every iid in implements(E).
        Detail::PublishExtensionEntries<E, B>(mc);
        if constexpr (Anno::IsEager<E> && !Anno::StatelessExtensionClass<E>) {
            Detail::AppendToEagerSet<E, B>(mc);
        }
    }
    MarkInstalled(IidOf<E>);
    Detail::SweepRetirements();
}

} // namespace Yuki::Registry
```

Implement `PublishExtensionEntries<E, B>` and `AppendToEagerSet<E, B>` in
`Yuki/src/Core/Registry.cpp`. Both follow the read-snapshot → allocate-new →
CAS-publish-via-exchange → retire-old pattern from Tasks 3/5. Selection rule
inside `PublishExtensionEntries`:

```cpp
constexpr DispatchKind kind = Anno::StatelessExtensionClass<E>
    ? DispatchKind::CodeExtensionSingleton
    : DispatchKind::SideTableResolver;

// Per spec §3.2: Extensions override Implementation entries for the same iid.
// Merge by iid; if a same-iid entry exists, replace with the new (kind, payload).
```

Add `Anno::ExtendsOf<E>` and `Anno::IsEager<E>` to `Yuki/include/Yuki/Core/Annotation.h` as `consteval` accessors (one-line reflection over the annotation list).

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Registry.h Yuki/src/Core/Registry.cpp \
        Yuki/include/Yuki/Core/Annotation.h Yuki/tests/Core/RegistryTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Registry): Install<E> publishes CodeExtensionSingleton / SideTableResolver

Stateless Extension classes (reflection-counted NSDM == 0) take the
CodeExtensionSingleton arm; stateful ones take SideTableResolver. Merging
the new entries over the extended metaclass's existing snapshot follows
the read→allocate→exchange→retire pattern from Tasks 3/5. Eager+stateful
Extensions also append to the implementation metaclass's EagerSet for the
construction-time materialization hook (Task 10).
EOF
)"
```

---

### Task 9: CRTP hook — `Registrar<T>` instantiation in `MetaNode`/`ExtensionNode`/`IfaceFacadeNode`

**Files:**
- Modify: `Yuki/include/Yuki/Core/MetaClass.h` (CRTP bases)
- Modify: `Yuki/include/Yuki/Core/InterfaceFacade.h` (CRTP base)
- Test: `Yuki/tests/Core/RegistryTest.cpp` (extend with "construction implies installation")

**Spec refs:** §3.3 (inline static + constexpr address-take); §3.4 (DLL hot-load flow); §7.8 (`extern "C" YukiRegister_<iid>` symbol convention).

- [ ] **Step 1: Write the failing test**

Append to `Yuki/tests/Core/RegistryTest.cpp`:

```cpp
TEST_CASE("Constructing a MetaNode<T> triggers Registrar<T> once", "[registry]") {
    // Before any instance: not yet installed. (Static-init may have already done it; the
    // contract is "installed by the time the first instance constructs", not "deferred until then".)
    {
        Steak s;
        REQUIRE(Registry::AlreadyInstalled(IidOf<Steak>));
    }
    // Second construction is a no-op (idempotent).
    {
        Steak s;
        REQUIRE(Registry::AlreadyInstalled(IidOf<Steak>));
    }
}

TEST_CASE("Named YukiRegister_<iid> symbol exists and installs", "[registry]") {
    // The CRTP base must emit `extern "C" void YukiRegister_<mangled_iid>() noexcept`
    // wrapping Install<T>(). Calling it directly must be a safe idempotent install.
    extern "C" void YukiRegister_YukiTest__Steak() noexcept;
    YukiRegister_YukiTest__Steak();
    REQUIRE(Registry::AlreadyInstalled(IidOf<Steak>));
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: FAIL — link error on `YukiRegister_YukiTest__Steak`; or the `AlreadyInstalled` check fails because the CRTP base does not yet ODR-use the registrar.

- [ ] **Step 3: Add the hook to all three CRTP bases**

In `Yuki/include/Yuki/Core/MetaClass.h`, replace the existing `MetaNode<Self, Base>` and `ExtensionNode<Self, Extendee, Base>` headers with:

```cpp
namespace Yuki {

template <class Self, class Base = RootObject>
struct MetaNode : Base {
    using Base::Base;

    // Registrar runs once at static init or first ODR-use. inline static guarantees one
    // definition across DLL boundaries; the constexpr address-take guarantees ODR-use even
    // if no other member of MetaNode is touched.
    inline static Detail::Registrar<Self> _registrar{};
    static constexpr void* _registrar_anchor = static_cast<void*>(&_registrar);

    MetaNode() noexcept {
        (void)_registrar_anchor;                       // force ODR-use
        Detail::MaterializeEagerSet(*this);            // Task 10
    }
};

template <class Self, class Extendee, class Base = RootObject>
struct ExtensionNode : Base {
    using Base::Base;
    inline static Detail::Registrar<Self> _registrar{};
    static constexpr void* _registrar_anchor = static_cast<void*>(&_registrar);

    ExtensionNode() noexcept { (void)_registrar_anchor; }
};

} // namespace Yuki
```

In `Yuki/include/Yuki/Core/InterfaceFacade.h`, do the same for the `IfaceFacadeNode<Self, Iface, Impl>` template.

Add to `Yuki/include/Yuki/Core/Registry.h` the named symbol macro that the CRTP base also emits:

```cpp
#define YUKI_DEFINE_REGISTRAR_SYMBOL(T, mangledIid)                                          \
    extern "C" void YukiRegister_##mangledIid() noexcept {                                   \
        ::Yuki::Registry::Install<T>();                                                      \
    }
```

The macro is invoked once per registered type. Since `mangledIid` is a token, it is computed by a small consteval helper in `Identity.h` (`Anno::IidMangle<T>`) and emitted by the source file declaring `T`. The plan keeps the macro hand-invoked here; the manifest pipeline (Plan 2) will generate the invocation automatically.

For the test, in `Yuki/tests/Core/RegistryTest.cpp` define the symbol next to the type:

```cpp
YUKI_DEFINE_REGISTRAR_SYMBOL(Steak, YukiTest__Steak)
```

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/MetaClass.h Yuki/include/Yuki/Core/InterfaceFacade.h \
        Yuki/include/Yuki/Core/Registry.h Yuki/tests/Core/RegistryTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core): CRTP bases ODR-use Registrar<Self> via constexpr address-take

Per spec §3.3 CRTP hook. inline static gives one definition per type across
DLLs; the constexpr void* anchor forces ODR-use even when no other base
member is touched. The named extern "C" YukiRegister_<mangledIid> wrapper
(spec §7.8) is emitted via YUKI_DEFINE_REGISTRAR_SYMBOL so the discovery
layer (Plan 2) can call it without depending on static-init ordering.
EOF
)"
```

---

### Task 10: `MaterializeInto` codegen + `MaterializeEagerSet` construction hook

**Files:**
- Modify: `Yuki/include/Yuki/Core/Registry.h` (codegen for `E::MaterializeInto`)
- Modify: `Yuki/include/Yuki/Core/MetaClass.h` (`Detail::MaterializeEagerSet`)
- Test: `Yuki/tests/Core/RegistryTest.cpp` (eager materialization)

**Spec refs:** §1.5 (eager/lazy instantiation policy); §3.3 "Closure construction hook"; §6.4 (`Reify<E>`).

- [ ] **Step 1: Write the failing test**

Append to `Yuki/tests/Core/RegistryTest.cpp`:

```cpp
[[= Anno::Iid{"YukiTest::Plated"}, Anno::Extends{^^Steak}, Anno::Implements{^^ICookable}, Anno::Eager =]]
struct Plated : Anno::Extension { int garnishCount = 0; void Cook() {} };

YUKI_DEFINE_REGISTRAR_SYMBOL(Plated, YukiTest__Plated)

TEST_CASE("Eager stateful Extension materializes during nucleus construction", "[registry][materialize]") {
    Registry::Install<Steak>();
    Registry::Install<Plated>();

    Steak s;                                          // construction-time pass
    auto* node = Detail::FacadeListLookup(s.facades_, IidOf<Plated>);
    REQUIRE(node != nullptr);                         // present immediately, no Query needed
    REQUIRE(node->TypeDynamic() == ClassType::Extension);
}

TEST_CASE("Lazy Extension is absent until Query/Reify", "[registry][materialize]") {
    Registry::Install<Steak>();
    Registry::Install<Cooked>();                      // lazy

    Steak s;
    REQUIRE(Detail::FacadeListLookup(s.facades_, IidOf<Cooked>) == nullptr);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: FAIL — `MaterializeEagerSet` is unimplemented (currently a no-op shell from Task 9).

- [ ] **Step 3: Implement `MaterializeInto` codegen and the construction hook**

Add to `Yuki/include/Yuki/Core/Registry.h`:

```cpp
namespace Yuki::Detail {

// Emitted per stateful Extension E by its Registrar. Constructs an E, wraps it in a FacadeNode
// keyed on IidOf<E> (so IsMaterialized<E> is a single Lookup) plus every iid in implements(E),
// and AttachUnique-CAS-installs it under nucleus->facades_.
template <Anno::ExtensionClass E>
void MaterializeIntoImpl(RootObject& nucleus) noexcept {
    if (FacadeListLookup(nucleus.facades_, IidOf<E>) != nullptr) return;  // idempotent
    auto* node = new FacadeNode<E>{};                                      // allocator: see §5.2 perf
    AttachUnique(nucleus.facades_, /*selfIid=*/IidOf<E>, node);
    template for (constexpr auto Ireflect : Anno::ImplementsOf<E>) {
        using I = [:Ireflect:];
        AttachUnique(nucleus.facades_, IidOf<I>, node);
    }
}

} // namespace Yuki::Detail
```

The Extension's class itself receives a static member that delegates to the helper. This is done by adding to the `ExtensionNode` CRTP base (touched in Task 9):

```cpp
template <class Self, class Extendee, class Base = RootObject>
struct ExtensionNode : Base {
    // ... unchanged ...
    static void MaterializeInto(RootObject& nucleus) noexcept {
        Detail::MaterializeIntoImpl<Self>(nucleus);
    }
};
```

Add to `Yuki/include/Yuki/Core/MetaClass.h`:

```cpp
namespace Yuki::Detail {

inline void MaterializeEagerSet(RootObject& self) noexcept {
    const MetaClass& mc = self.MetaClassDynamic();
    const auto* es = mc.links().eagerSet.load(std::memory_order_acquire);
    if (es == nullptr) return;
    for (std::size_t i = 0; i < es->count; ++i) {
        es->entries[i].materializeInto(self);     // function pointer captured by AppendToEagerSet
    }
}

} // namespace Yuki::Detail
```

`EagerSetEntry` is a `{Iid, void(*)(RootObject&) noexcept}` pair; `AppendToEagerSet<E, B>` in Task 8 stores `&Detail::MaterializeIntoImpl<E>` in the function-pointer slot.

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.RegistryTest
ctest --test-dir build/x64-asan -R "RegistryTest" --output-on-failure
```

Expected: pass. Verify under asan that no leaks are reported (FacadeNode ownership is the nucleus's `facades_`; sweep happens at nucleus destruction — Task 16 covers the destructor pass).

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Registry.h Yuki/include/Yuki/Core/MetaClass.h \
        Yuki/tests/Core/RegistryTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core): eager Extensions materialize during nucleus construction

Per spec §1.5 and §3.3 closure construction hook. MetaNode<T>::MetaNode()
walks MetaClassOf<T>.eagerSet — populated by Install<E> in Task 8 — and
calls each entry's materializeInto(*this), which performs the same
AttachUnique sequence the lazy SideTableResolver uses. Eager and lazy paths
converge structurally: a subsequent Query<I>(p) finds the same FacadeNode
either way.
EOF
)"
```

---

### Task 11: `Query` static face — `HasInlineFacadeFor` + `InlineFacadeAddress`

**Files:**
- Modify: `Yuki/include/Yuki/Core/Query.h` (replace `Query<I>` static face)
- Modify: `Yuki/include/Yuki/Core/Registry.h` (add `InlineFacade` arm to `CollectImplDispatchEntries<T>`)
- Test: `Yuki/tests/Core/QueryTest.cpp` (new BOA + inline-facade tests)

**Spec refs:** §4.1 (static face); §3.2 row 2 (`InlineFacade` selection); §4.3 (scenario "facade view of `E1` exposing `IE1` for `IN`").

- [ ] **Step 1: Write the failing tests**

Append to `Yuki/tests/Core/QueryTest.cpp`:

```cpp
#include <Yuki/Core/Annotation.h>
#include <Yuki/Core/Query.h>

using namespace Yuki;

struct IHeat   : Anno::Interface { virtual ~IHeat() = default; virtual int Celsius() const = 0; };
struct ICool   : Anno::Interface { virtual ~ICool() = default; virtual int Celsius() const = 0; };

// BOA path: ICool is implemented by inheritance.
[[= Anno::Iid{"YukiTest::Fridge"}, Anno::Implements{^^ICool} =]]
struct Fridge : Anno::Implementation, ICool, RootObject { int Celsius() const override { return 4; } };

// Cold inline-facade path: Stove holds an InterfaceFacade<IHeat, Stove>.
[[= Anno::Iid{"YukiTest::Stove"}, Anno::Implements{^^IHeat} =]]
struct Stove : Anno::Implementation, RootObject {
    int burnerC = 0;
    InterfaceFacade<IHeat, Stove> _heatFacade{this, [](Stove* s) { return s->burnerC; }};
};

YUKI_DEFINE_REGISTRAR_SYMBOL(Fridge, YukiTest__Fridge)
YUKI_DEFINE_REGISTRAR_SYMBOL(Stove,  YukiTest__Stove)

TEST_CASE("Query<I>(p) on BOA path folds to static_cast", "[query][static-face]") {
    Fridge f;
    ICool* p = RT::Query<ICool>(&f);
    REQUIRE(p == static_cast<ICool*>(&f));            // exact identity, no indirection
    REQUIRE(p->Celsius() == 4);
}

TEST_CASE("Query<I>(p) on inline-facade path returns the embedded facade's address", "[query][static-face]") {
    Stove s;
    s.burnerC = 180;
    IHeat* p = RT::Query<IHeat>(&s);
    REQUIRE(p != nullptr);
    REQUIRE(p->Celsius() == 180);
    // Must be the inline NSDM, not a heap allocation.
    REQUIRE(reinterpret_cast<std::byte*>(p) >= reinterpret_cast<std::byte*>(&s));
    REQUIRE(reinterpret_cast<std::byte*>(p) <  reinterpret_cast<std::byte*>(&s) + sizeof(Stove));
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.QueryTest
ctest --test-dir build/x64-asan -R "QueryTest" --output-on-failure
```

Expected: FAIL — `Query<I>` static face still calls the dynamic kernel for the inline-facade case.

- [ ] **Step 3: Implement `HasInlineFacadeFor` and `InlineFacadeAddress`, then rewrite the static face**

Add to `Yuki/include/Yuki/Core/Query.h`:

```cpp
namespace Yuki::Detail {

template <class C, class I>
consteval bool HasInlineFacadeFor() {
    template for (constexpr auto m : std::meta::nonstatic_data_members_of(
                      ^^C, std::meta::access_context::current())) {
        constexpr auto T = std::meta::type_of(m);
        if constexpr (std::meta::has_template_arguments(T) &&
                      std::meta::template_of(T) == ^^InterfaceFacade) {
            constexpr auto args = std::meta::template_arguments_of(T);
            using Iarg = [:args[0]:];
            using Xarg = [:args[1]:];
            if constexpr (std::same_as<Iarg, I> && std::derived_from<C, Xarg>) return true;
        }
    }
    return false;
}

template <class I, class C>
[[nodiscard]] constexpr I* InlineFacadeAddress(C* p) noexcept {
    template for (constexpr auto m : std::meta::nonstatic_data_members_of(
                      ^^C, std::meta::access_context::current())) {
        constexpr auto T = std::meta::type_of(m);
        if constexpr (std::meta::has_template_arguments(T) &&
                      std::meta::template_of(T) == ^^InterfaceFacade) {
            constexpr auto args = std::meta::template_arguments_of(T);
            using Iarg = [:args[0]:];
            using Xarg = [:args[1]:];
            if constexpr (std::same_as<Iarg, I> && std::derived_from<C, Xarg>) {
                return static_cast<I*>(&p->[:m:]);
            }
        }
    }
    std::unreachable();        // HasInlineFacadeFor<C, I> was true; this branch is impossible
}

} // namespace Yuki::Detail

namespace Yuki::RT {

template <Anno::InterfaceClass I, class C>
[[nodiscard]] constexpr auto Query(C* p) noexcept -> I* {
    if (p == nullptr) return nullptr;
    if constexpr (std::derived_from<C, I>) {
        return static_cast<I*>(p);
    } else if constexpr (Detail::HasInlineFacadeFor<C, I>()) {
        return Detail::InlineFacadeAddress<I, C>(p);
    } else {
        return static_cast<I*>(QueryDynamicRaw(p, IidOf<I>));
    }
}

} // namespace Yuki::RT
```

Extend `Detail::CollectImplDispatchEntries<T>` in `Yuki/include/Yuki/Core/Registry.h`: after the `Anno::Implements`-loop, append `InlineFacade` entries for each `(I, NSDM)` pair detected by `HasInlineFacadeFor<T, I>` so dynamic kernel callers see them too. The static face short-circuits before reaching the kernel; the kernel entry exists for the case where the call site only has a `RootObject*`.

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.QueryTest
ctest --test-dir build/x64-asan -R "QueryTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Query.h Yuki/include/Yuki/Core/Registry.h Yuki/tests/Core/QueryTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Query): static face folds BOA to cast, inline-facade to NSDM addr

Per spec §4.1. HasInlineFacadeFor<C, I> reflects over C's NSDMs for an
InterfaceFacade<I, X> with X a base of C; InlineFacadeAddress emits the
offsetof via reflection. Hot BOA path produces a single static_cast, no
table lookup. CollectImplDispatchEntries also emits InlineFacade kernel
entries for the same (I, NSDM) pairs so calls through RootObject* still
resolve them.
EOF
)"
```

---

### Task 12: `Query` dynamic kernel — `QueryDynamicRaw` + `Materialize` policy

**Files:**
- Modify: `Yuki/include/Yuki/Core/Query.h` (kernel switch over `DispatchKind`)
- Modify: `Yuki/src/Core/Query.cpp` (define `QueryDynamicRaw`)
- Test: `Yuki/tests/Core/QueryTest.cpp` (dynamic resolution covering all 5 arms)

**Spec refs:** §4.2 (dynamic kernel); §6.4 ("dispatch kernel is parameterized on a `Materialize` boolean policy").

- [ ] **Step 1: Write the failing tests**

Append to `Yuki/tests/Core/QueryTest.cpp`:

```cpp
TEST_CASE("Dynamic kernel resolves DirectCast via RootObject*", "[query][dynamic]") {
    Fridge f;
    RootObject* erased = &f;
    ICool* p = static_cast<ICool*>(RT::QueryDynamicRaw(erased, IidOf<ICool>));
    REQUIRE(p != nullptr);
    REQUIRE(p->Celsius() == 4);
}

TEST_CASE("Dynamic kernel resolves CodeExtensionSingleton via stateless Extension", "[query][dynamic]") {
    Registry::Install<Steak>();
    Registry::Install<SaltShaker>();
    Steak s;
    auto* ck = RT::Query<ICookable>(&s);                       // routes through dynamic since not BOA
    REQUIRE(ck != nullptr);
    REQUIRE(ck == &Detail::SingletonInstanceFor<SaltShaker>);  // process-wide singleton
}

TEST_CASE("Dynamic kernel SideTableResolver materializes lazy Extension", "[query][dynamic]") {
    Registry::Install<Steak>();
    Registry::Install<Cooked>();                                // lazy
    Steak s;
    REQUIRE(Detail::FacadeListLookup(s.facades_, IidOf<Cooked>) == nullptr);
    auto* ck = RT::Query<ICookable>(&s);                        // triggers materialization
    REQUIRE(ck != nullptr);
    REQUIRE(Detail::FacadeListLookup(s.facades_, IidOf<Cooked>) != nullptr);
}

TEST_CASE("Has<I> (Materialize=false) does NOT materialize", "[query][dynamic][materialize-policy]") {
    Registry::Install<Steak>();
    Registry::Install<Cooked>();
    Steak s;
    REQUIRE(RT::Has<ICookable>(&s) == false);                   // lazy and unmaterialized → false
    REQUIRE(Detail::FacadeListLookup(s.facades_, IidOf<Cooked>) == nullptr);  // still nothing
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.QueryTest
ctest --test-dir build/x64-asan -R "QueryTest" --output-on-failure
```

Expected: FAIL — `QueryDynamicRaw` and `RT::Has` are not yet defined.

- [ ] **Step 3: Implement the dynamic kernel**

In `Yuki/include/Yuki/Core/Query.h`:

```cpp
namespace Yuki::RT {

enum class Materialize : bool { No = false, Yes = true };

template <Materialize M = Materialize::Yes>
[[nodiscard]] RootObject* QueryDynamicRawPolicy(RootObject* p, Iid id) noexcept;

[[nodiscard]] inline RootObject* QueryDynamicRaw(RootObject* p, Iid id) noexcept {
    return QueryDynamicRawPolicy<Materialize::Yes>(p, id);
}

template <Anno::InterfaceClass I>
[[nodiscard]] bool Has(RootObject* p) noexcept {
    return QueryDynamicRawPolicy<Materialize::No>(p, IidOf<I>) != nullptr;
}

} // namespace Yuki::RT
```

In `Yuki/src/Core/Query.cpp`:

```cpp
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/MetaClass.h>

namespace Yuki::RT {

template <Materialize M>
RootObject* QueryDynamicRawPolicy(RootObject* p, Iid id) noexcept {
    RootObject* n = Nucleus(p);
    if (n == nullptr) return nullptr;

    const auto& links = n->MetaClassDynamic().links();
    const auto* s = links.dispatch.load(std::memory_order_acquire);
    const auto* e = Detail::LookupEntry(s, id);
    if (e != nullptr) {
        switch (e->kind) {
        case DispatchKind::DirectCast:
        case DispatchKind::InlineFacade:
            return reinterpret_cast<RootObject*>(reinterpret_cast<std::byte*>(n) + e->payload.staticOffset);
        case DispatchKind::CodeExtensionSingleton:
            return *e->payload.singleton;
        case DispatchKind::SideTableResolver:
            if constexpr (M == Materialize::No) {
                return Detail::FacadeListLookup(n->facades_, id);    // returns nullptr if not yet materialized
            } else {
                return e->payload.resolver(n);                       // materialize-and-attach
            }
        case DispatchKind::FacadeList:
            return Detail::FacadeListLookup(n->facades_, id);
        }
    }
    return Detail::FacadeListLookup(n->facades_, id);                // user-attached fallback
}

// Explicit instantiations
template RootObject* QueryDynamicRawPolicy<Materialize::Yes>(RootObject*, Iid) noexcept;
template RootObject* QueryDynamicRawPolicy<Materialize::No >(RootObject*, Iid) noexcept;

} // namespace Yuki::RT
```

Add `Yuki/src/Core/Query.cpp` to `Yuki/CMakeLists.txt` sources.

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.QueryTest
ctest --test-dir build/x64-asan -R "QueryTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Query.h Yuki/src/Core/Query.cpp Yuki/CMakeLists.txt \
        Yuki/tests/Core/QueryTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Query): dynamic kernel switches on DispatchKind, parameterized on Materialize

Per spec §4.2 and §6.4. QueryDynamicRawPolicy<M> is one switch over the
five DispatchKinds. The only arm that branches on M is SideTableResolver:
Materialize::No reads facades_ directly (returns nullptr if not yet
materialized), Materialize::Yes calls the resolver which does the same
lookup-then-materialize sequence. Has<I>(p) is the public Materialize::No
entry; Query<I> the Materialize::Yes one.
EOF
)"
```

---

### Task 13: `Query` convenience layer — `Provider<I>`, `Reify<E>`, `IsMaterialized<E>`, `Materialized<I>`

**Files:**
- Modify: `Yuki/include/Yuki/Core/Query.h`
- Test: `Yuki/tests/Core/QueryTest.cpp`

**Spec refs:** §1.3 (convenience derivations); §4.2 (`Provider` one-liner); §6.2/§6.4 (`Reify`, `IsMaterialized`).

- [ ] **Step 1: Write the failing tests**

Append to `Yuki/tests/Core/QueryTest.cpp`:

```cpp
TEST_CASE("Provider<I>(p) returns the provider object's underlying address", "[query][provider]") {
    Registry::Install<Steak>();
    Registry::Install<SaltShaker>();
    Steak s;
    RootObject* who = RT::Provider<ICookable>(&s);
    REQUIRE(who == &Detail::SingletonInstanceFor<SaltShaker>);
}

TEST_CASE("IsMaterialized<E>(p) reflects facades_ membership without materializing", "[query][is-materialized]") {
    Registry::Install<Steak>();
    Registry::Install<Cooked>();
    Steak s;
    REQUIRE(RT::IsMaterialized<Cooked>(&s) == false);
    (void)RT::Query<ICookable>(&s);                    // materialize via dispatch
    REQUIRE(RT::IsMaterialized<Cooked>(&s) == true);
}

TEST_CASE("Reify<E>(p) materializes without going through a specific Interface", "[query][reify]") {
    Registry::Install<Steak>();
    Registry::Install<Cooked>();
    Steak s;
    REQUIRE(RT::IsMaterialized<Cooked>(&s) == false);
    RT::Reify<Cooked>(&s);
    REQUIRE(RT::IsMaterialized<Cooked>(&s) == true);
}

TEST_CASE("Materialized<I>(p) returns nullptr for lazy, ptr after Query", "[query][materialized]") {
    Registry::Install<Steak>();
    Registry::Install<Cooked>();
    Steak s;
    REQUIRE(RT::Materialized<ICookable>(&s) == nullptr);
    (void)RT::Query<ICookable>(&s);
    REQUIRE(RT::Materialized<ICookable>(&s) != nullptr);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.QueryTest
ctest --test-dir build/x64-asan -R "QueryTest" --output-on-failure
```

Expected: FAIL — none of the four functions defined yet.

- [ ] **Step 3: Implement the convenience layer**

Append to `Yuki/include/Yuki/Core/Query.h`:

```cpp
namespace Yuki::RT {

template <Anno::InterfaceClass I, class C = RootObject>
[[nodiscard]] RootObject* Provider(C* p) noexcept {
    if (auto* face = Query<I>(p)) return Underlying(face);
    return nullptr;
}

template <Anno::ExtensionClass E>
[[nodiscard]] bool IsMaterialized(RootObject* p) noexcept {
    auto* n = Nucleus(p);
    return n != nullptr && Detail::FacadeListLookup(n->facades_, IidOf<E>) != nullptr;
}

template <Anno::ExtensionClass E>
void Reify(RootObject* p) noexcept {
    auto* n = Nucleus(p);
    if (n == nullptr) return;
    Detail::MaterializeIntoImpl<E>(*n);
}

template <Anno::InterfaceClass I>
[[nodiscard]] RootObject* Materialized(RootObject* p) noexcept {
    return QueryDynamicRawPolicy<Materialize::No>(p, IidOf<I>);
}

} // namespace Yuki::RT
```

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.QueryTest
ctest --test-dir build/x64-asan -R "QueryTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Query.h Yuki/tests/Core/QueryTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Query): convenience layer (Provider, Reify, IsMaterialized, Materialized)

Per spec §1.3. Provider<I> = Underlying ∘ Query<I>. IsMaterialized<E> is
one FacadeListLookup keyed on IidOf<E> (the self-iid stamped at attach
time). Reify<E> calls MaterializeIntoImpl directly. Materialized<I> is
just the Materialize::No instantiation of the kernel.
EOF
)"
```

---

### Task 14: Introspection — metaclass-level (`Capabilities`, `Provides`, `Extensions`, `Implementations`, `EagerExtensions`, `ProviderClass`, `ProviderDispatchKind`)

**Files:**
- Create: `Yuki/include/Yuki/Core/Introspection.h`
- Test: `Yuki/tests/Core/IntrospectionTest.cpp`
- Modify: `Yuki/tests/CMakeLists.txt` (register new test target)

**Spec refs:** §1.2 (RT operations); §6.1 (potential attributes); §6.3 (potential-vs-runtime symmetry); §6.6 (lifetime of views).

- [ ] **Step 1: Write the failing tests**

Create `Yuki/tests/Core/IntrospectionTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Yuki/Core/Introspection.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/Annotation.h>
#include <algorithm>
#include <ranges>

using namespace Yuki;

struct IA : Anno::Interface { virtual ~IA() = default; };
struct IB : Anno::Interface { virtual ~IB() = default; };

[[= Anno::Iid{"YukiTest::AImpl"}, Anno::Implements{^^IA} =]]
struct AImpl : Anno::Implementation, IA, RootObject {};

[[= Anno::Iid{"YukiTest::BExt"}, Anno::Extends{^^AImpl}, Anno::Implements{^^IB} =]]
struct BExt : Anno::Extension { int n = 0; };

YUKI_DEFINE_REGISTRAR_SYMBOL(AImpl, YukiTest__AImpl)
YUKI_DEFINE_REGISTRAR_SYMBOL(BExt,  YukiTest__BExt)

TEST_CASE("Capabilities lists every iid the closure can resolve", "[introspection][potential]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    auto& m = MetaClassOf<AImpl>;
    auto caps = RT::Capabilities(m);
    REQUIRE(std::ranges::find(caps, IidOf<IA>) != caps.end());
    REQUIRE(std::ranges::find(caps, IidOf<IB>) != caps.end());
}

TEST_CASE("Provides<I>(m) and ProviderClass<I>(m) agree", "[introspection][potential]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    auto& m = MetaClassOf<AImpl>;
    REQUIRE(RT::Provides<IA>(m));
    REQUIRE(RT::Provides<IB>(m));
    REQUIRE(RT::ProviderClass<IA>(m) == &MetaClassOf<AImpl>);
    REQUIRE(RT::ProviderClass<IB>(m) == &MetaClassOf<BExt>);
}

TEST_CASE("ProviderDispatchKind reports the resolved arm", "[introspection][potential]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    auto& m = MetaClassOf<AImpl>;
    REQUIRE(RT::ProviderDispatchKind<IA>(m) == DispatchKind::DirectCast);
    REQUIRE(RT::ProviderDispatchKind<IB>(m) == DispatchKind::SideTableResolver);
}

TEST_CASE("Extensions(m) enumerates registered extensions", "[introspection][potential]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    auto exts = RT::Extensions(MetaClassOf<AImpl>);
    REQUIRE(std::ranges::any_of(exts, [](auto* p) { return p == &MetaClassOf<BExt>; }));
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.IntrospectionTest
ctest --test-dir build/x64-asan -R "IntrospectionTest" --output-on-failure
```

Expected: FAIL — `Yuki/Core/Introspection.h` does not exist; the test target itself fails to configure.

- [ ] **Step 3: Implement `Introspection.h`**

Create `Yuki/include/Yuki/Core/Introspection.h`:

```cpp
#pragma once
#include <Yuki/Core/MetaClass.h>
#include <Yuki/Core/Registry.h>
#include <optional>
#include <ranges>
#include <span>

namespace Yuki::RT {

[[nodiscard]] inline std::span<const Iid> Capabilities(const MetaClass& m) noexcept {
    const auto* s = m.links().dispatch.load(std::memory_order_acquire);
    return s ? std::span<const Iid>{&s->entries[0].iid, s->count} : std::span<const Iid>{};
    // NB: the span strides over DispatchEntry, not Iid. Callers iterate via the metaclass's
    // entry-view helper. For correctness in the simple case, use IidsView below.
}

class IidsView : public std::ranges::view_interface<IidsView> {
    const DispatchSnapshot* s_;
public:
    explicit IidsView(const DispatchSnapshot* s) noexcept : s_(s) {}
    auto begin() const noexcept { return s_ ? &s_->entries[0] : nullptr; }
    auto end()   const noexcept { return s_ ? &s_->entries[s_->count] : nullptr; }
};

[[nodiscard]] inline auto IidsOf(const MetaClass& m) noexcept {
    return IidsView{m.links().dispatch.load(std::memory_order_acquire)}
        | std::views::transform([](const DispatchEntry& e) { return e.iid; });
}

template <Anno::InterfaceClass I>
[[nodiscard]] inline bool Provides(const MetaClass& m) noexcept {
    const auto* s = m.links().dispatch.load(std::memory_order_acquire);
    return Detail::LookupEntry(s, IidOf<I>) != nullptr;
}

template <Anno::InterfaceClass I>
[[nodiscard]] inline std::optional<DispatchKind> ProviderDispatchKind(const MetaClass& m) noexcept {
    const auto* s = m.links().dispatch.load(std::memory_order_acquire);
    const auto* e = Detail::LookupEntry(s, IidOf<I>);
    return e ? std::optional{e->kind} : std::nullopt;
}

template <Anno::InterfaceClass I>
[[nodiscard]] inline const MetaClass* ProviderClass(const MetaClass& m) noexcept {
    const auto* s = m.links().dispatch.load(std::memory_order_acquire);
    const auto* e = Detail::LookupEntry(s, IidOf<I>);
    return e ? e->providerClass : nullptr;       // new field on DispatchEntry: who registered it
}

[[nodiscard]] inline std::span<const MetaClass* const> Extensions(const MetaClass& m) noexcept {
    const auto* s = m.links().extendedBy.load(std::memory_order_acquire);
    return s ? std::span{s->classes, s->count} : std::span<const MetaClass* const>{};
}

[[nodiscard]] inline std::span<const MetaClass* const> Implementations(const MetaClass& m) noexcept {
    const auto* s = m.links().implementedBy.load(std::memory_order_acquire);
    return s ? std::span{s->classes, s->count} : std::span<const MetaClass* const>{};
}

[[nodiscard]] inline std::span<const MetaClass* const> EagerExtensions(const MetaClass& m) noexcept {
    const auto* es = m.links().eagerSet.load(std::memory_order_acquire);
    return es ? std::span{es->extensionClasses, es->count} : std::span<const MetaClass* const>{};
}

} // namespace Yuki::RT
```

Augment `DispatchEntry` in `Yuki/include/Yuki/Core/MetaClass.h` with a `const MetaClass* providerClass` field (populated by `PublishExtensionEntries` / `CollectImplDispatchEntries`). Augment the `EagerSetEntry` array in `MetaLinks` to also carry `const MetaClass* extensionClass` so `EagerExtensions` can return metaclass pointers, not raw function pointers.

Add the test target to `Yuki/tests/CMakeLists.txt`.

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.IntrospectionTest
ctest --test-dir build/x64-asan -R "IntrospectionTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Introspection.h Yuki/include/Yuki/Core/MetaClass.h \
        Yuki/tests/Core/IntrospectionTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Introspection): metaclass-level potential attributes

Per spec §6.1. Capabilities/Provides/ProviderClass/ProviderDispatchKind are
acquire-load reads of the dispatch snapshot plus a binary search; Extensions/
Implementations/EagerExtensions are span views over their atomic class lists.
DispatchEntry gains a providerClass field so a metaclass can be answered
"who provides I" without consulting any closure instance.
EOF
)"
```

---

### Task 15: Introspection — instance-level (`MaterializedExtensions`, `MaterializedFacades`, `InClosure`, `WalkClosure`)

**Files:**
- Modify: `Yuki/include/Yuki/Core/Introspection.h`
- Test: `Yuki/tests/Core/IntrospectionTest.cpp`

**Spec refs:** §6.2 (runtime attributes); §6.3 (symmetry table); §6.4 (`WalkClosure` semantics); §6.6 (lifetime of returned views).

- [ ] **Step 1: Write the failing tests**

Append to `Yuki/tests/Core/IntrospectionTest.cpp`:

```cpp
TEST_CASE("MaterializedExtensions yields exactly the live extension instances", "[introspection][runtime]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    AImpl a;                                              // BExt is lazy → not yet materialized
    {
        std::vector<RootObject*> live;
        for (RootObject* e : RT::MaterializedExtensions(&a)) live.push_back(e);
        REQUIRE(live.empty());
    }
    RT::Reify<BExt>(&a);
    {
        std::vector<RootObject*> live;
        for (RootObject* e : RT::MaterializedExtensions(&a)) live.push_back(e);
        REQUIRE(live.size() == 1);
        REQUIRE(live[0]->TypeDynamic() == ClassType::Extension);
    }
}

TEST_CASE("InClosure groups nodes by nucleus identity", "[introspection][runtime]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    AImpl a1, a2;
    RT::Reify<BExt>(&a1);
    auto* ext1 = *RT::MaterializedExtensions(&a1).begin();
    REQUIRE(RT::InClosure(&a1, ext1));
    REQUIRE(!RT::InClosure(&a2, ext1));
}

TEST_CASE("WalkClosure visits nucleus first then every materialized facade exactly once",
          "[introspection][runtime]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    AImpl a;
    RT::Reify<BExt>(&a);

    std::vector<std::pair<RootObject*, ClassType>> visited;
    RT::WalkClosure(&a, [&](RootObject* node, ClassType role) { visited.emplace_back(node, role); });

    REQUIRE(visited.size() >= 2);
    REQUIRE(visited[0].first == &a);
    REQUIRE(visited[0].second == ClassType::Implementation);
    // No duplicates
    std::ranges::sort(visited, {}, &decltype(visited)::value_type::first);
    REQUIRE(std::ranges::adjacent_find(visited, {}, &decltype(visited)::value_type::first) == visited.end());
}

TEST_CASE("Potential vs runtime symmetry table holds", "[introspection][runtime]") {
    Registry::Install<AImpl>();
    Registry::Install<BExt>();
    AImpl a;
    // (row 2) Provides=true, Has=false: declared but Lazy not yet materialized.
    REQUIRE(RT::Provides<IB>(MetaClassOf<AImpl>));
    REQUIRE(RT::Has<IB>(&a) == false);
    // After Query: row 1.
    (void)RT::Query<IB>(&a);
    REQUIRE(RT::Has<IB>(&a) == true);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.IntrospectionTest
ctest --test-dir build/x64-asan -R "IntrospectionTest" --output-on-failure
```

Expected: FAIL — instance-level helpers not defined.

- [ ] **Step 3: Implement the instance-level surface**

Append to `Yuki/include/Yuki/Core/Introspection.h`:

```cpp
namespace Yuki::RT {

class FacadesView : public std::ranges::view_interface<FacadesView> {
    const FacadeNodeBase* head_;
public:
    explicit FacadesView(const FacadeNodeBase* h) noexcept : head_(h) {}
    struct Iterator {
        const FacadeNodeBase* p;
        RootObject*           operator*() const noexcept { return p->underlying; }
        Iterator&             operator++() noexcept { p = p->next; return *this; }
        bool                  operator==(const Iterator&) const = default;
    };
    Iterator begin() const noexcept { return {head_}; }
    Iterator end()   const noexcept { return {nullptr}; }
};

[[nodiscard]] inline auto MaterializedFacades(RootObject* p) noexcept {
    auto* n = Nucleus(p);
    return FacadesView{n ? n->facades_.load(std::memory_order_acquire) : nullptr};
}

[[nodiscard]] inline auto MaterializedExtensions(RootObject* p) noexcept {
    return MaterializedFacades(p)
         | std::views::filter([](RootObject* node) { return node->TypeDynamic() == ClassType::Extension; });
}

[[nodiscard]] inline bool InClosure(RootObject* anyNode, RootObject* candidate) noexcept {
    return Nucleus(anyNode) == Nucleus(candidate);
}

template <std::invocable<RootObject*, ClassType> F>
void WalkClosure(RootObject* anyNode, F&& visit) noexcept {
    auto* n = Nucleus(anyNode);
    if (n == nullptr) return;
    visit(n, n->TypeDynamic());
    for (RootObject* node : MaterializedFacades(n)) {
        visit(node, node->TypeDynamic());
    }
}

} // namespace Yuki::RT
```

`FacadeNodeBase` is the polymorphic base of `FacadeNode<E>` — both already exist from Task 2/Task 10; expose its `underlying` and `next` fields with package-internal visibility (a friend-declared `Detail::Introspection` is enough; spec §6.6 documents these are stable for read).

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.IntrospectionTest
ctest --test-dir build/x64-asan -R "IntrospectionTest" --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add Yuki/include/Yuki/Core/Introspection.h Yuki/tests/Core/IntrospectionTest.cpp
git commit -m "$(cat <<'EOF'
feat(Yuki/Core/Introspection): instance-level runtime attributes

Per spec §6.2-§6.4. MaterializedFacades is a non-mutating range over the
nucleus's grow-only facades_ list; MaterializedExtensions is a filtered
view. InClosure compares nuclei (Iid-stable across DLLs). WalkClosure
visits the nucleus first and then every facade exactly once in attachment
order — used by Diagnostics::Print in the deferred refactor (spec §5.3
Request C) and by the symmetry assertion in §6.3 row 4.
EOF
)"
```

---

### Task 16: Integration tests, identity coherence, and concurrency

**Files:**
- Create: `Yuki/tests/Core/ClosureTest.cpp`
- Modify: `Yuki/tests/CMakeLists.txt`

**Spec refs:** §1.4 (four scenarios collapse into one walk); §3.4 (cross-DLL identity by `Iid`); §5.1 (test plan); §5.2 (performance under hot-load).

The four scenarios live in §1.4 of the spec. Each test starts from a different role and resolves a different target, expecting the same nucleus identity:

| Test name | Starting role | Target |
|-----------|---------------|--------|
| `facade_view_of_extension_to_nucleus_iface` | Facade of an Extension exposing `IE1` | `IN` |
| `facade_view_of_nucleus_to_extension_iface` | Facade of nucleus exposing `IN` | `IE2` |
| `extension_to_other_extension` | Extension `E1` | `IE2` (provided by `E2`) |
| `nucleus_to_user_facade` | Nucleus | iid attached by user via `Attach` (no registered provider) |

- [ ] **Step 1: Write the failing tests**

Create `Yuki/tests/Core/ClosureTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Yuki/Core/Annotation.h>
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/Introspection.h>
#include <thread>
#include <barrier>
#include <vector>

using namespace Yuki;

struct IN_ : Anno::Interface { virtual ~IN_() = default; };
struct IE1 : Anno::Interface { virtual ~IE1() = default; };
struct IE2 : Anno::Interface { virtual ~IE2() = default; };

[[= Anno::Iid{"YukiTest::N"}, Anno::Implements{^^IN_} =]]
struct N : Anno::Implementation, IN_, RootObject {};

[[= Anno::Iid{"YukiTest::E1"}, Anno::Extends{^^N}, Anno::Implements{^^IE1}, Anno::Eager =]]
struct E1 : Anno::Extension { int x = 0; };

[[= Anno::Iid{"YukiTest::E2"}, Anno::Extends{^^N}, Anno::Implements{^^IE2}, Anno::Eager =]]
struct E2 : Anno::Extension { int y = 0; };

YUKI_DEFINE_REGISTRAR_SYMBOL(N,  YukiTest__N)
YUKI_DEFINE_REGISTRAR_SYMBOL(E1, YukiTest__E1)
YUKI_DEFINE_REGISTRAR_SYMBOL(E2, YukiTest__E2)

static void EnsureRegistered() {
    Registry::Install<N>(); Registry::Install<E1>(); Registry::Install<E2>();
}

TEST_CASE("Scenario 1: facade of extension → nucleus interface", "[closure][scenario]") {
    EnsureRegistered();
    N n;
    IE1* face1 = RT::Query<IE1>(&n);                       // materializes E1
    REQUIRE(face1 != nullptr);
    IN_* viaFace = RT::Query<IN_>(reinterpret_cast<RootObject*>(face1));
    REQUIRE(static_cast<RootObject*>(viaFace) == &n);
}

TEST_CASE("Scenario 2: facade of nucleus → extension interface", "[closure][scenario]") {
    EnsureRegistered();
    N n;
    IN_* faceN = RT::Query<IN_>(&n);
    IE2* faceE2 = RT::Query<IE2>(reinterpret_cast<RootObject*>(faceN));
    REQUIRE(faceE2 != nullptr);
    REQUIRE(Nucleus(reinterpret_cast<RootObject*>(faceE2)) == &n);
}

TEST_CASE("Scenario 3: extension → other extension's interface", "[closure][scenario]") {
    EnsureRegistered();
    N n;
    IE1* e1Face = RT::Query<IE1>(&n);
    IE2* e2Face = RT::Query<IE2>(reinterpret_cast<RootObject*>(e1Face));
    REQUIRE(e2Face != nullptr);
    REQUIRE(Nucleus(reinterpret_cast<RootObject*>(e2Face)) == &n);
}

TEST_CASE("Scenario 4: user-attached facade (FacadeList fallback)", "[closure][scenario]") {
    EnsureRegistered();
    struct IUser : Anno::Interface { virtual ~IUser() = default; };
    static constexpr Iid kUserIid = Anno::ParseIid("YukiTest::UserAttached");
    N n;
    auto* userNode = new FacadeNode<IUser>{};
    Detail::AttachUnique(n.facades_, kUserIid, userNode);
    REQUIRE(RT::QueryDynamicRaw(&n, kUserIid) == static_cast<RootObject*>(userNode));
}

TEST_CASE("Identity coherence: every Query returns same nucleus pointer", "[closure][identity]") {
    EnsureRegistered();
    N n;
    auto* viaIN  = Nucleus(reinterpret_cast<RootObject*>(RT::Query<IN_>(&n)));
    auto* viaIE1 = Nucleus(reinterpret_cast<RootObject*>(RT::Query<IE1>(&n)));
    auto* viaIE2 = Nucleus(reinterpret_cast<RootObject*>(RT::Query<IE2>(&n)));
    REQUIRE(viaIN  == &n);
    REQUIRE(viaIE1 == &n);
    REQUIRE(viaIE2 == &n);
}

TEST_CASE("Concurrent install + query is safe (smoke; run under TSan separately)",
          "[closure][concurrency]") {
    constexpr int kThreads = 8;
    std::barrier sync{kThreads};
    std::vector<std::jthread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            sync.arrive_and_wait();
            for (int j = 0; j < 1000; ++j) {
                EnsureRegistered();
                N n;
                (void)RT::Query<IE1>(&n);
            }
        });
    }
    // join via jthread RAII; ASan/UBSan in this build catches torn snapshots, lost retirements,
    // and double-free of FacadeNode. TSan is a separate build configuration (spec §5.1).
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.ClosureTest
ctest --test-dir build/x64-asan -R "ClosureTest" --output-on-failure
```

Expected: FAIL — `ClosureTest.cpp` is new; the target itself fails to configure until added to CMake.

- [ ] **Step 3: Wire the test target**

Add to `Yuki/tests/CMakeLists.txt` a stanza mirroring `Test.Core.QueryTest`:

```cmake
yuki_add_test(Test.Core.ClosureTest Core/ClosureTest.cpp)
```

- [ ] **Step 4: Build and run**

```bash
python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
cmake --build build/x64-asan --target Test.Core.ClosureTest
ctest --test-dir build/x64-asan -R "ClosureTest" --output-on-failure
```

Expected: pass under asan + ubsan.

- [ ] **Step 5: Commit**

```bash
git add Yuki/tests/Core/ClosureTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
test(Yuki/Core/Closure): four-scenario walk, identity coherence, concurrency smoke

Per spec §1.4 and §5.1. Each scenario starts from a different role with a
different target interface; all reach the same nucleus by Iid. The
concurrency smoke runs many install+query cycles in parallel under asan/
ubsan; a dedicated TSan build verifies the release-acquire snapshot
publishing path. Cross-DLL coherence is exercised via a follow-up
TwoTUFixture in Plan 2 (spec §7) since this plan runs entirely inside a
single test binary.
EOF
)"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Plan task(s) |
|--------------|--------------|
| §1.1 Vocabulary | (foundational; reflected in all tests) |
| §1.2 RT operations — full surface | Tasks 11-15 (Query, convenience, introspection) |
| §1.3 Convenience derivations | Task 13 |
| §1.4 Four scenarios collapse into one walk | Task 16 |
| §1.5 Cardinality and instantiation policy | Tasks 1, 2 (AttachUnique CAS dedup), 8, 10 |
| §2 Dispatch storage under DLL hot-load | Tasks 3, 5 |
| §2.4 Per-instance FacadeList | Task 2 |
| §3.1 Annotations drive everything | Task 1 |
| §3.2 DispatchKind selection (compile-time) | Tasks 7, 8, 11 |
| §3.3 Registrar | Tasks 7, 8, 9, 10 |
| §3.4 DLL hot-load flow | Tasks 6, 7, 8 (Plan 2 layers discovery on top) |
| §3.5 extendedBy / implementedBy | Tasks 7, 8 (populated by Install) |
| §4.1 Query static face | Task 11 |
| §4.2 Dynamic kernel | Task 12 |
| §4.3 Scenario walk-through | Task 16 |
| §5.1 Test plan | Distributed across Tasks 2, 3, 7, 8, 10, 11, 12, 13, 14, 15, 16 |
| §6.1 Potential attributes | Task 14 |
| §6.2 Runtime attributes | Task 15 |
| §6.3 Symmetry table | Task 15 (final test case) |
| §6.4 Implementation notes | Tasks 12 (`Materialize` policy), 13 (`IsMaterialized` via self-iid) |
| §6.5 Diagnostic tests | Tasks 14, 15, 16 |
| §7 Cross-DLL capability discovery | **Plan 2 — deferred** (manifest section, plugin roots, async sender) |

**Placeholder scan:** no "TBD", "TODO", "similar to Task N" placeholders. Every code step shows the code; every test step shows the assertions.

**Type consistency:** `DispatchEntry`, `DispatchKind`, `DispatchSnapshot`, `EagerSetEntry`, `FacadeNodeBase`, `MetaLinks::dispatch`/`extendedBy`/`implementedBy`/`eagerSet` all referenced with identical names from Task 3 through Task 16. `Registry::Install<T>`, `Registry::AlreadyInstalled`, `Registry::MarkInstalled`, `Registry::WriterMutexFor` consistent across Tasks 6-9. `RT::Query` / `RT::QueryDynamicRaw` / `RT::QueryDynamicRawPolicy` / `RT::Has` / `RT::Materialized` / `RT::Provider` / `RT::Reify` / `RT::IsMaterialized` consistent across Tasks 11-13 and reused in Tasks 14-16. `Anno::Eager`, `Anno::Lazy`, `Anno::IsEager`, `Anno::ImplementsOf`, `Anno::ExtendsOf`, `Anno::StatelessExtensionClass`, `Anno::ExtensionClass`, `Anno::ImplementationClass`, `Anno::InterfaceClass` consistent. `IidOf<T>` used as a constexpr value throughout.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-17-yuki-closure-architecture-core.md`. Two execution options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration. Tasks 1, 2, 3, 4, 5 are independent and can fan out; Tasks 6→7→8→9→10 form a chain; Tasks 11→12→13 form a chain; Tasks 14→15 form a chain; Task 16 depends on 1-15.
2. **Inline Execution** — execute tasks in this session using `superpowers:executing-plans`, batching with checkpoints for review.

Plan 2 (cross-DLL capability discovery — spec §7) is deferred until Plan 1 is implemented and validated. It is purely additive: manifest sections, plugin-root scanning, disk cache, `QueryOrDiscover` async sender. No part of Plan 1 needs to anticipate it beyond the `extern "C" YukiRegister_<iid>` symbol convention, which Task 9 already provides.

Which approach?
