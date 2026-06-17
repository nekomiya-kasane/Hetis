# Mashiro Async Framework — L5 Structured Concurrency

**Status:** Draft v0.2 (layer-5 spec; instantiates async model §6.4 from `00-overview.md` v0.2)
**Date:** 2026-06-15 (v0.1) · 2026-06-16 (v0.2 synthesis pass)
**Author:** Mashiro Engine team — Subagent D
**Scope:** `Mashiro::Async::Structured` namespace; new sources under
`Mashiro/include/Mashiro/Async/Structured/` and `Mashiro/src/Async/Structured/`. Builds on L0–L4
(see `01-foundations.md`, `02-backends.md`, `03-adaptors.md`, `04-coroutine-tasks.md`). Composes
with `stdexec::counting_scope` (P3149); does not replace it.

### Revision history

- **v0.1** — initial draft. Vocabulary frozen by `00-overview.md` v0.1, §5 / §6.4 / §8.4. Defines
  `Scope`, `Nursery`, `LinkedScope`, `Supervised`, the escape rule, the scope-audit diagnostic, and
  the end-to-end cancellation flow diagram that L6 (`06-patterns.md`) and §8.4 of the umbrella
  spec depend on. Aliases stdexec primitives where the addition is purely tagging or ergonomics;
  reaches for new primitives only where the structured-shutdown semantic that the umbrella spec's
  §6.4 demands is not directly expressible in stdexec today.
- **v0.2** — synthesis pass (see `09-synthesis.md` §2.18, §7). No structural changes to L5; the
  patch list is one explicit reaffirmation: `Job` (defined by Subagent C in `04-coroutine-tasks.md`
  §6.2) **is a sender**, so `Scope::spawn(Job)` and `Nursery::spawn(Job)` are accepted through the
  standard `template<stdexec::sender S> spawn(S&&)` overload — no special-case overload, no new
  concept. The `Async::Detached` annotation (`01-foundations.md` v0.2 §5.2) is the *intent*
  marker for "this is a `Job`, not a `Task<T>`"; both satisfy the sender concept identically. The
  `ScopeTag` annotation (also added to `01-foundations.md` v0.2 §5.2) is the L1-blessed form of
  the tag value this layer attaches to every `Scope<Tag>`. See §6.1b for the explicit
  Job-as-sender clause.

---

## 1. Overview

L5 is the layer that turns "N senders running concurrently" into "N senders whose **lifetime** is
bounded by a single owner". The umbrella spec's §6.4 introduces the model — *"a region of code
that owns N child tasks and refuses to exit until all of them finish (or are cancelled)"* — and
§8.4 hands this layer four concrete artifacts to deliver: `Scope`, `Nursery`, `LinkedScope`,
`Supervised`. Each is built by composing `stdexec::counting_scope`, the L0 stop-token vocabulary,
and the L4 coroutine types; none of them invents a scheduler, a queue, or an executor.

The structural contract is one sentence: **nothing spawned in a scope may outlive the scope**.
That sentence is the *only* invariant L5 enforces. Every other property — error propagation,
cancellation cascade, supervision policy — falls out of how `counting_scope` already behaves
once we have arranged for the parent stop-token to be the child's stop-token, and for the parent
sender's completion to await the scope's settling.

L5 is intentionally thin. The reason `Scope` exists at all (rather than re-exporting
`counting_scope` directly under §5.1's L0 rules) is that the umbrella spec's diagnostics
contract (§5.4 and `08-cross-cutting.md`) demands a tagged scope: every spawned op-state must be
attributable in trace output to *which* scope it belongs to, and the scope itself must be
attributable to the layer / pattern that constructed it. That tagging plus the single-allocation
ring-buffer storage strategy is what `Mashiro::Async::Structured::Scope` adds. `Nursery`,
`LinkedScope`, `Supervised` then build on the tagged scope without any new state.

---

## 2. Goals

1. **Bounded lifetime.** Every sender spawned through L5 has a single statically-locatable
   owner. The owner's destruction (or its sender's completion) is the upper bound on every
   spawned op-state's lifetime. This is the structural-concurrency invariant that the rest of
   the framework — patterns, coroutine types, diagnostics — relies on.
2. **Structural cancellation.** A `RequestStop()` on the scope's stop-source cancels every
   spawned sender via the existing stdexec stop-callback machinery. No bespoke "iterate the
   children and call `cancel()`" code; no `std::atomic<bool> closed_`.
3. **Structural error propagation.** A child failing causes its siblings to be cancelled and
   the parent's completion to surface that error. The aggregation policy is fixed by the scope
   variant (`Nursery` = first-error-wins, `Supervised` = policy-driven).
4. **Single-allocation policy.** A `Scope` performs at most **one** heap allocation in its
   lifetime — the inline op-state ring-buffer. `Nursery::spawn(sender)` is heap-free if the
   sender's op-state size fits the inline budget. (Coroutine `Task<T>` frames are accounted at
   L4, not here.)
5. **Reflection-tagged.** Every `Scope` carries a `[[=Async::ScopeTag{name}]]`-derived static
   tag whose default is the source location at scope construction. Diagnostics (§7) consume
   this tag; production builds optimise it away.
6. **Composable.** `Scope` is a value held by reference; `Nursery` is a sender produced by the
   `with_nursery` factory; `LinkedScope` is a sub-scope created from a parent. Patterns in L6
   take a `Scope&` parameter — they never construct one implicitly.

