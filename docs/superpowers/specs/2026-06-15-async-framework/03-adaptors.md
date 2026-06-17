# Mashiro Async Framework — L3 Sender Adaptors

**Status:** Draft v0.2 (layer L3, written under the umbrella of `00-overview.md` v0.2)
**Date:** 2026-06-15
**Author:** Mashiro Engine team — Subagent C
**Scope:** `Mashiro::Async` namespace, headers under `Mashiro/include/Mashiro/Async/Adaptor/`, sources under `Mashiro/src/Async/Adaptor/`. Composes with L0/L1 (Subagent A — `01-foundations.md`) and L2 backends (Subagent B — `02-backends.md`); produces inputs for L4 (`04-coroutine-tasks.md`) and L6 patterns.

### Revision history

- **v0.1** — initial draft. Defines `bulk`, `batch`, `debounce`, `throttle`, `retry`, `timeout`, `race`, `materialise` / `dematerialise`. Each adaptor: form, completion signatures, op-state shape, allocation, cancellation checklist, worked example. Closes with the adaptor / domain interaction matrix.
- **v0.2** — incorporates synthesis-pass adjudications (`09-synthesis.md` §2.3, §2.16, §2.26, §2.30). §3 v0.2 introduces the per-adaptor `static_assert` cancellation audit (the audit helper lives in `Adaptor/detail/CancellationAudit.h`). §6 v0.2 names `std::pmr::vector<T>` explicitly as the `batch` value type, with a note flagging the future switch to `Mashiro::Core::Vector<T>` when core lands an allocator-aware vector. §9 / §11 v0.2 rename `Foundations::propagate_error_signatures` and `Foundations::union_error_signatures` references to L3-local helpers in `Mashiro::Async::Adaptor::detail::` — see §3.2 below. The published L1 helper `Foundations::union_signatures<S...>` is unchanged. §14 v0.2 adds the `with_allocator` and `with_stop_source` environment-rewriter adaptors; the status section is bumped to §16.

---

## 1. Purpose

This layer **adds nothing new to the sender vocabulary** — every adaptor here is a thin, framework-shaped wrapper over stdexec primitives or a small composition of them. The reasons L3 exists are:

1. **Capability-driven specialisation.** `bulk` defers to `BulkScheduler::schedule_bulk` when the receiver's environment exposes a bulk-capable scheduler, and otherwise expands to a `let_value` over `when_all` of unitary tasks. The decision is `consteval`, driven by `Traits::OffersBulk_v<S>` from L1.
2. **Time-based combinators.** stdexec does not ship `debounce` / `throttle` / `batch` — these are fundamental to UI / telemetry / reactive pipelines and benefit from a single, cancellation-correct, allocation-disciplined implementation.
3. **Reliability adaptors.** `retry` and `timeout` are two of the most-mistakenly-rolled adaptors in production code; doing them once, correctly, against the project's stop-token discipline is worth the seventy lines of header it costs.
4. **Domain-friendly shape.** Each adaptor is a tag-typed sender so backend domains (Tbb, Io, Platform) can `transform_sender` it into a backend-native equivalent without changing observable completion signatures.

L3 imposes no new vocabulary. It does not introduce a scheduler. It does not extend the concept hierarchy. It is built entirely on L0 (vocabulary), L1 (annotations + traits), and L2 (backend names).

---

## 2. Goals

- Provide every framework-promised adaptor in §8.3 of the overview.
- Guarantee per-adaptor: explicit `completion_signatures_of_t` specialisation, explicit op-state layout, explicit cancellation registration via `inplace_stop_callback`.
- Allocate zero heap memory on the synchronous start path of every adaptor. Time-based adaptors that need a timer use the `Io` scheduler's `async_timer`; the timer's op-state is owned by the adaptor's op-state, not heap-allocated.
- Cooperate with backend domains: each adaptor declares a tag type so `domain::transform_sender` can match it with no name games.
- Be reflection-introspectable: every adaptor's tag type carries the L1 annotations Subagent A's `Traits::*_v` queries expect.

## 3. Non-Goals

- New scheduler implementations. Adaptors compose existing L2 schedulers; they do not run work.
- Stream-graph orchestration (that is L6 `Patterns::Pipeline`).
- Coroutine task types (L4).
- Replacing stdexec's own `then`, `let_value`, `when_all`, `when_any`, `continues_on`, `stopped_as_optional`, `bulk`, etc. They are imported as-is from L0.

---

## 4. Cross-cutting structure

Every L3 adaptor follows the same six-part shape. Per-adaptor sections in §5–§12 fill in the details.

### 4.1 Tag, sender, and CPO surface

```cpp
namespace Mashiro::Async::Adaptor {

    // Tag type for domain-rewrite matching.
    struct BulkT { };

    // Pipeable closure produced by `bulk(n, fn)`.
    template<class N, class Fn>
    struct BulkClosure {
        N    n;
        Fn   fn;

        template<stdexec::sender Upstream>
        friend auto operator|(Upstream&& u, BulkClosure self)
            -> stdexec::sender auto;
    };

    // Free function — call site.
    template<class N, class Fn>
    constexpr auto bulk(N n, Fn fn) -> BulkClosure<N, Fn>;
}
```

The tag `BulkT` is the type a domain's `transform_sender` matches against. The closure is the value the user pipelines through `|`. The free function is the call-site spelling. Every adaptor in this spec follows this triple.

### 4.2 Completion-signature contract

Each adaptor specialises `stdexec::completion_signatures_of_t` for its sender type explicitly. Signatures are unioned, never silently extended. The default rules:

- **Value signatures** are inherited from upstream unless the adaptor reshapes them (`materialise` does, `bulk` does not).
- **Error signatures** are extended only when the adaptor itself can fail. `timeout`, `retry`, and `bulk` (when expanding to `when_all`) do; `debounce`, `throttle`, `batch`, and `race` do not (they only forward upstream errors).
- **Stopped signatures** are propagated when *any* upstream offers `set_stopped_t()` and unconditionally added by adaptors that themselves stop on cancellation (`timeout`).

Each adaptor section pins the exact union with a `using completion_signatures = ...` table.

### 4.3 Op-state shape

