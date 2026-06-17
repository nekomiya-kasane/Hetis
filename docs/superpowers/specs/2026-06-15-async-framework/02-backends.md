# Mashiro Async Framework — L2 Backends

**Status:** Draft v0.2 (Subagent B output for the umbrella spec at `00-overview.md`; incorporates synthesis pass `09-synthesis.md`)
**Date:** 2026-06-15
**Author:** Mashiro Engine team
**Scope:** `Mashiro::Async::Backend` namespace; new sources under `Mashiro/include/Mashiro/Async/Backend/` and `Mashiro/src/Async/Backend/`. Composes with the existing `Mashiro::Platform` layer (per `2026-06-11-platform-thread-infrastructure-design.md` v1.6) without modifying it.

### Revision history

- **v0.1** — initial draft. Lays out the five backends (`Inline`, `StaticPool`, `Tbb`, `Platform`, `Io`), their op-state shapes, capability annotations, domain rewrites, allocation policy, cancellation wiring, and the cross-backend pipeline example. Vocabulary is taken verbatim from `00-overview.md` §5; concept aliases and L1 annotations are taken verbatim from Subagent A's `01-foundations.md` deliverable. No new vocabulary is introduced here.
- **v0.2** — incorporates synthesis-pass adjudications (`09-synthesis.md` §2.6, §2.7, §2.23). §5.8
  publishes `Tbb::pipeline_rewrite_threshold_v` as a public `inline constexpr std::size_t` so the
  L6 pipeline-to-`tbb::flow::graph` rewrite has a documented threshold. §6.5 records the
  non-modelling of `BulkScheduler` by `Platform` and the L3 `static_assert` that enforces it. §7.2
  publishes the three Io op-state size constants (`timer_op_state_size_v`, `read_op_state_size_v`,
  `write_op_state_size_v`) so L3 adaptors can plan inline storage against a stable ABI. §7.2 also
  provides the `tag_invoke(get_io_context_t, scheduler)` body that completes the forward
  declaration in `01-foundations.md` §4.5. §8 adds the worked cross-backend pipeline example
  (Tbb → Platform → Io) requested by the umbrella's "two worked composition examples"
  deliverable. §9 status section added.

---

## 1. Overview

This spec describes **L2** of the Mashiro Async Framework — the concrete `scheduler` implementations that the rest of the stack composes against. There are exactly five:

| Backend | Models | Concept set | Forward progress | Owns |
|---------|--------|-------------|------------------|------|
| `Inline` | "Run on whoever called start" | `Scheduler` | `weakly_parallel` | nothing — pure compile-time |
| `StaticPool` | Generic CPU pool, work-stealing | `Scheduler`, `BulkScheduler`, `ParallelScheduler` | `parallel` | N worker `std::jthread`s, Chase-Lev deques |
| `Tbb` | Cooperative parallel runtime | `Scheduler`, `BulkScheduler`, `ParallelScheduler` | `parallel` | a `tbb::task_arena`, lifetime tied to the handle |
| `Platform` | OS-affinity thread (Win32 / AppKit / X11 / Wayland) | `Scheduler`, `AffineScheduler` | `concurrent` (single-threaded) | nothing — re-exports `Mashiro::Platform::scheduler` |
| `Io` | Proactor for async I/O | `Scheduler`, `IoScheduler` | `concurrent` | one io_uring instance (Linux) / one IOCP port (Windows), one reaper thread |

Every backend is a **value-type scheduler handle** (cheaply copyable, equality-comparable, pointer-sized or pointer-pair sized). No backend exposes a virtual-dispatch interface; the only erasure that exists in the framework is `any_scheduler` (defined by Subagent A at L0) and that is reserved for module / plugin boundaries — never used inside a pipeline that crosses two of these backends.

Every backend is independently linkable: `Mashiro::Async::Backend::StaticPool` lives in its own CMake target with its own translation units, and a binary that does not use it pays neither code size nor static initialiser cost. The TBB backend depends on `thirdparty/tbb` and nothing else from the framework (besides L0/L1). The Io backend depends on platform-specific syscalls (Linux ≥ 5.13 io_uring, Windows IOCP) and nothing else. The Platform backend depends on `Mashiro/Platform/PlatformThread.h` and adds nothing beyond aliases plus L1 annotations.

This spec is layered on Subagent A's L0/L1 vocabulary. Wherever a name like `Concepts::BulkScheduler`, `Traits::IsAffine_v<S>`, `Affine{Backend::Platform}`, or `OffersBulk` appears in this document, it is the type or value defined in `01-foundations.md`. This spec does not define any new concept, annotation, or trait.

---

## 2. Cross-backend invariants

These are properties **every** backend in this spec must satisfy. They are checked by the consteval verification block at the bottom of `Foundations.h` (Subagent A); a backend that ships without them does not compile against the framework.

### 2.1 Value semantics

A scheduler handle is `std::regular`: `default_constructible`, `copy_constructible`, `copy_assignable`, `equality_comparable`. A default-constructed handle compares equal to itself and is in a documented "empty" state (does not refer to any pool / arena / reactor); calling `schedule()` on an empty handle is undefined behaviour — debug builds assert. Two handles compare equal iff they refer to the same underlying execution context (same pool instance, same arena, same platform thread, same reactor); two distinct `StaticPool` instances therefore never compare equal even if they happen to have the same worker count.

