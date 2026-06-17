# Mashiro Async Framework — L4 Coroutine Task Types

**Status:** Draft v0.2 (layer L4, written under the umbrella of `00-overview.md` v0.2)
**Date:** 2026-06-15 (v0.1) · 2026-06-16 (v0.2 synthesis pass)
**Author:** Mashiro Engine team — Subagent C
**Scope:** `Mashiro::Async::Coro` namespace, headers under `Mashiro/include/Mashiro/Async/Coro/`. Composes with L0/L1 (Subagent A — `01-foundations.md`), L2 backends (Subagent B — `02-backends.md`), and L3 adaptors (`03-adaptors.md`). Forward-references L5 (`Scope`, `Nursery` — Subagent D's `05-structured.md`) for the `Job` lifetime contract.

### Revision history

- **v0.1** — initial draft. Defines `Task<T>`, `Stream<T>`, `Job`, the awaitable bridge (plain stdexec senders, custom user awaitables, `std::future` boundary, platform awaiters). Closes with the three-way `Manager::OpenFile` example.
- **v0.2** — synthesis pass (see `09-synthesis.md`). `stream::from_channel` renamed to `stream::from_queue` against a new `Concepts::AsyncQueue<T>` concept (§5.5); `Coro::stopped_signal` exception type added (§6.5b) for the unwind-shape of `set_stopped` inside coroutine bodies; `MASHIRO_FOR_CO_AWAIT(name, stream)` macro fallback added (§5.4b) for compilers that do not yet implement P2300/P3552's `for co_await`; `from_future` annotated `[[deprecated("migration boundary — see 07-extension.md §7.5")]]` (§7.3) to mark it explicitly as a migration boundary, not a steady-state primitive.

---

## 1. Purpose

The Mashiro async framework treats C++20 coroutines as *one of three peer task vocabularies* — alongside sender expressions (L0–L3) and structured patterns (L5–L6). L4 fixes the rules of that vocabulary:

- A **coroutine task** is a unit of asynchronous code whose author wants `co_await` and `co_return` instead of `then` / `let_value` chains.
- A coroutine task is reflection-introspectable: its scheduler-affinity, its completion signature, its allocation policy, and its cancellation behaviour are all queryable through `Traits::*_v` queries from L1.
- Coroutine tasks **never escape** their structural owner: a `Task<T>` is consumed by exactly one awaiter; a `Stream<T>` is consumed by exactly one `for co_await`; a `Job` is owned by exactly one `Scope` (L5).

L4 introduces three task types and one bridge:

| Type     | Models                                  | Built on                          |
|----------|-----------------------------------------|-----------------------------------|
| `Task<T>`   | Linear-with-branches "story"         | `exec::task<T>` (P3552) + P3941   |
| `Stream<T>` | Pull-driven async range              | sender-of-optional + `for co_await` integration |
| `Job`       | Detached task with structural lifetime | `Task<void>` + `Scope` (L5) ownership |
| Awaitable bridge | rules for awaiting senders / futures / platform awaiters | language-level `operator co_await` plumbing |

L4 introduces no new vocabulary outside these four constructs and the small number of helpers they require.

---

## 2. Goals

- Provide a coroutine task type whose body always resumes on a fixed scheduler (P3941 scheduler-affinity), with the affinity baked into the type.
- Provide a coroutine stream type whose `for co_await` integration matches the iteration shape of `std::generator` while remaining a sender-of-optional underneath.
- Provide `Job` as the canonical detached-but-structurally-owned task type (`Task<void>` does not express ownership; `Job` does).
- Bridge plain stdexec senders, user-defined awaitables, `std::future`, and platform-specific awaiters (e.g. Vulkan fence-await) into the coroutine vocabulary uniformly.
- Be reflection-introspectable: every coroutine task carries the L1 annotations `Traits::*_v` queries expect.

## 3. Non-Goals

- A coroutine *executor* — schedulers are L2, not L4.
- A bespoke `coroutine_traits` family — we use stdexec's coroutine integration (`with_awaitable_senders`, `as_awaitable`).
- A "task pool" or "fiber" abstraction. Coroutine frames are heap-allocated when HALO does not apply; that is documented at the type, not hidden.
- Cancellation tokens distinct from `inplace_stop_token`. Stop-tokens flow through coroutine task environments exactly as they flow through senders.

---

## 4. `Task<T>` — sequential narrative with branching

### 4.1 Definition

```cpp
namespace Mashiro::Async::Coro {

    // Affine task: resumes on the bound scheduler after every co_await.
    template<class T, Concepts::Scheduler S>
    using Task = exec::task<T, exec::__task::__default_task_context<S>>;

    // Convenience aliases.
    namespace Tasks {
        using Inline   = Task<void, Backend::Inline::scheduler>;
        using Pool     = Task<void, Backend::StaticPool::scheduler>;
        using Tbb      = Task<void, Backend::Tbb::scheduler>;
        using Platform = Task<void, Backend::Platform::scheduler>;
        using Io       = Task<void, Backend::Io::scheduler>;
    }
}
```

`Task<T>` is **a typedef** for `exec::task<T>` parameterised by the scheduler. It is not a wrapper, not a derived class. The whole reason coroutine-tasks-with-affinity work is P3941: the task's promise type stores a scheduler and `final_suspend` schedules onto it.

### 4.2 Scheduler-affinity contract (P3941)

When a `Task<T, S>`'s body executes `co_await sender`, the resumption is dispatched as follows:

1. The sender is started; it completes on its *own* completion scheduler (which may be different from `S`).
2. The promise's `final_awaiter` observes that the completion is not on `S` and inserts a `continues_on(S)` transition.
3. The coroutine resumes on `S`.

This makes `Task<T, Platform::scheduler>` **always resume on the platform thread** — which is the whole reason `Mashiro::Platform::Task<T>` (defined in `2026-06-11-platform-thread-infrastructure-design.md`) is a typedef for it. Manager bodies do not need to write `co_await schedule(plat)` after every cross-thread call; the affinity does that for them.

### 4.3 `co_await` of senders, tasks, and streams

The promise type satisfies stdexec's `with_awaitable_senders` mixin. That gives `co_await` of the following without further user code:

- **Sender:** `co_await s` connects `s` to a coroutine-receiver, starts it, suspends the coroutine, and resumes with the value (or throws on error, or unwinds on stopped).
- **Task<U>:** `co_await other_task` is sender-of-task; same dispatch as above.
- **Stream<U>:** `co_await stream.next()` returns the next element. The `for co_await (auto x : stream)` form is desugared (§5.4) into a loop over `stream.next()`.

Stop-token propagation is automatic: the awaiter retrieves the parent's stop-token from the promise's environment and forwards it to the awaited sender's receiver environment.

### 4.4 Frame allocation rules

A `Task<T, S>` invocation allocates **one heap frame** per call when HALO does not apply. The allocator is queried from the task's environment via `get_allocator(env)`; the default is `std::allocator<std::byte>`. The allocation is annotated:

```cpp
[[=Async::Allocates{Where::Frame}]]
template<class T, Concepts::Scheduler S>
class Task /* = exec::task<T, ...> */ ;
```

`Traits::AllocatesIn_v<Task<T,S>> == Where::Frame`.

HALO opportunities are documented per call site, not promised by the framework. The compiler elides the allocation when:

- The task's sole consumer is a non-escaping `co_await` (the call site can size the frame).
- The task has no captures that escape (no `co_return` of references to the frame).

Authors who care about HALO success annotate the call site with `[[clang::coro_await_elidable]]` (project convention) and verify with `Diagnostics::check_halo()` — but this is a quality-of-implementation concern, not a correctness one.

### 4.5 Completion signatures

```cpp
template<class T, class S>
struct stdexec::completion_signatures_of_t<Task<T, S>, Env> {
    using type = stdexec::completion_signatures<
        stdexec::set_value_t(T),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()
    >;
};
```

`set_error_t(std::exception_ptr)` is unconditional (the body may throw). `set_stopped_t()` is unconditional (the body may observe its stop-token). `T` may be `void`; the value signature is then `set_value_t()`.

### 4.6 Cancellation

A `Task<T, S>` that is `co_await`ed inherits its caller's stop-token. Inside the body:

- `co_await stop_token()` retrieves the inplace token and lets the user inspect / branch on it.
- A child `co_await sender` automatically forwards the token to the sender's receiver environment.
- A child `co_await child_task` likewise forwards.

When stop fires, every in-flight `co_await` resolves to the stopped path; the coroutine body unwinds (destructors of locals run on the resuming thread — i.e. on `S`); `final_suspend` reports `set_stopped` to the parent awaiter.

### 4.7 Reflection annotations

```cpp
[[=Async::Cancellable, =Async::Allocates{Where::Frame}, =Async::Affine{S::backend()}]]
template<class T, Concepts::Scheduler S>
class Task { /*...*/ };
```

`Traits::AffinityOf<Task<T, S>> == S::backend()`. For `Task<T, Platform::scheduler>` this is `Backend::Platform`; for `Task<T, Tbb::scheduler>` it is `Backend::Tbb`.

### 4.8 Worked example

```cpp
Mashiro::Platform::Task<File> Manager::OpenFile(std::string_view path) {
    auto handle = co_await Backend::Io::async_open(path, O_RDONLY);  // Io thread
    auto stat   = co_await Backend::Io::async_fstat(handle);          // Io thread
    co_return File{handle, stat};                                     // resumes on Platform thread
}
```

After `co_await Io::async_open` the body resumes on the Platform thread (P3941 affinity); the same is true after `co_await Io::async_fstat`. The `co_return` runs on the Platform thread. The `File` is constructed and returned to the caller, which itself resumes on its own affinity scheduler.

---

## 5. `Stream<T>` — pull-driven async range

### 5.1 Definition

```cpp
namespace Mashiro::Async::Coro {

    // Stream<T> is a sender-of-optional, plus a coroutine-friendly facade.
    template<class T>
    class Stream {
    public:
        using value_type = T;

        // sender-of-optional: completes with optional<T>; nullopt = end-of-stream.
        auto next() -> stdexec::sender_of<stdexec::set_value_t(std::optional<T>)> auto;

        // Coroutine-friendly iteration support (see §5.4).
        auto begin() -> StreamIterator<Stream>;
        auto end()   -> StreamSentinel;

        // Stop integration — fires when the producer is stopped.
        auto stop_token() const -> stdexec::inplace_stop_token;
    };
}
```

The underlying *transport* is implementation-defined — typically a SPSC queue inside the producer plus a `next()` sender that completes when an element is available. The framework provides three constructors (§5.5).

### 5.2 Completion signatures

```cpp
// Per-element pull (one call to next()).
using next_signatures = stdexec::completion_signatures<
    stdexec::set_value_t(std::optional<T>),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()
>;
```

`std::optional<T>` is the end-of-stream marker (a `set_value(std::nullopt)` means "no more elements, ever"). Errors propagate from the underlying source. Stop is observed when the consumer's stop-token fires *or* when the producer itself stops.

### 5.3 Back-pressure semantics

`Stream<T>` is **pull-driven**: the producer does not push elements until the consumer's `next()` is awaiting. The transport between producer and consumer is conceptually a one-slot rendezvous; concrete implementations may buffer (e.g. `from_channel` (§5.5) buffers up to its channel capacity).

There is no implicit back-pressure policy. A consumer that pulls slower than a producer can fill *just back-pressures the producer*. Lossy buffering (drop-oldest, drop-newest, latest-only) is a *separate combinator* layered atop `Stream<T>` — see L6's `Reactive` patterns; L4 itself does not have a buffer policy.

### 5.4 `for co_await` integration

The expression `for co_await (auto x : stream) BODY;` desugars to:

```cpp
{
    auto&& __rng = stream;
    auto __it    = __rng.begin();   // returns awaiter that yields iterator after first next()
    auto __end   = __rng.end();
    while (co_await (__it != __end)) {
        auto&& x = *__it;
        BODY;
        co_await ++__it;
    }
}
```

Where `*__it` is the most recently fetched value, `++__it` calls `next()` on the underlying sender and `co_await`s the result, and `__it != __end` is a sender that completes with `bool` (true while more, false on `nullopt`). This matches the language-feature shape proposed for C++26 (`for co_await`) and is implemented today in `Mashiro/include/Mashiro/Async/Coro/Stream.h` via `co_await stream.next()` desugaring.

### 5.4b `MASHIRO_FOR_CO_AWAIT` — macro fallback (v0.2)

C++26's `for co_await` is not implemented by every toolchain Mashiro must support (in particular, clang-p2996's reflection branch lags general coroutine work). The framework therefore publishes a macro fallback whose generated code is identical to §5.4's desugaring:

```cpp
// In <Mashiro/Async/Coro/Stream.h>:
#define MASHIRO_FOR_CO_AWAIT(NAME, STREAM)                                 \
    if (auto&& __mashiro_rng = (STREAM); true)                             \
        for (auto __mashiro_it = __mashiro_rng.begin(),                    \
                  __mashiro_end = __mashiro_rng.end();                     \
             co_await (__mashiro_it != __mashiro_end);                     \
             co_await ++__mashiro_it)                                      \
            if (auto&& NAME = *__mashiro_it; true)
```

Use:

```cpp
MASHIRO_FOR_CO_AWAIT(chunk, chunks) {
    process(chunk);
}
```

The macro form is **identical in semantics** to the language form — same cancellation behaviour, same scheduler-affinity, same allocation footprint. The framework's own headers use the language form where the toolchain supports it (gated on `__cpp_impl_coroutine >= 202404L` plus an MSVC version probe) and the macro form otherwise; user code may pick whichever reads better at the call site. The macro is intended to remain in the headers indefinitely — even after every supported toolchain implements the language form — because deeply-nested generic code occasionally prefers a single-token loop construct (the macro), while top-level demo code prefers the language form.

### 5.5 Stream factories

```cpp
namespace Mashiro::Async::Coro::stream {

    // Coroutine generator: body co_yields elements, framework lifts to Stream.
    template<class T, class Fn>
    auto generate(Fn body) -> Stream<T>;

    // Pull from any AsyncQueue<T> — the framework-published concept (see §5.5b).
    // Replaces v0.1's narrower from_channel(Channel<T>&) signature.
    template<Concepts::AsyncQueue Q>
    auto from_queue(Q& q) -> Stream<typename Q::value_type>;

    // Periodic emission on a timer.
    auto interval(std::chrono::nanoseconds period, Concepts::Scheduler auto sched)
        -> Stream<std::chrono::steady_clock::time_point>;
}
```