## 3. Non-Goals

- **Static escape detection.** C++ does not give us a way to prove at compile time that a
  spawned op-state cannot outlive the scope (the op-state's address may be captured into a
  raw pointer, into a coroutine frame on a different scope, into a global). We do not attempt
  it. §7 describes the *runtime* audit; static safety is delegated to the user-extension
  contract (`07-extension.md` §6) and to the project's clang-tidy ruleset.
- **Distributed scopes.** A `Scope` is single-process. Cross-process supervision is the
  responsibility of an L7 user extension; L5 does not promise stop-token transport across
  process boundaries.
- **Priority scheduling.** A `Scope` does not order spawned senders. Order, fairness, and
  priority are scheduler concerns (L2). `Nursery` reports completion *order* in
  `with_nursery`'s value channel only when explicitly asked (`when_all_ordered`).
- **Cancellation guarantees on third-party senders.** A scope can only cancel a child whose
  op-state honours its stop-token. Senders that ignore the token (e.g. blocking syscalls
  wrapped in raw `std::thread`) are out of scope; they are L7 issues.

## 4. Constraints

- **No new vocabulary.** L5 uses only the L0 vocabulary defined in `01-foundations.md` and the
  coroutine types from `04-coroutine-tasks.md`. New names introduced here (`Scope`, `Nursery`,
  `LinkedScope`, `Supervised`, `with_nursery`, `scope_audit`) are layer-local; they do not
  redefine an existing concept.
- **No scheduler.** L5 takes a scheduler as a parameter through the spawned sender's
  environment. It does not own one.
- **Single allocation.** As §2.4. The op-state ring buffer is sized at scope construction
  from the `Async::Allocates::Where::OpState` policy (see §5.3 of the umbrella spec).
- **Cancellation through stdexec only.** Stop-token flow is the existing `inplace_stop_token`
  / `inplace_stop_callback` machinery. L5 does not introduce a new cancellation channel.
- **Domain-rewrite-friendly.** `Supervised` is implemented as a `Nursery` plus a domain
  rewrite (see §6.4), not a separate primitive. This keeps the type list short and lets a
  backend specialise supervision (e.g. TBB's failure-group semantics) without subclassing.

---

## 5. Architecture

### 5.1 Topology

```
            ┌────────────────────────────────────────┐
            │  PlatformThread / pattern / coroutine  │   parent
            │  ────────────────────────────────────  │
            │  parent stop_source                    │
            └─────────────────┬──────────────────────┘
                              │  derives stop_token
                              ▼
                  ┌───────────────────────────┐
                  │  Mashiro::Async::         │
                  │      Structured::Scope    │   thin wrapper of
                  │  ─────────────────────    │   stdexec::counting_scope
                  │  - tag (ScopeTag)         │
                  │  - inline op-state ring   │
                  │  - parent stop_token      │
                  │  - scope stop_source      │
                  └────┬─────────┬────────────┘
                       │         │   spawn(sender)  →  stop_token wired in
                       ▼         ▼
                ┌──────────┐  ┌──────────┐  ┌──────────┐
                │ child A  │  │ child B  │  │ child C  │
                │ op-state │  │ op-state │  │ op-state │
                │ rcvr.env │  │ rcvr.env │  │ rcvr.env │
                │ has stop │  │ has stop │  │ has stop │
                └──────────┘  └──────────┘  └──────────┘
```

### 5.2 Cardinality

| Type            | Per program | Per parent | Notes                                                     |
|-----------------|-------------|------------|-----------------------------------------------------------|
| `Scope`         | many        | 0..*       | One per logical owner (server, request, frame, batch).    |
| `Nursery`       | many        | 0..*       | Lifetime = the with-block; constructed on every entry.    |
| `LinkedScope`   | many        | 0..*       | Cheaper than `Scope`: borrows parent's stop-source.       |
| `Supervised`    | many        | 0..*       | A `Nursery` with a policy domain installed.               |

### 5.3 Where each piece lives

| Header                                   | Type            | Reason                                                |
|------------------------------------------|-----------------|-------------------------------------------------------|
| `Mashiro/Async/Structured/Scope.h`       | `Scope`         | Long-lived owner; tag + ring-buffer.                  |
| `Mashiro/Async/Structured/Scope.h`       | `LinkedScope`   | Same TU as `Scope`; one storage strategy.             |
| `Mashiro/Async/Structured/Nursery.h`     | `Nursery`       | With-block; only meaningful inside a sender chain.    |
| `Mashiro/Async/Structured/Nursery.h`     | `with_nursery`  | Factory; returns a sender.                            |
| `Mashiro/Async/Structured/Supervised.h`  | `Supervised`    | Policy domain + thin wrapper around `Nursery`.        |

---

## 6. Components

### 6.1 `Mashiro/Async/Structured/Scope.h` — `Scope`

`Scope` is a long-lived owner of spawned senders. It is the L5 counterpart of
`stdexec::counting_scope` and the only type in this layer that owns storage.

```cpp
namespace Mashiro::Async::Structured {

    // Compile-time tag derived from a ScopeTag annotation or std::source_location.
    struct ScopeTag {
        std::string_view name;
        std::source_location loc;
    };

    template<ScopeTag Tag = ScopeTag{}>
    class Scope {
    public:
        // Construct empty. Storage is reserved lazily on first spawn.
        Scope() noexcept;

        // Construct with a parent stop-token; cancellation propagates from parent → scope.
        explicit Scope(stop_token parent) noexcept;

        // Non-copyable, non-movable: stable address is a part of the ABI for
        // every spawned op-state's environment.
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&)                 = delete;
        Scope& operator=(Scope&&)      = delete;

        // Pre: spawned senders have completed (debug assertion via on_empty()).
        ~Scope();

        // Spawn a sender into the scope. The sender's receiver environment will
        // expose this scope's stop-token; cancellation flows from
        // RequestStop() → every alive child.
        template<stdexec::sender S>
        void spawn(S&& s) noexcept;

        // Spawn-with-future: returns a sender that completes with the spawned
        // sender's value (or error / stopped). The future itself is owned by
        // the caller's op-state, not the scope.
        template<stdexec::sender S>
        [[nodiscard]] auto spawn_future(S&& s) noexcept;

        // Settles when every spawned sender has completed.
        [[nodiscard]] auto on_empty() noexcept;

        // Request cancellation of every alive child. Returns immediately;
        // children settle through their own stop-callbacks.
        void request_stop() noexcept;

        // Token derived from this scope's stop-source.
        [[nodiscard]] stop_token get_stop_token() const noexcept;

        // For diagnostics.
        [[nodiscard]] static constexpr ScopeTag tag() noexcept { return Tag; }
        [[nodiscard]] std::size_t alive_count() const noexcept;

    private:
        stdexec::counting_scope               scope_;
        stop_source                           own_stop_;     // scope-owned source
        stop_token                            parent_stop_;  // optional parent token
        Detail::OpStateRing                   ring_;         // single-allocation storage
        Detail::ParentStopBridge              bridge_;       // links parent → own_stop_
    };

} // namespace Mashiro::Async::Structured
```

**Tagging.** `Scope<Tag>` is parameterised on a non-type-template `ScopeTag` value so the tag
participates in the type. Two scopes with different tags are different types; this lets
`Diagnostics::trace_pipeline` distinguish them statically without a runtime field. The default
`ScopeTag{}` is filled in from `std::source_location::current()` at the site that *names* the
scope (an `inline static` per call site), so even an untagged scope has a unique compile-time
identity.

**Single-allocation ring buffer.** `Detail::OpStateRing` is a fixed-capacity, lazily-allocated
ring of type-erased op-state slots. It is sized from the `Async::Allocates` policy declared
on the scope's environment (default 32 slots × 256 bytes per slot, configurable per
construction). On the first `spawn()` the ring allocates once from
`get_allocator(get_env(receiver))`; subsequent `spawn()` calls reuse the ring. Spawned op-states
that fit the slot size are constructed in-place (heap-free); op-states that exceed the slot fall
back to allocator-fed heap allocation, *one* allocation per oversize op-state. The umbrella
spec's §5.3 guarantees this is at-most-one-allocation-per-`Scope` for the typical case; oversize
op-states are documented at the call site that produces them, never silent.

