# Yuki Object Model Y2 Core Implementation Plan — A2 Dispatch + Query

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land Y2 Core's dispatch + query infrastructure on top of A1. Build the three
dispatch arms (`InlineFacade`, `SideTableResolver`, `CodeExtensionSingleton`), the four-level
Query cache (L0 consteval / L1 fingerprint / L2 mergedDispatch / L3 invalidation), the
`Registry::Install<T>` cross-module seal checks (D7.2), eager extension chain ownership
(D11), and the user-facing `Query<I>(node)` entry point.

**Architecture:** Decisions D5 / D7.2 / D11 / D14 / D15 / D16 from spec
`2026-06-17-yuki-object-model-y2-design.md`. A1 (Tasks 1-12) has shipped; A3 (Tasks 21-23 —
introspection + integration) follows A2. This plan's task numbering continues from A1: T13
through T20.

**Tech Stack:** C++26 (clang-p2996 fork, LLVM 21.0.0git), MSVC ABI on Windows, libc++,
reflection (P2996) + annotations (P3394), `consteval`/`template for`/splices, Catch2 for
tests, CMake + Ninja, x64-asan build directory `build/x64-asan`.

**Environment activation (run before every build/compile):**
```
pwsh -NoProfile -Command "python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression; cd G:\Teaching\Vulkan; <cmd>"
```

**CMake test stanza:** `add_yuki_test(Core <NameTest>)` in `Yuki/tests/CMakeLists.txt`.
The `<NameTest>` argument is the **basename of the source file** (e.g.
`DispatchEntryTest` for `tests/Core/DispatchEntryTest.cpp`). The `Test` suffix is part of
the test name, not stripped.

**Branch policy:** stay on `main`. Do **not** create a topic branch — A1 already established
the convention of incremental commits on `main`. Never `--no-verify`, never amend after
review.

**Out of scope for this plan:** introspection surface (`IidsOf`/`Provides`/`Nucleus`/
`WalkClosure`/`MaterializedFacades`) and integration tests — those are **Plan A3**, written
after A2 lands. Also out of scope: cross-DLL discovery, plugin manifest, serialization (all
deferred to Plan B).

---

## File Structure

A2 extends `Yuki/include/Yuki/Core/` with the dispatch + query types and `Yuki/src/Core/`
with the out-of-line snapshot publish + invalidation kernels.

**Created (new files):**
- `Yuki/include/Yuki/Core/DispatchEntry.h` — `DispatchKind` enum, `DispatchEntry` POD, `DispatchSnapshot`
- `Yuki/include/Yuki/Core/MergedDispatch.h` — `MergedDispatchSnapshot` POD + `LookupMergedDispatch`
- `Yuki/src/Core/MergedDispatch.cpp` — out-of-line binary search
- `Yuki/include/Yuki/Core/FingerprintCache.h` — L1 4-slot ring + `Probe` / `Publish`
- `Yuki/include/Yuki/Core/QueryL0.h` — `IsBoaProvider` + `L0Shortcut`
- `Yuki/include/Yuki/Core/ExtendedList.h` — `ExtendedListSnapshot` + `ImplementedListSnapshot` PODs
- `Yuki/src/Core/Invalidation.cpp` — `BroadcastInvalidation` kernel
- `Yuki/include/Yuki/Core/Registry.h` — `Registry::Install<T>` template + runtime seal checks
- `Yuki/src/Core/Registry.cpp` — out-of-line writer mutex, snapshot publish, RCU retire
- `Yuki/include/Yuki/Core/EagerChain.h` — `EagerSetSnapshot` + park / hot-acquire primitives
- `Yuki/include/Yuki/Core/Query.h` — `Query<I>(node)` static face + `QueryDynamicRaw` kernel
- `Yuki/src/Core/Query.cpp` — out-of-line `QueryDynamicRaw`

**Modified:**
- `Yuki/include/Yuki/Core/MetaLinks.h` — replace `// L1 fingerprint cache slot lands in Task 15 (D15).` comment with `#include <Yuki/Core/FingerprintCache.h>` and a `FingerprintCache l1{};` member.

**Tests created (one file per public surface):**
- `Yuki/tests/Core/DispatchEntryTest.cpp`
- `Yuki/tests/Core/MergedDispatchTest.cpp`
- `Yuki/tests/Core/FingerprintCacheTest.cpp`
- `Yuki/tests/Core/QueryL0Test.cpp`
- `Yuki/tests/Core/InvalidationTest.cpp`
- `Yuki/tests/Core/RegistryTest.cpp`
- `Yuki/tests/Core/EagerChainTest.cpp`
- `Yuki/tests/Core/QueryTest.cpp`

---

## Task Decomposition Overview

Tasks are ordered so each one produces a working, testable artefact. T13 / T14 / T15 / T16
are mostly independent (each layer of the four-level cache); T17 produces the invalidation
kernel that T18 consumes; T19 (eager chain) and T20 (Query entry point) close the loop.

13. **`DispatchEntry` + `DispatchKind` + `DispatchSnapshot`** — Y2's three-arm dispatch POD
14. **L2 `mergedDispatch` flat snapshot + binary search + Important-wins tiebreak**
15. **L1 `FingerprintCache` 4-slot lock-free ring, wired into `MetaLinks`**
16. **L0 consteval `Query<I>` shortcut for BOA-provided interfaces**
17. **L3 invalidation: `ExtendedListSnapshot` + `BroadcastInvalidation` kernel**
18. **`Registry::Install<T>` with runtime seal checks (D7.2) + RCU publish**
19. **Eager extension chain ownership + deferred Acquire (D11)**
20. **`Query<I>(node)` static face + `QueryDynamicRaw` kernel — wires L0-L3 together**

The full task bodies follow.

---

### Task 13: `DispatchEntry` + `DispatchKind` + `DispatchSnapshot`

**Spec ref:** D14 (three dispatch arms), D7.3 (Important bit on the entry).

**Files:**
- Create: `Yuki/include/Yuki/Core/DispatchEntry.h`
- Test: `Yuki/tests/Core/DispatchEntryTest.cpp`

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/DispatchEntryTest.cpp`:
```cpp
#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/MetaCore.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <type_traits>

namespace M = Mashiro;
using namespace Yuki;

TEST_CASE("DispatchKind enumerates the three Y2 arms", AUTO_TAG) {
    static_assert(static_cast<int>(DispatchKind::InlineFacade)            == 0);
    static_assert(static_cast<int>(DispatchKind::SideTableResolver)       == 1);
    static_assert(static_cast<int>(DispatchKind::CodeExtensionSingleton)  == 2);
    static_assert(std::is_same_v<std::underlying_type_t<DispatchKind>, std::uint8_t>);
}

TEST_CASE("DispatchEntry stores iid / kind / seal-bits / arm", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    MetaCore probe{ .iid = i1, .name = "P", .role = ClassType::Implementation,
                    .implements = nullptr, .implementsCount = 0,
                    .extends = nullptr, .extendsCount = 0 };
    int armStorage = 0;
    DispatchEntry e{
        .iid           = i1,
        .kind          = DispatchKind::InlineFacade,
        .important     = true,
        .unique        = false,
        .final_        = false,
        .armOffset     = 0x40,
        .providerClass = &probe,
        .arm           = &armStorage,
    };
    REQUIRE(e.iid == i1);
    REQUIRE(e.kind == DispatchKind::InlineFacade);
    REQUIRE(e.important == true);
    REQUIRE(e.armOffset == 0x40);
    REQUIRE(e.providerClass == &probe);
    REQUIRE(e.arm == &armStorage);
}

