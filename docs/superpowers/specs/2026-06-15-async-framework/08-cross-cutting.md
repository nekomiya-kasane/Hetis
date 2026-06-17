# Mashiro Async Framework — Cross-Cutting Concerns

**Status:** Draft v0.2 (subagent E deliverable; spawned by `00-overview.md` §8.5 v0.2)
**Date:** 2026-06-15 (v0.1) · 2026-06-16 (v0.2 synthesis pass)
**Author:** Mashiro Engine team — Subagent E
**Scope:** Concerns that touch every layer (L0–L7): cancellation, allocation, errors, time, diagnostics, and the migration plan that turns the existing engine codebase into a framework consumer.

### Revision history

- **v0.1** — initial draft. Locks the cross-cutting contracts (cancellation, allocation, errors, time, diagnostics) and the migration plan with concrete code-path touchpoints.
- **v0.2** — synthesis pass (see `09-synthesis.md` §2.22, §2.9, §7). §6.1 adds the diagnostics
  registration ordering rule: diagnostics backends must be registered before any `Async::*`
  scheduler is constructed, with a debug-mode assertion gated on `MASHIRO_DIAGNOSTICS_REQUIRED`.
  §7.5 cross-references L4 v0.2 §7.3's `[[deprecated]]` `from_future` migration boundary so
  the migration plan and the deprecation warning point at each other. No structural changes to
  the layer-by-layer breakdowns of cancellation / allocation / errors / time, the migration
  phasing, or the touchpoint table.

---

## 1. Purpose

Each layer spec (`01-foundations.md` through `07-extension.md`) owns a vertical slice of the framework. The five cross-cutting concerns below cut horizontally — they appear in every layer, and the contract has to be consistent across all of them, otherwise pipelines composed across layers behave inconsistently.

Each concern is documented here as:

1. The **rule**, in one sentence — the invariant downstream code can rely on.
2. The **layer-by-layer breakdown** — what each layer does to honour the rule.
3. The **anti-patterns** — what *looks* compliant but isn't.
4. The **enforcement mechanism** — concept-check, consteval audit, debug assertion, or runtime diagnostic.

The concerns are independent. A change to the cancellation contract does not affect the allocation contract; a change to time semantics does not affect error semantics. They are listed in the order they bind layer specs (cancellation first because it is the most pervasive, time last because it is the most localised).

---

## 2. Cancellation

**Rule.** A correctly-built pipeline cancels structurally with no client code: `RequestStop()` at the top propagates through every receiver's environment, every cancellable op-state observes its stop-callback, every sender completes with `set_stopped`, and every coroutine unwinds — all without a per-component `closed_` flag, sentinel value, or polled `IsCancelled()` query.

The framework uses **exactly one** stop-token type internally: `stdexec::inplace_stop_token`, exported as `Mashiro::Async::stop_token` (overview section 5.2). `std::stop_token` is supported only at boundaries (e.g., `jthread` interop) via `bridge_stop_token(std::stop_token)`.

### 2.1 How the stop-token flows

The token flows through **the receiver's environment**. Every sender / awaiter / scheduler that needs to observe cancellation queries it via `stdexec::get_stop_token(stdexec::get_env(rcvr))`. There is no global cancellation flag; the token is per-pipeline, per-receiver, propagated by composition.

```
              stop_source (root)            // user-owned, lifetime bounded by Scope or top-level sync_wait
                 |
                 v get_token()
              stop_token                    // copyable, pointer-sized
                 |
                 v threaded through env
              receiver.env.stop_token       // every layer's receiver carries it
                 |
                 v queried by sender on start()
              op_state.callback             // inplace_stop_callback, op-state-scoped
                 |
                 v fires on stop request
              op_state.cancel()             // sender-specific cancel; completes with set_stopped
```

### 2.2 Per-layer behaviour

| Layer | Cancellation behaviour |
|---|---|
| **L0 — Vocabulary** | Re-exports `inplace_stop_source` / `inplace_stop_token` / `inplace_stop_callback` under `Mashiro::Async::*`. Defines `bridge_stop_token(std::stop_token)` for `jthread` interop. |
| **L1 — Annotations** | `Cancellable` annotation tags a sender/scheduler that completes promptly on stop. `Traits::IsCancellable_v<S>` is a consteval query reading the annotation. Adaptors that *require* cancellation (`timeout`, `race`) static_assert that their argument is `Cancellable`. |
| **L2 — Backends** | Every backend's schedule sender registers an `inplace_stop_callback` against the receiver's stop-token in `start()`. `Inline` short-circuits cancellation by checking `stop_requested()` synchronously before completing. `StaticPool` and `Tbb` use the callback to remove the work item from their queue (or mark it cancel-pending). `Platform` uses the callback to remove the coroutine handle from the MPSC mailbox. `Io` uses the callback to issue `IORING_OP_ASYNC_CANCEL` (Linux) or `CancelIoEx` (Windows). |
| **L3 — Adaptors** | Every adaptor that owns external state propagates the stop-token to its inner senders by *not interfering* — the wrapped receiver's environment forwards the token unchanged. Adaptors that introduce stop semantics (`timeout`, `race`) install their own stop-callback and forward through composition. |
| **L4 — Coroutine tasks** | `Task<T>::promise_type` queries the awaiting receiver's stop-token through its environment and exposes it via `get_stop_token` so awaitables inside the coroutine body see the token. `Stream<T>` fires `set_stopped` on the next `next()` after stop. `Job` follows the same rule. |
| **L5 — Structured concurrency** | `Scope::spawn(token, sender)` adopts the parent's stop-token. `Nursery` derives a child stop-source from the parent's token; on parent stop, the nursery's source is requested, propagating to every child. On child error, the nursery requests its own stop, cancelling siblings. `LinkedScope` carries a separate token whose request is forwarded from the parent. |
| **L6 — Patterns** | Every pattern accepts a stop-token through its receiver's environment. `parallel_for` cancels mid-iteration: in-flight chunks observe the token at their next chunk boundary. `pipeline` and `actor` cancel by closing their internal queues — every stage's `next()` completes with `set_stopped`. |
| **L7 — Extension** | User-defined schedulers must implement the cancellation callback. The `Cancellable` annotation declares the contract; the consteval verifier rejects schedulers that declare it without registering a callback. |

### 2.3 How user code installs callbacks correctly

For framework consumers, the cancellation rule is **install nothing**. The framework's senders, adaptors, and patterns observe the stop-token automatically; the user only writes:

```cpp
Mashiro::Async::stop_source stop;
auto pipe = source() | adaptor1 | adaptor2 | sink();
stdexec::sync_wait(stdexec::on(scheduler, pipe));     // blocks until stop or completion

// Elsewhere:
stop.request_stop();    // every op-state in the pipeline observes this and completes with set_stopped
```

For *adaptor* authors (Section 2 of `07-extension.md`), the rule is the cancellation checklist: register a callback in `start()`, drop it before completing, declare `Cancellable` in the annotation set.

For *coroutine* code that explicitly wants to react to cancellation:

```cpp
Task<void> work() {
    auto stop = co_await stdexec::get_stop_token();
    while (!stop.stop_requested()) {
        co_await one_step();
    }
}
```

