# Mashiro Async Framework — L6 Patterns

**Status:** Draft v0.2 (layer-6 spec; instantiates models §6.1–§6.5 from `00-overview.md` v0.2)
**Date:** 2026-06-15 (v0.1) · 2026-06-16 (v0.2 synthesis pass)
**Author:** Mashiro Engine team — Subagent D
**Scope:** `Mashiro::Async::Patterns` namespace; new sources under
`Mashiro/include/Mashiro/Async/Patterns/` and `Mashiro/src/Async/Patterns/`. Builds on L0–L5.
Patterns are *thin* — each is ≤ ~50 LOC of header. They take schedulers and scopes from the
caller, never construct their own.

### Revision history

- **v0.1** — initial draft. Vocabulary frozen by `00-overview.md` v0.1, §5 / §6 / §8.4. Spans
  the six patterns demanded by §8.4 (`parallel_for`, `pipeline`, `actor`, `reactive`,
  `fork_join`, `scatter_gather`), each instantiating one or more of the five async models from
  §6 of the umbrella. Closes with a cross-pattern composition example
  (`parallel_for` over a `pipeline` over a `Stream`) that confirms cancellation flows correctly
  through the composition.
- **v0.2** — synthesis pass (see `09-synthesis.md` §2.19, §7). Adds `pipeline_as_stream(p)` —
  the L6-resident bridge from a `pipeline(...)` sender to a `Coro::Stream<T>` (§3.8). The
  cross-pattern composition example (§10.2) is updated to use `pipeline_as_stream` in place of
  the v0.1 placeholder `stream::from_pipeline`. No other surface changes; the bridge lives in
  `Patterns/Pipeline.h` so that L4 `Stream` headers remain free of any dependency on L6
  patterns.

---

## 1. Overview

L6 is a catalogue of *thin* combinators built on top of L0–L5. Each pattern in this spec
satisfies six properties:

1. **Instantiates one or more async models from §6 of `00-overview.md`.** Every pattern is
   listed in §1.1 below with its model. Adding a new pattern that doesn't map onto §6 is a
   design-review reject.
2. **Takes a scheduler as a parameter.** Per `00-overview.md` §8.4. No pattern owns one. Where
   a pattern needs sequencing (`actor`'s mailbox), it takes a serial scheduler from the
   caller.
3. **Takes a scope as a parameter when it spawns.** A pattern that fans out (`fork_join`,
   `scatter_gather`, `pipeline`) takes an `L5::Scope&` or `L5::Nursery&` from the caller.
   Patterns that produce a single sender (`parallel_for`) do not, because their fan-out is
   bounded by `bulk`'s op-state.
4. **Has explicit completion signatures.** Per `00-overview.md` §5.7, every pattern declares
   its `completion_signatures_of_t` specialisation; signatures are unioned, not concatenated;
   `set_stopped_t()` propagates when any upstream offers it.
5. **Cancels structurally.** A stop-token request reaches every spawned op-state via the L5
   §8 cancellation chain. No pattern owns a `closed_` flag; no pattern installs a virtual
   cancellation hook. Cancellation behaviour is documented per pattern (§3 onwards).
6. **Composes.** Patterns nest: a `parallel_for` over the elements yielded by a `pipeline`
   over a `Stream<T>` is a single sender expression whose cancellation flows
   end-to-end. §10 of this spec is the worked composition test.

### 1.1 Pattern → model map

| Pattern             | Model from §6 of overview            | Scope it owns                        |
|---------------------|--------------------------------------|--------------------------------------|
| `parallel_for`      | §6.1 sender expression (over `bulk`) | None (bulk is op-state-scoped)       |
| `pipeline`          | §6.5 reactive / push-driven          | `Nursery` (per-stage spawn)          |
| `actor`             | §6.4 nursery + serial scheduler      | `Scope` (mailbox + behaviour spawn)  |
| `reactive`          | §6.5 + §6.3 stream                   | None (combinators are stream → stream) |
| `fork_join`         | §6.4 nursery                         | `Nursery` (with reduce)              |
| `scatter_gather`    | §6.1 over `bulk` + reduce            | None (gather buffer is sender-local) |

Patterns that own a scope do so because their lifetime is bounded by N spawned children whose
errors must be propagated; patterns without a scope use `bulk`'s built-in fan-out (whose
op-state is the scope, with single-allocation behaviour the L2 backend already guarantees).

### 1.2 Naming and surface

All patterns live under `Mashiro::Async::Patterns`. Each has a single header
(`Mashiro/Async/Patterns/<Pattern>.h`); each header pulls in only the L0/L1/L3/L5 vocabulary
it needs (no transitive include of unrelated patterns). The convenience aggregate
`Mashiro/Async/Async.h` re-exports the entire namespace.

---

## 2. `parallel_for(range, fn)`

**Header:** `Mashiro/Async/Patterns/ParallelFor.h`
**Model:** §6.1 sender expression, lowered onto `bulk` over a `BulkScheduler`.

