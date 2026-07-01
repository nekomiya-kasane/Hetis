# Yuki Object Model — Plan 2 (Y2) Design Spec

**Date:** 2026-06-17
**Status:** Approved for implementation planning
**Scope:** Clean refactor of Yuki Core (no Plan 1 compatibility). Object model, dispatch,
lifetime, cross-DLL discovery, serialization.

---

## 0. Goals

Y2 replaces Plan 1's CRTP-stacked `RootObject` family with a flatter, reflection-driven model
that fixes four standing critiques:

1. **No multi-vtable surprises.** `RootObject` is a single concrete C++ base; subclasses inherit
   it exactly once. Mixin bases must be non-polymorphic.
2. **Closure-wide seal annotations.** Three orthogonal seals (`Final`, `Unique`, `Important`)
   carry well-defined per-arm semantics, checked both at consteval and at `Install` time.
3. **Hierarchical monotonic refcount.** Every node manages its own count; facade ≥ underlying,
   extension ≥ extendee is a structural invariant.
4. **Multi-level Query cache.** L0 consteval shortcut + L1 per-`MetaLinks` 4-slot fingerprint +
   L2 flat `mergedDispatch` snapshot + L3 invalidation via `extendedBy`.

Plus two Plan-2 extensions:

5. **Lazy cross-DLL plugin model** with a consteval-generated manifest section.
6. **Reflection-driven closure serialization** with pluggable backends.

---

## 1. Object Model (D0–D6)

### D0. Single polymorphic base
`RootObject` is **non-template**. Any `RootObject` subclass `T` may have at most **one**
polymorphic base (the one in the `RootObject` chain). Additional bases must be non-polymorphic
mixins (no virtual functions, no virtual dtor). The toolchain enforces this via a `consteval`
predicate on the reflected base list and emits a diagnostic naming the offending base.

> Rationale: collapses Plan-1's `MetaNode` / `IfaceFacadeNode` / `ExtensionNode` chain into a
> single inheritance line. Mixins for utility traits stay allowed.

### D1. RootObject layout — exactly two words
`sizeof(RootObject) == 2 * sizeof(void*)`:

| Slot | Bits | Meaning |
|------|------|---------|
| vptr | 64 | C++ vtable pointer |
| `TaggedPayload metaWord_` | 64 | see D2 |

Stored as `std::atomic<uint64_t>` for refcount CAS. Initialization is one 8-byte store.
Non-CAS reads of immutable bits (role, ever_acquired-once-set) use a `memory_order_relaxed`
load on the same word and mask — no full acquire is needed because those bits are stable
after construction.

### D2. TaggedPayload bit layout
Single 64-bit word, little-endian interpretation:

```
[ arm_ptr 44 | refcount 16 | ever_acquired 1 | role 3 ]
  bits 20..63  bits 4..19    bit 3            bits 0..2
```

- **role (3 bit)**: `ClassType` enum — `None=0, Interface=1, Implementation=2, Extension=3,
  Imposter=4, Bridge=5`. Two values reserved.
- **ever_acquired (1 bit)**: set on first successful `Acquire`; never cleared. Used by lifetime
  diagnostics to distinguish "never owned" (stack/static) from "released to 0".
- **refcount (16 bit)**: saturating unsigned. Value `0xFFFF` is the **external-lifetime
  sentinel**: `Acquire`/`Release` are no-ops, dtor runs at scope exit. Stack and static
  instances initialize with this sentinel. Normal counted lifetime runs `0..0xFFFE`; reaching
  `0xFFFE` triggers a runtime saturation diagnostic (refcount inflation bug).
- **arm_ptr (44 bit)**: tagged pointer to the per-instance dispatch arm. 44 bits = canonical
  user-space pointer on x86-64 / AArch64 (we accept the implicit cap; documented as **D10'**
  caveat below). Decoded by sign-extending to 64 bits.

No separate `LifetimeMode` field — sentinel encodes it. Variable name in C++ is `metaWord_`.

**D10' caveat:** the 44-bit arm_ptr requires arm storage to live in the canonical user-space
window. Y2's `MakeOwned` allocates arms via the standard heap, which is always canonical;
embedders supplying a custom allocator must guarantee canonical addresses or set the external
sentinel and forgo arm packing.