Each adaptor's op-state is a struct with (a) the upstream op-state(s) inline (no heap), (b) the adaptor-specific state (timer handle, retry counter, batch buffer, atomic flag), (c) the receiver, and (d) the `inplace_stop_callback` member (one per adaptor that owns external state). Alignment is the maximum alignment of the members. No pointer indirection except where the adaptor genuinely owns multiple alternatives that cannot fit a discriminated union (`race`, `when_any`).

### 4.4 Allocation behaviour

Adaptors that internally schedule timers (`debounce`, `throttle`, `batch`, `timeout`) do **not** allocate; they hold an `Io::TimerOpState` member by value (its size is exposed by `Backend::Io::timer_op_state_size_v`). Adaptors that fan-out (`bulk` expansion path, `race`) hold the children's op-states inline through `stdexec::__variant` / `stdexec::__tuple` techniques — same approach `when_all` uses internally.

Heap fallback exists for one specific case: `bulk` over a non-`BulkScheduler`, when `n` is dynamic and the inline-children budget is exceeded. This path is documented at the call site by the `[[=Async::Allocates{Where::OpState}]]` annotation that flows through the closure into the sender environment.

### 4.5 Cancellation handling — the master checklist

Every adaptor section ends with a six-row table that must be filled in:

| Step | Requirement |
|------|-------------|
| 1. Token acquisition | Adaptor reads the stop-token from `get_stop_token(get_env(receiver))` once, in `start`. |
| 2. Callback installation | If the adaptor owns external state, `inplace_stop_callback<Cb>` is constructed *after* the upstream is started but *before* any external resource is acquired. |
| 3. Callback body | The callback is **trivial** — sets an atomic flag, calls `cancel()` on the timer / IO handle, and (if needed) wakes the scheduler. No allocation, no virtual dispatch, no upstream destruction. |
| 4. Race with completion | Completion paths (value / error / stopped) destroy the callback before destroying the upstream's op-state. Re-entry from a callback invoked during destruction is impossible because `inplace_stop_callback`'s destructor synchronises. |
| 5. Source of `set_stopped` | Adaptor completes with `set_stopped()` only when (a) cancellation occurred *and* (b) upstream has not already produced a value. Otherwise upstream's value/error wins. |
| 6. Stop signature | The adaptor's `completion_signatures` includes `set_stopped_t()` iff (a) upstream offers it, *or* (b) the adaptor itself stops on cancellation. |

Each adaptor section repeats the table, filled in.

### 4.6 Worked-example shape

Each adaptor section ends with a one-line worked example expressed as a sender expression, plus a one-paragraph commentary that highlights the cancellation, allocation, and rewrite behaviours.

### 4.7 Cancellation audit (v0.2)

Per `09-synthesis.md` §2.26, every L3 adaptor's op-state is subject to a `static_assert` audit:
the op-state must either register an `inplace_stop_callback` *or* carry no cancellable state.
"Cancellable state" is defined structurally: any member that owns external resources (file
descriptors, timer handles, kernel completion slots) or any member that is itself a sender's
op-state in turn requires a registered callback. Senders that fan-out (e.g. `race`, expansion-
path `bulk`) recurse — each child's op-state participates in the audit.

The audit helper lives at `Mashiro/include/Mashiro/Async/Adaptor/detail/CancellationAudit.h`:

```cpp
namespace Mashiro::Async::Adaptor::detail {

    // Reflects over Op's members; fails the assert if any member owns external state
    // and no inplace_stop_callback member is present.
    template<class Op>
    consteval bool AuditCancellation();

    template<class Op>
    inline constexpr bool cancellation_audited_v = AuditCancellation<Op>();

    #define MASHIRO_AUDIT_CANCELLATION(Op) \
        static_assert(::Mashiro::Async::Adaptor::detail::cancellation_audited_v<Op>, \
            "[Mashiro::Async::Adaptor] op-state owns cancellable state but does not " \
            "register an inplace_stop_callback")

} // namespace Mashiro::Async::Adaptor::detail
```

Every adaptor section closes its op-state declaration with `MASHIRO_AUDIT_CANCELLATION(MyOp)`.
The audit fires at template instantiation, not at every translation unit, so the diagnostic
appears once per adaptor type and identifies the offending member by its reflected name.

### 4.8 L3-local completion-signature helpers (v0.2)

The published L1 helper `Mashiro::Async::Traits::union_signatures<S...>` covers most
adaptors' signature unions. Two patterns are L3-local sugar and live in
`Adaptor/detail/SignatureHelpers.h`:

```cpp
namespace Mashiro::Async::Adaptor::detail {

    // Forward upstream's error completions unchanged. Equivalent to a single-sender call
    // to Traits::union_signatures filtered to set_error_t alternatives.
    template<stdexec::sender S, class Env = stdexec::empty_env>
    using propagate_errors = /* filtered subset of completion_signatures_of_t<S, Env> */;

    // Union the error completions of N senders (without re-emitting their value/stopped
    // alternatives — those are handled separately by the adaptor's own value-shape rule).
    template<stdexec::sender... Ss>
    using union_errors = /* fold over propagate_errors<Ss...> */;

} // namespace Mashiro::Async::Adaptor::detail
```

These are adaptor-internal — they are not exported from `Async::*` and not intended for
user code. Adaptor sections (§6, §9, §11 below) use them through the `detail::` qualified
name.

---

## 5. `bulk(n, fn)` — data-parallel fan-out

Forks `n` invocations of `fn(i)` for `i ∈ [0, n)` and joins on completion. When the receiver's environment exposes a `BulkScheduler`, the call lowers to that scheduler's native `schedule_bulk`; otherwise it expands to a `when_all` over `n` unitary `then` tasks scheduled on the upstream's `get_completion_scheduler<set_value_t>`.

### 5.1 Form

```cpp
namespace Mashiro::Async {

    // Free function — used as `bulk(n, fn)` or `s | bulk(n, fn)`.
    template<class N, class Fn>
    constexpr auto bulk(N n, Fn fn) -> Adaptor::BulkClosure<N, Fn>;

    // Sender form for users that already have a sender of (range_t, T).
    template<stdexec::sender S, class N, class Fn>
    constexpr auto bulk(S&& s, N n, Fn fn) -> stdexec::sender auto;
}
```

Example call:

```cpp
auto s = stdexec::just(image_view)
       | Async::continues_on(tbb_sched)
       | Async::bulk(image_view.height(), [&](std::size_t row) {
             process_row(image_view, row);
         });
```

