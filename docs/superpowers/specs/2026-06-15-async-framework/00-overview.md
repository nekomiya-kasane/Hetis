# Mashiro Async Framework — Overview & Contract

**Status:** Draft v0.2 (umbrella spec; incorporates synthesis pass from `09-synthesis.md`)
**Date:** 2026-06-15
**Author:** Mashiro Engine team
**Scope:** `Mashiro::Async` namespace; new sources under `Mashiro/include/Mashiro/Async/` and `Mashiro/src/Async/`. Composes with the existing `Mashiro::Platform` layer (per `2026-06-11-platform-thread-infrastructure-design.md`) without modifying it.

### Revision history

- **v0.1** — initial draft. Locked vocabulary, layer boundaries, file layout, subagent assignments.
- **v0.2** — incorporates the synthesis pass (`09-synthesis.md`) adjudication of cross-spec issues raised by Subagents A–E. Changes: §5.6 annotation list extended with `Detached` and `ScopeTag`; §5.3 allocation wording softened for the `Scope` ring buffer; §6.6 composition matrix corrected for the Stream / Nursery row; §8 subagent assignments marked complete and cross-referenced to the delivered layer specs.

---

## 1. Purpose

Build a **complete, layered, high-performance async fabric** for Mashiro that:

- Speaks **stdexec (P2300 + family)** as its only async vocabulary — sender / receiver / scheduler / domain / `task<T>` / `counting_scope` / `inplace_stop_token`. No bespoke executor, no hand-rolled future, no per-subsystem ad-hoc primitive.
- Supports **multiple execution backends** behind one scheduler concept: `Inline`, `StaticThreadPool`, `Tbb` (oneTBB task arena), `Platform` (the OS-affinity thread defined by the existing platform-thread spec), and `Io` (a stdexec-style proactor — io_uring on Linux, IOCP on Windows).
- Treats **TBB as a first-class backend** for data-parallel work (`tbb::task_arena`, `tbb::flow::graph`, `parallel_for` / `parallel_reduce`) and exposes it under the same scheduler concept that the rest of the framework consumes — clients write the same pipeline whether the work runs on TBB, the static pool, or io_uring.
- Treats **C++20 coroutines** as one of three peer task vocabularies (sender expression, coroutine `Task<T>`, `Stream<T>`). Choice is per-call-site, never imposed.
- Is **extensible by users**: add a new scheduler, a new sender adaptor, a new awaitable, a new domain, or a new structured-concurrency pattern *without* touching the framework's headers. All extension points are concept-checked, reflection-verified, and ABI-stable.
- Pays **zero runtime overhead for compile-time-decidable work**. Capability checks, scheduler-affinity transitions, contract verification, completion-signature unification, and route generation are `consteval` / `if constexpr` / `transform_sender`. Anything that can be a static type-system property *is* a static type-system property.
- Carries **no technical debt**: no virtual-dispatch executor pointer, no `std::function` in the hot path, no `std::any` event payload, no double-free under cancellation, no implicit allocation in distribution paths, no synonyms in the public surface.

This document fixes vocabulary, layer boundaries, file layout, and the cross-cutting contracts that every layer spec depends on. Layer-specific design lives in `01-foundations.md` through `07-extension.md`.

---

## 2. Design Principles

These bind every layer spec and every public header. A PR violating one of them is a design-review reject, not a style nit.

1. **One vocabulary.** Sender / receiver / scheduler / domain / stop_token / scope are *the* async types. New abstractions either model an existing concept or are rejected. There is no "framework future" type, no "framework executor" interface, no "framework callback" signature.

2. **Concept-first, type-erased never on hot paths.** Every extension point is a C++20 concept (`scheduler`, `sender`, `receiver`, `domain`, `stop_token` — plus framework-introduced concepts `BackendScheduler`, `IoScheduler`, `BulkScheduler`, `Awaitable`). Type erasure exists (`any_sender_of<Sigs...>`, `any_scheduler`) but is only used at module / plugin boundaries, never inside a pipeline.

3. **Compile-time first.** If a property can be checked at compile time, it must be. Capabilities (`OffersBulk`, `OffersIo`, `IsAffine`, `IsForwardProgress`), thread-affinity contracts (`RequiresPlatformThread`, `RunsAnywhere`), completion-signature exhaustiveness, scheduler equality optimisations — all `consteval`. Runtime checks survive only as debug-mode assertions or as optional diagnostics gated behind a `Mashiro::Async::Diagnostics` knob.

4. **Reflection drives schemas, not policy.** P2996 reflection + P3394 annotations generate: capability metadata tables, completion-signature unions, plugin descriptors, structured-log tags, and the `domain::transform_sender` rewrite tables. Reflection never decides *behaviour* (no virtual dispatch via reflection); it decides *what types compose with what*.

5. **Annotations are capability tags, not identity.** `[[=Async::Affine{Backend::Platform}]]` says "this sender requires the Platform thread", not "this *is* a platform sender". Multiple annotations compose; annotations are queried via `Traits::AffinityOf<S>`, never inspected by name.