### 2.1 Form

```cpp
namespace Mashiro::Async::Patterns {

    // Returns a sender that completes when fn has been invoked for every
    // element of the range on the supplied BulkScheduler.
    template<Concepts::BulkScheduler Sched, std::ranges::random_access_range R, class Fn>
        requires std::invocable<Fn&, std::ranges::range_reference_t<R>>
    [[nodiscard]] auto parallel_for(Sched sched, R&& range, Fn fn);

    // Convenience: take the range from a sender that produces a range.
    template<Concepts::BulkScheduler Sched, stdexec::sender S, class Fn>
    [[nodiscard]] auto parallel_for(Sched sched, S&& range_sender, Fn fn);

} // namespace Mashiro::Async::Patterns
```

### 2.2 Completion signatures

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(),                        // every fn invocation succeeded
    stdexec::set_error_t(std::exception_ptr),      // any fn invocation threw
    stdexec::set_stopped_t()                       // stop-token fired
>
```

Where `Fn` is `noexcept`, the error channel is dropped (via `with_error<never>` from L1).

### 2.3 Cancellation

A stop request fires every alive `bulk`-iteration's stop-callback through the L5 §8 chain. The
scheduler's `bulk` implementation determines what "fire" means:

- **StaticPool's `bulk`** stops dispatching new iterations and lets the in-flight ones
  complete; the iterations that have not yet been picked up complete with `set_stopped`.
- **Tbb's `bulk`** lowers via the domain rewrite to `tbb::parallel_for` over a
  `task_group_context`; cancellation calls `cancel_group_execution()` on the context, which
  stops further iteration *and* drains the workers cooperatively.

In both cases, the pattern itself owns no cancellation logic — it is the scheduler's
responsibility.

### 2.4 LOC budget

The header is ~40 LOC: a function-template `parallel_for` whose body is essentially
`stdexec::bulk(stdexec::just(std::ranges::size(range)), n, [](size_t i){ fn(range[i]); })`,
with one `if constexpr` branch for the sender-of-range overload. No internal state.

### 2.5 Worked example: TBB and StaticPool side by side

```cpp
auto pixels = std::span<Pixel>{...};

// On the static pool — work-stealing across N workers.
co_await Patterns::parallel_for(static_pool_sched(), pixels,
    [](Pixel& p){ p = tonemap(p); });

// On TBB — same call site, lowered to tbb::parallel_for via the domain rewrite.
co_await Patterns::parallel_for(tbb_sched(), pixels,
    [](Pixel& p){ p = tonemap(p); });
```

The two calls are textually identical except for the scheduler. The TBB path's domain rewrite
(see `02-backends.md` §3) intercepts the `bulk` expression and lowers it to
`tbb::parallel_for` with the right `affinity_partitioner`; the static-pool path keeps the
default lowering. Cancellation, completion signatures, and allocation behaviour are the same
on both — that is the entire point of taking a `BulkScheduler` rather than a concrete type.

---

## 3. `pipeline(stage1, stage2, ...)`

**Header:** `Mashiro/Async/Patterns/Pipeline.h`
**Model:** §6.5 reactive / push-driven dataflow.

### 3.1 Form

```cpp
namespace Mashiro::Async::Patterns {

    enum class BackpressurePolicy : uint8_t {
        Drop_Oldest,    // bounded queue; on full, evict the head
        Drop_Newest,    // bounded queue; on full, drop the incoming element
        Wait,           // unbounded queue; producer awaits consumer
        Latest_Only     // single-slot; on producer arrival, replace the slot
    };

    template<class T>
    struct Stage {
        std::function<auto(T) -> stdexec::sender_of<set_value_t(/* next-T */)>>  fn;
        BackpressurePolicy policy = BackpressurePolicy::Wait;
        std::size_t        capacity = 64;
    };

    // pipeline(s1, s2, ...) returns a sender that, when started, runs every
    // stage concurrently inside the supplied nursery. The pipeline completes
    // when the source completes and every queued element has drained through.
    template<class Sched, class... Stages>
    [[nodiscard]] auto pipeline(Structured::Nursery<>& n,
                                Sched sched,
                                Stages... stages);

} // namespace Mashiro::Async::Patterns
```

A *stage* is a sender-producing function — it takes one `T` and returns a sender whose
value-channel is the next-stage `T'`. Stages connect via internal queues sized by the stage's
`capacity` and disciplined by its `BackpressurePolicy`.

### 3.2 Completion signatures

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(),                        // source completed, all queues drained
    stdexec::set_error_t(std::exception_ptr),      // any stage failed
    stdexec::set_stopped_t()                       // stop-token fired