### 5.2 Completion signatures

```cpp
template<class S, class N, class Fn>
struct BulkSender { using is_sender = void; /* ... */ };

template<class S, class N, class Fn, class Env>
struct stdexec::completion_signatures_of_t<BulkSender<S, N, Fn>, Env> {
    using type = Foundations::union_signatures<
        completion_signatures_of_t<S, Env>,         // value/error/stopped from upstream
        completion_signatures<set_error_t(std::exception_ptr)>, // fn may throw
        completion_signatures<set_stopped_t()>      // any child cancelled
    >;
};
```

The value signatures of the upstream are forwarded unchanged (the canonical case is `set_value_t()`, but `bulk` is also defined when upstream completes with a value that `fn` ignores — `fn` is invoked with the index alone, the upstream value is preserved through the join). `set_error_t(std::exception_ptr)` is unioned in because user-supplied `fn` may throw; this is the only adaptor that adds an error type unconditionally.

### 5.3 Op-state shape

Two specialisations, dispatched at compile time by `Traits::OffersBulk_v<CompletionSched<S>>`:

```cpp
// Specialisation A: upstream completes on a BulkScheduler.
template<class S, class N, class Fn, class Recv>
struct BulkOp_Native {
    [[no_unique_address]] connect_result_t<S, BulkRecv<...>> upstream_op;
    [[no_unique_address]] Fn                                 fn;
    [[no_unique_address]] Recv                               recv;
    N                                                        n;
};

// Specialisation B: expansion path. Holds an inline array of child op-states,
// or — when n exceeds the inline budget — a heap-allocated buffer.
template<class S, class N, class Fn, class Recv>
struct BulkOp_Expand {
    static constexpr std::size_t kInlineChildren = 16;
    [[no_unique_address]] connect_result_t<S, ForwardRecv<...>> upstream_op;
    [[no_unique_address]] Fn                                    fn;
    [[no_unique_address]] Recv                                  recv;
    union {
        std::array<ChildOp, kInlineChildren> inline_children;
        ChildOp*                              heap_children;
    };
    std::atomic<std::size_t> remaining;
    std::atomic<int>         completion_state;  // 0 = pending, 1 = error, 2 = stopped
};
```

Alignment is `alignof(ChildOp)` rounded up to the cache line size when `kInlineChildren > 1`. The native specialisation has zero per-child storage in the adaptor — the backend owns child state.

### 5.4 Allocation behaviour

- Native path: zero allocation. Backend handles fan-out internally.
- Expansion path with `n ≤ 16`: zero allocation, inline array.
- Expansion path with `n > 16`: one allocation through `get_allocator(get_env(recv))`. Annotated `[[=Async::Allocates{Where::OpState}]]` on `BulkSender`, so `Traits::AllocatesIn_v<BulkSender>` reports it.

### 5.5 Cancellation checklist

| Step | Behaviour |
|------|-----------|
| 1. Token acquisition | `start` reads `get_stop_token(get_env(recv))`. |
| 2. Callback installation | One `inplace_stop_callback` registered before the first child is started; on the native path the backend is given the same token via the child receiver's environment. |
| 3. Callback body | Stores `2` into `completion_state`. The native path forwards to the backend's stop-source; the expansion path iterates child op-states and calls `cancel()` on each (cheap because each child is just a `then` over `schedule()`). |
| 4. Race with completion | The first child to observe the stopped state owns the join; later children's completions are dropped (no double-complete). |
| 5. Source of `set_stopped` | Emitted iff cancellation arrived before any child completed with an error. Errors take precedence over stopped (P2300 rule). |
| 6. Stop signature | Yes — `bulk` always advertises `set_stopped_t()` because cancellation is observable mid-fan-out. |

### 5.6 Worked example

```cpp
co_await (Async::just()
        | Async::continues_on(Backend::Tbb::scheduler())
        | Async::bulk(N, [&](std::size_t i){ rows[i] = render_row(scene, i); }));
```

On the Tbb backend the closure is rewritten by `Tbb::domain::transform_sender` into a `tbb::parallel_for(0, N, ...)` call and the expansion-path op-state is never instantiated — `Traits::OffersBulk_v<Tbb::scheduler> == true` selects specialisation A. On the `Inline` scheduler, specialisation B is selected, the children run sequentially on the calling thread, and `bulk` degenerates to a loop with structural cancellation through the shared stop-token.

---

## 6. `batch(window, max_size)` — coalesce stream elements

Coalesces successive elements of an upstream `Stream<T>` into batches that are emitted (a) when `max_size` elements have accumulated, or (b) when `window` time has elapsed since the *first* element of the current batch. `window` may be a `std::chrono::duration` or a `std::size_t` count (the count form is degenerate with `max_size` and exists for symmetry with reactive libraries).

### 6.1 Form

```cpp
namespace Mashiro::Async {
    template<class Window, class Size = std::size_t>
    constexpr auto batch(Window window, Size max_size = std::numeric_limits<Size>::max())
        -> Adaptor::BatchClosure<Window, Size>;
}
```

Example call:

```cpp
auto batched = events
             | Async::batch(std::chrono::milliseconds(16), 256);
// emits a Vector<Event> every 16ms or every 256 events, whichever first.
```

### 6.2 Completion signatures

`batch` operates on a `Stream<T>` (sender-of-optional, see L4). The output is a
`Stream<std::pmr::vector<T>>`:

```cpp
template<class S, class W, class N, class Env>
struct stdexec::completion_signatures_of_t<BatchSender<S, W, N>, Env> {
    using type = Foundations::union_signatures<
        completion_signatures<set_value_t(std::pmr::vector<value_t<S>>)>,
        Adaptor::detail::propagate_errors<S, Env>,
        completion_signatures<set_stopped_t()>
    >;
};
```

The value type changes from `T` to `std::pmr::vector<T>` (v0.2, per `09-synthesis.md`
§2.30); the polymorphic allocator is pulled from `get_allocator(get_env(recv))`. Error and
stopped signatures flow through unchanged.

> v0.2 forward-compatibility note: when Mashiro core publishes an allocator-aware
> `Mashiro::Core::Vector<T>` (currently absent), `batch`'s value type switches to that.
> The public adaptor shape (a container-of-`T`) does not change because consumers iterate
> via `begin()` / `end()` / `size()` and never name the container type directly. The
> switch is a soft ABI break documented in the `batch` header at that time.