6. **Composition over inheritance.** No abstract base classes in user-facing code. Layer dependencies go strictly downward (L0 → L1 → … → L7), and a higher layer is built by *composing* lower-layer types, never by deriving from them.

7. **Cancellation is structural.** `inplace_stop_token` flows through every receiver's environment. Cancellation propagates by sender topology — not by a global flag, not by a per-channel `closed_` boolean, not by sentinel values. A correctly-built pipeline cancels structurally with no client code.

8. **Allocation is explicit and lifted.** Hot paths (sender start, scheduler enqueue, sender completion) allocate zero heap memory. Coroutine frames may allocate when HALO does not apply — this is documented at the awaiter, never silent. Where allocation is unavoidable, the allocator is a stdexec-style queryable on the receiver's environment (`get_allocator(env)`), not a global.

9. **Diagnostics are zero-cost when off.** `Diagnostics::trace_pipeline(s)`, `Diagnostics::dump_op_state(op)`, `Diagnostics::detect_starvation()` compile to nothing in release unless explicitly enabled. Tracy / Perfetto integration is a domain rewrite, not a framework dependency.

10. **No technical debt budget.** Renaming an abstraction is preferred over adding an "old name" alias; deleting an unused capability is preferred over keeping it "for symmetry"; rewriting a layer is preferred over papering over a leaky boundary. The framework ships when each layer's spec passes review; partial deliveries land behind compile-time feature flags, not runtime ones.

---

## 3. Layer Map

The framework is **eight strict layers**. Each layer has its own spec file and its own subdirectory in the source tree. A given layer may depend only on layers below it.

```
┌──────────────────────────────────────────────────────────────────────┐
│ L7  Extension surface                                                 │
│     User-defined schedulers, senders, domains, awaitables.            │
│     Plugin descriptors generated by reflection.                       │
├──────────────────────────────────────────────────────────────────────┤
│ L6  Patterns                                                          │
│     parallel_for / pipeline / actor / reactive / fork-join /          │
│     scatter-gather. Built from L5 + L3.                               │
├──────────────────────────────────────────────────────────────────────┤
│ L5  Structured concurrency                                            │
│     scope / nursery / supervised / linked-cancellation. Built on      │
│     stdexec::counting_scope.                                          │
├──────────────────────────────────────────────────────────────────────┤
│ L4  Coroutine task types                                              │
│     Task<T> (one-shot), Stream<T> (async range), Job (fire-and-     │
│     forget). exec::task with scheduler affinity baked in.             │
├──────────────────────────────────────────────────────────────────────┤
│ L3  Sender adaptors & combinators                                     │
│     Framework-specific adaptors layered on stdexec primitives:        │
│     bulk, io, batch, debounce, throttle, retry, timeout, race.        │
├──────────────────────────────────────────────────────────────────────┤
│ L2  Backends                                                          │
│     scheduler implementations: Inline, StaticPool, Tbb, Platform,     │
│     Io. Each implements one or more of                                │
│     {BackendScheduler, BulkScheduler, IoScheduler, AffineScheduler}.  │
│     Domains for backend-specific rewrites.                            │
├──────────────────────────────────────────────────────────────────────┤
│ L1  Capability annotations & traits                                   │
│     [[=Async::Backend{...}]], [[=Async::Affine{...}]],                │
│     [[=Async::Bulk{...}]], [[=Async::Cancellable]],                   │
│     [[=Async::Allocates{...}]]. Reflection-driven Traits.             │
├──────────────────────────────────────────────────────────────────────┤
│ L0  Vocabulary (re-exports)                                           │
│     stdexec types under Mashiro::Async aliases. No new behaviour at   │
│     this layer — only renaming + concept aliasing for project style.  │
└──────────────────────────────────────────────────────────────────────┘
```

| Layer | Spec file | Owns |
|-------|-----------|------|
| L0 | `01-foundations.md` | Re-exports, namespace, concept aliases, completion-signature helpers |
| L1 | `01-foundations.md` | Capability annotations, `Traits::*`, consteval verifiers |
| L2 | `02-backends.md` | `Inline`, `StaticPool`, `Tbb`, `Platform`, `Io` schedulers + domains |
| L3 | `03-adaptors.md` | Framework sender adaptors (bulk, io, batch, debounce, etc.) |
| L4 | `04-coroutine-tasks.md` | `Task<T>`, `Stream<T>`, `Job`, awaitable bridges |
| L5 | `05-structured.md` | `Scope`, `Nursery`, supervised modes, linked cancellation |
| L6 | `06-patterns.md` | `parallel_for`, `pipeline`, `actor`, `reactive`, `fork_join`, `scatter_gather` |
| L7 | `07-extension.md` | User-extension contract, plugin discovery, ABI stability rules |

Cross-cutting (touches all layers): `08-cross-cutting.md` — cancellation, allocation, diagnostics, errors, time, migration.

---

## 4. Namespace and File Layout

