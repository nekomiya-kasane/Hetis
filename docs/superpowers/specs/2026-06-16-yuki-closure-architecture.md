# Yuki Closure Architecture & Hot-Loadable Dispatch — Design Spec

Status: Draft for review
Scope: `Yuki/Core/{Identity,MetaClass,RootObject,FacadeList,InterfaceFacade,Query,Diagnostics}.h` and their `.cpp`/test counterparts; new headers `Yuki/Core/{Registry,Introspection,Manifest,Discovery}.h`. Also touches `Mashiro/Core/ConcurrentSlabArena.h` only as a consumer.

## 1. Semantics: the closure of an implementation

### 1.1 Vocabulary

- **Nucleus** — the canonical `Implementation` instance reached by walking `Underlying` until the role is no longer `Extension`. Every closure has exactly one nucleus.
- **Closure** — the directed multigraph rooted at a nucleus `N`, containing:
  - `N` itself,
  - every `Extension` instance `E` such that the transitive closure of `RT::Extendee` from `E` reaches `N` (equivalently, `RT::Nucleus(E) == N`),
  - every facade instance attached to a member of the closure (an `InterfaceFacade<I, M>` whose `target_` points into the closure).
- **Capability set** of the closure: the union of interface ids implemented by `N` and by any extension `E` in the closure. Implementations and extensions are first-class providers of capabilities; facades are *views* onto a provider.
- **Provider** of `I` for closure rooted at `N`: the unique member of `{N} ∪ {extensions of N}` whose static `implements` list contains `I`. The closure is well-formed only if this provider exists and is unique.
- **M:N**:
  - one implementation may be extended by many extensions (1:N on the `extends` edge),
  - one extension class may extend many implementations (N:1 by enumerating `Anno::Extends`),
  - the relation between *capability* and *provider* is M:N across the closure.

### 1.2 RT operations — full surface

All free functions live in `namespace Yuki::RT`. All pointer-returning operations accept `nullptr` and return `nullptr` for it. The kernel `Detail::QueryDynamicRaw` is internal; `RT::QueryDynamic` is the typed public form.

**Navigation (pre-existing; preserved for compatibility):**

| Operation | Domain → Codomain | Notes |
|-----------|-------------------|-------|
| `Underlying(p)` | facade ↦ target; impl/ext ↦ self | one-step de-facade |
| `Target(p)` | facade ↦ target; non-facade ↦ nullptr | strict variant of `Underlying` |
| `Extendee(p)` | extension ↦ extendee; other ↦ nullptr | one-step de-extension |
| `Facades(p)` | impl ↦ `FacadeListHead*`; other ↦ nullptr | per-closure facade list head |
| `Nucleus(p)` | any closure node ↦ nucleus | iterative `Underlying` + `Extendee` |

**Query (the workhorse):**

| Operation | Returns | Materializes? |
|-----------|---------|---------------|
| `Query<I>(p)` | `I*` (best facade view of `I`) | yes — triggers Lazy resolvers |
| `Materialized<I>(p)` | `I*` or nullptr — only if already resolvable without materialization | no |
| `QueryDynamic(p, iid)` | `RootObject*` (interface arm) — typed-erased materializing | yes |
| `MaterializedDynamic(p, iid)` | `RootObject*` or nullptr | no |

`Query` and `Materialized` differ only in whether the `SideTableResolver` invokes its lazy body; `DirectCast`/`InlineFacade`/`CodeExtensionSingleton` answer identically under both. The non-materializing variants exist for diagnostics, capability probing, and safe iteration.

**Provider identification (who answers a query):**

| Operation | Returns |
|-----------|---------|
| `Provider<I>(p)` | `RootObject*` — `Underlying(Query<I>(p))`; the implementing instance |
| `ProviderClass<I>(m)` | `const MetaClass*` — class of the provider, no instance needed |
| `ProviderDispatchKind<I>(m)` | `std::optional<DispatchKind>` — which path resolves `I` |

`ProviderClass`/`ProviderDispatchKind` consult the metaclass's dispatch snapshot only; they do not require a closure instance.

**Potential attributes — metaclass-level (what a closure *can* expose):**

| Operation | Returns |
|-----------|---------|
| `Capabilities(m)` | range of `Iid` — every iid the dispatch snapshot covers |
| `Provides<I>(m)` | `bool` — runtime form (looks up dispatch snapshot) |
| `Provides<I, C>()` | `bool` `consteval` — compile-time form (reflection over `C`) |
| `Extensions(m)` | range of `const MetaClass*` — every extension registered against `m` (read of `m.links.extendedBy`) |
| `EagerExtensions(m)` | range of `const MetaClass*` — the subset of `Extensions(m)` materialized at nucleus construction |
| `Implementations(im)` | range of `const MetaClass*` — for an interface metaclass, every implementation/extension whose dispatch snapshot covers it (read of `im.links.implementedBy`) |

These are pure metaclass introspection; given just a `const MetaClass&`, callers can describe the *potential* shape of any closure of that class without holding an instance.

**Runtime attributes — instance-level (what a closure *currently* exposes):**

| Operation | Returns |
|-----------|---------|
| `Has<I>(p)` | `bool` — `Materialized<I>(p) != nullptr`; true unconditionally for `DirectCast`/`InlineFacade`/`CodeExtensionSingleton` |
| `IsMaterialized<E>(p)` | `bool` — extension-typed test; true iff an `E` instance currently resides in the closure |
| `MaterializedExtensions(p)` | range of `RootObject*` — extensions currently in `n→facades_` (filtered to `ClassType::Extension`) |
| `MaterializedFacades(p)` | range of `RootObject*` — every node in `n→facades_` regardless of role |
| `Cardinality(p, iid)` | `std::size_t` — 0 or 1 for closure-managed slots; ≥0 for unrecorded user attachments |

**Structural tests / iteration:**

| Operation | Returns |
|-----------|---------|
| `InClosure(node, root)` | `bool` — `Nucleus(node) == Nucleus(root)`, with `nullptr`-safety |
| `WalkClosure(root, visitor)` | `void` — invokes `visitor(RootObject*, ClassType)` on nucleus + every materialized facade |