`generate` is the most common: the user writes a coroutine body that `co_yield`s. The result is a `Stream<T>` whose `next()` sender resumes the body and waits for the next `co_yield`.

### 5.5b `Concepts::AsyncQueue<T>` — pull source concept (v0.2)

`from_queue` is parameterised by a concept rather than a concrete `Channel<T>` type so that any bounded-async-queue shape — `Channel<T>`, an MPSC queue published by L0 (`Mashiro::MpscQueue<T>`), a user-defined ring buffer with a sender-returning `pop()` — can feed a `Stream`. The concept is:

```cpp
namespace Mashiro::Async::Concepts {
    template<class Q>
    concept AsyncQueue = requires(Q& q) {
        typename Q::value_type;
        { q.pop() } -> stdexec::sender_of<stdexec::set_value_t(std::optional<typename Q::value_type>)>;
        { q.close() } -> std::same_as<void>;
    };
}
```

Semantics:

- `q.pop()` returns a sender that completes with `set_value(optional<value_type>{...})` when an element is available, or `set_value(std::nullopt)` after the queue is closed *and* empty.
- `q.close()` is idempotent; after `close()`, any in-flight `pop()` may still complete with a buffered value, and subsequent `pop()`s yield `nullopt`.
- The queue is not required to be MPSC, MPMC, or any particular topology; `from_queue` only consumes from a single consumer side and treats the queue opaquely.

