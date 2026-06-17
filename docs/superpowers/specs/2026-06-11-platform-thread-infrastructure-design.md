# Platform Thread Infrastructure — Design Spec

**Status:** Draft v1.5 (stdexec-based async fabric; `main` *is* the Platform thread)
**Date:** 2026-06-14
**Author:** Mashiro Engine team
**Scope:** `Mashiro::Platform` namespace; new sources under `Mashiro/include/Mashiro/Platform/` and `Mashiro/src/Platform/`.

### Revision history

- **v1.0** — initial draft.
- **v1.1** — fixes from internal review:
  - Removed `TimingManager` from the topology; high-precision timing exposes free functions in `Mashiro::Platform::Time`, not a Manager. Manager count is **15** throughout.
  - `BatchAwaiter::await_resume()` now returns a non-coroutine `BatchView` input range (no heap allocation per batch).
  - Removed unsound "in-place coalesce already-published slots" claim. Replaced with producer-side pre-publish coalescing for high-rate event kinds.
  - Reworded WndProc reentrancy guarantee: bookkeep handlers *do* run inside `DispatchMessage`; only user-initiated `OwnerTask` bodies are deferred to between pump iterations.
  - Documented `OwnerTask` lifetime contract: caller must keep the task alive until `co_await` completes; destroying a task with a pending continuation is UB.
  - Reworded `OwnerExecutor` pool sizing: heap fallback is a documented expected path under bursty contention, not exceptional.
  - `WindowManager` uses `ChunkedSlotMap<WindowState, WindowId>` for state storage; `SeqLock` array remains fixed at `kMaxWindows`.
  - `EventChannel` documents single-outstanding-waiter precondition with a debug-mode assertion.
  - Documented `OwnerTask` coroutine frame allocation: one heap allocation per call when HALO does not apply. The "no heap on hot paths" goal targets event distribution, not one-shot Manager calls.
  - Spelled out shutdown ordering in `PlatformThread::Run` post-loop.
  - All silent caps (`kMaxChannels`, `kPoolSize`, `kMaxWindows`) log a structured event and assert in debug builds when exceeded.
  - `GetDesc`/`GetSize` precondition explicit: caller must `IsValid(handle)` first; default-constructed return on invalid handle.
  - **New:** `SystemEvent` keeps the union (precise layout) but exposes a reflection-generated type-safe accessor `event.As<EventKind::WindowResize>()`. `consteval` schema check verifies every `EventKind` is bound to exactly one `Payload` member.
- **v1.2** — `EventHeader` removal:
  - Replaced the shared `EventHeader { window, sequence, timestamp, flags }` block with two empty-by-default mixins, `HasWindow` and `HasTimestamp`. Each event payload inherits only the mixins for the fields it actually carries — there is no `windowId == 0` sentinel for app-global events, and slow events (theme / display / power) do not pay for an unused timestamp.
  - Dropped `sequence` (no consumer) and `flags` (no consumer; `Synthetic`/`Coalesced`/`Replayed`/`Lost` were never read). `EventKind` is retained as the persistent stable id (used for keybinding configs and the `BookkeepFor` dispatch table).
  - Cross-cutting queries (`KindOf`, `WindowOf`, `TimestampOf`) are `std::visit` lambdas guarded by reflection-driven concepts (`WindowScoped`, `Timestamped`); non-participating alternatives are pruned at compile time, no runtime null check.
  - Per-event payload structs remain aggregates (no user-defined constructors); `kind` is initialised by an NSDMI that reads the `[[=PayloadFor{...}]]` annotation via reflection, so the discriminator and the binding cannot drift.
- **v1.3** — bookkeep convention + WindowManager lifecycle:
  - Replaced the `[[=BookkeepFor{EventKind::X}]]` annotation + per-Manager dispatch table with a pure naming-convention design: a Manager opts into bookkeeping for payload `P` by declaring `void On(const P&) noexcept`. The convention is lifted into the type system by `Event::Traits::HandlesBookkeep<M, P>`, and `DispatchBookkeep<M>(mgr, event)` is a single `std::visit` whose visitor lambda is `if constexpr`-pruned by that concept. After inlining the visit reduces to the same switch the annotation-driven `template for` produced, with one fewer concept to maintain and zero possibility of drift between parameter type and kind tag.
  - Documented `WindowManager` Create / Destroy internal ordering as new §7.4.1: WindowId allocation → SeqLock prime → `CreateWindowExW` (synchronous re-entry through bookkeep) → HWND patch → caller resume; mirror retirement order on Destroy. Spelled out the concurrency boundaries between any-thread queries (`IsValid`, `GetSize`, `GetDesc`) and the platform-thread writes during create / destroy churn.
  - §8.2 extended to note that the bookkeep-before-broadcast invariant covers `WindowDestroyEvent`: by the time a client wakes on it, the slot is already retired and `IsValid(handle)` returns false from any thread.
- **v1.4** — variant-only event model (no `EventKind`, no `PayloadFor`):
  - Removed the `EventKind` enum, the `[[=PayloadFor{...}]]` annotation, the CRTP `EventPayload<Derived>::kind` NSDMI, the `Traits::KindOf<T>()` reflection helper, the `KindOf(SystemEvent)` accessor, and the consteval `kEventKindCount` completeness check. The payload **type** is now the sole discriminator; `std::visit` / `std::holds_alternative<T>` is the only dispatch surface. §3 already lists event recording / replay as a non-goal, so the persistent-stable-id rationale that justified `EventKind` no longer applies; keybinding configs persist payload *type names* via `Traits::PayloadTypeName<T>()` instead.
  - The marker base is now `Event::Detail::EventPayloadBase` (empty). `WindowSpecificEvent` and `TimestampedEvent` (renamed from `HasWindow` / `HasTimestamp`) inherit it directly so any payload that uses either mixin is automatically a variant alternative; app-global payloads inherit the marker directly.
  - Variant materialisation is structural: `Detail::GetAllEventTypes()` reflects on `Mashiro::Event`, keeps every class that derives from the marker base, then drops anything that is itself the direct base of another candidate. This filters out the abstract bases (`WindowSpecificEvent`, `TimestampedEvent`, `KeyEventBase`, `FileSpecificEvent`) without any `final` decoration or naming convention. Adding a new event is a single struct declaration — no annotation, no enum entry, no extra check.
  - `[[=Platform::OnPlatform{...}]]` annotations remain on platform-specific payloads (`WindowDwmCompositionChangeEvent` → `WindowsOnly`, `WindowExposedEvent` → `LinuxOnly`, `WindowScaleChangeEvent` → `WaylandOnly`, `SelectionUpdateEvent` → `LinuxOnly`, `SessionUserSwitchEvent` → `WindowsOnly`). They tag a *capability*, not an identity, so `Traits::AvailableOn<T, P>` can statically prune backends.
  - Cross-cutting accessor `KindOf(SystemEvent)` is replaced by `NameOf(SystemEvent)` (returns the unqualified type name via reflection — useful for structured logs without committing to a numeric id). `WindowOf` / `TimestampOf` and `DispatchBookkeep` are unchanged.
- **v1.5** — stdexec-based async fabric; `main` *is* the Platform thread:
  - **Role assignment corrected.** Earlier drafts implicitly modelled "the calling thread of `Run()`" as a worker that spawned a separate Platform thread. That is wrong: HWND messages are routed to the *creating* thread's queue (Win32) and AppKit insists `NSApp.run` lives on the OS-blessed first thread (macOS). `PlatformThread::Run()` therefore executes on the thread that calls it — the design's invariant is "the Platform thread is whichever thread enters `Run()`", and the only ergonomic placement of that call is `main`. Client work runs on `std::jthread`s spawned *before* `Run()`. The "MainLoop" abstraction is gone — `Run()` *is* the loop, with no phase enum.
  - **stdexec is the async fabric.** `Mashiro::Platform` is now built directly on P2300 / P3552 / P3941 / P3149 / P2999 vocabulary instead of bespoke primitives:
    - `Mashiro::Platform::scheduler` — the platform-thread scheduler. A copyable, equality-comparable handle; its `schedule()` returns a sender that completes on the platform thread; its environment advertises `get_completion_scheduler<set_value_t>` so adaptors can fold transitions away when the upstream already completes there.
    - `Mashiro::Platform::Task<T>` — typedef for `exec::task<T>` bound to `platform_scheduler` via P3941 scheduler-affinity. The coroutine body always resumes on the platform thread after each `co_await sender`, so `OwnerTask` collapses into a re-export of stdexec's coroutine type with the right initial environment. The bespoke `TransferToOwner` awaiter is retired.
    - `Mashiro::Platform::stop_source` / `stop_token` — typedef for `stdexec::inplace_stop_source` / `inplace_stop_token`. Heap-free, op-state-scoped lifetime; threaded through every Manager call's environment via `get_stop_token`. `RequestStop()` calls `stop_.request_stop()`; in-flight senders surface as `set_stopped()` and unwind structurally — no `closed_` / sentinel proliferation.
    - `Mashiro::Platform::scope` — typedef for `stdexec::counting_scope` (P3149). Owns the lifetime of all spawned senders; shutdown awaits `scope.on_empty()` instead of hand-rolled drain loops.
    - `Mashiro::Platform::domain` — a stdexec domain (P2999/P3826) registered with `platform_scheduler`. `transform_sender` rewrites `continues_on(plat, _)` into a `PostThreadMessage`-based wake on Win32 and `dispatch_async(main_q, _)` on macOS, so OS-specific transport never leaks into client code.
  - **`OwnerExecutor` is recast as scheduler-internal state**, not a public component. The MPSC handle queue and `wakeEvent_` become the implementation of `platform_scheduler::schedule()`'s sender — the queue/wake are still there, but they are reached only through `stdexec::schedule(plat)` / `stdexec::continues_on(s, plat)`, never by name. (See §6.5 for the storage decision: bounded `MpscQueue<coroutine_handle<>, 256>`, no heap fallback; pool path retired.)
  - **`EventChannel` exposes senders alongside its awaitables.** `channel.next_event()` and `channel.next_batch()` are senders that complete with `set_value(SystemEvent)` / `set_value(BatchView)` on a fresh event, or `set_stopped()` on close *or* on stop-token request. Coroutine clients can still write `co_await channel.Next()` because senders are awaitable; non-coroutine clients now compose with `then` / `let_value` / `when_any`.
  - **Cross-thread wake stays direct.** Client coroutines complete a sender by calling `inplace_stop_callback`-style notifications that route directly to the platform thread's `wakeEvent_` (or the Win32 message queue, via `domain`). The earlier "main thread mediates client → platform handoff" idea is retired — there is no third thread between a client coroutine and the platform thread.
  - **Cancellation propagation is no longer hand-rolled.** Stop tokens flow naturally through every sender pipeline: `RequestStop()` ⇒ `stop_.request_stop()` ⇒ all in-flight `Task<T>` op-states observe their stop-callback and complete with `set_stopped` ⇒ `scope.on_empty()` settles ⇒ Manager destructors run on the platform thread.
  - **`Mashiro/Schedular/MainLoop.h` is removed.** It is empty in the working tree and represents an abstraction the design has converged away from. `PlatformThread.h` is the sole loop owner.
  - **Net deletions vs v1.4:** `OwnerTask<T>` (replaced by `Task<T>` typedef), `TransferToOwner` (folded into scheduler-affinity), public `OwnerExecutor` interface (recast as scheduler-internal), `EventChannel::Close()` (replaced by stop-token propagation), `closed_` / sentinel resume paths in §6.3.
- **v1.6** — bring-up corrections from the first end-to-end compile of `EventPump<Managers...>`:
  - **Owner-mailbox storage finalised as `MpscQueue<std::coroutine_handle<>, 256>`.** Earlier
    drafts (through v1.5) carried over the v1.4 Treiber-stack of pre-allocated nodes plus the
    `kPoolSize = 256` heap-fallback path. When the pump-side mailbox concretely landed it
    was re-evaluated off-spec and the pool was found to add cost with no observable benefit:
    bounded-array Vyukov makes ABA structurally impossible without a free-list CAS, the dequeued
    payload is an opaque `coroutine_handle<>` that the consumer resumes once and never re-
    references (so generation guards do not buy use-after-free protection), and overflow is
    handled by `TryPush` returning `false` (which `SubmitResume` escalates to `std::terminate`
    per the design-invariant contract — silently dropping a handle would leak the coroutine).
    The pool stays in the codebase for its real customers (waiter lists, slot-id-keyed
    registries); §6.5 and `MpscQueue.h`'s header doc record the rationale so the next reader
    does not re-litigate it.
  - **Bookkeep convention canonicalised on `m.On(const P&)`.** The earlier `EventPump`-scoped
    `HandlesBookkeep<M, P>` concept that required `m.Bookkeep(p)` was a synonym for the
    `Mashiro::Traits::Event::HandlesBookkeep` concept already published in `SystemEvent.h` —
    it is removed, and the dispatch site in `EventPump::DispatchEvent` is the same
    fold-expanded `if constexpr` that `Mashiro::DispatchBookkeep` uses, lifted across the
    Manager pack.
  - **`Window` Manager landed as the first concrete bookkeeper.** Owns a flat `vector<Entry>`
    registry of `(WindowId, NativeWindowHandle, size, dpiScale)`; `Adopt` mints monotonic
    `WindowId`s on `WM_NCCREATE` (no reuse — banning reuse sidesteps generation-counter
    overhead at the cost of 32 bits per destroyed window); `On(WindowCreate / WindowResize /
    WindowDpiChange / WindowDestroy)` keeps the entry up to date. Reverse lookup (`IdOf(native)`)
    is the path the Win32 translator takes on every native message that carries an HWND.
  - **`PlatformBackendWindows.cpp` landed.** Cross-thread Wake is a manual-reset
    `HANDLE` plus `MsgWaitForMultipleObjectsEx(QS_ALLINPUT, MWMO_INPUTAVAILABLE)` — chosen over
    `PostMessage(WM_NULL)` because the latter requires a hidden message-only window, can hit
    the per-thread 10000-message cap, and marshals through ALPC. Stop-token integration is an
    `inplace_stop_callback` that routes through the same `SetEvent` so the wait has exactly one
    exit path. Rationale is in the file's header comment.
  - **Run-loop adapter is `exec::repeat_until`.** The deprecated `repeat_effect_until` from
    earlier drafts is replaced; the body sender returns `stop_.stop_requested()` on each
    iteration so the surrounding adapter re-runs the wait+drain cycle until stop fires. The
    drain-after-stop guarantee is unchanged — the three drains run on the iteration that
    observes stop before the body returns `true`.

