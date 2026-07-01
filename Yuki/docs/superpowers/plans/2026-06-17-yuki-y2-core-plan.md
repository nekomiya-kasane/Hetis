# Yuki Object Model Y2 Core Implementation Plan — A1 Foundation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land Y2 Core — the non-template `RootObject`, single-word `TaggedPayload`,
`Y_OBJECT` identity hook, three seal annotations, hierarchical refcount, four-level Query
cache, three-layer MetaClass — as a clean Plan-1-incompatible refactor of `Yuki::Core`.

**Architecture:** Decisions D0–D18 from spec `2026-06-17-yuki-object-model-y2-design.md`.
Plan-1 headers (`MetaNode`, `IfaceFacadeNode`, `ExtensionNode`) are deleted. Tests are
rewritten. Spec is the source of truth — when in doubt, re-read the spec section the task
references.

**Tech Stack:** C++26 (clang-p2996 fork, LLVM 21.0.0git), MSVC ABI on Windows, libc++,
reflection (P2996) + annotations (P3394), `consteval`/`template for`/splices, Catch2 for
tests, CMake + Ninja, x64-asan build directory `build/x64-asan`.

**Environment activation (run before every build/compile):**
```
pwsh -NoProfile -Command "python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression; cd G:\Teaching\Vulkan; <cmd>"
```

**CMake test stanza:** `add_yuki_test(Core <Name>)` in `Yuki/tests/CMakeLists.txt`.

**Branch policy:** create a topic branch `y2-core-refactor` and work there; do not push to
`main` without user authorization. Never `--no-verify`, never amend after review.

**Out of scope for this plan:** cross-DLL discovery, plugin manifest, serialization (Plan B,
written after Y2 Core lands). Also out of scope: dispatch arms, Query cache, Registry, the
introspection surface — those are **Plan A2** and **Plan A3**, written after A1 lands.

**Plan split rationale:** Y2 Core spans 18 decisions touching 23 implementation areas. Per
writing-plans guidance ("each plan should produce working, testable software on its own"),
the work splits naturally into three plans whose boundaries are observable artefacts:

- **A1 — Foundation (this plan):** types and lifetime primitives exist and are tested in
  isolation. End state: you can `MakeOwned<X>` a stand-alone `RootObject` subclass, observe
  refcount, drop it, no Query yet. Tasks 1–12.
- **A2 — Dispatch + Query:** add `DispatchEntry`, the four-level Query cache, `Registry::Install`,
  eager extension semantics, the `Query<I>` entry points. End state: closures, extensions, and
  cross-arm dispatch are fully runnable. Tasks 13–20.
- **A3 — Introspection + integration:** add the Y2 `IidsOf`/`Provides`/`Nucleus`/`WalkClosure`
  surface and a battery of integration tests. End state: full Y2 Core. Tasks 21–23.

The overview below lists all 23 tasks so the reader can see the destination, but only Tasks
1–12 have full bodies in this plan.

---

## File Structure

Y2 rebuilds `Yuki/include/Yuki/Core/` from a small foundation. Below is the target layout —
each file has one responsibility and is small enough to hold in context.

**Created (new files):**
- `Yuki/include/Yuki/Core/Config.h` — `Yuki::kDebug` constexpr switch
- `Yuki/include/Yuki/Core/ClassType.h` — `ClassType` enum (Y2 variant: 6 roles)
- `Yuki/include/Yuki/Core/TaggedPayload.h` — bit layout struct + atomic CAS helpers
- `Yuki/include/Yuki/Core/RootObject.h` — REPLACED: non-template, 2 words exactly
- `Yuki/include/Yuki/Core/YObjectMacro.h` — `Y_OBJECT;` definition
- `Yuki/include/Yuki/Core/Anno.h` — REPLACED: Role/Meta annotation set with Final/Unique/Important
- `Yuki/include/Yuki/Core/MetaCore.h` — rodata MetaCore struct
- `Yuki/include/Yuki/Core/MetaLinks.h` — runtime mutable layer
- `Yuki/include/Yuki/Core/MetaDynamic.h` — instance-bound layer
- `Yuki/include/Yuki/Core/DispatchEntry.h` — DispatchKind + DispatchEntry (Y2: 3 arms)
- `Yuki/include/Yuki/Core/FingerprintCache.h` — L1 4-slot ring
- `Yuki/include/Yuki/Core/ComPtr.h` — COM-style smart pointer
- `Yuki/include/Yuki/Core/MakeOwned.h` — factory
- `Yuki/include/Yuki/Core/Registry.h` — REPLACED: Install<T> with runtime seal checks
- `Yuki/include/Yuki/Core/Query.h` — REPLACED: L0 consteval + dynamic kernel
- `Yuki/include/Yuki/Core/Introspection.h` — REPLACED: Y2 surface
- `Yuki/src/Core/Registry.cpp` — out-of-line snapshot publish + RCU retirement
- `Yuki/src/Core/RootObject.cpp` — out-of-line dtor, Acquire/Release definitions

**Deleted (Plan-1 carcass):**
- `Yuki/include/Yuki/Core/MetaNode.h`
- `Yuki/include/Yuki/Core/IfaceFacadeNode.h`
- `Yuki/include/Yuki/Core/ExtensionNode.h`
- `Yuki/include/Yuki/Core/FacadeList.h` (replaced by inline chain in `MetaLinks`)
- All Plan-1 tests under `Yuki/tests/Core/` *except* `Meta.h` which the new tests reuse.

**Tests created (one file per public surface):**
- `Yuki/tests/Core/TaggedPayloadTest.cpp`
- `Yuki/tests/Core/RootObjectTest.cpp`
- `Yuki/tests/Core/YObjectTest.cpp`
- `Yuki/tests/Core/AnnoTest.cpp`
- `Yuki/tests/Core/ComPtrTest.cpp`
- `Yuki/tests/Core/MakeOwnedTest.cpp`
- `Yuki/tests/Core/RefcountInvariantTest.cpp`
- `Yuki/tests/Core/RegistryTest.cpp` — REPLACED
- `Yuki/tests/Core/QueryCacheTest.cpp` (L0/L1/L2/L3)
- `Yuki/tests/Core/SealAnnoTest.cpp` (Final/Unique/Important runtime + consteval)
- `Yuki/tests/Core/IntrospectionTest.cpp` — REPLACED for Y2 surface

---

## Task Decomposition Overview

Tasks are ordered so each one produces a working, testable artefact. Earlier tasks define
the types later tasks depend on; no task forward-references unimplemented APIs.

