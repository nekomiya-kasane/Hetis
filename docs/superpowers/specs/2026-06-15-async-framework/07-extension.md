# Mashiro Async Framework — L7 Extension Surface

**Status:** Draft v0.2 (subagent E deliverable; spawned by `00-overview.md` §8.5 v0.2)
**Date:** 2026-06-15 (v0.1) · 2026-06-16 (v0.2 synthesis pass)
**Author:** Mashiro Engine team — Subagent E
**Scope:** `Mashiro::Async::Extension` namespace; user-extension contracts for new schedulers, sender adaptors, domains, awaitables, plugin descriptors, type-erasure boundaries, and migration of pre-framework code.

### Revision history

- **v0.1** — initial draft. Locks the user-extension contract for new schedulers, sender adaptors, domains, awaitables, plugin descriptors, and the type-erasure boundary; specifies the migration playbook for legacy async idioms.
- **v0.2** — synthesis pass (see `09-synthesis.md` §2.25, §2.9, §7). §3.1 adds the
  `Mashiro::Async::Extension::register_scheduler_v<T>` opt-in trait that lets user-extension
  schedulers (residing in user namespaces, *not* `Mashiro::Async::Backend::*`) participate in
  the L1 verifier's discovery walk. §9.1 adds an explicit migration cross-reference for the
  `from_future` boundary adaptor that L4 v0.2 marked `[[deprecated]]` — the deprecation is a
  diagnostic nudge, not a removal warning, and L7 §9.1 (this section) is the canonical
  migration destination it points at.

---

## 1. Purpose

This layer specifies **how downstream code adds new types to the framework without touching the framework's headers**. It does *not* introduce new vocabulary — every extension axis below is a *recipe* for composing types that already exist in L0–L6 (see overview §3, §5).

L7 has two readers:

1. **Engine subsystem authors** who need a new scheduler (Vulkan compute, audio worklet, RDMA NIC), a new sender adaptor (`tracing(span)`, `bounded_concurrency(n)`), or a new awaitable (a Vulkan timeline-semaphore wait).
2. **Plugin authors** who load runtime-mutable code that exposes scheduler / adaptor functionality across an ABI boundary (hot-reloadable computation backends, scriptable post-processing chains).

The contracts in this spec are **frozen by overview §5**. A user-defined scheduler that satisfies `Concepts::Scheduler` and declares its capabilities through L1 annotations is indistinguishable from a framework-provided one at every higher layer — adaptors compose it, patterns dispatch over it, structured concurrency owns its work. The framework does not know whether a sender came from L2 or from a user header.

---

## 2. Extension axes

| Axis | What is added | Where it lands | ABI surface |
|---|---|---|---|
| New scheduler | A type modelling `Concepts::Scheduler` (and optionally `BulkScheduler`, `IoScheduler`, `AffineScheduler`) | User header, included where used | None at framework level — the type is a value, not a runtime registration |
| New sender adaptor | A free function returning a sender expression, plus an op-state struct | User header | None |
| New domain | A `stdexec::domain`-modelling struct with `transform_sender` overloads | User header, registered with one or more user schedulers | None |
| New awaitable | A type with `await_ready` / `await_suspend` / `await_resume` plus optional environment queries | User header | None |
| Plugin scheduler / adaptor | A user scheduler exposed across a shared-library boundary through a reflection-generated descriptor + a `Mashiro::Async::Extension::Plugin` runtime registry | Plugin shared library + descriptor TU | Frozen ABI per descriptor schema (§7) |
| Type-erased pipeline boundary | A point where `any_sender_of<Sigs...>` / `any_scheduler` is materialised; framework users opt in explicitly | User header | One vtable + one allocation per crossing — never on a hot path |
| Migration of legacy code | Mechanical rewrite of `std::async` / raw `std::thread` / manual `condvar` pipelines into framework primitives | User TUs | None — this is a code-change checklist, not a runtime shim |

The axes are independent. A new scheduler does not need a new domain; a new sender adaptor does not need a new awaitable. The combinations that *do* compose (scheduler + domain to lower its `bulk` to a native API, scheduler + awaitable to expose its native completion handle) are documented as worked examples.

---

## 3. New scheduler — worked example: Vulkan compute scheduler

This section walks the full extension contract using a "GPU compute scheduler backed by Vulkan compute queues" as the running example. The scheduler accepts senders whose `set_value` callback is invoked when a Vulkan compute submission has completed; it sits next to the engine's render scheduler without depending on it.

### 3.1 What the user must do

1. **Model `Concepts::Scheduler`.** Define a value type with `schedule()` returning a sender, `operator==`, and a `get_completion_scheduler<set_value_t>` advertisement (see overview §5.1).
2. **Declare the op-state.** A struct that holds the receiver, the Vulkan submission state, and an `inplace_stop_callback` for cancellation. The op-state must not allocate beyond what the receiver's environment allocator authorises (see §3 of `08-cross-cutting.md`).
3. **Attach L1 annotations** on the scheduler type to declare capabilities (`Backend::User`, `Cancellable`, `Allocates::OpState` if any submission state is heap-borne). The `Traits::*_v` queries pick up the annotations through reflection without any registry call.
4. **Optionally register a domain** for `bulk(s, n, fn)` to lower to a single dispatch over `vkCmdDispatch` rather than the framework's default `let_value` over `when_all` of unitary tasks. The domain is the scheduler's customisation point — see §5 below.
5. **Honour the cancellation contract.** `inplace_stop_callback` must call `vkQueueWaitIdle` (or, if available, `vkResetCommandPool` with `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`) on the submission's queue and complete the receiver with `set_stopped`. No exception path; the queue handle is owned by the scheduler.
6. **Honour the forward-progress contract.** Vulkan compute queues are concurrent (one queue is one stream of submissions). The annotation must therefore be `concurrent`, not `parallel` — `Traits::ProgressOf_v<VkComputeScheduler>` returns `concurrent`, which is enough for `IoScheduler`-style I/O composition but excludes the scheduler from `ParallelScheduler` requirements.
7. **Opt into the L1 verifier's discovery walk via `register_scheduler_v<T>`** (v0.2). The L1 verifier (`01-foundations.md` §8.1) only walks `Mashiro::Async::Backend::*` by default; user schedulers in user namespaces (e.g. `Engine::Compute::VkComputeScheduler`) must explicitly opt in so the verifier picks them up:

   ```cpp
   // User TU, e.g. Engine/Compute/VkComputeScheduler.cpp:
   namespace Mashiro::Async::Extension {
       template<>
       inline constexpr bool register_scheduler_v<Engine::Compute::VkComputeScheduler> = true;
   }
   ```

   The opt-in keeps the discovery walk reflection-driven (no static list inside the framework) while still requiring user schedulers to declare their participation. See §3.1b for the full contract.