### D3. `Y_OBJECT;` macro
Every `RootObject` subclass must invoke `Y_OBJECT;` in a `public:` section. The macro:

1. `static_assert`s the enclosing class has a virtual dtor.
2. `static_assert`s the enclosing class is at `public:` access (consteval reflection on the
   current member's access).
3. Materializes `static constexpr auto kMetaCore` from reflection over `^^EnclosingClass`.
4. Wires the vtable hook used by `Nucleus()` and `TypeDynamic()`.
5. Emits a friend declaration for `Yuki::Detail::MetaHook<EnclosingClass>`.

Misplacement produces a diagnostic that names the access region (`private:` / `protected:`).
Reflection cannot inject virtuals, so the macro stays — this is the irreducible identity hook.

### D4. Annotation gating
Every direct `RootObject` subclass must carry exactly one of `Anno::Interface`,
`Anno::Implementation`, `Anno::Extension`, `Anno::Imposter`, `Anno::Bridge`. The toolchain
fires a `consteval` check during `Y_OBJECT` expansion; an unannotated subclass refuses to
compile. Indirect bases (a subclass of an annotated class) inherit the role from their
nearest annotated ancestor.

### D5. Interface is a `RootObject` subclass
`Interface` annotations imply `: public RootObject`. This matches CATIA's model and keeps the
walk path uniform. We note the option to peel `Interface` off `RootObject` later (purely
abstract interfaces with no payload) and leave it as a future refactor — no current code
relies on `Interface` *not* being a `RootObject`.

### D6. ClassType encoding
The 3-bit role lives in the low bits of `metaWord_`. `TypeDynamic()` returns
`ClassType(metaWord_ & 0x7)` — no vcall, no atomic load on the value (a relaxed read of the
word is sufficient because role bits are stable after construction).

---

## 2. Seal Annotations (D7)

Three orthogonal seals, each with a clearly-defined per-arm semantics:

| Anno | Scope | Meaning |
|------|-------|---------|
| `Final{I}` | Inheritance | No subclass of this class may re-implement `I`. |
| `Unique{I}` | Closure | At most one class in any closure of this nucleus implements `I`. |
| `Important{I}` | Closure | This impl always wins dispatch for `I`; at most one `Important` per `I` per closure. |

### D7.1 Consteval checks (single-TU view)
At `Y_OBJECT` time, the toolchain walks `kImplementsInfos<T>` + the inheritance chain via
reflection and rejects violations visible in the current TU. Diagnostics name both the
offending class and the originating seal.

### D7.2 Runtime checks in `Registry::Install<E>` (cross-module view)
Two independently-compiled modules can both define an `Important{I}` extension over the same
nucleus and only meet at process load. `Install<E>` therefore re-runs the seal checks against
the live `MetaLinks::dispatch` snapshot:

```cpp
// pseudo-code, inside Install<E> under the writer mutex
for (auto& info : kImplementsInfos<E>) {
    const DispatchEntry* prior = LookupEntry(snapshot, info.iid);
    if (prior && IsFinal(prior->providerClass, info.iid)) ReportConflictAndAbort(...);
    if (info.unique && prior)                              ReportConflictAndAbort(...);
    if (info.important && PriorIsImportant(prior))         ReportConflictAndAbort(...);
}
```

`ReportConflictAndAbort` is a fatal diagnostic — Y2 never silently picks a winner across
modules. Behavior gated on `Yuki::kDebug` (constexpr) controls verbosity, never on `#ifdef`.

### D7.3 Important wins
When `Important` is present, dispatch returns its arm regardless of registration order. The
`DispatchEntry` carries an `important` bit; the binary-search step prefers the entry with that
bit set when iids collide.

---

## 3. Lifetime & Refcount (D8–D13)

### D8. Hierarchical monotonic invariant
For every closure node `N` with upstream `U`:

```
refcount(N) >= refcount(U)       whenever refcount(U) > 0
```

Concretely: a facade for interface `I` over implementation `Impl` has
`refcount(facade) >= refcount(Impl)`; an extension `E` extending `Impl` has
`refcount(E) >= refcount(Impl)`. The invariant guarantees that `Impl` (the nucleus) outlives
every facade and every extension in its closure.

### D9. Each node self-manages
No node delegates its refcount to another. Each carries its own 16-bit count in `metaWord_`.
The hierarchical invariant is enforced at the *boundary* operations:

- Facade ctor (called from `MaterializeIntoImpl`): `Acquire(underlying_impl)`.
- Extension ctor (called from `MakeOwned<E>` or eager hook): **deferred** — see D11.
- Facade dtor: `Release(underlying_impl)`.
- Extension dtor: `Release(extendee)`.

### D10. `Acquire` / `Release` are atomic CAS on `metaWord_`
The 16-bit refcount field is updated by a compare-exchange loop on the full word; role and
arm_ptr bits are masked-preserved. Saturation at `0xFFFE` triggers diagnostic; the sentinel
`0xFFFF` short-circuits to no-op.

### D11. Eager extensions defer upstream-Acquire
If an `Anno::Eager` extension's ctor called `Acquire(extendee)` during the impl's own ctor
chain, the nucleus refcount would never reach 0. Eager extensions are constructed and parked
in the chain with `refcount=0` (held weakly by the chain — the chain owns them by direct
pointer, not via refcount). They `Acquire(extendee)` the first time user code obtains a
`ComPtr` to them via `Query`; on user-side `Release` returning to 0 they re-detach from the
extendee but stay parked in the chain. Final teardown happens in nucleus dtor, which walks the
chain and `delete`s every parked node.

### D12. `ComPtr<T>` is the user-facing handle
COM-style smart pointer: copy `Acquire`s, dtor `Release`s, `Adopt(T*)` takes a +1 reference,
`Detach()` yields a +1 raw pointer. `MakeOwned<T>(args...)` is the standard factory —
allocates via the standard heap, constructs with refcount = 1, returns `ComPtr<T>`. Stack /
static instances never go through `MakeOwned` and carry the external sentinel.

### D13. Diagnostics under `Yuki::kDebug`
When `Yuki::kDebug == true`, every `Release` checks the hierarchical invariant against the
upstream's count and `assert`s on violation. When `kDebug == false`, the asserts compile to
nothing; ABI is unchanged. The switch is `inline constexpr bool kDebug` in
`Yuki/Core/Config.h`, never `#ifdef`.

---

## 4. Dispatch &amp; Query Cache (D14–D16)

### D14. Dispatch arms
Three arms (Plan-1's `DirectCast` collapses into `InlineFacade` because Y2 facades always
own a small inline frame):

| Kind | Where the result lives |
|------|------------------------|
| `InlineFacade` | inline frame inside `Impl` (zero allocation, BOA interfaces) |
| `SideTableResolver` | heap-allocated facade lazily materialised by resolver fn |
| `CodeExtensionSingleton` | shared stateless extension singleton |

### D15. Four-level Query cache
- **L0 — consteval shortcut.** If `Impl` BOA-inherits `I`, `Query<I>` resolves to the
  inline-facade arm at compile time; zero runtime work.
- **L1 — per-`MetaLinks` 4-slot fingerprint.** A small ring of recent (iid, entry-ptr-or-null)
  pairs sits inside `MetaLinks`. Negative hits are cached too, so repeated
  "does this closure provide `I`?" misses cost a single iid compare. Updated lock-free on
  every L2 lookup.
- **L2 — flat `mergedDispatch` snapshot.** Atomic pointer to an iid-sorted array of
  `DispatchEntry`. Binary search; pure rodata once published.
- **L3 — invalidation broadcast.** `Install<E>` walks `extendedBy` of the affected nucleus and
  bumps each downstream's `MetaLinks::cacheEpoch_`. The next L1 lookup observes the epoch
  mismatch and clears its slots.

Plan-1 had only L2 + L3; Y2 adds L0 and L1, yielding an estimated **5×** speedup on the
hottest repeated-query workload (extrapolated from Plan-1 `IntrospectionTest` access patterns)
and turning negative queries into single-compare operations. The estimate is to be confirmed
by a Y2 microbenchmark once the implementation lands.

### D16. base-chain extension propagation
A closure walks its `base` chain to gather inherited extensions. Y2 flattens this at
`Install<E>` time: when registering an extension on a base, every downstream class's
`mergedDispatch` is regenerated. The walk happens once, at registration, not on every Query.

---

## 5. Registry &amp; MetaClass Layering (D17–D18)

### D17. Clean refactor, no Plan-1 compat
Y2 lands as a full rewrite of `Yuki::Core`. Plan-1 headers (`MetaNode`,
`IfaceFacadeNode`, `ExtensionNode`, the per-class CRTP families) are deleted. Tests are
rewritten on the Y2 surface. The Plan-1 → Y2 migration is *not* a deliverable.

### D18. Three-layer MetaClass
- **MetaCore** — `static constexpr`, rodata. Iid, name, kImplementsInfos, kExtendsInfo,
  seal flags, RegisterFn pointer, destructor pointer.
- **MetaLinks** — runtime, mutable under RCU. Atomic snapshot pointers for `dispatch`,
  `mergedDispatch`, `extendedBy`, `implementedBy`, `eagerSet`, plus `cacheEpoch_` and the L1
  fingerprint ring. One writer mutex per metaclass.
- **MetaDynamic** — runtime, instance-bound. Identity hook output from `Y_OBJECT`, the
  `kMetaCore` splice, and the registration-time-computed `mergedDispatch` pointer cached on
  each instance for L0 fast path.

Snapshot retirement uses RCU-by-epoch (Plan-1 already proved this works). Per-metaclass
writer mutex prevents racing `Install` calls on the same nucleus.

---

## 6. Plan 2 — Cross-DLL Plugin Architecture (P2-D1 … P2-D4)

### P2-D1. Install-only lifecycle
Plugins can be **discovered, loaded, and registered**. Once a DLL is realized into the
process its classes are present for the rest of the process lifetime. There is no
`Uninstall<T>`, no `UnloadPlugin`. Hot-reload requires a process restart.

> Rationale: every refcount, every cached pointer, every `mergedDispatch` snapshot would
> need an answer for "what if this metaclass disappears". The cost in design surface and
> diagnostic complexity is not worth it for a use case (live patching) that production
> systems handle with process supervisors.

### P2-D2. Two-phase lazy load
Discovery and realization are separated.

**Phase A — Manifest scan (process start).** The runtime walks a configured directory
(or an env-listed set), opens each candidate `.dll` / `.so` *without* `LoadLibrary`, maps
the file, locates the `.yuki_manifest` PE section (ELF `.note.yuki.manifest`), and pulls a
complete MetaCore shadow into the in-process manifest registry. This is pure file I/O — no
DLL code runs, no global ctors fire, no relocations apply. Cross-DLL seal-conflict checks
(D7.2 logic) run here against the shadow, so two plugins with conflicting `Important{I}`
fail loudly before either is actually loaded.

**Phase B — Lazy realization (on first use).** The first `MakeOwned<T>` or
`Query<T>` whose iid resolves into an unrealized DLL triggers:

1. `LoadLibrary` / `dlopen` on the DLL.
2. Invocation of the per-class `RegisterFn` whose RVA was recorded in the manifest.
3. Atomic flip of the in-process `MetaLinks` so subsequent queries take the fast path.

Realization is one-shot per DLL: the first user pays the `LoadLibrary` cost; everyone after
sees fully-realized metaclasses. Plugins that no one uses never load.

### P2-D3. Manifest format — complete MetaCore shadow
The manifest is a **consteval-emitted** binary blob placed in its own section
(`.yuki_manifest` / `.note.yuki.manifest`). Layout (LE, packed):

```
header {
  uint32_t magic;          // 'YK2M'
  uint16_t version;        // manifest schema version
  uint16_t flags;
  uint32_t class_count;
  uint32_t string_table_off;
  uint32_t string_table_len;
}
class_record[class_count] {
  Iid    iid;              // 16 B
  Iid    primary_base;     // 16 B, zero if RootObject is direct base
  uint32_t name_off;       // into string table
  uint16_t role;           // ClassType
  uint16_t seal_flags;     // Final/Unique/Important bits
  uint32_t implements_off; // [(iid, seal_flags), ...]
  uint32_t implements_cnt;
  uint32_t extends_off;    // [iid, ...]
  uint32_t extends_cnt;
  uint32_t register_fn_rva;
  uint32_t destructor_rva;
  uint32_t eager_bit;
}
```

The emission point is a `consteval` block over the TU's `RootObject` subclasses (collected
via reflection on translation-unit-local registrations); the block writes into a
`[[gnu::section(".yuki_manifest")]]` array. Zero runtime cost.