>
```

Per-element values flow through the queues; the pipeline as a whole returns a single value
(success/failure of the *flow*), not a `Stream` of element values. To consume per-element
output, terminate the pipeline with a *sink* stage that consumes `T` and returns
`stdexec::just()`.

### 3.3 Cancellation

A stop request:

1. Cancels every alive stage sender through the nursery's stop-source (L5 §8).
2. Drains queues without producing further elements — the queues' producers complete with
   `set_stopped`, the queues' consumers see "queue closed" and complete with `set_stopped`.
3. The pipeline's outer sender completes with `set_stopped` once `nursery.on_empty()` settles.

There is no bespoke "shutdown" code — the queues are senders, the stages are senders, the
nursery cancels them all uniformly.

### 3.4 Internal queues

Each stage's input queue is one of three types, chosen at compile time from the policy:

- **`Wait`** — `MpscQueue<T, N>` from Mashiro core; producer blocks (sender-suspends) on full.
- **`Drop_Oldest`** / **`Drop_Newest`** — bounded ring buffer with the named eviction policy.
- **`Latest_Only`** — single-slot atomic exchange (a single `std::atomic<std::optional<T>>`
  with sender-friendly waiters).

The queue type is a `template<BackpressurePolicy P, class T, size_t N>` alias; pipeline stages
of different policies interoperate because each stage's *output* into the next stage's *input*
goes through the next stage's queue, and the queue type is determined by the next stage's
declaration.

### 3.5 TBB rewrite — when the pipeline is large

When `sizeof...(Stages) >= 4` and `sched` is the `Tbb` scheduler, the TBB domain (per
`02-backends.md` §4) rewrites the entire pipeline expression into a `tbb::flow::graph`:

- Each stage becomes a `tbb::flow::function_node`.
- Internal queues become `tbb::flow::buffer_node` with the right policy.
- Backpressure is enforced by the graph's reservation semantics.
- The pipeline's outer sender wraps a `flow::graph::wait_for_all`.

The rewrite is observably equivalent (same completion signatures, same cancellation, same
per-element ordering), but the lowering avoids the per-stage stdexec receiver overhead, which
matters when the per-element work is small. Under four stages, the rewrite is skipped — the
overhead is below the threshold where TBB's flow-graph indirection pays off.

### 3.6 LOC budget

The header is ~50 LOC: one function template that pack-expands into a fold over
`let_value` chains, with stage queues as a `std::tuple` of typed queue instances. The TBB
rewrite is in the TBB backend's domain header, not in `Pipeline.h`.

### 3.7 Worked example

```cpp
auto packets = stream::from_socket(sock);

co_await with_nursery([&](auto& n) {
    return Patterns::pipeline(n, scheduler,
        Patterns::Stage<Packet>{
            .fn       = [](Packet p){ return parse(p); },
            .policy   = BackpressurePolicy::Drop_Oldest,
            .capacity = 256
        },
        Patterns::Stage<ParsedPacket>{
            .fn       = [](ParsedPacket p){ return validate(p); },
            .policy   = BackpressurePolicy::Wait,
            .capacity = 64
        },
        Patterns::Stage<ValidPacket>{
            .fn       = [&store](ValidPacket p){ return store.append(p); },
            .policy   = BackpressurePolicy::Wait,
            .capacity = 32
        });
});
```

If `parse` falls behind, the input queue spills to `Drop_Oldest`; if `validate` falls behind,
the parser stalls (`Wait`); if `append` falls behind, the validator stalls. The whole chain
cancels structurally on a stop request.

### 3.8 `pipeline_as_stream(p)` — bridge from pipeline sender to `Coro::Stream<T>` (v0.2)

The composition example in §10.2 needs to take the *output* of a `pipeline(...)` and feed it to
a `Stream<T>` consumer (e.g. `Nursery::spawn_stream_consumer`). The synthesis pass
(`09-synthesis.md` §2.19) adjudicated the bridge as an L6 concern — a pipeline is a pattern,
its conversion to a stream is a pattern operation — so the bridge lives in
`Patterns/Pipeline.h`, never in the L4 `Coro::stream::*` namespace.

```cpp
namespace Mashiro::Async::Patterns {

    // Convert a pipeline(...) expression into a Coro::Stream<T>. T is deduced
    // from the pipeline's terminal stage's value-channel element type.
    //
    // Use case: when the consumer prefers iterating with `for co_await`
    // (or MASHIRO_FOR_CO_AWAIT — see 04-coroutine-tasks.md §5.4b) over the
    // pipeline's native sender form. Allocates one internal channel sized by
    // the terminal stage's capacity; the channel's allocator is taken from the
    // ambient environment via get_allocator(get_env(receiver)).
    template<class PipelineSender>
    [[nodiscard]] auto pipeline_as_stream(PipelineSender p)
        -> Coro::Stream<typename Detail::PipelineValueType<PipelineSender>::type>;

} // namespace Mashiro::Async::Patterns
```

**Mechanics.** The bridge replaces the pipeline's terminal sink with a per-element write into
a `Channel<T>` (capacity = the terminal stage's capacity); the returned `Stream<T>` is
`Coro::stream::from_queue(channel)` (`04-coroutine-tasks.md` §5.5). The pipeline's own
nursery — already spawned at the caller's site — drives the channel's producer. Closing the
channel is handled by the pipeline's set_value / set_stopped completion path; the bridge
installs an `inplace_stop_callback` on the consumer's stop-token that closes the channel
early, propagating cancellation backward into the pipeline through the L5 §8 chain.

**Allocations.** One channel (per `pipeline_as_stream` call). The bridge owns no other state;
all per-element flow goes through the channel that already existed conceptually inside the
pipeline.

**Completion signatures of the resulting stream.**

```cpp
// Per-element pull (matches Stream<T>::next() — see 04-coroutine-tasks.md §5.2):
using next_signatures = stdexec::completion_signatures<
    stdexec::set_value_t(std::optional<T>),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()