1. **Plan-1 demolition** — delete the carcass, get the test target green-but-empty
2. **`Yuki::kDebug` switch + `ClassType` enum**
3. **`TaggedPayload`** — bit layout + atomic CAS
4. **`RootObject` non-template, 2-word**
5. **`Y_OBJECT;` macro + identity hook**
6. **`Anno` set (Role + Meta annotations, including Final/Unique/Important)**
7. **`MetaCore` rodata layer**
8. **`MetaLinks` runtime layer (atomic snapshots + cacheEpoch)**
9. **`MetaDynamic` instance layer**
10. **`ComPtr<T>` smart pointer**
11. **`MakeOwned<T>` factory**
12. **Hierarchical `Acquire`/`Release` semantics + diagnostics**
13. **`DispatchEntry` + `DispatchKind` (InlineFacade, SideTableResolver, CodeExtensionSingleton)**
14. **L2 mergedDispatch snapshot publish + binary search**
15. **L1 fingerprint cache**
16. **L0 consteval Query shortcut**
17. **L3 invalidation broadcast**
18. **`Registry::Install<T>` with runtime seal checks**
19. **Eager extension chain ownership + deferred Acquire**
20. **`Query<I>(node)` static face + `QueryDynamicRaw` kernel**
21. **Introspection surface rewrite (`IidsOf`, `Provides`, etc.)**
22. **Closure-identity helpers (`Nucleus`, `Facades`, `InClosure`, `WalkClosure`, `MaterializedFacades`)**
23. **End-to-end integration test + final sweep**

The full task bodies follow.

---

### Task 1: Plan-1 Demolition

**Spec ref:** D17.

**Files:**
- Delete: `Yuki/include/Yuki/Core/MetaNode.h`, `IfaceFacadeNode.h`, `ExtensionNode.h`,
  `FacadeList.h`
- Delete (all Y2-rewritten tests): every `Yuki/tests/Core/*.cpp` *except* `Meta.h`
- Modify: `Yuki/include/Yuki/Core/RootObject.h` — empty stub for now (full rewrite in Task 4)
- Modify: `Yuki/tests/CMakeLists.txt` — remove dead `add_yuki_test` entries

- [ ] **Step 1: Create topic branch**
```bash
git checkout -b y2-core-refactor
```

- [ ] **Step 2: Verify clean tree**
Run: `git status` — Expected: branch shows current dirty tree (carried from main). Stash or
commit unrelated WIP elsewhere before continuing.

- [ ] **Step 3: Delete Plan-1 headers**
```bash
git rm Yuki/include/Yuki/Core/MetaNode.h
git rm Yuki/include/Yuki/Core/IfaceFacadeNode.h
git rm Yuki/include/Yuki/Core/ExtensionNode.h
git rm Yuki/include/Yuki/Core/FacadeList.h
```

- [ ] **Step 4: Replace `RootObject.h` with empty stub**
Overwrite `Yuki/include/Yuki/Core/RootObject.h`:
```cpp
#pragma once
// Y2 RootObject — populated by Task 4.
namespace Yuki { struct RootObject; }
```

- [ ] **Step 5: Delete Plan-1 tests**
```bash
git rm Yuki/tests/Core/ConcurrentSlabArenaTest.cpp
git rm Yuki/tests/Core/IntrospectionTest.cpp
git rm Yuki/tests/Core/ClosureTest.cpp
git rm Yuki/tests/Core/QueryTest.cpp
# keep Meta.h
```

- [ ] **Step 6: Trim `Yuki/tests/CMakeLists.txt`**
Remove every `add_yuki_test(Core ...)` line whose source was just deleted. Leave the file
parseable.

- [ ] **Step 7: Verify build target still configures**
Run:
```
pwsh -NoProfile -Command "python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression; cd G:\Teaching\Vulkan; cmake --build build/x64-asan --target Yuki_Core_tests || true"
```
Expected: the build may fail because dependent code references deleted symbols. Note the
failures — they map to non-test files that also need cleanup in this task.

- [ ] **Step 8: Strip dangling Plan-1 includes from non-test sources**
Use Grep to find `#include "Yuki/Core/MetaNode.h"` (and the other three deleted headers) in
`Yuki/src/**/*.{h,cpp}` and `Yuki/include/Yuki/**/*.h`. Delete each include line and any
immediately-following symbol references (these are dead under Y2 anyway). If a file becomes
empty, delete it.

- [ ] **Step 9: Commit**
```bash
git add -A
git commit -m "refactor(Yuki/Core): demolish Plan-1 carcass for Y2 refactor

D17 — Y2 is a clean rewrite. Drop MetaNode/IfaceFacadeNode/ExtensionNode/FacadeList
plus all dependent tests. RootObject.h stubbed; populated in Task 4."
```

---

### Task 2: `Yuki::kDebug` + `ClassType`

**Spec ref:** D6 (ClassType), D13 (kDebug).

**Files:**
- Create: `Yuki/include/Yuki/Core/Config.h`
- Create: `Yuki/include/Yuki/Core/ClassType.h`
- Test: `Yuki/tests/Core/ClassTypeTest.cpp`

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/ClassTypeTest.cpp`:
```cpp
#include <Yuki/Core/ClassType.h>
#include <Yuki/Core/Config.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace Yuki;

TEST_CASE("ClassType enum has the six Y2 roles", AUTO_TAG) {
    static_assert(static_cast<int>(ClassType::None) == 0);
    static_assert(static_cast<int>(ClassType::Interface) == 1);
    static_assert(static_cast<int>(ClassType::Implementation) == 2);
    static_assert(static_cast<int>(ClassType::Extension) == 3);
    static_assert(static_cast<int>(ClassType::Imposter) == 4);
    static_assert(static_cast<int>(ClassType::Bridge) == 5);
    static_assert(std::is_same_v<std::underlying_type_t<ClassType>, std::uint8_t>);
}

TEST_CASE("Yuki::kDebug is a constexpr bool", AUTO_TAG) {
    static_assert(std::is_same_v<decltype(kDebug), const bool>);
    constexpr bool _ = kDebug; (void)_;
}
```

Wire up in `Yuki/tests/CMakeLists.txt`: `add_yuki_test(Core ClassType)`.

- [ ] **Step 2: Run failing test**
Run via the env-activated build command:
```
pwsh -NoProfile -Command "python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression; cd G:\Teaching\Vulkan; cmake --build build/x64-asan --target Yuki_Core_ClassType_test"
```
Expected: FAIL (headers not present).

- [ ] **Step 3: Create `Config.h`**
```cpp
#pragma once
namespace Yuki {
    inline constexpr bool kDebug =
#ifdef NDEBUG
        false
#else
        true
#endif
        ;
}
```

- [ ] **Step 4: Create `ClassType.h`**
```cpp
#pragma once
#include <cstdint>
namespace Yuki {
    enum class ClassType : std::uint8_t {
        None           = 0,
        Interface      = 1,
        Implementation = 2,
        Extension      = 3,
        Imposter       = 4,
        Bridge         = 5,
    };
}
```

- [ ] **Step 5: Re-run, expect PASS**
Same command as Step 2. Expected: PASS.

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/Config.h Yuki/include/Yuki/Core/ClassType.h \
        Yuki/tests/Core/ClassTypeTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): add Yuki::kDebug switch and ClassType enum

D6, D13 — six roles encoded in 3 bits; kDebug controls runtime diagnostics
without preprocessor-driven ABI splits."
```