---

## 1. Overview

Mashiro needs an OS abstraction layer that owns Win32 / X11 / Wayland resources with thread affinity (`HWND`, IME, clipboard, OLE DnD, system dialogs, Vulkan WSI surfaces). This spec defines that layer.

The Platform thread is **whichever thread enters `PlatformThread::Run()`**. Win32 message queues are routed to the *creating* thread, AppKit's main run-loop is fixed to the OS-blessed first thread, and X11/Wayland event-pumping libraries assume the same single-thread invariant. The only role assignment that satisfies all three is "the entry thread of the program — `main` — is the Platform thread"; client work (rendering, simulation, networking) lives on `std::jthread`s spawned before `Run()` returns control.

The design has two orthogonal data flows on that one Platform thread:

1. **Event flow (out-bound).** OS messages are translated into a canonical `SystemEvent` schema and broadcast to client threads through SPSC `EventChannel<>`s. Each channel exposes both a coroutine awaiter (`channel.Next()` / `channel.NextBatch()`) and a stdexec sender (`channel.next_event()` / `channel.next_batch()`); the awaiter is a thin shim over the sender so coroutine and non-coroutine clients share one transport.
2. **Call flow (in-bound).** Client coroutines call typed Manager APIs that return `Mashiro::Platform::Task<T>` — a typedef for `exec::task<T>` bound to the platform scheduler via P3941 scheduler-affinity. The coroutine body executes on the Platform thread; the caller resumes on its own thread when the result is ready. The transfer is mediated by `Mashiro::Platform::scheduler` — a stdexec scheduler whose `schedule()` sender completes on the Platform thread.

Async vocabulary throughout this spec is stdexec (P2300, plus P3552 `task<T>`, P3941 scheduler-affinity, P3149 `counting_scope`, P2999/P3826 domains). The Platform layer contributes one scheduler, one domain, one stop-source, and one scope; everything else is composition (`then`, `let_value`, `when_all`, `continues_on`, `start_detached`). Cancellation flows through `stdexec::inplace_stop_token` — no per-channel `closed_` flag, no sentinel events, no hand-rolled drain loops.

Managers are **state owners**, not event consumers. The Platform thread does *not* dispatch events to Managers; it only forwards events to clients and updates Manager bookkeeping in-line during translation.

## 2. Goals

- Single Platform thread is the sole owner of window-affinity OS resources, and that thread is `main`. No "OS thread + main thread" split.
- Async fabric is stdexec: senders, receivers, schedulers, `task<T>`, `counting_scope`, `inplace_stop_source`, domains. No bespoke executor / cancel / scope primitives.
- Client coroutines on render / logic / UI threads can `co_await` events and Manager calls naturally, with cross-thread wake driven by the platform scheduler's domain (no main-thread mediation, no extra hop).
- Zero runtime overhead for compile-time-decidable work: contract verification, route generation, scheduler-affinity transitions are all consteval / `if constexpr` / domain `transform_sender`.
- All thread-affinity rules expressed as types and annotations, verified at compile time via P2996 + P3394 + P3289 + P1306; runtime checks are debug-only assertions.
- Channel and mailbox cardinality is `O(clients) + 2`, not `O(producers × clients)`.
- No technical debt: no command catalog, no virtual-dispatch event router, no per-frame manager tick, no hand-rolled stop tokens.

## 3. Non-Goals

- Cross-process plugin ABI for system calls. (If needed later, derive a command catalog from the Manager API surface using reflection.)
- Recording / replay of system events. The Platform layer does not persist events.
- Audio playback, asset loading, scripting. Those are layers above Platform.
- Windowing on mobile / web targets. The first delivery covers Windows; Linux X11/Wayland follows the same abstraction with platform-specific `src/Platform/Linux/` translation files.

## 4. Constraints

- Toolchain: clang-p2996 (`coca-toolchain-p2996`) with `-freflection-latest`, `-std=gnu++26`. Mandatory features verified by `cmake/ReflectionFeatureProbes.cmake`: P2996 reflection, P3394 annotations, P3289 consteval blocks, P1306 expansion statements, P3491 `define_static_array`.
- Async fabric: stdexec (vendored at `thirdparty/stdexec/`, exposed as CMake target `stdexec`). MSVC translation units that include `<stdexec/execution.hpp>` need `/Zc:preprocessor /Zc:__cplusplus`; this is already set on the relevant TUs and verified by `tests/00-Thirdparty/stdexec_probe.cpp`. Reference papers: P2300 (sender/receiver), P3552 `exec::task<T>`, P3941 scheduler-affinity for coroutine tasks, P3149 `counting_scope`, P2999/P3826 sender domains.
- Existing infrastructure to reuse: `Generator<Ref,V,Alloc>`, `SpscQueue<T,N>`, `SpscByteRing<N>`, `InlineFunction<Sig,Cap>`, `ChunkedSlotMap<T,Id>`, `Result<T>`, `FixedString<N>`, `Traits::SequentialEnum`, `Traits::BitfieldEnum`, `Iota<N>`, `kCacheLineSize`, `SetCurrentThreadName`.
- Existing namespaces and conventions to respect: data-schema annotations live in `Core/Annotation.h`; do not extend that header with thread contracts.
- Cache-line aligned, lock-free where possible. No heap allocation on hot paths (event distribution, scheduler wake). Coroutine frame allocation for `Task<T>` is acceptable for one-shot Manager calls when HALO does not apply.
- All new headers under `Mashiro/include/Mashiro/Platform/`; all sources under `Mashiro/src/Platform/`. Platform-specific code under `src/Platform/Windows/` (this spec) and `src/Platform/Linux/` (deferred).

## 5. Architecture

### 5.1 Topology

```
┌──────────────────────────────────────────────────────────────────────┐
│              Platform Thread (= main; runs PlatformThread::Run)       │
│                                                                      │
│  ┌──────────────┐        ┌────────────────────────┐    SPSC          │
│  │ Win32 Pump   │───────▶│ Unified Event Writer   │──────────▶ Client A
│  │ (PeekMessage)│        │ (sole producer for all │    SPSC          │
│  └──────────────┘        │  EventChannels)        │──────────▶ Client B
│        ▲                 └────────────────────────┘                  │
│        │ wake event             ▲                                    │
│        │                        │ drain                              │
│  ┌──────────────┐        ┌──────────────┐                            │
│  │ MPSC Event   │◀───────│ Dedicated    │                            │
│  │ Inbox        │ submit │ thread mgrs  │                            │
│  └──────────────┘        │ (Gamepad,    │                            │
│        │                 │  FileWatch)  │                            │
│        ▼                 └──────────────┘                            │
│  ┌────────────────────────────────────────────┐                      │
│  │ Managers (state owners on platform thread) │                      │
│  │  Window, Input, Ime, Clipboard, Cursor,    │◀── Task<T> ──────────┤
│  │  DragDrop, Dialog, Surface, Appearance,    │   (cross-thread,     │
│  │  Accessibility                             │    co_await sender)  │
│  └────────────────────────────────────────────┘                      │
│        ▲                                                             │
│  ┌──────────────┐                                                    │
│  │ MPSC handle  │◀────── coroutine handles from any worker thread    │
│  │ queue        │        (= platform_scheduler::schedule() backend)  │
│  └──────────────┘                                                    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │ stdexec fabric: scheduler · domain · stop_source · scope     │    │
│  └──────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────┘

Free-threaded managers (any thread): Display, Power, AudioDevice
Free functions (not a Manager): `Mashiro::Platform::Time::*` for QPC, timer resolution, waitable timers.

Client threads (std::jthread, spawned before Run() takes over main):
   Render, Logic, Networking, …  — own coroutines that co_await senders
   produced by EventChannel and Managers.
```

### 5.2 Cardinality

| Channel / Queue | Count | Type |
|---|---|---|
| `EventChannel<>` | = number of client threads (typically 2–4) | SPSC, Platform thread is sole writer |
| MPSC event inbox | 1 | dedicated-thread managers → Platform thread |
| `platform_scheduler` continuation queue | 1 | any worker thread → Platform thread (coroutine handles, MPSC, recast of the v1.4 `OwnerExecutor` mailbox) |
| `Mashiro::Platform::scope` | 1 | structured ownership of all spawned senders (`exec::counting_scope`) |
| `Mashiro::Platform::stop_source` | 1 | `inplace_stop_source`; token threaded through every Manager call's environment |

Total: `N + 4` data structures where `N` = client threads. Not `producers × clients`. The two extra slots (scope + stop_source) carry no per-message cost — they are queried at suspension points, not at every event.

### 5.3 Platform Thread Loop

`Run()` executes on the calling thread (`main`), turning that thread into the Platform thread for the lifetime of the call. There is no inner thread spawn, no phase enum, and no MainLoop class.

```text
Run() (= main thread):
    set_thread_name("Platform")
    pump.bind(plat_scheduler, stop_source.get_token(), scope)
    loop until stop_source.stop_requested():
        pump.PumpOsMessages()           // PeekMessage + translate + bookkeep + broadcast
        pump.DrainExternalInbox()       // dedicated-thread events → bookkeep + broadcast
        plat_scheduler.DrainHandles()   // resume coroutine handles enqueued by other threads
        if no pending work:
            MsgWaitForMultipleObjects(wakeEvent, INFINITE, QS_ALLINPUT)
    // Shutdown — see §6.12 for the structured ordering driven by `scope.on_empty()`.
```

There are no phases. Order is fixed: pump → drain inbox → drain scheduler queue → wait. Bookkeep handlers run inside `DispatchMessage` (they are part of message translation and must complete before the message is acked). What is *not* permitted to run inside `DispatchMessage` is user-initiated `Task<T>` bodies; those are deferred to the explicit `plat_scheduler.DrainHandles()` step that runs only between pump iterations. The boundary protects against re-entrant Manager mutation while the OS is still inside its own dispatch.

`plat_scheduler.DrainHandles()` is the implementation of the platform scheduler's queued continuations — see §6.4 for the scheduler interface and §6.5 for the queue/wake internals that recast the former `OwnerExecutor`.

## 6. Components

### 6.1 `Mashiro/Platform/ThreadContract.h`

P3394 annotation types that describe the thread requirements of a Manager method.

```cpp
namespace Mashiro::Platform {

    enum class ThreadDomain : uint8_t {
        Platform,  // Must execute on the Platform thread.
        Any,       // Free-threaded; no transfer needed.
    };

    struct ThreadContract {
        ThreadDomain domain = ThreadDomain::Platform;
        constexpr bool operator==(const ThreadContract&) const = default;
    };

    inline constexpr ThreadContract kPlatformOnly{.domain = ThreadDomain::Platform};
    inline constexpr ThreadContract kAnyThread   {.domain = ThreadDomain::Any};

    enum class ScheduleMode : uint8_t {
        PlatformThread,    // Lives on Platform thread; mutators return Task<T>.
        DedicatedThread,   // Owns its own thread; emits via the event inbox.
        FreeThreaded,      // Stateless or atomically-protected; callable anywhere.
    };

    struct ManagerSchedule {
        ScheduleMode mode;
        constexpr bool operator==(const ManagerSchedule&) const = default;
    };

    inline constexpr ManagerSchedule kOnPlatformThread {.mode = ScheduleMode::PlatformThread};
    inline constexpr ManagerSchedule kOnDedicatedThread{.mode = ScheduleMode::DedicatedThread};
    inline constexpr ManagerSchedule kFreeThreaded     {.mode = ScheduleMode::FreeThreaded};

} // namespace Mashiro::Platform
```

Annotations are applied to Manager classes (`[[=kOnPlatformThread]]`) and methods (`[[=kPlatformOnly]]`, `[[=kAnyThread]]`). They are inert at runtime; verification is by `consteval` reflection (§6.7).

### 6.2 `Mashiro/Platform/SystemEvent.h`

The canonical event is a **`std::variant` of strongly-typed event structs**. Each event kind is its own struct that declares **only** the fields it actually carries; `SystemEvent` is the variant of all of them. Consumers dispatch with `std::visit` over an overload set — the type system, not a `switch (kind)`, enforces that every alternative is handled.

**Why `std::variant`, not a tagged union:** the variant *is* the discriminated type, so there is no `kind`-to-payload discipline to keep in sync and no unchecked downcast. `std::visit` over a well-formed overload set is a compile error if any alternative is unhandled, which makes adding a new event a *driven* change (the build breaks until every visitor is updated). The active alternative, copy/move, and lifetime are all handled by the standard library, so variable-length data (IME strings, file paths, clipboard blobs) can be owned **in-place** by the event struct (`std::string`, `std::vector<std::byte>`); an event is self-contained, with no side-channel offset/length to manage.

**Why no shared `EventHeader`.** A common header was tempting but every field failed under independent scrutiny. A discriminator field duplicates the variant's active alternative. `sequence` had no consumer in the design — channels are SPSC and the discriminator already orders events. `flags` (`Synthetic`/`Coalesced`/`Replayed`/`Lost`) had no consumer either. `windowId` is meaningful only for window-scoped events; carrying it on `DisplayChange`/`Power*`/`Gamepad*` invites a `windowId == 0` sentinel pattern, which contradicts the variant-as-discriminator principle. `timestamp` is only meaningful for time-sensitive consumers (input latency, gesture velocity, IME timeouts); slow observational events (theme, occlusion, power) emit at human timescales and don't need ns precision.

**Two empty-by-default mixins replace the header.** Window-scoped payloads inherit `WindowSpecificEvent` (one `WindowId` field). Time-sensitive payloads inherit `TimestampedEvent` (one `uint64_t` ns field). App-global payloads inherit `Detail::EventPayloadBase` directly and pay no per-event cost for fields they don't carry. Both mixins themselves derive from the marker base, so transitively every payload picks up exactly one copy of it. The mixins double as concept tags: `Traits::WindowScoped<T>` and `Traits::Timestamped<T>` are reflection-driven predicates so cross-cutting queries (`WindowOf`, `TimestampOf`) prune non-participating alternatives at compile time — there is no sentinel comparison and no runtime null check.