### 6.3 Op-state shape

```cpp
template<class S, class W, class N, class Recv>
struct BatchOp {
    [[no_unique_address]] connect_result_t<S, BatchRecv<...>> upstream_op;
    [[no_unique_address]] connect_result_t<TimerSender, TimerRecv<...>> timer_op;
    std::pmr::vector<value_t<S>>  buffer;          // grows up to max_size (v0.2)
    W                             window;
    N                             max_size;
    [[no_unique_address]] Recv    recv;
    std::atomic<std::uint8_t>     phase;           // 0 = empty, 1 = filling, 2 = flushing
    std::optional<inplace_stop_callback<StopCb>> stop_cb;
};
```

The buffer is constructed empty; its first allocation is deferred to the first element. The timer op-state is constructed inline (not started) and `start`ed when phase transitions 0 → 1.

### 6.4 Allocation behaviour

- One allocation when the first element of each batch arrives (the `std::pmr::vector<T>` reserve). The polymorphic allocator is queried from `get_allocator(get_env(recv))` (the rewriter `with_allocator(s, alloc)` at §14 below is the call-site spelling that supplies it). Subsequent appends within the batch are amortised reserves.
- Zero per-emission overhead beyond the move of the buffer into the downstream `set_value`.
- Annotated `[[=Async::Allocates{Where::Output}]]` on `BatchSender`.

### 6.5 Cancellation checklist

| Step | Behaviour |
|------|-----------|
| 1. Token acquisition | At `start`, before connecting upstream. |
| 2. Callback installation | Single callback constructed after upstream and timer ops are connected, before either is started. |
| 3. Callback body | Cancels the timer (`timer_op.cancel()`), forwards stop to upstream's stop-token (which is a child of ours), and stores `2` into `phase` to short-circuit any in-flight buffer append. |
| 4. Race with completion | A flush in progress is allowed to complete; the *next* element triggers `set_stopped` instead of starting a new batch. |
| 5. Source of `set_stopped` | Emitted when stop arrives during phase 0 or after a flush. If stop arrives during phase 1, the partial buffer is dropped (no half-batch leaks). |
| 6. Stop signature | Yes. |

### 6.6 Worked example

```cpp
auto telemetry = sensor.events()
               | Async::batch(std::chrono::milliseconds(50), 1024)
               | Async::then([](auto&& batch){ persist(batch); });
```

The 50 ms window absorbs ingest jitter; the 1024 cap puts a hard upper bound on memory pressure under sensor faults that produce burst-mode data. Cancellation through `stop_token` flushes any in-flight batch as a `set_stopped`, never as a partial value.

---

## 7. `debounce(dur)` — collapse bursts to one per interval

Emits the *most recent* upstream value if and only if `dur` has elapsed since the prior value. Bursts collapse to a single trailing emission; the leading-edge variant is `throttle` (§8). `debounce` is the canonical "user finished typing" / "settle" combinator.

### 7.1 Form

```cpp
namespace Mashiro::Async {
    template<class Dur>
    constexpr auto debounce(Dur dur) -> Adaptor::DebounceClosure<Dur>;
}
```

Example call:

```cpp
auto settled = textbox.changes() | Async::debounce(200ms);
```

### 7.2 Completion signatures

```cpp
template<class S, class D, class Env>
struct stdexec::completion_signatures_of_t<DebounceSender<S, D>, Env> {
    using type = Foundations::union_signatures<
        completion_signatures_of_t<S, Env>,
        completion_signatures<set_stopped_t()>
    >;
};
```

Value and error signatures are forwarded unchanged. `set_stopped_t()` is added unconditionally.

### 7.3 Op-state shape

```cpp
template<class S, class D, class Recv>
struct DebounceOp {
    [[no_unique_address]] connect_result_t<S, DebounceRecv<...>> upstream_op;
    [[no_unique_address]] connect_result_t<TimerSender, DebounceTimerRecv<...>> timer_op;
    std::optional<value_t<S>>    pending;          // most recent value
    D                            dur;
    [[no_unique_address]] Recv   recv;
    std::atomic<std::uint64_t>   gen;              // generation counter — see §7.5
    std::optional<inplace_stop_callback<StopCb>> stop_cb;
};
```

The generation counter is the linchpin: every new upstream value increments `gen`, and the timer-completion path checks `gen` against the value it captured when starting the timer; mismatch → discard. This is what gives `debounce` its "only the trailing edge wins" semantics without re-allocating a timer per element.

### 7.4 Allocation behaviour

Zero allocation. `pending` is a `std::optional<T>` member; the timer op-state is inline.

### 7.5 Cancellation checklist

| Step | Behaviour |
|------|-----------|
| 1. Token acquisition | `start`. |
| 2. Callback installation | Before upstream and timer are started. |
| 3. Callback body | Cancels the timer, requests-stop on the upstream-child token, sets `gen` to a sentinel (`UINT64_MAX`) so any outstanding timer firing is discarded. |
| 4. Race with completion | A timer firing concurrent with cancellation is gated by the `gen` check — at most one of `set_value` / `set_stopped` lands. |
| 5. Source of `set_stopped` | Emitted on stop *and* on upstream `set_stopped`. |
| 6. Stop signature | Yes. |

### 7.6 Worked example

```cpp
auto applied = filter.changes()
             | Async::debounce(150ms)
             | Async::then([](auto v){ apply_filter(v); });
```

A user dragging a slider produces dozens of `changes()` per second. `debounce(150ms)` makes `apply_filter` run exactly once, on the most recent value, 150 ms after the user stops. Cancelling the surrounding scope drops any pending value with `set_stopped`.

---

## 8. `throttle(dur)` — rate-limit emission

Emits at most one value per `dur`. Unlike `debounce`, the *leading edge* is preserved — the first value passes immediately and subsequent values within `dur` are dropped. A trailing-edge variant `throttle_trailing(dur)` is offered for symmetry with `debounce`.

### 8.1 Form

```cpp
namespace Mashiro::Async {
    template<class Dur>
    constexpr auto throttle(Dur dur) -> Adaptor::ThrottleClosure<Dur>;

    template<class Dur>
    constexpr auto throttle_trailing(Dur dur) -> Adaptor::ThrottleTrailingClosure<Dur>;
}
```