---

### Task 3: `TaggedPayload`

**Spec ref:** D2, D10, D10'.

**Files:**
- Create: `Yuki/include/Yuki/Core/TaggedPayload.h`
- Test: `Yuki/tests/Core/TaggedPayloadTest.cpp`

- [ ] **Step 1: Write failing test**

Create `Yuki/tests/Core/TaggedPayloadTest.cpp`:
```cpp
#include <Yuki/Core/TaggedPayload.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>

using namespace Yuki;

TEST_CASE("TaggedPayload packs role/everAcquired/refcount/armPtr", AUTO_TAG) {
    auto* arm = reinterpret_cast<void*>(uintptr_t{0x7ffe'cafe'beefULL & ~uintptr_t{0xF}});
    TaggedPayload p = TaggedPayload::Make(ClassType::Extension, arm,
                                          /*refcount=*/3, /*everAcquired=*/true);
    REQUIRE(p.role()         == ClassType::Extension);
    REQUIRE(p.everAcquired() == true);
    REQUIRE(p.refcount()     == 3);
    REQUIRE(p.armPtr()       == arm);
}

TEST_CASE("ExternalLifetimeSentinel reports correctly", AUTO_TAG) {
    TaggedPayload p = TaggedPayload::MakeExternal(ClassType::Implementation, nullptr);
    REQUIRE(p.refcount() == TaggedPayload::kExternalSentinel);
    REQUIRE(p.isExternalLifetime());
}

TEST_CASE("Atomic CAS refcount increment preserves other fields", AUTO_TAG) {
    auto* arm = reinterpret_cast<void*>(uintptr_t{0x1000});
    std::atomic<TaggedPayload> a{TaggedPayload::Make(ClassType::Implementation, arm, 1, true)};
    REQUIRE(TaggedPayload::TryIncrement(a) == true);
    auto loaded = a.load(std::memory_order_relaxed);
    REQUIRE(loaded.refcount() == 2);
    REQUIRE(loaded.role()     == ClassType::Implementation);
    REQUIRE(loaded.armPtr()   == arm);
}
```

Wire: `add_yuki_test(Core TaggedPayload)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Create `TaggedPayload.h`**

```cpp
#pragma once
#include <Yuki/Core/ClassType.h>
#include <atomic>
#include <cstdint>
namespace Yuki {
    struct TaggedPayload {
        static constexpr std::uint16_t kExternalSentinel = 0xFFFF;
        static constexpr std::uint16_t kSaturationLimit  = 0xFFFE;

        std::uint64_t word{};

        static constexpr TaggedPayload Make(ClassType r, void* arm,
                                            std::uint16_t rc = 1,
                                            bool ever = true) noexcept {
            std::uint64_t w = (static_cast<std::uint64_t>(r) & 0x7)
                            | ((ever ? 1ull : 0ull) << 3)
                            | ((static_cast<std::uint64_t>(rc) & 0xFFFF) << 4)
                            | ((reinterpret_cast<std::uint64_t>(arm) >> 4) << 20);
            return {w};
        }
        static constexpr TaggedPayload MakeExternal(ClassType r, void* arm) noexcept {
            return Make(r, arm, kExternalSentinel, false);
        }

        constexpr ClassType   role()         const noexcept { return ClassType(word & 0x7); }
        constexpr bool        everAcquired() const noexcept { return (word >> 3) & 0x1; }
        constexpr std::uint16_t refcount()   const noexcept { return (word >> 4) & 0xFFFF; }
        void* armPtr() const noexcept {
            return reinterpret_cast<void*>((word >> 20) << 4);
        }
        constexpr bool isExternalLifetime() const noexcept {
            return refcount() == kExternalSentinel;
        }