>;
```

`set_value(std::nullopt)` indicates the pipeline has completed; `set_error` is propagated
from any stage's error; `set_stopped` is propagated from any stop request on the consumer
side or on the pipeline's nursery.

**Reflection annotations.** The bridge's returned `Stream<T>` carries the same annotations as
any other `Stream<T>` (`[[=Async::Cancellable, =Async::Allocates{Where::OpState}]]`); the
allocation tag is *OpState* rather than *None* because the per-call channel is the bridge's
own non-amortised allocation, separate from the pipeline's nursery-amortised queues.

---

## 4. `actor<State>(behaviours...)`

**Header:** `Mashiro/Async/Patterns/Actor.h`
**Model:** §6.4 nursery + a serial scheduler per actor (`Inline` over an `MpscQueue`).

### 4.1 Form

```cpp
namespace Mashiro::Async::Patterns {

    template<class State>
    class Actor {
    public:
        // Construct an actor with an initial state and a list of behaviour
        // functions. Each behaviour is sender-returning and takes (State&,
        // Message). Behaviours are dispatched by message type.
        template<class... Behaviours>
        Actor(Structured::Scope<>& scope, State initial, Behaviours... behaviours);

        // Send a message; returns a sender that completes when the actor has
        // dequeued and processed it. The sender carries the behaviour's
        // value, error, or stopped channel.
        template<class Message>
        auto send(Message m) -> stdexec::sender_of<set_value_t(/* return-of-behaviour */)>;

        // Send-and-forget: the actor processes the message asynchronously;
        // caller does not observe the result.
        template<class Message>
        void cast(Message m);
    };

    // Factory.
    template<class State, class... Behaviours>
    [[nodiscard]] auto actor(Structured::Scope<>& scope, State initial, Behaviours... bs);

} // namespace Mashiro::Async::Patterns
```

### 4.2 Mailbox

The mailbox is the project's existing `Mashiro::MpscQueue<Message, N>` from
`Mashiro/Core/MpscQueue.h`. The pattern *uses* the existing primitive directly — it does not
wrap, alias, or re-implement it. (The umbrella spec's §4 refers to this queue by name; per
`00-overview.md` §8.4 the actor pattern's mailbox *is* that queue.)

`Message` is a `std::variant` over the message types accepted by the behaviour list. The
variant is built consteval from the behaviour pack: each behaviour `void(State&, M)` (or
sender-returning equivalent) contributes `M` as a variant alternative; duplicate alternatives
are an error at construction.

### 4.3 Serial dispatch

The actor spawns one *driver* sender into the supplied scope. The driver loops on
`mailbox.next()` (a sender from the queue), dispatches the dequeued message via `std::visit`
over the behaviour set, awaits the behaviour's returned sender, then loops.

The driver runs on the `Inline` scheduler (or whichever scheduler the caller specified at
construction — defaulting to `Inline`); the queue's MPSC discipline guarantees that
behaviours execute serially with respect to one another, so `State&` is access-safe without
any per-actor mutex.

### 4.4 Cancellation

A stop request on the actor's enclosing scope:

1. Fires the driver's stop-callback through L5 §8.
2. The driver's `mailbox.next()` op-state observes the stop and completes with
   `set_stopped`; the driver's `let_value` chain unwinds; the driver's spawn-receiver in the
   scope decrements the count.
3. Outstanding `send(...)` futures (from `spawn_future`) complete with `set_stopped` because
   their op-states' stop-tokens are the actor's stop-token.

There is no "actor.shutdown()" method — destruction (via scope drain) is shutdown.

### 4.5 Completion signatures of `send(m)`

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(/* return type of behaviour for M */),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()
>
```

`cast(m)` is fire-and-forget; it returns `void` and the message's completion is observable
only through the actor's state changes (or `Diagnostics`).

### 4.6 LOC budget

The header is ~50 LOC: the `Actor` class template, the variant-alternative deduction
metafunction (one `consteval` walk over the behaviour pack), the driver loop (one
`exec::repeat_until` over `let_value`). Internal state is exactly the mailbox + the user's
`State` + a stop-token; no extra primitive.

### 4.7 Worked example