### 3.1b `register_scheduler_v<T>` — user-side opt-in for the discovery walk (v0.2)

The synthesis pass (`09-synthesis.md` §2.25) adjudicated how user-defined schedulers in user namespaces are discovered by the L1 verifier (`01-foundations.md` v0.2 §8.1). The framework cannot enumerate user namespaces by reflection at framework-compile time — they do not exist yet. The opt-in trait is the canonical solution: every user scheduler that wants to participate in the verifier's invariants (capability-annotation consistency, concept satisfaction, allocation-policy declaration, cancellation-checklist enforcement) specialises a single trait in the `Mashiro::Async::Extension` namespace.

**The trait declaration (in `Mashiro/Async/Extension/Scheduler.h`):**

```cpp
namespace Mashiro::Async::Extension {

    // Default: not registered. User specialises this to opt their scheduler
    // into the L1 verifier's discovery walk.
    template<class T>
    inline constexpr bool register_scheduler_v = false;

} // namespace Mashiro::Async::Extension
```

**Use:**

```cpp
// In the user's scheduler-defining TU:
namespace Mashiro::Async::Extension {
    template<>
    inline constexpr bool register_scheduler_v<Engine::Compute::VkComputeScheduler> = true;
}

// The L1 verifier now walks Engine::Compute::VkComputeScheduler alongside the
// framework-provided backends in Mashiro::Async::Backend::*. Any annotation
// inconsistency (e.g. declaring [[=Async::Allocates{None}]] while the op-state
// declares a member of type std::optional<inplace_stop_callback<...>>) is now
// caught at framework-build time.
```

**What the verifier checks once the scheduler is registered:**

1. **Concept satisfaction matches annotations.** `[[=Async::OffersBulk]]` requires `Concepts::BulkScheduler<T>`; `[[=Async::OffersIo]]` requires `Concepts::IoScheduler<T>`; `[[=Async::Backend{Backend::User}]]` requires the type to declare `operator==`. (The full list lives in `01-foundations.md` v0.2 §8.1.)
2. **Capability-bit consistency.** If the scheduler declares `Cancellable` but the op-state has no `inplace_stop_callback` member, the verifier fails. The reflection helper that walks the op-state's members is `Detail::CountAnnotations` and friends from `01-foundations.md` v0.2 §6.2.
3. **Forward-progress declaration.** `get_forward_progress_guarantee` must be defined; the verifier exercises it as a constant expression.
4. **Allocation-policy declaration.** Every scheduler must declare exactly one `Allocates::Where` value, even if it is `None`.

**Cost.** The opt-in trait is a single `inline constexpr bool` — no runtime registry, no global initialisation, no DLL-export side effect. The verifier walks the trait specialisations at *framework-build time*, not at engine startup; user schedulers that fail any of the above checks fail to compile in the verifier's TU. Production builds are unaffected (`Mashiro::Async::Extension::VerifyExtensionScheduler<T>()` is consteval).

**Migration from manually-invoking the verifier.** L7 v0.1 documented `Mashiro::Async::Extension::VerifyExtensionScheduler<T>()` as the user-callable consteval verifier (§3.5). v0.2 keeps that helper unchanged — it is still callable for ad-hoc verification — but adds the discovery-walk opt-in trait so the framework's *centralised* verifier (in `01-foundations.md` §8.1) also picks the scheduler up automatically. Users may use either path; the trait is the lower-friction option for engines that have dozens of user-defined schedulers.

**Plugins.** Plugins delivered through the descriptor schema (§7) do **not** specialise `register_scheduler_v` — they are loaded at runtime, not at framework-build time, so the discovery walk cannot reach them by construction. Plugins satisfy their verifier obligations through the descriptor schema's capability-bit advertisement instead (§7.2).

### 3.2 Header sketch

```cpp
// User-defined scheduler. Lives in a user header, e.g. Engine/Compute/VkComputeScheduler.h.
// Depends only on L0/L1 framework headers + Vulkan headers; no framework backend included.

#include <Mashiro/Async/Foundations.h>          // L0 re-exports + L1 annotations
#include <Mashiro/Async/Concepts.h>             // BackendScheduler etc.
#include <Mashiro/Async/Extension/Scheduler.h>  // user-extension helpers (Section 3.5)
#include <vulkan/vulkan.h>

namespace Engine::Compute {

    using namespace Mashiro::Async;             // for Affine, OffersBulk, ...

    class [[=Async::Backend{Backend::User}]]
          [[=Async::Cancellable]]
          [[=Async::Allocates{Allocates::Where::OpState}]]
    VkComputeScheduler {
    public:
        // The scheduler is a value type — handle to a (queue, command-pool,
        // fence-pool) tuple owned elsewhere. Equality models "same queue".
        VkComputeScheduler() = default;
        explicit VkComputeScheduler(VkComputeQueueState& state) noexcept
            : state_{&state} {}

        bool operator==(const VkComputeScheduler&) const noexcept = default;

        // ---- The schedule sender ------------------------------------------
        struct sender;
        [[nodiscard]] sender schedule() const noexcept;

        // ---- Environment advertisement ------------------------------------
        struct env {
            VkComputeQueueState* state;
            template<class Tag>
            friend VkComputeScheduler tag_invoke(
                stdexec::get_completion_scheduler_t<Tag>, env e) noexcept {
                return VkComputeScheduler{*e.state};
            }
            template<class Tag>
            friend stdexec::forward_progress_guarantee tag_invoke(
                stdexec::get_forward_progress_guarantee_t, env) noexcept {
                return stdexec::forward_progress_guarantee::concurrent;
            }
        };

        // ---- Domain hook --------------------------------------------------
        struct domain;
        friend domain tag_invoke(stdexec::get_domain_t, VkComputeScheduler) noexcept {
            return {};
        }

    private:
        VkComputeQueueState* state_ = nullptr;
    };

}
```

### 3.3 Op-state sketch

The op-state owns the per-submission state: the command buffer recorded by `connect`, the fence the queue signals on completion, and the stop-callback. It deliberately keeps the receiver inline (no `std::function`, no virtual dispatch) — this is the same shape as `Mashiro::Platform::scheduler::sender::op_state` in the platform-thread spec section 6.5.