**No persistent stable id, no annotation.** Earlier drafts kept an `EventKind` enum + `[[=PayloadFor{kind}]]` annotation per payload to (a) survive declaration-order shifts in serialised configs and (b) drive the variant materialisation. Both rationales failed: §3 lists event recording / replay as a non-goal, and the only persistence consumer was keybinding configs, which only need stable identifiers for the input subset (`InputKeyDown`, `InputMouseButton`, …) and can name the *types* directly through reflection (`Traits::PayloadTypeName<T>()`). Removing the enum + annotation eliminates ~80 annotations, the `kind` NSDMI, the consteval completeness check, and the `EventKind <-> payload` drift class entirely. The variant materialiser instead recognises payloads structurally — any class in `Mashiro::Event` that derives from `Detail::EventPayloadBase` and is not itself a base of another candidate becomes a leaf alternative. The "leaf" filter is a reflection check (`bases_of` over the candidate set), so abstract bases like `WindowSpecificEvent` / `KeyEventBase` / `FileSpecificEvent` are excluded automatically without `final` decoration or naming convention.

**Platform-availability annotation stays.** `[[=Platform::OnPlatform{...}]]` is a *capability* tag, not an identity tag — it tells `Traits::AvailableOn<T, P>` that a payload is emitted only on a specific OS / display server, so backends can statically prune unsupported alternatives from translation tables (`WindowDwmCompositionChangeEvent` is Win32-only, `WindowExposedEvent` is Linux-only, `WindowScaleChangeEvent` is Wayland-only, `SelectionUpdateEvent` is Linux-only, `SessionUserSwitchEvent` is Win32-only). The annotation is recovered by reflection through `Mashiro::Traits::Anno::Get<Platform::OnPlatform>(^^T)`; portable payloads carry no annotation and resolve to `PlatformBit_All`.

> The previous **fixed-size POD/union transport** design (`sizeof(SystemEvent) == 64`, `union Payload`, reflection-generated `As<K>()`, side `SpscByteRing` for variable-length data) is **archived** in §6.2.1. It is retained only as rationale for the layout/throughput trade-offs; it is **not** the active design. The intermediate v1.1 / v1.2 / v1.3 shapes (variant alternatives sharing `EventHeader`; mixin-based with `EventKind` + `PayloadFor`) are superseded by the variant-only model above.

```cpp
namespace Mashiro {

    enum class WindowId : uint32_t { Invalid = 0 };

    inline namespace Event {

        namespace Detail {
            // Empty marker — the sole condition for SystemEvent membership.
            struct EventPayloadBase {};
        }

        // Cross-cutting mixins. Each derives from the marker so payloads that
        // inherit any of them transitively pick it up.
        struct WindowSpecificEvent : Detail::EventPayloadBase {
            WindowId windowId = WindowId::Invalid;
        };
        struct TimestampedEvent : Detail::EventPayloadBase {
            uint64_t timestamp = 0;          // monotonic ns
        };

        // One struct per concrete event — declares only what it carries.
        // No annotation, no enum tag, no NSDMI machinery — the type *is*
        // the discriminator on SystemEvent.
        struct WindowResizeEvent : WindowSpecificEvent, TimestampedEvent {
            ivec2 size{};
            bool  isMinimised = false;
        };
        struct WindowCloseEvent  : WindowSpecificEvent {};
        struct KeyDownEvent      : KeyEventBase {};            // mixins via base
        struct DisplayChangeEvent : Detail::EventPayloadBase { // app-global
            DisplayId display = DisplayId::Invalid;
            uvec2     resolution{};
            float     refreshHz = 0;
            float     dpiScale  = 1;
        };
        // Platform-specific payloads carry the capability annotation.
        struct [[=Platform::WindowsOnly]]
        WindowDwmCompositionChangeEvent : WindowSpecificEvent {
            bool compositionEnabled = false;
        };
        struct [[=Platform::LinuxOnly]]
        WindowExposedEvent : WindowSpecificEvent {
            ivec2 origin{};
            ivec2 extent{};
        };
        // ... one struct per concrete event

        namespace Traits {
            template<typename T> concept SystemEventPayload =
                std::is_class_v<T> && std::is_base_of_v<Detail::EventPayloadBase, T>;
            template<typename T> concept WindowScoped =
                std::is_base_of_v<WindowSpecificEvent, T>;
            template<typename T> concept Timestamped =
                std::is_base_of_v<TimestampedEvent, T>;

            template<SystemEventPayload T> consteval PlatformBit       PlatformsOf();
            template<SystemEventPayload T, PlatformBit P>
            inline constexpr bool                                       AvailableOn;
            template<SystemEventPayload T> consteval std::string_view  PayloadTypeName();
            template<typename M, typename P> concept                    HandlesBookkeep;
        }

    } // namespace Event

    // Materialised by reflection: every class in `Mashiro::Event` that derives
    // from `Detail::EventPayloadBase` AND is not the direct base of any other
    // candidate becomes a variant alternative. Abstract mixin/composition bases
    // (WindowSpecificEvent, KeyEventBase, FileSpecificEvent) are filtered out
    // by the leaf rule — no annotation, no `final` decoration required.
    using SystemEvent = [: std::meta::substitute(
        ^^std::variant, Detail::GetAllEventTypes()) :];

    // Cross-cutting accessors. Concept-guarded; non-participating alternatives
    // are pruned at compile time — no sentinel, no runtime null check.
    [[nodiscard]] std::string_view           NameOf      (const SystemEvent&) noexcept;
    [[nodiscard]] std::optional<WindowId>    WindowOf    (const SystemEvent&) noexcept;
    [[nodiscard]] std::optional<uint64_t>    TimestampOf (const SystemEvent&) noexcept;

    // Convention-based bookkeep dispatch (§6.7).
    template<typename M> void DispatchBookkeep(M& mgr, const SystemEvent& e) noexcept;

} // namespace Mashiro
```

`std::variant::index()` is intentionally **not** persistence-stable: inserting a new payload re-orders every subsequent index. Clients dispatch on the payload *type* (`std::visit`, `std::holds_alternative<T>`, the `Traits::PayloadTypeName<T>()` reflection); the variant index never escapes the in-memory pipeline. Keybinding configs that need a stable identifier name the input payload types (`"KeyDownEvent"`, `"MouseButtonEvent"`) directly through `PayloadTypeName` — type names are the persistent surface, not numeric ids.

Variable-length data (IME composition strings, file paths, clipboard blobs) is owned directly by the corresponding event struct (`std::string`, `std::vector<std::byte>`), so each event is self-contained. There is no side `SpscByteRing`: the SPSC queue stores moved `SystemEvent` values, and `EventChannel` (§6.3) moves events through the ring rather than `memcpy`-ing fixed-size POD.

Plain payloads (no owning containers) remain `std::is_trivially_copyable_v` — `WindowResizeEvent`, `KeyDownEvent`, `MouseButtonEvent`, `ScrollEvent`, `TouchEvent`, `PenEvent`, `TimerTickEvent`, `GamepadStateEvent`, etc. — so the trivial-copy hot path the v1.0 union design optimised for is preserved for the kinds that actually flood the queue.

#### 6.2.1 Archived: fixed-size POD/union transport (superseded)

> Retained for rationale only. **Not** the active design — superseded by the `std::variant` model in §6.2.

The original transport was a trivially-copyable, fixed-size canonical event sized to one cache line so emission was a single `memcpy`. The union form gave precise control over alignment and size (`static_assert(sizeof(SystemEvent) == 64)`) and a stable layout across stdlib versions; a `std::variant` of 30+ alternatives could not share an outer header and its discriminator + alignment depended on libc++ implementation details. Type-safe access was recovered with a P2996-reflection-generated accessor (`event.As<EventKind::WindowResize>()`) plus a `consteval` schema check that every `EventKind` was bound to exactly one `Payload` member. Variable-length data was stored in a side `SpscByteRing` per channel; the event carried an offset + length into that ring.

```cpp
// ARCHIVED — superseded by the std::variant model in §6.2.
namespace Mashiro::Platform {

    // Annotation binding an EventKind enumerator to a Payload member.
    struct PayloadFor { EventKind kind; constexpr bool operator==(const PayloadFor&) const = default; };

    struct SystemEvent {
        EventKind kind;
        uint16_t  flags;
        uint32_t  sequence;
        uint64_t  timestamp;

        union Payload {
            [[=PayloadFor{EventKind::WindowResize}]]    WindowResizePayload   resize;
            [[=PayloadFor{EventKind::InputKeyDown}]]    KeyPayload            key;
            [[=PayloadFor{EventKind::InputKeyUp}]]      KeyPayload            keyUp;
            // ... one annotated branch per EventKind
            alignas(8) uint8_t raw[48];
        } payload;

        // Reflection-generated type-safe accessor.
        // Compile error if K is unbound or the branch type doesn't match.
        // Debug-mode runtime assert: this->kind == K (or a kind that aliases the same payload).
        template<EventKind K>
        [[nodiscard]] auto&       As()       noexcept;
        template<EventKind K>
        [[nodiscard]] auto const& As() const noexcept;
    };

    static_assert(std::is_trivially_copyable_v<SystemEvent>);
    static_assert(sizeof(SystemEvent) == 64);

    consteval { Detail::VerifyEventSchema(); }

} // namespace Mashiro::Platform
```

### 6.3 `Mashiro/Platform/EventChannel.h`

SPSC channel from the Platform thread to one client thread. The Platform thread is the sole producer for every channel. Consumption is **sender-first**, with awaitable shims so coroutines read naturally:

```cpp
namespace Mashiro::Platform {

    template<uint32_t Capacity = 4096>
    class EventChannel {
    public:
        // Producer (Platform thread only). SystemEvent is a (possibly non-trivial)
        // variant, so events are *moved* into the ring rather than memcpy'd.
        bool     Emit(SystemEvent&& event) noexcept;
        uint32_t EmitBatch(std::span<SystemEvent>) noexcept;

        // Consumer surface — senders. Each is a stdexec sender that completes with:
        //   - set_value(SystemEvent)   on a fresh event,
        //   - set_value(BatchView)     for the batch sender, on a non-empty drain,
        //   - set_stopped()            when the platform stop_token observes a stop
        //                              request, OR the channel is detached on
        //                              shutdown.
        // No set_error: there is no error path on the consumer side.
        [[nodiscard]] auto next_event() noexcept -> next_event_sender;
        [[nodiscard]] auto next_batch() noexcept -> next_batch_sender;

        // Coroutine ergonomics — both senders are awaitable, but the legacy short
        // form is kept as a thin alias.
        auto Next()      noexcept { return next_event(); }
        auto NextBatch() noexcept { return next_batch(); }

        // Polling fast-path; never suspends.
        std::optional<SystemEvent> TryReceive() noexcept;

        // Producer / shutdown observers.
        uint32_t PendingCount() const noexcept;
        uint64_t DropCount()    const noexcept;   // diagnostic: events dropped on overflow

    private:
        SpscQueue<SystemEvent, Capacity> ring_;          // stores moved variant values
        std::atomic<std::coroutine_handle<>> waiter_{nullptr};
        std::atomic<uint64_t> dropCount_{0};
        // Producer-side coalescing memory (only touched by platform thread).
        // lastIndex_ is the routing key of the last push, taken from event.index().
        // The variant index is process-local (not persisted); coalescing only spans
        // the lifetime of one producer, so this is sufficient and avoids a runtime
        // type-name comparison on the hot path.
        std::size_t lastIndex_  = std::variant_npos;
        WindowId    lastWindow_ = WindowId::Invalid;
        uint32_t    lastSlot_   = ~0u;

#ifndef NDEBUG
        std::atomic<bool> awaiting_{false};                // single-waiter assertion
#endif
    };

} // namespace Mashiro::Platform
```

**Precondition (per-channel):** at most one outstanding `next_event()` / `next_batch()` op-state on a given channel. SPSC's contract is single *thread*, not single *coroutine*; two coroutines on the same client thread that both `start()` a sender on the same channel would race on `waiter_`. Debug builds enforce this with the `awaiting_` flag set on `start` and cleared on completion; release builds rely on caller discipline. The intended pattern is one consumer coroutine per channel; clients that want multiple readers attach multiple channels.

**Why senders, not just awaiters.** Senders compose. `when_any(channel.next_event(), timer.after(50ms))` gives a per-frame cap on event-await blocking with no extra type. `let_value(channel.next_event(), [&](auto& e) { … })` is a non-coroutine handler. The awaiter is preserved for the common `co_await` case, but it is now derived from the sender, not a separate code path.

**Stop-token integration replaces `Close()`.** A channel does not own its lifetime; `PlatformThread` does. On shutdown, the platform stop-source is requested; every `next_event_sender` op-state has registered a `stop_callback` against the receiver's stop token (which is the platform stop token, threaded through the receiver's environment). The callback exchanges the waiter handle out, completes the receiver with `set_stopped()`, and unwinds. There is no `closed_` flag, no per-channel sentinel, no `IsClosed()` polling. A client coroutine that wants to know "is the platform shutting down?" queries `stdexec::get_stop_token(get_env(*this))` — the same token surfaces through every layer.

**Wake protocol (lost-wake-free):**

1. `start()` checks `!ring_.Empty()`. If non-empty, complete with `set_value(ring_.Pop())` synchronously — no suspension.
2. If empty, register `stop_callback` against the receiver's stop token, then `waiter_.store(receiver_resume_handle)` (release).
3. After the store, re-check `!ring_.Empty()`. If true, attempt to reclaim the handle via `compare_exchange_strong(h, nullptr)`. Success → drop the callback, complete with `set_value`. Failure → producer already took the handle and will resume — stay suspended.
4. Producer's `Emit()` does `TryPush()`, then `waiter_.exchange(nullptr)` → resume handle if non-null. The handle resume runs the receiver's continuation; if the receiver belongs to a `Task<T>` bound to a different scheduler, P3552's transition machinery forwards the value to that scheduler.

**`next_batch_sender` completes with `BatchView`** — a lightweight non-coroutine input range that pops events from `ring_` lazily on iteration. No coroutine frame allocation. Iteration ends when the ring is observed empty *or* a configurable batch cap is hit. `BatchView` is move-only and tied to the channel's lifetime; it must be fully consumed (or destroyed) before the next sender on the channel is started.