```cpp
struct CounterState { int value = 0; };
struct Increment    { int by; };
struct Decrement    { int by; };
struct Read         {};

Async::Structured::Scope<> scope;

auto a = Patterns::actor(scope, CounterState{},
    [](CounterState& s, Increment m) -> stdexec::sender_of<set_value_t(int)> {
        s.value += m.by;
        return stdexec::just(s.value);
    },
    [](CounterState& s, Decrement m) -> stdexec::sender_of<set_value_t(int)> {
        s.value -= m.by;
        return stdexec::just(s.value);
    },
    [](CounterState& s, Read) -> stdexec::sender_of<set_value_t(int)> {
        return stdexec::just(s.value);
    });

a.cast(Increment{5});
a.cast(Increment{3});
int v = co_await a.send(Read{});  // 8
```

The actor is bound to `scope`'s lifetime; cancelling the scope cancels the driver and any
in-flight `send` futures.

---

## 5. `reactive` — combinators on `Stream<T>`

**Header:** `Mashiro/Async/Patterns/Reactive.h`
**Model:** §6.5 reactive / push-driven, on top of §6.3 streams.

### 5.1 Form

```cpp
namespace Mashiro::Async::Patterns::reactive {

    template<class T>
    [[nodiscard]] auto debounce(Coro::Stream<T> s, std::chrono::nanoseconds dur);

    template<class T>
    [[nodiscard]] auto throttle(Coro::Stream<T> s, std::chrono::nanoseconds dur);

    template<class A, class B, class Combiner>
    [[nodiscard]] auto combine_latest(Coro::Stream<A> a, Coro::Stream<B> b, Combiner f);

    template<class T, class U, class Mapper>
    [[nodiscard]] auto switch_map(Coro::Stream<T> s, Mapper f /* T -> Stream<U> */);

} // namespace Mashiro::Async::Patterns::reactive
```

Each combinator returns a `Stream<U>` whose subscription drives the upstream(s) and applies
the combinator's logic to produce downstream elements.

### 5.2 Completion signatures (per element)

`Stream<T>` (Subagent C) has the per-element shape
`stdexec::sender_of<set_value_t(std::optional<T>)>` for `next()`. The combinators preserve
that shape, with `set_stopped` on cancellation. The Stream's own end-of-stream is
`set_value(std::nullopt)`.

### 5.3 Cancellation

A consumer's stop-token cancels the combinator's `next()` op-state, which cancels its
upstream(s)' next-op-states transitively. Standard L5 §8 chain; no bespoke logic.

### 5.4 Per-combinator LOC

Each combinator is ≤ 30 LOC of header. They use `L3` adaptors directly:

- **`debounce`** — a `let_value` over `next()` followed by `timeout(dur)`; if the timeout
  fires before the next element arrives, emit the buffered element; otherwise replace the
  buffer.
- **`throttle`** — a `let_value` over `next()` that drops elements arriving within `dur` of
  the last emitted element.
- **`combine_latest`** — internally holds the latest `A` and the latest `B`; emits whenever
  either upstream produces, using the supplied `Combiner` over the latest pair. The
  internal storage is a single `std::pair<std::optional<A>, std::optional<B>>` in the
  combinator's op-state; no heap allocation.
- **`switch_map`** — `let_value` that, on each upstream `T`, cancels the previous inner
  `Stream<U>` (via stop-source request) and subscribes to the new one. The previous inner's
  op-state unwinds through L5 §8.

### 5.5 Why these four

These four are the canonical reactive combinators that *cannot* be expressed as a fold of L3
adaptors (they need internal state proportional to the combinator, not per-element). Other
combinators (`map`, `filter`, `take`, `take_while`, `merge`, `concat`) are L4 `Stream`
methods (Subagent C) or L3 adaptors (`then`, `filter`); they don't earn their own L6 entries.

### 5.6 Worked example

```cpp
auto raw_input = stream::from_channel(input_channel);
auto debounced = reactive::debounce(std::move(raw_input), 200ms);

co_await spawn_stream_consumer(nursery, std::move(debounced),
    [](InputEvent e){ return commit_input(e); });
```

A burst of 50 input events within 200ms produces *one* downstream emission (the last); the
consumer commits exactly one event.

---

## 6. `fork_join(fn1, fn2, ..., reduce)`

**Header:** `Mashiro/Async/Patterns/ForkJoin.h`
**Model:** §6.4 nursery + a typed reduce step.

### 6.1 Form

```cpp
namespace Mashiro::Async::Patterns {

    // Fork: spawn each fn into the nursery, await all, then apply reduce
    // to the tuple of results.
    template<class Reduce, class... Fns>
    [[nodiscard]] auto fork_join(Structured::Nursery<>& n,
                                  Reduce reduce,
                                  Fns... fns);

} // namespace Mashiro::Async::Patterns
```

Each `Fn` is a nullary sender-producing function — `Fn() -> sender_of<set_value_t(T_i)>`. The
`Reduce` callable is `Reduce(T_1, T_2, ..., T_n) -> R`. The pattern returns a sender whose
value channel is `set_value_t(R)`.