Every handle is at most pointer-pair sized (one indirect to the backend's shared state, plus an optional second word for handle-local data such as a queue index hint). No handle holds the backend's state by value; backends own their own state through the public construction site (`StaticPool::Builder{}.workers(N).build()`, `TbbBackend::adopt(arena)`, etc.).

### 2.2 No virtual dispatch

There is no `virtual` keyword anywhere in `02-backends.md`'s headers. Cross-thread transitions between two backends in a sender expression are resolved at the algorithm level — `continues_on(s, sched)` knows the static type of `sched` and dispatches by ADL into the destination scheduler's hooks. The cost of crossing backends is the cost of one push onto the destination's queue (which is whatever lock-free primitive that backend uses) plus one wake. No vtable load, no `std::function`.

### 2.3 Cancellation contract

Every `schedule()`-derived sender, every `schedule_bulk` sender, and every I/O sender registers an `inplace_stop_callback` against `stdexec::get_stop_token(get_env(rcvr))` on `start()`, and unregisters it on completion. The callback's body executes the backend's cancel path: erase the work item from its queue if still pending (StaticPool, Tbb, Platform), submit `IORING_OP_ASYNC_CANCEL` (Io / Linux), or call `CancelIoEx` on the bound handle (Io / Windows). The race between "callback fires" and "worker pops the item" is resolved by a per-item atomic state word (`Pending → Running → Done`) — losing the race means the work runs to completion and the cancel is observed at the next suspension point inside the work itself. None of this involves a virtual call.

### 2.4 Allocation policy

**Hot path: zero heap.** `schedule()`, `schedule_bulk(n, fn)` for compile-time-bounded `n`, and per-completion I/O sender start are heap-free. Op-states live inline in their connected receiver's storage; per-worker queues are bounded ring buffers preallocated at backend construction; per-bulk shape data fits in the op-state.

**Documented allocations:**

- `StaticPool::Builder::build()` allocates the worker array, the per-worker deque storage, and the global injection queue once at construction. No allocation during `schedule()`.
- `Tbb` allocates inside `tbb::task_arena::execute` per the oneTBB internal policy; that allocation is TBB's, not the framework's, and is queryable via `Allocates{Where::External}`.
- `Io` allocates the io_uring SQ/CQ ring once at reactor construction (`mmap`); IOCP allocates `OVERLAPPED` records from a per-reactor freelist, fall-back to `operator new` only when the freelist is exhausted. Both are tagged `Allocates{Where::OpState}` (the storage is op-state-shaped) and `Allocates{Where::External}` (because the kernel owns the actual memory in the io_uring case).
- `schedule_bulk(n, fn)` for runtime-`n` larger than the inline budget (`kBulkInlineMax = 64`) allocates one `n × sizeof(work_item)` block from the receiver's allocator (`get_allocator(env)`). The `Allocates{Where::OpState}` tag is set on the bulk sender's annotation set in this case.

**No allocation paths:**

- `Inline`. There is no state.
- `Platform`. Its op-state is the existing `Mashiro::Platform::scheduler::sender::op_state` (already heap-free per the platform thread spec §6.5).
- `Io` cancellation. The cancel path uses the existing op-state slot, no allocation.

### 2.5 Forward-progress guarantees

`Traits::ProgressOf_v<S>` is a `consteval` query (Subagent A) returning `stdexec::forward_progress_guarantee`. The values per backend:

| Backend | Progress | Why |
|---------|----------|-----|
| `Inline` | `weakly_parallel` | runs on whatever thread is calling, so making concurrent forward progress requires the caller to do so |
| `StaticPool` | `parallel` | N independent OS threads, each guaranteed to make forward progress as long as the OS schedules it |
| `Tbb` | `parallel` | TBB's worker threads make `parallel` progress; the arena adds task-stealing on top |
| `Platform` | `concurrent` | single thread (the platform thread); makes concurrent progress with itself trivially, but two op-states scheduled on the platform scheduler observe FIFO order |
| `Io` | `concurrent` | the reactor makes progress in the kernel; the user-space reaper is a single thread |

`ParallelScheduler` (overview §5.1) is satisfied iff `ProgressOf_v<S> >= parallel`. Therefore `StaticPool` and `Tbb` model `ParallelScheduler`; the others do not. Adaptors that require `parallel` progress (e.g., framework-level `bulk` over a non-bulk-capable scheduler that lowers to `when_all` of N tasks) reject `Inline` / `Platform` / `Io` at compile time with a diagnostic that names the violated concept.

### 2.6 Domain registration

Every backend may register a `stdexec::domain` to specialise sender expressions that use it. Domain rewrites are **purely structural**: they may fold, specialise, or thread platform-specific transport, but they may not change the observable completion signatures. Each rewrite in this spec is followed by a one-paragraph proof of observable equivalence — the proof must inspect the upstream's `completion_signatures_of_t`, the rewrite target's `completion_signatures_of_t`, and demonstrate their union is type-identical. A backend whose rewrites cannot satisfy this is rejected at review.

The umbrella spec (§5.5) defines the contract; this spec instantiates it five times.

---

## 3. `Inline` backend

### 3.1 Purpose

`Inline` is the trivial scheduler. It is a value-type handle with no state; calling `schedule()` on it returns a sender whose `start()` immediately calls `set_value(rcvr)` on the calling thread. It exists for three reasons:

1. **Tests.** Unit tests for adaptors that must observe deterministic ordering and a single thread of execution use `Inline` as their reference scheduler. `sync_wait` against an `Inline`-rooted pipeline never spawns a thread.
2. **Domain rewrites.** Framework algorithms that detect "the upstream and downstream schedulers are equal" sometimes rewrite the inner `continues_on(s, sched)` to `Inline` to make the elision visible to subsequent passes. The L3 spec details when this happens; here we only guarantee that the rewrite target type is well-defined.
3. **Reactive sinks.** Push-driven pipelines (L6 `reactive`) lift their terminal `sink(fn)` to a sender that completes on `Inline` so the producer's thread runs the sink without a queue hop. This is correct because reactive sinks are by contract small, non-blocking, and tolerant of producer-thread execution.

`Inline` is **not** a substitute for `Platform`. A sender that requires the OS-affinity thread (`RequiresPlatformThread` annotation) cannot be satisfied by `Inline` even if the calling thread happens to be the platform thread — the type system enforces this through the `Affine{Backend::Platform}` annotation, not through runtime identity.

### 3.2 Header sketch

```cpp
namespace Mashiro::Async::Backend::Inline {

    class [[=Async::Backend{Async::Backend::Inline}]] scheduler {
    public:
        using __id = scheduler;
        using __t  = scheduler;

        scheduler() noexcept = default;
        bool operator==(const scheduler&) const noexcept = default;

        struct sender;
        [[nodiscard]] sender schedule() const noexcept;

        struct env {
            template<class Tag>
            friend scheduler tag_invoke(stdexec::get_completion_scheduler_t<Tag>,
                                        env) noexcept { return {}; }
        };
    };

    inline constexpr scheduler get_scheduler() noexcept { return {}; }

}  // namespace Mashiro::Async::Backend::Inline
```

### 3.3 `schedule()` op-state

```text
inline_op_state<Rcvr>
├── rcvr : Rcvr               // by value, lives in receiver's storage
└── start() noexcept          // body: stdexec::set_value(std::move(rcvr))
                              // no allocation, no atomics, no callback
```

The op-state has the same size as `Rcvr`. There is no callback, no stop-token registration — `Inline` cannot be cancelled because its work runs synchronously on `start()` and is therefore never in a "pending" state observable to a stop callback. A receiver whose stop-token is already requested before `start()` is reached completes with `set_value` regardless; that is consistent with stdexec's contract that cancellation is opt-in per algorithm and a sender that has nothing to cancel does not synthesise `set_stopped`.

### 3.4 Capability declaration

```cpp
struct [[=Async::Backend{Async::Backend::Inline}]]
       [[=Async::Cancellable{}]]   // trivially — no work to cancel, but receivers' stop tokens are honoured by composition
       scheduler { ... };
```

`OffersBulk`, `OffersIo`, and `Affine` are all absent. The consteval verifier at the bottom of `Foundations.h` confirms `Concepts::Scheduler<Inline::scheduler>` is satisfied and `Concepts::BulkScheduler` / `Concepts::IoScheduler` / `Concepts::AffineScheduler` are not.

`Traits::ProgressOf_v<Inline::scheduler> == forward_progress_guarantee::weakly_parallel`.

### 3.5 Cancellation

There is no internal queue, no wake mechanism, and no work to remove. The receiver's stop callback is the caller's responsibility — `Inline` does not register one of its own.

### 3.6 Allocation

Zero. The op-state is rcvr-sized; `start()` calls `set_value` synchronously.

### 3.7 Domain rewrites

`Inline` registers **no** domain. It is the default lowering target — it never customises algorithms.

### 3.8 Equality semantics

All `Inline::scheduler` instances compare equal. This is the only backend in this spec for which two distinct values of the handle type compare equal; it is correct because there is no per-instance state to differentiate. The framework's transition-elision rewrite uses this equality to fold `continues_on(any, Inline) | continues_on(Inline, any)` to `continues_on(any, any)` — see L3.

---

## 4. `StaticPool` backend

### 4.1 Purpose

`StaticPool` is the framework's in-house generic CPU pool. It is the default backend for "background CPU work that has no special placement requirement" — anything that does not need the platform thread, does not need TBB's interop, and does not interleave with I/O completions. It is a fixed-size pool of N OS threads (default `N = std::thread::hardware_concurrency() - 1`, configurable at construction), each owning a Chase-Lev work-stealing deque, plus one global MPMC injection queue for cross-thread submissions from threads not owned by the pool.

The reason `StaticPool` exists alongside `Tbb` is that not every project links TBB, and several Mashiro subsystems run in environments (sandbox tests, freestanding tools) where adding TBB's runtime is undesirable. `StaticPool` is the small-footprint, dependency-free default; `Tbb` is the first-class option for compute-heavy workloads that benefit from oneTBB's mature stealing, NUMA awareness, and existing TBB-using code (see §5).

### 4.2 Header sketch

```cpp
namespace Mashiro::Async::Backend::StaticPool {

    class context;          // The owning state (pool); not a scheduler.
    class scheduler;        // The handle (cheap, copyable, equal iff same context).

    class context {
    public:
        struct config {
            std::size_t  worker_count    = std::thread::hardware_concurrency() - 1;
            std::size_t  queue_capacity  = 1024;        // per-worker deque
            std::size_t  global_capacity = 4096;        // global injection MPMC
            std::string  name            = "StaticPool";
        };

        explicit context(config = {});
        ~context();           // signals stop, joins all workers, drains
        context(const context&)            = delete;
        context& operator=(const context&) = delete;

        scheduler get_scheduler() noexcept;
        void      request_stop() noexcept;
        bool      stop_requested() const noexcept;

    private:
        struct impl;          // worker array, deques, global queue, stop-source
        std::unique_ptr<impl> p_;
    };

    class [[=Async::Backend{Async::Backend::StaticPool}]]
          [[=Async::OffersBulk{}]]
          [[=Async::Cancellable{}]]
          scheduler {
    public:
        using __id = scheduler;
        using __t  = scheduler;

        scheduler() noexcept = default;          // empty
        bool operator==(const scheduler&) const noexcept = default;

        struct sender;
        [[nodiscard]] sender schedule() const noexcept;

        template<class Shape, class Fn>
        [[nodiscard]] auto schedule_bulk(Shape n, Fn fn) const noexcept;

        struct env { context::impl* p; /* completion_scheduler hooks */ };

    private:
        friend class context;
        explicit scheduler(context::impl* p) noexcept : p_{p} {}
        context::impl* p_ = nullptr;
    };

    struct domain {
        template<class Sender, class Env>
        decltype(auto) transform_sender(Sender&& s, const Env& env) const;
    };

}  // namespace Mashiro::Async::Backend::StaticPool
```

`context` is the owning type; `scheduler` is the cheap handle. Two `scheduler` values compare equal iff their `p_` are the same pointer (i.e. they share a context). Constructing `scheduler{}` (default) yields an empty handle; `schedule()` on an empty handle is undefined behaviour and asserted in debug.

### 4.3 `schedule()` op-state

```text
            static_pool_op_state<Rcvr>
            ┌───────────────────────────────────────────┐
            │ Rcvr     rcvr;                            │  (rcvr-sized)
            │ context::impl*           ctx;             │  (8 B)
            │ std::atomic<state_t>     state{Pending};  │  (4 B)
            │ inplace_stop_callback<…> cb;              │  (1 ptr + inline state)
            └───────────────────────────────────────────┘
            alignas(kCacheLineSize)   // owners-flag fits one cache line
                                      // to avoid false sharing with Rcvr
```

The op-state lives **inline** in the receiver's storage — no heap. `start()`:

1. Tries to push `this` (typed as a `work_item*` whose `execute()` invokes `set_value(rcvr)`) onto the calling thread's local Chase-Lev deque if the calling thread is a pool worker; otherwise pushes onto the global MPMC injection queue.
2. Registers an `inplace_stop_callback` that CASes `state` from `Pending` to `Cancelled` and, if successful, removes the item from its queue (ABA-safe via the deque's tagged-pointer scheme). On loss (state was already `Running`), the callback returns; the work observes a `set_stopped` opportunity at the next suspension if the body itself awaits a stop-aware sender, otherwise the work runs to completion.
3. Wakes one parked worker via `eventcount_.notify_one()` (folly-style event count over a futex on Linux, `WaitOnAddress` on Windows). No `cv.notify_all`.

A worker's main loop:

```text
loop:
  item = local.pop()
       ?: steal_round_robin()
       ?: global.try_pop()
       ?: park_until(eventcount_, predicate)
  if (item == nullptr) break;             // stop requested
  state = item->state.exchange(Running);
  if (state == Pending) item->execute();           // -> set_value(rcvr)
  else                  item->execute_stopped();   // -> set_stopped(rcvr)
```

### 4.4 `schedule_bulk(n, fn)` op-state

`StaticPool` models `Concepts::BulkScheduler`. The bulk op-state shape:

```text
            static_pool_bulk_op_state<Shape, Fn, Rcvr>
            ┌───────────────────────────────────────────┐
            │ Rcvr                  rcvr;               │
            │ context::impl*        ctx;                │
            │ Shape                 n;                  │
            │ Fn                    fn;                 │   (callable; usually small)
            │ std::atomic<size_t>   remaining{n};       │   counts unfinished slices
            │ std::atomic<bool>     cancelled{false};   │
            │ inplace_stop_callback cb;                 │
            │ work_item             slices[Inline?];    │   inline if n <= kBulkInlineMax
            │ work_item*            slices_heap;        │   heap if n > kBulkInlineMax
            └───────────────────────────────────────────┘
            alignas(kCacheLineSize)
```

`schedule_bulk` partitions `[0, n)` into `min(n, worker_count)` slices, lazily — each slice is a `work_item` that executes a contiguous index range. The first slice runs on whichever thread starts the op-state (the calling thread); the rest are pushed to a fan of randomly chosen worker deques. Workers steal from each other's slices as they finish their own. The receiver completes with `set_value()` (no value) once `remaining` hits zero, or with `set_stopped()` if `cancelled` was observed before the last slice finished.

Allocation policy:

- `n <= kBulkInlineMax = 64`: zero allocation. Slices live in the op-state's inline array.
- `n >  kBulkInlineMax`: one allocation from `get_allocator(env)`, sized `n_slices * sizeof(work_item)`. The bulk sender's annotation set carries `Allocates{Where::OpState}` in this case (the verifier checks the annotation matches the dynamic shape).

This is the only `StaticPool` path that may allocate after construction; the inline budget is sized so that the common bulk case (per-frame parallel-for over a fixed range) is heap-free.

### 4.5 Capability declaration

```cpp
struct [[=Async::Backend{Async::Backend::StaticPool}]]
       [[=Async::OffersBulk{}]]
       [[=Async::Cancellable{}]]
       scheduler { /* ... */ };
```

The consteval verifier confirms:

- `Concepts::Scheduler<scheduler>` ✓
- `Concepts::BulkScheduler<scheduler>` ✓ (annotation says yes; concept body says yes)
- `Concepts::ParallelScheduler<scheduler>` ✓ (`ProgressOf_v == parallel`)
- `Concepts::AffineScheduler<scheduler>` ✗ (no `Affine` annotation, no `IsAffine_v`)
- `Concepts::IoScheduler<scheduler>` ✗ (no `OffersIo` annotation)

`Traits::ProgressOf_v<scheduler> == parallel`.

### 4.6 Cancellation

Per §2.3. The `inplace_stop_callback` body for the basic `schedule()` op-state:

```cpp
void operator()() noexcept {
    auto expected = state_t::Pending;
    if (op->state.compare_exchange_strong(expected, state_t::Cancelled)) {
        // Won the race against the worker. Try to remove from queue;
        // failure is benign — the worker will see Cancelled in `state` and call
        // execute_stopped() instead of execute().
        op->ctx->try_remove(op);
    }
    // Lost the race: state == Running. Worker is executing. We do not block;
    // the work itself must observe the stop token at its next suspension.
}
```

For `schedule_bulk`, the callback flips `cancelled` to `true`. Workers about to run a slice check this flag before incrementing `remaining`-down; once flipped, workers skip remaining slices and decrement directly, so the receiver settles with `set_stopped()` instead of `set_value()`.

### 4.7 Allocation summary

| Site | When | Allocator |
|------|------|-----------|
| `context::context(config)` | once | global `operator new` (worker array, deques, MPMC ring) |
| `schedule()` op-state | never | inline in receiver |
| `schedule_bulk` (n ≤ 64) | never | inline in op-state |
| `schedule_bulk` (n > 64) | once per call | `get_allocator(env)` |
| stop-callback | never | inline `inplace_stop_callback` |

### 4.8 Domain rewrites

`StaticPool` registers exactly two `transform_sender` rewrites:

**R1. `continues_on` self-elision.** `continues_on(s, sched)` where the upstream `s` already advertises `get_completion_scheduler<set_value_t>(get_env(s)) == sched` is rewritten to `s` directly. Observable equivalence: the upstream's value-completion signature is unchanged; the only effect of `continues_on` would have been a redundant queue hop, which by definition cannot change observable values or signatures (it can only change completion thread identity, which is identical by the equality precondition).

**R2. `bulk → schedule_bulk` lowering.** `bulk(s, n, fn)` where `s == schedule(sched)` for this scheduler is rewritten to `schedule_bulk(sched, n, fn)`. The L3 `bulk(s, n, fn)` adaptor lowers by default to `let_value(s, [&]{ return when_all(N tasks); })`. The rewrite replaces this expansion with a direct `schedule_bulk` call. Observable equivalence: `bulk`'s declared completion signatures (`set_value_t()`, `set_stopped_t()`, propagated `set_error`) are equal to `schedule_bulk`'s by the `BulkScheduler` concept's contract; the rewrite is a structural fold that does not introduce a new completion path.

### 4.9 Equality semantics

Two `StaticPool::scheduler` values compare equal iff they share a `context::impl*`. The implication: a process can host multiple `StaticPool::context` instances (e.g. a "render" pool and a "background-IO-staging" pool), and adaptors that compare schedulers (e.g. `continues_on` self-elision) discriminate them correctly.

---

## 5. `Tbb` backend

### 5.1 Purpose and first-class status

The `Tbb` backend wraps `tbb::task_arena` as a stdexec scheduler. It is **first-class** alongside `StaticPool`, and that promotion is justified concretely:

- **Composition with existing TBB code.** Mashiro already uses `tbb::concurrent_queue`, `tbb::concurrent_hash_map`, and `tbb::parallel_for` in compute-heavy paths (mesh processing, scene-graph dirty propagation). Forcing those callers to migrate to `StaticPool` would re-implement TBB's mature stealing on top of ours; first-class TBB integration lets them migrate to senders incrementally without abandoning the runtime they already depend on.
- **Mature stealing and NUMA awareness.** oneTBB's task-stealing scheduler has been tuned over a decade for cache-locality, hyperthread pairing, and NUMA topology (`tbb::task_arena::constraints::numa_id`). `StaticPool`'s Chase-Lev deques are a known-good baseline, but they do not match TBB's heuristics on heterogeneous hardware. For workloads where this matters (large parallel reductions, tile-based renderer pipelines), `Tbb` is the right backend.
- **Flow-graph integration.** `tbb::flow::graph` is the right primitive for static dataflow with diamond / fan-in / fan-out shapes. The framework exposes this through a domain-level rewrite of L6's `pipeline(...)` pattern (§5.6) — without TBB as a first-class backend, that rewrite would have nowhere to lower to.
- **Cooperative scheduling.** Code that already calls `tbb::this_task_arena::isolate(...)` continues to work inside a Manager method that returns a Mashiro `Task<T>` running on the `Tbb` scheduler — the surrounding arena is the same one the user constructed, so isolation, priorities, and `task_group` interop are preserved.

### 5.2 Header sketch

```cpp
namespace Mashiro::Async::Backend::Tbb {

    class context;          // Owns a tbb::task_arena.
    class scheduler;        // Cheap handle.

    class context {
    public:
        struct config {
            int  max_concurrency = tbb::task_arena::automatic;
            int  reserved_for_master = 0;
            unsigned priority = tbb::task_arena::priority::normal;
            std::optional<tbb::task_arena::constraints> constraints;
            std::string name = "TbbArena";
        };

        explicit context(config = {});
        explicit context(tbb::task_arena& adopted) noexcept;     // adopt path
        ~context();

        scheduler get_scheduler() noexcept;
        tbb::task_arena& arena() noexcept;                        // escape hatch
        void  request_stop() noexcept;                            // stop_source

    private:
        struct impl;
        std::unique_ptr<impl> p_;
    };

    class [[=Async::Backend{Async::Backend::Tbb}]]
          [[=Async::OffersBulk{}]]
          [[=Async::Cancellable{}]]
          scheduler {
    public:
        scheduler() noexcept = default;
        bool operator==(const scheduler&) const noexcept = default;

        struct sender;
        [[nodiscard]] sender schedule() const noexcept;

        template<class Shape, class Fn>
        [[nodiscard]] auto schedule_bulk(Shape n, Fn fn) const noexcept;

        struct env { context::impl* p; };

    private:
        friend class context;
        explicit scheduler(context::impl* p) noexcept : p_{p} {}
        context::impl* p_ = nullptr;
    };

    struct domain {
        template<class Sender, class Env>
        decltype(auto) transform_sender(Sender&& s, const Env& env) const;
    };

}  // namespace Mashiro::Async::Backend::Tbb
```

`tbb::task_arena` is the underlying execution context. `context::context(config)` constructs a fresh arena with the requested concurrency / NUMA constraints; `context::context(tbb::task_arena&)` adopts an arena the caller already owns (e.g. the application's global arena) — Mashiro does not take ownership in that path; the caller's arena outlives the `context`.

### 5.3 `schedule()` op-state

```text
            tbb_op_state<Rcvr>
            ┌───────────────────────────────────────────┐
            │ Rcvr                  rcvr;               │
            │ context::impl*        ctx;                │
            │ std::atomic<state_t>  state{Pending};     │
            │ inplace_stop_callback cb;                 │
            └───────────────────────────────────────────┘
            alignas(kCacheLineSize)
```

`start()`:

1. Submits a closure to `ctx->arena.enqueue([this]{ this->execute(); })`. `tbb::task_arena::enqueue` is the cross-arena submission entry point; it does not require the calling thread to be a member of the arena, and it does not block on submission. TBB's scheduler picks up the task on whichever worker is available.
2. Registers `inplace_stop_callback`. On fire, the callback CASes `state` `Pending → Cancelled`. TBB does not expose a "cancel a single enqueued task" operation, so on win we cannot remove the task; instead the worker observes `state == Cancelled` at the start of `execute()` and calls `set_stopped(rcvr)` directly.

The op-state is inline in the receiver. There is no per-op heap allocation by the framework. TBB itself may allocate a small task control block inside `enqueue` — that allocation is TBB's, governed by `tbb::cache_aligned_allocator` from the arena's pool, and is reflected by the `Allocates{Where::External}` annotation on the bulk variant (the basic `schedule()` op-state does not declare this annotation because the allocation is TBB-internal and amortised across many tasks).

### 5.4 `schedule_bulk(n, fn)` lowers to `tbb::parallel_for`

This is the central reason TBB earns first-class status:

```cpp
template<class Shape, class Fn>
auto scheduler::schedule_bulk(Shape n, Fn fn) const noexcept {
    return Detail::tbb_bulk_sender<Shape, Fn>{p_, n, std::move(fn)};
}
```

The bulk op-state's `start()` calls

```cpp
ctx->arena.execute([&]{
    tbb::parallel_for(tbb::blocked_range<Shape>{0, n},
        [&](const tbb::blocked_range<Shape>& r) {
            for (Shape i = r.begin(); i != r.end(); ++i) fn(i);
        });
});
```

inside an `enqueue`-then-resume wrapper so the calling thread is not blocked. (`tbb::parallel_for` is itself blocking on the calling thread; the framework wraps it so that the calling thread observes a sender completion, not a `parallel_for` return.) Cancellation flips a flag stored in the op-state; the inner `parallel_for` lambda checks it and returns early. This relies on TBB's `tbb::task_group_context` cancellation — the bulk op-state owns a `task_group_context`, and the stop callback calls `cancel_group_execution()` on it. TBB then propagates cancellation through the sub-tasks.

Forward progress: `parallel`. Cancellation latency: bounded by TBB's worker-loop cooperative check (microseconds).

### 5.5 Tbb capability declaration

```cpp
struct [[=Async::Backend{Async::Backend::Tbb}]]
       [[=Async::OffersBulk{}]]
       [[=Async::Cancellable{}]]
       [[=Async::Allocates{Async::Allocates::Where::External}]]
       [[=Async::IsForwardProgress{stdexec::forward_progress_guarantee::parallel}]]
       scheduler { /* ... */ };
```

The consteval verifier (per `01-foundations.md` §8) confirms, given the declarations above:

- `Concepts::Scheduler<Tbb::scheduler>` ✓ — `schedule()` returns a sender whose value-completion
  scheduler is `*this`.
- `Concepts::BulkScheduler<Tbb::scheduler>` ✓ — `schedule_bulk(n, fn)` is well-formed per §5.4.
- `Concepts::ParallelScheduler<Tbb::scheduler>` ✓ — `ProgressOf_v == parallel`.
- `Concepts::AffineScheduler<Tbb::scheduler>` ✗ — no `Affine` annotation, no `IsAffine_v`.
- `Concepts::IoScheduler<Tbb::scheduler>` ✗ — no `OffersIo` annotation, no Io CPOs.

`Traits::AllocatesIn_v<Tbb::scheduler>` returns `{Where::External}` — the verifier reads the
annotation and records that TBB allocations originate from inside oneTBB's own pool, not from the
framework's hot path. L3 adaptors querying this tag plan their own inline-storage strategy
accordingly: a `bulk` lowering targeting `Tbb` does not allocate framework-side, because the
framework's storage stays inline in the receiver and TBB's task-block allocation is external.

### 5.6 Tbb cancellation

Per §2.3's general rule. TBB-specific details:

- The basic `schedule()` op-state owns no `task_group_context` of its own (the work is a single
  closure submitted via `tbb::task_arena::enqueue`). The stop callback CASes `state` from `Pending`
  to `Cancelled`; on win, the worker observes `Cancelled` at the start of `execute()` and routes
  to `set_stopped(rcvr)` directly. On loss, the worker has already begun executing; the cancel is
  observed at the next cooperative point inside the user's closure if it awaits a stop-aware
  sender, otherwise the closure runs to completion (consistent with §2.3's race resolution).
- The `schedule_bulk` op-state owns a `tbb::task_group_context` that the wrapping `parallel_for`
  inherits. The stop callback calls `ctx_->cancel_group_execution()`. TBB then propagates
  cancellation through the active sub-tasks: workers that are between iterations stop iterating;
  workers that are inside a user iteration finish that iteration (TBB's cancel is cooperative, not
  preemptive) before checking the group context again. The framework also flips the op-state's own
  `cancelled` atomic flag, which the inner lambda checks at the top of each blocked range so the
  first cancel observation does not wait for TBB's next worker poll.

Worker-observation latency is therefore bounded by max(one user-iteration duration, TBB's
cooperative check interval). On modern TBB releases the cooperative interval is in the low
microseconds; framework-internal flag checks add no measurable overhead.

For the basic `schedule()` op-state, the cancellation callback is registered conditionally:

```cpp
if constexpr (Concepts::HasStopToken<env_of_t<Rcvr>>) {
    cb_.emplace(stdexec::get_stop_token(stdexec::get_env(rcvr_)), StopFn{this});
}
```

When the receiver's environment exposes `never_stop_token`, no callback is registered and the
op-state pays no cancellation overhead (per `01-foundations.md` §4.4's `HasStopToken<Env>`
concept, added by `09-synthesis.md` §2.13).

### 5.7 Tbb allocation summary

| Site | When | Allocator |
|------|------|-----------|
| `context::context(config)` | once | constructs `tbb::task_arena`; arena initialisation may allocate inside oneTBB |
| `context::context(tbb::task_arena&)` | never (adopt) | the caller owns the arena |
| `schedule()` op-state | never (framework-side) | inline in receiver; TBB may allocate a small task block on `enqueue`, tagged `Allocates{External}` |
| `schedule_bulk` op-state | never (framework-side) | inline in receiver; `tbb::parallel_for` allocates internal blocked-range nodes from TBB's arena pool, tagged `Allocates{External}` |
| stop-callback | never | inline `inplace_stop_callback` |

The framework's hot path is heap-free. Any allocation visible to a profiler running against a
`Tbb`-rooted pipeline is TBB-internal and amortised across many tasks per oneTBB's documented
arena-pool policy. The `Allocates{Where::External}` annotation is the verifier's record that the
framework knows about it; downstream adaptors that need to avoid all allocation (including
external) reject `Tbb` at compile time via `Traits::AllocatesIn_v<S>` membership check.

### 5.8 Tbb domain rewrites

`Tbb::domain` registers three `transform_sender` rewrites. Each is followed by the
observable-equivalence proof required by §2.6.

**R1. `continues_on` self-elision.** `continues_on(s, sched)` where the upstream `s` already
advertises `get_completion_scheduler<set_value_t>(get_env(s)) == sched` is rewritten to `s`
directly. *Equivalence proof:* the upstream's value-completion signature is unchanged; the only
effect of `continues_on` would have been a redundant `tbb::task_arena::enqueue` round-trip, which
by definition cannot change observable values or signatures (it can only change completion-thread
identity, which is identical by the equality precondition). The completion-signature union before
and after the rewrite is type-identical: `completion_signatures_of_t<decltype(s)>` on both sides.

**R2. `bulk → schedule_bulk` lowering.** `bulk(s, n, fn)` where `s == schedule(sched)` for this
`Tbb::scheduler` is rewritten to `schedule_bulk(sched, n, fn)`. The L3 `bulk(s, n, fn)` adaptor
lowers by default to `let_value(s, [&]{ return when_all(N tasks); })`; the rewrite replaces this
expansion with a direct `schedule_bulk` call that calls `tbb::parallel_for` per §5.4.
*Equivalence proof:* the L3 `bulk` adaptor declares
`completion_signatures<set_value_t(), set_stopped_t(), set_error_t(exception_ptr)>`, identical to
`schedule_bulk`'s declared signatures (per the `BulkScheduler` concept contract in
`01-foundations.md` §4.4). Value-completion thread is `sched` on both paths (the original `bulk`
completes after `when_all` of senders that themselves completed on `sched`; `schedule_bulk`
completes on `sched` by §5.4). Error and stopped channels propagate identically. Therefore the
rewrite is a structural fold, not a semantic change.

**R3. L6 `pipeline(...)` lowering to `tbb::flow::graph`.** When the L6 pattern
`pipeline(stage1, stage2, ..., stageK)` is built over `sched` and the stage count `K` exceeds the
published threshold, the domain rewrites the pipeline to a `tbb::flow::graph` with one
`function_node` per stage and `make_edge`-connected ports. The threshold is published in the
header:

```cpp
namespace Mashiro::Async::Backend::Tbb {
    // Per 09-synthesis.md §2.23: tunable, not a contract. Bumping this constant is a
    // benchmark-driven decision, not a behavioural change. Documented as a tunable so
    // implementers can adjust without touching client code or rewrite logic.
    inline constexpr std::size_t pipeline_rewrite_threshold_v = 4;
}
```

The rewrite predicate is `K >= pipeline_rewrite_threshold_v`. Pipelines shorter than the
threshold use the default L6 lowering (a chain of `let_value`-coupled stage senders); pipelines at
or above the threshold benefit from TBB's flow-graph machinery — concurrent backpressure across
diamond shapes, per-node concurrency limits, and TBB's mature work-stealing across nodes — at the
cost of a small per-graph fixed setup. *Equivalence proof:* the L6 `pipeline(...)` adaptor declares
`completion_signatures<set_value_t(value_of_last_stage_t), set_stopped_t(), set_error_t(exception_ptr)>`
where `value_of_last_stage_t` is the value type produced by the final stage. The flow-graph
rewrite preserves this exact union: each `function_node<In, Out>` lifts a stage's value-completion
into TBB's edge transport (which preserves the value type by `Out` template parameter); the
graph's `output_port` of the final node delivers `value_of_last_stage_t` to the framework's
receiver via a terminal `function_node<value_of_last_stage_t, std::tuple<>>` that calls
`stdexec::set_value(rcvr, std::move(out))`. Error propagation: any node that observes an exception
inside its body routes through the graph's exception channel (TBB exposes it via
`tbb::flow::graph::wait_for_all` exception propagation); the rewrite installs a graph-level
exception handler that calls `set_error(rcvr, current_exception())`. Stopped propagation: the
graph's `task_group_context` is cancelled on stop-token fire (same wiring as §5.6), and the
graph's terminal node observes `is_group_execution_cancelled()` then calls
`set_stopped(rcvr)`. Signature union before and after: identical. Value, error, and stopped
channels carry identical types on both sides of the rewrite.

The threshold value `4` is justified by `09-synthesis.md` §2.23's note that the choice is a
tunable: under that constant, the four-stage pipeline gains the most from flow-graph parallelism
because three or fewer stages do not exhibit enough work-stealing opportunities to amortise the
graph's setup overhead. Empirical re-tuning happens in the implementation phase against the
benchmark suite; the public constant lets re-tuners change one number rather than rewrite logic.

### 5.9 Tbb equality semantics

Two `Tbb::scheduler` values compare equal iff their `context::impl*` are the same pointer (i.e.
they refer to the same `tbb::task_arena`). The implication: an application can host multiple
`Tbb::context` instances (a global arena for compute, a constrained arena for low-priority
background work, a separately-NUMA-pinned arena for one socket), and adaptors that compare
schedulers — including the §5.8 R1 self-elision rewrite — discriminate them correctly. Adopting an
already-owned `tbb::task_arena` via `context::context(tbb::task_arena&)` shares equality with any
other `context` constructed against the same arena (because the same arena instance is recorded in
the `impl`); this is intentional and makes interop with TBB-using third-party code transparent.

---

## 6. `Platform` backend

### 6.1 Purpose

`Platform` is the OS-affinity scheduler — the single thread that owns the message loop on Win32,
the AppKit main queue on macOS, the X11 / Wayland event connection on Linux. It is the only
scheduler in this spec whose `schedule()` always completes on a *fixed*, *statically-known* thread.
All UI work, all OS-handle ownership, and any Manager whose `ManagerThreading::Platform` contract
binds it to the platform thread runs through this backend.

This spec re-exports the existing scheduler type defined in
`2026-06-11-platform-thread-infrastructure-design.md` §6.4 (`Mashiro::Platform::scheduler`). **No
modification to the platform-thread layer is made by this spec.** The `Async::Backend::Platform`
namespace adds exactly two things on top of the platform scheduler:

1. The L1 capability annotations the rest of the framework's verifier requires.
2. The L2 alias name (`Async::Backend::Platform::scheduler`) so client code can spell the platform
   scheduler in the same shape as the other four backends.

The platform-thread spec's existing components — `PlatformThread`, the `MpscQueue<handle, 256>`
mailbox, `wakeEvent_`, `Mashiro::Platform::domain`, `Mashiro::Platform::stop_source`,
`Mashiro::Platform::Task<T>`, `Mashiro::Platform::scope` — remain in place and remain authoritative
for behaviour. This layer is a pure alias + annotation overlay.

### 6.2 Header sketch

```cpp
// Mashiro/include/Mashiro/Async/Backend/Platform.h
#pragma once
#include <Mashiro/Platform/PlatformThread.h>   // brings Mashiro::Platform::scheduler in

namespace Mashiro::Async::Backend::Platform {

    // L2 alias of the platform-thread scheduler. Capability annotations are attached
    // by a small wrapper type so the underlying Mashiro::Platform::scheduler stays
    // annotation-free (the platform-thread spec is independent of the Async framework).
    class [[=Async::Backend{Async::Backend::Platform}]]
          [[=Async::Affine{Async::Backend::Platform}]]
          [[=Async::Cancellable{}]]
          [[=Async::IsForwardProgress{stdexec::forward_progress_guarantee::concurrent}]]
          scheduler {
    public:
        using __id = scheduler;
        using __t  = scheduler;

        scheduler() noexcept = default;
        explicit scheduler(Mashiro::Platform::PlatformThread& owner) noexcept : inner_{owner} {}
        explicit scheduler(Mashiro::Platform::scheduler s) noexcept : inner_{s} {}

        bool operator==(const scheduler&) const noexcept = default;

        // Pass-through: every sender / env / domain hook delegates to the inner scheduler.
        [[nodiscard]] auto schedule() const noexcept { return inner_.schedule(); }

        struct env : Mashiro::Platform::scheduler::env {};

        friend Mashiro::Platform::domain
        tag_invoke(stdexec::get_domain_t, scheduler s) noexcept {
            return stdexec::get_domain(s.inner_);
        }

    private:
        Mashiro::Platform::scheduler inner_;
    };

    // Convenience: the canonical handle for the process-global platform thread.
    [[nodiscard]] scheduler get_scheduler() noexcept;

}  // namespace Mashiro::Async::Backend::Platform
```

The `scheduler` wrapper is a single pointer wide (the inner `Mashiro::Platform::scheduler` is one
`PlatformThread*`). Two `Backend::Platform::scheduler` values compare equal iff their inner
platform schedulers compare equal — i.e. iff they refer to the same `PlatformThread`. The
annotation `Affine{Async::Backend::Platform}` makes the scheduler model `AffineScheduler` per
`01-foundations.md` §4.4.

### 6.3 `schedule()` op-state

```text
            Backend::Platform::scheduler::sender
              ≡ Mashiro::Platform::scheduler::sender         (per 2026-06-11 §6.5)

            op_state<Rcvr>
            ┌───────────────────────────────────────────────────┐
            │ Rcvr                              rcvr;            │
            │ PlatformThread*                   owner;           │
            │ std::optional<inplace_stop_callback<StopFn>> cb;   │
            └───────────────────────────────────────────────────┘
```

The op-state is exactly the one defined in `2026-06-11-platform-thread-infrastructure-design.md`
§6.5. This spec adds no member, no atomic, no extra storage. `start()`:

1. If the calling thread is the platform thread, calls `set_value(rcvr)` synchronously
   (`await_ready == true` fast path, zero suspension).
2. Otherwise registers the `inplace_stop_callback` and `SubmitResume`s the coroutine handle onto
   the platform thread's `MpscQueue<handle, 256>` mailbox; `wakeEvent_` is signalled inside
   `SubmitResume`.
3. When the platform thread drains the handle, it calls `op_state::resume_on_platform()`, which
   resets the callback and calls `set_value(rcvr)`.

The L2 wrapper adds no behaviour at any of these steps.

### 6.4 Capability declaration

```cpp
struct [[=Async::Backend{Async::Backend::Platform}]]
       [[=Async::Affine{Async::Backend::Platform}]]
       [[=Async::Cancellable{}]]
       [[=Async::IsForwardProgress{stdexec::forward_progress_guarantee::concurrent}]]
       scheduler { /* ... */ };
```

The consteval verifier confirms:

- `Concepts::Scheduler<Backend::Platform::scheduler>` ✓
- `Concepts::AffineScheduler<Backend::Platform::scheduler>` ✓ — `Affine` annotation present,
  `Traits::IsAffine_v == true`.
- `Concepts::ParallelScheduler<Backend::Platform::scheduler>` ✗ — `ProgressOf_v == concurrent`,
  which is **strictly less than** `parallel`. The platform thread is single-threaded; concurrent
  forward progress is trivially satisfied with itself, but it is not parallel.
- `Concepts::BulkScheduler<Backend::Platform::scheduler>` ✗ — no `OffersBulk` annotation, no
  `schedule_bulk` member.
- `Concepts::IoScheduler<Backend::Platform::scheduler>` ✗ — no `OffersIo` annotation, no Io CPOs.

`Traits::ProgressOf_v<Backend::Platform::scheduler> == concurrent`. `Traits::AffinityOf_v` returns
`Backend::Platform`. `Traits::IsAffine_v == true`.

### 6.5 v0.2 note — Platform is not a BulkScheduler

Per `09-synthesis.md` §2.7, `Platform` does **not** model `BulkScheduler`. Code attempting `bulk`
over Platform is rejected by L3's `static_assert`; use `Tbb` or `StaticPool` for parallel work,
then `continues_on(Platform)` to land on the platform thread when the parallel phase completes.

Rationale: `bulk` either fans the children out across multiple threads (defeating the
single-thread affinity the platform thread guarantees) or serialises them on the platform thread
(in which case the algorithm is no longer doing parallel work and the user wanted a plain
`for`-loop, not `bulk`). Both outcomes are wrong; the static rejection forces the user to make the
choice explicit at compile time. The L3 rejection diagnostic names the platform scheduler and
points to this section.

```cpp
// L3 Adaptor/Bulk.h (excerpt; full text in 03-adaptors.md §13).
template<Concepts::Scheduler Sched, class Shape, class Fn>
auto bulk(Sched sched, Shape n, Fn fn) {
    static_assert(Concepts::BulkScheduler<Sched>,
        "bulk requires a BulkScheduler. Platform is single-threaded and intentionally "
        "does not model BulkScheduler — see 02-backends.md §6.5. For parallel work, "
        "use Tbb or StaticPool, then continues_on(Platform) to return to the platform thread.");
    // ... lowering ...
}
```

### 6.6 Cancellation

Defers to the platform-thread spec's existing `inplace_stop_source` (per
`2026-06-11-platform-thread-infrastructure-design.md` §6.8 and `09-synthesis.md` §2.14). No bridge
is needed because the platform layer already uses `stdexec::inplace_stop_source` /
`inplace_stop_token` natively. The L2 wrapper does not interpose a callback of its own; the
underlying op-state's existing callback (registered in
`Mashiro::Platform::scheduler::sender::op_state::start`) is the only cancellation point, and it
follows the contract documented at §2.3 — best-effort cancel against the MPSC mailbox, with the
stop-token observable to the resumed coroutine on the platform thread if the cancel loses the race
against the handle being dequeued.