TEST_CASE("DispatchSnapshot is a (count, entries) POD", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    DispatchEntry e{ .iid = i1, .kind = DispatchKind::SideTableResolver };
    DispatchSnapshot snap{ .count = 1, .entries = &e };
    REQUIRE(snap.count == 1);
    REQUIRE(snap.entries[0].iid == i1);
}
```

Wire: `add_yuki_test(Core DispatchEntryTest)`.

- [ ] **Step 2: Run, expect FAIL** (header doesn't exist).

- [ ] **Step 3: Create `DispatchEntry.h`**

```cpp
/**
 * @file DispatchEntry.h
 * @brief D14 — the three-arm dispatch entry + snapshot POD.
 *
 * One @ref DispatchEntry per (interface, providing class) pair. The @ref kind discriminates
 * between Y2's three storage arms; @ref arm is interpreted per-kind:
 *   - @c InlineFacade           — pointer to the inline facade subobject inside the impl frame
 *   - @c SideTableResolver      — pointer to a resolver function `RootObject*(*)(RootObject*)`
 *   - @c CodeExtensionSingleton — pointer to the singleton stateless-extension instance
 *
 * @c important / @c unique / @c final_ replicate the D7 seal bits onto the entry so dispatch
 * and the cross-module Install<E> check (D7.2) can read them without re-walking annotations.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Identity.h>

#include <cstddef>
#include <cstdint>

namespace Yuki {

    struct MetaCore;  // forward-decl avoids include cycle with MetaCore.h.

    /// @brief The three Y2 dispatch arms (D14).
    enum class DispatchKind : std::uint8_t {
        InlineFacade           = 0,
        SideTableResolver      = 1,
        CodeExtensionSingleton = 2,
    };

    /**
     * @brief One entry in a class's dispatch table (D14).
     *
     * Plain aggregate; lives in rodata when published as part of a @ref DispatchSnapshot.
     * Field order is stable — the binary search in @ref LookupMergedDispatch only touches
     * @ref iid and @ref important, so future fields appended at the tail are non-breaking.
     */
    struct DispatchEntry {
        Iid              iid{};
        DispatchKind     kind{DispatchKind::InlineFacade};
        bool             important{false};
        bool             unique{false};
        bool             final_{false};
        std::uint32_t    armOffset{0};
        const MetaCore*  providerClass{nullptr};
        void*            arm{nullptr};
    };

    /// @brief Iid-sorted (or insertion-order) view over a contiguous @ref DispatchEntry array.
    struct DispatchSnapshot {
        std::size_t          count{0};
        const DispatchEntry* entries{nullptr};
    };

} // namespace Yuki
```

- [ ] **Step 4: Re-run, expect PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/DispatchEntry.h Yuki/tests/Core/DispatchEntryTest.cpp \
        Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): DispatchEntry + DispatchKind + DispatchSnapshot (D14)

Three-arm dispatch POD: InlineFacade, SideTableResolver, CodeExtensionSingleton.
Important/unique/final seal bits replicated onto the entry so the cross-module
Install<E> check (D7.2) and Important-wins binary search (D7.3) read them
without re-walking annotations."
```

---

### Task 14: L2 `mergedDispatch` flat snapshot + binary search

**Spec ref:** D15 L2, D7.3 (Important-wins tiebreak).

**Files:**
- Create: `Yuki/include/Yuki/Core/MergedDispatch.h`
- Create: `Yuki/src/Core/MergedDispatch.cpp`
- Test: `Yuki/tests/Core/MergedDispatchTest.cpp`

`MergedDispatchSnapshot` is iid-sorted, flattened across the class + every inherited base
(D16 fans this out at Install time). Binary search is the L2 read path; the
Important-wins tiebreak applies when two entries share the same iid (this happens when an
Extension Importants over its nucleus's existing impl).

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/MergedDispatchTest.cpp`:
```cpp
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/DispatchEntry.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

namespace M = Mashiro;
using namespace Yuki;

TEST_CASE("LookupMergedDispatch returns nullptr on empty snapshot", AUTO_TAG) {
    MergedDispatchSnapshot snap{ .count = 0, .entries = nullptr };
    constexpr Iid i1{M::Uuid{1, 0}};
    REQUIRE(LookupMergedDispatch(&snap, i1) == nullptr);
}

TEST_CASE("LookupMergedDispatch hits an iid-sorted entry", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    constexpr Iid i2{M::Uuid{2, 0}};
    constexpr Iid i3{M::Uuid{3, 0}};
    DispatchEntry e[3] = {
        { .iid = i1, .kind = DispatchKind::InlineFacade },
        { .iid = i2, .kind = DispatchKind::SideTableResolver },
        { .iid = i3, .kind = DispatchKind::CodeExtensionSingleton },
    };
    MergedDispatchSnapshot snap{ .count = 3, .entries = e };
    REQUIRE(LookupMergedDispatch(&snap, i1) == &e[0]);
    REQUIRE(LookupMergedDispatch(&snap, i2) == &e[1]);
    REQUIRE(LookupMergedDispatch(&snap, i3) == &e[2]);
}

TEST_CASE("LookupMergedDispatch returns nullptr on miss", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    constexpr Iid i2{M::Uuid{2, 0}};
    constexpr Iid i9{M::Uuid{9, 0}};
    DispatchEntry e[2] = {
        { .iid = i1, .kind = DispatchKind::InlineFacade },
        { .iid = i2, .kind = DispatchKind::InlineFacade },
    };
    MergedDispatchSnapshot snap{ .count = 2, .entries = e };
    REQUIRE(LookupMergedDispatch(&snap, i9) == nullptr);
}

TEST_CASE("LookupMergedDispatch prefers Important on iid tie (D7.3)", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    // Two entries with the same iid; the second is Important. The function must
    // return the Important entry regardless of insertion order.
    DispatchEntry e[2] = {
        { .iid = i1, .kind = DispatchKind::InlineFacade,   .important = false },
        { .iid = i1, .kind = DispatchKind::SideTableResolver, .important = true },
    };
    MergedDispatchSnapshot snap{ .count = 2, .entries = e };
    auto* hit = LookupMergedDispatch(&snap, i1);
    REQUIRE(hit != nullptr);
    REQUIRE(hit->important == true);
    REQUIRE(hit->kind == DispatchKind::SideTableResolver);
}
```

Wire: `add_yuki_test(Core MergedDispatchTest)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `MergedDispatch.h`**

```cpp
/**
 * @file MergedDispatch.h
 * @brief D15 L2 — flattened, iid-sorted dispatch snapshot + binary-search lookup.
 *
 * The L2 read path of the four-level Query cache. @ref MergedDispatchSnapshot is published
 * by Task 18's `Registry::Install<T>` after flattening the class + every inherited base
 * (D16). Lookup is a branch-light binary search returning a pointer into the snapshot's
 * rodata-resident @ref DispatchEntry array, or @c nullptr on miss.
 *
 * Important-wins (D7.3) is resolved here, not at the call site: when two entries share an
 * iid (an Extension over a base impl, for instance), the Important entry is returned even
 * if its insertion order would otherwise lose. The binary search lands on *any* matching
 * entry; the function then linearly scans the equal-iid run for an Important entry.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>

#include <cstddef>

namespace Yuki {

    /// @brief Iid-sorted, flattened dispatch table for a class + its inherited bases (D16).
    struct MergedDispatchSnapshot {
        std::size_t          count{0};
        const DispatchEntry* entries{nullptr};
    };

    /**
     * @brief Binary-search a @ref MergedDispatchSnapshot for @p iid.
     *
     * Returns a pointer to the matching entry, or @c nullptr if not found. Resolves
     * Important-wins (D7.3) by linearly scanning the equal-iid run; the first Important
     * entry beats every non-Important entry sharing the same iid.
     *
     * @param snap Snapshot to search; may be @c nullptr or empty (returns @c nullptr).
     * @param iid  Interface identifier to look up.
     */
    [[nodiscard]] const DispatchEntry* LookupMergedDispatch(
        const MergedDispatchSnapshot* snap, Iid iid) noexcept;

} // namespace Yuki
```

- [ ] **Step 4: Create `Yuki/src/Core/MergedDispatch.cpp`**

```cpp
#include <Yuki/Core/MergedDispatch.h>

namespace Yuki {

    const DispatchEntry* LookupMergedDispatch(
            const MergedDispatchSnapshot* snap, Iid iid) noexcept {
        if (!snap || snap->count == 0 || snap->entries == nullptr) {
            return nullptr;
        }
        std::size_t lo = 0;
        std::size_t hi = snap->count;
        while (lo < hi) {
            std::size_t mid = lo + (hi - lo) / 2;
            const DispatchEntry& e = snap->entries[mid];
            if (e.iid < iid) {
                lo = mid + 1;
            } else if (iid < e.iid) {
                hi = mid;
            } else {
                // Walk left to the start of the equal-iid run, then scan for Important.
                std::size_t start = mid;
                while (start > 0 && snap->entries[start - 1].iid == iid) --start;
                std::size_t end = mid + 1;
                while (end < snap->count && snap->entries[end].iid == iid) ++end;
                for (std::size_t i = start; i < end; ++i) {
                    if (snap->entries[i].important) return &snap->entries[i];
                }
                return &snap->entries[start];
            }
        }
        return nullptr;
    }

} // namespace Yuki
```

Wire `MergedDispatch.cpp` into the `Yuki` target's source list (whichever CMake list builds
`libYuki` — A1 already added `Yuki/src/Core/RootObject.cpp`, follow the same pattern).

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/MergedDispatch.h Yuki/src/Core/MergedDispatch.cpp \
        Yuki/tests/Core/MergedDispatchTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): mergedDispatch L2 snapshot + binary search (D15 L2, D7.3)