**Parent linkage.** When constructed with a parent token, `Detail::ParentStopBridge`
registers an `inplace_stop_callback` on the parent that calls
`own_stop_.request_stop()` on fire. The bridge's destructor unregisters; lifetime is bounded by
the `Scope`. There is no dynamic allocation (the callback storage is a member of the bridge).

**Spawn semantics.** `spawn(s)` constructs a receiver whose environment exposes:
- `get_stop_token` returns `own_stop_.get_token()`.
- `get_scheduler` defers to whatever the spawned sender already had — `Scope` does not impose a
  scheduler, per the §4 constraint.
- `get_allocator` is the scope's allocator (used for the op-state itself; the spawned sender's
  internal allocations follow stdexec's existing rules).

The receiver is wrapped through `counting_scope::join`-style accounting: each spawn increments
the count, each child completion decrements it, and `on_empty()` is the sender that completes
when the count returns to zero.

**`on_empty()` settling.** `on_empty()` returns a sender that completes with `set_value()` once
the count reaches zero. It is the **only** correct way to await scope drain — looping on
`alive_count()` is racy and forbidden in production code (debug-only).

**Lifetime rules.**
1. The `Scope` must outlive every sender spawned into it.
2. A `Scope`'s destructor asserts `alive_count() == 0` in debug mode; in release, destroying a
   non-empty scope is undefined behaviour and a violation of the §1 invariant. The standard
   way to drain is `co_await scope.on_empty()` (or its sender equivalent in non-coroutine
   contexts).
3. `request_stop()` does **not** wait. It signals cancellation; the caller must still
   `co_await scope.on_empty()` to observe the children settling.

### 6.1b `Scope::spawn(Job)` — Job-as-sender (v0.2 reaffirmation)

Subagent D raised a coordination question with Subagent C (`09-synthesis.md` §2.18) on whether
`Scope::spawn(Job)` requires a `Job`-specific overload. The synthesis adjudication is **no, it
does not**: `Coro::Job<S>` (`04-coroutine-tasks.md` §6.2) is a coroutine type built on
`exec::task<void>` and therefore satisfies `stdexec::sender`. The existing
`template<stdexec::sender S> void spawn(S&& s)` overload (§6.1) is the sole entry point. The
`Async::Detached` annotation discriminates intent (a `Job` is detached; a `Task<T>` is
co-awaitable), and Subagent D's `Detail::JobImpl` constructor is the *only* code path that
hands a `Job`'s underlying sender to `Scope::spawn` — direct construction of `Job` outside
`Scope::spawn` is forbidden by L4 §6.2.