`bridge_stop_token(std::stop_token)` (per `01-foundations.md` §9) is **never** needed inside the
framework against `Platform`. It exists exclusively for boundary interop with code that hands the
framework a `std::stop_token` (e.g. a `jthread` owned by user code). The platform thread's
internal stop is `inplace_stop_source` end-to-end.

### 6.7 Allocation

Zero per-op allocation, by inheritance from the platform-thread spec §6.5. The platform thread's
`MpscQueue<handle, 256>` is preallocated at `PlatformThread::Run()` entry; `wakeEvent_` is a
single OS handle; the per-op `inplace_stop_callback` is inline in the op-state. No allocation
happens on `schedule()`, on cancellation, or on completion. Bursts past 256 in-flight handles
return `false` from `MpscQueue::TryPush` (observable back-pressure); cross-thread awaiters
terminate per the contract documented at §6.5 of the platform-thread spec. There is no heap
fallback path.

### 6.8 Domain rewrites

The L2 `Backend::Platform` namespace does not register a new domain. It re-exports the existing
`Mashiro::Platform::domain` (per `2026-06-11-platform-thread-infrastructure-design.md` §6.8) via
the `tag_invoke(get_domain_t, scheduler)` hook in the wrapper. That domain already rewrites
`continues_on(any, Platform)` to a platform-appropriate wake:

- **Win32:** `PostThreadMessage(plat_tid, MASHIRO_RESUME_MSG, lparam, ...)` — the message-pump
  inside `PlatformThread::Run()` dispatches `MASHIRO_RESUME_MSG` to a handler that calls
  `coroutine_handle.resume()`. This avoids the `wakeEvent_` + `MsgWaitForMultipleObjectsEx`
  round-trip when the calling thread can talk directly to the message queue.
- **macOS / AppKit:** `dispatch_async(dispatch_get_main_queue(), ^{ handle.resume(); })` — the
  main-queue dispatcher resumes the coroutine on the next runloop tick.
- **Linux / X11:** posts an X11 client message to the connection's event queue; the X11 main loop
  inside `PlatformThread::Run()` dispatches it the same way as for Win32 messages.
- **Linux / Wayland:** writes a single byte to a dedicated `eventfd` the Wayland event loop is
  polling; the event-loop handler reads the byte and resumes the coroutine.

*Equivalence proof:* the default lowering for `continues_on(s, plat)` posts the receiver's
coroutine handle onto `MpscQueue<handle, 256>` and signals `wakeEvent_`; the platform-rewritten
path posts via `PostThreadMessage` / `dispatch_async` / X11-client-message / `eventfd` and
resumes through the same `coroutine_handle.resume()` call site. Both paths complete with
`set_value` on the platform thread; both observe the same stop-token through the same
`inplace_stop_callback` chain; both share the same completion-signature union
`completion_signatures<set_value_t(), set_stopped_t()>` (per
`2026-06-11-platform-thread-infrastructure-design.md` §6.5). The rewrite changes the transport
mechanism, not the observable signal. Per §2.6's contract, that is the only kind of change a
domain rewrite is permitted to make.