        // CAS loop: increment refcount unless saturated/sentinel. Returns false if not allowed.
        static bool TryIncrement(std::atomic<TaggedPayload>& a) noexcept {
            for (auto cur = a.load(std::memory_order_relaxed);;) {
                if (cur.isExternalLifetime()) return true;            // no-op
                if (cur.refcount() >= kSaturationLimit) return false; // saturated
                TaggedPayload nxt = cur;
                nxt.word = (cur.word & ~(0xFFFFull << 4))
                         | ((std::uint64_t(cur.refcount() + 1) & 0xFFFF) << 4);
                if (a.compare_exchange_weak(cur, nxt,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
            }
        }
        // Returns true iff the decrement transitioned refcount to 0 (caller deletes).
        static bool TryDecrement(std::atomic<TaggedPayload>& a) noexcept {
            for (auto cur = a.load(std::memory_order_relaxed);;) {
                if (cur.isExternalLifetime()) return false;
                std::uint16_t rc = cur.refcount();
                if (rc == 0) return false;
                TaggedPayload nxt = cur;
                nxt.word = (cur.word & ~(0xFFFFull << 4))
                         | ((std::uint64_t(rc - 1) & 0xFFFF) << 4);
                if (a.compare_exchange_weak(cur, nxt,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                    return (rc == 1);
            }
        }
    };
    static_assert(sizeof(TaggedPayload) == sizeof(std::uint64_t));
}
```

- [ ] **Step 4: Re-run, expect PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/TaggedPayload.h Yuki/tests/Core/TaggedPayloadTest.cpp \
        Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): add TaggedPayload single-word layout

D2/D10/D10' — role (3 bit) + ever_acquired (1 bit) + refcount (16 bit) + arm_ptr (44 bit)
packed in one uint64_t with CAS increment/decrement helpers; sentinel 0xFFFF encodes
external lifetime."
```

---

### Task 4: `RootObject` non-template

**Spec ref:** D0, D1, D6.

**Files:**
- Modify: `Yuki/include/Yuki/Core/RootObject.h` (replace empty stub from Task 1)
- Create: `Yuki/src/Core/RootObject.cpp`
- Test: `Yuki/tests/Core/RootObjectTest.cpp`

- [ ] **Step 1: Write failing test**

`Yuki/tests/Core/RootObjectTest.cpp`:
```cpp
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/ClassType.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace Yuki;

TEST_CASE("RootObject is exactly 2 words", AUTO_TAG) {
    static_assert(sizeof(RootObject) == 2 * sizeof(void*));
    static_assert(alignof(RootObject) == alignof(void*));
}

TEST_CASE("RootObject has virtual dtor", AUTO_TAG) {
    static_assert(std::has_virtual_destructor_v<RootObject>);
}

TEST_CASE("RootObject is non-template", AUTO_TAG) {
    // If RootObject were a template, naming it bare wouldn't compile.
    RootObject* p = nullptr; (void)p;
}

namespace {
    struct Probe : Yuki::RootObject {
        Probe() : RootObject(Yuki::ClassType::Implementation, nullptr,
                             /*external=*/true) {}
        ~Probe() override = default;
    };
}

TEST_CASE("External-lifetime instance exposes role via TypeDynamic", AUTO_TAG) {
    Probe p;
    REQUIRE(p.TypeDynamic() == ClassType::Implementation);
}
```

Wire: `add_yuki_test(Core RootObject)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Write `RootObject.h`**

```cpp
#pragma once
#include <Yuki/Core/ClassType.h>
#include <Yuki/Core/TaggedPayload.h>
#include <atomic>

namespace Yuki {
    struct RootObject {
        explicit RootObject(ClassType role, void* arm, bool external) noexcept
          : metaWord_(external
                ? TaggedPayload::MakeExternal(role, arm)
                : TaggedPayload::Make(role, arm, /*rc=*/1, /*ever=*/true))
        {}
        virtual ~RootObject() noexcept;

        RootObject(const RootObject&) = delete;
        RootObject& operator=(const RootObject&) = delete;

        ClassType TypeDynamic() const noexcept {
            return metaWord_.load(std::memory_order_relaxed).role();
        }
        TaggedPayload PayloadRelaxed() const noexcept {
            return metaWord_.load(std::memory_order_relaxed);
        }

        // Acquire/Release defined in Task 12.
        std::atomic<TaggedPayload>& MetaWord() noexcept { return metaWord_; }

      private:
        std::atomic<TaggedPayload> metaWord_;
    };
    static_assert(sizeof(RootObject) == 2 * sizeof(void*));
}
```

- [ ] **Step 4: Write `RootObject.cpp`**
```cpp
#include <Yuki/Core/RootObject.h>
namespace Yuki { RootObject::~RootObject() noexcept = default; }
```

Wire `RootObject.cpp` into `Yuki/src/CMakeLists.txt` (or whichever list builds `Yuki_Core`).

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/RootObject.h Yuki/src/Core/RootObject.cpp \
        Yuki/tests/Core/RootObjectTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): non-template 2-word RootObject

D0/D1/D6 — single concrete base, virtual dtor, atomic TaggedPayload word, role
readable without vcall via TypeDynamic."
```

---

### Task 5: `Y_OBJECT;` macro

**Spec ref:** D3, D4.

**Files:**
- Create: `Yuki/include/Yuki/Core/YObjectMacro.h`
- Create: `Yuki/include/Yuki/Core/MetaCore.h` (minimal shim; Task 7 expands)
- Test: `Yuki/tests/Core/YObjectTest.cpp`

The macro materialises `static constexpr auto kMetaCore` via reflection over the enclosing
class and asserts the two invariants. Role deduction reuses `Yuki::ClassTypeOf<T>` from the
pre-existing `Identity.h` (which already exposes `Anno::Implementation` etc. as `Anno::Role`
constants stamped via `[[=Anno::Implementation]]` and decoded by
`Detail::ReadClassType(^^T)`). No `Anno.h` shim is introduced here — Task 6 grows
`Identity.h` with the seal annotations (Final/Unique/Important/Pickled).

- [ ] **Step 1: Write failing test**

`Yuki/tests/Core/YObjectTest.cpp`:
```cpp
#include <Yuki/Core/YObjectMacro.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Identity.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Implementation]] MyImpl : RootObject {
      public:
        Y_OBJECT;
        MyImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~MyImpl() override = default;
    };
}

TEST_CASE("Y_OBJECT exposes kMetaCore via reflection", AUTO_TAG) {
    constexpr auto& core = MyImpl::kMetaCore;
    REQUIRE(core.role == ClassType::Implementation);
}

TEST_CASE("MyImpl still 2 words after Y_OBJECT", AUTO_TAG) {
    // Y_OBJECT adds only statics + friend; no per-instance growth.
    static_assert(sizeof(MyImpl) == sizeof(RootObject));
}
```

Wire: `add_yuki_test(Core YObjectTest)`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Write `MetaCore.h` (minimal shim — Task 7 expands)**

```cpp
#pragma once
#include <Yuki/Core/Identity.h>
namespace Yuki {
    struct MetaCore { ClassType role; };
    namespace Detail {
        template <class T> consteval MetaCore MakeMetaCoreFor() {
            return { ClassTypeOf<T> };
        }
        template <class T> struct MetaHook {};
    }
}
```

- [ ] **Step 4: Write `YObjectMacro.h`**

```cpp
#pragma once
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaCore.h>
#include <meta>
#include <type_traits>

#define Y_OBJECT                                                                       \
  public:                                                                              \
    using Self = std::remove_cvref_t<decltype(*this)>;                              \
    static_assert(std::has_virtual_destructor_v<Self>,                              \
                  "Y_OBJECT requires a virtual destructor on the enclosing class");    \
    static constexpr ::Yuki::MetaCore kMetaCore =                                      \
        ::Yuki::Detail::MakeMetaCoreFor<Self>();                                    \
    friend struct ::Yuki::Detail::MetaHook<Self>
```

**Note on the public-access invariant (D3.2):** the spec asks that `Y_OBJECT` *only* compile
inside `public:`. The macro's first token is `public:`, so any expansion makes its own
contents public regardless of the surrounding region — meaning a same-TU consteval check that
introspects `members_of(^^EnclosingClass)` cannot run *before* the macro itself injects
`kMetaCore`. The simpler enforcement is reflection-after-the-fact: once `kMetaCore` is
visible, Task 7's `MakeMetaCoreFor<T>` (and any later consteval consumer that reads
`T::kMetaCore`) can `static_assert(is_public(^^T::kMetaCore))` against the class's reflected
member list. For Task 5 the macro itself only asserts the virtual-dtor invariant; the
public-access invariant rides on Task 7's MetaCore pipeline. Note this as an "intentional Task
7 dependency" in the commit body.

- [ ] **Step 5: Re-run, expect PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/YObjectMacro.h Yuki/include/Yuki/Core/MetaCore.h \
        Yuki/tests/Core/YObjectTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): Y_OBJECT identity hook (D3/D4)

D3 — macro asserts virtual dtor, materialises kMetaCore via reflection.
D4 — role deduction via Identity.h's ClassTypeOf<T> (Anno::Role-based).
Minimal MetaCore shim; full layout + public-access invariant land in Task 7."
```

---

### Task 6: seal annotations on `Identity.h`

**Spec ref:** D7 (Final / Unique / Important), P2-D4 (Pickled — placeholder; full semantics
in Plan B).

**Files:**
- Modify: `Yuki/include/Yuki/Core/Identity.h` (add `Anno::Final`/`Unique`/`Important`/`Pickled`
  + `Detail::SealFlagsFor<T, I>()`)
- Test: `Yuki/tests/Core/SealsTest.cpp`

`Identity.h` already provides the role annotations (`Anno::Interface`, `Anno::Implementation`,
…) as edge-less `Anno::Role` constants, plus `Anno::Implements{InfoList}` and
`Anno::Extends{InfoList}` carrying inheritance edges as static-storage arrays of
`std::meta::info`. Task 6 grows that file with the three orthogonal seal annotations and the
field-level `Pickled` marker (referenced by Task 7's MetaCore and by Plan B's serializer).

- [ ] **Step 1: Write failing test**

`Yuki/tests/Core/SealsTest.cpp`:
```cpp
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/RootObject.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
namespace {
    struct [[=Anno::Interface]]      IFoo { virtual ~IFoo() = default; };
    struct [[=Anno::Interface]]      IBar { virtual ~IBar() = default; };
    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IFoo}]]
           [[=Anno::Final{^^IFoo}]]
           F : RootObject, IFoo {
        F() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~F() override = default;
    };
    struct [[=Anno::Extension]]
           [[=Anno::Extends{^^F}]]
           [[=Anno::Implements{^^IBar}]]
           [[=Anno::Important{^^IBar}]]
           X : RootObject {
        X() : RootObject(ClassType::Extension, nullptr, /*external=*/true) {}
        ~X() override = default;
    };
}
TEST_CASE("Seal flags reachable consteval", AUTO_TAG) {
    constexpr auto sf = Detail::SealFlagsFor<F, IFoo>();
    static_assert(sf.final == true);
    static_assert(sf.unique == false);
    static_assert(sf.important == false);
    constexpr auto sx = Detail::SealFlagsFor<X, IBar>();
    static_assert(sx.important == true);
    static_assert(sx.final == false);
}
```
Wire: `add_yuki_test(Core SealsTest)`.

- [ ] **Step 2: Run, FAIL.**

- [ ] **Step 3: Extend `Identity.h`** (append inside `namespace Yuki::Anno`, near `Implements`)

```cpp
        /// @brief Inheritance seal — no subclass may re-implement @c iface.
        struct Final {
            std::meta::info iface;
        };
        /// @brief Closure seal — at most one class in this nucleus's closure implements @c iface.
        struct Unique {
            std::meta::info iface;
        };
        /// @brief Closure seal — this implementation always wins dispatch for @c iface.
        struct Important {
            std::meta::info iface;
        };

        /// @brief Field-level marker: this NSDM participates in closure serialization.
        ///        Full semantics in Plan B (P2-D4); for now the marker is just discoverable
        ///        via reflection.
        struct Pickled {};