Iid-sorted flat snapshot published by Install<T>. Binary-search lookup with an
equal-iid linear-scan tiebreak that lets an Important entry beat any
non-Important sharing the same iid (D7.3 Important wins)."
```

---

### Task 15: L1 `FingerprintCache` 4-slot ring, wired into `MetaLinks`

**Spec ref:** D15 L1.

**Files:**
- Create: `Yuki/include/Yuki/Core/FingerprintCache.h`
- Modify: `Yuki/include/Yuki/Core/MetaLinks.h` (replace the L1 placeholder comment)
- Test: `Yuki/tests/Core/FingerprintCacheTest.cpp`

Lock-free reader; writers Publish via round-robin. Negative hits are cached as
`entry == nullptr` paired with a non-zero @c iid match (the slot's iid is the lookup key;
"no entry, but we remember the iid" is the negative-hit shape). An epoch field on the
cache snapshots the witness `MetaLinks::cacheEpoch`; on probe, a mismatch with the live
epoch evicts the slot.

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/FingerprintCacheTest.cpp`:
```cpp
#include <Yuki/Core/FingerprintCache.h>
#include <Yuki/Core/DispatchEntry.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace M = Mashiro;
using namespace Yuki;

TEST_CASE("Probe on empty cache returns nullopt", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    REQUIRE(!Probe(c, i1, /*liveEpoch=*/0).has_value());
}

TEST_CASE("Publish then Probe is a positive hit", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    DispatchEntry e{ .iid = i1, .kind = DispatchKind::InlineFacade };
    Publish(c, i1, &e, /*epoch=*/7);
    auto hit = Probe(c, i1, /*liveEpoch=*/7);
    REQUIRE(hit.has_value());
    REQUIRE(*hit == &e);
}

TEST_CASE("Negative hit caches a miss", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    Publish(c, i1, /*entry=*/nullptr, /*epoch=*/3);
    auto hit = Probe(c, i1, /*liveEpoch=*/3);
    REQUIRE(hit.has_value());
    REQUIRE(*hit == nullptr);
}

TEST_CASE("Epoch mismatch invalidates a slot", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    DispatchEntry e{ .iid = i1, .kind = DispatchKind::InlineFacade };
    Publish(c, i1, &e, /*epoch=*/1);
    REQUIRE(!Probe(c, i1, /*liveEpoch=*/2).has_value());
}

TEST_CASE("Ring evicts old slots after 4 publishes", AUTO_TAG) {
    FingerprintCache c{};
    DispatchEntry e[5]{};
    for (int k = 0; k < 5; ++k) {
        e[k].iid = Iid{M::Uuid{static_cast<std::uint64_t>(k + 1), 0}};
        Publish(c, e[k].iid, &e[k], /*epoch=*/9);
    }
    // The first publish is guaranteed evicted by the 5th publish on a 4-slot ring.
    REQUIRE(!Probe(c, e[0].iid, /*liveEpoch=*/9).has_value());
    REQUIRE(Probe(c, e[4].iid, /*liveEpoch=*/9).has_value());
}
```

Wire: `add_yuki_test(Core FingerprintCacheTest)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `FingerprintCache.h`**

```cpp
/**
 * @file FingerprintCache.h
 * @brief D15 L1 — per-MetaLinks 4-slot lock-free fingerprint ring.
 *
 * Sits inside @ref MetaLinks. Caches recent positive *and* negative Query lookups so
 * repeated "does this closure provide @c I?" probes never reach L2. Lock-free reader
 * with @c memory_order_acquire on each slot; writer publishes via @c memory_order_release
 * and a round-robin index incremented with @c memory_order_relaxed.
 *
 * Slot encoding:
 *  - `iid == 0`  → empty slot, never a hit
 *  - `iid != 0`, `entry != nullptr` → positive hit
 *  - `iid != 0`, `entry == nullptr` → negative hit (we asked, the closure refused)
 *
 * Stale slots are evicted via the cache's @c witnessEpoch — the snapshot of
 * @ref MetaLinks::cacheEpoch at publish time. A probe with a higher live epoch evicts
 * the slot (treated as a miss, not a hit). Task 17's @ref BroadcastInvalidation bumps the
 * epoch on downstream classes after Install<E>; Task 18 bumps it on the installing class.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Identity.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Yuki {

    struct DispatchEntry;

    struct FingerprintSlot {
        std::atomic<Iid>                   iid{Iid{}};
        std::atomic<const DispatchEntry*>  entry{nullptr};
        std::atomic<std::uint64_t>         epoch{0};
    };

    struct FingerprintCache {
        static constexpr std::size_t kSlots = 4;
        std::atomic<std::uint64_t> witnessEpoch{0};
        FingerprintSlot            slots[kSlots]{};
        std::atomic<std::uint32_t> writeIdx{0};
    };

    /**
     * @brief Probe the cache for @p iid against @p liveEpoch.
     *
     * @return @c nullopt on miss; an optional containing the cached entry pointer on hit
     *         (the inner pointer may itself be @c nullptr for a cached negative hit).
     */
    [[nodiscard]] inline std::optional<const DispatchEntry*> Probe(
            FingerprintCache& c, Iid iid, std::uint64_t liveEpoch) noexcept {
        for (auto& s : c.slots) {
            Iid slotIid = s.iid.load(std::memory_order_acquire);
            if (slotIid == iid && !(slotIid == Iid{})) {
                std::uint64_t e = s.epoch.load(std::memory_order_acquire);
                if (e != liveEpoch) return std::nullopt;
                return s.entry.load(std::memory_order_acquire);
            }
        }
        return std::nullopt;
    }

    /// @brief Publish a (iid, entry, epoch) triple to a round-robin slot.
    inline void Publish(FingerprintCache& c, Iid iid,
                        const DispatchEntry* entry, std::uint64_t epoch) noexcept {
        std::uint32_t idx = c.writeIdx.fetch_add(1, std::memory_order_relaxed)
                          % FingerprintCache::kSlots;
        FingerprintSlot& s = c.slots[idx];
        s.epoch.store(epoch, std::memory_order_release);
        s.entry.store(entry, std::memory_order_release);
        s.iid.store(iid,     std::memory_order_release);
    }

} // namespace Yuki
```

- [ ] **Step 4: Wire `FingerprintCache` into `MetaLinks.h`**

In `Yuki/include/Yuki/Core/MetaLinks.h`, replace the line
```
        // L1 fingerprint cache slot lands in Task 15 (D15).
```
with the include (added at the top of the file, next to the other includes)
```cpp
#include <Yuki/Core/FingerprintCache.h>
```
and the member declaration (added inside `struct MetaLinks`, immediately after the
`cacheEpoch` line)
```cpp
        /// D15 L1 — per-class 4-slot lock-free fingerprint ring.
        FingerprintCache l1{};
```

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/FingerprintCache.h Yuki/include/Yuki/Core/MetaLinks.h \
        Yuki/tests/Core/FingerprintCacheTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): L1 4-slot fingerprint cache wired into MetaLinks (D15 L1)

Per-MetaLinks lock-free ring with positive + negative caching. Slot epoch is
snapshotted at Publish time and compared at Probe time against the live
MetaLinks::cacheEpoch; mismatch evicts the slot. Round-robin write index keeps
the implementation lock-free without per-slot CAS."
```

---

### Task 16: L0 consteval `Query<I>` shortcut

**Spec ref:** D15 L0, D14 (InlineFacade arm).