### 4.1 Namespace tree

```
Mashiro::Async                    — L0/L1/L3 public surface
Mashiro::Async::Concepts          — concept aliases (BackendScheduler, ...)
Mashiro::Async::Traits            — consteval queries (AffinityOf, BulkOf, ...)
Mashiro::Async::Backend           — L2 backend types
Mashiro::Async::Backend::Inline
Mashiro::Async::Backend::StaticPool
Mashiro::Async::Backend::Tbb
Mashiro::Async::Backend::Platform — alias of Mashiro::Platform's scheduler
Mashiro::Async::Backend::Io
Mashiro::Async::Coro              — L4 coroutine types
Mashiro::Async::Structured        — L5 scope / nursery
Mashiro::Async::Patterns          — L6 patterns
Mashiro::Async::Extension         — L7 user-extension helpers
Mashiro::Async::Diagnostics       — cross-cutting diagnostics
```

No `using namespace` exposing these is permitted in headers. `Mashiro::Async` is the only namespace that downstream code is expected to type by name.

### 4.2 Header layout

```
Mashiro/include/Mashiro/Async/
├── Async.h                    — single-header convenience (re-exports L0..L4)
├── Foundations.h              — L0 re-exports, L1 annotations
├── Concepts.h                 — concept aliases
├── Traits.h                   — consteval traits
├── Backend/
│   ├── Inline.h
│   ├── StaticPool.h
│   ├── Tbb.h
│   ├── Platform.h             — alias header, includes Mashiro/Platform/PlatformThread.h
│   └── Io.h                   — proactor abstraction; impls in src/Async/Backend/Io/{Linux,Windows}/
├── Adaptor/
│   ├── Bulk.h
│   ├── Batch.h
│   ├── Debounce.h
│   ├── Retry.h
│   ├── Timeout.h
│   └── Race.h
├── Coro/
│   ├── Task.h
│   ├── Stream.h
│   └── Job.h
├── Structured/
│   ├── Scope.h
│   └── Nursery.h
├── Patterns/
│   ├── ParallelFor.h
│   ├── Pipeline.h
│   ├── Actor.h
│   ├── Reactive.h
│   ├── ForkJoin.h
│   └── ScatterGather.h
├── Extension/
│   ├── Scheduler.h            — concept + helper for user schedulers
│   ├── Sender.h               — sender authorship helpers
│   ├── Domain.h               — domain authorship helpers
│   └── Plugin.h               — runtime plugin discovery (opt-in)
└── Diagnostics/
    ├── Trace.h
    ├── Counters.h
    └── Probes.h
```

Sources mirror under `Mashiro/src/Async/`. Tests under `Mashiro/tests/Async/`. Demos under `Mashiro/demos/Async/`.

---

## 5. Cross-Layer Contracts

These are **frozen** by this overview. A layer spec that violates one is rejected.

### 5.1 Scheduler concept hierarchy

```cpp
namespace Mashiro::Async::Concepts {

    // Re-export of stdexec::scheduler — every backend models this.
    template<class S>
    concept Scheduler = stdexec::scheduler<S>;

    // Bulk-capable scheduler: schedule_bulk(s, n, fn) is well-formed.
    template<class S>
    concept BulkScheduler = Scheduler<S> && requires (S s, std::size_t n) {
        stdexec::schedule_bulk(s, n, [](std::size_t){});
    };

    // I/O-capable scheduler: exposes async_read / async_write / async_accept etc.
    // via the Io::operations CPO set.
    template<class S>
    concept IoScheduler = Scheduler<S> && Io::supports_operations<S>;

    // Affine scheduler: schedule(s) always completes on a fixed thread,
    // and equality-comparable schedulers compare equal iff that thread coincides.
    template<class S>
    concept AffineScheduler = Scheduler<S> && Traits::IsAffine_v<S>;

    // Forward-progress guarantee (stdexec::forward_progress_guarantee_of).
    template<class S>
    concept ParallelScheduler = Scheduler<S> &&
        Traits::ProgressOf_v<S> >= stdexec::forward_progress_guarantee::parallel;
}
```

Backends declare which concepts they model via L1 annotations; the consteval verifier in `Traits.h` checks that the declared capability set matches the actual concept satisfaction. Mismatch = compile error.

### 5.2 Stop-token type

The framework uses **exactly one** stop-token type internally: `stdexec::inplace_stop_token`, exported as `Mashiro::Async::stop_token`. `std::stop_token` is supported at the **boundary** (e.g. `jthread` interop) via an explicit adaptor `bridge_stop_token(std::stop_token)`. Backends must accept `inplace_stop_token` from receiver environments; they may *not* require a different stop-token type.

### 5.3 Allocation contract