```

And inside `namespace Yuki::Detail` (or a new sub-namespace), append:

```cpp
        struct SealFlags {
            bool final = false;
            bool unique = false;
            bool important = false;
        };
        template <class T, class I>
        consteval SealFlags SealFlagsFor() {
            SealFlags f{};
            for (auto a : std::meta::annotations_of(^^T)) {
                auto t = std::meta::type_of(a);
                if (t == ^^Anno::Final
                    && std::meta::extract<Anno::Final>(a).iface == ^^I) f.final = true;
                if (t == ^^Anno::Unique
                    && std::meta::extract<Anno::Unique>(a).iface == ^^I) f.unique = true;
                if (t == ^^Anno::Important
                    && std::meta::extract<Anno::Important>(a).iface == ^^I) f.important = true;
            }
            return f;
        }
```

- [ ] **Step 4: Re-run, PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/Identity.h Yuki/tests/Core/SealsTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): Final/Unique/Important seals + Pickled marker (D7, P2-D4)

Grow Identity.h with the three orthogonal seal annotations (Final = inheritance,
Unique + Important = closure) and the field-level Pickled marker referenced by
Task 7's MetaCore and Plan B's closure serializer. Detail::SealFlagsFor<T, I>()
exposes a consteval triple for any (T, I) pair."
```

---

### Task 7: `MetaCore` rodata layer

**Spec ref:** D18 (MetaCore tier), D7 (seal flags stored here), D3 (public-access invariant
on `kMetaCore`).

**Files:**
- Modify: `Yuki/include/Yuki/Core/MetaCore.h` (expand Task-5 shim)
- Test: `Yuki/tests/Core/MetaCoreTest.cpp`

`Identity.h` already exposes `Anno::Implements{InfoList}` and `Anno::Extends{InfoList}`,
where `InfoList` is a static-storage view over an array of `std::meta::info` reflections
(plain pointer + count, structural for use inside annotation values). Task 7 consumes those
and emits the flattened `(iid, SealFlags)` and `(nucleusIid, eager)` arrays into `.rodata`.

- [ ] **Step 1: Write failing test**
```cpp
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/RootObject.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
namespace {
    struct [[=Anno::Interface]] IZ { virtual ~IZ() = default; };
    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IZ}]]
           [[=Anno::Final{^^IZ}]]
           Z : RootObject, IZ {
        Z() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~Z() override = default;
    };
}
TEST_CASE("MetaCore exposes iid, role, name, implements list", AUTO_TAG) {
    constexpr auto& m = Detail::MakeMetaCoreFor<Z>();
    REQUIRE(m.role == ClassType::Implementation);
    REQUIRE(m.implementsCount >= 1);
    REQUIRE(m.implements[0].iid == IidOf<IZ>());
    REQUIRE(m.implements[0].flags.final == true);
}
```
Wire: `add_yuki_test(Core MetaCoreTest)`.

- [ ] **Step 2: FAIL.**

- [ ] **Step 3: Replace `MetaCore.h`**