**Files:**
- Create: `Yuki/include/Yuki/Core/QueryL0.h`
- Test: `Yuki/tests/Core/QueryL0Test.cpp`

When `Impl` BOA-inherits an `Anno::Implementation` of `I` directly (i.e., I appears in
`MetaCore<Impl>::implements`), the dispatch arm is provably an InlineFacade and resolvable
at compile time. `IsBoaProvider<Impl, I>` returns true in that case; `L0Shortcut<Impl, I>`
returns a pointer to a function-local `static constexpr DispatchEntry` (program-lifetime,
constant-initialised) or `nullptr` if the L0 fast path doesn't apply.

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/QueryL0Test.cpp`:
```cpp
#include <Yuki/Core/QueryL0.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Interface]]      IFastA { virtual ~IFastA() = default; };
    struct [[=Anno::Interface]]      IFastB { virtual ~IFastB() = default; };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IFastA}]]
           FastImpl : RootObject, IFastA {
      public:
        Y_OBJECT;
        FastImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~FastImpl() override = default;
    };

    struct [[=Anno::Implementation]]  // no Implements annotation at all.
           LonelyImpl : RootObject {
      public:
        Y_OBJECT;
        LonelyImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~LonelyImpl() override = default;
    };
}

TEST_CASE("L0 positive: BOA provider is detected", AUTO_TAG) {
    static_assert(Detail::IsBoaProvider<FastImpl, IFastA>());
    constexpr const DispatchEntry* e = Detail::L0Shortcut<FastImpl, IFastA>();
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::InlineFacade);
    REQUIRE(e->iid == IidOf<IFastA>());
}

TEST_CASE("L0 negative: no implements annotation", AUTO_TAG) {
    static_assert(!Detail::IsBoaProvider<LonelyImpl, IFastA>());
    REQUIRE(Detail::L0Shortcut<LonelyImpl, IFastA>() == nullptr);
}

TEST_CASE("L0 negative: impl does not provide the wrong iface", AUTO_TAG) {
    static_assert(!Detail::IsBoaProvider<FastImpl, IFastB>());
    REQUIRE(Detail::L0Shortcut<FastImpl, IFastB>() == nullptr);
}
```

Wire: `add_yuki_test(Core QueryL0Test)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `QueryL0.h`**

```cpp
/**
 * @file QueryL0.h
 * @brief D15 L0 — consteval Query shortcut for BOA-provided interfaces.
 *
 * L0 is the cheapest layer of the four-level Query cache: when @c Impl's @c MetaCore
 * statically lists @c I in @c kImplementsArr (a BOA / implementation-via-inheritance
 * relationship), the dispatch arm is provably an @c InlineFacade and the entire Query
 * folds to a constant @c DispatchEntry* at compile time.
 *
 * @ref Detail::IsBoaProvider returns true on that L0-eligible case. @ref Detail::L0Shortcut
 * returns a pointer to a function-local `static constexpr DispatchEntry` (program lifetime,
 * constant-initialised) when L0 applies; otherwise @c nullptr.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaCore.h>

#include <type_traits>

namespace Yuki::Detail {

    /**
     * @brief @c true if @p Impl statically provides @p I via its @ref kImplementsArr.
     *
     * Walks @c kImplementsArr<Impl> at constant evaluation, comparing each iid against
     * @c IidOf<I>(). Returns @c false for @c Impl that is not an @c ImplementationClass
     * (Interfaces, Extensions, etc. take other arms).
     */
    template<class Impl, class I>
    consteval bool IsBoaProvider() {
        if constexpr (!ImplementationClass<Impl>) {
            return false;
        } else {
            for (std::size_t k = 0; k < kImplementsArr<Impl>.size(); ++k) {
                if (kImplementsArr<Impl>[k].iid == IidOf<I>()) return true;
            }
            return false;
        }
    }

    /**
     * @brief Return a program-lifetime @c DispatchEntry* for the (Impl, I) L0 fast path.
     *
     * @return Pointer to a function-local @c static @c constexpr entry when
     *         @ref IsBoaProvider returns true; @c nullptr otherwise.
     */
    template<class Impl, class I>
    constexpr const DispatchEntry* L0Shortcut() noexcept {
        if constexpr (!IsBoaProvider<Impl, I>()) {
            return nullptr;
        } else {
            static constexpr DispatchEntry kEntry{
                .iid           = IidOf<I>(),
                .kind          = DispatchKind::InlineFacade,
                .important     = false,
                .unique        = false,
                .final_        = false,
                .armOffset     = 0,
                .providerClass = &Impl::kMetaCore,
                .arm           = nullptr,  // resolved per-instance at Query call site.
            };
            return &kEntry;
        }
    }

} // namespace Yuki::Detail
```

- [ ] **Step 4: Re-run, expect PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/QueryL0.h Yuki/tests/Core/QueryL0Test.cpp \
        Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): L0 consteval Query shortcut (D15 L0)

When Impl statically lists I in kImplementsArr, IsBoaProvider<Impl, I> is true
and L0Shortcut returns a program-lifetime DispatchEntry* describing the
InlineFacade arm. Query<I>(node) (Task 20) folds the entire lookup to this
constant when L0 applies; the runtime caches never see those calls."
```

---

### Task 17: L3 invalidation — `ExtendedListSnapshot` + `BroadcastInvalidation`

**Spec ref:** D15 L3, D16 (extendedBy propagation), D18 (snapshot types).

**Files:**
- Create: `Yuki/include/Yuki/Core/ExtendedList.h`
- Create: `Yuki/src/Core/Invalidation.cpp`
- Test: `Yuki/tests/Core/InvalidationTest.cpp`

`ExtendedListSnapshot` is the rodata-published reverse edge: for a nucleus N, the list of
every downstream `MetaLinks*` whose closure includes N. `ImplementedListSnapshot` mirrors
the same shape for the implementedBy reverse edge. `BroadcastInvalidation` walks the list
and calls `MetaLinks::BumpCacheEpoch()` on each downstream — the kernel Task 18's
`Install<E>` calls after publishing a new dispatch snapshot.

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/InvalidationTest.cpp`:
```cpp
#include <Yuki/Core/ExtendedList.h>
#include <Yuki/Core/MetaLinks.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

TEST_CASE("BroadcastInvalidation bumps every downstream's cacheEpoch", AUTO_TAG) {
    MetaLinks downA{}, downB{}, downC{};
    const MetaLinks* downs[3] = { &downA, &downB, &downC };
    ExtendedListSnapshot snap{ .count = 3, .downstreams = downs };

    auto eA = downA.cacheEpoch.load();
    auto eB = downB.cacheEpoch.load();
    auto eC = downC.cacheEpoch.load();

    BroadcastInvalidation(&snap);

    REQUIRE(downA.cacheEpoch.load() == eA + 1);
    REQUIRE(downB.cacheEpoch.load() == eB + 1);
    REQUIRE(downC.cacheEpoch.load() == eC + 1);
}

TEST_CASE("BroadcastInvalidation on empty / nullptr snapshot is a no-op", AUTO_TAG) {
    BroadcastInvalidation(nullptr);
    ExtendedListSnapshot empty{ .count = 0, .downstreams = nullptr };
    BroadcastInvalidation(&empty);
    SUCCEED();
}

TEST_CASE("ImplementedListSnapshot mirrors ExtendedList shape", AUTO_TAG) {
    MetaLinks one{};
    const MetaLinks* arr[1] = { &one };
    ImplementedListSnapshot snap{ .count = 1, .providers = arr };
    REQUIRE(snap.count == 1);
    REQUIRE(snap.providers[0] == &one);
}
```

Wire: `add_yuki_test(Core InvalidationTest)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `ExtendedList.h`**

```cpp
/**
 * @file ExtendedList.h
 * @brief D15 L3 / D16 — reverse-edge snapshots + invalidation broadcast.
 *
 * @ref ExtendedListSnapshot is the published reverse edge of @c Anno::Extends — for a
 * nucleus N, the list of every downstream class's @ref MetaLinks whose closure includes N.
 * @ref ImplementedListSnapshot mirrors the shape for @c Anno::Implements. Both are
 * rodata-resident once published by @c Registry::Install<T> (Task 18); the L3 invalidation
 * kernel below walks the extendedBy list and bumps every downstream's cacheEpoch.
 *
 * @ingroup Core
 */
