# Mashiro Async Framework — L0 Vocabulary & L1 Capability Layer

**Status:** Draft v0.2 (foundations spec; sets the L0 / L1 contract that every higher layer consumes)
**Date:** 2026-06-15
**Author:** Mashiro Engine team — Subagent A
**Scope:** `Mashiro::Async`, `Mashiro::Async::Concepts`, `Mashiro::Async::Traits` namespaces. New headers under `Mashiro/include/Mashiro/Async/Foundations.h`, `Concepts.h`, `Traits.h`. Composes with the existing `Mashiro::Platform` layer (per `2026-06-11-platform-thread-infrastructure-design.md`) without modifying it. Bound by the umbrella spec `00-overview.md`.

### Revision history

- **v0.1** — initial draft. Locks the L0 re-export surface, the concept aliases, the annotation
  set, the `Traits::*` consteval queries, the completion-signature helpers, the consteval
  capability-vs-concept verification block, and the `bridge_stop_token` adaptor. No source
  files are landed by this spec — it is documentation only. Vocabulary, namespaces, and
  layer ownership are inherited from `00-overview.md` §2 / §4 / §5 and are not redefined here.
- **v0.2** — incorporates synthesis pass adjudications (`09-synthesis.md` §2.1, §2.4, §2.13,
  §2.15, §2.24, §2.25). §4.3 v0.2 publishes the British `Async::materialise` / `dematerialise`
  aliases. §4.4 v0.2 forward-declares the `get_io_context_t` CPO and adds the new
  `HasStopToken<Env>` concept for the fast-path branch in hot backends. §5.2 v0.2 adds the
  `Detached` and `ScopeTag` annotations consumed by L4 and L5 respectively. §6.2 v0.2 adds
  the `IsDetached_v<T>` and `ScopeTagOf<S>` queries. §8.1 v0.2 documents how
  `register_scheduler_v<T>` (defined by L7) extends the verifier's discovery walk to
  user-namespace schedulers without compromising the reflection-only contract. §12 open
  issues 1, 4, 7 are now resolved by the synthesis note and marked accordingly.

---

## 1. Purpose

The Async framework promises one async vocabulary: stdexec. L0 is the re-export layer that gives
that vocabulary a Mashiro-spelling and a Mashiro-namespace; it adds **no behaviour**. L1 is the
capability-tag layer that lets a backend *say* what it offers (bulk dispatch, I/O, thread
affinity, allocation policy) and lets every higher layer *query* those statements through pure
P2996 reflection — no out-of-band registry, no static map, no virtual call.

This spec fixes:

- which `stdexec::*` types appear under `Mashiro::Async::*` and why renames earn their keep;
- the framework's concept hierarchy (`Scheduler`, `BulkScheduler`, `IoScheduler`,
  `AffineScheduler`, `ParallelScheduler`) and how it relates to L1 declarations;
- the L1 annotation set (`Backend`, `Affine`, `OffersBulk`, `OffersIo`, `Cancellable`,
  `Allocates`, `IsForwardProgress`) and the rules for composing them;
- the `Traits::*` reflection-driven queries every higher layer uses to ask "what does this
  scheduler offer?" without naming a backend;
- the completion-signature helpers (`with_error`, `union_signatures`, `propagate_stopped`)
  that L3/L4 adaptors lean on;
- the consteval verifier that fails compilation when a backend's declared capability set does
  not match the concepts it satisfies;