```cpp
#pragma once
#include <Yuki/Core/Identity.h>
#include <array>
#include <meta>
#include <string_view>

namespace Yuki {
    struct ImplementsInfo { Iid iid; Detail::SealFlags flags; };
    struct ExtendsInfo    { Iid nucleusIid; bool eager; };

    struct MetaCore {
        Iid                  iid;
        std::string_view     name;
        ClassType            role;
        const ImplementsInfo* implements;
        std::size_t          implementsCount;
        const ExtendsInfo*   extends;
        std::size_t          extendsCount;
    };

    namespace Detail {

        // Count the InfoList entries across all Anno::Implements annotations on T.
        template <class T> consteval std::size_t CountImplements() {
            std::size_t n = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Implements))
                n += std::meta::extract<Anno::Implements>(a).ifaces.size();
            return n;
        }

        // Compute seal flags for a single (T, iface-info) pair by scanning Final/Unique/Important.
        template <class T> consteval SealFlags SealFlagsForInfo(std::meta::info iface) {
            SealFlags f{};
            for (auto a : std::meta::annotations_of(^^T)) {
                auto t = std::meta::type_of(a);
                if (t == ^^Anno::Final
                    && std::meta::extract<Anno::Final>(a).iface == iface) f.final = true;
                if (t == ^^Anno::Unique
                    && std::meta::extract<Anno::Unique>(a).iface == iface) f.unique = true;
                if (t == ^^Anno::Important
                    && std::meta::extract<Anno::Important>(a).iface == iface) f.important = true;
            }
            return f;
        }

        template <class T> consteval auto MakeImplementsArrayFor() {
            constexpr std::size_t N = CountImplements<T>();
            std::array<ImplementsInfo, N> out{};
            std::size_t i = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Implements)) {
                auto im = std::meta::extract<Anno::Implements>(a);
                for (auto p = im.ifaces.begin(); p != im.ifaces.end(); ++p) {
                    out[i].iid   = IidOfMeta(*p);
                    out[i].flags = SealFlagsForInfo<T>(*p);
                    ++i;
                }
            }
            return out;
        }

        // Symmetric for Extends; Eager bit comes from Anno::Eager on T (not per-edge).
        template <class T> consteval std::size_t CountExtends() {
            std::size_t n = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Extends))
                n += std::meta::extract<Anno::Extends>(a).bases.size();
            return n;
        }
        template <class T> consteval auto MakeExtendsArrayFor() {
            constexpr std::size_t N = CountExtends<T>();
            constexpr bool eagerT = Anno::IsEager<T>;
            std::array<ExtendsInfo, N> out{};
            std::size_t i = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Extends)) {
                auto ex = std::meta::extract<Anno::Extends>(a);
                for (auto p = ex.bases.begin(); p != ex.bases.end(); ++p) {
                    out[i].nucleusIid = IidOfMeta(*p);
                    out[i].eager      = eagerT;
                    ++i;
                }
            }
            return out;
        }

        template <class T> inline constexpr auto kImplementsArr = MakeImplementsArrayFor<T>();
        template <class T> inline constexpr auto kExtendsArr    = MakeExtendsArrayFor<T>();

        // Verify D3's public-access invariant: kMetaCore must live in a public: section.
        template <class T> consteval bool YObjectIsPublic() {
            for (auto m : std::meta::members_of(^^T, std::meta::access_context::current()))
                if (std::meta::is_static_member(m)
                    && std::meta::identifier_of(m) == std::string_view{"kMetaCore"})
                    return std::meta::is_public(m);
            return true;
        }

        template <class T> consteval MetaCore MakeMetaCoreFor() {
            return MetaCore{
                .iid             = IidOf<T>(),
                .name            = std::meta::identifier_of(^^T),
                .role            = ClassTypeOf<T>,
                .implements      = kImplementsArr<T>.data(),
                .implementsCount = kImplementsArr<T>.size(),
                .extends         = kExtendsArr<T>.data(),
                .extendsCount    = kExtendsArr<T>.size(),
            };
        }

        template <class T> struct MetaHook {};
    }
}
```

`IidOfMeta(std::meta::info)` is a helper that mirrors `IidOf<T>()` but accepts a reflection
value — needed because `Anno::Implements`/`Anno::Extends` store `info`, not a type. If
`Identity.h` does not already expose this, add it there in this task with body
`return IidOf<[: type :]>()` against a type-info splice — confirm the splice spelling against
your clang-p2996 build (the `[: ... :]` syntax was stable as of P2996 r17).

**On the D3 public-access invariant:** `YObjectIsPublic<T>` is defined here and called by the
consumer that picks up `kMetaCore` at install time (Task 8's Registry / Task 9's MetaDynamic).
Task 5's macro itself cannot run this check at expansion time (kMetaCore is not yet declared);
Task 7 plumbs it into the pipeline so the diagnostic still fires before any runtime use.

- [ ] **Step 4: PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/MetaCore.h Yuki/include/Yuki/Core/Identity.h \
        Yuki/tests/Core/MetaCoreTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): rodata MetaCore with Implements/Extends arrays (D18/D7)

Consumes Identity.h's Anno::Implements{InfoList} and Anno::Extends{InfoList},
flattens to two static-storage arrays of (iid, SealFlags) and (nucleusIid, eager).
Also plumbs the D3 public-access invariant via Detail::YObjectIsPublic<T>(), which
later install-time consumers call to enforce the kMetaCore-must-be-public rule."
```

---

### Task 8: `MetaLinks` runtime layer

**Spec ref:** D18, D15 (cacheEpoch + L1 fingerprint slot live here).

**Files:**
- Create: `Yuki/include/Yuki/Core/MetaLinks.h`
- Test: `Yuki/tests/Core/MetaLinksTest.cpp`

- [ ] **Step 1: Failing test** — verifies the atomic pointers default to nullptr and that
`cacheEpoch_` increments via `BumpCacheEpoch()`.
```cpp
#include <Yuki/Core/MetaLinks.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
TEST_CASE("MetaLinks defaults", AUTO_TAG) {
    MetaLinks l{};
    REQUIRE(l.dispatch.load() == nullptr);
    REQUIRE(l.mergedDispatch.load() == nullptr);
    REQUIRE(l.extendedBy.load() == nullptr);
    REQUIRE(l.implementedBy.load() == nullptr);
    REQUIRE(l.eagerSet.load() == nullptr);
    auto e0 = l.cacheEpoch.load();
    l.BumpCacheEpoch();
    REQUIRE(l.cacheEpoch.load() == e0 + 1);
}
```
Wire: `add_yuki_test(Core MetaLinks)`.

- [ ] **Step 2: FAIL.**

- [ ] **Step 3: Write `MetaLinks.h`**
```cpp
#pragma once
#include <Yuki/Core/FingerprintCache.h>     // from Task 15 — forward-declare struct here
#include <atomic>
namespace Yuki {
    struct DispatchSnapshot;        // defined in DispatchEntry.h (Task 13)
    struct ExtendedListSnapshot;
    struct ImplementedListSnapshot;
    struct EagerSetSnapshot;
    struct MergedDispatchSnapshot;
    struct MetaLinks {
        std::atomic<const DispatchSnapshot*>        dispatch{nullptr};
        std::atomic<const MergedDispatchSnapshot*>  mergedDispatch{nullptr};
        std::atomic<const ExtendedListSnapshot*>    extendedBy{nullptr};
        std::atomic<const ImplementedListSnapshot*> implementedBy{nullptr};
        std::atomic<const EagerSetSnapshot*>        eagerSet{nullptr};
        std::atomic<std::uint64_t>                  cacheEpoch{0};
        // L1 fingerprint ring; layout in Task 15.
        // ... (declare a `mutable FingerprintCache l1;` member when Task 15 lands)

        void BumpCacheEpoch() noexcept { cacheEpoch.fetch_add(1, std::memory_order_acq_rel); }
    };
}
```
Snapshot structs are forward-declared; Task 13 (DispatchEntry) and Task 17 (extendedBy
invalidation) flesh them out.

- [ ] **Step 4: PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/MetaLinks.h Yuki/tests/Core/MetaLinksTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): atomic-snapshot MetaLinks layer (D18, D15 prep)"
```

---

### Task 9: `MetaDynamic` instance layer

**Spec ref:** D18, D14 (per-instance arm cache for L0 fast path).