### 8.2 Completion signatures

Identical to `debounce`: upstream value/error forwarded, `set_stopped_t()` added.

### 8.3 Op-state shape

```cpp
template<class S, class D, class Recv>
struct ThrottleOp {
    [[no_unique_address]] connect_result_t<S, ThrottleRecv<...>> upstream_op;
    [[no_unique_address]] connect_result_t<TimerSender, ThrottleTimerRecv<...>> timer_op;
    D                            dur;
    [[no_unique_address]] Recv   recv;
    std::atomic<bool>            gate_open;        // true → next value passes; false → drop
    std::optional<inplace_stop_callback<StopCb>> stop_cb;
};
```

`gate_open` starts true. On every passed value, `gate_open` is set false and the timer is started; on timer fire, `gate_open` is set true again. Dropped values are discarded inline (no buffer).

### 8.4 Allocation behaviour

Zero allocation.

### 8.5 Cancellation checklist

| Step | Behaviour |
|------|-----------|
| 1. Token acquisition | `start`. |
| 2. Callback installation | Before any value is admitted. |
| 3. Callback body | Cancels timer, requests-stop on upstream-child token, latches `gate_open` to false to short-circuit subsequent admits. |
| 4. Race with completion | A timer firing concurrent with stop is benign — it only flips `gate_open`, no emission. |
| 5. Source of `set_stopped` | Emitted on stop or upstream stop. |
| 6. Stop signature | Yes. |

### 8.6 Worked example

```cpp
auto sampled = mouse.moves() | Async::throttle(16ms);  // ~60 Hz
```

Mouse hardware delivers 1000 Hz on modern devices; the renderer cares about frame rate. `throttle(16ms)` admits the first move per frame and discards the rest. Cancellation drops any in-flight throttle window.

---

## 9. `retry(policy)` — re-run a sender on failure

Repeatedly connects-and-starts the upstream sender until it (a) completes with `set_value`, (b) is cancelled, or (c) the policy declares the failure terminal. Each attempt is a fresh op-state; the upstream sender is required to be `connectable` (i.e. movable / copyable) — `retry` does not work on `&&`-only senders. Retry policies are values that satisfy the `RetryPolicy` concept (§9.3).

### 9.1 Form

```cpp
namespace Mashiro::Async {
    template<class Policy>
    constexpr auto retry(Policy policy) -> Adaptor::RetryClosure<Policy>;

    namespace Retry {
        struct Fixed       { std::chrono::nanoseconds delay; std::size_t max_attempts; };
        struct Exponential { std::chrono::nanoseconds initial; double factor; std::chrono::nanoseconds cap; std::size_t max_attempts; };
        // User-defined policies model the RetryPolicy concept (§9.3).
    }
}
```

Example call:

```cpp
auto resilient = http.get(url) | Async::retry(Retry::Exponential{50ms, 2.0, 5s, 6});
```

### 9.2 Completion signatures

```cpp
template<class S, class P, class Env>
struct stdexec::completion_signatures_of_t<RetrySender<S, P>, Env> {
    using type = Foundations::union_signatures<
        completion_signatures_of_t<S, Env>,            // value forwarded
        // Errors only surface when the policy gives up; the policy may also
        // synthesise its own error type.
        completion_signatures<set_error_t(typename P::final_error_t)>,
        completion_signatures<set_stopped_t()>
    >;
};
```

The exact error union depends on the policy: `Retry::Fixed` and `Retry::Exponential` use `final_error_t = std::exception_ptr`. A user policy may define `final_error_t = std::variant<...>` to surface the *cause* of the final failure.

### 9.3 `RetryPolicy` concept

```cpp
template<class P>
concept RetryPolicy = requires (P& p, std::exception_ptr e, std::size_t n) {
    typename P::final_error_t;
    { p.should_retry(e, n) } -> std::same_as<std::optional<std::chrono::nanoseconds>>;
    // nullopt → give up (terminal); some(d) → wait d, then retry.
    { p.terminal_error(e) } -> std::convertible_to<typename P::final_error_t>;
};
```

The framework provides `Fixed` and `Exponential`; users plug in custom policies (e.g. circuit breakers) by modelling the concept.

### 9.4 Op-state shape

```cpp
template<class S, class P, class Recv>
struct RetryOp {
    [[no_unique_address]] S         upstream;       // re-connectable on each attempt
    [[no_unique_address]] P         policy;
    [[no_unique_address]] Recv      recv;
    std::size_t                     attempt;
    Variant<                                       // exec::__variant
        Empty,
        connect_result_t<S, RetryRecv<...>>,        // active attempt
        connect_result_t<TimerSender, RetryTimerRecv<...>> // active backoff
    > state;
    std::optional<inplace_stop_callback<StopCb>> stop_cb;
};
```

The variant lets exactly one child op-state exist at a time without dynamic allocation. Re-entering a new attempt destroys the old child in place and constructs the next.

### 9.5 Allocation behaviour

Zero allocation when upstream is move-connectable (the typical case). When upstream is reference-only, `retry` reports a compile error pointing at the policy site.

### 9.6 Cancellation checklist

| Step | Behaviour |
|------|-----------|
| 1. Token acquisition | `start`. |
| 2. Callback installation | Once at construction; survives across attempts. |
| 3. Callback body | Calls `cancel()` on the active variant alternative — either the child sender or the backoff timer. |
| 4. Race with completion | Cancellation during a backoff cancels the timer and emits `set_stopped` immediately; cancellation during an attempt forwards to the child receiver's stop-token. |
| 5. Source of `set_stopped` | On stop, or when upstream itself stops. |
| 6. Stop signature | Yes. |

### 9.7 Worked example

```cpp
auto loaded = vk.loadShader(path)
            | Async::retry(Retry::Exponential{10ms, 2.0, 1s, 4})
            | Async::timeout(5s);
```

Pairs `retry` with `timeout` (§10): `retry` keeps re-running the load on transient failures with exponential backoff capped at 1 s; the outer `timeout` ensures the *whole* retry chain bounds at 5 s wall-clock. Stop-token from the surrounding scope cancels both.

---

## 10. `timeout(dur)` — bound a sender's wall-clock duration