**Explicit materialization:**

| Operation | Returns |
|-----------|---------|
| `Reify<E>(p)` | `E*` — force-create `E` in `p`'s closure if absent; idempotent. Returns the existing or fresh instance |

`Reify` is the public form of the resolver's materialization path. Use cases: an author marked `E` as `Anno::Lazy` but a consumer wants to pre-warm; tests that want to exercise the materialized state without probing every interface `E` provides.

### 1.3 Convenience derivations

Three identities hold and are documented so consumers can compose:

- `Provider<I>(p) == Underlying(Query<I>(p))`.
- `Has<I>(p) == (Materialized<I>(p) != nullptr)` — and for `DirectCast`/`InlineFacade`/`CodeExtensionSingleton` this is structurally `true`.
- `InClosure(a, b) == (Nucleus(a) == Nucleus(b))`.

The naming convention is uniform: `Query`/`Provider`/`Has` are interface-keyed; `IsMaterialized`/`Reify` are extension-keyed; `Capabilities`/`Extensions`/`Implementations` are metaclass-keyed and return ranges.

### 1.4 The four scenarios collapse into one walk

Given any starting node `p` and target interface `I`:

1. `n := Nucleus(p)` — one tight loop, never recurses; tagged-pointer decoding is a shift + mask, no vcall.
2. `entry := lookup(n→MetaClassDynamic().links.dispatch, I)` — single atomic-snapshot load + small ordered table search.
3. The `DispatchKind` of the entry directs the remaining steps:
   - `DirectCast` → `static_cast<I*>(n)` (provider is the nucleus).
   - `InlineFacade` → reflect onto the nucleus's NSDM that is an `InterfaceFacade<I, _>` and return its address.
   - `CodeExtensionSingleton` → return the global stateless-extension facade for `(I, N's class)`.
   - `SideTableResolver` → walk `n→facades_` (the closure's per-instance `FacadeList`) for `(iid == I)`; missing means no live stateful extension; the registrar materializes one on demand.
   - `FacadeList` → reserved slot indicating "this iid is expected to be attached at runtime"; look up `n→facades_` for the matching node. Distinct from the unrecorded fallback only in that misses are explicit "not yet attached" rather than "no such capability".

`Provider(p)` returns `Underlying(Query<I>(p))`; for `DirectCast`/`InlineFacade` it is `n`; for `SideTableResolver` it is the per-closure live `Extension` instance; for `CodeExtensionSingleton` it is the process-global stateless extension instance.

### 1.5 Cardinality and instantiation policy

**Cardinality.** For any closure `C` rooted at nucleus `N` and any Extension class `E`, the closure contains **at most one** instance of `E`. This is a structural invariant, enforced by routing all `SideTableResolver` resolutions through the closure's shared `N→facades_` list with iid-keyed deduplication: the resolver checks for a node matching `IidOf<E>`, returns it if present, materializes-then-attaches if absent. The check-and-attach is a single CAS on the list head (Vyukov-style).

For `CodeExtensionSingleton` the invariant is trivially satisfied: a process-global singleton is shared by every closure.

For an Extension class `E` declaring `Extends{N1, N2, ...}` (one extension extending multiple implementations): each `(E, Ni)` pair is a *distinct* `E` instance, one per `Ni`-rooted closure. There is no cross-closure sharing of stateful extension instances.

**Instantiation timing.** Extensions and Interfaces both distinguish two modes:

- `Anno::Eager` — materialized at *closure construction time* (in the nucleus's `MetaNode<T>` default constructor, via a registrar-generated hook).
- `Anno::Lazy` — materialized at *first query* (on the first `Query` that resolves to this capability).

Defaults (annotation may be omitted):

| Provider category | Default mode | Rationale |
|-------------------|--------------|-----------|
| Stateless Extension (`CodeExtensionSingleton`) | Eager | A process-global singleton has no per-closure cost; deferring its construction buys nothing. |
| Stateful Extension (`SideTableResolver`) | Lazy | Materialization may be expensive; defer until the capability is actually used. |
| Cold inline Interface facade (NSDM of nucleus) | Eager (by storage) | Lives in the nucleus's own layout; lifetime begins with the nucleus. |
| User-attached Interface facade (`FacadeList` kind or unrecorded) | Lazy (by attachment) | Comes into existence only when user code calls `Attach`. |
| BOA Interface (`DirectCast`) | Eager (by inheritance) | Inseparable from the nucleus. |

Authors may override the Extension default with `Anno::Eager` / `Anno::Lazy` on the Extension class.

**Where interfaces live (storage decision).** *Mixed-but-consolidated*:

- Capabilities owned by the nucleus directly (`DirectCast`, `InlineFacade`) live in the nucleus's own layout — no list traversal, no allocation; the dispatch entry encodes a static offset.
- Every other interface facade — whether produced by an Extension or attached by user code — lives in the closure's single shared `facades_` list rooted at the nucleus.

This consolidation has three consequences worth noting:

1. **One sweep on destruction.** When the nucleus dies, one walk over `facades_` drops every Extension instance and every user-attached facade in the closure. A tree-of-lists ("each Extension owns its interface facades") would require recursing the extension graph and is rejected for that reason.
2. **Logical vs. storage ownership.** An Extension class `E` *logically* owns the capability `I` it provides; the resolver function captured in the dispatch entry knows which Extension to materialize. But the *storage* of the materialized facade (and the `E` instance it embeds) is the closure's shared list. Diagnostics that report "who provides `I`" use the dispatch entry's resolver identity, not the list location.
3. **Single-step lookup on the hot path.** `QueryDynamicRaw(p, iid)` does one binary search on the dispatch snapshot, one switch, and at most one list walk (the resolver's `facades_` lookup). There is no "find provider, then find facade" two-hop.

The rejected alternative — "interface lives under its provider" — would split storage between the nucleus (BOA/inline) and per-Extension sublists. This forces every dynamic query to first identify the provider, then walk that provider's sublist; it doubles the read-path constant factor and complicates the destruction sweep. The consolidated form gives up nothing in terms of ownership semantics (the dispatch entry carries provider identity) and is strictly cheaper at the hot path.

## 2. Dispatch storage under DLL hot-load

### 2.1 What changes vs. the current code

Today: `MetaLinks::dispatch` is `std::span<const DispatchEntry>` — a fixed view valid only for entries known at the metaclass's translation unit. This cannot accept extensions introduced by a DLL loaded after the metaclass was instantiated.

Change: introduce an immutable **DispatchSnapshot**, owned by the metaclass via an `std::atomic<const DispatchSnapshot*>`. Readers do an acquire-load and treat the snapshot as immutable for the lifetime of their query. Writers (registrars) install a new snapshot under a per-metaclass mutex with release semantics; the old snapshot is retired (kept alive until the global epoch advances; see 2.3).

```cpp
struct DispatchSnapshot {
    std::size_t                          count;
    const DispatchEntry*                 entries;  // sorted by iid for binary search
    const DispatchSnapshot*              previous; // for epoch retirement
};

struct MetaLinks { /* ... */
    std::atomic<const DispatchSnapshot*> dispatch; // replaces std::span
    // extendedBy/implementedBy keep their meaning; also become atomic snapshots.
};
```

`DispatchEntry` keeps its shape (`iid`, `kind`, `payload`). The five kinds map to four payload variants (FacadeList carries no payload — the lookup is always `n→facades_`):

- `DirectCast`             → `payload.staticOffset` (already used).
- `InlineFacade`           → `payload.staticOffset`.
- `CodeExtensionSingleton` → `payload.singleton` — `RootObject* const*` indirection to a process-global facade for a stateless code extension.
- `SideTableResolver`      → `payload.resolver` — `RootObject* (*)(RootObject* nucleus) noexcept`; the registrar that materializes or finds a stateful extension's facade in `n→facades_`.
- `FacadeList`             → no payload; signals "an attached facade is expected at runtime; read `n→facades_`". Used when the *closure declares statically* that an iid will be supplied later by user attachment but wants the snapshot to reserve the slot so misses become explicit "not yet attached" rather than ambiguous fallbacks.

### 2.2 Read path

```cpp
const DispatchSnapshot* s = links.dispatch.load(std::memory_order_acquire);
// binary_search over s->entries by iid; entries are stable for the life of *s
```

Zero allocation on the read path. Branch on `entry.kind` is a small switch; the fast `DirectCast` arm is a single add. The static face (`Query<I, C>(C*)`) keeps folding to a cast when `derived_from<C,I>` regardless of DLLs — the dynamic table is only consulted when the static face cannot decide.

### 2.3 Write path (DLL load) and retirement

- Each metaclass `M` holds a `Mashiro::SpinMutex` (writers serialize) and the snapshot pointer (readers atomic-load).
- `Registry::Install(MetaCore& m)` builds the delta and CAS-publishes a new snapshot whose `previous` chains to the old one.
- Snapshots are retired through a process-wide quiescent-state list. A reader's `acquire`-load pins a snapshot for its (very short) query; once all in-flight queries observed at install time have completed, retired snapshots are freed by a maintenance pass on the next install. This is RCU-by-epoch, not RCU-by-thread; queries are leaf operations and naturally short.
- No unload: DLLs may be loaded but not unloaded. This is a deliberate scoping decision — it lets us retire snapshots safely without per-thread bookkeeping.

### 2.4 Per-instance `FacadeList`

Already exists (`FacadeListHead`/`FacadeNode`). The closure design reuses it unchanged for `SideTableResolver` and user-attached `FacadeList`. Allocation continues through `Mashiro::ConcurrentSlabArena` (one slab per process; nodes are 4 words).

## 3. Registration API & DLL hot-load

### 3.1 The two annotations already drive everything

Reflection on `^^T` extracts:
- the role (`Anno::Implementation`/`Interface`/`Extension`/`Bridge`),
- `Anno::Implements{...}`/`Anno::Extends{...}` lists,
- the `Iid` per `Anno::Iid` override or hashed type id,
- the kind discriminator below.

### 3.2 DispatchKind selection (compile-time)

For an Implementation class `C` and an interface `I` in its closure capability set, the kind is chosen at the *registrar* of the *provider*. The Eager/Lazy column reflects defaults; explicit `Anno::Eager`/`Anno::Lazy` on the Extension overrides.

| Provider of `I` | Stateful? | Default mode | DispatchKind |
|-----------------|-----------|--------------|--------------|
| `C` itself, `C : I` (BOA, hot) | n/a | Eager (inheritance) | `DirectCast` |
| `C` itself, NSDM `InterfaceFacade<I, C>` (cold inline) | n/a | Eager (NSDM)       | `InlineFacade` |
| Extension `E` with `Extends C` (or one of `C`'s bases), `E` stateless | no  | Eager (singleton)  | `CodeExtensionSingleton` |
| Extension `E` with `Extends C`, `E` stateful                          | yes | Lazy (resolver)    | `SideTableResolver` |

**Stateless discriminator (replaces the obsolete `sizeof(E) <= 1`)**:

```cpp
template <ExtensionClass E>
concept StatelessExtensionClass = []() consteval {
    // No non-inherited non-static data members beyond the anchor.
    constexpr auto own = std::meta::nonstatic_data_members_of(^^E, std::meta::access_context::current());
    // Filter members declared in E (skip those introduced by the anchor base).
    std::size_t ownCount = 0;
    template for (constexpr auto m : own) {
        if (std::meta::parent_of(m) == ^^E) ++ownCount;
    }
    return ownCount == 0;
}();
```

Equivalent runtime check (cheap sanity assert): `sizeof(E) == sizeof(Detail::ExtensionAnchorBase)`. The size form is used only for diagnostic assertions; the concept (reflection) is the source of truth.

### 3.3 Registrar (CRTP, runs once at static init or DLL load)

```cpp
namespace Yuki::Detail {
    template <class T>
    struct Registrar {
        Registrar() noexcept {
            if (Yuki::Registry::AlreadyInstalled(MetaCoreOf<T>.iid())) return;
            Yuki::Registry::Install<T>();   // emits dispatch delta(s)
        }
    };
}
```

`Registry::Install<T>()` runs a reflection-driven body that:

1. For Implementations: emits entries for each `I` in the closure of `Anno::Implements` over `T` and its OM-inherited bases — `DirectCast` for hot BOA, `InlineFacade` for cold inline facades. Publishes one new snapshot for `MetaClassOf<T>`.
2. For Extensions: walks `Anno::Extends{B1, B2, ...}`. For each base `B` and each `I` in `implements(E)`, locates `MetaClassOf<B>` and publishes a new snapshot adding `(I → CodeExtensionSingleton | SideTableResolver)` to `B`'s dispatch table. The singleton or the resolver function pointer is generated by the registrar. **Dependency contract:** an Extension's TU/DLL must link against the TU/DLL declaring each `B` it extends, so `MetaClassOf<B>` is present when the extension's registrar runs. Out-of-order registration is not supported and asserts in debug.
3. Idempotent: keyed on `iid`. Re-install is a no-op.
4. Thread-safe: per-metaclass writer mutex; snapshot publish under release; retirement via 2.3.
5. **Eager-set bookkeeping.** Each Implementation metaclass also carries an immutable, atomically-published list of *eager extensions for closures of this implementation* (an `EagerSet`). Stateful eager Extensions append themselves at install time; stateless eager Extensions need no per-closure presence and so are omitted from the set (the singleton suffices).

**Closure construction hook.** The `MetaNode<T>` default constructor, after laying down the anchor base and arming the payload tag, performs a `for each E in MetaClassOf<T>.eagerSet: E::MaterializeInto(*this)` pass. `MaterializeInto` is a static member generated by the Extension's registrar that constructs an `E` instance, embeds it in a `FacadeNode`, and CAS-attaches it under `this->facades_` keyed by `IidOf<each I in implements(E)>`. The pass is sequential within a single nucleus's construction — there is no concurrent writer to a brand-new nucleus's list. The lazy path uses the same `FacadeNode` shape, so a subsequent `SideTableResolver` lookup is structurally identical for eager and lazy.

The registrar is instantiated by an inline static member of the CRTP base, ODR-used through a constexpr address-take in the same base:

```cpp
template <class T, class Base = void>
struct MetaNode : /* ... */ {
    inline static Detail::Registrar<T> _registrar{};
    static constexpr void* _registrar_anchor = static_cast<void*>(&_registrar); // ODR-uses _registrar
};
```

The `inline static` ensures a single definition across translation units (including DLLs — the registrar is keyed on `iid`, so duplicate construction across DLL boundaries deduplicates inside `Install`). The constexpr address-take guarantees instantiation even when no other member of the CRTP base is touched. No compiler-specific attributes are needed.

### 3.4 DLL hot-load flow

1. DLL is `LoadLibrary`'d. Its `Registrar<T>` static objects construct.
2. Each one calls `Registry::Install<T>()` which CAS-publishes new snapshots on the relevant metaclasses (both the new type's own and the metaclasses of any types it extends, if those metaclasses exist in the host).
3. Cross-DLL identity coherence is by `Iid`. The metaclass for an interface `I` defined in the host is the one published in the host; a DLL that re-declares `I` must use the same `Iid` (annotation override) or be detected as a duplicate by hash.
4. Readers observing an old snapshot continue to be correct; subsequent queries see the new snapshot.

### 3.5 What `extendedBy`/`implementedBy` become

Both fields in `MetaLinks` move from declared-but-unpopulated to `std::atomic<const ClassSnapshot*>` (a flat list of `MetaCore*`). They are populated symmetrically by the registrar at install time. They are read by diagnostics (`IsAKindOf` in reverse, `Print`) and by reflection-of-the-graph users; they are not on the query hot path.

## 4. Query / Provider data flow

### 4.1 Static face

`RT::Query<I>(C* p)` where `C` is the concrete static type:

```cpp
template <Anno::InterfaceClass I, class C>
[[nodiscard]] constexpr auto Query(C* p) noexcept {
    if constexpr (std::derived_from<C, I>) {
        // BOA hot path — folds to a cast, no table lookup.
        return static_cast<I*>(p);
    } else if constexpr (Detail::HasInlineFacadeFor<C, I>) {
        // Cold inline facade — reflect the NSDM that is InterfaceFacade<I, C> (or InterfaceFacade<I, base of C>),
        // return its address. Compile-time offsetof via reflection.
        return Detail::InlineFacadeAddress<I, C>(p);
    } else {
        // No statically known capability — fall through to dynamic.
        return static_cast<I*>(RT::QueryDynamicRaw(p, IidOf<I>));
    }
}
```

`HasInlineFacadeFor<C, I>` is a `consteval` predicate that scans the NSDMs of `C` (via reflection) for a member whose type is `InterfaceFacade<I, X>` with `X` a base of (or equal to) `C`. `InlineFacadeAddress<I, C>` returns `&(p->theMember)` via the reflected member.

### 4.2 Dynamic kernel

```cpp
namespace Yuki::RT {
    [[nodiscard]] RootObject* QueryDynamicRaw(RootObject* p, Iid id) noexcept {
        RootObject* n = Nucleus(p);
        if (!n) return nullptr;

        const auto& links = n->MetaClassDynamic().links();
        const DispatchSnapshot* s = links.dispatch.load(std::memory_order_acquire);
        const DispatchEntry* e = Detail::LookupEntry(s, id); // binary search by iid

        if (e) {
            switch (e->kind) {
                case DispatchKind::DirectCast:
                    return reinterpret_cast<RootObject*>(reinterpret_cast<std::byte*>(n) + e->payload.staticOffset);
                case DispatchKind::InlineFacade:
                    return reinterpret_cast<RootObject*>(reinterpret_cast<std::byte*>(n) + e->payload.staticOffset);
                case DispatchKind::CodeExtensionSingleton:
                    return *e->payload.singleton;       // stable, process-wide
                case DispatchKind::SideTableResolver:
                    return e->payload.resolver(n);      // walks/materializes facades_
                case DispatchKind::FacadeList:
                    return Detail::FacadeListLookup(n->facades_, id); // user-attached only
            }
        }
        // Fallback: user-attached facade with no matching dispatch entry.
        return Detail::FacadeListLookup(n->facades_, id);
    }
}
```

`Provider` is one line:

```cpp
namespace Yuki::RT {
    template <Anno::InterfaceClass I>
    [[nodiscard]] RootObject* Provider(RootObject* p) noexcept { return Underlying(Query<I>(p)); }
}
```

### 4.3 Scenarios from §1 walk-through

Let `N` = nucleus, `E1, E2` = extensions of `N`, `IE1, IE2` = interfaces implemented by `E1, E2`, `IN` = interface implemented by `N`.

| Starting `p` | Target `I` | `Nucleus(p)` | Provider | Dispatch kind | Result |
|--------------|------------|--------------|----------|---------------|--------|
| Facade view of `E1` exposing `IE1` | `IN` | `N` | `N` | `DirectCast`/`InlineFacade` | view of `N` as `IN` |
| `E1` instance | `IE2` (sibling) | `N` | `E2` (materialized on demand) | `SideTableResolver` | view of `E2` as `IE2` |
| `N` instance | `IE1` | `N` | `E1` (materialized on demand) | `SideTableResolver` (or singleton if stateless) | view of `E1` as `IE1` |
| Facade view of `N` exposing `IN` | `IE1` | `N` | `E1` | `SideTableResolver` | view of `E1` as `IE1` |

All four reduce to a single `Nucleus → snapshot lookup → kind switch` pipeline. No special cases for "facade-over-extension" or "extension-over-facade" — `Nucleus` already normalizes the start point.

## 5. Test plan, performance, debt, and roadmap

### 5.1 Test plan (Catch2, x64-asan + ubsan)

New file `Yuki/tests/Core/ClosureTest.cpp`:

- **Closure topology**: nucleus is unique; `Nucleus(facade(ext(impl))) == impl`; extensions on disjoint impls have disjoint closures.
- **Capability set**: every `Anno::Implements` of `N` and of every extension of `N` resolves via `Query`; nothing else does.
- **Four scenarios** from §4.3 — one `SECTION` per row.
- **Stateless vs stateful extension**: registrar picks `CodeExtensionSingleton` for empty-NSDM extensions; `SideTableResolver` otherwise. Verify the singleton is one address across instances; verify the resolver materializes at most one facade per `(nucleus, iid)`.
- **Cardinality**: for any stateful Extension `E` and nucleus `n`, repeated `Query<I>(p)` for any `I` provided by `E`, from any starting node in the closure, returns a facade whose `Underlying` is the same `E` address — i.e., exactly one `E` instance per closure. A two-thread parallel race that triggers materialization must still result in one address (the losing thread observes the winner's node via the CAS).
- **Eager vs lazy materialization**: an `Anno::Eager` stateful Extension is observable in `n→facades_` immediately after the nucleus is constructed (no query needed); an `Anno::Lazy` stateful Extension is absent from `n→facades_` until the first matching `Query`. The `Anno::Eager` default for stateless and the `Anno::Lazy` default for stateful are exercised without explicit annotation.
- **Cross-closure isolation**: an Extension class `E` that extends two implementations `N1` and `N2` produces a distinct `E` instance per closure; mutating state on the `N1`-closure's `E` is invisible to the `N2`-closure's `E`.
- **DLL hot-load simulation**: rather than a real DLL, drive `Registry::Install<T>()` in a second TU compiled separately and link-loaded after first queries have run. Verify pre-install queries miss; post-install queries hit; pre-install snapshot pointers (captured by a paused reader) remain valid until the next install retires them.
- **Concurrent install vs. query**: TBB parallel-for of queries while another thread installs; asan + TSan run; require no torn reads (snapshot pointer is atomic).
- **Identity coherence**: an interface declared with an overridden `Anno::Iid` produces the same `Iid` across TUs/DLLs; the dispatch entry inserted by a foreign-DLL extension's registrar lands in the same slot as a host query for that interface.

Extend `Yuki/tests/Core/QueryTest.cpp` with the four scenario cases per the user's original request — they share fixtures with ClosureTest where convenient.

### 5.2 Performance

- Read path is one acquire load + one binary search + one switch. The `DirectCast` arm is a single offset add. The static face still folds to a cast and never touches the table.
- Writes are O(log n) inside a per-metaclass mutex; snapshots are immutable so no reader synchronization beyond the acquire load.
- Memory: each metaclass holds one live snapshot plus retired snapshots until the next install pass frees them. Retired snapshots are small (`{count, entries*, previous*}` plus the entries themselves, allocated together).
- `FacadeList` per closure is unchanged.

### 5.3 Cleanup of current debt

- `MetaLinks::dispatch` becomes atomic snapshot (replace `std::span`).
- `MetaLinks::extendedBy`/`implementedBy` populated.
- `Identity.h`: `StatelessExtensionClass` rewritten with the reflection-based definition (§3.2); `sizeof <= 1` removed. `Anno::Eager`/`Anno::Lazy` added as Extension/Interface annotations.
- `Query.h`: dispatch enum already has `CodeExtensionSingleton`/`SideTableResolver` — the registrar finally populates them.
- `Diagnostics.h`: `Print`/`Format` (Request C, deferred) will read the populated `implementedBy` and the new introspection surface (`Capabilities`, `MaterializedExtensions`, `WalkClosure`) to render closure topology — out of scope for this spec but the data becomes available here.

### 5.4 Roadmap (sequence; `‖` marks parallelizable groups)

1. `Identity.h` — replace stateless concept; introduce `Anno::Eager`/`Anno::Lazy`; `IidOf` overload for runtime `MetaCore*` lookup. (foundation)
2. `FacadeList.h` — add `Lookup(head, iid)` reading helper and `AttachUnique(head, node)` (CAS that no-ops if iid already present); no API churn. ‖ with (1).
3. `MetaClass.h` — `DispatchSnapshot` type; atomic dispatch pointer; binary-search lookup; retirement list scaffolding; `EagerSet` atomic-list field.
4. `Registry` (new header `Yuki/Core/Registry.h`) — `Install<T>()`, `AlreadyInstalled(iid)`, writer mutex map keyed by metaclass.
5. CRTP hooks in `MetaNode`/`ExtensionNode`/`IfaceFacadeNode` to instantiate `Detail::Registrar<T>` once per class; `MetaNode<T>` constructor runs the eager-set materialization pass. ‖ with (4) once the API in (4) is stable.
6. `Query.h` — static face uses `HasInlineFacadeFor`; dynamic kernel becomes the §4.2 switch parameterized on a `Materialize` policy; `SideTableResolver` resolver bodies generated by the Extension's registrar do `AttachUnique` then return; `Reify<E>` exposed as the materializing entry point.
7. `Introspection.h` (new header) — `Capabilities`, `Provides`, `Extensions`, `EagerExtensions`, `Implementations`, `Has`, `IsMaterialized`, `MaterializedExtensions`, `MaterializedFacades`, `InClosure`, `WalkClosure`, `ProviderClass`, `ProviderDispatchKind`. Pure views over the dispatch snapshots and `facades_`; no new state.
8. `Manifest.h` (new header) — `ManifestHeader`, `ManifestRecord`, consteval block generating the per-TU manifest array via reflection over annotated types, section-placement attribute, named extern-"C" `YukiRegister_<iid>` wrapper per type. Cross-platform section name macro. ‖ with (7).
9. `Discovery.h` + `Discovery.cpp` (new) — `Registry::AddPluginRoot`, `Registry::DiscoverPlugins` (synchronous + stdexec-returning variants), `CapabilityIndex` data type, manifest reader using `LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE)` / mmap, `.yukicache` read/write. Depends on (8) for the format; depends on (4) for `Install` symbol convention.
10. `Query.h` extension — `QueryOrDiscover<I>(p)` and `ReifyOrDiscover<E>(p)` as `stdexec::sender`-returning coroutine-awaitable forms. Depends on (9).
11. Tests: `ClosureTest.cpp` new; `QueryTest.cpp` extended; introspection scenarios from §6.5 added; `DiscoveryTest.cpp` new covering manifest round-trip, cache hit/miss, on-miss DLL load, sender error paths. ‖ across the scenarios once (10) compiles.

## 6. Diagnostics & introspection — potential and runtime attributes

The RT surface in §1.2 partitions naturally into two views of a closure: **potential attributes** describe what closures of a given metaclass *can* carry; **runtime attributes** describe what a particular closure instance *currently* carries. Both are first-class and exposed through the same naming conventions.

### 6.1 Potential attributes (metaclass-level)

A user holding only a `const MetaClass&` (or a class id) can introspect a closure's full potential without an instance:

```cpp
const MetaClass& m = MetaClassOf<MyImpl>;
for (Iid i : RT::Capabilities(m)) { /* every interface this class can resolve */ }
for (const MetaClass* ext : RT::Extensions(m)) { /* every extension class registered to m */ }
const MetaClass* who = RT::ProviderClass<ISomething>(m);    // nullptr if not provided
auto kind = RT::ProviderDispatchKind<ISomething>(m);        // optional<DispatchKind>
```

These reads consult only the metaclass's atomic dispatch snapshot (and the `extendedBy`/`implementedBy` lists). They are O(log n) on `Capabilities`/`ProviderClass`/`ProviderDispatchKind` (binary search) and O(n) on `Extensions`/`Implementations` (linear scan of the relevant list). They never touch any closure instance.

`Capabilities(m)` is closed under DLL hot-load: after a DLL adds a new Extension that implements `INew`, every subsequent call to `Capabilities(MetaClassOf<TargetImpl>)` includes `IidOf<INew>`. The atomic snapshot guarantees readers see a consistent set.

### 6.2 Runtime attributes (instance-level)

Given a closure node `p` (any role), runtime introspection answers "what is this closure *right now*":

```cpp
RootObject* p = /* ... */;
if (RT::Has<ISomething>(p)) { /* resolvable without materializing */ }
if (RT::IsMaterialized<MyExtension>(p)) { /* MyExtension already lives in this closure */ }
for (RootObject* ext : RT::MaterializedExtensions(p)) { /* live extension instances */ }
RT::WalkClosure(p, [](RootObject* node, ClassType role) { /* visit every member */ });
```

`Has`/`IsMaterialized` are non-mutating: they never trigger a Lazy resolver. This is critical for diagnostics that must not perturb the system under inspection (debugger views, profilers, save-state serializers).

### 6.3 The potential-vs-runtime symmetry

For any interface `I` and node `p`, the relationship is:

| `Provides<I>(m)` (potential) | `Has<I>(p)` (runtime) | Meaning |
|------------------------------|-----------------------|---------|
| true | true | Materialized & ready |
| true | false | Declared but Lazy-not-yet-materialized; a `Query<I>(p)` will materialize and succeed |
| false | false | Closure cannot provide `I` |
| false | true | Impossible — would indicate a torn dispatch snapshot or a corrupted facades list (assert in debug) |

The fourth row gives us a useful invariant: any debug build can assert `Provides<I>(MetaClassOf(p)) || !Has<I>(p)` after every dispatch publish.

### 6.4 Implementation notes

- `Materialized<I>(p)`/`Has<I>(p)`: the dispatch kernel is parameterized on a `Materialize` boolean policy. The `SideTableResolver` arm reads `facades_` first; if absent and `Materialize == false`, it returns `nullptr` without invoking the resolver. Other arms answer identically under both policies (they have no Lazy state).
- `IsMaterialized<E>(p)`: each Extension's `Registrar` stamps a *self-iid* into the Extension's `FacadeNode` at attachment time (in addition to the per-interface iids). `IsMaterialized<E>` is then a single `Lookup(n→facades_, IidOf<E>)`. No reflection on the Extension's interface list is needed at the call site.
- `Capabilities(m)` / `Extensions(m)`: return ranges are views over the metaclass's atomic snapshots; the caller must complete iteration before the next snapshot retirement pass. For long-lived enumerations, materialize into a local container.
- `WalkClosure(root, visitor)`: walks the nucleus's `facades_` exactly once, in attachment order. Visits the nucleus first, then every `FacadeNode`'s underlying object. Visitor return value is `void`; cancellation is not supported in v1.
- `Reify<E>(p)`: implemented by calling the resolver directly with `Materialize == true`; the resolver's idempotent `AttachUnique` (§5.4 step 2) handles concurrent reification races.

### 6.5 Tests

Add to `Yuki/tests/Core/ClosureTest.cpp`:

- **Potential coverage**: `Capabilities(MetaClassOf<C>)` enumerates exactly the iids registered by `C`'s and `C`'s extensions' registrars; after a simulated DLL install adds `IX`, `IidOf<IX>` appears.
- **Non-materializing probe**: build a closure containing a Lazy `E`; assert `IsMaterialized<E>(p) == false`; call `Has<I>(p)` (where `I` is provided by `E`) → still false; call `Query<I>(p)` → triggers materialization; now `IsMaterialized<E>(p) == true`.
- **Reify**: pre-warm with `Reify<E>(p)`; verify a follow-up `Query<I>(p)` returns the same `E` instance (no second materialization).
- **WalkClosure visit order**: nucleus visited first; every materialized extension visited exactly once; no facades visited twice; user-attached facades visited in attachment order.
- **Potential/runtime symmetry**: for randomly assembled closures, the table in §6.3 holds; the impossible row is asserted via debug-only check.

### 6.6 Lifetime of returned views

`Capabilities`/`Extensions`/`Implementations` return ranges backed by atomic snapshots. The snapshot is pinned for the duration of one acquire-load, which is the call itself. Callers that need a stable view across yields or asynchronous boundaries must copy into a local container. `MaterializedExtensions`/`MaterializedFacades` are views over `n→facades_`; that list is monotonically grow-only within a closure's lifetime (no in-list removal — facades die only when the nucleus dies), so iteration is safe without snapshotting.

## 7. Cross-DLL capability discovery

### 7.1 The gap

§2 and §3 cover dispatch snapshots and registrar-driven install. They assume the *provider's DLL is already loaded*. The open question is: what does `Query<I>(p)` do when a provider for `I` exists in a DLL on disk that has not been loaded? Two failure modes:

- **Silent miss.** The query returns nullptr even though a provider exists. Consumers must know in advance which DLLs to load — defeats the purpose of a discoverable capability system.
- **Eager load-everything.** The host loads every DLL it can find at startup. Pays per-DLL load cost for capabilities that 99% of consumers never query.

A discovery layer is needed that is cheap enough to populate by default and precise enough that a `Query` miss can decide "no provider exists" vs "provider exists but is in DLL X".

### 7.2 Two layers, sharply separated

| Layer | Question it answers | Persistence | Cost |
|-------|---------------------|-------------|------|
| **Capability Index** (this section) | "Can a provider for `(iid, target metaclass)` exist in any DLL on disk we know about?" | Manifest scan; cached on disk between runs | One-time per startup or per plugin-root change |
| **Dispatch Snapshot** (§2) | "What is the provider for `(iid, target metaclass)` *right now*, in this process?" | Atomic snapshots per metaclass | One acquire-load per query |

Discovery does not load executable code. Discovery *enables* loading by telling the registry "if you want this capability, load this DLL". The two layers never duplicate each other — the index points to a DLL path + registrar symbol; the snapshot points to the registrar's published entries.

### 7.3 Manifest format

A custom section per DLL — `.yukimani` on PE, `.note.yuki.manifest` on ELF, `__YUKI,__manifest` on Mach-O — holds a flat, position-independent record array followed by a string table:

```cpp
struct ManifestHeader {
    uint32_t magic;              // 'YUKI'
    uint32_t version;             // monotone; readers reject unknown
    uint32_t recordCount;
    uint32_t stringTableOffset;   // bytes from header
    uint32_t stringTableSize;
};

struct ManifestRecord {
    uint32_t recordKind;          // matches ClassType
    Iid      iid;                  // primary identity
    uint32_t flags;                // eager/lazy/stateless/inline/BOA bitfield
    uint32_t nameOffset;           // → string table (UTF-8, NUL-terminated)
    uint32_t implementsOffset;     // → iid array within the section
    uint32_t implementsCount;
    uint32_t extendsOffset;        // → iid array
    uint32_t extendsCount;
    uint32_t registrarSymbolOffset; // → exported function name to call after LoadLibrary
};
```

The whole section is generated by a C++26 `consteval` block that reflects over annotated types in the TU and emits a `[[no_unique_address]] inline constexpr` array placed via `[[gnu::section(".yukimani")]]`/`__declspec(allocate(".yukimani"))`. No runtime code runs to build the manifest; the linker collates per-TU arrays into a single section.

Cross-compiler portability: the format is endian-fixed (little-endian on the platforms we target) and uses `uint32_t` offsets everywhere. `Iid` is a fixed-width 128-bit value already used at runtime.

### 7.4 Discovery scope

Not PATH. The registry has an explicit, host-controlled list of **plugin roots**:

- Default roots: the directory containing the host executable, and a subdirectory `./plugins` next to it.
- `Yuki::Registry::AddPluginRoot(path)` appends; `Yuki::Registry::DiscoverPlugins()` rescans.

DLLs in plugin roots are enumerated with the OS directory API. Each is opened as a **data file**, not as executable code: `LoadLibraryEx(path, LOAD_LIBRARY_AS_DATAFILE)` on Windows; `mmap` + ELF/Mach-O section parse on POSIX. The manifest section is read; the file is closed. No DllMain, no static constructors, no symbol resolution.

Per-DLL discovery cost: one open + one section read ≈ a few hundred microseconds. Typical plugin directory has 10–100 DLLs → 1–100 ms total. Caching (see 7.6) drops warm-start scans to a single mtime check.

### 7.5 CapabilityIndex shape

```cpp
struct CapabilityIndex {
    // key = (target metaclass iid, provided interface iid) → location to load from
    // for Implementations: target metaclass iid == own iid
    // for Extensions:      target metaclass iid == each extendee's iid (one entry per extendee)
    flat_hash_map<std::pair<Iid, Iid>, ProviderLocation> entries;
};
struct ProviderLocation {
    std::filesystem::path dllPath;
    std::string           registrarSymbol;  // exported "C" function: void YukiRegister_<mangled>()
    uint32_t              flags;            // eager/lazy/etc from manifest
};
```

The index is built once per `DiscoverPlugins()` call. It is the *negative-cache eliminator* for queries: a query that misses both the dispatch snapshot and the index is definitively unprovidable.

### 7.6 Disk cache

A `.yukicache` file in each plugin root stores `(dll_path, mtime, manifest_bytes)`. On rescan, if every DLL's mtime matches the cache, the index is rebuilt from cached manifest bytes — no I/O on the DLLs themselves. Cache invalidates per file on mtime mismatch. Cache format is the same flat record format as the section; the cache is the union of all sections from one root, prefixed with `(path, mtime)` per source DLL.

### 7.7 Query integration

Two query forms, separated by whether the caller will tolerate a DLL load:

- `RT::Query<I>(p)` and `RT::QueryDynamic(p, iid)` — synchronous, snapshot-only. Behavior unchanged from §4. If the dispatch snapshot misses, return nullptr. **The capability index is not consulted** — these are the hot path; we do not pay even an index lookup here.

- `RT::QueryOrDiscover<I>(p) -> stdexec::sender<I*>` — opt-in asynchronous form. On dispatch miss, consults the `CapabilityIndex` for `(MetaClassOf(p).iid, IidOf<I>)`. If found, performs `LoadLibrary(location.dllPath)`; the DLL's `Registrar<T>` statics fire, publishing snapshots; the sender then retries the synchronous query and completes with the typed result. If the index also misses, completes with nullptr. Errors (load failure, registrar symbol missing) complete the sender with the OS error.

Choosing async for the discovery path is deliberate: DLL load can take tens of milliseconds on cold I/O; consumers that opt in get parallelism via stdexec, and the hot path stays branch-free of any discovery logic. Coroutines fall out naturally:

```cpp
co_await RT::QueryOrDiscover<IRenderer>(scene);
```

`Reify<E>` gets the same treatment: `RT::ReifyOrDiscover<E>(p) -> stdexec::sender<E*>` for explicit pre-warm across DLL boundaries.

### 7.8 Registrar symbol convention

Each manifest record's `registrarSymbol` names an extern-"C" function exported by the DLL: `void YukiRegister_<mangled_iid_hex>() noexcept`. When the discovery layer triggers a load, it calls this symbol explicitly rather than relying on static-init ordering. Two benefits:

- Static-init ordering across DLLs is brittle; an explicit call is deterministic.
- The DLL can also be statically linked (its registrars run as static-init); the explicit symbol is a no-op in that case (it asserts `AlreadyInstalled` and returns).

The CRTP base in §3.3 emits both the static-init `Registrar<T>{}` and the named extern-"C" `YukiRegister_<iid>` wrapping the same `Install<T>()` body. Same code, two entry points.

### 7.9 Honest assessment of your original proposal

| Proposal element | Verdict | Replacement |
|------------------|---------|-------------|
| Burn metadata into a special DLL section | **Adopted.** Section name + manifest format defined in 7.3. | — |
| Burn `MetaClass` directly | **Rejected.** MetaClass holds atomics, function pointers, self-references — cannot be serialized as-is. | Burn a flat manifest; reconstruct MetaClass on DLL load. |
| Scan all DLLs on PATH at startup | **Rejected.** Wrong scope (PATH has 500+ unrelated DLLs); 2.5–5s startup tax for unused capability. | Explicit plugin roots; default to exe dir + `./plugins`. |
| Build complete capability graph at startup | **Partial.** A complete *index* is cheap with caching; eagerly *loading* every plugin is not. | Index eagerly; load lazily on first miss via `QueryOrDiscover`. |
| Discover ⇒ load ⇒ query | **Adopted with separation.** Discovery (manifest scan, no code) is separate from loading (registrar fires). | Two layers in 7.2. |

### 7.10 Out of scope (for this spec)

- **Signed manifests.** A malicious DLL in a plugin directory can lie about its capabilities. Signature verification is an orthogonal concern; the manifest format reserves a header field for a future signature block.
- **Hot rescan.** A file-system watcher that re-runs `DiscoverPlugins()` when a plugin directory changes is a follow-up — the index supports incremental update but the watcher is not in v1.
- **Cross-process index sharing.** Each process builds its own index. A shared on-disk index (per machine) is feasible but adds locking complexity; deferred.
- **Versioned interfaces.** If `I` evolves, the manifest carries one iid per version; the index treats them as distinct capabilities. No automatic version negotiation in v1.

## Appendix A — DispatchEntry payload layout

```cpp
struct DispatchEntry {
    Iid               iid;
    DispatchKind      kind;
    union Payload {
        std::ptrdiff_t        staticOffset; // DirectCast, InlineFacade
        RootObject* const*    singleton;    // CodeExtensionSingleton
        RootObject* (*resolver)(RootObject* nucleus) noexcept; // SideTableResolver
        // FacadeList: payload unused; lookup is always n->facades_
    } payload;
};
```

Entries are sorted by `iid` so lookup is `std::ranges::lower_bound` on the snapshot's contiguous buffer.

## Appendix B — Out of scope

- DLL **unload** (deliberately not supported; registrar install is monotone).
- Cross-process identity (`Iid` coherence is per-process).
- Print/Format CPO polish (Request C, deferred to a follow-up spec).
- Async/coroutine integration for capability resolution — `Query` is synchronous and intentionally so; coroutine consumers wrap the synchronous call.