- the `bridge_stop_token` adaptor that maps `std::stop_token` (jthread / coroutine boundary)
  onto `stdexec::inplace_stop_token` (the framework's only internal stop type).

What this spec does **not** cover (owners in parentheses): backend bodies (Subagent B / L2),
sender adaptor bodies (Subagent C / L3), coroutine task types (Subagent C / L4), structured
scope / nursery (Subagent D / L5), patterns (Subagent D / L6), extension surface (Subagent E /
L7), cross-cutting cancellation/allocation/diagnostics policy (Subagent E / cross-cutting).

---

## 2. Scope and non-goals

In scope:

- §4 L0 re-export surface — type aliases, function aliases, concept aliases.
- §5 L1 capability annotations — `[[=Async::...]]` tags + composition rules.
- §6 `Traits::*` consteval queries — reflection-driven introspection.
- §7 Completion-signature helpers.
- §8 Consteval capability-vs-concept verifier.
- §9 `bridge_stop_token` adaptor.
- §10 Closing worked example: `MockScheduler` declares capabilities, `Traits::*` queries it,
  the verifier accepts or rejects.

Out of scope (rejected if it shows up in this file):

- Any backend's `schedule()` body, op-state, or domain.
- Any sender adaptor body (`bulk`, `timeout`, `retry`, …).
- Any coroutine-task definition.
- Any allocation policy beyond *declaring* it via `Allocates`.
- Any diagnostics or trace primitive.
- Any plugin or runtime-discovery mechanism.

Non-goals (called out per overview §10 and platform-thread spec §3):

- A new executor concept. `stdexec::scheduler` is the only scheduler concept.
- A new stop-token type. `stdexec::inplace_stop_token` is the only internal stop type;
  `std::stop_token` is supported solely at the boundary via §9.
- A new completion-signature primitive. We thinly wrap stdexec's primitives; we do not
  reinvent them.
- A name-based or string-keyed annotation registry. Annotations are queried by P2996
  reflection (`annotations_of`, `annotations_with`, `members_of`) only.

---

## 3. Layer position

Per overview §3 / §8.1, this spec owns L0 and L1:

```
L7  Extension surface              ← Subagent E
L6  Patterns                       ← Subagent D
L5  Structured concurrency         ← Subagent D
L4  Coroutine task types           ← Subagent C
L3  Sender adaptors & combinators  ← Subagent C
L2  Backends                       ← Subagent B
L1  Capability annotations & Traits   ← THIS SPEC
L0  Vocabulary (re-exports)           ← THIS SPEC
```

L1 depends only on L0. L0 depends only on `stdexec` and on the toolchain's reflection
features. Every higher layer depends on L0 + L1 transitively; nothing here may upcall.

---

## 4. L0 — Vocabulary re-export surface

### 4.1 Header layout

L0 spans three headers; all live under `Mashiro/include/Mashiro/Async/`.

| Header | Owns |
|---|---|
| `Foundations.h` | `Mashiro::Async::*` re-exports of stdexec types and free functions, the L1 annotation `struct`s, the consteval verifier (§8), `bridge_stop_token` (§9). |
| `Concepts.h` | `Mashiro::Async::Concepts::*` concept aliases (§4.4). Includes `Foundations.h`. |
| `Traits.h` | `Mashiro::Async::Traits::*` consteval queries (§6) and the completion-signature helpers (§7). Includes `Concepts.h`. |

`Foundations.h` is the only header a backend author has to include; it pulls in stdexec and
declares the annotation types. `Concepts.h` and `Traits.h` are pulled in by L2/L3 spec files
and by user code that wants to query capabilities.

### 4.2 Re-export rules

Re-exports follow three rules:

1. **One vocabulary, one spelling.** A type that has a stdexec name and a Mashiro-style name
   gets *exactly one* alias; the stdexec name remains visible through `stdexec::` for
   library-internal use, but every header in `Mashiro/Async/` and every doc consumes the
   Mashiro-spelling.
2. **Only rename when style or namespace hygiene wins.** No renames for taste. The bar is
   "this name reads better in Mashiro headers than the stdexec spelling does, *and* the
   rename does not invent a synonym that hides the stdexec primitive's semantics."
3. **No reordering of template parameters, no defaulting of policy parameters.** Aliases are
   strict — `using x = y;` not `using x = y_with_baked_in_policy;`.

### 4.3 Re-export table

| Mashiro spelling | stdexec source | Why this rename earns its keep |
|---|---|---|
| `Async::sender` | `stdexec::sender` (concept) | Visible in concept positions in framework headers; namespace-only rename. |
| `Async::receiver` | `stdexec::receiver` (concept) | Same. |
| `Async::scheduler` *(concept)* | `stdexec::scheduler` (concept) | Distinct from `Backend::Platform::scheduler` *type* — concept lives in `Mashiro::Async::Concepts::Scheduler` (§4.4) to avoid the concept/type name collision the platform spec already runs into. |
| `Async::operation_state` | `stdexec::operation_state` | Namespace hygiene. |
| `Async::env_of_t` | `stdexec::env_of_t` | Namespace hygiene; queried in every Trait. |
| `Async::stop_token` | `stdexec::inplace_stop_token` | **Earns its keep.** Per overview §5.2 / platform spec §6.7: the framework's stop-token type is `inplace_stop_token`. The shorter spelling is correct because there is no second token type in scope. `std::stop_token` is reachable only via §9. |
| `Async::stop_source` | `stdexec::inplace_stop_source` | Same rationale. |
| `Async::stop_callback_for_t<Tok, Cb>` | `stdexec::stop_callback_for_t` | Namespace hygiene. |
| `Async::scope` | `stdexec::counting_scope` (P3149) | **Earns its keep.** "Scope" is the umbrella vocabulary §6.4 / overview §6 uses; `counting_scope` is a stdexec implementation noun. The alias preserves the `counting_scope` name for L5 implementation references but lets every higher-layer doc say "scope". |
| `Async::completion_signatures<...>` | `stdexec::completion_signatures<...>` | Namespace hygiene. |
| `Async::completion_signatures_of_t<S, Env>` | `stdexec::completion_signatures_of_t` | Namespace hygiene. |
| `Async::set_value_t`, `Async::set_error_t`, `Async::set_stopped_t` | `stdexec::set_value_t`, etc. | Namespace hygiene; cited heavily in §7 / signatures contracts. |
| `Async::just`, `Async::just_error`, `Async::just_stopped` | `stdexec::just`, etc. | Namespace hygiene. |
| `Async::then`, `Async::let_value`, `Async::let_error`, `Async::let_stopped` | `stdexec::then`, etc. | Namespace hygiene. |
| `Async::when_all`, `Async::when_any` | `stdexec::when_all` / `exec::when_any` | Namespace hygiene + co-locates the two waits in the same namespace (`when_any` lives in `exec::` upstream; the alias hides the difference). |
| `Async::start_detached` | `stdexec::start_detached` | Namespace hygiene; **L7 will require users to read its `Cancellable` warnings** — one alias is easier to footnote than two namespaces. |
| `Async::sync_wait` | `stdexec::sync_wait` | Namespace hygiene. Boundary-only (per overview §6.7's "wrong primitive" guidance). |
| `Async::continues_on` | `stdexec::continues_on` | Namespace hygiene. |
| `Async::schedule` | `stdexec::schedule` | Namespace hygiene. |
| `Async::schedule_bulk` | `stdexec::schedule_bulk` | Namespace hygiene; L2/L3 mention. |
| `Async::transfer_just` | `stdexec::transfer_just` | Namespace hygiene; rarely needed by users but cited in §7 and L3 specs. |
| `Async::get_stop_token` | `stdexec::get_stop_token` | Namespace hygiene. |
| `Async::get_completion_scheduler` | `stdexec::get_completion_scheduler` | Namespace hygiene. |
| `Async::get_allocator` | `stdexec::get_allocator` | Namespace hygiene; queried by `Traits::AllocatesIn_v` (§6). |
| `Async::get_domain` | `stdexec::get_domain` | Namespace hygiene; L2 backend domains. |
| `Async::default_domain` | `stdexec::default_domain` | Namespace hygiene. |
| `Async::transform_sender` | `stdexec::transform_sender` | Namespace hygiene. |
| `Async::forward_progress_guarantee` | `stdexec::forward_progress_guarantee` (enum) | Namespace hygiene; queried by `Traits::ProgressOf_v`. |
| `Async::any_sender_of<Sigs...>` | `exec::any_sender_of<Sigs...>` | Namespace hygiene; per overview §2 boundary-only. |
| `Async::materialize` / `Async::materialise` | `stdexec::materialize` (P2300) | **v0.2 — British alias.** Both spellings are exposed; project-style guideline is to use the British form in framework headers (per `09-synthesis.md` §2.4). User code may use either; the two names refer to the same `inline constexpr` object so ADL is unambiguous. |
| `Async::dematerialize` / `Async::dematerialise` | `stdexec::dematerialize` (P2300) | **v0.2 — British alias.** Same rationale. |

**Rejected renames** (called out so reviewers don't relitigate):

- `Async::Sender` (PascalCase) — rejected. Concepts in `Concepts::*` are PascalCase; primitive
  type aliases stay lowercase to mirror stdexec.
- `Async::task<T>` at L0 — rejected. `Task<T>` (PascalCase) is L4 and carries a scheduler-affinity
  contract; no L0 alias exists for `exec::task<T>` because L0 must not bake in policy.
- `Async::future<T>` — rejected outright. Per overview §2, "no framework future type."
- `Async::executor`, `Async::callback`, `Async::async` — rejected outright per overview §2.

### 4.4 Concept aliases

Five concept aliases live in `Mashiro::Async::Concepts`. They are queried by L2 backends to
declare what they offer and by every higher layer to constrain templates. Each is stated as a
full concept body so the spec can be lifted verbatim into `Concepts.h` review.

```cpp
// Concepts.h — sketch (declarations + concept bodies, not full implementations)
#pragma once
#include "Foundations.h"

namespace Mashiro::Async::Concepts {

    // Re-export of stdexec::scheduler. Every backend models this.
    template<class S>
    concept Scheduler = stdexec::scheduler<S>;

    // Bulk-capable scheduler: schedule_bulk(s, n, fn) is well-formed.
    // The lambda body is intentionally trivial — we test well-formedness, not behaviour.
    template<class S>
    concept BulkScheduler =
        Scheduler<S> &&
        requires (S s, std::size_t n) {
            { stdexec::schedule_bulk(s, n, [](std::size_t) noexcept {}) }
                -> stdexec::sender;
        };

    // I/O-capable scheduler. We do *not* introduce an Io::supports_operations CPO at L0
    // (that's L2 territory). Instead we use the L1 OffersIo annotation as the source of
    // truth and assert in §8 that any scheduler annotated OffersIo also satisfies
    // whatever shape the L2 Io adaptors require. The concept here is a structural hook
    // that L3 Io adaptors can SFINAE on.
    template<class S>
    concept IoScheduler =
        Scheduler<S> &&
        Traits::OffersIo_v<S> &&
        requires (S s) {
            // The Io backend exposes one well-known free function via tag_invoke,
            // get_io_context, that returns a pointer-to-implementation. L2 details
            // the type; here we only require the call is well-formed and yields
            // a non-void result.
            { stdexec::tag_invoke(get_io_context_t{}, s) } -> std::convertible_to<void*>;
        };

    // Affine scheduler: schedule(s) always completes on a fixed thread, and
    // equality-comparable schedulers compare equal iff that thread coincides.
    // The Trait IsAffine_v is annotation-driven; the requires-clause re-checks
    // structurally that the scheduler exposes a stable identity on its env.
    template<class S>
    concept AffineScheduler =
        Scheduler<S> &&
        Traits::IsAffine_v<S> &&
        std::equality_comparable<S>;

    // Forward-progress guarantee — at least `parallel`. Mirrors stdexec's
    // forward_progress_guarantee enum.
    template<class S>
    concept ParallelScheduler =
        Scheduler<S> &&
        (Traits::ProgressOf_v<S> >= stdexec::forward_progress_guarantee::parallel);

    // v0.2 (per 09-synthesis.md §2.13). Env carries a non-`never` stop-token.
    // Hot backends (Inline, short StaticPool items) branch on this concept via
    // `if constexpr (HasStopToken<Env>)` to skip the `inplace_stop_callback`
    // registration when the receiver advertises `stdexec::never_stop_token`.
    // The branch is compile-time; absence of a stop token means cancellation is a
    // no-op for that operation — correct behaviour per the cancellation contract.
    template<class Env>
    concept HasStopToken =
        !std::same_as<
            std::remove_cvref_t<decltype(stdexec::get_stop_token(std::declval<const Env&>()))>,
            stdexec::never_stop_token>;

} // namespace Mashiro::Async::Concepts
```

The relationship between concept satisfaction and L1 annotation declaration is **bi-conditional
and verified at compile time**. A backend says "I offer bulk" by attaching `[[=OffersBulk]]` to
its scheduler type; the verifier in §8 then asserts `Concepts::BulkScheduler<S>` actually
holds. A backend that satisfies `BulkScheduler<S>` but does not annotate is *also* an error —
we fail loud rather than let the L1 layer silently disagree with reality. The error-message
strategy is in §8.

### 4.5 `get_io_context_t` forward declaration (v0.2)

Per `09-synthesis.md` §2.24, the `get_io_context_t` CPO is **declared** in `Foundations.h` and
**customised** in the L2 Io backend. Same forward-declaration pattern stdexec uses for
`get_scheduler` / `get_stop_token`: the L0 header owns the tag type, the L2 header provides
the `tag_invoke` overloads on the Io scheduler.

```cpp
namespace Mashiro::Async {

    // CPO tag type. Empty; pure-tag, no behaviour. The Io backend (L2) supplies the
    // tag_invoke overload that returns a pointer to its internal IoContext type.
    // L3 Io adaptors call this CPO to reach the backend's submission queue / completion port.
    struct get_io_context_t {
        template<class Sched>
        constexpr auto operator()(Sched&& s) const
            noexcept(noexcept(stdexec::tag_invoke(get_io_context_t{}, std::forward<Sched>(s))))
            -> decltype(stdexec::tag_invoke(get_io_context_t{}, std::forward<Sched>(s)))
        {
            return stdexec::tag_invoke(get_io_context_t{}, std::forward<Sched>(s));
        }
    };

    inline constexpr get_io_context_t get_io_context{};

} // namespace Mashiro::Async
```

A scheduler that does not specialise `tag_invoke(get_io_context_t, S)` is simply not an
`IoScheduler` — the SFINAE in `Concepts::IoScheduler` (§4.4) excludes it cleanly.

### 4.6 What L0 does not own

- The `Backend` enum (§5.1) and the annotation `struct`s (§5.2) live in `Foundations.h` but
  are L1 by responsibility — L0 only re-exports stdexec; tagging is the L1 job.
- The `Traits::*_v` variables (§6) live in `Traits.h`; they are L1 logic over L0 types.
- The verifier (§8) lives at the bottom of `Foundations.h` so it can see every backend's
  declaration before any other header runs; it is L1 logic that consumes L0 types.

---

## 5. L1 — Capability annotations

### 5.1 The `Backend` enum

The `Backend` enum is the only string-free identity attached to a scheduler. Per overview §5.6
it lives in `Foundations.h`:

```cpp
namespace Mashiro::Async {

    enum class Backend : std::uint8_t {
        Inline,        // L2 Inline backend
        StaticPool,    // L2 StaticPool backend
        Tbb,           // L2 Tbb backend
        Platform,      // alias of Mashiro::Platform::scheduler
        Io,            // L2 Io backend (proactor)
        User           // any L7-defined scheduler that does not use one of the above
    };

} // namespace Mashiro::Async
```

`User` exists so a downstream scheduler can satisfy the verifier without claiming to be one of
the framework-shipped backends. It is *not* a wildcard for "I refuse to declare" — a backend
that declares `Backend::User` must still attach the rest of its capability annotations and
still satisfies the bi-conditional in §8.

### 5.2 Annotation `struct`s

All annotations are P3394 `[[=...]]` annotations. They are aggregate `struct`s with `constexpr`
operator==, so they are usable in `consteval` contexts and comparable for set-equality without
custom logic. Per overview §2 / §5.6, annotations are queryable by reflection only — they are
*never* inspected at runtime.

```cpp
namespace Mashiro::Async {

    // ---- Identity ---------------------------------------------------------

    // Tag the backend identity. Exactly one BackendTag per scheduler type is required
    // by §8's verifier. Multiple tags are an error.
    struct BackendTag {
        Backend kind;
        constexpr bool operator==(const BackendTag&) const = default;
    };

    // ---- Capabilities -----------------------------------------------------

    // The scheduler completes its schedule() sender on a fixed, statically-known thread.
    // `affinity` names *which* thread role; `Backend::Platform` is the canonical case.
    // A scheduler may carry Affine for a non-Platform backend if it is itself a
    // single-thread scheduler (e.g. an L7 actor's serial executor).
    struct Affine {
        Backend affinity;
        constexpr bool operator==(const Affine&) const = default;
    };

    // The scheduler models Concepts::BulkScheduler — `schedule_bulk(s, n, fn)` is
    // well-formed and produces a bulk-launching sender. Empty struct: presence is
    // the entire payload.
    struct OffersBulk {
        constexpr bool operator==(const OffersBulk&) const = default;
    };

    // The scheduler models Concepts::IoScheduler — exposes the L2 Io operation set
    // (async_read / async_write / async_accept / async_connect / async_timer).
    struct OffersIo {
        constexpr bool operator==(const OffersIo&) const = default;
    };

    // The scheduler honours stop_token cancellation on every sender it produces.
    // Every backend the framework ships is Cancellable; the annotation exists so a
    // user-extension scheduler may declare itself non-cancellable (a synchronous
    // unit-test stub, e.g.) and L3 adaptors can fail loud or fall back.
    struct Cancellable {
        constexpr bool operator==(const Cancellable&) const = default;
    };

    // Allocation policy. `Where` mirrors overview §5.3:
    //   Frame    — coroutine frame allocation (HALO-elidable)
    //   OpState  — sender op-state heap (only when stack-embedding is impossible)
    //   Output   — produced value owns heap (e.g. Stream<T> elements with std::string)
    //   External — third-party library (TBB arena, io_uring rings) heap
    struct Allocates {
        enum class Where : std::uint8_t { Frame, OpState, Output, External };
        Where where;
        constexpr bool operator==(const Allocates&) const = default;
    };

    // Forward-progress guarantee, mirrors stdexec's enum. Backends must declare exactly
    // one ProgressTag; the value is queried by Traits::ProgressOf_v.
    struct ProgressTag {
        stdexec::forward_progress_guarantee progress;
        constexpr bool operator==(const ProgressTag&) const = default;
    };

    // ---- L4 / L5 type-shape tags (v0.2, per 09-synthesis.md §2.1) ---------

    // Tag detached-lifetime coroutine task types (Job carries it; Task<T> does not).
    // Exactly-one per task type. Queried by Traits::IsDetached_v<T> (§6.2) and by
    // Scope::spawn (L5) to choose the detached-spawn overload. This annotation does
    // NOT participate in the §8 capability-vs-concept verifier — it tags L4 types,
    // not backends, and there is no L0 concept that corresponds to "is detached".
    struct Detached {
        constexpr bool operator==(const Detached&) const = default;
    };

    // Tag every Scope class template with a compact source-location-derived constant.
    // Exactly-one per scope. Queried by Traits::ScopeTagOf<S> (§6.2). If the user
    // declares a Scope without an explicit ScopeTag, the L5 spec defaults it from
    // std::source_location::current(). Does not participate in the §8 verifier.
    struct ScopeTag {
        std::uint64_t value;
        constexpr bool operator==(const ScopeTag&) const = default;
    };

} // namespace Mashiro::Async
```

### 5.3 What each annotation tags

| Annotation | Attaches to | Read by | Composes by |
|---|---|---|---|
| `BackendTag{...}` | scheduler type | `Traits::BackendOf<S>`, §8 verifier | exclusive — exactly one per scheduler |
| `Affine{...}` | scheduler type | `Traits::AffinityOf<S>`, `Traits::IsAffine_v<S>`, §8 verifier | optional — at most one |
| `OffersBulk` | scheduler type | `Traits::OffersBulk_v<S>`, §8 verifier | optional — presence-only |
| `OffersIo` | scheduler type | `Traits::OffersIo_v<S>`, §8 verifier | optional — presence-only |
| `Cancellable` | scheduler type *or* sender expression type | `Traits::IsCancellable_v<T>` | optional; absence means "framework assumes not cancellable" |
| `Allocates{Where}` | scheduler type *or* sender expression type | `Traits::AllocatesIn_v<T>` | additive — multiple `Allocates` of different `Where` permitted; multiple of the same `Where` is an error |
| `ProgressTag{...}` | scheduler type | `Traits::ProgressOf_v<S>`, §8 verifier | exclusive — exactly one |
| `Detached` *(v0.2)* | coroutine task types (L4) | `Traits::IsDetached_v<T>`, L5 `Scope::spawn` | exclusive — exactly one per type; absent on `Task<T>`, present on `Job` |
| `ScopeTag{...}` *(v0.2)* | scope class templates (L5) | `Traits::ScopeTagOf<S>` | exclusive — exactly one per scope; defaulted by L5 if user omits |

### 5.4 Composition rules

1. **Exactly-one annotations** (`BackendTag`, `ProgressTag`): the verifier counts via
   `annotations_with` and `static_assert`s the count is one. Zero or two is a compile error
   with the exact reflected scheduler name in the message (§8).
2. **Optional, presence-only** (`OffersBulk`, `OffersIo`, `Cancellable`): zero or one.
   Duplicate presence is a compile error.
3. **Optional, valued** (`Affine`): zero or one. Duplicate is a compile error.
4. **Additive** (`Allocates`): zero or more, with the rule that no two `Allocates` may carry
   the same `Where`. Duplicate `Where` is a compile error.

Composition rules are checked by the §8 verifier at the bottom of `Foundations.h`. They are
*not* checked by the annotation `struct`s themselves; an annotation is a passive tag.

### 5.5 What annotations are *not*

- Not identity. `Affine{Backend::Platform}` says "this scheduler is bound to whichever thread
  the Platform backend names"; it does not make the scheduler *be* the Platform scheduler.
  Identity is `BackendTag`.
- Not behaviour. An adaptor never branches on `OffersBulk` at runtime. The branch is
  `if constexpr (Traits::OffersBulk_v<S>)` — taken at template instantiation, gone at code-gen.
- Not strings. The framework refuses to introduce a `name`-keyed annotation; if a downstream
  needs a string identity, it is generated from the scheduler type's reflected name
  (`std::meta::display_string_of(^^S)`), not stored.

---

## 6. `Traits::*` consteval queries

`Traits.h` exposes a small set of consteval queries built from P2996 reflection over the L1
annotation `struct`s declared in `Foundations.h`. Every query is **pure** — no side state, no
runtime fallback. Each Trait body is under twenty lines (this is a hard ceiling — bigger
bodies indicate the Trait is doing policy, not querying, and belong elsewhere).

### 6.1 The reflection helper

A single helper, `Detail::FirstAnnotation<T, Tag>()`, encapsulates the
"give me the unique annotation of type `Tag` on `T`" pattern:

```cpp
namespace Mashiro::Async::Traits::Detail {

    template<typename Tag, std::meta::info Subject>
    consteval auto AnnotationsOnSubject() {
        constexpr auto annos = std::meta::annotations_with(Subject, ^^Tag);
        return annos;
    }

    template<typename Tag, typename T>
    consteval std::optional<Tag> FirstAnnotation() {
        constexpr auto annos = AnnotationsOnSubject<Tag, ^^T>();
        if constexpr (annos.size() == 0) return std::nullopt;
        else                             return std::meta::extract<Tag>(annos.front());
    }

    template<typename Tag, typename T>
    consteval std::size_t CountAnnotations() {
        return AnnotationsOnSubject<Tag, ^^T>().size();
    }

} // namespace Mashiro::Async::Traits::Detail
```

The verifier in §8 calls `CountAnnotations` to enforce composition rules (§5.4); the queries
below call `FirstAnnotation` to extract values.

### 6.2 The Trait set

```cpp
namespace Mashiro::Async::Traits {

    // Identity: which Backend enumerator does this scheduler claim?
    template<class S>
    inline constexpr Backend BackendOf =
        Detail::FirstAnnotation<BackendTag, S>().value_or(BackendTag{Backend::User}).kind;

    // Affinity: if Affine{...} is present, return its backend tag wrapped in optional.
    // Used by §8 verifier and by L3 sender adaptors that fold `continues_on(s, sched)`
    // when `sched` is the same affinity as `s`'s completion scheduler.
    template<class S>
    inline constexpr std::optional<Backend> AffinityOf =
        []() -> std::optional<Backend> {
            constexpr auto a = Detail::FirstAnnotation<Affine, S>();
            if constexpr (!a.has_value()) return std::nullopt;
            else                          return a->affinity;
        }();

    // Boolean: is this scheduler annotated as offering bulk dispatch?
    template<class S>
    inline constexpr bool OffersBulk_v =
        Detail::CountAnnotations<OffersBulk, S>() == 1;

    // Boolean: is this scheduler annotated as offering I/O operations?
    template<class S>
    inline constexpr bool OffersIo_v =
        Detail::CountAnnotations<OffersIo, S>() == 1;

    // Boolean: is this scheduler thread-affine?
    template<class S>
    inline constexpr bool IsAffine_v =
        Detail::CountAnnotations<Affine, S>() == 1;

    // Forward-progress level. ProgressTag is exactly-one — verifier asserts; here we
    // unwrap and assume presence. A scheduler missing ProgressTag is rejected at §8.
    template<class S>
    inline constexpr stdexec::forward_progress_guarantee ProgressOf_v =
        Detail::FirstAnnotation<ProgressTag, S>()
            .value_or(ProgressTag{stdexec::forward_progress_guarantee::weakly_parallel})
            .progress;

    // Cancellability: present on the type, or — for sender expressions — propagated
    // by the §7 propagate_stopped helper.
    template<class T>
    inline constexpr bool IsCancellable_v =
        Detail::CountAnnotations<Cancellable, T>() == 1;

    // Allocation: returns an array of Allocates::Where the type declares.
    // Used by Diagnostics::AllocCheck (cross-cutting spec) and L5 scope sizing.
    template<class T>
    inline constexpr auto AllocatesIn_v = []() consteval {
        constexpr auto annos = Detail::AnnotationsOnSubject<Allocates, ^^T>();
        std::array<Allocates::Where, annos.size()> out{};
        for (std::size_t i = 0; i < annos.size(); ++i)
            out[i] = std::meta::extract<Allocates>(annos[i]).where;
        return out;
    }();

    // Convenience: the reflected display name of a backend's scheduler type, used by
    // §8 error messages and Diagnostics tags. Pure reflection — no string storage.
    template<class S>
    inline constexpr std::string_view NameOf =
        std::meta::define_static_string(std::meta::display_string_of(^^S));

    // ---- L4 / L5 type-shape queries (v0.2) -----------------------------------

    // Is this coroutine task type detached-lifetime? True for Job, false for Task<T>.
    // Used by L5 Scope::spawn to choose between the structured and detached overloads.
    template<class T>
    inline constexpr bool IsDetached_v =
        Detail::CountAnnotations<Detached, T>() == 1;

    // Return the scope tag value attached to a Scope<...> class template. The L5 spec
    // defaults it from std::source_location::current() if the user omits it.
    template<class S>
    inline constexpr std::uint64_t ScopeTagOf =
        Detail::FirstAnnotation<ScopeTag, S>().value_or(ScopeTag{0}).value;

} // namespace Mashiro::Async::Traits
```

### 6.3 The reflection algorithm

For every Trait the algorithm is the same three-step pipeline:

1. `^^T` — reify the type as a `std::meta::info`.
2. `std::meta::annotations_with(^^T, ^^Tag)` — collect every annotation of the requested type
   on `T`. P2996 returns these in declaration order.
3. `std::meta::extract<Tag>(info)` — materialise the `Tag` value back as a `consteval` value.

The verifier in §8 walks `Backend::*` namespace via `members_of(^^Backend::Inline_namespace)`
to find every scheduler type, and applies the queries above to each one.

### 6.4 Reflection-only contract

- No Trait stores state outside the consteval evaluation.
- No Trait reads from a global registry, file, or environment variable.
- Every Trait body must be self-contained and re-entrant under `consteval` evaluation
  (P2996's reflection model permits this; we rely on it).
- A Trait that needs information not available through reflection is rejected at design
  review: either the information is encoded in an annotation (and queried as above) or it
  belongs in another layer.

---

## 7. Completion-signature helpers

`Traits.h` exposes three thin wrappers over stdexec's signature primitives. Each wrapper earns
its keep by giving L3/L4 a *named, search-able* surface for one specific composition.

### 7.1 `with_error<E, S>`

```cpp
namespace Mashiro::Async::Traits {

    // Augment a sender-completion signature pack with an additional set_error_t<E>.
    // Composes by union with stdexec::completion_signatures_of_t<S, Env>.
    //
    // Wrapper rationale:
    //   - L3 adaptors (timeout, retry, race) need to opt into typed errors per
    //     overview §5.7's "no silent set_error_t". Saying `with_error<MyErr, S>` at
    //     the declaration site is more searchable than the raw stdexec spelling.
    //   - The wrapper enforces that E is non-void and not already in S's signatures,
    //     producing a focused error message instead of the deeper stdexec mismatch.
    template<class E, stdexec::sender S, class Env = stdexec::empty_env>
    using with_error = stdexec::transform_completion_signatures_of<
        S, Env,
        stdexec::completion_signatures<stdexec::set_error_t(E)>>;

    static_assert(/* E must not be void */ true);   // enforced in real header

} // namespace Mashiro::Async::Traits
```

`with_error` is the *only* path by which a framework adaptor introduces a new error
completion. Per overview §5.7 a framework-provided adaptor that wants to surface
`set_error_t(std::exception_ptr)` does so by composing with `with_error<std::exception_ptr,
…>` at its signature declaration site, never by silent injection.

### 7.2 `union_signatures<S...>`

```cpp
namespace Mashiro::Async::Traits {

    // Union the completion signatures of N senders; the result is a single
    // completion_signatures<...> that contains each unique completion exactly once.
    //
    // Wrapper rationale:
    //   - L3 race / when_any / let_value already need this; stdexec exposes the
    //     primitive as `transform_completion_signatures` chained N-1 times. The
    //     wrapper collapses the chain into one declaration site.
    //   - It prevents accidental concatenation (signature duplication) which is the
    //     overview §5.7 anti-pattern.
    template<stdexec::sender... S>
    using union_signatures = /* chained merge of completion_signatures_of_t<S, Env>... */
        Detail::UnionSignaturesImpl<S...>;

} // namespace Mashiro::Async::Traits
```

The implementation is a fold over `transform_completion_signatures`; the user-visible alias
hides that and keeps the signature-merge intent obvious.

### 7.3 `propagate_stopped<S>`

```cpp
namespace Mashiro::Async::Traits {

    // Ensure the result type carries set_stopped_t() iff S does.
    //
    // Wrapper rationale:
    //   - Overview §5.7: set_stopped_t() must propagate when any upstream offers it.
    //   - L3 adaptors that *introduce* their own stop semantics (timeout, race) need
    //     to compose set_stopped_t into their signatures even when the upstream
    //     does not offer it. The wrapper makes the propagation explicit:
    //       using sigs = union_signatures<propagate_stopped<S>, ...>;
    //   - Without the wrapper, every adaptor reimplements the conditional via
    //     `__has_completion<set_stopped_t()>` — the wrapper centralises the test.
    template<stdexec::sender S, class Env = stdexec::empty_env>
    using propagate_stopped = std::conditional_t<
        Detail::HasStoppedCompletion<S, Env>,
        stdexec::completion_signatures<stdexec::set_stopped_t()>,
        stdexec::completion_signatures<>>;

} // namespace Mashiro::Async::Traits
```

These three are the only signature helpers L0 ships. Adaptor-specific helpers (e.g.
`bulk_signatures<S, Range>`) live with their adaptors in L3.

---

## 8. Consteval verification block

The verifier is a single `consteval` block at the bottom of `Foundations.h`. It runs once per
translation unit that includes the header, *after* every framework-shipped backend has had its
scheduler type declared and annotated. The block enumerates the registered backends, queries
their annotations, and `static_assert`s the bi-conditional between L1 declarations and the L0
concept satisfaction.

### 8.1 Discovery: how the verifier finds every backend

Backends are not enumerated by hand. The verifier walks `Mashiro::Async::Backend` and applies
two filters:

1. The candidate is a class type.
2. The candidate carries exactly one `BackendTag` annotation.

```cpp
namespace Mashiro::Async::Detail {

    consteval auto DiscoverBackendSchedulers() {
        constexpr auto ns = ^^Mashiro::Async::Backend;
        std::vector<std::meta::info> out;
        // P1306 expansion-statement walk over namespace members.
        template for (constexpr auto m : std::meta::members_of(ns)) {
            if constexpr (std::meta::is_type(m) &&
                          std::meta::is_class_type(m) &&
                          std::meta::annotations_with(m, ^^BackendTag).size() == 1) {
                out.push_back(m);
            }
        }
        return std::define_static_array(out);
    }

} // namespace Mashiro::Async::Detail
```

Sub-namespaces (`Backend::Inline`, `Backend::StaticPool`, …) are walked transitively by
`members_of` per P2996; this picks up every scheduler regardless of where in the
`Backend` tree it lives. A scheduler that lives outside `Backend::*` is *not* discovered
by the namespace walk alone — user-extension schedulers (L7) opt in by specialising the
`Mashiro::Async::Extension::register_scheduler_v<T>` boolean trait (defined by L7;
v0.2, per `09-synthesis.md` §2.25). The verifier extends its discovery set with every
type `T` for which `register_scheduler_v<T> == true`:

```cpp
// Discovery is the union of:
//   (a) every class type in Mashiro::Async::Backend::* with exactly one BackendTag, AND
//   (b) every type T with Mashiro::Async::Extension::register_scheduler_v<T> == true.
//
// L0/L1 own (a); L7 owns (b). The verifier walks both sets and applies VerifyOneBackend
// identically. This keeps L0 free of any out-of-band registry and keeps user-extension
// schedulers participating in the same bi-conditional contract that framework-shipped
// backends satisfy.
```

The trait variable itself lives at L7. L0 declares it as `false` by default; L7 user code
opens namespace `Mashiro::Async::Extension` and specialises it to `true` for the user's
scheduler. The set is reflection-driven (no static list, no runtime registry).

### 8.2 The bi-conditional checks

For each discovered scheduler type `S`:

```cpp
namespace Mashiro::Async::Detail {

    template<class S>
    consteval void VerifyOneBackend() {
        // 1. Identity: exactly one BackendTag, exactly one ProgressTag.
        static_assert(
            std::meta::annotations_with(^^S, ^^BackendTag).size() == 1,
            "[Mashiro::Async] backend scheduler must carry exactly one BackendTag");
        static_assert(
            std::meta::annotations_with(^^S, ^^ProgressTag).size() == 1,
            "[Mashiro::Async] backend scheduler must carry exactly one ProgressTag");

        // 2. L0 concept satisfaction (Scheduler is mandatory).
        static_assert(Concepts::Scheduler<S>,
            "[Mashiro::Async] backend scheduler must model Concepts::Scheduler");

        // 3. Bi-conditional: OffersBulk annotation iff BulkScheduler concept.
        constexpr bool tagBulk  = Traits::OffersBulk_v<S>;
        constexpr bool isBulk   = Concepts::BulkScheduler<S>;
        static_assert(tagBulk == isBulk,
            "[Mashiro::Async] OffersBulk annotation does not match BulkScheduler "
            "concept satisfaction (see Traits::NameOf<S> for the offending type)");

        // 4. Bi-conditional: OffersIo annotation iff IoScheduler concept.
        constexpr bool tagIo  = Traits::OffersIo_v<S>;
        constexpr bool isIo   = Concepts::IoScheduler<S>;
        static_assert(tagIo == isIo, /* … */);

        // 5. Bi-conditional: IsAffine annotation iff AffineScheduler concept.
        constexpr bool tagAffine = Traits::IsAffine_v<S>;
        constexpr bool isAffine  = Concepts::AffineScheduler<S>;
        static_assert(tagAffine == isAffine, /* … */);

        // 6. Composition rules (§5.4) — duplicate Allocates::Where, etc.
        Detail::CheckAllocatesUnique<S>();
    }

    consteval void VerifyAllBackends() {
        constexpr auto schedulers = DiscoverBackendSchedulers();
        template for (constexpr auto sInfo : schedulers) {
            using S = [: sInfo :];
            VerifyOneBackend<S>();
        }
    }

} // namespace Mashiro::Async::Detail

// The block fires at header inclusion time.
consteval { Mashiro::Async::Detail::VerifyAllBackends(); }
```

### 8.3 Error-message strategy

The framework's bar is "no template instantiation noise leaks into the user's diagnostic".
Three rules:

1. **Every `static_assert` message starts with `[Mashiro::Async] `** and names the offending
   scheduler by its reflected display name (`Traits::NameOf<S>`). Reviewers grep for
   `[Mashiro::Async]` to find every framework-introduced diagnostic.
2. **The message names the contract, not the implementation.** "OffersBulk annotation does
   not match BulkScheduler concept satisfaction" is the contract phrasing; the message does
   not say "schedule_bulk is not callable". Users debug at the contract level first.
3. **The verifier never fires inside a deep template** — it fires exactly once at header
   inclusion. The diagnostic appears at the include line, not at the use site of a sender
   pipeline. This isolates the L1 consistency check from L3/L4 instantiation churn.

When a user-extension backend (`Backend::User`) wants to participate in the verifier, it
opts in through the L7 contract (separate spec). L0/L1 do not provide an extension hook beyond
"declare your scheduler in `Backend::*` and the discovery walk picks it up."

---

## 9. `bridge_stop_token` — std-token bridge

### 9.1 Purpose

The framework uses `Async::stop_token` (= `stdexec::inplace_stop_token`) internally. At the
boundary, two interop targets exist that speak `std::stop_token`:

- `std::jthread` — every client-thread used in the platform spec is a `std::jthread`, and its
  destructor calls `request_stop()` on its associated `std::stop_source`.
- `std::async` migration paths in user code (overview §8.5 / cross-cutting migration plan).

`bridge_stop_token(std::stop_token)` produces an `Async::stop_token` that fires when the
underlying `std::stop_token` does. It is the **only** L0 conversion between the two token
types; per overview §5.2, every other place in the framework speaks `inplace_stop_token`.

### 9.2 Sketch

```cpp
namespace Mashiro::Async {

    // RAII handle: owns an inplace_stop_source plus a std::stop_callback that fires
    // request_stop on the source when the std::stop_token is signalled.
    //
    // Lifetime: the returned Bridge must outlive any sender pipeline that names its
    // token. The typical pattern is to store the Bridge on the receiver's environment
    // or in the pipeline's setup scope.
    class StopTokenBridge {
    public:
        explicit StopTokenBridge(std::stop_token external) noexcept;
        StopTokenBridge(StopTokenBridge&&) = delete;             // pinned by callback ptr
        StopTokenBridge(const StopTokenBridge&) = delete;
        ~StopTokenBridge();

        [[nodiscard]] stop_token token() const noexcept { return source_.get_token(); }

    private:
        stdexec::inplace_stop_source              source_;
        std::stop_callback<                                   // RAII; UB if external is detached
            decltype([](StopTokenBridge* b) noexcept { b->source_.request_stop(); })>
                                                  callback_;
    };

    [[nodiscard]] inline auto bridge_stop_token(std::stop_token tok) noexcept {
        return std::make_unique<StopTokenBridge>(std::move(tok));
    }

} // namespace Mashiro::Async
```

### 9.3 Lifetime, allocation, and cancellation latency

- **Lifetime.** `StopTokenBridge` is *pinned* — its `std::stop_callback` registers a pointer
  to the bridge with the external `std::stop_source`. Moving the bridge would invalidate that
  pointer; the type is therefore non-copyable and non-movable. The `bridge_stop_token` factory
  returns `std::unique_ptr<StopTokenBridge>` so callers can hand it to a sender pipeline as
  shared environment state.
- **Allocation.** This is the **one** documented allocation in L0/L1 hot paths. The
  `unique_ptr` allocates one `StopTokenBridge` (≈ size of the stop-source plus the callback
  thunk). Per overview §5.3 this is acceptable because the allocation is at the boundary, not
  inside a sender pipeline; the bridge is constructed once per cross-token transition. The
  spec calls this out so reviewers do not file it as a §5.3 violation.
- **Cancellation latency.** When the external `std::stop_source` calls `request_stop()`, the
  bridge's `std::stop_callback` fires synchronously on the requesting thread. That callback
  calls `inplace_stop_source::request_stop()`, which fires every registered
  `inplace_stop_callback` synchronously. Total latency from `external.request_stop()` to
  every framework op-state observing stop = **O(#callbacks)**, all on the requesting thread,
  no kernel transition, no work-queue hop.

### 9.4 What `bridge_stop_token` is *not*

- Not a stop-token *generator* — there is no "create a stop-token from thin air" path. Every
  token in the framework comes from either a stdexec stop-source or a `std::stop_source` that
  the bridge wraps.
- Not bidirectional. A framework `Async::stop_token` cannot be converted into a
  `std::stop_token`. If user code at the framework boundary has a `std::jthread` waiting on a
  `std::stop_source`, the `Async` pipeline must be the source-of-truth and the user code must
  poll `Async::stop_token` directly.
- Not cancellation-with-reason. The two token types are pure stop-or-not booleans; there is
  no exception propagation across the bridge.

---

## 10. Worked example — `MockScheduler`

This section walks an L7 user-extension backend through the L0/L1 contract end-to-end. The
example is intentionally minimal so the focus stays on annotation declaration, `Traits`
queries, and the verifier's reaction to a mismatch. None of this code is implementation — the
example is a documentation artefact.

### 10.1 The good case — correct declaration

```cpp
// User code, somewhere reachable from Mashiro::Async::Backend::*.
namespace Mashiro::Async::Backend::Mock {

    // A toy scheduler that synchronously runs the receiver on the calling thread,
    // claims to offer bulk dispatch, and is annotated as such.
    struct [[=BackendTag{Backend::User}]]
           [[=ProgressTag{stdexec::forward_progress_guarantee::weakly_parallel}]]
           [[=OffersBulk]]
           [[=Cancellable]]
           [[=Allocates{Allocates::Where::OpState}]]
    MockScheduler {
        // schedule() — minimal sender that invokes the receiver inline.
        struct sender { /* … completion: set_value_t() … */ };
        sender schedule() const noexcept;

        // schedule_bulk() — required because OffersBulk is declared.
        struct bulk_sender { /* … completion: set_value_t() … */ };
        bulk_sender schedule_bulk(std::size_t n, auto fn) const noexcept;

        bool operator==(const MockScheduler&) const noexcept = default;
    };

} // namespace Mashiro::Async::Backend::Mock
```

What compiles:

- The verifier (§8.2) discovers `MockScheduler` via the namespace walk.
- It checks exactly one `BackendTag` (yes — `Backend::User`) and one `ProgressTag`. Pass.
- `Concepts::Scheduler<MockScheduler>` is satisfied because `schedule()` returns a sender.
- `Traits::OffersBulk_v<MockScheduler>` is `true`; `Concepts::BulkScheduler<MockScheduler>`
  is `true` because `schedule_bulk` is well-formed. Bi-conditional holds.
- `Traits::OffersIo_v<MockScheduler>` is `false`; `Concepts::IoScheduler<MockScheduler>` is
  `false` (no `tag_invoke(get_io_context_t, …)`). Bi-conditional holds.
- `Traits::IsAffine_v<MockScheduler>` is `false`; `Concepts::AffineScheduler<MockScheduler>`
  is `false`. Bi-conditional holds.
- `Traits::AllocatesIn_v<MockScheduler>` returns `[Allocates::Where::OpState]`.
- `Traits::IsCancellable_v<MockScheduler>` is `true`.

The verifier completes silently. L3 adaptors that template on `Concepts::BulkScheduler` will
accept `MockScheduler` as a bulk-capable scheduler.

### 10.2 The mismatch case — declaration lies

```cpp
namespace Mashiro::Async::Backend::Mock {

    // *Claims* OffersBulk but does NOT define schedule_bulk. The L1 declaration
    // and the L0 concept satisfaction disagree.
    struct [[=BackendTag{Backend::User}]]
           [[=ProgressTag{stdexec::forward_progress_guarantee::weakly_parallel}]]
           [[=OffersBulk]]                       // <-- the lie
           [[=Cancellable]]
    MockSchedulerBad {
        struct sender { /* … */ };
        sender schedule() const noexcept;
        // No schedule_bulk — Concepts::BulkScheduler<MockSchedulerBad> is false.

        bool operator==(const MockSchedulerBad&) const noexcept = default;
    };

}
```

The verifier fires. The diagnostic the user sees is exactly:

```
error: static_assert failed: "[Mashiro::Async] OffersBulk annotation does not match
BulkScheduler concept satisfaction (offending type:
Mashiro::Async::Backend::Mock::MockSchedulerBad)"
```

The message uses `Traits::NameOf<MockSchedulerBad>` to substitute the offending type. There
is no template-instantiation backtrace — the assertion lives inside `VerifyOneBackend<S>`
which is only ever invoked from the top-level `consteval` block, so the diagnostic surfaces at
the include line.

### 10.3 Composition violation

```cpp
struct [[=BackendTag{Backend::User}]]
       [[=BackendTag{Backend::User}]]            // duplicate — composition rule §5.4(1)
       [[=ProgressTag{stdexec::forward_progress_guarantee::weakly_parallel}]]
MockSchedulerDup { /* … */ };
```

Verifier output:

```
error: static_assert failed: "[Mashiro::Async] backend scheduler must carry exactly one
BackendTag (offending type: …::MockSchedulerDup, found 2)"
```

A duplicate `Allocates::Where` produces the equivalent `[Mashiro::Async] Allocates::Where
duplicate (offending type: …, where: OpState)` message.

### 10.4 Querying from a higher layer

L3 adaptor code reading from `MockScheduler` looks like:

```cpp
namespace Mashiro::Async::Adaptor {

    template<class Sched, class Range, class Fn>
    auto bulk(Sched sched, Range r, Fn fn) {
        if constexpr (Traits::OffersBulk_v<Sched>) {
            // Lower to native bulk dispatch.
            return Detail::NativeBulk(sched, r, std::move(fn));
        } else {
            // Fall back to let_value over when_all of unitary tasks (overview §8.3).
            return Detail::LetValueBulk(sched, r, std::move(fn));
        }
    }

}
```

`Traits::OffersBulk_v<MockScheduler>` is `true` — pure consteval, no virtual dispatch, no
runtime branch. The fallback path is dead code at instantiation. This is the entire point of
the L1 layer: a backend declares its capability, every higher layer specialises on it without
naming the backend.

---

## 11. Cross-references

- Overview §2 — design principles. This spec is the embodiment of "Concept-first",
  "Compile-time first", "Reflection drives schemas, not policy", "Annotations are capability
  tags, not identity".
- Overview §4 — namespace and header layout. This spec lands `Foundations.h`, `Concepts.h`,
  `Traits.h`.
- Overview §5.1 — scheduler concept hierarchy. Concept bodies are §4.4 above; bi-conditional
  with annotations is §8.
- Overview §5.2 — stop-token type. `Async::stop_token = stdexec::inplace_stop_token`; bridge
  to `std::stop_token` is §9.
- Overview §5.3 — allocation contract. L0/L1 are zero-allocation except for §9.3's documented
  one allocation per `bridge_stop_token` call.
- Overview §5.6 — capability annotation contract. Annotations are §5.2; verifier is §8.
- Overview §5.7 — completion-signature contract. Helpers are §7.
- Platform-thread spec §6.4 — `Mashiro::Platform::scheduler` becomes
  `Mashiro::Async::Backend::Platform::scheduler` via re-export (Subagent B's territory). No
  modifications to the platform spec.
- L2 spec (`02-backends.md`) — every backend's annotation set MUST be consistent with
  this spec's verifier.
- L3 spec (`03-adaptors.md`) — adaptors use `Traits::*_v` to specialise; this spec is the
  vocabulary they consume.
- L4 spec (`04-coroutine-tasks.md`) — `Task<T>` uses §7 helpers and `Traits::IsCancellable_v`.
- L7 spec (`07-extension.md`) — extension hook for user backends to opt into the §8 verifier.

---

## 12. Open issues

These could not be fully resolved within the overview's frozen vocabulary; they were flagged
for the synthesis pass (overview §9). Resolution status (per `09-synthesis.md`) is annotated
on each item.

1. **`get_io_context_t` location.** *(Resolved v0.2; see §4.5.)* The CPO is declared in
   `Foundations.h` as an empty pure-tag, and the L2 Io backend supplies the `tag_invoke`
   overload. Same pattern stdexec uses for `get_scheduler` / `get_stop_token`.
2. **`Cancellable` on senders vs schedulers.** §5.3 lists `Cancellable` as attaching to
   either a scheduler type or a sender expression type. The §8 verifier walks scheduler
   types only; a sender that *should* be cancellable but isn't annotated is not caught. The
   right place to enforce this is L3 (each adaptor declares its own cancellation contract);
   noted here so the L3 spec audits its adaptors against `Traits::IsCancellable_v`.
3. **`Allocates::Where::Frame` and HALO.** The `Frame` enumerator is meaningful only for
   coroutine task types (L4). Listing it as an L1 enum at L0 leaks an L4 concern down. Two
   options: (a) leave it here as a forward-compatibility hook (chosen for now), (b) move it
   to L4 and re-spec `Allocates` as scheduler-only at L0/L1. Subagent C should pick during
   L4 drafting; this spec is willing to drop the enumerator.
4. **`User` `BackendTag` and the verifier.** *(Resolved v0.2; see §8.1.)* L7 owns
   `Mashiro::Async::Extension::register_scheduler_v<T>`; the verifier discovers the union of
   the `Backend::*` namespace walk and every type `T` with `register_scheduler_v<T> == true`.
   Reflection-driven, no out-of-band registry.
5. **Re-entrancy of `template for`.** The verifier's discovery loop uses P1306
   expansion-statements over a `define_static_array` of `std::meta::info`. The toolchain's
   feature probes (per platform spec §4) cover P1306 + P3491 individually, but this spec
   composes them in a way the codebase has not yet exercised at scale. A small probe TU
   should land alongside the L0 header to verify behaviour before L2 depends on it.
6. **`BackendTag{Backend::Platform}` collision with `Affine{Backend::Platform}`.** The
   Platform backend will carry both. They are *not* synonyms — `BackendTag` is identity,
   `Affine` is capability. Documenting this here so the L2 spec's Platform alias section does
   not conflate them.
7. **`union_signatures` empty pack.** *(Resolved v0.2; per `09-synthesis.md` §2.3.)* The
   empty-pack case is a hard `static_assert` ("union of zero senders is meaningless"); L3
   confirmed this is the correct policy.
8. **Bridging the platform stop-source.** `Mashiro::Platform::stop_source` is already an
   `inplace_stop_source` (platform spec §6.7), so no bridge is needed when crossing into
   the platform layer. This is good — it means §9 is purely for `std::jthread` and migrating
   user code, never for platform interop. Documented so Subagent E does not waste a
   migration-plan paragraph on it.

---

## 13. Status

- v0.1 — drafted 2026-06-15. Locks the L0 re-export surface, the L1
  annotation set, the `Traits::*` query set, the completion-signature helpers, the consteval
  verifier, and the `bridge_stop_token` adaptor.
- **v0.2 (this document)** — incorporates synthesis-pass adjudications. Adds British
  `materialise`/`dematerialise` aliases (§4.3), `HasStopToken` concept (§4.4),
  `get_io_context_t` forward declaration (§4.5), `Detached` and `ScopeTag` annotations
  (§5.2 + §5.3), `IsDetached_v` and `ScopeTagOf` queries (§6.2), and the L7 opt-in
  discovery extension (§8.1). Resolves §12 open issues 1, 4, 7. The `Allocates::Where::Frame`
  placement (open issue 3) stays in L1 — `09-synthesis.md` §2.27 rejects the relocation
  because `Where` is meant to be uniform across the framework.
- v1.0: post-implementation revision after `Foundations.h` / `Concepts.h` / `Traits.h` land
  and the L2 backends compile against them end-to-end.