### 6.2 Completion signatures

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(R),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()
>
```

### 6.3 Cancellation and error propagation

`fork_join` is *exactly* a `Nursery::spawn_future` over each `fn`, then a
`stdexec::when_all` of the futures, then a `then(reduce)`. So:

- Any child failing cancels its siblings (Nursery's first-error-wins).
- A parent stop cancels every child via L5 §8.
- The reduce step runs only on the all-success path.

### 6.4 LOC budget

The header is ~30 LOC. The pattern is a fold-expression over `nursery.spawn_future(fn())`,
captured into a `std::tuple`, fed to `stdexec::when_all`, then `then(apply(reduce, _))`.

### 6.5 Worked example

```cpp
co_await with_nursery([&](auto& n) {
    return Patterns::fork_join(n,
        [](int a, int b, std::string c) -> Result {
            return Result{a + b, std::move(c)};
        },
        []{ return compute_a(); },     // sender-of<int>
        []{ return compute_b(); },     // sender-of<int>
        []{ return compute_c(); });    // sender-of<string>
});
```

If `compute_b` fails, `compute_a` and `compute_c` are cancelled; the outer sender completes
with `set_error`. If the parent of `with_nursery` is cancelled, all three children cancel.

---

## 7. `scatter_gather(input_range, work_fn, gather_fn)`

**Header:** `Mashiro/Async/Patterns/ScatterGather.h`
**Model:** §6.1 over `bulk` (scatter), then a reduce (gather).

### 7.1 Form

```cpp
namespace Mashiro::Async::Patterns {

    // Scatter every element of input through work_fn (running in parallel on
    // sched), gather every result via gather_fn (running serially on the
    // caller's continuation).
    template<Concepts::BulkScheduler Sched,
             std::ranges::random_access_range R,
             class Work,
             class Gather>
    [[nodiscard]] auto scatter_gather(Sched sched,
                                       R&& input,
                                       Work work_fn,    // T -> U
                                       Gather gather_fn /* span<U> -> R */);

} // namespace Mashiro::Async::Patterns
```

### 7.2 Completion signatures

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(R),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()
>
```

### 7.3 Cancellation

Same as `parallel_for` for the scatter phase; the gather phase runs only on full success and
is uncancellable (it is a single function call). A stop arriving during scatter cancels the
remaining bulk iterations and the pattern completes with `set_stopped` without ever invoking
`gather_fn`.

### 7.4 Internal state — and why this is more than `parallel_for + reduce`

`scatter_gather` owns the **gather buffer** — a `std::vector<U>` of size
`std::ranges::size(input)` allocated once at scatter start. Each `work_fn` invocation writes
its result into the buffer slot keyed by its iteration index; `gather_fn` consumes the full
buffer.

`parallel_for + reduce` would require the user to allocate the buffer themselves *and* synchronise
writes (because `parallel_for` does not contract per-iteration disjoint output). `scatter_gather`
encapsulates both: the allocation policy is documented (one allocation, sized at start, freed
when the outer sender completes), and the per-iteration writes are disjoint by construction
(slot index = iteration index).

The buffer is the *only* internal state, and the umbrella spec's §8.4 constraint — "if a
pattern needs internal state beyond a `Scope`, justify it" — is satisfied by the gather
semantics: without the buffer, the pattern is just `parallel_for`.

### 7.5 LOC budget

The header is ~40 LOC: one function template that constructs the buffer, calls
`bulk(... [&buffer, &work_fn](size_t i){ buffer[i] = work_fn(input[i]); })`, then chains
`then([&](){ return gather_fn(std::span{buffer}); })`.

### 7.6 Worked example

```cpp
auto histogram = co_await Patterns::scatter_gather(
    tbb_sched(),
    pixel_buffer,
    [](Pixel p) -> uint32_t { return luminance_bucket(p); },
    [](std::span<uint32_t> buckets) -> Histogram {
        Histogram h;
        for (auto b : buckets) ++h.bins[b];
        return h;
    });
```

Scatter runs in parallel via TBB's `bulk` lowering; gather runs once on the caller's
continuation. Cancellation during scatter completes the outer sender with `set_stopped`
without entering gather.

---

## 8. Cancellation behaviour summary

| Pattern           | Source of cancellation        | Cancel propagates via                            |
|-------------------|-------------------------------|--------------------------------------------------|
| `parallel_for`    | Caller's stop_token           | `bulk`'s scheduler-specific cancel               |
| `pipeline`        | Caller's stop_token           | Nursery → every stage → queues drain             |
| `actor`           | Scope's stop_source           | Driver's mailbox.next() observes stop            |
| `reactive::*`     | Caller's stop_token           | Stream::next() observes stop, transitively       |
| `fork_join`       | Nursery's stop_source         | Nursery's per-child cancel (L5 §8)               |
| `scatter_gather`  | Caller's stop_token           | `bulk`'s scheduler-specific cancel               |