The `co_await get_stop_token()` is the canonical query. There is no `Mashiro::Async::IsCancelled()` free function; the answer always comes through the token.

### 2.4 Anti-patterns and how the framework prevents them

| Anti-pattern | Why it is wrong | How it is prevented |
|---|---|---|
| Per-channel `closed_` flag | Duplicates the stop-token; ordering against `waiter_` is hand-rolled per channel | `EventChannel` (platform-thread spec section 6.3) uses the stop-token directly; v1.4's `Close()` is removed |
| Sentinel "stopped" event values (`std::nullopt`, `EventKind::Stopped`) | Pollutes the value channel with a control signal | `set_stopped` is a separate completion channel; `let_stopped` and `stopped_as_optional` are the conversion adaptors |
| Polling `IsCancelled()` in a loop | Active polling instead of cooperative suspension | The framework exposes `co_await get_stop_token()` and the token's `stop_requested()` query; loops `while (!token.stop_requested())` are correct because the suspension point is `co_await one_step()`, not the check |
| Throwing an exception to signal cancellation | Mixes cancellation with errors | `set_stopped` is the exclusive cancellation channel; exceptions become `set_error(exception_ptr)`; the framework's `Result<T>` does not represent cancellation |
| Holding `inplace_stop_callback` past completion | Callback may fire on moved-from receiver | `cancel.reset()` *before* `set_*` is the rule; the consteval verifier walks the op-state and complains if the order is wrong |
| Storing `stop_token` in a long-lived data structure | Token outlives its source | `inplace_stop_token` is op-state-scoped; the framework rejects attempts to copy it into a non-op-state member; if a long-lived token is needed, use `std::stop_token` at the boundary and bridge in |

### 2.5 The `Cancellable` annotation semantics

`Cancellable` is an L1 capability annotation (overview section 5.6). When present:

- The sender / scheduler **promptly** completes with `set_stopped` on stop request. "Promptly" means "within the time a syscall takes to return, plus the time the next suspension point of the calling coroutine takes to run". This excludes synchronous CPU work that ignores the token.
- Adaptors that compose with `Cancellable` senders inherit the annotation through reflection; the framework's default lowering propagates it.
- Adaptors that do **not** inherit it (because they interpose blocking work, e.g., a hypothetical `synchronous_compute` adaptor) drop the annotation in their declared output. The user sees the lost capability through `Traits::IsCancellable_v` and can decide whether to insert a `forced_stop` checkpoint.

When **absent**:

- The sender / scheduler may take an unbounded time to complete after a stop request. This is documented at the type, not the call site.
- Adaptors that *require* `Cancellable` (`timeout`, `race`, the cancellation-on-shutdown discipline of `Scope::on_empty()`) static_assert and reject composition at compile time.