The contract:

```cpp
// At Nursery / Scope construction:
nursery.spawn(prefetch(asset));   // prefetch returns Coro::Task<void> — accepted as sender.
nursery.spawn(some_sender);       // accepted as sender.

// Inside Nursery::spawn(coroutine_task):
//   - constructs a Detail::JobImpl that wraps the task
//   - the JobImpl satisfies the sender concept (it forwards to the task's promise)
//   - the resulting Job<S> handle is enrolled with the underlying counting_scope
//   - the caller observes a Job<S> return value only for spawn_with_handle (rare)
```

`Traits::IsDetached_v<Job<S>>` is `true`; `Traits::IsDetached_v<Task<T,S>>` is `false`. L5
patterns that need to assert "this spawn is detached" check the annotation, not the type. The
escape rule (§7) applies identically to both shapes — a `Task<void>` spawned into a `Nursery`
is structurally indistinguishable from a `Job` at runtime; the difference is only in the
syntactic spawn site (`spawn_task` vs the bare `spawn`).

### 6.2 `Mashiro/Async/Structured/Scope.h` — `LinkedScope`

`LinkedScope` is a sub-scope whose stop-source is the parent's. It is strictly cheaper than
`Scope` because it does not own a stop-source — only a token and a counting wrapper.

```cpp
namespace Mashiro::Async::Structured {

    template<ScopeTag Tag = ScopeTag{}>
    class LinkedScope {
    public:
        explicit LinkedScope(stop_token parent) noexcept;

        LinkedScope(const LinkedScope&)            = delete;
        LinkedScope& operator=(const LinkedScope&) = delete;
        LinkedScope(LinkedScope&&)                 = delete;
        LinkedScope& operator=(LinkedScope&&)      = delete;

        ~LinkedScope();  // assert alive_count() == 0 in debug

        template<stdexec::sender S>
        void spawn(S&& s) noexcept;

        template<stdexec::sender S>
        [[nodiscard]] auto spawn_future(S&& s) noexcept;

        [[nodiscard]] auto on_empty() noexcept;

        // Borrowed token — has the same lifetime as the parent's stop-source.
        [[nodiscard]] stop_token get_stop_token() const noexcept { return parent_; }

    private:
        stdexec::counting_scope scope_;
        stop_token              parent_;
        Detail::OpStateRing     ring_;
    };

} // namespace Mashiro::Async::Structured
```