### 6.9 Equality semantics

Two `Backend::Platform::scheduler` values compare equal iff they wrap inner
`Mashiro::Platform::scheduler` values that compare equal — i.e. iff they refer to the same
`PlatformThread`. In practice a process has exactly one platform thread, so the equality
discriminator is rarely exercised by application code; tests that construct multiple
`PlatformThread` instances (which the platform-thread spec §5.2 permits for test fixtures only)
observe the equality discriminator working correctly.

---

## 7. `Io` backend

### 7.1 Purpose and concept

`Io` is the proactor backend: a stdexec-style asynchronous I/O scheduler whose sender adaptors
(`async_read`, `async_write`, `async_accept`, `async_connect`, `async_timer`) submit operations to
an OS proactor and complete the receiver when the kernel reports completion. There is **one**
`IoContext` per backend instance; each context owns either one io_uring submission/completion
queue pair (Linux ≥ 5.13) or one IOCP completion port (Windows), plus a single dedicated reaper
thread that pumps completions and calls the receivers' completion handlers.

`Io` does not implement file or socket abstractions itself — those are L3 / user-layer concerns.
It owns the *submission and completion plumbing*, and exposes that plumbing through senders.
Forward-progress is `concurrent` (single reaper thread, kernel makes progress in parallel inside);
operations are cancellable through the receiver's stop-token, which the backend translates into
`IORING_OP_ASYNC_CANCEL` (Linux) or `CancelIoEx` (Windows).