#pragma once

#include <cstddef>

namespace Yuki {

    struct MetaLinks;

    /// @brief Downstream MetaLinks that include this nucleus in their closure (D16).
    struct ExtendedListSnapshot {
        std::size_t              count{0};
        const MetaLinks* const*  downstreams{nullptr};
    };

    /// @brief Provider MetaLinks for a given interface — the implementedBy reverse edge.
    struct ImplementedListSnapshot {
        std::size_t              count{0};
        const MetaLinks* const*  providers{nullptr};
    };

    /// @brief Walk @p snap and call @c BumpCacheEpoch() on each downstream MetaLinks.
    ///
    /// No-op when @p snap is @c nullptr or has @c count == 0. The const-qualification on
    /// the snapshot is honest at the read layer — the kernel @c const_cast s downstream
    /// pointers to call @c BumpCacheEpoch (a non-const member), because the cacheEpoch
    /// store is logically a mutation on the link layer, not on the snapshot.
    void BroadcastInvalidation(const ExtendedListSnapshot* snap) noexcept;

} // namespace Yuki
```

- [ ] **Step 4: Create `Yuki/src/Core/Invalidation.cpp`**

```cpp
#include <Yuki/Core/ExtendedList.h>
#include <Yuki/Core/MetaLinks.h>

namespace Yuki {

    void BroadcastInvalidation(const ExtendedListSnapshot* snap) noexcept {
        if (!snap || snap->count == 0 || snap->downstreams == nullptr) return;
        for (std::size_t i = 0; i < snap->count; ++i) {
            const MetaLinks* down = snap->downstreams[i];
            if (!down) continue;
            const_cast<MetaLinks*>(down)->BumpCacheEpoch();
        }
    }

} // namespace Yuki
```

Wire `Invalidation.cpp` into the `Yuki` target's source list.

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/ExtendedList.h Yuki/src/Core/Invalidation.cpp \
        Yuki/tests/Core/InvalidationTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): L3 invalidation broadcast over extendedBy (D15 L3, D16)

ExtendedListSnapshot publishes the reverse edge of Anno::Extends as a flat
(MetaLinks* const*) list. BroadcastInvalidation walks it and bumps every
downstream's cacheEpoch — the Install<E> hook that turns L1 fingerprint hits
on stale entries into L1 misses on the next probe."
```

---

### Task 18: `Registry::Install<T>` with runtime seal checks (D7.2)

**Spec ref:** D7.2 (cross-module seal check), D15 L3 (broadcast after publish),
D16 (mergedDispatch regeneration).

**Files:**
- Create: `Yuki/include/Yuki/Core/Registry.h`
- Create: `Yuki/src/Core/Registry.cpp`
- Test: `Yuki/tests/Core/RegistryTest.cpp`

`Registry::Install<T>` is the runtime entry point that materialises a class's dispatch
table. It walks `kImplementsArr<T>`, runs the cross-module seal checks against the live
`MetaLinks::dispatch` snapshot, builds a new sorted `DispatchSnapshot` + flattened
`MergedDispatchSnapshot`, atomic-publishes them, bumps the local cacheEpoch, and broadcasts
invalidation downstream.

RCU retirement of the old snapshot is approximated for A2 by an in-place deferred-delete
pool (a `std::vector<std::unique_ptr<DispatchEntry[]>>` guarded by the per-nucleus mutex).
Full epoch-RCU is deferred to A3.

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/RegistryTest.cpp`:
```cpp
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Interface]] IRegA { virtual ~IRegA() = default; };
    struct [[=Anno::Interface]] IRegB { virtual ~IRegB() = default; };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IRegA}]]
           PlainImpl : RootObject, IRegA {
      public:
        Y_OBJECT;
        PlainImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~PlainImpl() override = default;
    };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IRegB}]]
           [[=Anno::Final{^^IRegB}]]
           FinalImpl : RootObject, IRegB {
      public:
        Y_OBJECT;
        FinalImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~FinalImpl() override = default;
    };
}

TEST_CASE("Install<T> publishes a DispatchSnapshot the class can read", AUTO_TAG) {
    Registry::Install<PlainImpl>();
    const auto* snap = PlainImpl::Meta().links->dispatch.load(std::memory_order_acquire);
    REQUIRE(snap != nullptr);
    REQUIRE(snap->count >= 1);
    REQUIRE(snap->entries[0].iid == IidOf<IRegA>());
}

TEST_CASE("Install<T> publishes a MergedDispatchSnapshot reachable via L2", AUTO_TAG) {
    Registry::Install<PlainImpl>();
    const auto* merged =
        PlainImpl::Meta().links->mergedDispatch.load(std::memory_order_acquire);
    REQUIRE(merged != nullptr);
    REQUIRE(LookupMergedDispatch(merged, IidOf<IRegA>()) != nullptr);
}

TEST_CASE("Install<T> bumps cacheEpoch", AUTO_TAG) {
    auto eBefore = FinalImpl::Meta().links->cacheEpoch.load();
    Registry::Install<FinalImpl>();
    auto eAfter = FinalImpl::Meta().links->cacheEpoch.load();
    REQUIRE(eAfter > eBefore);
}

TEST_CASE("Install<T> is idempotent on the same class", AUTO_TAG) {
    // Second call publishes a new snapshot but does NOT abort — the seal check sees
    // "prior entry from the same providerClass" and accepts it as a re-install.
    Registry::Install<PlainImpl>();
    Registry::Install<PlainImpl>();
    SUCCEED();
}
```

Wire: `add_yuki_test(Core RegistryTest)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `Registry.h`**

```cpp
/**
 * @file Registry.h
 * @brief D7.2 / D15 L3 / D16 — runtime install entry point with cross-module seal checks.
 *
 * `Registry::Install<T>()` materialises a class's dispatch table: walks
 * `kImplementsArr<T>`, runs the D7.2 seal checks against the live snapshot, builds a new
 * sorted DispatchSnapshot + flattened MergedDispatchSnapshot, atomic-publishes them,
 * bumps the local cacheEpoch, and broadcasts invalidation to downstream classes.
 *
 * Per-nucleus writer mutex serialises concurrent Install calls on the same class;
 * readers run lock-free against the atomic snapshot pointers in MetaLinks.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaLinks.h>

namespace Yuki {

    /** @cond INTERNAL */
    namespace Detail {
        /// @brief Out-of-line installer kernel shared by every Install<T> instantiation.
        ///
        /// Takes a (MetaCore*, MetaLinks*) pair — i.e., a type-erased MetaDynamic — so the
        /// kernel does not have to be templated. The kernel runs the seal checks, builds
        /// new snapshots from kImplementsArr<T> (passed in by pointer + length), publishes,
        /// and broadcasts.
        void InstallKernel(const MetaCore* core, MetaLinks* links,
                           const ImplementsInfo* implements, std::size_t implementsCount) noexcept;
    } // namespace Detail
    /** @endcond */

    struct Registry {
        /// @brief Install class @p T into the runtime registry (D7.2 / D15 L3 / D16).
        ///
        /// Idempotent on the same class — a second call publishes a fresh snapshot but
        /// the seal check accepts the prior entry whose @c providerClass equals
        /// @c &T::kMetaCore as a re-install rather than a conflict.
        template<class T>
        static void Install() noexcept {
            Detail::InstallKernel(
                &T::kMetaCore,
                &Detail::gLinksFor<T>,
                kImplementsArr<T>.data(),
                kImplementsArr<T>.size());
        }
    };

} // namespace Yuki
```

- [ ] **Step 4: Create `Yuki/src/Core/Registry.cpp`**