**Use case.** A long-running server owns a `Scope` for its lifetime; for each incoming request
it constructs a `LinkedScope(server_scope.get_stop_token())` to bound the request's spawned
work. Cancelling the *server* (via the server scope's stop-source) cancels every request's
spawned work transitively — because every `LinkedScope`'s spawned senders see the parent's
stop-token directly, with no bridge in between. Cancelling the *request* (by destructing the
`LinkedScope`'s coroutine, or by an explicit local stop-source) is the responsibility of the
caller; `LinkedScope` itself has no `request_stop()` because it does not own one.

**Why no `request_stop()`.** `LinkedScope` deliberately omits a per-scope cancellation channel.
Adding one would either (a) require owning a stop-source — at which point it becomes a
`Scope` — or (b) require a more elaborate bridge that ORs a per-scope flag into the parent's
token, which would re-introduce a hand-rolled cancellation path that the umbrella spec's §5.4
explicitly bans. If a request needs *both* the server's cancellation and a local one, use a
`Scope(server_scope.get_stop_token())` — one allocation, both channels.

**Single-allocation guarantee.** `LinkedScope` reuses the same `OpStateRing` strategy as
`Scope`. The bridge member is gone (no parent-to-own redirect needed); the ring is the only
heap allocation, and it is amortised across every spawn.

### 6.3 `Mashiro/Async/Structured/Nursery.h` — `Nursery` and `with_nursery`

`Nursery` is the **with-block** variant of `Scope`. It is the model from §6.4 of the umbrella
spec — *"a region of code that owns N child tasks and refuses to exit until all of them finish
(or are cancelled)"*. The ergonomic difference from `Scope` is the deliberate one: a `Nursery`
exists only inside the body of a `with_nursery` factory; its lifetime is the sender that
`with_nursery` returns, and the spawned children are settled before the sender completes.

```cpp
namespace Mashiro::Async::Structured {

    template<ScopeTag Tag = ScopeTag{}>
    class Nursery {
    public:
        // Spawn a sender into the nursery. Identical contract to Scope::spawn:
        // the sender's environment carries the nursery's stop-token.
        template<stdexec::sender S>
        void spawn(S&& s) noexcept;

        // Spawn a coroutine task. Equivalent to spawn(stdexec::just_from(...)),
        // but documents intent and lets diagnostics tag the spawn site as a task.
        template<class T>
        void spawn_task(Coro::Task<T>&& t) noexcept;

        // Spawn a stream consumer: subscribes fn to every element of the
        // stream, completes when the stream ends or stop fires. fn returns a
        // sender so the consumer can be async.
        template<class T, class Fn>
        void spawn_stream_consumer(Coro::Stream<T> s, Fn fn) noexcept;

        // Borrowed accessors — match Scope.
        [[nodiscard]] stop_token get_stop_token() const noexcept;
        [[nodiscard]] std::size_t alive_count() const noexcept;
        [[nodiscard]] static constexpr ScopeTag tag() noexcept { return Tag; }

        // No public construction; Nursery is constructed only by with_nursery.
    private:
        friend class Detail::NurseryFactory<Tag>;
        explicit Nursery(stop_token parent) noexcept;

        Scope<Tag> impl_;          // delegates storage and tagging
        std::atomic<bool> errored_ = false;
    };

    // Factory: returns a sender that runs body(nursery) and completes only
    // after every spawned child has settled.
    template<ScopeTag Tag = ScopeTag{}, class Body>
    [[nodiscard]] auto with_nursery(Body body) noexcept;

} // namespace Mashiro::Async::Structured
```

**Sender shape.** `with_nursery(body)` returns a sender with the completion signatures:

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(Body::result_type),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()
>
```

— with `Body::result_type` being whatever the body's sender or coroutine return type yields.
Where the body returns `void`, the value channel is `set_value_t()` (no payload). The error
and stopped channels are always present because *any* spawned child can fail or be cancelled,
and `Nursery` aggregates those into the parent's signatures even if the body itself cannot
produce them.

**Spawn semantics.** `spawn(s)` is identical in mechanics to `Scope::spawn(s)` — the receiver's
environment carries the nursery's stop-token, the op-state lives in the ring buffer, and
completion routes through the counting wrapper. The semantic difference is the *aggregation*
described next.

**Error propagation: one fails → siblings cancelled.** When a spawned child completes with
`set_error`, the nursery atomically transitions `errored_` from `false` to `true` (the first
error wins) and calls `request_stop()` on its underlying `Scope`. Every other alive child sees
its stop-token fire and unwinds through `set_stopped`. The nursery's own completion is delayed
until the underlying `on_empty()` settles — meaning every sibling has unwound — and *then*
the nursery completes the parent's receiver with the captured error. The captured error is the
*first* one observed; subsequent errors from siblings are dropped (logged via
`Diagnostics::scope_audit` in test mode; see §7). If we wanted "collect all errors", that is
the job of `Supervised` (§6.4) with the appropriate policy.

**Parent-cancel-cascades-down.** When the parent of `with_nursery(...)` itself receives a stop
request, the request flows through the `Nursery` to its `Scope` to every spawned child via the
existing stop-callback chain — three hops, all of which are stdexec primitives, none of which
allocate. There is no need for `Nursery` to register a callback on its own input: stdexec's
sender machinery already wires the parent's environment into the receiver passed to the
nursery's body, so the nursery's stop-source is *constructed from* the parent's token. The
result is that cancelling the outermost `Task` cancels every transitively-spawned sender with
no client-side bookkeeping.

**Why this is more than `Scope`.** A free-standing `Scope` is a *value* whose lifetime the
client manages. The user can spawn into it, forget about it, drop it, and (in debug) hit an
assertion when they exit without draining. A `Nursery` is a *with-block* — its scope-bracket
*is* the sender, and the sender's completion *is* the drain. The two semantics are
equivalent in primitives but profoundly different in safety: a `Nursery` cannot be exited
without drain, because exit *is* drain. The umbrella spec's §6.4 calls out this difference
("the with-block scoping is the semantic difference") and L6 patterns (`fork_join`, the worked
example in §10 of this spec) prefer `Nursery` for that reason.

**`spawn_stream_consumer`.** A small convenience that combines `Stream<T>::subscribe` with a
nursery spawn: the consumer is its own sender, the nursery owns its lifetime, and cancellation
of the nursery cancels the subscription. The implementation is a `let_value` over
`stream.next()`, looped via `exec::repeat_until`, returning when the stream completes or the
stop-token fires. (Stream itself is a Subagent C concern; this method takes it by name.)

### 6.4 `Mashiro/Async/Structured/Supervised.h` — `Supervised`

`Supervised` is a `Nursery` with a **policy** that decides what to do when a child fails. The
canonical policies are:

- **`Restart{n}`** — restart the failed child up to `n` times before propagating.
- **`LogAndSkip`** — log the error, drop the child, leave the rest of the nursery alive.
- **`Propagate`** — equivalent to plain `Nursery` (one fails → all cancelled).
- **`Collect`** — let every child run to completion (no cascading cancellation), collect all
  errors into a `std::vector<std::exception_ptr>`, and propagate the vector.

```cpp
namespace Mashiro::Async::Structured {

    struct Restart    { std::size_t max_attempts = 3; };
    struct LogAndSkip {};
    struct Propagate  {};
    struct Collect    {};

    template<class Policy>
    concept SupervisorPolicy = std::same_as<Policy, Restart>
                            || std::same_as<Policy, LogAndSkip>
                            || std::same_as<Policy, Propagate>
                            || std::same_as<Policy, Collect>;

    template<SupervisorPolicy Policy, ScopeTag Tag = ScopeTag{}, class Body>
    [[nodiscard]] auto with_supervised(Policy policy, Body body) noexcept;

} // namespace Mashiro::Async::Structured
```

**Implementation: a domain rewrite, not a new primitive.** `Supervised` does not introduce a
new scope type — it constructs an ordinary `Nursery` and registers a *child-completion domain*
that intercepts each spawned child's `set_error` channel. The domain's `transform_sender`
specialisation wraps each spawned sender in a small adaptor whose receiver implements:

```cpp
// Per-child receiver, parameterised on Policy:
void set_error(std::exception_ptr e) noexcept {
    if constexpr (std::same_as<Policy, Restart>)    /* re-spawn ≤ max_attempts */;
    if constexpr (std::same_as<Policy, LogAndSkip>) /* log via Diagnostics */;
    if constexpr (std::same_as<Policy, Propagate>)  /* set_error on parent */;
    if constexpr (std::same_as<Policy, Collect>)    /* push into vector */;
}
```

The compiler eliminates the unused branches via `if constexpr`, and the policy's storage
(restart count, error vector) lives as a member of the supervised wrapper sender — no extra
allocation. `Nursery::spawn` is reused unchanged; the domain rewrite runs at sender-pipeline
construction time, so every spawn observed inside `with_supervised`'s body is rewritten
automatically.

**Why a rewrite, not a subclass.** A subclass would either (a) duplicate `Nursery`'s storage
(violating §2.4) or (b) hold a `Nursery&` and add a layer of indirection (violating §2 of the
umbrella spec — "concept-first, type-erased never on hot paths"). A domain rewrite is the
canonical stdexec way to inject behaviour into a sender expression without changing observable
completion signatures, and it is exactly the rewrite-as-customisation pattern that the
umbrella spec's §5.5 sanctions.

**Completion signatures.** `Propagate` is the same as `Nursery`. `LogAndSkip` and `Restart`
remove the error channel for *child* errors (parent-to-nursery errors still propagate); the
nursery completes with `set_value` if every retained child reached `set_value` and the body
completed. `Collect` rewrites the error channel into a value channel:
`set_value_t(std::vector<std::exception_ptr>, Body::result_type)`.

**Use case.** A media server spawns N stream encoders under
`with_supervised(LogAndSkip{}, ...)` — one encoder failing should not tear down the whole
server. A retry loop over flaky downloads uses `Restart{5}`. A batch of independent jobs whose
errors should all be reported at the end uses `Collect`. The vanilla `Nursery` (`Propagate`)
remains the default for "these things are jointly necessary".

---

## 7. The Escape Rule

The **escape rule** is the one structural-concurrency invariant L5 enforces:

> *Nothing spawned in a `Scope` (or `Nursery`, `LinkedScope`, `Supervised`) may outlive
> that scope.*

"Outlive" here means: an op-state, a coroutine frame, a stop-callback, or any other piece of
state whose address is captured into something with a longer lifetime than the scope.

### 7.1 Why we cannot enforce it statically

C++ does not have a borrow checker. A spawned op-state's address could be:

1. Captured into a coroutine frame allocated on a *different* scope (legal at the type system
   level, illegal by the structural-concurrency invariant).
2. Stored in a `std::function` whose lifetime is global.
3. Passed as a raw pointer into a third-party library.
4. Saved into a `std::shared_ptr` whose use-count is incremented from a thread that the scope
   does not see.

None of these are detectable by the type system. We do not pretend otherwise. The framework
contract is "you do not do this"; the framework provides one tool to *catch* you when you do.

### 7.2 `Diagnostics::scope_audit()` — the runtime trace

In test mode (`MASHIRO_ASYNC_DIAGNOSTICS=1`), every `Scope` registers itself with the global
`Diagnostics::ScopeRegistry` on construction and deregisters on destruction. Each spawned
op-state is tagged with the scope's `ScopeTag` and a monotonic spawn-id; the tag flows through
the receiver's environment and is visible to every adaptor downstream.

`Diagnostics::scope_audit()` walks the registry at any point and reports:

- **Live op-states whose scope tag does not match their address-of-storage.** This is the
  structural-escape signature: an op-state that lives in a different scope's ring buffer
  than its tag claims.
- **Op-states whose scope is `nullptr`.** This is the "spawned then scope died" case.
- **Op-states with stop-callbacks installed on a stop-source whose scope is dead.** This is
  the use-after-free precursor.

In production builds, `scope_audit()` is a no-op and the registry is `[[no_unique_address]]`
empty.

The audit is a forward reference to `08-cross-cutting.md` (Subagent E's spec). L5 only declares
the contract — *every* `Scope`, `Nursery`, `LinkedScope`, and `Supervised` must register
through the registry hook so the audit can find them. That registration is one
`if constexpr (Diagnostics::Enabled)` line in each constructor / destructor; release builds
elide it.

### 7.3 Static lints that catch the common case

clang-tidy rules in the project's lint config catch the most common escape patterns even
without the runtime audit:

1. Capturing a `Scope&` (or `Nursery&`) into a lambda whose address is stored beyond the
   enclosing function.
2. Spawning a sender that holds a reference to a stack object below the scope's lifetime.
3. Returning a `Coro::Job` (the L4 "fire-and-forget" task) from inside `with_nursery`'s body.

These are catch-and-warn rules, not framework guarantees. The framework's contract is the
runtime audit.

---

## 8. Cancellation flow — end-to-end diagram

The §6.4 model in the umbrella spec ("a region of code that owns N child tasks") is hollow
without a precise specification of *how* a stop request, originating at the parent of a
nursery (or at the user's `RequestStop()` button), reaches every spawned child's op-state and
unwinds it. The diagram below traces a single stop-token-initiated cancellation from origin
to settle, with every primitive named.

```
┌────────────────────────────────────────────────────────────────────────────┐
│  Origin: user code, signal handler, or a sibling sender's set_error.       │
│                                                                            │
│      auto t = parent_stop_source.request_stop();                           │
│                                                                            │
└────────────────────────────────────┬───────────────────────────────────────┘
                                     │  (1) inplace_stop_source state flips
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  parent_stop_source's callback list is walked.                             │
│  Among the registered callbacks is the one installed by                    │
│  Detail::ParentStopBridge when the Scope was constructed with a            │
│  parent token (§6.1).                                                      │
└────────────────────────────────────┬───────────────────────────────────────┘
                                     │  (2) bridge.callback() fires
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  Scope::own_stop_.request_stop()                                           │
│                                                                            │
│  This is the scope's own stdexec::inplace_stop_source. It now flips, and   │
│  its own callback list is walked.                                          │
└────────────────────────────────────┬───────────────────────────────────────┘
                                     │  (3) every spawned child's
                                     │       stop_callback fires
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  For each alive op-state in the scope's ring buffer:                       │
│                                                                            │
│    The receiver's environment exposed get_stop_token() == own_stop_token.  │
│    The op-state, when it started, registered an inplace_stop_callback on   │
│    that token with a sender-specific cancellation handler.                 │
│                                                                            │
│    The handler runs synchronously, on the thread that called               │
│    request_stop(). Per the umbrella spec's §5.4, the handler is short:     │
│    flip a flag, post a wake, kick a syscall (CancelIoEx,                   │
│    IORING_OP_ASYNC_CANCEL, kqueue EV_DELETE). It does not block.           │
└────────────────────────────────────┬───────────────────────────────────────┘
                                     │  (4) sender-specific wake
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  The sender's wake routes back to whichever scheduler it was running on.   │
│                                                                            │
│  StaticPool: the pool's idle worker pops the cancelled op-state's          │
│              continuation and resumes it.                                  │
│  Tbb:        the arena thread observes the cancellation flag and unwinds.  │
│  Platform:   the wakeEvent_ is set, MsgWaitForMultipleObjectsEx returns,   │
│              the platform thread drains the cancelled op-state.            │
│  Io:         io_uring's CQE / IOCP's GetQueuedCompletionStatusEx reports   │
│              ECANCELED; the op-state completes with set_stopped.           │
└────────────────────────────────────┬───────────────────────────────────────┘
                                     │  (5) sender completes
                                     │       with set_stopped()
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  The receiver attached by Scope::spawn (the counting-scope receiver)       │
│  catches set_stopped, decrements the scope's count, and — if the count     │
│  reaches zero — completes Scope::on_empty()'s sender with set_value().     │
└────────────────────────────────────┬───────────────────────────────────────┘
                                     │  (6) on_empty() resumes
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  The Nursery's outer sender (the one returned by with_nursery) was         │
│  awaiting on_empty(). It now propagates the appropriate completion to     │
│  its own receiver:                                                         │
│                                                                            │
│    - If a child set_error was observed: set_error(captured exception_ptr). │
│    - If only set_stopped or set_value were observed: set_stopped() if      │
│      the parent's stop-token was requested, else set_value(body_result).   │
└────────────────────────────────────────────────────────────────────────────┘
```

**Properties of this flow that fall out of the design:**

1. **No allocation.** Every step is either a pre-registered callback (heap storage owned by
   the registering op-state) or an inline state flip. No `std::function`, no `shared_ptr`, no
   thread-local stash.
2. **No virtual call.** Every callback is a templated `inplace_stop_callback<Fn>` — `Fn` is
   known statically at the registration site.
3. **No race window.** `inplace_stop_source` guarantees that callbacks registered before the
   request fire exactly once, and callbacks registered after the request fire immediately
   (synchronously, on the registering thread). New spawns into a stopped scope therefore
   observe the stop immediately and complete with `set_stopped` at start time — no
   "spawned-but-never-cancelled" gap.
4. **Bounded depth.** The chain is at most three stop-source flips (parent → scope → child),
   regardless of how deep the scope nesting goes. `LinkedScope` short-circuits the middle hop
   (it has no own_stop_), reducing it to two flips.
5. **Reentrancy-safe.** A callback firing during step (3) cannot recursively call
   `request_stop()` on the same scope; `inplace_stop_source` is reentrancy-aware and the
   second request is a no-op.

---

## 9. Composition with other layers

### 9.1 With L2 backends

`Scope` and `Nursery` are scheduler-agnostic. Spawning a sender that runs on the `Tbb`
backend, the `StaticPool` backend, or the `Platform` backend works identically — the receiver's
environment carries the stop-token, the backend honours it, and the scope sees the
completion. The umbrella spec's §6.6 combination matrix records this in the "Sender / Nursery"
cell ("`with_nursery` is a sender") — a `Nursery` *is* a sender, and it composes with any
backend's `continues_on` / `schedule` chain in the obvious way.

### 9.2 With L3 adaptors

Adaptors that wrap a sender (e.g. `retry`, `timeout`, `bulk`) compose with `Scope::spawn`
without ceremony — the adaptor preserves the receiver's environment, so the stop-token still
reaches the inner op-state. The one subtlety is `race`: a `race` over senders spawned into a
nursery cancels the losers' op-states through the *nursery's* stop-source via the same
mechanism described in §8 step (3).

### 9.3 With L4 coroutine types

`Coro::Task<T>` and `Coro::Stream<T>` are senders on the outside; they compose with `Scope`
through the same `spawn`/`spawn_future` mechanics as any other sender. `Coro::Job` is the
fire-and-forget L4 task — spec §4 of `04-coroutine-tasks.md` declares that `Job`'s lifetime is
"owned by the parent `Scope`", and that ownership is exactly the L5 contract: a `Job` is
*always* spawned into a scope (there is no detached `Job`); the scope's drain reaps it.

`spawn_task` (§6.3) is the convenience for taking a `Task<T>` directly without naming it as a
sender; `spawn_stream_consumer` is the convenience for "subscribe to this stream and run fn
on each element until it ends or stop fires".

### 9.4 With L6 patterns

L6 patterns (`parallel_for`, `pipeline`, `actor`, `reactive`, `fork_join`, `scatter_gather`)
each take an explicit `Scope&` (or `Nursery&`) parameter when they spawn anything that has
non-trivial lifetime. The constraint from `00-overview.md` §8.4 is "no pattern introduces a
scheduler" — they take one as a parameter; the same applies to scopes. A pattern that needs
to spawn N concurrent senders (e.g. `fork_join`) takes a `Nursery&` from the caller, never
constructs one implicitly.

---

## 10. Examples

### 10.1 Plain `Scope` — long-running owner

```cpp
class Server {
    Async::Structured::Scope<ScopeTag{"Server"}> scope_;

public:
    Server(Async::stop_token parent) : scope_(parent) 

    void on_request(Request req) {
        scope_.spawn(handle_request(std::move(req)));
    }

    Async::Coro::Task<void> shutdown() {
        scope_.request_stop();
        co_await scope_.on_empty();   // awaits every alive request
    }
};
```

### 10.2 `Nursery` — bounded fan-out inside a function

```cpp
Async::Coro::Task<Result> render_frame(Frame f) {
    co_return co_await Async::Structured::with_nursery<ScopeTag{"render_frame"}>(
        [&](auto& nursery) -> Async::Coro::Task<Result> {
            Result r;
            nursery.spawn(rasterise_geometry(f, r));
            nursery.spawn(rasterise_ui(f, r));
            nursery.spawn(rasterise_overlay(f, r));
            // returning here would not exit until every spawn settles
            co_await nursery.on_empty();
            co_return r;
        });
}
```

### 10.3 `LinkedScope` — per-request scope inside a server scope

```cpp
Async::Coro::Task<Response> handle_request(Request r,
                                           Async::stop_token server_stop) {
    Async::Structured::LinkedScope<ScopeTag{"request"}> req_scope(server_stop);
    req_scope.spawn(authenticate(r));
    req_scope.spawn(load_session(r));
    req_scope.spawn(prefetch_assets(r));

    co_await req_scope.on_empty();   // server stop or all-finished — both end here
    co_return Response{};
}
```

### 10.4 `Supervised` — failure-tolerant batch

```cpp
auto run_encoders(std::span<const StreamConfig> configs) {
    return Async::Structured::with_supervised(
        Async::Structured::LogAndSkip{},
        [&](auto& nursery) {
            for (auto const& cfg : configs)
                nursery.spawn(start_encoder(cfg));
        });
}
```

If `start_encoder(cfg[3])` fails, the rest keep running; the failure is logged via
`Diagnostics::scope_audit` with the spawn site recorded. If the parent of `run_encoders`
issues a stop, every encoder cancels through the §8 chain.

---

## 11. Decisions and alternatives

- **Why not re-export `counting_scope` as `Scope`?** The umbrella spec's §5.3 demands a
  diagnostics-tagged scope and a single-allocation ring-buffer storage strategy that
  `counting_scope`'s default doesn't offer. The thin wrapper buys both without forking
  stdexec.
- **Why no `Scope::join()` blocking method?** Blocking violates §2.7 of the umbrella spec
  ("cancellation is structural") — every wait must be expressible as a sender or `co_await`,
  not as `pthread_cond_wait`. `on_empty()` is the only correct primitive.
- **Why is `Supervised` a domain rewrite, not a class?** §6.4 above; in summary, a domain
  rewrite reuses every primitive, adds zero indirection, and matches stdexec's customisation
  point exactly.
- **Why does `Nursery` track `errored_` atomically?** Two children failing concurrently must
  agree on which error wins; the atomic CAS is the simplest correct path. Cost is one cache
  line per nursery, only consulted on the failure path.
- **Why one allocation, not zero?** A truly heap-free scope would require a fixed inline ring
  whose size is a template parameter. We considered it; it forces the call site to know how
  many spawns it will perform, which is exactly the friction L5 is meant to remove. The
  one-allocation policy is a pragmatic compromise: amortised across spawns, accountable via
  `Diagnostics::AllocCheck`, configurable per scope.
- **Why no `cancel_on_error = false` knob on `Nursery`?** That knob is `Supervised` with the
  `Collect` policy. Adding a duplicate spelling on the base nursery would split the design
  surface. The umbrella spec's design principles (§2.10) prefer one canonical name per
  capability.

---

## 12. Glossary

- **Scope** — long-lived owner of spawned senders; the L5 wrapper of `stdexec::counting_scope`.
- **Nursery** — with-block variant of `Scope`; a sender whose completion awaits drain.
- **LinkedScope** — sub-scope that borrows a parent's stop-token; cheaper than `Scope`.
- **Supervised** — `Nursery` with a failure policy (Restart / LogAndSkip / Propagate / Collect).
- **Escape rule** — the structural-concurrency invariant: nothing spawned outlives its scope.
- **scope_audit** — diagnostics function that traces structural-escape violations in test mode.
- **ScopeTag** — compile-time tag on a scope, default-derived from `std::source_location`.