### 7.2 Header sketch

```cpp
// Mashiro/include/Mashiro/Async/Backend/Io.h
#pragma once
#include <Mashiro/Async/Foundations.h>     // brings get_io_context_t (fwd decl from §4.5)
#include <Mashiro/Async/Concepts.h>
#include <chrono>
#include <span>

namespace Mashiro::Async::Backend::Io {

    class context;        // Owns proactor (io_uring on Linux, IOCP on Windows) + reaper thread.
    class scheduler;      // Cheap handle; equal iff same context.

    class context {
    public:
        struct config {
            unsigned    sq_entries = 256;             // io_uring submission queue capacity
            unsigned    cq_entries = 1024;            // io_uring completion queue capacity
            std::size_t iocp_overlapped_pool = 1024;  // Windows OVERLAPPED freelist capacity
            std::string name = "IoContext";
        };

        explicit context(config = {});
        ~context();                                   // signals stop, joins reaper, closes ring/port
        context(const context&)            = delete;
        context& operator=(const context&) = delete;

        scheduler get_scheduler() noexcept;
        void      request_stop() noexcept;
        bool      stop_requested() const noexcept;

    private:
        struct impl;
        std::unique_ptr<impl> p_;
    };

    class [[=Async::Backend{Async::Backend::Io}]]
          [[=Async::OffersIo{}]]
          [[=Async::Cancellable{}]]
          [[=Async::Allocates{Async::Allocates::Where::OpState}]]
          [[=Async::Allocates{Async::Allocates::Where::External}]]
          [[=Async::IsForwardProgress{stdexec::forward_progress_guarantee::concurrent}]]
          scheduler {
    public:
        using __id = scheduler;
        using __t  = scheduler;

        scheduler() noexcept = default;
        bool operator==(const scheduler&) const noexcept = default;

        struct sender;
        [[nodiscard]] sender schedule() const noexcept;

        struct env { context::impl* p; /* completion_scheduler hooks */ };

    private:
        friend class context;
        explicit scheduler(context::impl* p) noexcept : p_{p} {}
        context::impl* p_ = nullptr;
    };

    // I/O sender adaptors. Each returns a sender whose op-state is incomplete in the header
    // (forward-declared in detail::) so the framework can keep op-state ABI stable. Public size
    // constants below let L3 adaptors plan inline storage against this ABI.
    sender async_read   (scheduler s, int fd_or_handle, std::span<std::byte>       buf);
    sender async_write  (scheduler s, int fd_or_handle, std::span<const std::byte> buf);
    sender async_accept (scheduler s, int listener_fd_or_handle);
    sender async_connect(scheduler s, int sock_fd_or_handle, /* endpoint */ const void* ep, std::size_t ep_size);
    sender async_timer  (scheduler s, std::chrono::nanoseconds dur);

    // Public size constants — L3 adaptors plan inline storage against these.
    // Bumping a size is an ABI break and is documented in the file header.
    inline constexpr std::size_t timer_op_state_size_v = /* sizeof(detail::TimerOpState) */ 64;
    inline constexpr std::size_t read_op_state_size_v  = /* sizeof(detail::ReadOpState)  */ 96;
    inline constexpr std::size_t write_op_state_size_v = /* sizeof(detail::WriteOpState) */ 96;

    // Domain. Registers Io-specific rewrites (see §7.6).
    struct domain {
        template<class Sender, class Env>
        decltype(auto) transform_sender(Sender&& s, const Env& env) const;
    };

    // Completes the forward declaration in 01-foundations.md §4.5.
    // Returns the IoContext that the scheduler binds to. Used by L3 adaptors that need
    // to thread submissions through a known context (e.g. async_read called on a scheduler
    // routed via an environment query).
    [[nodiscard]] context& tag_invoke(get_io_context_t, scheduler s) noexcept;

}  // namespace Mashiro::Async::Backend::Io
```

The op-state types `detail::TimerOpState`, `detail::ReadOpState`, `detail::WriteOpState` remain
**incomplete** in the public header. Only their sizes are exposed. This is the ABI-stability lever
required by `09-synthesis.md` §2.6: L3 adaptors planning inline storage (e.g. `timeout(d)`
composed with `async_timer` wants to know how much space the timer op-state needs) read the
public `*_op_state_size_v` constants; the implementation can change op-state layout freely as
long as the published size does not shrink. A size bump is an ABI break and is documented in the
file header comment.

The actual values `64`, `96`, `96` above are placeholders for the implementation phase — the spec
fixes the *contract* (these are the names L3 reads), not the values. The implementation publishes
exact sizes by `sizeof(detail::TimerOpState)` etc. once the op-state types are finalised; the
header comment near the constants records the policy that increases are ABI breaks.

The `tag_invoke(get_io_context_t, scheduler)` overload completes the L0 CPO declaration in
`01-foundations.md` §4.5. Its body is one pointer dereference (`return *s.p_;`), giving L3
adaptors a stable way to reach the underlying context without naming `Io::context` by type.