```cpp
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/ExtendedList.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Yuki::Detail {

    namespace {
        // Per-nucleus writer mutex. Keyed by MetaLinks* — one entry per Y_OBJECT class.
        std::mutex& MutexFor(MetaLinks* links) {
            static std::mutex tableMutex;
            static std::unordered_map<MetaLinks*, std::unique_ptr<std::mutex>> table;
            std::lock_guard<std::mutex> g(tableMutex);
            auto it = table.find(links);
            if (it == table.end()) {
                auto [ins, _] = table.emplace(links, std::make_unique<std::mutex>());
                return *ins->second;
            }
            return *it->second;
        }

        // Deferred-delete pool keeping retired snapshots alive until Install returns.
        // Full epoch-RCU is deferred to A3; for A2 the pool grows for the program lifetime
        // (snapshots are tiny, churn is bounded by the number of classes).
        struct RetiredPool {
            std::vector<std::unique_ptr<DispatchEntry[]>>        entries;
            std::vector<std::unique_ptr<DispatchSnapshot>>       dispatchSnaps;
            std::vector<std::unique_ptr<MergedDispatchSnapshot>> mergedSnaps;
        };
        RetiredPool& Pool() {
            static RetiredPool p;
            return p;
        }
        std::mutex& PoolMutex() {
            static std::mutex m;
            return m;
        }
    } // namespace

    void InstallKernel(const MetaCore* core, MetaLinks* links,
                       const ImplementsInfo* implements, std::size_t implementsCount) noexcept {
        std::lock_guard<std::mutex> g(MutexFor(links));

        // 1) Read the live snapshot (may be null on first install).
        const DispatchSnapshot* prior = links->dispatch.load(std::memory_order_acquire);

        // 2) Per-info seal check (D7.2). On violation, abort with a diagnostic.
        if (prior) {
            for (std::size_t k = 0; k < implementsCount; ++k) {
                const ImplementsInfo& info = implements[k];
                const DispatchEntry* match = nullptr;
                for (std::size_t j = 0; j < prior->count; ++j) {
                    if (prior->entries[j].iid == info.iid) { match = &prior->entries[j]; break; }
                }
                if (!match) continue;
                // Re-install from the same providerClass is benign.
                if (match->providerClass == core) continue;
                // Final on the prior — derived class may not re-implement.
                if (match->final_) std::abort();
                // Unique on this install — at most one provider per closure.
                if (info.flags.unique) std::abort();
                // Important on both — fatal cross-module conflict (D7.2).
                if (match->important && info.flags.important) std::abort();
            }
        }

        // 3) Build a fresh iid-sorted entry array (own count + carry-over of prior entries
        //    whose iid is not shadowed by an Important-wins from this install).
        std::vector<DispatchEntry> built;
        built.reserve(implementsCount + (prior ? prior->count : 0));
        for (std::size_t k = 0; k < implementsCount; ++k) {
            const ImplementsInfo& info = implements[k];
            built.push_back(DispatchEntry{
                .iid           = info.iid,
                .kind          = DispatchKind::InlineFacade,  // refined per-arm in A3.
                .important     = info.flags.important,
                .unique        = info.flags.unique,
                .final_        = info.flags.final,
                .armOffset     = 0,
                .providerClass = core,
                .arm           = nullptr,
            });
        }
        if (prior) {
            for (std::size_t j = 0; j < prior->count; ++j) {
                if (prior->entries[j].providerClass == core) continue;  // replaced.
                built.push_back(prior->entries[j]);
            }
        }
        std::sort(built.begin(), built.end(),
                  [](const DispatchEntry& a, const DispatchEntry& b) { return a.iid < b.iid; });

        // 4) Move the built vector into program-lifetime storage.
        const std::size_t n = built.size();
        auto entries = std::make_unique<DispatchEntry[]>(n);
        for (std::size_t i = 0; i < n; ++i) entries[i] = built[i];

        auto snap = std::make_unique<DispatchSnapshot>();
        snap->count = n;
        snap->entries = entries.get();

        // 5) MergedDispatchSnapshot — for A2 we publish the same array as the merged view
        //    (full base-chain flattening per D16 lands when A3 adds the base walker).
        auto merged = std::make_unique<MergedDispatchSnapshot>();
        merged->count = n;
        merged->entries = entries.get();

        // 6) Publish under release ordering.
        const DispatchSnapshot*       newDispatch = snap.get();
        const MergedDispatchSnapshot* newMerged   = merged.get();
        {
            std::lock_guard<std::mutex> p(PoolMutex());
            Pool().entries.push_back(std::move(entries));
            Pool().dispatchSnaps.push_back(std::move(snap));
            Pool().mergedSnaps.push_back(std::move(merged));
        }
        links->dispatch.store(newDispatch, std::memory_order_release);
        links->mergedDispatch.store(newMerged, std::memory_order_release);

        // 7) Bump epoch and broadcast invalidation.
        links->BumpCacheEpoch();
        const ExtendedListSnapshot* downs =
            links->extendedBy.load(std::memory_order_acquire);
        BroadcastInvalidation(downs);

        (void)core;  // reserved for the A3 diagnostic format.
    }

} // namespace Yuki::Detail
```

Wire `Registry.cpp` into the `Yuki` target's source list.

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/Registry.h Yuki/src/Core/Registry.cpp \
        Yuki/tests/Core/RegistryTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): Registry::Install<T> with cross-module seal checks (D7.2)

Per-nucleus writer mutex serialises Install on a single class; readers stay
lock-free against the atomic snapshot pointers in MetaLinks. Builds a sorted
DispatchSnapshot + MergedDispatchSnapshot from kImplementsArr<T>, runs the
Final / Unique / Important cross-module check (D7.2) against the prior live
snapshot, publishes under release ordering, bumps cacheEpoch, broadcasts
invalidation downstream (D15 L3). Full base-chain flattening per D16 and
epoch-RCU snapshot retirement land in A3; A2 uses an in-place deferred-delete
pool that grows once per Install call."
```

---

### Task 19: Eager extension chain ownership + deferred Acquire

**Spec ref:** D11 (Eager defers upstream Acquire), D14 (CodeExtensionSingleton arm).

**Files:**
- Create: `Yuki/include/Yuki/Core/EagerChain.h`
- Create: `Yuki/src/Core/EagerChain.cpp`
- Test: `Yuki/tests/Core/EagerChainTest.cpp`

`EagerSetSnapshot` is the published list of parked eager extensions for a nucleus. A
parked extension carries `refcount = 0` in its TaggedPayload — the chain owns it by raw
pointer, not via refcount, so its mere existence never pins the nucleus. The first time
user code obtains a `ComPtr` to a parked eager via Query, `HotAcquireEager` bumps the
extension's refcount to 1 and `Acquire`s the extendee. When the user-side last `Release`
runs the extension back to refcount 0, `DetachEagerOnRelease` un-Acquires the extendee but
keeps the extension parked for future re-Acquire; final teardown happens in the nucleus
dtor (out of A2 scope — A3 adds the chain walker).

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/EagerChainTest.cpp`:
```cpp
#include <Yuki/Core/EagerChain.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct ExtendeeImpl : RootObject {
      public:
        Y_OBJECT;
        ExtendeeImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~ExtendeeImpl() override = default;
    };

    struct EagerExt : RootObject {
      public:
        Y_OBJECT;
        EagerExt() : RootObject(ClassType::Extension, nullptr, /*external=*/false) {}
        ~EagerExt() override = default;
    };
}

TEST_CASE("ParkEager drops refcount to 0", AUTO_TAG) {
    auto* ext = new EagerExt();   // refcount = 1 after ctor.
    REQUIRE(ext->PayloadRelaxed().refcount() == 1);
    ParkEager(ext);
    REQUIRE(ext->PayloadRelaxed().refcount() == 0);
    delete ext;
}

TEST_CASE("HotAcquireEager bumps ext to 1 and pins the extendee", AUTO_TAG) {
    auto* extendee = new ExtendeeImpl();
    auto* ext = new EagerExt();
    ParkEager(ext);
    REQUIRE(ext->PayloadRelaxed().refcount() == 0);
    auto extendeeBefore = extendee->PayloadRelaxed().refcount();

    HotAcquireEager(ext, extendee);
    REQUIRE(ext->PayloadRelaxed().refcount() == 1);
    REQUIRE(extendee->PayloadRelaxed().refcount() == extendeeBefore + 1);

    delete ext;
    delete extendee;
}

TEST_CASE("DetachEagerOnRelease un-Acquires extendee + re-parks ext", AUTO_TAG) {
    auto* extendee = new ExtendeeImpl();
    auto* ext = new EagerExt();
    ParkEager(ext);
    HotAcquireEager(ext, extendee);
    auto extendeeHot = extendee->PayloadRelaxed().refcount();

    DetachEagerOnRelease(ext, extendee);
    REQUIRE(ext->PayloadRelaxed().refcount() == 0);
    REQUIRE(extendee->PayloadRelaxed().refcount() == extendeeHot - 1);

    delete ext;
    delete extendee;
}

TEST_CASE("EagerSetSnapshot is a (count, parked) POD", AUTO_TAG) {
    auto* ext = new EagerExt();
    ParkEager(ext);
    RootObject* arr[1] = { ext };
    EagerSetSnapshot snap{ .count = 1, .parked = arr };
    REQUIRE(snap.count == 1);
    REQUIRE(snap.parked[0] == ext);
    delete ext;
}
```