**Producer-side coalescing for high-rate payloads.** For `MouseMoveEvent` and similar payloads where only the latest sample matters, the producer (`EventPump`) checks before push: if the *unpublished* tail slot would coincide with the previous push of the same alternative (`event.index() == lastIndex_`) for the same window AND the consumer has not yet advanced past `lastSlot_`, the previous slot is move-assigned the new event before re-publishing the same `tail_`. This is sound because (a) only the producer touches unpublished slots, and (b) the consumer's view of `tail_` cannot regress. If the consumer has already advanced past `lastSlot_`, coalescing is skipped and a new event is pushed normally. Coalescing is opt-in per payload type via a constexpr predicate keyed on the variant alternative.

**Overflow.** When `TryPush` fails (ring full), `Emit` increments `dropCount_` and returns `false`; the event is lost. `EventPump` logs structured drops at info level. Clients can query `PendingCount()` and `DropCount()` for back-pressure diagnostics.

### 6.4 `Mashiro/Platform/Scheduler.h` — `Mashiro::Platform::scheduler`

The platform scheduler is the public face of the cross-thread call flow. It is a copyable, equality-comparable handle (model of `stdexec::scheduler`) whose `schedule()` returns a sender that completes on the Platform thread.

```cpp
namespace Mashiro::Platform {

    class PlatformThread;            // forward decl; defined in §6.12.

    class scheduler {
    public:
        using __id   = scheduler;
        using __t    = scheduler;

        scheduler() = default;       // an empty scheduler equates to "no platform thread"
        explicit scheduler(PlatformThread& owner) noexcept : owner_{&owner} {}

        bool operator==(const scheduler&) const noexcept = default;

        // The schedule sender. Awaiting it on a non-platform thread enqueues the
        // continuation onto the platform thread's MPSC handle queue and signals
        // wakeEvent_. Awaiting it from the platform thread completes synchronously
        // (await_ready == true) — zero suspension, zero kernel call.
        struct sender;
        [[nodiscard]] sender schedule() const noexcept;

        // Environment query — lets adaptors fold transitions away when the upstream
        // already completes here. `continues_on(plat, just(x))` becomes `just(x)`
        // after `transform_sender` because the inner sender already advertises
        // `get_completion_scheduler<set_value_t>(env) == plat`.
        struct env {
            PlatformThread* owner;
            template<class Tag>
            friend scheduler tag_invoke(stdexec::get_completion_scheduler_t<Tag>,
                                        env e) noexcept { return scheduler{*e.owner}; }
        };

        // Domain hook — see §6.8. The domain rewrites cross-thread `continues_on`
        // into a wake that uses Win32 PostThreadMessage / macOS dispatch_async, so
        // OS-specific transport never leaks into client code.
        friend domain tag_invoke(stdexec::get_domain_t, scheduler) noexcept { return {}; }

    private:
        PlatformThread* owner_ = nullptr;
    };

    // Free function for callers that prefer the verb form.
    [[nodiscard]] inline auto schedule_on(scheduler s) noexcept { return s.schedule(); }

} // namespace Mashiro::Platform
```

The scheduler is the **only** way client code names the Platform thread. There is no `IsOnPlatformThread()` test exposed publicly — code that needs to know "am I here?" composes a sender and lets the scheduler answer at the appropriate suspension point. The `await_ready` fast path covers the common case (already on the platform thread → no suspension).

`scheduler::sender` is the implementation surface that owns the cross-thread queue/wake; see §6.5.

### 6.5 `Mashiro/Platform/Scheduler.cpp` — schedule sender internals (recasts the v1.4 `OwnerExecutor`)

The schedule sender's `start` enqueues the receiver's stop-aware completion handle onto a bounded
`Mashiro::MpscQueue<std::coroutine_handle<>, 256>` (the project's Vyukov-style MPSC ring); the
platform thread drains the queue between pump iterations. `wakeEvent_` is preserved from the v1.4
`OwnerExecutor` and signalled through `MpscQueue`'s `TryPush` returning `true`. Internals are
reached exclusively through `stdexec::schedule(plat)` / `stdexec::continues_on(s, plat)` /
awaiting `Task<T>`; there is no `OwnerExecutor::Instance()` or `OwnerExecutor::Enqueue` to call by
name.

**Storage choice — re-confirmed off-spec.** The v1.4 spec proposed a Treiber-stack of pre-allocated
nodes (drawn from `ConcurrentObjectPool<Node, 256>`) with a heap fallback for bursts. That choice
was re-evaluated when the pump-side mailbox concretely landed, and the pool was found to add cost
with no observable benefit:

- The pool's first guarantee, **ABA safety on a free list**, is moot — `MpscQueue` is bounded-array
  Vyukov, whose per-cell `seq` handshake makes ABA structurally impossible without ever using a
  pointer-CAS free list. A pool-backed implementation would add a hot-path free-list CAS for no
  correctness benefit.
- The pool's second guarantee, **generation-guarded slot identity that survives past dequeue**,
  decomposes for this workload too: the dequeued payload is a `std::coroutine_handle<>`, an opaque
  pointer the consumer resumes exactly once and never re-references; there is no node identity to
  preserve, no slot to keep alive, no use-after-free window to guard.
- The pool's third feature, **a heap fallback past `kPoolSize`**, is replaced by `TryPush` returning
  `false` on a full ring — i.e. observable back-pressure rather than silent allocation. Cross-thread
  awaiters are required to honour the back-pressure (terminate or retry), which is consistent with
  the same contract the external inbox already exposes (spec §6.8).

The bounded `MpscQueue` is therefore the *cheaper* primitive at every axis: no free-list CAS, no
per-slot generation counter, no fallback path, no allocation past initialisation, deterministic
256-slot capacity. The pool stays in the codebase for its real customers (waiter lists, slot-id-
keyed registries) and the rationale for not reusing it here is captured in `MpscQueue.h`'s header
doc so the next reader does not re-litigate it.

```cpp
struct scheduler::sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_stopped_t()>;

    PlatformThread* owner;

    template<class Rcvr> struct op_state {
        Rcvr   rcvr;
        std::optional<stdexec::inplace_stop_callback<StopFn>> cb;

        void start() noexcept {
            if (owner_->IsOnPlatformThread()) {
                stdexec::set_value(std::move(rcvr));
                return;
            }
            cb.emplace(stdexec::get_stop_token(stdexec::get_env(rcvr)), StopFn{this});
            // SubmitResume pushes our coroutine_handle<> onto MpscQueue<…, 256>
            // and signals wakeEvent_; back-pressure is observable as TryPush=false.
            owner_->SubmitResume(stdexec::__coro::__continuation_handle(this));
        }
        // resume_on_platform() is invoked from PlatformThread::DrainHandles().
        void resume_on_platform() noexcept {
            cb.reset();
            stdexec::set_value(std::move(rcvr));
        }
        // StopFn runs on whatever thread requested the stop; cancellation is
        // best-effort once the handle is in the mailbox — losing the race
        // means the platform thread will resume the coroutine, which is then
        // expected to read the stop token at its next suspension point.
    };

    template<class Rcvr>
    op_state<Rcvr> connect(Rcvr rcvr) const { return {std::move(rcvr), std::nullopt}; }

    env get_env() const noexcept { return {owner}; }
};
```

The bounded ring's 256-slot capacity covers steady-state bursts (tens of in-flight calls per
worker thread). `TryPush` returning `false` on a full ring is the documented back-pressure signal;
cross-thread awaiters that sized for the worst case observe the failure and `std::terminate` per
the design-invariant contract on `EventPump::SubmitResume`. There is no heap fallback — if profiling
shows the cap is hit, raise the constant; do not paper over it with a fallback path. `wakeEvent_` is
signalled inside `SubmitResume` after the push succeeds; coalescing under `MsgWaitForMultipleObjectsEx`
collapses redundant signals at the kernel boundary, so producers do not need to rate-limit.

### 6.6 `Mashiro/Platform/Task.h` — `Mashiro::Platform::Task<T>`

`Task<T>` is the coroutine return type for any work that must run on the platform thread. It is a thin alias around `exec::task<T>` (P3552) bound to `scheduler` via P3941 scheduler-affinity:

```cpp
namespace Mashiro::Platform {

    template<class T = void>
    using Task = exec::task<T, exec::default_task_context<scheduler>>;
    //                                              ^^^^^^^^^^^^^^
    //   P3941 scheduler-affinity — every co_await sender re-schedules
    //   onto `scheduler` after the awaited sender completes, so the
    //   coroutine body always runs on the platform thread regardless
    //   of which scheduler the awaited sender completes on.
} // namespace Mashiro::Platform
```

What scheduler-affinity buys, semantically:

- **Initial suspend ≡ schedule on platform thread.** When a Manager method that returns `Task<T>` is called from a worker thread, the *first* resume happens on the platform thread automatically — the same effect the v1.4 `TransferToOwner` initial-suspend awaiter used to provide, but now derived from `task<T>`'s contract instead of bolted on. If the caller is already on the platform thread, the initial schedule sender's `await_ready()` returns true and the body runs synchronously with zero suspension.
- **Every `co_await` returns to the platform thread.** A Manager method that does `co_await platform.Surfaces().AttachVulkan(*window, inst);` does *not* leak the surface scheduler back into the surrounding body — `task<T>` re-schedules onto `scheduler` after the inner sender completes. This eliminates the v1.4 hand-rolled "remember to switch back" idiom.
- **Final suspend ≡ resume caller on caller's scheduler.** When the body finishes, `task<T>` resumes the awaiting receiver using *its* environment's scheduler — for a `co_await Manager().Method(...)` call from a worker coroutine running under a separate scheduler, the worker is resumed on its own scheduler, not on the platform thread.

`Task<T>` is awaitable (it satisfies the `awaitable` concept by inheriting from `task<T>`), so client coroutines write `co_await platform.Windows().Create(...)` exactly as before. It is also a sender — non-coroutine clients can `stdexec::start_detached(platform.Windows().Create(...))` or compose with `when_all` / `let_value`.

**Lifetime.** Same rule as v1.4 `OwnerTask`: the caller must keep the `Task<T>` alive until `co_await` (or `start_detached`) completes. Destroying a task whose body has been scheduled but not yet completed is undefined behaviour. The natural usage `co_await Manager().Method(...)` as a temporary remains safe because the temporary persists across the suspension. For "fire and forget" call sites that cannot keep the task alive, route through `Mashiro::Platform::scope` (§6.7) — its `spawn(token, sender)` adopts ownership.

**Coroutine frame allocation.** `task<T>` carries a coroutine frame; the compiler can elide the allocation only when HALO proves the frame does not escape. Cross-thread transfer always escapes, so each cross-thread call allocates one frame on the heap. Same rationale as v1.4: the "no heap on hot paths" goal targets event distribution, not one-shot Manager calls. If profiling shows Manager-call frame allocation is hot, slot a custom allocator into `task<T>`'s `default_task_context` — the P3552 envelope already exposes the allocator hook through `get_allocator(env)`.

### 6.7 `Mashiro/Platform/Scope.h` — `Mashiro::Platform::scope`

`scope` owns the lifetime of every sender that is started without an explicit `co_await` in client code — Manager-internal background work, dedicated-thread shutdown handshakes, and any "fire and forget" call site that cannot keep a `Task<T>` alive. It is a thin alias around `exec::counting_scope` (P3149):

```cpp
namespace Mashiro::Platform {

    using scope = exec::counting_scope;

    // Convenience: spawn and forget, with structured ownership.
    template<stdexec::sender S>
    void Spawn(scope& sc, stop_token stop, S&& s) noexcept {
        sc.spawn(stop, std::forward<S>(s));
    }

    // Convenience: spawn-with-future for senders whose result the caller wants.
    template<stdexec::sender S>
    [[nodiscard]] auto SpawnFuture(scope& sc, stop_token stop, S&& s) noexcept {
        return sc.spawn_future(stop, std::forward<S>(s));
    }

    // Settles when every sender owned by the scope has completed (set_value /
    // set_error / set_stopped). Used in shutdown — see §6.12.
    [[nodiscard]] auto Joined(scope& sc) noexcept { return sc.on_empty(); }

} // namespace Mashiro::Platform
```

Why `counting_scope` and not a hand-rolled "list of in-flight handles":

- **Structured.** `on_empty()` is a sender that completes when the scope's count drops to zero. Shutdown becomes `co_await scope.on_empty()` — a single suspension that resumes when every spawned sender has settled. No manual `while (count_ > 0)` polling, no missed wake-ups, no dangling continuations.
- **Cancellation flows in.** `spawn(stop_source.get_token(), s)` registers the token with the spawned sender's environment; `request_stop()` propagates to every alive child. Combined with the platform `stop_source` (§6.8), one `RequestStop()` call cancels every spawned background sender at once.
- **Allocation policy is configurable.** P3149 leaves the spawn allocator a customisation point; we slot the project's existing pool allocator into the scope so spawned coroutine frames come from the same arena as the rest of the platform thread's work.