Cross-DLL conflict detection at Phase A uses only the manifest — no DLL code runs. The
iid chain (`primary_base`) lets the scanner walk inheritance virtually.

### P2-D4. Closure serialization
**Goal:** stream a closure (nucleus + all extensions, no facades) to a backend, and
rehydrate it later — possibly in a different process — preserving in-closure pointer
identity.

#### Schema generation (reflection-driven)
Fields opt in via `[[=Anno::Pickled]]`. A `consteval` walk over `nonstatic_data_members_of`
on each `RootObject` subclass yields a `static constexpr Schema` describing field offsets,
types, and an optional `Anno::SchemaVersion{N}`. Unknown trailing fields on read are
skipped; missing fields default-init.

#### Wire format
TLV records, no padding:

```
closure_stream {
  uint32_t magic;          // 'YK2P'
  uint16_t version;
  uint16_t backend_id;     // 0=binary-LE, 1=msgpack, ...
  record nucleus;
  record extensions[*];
  uint32_t end_marker;     // 'YK2E'
}
record {
  Iid       class_iid;       // 16 B
  uint16_t  schema_version;
  uint32_t  payload_len;
  byte      payload[payload_len];
}
```

In-closure pointer fields serialize as `(role, target_iid, target_index)` where
`target_index` selects among multiple instances of the same iid in the same closure.
Out-of-closure pointer fields refuse to serialize: `static_assert` fires if a `Pickled`
field is a `RootObject*` whose target isn't reachable through the same nucleus.

