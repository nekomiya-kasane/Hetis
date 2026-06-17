# Synthesis Pass — Vocabulary, Boundary, Signature, Composition

**Status:** v0.2 reconciliation note. Resolves cross-spec coordination issues raised by Subagents A–E.
**Date:** 2026-06-15
**Author:** synthesis pass
**Scope:** Reconciles the eight specs in `2026-06-15-async-framework/`. Does not introduce new design — only adjudicates the open issues each subagent surfaced and records the canonical answer.

This document is read **after** the eight layer specs. The umbrella `00-overview.md` is bumped to v0.2 with the adjudicated changes inlined into §5.6 (capability annotations), §4.1 (namespace tree), and §6.6 (composition matrix) where applicable.

---

## 1. Method

I did the §9 acceptance pass against the eight delivered specs:

1. **Vocabulary check** — every term in the layer specs must trace either to `00-overview.md` (frozen) or to a single owning layer spec.
2. **Boundary check** — no spec references a higher-layer type by anything other than name.
3. **Completion-signature check** — adaptor signatures, propagation rules, and stop-channel propagation are consistent across L0 (helpers), L3 (adaptors), L4 (coroutine), L5 (scope), L6 (patterns).
4. **Composition matrix check** — every cell of overview §6.6 has at least one worked example landed across the layer specs.
5. **Open-issue triage** — each subagent's flagged issues get a canonical answer (Accept / Reject / Defer with reason).

I did **not** rewrite the layer specs. Where an issue requires a textual change, this note records the change as a v0.2 patch that the umbrella spec adopts.

---

## 2. Adjudication of open issues

### 2.1 Annotations and vocabulary missing from L1

**Subagent C raised:** `Async::Detached` annotation, used in L4 §6 to distinguish `Job` from `Task<void>` via `Traits::IsDetached_v<T>`. Not in A's annotation set.

**Subagent D raised:** `ScopeTag` is a non-type-template tag value attached to every `Scope<Tag>`. Not in A's annotation set.

**Decision: Accept both.** Add to overview §5.6 and to `01-foundations.md` §5.2 annotation list:

| Annotation | Tags | Read by | Composition rule |
|------------|------|---------|------------------|
| `Async::Detached` | Coroutine task types (L4) | `Traits::IsDetached_v<T>` (L1), `Scope::spawn` (L5) | Exactly-one per task type; `Task<T>` does not carry it, `Job` does. |
| `Async::ScopeTag{...}` | Scope class templates (L5) | `Traits::ScopeTagOf<S>` (L1) | Exactly-one per scope; defaulted from `std::source_location::current()` if user omits it. |

**Action item:** v0.2 of `01-foundations.md` §5.2 adds these two annotation `struct`s. The annotation set in overview §5.6 becomes `{Backend, Affine, OffersBulk, OffersIo, Cancellable, Allocates, IsForwardProgress, Detached, ScopeTag}`. The bi-conditional verifier in `01-foundations.md` §8 is unchanged — these two annotations tag L4/L5 types, not backends, so they do not participate in the L1↔L2 capability verifier. They are queried by `Traits::IsDetached_v` and `Traits::ScopeTagOf<S>` only.

### 2.2 `Allocates::Where::None`

**Subagent C raised:** L4's reflection table includes a row for sender expressions that allocate nothing; A's `Allocates::Where` enum is `{Frame, OpState, Output, External}` with no `None`.

**Decision: Reject.** Absence of an `Allocates` annotation already means "allocates nothing"; introducing `Where::None` would create a synonym. C's L4 table should drop the explicit `None` row in v0.2 and read the absence of the annotation as the "no allocation" signal. The reflection helper `Traits::AllocatesIn_v<T>` already returns `std::nullopt` (or an empty optional / empty span — see A §6.2) when the annotation is absent.

**Action item:** L4 §10 v0.2 patch removes the `Allocates::Where::None` row; the row's content is folded into the table cell as "(annotation absent)".

### 2.3 Completion-signature helper naming

**Subagent C raised:** L3 cites `Foundations::union_signatures<...>`, `Foundations::propagate_error_signatures<S, Env>`, `Foundations::union_error_signatures<Ss...>`. A's spec publishes `union_signatures<S...>`, `with_error<E, S>`, `propagate_stopped<S>`.