- L0–L3 hot paths: zero heap allocation. Verified by `Diagnostics::AllocCheck` in test mode.
- L4 coroutine frames: one allocation per `Task<T>` / `Stream<T>` invocation when HALO does not apply. Documented at the type, not the call site.
- L5 scope: at most one allocation per `Scope` for the ring buffer of inline op-state slots. Op-states that exceed the per-slot inline budget allocate individually; this is documented at the spawn site, not silent. The inline budget is a per-`Scope` template parameter defaulting to `sizeof(void*) * 8`.
- L6/L7: allocation policy follows the underlying L2 scheduler; user-extension schedulers declare their policy via `[[=Async::Allocates{...}]]`.

### 5.4 Cancellation contract

Every sender that owns external state (timers, IO handles, child senders) **must** register an `inplace_stop_callback` on the receiver's stop-token. Cancellation never traverses a virtual call. Cancellation completes the sender with `set_stopped` — never with a synthesised error, never with a partial value.

### 5.5 Domain rewrite contract

A backend may register a `stdexec::domain` to rewrite sender expressions. Rewrites are **purely structural**: they may fold transitions (`continues_on(s, sched)` where `s` already completes on `sched`), specialise adaptors (`bulk` over `Tbb` lowers to `tbb::parallel_for`), or thread platform-specific transport (Platform domain rewrites `continues_on(plat, _)` to a `PostThreadMessage` wake on Win32). Domains may **not** change observable completion signatures.

### 5.6 Capability annotation contract

```cpp
namespace Mashiro::Async {
    enum class Backend : std::uint8_t { Inline, StaticPool, Tbb, Platform, Io, User };

    struct Affine    { Backend backend; };
    struct OffersBulk{ };
    struct OffersIo  { };
    struct Cancellable{ };
    struct Allocates { enum class Where { Frame, OpState, Output, External } where; };

    // v0.2 additions (per 09-synthesis.md §2.1).
    // Detached tags coroutine task types that have detached lifetime (Job, not Task<T>);
    // queried by Traits::IsDetached_v<T> and by Scope::spawn (L5).
    struct Detached  { };

    // ScopeTag is attached to every Scope<...> class template; queried by Traits::ScopeTagOf<S>.
    // The tag value is a compact source-location-derived constant; if the user omits it the
    // L5 spec defaults it from std::source_location::current().
    struct ScopeTag  { std::uint64_t value; };
}
```

The full annotation set is therefore `{Backend, Affine, OffersBulk, OffersIo, Cancellable, Allocates, IsForwardProgress, Detached, ScopeTag}`. `Detached` and `ScopeTag` tag L4/L5 types respectively and do **not** participate in the L1 ↔ L2 capability verifier in `01-foundations.md` §8 — they are queried by `Traits::IsDetached_v` and `Traits::ScopeTagOf<S>` only.

Annotations attach to **scheduler types**, to **coroutine task types** (`Detached`), to **scope class templates** (`ScopeTag`), and to **sender expressions producing senders** (the latter via `transform_sender` propagation). They are queryable through `Traits::*_v<T>` consteval variables. They are *never* inspected at runtime.

### 5.7 Completion-signature contract

Every framework-provided sender adaptor **declares its completion signatures explicitly** via `stdexec::completion_signatures_of_t` specialisation. Signatures are unioned (not concatenated): adaptors that may add `set_error_t(std::exception_ptr)` opt in by composition with `with_error<E>`, never silently. `set_stopped_t()` is propagated when any upstream offers it.

---

## 6. Async Models — How They Compose, What They Model

The framework supports five distinct async models. Each models a different aspect of "the world that happens while we wait", and each composes with the others through the sender vocabulary.

### 6.1 Sender expression — pure value-flow algebra

**Models:** Stateless transformations of asynchronous values. *"This value, when it arrives, becomes that value."*
**Form:** `auto s = src() | then(f) | let_value(g) | continues_on(sched);`
**No coroutine frame, no implicit state, no allocation.** The op-state is the receiver's stack frame.
**Use when:** the pipeline is straight-line, the work is short, and you want maximum codegen quality. Domain rewriters can collapse entire chains.

### 6.2 Coroutine `Task<T>` — sequential narrative with branching

**Models:** A linear-with-branches story whose author wants to write `co_await` and read `if`. *"Do A, then if X do B else do C, then D."*
**Form:** `Task<T> work() { auto a = co_await fetch(); if (a.ok) co_return co_await commit(a); else co_return rollback(); }`
**Backed by `exec::task<T>`** with scheduler affinity baked in (P3941). Resumes on a declared scheduler after every `co_await`.
**Use when:** the control flow is genuinely sequential and easier to read top-to-bottom than as `let_value` nesting. Heap-frame cost is one allocation per call, often elided by HALO.

### 6.3 `Stream<T>` — pull-driven async range

**Models:** A bounded or unbounded sequence whose elements arrive over time and the consumer pulls one at a time. *"Whenever the next event happens, hand it to me."*
**Form:** `Stream<Event> evs = channel.events(); for co_await (auto e : evs) handle(e);`
**Built on `exec::async_scope` + sender-of-optional**. Consumer back-pressures the producer naturally (no buffer if the consumer is slow). Cancellation closes the stream cleanly.
**Use when:** the asynchrony is *iteration*, not a one-shot value — input events, telemetry samples, parsed packets, frames.