Completes the upstream with `set_stopped()` if it does not produce any completion within `dur`. The error path of upstream is forwarded; the value path is forwarded; only "no completion at all in time" maps to stopped.

### 10.1 Form

```cpp
namespace Mashiro::Async {
    template<class Dur>
    constexpr auto timeout(Dur dur) -> Adaptor::TimeoutClosure<Dur>;
}
```

Example call:

```cpp
auto bounded = network.fetch(url) | Async::timeout(2s);
```

### 10.2 Completion signatures

```cpp
template<class S, class D, class Env>
struct stdexec::completion_signatures_of_t<TimeoutSender<S, D>, Env> {
    using type = Foundations::union_signatures<
        completion_signatures_of_t<S, Env>,
        completion_signatures<set_stopped_t()>     // unconditional
    >;
};
```

### 10.3 Op-state shape

```cpp
template<class S, class D, class Recv>
struct TimeoutOp {
    [[no_unique_address]] connect_result_t<S, TimeoutRecv<...>> upstream_op;
    [[no_unique_address]] connect_result_t<TimerSender, TimeoutTimerRecv<...>> timer_op;
    D                            dur;
    [[no_unique_address]] Recv   recv;
    std::atomic<std::uint8_t>    winner;           // 0 = pending, 1 = upstream, 2 = timer
    std::optional<inplace_stop_callback<StopCb>> stop_cb;
};
```

The `winner` atomic resolves the upstream-vs-timer race exactly once; the loser's completion is discarded.

### 10.4 Allocation behaviour

Zero allocation.

### 10.5 Cancellation checklist

| Step | Behaviour |
|------|-----------|
| 1. Token acquisition | `start`. |
| 2. Callback installation | Before either child is started. |
| 3. Callback body | Cancels both the upstream and the timer. The first to complete wins; subsequent completions are dropped. |
| 4. Race with completion | `winner` CAS resolves the three-way race (upstream, timer, stop) exactly once. |
| 5. Source of `set_stopped` | Emitted when the timer wins, or when external stop wins. |
| 6. Stop signature | Yes (unconditional). |

### 10.6 Worked example

```cpp
co_await (load_asset(path) | Async::timeout(500ms));
```

If the asset load takes longer than 500 ms, the surrounding `co_await` resumes with the cancellation path of the coroutine and the underlying load's stop-token has fired, freeing any held resources. No exception is thrown.

---

## 11. `race(s1, s2, ...)` — first-to-complete wins

Connects N senders, starts them all, and forwards the first completion (value or error). Losers are cancelled via their stop-token and their op-states are destroyed before `race` itself completes. Identical in spirit to stdexec's `when_any`, but `race` adds two project-specific obligations:

- **Cancellation-on-loss is observable.** Losers' op-states observe `set_stopped` before destruction; this is the contract that lets users register cleanup in their own `set_stopped` handler.
- **Heterogeneous signatures.** When senders have different value types, the result type is `std::variant<value_t<s1>, value_t<s2>, ...>` (or, for value-shape unification, the user's explicit `union_value<As...>` opt-in).

### 11.1 Form

```cpp
namespace Mashiro::Async {
    template<stdexec::sender... Ss>
    constexpr auto race(Ss&&... ss) -> RaceSender<std::decay_t<Ss>...>;

    template<class T, stdexec::sender... Ss>
    constexpr auto race_into(Ss&&... ss) -> RaceIntoSender<T, std::decay_t<Ss>...>;
    // race_into pre-declares the value type, useful when senders complete with
    // convertible-but-distinct value types.
}
```

### 11.2 Completion signatures

```cpp
template<class... Ss, class Env>
struct stdexec::completion_signatures_of_t<RaceSender<Ss...>, Env> {
    using type = Foundations::union_signatures<
        completion_signatures<set_value_t(std::variant<value_t<Ss>...>)>,
        Adaptor::detail::union_errors<Ss...>,                      // v0.2 — L3-local helper
        completion_signatures<set_stopped_t()>
    >;
};
```

### 11.3 Op-state shape

```cpp
template<class... Ss, class Recv>
struct RaceOp {
    Tuple<connect_result_t<Ss, RaceRecv<...>>...> children;   // exec::__tuple
    [[no_unique_address]] Recv                    recv;
    std::atomic<int>                              winner;     // -1 pending, else child index
    std::array<inplace_stop_source, sizeof...(Ss)> child_sources;
    std::optional<inplace_stop_callback<StopCb>>  stop_cb;
};
```

Each child is given its own stop-source as a child of the parent token; the winner-resolving CAS triggers cancellation on every other child source.

### 11.4 Allocation behaviour

Zero allocation. The tuple of op-states is stored inline; the per-child stop-source array is fixed size.

### 11.5 Cancellation checklist

| Step | Behaviour |
|------|-----------|
| 1. Token acquisition | `start`. |
| 2. Callback installation | One outer callback registered on the parent token; per-child callbacks are not used (we use stop-sources directly). |
| 3. Callback body | Iterates `child_sources` and calls `request_stop()` on each. |
| 4. Race with completion | Every child's `set_*` path attempts `winner.compare_exchange_strong(-1, my_index)`; the loser's completion is dropped. |
| 5. Source of `set_stopped` | Emitted iff cancellation arrived before any child completed with value or error. |
| 6. Stop signature | Yes. |

### 11.6 Worked example

```cpp
auto first = Async::race(http.fetch_primary(url), http.fetch_replica(url));
co_await first;
```

Hedged-request pattern: fire the same request at two replicas; the faster one wins, the slower is cancelled. The losing replica's `fetch_*` op-state observes `set_stopped` and returns its connection to the pool.

---

## 12. `materialise` / `dematerialise` — completion-signature reification

`materialise(s)` converts an arbitrary sender into a sender that always completes with `set_value`, carrying a `Signal<S>` value that records *which* of {value, error, stopped} the upstream produced and the payload thereof. `dematerialise(s')` is the inverse: a sender of `Signal<T>` is converted back to a sender that re-emits the original completion signal.

These are P2300's `materialize` / `dematerialize` operators, surfaced under project naming and with two project-specific niceties:

1. The `Signal<S>` type is a typed `std::variant` (no `std::any`), so consumers can `std::visit` exhaustively.
2. `dematerialise` validates at compile time that the input value type is exactly `Signal<T>` for some `T` whose `Signal<T>` matches the materialised sender's type.

### 12.1 Form

```cpp
namespace Mashiro::Async {

    template<class T, class E = std::exception_ptr>
    using Signal = std::variant<
        SignalValue<T>,        // wraps T (or std::tuple<Ts...> for multi-arg)
        SignalError<E>,
        SignalStopped
    >;

    template<stdexec::sender S>
    constexpr auto materialise(S&& s) -> MaterialiseSender<std::decay_t<S>>;

    template<stdexec::sender S>
    constexpr auto dematerialise(S&& s) -> DematerialiseSender<std::decay_t<S>>;
}
```

### 12.2 Completion signatures

```cpp
template<class S, class Env>
struct stdexec::completion_signatures_of_t<MaterialiseSender<S>, Env> {
    using type = completion_signatures<
        set_value_t(Signal<value_t<S, Env>, error_t<S, Env>>)
    >;
    // No error path, no stopped path. The whole signal collapses into a value.
};

template<class S, class Env>
struct stdexec::completion_signatures_of_t<DematerialiseSender<S>, Env> {
    using type = /* derived from the Signal<T, E> alternative parameters */;
};
```

### 12.3 Op-state shape

`MaterialiseOp` is a forwarding op-state: it captures upstream's completion path through a custom receiver whose `set_value` / `set_error` / `set_stopped` all funnel into `recv.set_value(Signal{...})`. Single bool `done` to short-circuit double-completion under stop.

`DematerialiseOp` is a `let_value`-style op-state: receives `Signal<T, E>`, dispatches via `std::visit` into one of three completion paths.

### 12.4 Allocation behaviour

Zero allocation in both.

### 12.5 Cancellation checklist (combined)

| Step | Materialise | Dematerialise |
|------|-------------|---------------|
| 1. Token acquisition | `start` | `start` |
| 2. Callback installation | Forwards parent stop-token to upstream | Forwards parent stop-token to upstream |
| 3. Callback body | Forwards | Forwards |
| 4. Race with completion | `done` flag | `done` flag |
| 5. Source of `set_stopped` | Emitted as `Signal{SignalStopped{}}` (a value!) — never as an actual `set_stopped` | Emitted when `Signal{SignalStopped{}}` is received |
| 6. Stop signature | No (`materialise` swallows stopped into a value) | Yes |

This is the single most subtle behaviour in L3: **`materialise` does not propagate `set_stopped` upward**. That is the whole point — it reifies the signal as a value so the consumer can do something other than the default propagate-and-die. The closest analogue in stdexec is `stopped_as_optional`, which is a special case of `materialise` over a one-arg sender.

### 12.6 Worked example

```cpp
auto inspected = unreliable_sender()
               | Async::materialise()
               | Async::then([](auto sig){
                     std::visit(overload{
                         [](SignalValue<int> v){ log("value", v.value); },
                         [](SignalError<std::exception_ptr> e){ log("error"); },
                         [](SignalStopped){ log("stopped"); },
                     }, sig);
                     return sig;
                 })
               | Async::dematerialise();
```

Useful for tracing: the `then` observes every completion signal exactly once, with no risk of throwing or losing the original completion. After `dematerialise`, downstream sees the original signal indistinguishable from the un-materialised version.

---

## 13. Adaptor / Domain Interaction

Each L2 backend (Subagent B's `02-backends.md`) registers a stdexec domain. Domains may install `transform_sender` rewrites that match L3 adaptor *tag types* and lower them into backend-native equivalents. This section pins which adaptors are rewrite candidates, which are pass-through, and the rule for deciding.

### 13.1 Rewrite-candidate matrix

| Adaptor | Inline | StaticPool | Tbb | Platform | Io |
|---------|--------|------------|-----|----------|-----|
| `bulk` | sequential loop (default expansion) | `schedule_bulk` (native) | `tbb::parallel_for` (rewrite) | invalid (`Platform` is single-threaded; static_assert) | invalid (Io is concurrent, not parallel) |
| `batch` | pass-through | pass-through | pass-through | timer = Platform timer | timer = `Io::async_timer` (rewrite when upstream is `Io`) |
| `debounce` | pass-through | pass-through | pass-through | timer = Platform timer | timer = `Io::async_timer` |
| `throttle` | pass-through | pass-through | pass-through | timer = Platform timer | timer = `Io::async_timer` |
| `retry` | pass-through | pass-through | pass-through | pass-through | pass-through |
| `timeout` | pass-through | pass-through | pass-through | timer = Platform timer | timer = `Io::async_timer` |
| `race` | pass-through | pass-through | pass-through | pass-through | pass-through |
| `materialise` | pass-through (always) | pass-through | pass-through | pass-through | pass-through |
| `dematerialise` | pass-through (always) | pass-through | pass-through | pass-through | pass-through |

### 13.2 Rewrite-decision rule

A backend's `transform_sender` is allowed to rewrite an L3 adaptor expression iff *all four* hold:

1. **Tag match.** The expression's outermost sender wraps an `Adaptor::*T` tag for which the backend has registered a rewrite.
2. **Capability fit.** The rewrite uses backend features the backend genuinely offers (e.g. `Tbb::parallel_for` is allowed only because `Tbb` models `BulkScheduler`).
3. **Signature equivalence.** The rewritten sender's `completion_signatures_of_t` is **structurally equal** to the original's. Adding `set_error_t` is forbidden; the rewriter is a *behaviour-preserving* transform.
4. **Cancellation parity.** The rewritten sender registers an `inplace_stop_callback` whose body is at least as responsive as the original's. (Native backends are usually *more* responsive — `IORING_OP_ASYNC_CANCEL` beats a software timer.)

### 13.3 Pass-through senders

Adaptors marked "pass-through" in §13.1 are not rewritten by the backend. They are still observable by domains for purposes other than rewrite (e.g. diagnostics, scheduling hints). Pass-through means the framework's default expansion runs unchanged.

### 13.4 Cross-backend pipelines

When a pipeline crosses backends (`continues_on(plat, fetch | retry(Fixed{...})) | continues_on(io, ...)`), each adaptor is rewritten by *its* backend's domain — the one that owns the upstream completion. The adaptor that straddles two backends (e.g. `timeout` whose upstream is on `Tbb` but whose timer needs `Io`) defers to the upstream's domain first; that domain may delegate to a sibling domain via the `register_subdomain` mechanism that Subagent B specifies.

### 13.5 User adaptors

L7 (`07-extension.md`) describes the user-extension contract for new sender adaptors. The ABI obligation for L3 itself: every framework adaptor exposes its tag type publicly so that user-written domains can also rewrite the framework's adaptors when running over user backends.

---

## 14. Environment-rewriter adaptors (v0.2)

Per `09-synthesis.md` §2.16, two environment-rewriting adaptors live in L3 and are used by
cross-cutting code (E's `08-cross-cutting.md`) and by `batch` / `bulk` callers that want to
supply a non-default allocator or a non-default stop-source without touching their pipeline
shape. Both are pure rewrites of the receiver's environment via stdexec's `read_env` /
`write_env` mechanism. Neither allocates; neither owns op-state state.

### 14.1 `with_allocator(s, alloc)`

```cpp
namespace Mashiro::Async {

    // Replace the allocator query in the receiver's environment with `alloc`.
    // The upstream sender sees `get_allocator(env) == alloc` for the duration of its
    // operation; outer scopes are unaffected. The result type's completion signatures
    // are identical to S's — this is a pure environment rewrite.
    template<stdexec::sender S, class Allocator>
    [[nodiscard]] auto with_allocator(S&& s, Allocator alloc);

    // Pipeable form.
    template<class Allocator>
    [[nodiscard]] constexpr auto with_allocator(Allocator alloc);

}  // namespace Mashiro::Async
```

Use site:

```cpp
std::pmr::monotonic_buffer_resource arena{8 * 1024};
std::pmr::polymorphic_allocator<std::byte> alloc{&arena};

auto pipeline = sensor.events()
              | Async::batch(50ms, 1024)
              | Async::with_allocator(alloc);     // batch's pmr::vector now lives in `arena`
```

Op-state shape: a single member that captures `alloc` by value plus the upstream op-state,
plus a custom receiver that overrides `get_allocator` on its environment. Zero allocation;
the cancellation audit (§4.7) is vacuous because no external state is owned.

### 14.2 `with_stop_source(s, src)`

```cpp
namespace Mashiro::Async {

    // Replace the stop-source query in the receiver's environment with `src`.
    // The upstream sender sees `get_stop_token(env) == src.get_token()` and treats `src`
    // as the source of truth for cancellation. Used by cross-cutting code that wants to
    // graft an externally-owned cancellation source onto a sender without restructuring
    // the surrounding pipeline (e.g. a per-request stop-source inside a server scope).
    template<stdexec::sender S>
    [[nodiscard]] auto with_stop_source(S&& s, stdexec::inplace_stop_source& src);

    // Pipeable form.
    [[nodiscard]] constexpr auto with_stop_source(stdexec::inplace_stop_source& src);

}  // namespace Mashiro::Async
```

Use site:

```cpp
stdexec::inplace_stop_source per_request_src;
auto pipeline = http.handle(req)
              | Async::with_stop_source(per_request_src);
// Calling per_request_src.request_stop() cancels the pipeline; the outer (scope-level)
// stop-source still applies through the normal nested-token chain.
```

Lifetime rule: `src` must outlive the op-state. The adaptor does not take ownership; passing
a temporary is rejected at compile time via a `decltype((src))` SFINAE that requires an
lvalue reference.

Op-state shape: one pointer to `src`, plus the upstream op-state, plus a custom receiver
that overrides `get_stop_token` on its environment. Zero allocation. Cancellation is
external — propagation is purely the upstream's responsibility, so the audit is vacuous.

### 14.3 Why these are L3-owned

Both adaptors are environment rewriters, not algorithmic adaptors — they do not produce
new sender shapes, they do not introduce timers, they do not fan-out. They live at L3
because:

1. they share L3's tag-typed pipeable-closure machinery (`Mashiro::Async::Adaptor::*`);
2. they are referenced by L3 algorithmic adaptors (`batch`, `bulk` expansion path) via the
   allocator query and by L5 / cross-cutting via the stop-source query; lower-layer placement
   would force L1 or L2 to depend on either, violating the layer map;
3. they parallel stdexec's own environment-rewriter `read_env` / `write_env` patterns and
   benefit from the same `transform_sender` interaction L3 already documents (§13).

The bridge to L0's `Foundations::union_signatures` is unchanged — environment rewriters are
signature-preserving and do not invoke the helper.

---

## 15. Reflection-Introspectability

Every L3 adaptor sender type carries the L1 annotations Subagent A's `Traits::*_v` queries expect:

```cpp
namespace Mashiro::Async::Adaptor {

    [[=Async::Cancellable]]
    template<class S, class N, class Fn>
    struct BulkSender { /*...*/ };

    [[=Async::Cancellable, =Async::Allocates{Where::Output}]]
    template<class S, class W, class N>
    struct BatchSender { /*...*/ };

    // ... and so on for each adaptor.
}
```

`Traits::IsCancellable_v<S>` returns true for every L3 adaptor. `Traits::AllocatesIn_v<BatchSender>` returns `Where::Output`. `Traits::IsBulkExpansion_v<S>` is a special L3-introduced trait that is true on `BulkSender` and false elsewhere; it is the only L3 trait that L4 / L6 query. (Coroutine task types — L4 — are introspectable through the same mechanism; see `04-coroutine-tasks.md`.)

---

## 16. Status

- v0.1 — drafted 2026-06-15. Pins the adaptor surface, op-state shapes, completion signatures, cancellation contracts, and the adaptor / domain interaction matrix. Worked examples are illustrative; full implementations live in `Mashiro/src/Async/Adaptor/` and ship after L2 backends are landed.
- **v0.2 (this document)** — incorporates synthesis-pass adjudications (`09-synthesis.md` §2.3, §2.16, §2.26, §2.30). §4.7 adds the cancellation audit helper; §4.8 documents L3-local signature helpers (`detail::propagate_errors`, `detail::union_errors`); §6 switches `batch`'s value type to `std::pmr::vector<T>` with a forward-compatibility note; §9 / §11 use the renamed L3-local helpers; §14 (new) adds `with_allocator` and `with_stop_source`; §15 is the renumbered Reflection-Introspectability section.
- v1.0: post-implementation revision once `Mashiro/demos/Async/` exercises every adaptor end-to-end, including the cross-backend pipeline.