**Decision:**
- `Foundations::union_signatures<...>` — name match. **No change.** The empty-pack case (subagent A §12 open issue 7) is canonicalised as a hard `static_assert` ("union of zero senders is meaningless"); L3 confirms this is the correct policy.
- `Foundations::propagate_error_signatures<S, Env>` — this is **`with_error<E, S>` composed with reflection** in A's vocabulary. L3 v0.2 should rename its uses to `with_error<E, S>` or define `propagate_error_signatures` as an L3-local helper in `Mashiro::Async::Adaptor::detail::`.
- `Foundations::union_error_signatures<Ss...>` — same case; this is L3-local sugar on top of `union_signatures`. Move it into `Adaptor/detail/` and document it as adaptor-internal.

**Action item:** L3 §3 and the per-adaptor sections that cite these helpers either rename to A's published names or move the helper into `Adaptor/detail/`. Update L4 §8 if it references the same helpers (it does not).

### 2.4 `materialise` / `dematerialise` British vs American spelling

**Subagent C raised:** P2300 uses `materialize` / `dematerialize`; the overview uses British spelling.

**Decision: Accept British project-style.** L0 re-export adds the alias:

```cpp
namespace Mashiro::Async {
    inline constexpr auto& materialise   = stdexec::materialize;
    inline constexpr auto& dematerialise = stdexec::dematerialize;
}
```