### 6.4 Structured scope (`Nursery`) — fan-out with bounded lifetime

**Models:** A region of code that owns N child tasks and refuses to exit until all of them finish (or are cancelled). *"Within this block, these things happen in parallel; nothing escapes."*
**Form:** `co_await with_nursery([&](Nursery& n){ n.spawn(a()); n.spawn(b()); });`
**Built on `stdexec::counting_scope`** with structured-shutdown semantics. Cancellation propagates downward (parent cancels → children cancel) and errors propagate upward (one child fails → siblings cancelled).
**Use when:** parallel work has a bounded extent and a single owner. The default for "do these N things concurrently".

### 6.5 Reactive / push-driven flow

**Models:** A push-driven dataflow graph where elements traverse stages without the consumer pulling. *"Whenever a sample is ready, it flows through filter A, then B, then sinks into C — nobody waits."*
**Form:** `pipeline(source) | filter(p) | map(f) | sink(s)` — built from `Stream<T>` plus operators with internal queues.
**Use when:** the work is throughput-dominated and back-pressure is handled by buffering policy (drop-oldest, drop-newest, wait, latest-only) rather than by consumer pull. Audio processing, telemetry pipelines, scene-graph dirty propagation.

### 6.6 Combination matrix

| Outer / Inner | Sender | Task<T> | Stream<T> | Nursery | Reactive |
|---------------|--------|---------|-----------|---------|----------|
| **Sender**    | `then`, `let_value` | `co_await sender` inside | `let_value([](auto v){ return next(stream); })` | `with_nursery` is a sender | `pipeline` factory returns sender |
| **Task<T>**   | `co_await s` | `co_await task` | `for co_await (e : stream)` | `co_await with_nursery(...)` | `co_await pipeline.run()` |
| **Stream<T>** | sender → stream via `into_stream(s)` | `Stream::from_task(t)` | `merge`, `zip`, `concat` (Stream combinators) | `n.spawn_stream_consumer(s, fn)` | reactive *is* a stream graph |
| **Nursery**   | `n.spawn(sender)` | `n.spawn_task(task)` | `n.spawn_stream_consumer(s, fn)` | nested `with_nursery` | `n.spawn(pipeline_as_sender)` |
| **Reactive**  | `source := sender` | `stage := task-per-element` | underlying transport | nursery owns the graph's lifetime | composition |

v0.2 note (per `09-synthesis.md` §2.10): the Stream/Nursery cell names the lifetime-binding operation (`n.spawn_stream_consumer`); the Stream/Stream cell holds the stream combinators (`merge`, `zip`, `concat`). The Nursery/Reactive cell uses `pipeline_as_sender` so the type of the spawned operation is unambiguously a sender (the `pipeline_as_stream` bridge is the Stream-typed counterpart, defined in L6 §3).

### 6.7 Modelling guide — pick the right tool

| Problem shape | Right primitive | Wrong primitive |
|---------------|-----------------|-----------------|
| One async value, simple transform | sender expression | Task<T> (over-allocates) |
| One async value, complex branching | Task<T> | nested let_value (unreadable) |
| Sequence of values, consumer-paced | Stream<T> | callback list (unstructured) |
| N parallel children with one owner | Nursery | detached threads (leaks) |
| Throughput pipeline, backpressure | Reactive / pipeline | Stream<T> if you need fan-out queues |
| CPU-bound parallel-for | `bulk` over Tbb backend | manual `for ... spawn` |
| Single OS-affinity call | sender → `continues_on(platform)` | dispatch_async by hand |
| "Race two operations, take first" | `when_any` on senders | manual flag + cancel |
| "Wait for all of N" | `when_all` or Nursery | `sync_wait` × N (serialises) |
| File / socket I/O | Io scheduler + sender adaptors | thread-per-handle |

These mappings drive the L6 patterns spec.

---

## 7. Backend Map

Concrete backends and what they model. Specs in `02-backends.md`.

| Backend | Models | Scheduler concept set | Forward progress | Notes |
|---------|--------|----------------------|------------------|-------|
| **Inline** | "Run on whoever called start" | Scheduler | weakly_parallel | Trivial. Used by tests, by adaptors that elide transitions, and by reactive sinks. |
| **StaticPool** | Generic CPU pool, work-stealing | Scheduler, BulkScheduler, ParallelScheduler | parallel | N worker threads, lock-free Chase-Lev deques. Default for "background CPU work". |
| **Tbb** | Cooperative parallel runtime | Scheduler, BulkScheduler, ParallelScheduler | parallel | Wraps `tbb::task_arena`; `bulk` lowers to `tbb::parallel_for`; integrates with TBB's flow graph for L6 patterns. |
| **Platform** | OS-affinity thread (Win32 HWND, AppKit, X11) | Scheduler, AffineScheduler | concurrent (single-threaded) | Re-export of `Mashiro::Platform::scheduler`. The only scheduler whose `schedule()` always completes on a fixed thread known statically. |
| **Io** | Proactor for async I/O | Scheduler, IoScheduler | concurrent | io_uring on Linux ≥ 5.13, IOCP on Windows. Sender adaptors: `async_read`, `async_write`, `async_accept`, `async_connect`, `async_timer`. |