Public API surface that takes a scope: `PlatformThread::scope()` returns the platform-thread-owned scope; Managers that need to spawn background work (e.g., `FileWatchManager`'s IOCP completion fan-out) take `scope&` via constructor and use `Spawn` / `SpawnFuture` instead of holding `std::jthread`s directly. The two dedicated-thread Managers (`GamepadManager`, `FileWatchManager`) keep their `std::jthread` because their bodies are blocking syscalls outside stdexec's reach; their *event-emit* paths route through `pump_.SubmitExternal`, which is already in scope.

### 6.8 `Mashiro/Platform/Stop.h` and `Mashiro/Platform/Domain.h` — cancellation + domain rewrites

#### `Mashiro::Platform::stop_source` / `stop_token`

```cpp
namespace Mashiro::Platform {

    using stop_source   = stdexec::inplace_stop_source;
    using stop_token    = stdexec::inplace_stop_token;

    template<class Fn>
    using stop_callback = stdexec::inplace_stop_callback<Fn>;

} // namespace Mashiro::Platform
```

`inplace_stop_source` is preferred over `std::stop_source` for two reasons:

- **No heap allocation.** Storage is inline in the source; tokens are pointer-sized and refer to the source's address. The platform `stop_source` lives as a member of `PlatformThread`, so its lifetime is bounded by the loop and tokens never outlive it.
- **Designed for op-state-scoped lifetime.** Receivers expose stop tokens through `get_stop_token(get_env(rcvr))`; sender op-states register `stop_callback`s on `start` and unregister on completion. The whole pattern is heap-free and observable — every `Task<T>`, every `EventChannel` await, every spawned sender gets the platform stop token through its receiver's environment.

`PlatformThread::Run()` constructs the source on entry, exposes its token through `PlatformThread::stop_token() noexcept` and `PlatformThread::scope()` (which adopts it), and calls `request_stop()` on shutdown (§6.12). Manager methods that internally spawn senders pass `stop_token()` into their `Spawn` calls; that one call point is the boundary between the platform's stop-source and any client environment.

#### `Mashiro::Platform::domain`

A stdexec domain (P2999/P3826) registered with `scheduler` so adaptors can be customised per-platform without leaking OS-specific transport into client code:

```cpp
namespace Mashiro::Platform {

    struct domain {
        // Default rule: forward to the algorithm's default lowering.
        template<stdexec::sender_expr S, class Env>
        decltype(auto) transform_sender(S&& s, const Env& env) const {
            return stdexec::default_domain{}.transform_sender(std::forward<S>(s), env);
        }

        // Specialise `continues_on(plat, _)` so the wake uses the OS-appropriate
        // path: Win32 PostThreadMessage when the calling thread is *not* the
        // platform thread (avoids the wakeEvent + MsgWaitForMultipleObjects
        // round-trip when we already have the message queue handy); macOS
        // dispatch_async to the main queue.
        template<stdexec::sender Inner, class Env>
            requires stdexec::__is_continues_on<Inner>
        auto transform_sender(Inner&& s, const Env& env) const {
            return Detail::RewriteContinuesOnPlatform(std::forward<Inner>(s), env);
        }
    };

    // Free hook: schedulers expose their domain via tag_invoke(get_domain_t, …).
    inline domain get_domain(scheduler) noexcept { return {}; }

} // namespace Mashiro::Platform
```

What the domain achieves:

- **OS-specific transport is local to one rewrite point.** `continues_on(plat, sender)` is the canonical "transition back to the platform thread" expression; without a domain, every backend would need to teach the algorithm how to wake the platform thread. With the domain, every `continues_on` to the platform scheduler funnels through `Detail::RewriteContinuesOnPlatform`, which knows about `PostThreadMessage` (Win32) and `dispatch_async(main_q, _)` (macOS). Client code never names either API.
- **Compile-time customisation, zero runtime overhead.** `transform_sender` is consteval at the algorithm level; the rewrite happens at sender-pipeline construction, not at execution.
- **Open for extension.** A future `RtPlatform` thread (real-time audio, e.g.) declares its own `rt_domain` and registers different rewrites. Client code does not change.

The default lowering is the same MPSC + `wakeEvent_` mechanism as the schedule sender (§6.5); the domain only takes over when the OS exposes a more efficient path for the specific algorithm being lowered.

### 6.9 `Mashiro/Platform/SeqLock.h`

Single-writer, multi-reader lock-free reader for composite values. Used for any-thread queries on Manager state (e.g., `WindowManager::GetDesc`).

```cpp
namespace Mashiro::Platform {

    template<typename T> requires std::is_trivially_copyable_v<T>
    class SeqLock {
    public:
        void Write(const T& v) noexcept;          // Platform thread only
        [[nodiscard]] T Read() const noexcept;    // any thread, retry on torn read
    private:
        alignas(Platform::kCacheLineSize) std::atomic<uint32_t> seq_{0};
        T data_{};
    };

} // namespace Mashiro::Platform
```

Writers bump `seq_` to odd before mutation, even after; readers retry while seq is odd or differs across the read.

### 6.10 `Mashiro/Platform/ManagerTraits.h`

Compile-time verification of Manager API contracts via P2996 reflection on P3394 annotations.

```cpp
namespace Mashiro::Platform::Detail {

    consteval bool IsTaskSpecialization(std::meta::info type);
    // True iff `type` is a specialisation of `Mashiro::Platform::Task<T>`
    // (which itself is `exec::task<T, exec::default_task_context<scheduler>>`).
    // The check is structural — it walks the alias chain, so direct
    // `exec::task<...>` returns from a Manager are also accepted.

    template<typename Manager>
    consteval void VerifyManagerContracts();
    // For each public function member m of Manager:
    //   - if annotations_of(m, ^^ThreadContract) yields kPlatformOnly:
    //       static_assert IsTaskSpecialization(return_type_of(m))
    //   - if it yields kAnyThread:
    //       static_assert NOT IsTaskSpecialization(return_type_of(m))

    template<typename... Managers>
    consteval void VerifySchedulingContracts();
    // For each Manager M in Managers:
    //   - require exactly one ManagerSchedule annotation on ^^M
    //   - if PlatformThread: every Platform-domain method returns Task<T>
    //   - if FreeThreaded:   no method returns Task<T>
    //   - if DedicatedThread: no public method returns Task<T>

} // namespace Mashiro::Platform::Detail
```

#### Bookkeep dispatch — convention, not annotation

`EventPump` updates Manager state **in line with translation, before broadcast**. Earlier drafts modelled this with a `[[=BookkeepFor{EventKind::X}]]` annotation per handler plus a per-Manager dispatch table. The annotation added zero information that the parameter type didn't already carry: a method that takes `const WindowResizeEvent&` is, by construction, a handler for `WindowResize`. Carrying the kind on a separate annotation invites drift between the parameter type and the kind tag — a class of bug the type system can rule out for free.

The current design drops the annotation and the table. A Manager opts into bookkeeping for payload `P` purely by declaring a member function

```cpp
void On(const P& payload) noexcept;   // any access — friend EventPump if private
```

where `P` is any `SystemEventPayload` (i.e., any class derived from `Detail::EventPayloadBase`). The convention *is* the protocol. `Mashiro::Event::Traits::HandlesBookkeep<M, P>` lifts that convention into the type system:

```cpp
template<typename M, typename P>
concept HandlesBookkeep =
    SystemEventPayload<P> &&
    requires(M& m, const P& p) { m.On(p); };
```

Dispatch lives next to the other cross-cutting accessors over `SystemEvent` (`NameOf`, `WindowOf`, `TimestampOf`). It is a single `std::visit` over the active alternative whose visitor lambda is `if constexpr`-guarded by `HandlesBookkeep<M, P>`:

```cpp
template<typename M>
inline void DispatchBookkeep(M& mgr, const SystemEvent& e) noexcept {
    std::visit(
        [&mgr](const auto& payload) noexcept {
            using P = std::remove_cvref_t<decltype(payload)>;
            if constexpr (Event::Traits::HandlesBookkeep<M, P>) {
                mgr.On(payload);
            }
        },
        e);
}
```

What this gives, mechanically:

- **No table.** The compiler instantiates one arm of the visitor per variant alternative; the matched arm becomes a direct member call, the unmatched arms collapse to empty bodies. After inlining the visit reduces to a switch-on-`index()` whose dead arms are eliminated — the same code the annotation-driven `template for` produced, with one fewer concept to maintain.
- **No drift.** Renaming the payload type renames the handler's parameter; if the handler signature stops naming a `SystemEventPayload`, `HandlesBookkeep` quietly evaluates to `false` at the call site rather than silently calling the wrong overload.
- **Zero cost for non-participants.** A Manager that bookkeeps only `WindowResizeEvent` and `WindowFocusEvent` pays nothing for the keyboard / IME / display arms — those instantiations resolve to the empty `else`.
- **No friend gymnastics.** Bookkeep handlers can stay private; `DispatchBookkeep` is templated on `M`, so a `friend class EventPump;` (or a single `friend void DispatchBookkeep<>(...)` declaration) is sufficient to admit it.

`EventPump` calls `DispatchBookkeep<M>(mgr, event)` once per Platform-thread Manager between translate / timestamp and `Broadcast`. The bookkeep-before-broadcast invariant is preserved by ordering, not by the annotation set: when a client coroutine wakes on `co_await channel.Next()`, every Manager's any-thread query already reflects the post-event state.

### 6.11 `Mashiro/Platform/EventPump.h`

OS message translator. Sole producer for all attached `EventChannel`s. Updates Manager bookkeeping in line with translation. Receives the platform fabric (scheduler, stop-token, scope) at bind time so it can produce stop-aware sender completions for in-flight `next_event` op-states.

```cpp
namespace Mashiro::Platform {

    class EventPump {
    public:
        // Bind the pump to its execution fabric and the Manager set. Called by
        // PlatformThread::Run() after the scheduler / stop-source / scope are
        // constructed. References to Managers come from a parameter pack that
        // PlatformThread expands once at bind time.
        template<class... Managers>
        void Bind(scheduler plat, stop_token stop, scope& sc, Managers&... mgrs) noexcept;

        void AttachChannel(EventChannel<>& channel) noexcept;

        // Platform thread loop entry points (called by Run()).
        void PumpOsMessages();         // PeekMessage + translate + bookkeep + broadcast
        void DrainExternalInbox();     // dedicated-thread mgrs → bookkeep + broadcast
        void WaitForWork(void* wakeEvent) noexcept;  // MsgWaitForMultipleObjects
        bool HasPending() const noexcept;

        // Dedicated-thread managers call this from their own thread.
        void SubmitExternal(SystemEvent&& event) noexcept;

    private:
        scheduler  plat_{};
        stop_token stop_{};
        scope*     scope_ = nullptr;

        static constexpr size_t kMaxChannels = 8;
        EventChannel<>* channels_[kMaxChannels]{};
        uint8_t         channelCount_ = 0;

        MpscQueue<SystemEvent> externalInbox_;   // stores moved variant values

        void Broadcast(SystemEvent&& event) noexcept;
        template<typename M> void DispatchBookkeep(M& mgr, const SystemEvent& event) noexcept;
        std::optional<SystemEvent> TranslateWin32(/* MSG */) noexcept;  // platform-specific impl
    };

} // namespace Mashiro::Platform
```

Per-event order on the Platform thread: translate → stamp `TimestampedEvent::timestamp` (only on alternatives that opted into the mixin, via a `template for` over `SystemEvent`'s alternative list) → dispatch bookkeep to all managers (`template for`) → broadcast (move) to all channels. This guarantees that when a client reads an event from its channel, every Manager's any-thread query already reflects the post-event state. Time-insensitive alternatives skip the stamp step entirely — the `if constexpr (Timestamped<T>)` arm is pruned at compile time.

There is no `DetachChannel` and no `Close`. A channel's lifetime is bounded by `PlatformThread`'s; on shutdown, the pump simply stops calling `Emit`, and `next_event` op-states observe `stop_token` and complete with `set_stopped()`. Removing channels mid-run was never used and would have raced with `Broadcast`; the simplification is intentional.

### 6.12 `Mashiro/Platform/PlatformThread.h`

Owns the Platform thread, its Pump, scheduler, scope, stop-source, and all Managers. `Run()` *takes over* the calling thread — there is no inner thread spawn.

```cpp
namespace Mashiro::Platform {

    class PlatformThread {
    public:
        // Run on the calling thread (= main). Does not return until the platform
        // stop_source has been requested AND every spawned sender owned by scope_
        // has settled. Throws on construction failure of the wake event; never
        // throws after Run() begins.
        void Run();

        // Cancellation entry point. Idempotent. Safe to call from any thread.
        void RequestStop() noexcept;

        // stdexec fabric handles. Lifetimes match PlatformThread's.
        [[nodiscard]] scheduler  Scheduler()   noexcept;        // platform_scheduler
        [[nodiscard]] stop_token StopToken()   const noexcept;  // observes stop_source_
        [[nodiscard]] scope&     Scope()       noexcept;        // counting_scope

        // Platform-thread managers
        WindowManager&             Windows();
        InputManager&              Input();
        ImeManager&                Ime();
        ClipboardManager&          Clipboard();
        CursorManager&             Cursor();
        DragDropManager&           DragDrop();
        DialogManager&             Dialogs();
        SurfaceManager&            Surfaces();
        SystemAppearanceManager&   Appearance();
        AccessibilityManager&      Accessibility();

        // Dedicated-thread managers (PlatformThread spawns/joins them)
        GamepadManager&    Gamepads();
        FileWatchManager&  FileWatches();

        // Free-threaded managers (state lives here, but APIs are callable anywhere)
        DisplayManager&        Displays();
        PowerManager&          Power();
        AudioDeviceManager&    AudioDevices();

        // Channel attach (no detach — lifetime is shutdown-driven via stop_token)
        void AttachChannel(EventChannel<>& channel) noexcept;

        // Implementation hook used by scheduler::sender; not for client code.
        bool IsOnPlatformThread() const noexcept;

    private:
        stop_source                 stop_;
        scope                       scope_;          // exec::counting_scope
        EventPump                   pump_;
        Detail::HandleQueue         queue_;          // MPSC handle queue (was OwnerExecutor)
        void*                       wakeEvent_ = nullptr;
        std::thread::id             owner_{};

        // ... Manager members (declaration order = teardown order, reversed)
    };

} // namespace Mashiro::Platform
```

`Run()` body:

```text
SetCurrentThreadName("Platform")
owner_      = std::this_thread::get_id()
wakeEvent_  = CreateEventW(nullptr, FALSE, FALSE, nullptr)
queue_.Initialize(owner_, wakeEvent_)
pump_.Bind(this->Scheduler(), this->StopToken(), this->Scope())

while (!stop_.stop_requested()) {
    pump_.PumpOsMessages();          // PeekMessage + translate + bookkeep + broadcast
    pump_.DrainExternalInbox();      // dedicated-thread events → bookkeep + broadcast
    queue_.Drain();                  // resume coroutine handles enqueued via plat_scheduler
    if (!pump_.HasPending() && queue_.IsEmpty() && !stop_.stop_requested()) {
        pump_.WaitForWork(wakeEvent_);   // MsgWaitForMultipleObjects
    }
}

// === Shutdown — structured, driven by stop_source + scope ==================
//
// All steps run on the platform thread. Each is one suspension point at most;
// nothing blocks indefinitely.
//
// 1. Drain queue + inbox once more so the *current* set of in-flight handles
//    observes the stop request through their receivers' environments. They
//    complete with set_stopped (cancellable senders) or set_value (already-
//    decided senders that were just waiting on the queue).
queue_.Drain();
pump_.DrainExternalInbox();

// 2. Stop dedicated-thread managers — they live on std::jthread because their
//    bodies are blocking syscalls (XInputGetState, ReadDirectoryChangesW) that
//    stdexec cannot suspend. Their event-emit path is already cancelled by the
//    stop_token; here we just join.
gamepadMgr_.Stop();
fileWatchMgr_.Stop();
pump_.DrainExternalInbox();          // last pass for in-flight emits

// 3. Wait for every spawned sender to settle. scope_.on_empty() is a sender
//    that completes when the count drops to zero. We sync_wait on it from the
//    platform thread itself, with the platform's run_loop draining anything
//    that needs the platform thread to finish (e.g., a Manager-internal sender
//    that re-schedules onto the platform thread to release an OS handle).
stdexec::sync_wait(stdexec::on(this->Scheduler(), stdexec::Joined(scope_)));

// 4. Manager destructors run when PlatformThread itself is destroyed — they
//    run on the platform thread by definition, so DestroyWindow / clipboard
//    cleanup / OLE revoke happen on the owning thread.
//    Run() simply returns; the caller (= main) is now free to destroy the
//    PlatformThread object on its way out of scope.
//
// === End of shutdown ========================================================
```

`RequestStop()` is a single line: `stop_.request_stop(); SetEvent(wakeEvent_);`. The stop request is observed by every receiver downstream of the platform stop-token (every `Task<T>`, every channel sender, every `scope`-owned spawn) at its next suspension point; the `SetEvent` ensures the platform thread wakes from `MsgWaitForMultipleObjects` even when there are no pending OS messages or queued handles. Idempotent: `request_stop()` is a no-op the second time.

Callers must not destroy `PlatformThread` until `Run()` returns. The structured-shutdown sequence above guarantees that when `Run()` returns, every spawned sender has settled and every dedicated thread has been joined — destruction is safe with no further synchronisation.

The "MainLoop" abstraction is gone. `Mashiro/Schedular/MainLoop.h` (currently empty) is removed in this change. `PlatformThread.h` is the sole loop owner.

## 7. Managers

Fifteen Managers split by scheduling mode. Each is a separate header under `Platform/Managers/`. Public APIs follow the patterns above; only the responsibilities and the OS APIs they wrap differ.

High-precision timing (QPC, timer resolution, waitable timers) is not a Manager — it has no thread affinity, no state to coordinate, and no events. It lives as free functions under `Mashiro::Platform::Time` (`Time::Now()`, `Time::SetTimerResolution(ms)`, `Time::CreateWaitableTimer()`, etc.).

### 7.1 Platform-thread managers

| Manager | Responsibility | Win32 APIs (representative) | Events emitted |
|---|---|---|---|
| `WindowManager` | Create/destroy/move/resize/mode/decorations/title/icon, DPI awareness, focus, opacity | `CreateWindowExW`, `SetWindowPos`, `SetWindowTextW`, `SetWindowLongPtrW`, `SetProcessDpiAwarenessContext`, `GetDpiForWindow` | `WindowResize`, `WindowClose`, `WindowFocus`, `WindowDpiChange`, `WindowMove` |
| `InputManager` | Cooked + raw keyboard, mouse (move/button/wheel/raw motion), touch, pen | `WM_KEY*`, `WM_CHAR`, `WM_MOUSE*`, `WM_INPUT` (`RegisterRawInputDevices`), `WM_TOUCH`, `WM_POINTER` | `InputKeyDown/Up`, `InputChar`, `InputMouseMove/Button`, `InputScroll`, `InputTouch`, `InputPen` |
| `ImeManager` | Composition string, candidate window placement, commit results, optional TSF integration | `ImmGet/SetCompositionStringW`, `ImmSetCandidateWindow`, TSF `ITfThreadMgr` | `ImeComposition`, `ImeCommit`, `ImeCandidateList` |
| `ClipboardManager` | Set/get text, HTML, RTF, image, custom formats; change notifications | `OpenClipboard`, `Get/SetClipboardData`, `RegisterClipboardFormatW`, `AddClipboardFormatListener` | `ClipboardUpdate` |
| `CursorManager` | Cursor shape, hide, custom image, confine, lock (FPS) | `SetCursor`, `LoadCursorW`, `CreateIconIndirect`, `ClipCursor`, `SetCursorPos`, `GetCursorPos` | (none — pure command target) |
| `DragDropManager` | Register drop targets, deliver drag events, initiate drags | OLE `RegisterDragDrop`, `IDropTarget`, `DoDragDrop`, `IDataObject` | `DragEnter`, `DragOver`, `DragDrop`, `DragLeave` |
| `DialogManager` | Native file open / save / folder picker, color picker, message box, font picker | `IFileOpenDialog`, `IFileSaveDialog`, `ChooseColorW`, `MessageBoxW`, `ChooseFontW` | (none — returns `Task<Result<...>>`) |
| `SurfaceManager` | Vulkan WSI surface create / destroy bound to a `WindowHandle` | `vkCreateWin32SurfaceKHR`, `vkDestroySurfaceKHR` | (none) |
| `SystemAppearanceManager` | Dark/light, accent color, high contrast, theme change subscription, immersive dark window attribute | Registry `Themes\Personalize`, `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)`, `WM_SETTINGCHANGE` | `AppearanceChange` |
| `AccessibilityManager` | UI Automation provider per window, focus / live-region announcements | `IRawElementProviderSimple`, `UiaReturnRawElementProvider`, `UiaRaiseAutomationEvent` | (none — responds to AT-SPI / UIA queries) |

### 7.2 Dedicated-thread managers

| Manager | Responsibility | OS APIs | Events emitted |
|---|---|---|---|
| `GamepadManager` | XInput / GameInput / evdev polling, hot-plug, vibration | `XInputGetState`, `XInputSetState`, `EVIOCSFF` | `GamepadConnect`, `GamepadDisconnect`, `GamepadState` |
| `FileWatchManager` | Directory change subscription, recursive, IOCP / inotify | `ReadDirectoryChangesW` + IOCP, `inotify_add_watch` + epoll | `FileCreated`, `FileModified`, `FileDeleted`, `FileRenamed` |

Both run on a `std::jthread` owned by `PlatformThread`. They emit events by calling `pump_.SubmitExternal(event)`, which pushes to the MPSC inbox and signals `wakeEvent_`. The Platform thread drains the inbox, performs bookkeeping, and broadcasts.

### 7.3 Free-threaded managers

| Manager | Responsibility | OS APIs | Bookkeeping driver |
|---|---|---|---|
| `DisplayManager` | Enumerate monitors, modes, DPI per monitor, ICC profiles, HDR caps | `EnumDisplayMonitors`, `GetMonitorInfoW`, `EnumDisplaySettingsExW`, `DXGI_OUTPUT_DESC1` | `WM_DISPLAYCHANGE` → `On(const DisplayChangeEvent&)` writes the cache via `SeqLock` |
| `PowerManager` | Prevent sleep / display off, battery query, sleep events | `SetThreadExecutionState`, `GetSystemPowerStatus`, `WM_POWERBROADCAST` | `WM_POWERBROADCAST` → `On(const PowerStateChangeEvent&)` updates and emits `PowerStateChange` |
| `AudioDeviceManager` | Endpoint enumeration, default device, hot-plug | WASAPI `IMMDeviceEnumerator`, `IMMNotificationClient` | callback thread posts via `SubmitExternal` |

These Managers' query methods are `[[=kAnyThread]]` and read from `SeqLock<T>` directly — no transfer, no allocation, no waiting.

### 7.4 Concrete Manager interface example

```cpp
namespace Mashiro::Platform {

    struct WindowHandle    { uint32_t id = 0; explicit operator bool() const noexcept; };
    struct NativeWindowView{ void* hwnd = nullptr; };

    class [[=kOnPlatformThread]] WindowManager {
    public:
        [[=kPlatformOnly]] Task<Result<WindowHandle>> Create(Window::WindowDesc desc);
        [[=kPlatformOnly]] Task<VoidResult>          Destroy(WindowHandle window);
        [[=kPlatformOnly]] Task<VoidResult>          SetTitle(WindowHandle window, std::string title);
        [[=kPlatformOnly]] Task<VoidResult>          SetSize(WindowHandle window, Window::Size size);
        [[=kPlatformOnly]] Task<VoidResult>          SetMode(WindowHandle window, Window::Mode mode);
        [[=kPlatformOnly]] Task<VoidResult>          Show(WindowHandle window);
        [[=kPlatformOnly]] Task<VoidResult>          Hide(WindowHandle window);

        [[=kAnyThread]] Window::WindowDesc GetDesc(WindowHandle window) const noexcept;
        [[=kAnyThread]] Window::Size       GetSize(WindowHandle window) const noexcept;
        [[=kAnyThread]] NativeWindowView   GetNativeView(WindowHandle window) const noexcept;
        [[=kAnyThread]] bool               IsValid(WindowHandle window) const noexcept;

    private:
        friend class EventPump;
        // Bookkeep handlers — convention-based discovery via
        // Event::Traits::HandlesBookkeep<WindowManager, P>. The dispatcher
        // calls the overload whose parameter type matches the active
        // SystemEvent alternative; no annotation tags the kind, since the
        // parameter type already carries it.
        void On(const WindowResizeEvent&)    noexcept;
        void On(const WindowCloseEvent&)     noexcept;
        void On(const WindowMoveEvent&)      noexcept;
        void On(const WindowDpiChangeEvent&) noexcept;
        void On(const WindowFocusEvent&)     noexcept;
        void On(const WindowDestroyEvent&)   noexcept; ///< retires the WindowState slot.

        // State storage: stable handles, no fixed cap, used existing primitive.
        struct WindowState {
            void* hwnd = nullptr;
            bool  alive = true;
        };
        ChunkedSlotMap<WindowState, WindowId> windows_;

        // SeqLock array — fixed capacity sufficient for all reasonably
        // concurrent windows (any-thread queries hit this directly). The
        // SlotMap may exceed kMaxLiveWindows transiently during create/destroy
        // churn; queries on a window beyond this cap fall through to a
        // mutex-guarded slow path (rare; only matters for >256 simultaneous
        // windows, which exceeds Win32's practical limits anyway).
        static constexpr size_t kMaxLiveWindows = 256;
        SeqLock<Window::WindowDesc> descs_[kMaxLiveWindows];
    };

    consteval { Detail::VerifyManagerContracts<WindowManager>(); }

} // namespace Mashiro::Platform
```

#### 7.4.1 `WindowManager` lifecycle — internal ordering

`Create` and `Destroy` are the two methods whose body must interleave with OS message traffic correctly. Both are `Task<…>`, so their bodies always run on the platform thread under `queue_.Drain()` (the platform scheduler's queued-continuation drain step in §6.12); the ordering below is *within* a single body, between the platform thread's own steps.

##### `Create(WindowDesc) → Task<Result<WindowHandle>>`

```text
0. (caller, any thread)  co_await platform.Windows().Create(desc)
                         → Task<T>'s initial-suspend sender (= schedule(plat))
                           enqueues the body's resume handle on platform_scheduler;
                           the body resumes inside queue_.Drain() (§6.12).

On the platform thread, in order, atomic with respect to OS messages
(no PeekMessage runs between steps 1–7):

1. id   = windows_.Allocate({ .hwnd = nullptr, .alive = true });
                         // ChunkedSlotMap returns a stable WindowId; the slot
                         // is reachable but the HWND field is still null.
2. if (id.index >= kMaxLiveWindows) → log + return ErrorCode::WindowCapExceeded;
3. descs_[id.index].Write(desc);
                         // SeqLock primed first so any-thread queries observe
                         // the requested desc the moment IsValid flips true.
4. hwnd = CreateWindowExW(... lpParam = WindowId{id} ...);
                         // WM_NCCREATE / WM_CREATE / first WM_SIZE / WM_MOVE
                         // re-enter this thread synchronously inside
                         // CreateWindowExW. Their WndProc routes them through
                         // EventPump::TranslateWin32 → DispatchBookkeep, which
                         // finds On(const WindowResizeEvent&) etc. — at this
                         // point windows_[id].hwnd is still null, so handlers
                         // that need it look up by WindowId, not by HWND.
5. if (hwnd == nullptr)  → windows_.Release(id); return last-error;
6. windows_[id].hwnd = hwnd;
                         // The HWND ↔ WindowId mapping is now bi-directional.
7. return Result<WindowHandle>{ WindowHandle{ id } };
                         // final_suspend → resumes the caller on its thread.
```

The bookkeep events fired *inside* `CreateWindowExW` (step 4) reach attached `EventChannel`s via `Broadcast` in the normal way. A client coroutine can therefore observe a `WindowResizeEvent` for a window before its own `co_await Create(...)` resumes — the platform thread has already published the size, but the worker thread hasn't been rescheduled yet. This is correct: `IsValid(handle)` returns `true` (the slot is alive from step 1) and `GetSize(handle)` returns the size that was just broadcast. There is no `WindowCreateEvent`; the first observable signal of a new window is whichever OS event fires first inside step 4 (`WindowMoveEvent` / `WindowResizeEvent` / `WindowDpiChangeEvent`, in that typical order).

##### `Destroy(WindowHandle) → Task<VoidResult>`

```text
0. (caller, any thread)  co_await platform.Windows().Destroy(handle)
                         → body resumes on the platform thread.

1. if (!windows_.Contains(handle.id)) → return ErrorCode::InvalidHandle;
2. windows_[handle.id].alive = false;
                         // Any-thread IsValid(handle) now returns false. In-
                         // flight GetSize / GetDesc readers either complete
                         // their SeqLock read against the still-valid slot
                         // or observe alive==false and bail.
3. hwnd = windows_[handle.id].hwnd;
4. DestroyWindow(hwnd);
                         // WM_DESTROY / WM_NCDESTROY re-enter synchronously.
                         // EventPump::TranslateWin32 produces a
                         // WindowDestroyEvent, DispatchBookkeep calls
                         // WindowManager::On(const WindowDestroyEvent&), which:
                         //   a. windows_[id].hwnd = nullptr;
                         //   b. windows_.Release(id);
                         // and Broadcast then publishes the event to every
                         // attached EventChannel.
5. return VoidResult{};
                         // The slot is already retired by the time the body
                         // returns to final_suspend.
```

The order of events from a client's perspective is therefore: **bookkeep → broadcast → caller resume**. By the time `co_await Destroy(handle)` returns to the worker thread, every other client thread has either already received `WindowDestroyEvent` on its channel or will on its next `Next()` / `NextBatch()` call — and `IsValid(handle)` returns `false` from any thread.

If the OS surfaces `WM_CLOSE` first (user clicked the close box), `EventPump` produces a `WindowCloseEvent`, the client handles it (or doesn't), and the window stays alive until somebody calls `Destroy`. `WindowCloseEvent` is advisory; `WindowDestroyEvent` is the slot-retirement signal.

##### Concurrency boundaries

| Reader / writer | Access | Guarantee |
|---|---|---|
| `IsValid(handle)` (any thread) | reads `alive` flag through `ChunkedSlotMap`'s atomic generation counter | False the instant Destroy step 2 commits, even if the HWND is still alive in step 4 |
| `GetSize` / `GetDesc` (any thread) | `SeqLock<WindowDesc>::Read()` on `descs_[id.index]` | Always reads either the value written by Create step 3 or by a later `On(const WindowResizeEvent&)`; readers that race with Destroy retry once and observe the post-Destroy state |
| `On(const WindowResizeEvent&)` (platform thread, inside `DispatchMessage`) | writes `descs_[id.index]` | Single writer; no contention with itself |
| Caller's `co_await` resume | scheduled by `final_suspend` | Always runs *after* the bookkeep arm of step 4 has retired the slot for Destroy, and *after* step 6 has bound the HWND for Create |

## 8. Data flow

### 8.1 Cross-thread call (worker → Manager)

```text
Worker coroutine (running under some client scheduler client_sch):
    co_await platform.Windows().Create(desc)

Steps:
1. Task<Result<WindowHandle>> is created. exec::task<T>'s promise constructs an
   environment whose `get_scheduler` query returns `platform.Scheduler()` (the
   `default_task_context<scheduler>` binding from §6.6).
2. `initial_suspend()` produces a sender equivalent to
   `stdexec::schedule(platform.Scheduler())`. await_ready() returns true iff
   `platform.IsOnPlatformThread()`. Same-thread fast path: zero suspension.
3. Cross-thread case: the schedule sender's `start` enqueues the resume handle
   on `queue_` (CAS-on-empty triggers `SetEvent(wakeEvent_)`). The receiver's
   environment carries `platform.StopToken()`, so the schedule op-state
   registers an `inplace_stop_callback` against it; if a stop request arrives
   before the platform thread picks up the handle, the callback removes the
   handle from the queue and completes with `set_stopped`, unwinding the task
   without ever entering the body.
4. Platform thread wakes from MsgWaitForMultipleObjects.
5. PumpOsMessages → DrainExternalInbox → queue_.Drain().
6. queue_.Drain() resumes the Task body — runs Create(desc) on platform thread.
7. Create allocates HWND, fills SeqLock, `co_return Result<WindowHandle>{...}`.
   `final_suspend` schedules the caller's resume on the *caller's* scheduler
   (`get_completion_scheduler<set_value_t>` of the awaiting receiver), which is
   `client_sch` for a worker coroutine.
8. Caller wakes on client_sch when its scheduler runs the resume handle.
9. await_resume returns Result<WindowHandle> to the worker.
```

If the caller is already on the platform thread (e.g., a Manager calling another Manager), step 2's `await_ready()` returns true and the body runs synchronously with no enqueue, no kernel call, no resume. Stop-token observation also collapses: the body's first `co_await` checks the token at suspension and either continues or completes with `set_stopped()`.

This whole flow is `Mashiro::Platform`'s contribution; the rest is `exec::task<T>`. There is no bespoke "TransferToOwner", no `OwnerTask::Promise::await_transform` — `task<T>` already does the transformation that re-schedules onto `scheduler` after every awaited inner sender, which is what scheduler-affinity (P3941) means.

### 8.2 OS event flow (OS → client)

```text
1. Platform thread is in MsgWaitForMultipleObjects.
2. Win32 posts a message; PeekMessage returns it.
3. EventPump::TranslateWin32 maps WM_* → SystemEvent.
4. Sequence + timestamp assigned.
5. EventPump::DispatchBookkeep<M>(mgr, event) for each platform-thread Manager:
   - std::visit over the active alternative; the matched arm is `if constexpr`-
     guarded by Event::Traits::HandlesBookkeep<M, P> so unmatched arms collapse
     to empty bodies and are eliminated after inlining.
   - The matching M::On(const P&) overload runs and updates SeqLock<T> for that
     Manager. No annotation, no kind-keyed table — the parameter type is the
     dispatch key.
6. EventPump::Broadcast(event):
   - For i in [0, channelCount_): channels_[i]->Emit(event).
   - Each Emit pushes to SPSC ring + waiter_.exchange(nullptr) → resume.
7. Client coroutines (one per channel) wake on their own threads.
8. Their schedulers resume the suspended `co_await channel.Next()`.
9. The sender completes with `set_value(SystemEvent)` (or `set_value(BatchView)` for the batch sender). On shutdown, the platform stop-token's request causes the same op-state to complete with `set_stopped()` instead, and the awaiting coroutine unwinds.
```

The client coroutine sees a fully consistent state: when it reads `WindowResize`, calling `platform.Windows().GetSize(window)` returns the new size because step 5 ran before step 6. The same ordering applies to `WindowDestroyEvent`: by the time a client wakes on it, `WindowManager::On(const WindowDestroyEvent&)` has already retired the slot (see §7.4.1), so `IsValid(handle)` returns `false` and any racing `GetSize` either retries the SeqLock and bails on `alive == false` or observes the pre-destroy state of an already-doomed slot — both are acceptable, since the handle is dead either way.

### 8.3 Dedicated-thread event flow

```text
GamepadManager poll thread (4ms cadence):
    XInputGetState returns new state.
    SystemEvent ev{kind = GamepadState, payload.gamepad = ...};
    pump_.SubmitExternal(ev);

SubmitExternal:
    externalInbox_.Push(ev);          // MPSC enqueue
    SetEvent(wakeEvent_);               // wake platform thread

Platform thread:
    DrainExternalInbox pops events from inbox; for each event:
        DispatchBookkeep<...>(...);
        Broadcast(event);
```

Latency added by the inbox hop is one MPSC push + one SetEvent + drain (≪ 1 µs); imperceptible relative to the 4 ms polling period.

### 8.4 Shutdown

```text
Caller (any thread, often a client coroutine reacting to WindowCloseEvent):
    platform.RequestStop()
        stop_.request_stop()           // observable through every receiver
        SetEvent(wakeEvent_)            // ensure the platform thread wakes

Run() exits the loop on the next iteration (stop_.stop_requested() is true).

Structured shutdown — driven by stop_token + scope.on_empty(), not bespoke
Close() / closed_ machinery:

1. queue_.Drain()  — in-flight Task<T> handles either resume their body once
   (if they had already been picked off the queue) or complete with set_stopped
   via their schedule-sender's stop_callback. No handle is leaked.
2. pump_.DrainExternalInbox() — last-pass for events submitted by dedicated
   threads up to the moment they were stopped.
3. gamepadMgr_.Stop() / fileWatchMgr_.Stop()  — request_stop() on each jthread,
   then join. Their event-emit path was already cancelled by the stop_token;
   here we just release the OS handle (CancelIoEx + close, etc.) and join.
4. sync_wait(on(scheduler, scope_.on_empty()))  — every spawned sender owned by
   scope_ has either set_value'd or set_stopped'd. The platform thread itself
   drains its own queue while waiting (the `on(scheduler, …)` wraps the wait
   so the platform thread keeps servicing in-flight platform-affine work).
5. Manager destructors run when PlatformThread itself is destroyed (after Run()
   returns). They run on the platform thread by definition, so DestroyWindow,
   clipboard cleanup, OLE revoke, etc., happen on the owning thread.
```

For each `EventChannel<>` that has an outstanding `next_event` / `next_batch` op-state, the platform stop-token's request fires the registered `inplace_stop_callback`, which exchanges the waiter handle out of `waiter_` and completes the receiver with `set_stopped()`. Client coroutines awaiting the channel see their `co_await` return into a `set_stopped` continuation — `Task<void>`-style coroutines unwind through their own `final_suspend` and propagate `set_stopped` upward, terminating cleanly. There is no `std::nullopt` sentinel and no `closed_` flag.

## 9. Error handling

- Fallible Manager APIs return `Task<Result<T>>` (where `Result<T> = std::expected<T, ErrorCode>`). Callers chain with `.and_then` / `.or_else` after `co_await`, or compose with `let_value` / `let_error` for sender-style pipelines.
- Infallible Manager APIs (`Show`, `Hide`, `SetTitle` after window is alive) return `Task<VoidResult>` — they can still report errors (e.g., `WindowHandle` is invalid).
- Any-thread queries (`GetSize`, `GetDesc`) have a precondition: caller must call `IsValid(handle)` first. Calling them on an invalid handle returns a default-constructed value silently — no error path. This keeps the hot read path free of error-handling code; safety is the caller's contract obligation.
- Coroutine exceptions (`unhandled_exception` in `Task<T>`'s promise) are forwarded as `set_error(std::exception_ptr)` and rethrown when the awaiting receiver consumes the value. The Platform thread itself never propagates exceptions out of `Run()`; it logs and continues. Senders spawned through `Spawn(scope, …)` route their `set_error` to the scope's error-handling policy (default: log + drop), since there is no awaiter to rethrow to.
- Cancellation is *not* an error. `set_stopped` flows through `Task<T>` and channel senders without involving `Result<T>`; callers that want to react to cancellation use `let_stopped` or `stopped_as_optional`. The `Result<T>` contract is for *operation* errors (`InvalidHandle`, `WindowCapExceeded`, OS error codes), not for "we shut down before the operation completed".
- Channel overflow: `Emit` returns `false` when the SPSC ring is full and increments `dropCount_`. `EventPump` emits a structured log entry per drop (payload type name via `NameOf`, channel index, total drop count) at info level. Clients can query `PendingCount()` and `DropCount()` and respond to back-pressure (e.g., a render thread that has stalled). Producer-side coalescing (§6.3) reduces drops for high-rate payloads like `MouseMoveEvent`.
- Mailbox overflow in `platform_scheduler`'s handle queue (the recast `OwnerExecutor` storage): the schedule sender's `start` calls `EventPump::SubmitResume`, which `std::terminate`s on a full ring. As §6.5 documents, the ring is sized for steady-state bursts and a full ring is a design-invariant violation, not a runtime condition; silently dropping a coroutine handle would leak the coroutine.
- Silent caps (`kMaxChannels = 8`, `kMaxLiveWindows = 256`): when exceeded, log a structured warning and assert in debug builds. Release builds degrade gracefully (channel attach fails, window query falls through to slow path) rather than crashing. The cross-thread coroutine mailbox (`MpscQueue<coroutine_handle<>, 256>`, §6.5) is *not* a soft cap — overflow `std::terminate`s, since silently dropping a handle would leak a coroutine.

## 10. Testing

Unit tests live under `Mashiro/tests/Platform/`. Tests are written with the project's existing Catch2 setup.

| Test target | Verifies |
|---|---|
| `EventChannelTest.cpp` | `co_await channel.Next()` and `next_event()`-as-sender, batch drain, lost-wake protocol under thread interleave (ASan + UBSan), `set_stopped` on stop-token request, ring overflow |
| `TaskTest.cpp` | Same-thread fast path produces zero suspensions, cross-thread transfer resumes on the platform thread, scheduler-affinity returns to platform thread after each `co_await`, set_value / set_error / set_stopped channels exercised, void specialisation |
| `SchedulerTest.cpp` | Recasts the v1.4 `OwnerExecutorTest`: MPSC enqueue from many threads via `stdexec::schedule(plat)`, drain order, pool exhaustion fallback, wake-event coalescing (CAS-on-empty), `get_completion_scheduler` query, `transform_sender` of `continues_on(plat, _)` collapses when upstream already completes there |
| `StopAndScopeTest.cpp` | `inplace_stop_source.request_stop()` propagates to a chain of `Task<T>`s and channel senders, `counting_scope.on_empty()` settles after every spawned sender completes, ordering-under-cancellation matches §8.4 |
| `SeqLockTest.cpp` | Single-writer multi-reader correctness under contention; tearing detection in stress tests |
| `ManagerTraitsTest.cpp` | A deliberately-mis-annotated Manager fails to compile (verified via `try_compile` CMake probes); the same probes assert that `Task<T>` is recognised by `IsTaskSpecialization` |
| `EventPumpTest.cpp` | Translation, timestamp stamping on `Timestamped` alternatives only, broadcast cardinality, bookkeeping precedes broadcast |
| `WindowManagerTest.cpp` | Create / destroy, title / size / mode mutations, `GetSize` returns post-event state after `WindowResize` is broadcast |
| `PlatformThreadIntegrationTest.cpp` | Drive `Run()` from the test thread (Run = main = test thread), attach a channel, call `Create` from a worker `std::jthread` coroutine, observe `WindowResize` from the channel, exercise structured shutdown via `RequestStop` + `scope.on_empty()` |

The negative compile probes for `ManagerTraitsTest.cpp` follow the pattern of `cmake/ReflectionFeatureProbes.cmake`: each negative case is its own tiny TU that *must* fail; CMake asserts the compile failure.

## 11. Decisions and alternatives

| Decision | Alternative considered | Why this won |
|---|---|---|
| Sole producer = Platform thread | Per-producer-per-client SPSC (cartesian product of channels) | Cartesian explodes channel count to `O(producers × clients)`; inbox model keeps it `O(clients)` and reuses existing SPSC primitives |
| Manager as state owner, not event consumer | Pub/sub Manager subscribing to events | The user's original intent was "Platform forwards events to clients; Managers only execute requests". Pub/sub mixed two orthogonal data flows and required Manager-side dispatch tables that duplicated client-side handling |
| Phase-less main loop (Pump → Drain → Wait) | 7-phase loop with consteval phase table | No real per-frame work justified phases; phases added complexity without preventing the only real reentrancy hazard (WndProc), which is already prevented by running coroutine bodies only between Pump and Wait |
| `Task<T>` = `exec::task<T>` bound to `platform_scheduler` via P3941 scheduler-affinity | (a) bespoke `OwnerTask<T>` + `TransferToOwner` initial_suspend (v1.4); (b) explicit `co_await SwitchToPlatform()` inside every method | The bespoke initial-suspend was a re-derivation of what scheduler-affinity already provides. With P3941, every `co_await sender` automatically resumes on `platform_scheduler` — so a Manager method that awaits `Surfaces().AttachVulkan(…)` doesn't need to remember to switch back. Explicit per-method switches are a forgetting hazard. The contract verification (`IsTaskSpecialization` over the return type's annotation set) keeps the same compile-time guarantee with one fewer concept |
| `platform_scheduler` is a stdexec scheduler; the v1.4 `OwnerExecutor` becomes its private state | Keep `OwnerExecutor` as a public component | Public knowledge of the executor leaked an implementation detail (the MPSC handle queue) into client code. Naming the abstraction `scheduler` lets clients reach for stdexec adaptors (`continues_on`, `schedule`, `schedule_from`) instead of a Manager-level API; the queue/wake stay, hidden behind the schedule sender |
| Cancellation via `inplace_stop_source` threaded through receiver environments | Per-channel `Close()` + `closed_` flag + `std::optional<SystemEvent>` sentinel; bespoke `RequestStop` propagation | One stop-source for the whole platform layer expresses one operational fact (the platform is shutting down). Channels, tasks, and spawned senders all observe it through their receivers' environments — no per-component "is this thing closed?" flag, no sentinel value distinguishing "stopped" from "no event yet". Heap-free (`inplace_*`), op-state-scoped, and integrates with `let_stopped` / `stopped_as_optional` for clients that want to differentiate cancellation from value |
| Structured shutdown via `exec::counting_scope::on_empty()` | Hand-rolled in-flight handle list + manual drain loop | `counting_scope` is the stdexec primitive for "I want to spawn senders without `start_detached`'s leak risk". Its `on_empty()` is a sender, so shutdown is one `sync_wait(on(scheduler, scope.on_empty()))` line and the platform thread services its own queue while waiting — no missed wake-ups, no manual count, no off-by-one |
| `Mashiro::Platform::domain` (P2999/P3826) for OS-specific `transform_sender` rewrites | Hard-code OS-specific paths inside each algorithm | Domains let one rewrite point teach every algorithm about Win32 `PostThreadMessage` / macOS `dispatch_async`. Without it, a hypothetical `bulk(plat, n, fn)` would need its own OS dispatch story; with it, the domain folds the transition into whatever the OS does best |
| `main` thread is the Platform thread; client work runs on `std::jthread`s | "OS thread + main thread" split where main spawns Platform | Win32 routes HWND messages to the *creating* thread, AppKit pins its run-loop to the OS-blessed first thread, and X11/Wayland event-pumping libraries assume single-thread invariants. The only role assignment that satisfies all three is "main = Platform". The earlier "main mediates client → platform handoff" idea was a third thread that added latency without buying isolation; client coroutines now `SetEvent(wakeEvent_)` directly through the platform scheduler |
| Bookkeeping by member-function-name convention (`On(const P&)`) discovered via reflection-driven concept | (a) annotation + consteval table, (b) `IEventConsumer` virtual interface | The parameter type is the kind tag — adding `[[=BookkeepFor{kind}]]` only invites drift. The convention-based design folds the dispatch table into a single `std::visit` whose arms are `if constexpr`-pruned by `HandlesBookkeep<M, P>`; after inlining it produces the same switch the annotation-driven `template for` did, with one fewer concept to maintain. The virtual interface lost on indirection cost and on losing the typed-alternative call signature |
| Single waiter per channel (atomic handle) | Waiter list / multiplex | Channels are SPSC by construction; consumer affinity is single-threaded, so multiple waiters on one channel cannot exist |
| `SeqLock` for composite any-thread queries | Mutex / shared_mutex | Lock-free read; writer is the Platform thread (single writer); composite values fit in one or two cache lines |
| Stop-token-as-shutdown (one `inplace_stop_source` for the whole platform) | (v1.4) per-channel `Close()` broadcast on shutdown; (older) per-channel sentinel `SystemEvent` | One stop-source observable through every receiver's environment is a single source of truth — channels, `Task<T>`s, and `scope`-owned spawns all see the same fact. `Close()` was a per-channel re-derivation of the same signal, plus a `closed_` flag that needed its own ordering against `waiter_`. Sentinel events polluted the event schema for shutdown bookkeeping |
| 15 Managers split by scheduling mode | Single monolithic `PlatformManager` | Each Manager owns one OS resource family; small files, focused tests, independent compile units |
| `SystemEvent` is a `std::variant` of one struct per leaf payload type, materialised structurally via reflection over the `Detail::EventPayloadBase` derivation graph | Tagged union with reflection-generated accessor (v1.0); enum-tagged variant with `[[=PayloadFor{kind}]]` (v1.1–v1.3) | The variant *is* the discriminator; `std::visit` over an exhaustive overload set is a compile-time check, and per-payload owning containers (`std::string`, `std::vector`) keep events self-contained — no side ring for variable-length data. Plain payloads stay `is_trivially_copyable_v`, preserving the v1.0 hot-path property. Dropping the `EventKind` enum and `PayloadFor` annotation eliminates the discriminator/payload drift class entirely; persistence consumers (keybinding configs) name the payload *types* through `Traits::PayloadTypeName<T>()` |
| Per-event opt-in mixins (`WindowSpecificEvent` / `TimestampedEvent`) | Shared `EventHeader { window, sequence, timestamp, flags }` block on every alternative (v1.1) | Of the four header fields, only the discriminator (now the variant's active alternative) and `timestamp` (kept on the time-sensitive subset only) had consumers. `windowId` on a non-window-scoped event invites a `== 0` sentinel that contradicts the variant-as-discriminator principle; `sequence` and `flags` had no consumer. Mixins make the participation set type-driven and let `std::visit` lambdas guard cross-cutting queries with `if constexpr (WindowScoped<T> / Timestamped<T>)` — non-participating alternatives are pruned at compile time |
| Producer-side coalescing of unpublished slots | In-place overwrite of already-published slots | Already-published overwrite would race with the consumer reading the slot, breaking SPSC's single-writer-of-tail invariant |
| `BatchView` non-coroutine input range | `Generator<const SystemEvent&>` | Generator allocates a coroutine frame; events are the hottest path; BatchView pops lazily from the SPSC ring with zero allocation |
| `ChunkedSlotMap` for `WindowState` storage | Fixed-size array with `count_` | Existing primitive; no silent cap; `SeqLock` array remains fixed because cross-thread query cap (256) is generous |
| High-precision timing as free functions, not a Manager | `TimingManager` | No thread affinity, no state to coordinate, no events to emit; a Manager class would be ceremony around static functions |

## 12. Examples

### 12.1 Minimal `int main()` — `main` is the Platform thread

```cpp
#include <Mashiro/Platform/PlatformThread.h>
#include <stdexec/execution.hpp>
#include <thread>

int main() {
    Mashiro::Platform::PlatformThread platform;

    // Client work runs on jthreads spawned BEFORE Run() takes over main.
    // Each jthread owns its own scheduler (e.g., a static_thread_pool::scheduler
    // or its own run_loop); the platform scheduler reaches them through
    // continues_on at the end of every Task<T>'s body.
    std::jthread render{[&](std::stop_token st) {
        // RunRenderClient returns a Mashiro::Platform::Task<void>, but we
        // start it on a per-thread run_loop because the render thread has its
        // own scheduler. sync_wait runs the task to completion on that loop.
        stdexec::run_loop loop;
        std::stop_callback cb{st, [&] { loop.finish(); }};
        stdexec::sync_wait(stdexec::on(loop.get_scheduler(),
                                       RunRenderClient(platform)));
    }};

    // Run() takes over `main` for the lifetime of the platform. It returns
    // when RequestStop() has been called and every spawned sender has
    // settled (see §6.12 / §8.4).
    platform.Run();

    // render's destructor signals its stop_token and joins. By construction
    // RunRenderClient observed platform.StopToken() (it derived from the
    // platform stop-source via its receiver's environment) and unwound
    // before Run() returned.
    return 0;
}
```

The role assignment is explicit: `main` becomes the Platform thread inside `Run()`, the render jthread runs the client coroutine, and the only synchronisation between them is `Mashiro::Platform::scheduler` (for client → platform calls) plus `EventChannel<>` senders (for platform → client events).

### 12.2 Render client coroutine

```cpp
Mashiro::Platform::Task<void>
RunRenderClient(Mashiro::Platform::PlatformThread& platform) {
    Mashiro::Platform::EventChannel<> events;
    platform.AttachChannel(events);

    auto window = co_await platform.Windows().Create({
        .title = "Mashiro",
        .size  = {1920, 1080},
        .flags = Window::Flags::Resizable | Window::Flags::HighDpi,
    });
    if (!window) co_return;  // Result<WindowHandle> — error path

    // Sender-style loop. when_any of the batch sender + a stop_token-aware
    // never_sender lets the platform's stop_token cancel the wait without
    // a per-channel close call. set_stopped propagates to co_return.
    for (;;) {
        auto batch = co_await events.NextBatch();   // set_stopped → break
        for (const auto& event : batch) {
            std::visit([&]<typename P>(const P& payload) {
                if constexpr (std::is_same_v<P, WindowResizeEvent>) {
                    RecreateSwapchain(payload.size.x, payload.size.y);
                } else if constexpr (std::is_same_v<P, WindowCloseEvent>) {
                    platform.RequestStop();
                }
            }, event);
        }
        if (platform.Windows().IsValid(*window)) {
            auto size = platform.Windows().GetSize(*window);  // SeqLock, any-thread
            RenderFrame(size);
        }
    }

    co_await platform.Windows().Destroy(*window);
}
```

`co_await events.NextBatch()` completes with `set_stopped()` when the platform stop-source is requested; `Task<void>`'s coroutine machinery turns that into a `co_return` and unwinds — the loop exits structurally without an `IsClosed()` poll.

### 12.3 Polling style (no coroutine)

```cpp
void GameTick(Mashiro::Platform::PlatformThread& platform,
              Mashiro::Platform::EventChannel<>& events) {
    while (auto event = events.TryReceive()) {
        gameWorld.HandleSystemEvent(*event);
    }
    gameWorld.Simulate(dt);
}
```

### 12.4 Cross-Manager call from a Manager method

```cpp
Mashiro::Platform::Task<Result<WindowHandle>>
WindowManager::CreateWithSurface(Window::WindowDesc desc, vk::Instance inst) {
    auto window = co_await Create(desc);     // already on platform thread → no transfer
    if (!window) co_return std::unexpected{window.error()};
    co_await platform_.Surfaces().AttachVulkan(*window, inst);
    co_return *window;
}
```

Scheduler-affinity (P3941) means the body returns to `platform_scheduler` after `AttachVulkan` completes, so `co_return *window` runs on the platform thread regardless of which scheduler `AttachVulkan`'s sender completed on. No manual switch-back.

### 12.5 Sender-only client (no coroutine, full stdexec composition)

```cpp
auto run_console_client(Mashiro::Platform::PlatformThread& platform) {
    using namespace stdexec;
    Mashiro::Platform::EventChannel<> events;
    platform.AttachChannel(events);

    return platform.Windows().Create(/* desc */)
         | let_value([&](auto& window) {
               return events.next_event()
                    | let_value([&](const SystemEvent& e) {
                          // Handle e on whichever scheduler the channel
                          // completed on; client orchestrates its own
                          // continues_on(...) to migrate as needed.
                          return just();
                      })
                    | repeat()                           // until set_stopped
                    | let_stopped([&]{ return platform.Windows().Destroy(window); });
           });
}
```

Demonstrates that nothing in the platform requires a coroutine — `Task<T>` is convenience, not necessity.

## 13. Glossary

- **Platform thread:** Single OS thread that owns thread-affine OS resources (HWND, IME, clipboard, OLE DnD, system dialogs, Vulkan surfaces). Concretely: whichever thread enters `PlatformThread::Run()`. The only ergonomic placement is `main`.
- **Client thread:** Any non-Platform thread that runs application logic — render, logic, UI, networking. Multiple are expected. Typically `std::jthread`s spawned before `main` enters `Run()`.
- **Worker thread:** Thread submitting a `Task<T>` request. Same as client thread when it happens to be running coroutines.
- **Manager:** A class owning one OS resource family (windows, input, clipboard, …). Either platform-thread, dedicated-thread, or free-threaded.
- **Bookkeeping:** Manager state updates performed by `EventPump` during translation, before `Broadcast`. Distinct from event consumption.
- **EventChannel:** SPSC ring + atomic waiter handle. Platform thread is the sole producer. Exposes both senders (`next_event` / `next_batch`) and awaiters (`Next` / `NextBatch`).
- **`Mashiro::Platform::scheduler`:** stdexec scheduler whose `schedule()` sender completes on the Platform thread. The public face of the cross-thread call flow; clients never name `OwnerExecutor` directly. (See §6.4–§6.5.)
- **`Mashiro::Platform::Task<T>`:** Typedef for `exec::task<T, exec::default_task_context<scheduler>>`. P3941 scheduler-affinity guarantees the body resumes on the Platform thread after every `co_await sender`. Replaces v1.4's bespoke `OwnerTask<T>`. (See §6.6.)
- **`Mashiro::Platform::stop_source` / `stop_token`:** Typedef for `stdexec::inplace_stop_source` / `inplace_stop_token`. Heap-free; the platform stop-source's token is threaded through every receiver's environment, so `RequestStop()` cancels every in-flight sender at once. (See §6.8.)
- **`Mashiro::Platform::scope`:** Typedef for `exec::counting_scope`. Owns the lifetime of every spawned background sender; `scope.on_empty()` settles when all spawned senders have completed. Used by structured shutdown. (See §6.7.)
- **`Mashiro::Platform::domain`:** stdexec domain registered with `scheduler` so `transform_sender` can rewrite OS-specific transitions (Win32 `PostThreadMessage`, macOS `dispatch_async`) at sender-pipeline construction time, not at execution. (See §6.8.)
- **Bookkeep handler:** A Manager member function `On(const P&) noexcept` that updates state when payload `P` is being broadcast. Discovered by `Event::Traits::HandlesBookkeep<M, P>`; no annotation tags it.
- **Free-threaded Manager:** Holds OS state but exposes only any-thread queries; mutation events arrive via Platform-thread bookkeeping into a `SeqLock`.

---

*End of design spec.*