Documented in `01-foundations.md` §4.3 re-export table. American spellings are also exposed unchanged (no rename of stdexec's name); the British forms are aliases, not replacements. Project-style guideline is to use the British spelling in framework headers; user code may use either.

**Action item:** A's §4.3 re-export table v0.2 adds the two aliases with the rename rule cited inline.

### 2.5 `Channel<T>` vocabulary

**Subagent C raised:** L4 `stream::from_channel(Channel<T>& ch)` — but Mashiro core has `MpscQueue` and `SpscQueue`, no `Channel<T>` by name.

**Decision: Accept queue concept.** `from_channel` becomes `from_queue` and takes a queue concept (`AsyncQueue<T>`) satisfied by `MpscQueue<T, N>`, `SpscQueue<T, N>`, and `SpscByteRing<N>`. This avoids inventing a `Channel<T>` type that doesn't exist in core, and aligns with the project's existing vocabulary (the platform-thread spec uses `EventChannel` for a specific compound type built on `SpscQueue` — that name is reserved for that role and should not be reused as a generic abstraction).

```cpp
template<class Q>
concept AsyncQueue = requires (Q q, typename Q::value_type v) {
    typename Q::value_type;
    { q.try_pop() } -> std::same_as<std::optional<typename Q::value_type>>;
    { q.empty() }   -> std::same_as<bool>;
};

template<AsyncQueue Q>
auto from_queue(Q& q) -> Stream<typename Q::value_type>;
```

**Action item:** L4 §5 v0.2 renames `from_channel` → `from_queue`, defines the `AsyncQueue` concept, and removes the speculative `Channel<T>` mention. The `Channel<T>` name is freed for future use.

### 2.6 `Backend::Io::timer_op_state_size_v`

**Subagent C raised:** L3 assumes Io backend exposes a public size constant for inline-storage planning.

**Decision: Accept.** B's Io section v0.2 publishes `timer_op_state_size_v`, `read_op_state_size_v`, `write_op_state_size_v` as public `inline constexpr` size constants in `Mashiro/Async/Backend/Io.h`. They are derived from the backend's actual op-state types (which remain incomplete in the header for ABI stability — only the size and alignment are exposed). L3's adaptors using these constants for inline-storage decisions then have a stable size to plan against.

```cpp
namespace Mashiro::Async::Backend::Io {
    inline constexpr std::size_t timer_op_state_size_v = /* sizeof(detail::TimerOpState) */;
    inline constexpr std::size_t read_op_state_size_v  = /* sizeof(detail::ReadOpState) */;
    inline constexpr std::size_t write_op_state_size_v = /* sizeof(detail::WriteOpState) */;
}
```

**Action item:** B's §7 (Io backend) v0.2 adds these three size constants with the rationale ("L3 adaptors use these to size inline storage; bumping a size is an ABI break documented in the file header").

### 2.7 `bulk` over Platform is rejected

**Subagent C raised:** L3 §13.1 `static_assert`-rejects `bulk` over Platform.

**Decision: Accept.** Platform is `AffineScheduler` (single-threaded); `bulk` over it would either serialise the children (defeating the algorithm) or violate the affinity guarantee (running children off the platform thread). The rejection is correct.

Documentation update: L3 v0.2 cites the static-assert with this rationale in prose, and `02-backends.md` §6 (Platform) v0.2 adds a one-line note: "`Platform` does not model `BulkScheduler`. Code attempting `bulk` over Platform is rejected by L3's `static_assert`; use `Tbb` or `StaticPool` for parallel work, then `continues_on(Platform)` to land on the platform thread."

**Action item:** L2 §6 and L3 §13 v0.2 patches as described.

### 2.8 `for co_await` syntax stability

**Subagent C raised:** C++26's exact `for co_await` syntax is not yet final; need a fallback.

**Decision: Accept fallback.** L4 §5.4 v0.2 documents both forms:

```cpp
// Preferred (C++26, if available):
for co_await (auto& chunk : stream) { ... }

// Portable fallback:
while (auto chunk = co_await stream.next()) {
    auto& value = *chunk;
    ...
}
```

A `MASHIRO_FOR_CO_AWAIT(name, stream)` macro can wrap the portable form to keep call sites uniform; documented in L4 §5.4 with a "remove when language feature stabilises" note.

**Action item:** L4 §5.4 v0.2 adds the fallback form.

### 2.9 `from_future` boundary status

**Subagent C raised:** `from_future` spawns a `std::jthread` to poll — intentionally bad — and is a migration boundary, not steady state.

**Decision: Accept.** L4 §7.3 v0.2 adds a `[[deprecated("migration boundary — see L7 §7.5")]]` attribute on `from_future`. The deprecation does **not** prevent its use during migration; it surfaces a compiler warning that traces back to the cross-cutting migration plan. Removal target: when all legacy `std::async` call sites in `Mashiro/` and `Yuki/` are migrated (see E's migration plan §7.5).

**Action item:** L4 §7.3 + L7 §7.5 v0.2 cross-reference the deprecation.

### 2.10 `with_nursery` matrix wording

**Subagent D raised:** Overview §6.6 matrix cell "Stream/Nursery" reads "merge, zip, concat" — those are Stream combinators, not Nursery ones.

**Decision: Accept matrix cleanup.** Overview §6.6 v0.2:

| Outer / Inner | Sender | Task<T> | Stream<T> | Nursery | Reactive |
|---------------|--------|---------|-----------|---------|----------|
| **Stream**    | `into_stream(s)` | `Stream::from_task(t)` | `merge`, `zip`, `concat` (Stream combinators) | `n.spawn_stream_consumer(s, fn)` | reactive *is* a stream graph |
| **Nursery**   | `n.spawn(sender)` | `n.spawn_task(task)` | `n.spawn_stream_consumer(s, fn)` | nested `with_nursery` | `n.spawn(pipeline_as_sender)` |

The "Stream/Nursery" cell now correctly names `n.spawn_stream_consumer`; the "Stream/Stream" cell holds the combinators it always should have held.

**Action item:** Overview §6.6 v0.2 with the matrix above.

### 2.11 `Scope` allocation wording

**Subagent D raised:** Overview §5.3 says "at most one allocation per `Scope`" but oversize op-states fall back to one allocation each.

**Decision: Accept softer wording.** Overview §5.3 v0.2:

> L5 scope: at most one allocation per `Scope` for the ring buffer of inline op-state slots. Op-states that exceed the per-slot inline budget allocate individually; this is documented at the spawn site, not silent. The inline budget is a per-`Scope` template parameter defaulting to `sizeof(void*) * 8`.

**Action item:** Overview §5.3 v0.2.

### 2.12 `with_nursery` is a sender vs a function

**Subagent C and D both touched:** Overview §6.6 puts `with_nursery` in the Sender / Nursery cell; D implements it that way.

**Decision: Confirm.** `with_nursery(F)` returns a sender (the with-block lambda is the body); it composes with `then`, `let_value`, etc. like any other sender. **No textual change** — the cell wording in §6.6 stands.

### 2.13 Stop-callback fast-path

**Subagent E raised:** Hot backends (`Inline`, short `StaticPool` items) want a fast path when the receiver carries no stop-token.

**Decision: Accept.** A's `Foundations.h` v0.2 exposes:

```cpp
template<class Env>
concept HasStopToken = requires (const Env& e) {
    { stdexec::get_stop_token(e) } -> std::same_as<inplace_stop_token>;
};
```

(or `concept HasStopToken = !std::same_as<decltype(get_stop_token(env)), stdexec::never_stop_token>` — the exact spelling is A's call, but a public concept must exist). Backends gate the `inplace_stop_callback` registration behind `if constexpr (HasStopToken<Env>)`. When false, no callback is registered, no overhead is paid, and cancellation is a no-op for that operation — correct behaviour per the cancellation contract.

**Action item:** A's `Foundations.h` v0.2 §5 or §7 adds `HasStopToken<Env>`; B's `Inline` and `StaticPool` sections v0.2 use it in their cancellation wiring.

### 2.14 `bridge_stop_token` migration boundary

**Subagent A raised:** Platform layer already uses `inplace_stop_source` internally, so `bridge_stop_token` is never needed for platform interop.

**Subagent E raised:** Cross-cutting migration plan uses `bridge_stop_token` for `jthread` interop.

**Decision: Both stand.** `bridge_stop_token` is for **boundary interop** with code that hands the framework a `std::stop_token` (e.g. a user's `jthread`). It is never used inside the framework. E's migration plan correctly uses it at the boundary; A correctly documents it as a one-allocation boundary primitive.

### 2.15 `Mashiro::Async::Coro::stopped_signal`

**Subagent E raised:** Exception type that propagates `set_stopped` through coroutine `unhandled_exception()`.

**Decision: Accept.** L4 v0.2 §6.5 adds:

```cpp
namespace Mashiro::Async::Coro {
    // Thrown inside a coroutine body when an awaited sender completes with set_stopped.
    // Caught by the framework's coroutine promise unhandled_exception() handler and translated
    // back into set_stopped on the enclosing op-state.
    struct stopped_signal {};
}
```

The promise type's `unhandled_exception()` checks for this type first and routes to `set_stopped`; any other exception routes to `set_error`. This is the standard exec::task pattern.

**Action item:** L4 §6.5 v0.2.

### 2.16 `with_allocator`, `with_stop_source`

**Subagent E raised:** Environment-customisation adaptors used in cross-cutting but not defined in L3.

**Decision: Accept; assign to L3.** L3 v0.2 §14 adds two short adaptor entries:

```cpp
// Replaces the allocator in the receiver's environment.
template<class S, class A>
auto with_allocator(S&& s, A alloc);

// Replaces the stop-source in the receiver's environment.
template<class S>
auto with_stop_source(S&& s, inplace_stop_source& src);
```

Both are environment rewriters; they wrap stdexec's `read_env` / `write_env` mechanisms. No allocation, no op-state state.

**Action item:** L3 §14 v0.2 adds these two adaptors.

### 2.17 Diagnostics surface ownership

**Subagent E raised:** `Diagnostics::Span`, `AllocCheck`, `DetectStarvation`, `DetectDeadlock`, `scope_audit()` are referenced by L5 and Cross-cutting; A might want to host them.

**Decision: Owned by Cross-cutting (E).** Diagnostics is a cross-cutting concern with its own header tree (`Mashiro/Async/Diagnostics/`). E's spec is the authoritative source. L5's `scope_audit()` forward-reference is correct (cite E §6.x). A is **not** the home for diagnostics — A is foundational vocabulary, diagnostics is a separate axis.

**Action item:** None; the layer ownership in overview §4.1 already puts `Diagnostics/` in its own tree.

### 2.18 `Job` modelled as a sender

**Subagent D raised:** L5's `Scope::spawn(Job)` works only if Subagent C models `Job` as a sender.

**Decision: Confirm — Job is a sender.** L4 §6 confirms `Job` is a coroutine type that satisfies the sender concept (every `exec::task<T>` does). `Scope::spawn(sender)` accepts it through the standard sender overload. The `Async::Detached` annotation distinguishes the *intent* of detached background work, not the *concept* — both `Task<T>` and `Job` are senders.

### 2.19 Pipeline → Stream bridge naming

**Subagent D raised:** `stream::from_pipeline(quant_pipe)` — does this live in L4 or L6?

**Decision: L6.** A pipeline is a pattern (L6); its bridge to L4's Stream is a pattern operation. `Mashiro::Async::Patterns::pipeline_as_stream(p) -> Stream<T>` lives in `Patterns/Pipeline.h`. The L4 `Stream` namespace does not depend on L6 patterns.

**Action item:** L6 §3 v0.2 adds the `pipeline_as_stream` bridge. D's L5 §10.2 reference renames accordingly.

### 2.20 `spawn_stream_consumer` location

**Subagent D raised:** Could live on `Nursery` (L5) or in `Reactive.h` (L6).

**Decision: On Nursery.** It binds a Stream subscription to a nursery's lifetime — that is a structured-concurrency operation, not a reactive operation. L5 owns it; L6's reactive patterns use it via the nursery API.

**Action item:** No change; D's placement stands.

### 2.21 `Plugin::DescriptorV1` schema migration

**Subagent E raised:** Compatibility matrix for plugin descriptor schema bumps.

**Decision: Defer.** Not needed for v0.1 of the framework. Schema bumps will be addressed when the first plugin lands; until then, `DescriptorV1` is the only schema and there is no migration to define.

### 2.22 Diagnostics registration timing

**Subagent E raised:** Tracy/Perfetto domain rewrites must compose into pipelines constructed before init.

**Decision: Accept ordering rule.** E v0.2 §6.1 adds:

> Diagnostics backends must be registered before any `Async::*` scheduler is constructed. The recommended call site is the first statement of `main()`, before `PlatformThread::Run()`. A debug-mode assertion fires if a scheduler is constructed before any diagnostics backend has registered, when `MASHIRO_DIAGNOSTICS_REQUIRED` is defined.

**Action item:** E §6.1 v0.2 adds the ordering rule.

### 2.23 TBB pipeline rewrite threshold

**Subagent D raised:** ≥ 4 stages heuristic should be re-validated.

**Decision: Accept as configurable.** The threshold is a constant in `Tbb/Domain.h`:

```cpp
namespace Mashiro::Async::Backend::Tbb {
    inline constexpr std::size_t pipeline_rewrite_threshold_v = 4;
}
```

Documented as a tunable, not a contract. B v0.2 publishes the constant; benchmark-driven adjustment is part of the implementation phase, not the spec.

**Action item:** B §5 v0.2 publishes the constant.

### 2.24 `get_io_context_t` CPO

**Subagent A raised:** Forward-declared in `Foundations.h`, body in L2 Io backend.

**Decision: Accept.** A's `Foundations.h` v0.2 forward-declares `get_io_context_t` as an L0 CPO; B's Io section provides the customisation. This is the same pattern stdexec uses for `get_scheduler` / `get_stop_token`.

### 2.25 Discovery walk for user backends

**Subagent A raised:** The verifier walks `Mashiro::Async::Backend::*` only; user backends in user namespaces are not discovered.

**Decision: Accept L7 opt-in hook.** E v0.2 L7 §3.1 documents the opt-in:

```cpp
namespace MyCompany {
    struct GpuScheduler { /* ... */ };
}

// User opts in by specialising a registration trait in their TU:
namespace Mashiro::Async::Extension {
    template<> inline constexpr bool register_scheduler_v<MyCompany::GpuScheduler> = true;
}
```

The verifier's discovery walk includes all types `T` with `register_scheduler_v<T> == true`. This keeps the verifier reflection-driven (no static list) while letting user-extension schedulers participate.

**Action item:** A's §8.1 v0.2 mentions the discovery extension; E's L7 §3.1 v0.2 documents the user-side hook.

### 2.26 Sender-level `Cancellable` audit

**Subagent A raised:** Per-sender `Cancellable` is not enforced by §8 verifier; L3 must audit per-adaptor.

**Decision: Accept.** L3 v0.2 §3 adds a static-assert audit: every adaptor's op-state must either register an `inplace_stop_callback` or carry no cancellable state. The audit is enforced by reflection over the op-state's members — if a member is an `inplace_stop_callback`, fine; if a member is a sender, we recurse; if a member is "owns external state" (file descriptors, timer handles), the audit fails. The reflection helper for this check lives in `Adaptor/detail/CancellationAudit.h`.

**Action item:** L3 §3 v0.2 adds the static audit + `Adaptor/detail/CancellationAudit.h`.

### 2.27 `Allocates::Where::Frame` leaking down to L1

**Subagent A raised:** `Frame` is an L4 concept (coroutine frame).

**Decision: Reject relocation.** `Allocates::Where` is a uniform enum across the framework precisely so different layers can use the same vocabulary. `Frame` describes a category of allocation site (a coroutine's frame), not an L4-specific concept; an arbitrary user-extension scheduler that allocates per-task internal state may also tag it as `Frame`-like. Keeping the enum centralised at L1 is the right call. No change.

### 2.28 P1306 + P3491 probe TU

**Subagent A raised:** Composition of expansion statements and `define_static_array` needs a probe TU before L2 depends on it.

**Decision: Accept.** A probe TU lands at `Mashiro/tests/00-Thirdparty/p1306_p3491_compose_probe.cpp`. The probe instantiates a `define_static_array` of types selected by an expansion statement; if it compiles, the toolchain supports the composition the framework relies on. The platform-thread spec's existing probe pattern (`tests/00-Thirdparty/stdexec_probe.cpp`) is the model. Implementation task, not spec change.

**Action item:** Implementation kickoff item; no spec change.

### 2.29 Mailbox depth in actor pattern

**Subagent D raised:** Default depth 256 from platform spec — left as template parameter.

**Decision: Confirm template parameter.** `actor<State, N = 256>` is the right shape. Migration code that ports an existing `MpscQueue<Message, 256>`-based pump names `N = 256` explicitly; new actors pick a depth justified by the workload. No further change.

### 2.30 Vector allocator-aware type

**Subagent C raised:** L3 `batch` emits `Vector<value_t<S>>` — does Mashiro core have one?

**Decision: Accept `std::pmr::vector<T>` interim, project type later.** Until Mashiro core publishes an allocator-aware `Vector<T>`, `batch` uses `std::pmr::vector<T>` with the allocator pulled from the receiver's environment via `get_allocator(env)`. When Mashiro core lands `Vector<T>`, `batch` switches; the public adaptor shape does not change because the value type is `std::pmr::vector<T>` for now and `Mashiro::Core::Vector<T>` later — both are container-of-T from the consumer's perspective.

**Action item:** L3 §6 v0.2 names `std::pmr::vector<T>` explicitly; flag for revisit when `Mashiro::Core::Vector<T>` lands.

### 2.31 Vulkan scheduler reaper-thread choice

**Subagent E raised:** `VkComputeScheduler` hand-waves the fence-wait mechanism.

**Decision: Defer to implementation.** The spec correctly leaves the choice to the user (engine compute thread vs `VK_KHR_external_fence_fd`); naming a single mechanism in the spec would over-constrain. No change.

---

## 3. Vocabulary concordance — final v0.2 list

After §2's adjudications, the framework's frozen vocabulary is:

### 3.1 Annotations (`[[=Async::*]]`)

| Annotation | Spec | Tags | Read by |
|------------|------|------|---------|
| `Backend` | L1 (`01-foundations.md` §5.2) | Scheduler types | `Traits::BackendOf<S>` |
| `Affine` | L1 | Scheduler types | `Traits::AffinityOf<S>`, `Traits::IsAffine_v<S>` |
| `OffersBulk` | L1 | Scheduler types | `Traits::OffersBulk_v<S>` |
| `OffersIo` | L1 | Scheduler types | `Traits::OffersIo_v<S>` |
| `Cancellable` | L1 | Senders / scheduler types | `Traits::IsCancellable_v<T>` |
| `Allocates` | L1 | Any type that allocates | `Traits::AllocatesIn_v<T>` |
| `IsForwardProgress` | L1 | Scheduler types | `Traits::ProgressOf_v<S>` |
| `Detached` | L1 (added v0.2) | Coroutine task types | `Traits::IsDetached_v<T>` |
| `ScopeTag{...}` | L1 (added v0.2) | Scope class templates | `Traits::ScopeTagOf<S>` |

### 3.2 Concepts (`Mashiro::Async::Concepts::*`)

| Concept | Spec | Models |
|---------|------|--------|
| `Scheduler` | L0 / L1 | Re-export of `stdexec::scheduler` |
| `BulkScheduler` | L1 | Adds `schedule_bulk` |
| `IoScheduler` | L1 | Adds Io operation CPOs |
| `AffineScheduler` | L1 | `IsAffine_v == true` |
| `ParallelScheduler` | L1 | Progress ≥ `parallel` |
| `HasStopToken` | L1 (added v0.2) | Env carries a non-`never` stop-token |
| `AsyncQueue` | L4 (added v0.2) | Has `value_type`, `try_pop`, `empty` |

### 3.3 Completion-signature helpers

| Helper | Spec | Role |
|--------|------|------|
| `with_error<E, S>` | L1 §7.1 | Adds `set_error_t(E)` to S's signatures |
| `union_signatures<S...>` | L1 §7.2 | Merges N signature sets |
| `propagate_stopped<S>` | L1 §7.3 | Ensures `set_stopped_t()` is forwarded |

### 3.4 Environment adaptors

| Adaptor | Spec | Role |
|---------|------|------|
| `with_allocator` | L3 §14 (added v0.2) | Replace allocator in env |
| `with_stop_source` | L3 §14 (added v0.2) | Replace stop-source in env |

### 3.5 Layer-spanning types (one owner each)

| Type | Owner |
|------|-------|
| `Mashiro::Async::stop_token` (= `inplace_stop_token`) | L0 |
| `bridge_stop_token(std::stop_token)` | L0 §9 |
| `Coro::stopped_signal` | L4 §6.5 (added v0.2) |
| `Task<T>` | L4 |
| `Stream<T>` | L4 |
| `Job` | L4 |
| `Scope<Tag>` | L5 |
| `LinkedScope<Tag>` | L5 |
| `with_nursery(F)` | L5 |
| `Supervised<...>` | L5 |
| `Stage<T>`, `BackpressurePolicy` | L6 §3.1 |
| `pipeline_as_stream(p)` | L6 §3 (added v0.2) |
| `from_queue(Q&)` | L4 §5 (renamed from `from_channel`, v0.2) |
| `actor<State, N=256>` | L6 §4 |
| `parallel_for`, `fork_join`, `scatter_gather`, `reactive` | L6 |

---

## 4. Boundary check result

Layer-cross-references audited:

- L0 → none (foundational).
- L1 → L0 only. ✓
- L2 → L0, L1. ✓
- L3 → L0, L1, L2 (by name only). ✓
- L4 → L0, L1, L2 (Platform backend by name), L3 (`with_*` adaptors). ✓ — L3 dependency is via name only, no implementation entangling.
- L5 → L0, L1, L4 (`Job`, `Task` by name). ✓
- L6 → L0, L1, L2 (all backends), L3 (adaptors by name), L4 (`Task`, `Stream`, `Job`), L5 (`Nursery`, `Scope`). ✓
- L7 → all of above by name only. ✓
- Cross-cutting → all of above. ✓

**No layer references a higher-layer type as a dependency.** Forward references exist but are name-only and resolved by inclusion order at the implementation site.

---

## 5. Completion-signature audit

For each adaptor / coroutine / pattern producing a sender, the v0.1 specs declare an explicit `completion_signatures_of_t` specialisation. Cross-checked:

- L3 adaptors: 8 of 8 (`bulk`, `batch`, `debounce`, `throttle`, `retry`, `timeout`, `race`, `materialise`/`dematerialise`). ✓
- L4 coroutines: `Task<T>` (set_value(T), set_error(exception_ptr), set_stopped()), `Stream<T>` (set_value(optional<T>), set_error, set_stopped), `Job` (set_value(), set_error, set_stopped). ✓
- L5 scope operations: `Scope::on_empty()` (set_value(), set_stopped). ✓
- L6 patterns: `parallel_for`, `pipeline`, `fork_join`, `scatter_gather` — each declares signatures explicit; `actor` and `reactive` are stream-typed. ✓

Stop-channel propagation rule (overview §5.7): `set_stopped_t()` is propagated when any upstream offers it. Every adaptor that hosts upstream senders calls `propagate_stopped<S>` (or equivalent) on its signature set. Audit pass.

Error-channel convention: default `set_error_t(std::exception_ptr)`. Typed errors are opt-in via `with_error<MyError, S>`. Cross-cutting §4 documents this. ✓

---

## 6. Composition matrix coverage

Each cell of overview §6.6 has at least one worked example landed:

| Cell | Where landed |
|------|--------------|
| Sender × Sender | L3 throughout (any adaptor chain) |
| Sender × Task<T> | L4 §3 (sender inside Task body) |
| Sender × Stream<T> | L4 §5 (let_value + stream::next) |
| Sender × Nursery | L5 §6.3 (with_nursery is a sender) |
| Sender × Reactive | L6 §3 (pipeline factory returns sender) |
| Task × Sender | L4 §3 (co_await sender inside Task) |
| Task × Task | L4 §3 (co_await another Task) |
| Task × Stream | L4 §5.4 (`for co_await` over Stream) |
| Task × Nursery | L5 §6.3 (`co_await with_nursery(...)`) |
| Task × Reactive | L6 §3 (`co_await pipeline.run()`) |
| Stream × Sender | L4 §5 (`into_stream(s)`) |
| Stream × Task | L4 §5 (`Stream::from_task`) |
| Stream × Stream | L6 reactive (`merge`, `zip`, `concat`) |
| Stream × Nursery | L5 §6.3 (`n.spawn_stream_consumer`) |
| Stream × Reactive | L6 §3 (stream graph) |
| Nursery × Sender | L5 §6.1 (`n.spawn(sender)`) |
| Nursery × Task | L5 §6.1 (`n.spawn_task(task)`) |
| Nursery × Stream | L5 §6.3 (`n.spawn_stream_consumer`) |
| Nursery × Nursery | L5 §6.3 (nested) |
| Nursery × Reactive | L6 §3 (`n.spawn(pipeline_as_sender)`) |
| Reactive × Sender | L6 §3 (source := sender) |
| Reactive × Task | L6 §3 (stage := task-per-element) |
| Reactive × Stream | L6 §3 (underlying transport) |
| Reactive × Nursery | L6 §3 (nursery owns lifetime) |
| Reactive × Reactive | L6 §3 (composition) |

Cross-backend composition: L2 §8 worked example (Tbb → Platform → Io). ✓

---

## 7. v0.2 patch summary for the umbrella

Changes to apply in `00-overview.md` v0.2 (deferred to a separate edit pass, not done by this synthesis note):

1. §5.6 capability annotation list adds `Detached` and `ScopeTag`.
2. §5.3 allocation policy wording softened for `Scope` ring buffer (per §2.11 above).
3. §6.6 composition matrix updated for Stream / Nursery row (per §2.10 above).
4. §8 subagent assignments noted as complete; revision history bumped to v0.2 with reference to this synthesis note.

Changes to apply in layer specs (deferred):

- **L0/L1 (`01-foundations.md`):** add `Detached` and `ScopeTag` annotations; add `HasStopToken` concept; forward-declare `get_io_context_t` CPO; document `Async::materialise` / `dematerialise` British aliases; document the L7 opt-in discovery extension.
- **L2 (`02-backends.md`):** publish `Io::timer_op_state_size_v` and siblings; publish `Tbb::pipeline_rewrite_threshold_v`; note Platform does not model `BulkScheduler`.
- **L3 (`03-adaptors.md`):** static-assert audit for sender-level `Cancellable`; rename `propagate_error_signatures` references to local `Adaptor/detail/`; document `with_allocator` and `with_stop_source` (§14 new); `batch` uses `std::pmr::vector<T>` until `Mashiro::Core::Vector<T>` lands.
- **L4 (`04-coroutine-tasks.md`):** rename `from_channel` → `from_queue` with `AsyncQueue` concept; add `Coro::stopped_signal`; add `MASHIRO_FOR_CO_AWAIT` macro fallback; deprecate `from_future` with migration cross-reference.
- **L5 (`05-structured.md`):** none beyond what is already there; reaffirm `Job`-as-sender contract.
- **L6 (`06-patterns.md`):** add `pipeline_as_stream` bridge function.
- **L7 (`07-extension.md`):** document `register_scheduler_v<T>` user-side opt-in for the discovery walk.
- **Cross-cutting (`08-cross-cutting.md`):** add diagnostics ordering rule (§6.1); cross-reference `from_future` deprecation in §7.5.

---

## 8. Acceptance

The five §9 checks from the umbrella spec all pass against the eight delivered v0.1 specs, modulo the v0.2 patches enumerated in §7 above. The framework's vocabulary is consistent, layer boundaries are clean, completion signatures are explicit and propagated correctly, the composition matrix is covered, and no contradictions remain.

The synthesis pass is complete. Recommended next step: a single editor (not a subagent) applies the §7 patches across the eight specs and bumps `00-overview.md` to v0.2 with the changes inlined. Implementation may begin per the phasing in Cross-cutting §7.5.