Backends are **independent translation units** in `src/Async/Backend/`. Linking only what you need is straightforward — no backend pulls another into the binary unless explicitly composed.

---

## 8. Subagent Task Decomposition

**v0.2 status:** all five subagent deliverables landed; cross-spec coordination resolved by the synthesis pass (`09-synthesis.md`). Boundaries below were **frozen** by v0.1 of this overview and were honoured by every layer spec. The breakdown is preserved as a historical record of how the work was decomposed.

The remaining specs are written by five parallel subagents. Boundaries below are **frozen** by this overview; subagents may not redefine vocabulary or move responsibility across boundaries.

### 8.1 Subagent A — Foundations (L0 + L1)

**Output:** `01-foundations.md`

**Responsibilities:**

- Define the L0 re-export surface: which `stdexec::*` types become `Mashiro::Async::*`, with what aliasing rules. Justify each renaming (style consistency, namespace hygiene), reject gratuitous renames.
- Define the **concept alias layer** (`Concepts::*`) with full concept bodies. Show the consteval relationship between concept satisfaction and capability annotation declaration.
- Define the **L1 annotation set** (`Affine`, `OffersBulk`, `OffersIo`, `Cancellable`, `Allocates`, plus any others you justify). Each annotation: structure, what it tags, who reads it, how it composes.
- Define **`Traits::*`** consteval queries: `AffinityOf<S>`, `BackendOf<S>`, `OffersBulk_v<S>`, `OffersIo_v<S>`, `IsAffine_v<S>`, `ProgressOf_v<S>`, `AllocatesIn_v<S>`, etc. Show the reflection algorithm that drives them.
- Define **completion-signature helpers**: `with_error<E, S>`, `union_signatures<S...>`, `propagate_stopped<S>` — what stdexec primitives they wrap and why thin wrappers earn their keep.
- Define the **consteval verification block** that runs at the bottom of `Foundations.h`: every backend's declared capability set must match concept satisfaction. Show the error message strategy when a mismatch is detected.
- Define the `bridge_stop_token` adaptor (std → inplace) and any other interop primitives at the L0 boundary.

**Constraints:**