`from_queue` translates each `pop()` into a `next()` of the resulting `Stream<T>`. When the consumer's stop-token fires, `from_queue` calls `q.close()` (via an `inplace_stop_callback`) and then forwards the stopped signal to the consumer.

**Migration note:** v0.1 named this factory `from_channel(Channel<T>&)`. The rename is mechanical (concept generalises the parameter); `Channel<T>` is expected to satisfy `AsyncQueue` and so existing call sites compile unchanged after the rename. A deprecation shim `from_channel<T>(Channel<T>&) → from_queue(ch)` is published in L7 (`07-extension.md` §7.5).

### 5.6 Stop-token integration

A `Stream<T>` carries its own `inplace_stop_source`; `stream.stop_token()` returns the corresponding token. When a consumer's outer stop-token fires while `co_await stream.next()` is outstanding, the bridge:

1. Forwards the stop to the producer (via the producer's child stop-source).
2. Completes the outstanding `next()` with `set_stopped()`.
3. Subsequent `next()` calls also complete with `set_stopped()`.

Producers that hold external resources (file descriptors, sockets) register an `inplace_stop_callback` on the stream's stop-token at construction; that callback closes the resource and reaches `set_stopped()` to any pending consumer.

### 5.7 Reflection annotations

```cpp
[[=Async::Cancellable, =Async::Allocates{Where::OpState}]]
template<class T>
class Stream { /*...*/ };
```

The `OpState` allocation tag covers the producer's state. Producers built on `generate` additionally allocate a coroutine frame (annotated through composition of the `generate`-internal task type).

### 5.8 Worked example

```cpp
auto chunks = Manager::OpenFile(path)
            | Async::let_value([](File& f) {
                  return stream::generate<Chunk>([&]() -> Mashiro::Platform::Task<void> {
                      while (auto buf = co_await f.next_chunk()) {
                          co_yield Chunk{*buf};
                      }
                  });
              });

for co_await (auto& chunk : chunks) {
    process(chunk);
}
```

The whole chain is cancellation-correct: stopping the surrounding scope propagates to `chunks`, which propagates to the generator's body, which observes the stop on its next `co_await f.next_chunk()` and unwinds.

---

## 6. `Job` — detached task with structural lifetime

### 6.1 Why `Job` exists

A bare `Task<void>` is **not detachable** — its destruction without `co_await` is a programmer error (the framework asserts in debug, leaks in release). Detaching is sometimes the right thing: a fire-and-forget telemetry write, a background prefetch, a UI animation that runs until the parent scope ends. For those, the right type is **`Job`**.

`Job` differs from `Task<void>` in three ways:

1. **Lifetime is owned by a `Scope`** (L5 — Subagent D's `05-structured.md`). `Scope::spawn(job)` enrolls the job in the scope's `counting_scope`; `scope.on_empty()` settles after every enrolled job completes.
2. **Errors are routed to the scope's error policy** (default: terminate; supervised: log; ...). A detached `Task<void>` that throws has no way to surface; a `Job` always does.
3. **The job's stop-token is a child of the scope's stop-token.** Cancelling the scope cancels every enrolled job.

### 6.2 Definition

```cpp
namespace Mashiro::Async::Coro {

    template<Concepts::Scheduler S>
    class Job {
    public:
        // Constructed only via Scope::spawn(); not user-instantiable directly.
        Job(Job&&) = default;
        Job& operator=(Job&&) = default;
        ~Job();   // observes that the job has settled

        // Inspect — useful for diagnostics, never for control flow.
        bool is_done() const noexcept;

    private:
        // Constructed by Scope::spawn(task) — see L5.
        explicit Job(Detail::JobImpl* impl);
        Detail::JobImpl* impl_;
    };
}
```

A `Job` is **a handle**, not the running coroutine itself; the coroutine is owned by the `Scope`. The handle is for observation (`is_done()`) and for use as the type of a `Scope::spawn` return value. Most users never name it.

### 6.3 Cancellation

When the parent `Scope`'s stop-token fires:

1. Every enrolled `Job`'s stop-token fires.
2. Each job's coroutine body observes the stop on its next `co_await sender`.
3. The body unwinds; `final_suspend` reports completion to the scope.
4. The scope's `counting_scope` decrements its in-flight count; when zero, `on_empty()` settles.

### 6.4 What `Job` expresses that `Task<void>` cannot

- "I have launched this work and I will not personally await it, but the surrounding region of code is guaranteed not to exit until it completes."
- "If this work fails, the failure is observable to the surrounding region (the scope)."
- "If the surrounding region is cancelled, this work is cancelled."

These three properties are exactly the structured-concurrency contract. `Task<void>` *is* a coroutine; it has none of these properties on its own. `Job` *binds* a coroutine to a scope, which gives all three.

### 6.5 Reflection annotations

```cpp
[[=Async::Cancellable, =Async::Allocates{Where::Frame}, =Async::Detached]]
template<Concepts::Scheduler S>
class Job { /*...*/ };
```

`Async::Detached` is an L4-introduced annotation; `Traits::IsDetached_v<T>` is true on `Job` and false on `Task<T>`. Subagent D's L5 spec uses this to validate that `Scope::spawn` is given either a sender or a task that becomes a job.

### 6.5b `Coro::stopped_signal` — the unwind exception (v0.2)

The synthesis pass (`09-synthesis.md` §2.15) pinned the *shape* of `set_stopped` inside a coroutine body. The shape is: an exception of type `Coro::stopped_signal` is thrown from the awaiter, propagates through `co_await` like any other exception (destructors of locals run, `catch` blocks may observe it), and is converted back to `set_stopped()` at `final_suspend` if it reaches the promise unrethrown.

```cpp
namespace Mashiro::Async::Coro {

    // Thrown from co_await's resume path when the awaited sender completes via
    // set_stopped. Reaches the promise via final_suspend and is reported as
    // set_stopped to the parent awaiter.
    struct stopped_signal {
        // Empty by design: stopped is unparameterised. Users who need a
        // structured cancellation reason wrap stopped_signal in their own
        // exception type and pre-empt the stop via inplace_stop_callback.
    };
}
```

Three rules govern its use:

1. **Catching `stopped_signal` is allowed**, but rethrowing or letting it propagate is the *default*. Catching it to perform cleanup (closing a file, releasing a GPU resource) before rethrowing is the common pattern; catching to convert it to a value (`return std::nullopt`) is allowed but unusual.
2. **`catch (...)` catches `stopped_signal`.** Authors of generic catch-all error logging must check for `stopped_signal` first and rethrow if caught — logging "an exception happened" when the user *asked* to stop is a defect. The framework's `with_logger` adaptor (`08-cross-cutting.md` §3.4) excludes `stopped_signal` from its log output by default.
3. **`stopped_signal` does not derive from `std::exception`.** This is deliberate. `std::exception` is reserved for errors; cancellation is not an error. Code that catches `std::exception` to log/route errors does *not* catch `stopped_signal` and therefore does *not* misclassify cancellation.

The exception is unwound on the *resuming* thread — i.e. on the task's affinity scheduler — so destructors run with the same scheduler affinity as the rest of the body.

### 6.6 Worked example (forward-references L5)

```cpp
co_await Async::with_nursery([&](Nursery& n) {
    n.spawn(prefetch(asset_a));    // Job
    n.spawn(prefetch(asset_b));    // Job
    return load_index();           // sender; awaited by the nursery exit
});
```

`prefetch(...)` returns a `Task<void, ...>`; `Nursery::spawn(task)` constructs a `Job` for it and enrols it in the nursery's scope. The `with_nursery` block does not exit until both jobs settle (and `load_index()` completes).

---

## 7. Awaitable bridge

The framework supports awaiting four kinds of awaitables inside any `Task<T>` or `Job` body. This section pins the rules.

### 7.1 Plain stdexec senders

```cpp
auto v = co_await some_sender;
```

Dispatched via `stdexec::as_awaitable(sender, env)` from the promise. The receiver constructed by `as_awaitable` forwards completion to the coroutine, throws on `set_error`, and unwinds on `set_stopped`. Stop-tokens propagate from the promise's environment.

### 7.2 User awaitables

A type satisfying `stdexec::__awaitable` (i.e. one that has `await_ready` / `await_suspend` / `await_resume`) is awaited natively. The promise's `await_transform` does not wrap user awaitables — they are consumed by the language as-is.

User awaitables are **not** sender-of-T; they do not carry completion signatures. They are appropriate for low-level awaiters (e.g. wrapping a hardware fence) where sender machinery is unwanted overhead. The cost: user awaitables do not participate in stop-token propagation unless the awaitable itself queries the promise's environment.

### 7.3 `std::future` (boundary only — deprecated v0.2)

```cpp
// In <Mashiro/Async/from_future.h>:
template<class T>
[[deprecated("migration boundary — see 07-extension.md §7.5")]]
auto from_future(std::future<T>&& fut) -> stdexec::sender_of<stdexec::set_value_t(T)> auto;

// Usage (call sites surface a deprecation diagnostic — intentional):
auto x = co_await Async::from_future(std::move(fut));
```

`from_future` is an explicit boundary adaptor — not implicit. Awaiting a `std::future` directly is rejected at compile time (the awaitable bridge does not include `std::future`). The reason: `std::future` does not participate in stop-token propagation, has its own threading model, and silently introduces blocking on `std::launch::deferred`. Forcing the user through `from_future` makes the boundary visible.

`from_future` itself spawns a small `std::jthread` that polls the future and completes a sender; this is acknowledged as the wrong shape for hot-path interop and is intended only for migration from legacy code. The `[[deprecated]]` attribute is **the marker, not the death warrant** — `from_future` will remain in the headers for the foreseeable future so legacy code keeps compiling, but every call site surfaces a diagnostic so authors are nudged toward the L7 migration path (Subagent E's `07-extension.md` §7.5) at every recompile. New code MUST NOT use `from_future`; reviewers reject patches that introduce new call sites.

### 7.4 Platform-specific awaiters (Vulkan fences, etc.)

Some platforms ship their own awaiter shapes:

- Vulkan fence-await on a graphics queue.
- Win32 `WaitForSingleObject` on a `HANDLE`.
- Linux `epoll`-based one-off readiness on a `fd`.

These are exposed as **typed awaitables** in the relevant subsystem header (e.g. `Mashiro::Vulkan::Fence::Awaiter`) and consumed via `co_await fence.wait()`. The awaiter's `await_suspend` registers the coroutine handle with the platform's wait mechanism; the coroutine resumes when the platform fires.

To participate in stop-token propagation, a platform awaiter must:

1. Read the parent stop-token from the promise's environment via the standard `get_stop_token(get_env(...))` pattern.
2. Register an `inplace_stop_callback` whose body cancels the platform wait (e.g. `vkCancelWait`, `CancelIoEx`) and resumes the coroutine on the stopped path.

Platform awaiters that do not participate in stop-token propagation are **not allowed in framework headers**. (Users may write their own, but the framework's quality bar requires propagation.)

### 7.5 The bridge's compile-time guarantee

A consteval check inside the promise type asserts that every awaited expression matches one of the four categories (sender, user awaitable, explicit `from_future`, platform awaiter). Anything else — most commonly, accidentally awaiting a `std::function` or a `Task<T>` that has been moved-from — is a compile error pointing at the await site.

---

## 8. Manager-call worked example — three ways

The closing demonstration. The same conceptual API, written three different ways, with a discussion of when each is appropriate.

The API: `Manager::OpenFile(path) → File`. We assume the file is opened on the Io scheduler and the result is returned on the caller's scheduler.

### 8.1 Sender expression form

```cpp
auto open_file_s(std::string_view path) {
    return Backend::Io::async_open(path, O_RDONLY)
         | Async::let_value([](FileHandle h) {
               return Backend::Io::async_fstat(h)
                    | Async::then([h](auto stat){ return File{h, stat}; });
           });
}
```

Type: `stdexec::sender_of<set_value_t(File), set_error_t(std::exception_ptr), set_stopped_t()>`. No coroutine frame allocated. The whole expression is a value; consumers `co_await` it or feed it to a sender pipeline.

**When appropriate:** when the call has no branches, will be composed into larger sender expressions, and HALO is desirable. The caller's syntax (`co_await open_file_s(p)`) is identical to the `Task<T>` form, but no frame is allocated.

### 8.2 `Task<T>` form

```cpp
Mashiro::Platform::Task<File> Manager::OpenFile(std::string_view path) {
    auto handle = co_await Backend::Io::async_open(path, O_RDONLY);
    auto stat   = co_await Backend::Io::async_fstat(handle);
    co_return File{handle, stat};
}
```

One coroutine frame allocation per call (HALO may elide). The body is linear; the type is reflection-introspectable (`Traits::AffinityOf == Backend::Platform`). Resumption after each `co_await` is on the Platform thread.

**When appropriate:** when the body has *real* sequential structure — branches (`if (stat.size > kThreshold) ...`), early returns (`if (!handle.ok) co_return ...`), exception handling (`try { ... } catch (FileError&) { ... }`). Sender expressions that try to express these are unreadable.

### 8.3 `Stream<T>` form (a different question)

```cpp
Coro::Stream<Chunk> Manager::OpenFileStream(std::string_view path) {
    return Coro::stream::generate<Chunk>([path]() -> Mashiro::Platform::Task<void> {
        auto handle = co_await Backend::Io::async_open(path, O_RDONLY);
        while (auto buf = co_await Backend::Io::async_read(handle, /*size=*/64_KiB)) {
            co_yield Chunk{*buf};
        }
        co_await Backend::Io::async_close(handle);
    });
}
```

Returns `Stream<Chunk>`. Consumer iterates with `for co_await`. Cancellation closes the file deterministically.

**When appropriate:** when the file is large and consumed iteratively — the consumer wants the *next* chunk, not the *whole* file. The shape models iteration; it would be wrong to express it as a `Task<Vector<Chunk>>` (the whole-file allocation defeats the purpose).

### 8.4 Choosing between the three

| Shape of the call | Right type |
|-------------------|------------|
| One async value, no branching, will be composed | sender expression |
| One async value, with branching / try-catch / early return | `Task<T>` |
| Sequence of values, consumer-paced | `Stream<T>` |
| Detached background work, structurally owned | `Job` (constructed via `Scope::spawn`) |

The caller does not pay for what they do not need: sender expressions allocate nothing, `Task<T>` allocates one frame, `Stream<T>` allocates one frame for the generator plus its transport. `Job` is a `Task<void>` plus scope-enrollment metadata.

---

## 9. Reflection-Introspectability

Every L4 type is queryable through Subagent A's `Traits::*_v` consteval queries:

| Type | `Traits::AffinityOf<T>` | `Traits::AllocatesIn_v<T>` | `Traits::IsCancellable_v<T>` | `Traits::IsDetached_v<T>` |
|------|-------------------------|----------------------------|------------------------------|---------------------------|
| `Task<T, S>` | `S::backend()` | `Where::Frame` | `true` | `false` |
| `Stream<T>`  | (transport-dependent) | `Where::OpState` | `true` | `false` |
| `Job`        | (scheduler bound at spawn) | `Where::Frame` | `true` | `true` |
| sender expression (`open_file_s`) | propagated from the inner-most scheduler-bearing sender | `Where::None` | `true` | `false` |

The reflection algorithm (L1 in Subagent A) walks the type, reads `[[=Async::*]]` annotations, and combines them. L4 types use only annotations defined in L1; no L4-private annotations except `Async::Detached`, which is added to L1's annotation set in `01-foundations.md`.

---

## 10. Status

- v0.1 (initial draft): drafted 2026-06-15. Pins the coroutine task vocabulary, scheduler-affinity contract, allocation rules, awaitable-bridge categories, and the three-way Manager-call demonstration.
- **v0.2 (this revision):** synthesis-pass adjustments landed 2026-06-16. `Async::Detached` confirmed in L1's annotation set (`01-foundations.md` §5.2). `stream::from_channel` → `stream::from_queue<AsyncQueue>` (§5.5 + new §5.5b). `Coro::stopped_signal` exception type defined (§6.5b). `MASHIRO_FOR_CO_AWAIT` macro fallback published (§5.4b). `from_future` marked `[[deprecated("migration boundary — see 07-extension.md §7.5")]]` (§7.3). All §7 patches from `09-synthesis.md` applied; no remaining vocabulary drift relative to Subagent A (`01-foundations.md` v0.2) and Subagent B (`02-backends.md` v0.2).
- v1.0: post-implementation revision once `Mashiro/demos/Async/` exercises every coroutine type end-to-end, including the `Job` lifetime contract under `Scope` (Subagent D's `05-structured.md` must land first).