The consteval verifier (Subagent A's `Foundations.h`) cross-checks that a scheduler declaring `Cancellable` actually registers a stop-callback in its op-state. The check is reflection-based: the op-state's member set is walked for an `inplace_stop_callback<...>` member; absence with the annotation is a compile error.

### 2.6 Cancellation and shutdown

The platform-thread spec (section 8.4) treats shutdown as a structured cancellation: `RequestStop()` triggers `stop_.request_stop()`, every in-flight `Task<T>` and channel sender observes the token, every spawned sender owned by `scope` completes with `set_stopped`, and `scope.on_empty()` settles. The framework follows the same discipline at every layer — there is no separate "shutdown" mechanism.

### 2.7 Cancellation latency

Cancellation is **prompt** for `Cancellable`-annotated senders, but "prompt" has a useful definition. The latency budget for a cancel-to-completion round-trip is:

| Sender shape | Latency budget | Why |
|---|---|---|
| `Inline::schedule()` | 0 (synchronous check before completing) | Same-thread, immediate |
| `StaticPool::schedule()` queued but not running | 1 work-stealing victim check (microseconds) | Worker observes the cancel flag on next dequeue |
| `StaticPool::schedule()` running user code | Until next suspension point | The framework cannot interrupt running CPU code; the user code observes the token at its own granularity |
| `Tbb::schedule_bulk` | One TBB task boundary (microseconds) | TBB's cancellation is per-arena; the framework's bulk lowering propagates |
| `Platform::schedule()` queued | One pump iteration (sub-millisecond on modern hardware) | The MPSC handle queue is drained between iterations |
| `Io::async_read` etc. | One syscall round-trip (kernel-dependent) | `IORING_OP_ASYNC_CANCEL` / `CancelIoEx` |
| User-defined Vulkan scheduler | Until `vkQueueWaitIdle` returns (milliseconds) | Vulkan does not retract individual submissions; cancel waits for the queue to drain |

The framework documents this latency budget per backend; user code that needs tighter cancel-to-completion should not compose with backends that exceed its budget. The annotation set does not currently encode latency — that is a deliberate omission to avoid policy-by-annotation creep.

### 2.8 Cancellation interop with `std::stop_token`

Code that bridges to legacy APIs (a `jthread`-based subsystem, a third-party library that takes `std::stop_token`) uses `bridge_stop_token(std::stop_token)`:

```cpp
std::jthread legacy{[&](std::stop_token st) {
    auto bridged = Mashiro::Async::bridge_stop_token(st);
    // 'bridged' is an inplace_stop_token whose source's lifetime is bounded
    // by 'bridged' (RAII). When 'st' fires, the bridged source is requested.
    sync_wait(work_pipeline() | with_stop_source(bridged));
}};
```

The bridge introduces one allocation (the bridge's owned `inplace_stop_source`) and one stop-callback registration on the input `std::stop_token`. The cost is paid once at the boundary; inside the framework, everything stays on `inplace_stop_token`.

---

## 3. Allocation

**Rule.** Hot paths allocate zero heap memory. Where allocation is unavoidable, the allocator is queried from the receiver's environment (`get_allocator(env)`), not pulled from a global. Coroutine frame allocation is documented at the coroutine type, never silent at the call site.

### 3.1 Per-layer allocation policy

| Layer | Allocation behaviour | Notes |
|---|---|---|
| **L0 — Vocabulary** | Zero | Re-exports are aliases; aliases do not allocate |
| **L1 — Annotations & Traits** | Zero | Annotations are inert at runtime; `consteval` queries are compile-time |
| **L2 — Backends — Inline** | Zero | Op-state is the receiver's stack frame |
| **L2 — Backends — StaticPool** | One allocation per `start_detached`-rooted submission | Pool has a per-thread free-list; submissions reach a worker through bounded MPSC |
| **L2 — Backends — Tbb** | Per `tbb::task` allocation by TBB | Outside our control; TBB's per-arena allocator is its own concern |
| **L2 — Backends — Platform** | Zero on the schedule fast path | The MPSC handle queue is bounded (256 slots, see platform-thread spec section 6.5); overflow `terminate`s rather than falling back to heap |
| **L2 — Backends — Io** | Zero on the submission fast path | io_uring SQE / IOCP completion entries are pre-allocated rings; the op-state lives in the awaiter's storage |
| **L3 — Adaptors** | Zero on the hot path | `bulk`, `batch`, `debounce`, `retry`, `timeout`, `race`: op-state is composed from inner op-states; no heap |
| **L4 — Coroutine tasks** | One allocation per `Task<T>` invocation when HALO does not apply | Documented at the type; `Stream<T>` allocates the iterator state in a similar way |
| **L5 — Structured concurrency** | One allocation per `Scope` for its op-state ring buffer; `spawn()` is heap-free if the spawned op-state fits the inline budget | `counting_scope` (P3149) provides the inline budget |
| **L6 — Patterns** | Follows the underlying scheduler | `parallel_for(Tbb, ...)` allocates per `tbb::task`; `parallel_for(StaticPool, ...)` allocates one per submission |
| **L7 — Extension** | User-declared via `Allocates::Where{...}` | The annotation tags where the scheduler/adaptor allocates; `Diagnostics::AllocCheck` flags unannotated allocations |

### 3.2 The `get_allocator(env)` query

`stdexec::get_allocator(env)` is the queryable on the receiver's environment that returns the allocator the sender should use for any non-pool storage. The default is `std::allocator<std::byte>`; the framework's hot paths use a PMR-based arena allocator slotted in by the scheduler that owns the work.

```cpp
template<class Rcvr>
void op_state<Rcvr>::start() noexcept {
    auto env  = stdexec::get_env(rcvr);
    auto alloc = stdexec::get_allocator(env);   // queryable
    // Allocate any per-submission storage from `alloc`. If `alloc` is the
    // default std::allocator, we are on a slow path — log and proceed.
    storage_ = alloc.allocate(N);
    ...
}
```

Senders that require a specific allocator (e.g., a Vulkan compute scheduler that needs device-coherent memory) declare their requirement in their environment query, not by hardcoding a `new`. The user composing the pipeline supplies the allocator via `with_allocator(my_arena)` adaptor (an L3 adaptor).

### 3.3 PMR-based arena allocator integration

The framework's default allocator implementation is a PMR arena per scheduler. The arena is created at scheduler construction, sized for steady-state work (heuristic: 4 MiB per worker thread), and reset between major phases (frames, requests, transactions). Resetting is cheap (one pointer reset, no destructor calls because the arena's contents are POD or destructor-empty), and per-allocation cost is one bump.

The arena's lifecycle is the user's responsibility — the framework does not call `reset()` automatically. The user calls `arena.reset()` between frames; the framework's `Diagnostics::AllocCheck` warns if an arena has not been reset between two `RequestStop()` cycles (a likely sign of mis-scoped lifetime).

### 3.4 Coroutine frame allocation

`Task<T>` and `Stream<T>` carry a coroutine frame; the compiler can elide the allocation only when HALO proves the frame does not escape. **Cross-thread transfer always escapes**, so each cross-thread Manager call allocates one frame on the heap (platform-thread spec section 6.6 has the same rule).

The framework's strategy:

1. **Document at the type, not the call site.** `Task<T>`'s class comment lists "one heap allocation per call when HALO does not apply".
2. **Slot a custom allocator into `task<T>`'s `default_task_context`.** P3552 exposes the allocator hook through the task's environment; the framework binds the per-scheduler arena to the task's allocator slot.
3. **Profile, do not guess.** If profiling shows frame allocation is hot, the user reaches for the arena allocator; the framework does not switch to a different coroutine type silently.

### 3.5 Allocation accounting via `Diagnostics::AllocCheck`

`AllocCheck` is a debug-mode RAII guard that hooks the active allocator and records every allocation that happens inside its scope. Use:

```cpp
{
    Mashiro::Async::Diagnostics::AllocCheck guard{"frame-pipeline"};
    auto pipe = source() | adapt1 | adapt2 | sink();
    stdexec::sync_wait(pipe);
    // guard's destructor logs the allocation count + size; configurable
    // threshold per scope, with structured-log output naming the call site.
}
```

`AllocCheck` is zero-cost when off (compiles to nothing in release builds with `MASHIRO_ASYNC_DIAG=OFF`). When on, the per-allocation overhead is one atomic increment plus one tagged log entry. The expected workflow is to wrap a hot-path test in `AllocCheck` with a threshold of zero; any allocation breaks the test.

### 3.6 Anti-patterns

| Anti-pattern | Why it is wrong | Prevention |
|---|---|---|
| `new` / `delete` inside a sender's `start` | Bypasses the environment allocator | Code review + `AllocCheck` in tests |
| `std::function` in op-state | Allocates per construction | The framework's adaptors use `InlineFunction<Sig, Cap>` (Mashiro core); ban `std::function` in op-state member declarations |
| Per-receiver `std::vector` resized at runtime | Linear-cost reallocation on a hot path | Use `boost::small_vector`-equivalent or pre-size at op-state construction |
| Allocating once per `set_value` call | Quadratic cost for a chain of N adaptors | Op-states are composed at construction; per-completion allocations are forbidden |
| Holding a `shared_ptr` to share state across senders in a pipeline | Reference counts on every channel | Op-states have a tree shape; share state via parent op-state or via a non-owning pointer |

---

## 4. Errors

**Rule.** The default error completion is `set_error_t(std::exception_ptr)`. Users opt into typed errors via `with_error<MyError>` — but `std::expected` (or `Result<T>`) is **not** the framework's primary error type, because composition of expected-shaped values explodes completion-signature unions.

### 4.1 Why exception_ptr by default

P2300 mandates that completion signatures union across composed senders. A sender chain `a | then(f) | let_value(g)` has a completion signature that is the union of `a`'s, `f`'s, and `g`'s completion signatures. If every adaptor declared its own typed error, the resulting union would grow combinatorially:

```
a: set_value(int), set_error(IoError)
f: set_value(string), set_error(ParseError)
g: set_value(Document), set_error(ValidationError)

union: set_value(Document), set_error(IoError),
                            set_error(ParseError),
                            set_error(ValidationError)
```

Every consumer would have to handle every error type. In practice users want **one** error path that reports "something went wrong upstream"; `exception_ptr` provides exactly that:

```
a: set_value(int), set_error(exception_ptr)
f: set_value(string), set_error(exception_ptr)
g: set_value(Document), set_error(exception_ptr)

union: set_value(Document), set_error(exception_ptr)
```

The composition is uniform, the consumer handles one type, and the actual error is recovered by `try { std::rethrow_exception(p); } catch (const IoError& e) { ... } catch (const ParseError& e) { ... }` at the boundary that needs it.

### 4.2 Typed errors via `with_error<E>`

When a typed error genuinely matters (because the consumer routes on the type, not just on its presence), the user opts in:

```cpp
auto chain = read_file(path)
    | with_error<IoError>{}              // declares set_error_t(IoError) explicitly
    | parse_json()                       // its set_error stays exception_ptr
    | with_error<ParseError>{};          // hoists ParseError separately

// Consumer sees: set_error(IoError), set_error(ParseError), set_error(exception_ptr)
let_error(chain, [](auto&& e) { ... })   // handles all three
```

`with_error<E>` is an L3 adaptor that catches `exception_ptr` from its upstream and, if the inner exception is of type `E`, completes with `set_error(E)` instead. Senders downstream of `with_error<E>` see the typed channel as a separate completion signature.

The user pays the cost of typed errors deliberately, in the small region where they help. The default keeps the union small.

### 4.3 Conversion at boundaries

Three boundaries warrant explicit conversion:

- **`std::future` interop.** A `std::future<T>` carries a `std::exception_ptr` natively; the framework's `as_future_sender(future)` adaptor wraps it. The error channel is `exception_ptr` by construction.
- **`std::expected<T, E>` interop.** A function returning `Result<T> = std::expected<T, ErrorCode>` is wrapped by `from_result(fn)`; the resulting sender's completion signature is `set_value(T), set_error(ErrorCode)`. Inside a pipeline, `with_error<ErrorCode>` keeps the typed channel; outside, the user can `.value_or_throw()` to surface as `exception_ptr`.
- **C-API errno / GetLastError.** The framework's wrappers (`async_read`, `async_write`) translate OS error codes to a typed `IoError` enum and complete with `set_error(IoError)`. Adapters higher in the stack convert to `exception_ptr` if they need to compose with non-Io senders.

### 4.4 Why not `std::expected` as the primary error type

`std::expected<T, E>` looks attractive because it is value-semantic and avoids `try/catch` at the consumer. The composition complexity is the disqualifier:

- Every `then(f)` where `f: T -> U` becomes `then(f)` where `f: T -> expected<U, E>`. The user must thread `E` through every transformation.
- Every `let_value(g)` where `g: T -> sender<U>` becomes `g: T -> sender<expected<U, E>>`. The signature gets nested.
- `when_all(s1, s2)` where each is `sender<expected<T, E1>>` and `sender<expected<U, E2>>` produces a `sender<tuple<expected<T, E1>, expected<U, E2>>>`. The consumer flattens by hand.
- Cancellation (`set_stopped`) is orthogonal to `expected` — the consumer still has to handle the `set_stopped` channel separately.

The framework's choice — `set_value(T)` and `set_error(exception_ptr)` plus `set_stopped()` — keeps the three orthogonal concerns on three orthogonal channels. `Result<T>` (= `std::expected<T, ErrorCode>`) lives at API surfaces (Manager methods), where the typed error is part of the contract; inside pipelines, errors flow through `set_error`.

### 4.5 Error-handling adaptors

| Adaptor | Effect |
|---|---|
| `let_error(s, f)` | If `s` completes with `set_error(e)`, run `f(e)` which itself returns a sender; otherwise pass `set_value` through |
| `let_stopped(s, f)` | Same shape, but for `set_stopped` |
| `upon_error(s, f)` | `f(e)` is a synchronous transformation, not a sender |
| `upon_stopped(s, f)` | Same shape, synchronous |
| `with_error<E>` | Hoist a typed error out of `exception_ptr` |
| `stopped_as_optional(s)` | Convert `set_stopped` to `set_value(nullopt)`; useful when cancellation is "no result" rather than "abort" |
| `stopped_as_error(s, f)` | Convert `set_stopped` to `set_error(f())`; rarely correct, but available for boundaries |

The preferred composition is `let_error` plus `let_stopped` — explicit handlers, no exception-throw at the consumer. `try/catch` on the rethrown `exception_ptr` is the boundary form.

### 4.6 Coroutine error semantics

`Task<T>::promise_type::unhandled_exception()` captures the active exception and routes it to the awaiting receiver's `set_error(exception_ptr)`. Inside the coroutine body, `co_await sender` rethrows the inner sender's `set_error(exception_ptr)` as the original exception (via `std::rethrow_exception`), so `try/catch` inside a coroutine works as expected. `set_stopped` from an inner sender unwinds the coroutine via a dedicated `Mashiro::Async::Coro::stopped_signal` exception that the promise's `unhandled_exception` recognises and turns into `set_stopped` on the outer receiver.

The user does **not** see `stopped_signal` unless they explicitly catch it. The pattern `try { co_await sender; } catch (...) { ... }` catches errors but does not catch cancellation; cancellation surfaces as the coroutine's `final_suspend` propagating `set_stopped` upward.

---

## 5. Time

**Rule.** Time is a sender source (`async_timer`), not a primitive. Monotonic clocks drive cancellation deadlines and back-pressure; wall-clock is exposed only for stamping events that need calendar semantics.

### 5.1 The `async_timer` source

`async_timer(duration)` is a sender provided by the `Io` backend (overview section 7). It completes with `set_value()` after `duration` of monotonic time has elapsed, or with `set_stopped()` if the receiver's stop-token is requested before then.

```cpp
auto timeout_pipe = work() | timeout(async_timer(50ms));   // L3 adaptor uses the timer
co_await async_timer(100ms);                               // bare wait
auto next_tick = async_timer(16ms) | repeat();             // periodic ticking
```

`async_timer` is implemented per-backend:

- **Linux (Io):** `IORING_OP_TIMEOUT` against `CLOCK_MONOTONIC`.
- **Windows (Io):** `CreateWaitableTimerEx` + `IOCP` association with monotonic timer (`QueryPerformanceCounter`-derived).
- **Inline / StaticPool / Tbb:** A shared timer wheel maintained by the framework's `TimerService` (one per process), driven by the `Io` backend if present and falling back to `std::condition_variable::wait_for` otherwise.

The user does not name the implementation — `async_timer` is dispatched on the receiver's scheduler's environment, with the `Io` backend taking precedence when available.

### 5.2 Monotonic vs wall-clock

| Use case | Clock | Source |
|---|---|---|
| Timeout / deadline / interval | Monotonic | `async_timer(duration)` (relative) |
| Schedule at a specific monotonic instant | Monotonic | `async_at(steady_clock::time_point)` |
| Stamp a `TimestampedEvent` for input latency | Monotonic | `Mashiro::Platform::Time::Now()` (QueryPerformanceCounter / `clock_gettime(CLOCK_MONOTONIC)`) |
| Calendar timestamp for log / persistence | Wall-clock | `std::chrono::system_clock::now()` outside the framework's surface |

The framework does **not** expose wall-clock as a sender source. Wall-clock is non-monotonic (NTP corrections, DST), and its use in cancellation logic is a bug; users that need calendar timestamps for log lines call `std::chrono::system_clock::now()` directly.

### 5.3 Suspend / resume behaviour

Modern operating systems suspend monotonic clocks during system sleep (S3, S4, hibernate). After resume, monotonic time appears to have not advanced; a 100 ms timer started before sleep completes 100 ms after resume, not 100 ms after submission.

This is the **right** behaviour for most async work: a timeout for a network request should not fire while the laptop is suspended. The framework documents this property at `async_timer`'s declaration. For workloads that need wall-clock-accurate deadlines (a calendar reminder, a session expiry), the user composes `async_timer` with a wall-clock check at the boundary.

### 5.4 Integration with platform sleep mechanisms

The platform-thread spec (section 5.3) ends each loop iteration in `MsgWaitForMultipleObjectsEx(QS_ALLINPUT | timeout)`. The framework's `TimerService` registers its earliest deadline with the platform's wait so the platform thread wakes precisely when the next timer fires, not on a polling tick. The integration is one-way: the timer service informs the platform of the next deadline; the platform does not own timer state.

On Linux, `epoll_wait`'s timeout argument plays the same role; on macOS, `dispatch_after` integrates natively with the main run-loop. The user does not see any of this — `async_timer(d)` is the surface.

### 5.5 Timer wheel design

The framework's `TimerService` uses a hierarchical timer wheel for O(1) insert / cancel / fire. The wheel is a single shared resource (one per process), accessed by every backend that needs a timer. Cancellation is structural: when a timer's receiver requests stop, the timer's op-state's stop-callback removes the entry from the wheel and completes with `set_stopped`. The wheel's per-bucket lock is held only during insertion and firing; the steady-state cost is one `compare_exchange` per timer on cancellation.

---

## 6. Diagnostics

**Rule.** Trace spans, op-state introspection, starvation detection, and deadlock heuristics are zero-cost when off. Tracy / Perfetto integration is a domain rewrite, not a framework dependency.

### 6.1 The `Diagnostics::Trace` API

```cpp
namespace Mashiro::Async::Diagnostics {

    struct Span {
        static Span Begin(std::string_view name) noexcept;
        void End() noexcept;
        void EndCancelled() noexcept;
        void EndError() noexcept;
        // Nested spans, attributes, events — same shape as OTel
        Span Nested(std::string_view name) noexcept;
        void AddAttribute(std::string_view key, std::string_view value) noexcept;
    };

    // Compile-time guard. When off, every Span method compiles to a no-op.
    inline constexpr bool kEnabled = MASHIRO_ASYNC_DIAG_ENABLED;
}
```

When `MASHIRO_ASYNC_DIAG_ENABLED` is `false`, every `Diagnostics::*` call site compiles to nothing. The compiler eliminates the call, the argument evaluation, and any code that constructs the call's arguments — verified by ODR-test and assembly inspection. This is the same design discipline the framework applies to `assert`-style checks.

When on, spans are routed to a registered backend through a single function-pointer indirection (set once at process init). The framework ships with two backends: a structured-log backend (writes to `Mashiro::Core::StructuredLogger`) and a Tracy backend (when `thirdparty/tracy/` is linked). Perfetto is supported through the same backend interface; users register their own backend by setting the function pointer.

**Ordering rule (v0.2).** *Diagnostics backends must be registered before any `Async::*` scheduler is constructed.* The recommended call site is the first statement of `main()`, before `PlatformThread::Run()`. A debug-mode assertion fires if a scheduler is constructed before any diagnostics backend has registered, when `MASHIRO_DIAGNOSTICS_REQUIRED` is defined:

```cpp
int main(int argc, char** argv) {
    // First: register diagnostics backends. Domains constructed later (Tracy,
    // Perfetto, custom) will compose into pipelines from the moment they are
    // built, including the platform scheduler's own pipeline.
    Mashiro::Async::Diagnostics::RegisterStructuredLogBackend();
#if MASHIRO_HAS_TRACY
    Mashiro::Async::Diagnostics::RegisterTracyBackend();
#endif

    // Only now construct schedulers / scopes.
    Mashiro::Platform::PlatformThread::Run(/*...*/);
}
```

The rationale: Tracy / Perfetto domain rewrites (§6.5) compose into sender pipelines at *construction* time, not at execution. A scheduler constructed before the diagnostics backend registers therefore builds its internal pipelines without the diagnostics domain in their environments — the result is silent under-tracing, not a crash, and the gap is invisible without the assertion. The assertion is gated on `MASHIRO_DIAGNOSTICS_REQUIRED` (a separate macro from `MASHIRO_ASYNC_DIAG_ENABLED`) so projects that deliberately defer diagnostics registration (a profile-on-demand toggle, an admin-triggered enable) can suppress the assertion at compile time without losing the underlying diagnostics machinery.

The synthesis pass (`09-synthesis.md` §2.22) recorded this rule after Subagent E flagged the construction-order gap. The platform thread's own scheduler construction (`Mashiro/src/Platform/PlatformThread.cpp`) is expected to be the *first* scheduler, so the assertion practically reduces to "register diagnostics before `PlatformThread::Run()` returns" — but the framework spells out the broader rule because user-defined schedulers may be constructed earlier in subsystem init.

### 6.2 Op-state introspection

```cpp
template<class OpState>
auto Diagnostics::DumpOpState(const OpState& op) -> std::string;
```

`DumpOpState` walks the op-state's reflected member set and produces a structured dump: type names, current state of nested op-states (via reflection), held stop-callbacks, allocated buffers. The cost is one reflection-driven traversal per call — not free, but acceptable on a debug-mode breakpoint.

The introspection respects privacy: members marked with the `[[=Diagnostics::Opaque]]` annotation are dumped as `<opaque>` only. The framework uses this for receiver state that contains `std::function`-like type-erased payloads (rare; `any_sender_of`'s erased state).

### 6.3 Starvation detection

`Diagnostics::DetectStarvation()` is a debug-mode probe that walks every active scheduler's queue and reports the longest in-flight op-state age. When an op-state has been queued for more than a configurable threshold (default 5 seconds), the probe logs a structured warning naming the scheduler, the op-state's reflected type, and its declared annotations.

The probe runs on a background thread when enabled (in tests only). Production builds do not run it; the cost is one wake-up per second plus one traversal per scheduler.

### 6.4 Deadlock heuristics

A deadlock in the framework is rare because cancellation is structural and shutdown is bounded. But user code can create deadlocks: two `Task<T>`s mutually awaiting each other, a pipeline whose feedback loop forms a cycle, a scope waiting on a sender that depends on the same scope's `on_empty()`.

`Diagnostics::DetectDeadlock()` walks the receiver-environment graph (each receiver knows its sender's predecessor through reflection) and checks for cycles. Cycle detection is O(V+E) per traversal; the heuristic runs on a debug-mode timer (every 30 seconds default). On detection, it logs a structured event listing the cycle's vertices and the receivers' types.

The heuristic is opt-in. It is not run in release; deadlock-prone code is expected to fail tests, and tests opt in via `Diagnostics::EnableDeadlockDetection()` at fixture setup.

### 6.5 Tracy / Perfetto as domain rewrites

Tracy and Perfetto integrate via a **domain rewrite**, not a framework dependency. The user composes their pipeline with `Mashiro::Async::Diagnostics::TracyDomain{}` (or `PerfettoDomain{}`) in the receiver's environment; the domain rewrites every sender expression to wrap its op-state's `start` / `set_value` / `set_error` / `set_stopped` calls with Tracy / Perfetto begin / end markers.

The framework itself does not link against Tracy or Perfetto; the user's build system links them when the corresponding domain is used. Subsystems that do not enable the domain pay no cost.

### 6.6 Allocation accounting

Already covered in Section 3.5. `Diagnostics::AllocCheck` is one of the diagnostics surfaces; it shares the on/off discipline.

### 6.7 Zero-cost when off

Every `Diagnostics::*` call site compiles through a `if constexpr (kEnabled)` guard. When off, the call expression is `(void)0;`. The compiler eliminates the call frame, the arguments, and any side-effect-free code that constructed the arguments. The framework tests this by compiling with `-Os` and asserting that disassembly of a `Span::Begin("x"); ...; span.End();` block contains no instructions referencing the diagnostics module.

When on, the per-call cost is one function-pointer dispatch + one structured-log entry. For Tracy / Perfetto domain rewrites, the cost is one Tracy/Perfetto API call per begin/end. Costs are measured in benchmarks under `Mashiro/tests/Async/Diagnostics/`.

---

## 7. Migration plan

**Rule.** Migration is layered: deliver L0–L1 first, then L2, then L3+L4, then L5+L6, then L7. Existing code that already conforms to a layer's contract migrates by inclusion (the type was already a sender / scheduler / coroutine task in spirit); existing code that does not conform migrates per the playbook in `07-extension.md` section 9.

### 7.1 Order of layer delivery

| Phase | Layer set | Duration target | Gate |
|---|---|---|---|
| 1 | L0 (vocabulary), L1 (annotations + traits) | 2 weeks | All `Concepts::*` evaluate; `Traits::*` reflection-driven and tested |
| 2 | L2 backends — `Inline`, `StaticPool`, `Platform` (alias of existing) | 3 weeks | Each backend has its own translation unit + Catch2 tests; CMake target per backend |
| 3 | L2 backends — `Tbb`, `Io` | 4 weeks | `Tbb` uses `thirdparty/tbb`; `Io` Linux + Windows |
| 4 | L3 adaptors + L4 coroutine tasks | 4 weeks | Adaptors composed against L2 backends; `Task<T>` / `Stream<T>` work end-to-end |
| 5 | L5 structured concurrency + L6 patterns | 3 weeks | `Nursery`, `Scope`, `parallel_for`, `pipeline`, `actor` integrated; cancellation flow tested |
| 6 | L7 extension surface | 2 weeks | `VkComputeScheduler` example lands; plugin descriptor schema published |
| 7 | Cross-cutting hardening | 2 weeks | `Diagnostics::AllocCheck` zero-allocation tests on every hot path; deadlock heuristics in CI |

The schedule is illustrative; the real gating is each phase's spec-acceptance criteria from overview section 9.

### 7.2 Existing code: alignment and gaps

The platform-thread spec (`2026-06-11-platform-thread-infrastructure-design.md` v1.6) was authored against stdexec from the start. Most of it already conforms to the framework's contracts; this section walks the alignment.

#### 7.2.1 Already conforms — no migration needed

| Code path | Alignment | Reference |
|---|---|---|
| `Mashiro/include/Mashiro/Platform/PlatformThread.h` | Hosts the platform scheduler / scope / stop-source; `Run()` is the loop owner | Platform-thread spec sections 5.3, 6.12 |
| `Mashiro/src/Platform/PlatformThread.cpp` | `inplace_stop_source` + `counting_scope` already in use; `StopBridge` solves the cross-thread `RequestStop` problem | Platform-thread spec section 6.12 |
| `Mashiro/src/Platform/Windows/PlatformBackendWindows.cpp` | `MsgWaitForMultipleObjectsEx(QS_ALLINPUT, MWMO_INPUTAVAILABLE)` + manual-reset wake event with `inplace_stop_callback`; rationale documented | Platform-thread spec sections 6, 11 (decision: wake event vs PostMessage); aligns with cross-cutting section 5.4 (platform sleep integration) |
| `Mashiro/src/Platform/Linux/PlatformBackendLinux.cpp` | `epoll` on display-server fd + eventfd wake; same shape as Windows | Platform-thread spec section 6 |
| `Mashiro/include/Mashiro/Platform/EventChannel.h` | SPSC ring + waiter handle pattern; used as the channel underlying the framework's `Stream<T>`-of-`SystemEvent` | Platform-thread spec section 6.3; aligns with L4 `Stream<T>` shape |
| `Mashiro/include/Mashiro/Platform/EventPump.h` | Bookkeep-via-convention + reflection-driven dispatch; same model as the framework's L1 `Traits` | Platform-thread spec sections 6.10, 6.11 |
| `Mashiro/include/Mashiro/Platform/SystemEvent.h` | Variant-of-payloads materialised by reflection over the marker base; the framework's `Traits::PayloadTypeName<T>()` reuses the same reflection helper | Platform-thread spec sections 6.2, 7 |
| `Mashiro/include/Mashiro/Platform/ThreadContract.h` | `kPlatformOnly` / `kAnyThread` annotations are L1-shaped capability tags; the framework's `Affine{Backend::Platform}` annotation reads them | Platform-thread spec section 6.1 |
| `Mashiro/include/Mashiro/Core/MpscQueue.h` | The 256-slot Vyukov-style ring used by `platform_scheduler::SubmitResume`; the framework's `StaticPool` reuses it for its work-item queue | Platform-thread spec sections 6.5, 9 |
| `Mashiro/include/Mashiro/Core/SpscRingBuffer.h` | Underlies `EventChannel`; the framework's `Stream<T>` and `pipeline` reuse it | Platform-thread spec section 6.3 |
| `Mashiro/include/Mashiro/Core/SeqLock.h` | Single-writer, multi-reader lock-free reader for any-thread Manager queries; the framework re-exports it from `Diagnostics` for trace-state reads | Platform-thread spec section 6.9 |
| `Mashiro/include/Mashiro/Core/StructuredLogger.h` | Default `Diagnostics` backend; framework's `Trace::Span` routes to it when no Tracy/Perfetto domain is active | Cross-cutting section 6.1 |
| `Mashiro/include/Mashiro/Core/InlineFunction.h` | Used in op-state members where `std::function` would otherwise allocate | Cross-cutting section 3.6 anti-pattern table |
| `Mashiro/include/Mashiro/Core/Result.h` | `std::expected<T, ErrorCode>` API surface; framework's `with_error<ErrorCode>` adaptor wraps it | Cross-cutting section 4.3 |

#### 7.2.2 Migrates by re-export — alias, no structural change

| Code path | What the framework adds |
|---|---|
| `Mashiro::Platform::scheduler` | Becomes `Mashiro::Async::Backend::Platform` via a one-line alias header `Mashiro/Async/Backend/Platform.h`; the L1 `Affine{Backend::Platform}` and `Cancellable` annotations are added on the alias, not on the underlying type |
| `Mashiro::Platform::Task<T>` | Re-used as one of the L4 coroutine task types; no separate `Async::Task<T>` is introduced for the platform-affine case — the existing typedef is the canonical name |
| `Mashiro::Platform::stop_source` / `stop_token` | Re-exported as `Mashiro::Async::stop_source` / `stop_token` for non-platform pipelines; backends and adaptors operate on the latter alias |
| `Mashiro::Platform::scope` | Re-exported as `Mashiro::Async::Structured::Scope`; the platform-thread scope is *one* `Scope` instance — others may exist per subsystem |
| `Mashiro::Platform::domain` | Re-exported as `Mashiro::Async::Backend::Platform::domain`; user-defined domains compose by delegation |

#### 7.2.3 Migrates with structural change — first to port

These are the items the framework actively rewrites. They are scheduled in phase 4 (L3 + L4) because the framework's adaptors and coroutine machinery are the prerequisites.

| Code path | Change | Rationale |
|---|---|---|
| `Mashiro/demos/Playground/Main.cpp` | Rewrite the demo's main loop to use `Mashiro::Async::Patterns::pipeline` instead of the ad-hoc `while` loop | Demonstrates the framework end-to-end on a real workload |
| `Mashiro/demos/Playground/VulkanCube.cpp` | Replace the per-frame `vkQueueSubmit` + `vkQueueWaitIdle` synchronisation with `VkComputeScheduler` + `co_await` | Validates the L7 extension surface against a real Vulkan workload |
| `Mashiro/demos/Playground/OpenGLCube.cpp` | Same shape as VulkanCube but on OpenGL; tests that user-defined schedulers compose with the framework even when the underlying API is callback-based | OpenGL has no fence equivalent; the user defines a polling scheduler |
| `Mashiro/demos/Core/ToJsonDemo.cpp` | Replace any sync I/O with `Io::async_read` + `parse_json` adaptor | Tests the `Io` backend integration |
| `Mashiro/demos/Core/ToStringDemo.cpp` | No async work; left as-is | Pure compute |
| `Yuki/tests/Core/QueryTest.cpp` | Replace any synchronous test fixtures that ran reflection queries on the main thread with `Inline::scheduler()` so the framework's `consteval` audits run inside the test pipeline | Validates that the framework's compile-time machinery composes with Yuki's reflection-driven query types |
| `Yuki/tests/Core/IdentityTest.cpp` / `MetaClassTest.cpp` / `RootObjectTest.cpp` / `RefCountedTest.cpp` | Add `Mashiro::Async::Diagnostics::AllocCheck` guards around test bodies that touch the framework | Catches allocation regressions in the Yuki object model when integrated with framework pipelines |

The first concrete migration touchpoint is `Mashiro/demos/Playground/Main.cpp` — it is the smallest demo that exercises the platform thread, has no external dependencies, and serves as the canonical "hello async" example.

#### 7.2.4 Does NOT migrate — and why

| Code path | Reason |
|---|---|
| `Mashiro/src/Platform/Windows/ThreadNaming.cpp` (`SetCurrentThreadName`) | Platform-helper free function; not async work, no migration target |
| `Mashiro/include/Mashiro/Core/StackTrace.h` / `Mashiro/src/Core/StackTrace.cpp` | Synchronous stack-walk used in diagnostics; called from one place, no async involvement |
| `Mashiro/include/Mashiro/Core/ABI.h` / `Mashiro/src/Core/ABI.cpp` | Compile-time / startup-time only; not in the async fabric |
| `Mashiro/include/Mashiro/Core/Annotation.h` | Schema-level annotation infrastructure; the framework's L1 annotations are *consumers* of this header, not replacements |
| `Mashiro/include/Mashiro/Core/Hash.h`, `Flags.h`, `FixedString.h`, `TypeTraits.h`, `Meta.h`, `RefCountedMixin.h` | Pure-compile-time utilities; orthogonal to async |
| `Mashiro/include/Mashiro/Core/ChunkedSlotMap.h` | Used inside Managers for state storage; not part of the async surface |
| `Mashiro/include/Mashiro/Core/ConcurrentObjectPool.h` | Used by waiter lists / slot-id-keyed registries; not retired (platform-thread spec section 6.5 documents why) |
| `Mashiro/include/Mashiro/Core/ConcurrentSlabArena.h` | Memory primitive; the framework's PMR arena allocator may use it under the hood, but the API stays |
| `Mashiro/include/Mashiro/Core/CLI.h` | Command-line parsing for tooling; not async work |
| `Mashiro/include/Mashiro/Core/ToString.h`, `ToJson.h`, `DumbPtr.h`, `DumbPtrJson.h`, `LinearAllocator.h`, `Functional.h`, `Polymorphism.h`, `SOA.h`, `Int128.h`, `FalseSharing.h`, `ErrorCode.h` | Pure data / compute utilities |
| `Yuki/include/Yuki/Core/Identity.h`, `MetaClass.h`, `Descriptors.h`, `Diagnostics.h`, `InterfaceFacade.h`, `Query.h`, `RootObject.h`, `FacadeList.h` | Yuki object model; orthogonal — Yuki types are values, not async work. The framework consumes Yuki reflection (annotations, traits) but does not touch its public API |
| `thirdparty/*` | Vendored libraries; no migration. The framework consumes `stdexec`, `tbb`, `tracy`, `perfetto`, `vulkan-headers` as-is |

### 7.3 Deprecation steps

The framework introduces no synonyms for existing types. Where the framework adds an alias (`Mashiro::Async::Backend::Platform` for `Mashiro::Platform::scheduler`), both names continue to work — the alias is the new canonical form, the original is the historical name. There is no removal phase.

The only deprecation is of *idioms*, not of types:

1. **`std::async` in framework consumers.** Discouraged in code review; replaced by `Task<T>` over `StaticPool::scheduler()` or by sender expressions. Existing call sites are migrated per `07-extension.md` section 9.
2. **Detached `std::thread`.** Discouraged in code review; replaced by `Scope::spawn` or `Nursery::spawn`. Existing call sites — there are none in the engine codebase — would migrate the same way.
3. **Manual condvar pipelines.** The `Stream<T>` / `pipeline` migration playbook applies (`07-extension.md` section 9.3). Existing call sites — there are none in the engine codebase — would migrate the same way.

### 7.4 What is NOT migrated and why

The platform layer's dedicated-thread Managers (`GamepadManager`, `FileWatchManager` per platform-thread spec section 7.2) keep their `std::jthread`s. The reasons are documented in the platform-thread spec section 7.2 and remain valid:

- The thread bodies are blocking syscalls (`XInputGetState`, `ReadDirectoryChangesW`) that stdexec cannot suspend.
- The event-emit path is already framework-aligned: it pushes to an MPSC inbox observed by the framework.
- Migrating to a sender-driven model would require non-blocking equivalents (`GameInput`'s callback API, `IOCP` for file watches), which are platform-version-gated and not universally available.

The decision is to migrate the *event-emit* path (which is already aligned) and keep the *blocking syscall* path on a thread.

### 7.5 Migration touchpoints — concrete checklist

Concrete tree paths the migration plan touches, by phase. Phase numbers refer to section 7.1.

**Phase 1 (L0/L1):**

- New: `Mashiro/include/Mashiro/Async/Foundations.h`
- New: `Mashiro/include/Mashiro/Async/Concepts.h`
- New: `Mashiro/include/Mashiro/Async/Traits.h`
- Reuses: `Mashiro/include/Mashiro/Core/Annotation.h` (no change)
- Reuses: `Mashiro/include/Mashiro/Platform/ThreadContract.h` (no change; the new `Affine` annotation reads its enum)

**Phase 2 (L2 — Inline / StaticPool / Platform):**

- New: `Mashiro/include/Mashiro/Async/Backend/Inline.h`
- New: `Mashiro/include/Mashiro/Async/Backend/StaticPool.h`
- New: `Mashiro/src/Async/Backend/StaticPool.cpp` — uses `Mashiro/include/Mashiro/Core/MpscQueue.h` for its work-item queue
- New: `Mashiro/include/Mashiro/Async/Backend/Platform.h` — alias header that includes `Mashiro/Platform/PlatformThread.h` and adds annotations
- Reuses: `Mashiro/include/Mashiro/Platform/PlatformThread.h` (no change)
- Reuses: `Mashiro/src/Platform/PlatformThread.cpp` (no change)
- Reuses: `Mashiro/src/Platform/Windows/PlatformBackendWindows.cpp` (no change; the wake event + `MsgWaitForMultipleObjectsEx` is the canonical platform-thread integration documented in cross-cutting section 5.4)
- Reuses: `Mashiro/src/Platform/Linux/PlatformBackendLinux.cpp` (no change)

**Phase 3 (L2 — Tbb / Io):**

- New: `Mashiro/include/Mashiro/Async/Backend/Tbb.h`
- New: `Mashiro/src/Async/Backend/Tbb.cpp` — depends on `thirdparty/tbb`
- New: `Mashiro/include/Mashiro/Async/Backend/Io.h`
- New: `Mashiro/src/Async/Backend/Io/Linux/IoUring.cpp`
- New: `Mashiro/src/Async/Backend/Io/Windows/Iocp.cpp`

**Phase 4 (L3 / L4):**

- New: `Mashiro/include/Mashiro/Async/Adaptor/{Bulk,Batch,Debounce,Retry,Timeout,Race}.h`
- New: `Mashiro/include/Mashiro/Async/Coro/{Task,Stream,Job}.h`
- Migration: `Mashiro/demos/Playground/Main.cpp` (first end-to-end demo)
- Deprecation marker: `Async::from_future` is published with
  `[[deprecated("migration boundary — see 07-extension.md §7.5")]]` per L4 v0.2 §7.3. The
  deprecation surfaces a compile-time diagnostic at every existing call site; the diagnostic
  *traces back to this migration plan*. New call sites are reviewer-rejected. Audited
  third-party boundaries use the non-deprecated `Async::Extension::from_future` spelling
  (L7 v0.2 §9.1b). Removal target: when every legacy `std::async` / `std::future` call site
  in `Mashiro/` and `Yuki/` has been migrated per `07-extension.md` §9 — at which point the
  deprecated overload is retained as a header-only shim that forwards to
  `Async::Extension::from_future` (no behaviour change, deprecation kept).

**Phase 5 (L5 / L6):**

- New: `Mashiro/include/Mashiro/Async/Structured/{Scope,Nursery}.h`
- New: `Mashiro/include/Mashiro/Async/Patterns/{ParallelFor,Pipeline,Actor,Reactive,ForkJoin,ScatterGather}.h`
- Migration: `Mashiro/demos/Playground/VulkanCube.cpp` and `OpenGLCube.cpp`
- Reuses: `Yuki/tests/Core/QueryTest.cpp` and friends — pipelines added but Yuki types unchanged

**Phase 6 (L7):**

- New: `Mashiro/include/Mashiro/Async/Extension/{Scheduler,Sender,Domain,Plugin}.h`
- Worked example: `Engine/Compute/VkComputeScheduler.h` (under engine code, not framework code)

**Phase 7 (cross-cutting hardening):**

- New: `Mashiro/include/Mashiro/Async/Diagnostics/{Trace,Counters,Probes}.h`
- New: `Mashiro/src/Async/Diagnostics/StructuredLoggerBackend.cpp` — uses `Mashiro/include/Mashiro/Core/StructuredLogger.h`
- New: `Mashiro/src/Async/Diagnostics/TracyBackend.cpp` — depends on `thirdparty/tracy`
- New: `Mashiro/tests/Async/Diagnostics/AllocCheckTest.cpp`
- New: `Mashiro/tests/Async/Diagnostics/StarvationTest.cpp`
- New: `Mashiro/tests/Async/Cancellation/{ChainTest,NurseryTest,ScopeShutdownTest}.cpp`

### 7.6 Acceptance criteria for migration completion

Each migration touchpoint has a measurable acceptance gate. The migration is "complete" when every gate has been observed in CI:

| Touchpoint | Acceptance gate |
|---|---|
| L0/L1 delivery | `Concepts::*` and `Traits::*` evaluate to expected values for every framework backend; the consteval verifier rejects a deliberately-mis-annotated test fixture |
| L2 backend delivery | Each backend has a Catch2 test that runs `schedule()` cross-thread, observes cancellation via `inplace_stop_source::request_stop()`, and verifies zero allocation on the schedule fast path via `Diagnostics::AllocCheck` |
| L3 adaptor delivery | Every adaptor has a test that exercises its completion-signature declaration (asserted via `static_assert`), its cancellation checklist (asserted via `consteval` + runtime stop-test), and its allocation policy |
| L4 coroutine task delivery | `Task<T>` and `Stream<T>` exercise scheduler-affinity round-trip; HALO is observed to elide allocation on the same-thread fast path; cancellation propagates through `co_await` correctly |
| L5 structured concurrency | `Nursery` cancels siblings on child error; `Scope::on_empty()` settles after all spawned senders complete; `Diagnostics::scope_audit()` catches a deliberate escape |
| L6 patterns | `parallel_for(Tbb, ...)`, `pipeline(...)`, `actor(...)` each have a Catch2 test that composes them with at least one other pattern |
| L7 extension | `VkComputeScheduler` example compiles and runs; `Plugin::DescriptorV1` round-trips through a synthetic plugin loader; `any_sender_of` is exercised at the boundary discipline |
| Cross-cutting | `AllocCheck` zero-allocation assertion holds for every documented hot path; `DetectStarvation` flags a deliberately-stalled scheduler; `DetectDeadlock` flags a synthetic cycle |

Each gate is a CI-runnable assertion. The framework ships when every gate passes for one full CI run on Linux + Windows.

### 7.7 Risks and mitigations

| Risk | Mitigation |
|---|---|
| HALO does not elide enough coroutine allocations to keep `Task<T>` cheap | Slot a custom allocator into `default_task_context`; document the per-call allocation count; profile real workloads before passing judgement |
| `transform_sender` rewrites are too aggressive and change observable behaviour | Domain-rewrite tests assert observable equivalence by comparing pre- and post-rewrite completion sequences on a deterministic input |
| `inplace_stop_callback` registration overhead dominates short senders | Measured per backend; if the cost is significant for a hot-path backend, consider a "no cancellation" fast path gated by the absence of the `Cancellable` annotation on the receiver's environment |
| Plugin descriptor schema needs a new field before v1.0 | The schema is versioned; new fields bump the schema version and the engine-side branch interprets old descriptors as zero-filled in the new field |
| User-defined schedulers proliferate and exhaust review bandwidth | The user-extension contract is small (Section 3 of `07-extension.md`); the consteval verifier catches most bugs; review focuses on correctness of cancellation and allocation, not on style |

---

## 8. Status

- v0.1 (initial draft): drafted 2026-06-15 by Subagent E. Locks the cross-cutting contracts (cancellation, allocation, errors, time, diagnostics) and the migration plan with concrete code-path touchpoints.
- **v0.2 (this revision):** synthesis-pass adjustments landed 2026-06-16. §6.1 adds the diagnostics-registration ordering rule (`09-synthesis.md` §2.22) with the `MASHIRO_DIAGNOSTICS_REQUIRED`-gated debug assertion. §7.5 cross-references L4 v0.2's `[[deprecated]]` `from_future` marker and L7 v0.2's audited-boundary `Async::Extension::from_future` spelling, completing the bidirectional pointer-pair the synthesis pass §2.9 mandated. No structural changes to the cancellation, allocation, error, or time contracts; no changes to the migration phasing or acceptance gates.
- v1.0: post-implementation revision after phases 1–7 land and the migration touchpoints in §7.5 are exercised end-to-end.

---

*End of cross-cutting concerns spec.*