In every row the propagation goes through stdexec primitives with no bespoke logic. The L5
§8 diagram is the same diagram for every pattern; only the source of the initial
`request_stop` differs.

---

## 9. Allocation summary

| Pattern           | Allocations                                                              |
|-------------------|---------------------------------------------------------------------------|
| `parallel_for`    | None. `bulk`'s op-state is sender-local.                                  |
| `pipeline`        | One per internal queue (`MpscQueue<T, N>`) at construction; queues are amortised. |
| `actor`           | One for the mailbox; one per `spawn_future` call (op-state + future state). |
| `reactive::*`     | None. Combinator state lives in the sender's op-state.                    |
| `fork_join`       | One per child via `spawn_future` (op-state); when_all is sender-local.    |
| `scatter_gather`  | One — the gather buffer.                                                  |

Per the umbrella spec's §5.3, every allocation above is queryable via
`get_allocator(env)` from the receiver's environment, so pool / arena / debug allocators
plug in without changing pattern code.

---

## 10. Cross-pattern composition test

The umbrella spec's §8.4 demands a worked example showing `parallel_for` over a `pipeline` over
a `Stream<T>`, and confirming that cancellation flows correctly through the composition. This
section is that example.

### 10.1 Scenario

A camera streams frames as `Stream<Frame>`. Each frame goes through a 3-stage pipeline
(decode → denoise → quantise) producing `QuantFrame`. Each `QuantFrame` is then split into
tiles and `parallel_for` runs an encoder per tile.

### 10.2 Code

```cpp
Async::Coro::Task<void> process_camera(Camera cam,
                                       Async::stop_token parent) {
    using namespace Async::Patterns;
    using namespace Async::Structured;

    Scope<ScopeTag{"camera"}> outer(parent);

    co_await with_nursery<ScopeTag{"camera-nursery"}>(
        [&](auto& nursery) -> Async::Coro::Task<void> {

            auto frames = stream::from_camera(cam);

            // 3-stage pipeline running inside the nursery.
            auto quant_pipe = pipeline(nursery, scheduler,
                Stage<Frame>{
                    .fn = [](Frame f){ return decode(f); },
                    .policy = BackpressurePolicy::Drop_Oldest,
                    .capacity = 8 },
                Stage<DecodedFrame>{
                    .fn = [](DecodedFrame f){ return denoise(f); },
                    .policy = BackpressurePolicy::Wait,
                    .capacity = 4 },
                Stage<DenoisedFrame>{
                    .fn = [&](DenoisedFrame f){ return quantise(f); },
                    .policy = BackpressurePolicy::Wait,
                    .capacity = 2 });

            // The pipeline's terminal stage feeds quantised frames into a
            // Stream<QuantFrame> via the L6 bridge (see §3.8). Per-frame work
            // then runs parallel_for over the tiles.
            auto quants = pipeline_as_stream(quant_pipe);

            nursery.spawn_stream_consumer(std::move(quants),
                [&](QuantFrame qf) -> Async::Coro::Task<void> {
                    auto tiles = qf.tiles();
                    co_await parallel_for(
                        tbb_sched(),
                        tiles,
                        [&](Tile& t){ encode_tile(t); });
                });

            co_await nursery.on_empty();
        });
}
```

### 10.3 Cancellation analysis

A stop request on `parent` flows through the chain:

1. **`parent` → `outer` (Scope)**: `parent_stop_callback` registered by `outer`'s
   `ParentStopBridge` fires; `outer.own_stop_.request_stop()` flips.