Wire: `add_yuki_test(Core EagerChainTest)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `EagerChain.h`**

```cpp
/**
 * @file EagerChain.h
 * @brief D11 — eager extension chain ownership + deferred Acquire.
 *
 * Eager extensions are constructed alongside their nucleus but cannot Acquire the
 * extendee at ctor time without breaking the hierarchical invariant: the nucleus's
 * own ctor has not finished, the user's MakeOwned has not adopted the +1 yet, and a
 * cycle would emerge. Y2's resolution: park the extension in the nucleus's eager set
 * with @c refcount = 0. The chain owns parked extensions by raw pointer.
 *
 * The first time user code obtains a @c ComPtr to a parked eager (via Query),
 * @ref HotAcquireEager bumps the extension's refcount to 1 and Acquires the extendee.
 * The user-side last Release lands the extension back at refcount 0;
 * @ref DetachEagerOnRelease un-Acquires the extendee and re-parks. Final teardown of
 * parked extensions happens in the nucleus dtor (A3).
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/RootObject.h>

#include <cstddef>

namespace Yuki {

    /// @brief Published list of parked eager extensions for a nucleus.
    struct EagerSetSnapshot {
        std::size_t       count{0};
        RootObject* const* parked{nullptr};
    };

    /// @brief Force @p ext's TaggedPayload refcount down to 0 (parked state).
    ///
    /// Called by the eager-chain installer after constructing the extension. @p ext must
    /// have @c refcount == 1 on entry (the post-ctor count from RootObject(external=false)).
    void ParkEager(RootObject* ext) noexcept;

    /// @brief Bump a parked eager's refcount to 1 and Acquire @p extendee.
    ///
    /// Called the first time Query hands out a @c ComPtr to a parked eager.
    /// Precondition: @p ext is currently parked (refcount == 0). On return:
    ///  - @p ext has refcount == 1
    ///  - @p extendee has been Acquired by exactly one ref on behalf of @p ext.
    void HotAcquireEager(RootObject* ext, RootObject* extendee) noexcept;

    /// @brief Re-park @p ext and Release @p extendee.
    ///
    /// Called by the user-side ComPtr last-Release path when an eager's refcount
    /// transitions to 0. Does NOT delete the extension — the chain still owns it.
    void DetachEagerOnRelease(RootObject* ext, RootObject* extendee) noexcept;

} // namespace Yuki
```

- [ ] **Step 4: Create `Yuki/src/Core/EagerChain.cpp`**

```cpp
#include <Yuki/Core/EagerChain.h>
#include <Yuki/Core/TaggedPayload.h>

namespace Yuki {

    namespace {
        // Force the refcount field of a TaggedPayload word to a specific value while
        // preserving role/everAcquired/armPtr bits.
        void StoreRefcount(std::atomic<TaggedPayload>& a, std::uint16_t rc) noexcept {
            for (auto cur = a.load(std::memory_order_relaxed);;) {
                TaggedPayload nxt = cur;
                nxt.word = (cur.word & ~(0xFFFFull << 4))
                         | ((std::uint64_t(rc) & 0xFFFF) << 4);
                if (a.compare_exchange_weak(cur, nxt,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) return;
            }
        }
    } // namespace

    void ParkEager(RootObject* ext) noexcept {
        if (!ext) return;
        StoreRefcount(ext->MetaWord(), 0);
    }

    void HotAcquireEager(RootObject* ext, RootObject* extendee) noexcept {
        if (!ext) return;
        StoreRefcount(ext->MetaWord(), 1);
        Acquire(extendee);
    }

    void DetachEagerOnRelease(RootObject* ext, RootObject* extendee) noexcept {
        if (!ext) return;
        StoreRefcount(ext->MetaWord(), 0);
        (void)Release(extendee);
    }

} // namespace Yuki
```

Wire `EagerChain.cpp` into the `Yuki` target's source list.

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/EagerChain.h Yuki/src/Core/EagerChain.cpp \
        Yuki/tests/Core/EagerChainTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): eager extension chain ownership + deferred Acquire (D11)

ParkEager drops a freshly-constructed eager to refcount=0; the chain owns it by
raw pointer. HotAcquireEager bumps to refcount=1 and Acquires the extendee on
first user pickup. DetachEagerOnRelease re-parks on user-side last Release.
Final teardown of parked extensions lands in A3's nucleus dtor chain walker."
```

---

### Task 20: `Query<I>(node)` static face + `QueryDynamicRaw` kernel

**Spec ref:** D14 (three arms), D15 (four-level cache), D5 (Interface is RootObject subclass).

**Files:**
- Create: `Yuki/include/Yuki/Core/Query.h`
- Create: `Yuki/src/Core/Query.cpp`
- Test: `Yuki/tests/Core/QueryTest.cpp`

`Query<I>(node)` is the user-facing entry point. It folds the four-level cache together:
L0 consteval shortcut (Task 16), L1 fingerprint probe (Task 15), L2 binary search via
`QueryDynamicRaw` (Task 14), L3 invalidation is consumed via the epoch passed to
`Probe`/`Publish`. Per-arm result production:
 - `InlineFacade`           → `ComPtr<I>(static_cast<I*>(node))` (BOA path; node *is* an I)
 - `SideTableResolver`      → invoke the resolver, wrap result
 - `CodeExtensionSingleton` → call `HotAcquireEager` (Task 19) and wrap the parked instance

A2 wires the InlineFacade arm end-to-end; the other two arms are stubbed (return a null
`ComPtr<I>`) with a comment pointing at A3 for the full materialisation.

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/QueryTest.cpp`:
```cpp
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include <Yuki/Core/MakeOwned.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    // D5: Interface is a RootObject subclass. Single-base chain (IQA→RootObject,
    // QImpl→IQA) avoids the diamond and matches A1's static_assert(sizeof(RootObject)
    // == 2*sizeof(void*)) without forcing virtual inheritance overhead. IQA's ctor
    // forwards a role+external arg through so QImpl can set its own role at
    // construction without re-declaring RootObject as a direct base.
    struct [[=Anno::Interface]] IQA : RootObject {
        IQA(ClassType role, bool external)
            : RootObject(role, nullptr, external) {}
        ~IQA() override = default;
        virtual int Answer() const = 0;
    };
    struct [[=Anno::Interface]] IQB : RootObject {
        IQB(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IQB() override = default;
    };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IQA}]]
           QImpl : IQA {
      public:
        Y_OBJECT;
        QImpl() : IQA(ClassType::Implementation, /*external=*/false) {}
        ~QImpl() override = default;
        int Answer() const override { return 42; }
    };
}

TEST_CASE("Query<I>(node) takes the L0 fast path for a BOA provider", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    ComPtr<IQA> a = Query<IQA>(impl.Get());
    REQUIRE(a);
    REQUIRE(a->Answer() == 42);
}

TEST_CASE("Query<I>(node) misses for an unimplemented interface", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    ComPtr<IQB> b = Query<IQB>(impl.Get());
    REQUIRE(!b);
}

TEST_CASE("QueryDynamicRaw hits via L2 binary search", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    const DispatchEntry* e = QueryDynamicRaw(impl->Meta().links, IidOf<IQA>());
    REQUIRE(e != nullptr);
    REQUIRE(e->iid == IidOf<IQA>());
}

TEST_CASE("ComPtr lifetime through Query keeps the node alive", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    auto before = impl->PayloadRelaxed().refcount();
    {
        ComPtr<IQA> a = Query<IQA>(impl.Get());
        REQUIRE(impl->PayloadRelaxed().refcount() == before + 1);
    }
    REQUIRE(impl->PayloadRelaxed().refcount() == before);
}
```

Wire: `add_yuki_test(Core QueryTest)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `Query.h`**