### 7.3 Op-state shapes

`async_read`:

```text
            Io::detail::ReadOpState<Rcvr>
            ┌───────────────────────────────────────────────────────┐
            │ Rcvr                              rcvr;                │
            │ context::impl*                    ctx;                 │
            │ int                               fd_or_handle;        │
            │ std::span<std::byte>              buf;                 │
            │ // Linux:  sqe_user_data_t        user_data;           │
            │ // Windows: OVERLAPPED            ov;  (16 B for body) │
            │ std::atomic<state_t>              state{Pending};      │
            │ std::optional<inplace_stop_callback<StopFn>> cb;       │
            └───────────────────────────────────────────────────────┘
            alignas(kCacheLineSize)
```

`start()`:

- **Linux:** acquires a free SQE slot from `ctx->ring.sq`, fills it with
  `IORING_OP_READ`/`IORING_OP_READV`, sets `user_data` to the op-state's address (passed back
  in the CQE), advances the SQ tail, kicks the ring (`io_uring_enter` if needed, else relying on
  the reaper's poll). Registers the stop callback.
- **Windows:** acquires an `OVERLAPPED` from the per-context freelist, fills it,
  `ReadFile(handle, buf.data(), buf.size(), nullptr, &op->ov)`. If `ReadFile` returns synchronously
  (`ERROR_IO_PENDING` not set), completes immediately on the calling thread. Else the IOCP reaper
  picks up the completion via `GetQueuedCompletionStatusEx`. Registers the stop callback.

On completion the reaper thread looks up the op-state from the CQE `user_data` (Linux) or the
`OVERLAPPED` container_of (Windows) and calls `set_value(rcvr, bytes_read)` or
`set_error(rcvr, std::system_error{...})` or `set_stopped(rcvr)`.

`async_write` is structurally identical with `IORING_OP_WRITE` / `WriteFile`.

`async_timer`:

```text
            Io::detail::TimerOpState<Rcvr>
            ┌───────────────────────────────────────────────────────┐
            │ Rcvr                              rcvr;                │
            │ context::impl*                    ctx;                 │
            │ std::chrono::nanoseconds          dur;                 │
            │ // Linux:  __kernel_timespec      ts;                  │
            │ // Linux:  sqe_user_data_t        user_data;           │
            │ // Windows: HANDLE                timer_handle;        │
            │ std::atomic<state_t>              state{Pending};      │
            │ std::optional<inplace_stop_callback<StopFn>> cb;       │
            └───────────────────────────────────────────────────────┘
            alignas(kCacheLineSize)
```

`start()`:

- **Linux:** submits `IORING_OP_TIMEOUT` with `ts` set to `dur`, `count=1`, `user_data` to the
  op-state.
- **Windows:** creates a waitable timer with `CreateWaitableTimerExW`, sets it with
  `SetWaitableTimer(timer_handle, &due, 0, nullptr, nullptr, FALSE)`, registers the timer with
  the IOCP via `BindIoCompletionCallback` or an equivalent thread-pool wait.

Completion calls `set_value(rcvr)` on timer expiry, `set_stopped(rcvr)` on cancellation.

No allocation per `async_*` call: the op-state lives inline inside the connected receiver's
storage (the L3 adaptor that composes `async_read` is responsible for siting the op-state inside
its own op-state, which is itself inline in the higher-up receiver). The kernel-side state (SQE
slot on Linux, `OVERLAPPED` on Windows) is drawn from the context's preallocated pool, not from
the heap.

### 7.4 Cancellation wiring

Per §2.3. The Io-specific cancel path:

- **Linux:** the stop callback submits an `IORING_OP_ASYNC_CANCEL` SQE whose `addr` field is the
  `user_data` of the target op-state. io_uring matches the cancel against the in-flight SQE and
  posts a CQE for the original operation with `-ECANCELED`; the reaper observes the negative
  result and routes to `set_stopped(rcvr)` instead of `set_value`.
- **Windows:** the stop callback calls `CancelIoEx(handle, &op->ov)`. The kernel marks the
  operation cancelled; the IOCP reaper observes `GetOverlappedResult` returning false with
  `ERROR_OPERATION_ABORTED` and routes to `set_stopped(rcvr)`.

Race resolution between cancellation and natural completion uses the per-op atomic `state`
word: CAS from `Pending` to `CancelRequested` on stop-callback fire; the reaper CAS from
`Pending`/`CancelRequested` to `Completed` on natural completion or to `Cancelled` on cancel CQE.
The state word is the single source of truth for which completion channel fires; both the cancel
path and the natural-completion path read it, and the channel chosen is whichever transition
won the CAS.

Bridging is unnecessary: the receiver's stop-token is already `inplace_stop_token` end-to-end (per
`01-foundations.md` §4.4 / §9), so the callback shape is the framework-standard one and no
adaptation happens at the Io boundary.

### 7.5 Allocation summary

| Site | When | Allocator |
|------|------|-----------|
| `context::context(config)` Linux | once | `mmap` of io_uring SQ and CQ; tagged `Allocates{External}` |
| `context::context(config)` Windows | once | `CreateIoCompletionPort` + `operator new` for the OVERLAPPED freelist (`config.iocp_overlapped_pool` entries); tagged `Allocates{External}` |
| reaper thread | once | `std::jthread` construction |
| per-op state (read/write/timer/accept/connect) | never | inline in the receiver's storage |
| OVERLAPPED on Windows | drawn from freelist | freelist; overflow falls back to `operator new` and the spent OVERLAPPED is `delete`d after completion (rare) |
| SQE on Linux | drawn from ring | the ring is preallocated; SQ overflow returns submission failure observable to the caller (the framework retries after the reaper drains; never blocks on submission for steady-state load) |
| stop-callback | never | inline `inplace_stop_callback` |

The hot path (per-op `start()`) is heap-free in the steady state. The Windows freelist overflow
path is documented as the only post-construction allocation site; the freelist capacity is sized
at construction so that overflow is rare in practice (1024 in-flight OVERLAPPED records covers
typical server workloads). The `Allocates{Where::External}` annotation captures the kernel-side
allocation (the io_uring rings live in kernel memory); the `Allocates{Where::OpState}` annotation
captures the per-op kernel-state storage. Both are queryable via `Traits::AllocatesIn_v<scheduler>`.

### 7.6 Domain rewrites

`Io::domain` registers one rewrite of interest at the L2 layer:

**R1. Timer-fusion.** `async_timer(d1) | timeout(d2)` where both `d1` and `d2` are
`consteval`-evaluable folds to a single `async_timer(min(d1, d2))` with the value-or-stopped
selection chosen at the resulting deadline. *Equivalence proof:* the upstream `async_timer(d1)`
completes with `set_value()` at time `d1`; `timeout(d2)` wrapping it completes with `set_stopped`
at time `d2` if the upstream has not yet completed. The combined behaviour is therefore:

- if `d1 <= d2`: completes with `set_value` at `d1`;
- if `d1 >  d2`: completes with `set_stopped` at `d2`.

A single timer at `min(d1, d2)` reproduces this behaviour exactly when paired with the right
channel: at `min(d1, d2)`, fire `set_value` if `d1 == min(d1, d2)`, fire `set_stopped` otherwise.
The rewritten op-state stores one boolean (the channel choice) instead of two timers' worth of
state; both completion signatures (`set_value_t()`, `set_stopped_t()`) are preserved as a union
identical to the original `timeout(async_timer(d1), d2)`. This rewrite halves kernel timer
submissions for the common "fire-or-give-up" pattern.

Runtime `d1` / `d2` cases are not rewritten — the predicate "both deadlines are `consteval`" is
checked at sender-construction time and falls through to the default lowering when either is
runtime-valued. This keeps the rewrite a pure compile-time fold per §2.6.

L3 adaptor-level Io rewrites (e.g. `read | timeout` lowering to `read` with a linked timeout SQE)
are documented in `03-adaptors.md`; they reuse the Io context's submission machinery and are not
L2 concerns.

### 7.7 Equality semantics

Two `Io::scheduler` values compare equal iff their `context::impl*` are the same pointer — i.e.
iff they refer to the same `IoContext`, which means the same io_uring instance (Linux) or the
same IOCP port (Windows), and the same reaper thread. An application may host multiple
`Io::context` instances (one per network namespace; one per shard in a sharded server design),
and the equality discriminator distinguishes them correctly.

### 7.8 Forward progress