```cpp
struct VkComputeScheduler::sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(VkResult),
        stdexec::set_stopped_t()>;

    VkComputeQueueState* state;

    template<class Rcvr>
    struct op_state {
        Rcvr                                                    rcvr;
        VkComputeQueueState*                                    state;
        VkCommandBuffer                                         cmd     = VK_NULL_HANDLE;
        VkFence                                                 fence   = VK_NULL_HANDLE;
        std::optional<stdexec::inplace_stop_callback<StopFn>>   cancel;
        // No heap allocation — fence and cmd are drawn from per-queue pools
        // owned by VkComputeQueueState. The op-state's storage is the
        // receiver's stack frame (or its enclosing op-state's storage).

        void start() noexcept;
        void on_fence_signalled() noexcept;     // called by VkComputeQueueState's reaper

        struct StopFn {
            op_state* self;
            void operator()() noexcept {
                // Cancel path: queue-idle is the only portable way to retract
                // an in-flight Vulkan submission. The reaper observes the
                // returned fence + the cancel flag and routes to set_stopped.
                self->state->RequestCancel(self->fence);
            }
        };
    };

    template<class Rcvr>
    op_state<Rcvr> connect(Rcvr rcvr) const {
        return {std::move(rcvr), state, VK_NULL_HANDLE, VK_NULL_HANDLE, std::nullopt};
    }

    env get_env() const noexcept { return {state}; }
};
```

The reaper (an internal owner-of-the-queue thread, or a polling step inside `VkComputeQueueState`) observes `vkGetFenceStatus(fence) == VK_SUCCESS` and dispatches to `op_state::on_fence_signalled`, which clears `cancel`, releases the fence + command buffer back to the pool, and completes with `set_value` or (on cancellation) `set_stopped`.

### 3.4 Why this shape

- **No virtual dispatch.** The scheduler is a value, the sender is a value, the op-state is a struct. Every transition is a direct call after concept-check.
- **No global state.** `VkComputeQueueState` is owned by the engine's compute subsystem; the scheduler is a handle to it. Multiple subsystems can hold disjoint queue-states without interfering.
- **No surprise allocation.** Command buffers and fences are pooled. The op-state's storage is the receiver's storage (per overview section 5.3).
- **Cancellation is structural.** `inplace_stop_callback` is registered in `start`, dropped in completion. The framework's `Cancellable` annotation says "this scheduler completes promptly on stop"; without the annotation, adaptors that need the guarantee (`timeout`, `race`) refuse to compose at compile time (overview section 5.6).
- **Forward progress is `concurrent`.** Sufficient for chained submissions; `bulk` lowering is an option (Section 5), not a requirement.

### 3.5 The `Extension::Scheduler` helper header

`Mashiro/Async/Extension/Scheduler.h` provides three things and nothing else:

1. A CRTP-style mixin `SchedulerHelper<Derived>` that supplies the boilerplate `tag_invoke` overloads needed for `stdexec::scheduler` modelling (`schedule`, `get_completion_scheduler`, `get_forward_progress_guarantee`). Use of the helper is *optional* — a hand-written scheduler is equally valid.
2. A `consteval` verifier `VerifyExtensionScheduler<S>()` that asserts the declared L1 annotation set matches the concept satisfaction (e.g., declaring `OffersBulk` requires `BulkScheduler<S>` to be true). This is the same verifier framework backends run; users opt in by writing `consteval { Mashiro::Async::Extension::VerifyExtensionScheduler<VkComputeScheduler>(); }` in their TU.
3. An optional `Plugin::Register<S>(name)` registry entry point — only for users that need cross-DSO discovery (Section 7).

The helper has no runtime state and no per-scheduler storage cost. It is a header-only convenience.

### 3.6 Allocation policy

`VkComputeScheduler` declares `Allocates::Where::OpState` because each submission borrows a fence + command buffer slot from the queue's pool. The query `Traits::AllocatesIn_v<VkComputeScheduler>` returns `OpState`; the cross-cutting allocation contract (`08-cross-cutting.md` section 3) treats this as "permitted on hot path because it is pool-borne". If the scheduler later switched to per-submission `vkAllocateCommandBuffers`, the annotation would change to `Allocates::Where::External` and `Diagnostics::AllocCheck` would flag the regression.

### 3.7 Forward-progress declaration

`get_forward_progress_guarantee` returns `concurrent`. This excludes the scheduler from `ParallelScheduler`-requiring patterns (`parallel_for` will refuse to compose with it directly). The user may still write `parallel_for(StaticPool, range, fn)` and inside `fn` `co_await VkComputeScheduler.schedule()` — the boundary is sharp and the compiler enforces it.

### 3.8 Cancellation in detail

`vkQueueWaitIdle` is the user's only portable retraction primitive. When `inplace_stop_callback` fires:

1. `StopFn::operator()` calls `state_->RequestCancel(fence)`. The queue-state sets a "cancel pending" flag keyed on the fence handle.
2. The reaper thread, on its next polling pass, checks the flag before checking `vkGetFenceStatus`. If cancel is pending and the fence is still unsignalled, the reaper calls `vkQueueWaitIdle` for the queue (which blocks until *all* in-flight work on that queue has drained — Vulkan does not retract individual submissions). After idle, the cancelled fence's op-state completes with `set_stopped`.
3. If the fence had already signalled by the time the reaper got there, the cancel is racing with completion; the reaper ignores the cancel flag and completes with `set_value`. This is correct: `set_stopped` is a best-effort signal, not a guarantee of "did not run".

The `Cancellable` annotation lets users distinguish the two outcomes via `let_stopped`. The forward-progress annotation `concurrent` is unaffected — concurrency is about steady-state, not cancel latency.

### 3.9 ABI considerations

The user-defined scheduler is an in-process value type; its ABI surface is whatever the compiler chooses for `VkComputeScheduler` and its `sender` / `op_state`. There is **no framework ABI** — the scheduler is consumed by template instantiation at every call site, not through a vtable. Type-erased boundaries (Section 8) are the only place where ABI stability becomes relevant.

If the user wants the scheduler to be addressable by name across translation units (e.g., a plugin loader passing it through a `void*` to engine code), Section 7's plugin descriptor schema applies.

### 3.10 Reaper sketch — completing fences without burning a thread

A naive Vulkan-fence-based scheduler dedicates one thread per queue to `vkWaitForFences(VK_TRUE, UINT64_MAX, ...)`. That works but burns a thread; the framework's expectation is that user schedulers integrate with the existing async fabric so the engine has one fewer dedicated thread to budget.

Two patterns are recommended:

1. **Polling reaper on the engine's existing compute thread.** If the engine already has a thread that orchestrates compute submissions (most do), the reaper is a periodic step on that thread: `for (auto& fence : pending) if (vkGetFenceStatus(...) == VK_SUCCESS) complete(fence);`. The cadence is the engine's frame cadence; latency is bounded by frame time. No new thread.
2. **`VK_KHR_external_fence_fd` + Io backend.** On Linux with the extension, the Vulkan fence is exported as a `dma-buf` fd and submitted to the framework's `Io` backend as an `epoll`-watched read; completion is observed natively without a polling thread. This is the higher-quality path; falls back to (1) when the extension is unavailable.

The reaper does not block in `vkWaitForFences`. It polls, and the polling cadence is set by whatever thread it shares with. The framework's `Cancellable` annotation is honoured: when a fence's op-state is cancelled, the reaper observes the cancel flag on its next pass and routes to `set_stopped` regardless of the underlying fence state.

### 3.11 Common pitfalls when authoring a new scheduler

| Pitfall | Why it bites | Fix |
|---|---|---|
| Holding the receiver by reference instead of by value | Receiver lifetime is the op-state's storage; reference is fine but interferes with op-state move semantics | Hold by value; `Rcvr` is concept-checked |
| Throwing from `start()` | `start` is `noexcept`; throw breaks the op-state contract | Route failure to `set_error` |
| Forgetting `get_completion_scheduler` | Adaptors cannot fold `continues_on(s, sched)` away when they should | Implement the env query (Section 3.2) |
| Forgetting `get_forward_progress_guarantee` | `ParallelScheduler` queries fail; patterns like `parallel_for` reject the scheduler | Default is `concurrent`; declare explicitly |
| Annotating `Cancellable` without registering a callback | Compile-time verifier fails | Register the callback or drop the annotation |
| Allocating in `start` without an annotation | `Diagnostics::AllocCheck` flags it | Annotate `Allocates::Where::OpState` (or Frame, Output, External) |
| Comparing schedulers by address (`this == &other`) | Schedulers are values; multiple handles to the same backing state should compare equal | Compare by the backing-state pointer or by an equivalent stable id |

---

## 4. New sender adaptor

A user-defined adaptor is a **function template** that consumes upstream senders and returns a sender expression. The framework provides three things at L3 that user adaptors should reuse rather than reinvent: `stdexec::sender_adaptor_closure` for pipe-syntax compatibility, the explicit `completion_signatures_of_t` declaration (overview section 5.7), and the cancellation checklist (`08-cross-cutting.md` section 2).

### 4.1 Worked example: `tracing(span_name)` adaptor

`tracing(name)` annotates a sender with a structured-trace span; the span begins on `start` and ends on the first completion (`set_value` / `set_error` / `set_stopped`). The adaptor is observably-equivalent to its argument — its only effect is on the diagnostics surface.

```cpp
namespace Engine::Diag {

    template<stdexec::sender Upstream>
    struct tracing_sender {
        using sender_concept = stdexec::sender_t;

        // Completion signatures are *exactly* upstream's — tracing adds no new
        // channel. This is the rule, not the exception: an adaptor that
        // changes signatures is a different operator (e.g., with_error<E>),
        // not a tracing wrapper.
        template<class Env>
        using completion_signatures =
            stdexec::completion_signatures_of_t<Upstream, Env>;

        Upstream                upstream;
        std::string_view        name;

        template<class Rcvr>
        struct op_state {
            // Wrap rcvr so we observe its completion. The wrapper forwards
            // every channel; on first invocation it ends the span.
            struct rcvr_wrapper {
                using receiver_concept = stdexec::receiver_t;
                Rcvr            inner;
                Mashiro::Async::Diagnostics::Span span;

                template<class... A>
                friend void tag_invoke(stdexec::set_value_t, rcvr_wrapper&& r, A&&... a) noexcept {
                    r.span.End();
                    stdexec::set_value(std::move(r.inner), std::forward<A>(a)...);
                }
                friend void tag_invoke(stdexec::set_stopped_t, rcvr_wrapper&& r) noexcept {
                    r.span.EndCancelled();
                    stdexec::set_stopped(std::move(r.inner));
                }
                template<class E>
                friend void tag_invoke(stdexec::set_error_t, rcvr_wrapper&& r, E&& e) noexcept {
                    r.span.EndError();
                    stdexec::set_error(std::move(r.inner), std::forward<E>(e));
                }
                friend stdexec::env_of_t<Rcvr>
                tag_invoke(stdexec::get_env_t, const rcvr_wrapper& r) noexcept {
                    return stdexec::get_env(r.inner);
                }
            };

            // Inner op-state of the upstream connected to the wrapped receiver.
            stdexec::connect_result_t<Upstream, rcvr_wrapper> inner;

            void start() noexcept { stdexec::start(inner); }
        };

        template<class Rcvr>
        op_state<Rcvr> connect(Rcvr rcvr) && {
            using W = typename op_state<Rcvr>::rcvr_wrapper;
            return {stdexec::connect(std::move(upstream),
                W{std::move(rcvr), Mashiro::Async::Diagnostics::Span::Begin(name)})};
        }
    };

    inline auto tracing(std::string_view name) {
        return stdexec::sender_adaptor_closure{
            [name]<stdexec::sender U>(U&& u) {
                return tracing_sender<std::decay_t<U>>{std::forward<U>(u), name};
            }
        };
    }

}
```

Use site: `auto traced = compute_pipeline() | Engine::Diag::tracing("frame");`

### 4.2 Completion-signature declaration

Every framework adaptor declares its `completion_signatures` explicitly (overview section 5.7). The user adaptor follows the same rule:

- **Pass-through adaptors** (`tracing`, `bounded_concurrency`, `with_priority`) inherit `completion_signatures_of_t<Upstream, Env>`.
- **Adaptors that may add an error channel** (`retry_with_log`) opt into `with_error<E>` *explicitly* in the declared signatures. Silent error introduction is a review-time reject.
- **Adaptors that may complete stopped** (`timeout`, `race`) declare `set_stopped_t()` in their signatures even if the upstream did not.
- **Adaptors that erase signatures** (`materialise`) follow stdexec's reification convention — completion signatures of the materialised sender are `set_value_t(materialised<...>)`.