```cpp
/**
 * @file Query.h
 * @brief D14 / D15 — Query<I>(node) static face + the QueryDynamicRaw L2 kernel.
 *
 * The user-facing entry point of the four-level cache. The static face is templated on
 * the target interface so the L0 consteval shortcut (@ref Detail::L0Shortcut) can resolve
 * against `kImplementsArr<Impl>` whenever the impl type is statically known; on a miss,
 * the call falls through to the runtime kernel that probes L1, then L2, then optionally
 * publishes the result back into L1.
 *
 * Per-arm result production (D14):
 *  - @c InlineFacade           — @p node is itself an @c I subobject (D5); static_cast wraps it.
 *  - @c SideTableResolver      — stubbed in A2; resolver invocation lands in A3.
 *  - @c CodeExtensionSingleton — stubbed in A2; HotAcquireEager wiring lands in A3.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/FingerprintCache.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaLinks.h>
#include <Yuki/Core/QueryL0.h>
#include <Yuki/Core/RootObject.h>

namespace Yuki {

    /// @brief Runtime kernel: probe L1, then L2; publish L2 result into L1; return entry or null.
    [[nodiscard]] const DispatchEntry* QueryDynamicRaw(MetaLinks* links, Iid iid) noexcept;

    /**
     * @brief Resolve a @c ComPtr<I> for @p node by walking the four-level cache.
     *
     * Templated on the static type @c T of @p node so L0 can probe @c kImplementsArr<T>
     * and Meta().links is reachable without a virtual hook. (The type-erased
     * @c Query<I>(RootObject*) entry point — needed for true closure-walking from a
     * generic node — lands in A3 once Y_OBJECT exposes a virtual MetaLinks accessor.)
     */
    template<class I, class T>
    [[nodiscard]] ComPtr<I> Query(T* node) noexcept {
        if (!node) return {};
        if constexpr (Detail::IsBoaProvider<T, I>()) {
            // L0 fast path: I is statically known to be a base subobject of T.
            return ComPtr<I>(static_cast<I*>(node));
        } else {
            MetaLinks* links = T::Meta().links;
            const DispatchEntry* e = QueryDynamicRaw(links, IidOf<I>());
            if (!e) return {};
            switch (e->kind) {
                case DispatchKind::InlineFacade:
                    return ComPtr<I>(static_cast<I*>(node));
                case DispatchKind::SideTableResolver:
                    // A3 plumbs the resolver invocation; A2 stubs to null.
                    return {};
                case DispatchKind::CodeExtensionSingleton:
                    // A3 routes through HotAcquireEager; A2 stubs to null.
                    return {};
            }
            return {};
        }
    }

} // namespace Yuki
```

- [ ] **Step 4: Create `Yuki/src/Core/Query.cpp`**

```cpp
#include <Yuki/Core/Query.h>

namespace Yuki {

    const DispatchEntry* QueryDynamicRaw(MetaLinks* links, Iid iid) noexcept {
        if (!links) return nullptr;
        const std::uint64_t epoch = links->cacheEpoch.load(std::memory_order_acquire);

        // L1 probe.
        if (auto hit = Probe(links->l1, iid, epoch)) {
            return *hit;  // may be nullptr — a cached negative hit.
        }

        // L2 binary search.
        const MergedDispatchSnapshot* merged =
            links->mergedDispatch.load(std::memory_order_acquire);
        const DispatchEntry* e = LookupMergedDispatch(merged, iid);

        // Publish the (possibly null) result back into L1 with the witnessed epoch.
        Publish(links->l1, iid, e, epoch);
        return e;
    }

} // namespace Yuki
```

Wire `Query.cpp` into the `Yuki` target's source list.

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/Query.h Yuki/src/Core/Query.cpp \
        Yuki/tests/Core/QueryTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): Query<I>(node) + QueryDynamicRaw kernel (D14, D15)

Static face folds the four-level cache: L0 consteval IsBoaProvider check on the
Impl-typed overload; runtime kernel probes L1 against the live cacheEpoch then
falls through to LookupMergedDispatch (L2) and publishes the result (positive or
negative) back into L1. InlineFacade arm wired end-to-end; SideTableResolver
and CodeExtensionSingleton stubbed for A3 with comments at the switch cases."
```

---

## Tasks 21-23 (deferred to plan A3)

| # | Task | Plan |
|---|------|------|
| 21 | Introspection surface rewrite (`IidsOf`, `Provides`, `ProviderClass`, ...)        | A3 |
| 22 | Closure-identity helpers (`Nucleus`, `Facades`, `InClosure`, `WalkClosure`, ...) | A3 |
| 23 | End-to-end integration tests + final sweep (full SideTableResolver/CodeExtensionSingleton arms; epoch-RCU snapshot retirement; base-chain mergedDispatch flattening per D16; nucleus dtor walker for parked eagers) | A3 |

**A3 starts** when A2 ships. A3's first task is unblocked by Task 18's published
DispatchSnapshot + MergedDispatchSnapshot pair.

---

## Self-Review Notes

- **Spec coverage:** D5 (Interface-is-RootObject) → T20 InlineFacade arm. D7.2
  (cross-module seal check) → T18. D11 (Eager deferred Acquire) → T19. D14 (three arms)
  → T13, T20. D15 L0/L1/L2/L3 → T16, T15, T14, T17 respectively (consumed end-to-end in
  T20). D16 (base-chain propagation) → T18 (flat snapshot is published; full base
  flattening is the documented A3 deferral on the `mergedDispatch` line).
- **Placeholder scan:** the only forward references in the plan body point at A3 for the
  SideTableResolver / CodeExtensionSingleton arm materialisation (T20), the nucleus dtor
  walker for parked eagers (T19), and full D16 base-chain flattening (T18). All are
  reachable via the published types A2 lands; no T20 step references an A2 API that
  doesn't exist yet.
- **Forward references:** T13 → T14 (DispatchEntry consumed) → T15 (FingerprintCache wired
  into MetaLinks; consumes DispatchEntry forward decl) → T16 (consumes DispatchEntry +
  MetaCore) → T17 (consumes MetaLinks::BumpCacheEpoch) → T18 (consumes T13/T14/T17) →
  T19 (uses RootObject + TaggedPayload only — independent) → T20 (consumes T14/T15/T16/T18).
- **Type-spelling consistency:** `DispatchEntry`, `DispatchKind`, `DispatchSnapshot`,
  `MergedDispatchSnapshot`, `FingerprintCache`, `ExtendedListSnapshot`,
  `ImplementedListSnapshot`, `EagerSetSnapshot` spelled identically across all tasks.
  `DispatchEntry::arm` (not `armPtr_`); `MetaLinks::cacheEpoch` (not `cacheEpoch_`) —
  matches the live A1 header.
- **CMake test stanzas:** `add_yuki_test(Core <NameTest>)` — every task ends with the
  `Test` suffix attached. The existing `Yuki/tests/CMakeLists.txt` already follows that
  convention (see `RefcountInvariantTest`, `MetaCoreTest`).
- **Reflection-API spelling caveat:** clang-p2996 occasionally renames helpers; if a
  symbol your toolchain has differs from this plan's spelling (e.g. `extract` vs
  `value_of`), swap to the nearest equivalent and add an `// XXX y2-spec: see Task N`
  comment pointing back here.
- **A2 scope discipline:** the SideTableResolver / CodeExtensionSingleton arms in T20 are
  intentionally stubbed. The MergedDispatchSnapshot in T18 is intentionally a copy of the
  DispatchSnapshot rather than a full base-chain flatten. Both deferrals are documented
  inline in the commit bodies and in the A3 deferral table above. Do NOT expand them
  during A2 execution — A3 owns those.

---

## Execution Handoff

Plan complete and saved to
`docs/superpowers/plans/2026-06-18-yuki-y2-core-foundation-a2.md`. Two execution options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per task, two-stage
   review (spec compliance, then code quality) between tasks, fast iteration. Mirrors
   A1's execution mode.
2. **Inline Execution** — execute tasks in this session via the executing-plans skill,
   batch execution with checkpoints for review.

Which approach?