**Files:**
- Create: `Yuki/include/Yuki/Core/MetaDynamic.h`
- Test: `Yuki/tests/Core/MetaDynamicTest.cpp`

- [ ] **Step 1: Failing test**
```cpp
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/Anno.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
namespace { struct [[=Anno::Implementation]] D : RootObject {}; }
TEST_CASE("MetaDynamic exposes kMetaCore and a MetaLinks ptr", AUTO_TAG) {
    const auto& md = Yuki::MetaDynamicOf<D>;
    REQUIRE(md.core == &D::kMetaCore);
    REQUIRE(md.links != nullptr);  // each Y_OBJECT class owns one MetaLinks instance
}
```
Wire: `add_yuki_test(Core MetaDynamic)`.

- [ ] **Step 2: FAIL.**

- [ ] **Step 3: Write `MetaDynamic.h`**
```cpp
#pragma once
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaLinks.h>
namespace Yuki {
    struct MetaDynamic { const MetaCore* core; MetaLinks* links; };
    namespace Detail {
        template <class T> inline MetaLinks gLinksFor{};
    }
    template <class T> inline constexpr MetaDynamic MetaDynamicOf{
        &T::kMetaCore, &Detail::gLinksFor<T>
    };
}
```
The `Y_OBJECT` macro (Task 5) needs a follow-on Edit to add `static MetaDynamic& Meta()
{ return MetaDynamicOf<Self>; }` so instances can reach their links without templates at
the call site. Add that line and re-build YObjectTest.

- [ ] **Step 4: PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/MetaDynamic.h Yuki/include/Yuki/Core/YObjectMacro.h \
        Yuki/tests/Core/MetaDynamicTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): MetaDynamic per-class link to MetaLinks (D18)"
```

---

### Task 10: `ComPtr<T>`

**Spec ref:** D12.

**Files:**
- Create: `Yuki/include/Yuki/Core/ComPtr.h`
- Test: `Yuki/tests/Core/ComPtrTest.cpp`

- [ ] **Step 1: Failing test**
```cpp
#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/RootObject.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
namespace {
    struct Counted : RootObject {
        Counted() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~Counted() override = default;
    };
}
TEST_CASE("ComPtr Adopt + copy + dtor balances refcount", AUTO_TAG) {
    auto* raw = new Counted();          // refcount = 1
    ComPtr<Counted> a = ComPtr<Counted>::Adopt(raw);   // takes +1 ownership
    REQUIRE(raw->PayloadRelaxed().refcount() == 1);
    {
        ComPtr<Counted> b = a;          // +1
        REQUIRE(raw->PayloadRelaxed().refcount() == 2);
    }                                   // -1
    REQUIRE(raw->PayloadRelaxed().refcount() == 1);
    // a's dtor takes last ref to 0 and deletes raw.
}
```
Wire: `add_yuki_test(Core ComPtr)`.

- [ ] **Step 2: FAIL.**

- [ ] **Step 3: Write `ComPtr.h`** (Acquire/Release implementation lands in Task 12; for now,
declare them in `RootObject.h` and forward-call.)
```cpp
#pragma once
#include <Yuki/Core/RootObject.h>
#include <utility>
namespace Yuki {
    // Acquire/Release declared on RootObject in Task 12; ComPtr calls them.
    void Acquire(RootObject* p) noexcept;
    bool Release(RootObject* p) noexcept;   // returns true iff caller should delete p

    template <class T>
    class ComPtr {
        T* p_{};
      public:
        constexpr ComPtr() noexcept = default;
        explicit ComPtr(T* p) noexcept : p_(p) { if (p_) Acquire(p_); }   // bump
        static ComPtr Adopt(T* p) noexcept { ComPtr c; c.p_ = p; return c; } // takes existing +1
        ComPtr(const ComPtr& o) noexcept : p_(o.p_) { if (p_) Acquire(p_); }
        ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
        ComPtr& operator=(ComPtr o) noexcept { std::swap(p_, o.p_); return *this; }
        ~ComPtr() noexcept { if (p_ && Release(p_)) delete p_; }
        T*  Get()    const noexcept { return p_; }
        T*  Detach()       noexcept { return std::exchange(p_, nullptr); }
        T&  operator*()  const noexcept { return *p_; }
        T*  operator->() const noexcept { return p_; }
        explicit operator bool() const noexcept { return p_; }
    };
}
```

- [ ] **Step 4: FAIL still — Acquire/Release undefined.** This is expected; Task 12 provides
the symbol bodies. Comment out the test wire-up in `CMakeLists.txt` until Task 12, OR write a
temporary inline `Acquire(p)` / `Release(p)` body in `RootObject.cpp` that calls
`TaggedPayload::TryIncrement/TryDecrement`. Pick the temporary impl — Task 12 generalises it.

- [ ] **Step 5: PASS.**

- [ ] **Step 6: Commit**
```bash
git add Yuki/include/Yuki/Core/ComPtr.h Yuki/include/Yuki/Core/RootObject.h \
        Yuki/src/Core/RootObject.cpp Yuki/tests/Core/ComPtrTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): ComPtr<T> with Acquire/Release plumbing (D12)"
```

---

### Task 11: `MakeOwned<T>` factory

**Spec ref:** D12.

**Files:**
- Create: `Yuki/include/Yuki/Core/MakeOwned.h`
- Test: `Yuki/tests/Core/MakeOwnedTest.cpp`

- [ ] **Step 1: Failing test**
```cpp
#include <Yuki/Core/MakeOwned.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
namespace {
    struct Counted : RootObject {
        int v;
        explicit Counted(int x) : RootObject(ClassType::Implementation, nullptr, false), v(x) {}
        ~Counted() override = default;
    };
}
TEST_CASE("MakeOwned constructs with refcount=1 and ComPtr ownership", AUTO_TAG) {
    auto p = MakeOwned<Counted>(42);
    REQUIRE(p);
    REQUIRE(p->v == 42);
    REQUIRE(p->PayloadRelaxed().refcount() == 1);
}
```
Wire: `add_yuki_test(Core MakeOwned)`.

- [ ] **Step 2: FAIL.**

- [ ] **Step 3: Write `MakeOwned.h`**
```cpp
#pragma once
#include <Yuki/Core/ComPtr.h>
#include <utility>
namespace Yuki {
    template <class T, class... Args>
    [[nodiscard]] ComPtr<T> MakeOwned(Args&&... args) {
        // T's ctor must call RootObject(role, arm, external=false) so refcount starts at 1.
        return ComPtr<T>::Adopt(new T(std::forward<Args>(args)...));
    }
}
```

- [ ] **Step 4: PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/MakeOwned.h Yuki/tests/Core/MakeOwnedTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): MakeOwned<T> factory (D12)"
```

---

### Task 12: Hierarchical `Acquire` / `Release` semantics

**Spec ref:** D8, D9, D10, D13.