A consteval helper `Extension::DeclareSignatures<S>()` cross-checks the declared signatures against what the op-state actually invokes (`set_value` / `set_error` / `set_stopped` calls inside the op-state's body, walked by reflection). The check is opt-in per adaptor; framework adaptors run it.

### 4.3 Op-state design rules

A user op-state must:

1. **Hold the receiver by value.** Never by pointer or by reference — the receiver outlives the op-state by construction (the receiver's lifetime is *the* outer storage), but holding by reference makes the op-state non-movable, which interferes with `let_value` and friends.
2. **Own its dependencies.** Inner op-states (from connecting to upstream senders) live as data members. The framework's storage discipline assumes the op-state graph is a tree, not a DAG.
3. **Provide `start() noexcept`.** `start` may not throw. Allocation, OS calls, and any other failure-prone work happens here, with failure routed to `set_error` rather than thrown.
4. **Drop the cancel callback before completing.** If the op-state holds an `inplace_stop_callback`, the callback's destructor *must* run before any of `set_value` / `set_error` / `set_stopped` — otherwise the callback can fire on a moved-from receiver. The pattern is `cancel.reset(); set_value(std::move(rcvr));`.
5. **Be move-only or pinned.** The framework treats op-states as pinned (address-stable) once `start` has been called. The user adaptor must respect this: if it stores pointers into its own op-state, those pointers must not survive a move-construction; alternatively, the op-state is `[[clang::no_unique_address]]`-friendly and trivially-destructible-by-move-construction.

### 4.4 Cancellation checklist for user adaptors

Every user adaptor that owns external state (timers, file handles, child senders, OS resources) must satisfy:

- [ ] On `start`, register an `inplace_stop_callback` against `stdexec::get_stop_token(stdexec::get_env(rcvr))`.
- [ ] The callback must promptly initiate cancellation of the external state. "Promptly" means "before the next suspension point of the calling chain"; long-running cancel paths (waiting for a kernel `IOCP` cancel reply) run on a reaper thread, not in the callback body.
- [ ] On any of the three completion channels, drop the callback (`cancel.reset()` *before* `set_*`).
- [ ] Cancellation completes the receiver with `set_stopped`, never with a synthesised error.
- [ ] The adaptor declares `set_stopped_t()` in its completion signatures.
- [ ] A pass-through adaptor that does not own external state still propagates the upstream's cancellation by *not interfering* — the wrapped receiver's environment must forward the stop-token unchanged.

The checklist is mechanical; the consteval verifier `Extension::CancellationCheck<S>()` (header-only helper in `Extension/Sender.h`) inspects the op-state's reflected member set and complains if a `Cancellable` annotation is missing when the op-state holds an `inplace_stop_callback`, or if the adaptor declares `Cancellable` without registering a callback.

### 4.5 ABI considerations

User adaptors are template instantiations; their ABI surface is whatever the compiler emits for the instantiated `op_state` and `sender` types. **Do not** put a user adaptor behind `any_sender_of` unless the call site genuinely needs ABI stability — the type erasure adds a vtable + at least one allocation per crossing (Section 8). The recommended pattern is to keep adaptor types fully visible and let the compiler inline.

If a user adaptor must cross a DSO boundary (plugin scenario), follow the plugin-descriptor schema in Section 7. The adaptor itself stays in the plugin DSO; the descriptor exports a stable factory function plus a frozen completion-signature schema.

---

## 5. New domain

A **domain** is a customisation point for `transform_sender`. It rewrites sender expressions at construction time — never at execution. Domains are the right tool when the user has a scheduler with a more efficient native API for some algorithms (`bulk` lowering to `tbb::parallel_for`, `vkCmdDispatch`, `clEnqueueNDRangeKernel`, an `MPI_Bcast`, or similar) and wants the framework's generic algorithm machinery to dispatch to that native API automatically.

### 5.1 When a domain is appropriate

Use a domain when **all** of:

1. The rewrite is **structurally observable-equivalent** — the rewritten sender produces the same value sequence on the same channels with the same scheduler-affinity.
2. The native path is **strictly cheaper** — fewer allocations, fewer thread hops, native scheduling primitives unavailable to the generic lowering.
3. The rewrite is **decidable from the sender expression's static type** — the domain inspects the algorithm tag and the upstream type, not runtime state.

Example: `bulk(VkComputeScheduler, n, fn)` lowers to a single `vkCmdDispatch(n)` rather than `n` separate `vkComputeScheduler.schedule()` chains. The native path is asymptotically faster (one submission vs `n`) and the result is observably the same.

### 5.2 When a domain is NOT appropriate

A domain may **not**:

- **Change observable semantics.** Domains are an optimisation, not a redirection. A domain that rewrote `then(s, f)` to `then(s, g)` would be a bug.
- **Inject side effects.** Tracing / logging is an *adaptor* (Section 4.1), not a domain. Adaptors compose explicitly; domains would inject implicitly, which makes diagnostics depend on backend choice.
- **Change cancellation semantics.** A rewrite that elided a `Cancellable` guarantee would compile but break user expectation. The framework verifies this: any rewrite of a `Cancellable`-tagged sender must produce a `Cancellable`-tagged sender.
- **Bridge to a foreign scheduler silently.** If the rewrite needs to escape to a different scheduler, the domain must produce a sender expression whose `get_completion_scheduler` advertisement reflects the new scheduler. Silent migration to a different scheduler is rejected by the consteval audit.

### 5.3 Worked example: `VkComputeScheduler::domain`

```cpp
struct VkComputeScheduler::domain {
    // Default rule: defer to the algorithm's default lowering.
    template<stdexec::sender_expr S, class Env>
    decltype(auto) transform_sender(S&& s, const Env& env) const {
        return stdexec::default_domain{}.transform_sender(std::forward<S>(s), env);
    }

    // Specialise bulk(s, n, fn) when s completes on VkComputeScheduler.
    template<stdexec::sender Inner, class Env>
        requires Mashiro::Async::Traits::CompletesOn<Inner, VkComputeScheduler>
    auto transform_sender(Mashiro::Async::Adaptor::bulk_sender<Inner, std::size_t, /*Fn*/auto> s,
                          const Env& env) const {
        // Inspect Fn — if it is a stateless function pointer or an empty
        // lambda, we can compile it into a SPIR-V dispatch ahead of time and
        // submit a single vkCmdDispatch(n). The result is a sender that
        // completes when the GPU fence signals.
        return Detail::CompileAndSubmitDispatch(std::move(s), env);
    }
};
```

The framework discovers the domain through `tag_invoke(get_domain_t, scheduler)`. Sender expressions whose root scheduler is `VkComputeScheduler` route through this domain at construction; sender expressions on other schedulers are unaffected.

### 5.4 Capability injection through a domain

A common legitimate use of a domain is **annotation propagation**: when an upstream sender is `Cancellable` and the adaptor preserves cancellation, the domain rewrites the resulting sender to carry the `Cancellable` annotation explicitly so downstream `Traits::IsCancellable_v` queries succeed. The framework's default lowering does this automatically for built-in adaptors; user adaptors that do not propagate via reflection need the domain rewrite to lift the annotation.

### 5.5 Registration

A domain is registered with a scheduler by `tag_invoke(get_domain_t, scheduler) -> domain` (overview section 5.5). There is no global registry. The scheduler owns its domain by type identity; the framework picks it up through ADL when the sender expression is constructed.

If the user wants to *additionally* extend a framework-provided domain (e.g., add a rewrite that the `Tbb` domain does not provide), the recommended pattern is **layered domains**: the user defines a domain that defers to the framework's domain for unknown rewrites and adds rewrites of its own. There is no inheritance — composition is by `transform_sender` delegation.

---

## 6. New awaitable

A user awaitable is any type with `await_ready` / `await_suspend` / `await_resume`. Awaiting a user awaitable inside a `Task<T>` or `Stream<T>` follows C++26 coroutine rules; the framework adds two requirements on top.

### 6.1 Awaiter shape

```cpp
struct VkTimelineWaiter {
    VkDevice    device;
    VkSemaphore timeline;
    uint64_t    target_value;

    bool await_ready() const noexcept {
        uint64_t v = 0;
        vkGetSemaphoreCounterValue(device, timeline, &v);
        return v >= target_value;
    }

    template<class Promise>
    void await_suspend(std::coroutine_handle<Promise> h) noexcept {
        // Forward the promise's stop-token so cancellation works.
        auto env  = h.promise().get_env();
        auto stop = stdexec::get_stop_token(env);
        // Register a stop-callback that signals an internal completion path.
        // The reaper polls vkGetSemaphoreCounterValue and resumes h when
        // target_value is reached, OR sets a cancel flag and resumes through
        // the stopped path.
        Detail::Reaper::Register(this, h, stop);
    }

    void await_resume() {
        if (Detail::Reaper::Cancelled(this)) {
            // Surface as set_stopped on the coroutine's promise. Task<T>
            // recognises this and unwinds; user code never sees an exception.
            throw Mashiro::Async::Coro::stopped_signal{};
        }
    }
};
```

### 6.2 Integration with `Task<T>` and `Stream<T>`

`Task<T>` and `Stream<T>` are scheduler-affine (overview section 6.2). After `await_resume`, the coroutine is rescheduled onto the task's bound scheduler — the user awaitable does not need to do this manually. If the awaitable wants to *opt out* of re-scheduling (e.g., because its `await_resume` is cheap and the caller is happy on whatever thread the awaiter completed on), it must declare an `as_awaitable_for<scheduler>` trait that the task framework reads via reflection.

### 6.3 Scheduler-affinity considerations

Awaiting a user awaitable inside `Task<T>` always re-schedules onto the task's scheduler after `await_resume` returns. This is the same rule that applies to awaiting a sender: the task's `default_task_context<Sched>` binding (P3941) takes precedence over wherever the awaitable's `await_suspend` resumed the coroutine.

If the user awaitable is itself a sender (some are: stdexec's senders are awaitable when their environment supports it), the framework's awaitable bridge in L4 (`04-coroutine-tasks.md` section 5) handles the dispatch correctly. Users authoring custom awaitables that are *not* senders need to mind the affinity manually if they care about the resumption thread.

### 6.4 Cancellation in awaitables

A user awaitable that owns external state (a Vulkan timeline wait, a `WaitForSingleObjectEx`, a libuv handle) must register a stop-callback during `await_suspend` and tear down in `await_resume`. The pattern mirrors the sender-adaptor checklist (Section 4.4). If the awaitable does not own external state (it just polls a value), no callback is needed; cancellation is observed naturally on the next `co_await`.

---

## 7. Plugin descriptors — runtime-loadable schedulers and adaptors

The plugin path is the **only** place where the framework crosses an ABI boundary. Plugins are shared libraries (`.dll` / `.so`) loaded at runtime — typical use cases are hot-reloadable computation backends (an experimental compute scheduler the engine wants to swap without a relink) and scriptable post-processing chains (a user-authored adaptor delivered by an asset pipeline). The plugin descriptor schema is **frozen** across plugin versions; everything else (the plugin's actual scheduler / adaptor types) is recompile-required.

### 7.1 Use case: hot-reloadable computation backend

The engine's compute subsystem ships with `VkComputeScheduler` baked in. A plugin DSO can deliver a *second* scheduler (`OpenCLComputeScheduler`, `CudaStreamScheduler`, `MetalCommandBufferScheduler`) without the engine needing to know about it at compile time. The plugin's descriptor advertises the scheduler's capability set, completion signatures, and lifetime ABI; the engine's plugin loader materialises a type-erased `any_scheduler` over the descriptor and passes it through to user code via a runtime registry.

The cost is the type erasure: `any_scheduler` carries a vtable, and each `schedule()` call allocates one op-state on the receiver's environment allocator (or on the heap if no allocator is supplied). This is fine for plugin-level dispatch, where the latency of one allocation per submission is dwarfed by the work itself; it is **not** acceptable on a hot path (Section 8).

### 7.2 Descriptor schema

The descriptor is a POD struct, exported with C linkage by every plugin DSO. The schema is reflection-generated on the engine side from the framework's concept set; the plugin's build system links against a header-only "plugin SDK" derived from the same schema.

```cpp
namespace Mashiro::Async::Extension::Plugin {

    // Frozen across plugin versions. Adding a field is a breaking change.
    struct DescriptorV1 {
        uint32_t        schema_version;          // = 1
        const char*     plugin_name;             // null-terminated UTF-8
        const char*     plugin_uuid;             // 36-char canonical UUID
        uint32_t        capability_bits;         // BackendBits + CapabilityBits
        const char*     completion_signatures;   // canonical mangled string

        // Type-erased factory for the scheduler. Plugin owns the vtable.
        struct SchedulerVTable {
            void  (*destroy)(void* sched) noexcept;
            void  (*schedule)(void* sched, void* receiver_storage) noexcept;
            bool  (*equal)(const void* a, const void* b) noexcept;
            // ... extension hooks per concept (bulk, io)
        };

        const SchedulerVTable*  scheduler_vtable;
        void*                   scheduler_state;
    };

    // Exported by the plugin DSO with this fixed name.
    extern "C" __declspec(dllexport) const DescriptorV1* MashiroPluginDescriptor() noexcept;

}
```

The fields are deliberately minimal: the plugin's scheduler state is opaque to the engine, the vtable is the only call surface, and the completion-signature string is the only schema check the engine performs at load time.

### 7.3 ABI stability rules

| Aspect | Frozen across plugin versions | Recompile-required when changed |
|---|---|---|
| `DescriptorV1` layout | Yes | Adding a field bumps `schema_version` → engine-side branch |
| `SchedulerVTable` layout | Yes per major | Adding a hook bumps the major version |
| Plugin's own scheduler / op-state types | No (opaque) | Plugin recompiles freely |
| L1 capability annotations | Yes (encoded as bits in `capability_bits`) | Adding a capability bit is additive |
| Completion-signature canonical string format | Yes | Format is documented in `Plugin/Schema.h` |

The plugin descriptor's frozen surface is **deliberately small**. We resist the temptation to expose more — every additional frozen field is a future migration cost, and plugins genuinely need only the type-erased vtable plus the capability advertisement.

### 7.4 Engine-side loading

```cpp
// Engine-side. Loads a plugin DSO and registers its scheduler with the
// runtime registry. Returns an any_scheduler bound to the plugin's
// scheduler_state through the descriptor's vtable.
auto LoadComputePlugin(std::filesystem::path dso) -> any_scheduler;
```

The engine treats the resulting `any_scheduler` as it would any other type-erased scheduler — it composes through `continues_on`, queries capabilities through the declared bits, and observes cancellation through the vtable's stop-handling hook. Hot-reload is supported by unloading the DSO after `scope.on_empty()` settles; the engine's plugin manager owns the DSO handle and unloads it under the same structured-shutdown discipline as everything else.

### 7.5 What plugins cannot do

- **Plugins cannot define new annotations.** `capability_bits` enumerates the L1 annotation set known to the engine at the time the plugin SDK was generated. New capabilities in newer plugin SDKs are simply unknown bits to older engines — the plugin loader rejects them with a structured-log warning.
- **Plugins cannot redefine vocabulary.** A plugin that wants to expose a sender adaptor exposes it as a *named factory* in its descriptor; the engine wraps the resulting senders in `any_sender_of<...>`. Plugins do not export new types.
- **Plugins cannot bypass cancellation.** The vtable's `schedule` hook is required to register a stop-callback against the receiver's environment; the engine verifies this at load time by submitting a cancellation probe (a no-op sender that immediately requests stop).

---

## 8. Type-erasure boundary

`any_sender_of<Sigs...>` and `any_scheduler` are the framework's escape hatches for code that genuinely cannot template. They are documented carefully because the temptation to reach for them is high and the cost is real.

### 8.1 When type erasure is appropriate

- **Module / DSO boundaries.** A plugin loader cannot template across a runtime boundary; `any_scheduler` is the right tool (Section 7).
- **Heterogeneous storage.** A scene-graph node that stores "the next async event handler" in a `std::vector<any_sender_of<set_value_t()>>` cannot template; the storage itself is type-erased.
- **Public C++ API surface for tools / scripting.** A debugger / inspector that wants to introspect a pipeline at runtime needs the type-erased shape; the inspected pipeline has nothing to lose by exposing the erased view.

### 8.2 When type erasure is forbidden

- **Anywhere on a hot path.** Every `any_sender_of` connection allocates and incurs a virtual call on every channel (`set_value` / `set_error` / `set_stopped`). On a per-frame / per-event hot path, this is rejected at review.
- **Inside a pipeline.** A pipeline composed of templated adaptors that suddenly has an `any_sender_of` in the middle has lost the compile-time fold for everything downstream of it. Either erase at the boundary (input or output) or not at all.
- **For schedulers chosen at compile time.** If the choice between `Tbb` and `StaticPool` is a build-system decision, the answer is `if constexpr` and a single template parameter, not `any_scheduler`.

### 8.3 Exact ABI cost

| Operation | Cost (approximate) |
|---|---|
| Construct `any_sender_of` from a concrete sender | One allocation (op-state + vtable pointer); `O(sizeof(op_state))` move |
| `connect`(any_sender, receiver) | One allocation (erased receiver wrapper); virtual call to inner connect |
| `start` | Virtual call |
| `set_value` / `set_error` / `set_stopped` | Virtual call |
| `any_scheduler::schedule()` | One allocation (sender state); virtual call |
| Equality (`any_scheduler::operator==`) | One virtual call (returns false unless both erase the same concrete type) |

Numbers are illustrative — the exact cost depends on inliner behaviour and allocator choice. The point is that **every operation on a type-erased value is at minimum one virtual call and possibly one allocation**, vs. zero for the templated path. Patterns like `parallel_for` that issue many `schedule()` calls amplify this linearly.

### 8.4 The boundary discipline

The recommended pattern is **erase at the boundary, template inside**:

```cpp
// Public API surface — type-erased.
auto LoadAndRunPipeline(any_sender_of<set_value_t(Image)> input) -> Task<Image>;

// Inside the implementation — fully templated, no erasure cost on the
// hot path.
template<stdexec::sender S>
Task<Image> RunPipelineImpl(S input) {
    auto preprocessed = co_await (input | preprocess | denoise | tonemap);
    co_return preprocessed;
}

// At the boundary, peel the erasure.
auto LoadAndRunPipeline(any_sender_of<set_value_t(Image)> input) -> Task<Image> {
    return RunPipelineImpl(std::move(input));
}
```

The erasure cost is paid once at the call site of `LoadAndRunPipeline`; the pipeline itself is templated. This is the only acceptable pattern.

---

## 9. Migration of existing code

Pre-framework code in the engine uses three legacy idioms: `std::async`, raw `std::thread`, and manual `std::condition_variable` pipelines. Migration is mechanical; this section is the checklist.

### 9.1 `std::async` → `Task<T>` or sender expression

| Legacy | Framework |
|---|---|
| `std::async(std::launch::async, fn)` returning `std::future<T>` | `Task<T>` started on `StaticPool::scheduler()` via `start_detached` or `co_await` |
| `future.wait()` | `co_await task` (inside another task) or `sync_wait(task)` (at top level) |
| `future.get()` | `co_await task` returns the value; exceptions propagate via `set_error(exception_ptr)` |
| `std::async(std::launch::deferred, fn)` | `let_value(just(), [&] { return fn(); })` — runs lazily on the awaiting context |

The migration is straightforward when the legacy code is "compute-bound, return one value, wait for it". `std::async`'s thread-of-execution semantics become "completes on whatever scheduler the task's environment binds"; the framework's default for a free `Task<T>` is the `StaticPool` (configured at engine init).

#### 9.1b `Async::from_future(std::future<T>)` — the migration boundary (v0.2)

L4 v0.2 marked `Async::from_future` with `[[deprecated("migration boundary — see 07-extension.md §7.5")]]` (`04-coroutine-tasks.md` v0.2 §7.3). The deprecation is **not** a removal warning — `from_future` stays in the headers for the foreseeable future so legacy code keeps compiling. The deprecation is a *diagnostic nudge*: every call site surfaces a warning, prompting the author to migrate to one of the §9.1 / §9.3 framework primitives.

The migration boundary's structural shape:

```cpp
// Legacy boundary (still compiles, raises a deprecation diagnostic):
auto x = co_await Async::from_future(std::move(legacy_future));

// Step 1 (preferred): replace the producer of legacy_future. If the producer
// is also engine code, migrate it to return a sender or Task<T> directly per
// §9.1's table; from_future then has no caller.
auto x = co_await produce_x_sender();   // produce_x_sender returns a Task<T> or sender

// Step 2 (when the producer is third-party code we cannot rewrite): keep the
// boundary at the precise call site, but use Async::Extension::from_future
// — the *non-deprecated* spelling — to mark that this is a deliberate, audited
// boundary and not an accidental new use. The two are observationally
// identical; the spelling difference is a code-review signal.
auto x = co_await Async::Extension::from_future(std::move(third_party_future));
```

The `Async::Extension::from_future` overload is **identical in implementation** to `Async::from_future` (same `std::jthread` poller, same boundary semantics) — the rename is purely a marker for "this boundary is intentional and reviewed". Reviewers reject patches that introduce new `Async::from_future` call sites; `Async::Extension::from_future` requires sign-off but is permitted at known third-party boundaries.

Long-term, the framework's intent is for the deprecated spelling to remain available but to never appear in the engine's own sources. Third-party boundaries audited and tagged with `Async::Extension::from_future` are tracked in the engine's `docs/integration/foreign-async-boundaries.md` ledger (out-of-scope for this spec) so an inventory exists.

### 9.2 Raw `std::thread` → structured task on a scheduler

| Legacy | Framework |
|---|---|
| `std::thread t([&] { worker(); }); t.detach();` | `start_detached(StaticPool.schedule() | then([&] { worker(); }))` — but prefer to spawn on a `Scope` |
| `std::thread t([&] { worker(); }); t.join();` | `co_await (StaticPool.schedule() | then([&] { worker(); }))` |
| `std::jthread` with a stop-token | `Nursery::spawn(...)` — the nursery's stop-token is the framework's stop-token |
| `std::this_thread::sleep_for(d)` | `co_await async_timer(d)` (from `Io` scheduler) |

Detached threads are a **leak** in the framework's worldview; the migration target is always a `Scope` or `Nursery`. The cross-cutting `08-cross-cutting.md` section 6 has the detailed playbook.

### 9.3 Manual `condition_variable` pipeline → `Stream<T>` or sender pipe

| Legacy | Framework |
|---|---|
| `std::queue<T>` + `std::mutex` + `std::condition_variable` | `Stream<T>` (L4 pull-driven) or `pipeline(...)` (L6 push-driven) |
| `cv.notify_one()` after enqueue | `Channel<T>::push(t)` — wakes the awaiting `next` sender |
| `cv.wait(lock, [&] { return !q.empty() || done; })` | `co_await stream.next()` — returns `optional<T>` (empty on close) |
| `done = true; cv.notify_all();` | Stop-token request — observed by every `next` op-state |

The manual condvar pipeline almost always has a "done" flag; the framework's stop-token absorbs that flag. Migration eliminates one synchronisation primitive per pipeline.

### 9.4 Migration checklist for a single subsystem

1. **Identify the legacy primitives.** Search for `std::async`, raw `std::thread`, `std::condition_variable`, `std::promise`, `std::future` in the subsystem's headers and sources.
2. **Determine the right framework primitive.** Use overview section 6.7 ("modelling guide — pick the right tool"). Compute-bound one-shot work → `Task<T>` on `StaticPool`. Iteration over async values → `Stream<T>`. Fan-out with bounded lifetime → `Nursery`. Throughput pipeline → reactive `pipeline`.
3. **Identify the ownership boundary.** Detached work needs a `Scope`. Awaited work needs a parent `Task<T>` or a `sync_wait` at the top level.
4. **Identify the stop signal.** Replace `done` flags, `closed_` booleans, and sentinel values with the propagated stop-token. The token flows through every receiver's environment automatically.
5. **Identify the error path.** Replace `try/catch` around `future.get()` with `set_error(exception_ptr)` propagation; replace bool-return + errno with `Result<T>` (overview ties it through `with_error<E>`).
6. **Run the framework's allocation audit.** `Diagnostics::AllocCheck` flags unintended heap allocations; the migration target is zero allocations on the hot path. Coroutine frames may allocate when HALO does not apply — document each at the call site.
7. **Add a regression test.** A unit test that stresses the new pipeline under cancellation; the framework's `Diagnostics::scope_audit()` catches leaks.

### 9.5 What does NOT migrate

- **Code interfacing with foreign async APIs.** A subsystem that hands a `std::future` to a third-party library keeps the `std::future` at the boundary; the migration is internal to the subsystem.
- **Code that genuinely needs `std::thread` semantics.** Long-running OS-blocking work (a polling loop on `XInputGetState`, a sync `ReadDirectoryChangesW`) stays on a dedicated `std::jthread` because stdexec cannot suspend a blocking syscall. The thread's *event-emit path* migrates (it pushes to an MPSC inbox observed by the framework); the thread itself stays.
- **One-line `std::this_thread::sleep_for(...)` in tests.** The test cost is zero and the migration cost is non-zero. Leave it.

---

## 10. Status

- v0.1 (initial draft): drafted 2026-06-15 by Subagent E. Locks the user-extension contract for new schedulers, sender adaptors, domains, awaitables, plugin descriptors, and the type-erasure boundary; specifies the migration playbook for legacy async idioms.
- **v0.2 (this revision):** synthesis-pass adjustments landed 2026-06-16. §3.1 step 7 + §3.1b add the `Mashiro::Async::Extension::register_scheduler_v<T>` opt-in trait so user-namespace schedulers participate in the L1 verifier's discovery walk (`09-synthesis.md` §2.25). §9.1b documents `Async::from_future`'s `[[deprecated]]` migration nudge from L4 v0.2 §7.3 and introduces the `Async::Extension::from_future` audited-boundary spelling. No structural changes to the extension axes table (§2) or the plugin descriptor schema (§7).
- v1.0: post-implementation revision after the first user-defined scheduler (`VkComputeScheduler`) lands in the engine's compute subsystem and exercises every contract in this spec.

---

*End of L7 extension surface spec.*