- Do not specify backend implementations (that's L2).
- Do not specify any sender adaptor body (that's L3).
- Use only stdexec, P2996, P3394, P3289 features. No P2300 extensions that aren't in the constraint list of the platform-thread spec.
- All annotations must be queryable purely by reflection — no out-of-band registry.

**Deliverables:** ~500 lines of markdown, header sketches with concept bodies and annotation `struct`s, one worked example showing how a backend's annotations drive a `Traits::*_v` query.

### 8.2 Subagent B — Backends (L2)

**Output:** `02-backends.md`

**Responsibilities:**

- For each of the five backends (`Inline`, `StaticPool`, `Tbb`, `Platform`, `Io`):
  - Class layout: scheduler handle type, internal state, equality semantics.
  - `schedule()` sender op-state: data members, alignment, allocation behaviour.
  - Capability declaration via L1 annotations (must match concept satisfaction).
  - Forward-progress guarantee.
  - Cancellation handling: how `inplace_stop_callback` is wired into the wake mechanism.
  - Allocation policy.
  - Domain registration: what `transform_sender` rewrites this backend installs and why each rewrite is observably-equivalent.
- **Tbb-specific:** show how `tbb::task_arena` maps to a stdexec scheduler; how `schedule_bulk` lowers to `tbb::parallel_for` via domain rewrite; how TBB's `flow::graph` is exposed as a domain-level optimisation when a `pipeline(...)` expression is detected.
- **Io-specific:** the proactor abstraction. Define the I/O sender adaptors (`async_read(fd, buf)`, `async_write`, `async_accept`, `async_connect`, `async_timer(dur)`) at the *concept* level; the L3 spec details adaptor code, but you specify the contract here. Show io_uring submission queue ownership, IOCP completion port ownership, and how stop-token cancellation maps to `IORING_OP_ASYNC_CANCEL` / `CancelIoEx`.
- **Platform integration:** import the existing `Mashiro::Platform::scheduler` from the platform-thread spec. Specify *exactly* what the alias adds (capability annotations, `Affine{Platform}` tag) and confirm zero modifications to `Mashiro::Platform`.
- **Composition:** a worked example of a sender expression that crosses Tbb → Platform → Io and how each transition is either a domain rewrite or a real wake.

**Constraints:**

- No new vocabulary — use only L0/L1 types from Subagent A.
- No virtual dispatch in the scheduler. Every scheduler is a value type.
- Each backend must be optional at link time (CMake target per backend).
- TBB backend depends on `thirdparty/tbb`; Io backend on platform-specific syscalls only.
- Platform backend includes `Mashiro/Platform/PlatformThread.h` and adds nothing beyond aliases + annotations.

**Deliverables:** ~800 lines of markdown with one section per backend, header sketches, op-state diagrams, two worked composition examples, the cross-backend pipeline example.

### 8.3 Subagent C — Adaptors & Coroutine Tasks (L3 + L4)

**Output:** `03-adaptors.md`, `04-coroutine-tasks.md`

**Responsibilities (L3 — adaptors):**

- The framework-introduced adaptor set (each layered on stdexec primitives, none reinventing them):
  - `bulk(n, fn)` — defers to `BulkScheduler::schedule_bulk` if available; otherwise expands to a `let_value` over `when_all` of unitary tasks.
  - `batch(window, max_size)` — coalesces a stream's elements into batches.
  - `debounce(dur)` — collapses bursts of values to one per `dur`.
  - `throttle(dur)` — rate-limits emission.
  - `retry(policy)` — retries a sender with exponential / fixed / custom backoff.
  - `timeout(dur)` — completes with `set_stopped` if upstream doesn't produce within `dur`.
  - `race(s1, s2, ...)` — first-to-complete wins; others cancelled.
  - `materialise` / `dematerialise` — completion-signature reification (P2300 vocabulary).
- For each: form, completion signatures, op-state shape, allocation behaviour, cancellation handling, worked example.
- Define how adaptors interact with backend domains (which adaptors are rewrite candidates, which are pass-through).

**Responsibilities (L4 — coroutine tasks):**

- `Task<T>`: `exec::task<T>` typedef + scheduler-affinity contract. How `co_await` of a sender / Task / Stream is dispatched. Frame allocation rules and HALO opportunities.
- `Stream<T>`: definition built on sender-of-optional, `for co_await` integration, back-pressure semantics, stop-token integration. Operators that produce Streams (`stream::generate`, `stream::from_channel`, `stream::interval`).
- `Job`: detached task with structural lifetime owned by the parent `Scope` (forward reference to L5). Why `Job` exists — what it expresses that `Task<void>` cannot.
- The **awaitable bridge**: rules for awaiting plain stdexec senders, custom user awaitables, `std::future` (boundary only), and platform-specific awaiters (e.g. Vulkan fence-await).
- Worked example: a Manager call written three ways (sender, Task, Stream) and discussion of when each is appropriate.

**Constraints:**

- No new scheduler — reuse L2 backends.
- All adaptors must be cancellation-correct (verified by an explicit checklist in the spec).
- All adaptors expose explicit completion signatures via `completion_signatures_of_t` specialisation.
- Coroutine types must be reflection-introspectable (Subagent A's `Traits` should work on them).

**Deliverables:** ~600 lines for L3 (one subsection per adaptor) + ~500 lines for L4. Include the three-way Manager-call example as the closing demonstration.

### 8.4 Subagent D — Structured Concurrency & Patterns (L5 + L6)

**Output:** `05-structured.md`, `06-patterns.md`

**Responsibilities (L5 — structured concurrency):**

- `Scope`: thin wrapper over `stdexec::counting_scope` that adds reflection-friendly tagging and a single-allocation ring buffer of op-states. Lifetime rules, `spawn()` semantics, `on_empty()` settling.
- `Nursery`: the with-block API (`with_nursery([](Nursery&){...})`). Spawn semantics, error propagation (one fails → siblings cancelled), parent-cancel-cascades-down semantics.
- `LinkedScope`: sub-scope whose stop-token derives from the parent's. Use case: per-request scope inside a server scope.
- `Supervised`: scope variant that catches child errors and applies a policy (restart, log-and-skip, propagate). Built on top of `Nursery` with a domain rewrite, not a new primitive.
- The **escape rule**: nothing spawned in a scope may outlive the scope. How this is verified (it isn't, statically — but `Diagnostics::scope_audit()` traces escapes in test mode).
- Cancellation flow diagram: parent stop-source → scope stop-source → child stop-tokens → sender op-states → callback → wake.

**Responsibilities (L6 — patterns):**

- `parallel_for(range, fn)`: built on `bulk` over a `BulkScheduler`. Returns a sender. Worked-example: TBB and StaticPool side by side.
- `pipeline(stage1, stage2, ...)`: reactive pipeline. Each stage is a sender-producing function; stages connect via internal queues. Backpressure policy is a stage parameter. TBB domain rewrites the whole pipeline to `tbb::flow::graph` when stage count ≥ 4.
- `actor<State>(behaviours...)`: actor model on top of a serial executor (`Inline` over an `MpscQueue`). Each behaviour is a sender-returning function. Mailbox is the `MpscQueue<Message>` from Mashiro core.
- `reactive`: combinators for push-driven dataflow (`debounce`, `throttle`, `combine_latest`, `switch_map`). Built on `Stream<T>` from L4.
- `fork_join(fn1, fn2, ..., reduce)`: classic fork-join over a Nursery, with a typed reduce step.
- `scatter_gather(input_range, work_fn, gather_fn)`: bulk scatter, then reduce. Distinguish from `parallel_for + reduce` (this version owns the gather buffer).
- For each pattern: which model from §6 it instantiates, completion signatures, cancellation behaviour, worked example.

**Constraints:**

- Patterns are *thin*: each is ≤ 50 LOC of header. If a pattern needs internal state beyond a `Scope`, justify it.
- No pattern introduces a scheduler — they accept one as a parameter.
- Patterns must compose (test: `parallel_for` over a `pipeline` over a `Stream`).

**Deliverables:** ~500 lines for L5 + ~700 lines for L6. Include the cancellation flow diagram and the cross-pattern composition example.

### 8.5 Subagent E — Extension Surface & Cross-Cutting (L7 + cross-cutting)

**Output:** `07-extension.md`, `08-cross-cutting.md`

**Responsibilities (L7 — extension):**

- The user-extension contract for each extension axis:
  - **New scheduler:** what concepts to model, what annotations to attach, how to register a domain (if needed). Worked example: a "GPU compute scheduler" backed by Vulkan compute queues.
  - **New sender adaptor:** completion-signature declaration, op-state design, cancellation checklist, ABI considerations.
  - **New domain:** when a domain is appropriate (rewrite optimisation, capability injection) vs. when it isn't (changing semantics is forbidden).
  - **New awaitable:** awaiter shape, how it integrates with `Task<T>` and `Stream<T>`, scheduler-affinity considerations.
- **Plugin descriptors:** reflection-driven generation of plugin metadata for runtime-loadable schedulers/adaptors. Use case: hot-reloadable computation backends. Plugin descriptor schema, ABI stability rules (what's frozen across plugin versions, what's recompile-required).
- **Type erasure boundary:** when `any_sender_of` and `any_scheduler` are appropriate, exact ABI cost, when they are forbidden.
- Migration of existing code (e.g. legacy `std::async`, raw `std::thread`, manual condvar pipelines) to the framework. Provide a checklist.

**Responsibilities (cross-cutting):**

- **Cancellation:** exhaustive treatment. How stop-tokens flow through every L0–L6 type. How user code installs callbacks correctly. Anti-patterns and how the framework prevents them. The `Cancellable` annotation semantics.
- **Allocation:** the allocation policy table per layer. How `get_allocator(env)` is queried. How users plug in arena allocators (PMR-based). Allocation accounting via `Diagnostics::AllocCheck`.
- **Errors:** error model. `set_error_t(std::exception_ptr)` is the default; users may opt into typed errors via `with_error<MyError>`. Conversion rules at boundaries. Why we do not use `std::expected` as the primary error type (composition complexity in completion signatures).
- **Time:** `async_timer` source, monotonic vs. wall-clock, suspend/resume behaviour, integration with the platform sleep mechanism.
- **Diagnostics:** trace spans, op-state introspection, starvation detection, deadlock heuristics. Tracy / Perfetto domain. All zero-cost when off.
- **Migration plan:** order of layer delivery, which existing Mashiro code (Platform, Yuki tests, Mashiro demos) ports to the framework first, deprecation steps, what *isn't* migrated and why.

**Constraints:**

- L7 may not introduce new vocabulary — only patterns for users to add their own types.
- Cross-cutting must respect every layer's contract (no surprise allocations, no surprise virtual calls, no global state).
- All examples must compile with the project's clang-p2996 toolchain (verified during synthesis).

**Deliverables:** ~700 lines for L7 + ~800 lines for cross-cutting. The migration plan must list concrete code paths in the existing tree (e.g. "`Mashiro/src/Platform/Windows/PlatformBackendWindows.cpp` already conforms; no migration needed").

---

## 9. Synthesis & Acceptance

Once all five subagents land their specs:

1. **Vocabulary check.** Concordance pass: every term in every spec appears in this overview's section §2 / §5 / §6, or is introduced and defined in the layer that owns it.
2. **Boundary check.** No layer references types or behaviours from a higher layer. Verified by grep + manual review.
3. **Completion-signature check.** For every adaptor, pattern, and coroutine type, the declared signatures are unioned correctly and stop_t propagates as documented.
4. **Compile check.** Each spec's code sketches compile against `thirdparty/stdexec` with the project toolchain (sketches are not exhaustive but must be self-consistent).
5. **Composition check.** The §6.6 combination matrix has at least one worked example per cell (across the five specs).

A spec passes review when (1)–(5) are satisfied. The framework ships when all eight specs pass.

---

## 10. Status

- v0.1 — drafted 2026-06-15. Locked vocabulary, boundaries, and subagent assignments.
- **v0.2 (this document)** — incorporates the synthesis pass (`09-synthesis.md`) adjudicating the
  open issues raised by the five subagent deliverables. §5.3, §5.6, §6.6, and §8 updated;
  no vocabulary drift remaining; layer specs cross-reference this overview.
- v1.0: post-implementation revision after first end-to-end pipeline lands in `Mashiro/demos/Async/`.