2. **`outer` → `with_nursery`'s sender**: the nursery's stop-token is a child of `outer`'s
   stop-token by construction (the nursery is a sender whose receiver's environment is
   derived from `outer`'s). The nursery's stop-source fires.
3. **Nursery → `quant_pipe`**: the pipeline's outer sender registered an
   `inplace_stop_callback` on the nursery's stop-token at start. The callback flips the
   nursery's stop-source for the pipeline. Each stage's sender op-state observes this and
   completes with `set_stopped`. The internal queues' producers and consumers see the stage
   senders cancel and themselves complete with `set_stopped`.
4. **Nursery → `spawn_stream_consumer`**: the consumer's `let_value` chain over
   `quants.next()` is cancelled the same way; `quants.next()` is a stream over the pipeline's
   output, so its cancellation propagates back into the pipeline (step 3).
5. **Stream consumer → `parallel_for`**: each in-flight `parallel_for` invocation has a
   stop-token derived from the consumer's stop-token. TBB's `task_group_context` is cancelled
   via `cancel_group_execution()`; in-flight tile encoders unwind cooperatively. New tiles are
   not dispatched.
6. **Children settle, `nursery.on_empty()` resumes** → `with_nursery`'s outer sender
   completes with `set_stopped` (per Nursery's first-error-or-stop rule, §6.3 of L5).
7. **`outer`** is unaffected here (no spawned senders were directly in `outer`); its
   destructor in `process_camera`'s frame asserts `alive_count() == 0` and runs cleanly.

Every step is a stdexec primitive — `inplace_stop_source`, `inplace_stop_callback`,
`request_stop`, `counting_scope::on_empty`, scheduler-specific cancel — and every transition
is the L5 §8 chain applied at one level deeper. The composition is *transparent* to
cancellation: each pattern in the chain participates by virtue of taking and honouring a
stop-token, and no pattern needs special composition logic for cancellation to flow
correctly.

### 10.4 Allocation analysis

End-to-end, the composition allocates:

- One ring buffer for `outer` (lazy on first spawn — but `outer` itself doesn't spawn here,
  so zero).
- One ring buffer for the nursery (lazy on first spawn — fires when `pipeline` and
  `spawn_stream_consumer` register).
- Three queues in `quant_pipe` — one per stage.
- One coroutine frame for the stream-consumer task (HALO may elide).
- One coroutine frame for `process_camera` itself (HALO may elide).

All queries via `get_allocator(env)`; tests verify with `Diagnostics::AllocCheck` (cross-cutting).

---

## 11. Interaction with L7 user extensions

A user-provided pattern (an L7 contribution) must:

1. Take its scheduler(s) as parameter(s); not own one.
2. Take a `Scope&` / `Nursery&` parameter if it spawns; not own one.
3. Declare its completion signatures.
4. Honour stop-tokens in the receiver environment (mandated by the `Cancellable`
   annotation).
5. Document its allocation policy via `[[=Async::Allocates{...}]]`.

The five rules above are the L6 invariants restated as the L7 extension contract. Subagent E's
spec (`07-extension.md`) elaborates on the ABI details.

---

## 12. Decisions and alternatives

- **Why is `pipeline` not built on `Stream<T>` end-to-end?** It could be — every stage's
  output is structurally a `Stream`. But threading per-element through stream-of-stream
  combinators introduces an extra `optional<T>` and a per-element sender allocation that
  we don't need. The pipeline owns its queues directly, with element types not wrapped in
  `optional`; end-of-stream is a stage-completion event, not a per-element value.
- **Why does `actor` use `Inline` by default rather than the platform scheduler?** Actors
  are *serial*, not *thread-pinned*. Using `Inline` lets the actor run wherever its
  message-sending caller runs (trampoline-style), with the MPSC mailbox preserving
  serialisation. If a user wants pinning, they pass `platform_sched()` at construction.
- **Why doesn't `fork_join` accept a heterogeneous-tuple input directly (e.g.,
  `fork_join(std::tuple{fn1, fn2, fn3}, reduce)`)?** Variadic packs compose better with
  reflection-driven trait queries and produce nicer diagnostics. The tuple form is
  trivially `fork_join(n, reduce, std::get<0>(t), std::get<1>(t), ...)` and not worth a
  separate overload.
- **Why is `scatter_gather`'s buffer not user-supplied?** A user-supplied buffer would
  require us to take a `std::span<U>` of pre-correct size and trust the caller — at which
  point the pattern is just `parallel_for`. The whole point is encapsulation of the buffer
  lifetime.
- **Why no `pipeline` rewrite for `<= 4` stages on TBB?** Below the threshold, TBB's
  `flow::graph` indirection overhead exceeds the per-stage stdexec receiver cost we'd
  save. Above the threshold, the situation reverses. The threshold is empirical — it can
  be revisited per release; the rewrite *predicate* is in the TBB domain, and changing the
  threshold is a one-line patch.
- **Why no `select` pattern?** `select` (race / first-of-N) is L3
  (`race(s1, s2, ...)` in `03-adaptors.md`). It does not earn an L6 entry because it has
  no scope, no fan-out beyond the senders it receives, and no pattern-specific cancellation
  semantics — it is a sender combinator, full stop.
- **Why no `barrier` / `latch` / `semaphore` patterns?** These are synchronisation
  primitives, not patterns. They belong in the future
  `Mashiro/Async/Synchro.h` (an L3 / L4 primitive set) if a use case crystallises. Adding
  them now without a justifying use case would clutter L6.

---

## 13. Glossary

- **parallel_for** — sender expression over `bulk` for data-parallel application of `fn`.
- **pipeline** — multi-stage push-driven flow with per-stage backpressure policy.
- **actor** — `State` plus a behaviour set, dispatched serially via an `MpscQueue` mailbox.
- **reactive** — combinators on `Stream<T>` that need state proportional to the combinator
  (`debounce`, `throttle`, `combine_latest`, `switch_map`).
- **fork_join** — spawn-N-and-reduce over a `Nursery`, with a typed reduce step.
- **scatter_gather** — `parallel_for` plus an owned gather buffer plus a serial reduce.
- **BackpressurePolicy** — pipeline stage's queue discipline (`Drop_Oldest`, `Drop_Newest`,
  `Wait`, `Latest_Only`).
- **Stage** — one unit of a `pipeline`; a sender-producing function plus its policy/capacity.