#### Backend concept
```cpp
template <class B>
concept SerializerBackend = requires (B b, std::span<const std::byte> r,
                                      std::span<std::byte> w) {
    { b.write_bytes(r) } noexcept -> std::same_as<void>;
    { b.read_bytes (w) } noexcept -> std::same_as<bool>;
};
```

Y2 ships `BinaryLEBackend` (zero-copy on LE hosts) and `MsgPackBackend` (cross-language).
Users plug in their own — gRPC, Cap'n Proto, encrypted streams — by satisfying the concept.

#### Async I/O via stdexec
Cold-path serialization composes via `std::execution::sender`s:

```cpp
auto snd = Yuki::SerializeClosure(root, BinaryLEBackend{file})
         | std::execution::then([](auto stats) { /* log */; });
```

Senders fit because (a) the work is I/O-bound, (b) cancellation is meaningful (partial
writes are recoverable), (c) the hot `Query` path is untouched. Synchronous
`SerializeClosureSync` wraps `sync_wait` for users who don't want sender plumbing.

#### Cross-DLL deserialize
On read, each record's `class_iid` is looked up in the manifest registry (P2-D2). If the
owning DLL is not yet realized, deserialization triggers lazy realization first, then
calls `MakeOwned`-equivalent on the resolved metaclass with reflected
field-copy. Identity rewiring runs as a second pass once every record is materialized.

#### Identity preservation
Pointer-rewire happens after every record is materialised. The deserializer maintains a
local `(role, iid, index) -> RootObject*` map; the second pass walks each materialised
node's `Pickled` pointer fields and substitutes. Cycles handled by construction order
(nucleus first, then extensions in stream order, then rewire).

---

## 7. Out-of-scope

- Plan-1 compatibility shim.
- Hot DLL unloading.
- Cross-process shared-memory closures (the serializer covers the use case).
- Reflection on private members for the manifest (only public metaclass surface ships).

## 8. Open items

None blocking implementation. Two future considerations recorded:

- Interface peeling from `RootObject` (D5 future option).
- Custom-allocator embedding outside the canonical 44-bit window (D10' caveat).