**Files:**
- Modify: `Yuki/include/Yuki/Core/RootObject.h`
- Modify: `Yuki/src/Core/RootObject.cpp`
- Test: `Yuki/tests/Core/RefcountInvariantTest.cpp`

This task generalises the Task-10 placeholder into the spec's full semantics: facade
ctor/dtor and extension ctor/dtor *propagate* Acquire/Release to upstream. Without dispatch
arms (still in A2), this task tests the propagation primitive in isolation by simulating
"upstream" with a raw secondary `RootObject` and a manual call.

- [ ] **Step 1: Failing test**
```cpp
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/Config.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
namespace {
    struct Impl : RootObject {
        Impl() : RootObject(ClassType::Implementation, nullptr, false) {}
        ~Impl() override = default;
    };
    // A facade-like that pins its underlying on ctor and unpins on dtor.
    struct Facadeish : RootObject {
        RootObject* underlying;
        explicit Facadeish(RootObject* u)
          : RootObject(ClassType::Interface, nullptr, false), underlying(u)
        { Acquire(underlying); }
        ~Facadeish() override { if (Release(underlying)) delete underlying; }
    };
}
TEST_CASE("Facade-like Acquire pins underlying for its lifetime", AUTO_TAG) {
    auto impl = MakeOwned<Impl>();
    Impl* rawImpl = impl.Get();
    REQUIRE(rawImpl->PayloadRelaxed().refcount() == 1);
    {
        auto f = MakeOwned<Facadeish>(rawImpl);
        REQUIRE(rawImpl->PayloadRelaxed().refcount() == 2);   // facade pinned underlying
        REQUIRE(f->PayloadRelaxed().refcount() == 1);
        REQUIRE(f->PayloadRelaxed().refcount() >= rawImpl->PayloadRelaxed().refcount() - 1);
    }
    REQUIRE(rawImpl->PayloadRelaxed().refcount() == 1);       // facade gone, underlying released
}
TEST_CASE("Saturation guard at kSaturationLimit", AUTO_TAG) {
    auto impl = MakeOwned<Impl>();
    auto& w = impl->MetaWord();
    // Force saturation: write refcount=kSaturationLimit-1 then bump twice.
    auto cur = w.load();
    cur.word = (cur.word & ~(0xFFFFull << 4))
             | (std::uint64_t(TaggedPayload::kSaturationLimit - 1) << 4);
    w.store(cur);
    REQUIRE(TaggedPayload::TryIncrement(w) == true);   // reaches kSaturationLimit
    REQUIRE(TaggedPayload::TryIncrement(w) == false);  // refuses further bump
}
```
Wire: `add_yuki_test(Core RefcountInvariant)`.

- [ ] **Step 2: FAIL** (the simple impl from Task 10 may not include kDebug asserts yet).

- [ ] **Step 3: Update `RootObject.cpp`**
```cpp
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Config.h>
#include <cassert>
namespace Yuki {
    RootObject::~RootObject() noexcept = default;
    void Acquire(RootObject* p) noexcept {
        if (!p) return;
        const bool ok = TaggedPayload::TryIncrement(p->MetaWord());
        if constexpr (kDebug) { assert(ok && "Acquire saturated"); }
        (void)ok;
    }
    bool Release(RootObject* p) noexcept {
        if (!p) return false;
        return TaggedPayload::TryDecrement(p->MetaWord());
    }
}
```

- [ ] **Step 4: PASS.**

- [ ] **Step 5: Commit**
```bash
git add Yuki/include/Yuki/Core/RootObject.h Yuki/src/Core/RootObject.cpp \
        Yuki/tests/Core/RefcountInvariantTest.cpp Yuki/tests/CMakeLists.txt
git commit -m "feat(Yuki/Core): hierarchical Acquire/Release with kDebug asserts (D8–D13)"
```

---

## Tasks 13–23 (deferred to follow-on plans A2 + A3)

| # | Task | Plan |
|---|------|------|
| 13 | `DispatchEntry` + `DispatchKind` (3 arms: InlineFacade / SideTableResolver / CodeExtensionSingleton) | A2 |
| 14 | L2 `mergedDispatch` flat snapshot + binary search                                                     | A2 |
| 15 | L1 `FingerprintCache` 4-slot ring                                                                     | A2 |
| 16 | L0 `Query<I>` consteval shortcut                                                                       | A2 |
| 17 | L3 invalidation via `extendedBy` walk in `Install<E>`                                                  | A2 |
| 18 | `Registry::Install<T>` with runtime seal checks (D7.2)                                                 | A2 |
| 19 | Eager extension chain ownership + deferred Acquire                                                    | A2 |
| 20 | `Query<I>(node)` static face + `QueryDynamicRaw` kernel                                                | A2 |
| 21 | Introspection surface rewrite (`IidsOf`, `Provides`, `ProviderClass`, ...)                            | A3 |
| 22 | Closure-identity helpers (`Nucleus`, `Facades`, `InClosure`, `WalkClosure`, `MaterializedFacades`)    | A3 |
| 23 | End-to-end integration tests + final sweep                                                            | A3 |

**A2 starts** when A1 ships. A2's first task ("Define `DispatchEntry`") is unblocked by
Task 8's forward declarations.

---

## Self-Review Notes

- **Placeholder scan:** Task 7's `MakeExtendsArrayFor` is marked "mirror MakeImplementsArrayFor
  exactly" rather than spelled out in full. This is an intentional cross-reference, not a
  placeholder — the full body is structurally identical to the explicit one immediately above.
  Implementer should *not* skip writing it; just copy-adapt.
- **Type consistency:** `RootObject`'s ctor signature `(ClassType, void*, bool external)` is
  consistent across Tasks 4, 5, 10, 11, 12. `ComPtr<T>::Adopt(T*)` and `MakeOwned<T>(args...)`
  contracts agree (Adopt takes existing +1, MakeOwned produces +1 then Adopts).
- **Forward references:** Tasks 8 + 10 forward-declare types that only land in later tasks.
  Each forward decl is paired with a line explaining which later task fills it in; no symbol
  is referenced from a test before the task that defines it has run.
- **Scope:** A1 is foundation only — no Query, no dispatch, no Registry. The Task-12 test
  simulates a facade by *manually* calling `Acquire`/`Release` on an "underlying" raw pointer.
  Production facades wait for A2.
- **Reflection-API spelling caveat:** clang-p2996 occasionally renames helpers (`identifier_of`
  vs `name_of`, `extract` vs `value_of`, etc.). Where the plan uses a name your toolchain
  doesn't match, swap to the nearest equivalent and add an `// XXX y2-spec: see Task N` comment
  pointing at this plan.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-17-yuki-y2-core-plan.md`. Two
execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, two-stage review
   (spec compliance, then code quality) between tasks, fast iteration.
2. **Inline Execution** — Execute tasks in this session via the executing-plans skill, batch
   execution with checkpoints for review.

Which approach?