`Traits::ProgressOf_v<Io::scheduler> == concurrent`. The single reaper thread is the bottleneck on
the user-space side, but the kernel makes progress in parallel on the device side (the io_uring
SQ poller, or the IOCP completion port's internal threadpool on older Windows configurations).
For workloads that need the user-space side to be parallel, the framework's convention is to
front the Io scheduler with a `StaticPool` or `Tbb` worker pool — the I/O sender completes on the
reaper thread, then a `continues_on(WorkerPool)` hop moves the result onto a parallel pool. This
keeps the Io reaper hot-path lean (it does nothing but dispatch).

---

## 8. Cross-backend pipeline example

This section illustrates the framework's central composition guarantee: a single sender
expression can route work through three backends — `Tbb` for parallel compute, `Platform` for
OS-affinity presentation, `Io` for asynchronous output — without any virtual dispatch, without
any boundary erasure, and with one cancellation token that threads all three.

### 8.1 The scenario

A frame in a renderer's capture pipeline needs to:

1. **Compute** — run a parallel transform over a vertex buffer (per-vertex skinning) and produce
   a packed framebuffer-staging blob. The work is CPU-bound and embarrassingly parallel; it
   belongs on `Tbb`'s `schedule_bulk`.
2. **Present** — hand the staging blob to a platform-thread-owned render buffer that uploads it
   to a Vulkan swapchain. This step touches OS-owned handles (the platform-thread's renderer
   manager) and must run on the platform thread.
3. **Capture** — write the framebuffer bytes to a memory-mapped capture file. This is a kernel
   I/O operation that benefits from being asynchronous; it belongs on the `Io` backend's
   `async_write`.

### 8.2 The sender expression

```cpp
using namespace Mashiro::Async;

auto frame_pipeline(Backend::Tbb::scheduler      compute,
                    Backend::Platform::scheduler platform,
                    Backend::Io::scheduler       io,
                    std::span<Vertex>            vertices,
                    std::span<std::byte>         staging_buffer,
                    int                          capture_fd_or_handle) {
    return
        // Phase 1 — parallel skinning on Tbb (bulk over vertex count).
        stdexec::schedule(compute)
        | stdexec::let_value([=] {
            return stdexec::schedule_bulk(compute, vertices.size(),
                [=](std::size_t i) {
                    staging_buffer[i] = pack(skin(vertices[i]));
                });
          })
        // Phase 2 — hop to the platform thread and present.
        | stdexec::continues_on(platform)
        | stdexec::then([=] {
            // Now executing on the platform thread — safe to touch the renderer manager.
            PlatformRenderer::Instance().PresentStagingBuffer(staging_buffer);
          })
        // Phase 3 — hop to the Io reaper and write the framebuffer to the capture file.
        | stdexec::continues_on(io)
        | stdexec::let_value([=] {
            return Backend::Io::async_write(io, capture_fd_or_handle, staging_buffer);
          })
        // Final value channel: the byte count returned by async_write.
        | stdexec::then([](std::size_t bytes_written) {
            FrameCapture::RecordWritten(bytes_written);
          });
}
```

### 8.3 What each transition does

**Compute → Platform (`continues_on(platform)`).** Resolved by the Platform domain's rewrite of
`continues_on(any, Platform)` per §6.8. The Tbb worker that finished the last bulk slice posts
the receiver's continuation onto the platform thread via the platform-appropriate transport
(`PostThreadMessage` on Win32, `dispatch_async` on macOS, X11 client message on X11, `eventfd`
write on Wayland). No queue hop through Mashiro::MpscQueue here when the platform domain rewrite
is active — the OS message channel is the wake. The Tbb thread does not block; it returns to its
worker loop. The platform thread picks up the message at its next pump iteration and resumes the
coroutine into the `then` lambda. Per §6.8's equivalence proof, the completion signatures
(`set_value_t()`, `set_stopped_t()`) are preserved across the rewrite.

**Platform → Io (`continues_on(io)`).** No domain rewrite applies — the Io context does not
expose a "post a continuation" CPO, only I/O submission CPOs. The default `continues_on` lowering
runs: on the platform thread, the receiver's continuation is enqueued onto an Io-context-internal
work queue (the reaper thread drains it between completion-port polls). The platform thread does
not block; it returns to its loop. The Io reaper resumes the coroutine into the `let_value`
lambda. Then `async_write(io, capture_fd, staging_buffer)` submits a real I/O operation: on Linux
it acquires an SQE slot, writes `IORING_OP_WRITE`, advances the SQ tail; on Windows it acquires
an OVERLAPPED from the freelist, calls `WriteFile`. When the kernel completes the write, the
reaper observes the CQE / completion-port event and calls `set_value(rcvr, bytes_written)`.

### 8.4 Completion-signature union

Each stage contributes signatures; the union (per `01-foundations.md` §7.2's `union_signatures`)
of the whole pipeline is:

```cpp
completion_signatures<
    set_value_t(),                          // final then() returns void
    set_error_t(std::exception_ptr),        // any stage's exception propagates here
    set_stopped_t()                         // any stage's cancellation lands here
>
```

Per-stage breakdown:

- `schedule(compute) | schedule_bulk`: `set_value_t(), set_stopped_t()` (Tbb's bulk signatures).
- `continues_on(platform)`: preserves `set_value_t(), set_stopped_t()` (per §6.8); no new error
  channel.
- `then` on platform: `then` may throw, so `set_error_t(std::exception_ptr)` is introduced.
- `continues_on(io)`: preserves; no new error channel.
- `async_write(io, ...)`: `set_value_t(std::size_t), set_error_t(std::system_error),
  set_stopped_t()` — the `system_error` is widened to `std::exception_ptr` by `union_signatures`
  when merged with the upstream `then`'s `set_error_t(std::exception_ptr)`.
- Final `then`: `set_value_t()` (returns void), `set_error_t(std::exception_ptr)`.

The union collapses to the three-channel signature above, declared explicitly on the pipeline by
stdexec's `completion_signatures_of_t` deduction. No surprise channels appear.

### 8.5 Cancellation token threading

The pipeline receives an `inplace_stop_token` from its enclosing receiver's environment (e.g. the
`Scope` it was spawned into — see L5). The token is threaded into every stage's op-state by
sender-receiver wiring; each backend registers its own `inplace_stop_callback` against the same
token:

- **Tbb stage**: the bulk op-state's callback (per §5.6) flips the op-state's `cancelled` atomic
  and calls `tbb::task_group_context::cancel_group_execution()`. In-flight slices observe the
  cancel cooperatively; the bulk completes with `set_stopped`.
- **Platform stage**: the platform scheduler's op-state callback (per §6.6 / platform spec §6.5)
  CASes the MPSC entry's state to `Cancelled`. If the entry has not yet been dequeued, the
  resume-handler observes `Cancelled` and routes to `set_stopped`. If the entry has been
  dequeued, the `then` lambda runs to completion (best-effort cancel).
- **Io stage**: the Io op-state's callback (per §7.4) submits `IORING_OP_ASYNC_CANCEL` (Linux) or
  calls `CancelIoEx` (Windows). The reaper observes the cancel completion and routes the receiver
  to `set_stopped`.

Stop-token propagation does not require any framework code in `frame_pipeline` itself — it is
handled by the sender-receiver concept's environment plumbing. Cancellation from any source (the
enclosing scope, a sibling task that errored out, a user signal) lands cleanly at whichever stage
is currently in-flight; no stage leaks a half-completed kernel submission, no platform message
sits unhandled in the MPSC ring, no Tbb worker slice fails to observe the cancel.

### 8.6 Analysis

The cross-backend example is two paragraphs of value:

The first paragraph is **what the type system enforces statically**. There is no `any_scheduler`
in the pipeline; every `continues_on` call sees a statically-typed scheduler on both sides, so
the domain rewrites (Platform's `continues_on` rewrite, Tbb's `bulk` lowering, Io's
timer-fusion-if-applicable) all happen at sender-construction time, not at execution. The
verifier per `01-foundations.md` §8 confirms each scheduler's capability annotations match the
concepts the pipeline relies on: `compute` models `BulkScheduler` (the `schedule_bulk` call is
well-formed), `platform` models `AffineScheduler` (the `PresentStagingBuffer` call sits inside a
`then` that the type system guarantees runs on the platform thread), `io` models `IoScheduler`
(the `async_write` adaptor is exposed). A misuse — e.g. `schedule_bulk(platform, ...)` — fails to
compile per `Platform`'s lack of `OffersBulk` annotation and §6.5's static assertion.

The second paragraph is **what the runtime delivers**. The pipeline allocates zero heap memory
post-construction (per §2.4 across all three backends); the only allocations are TBB-internal
task blocks (`Allocates{External}` on Tbb) and the Io context's kernel-side ring or OVERLAPPED
freelist (`Allocates{External}` and `Allocates{OpState}` on Io), both amortised across the
process lifetime. The three backends communicate through the cheapest available transport:
Tbb→Platform is one `PostThreadMessage` / `dispatch_async` / X11-message / `eventfd` write (no
queue hop on the framework side); Platform→Io is one Io-context-internal queue push; Io→userland
is a kernel completion event. No virtual call appears at any transition. The stop-token threading
is uniform: one `inplace_stop_token` covers all three backends' callbacks, no `bridge_stop_token`
is needed, and cancellation latency is bounded by max(TBB cooperative-check interval, MPSC drain
latency, kernel cancel-completion latency). This is the framework's "one vocabulary, five
backends, zero technical debt" promise made concrete.

---

## 9. Status

- **v0.1** — drafted 2026-06-15. Five backends specified (`Inline`, `StaticPool`, `Tbb`,
  `Platform`, `Io`); op-state shapes, capability annotations, domain rewrites, allocation
  policy, cancellation wiring laid out. Cross-backend pipeline example deferred to v0.2.
- **v0.2 (this document)** — incorporates synthesis-pass adjudications (`09-synthesis.md` §2.6,
  §2.7, §2.23). §5.8 publishes `Tbb::pipeline_rewrite_threshold_v` as a public
  `inline constexpr std::size_t` with the equivalence proof for the
  `pipeline → tbb::flow::graph` lowering (R3). §6.5 publishes the "Platform is not a
  BulkScheduler" note with the L3 static-assert citation. §7.2 publishes
  `timer_op_state_size_v`, `read_op_state_size_v`, `write_op_state_size_v` as public size
  constants for L3 inline-storage planning, and adds the `tag_invoke(get_io_context_t, scheduler)`
  body completing the forward declaration in `01-foundations.md` §4.5. §8 adds the worked
  cross-backend pipeline example (Tbb → Platform → Io) — the umbrella's "two worked composition
  examples" deliverable, of which this is the cross-backend one (the per-backend examples are
  embedded in §§3–7). §9 status section added.
- v1.0: post-implementation revision after the five backend translation units land under
  `Mashiro/src/Async/Backend/` and the cross-backend pipeline example compiles end-to-end as a
  demo in `Mashiro/demos/Async/`.

*End of L2 backends spec.*
