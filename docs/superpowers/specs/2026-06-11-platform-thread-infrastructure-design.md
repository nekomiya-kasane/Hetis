# Platform Thread Infrastructure — Design Spec

**Status:** Draft v1.1 (review fixes applied)
**Date:** 2026-06-11
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

---

## 1. Overview

Mashiro needs an OS abstraction layer that owns Win32 / X11 / Wayland resources with thread affinity (`HWND`, IME, clipboard, OLE DnD, system dialogs, Vulkan WSI surfaces). This spec defines that layer.

The design has two orthogonal data flows on a dedicated **Platform thread**:

1. **Event flow (out-bound):** OS messages are translated into a canonical `SystemEvent` schema and broadcast to client threads through SPSC `EventChannel<>`s. Clients consume events with `co_await channel.Next()` or `co_await channel.NextBatch()`.
2. **Call flow (in-bound):** Client coroutines call typed Manager APIs that return `OwnerTask<T>`. The task transparently transfers execution to the Platform thread when needed and resumes the caller on completion.

Managers are **state owners**, not event consumers. The Platform thread does *not* dispatch events to Managers; it only forwards events to clients and updates Manager bookkeeping in-line during translation.

## 2. Goals

- Single Platform thread is the sole owner of window-affinity OS resources.
- Client coroutines on render / logic / UI threads can `co_await` events and Manager calls naturally, with cross-thread wake.
- Zero runtime overhead for compile-time-decidable work: contract verification, route generation, phase tables.
- All thread-affinity rules expressed as types and annotations, verified at compile time via P2996 + P3394 + P3289 + P1306.
- Channel and mailbox cardinality is `O(clients) + 2`, not `O(producers × clients)`.
- No technical debt: no command catalog, no virtual-dispatch event router, no per-frame manager tick.

## 3. Non-Goals

- Cross-process plugin ABI for system calls. (If needed later, derive a command catalog from the Manager API surface using reflection.)
- Recording / replay of system events. The Platform layer does not persist events.
- Audio playback, asset loading, scripting. Those are layers above Platform.
- Windowing on mobile / web targets. The first delivery covers Windows; Linux X11/Wayland follows the same abstraction with platform-specific `src/Platform/Linux/` translation files.

## 4. Constraints

- Toolchain: clang-p2996 (`coca-toolchain-p2996`) with `-freflection-latest`, `-std=gnu++26`. Mandatory features verified by `cmake/ReflectionFeatureProbes.cmake`: P2996 reflection, P3394 annotations, P3289 consteval blocks, P1306 expansion statements, P3491 `define_static_array`.
- Existing infrastructure to reuse: `Generator<Ref,V,Alloc>`, `SpscQueue<T,N>`, `SpscByteRing<N>`, `InlineFunction<Sig,Cap>`, `ChunkedSlotMap<T,Id>`, `Result<T>`, `FixedString<N>`, `Traits::SequentialEnum`, `Traits::BitfieldEnum`, `Iota<N>`, `kCacheLineSize`, `SetCurrentThreadName`.
- Existing namespaces and conventions to respect: data-schema annotations live in `Core/Annotation.h`; do not extend that header with thread contracts.
- Cache-line aligned, lock-free where possible. No heap allocation on hot paths.
- All new headers under `Mashiro/include/Mashiro/Platform/`; all sources under `Mashiro/src/Platform/`. Platform-specific code under `src/Platform/Windows/` (this spec) and `src/Platform/Linux/` (deferred).

## 5. Architecture

### 5.1 Topology

```
┌──────────────────────────────────────────────────────────────────────┐
│                        Platform Thread                                │
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
│  │  Window, Input, Ime, Clipboard, Cursor,    │◀── OwnerTask<T> ─────┤
│  │  DragDrop, Dialog, Surface, Appearance,    │   (cross-thread,     │
│  │  Accessibility                             │    co_await)         │
│  └────────────────────────────────────────────┘                      │
│        ▲                                                             │
│  ┌──────────────┐                                                    │
│  │ MPSC         │◀────── coroutine handles from any worker thread    │
│  │ OwnerExecutor│                                                    │
│  └──────────────┘                                                    │
└──────────────────────────────────────────────────────────────────────┘

Free-threaded managers (any thread): Display, Power, AudioDevice
Free functions (not a Manager): `Mashiro::Platform::Time::*` for QPC, timer resolution, waitable timers.
```

### 5.2 Cardinality

| Channel / Queue | Count | Type |
|---|---|---|
| `EventChannel<>` | = number of client threads (typically 2–4) | SPSC, Platform thread is sole writer |
| MPSC event inbox | 1 | dedicated-thread managers → Platform thread |
| MPSC `OwnerExecutor` mailbox | 1 | any worker thread → Platform thread (coroutine handles) |

Total: `N + 2` where `N` = client threads. Not `producers × clients`.

### 5.3 Platform Thread Loop

```text
loop:
    pump.PumpOsMessages()       // PeekMessage + translate + bookkeep + broadcast
    pump.DrainExternalInbox()   // dedicated-thread events → bookkeep + broadcast
    executor.DrainAll()         // resume all pending OwnerTask coroutine handles
    if no pending work:
        MsgWaitForMultipleObjects(wakeEvent, INFINITE, QS_ALLINPUT)
```

There are no phases. Order is fixed: pump → drain inbox → drain executor → wait. Bookkeep handlers run inside `DispatchMessage` (they are part of message translation and must complete before the message is acked). What is *not* permitted to run inside `DispatchMessage` is user-initiated `OwnerTask` bodies; those are deferred to the explicit `executor_.DrainAll()` step that runs only between pump iterations. The boundary protects against re-entrant Manager mutation while the OS is still inside its own dispatch.

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
        PlatformThread,    // Lives on Platform thread; mutators return OwnerTask<T>.
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

The canonical event is a **`std::variant` of strongly-typed event structs**. Each event kind is its own struct that carries the common header (`EventHeader`) plus exactly the data that kind needs; `SystemEvent` is the variant of all of them. Consumers dispatch with `std::visit` over an overload set — the type system, not a `switch (kind)`, enforces that every alternative is handled.

**Why `std::variant`, not a tagged union:** the variant *is* the discriminated type, so there is no `kind`-to-payload discipline to keep in sync and no unchecked downcast. `std::visit` over a well-formed overload set is a compile error if any alternative is unhandled, which makes adding a new event a *driven* change (the build breaks until every visitor is updated). The active alternative, copy/move, and lifetime are all handled by the standard library, so variable-length data (IME strings, file paths, clipboard blobs) can be owned **in-place** by the event struct (`std::u16string`, `FixedString<N>`, `std::vector<std::byte>`); an event is self-contained, with no side-channel offset/length to manage.

`EventKind` is **kept** as a compile-time routing key (one enumerator per alternative), bridged to the variant by `KindOf<T>()`. It is used by producer-side coalescing (§6.3) and the `BookkeepFor{EventKind}` dispatch table (§6.7) — both of which are compile-time tables, not runtime tag reads.

> The previous **fixed-size POD/union transport** design (`sizeof(SystemEvent) == 64`, `union Payload`, reflection-generated `As<K>()`, side `SpscByteRing` for variable-length data) is **archived** in §6.2.1. It is retained only as rationale for the layout/throughput trade-offs; it is **not** the active design.

```cpp
namespace Mashiro::Platform {

    using WindowId = uint32_t;

    // Compile-time routing key: one enumerator per event alternative.
    // Order MUST match the SystemEvent variant alternative order.
    enum class EventKind : uint16_t {
        WindowCreate, WindowResize, WindowClose, WindowFocus,
        WindowDpiChange, WindowMove, WindowThemeChange,
        InputKeyDown, InputKeyUp, InputChar,
        InputMouseMove, InputMouseButton, InputScroll,
        InputTouch, InputPen,
        ImeComposition, ImeCommit, ImeCandidateList,
        ClipboardUpdate,
        DragEnter, DragOver, DragDrop, DragLeave,
        DisplayChange, PowerStateChange, AppearanceChange,
        GamepadConnect, GamepadDisconnect, GamepadState,
        FileCreated, FileModified, FileDeleted, FileRenamed,
    };
    static_assert(Traits::SequentialEnum<EventKind>);

    // Common header carried by every event. Stamped by EventPump on emission.
    struct EventHeader {
        WindowId window   = 0;   // 0 = not window-scoped
        uint32_t sequence = 0;   // monotonic per EventPump
        uint64_t timestamp= 0;   // QPC ticks
        uint16_t flags    = 0;   // kind-agnostic bits (synthetic, coalesced, ...)
    };

    // One struct per event kind. Each embeds the header and owns its own data.
    struct WindowResizeEvent  { EventHeader header; uint32_t width, height; };
    struct WindowCloseEvent   { EventHeader header; };
    struct KeyDownEvent       { EventHeader header; KeyCode code; Modifiers mods; bool repeat; };
    struct KeyUpEvent         { EventHeader header; KeyCode code; Modifiers mods; };
    struct ImeCompositionEvent{ EventHeader header; std::u16string text; uint32_t caret; };
    struct FileRenamedEvent   { EventHeader header; FixedString<260> oldPath, newPath; };
    // ... one struct per EventKind enumerator

    // The canonical event: a variant of all event structs. The active
    // alternative is the source of truth; there is no separate `kind` field.
    using SystemEvent = std::variant<
        WindowCreateEvent, WindowResizeEvent, WindowCloseEvent, WindowFocusEvent,
        WindowDpiChangeEvent, WindowMoveEvent, WindowThemeChangeEvent,
        KeyDownEvent, KeyUpEvent, CharEvent,
        MouseMoveEvent, MouseButtonEvent, ScrollEvent,
        TouchEvent, PenEvent,
        ImeCompositionEvent, ImeCommitEvent, ImeCandidateListEvent,
        ClipboardUpdateEvent,
        DragEnterEvent, DragOverEvent, DragDropEvent, DragLeaveEvent,
        DisplayChangeEvent, PowerStateChangeEvent, AppearanceChangeEvent,
        GamepadConnectEvent, GamepadDisconnectEvent, GamepadStateEvent,
        FileCreatedEvent, FileModifiedEvent, FileDeletedEvent, FileRenamedEvent
    >;

    // Compile-time bridge: alternative type -> EventKind (and the reverse via
    // std::variant_alternative_t<size_t(K), SystemEvent>). KindOf is consteval
    // so it folds away; nothing reads a runtime tag on the hot path.
    template<class T>
    [[nodiscard]] consteval EventKind KindOf() noexcept;            // index_of<T> in SystemEvent
    [[nodiscard]] inline    EventKind KindOf(const SystemEvent& e) noexcept  // = EventKind(e.index())
    { return static_cast<EventKind>(e.index()); }

    // Access an event's header regardless of the active alternative.
    [[nodiscard]] const EventHeader& HeaderOf(const SystemEvent& e) noexcept; // std::visit -> .header

    // Consteval schema check: the variant alternative order matches the
    // EventKind enumerator order, every alternative embeds EventHeader, and
    // every EventKind maps to exactly one alternative. Failure = compile error.
    consteval { Detail::VerifyEventSchema(); }

} // namespace Mashiro::Platform
```

Variable-length data (IME composition strings, file paths, clipboard blobs) is owned directly by the corresponding event struct (`std::u16string`, `FixedString<N>`, `std::vector<std::byte>`), so each event is self-contained. There is no side `SpscByteRing`: the SPSC queue stores moved `SystemEvent` values, and `EventChannel` (§6.3) moves events through the ring rather than `memcpy`-ing fixed-size POD.

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

Awaitable SPSC channel. The Platform thread is the sole producer for every channel. A consumer coroutine on a client thread can `co_await channel.Next()` (single event) or `co_await channel.NextBatch()` (drain-as-range).

**Precondition (per-channel):** at most one outstanding `Next()` / `NextBatch()` on a given channel. SPSC's contract is single *thread*, not single *coroutine*; two coroutines on the same client thread that both `co_await` the same channel would race on `waiter_`. Debug builds enforce this with an atomic flag set on suspend and cleared on resume; release builds rely on caller discipline. The intended pattern is one consumer coroutine per channel; clients that want multiple readers attach multiple channels.

```cpp
namespace Mashiro::Platform {

    template<uint32_t Capacity = 4096>
    class EventChannel {
    public:
        // Producer (Platform thread only). SystemEvent is a (possibly non-trivial)
        // variant, so events are *moved* into the ring rather than memcpy'd.
        bool Emit(SystemEvent&& event) noexcept;            // move into ring + wake waiter
        uint32_t EmitBatch(std::span<SystemEvent>) noexcept;
        void Close() noexcept;

        // Consumer (client thread, single outstanding await)
        std::optional<SystemEvent> TryReceive() noexcept;
        auto Next()      noexcept -> NextAwaiter;          // suspends until event or close
        auto NextBatch() noexcept -> BatchAwaiter;         // suspends, returns BatchView
        bool IsClosed()  const noexcept;
        uint32_t PendingCount() const noexcept;
        uint64_t DropCount()    const noexcept;            // diagnostic: # events dropped on overflow

    private:
        SpscQueue<SystemEvent, Capacity> ring_;   // stores moved variant values
        std::atomic<std::coroutine_handle<>> waiter_{nullptr};
        std::atomic<bool>     closed_{false};
        std::atomic<uint64_t> dropCount_{0};
        // Producer-side coalescing memory (only touched by platform thread).
        // lastKind_ is the routing key of the last push, taken from KindOf(event).
        EventKind lastKind_   = EventKind{0xFFFF};
        WindowId  lastWindow_ = 0;
        uint32_t  lastSlot_   = ~0u;

#ifndef NDEBUG
        std::atomic<bool> awaiting_{false};                // single-waiter assertion
#endif
    };

} // namespace Mashiro::Platform
```

**Wake protocol (lost-wake-free):**

1. `await_ready()` returns true if `!ring_.Empty() || closed_`.
2. `await_suspend(h)` stores `h` into `waiter_` (release).
3. After the store, re-check `!ring_.Empty() || closed_`. If true, attempt to reclaim the handle via `compare_exchange_strong(h, nullptr)`. Success → return `false` (don't suspend). Failure → producer already took the handle and will resume — stay suspended.
4. Producer's `Emit()` does `TryPush()`, then `waiter_.exchange(nullptr)` → `resume()` if non-null. The handle resume happens on the platform thread; coroutines may co-await an executor of their own to migrate back to their owning client thread (ApplicationLayer concern, not Platform's).

**`BatchAwaiter::await_resume()` returns `BatchView`** — a lightweight non-coroutine input range that pops events from `ring_` lazily on iteration. No coroutine frame allocation. Iteration ends when the ring is observed empty *or* a configurable batch cap is hit. `BatchView` is move-only and tied to the channel's lifetime; it must be fully consumed (or destroyed) before the next `co_await`.

**Producer-side coalescing for high-rate kinds.** For `MouseMoveEvent` and similar kinds where only the latest sample matters, the producer (`EventPump`) checks before push: if the *unpublished* tail slot would coincide with the previous push of the same kind (`KindOf(event) == lastKind_`) for the same window AND the consumer has not yet advanced past `lastSlot_`, the previous slot is move-assigned the new event before re-publishing the same `tail_`. This is sound because (a) only the producer touches unpublished slots, and (b) the consumer's view of `tail_` cannot regress. If the consumer has already advanced past `lastSlot_`, coalescing is skipped and a new event is pushed normally. Coalescing is opt-in per `EventKind` via a constexpr table.

**Overflow.** When `TryPush` fails (ring full), `Emit` increments `dropCount_` and returns `false`; the event is lost. `EventPump` logs structured drops at info level. Clients can query `PendingCount()` and `DropCount()` for back-pressure diagnostics.

### 6.4 `Mashiro/Platform/OwnerTask.h`

Owner-affine coroutine task. `co_await`-able from any coroutine on any thread.

```cpp
namespace Mashiro::Platform {

    struct TransferToOwner {
        bool await_ready() noexcept;                     // IsOnPlatformThread()
        void await_suspend(std::coroutine_handle<>) noexcept;  // executor_.Enqueue(h)
        void await_resume() noexcept {}
    };

    template<typename T = void>
    class [[nodiscard]] OwnerTask {
    public:
        struct Promise; using promise_type = Promise;
        using handle_type = std::coroutine_handle<Promise>;

        bool await_ready() const noexcept;
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept;
        T    await_resume();
        // ... move-only, destroys handle on destruction
    };

} // namespace Mashiro::Platform
```

`Promise::initial_suspend()` returns `TransferToOwner{}`. Result: when a Manager method that returns `OwnerTask<T>` is called from a worker thread, the body of that coroutine executes on the Platform thread; the caller is resumed on its own thread when the result is ready.

If the caller is already on the Platform thread, `TransferToOwner::await_ready()` returns true and the body runs synchronously with zero suspension.

**Lifetime contract.** The caller must keep the `OwnerTask` object alive until `co_await` completes. Destroying a task while its handle is queued in `OwnerExecutor` or actively suspended on another thread is undefined behaviour. The natural usage — `co_await Manager().Method(...)` as a temporary — is safe because the temporary persists across the suspension. Storing a task into a struct and destroying that struct while pending is the violation to avoid.

**Coroutine frame allocation.** `OwnerTask` carries a coroutine frame. The compiler can elide the allocation (HALO) only when it proves the frame does not escape — for cross-thread transfer it always escapes, so each cross-thread call allocates one frame on the heap. The "no heap on hot paths" goal in §4 applies to *event distribution* (the per-frame mouse/key event flood), not to one-shot Manager calls. If a future profile shows Manager-call frame allocation is hot, we can add a custom `Promise::operator new` backed by an `Mashiro::LinearAllocator` per worker.

### 6.5 `Mashiro/Platform/OwnerExecutor.h`

MPSC queue of coroutine handles. Multiple worker threads submit; the Platform thread drains.

```cpp
namespace Mashiro::Platform {

    class OwnerExecutor {
    public:
        void Initialize(uint32_t ownerThreadId, void* wakeEvent) noexcept;
        void Enqueue(std::coroutine_handle<> h) noexcept;  // any thread
        void DrainAll() noexcept;                          // platform thread
        bool HasPending() const noexcept;
        bool IsOnPlatformThread() const noexcept;
        static OwnerExecutor& Instance() noexcept;
    };

} // namespace Mashiro::Platform
```

Implementation: intrusive Treiber stack of pre-allocated nodes, sized by `kPoolSize = 256`. The pool covers the steady-state burst — tens of in-flight calls per worker thread — while the heap fallback covers genuinely bursty contention (e.g., a worker dispatching hundreds of file-watch unsubscribes during shutdown). The fallback is documented and counted, not exceptional. If the counter shows steady use, raise `kPoolSize`; do not treat it as a bug.

`Enqueue` ends with `SetEvent(wakeEvent_)` only when the queue transitioned from empty to non-empty (CAS on the head observed null), avoiding redundant kernel calls.

### 6.6 `Mashiro/Platform/SeqLock.h`

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

### 6.7 `Mashiro/Platform/ManagerTraits.h`

Compile-time verification of Manager API contracts via P2996 reflection on P3394 annotations.

```cpp
namespace Mashiro::Platform::Detail {

    consteval bool IsOwnerTaskSpecialization(std::meta::info type);

    template<typename Manager>
    consteval void VerifyManagerContracts();
    // For each public function member m of Manager:
    //   - if annotations_of(m, ^^ThreadContract) yields kPlatformOnly:
    //       static_assert return_type_of(m) is OwnerTask<T>
    //   - if it yields kAnyThread:
    //       static_assert return_type_of(m) is NOT OwnerTask<T>

    template<typename... Managers>
    consteval void VerifySchedulingContracts();
    // For each Manager M in Managers:
    //   - require exactly one ManagerSchedule annotation on ^^M
    //   - if PlatformThread: every Platform-domain method returns OwnerTask
    //   - if FreeThreaded:   no method returns OwnerTask
    //   - if DedicatedThread: no public method returns OwnerTask

    // Bookkeeping route generation: a private method tagged
    // [[=BookkeepFor{EventKind::X}]] is invoked by EventPump when an event of
    // kind X is being broadcast. Dispatch is generated by `template for`; the
    // current event's kind is read once via KindOf(event) (== EventKind(e.index()))
    // and matched against the table key, then the matching alternative is
    // recovered with std::get<std::variant_alternative_t<size_t(K), SystemEvent>>.
    struct BookkeepFor { EventKind kind; bool operator==(const BookkeepFor&) const = default; };

    template<typename Manager>
    consteval auto BuildBookkeepTable();
    // returns a std::define_static_array of {EventKind, std::meta::info}.

} // namespace Mashiro::Platform::Detail
```

`EventPump::DispatchBookkeep<Manager>(mgr, event)` is `template for`-expanded over `BuildBookkeepTable<Manager>()`, becoming a static switch on `KindOf(event)` with zero virtual calls; the matched arm passes the typed alternative (`std::get<...>(event)`) to the bookkeep handler.

### 6.8 `Mashiro/Platform/EventPump.h`

OS message translator. Sole producer for all attached `EventChannel`s. Updates Manager bookkeeping in line with translation.

```cpp
namespace Mashiro::Platform {

    class EventPump {
    public:
        void AttachChannel(EventChannel<>& channel) noexcept;
        void DetachChannel(EventChannel<>& channel) noexcept;
        void BindManagers(/* references to all platform-thread managers */) noexcept;

        // Platform thread loop entry points
        void PumpOsMessages();      // PeekMessage + translate + bookkeep + broadcast
        void DrainExternalInbox();  // dedicated-thread mgrs → bookkeep + broadcast
        void WaitForWork(void* wakeEvent) noexcept;  // MsgWaitForMultipleObjects

        // Dedicated-thread managers call this from their own thread
        void SubmitExternal(SystemEvent&& event) noexcept;

    private:
        static constexpr size_t kMaxChannels = 8;
        EventChannel<>* channels_[kMaxChannels]{};
        uint8_t channelCount_ = 0;

        MpscQueue<SystemEvent> externalInbox_;   // stores moved variant values
        uint32_t nextSequence_ = 0;

        void Broadcast(SystemEvent&& event) noexcept;
        template<typename M> void DispatchBookkeep(M& mgr, const SystemEvent& event) noexcept;
        std::optional<SystemEvent> TranslateWin32(/* MSG */) noexcept;  // platform-specific impl
    };

} // namespace Mashiro::Platform
```

Per-event order on the Platform thread: translate → stamp `EventHeader` (sequence + timestamp) on the active alternative → dispatch bookkeep to all managers (`template for`) → broadcast (move) to all channels. This guarantees that when a client reads an event from its channel, every Manager's any-thread query already reflects the post-event state.

### 6.9 `Mashiro/Platform/PlatformThread.h`

Owns the Platform thread, its Pump, Executor, and all Managers.

```cpp
namespace Mashiro::Platform {

    class PlatformThread {
    public:
        void Run();                               // does not return until RequestStop
        void RequestStop() noexcept;

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

        // Channel attach / detach
        void AttachChannel(EventChannel<>& channel) noexcept;
        void DetachChannel(EventChannel<>& channel) noexcept;

        OwnerExecutor& Executor() noexcept;
    };

} // namespace Mashiro::Platform
```

`Run()`:

```text
SetCurrentThreadName("Platform")
wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr)
executor_.Initialize(GetCurrentThreadId(), wakeEvent)
pump_.BindManagers(...)

while (running_) {
    pump_.PumpOsMessages();
    pump_.DrainExternalInbox();
    executor_.DrainAll();
    if (!pump_.HasPending() && !executor_.HasPending()) {
        pump_.WaitForWork(wakeEvent);
    }
}

// Shutdown ordering — every step runs on the platform thread:
// 1. Drain executor one last time so any in-flight OwnerTasks complete.
executor_.DrainAll();

// 2. Stop dedicated-thread managers and join their threads. Their inbox is
//    no longer fed; remaining inbox events are drained next.
gamepadMgr_.Stop();
fileWatchMgr_.Stop();
pump_.DrainExternalInbox();

// 3. Close client channels. Each Close() wakes its waiter so client
//    coroutines see std::nullopt / closed and unwind.
for each channel attached: channel.Close();

// 4. Destroy platform-thread managers. Their destructors run here, on the
//    platform thread, so DestroyWindow / clipboard cleanup / OLE revoke
//    happen on the owning thread.
//    (Manager members are destroyed in reverse declaration order when
//    PlatformThread itself is destroyed; this Run() simply returns.)
```

`RequestStop()` flips `running_ = false` and `SetEvent(wakeEvent_)` so the wait wakes immediately. Callers must not destroy `PlatformThread` until `Run()` returns.

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
| `DialogManager` | Native file open / save / folder picker, color picker, message box, font picker | `IFileOpenDialog`, `IFileSaveDialog`, `ChooseColorW`, `MessageBoxW`, `ChooseFontW` | (none — returns `OwnerTask<Result<...>>`) |
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
| `DisplayManager` | Enumerate monitors, modes, DPI per monitor, ICC profiles, HDR caps | `EnumDisplayMonitors`, `GetMonitorInfoW`, `EnumDisplaySettingsExW`, `DXGI_OUTPUT_DESC1` | `WM_DISPLAYCHANGE` → `OnPump_DisplayChange` writes the cache via `SeqLock` |
| `PowerManager` | Prevent sleep / display off, battery query, sleep events | `SetThreadExecutionState`, `GetSystemPowerStatus`, `WM_POWERBROADCAST` | `WM_POWERBROADCAST` → `OnPump_PowerStateChange` updates and emits `PowerStateChange` |
| `AudioDeviceManager` | Endpoint enumeration, default device, hot-plug | WASAPI `IMMDeviceEnumerator`, `IMMNotificationClient` | callback thread posts via `SubmitExternal` |

These Managers' query methods are `[[=kAnyThread]]` and read from `SeqLock<T>` directly — no transfer, no allocation, no waiting.

### 7.4 Concrete Manager interface example

```cpp
namespace Mashiro::Platform {

    struct WindowHandle    { uint32_t id = 0; explicit operator bool() const noexcept; };
    struct NativeWindowView{ void* hwnd = nullptr; };

    class [[=kOnPlatformThread]] WindowManager {
    public:
        [[=kPlatformOnly]] OwnerTask<Result<WindowHandle>> Create(Window::WindowDesc desc);
        [[=kPlatformOnly]] OwnerTask<VoidResult>          Destroy(WindowHandle window);
        [[=kPlatformOnly]] OwnerTask<VoidResult>          SetTitle(WindowHandle window, std::string title);
        [[=kPlatformOnly]] OwnerTask<VoidResult>          SetSize(WindowHandle window, Window::Size size);
        [[=kPlatformOnly]] OwnerTask<VoidResult>          SetMode(WindowHandle window, Window::Mode mode);
        [[=kPlatformOnly]] OwnerTask<VoidResult>          Show(WindowHandle window);
        [[=kPlatformOnly]] OwnerTask<VoidResult>          Hide(WindowHandle window);

        [[=kAnyThread]] Window::WindowDesc GetDesc(WindowHandle window) const noexcept;
        [[=kAnyThread]] Window::Size       GetSize(WindowHandle window) const noexcept;
        [[=kAnyThread]] NativeWindowView   GetNativeView(WindowHandle window) const noexcept;
        [[=kAnyThread]] bool               IsValid(WindowHandle window) const noexcept;

    private:
        friend class EventPump;
        // Bookkeep handlers receive the typed alternative (the dispatcher already
        // matched KindOf(event)), so no downcast or kind re-check is needed.
        [[=Detail::BookkeepFor{EventKind::WindowResize}]]   void OnPump_WindowResize(const WindowResizeEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowClose}]]    void OnPump_WindowClose(const WindowCloseEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowMove}]]     void OnPump_WindowMove(const WindowMoveEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowDpiChange}]]void OnPump_WindowDpiChange(const WindowDpiChangeEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowFocus}]]    void OnPump_WindowFocus(const WindowFocusEvent&) noexcept;

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

## 8. Data flow

### 8.1 Cross-thread call (worker → Manager)

```text
Worker coroutine:
    co_await platform.Windows().Create(desc)

Steps:
1. OwnerTask<Result<WindowHandle>> is created.
2. Promise::initial_suspend returns TransferToOwner{}.
3. TransferToOwner::await_ready() == false (not on platform thread).
4. await_suspend(h) → executor_.Enqueue(h); SetEvent(wakeEvent_) (if was empty).
5. Worker coroutine remains suspended.
6. Platform thread wakes from MsgWaitForMultipleObjects.
7. PumpOsMessages → DrainExternalInbox → executor_.DrainAll().
8. DrainAll resumes the OwnerTask body — runs Create(desc) on platform thread.
9. Create allocates HWND, fills SeqLock, return_value(...) — final_suspend resumes the caller.
10. Caller (worker thread) wakes when its scheduler runs h.
11. await_resume returns Result<WindowHandle> to the worker.
```

If the caller is already on the platform thread (e.g., a Manager calling another Manager), step 3 returns true and the body runs synchronously with no enqueue, no kernel call, no resume.

### 8.2 OS event flow (OS → client)

```text
1. Platform thread is in MsgWaitForMultipleObjects.
2. Win32 posts a message; PeekMessage returns it.
3. EventPump::TranslateWin32 maps WM_* → SystemEvent.
4. Sequence + timestamp assigned.
5. EventPump::DispatchBookkeep<M>(mgr, event) for each platform-thread Manager:
   - template-for over Detail::BuildBookkeepTable<M>() — pure switch.
   - Each handler updates SeqLock<T> for its Manager.
6. EventPump::Broadcast(event):
   - For i in [0, channelCount_): channels_[i]->Emit(event).
   - Each Emit pushes to SPSC ring + waiter_.exchange(nullptr) → resume.
7. Client coroutines (one per channel) wake on their own threads.
8. Their schedulers resume the suspended `co_await channel.Next()`.
9. await_resume returns std::optional<SystemEvent> (or yields a Generator).
```

The client coroutine sees a fully consistent state: when it reads `WindowResize`, calling `platform.Windows().GetSize(window)` returns the new size because step 5 ran before step 6.

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
Caller: platform.RequestStop()
    sets running_ = false
    SetEvent(wakeEvent_)

Run() exits the loop after the next iteration.
For each attached EventChannel<>: channel.Close();
    closed_.store(true)
    waiter_.exchange(nullptr) → resume (returns nullopt)

Each client coroutine sees `std::nullopt` from its `co_await Next()` (or empty Generator from NextBatch) and exits.

GamepadManager.Stop() → request_stop() on jthread; join.
FileWatchManager.Stop() → CancelIoEx + close handle; join.

All Managers destructors release OS resources (DestroyWindow for live windows, etc.).
```

## 9. Error handling

- Fallible Manager APIs return `OwnerTask<Result<T>>` (where `Result<T> = std::expected<T, ErrorCode>`). Callers chain with `.and_then` / `.or_else` after `co_await`.
- Infallible Manager APIs (`Show`, `Hide`, `SetTitle` after window is alive) return `OwnerTask<VoidResult>` — they can still report errors (e.g., `WindowHandle` is invalid).
- Any-thread queries (`GetSize`, `GetDesc`) have a precondition: caller must call `IsValid(handle)` first. Calling them on an invalid handle returns a default-constructed value silently — no error path. This keeps the hot read path free of error-handling code; safety is the caller's contract obligation.
- Coroutine exceptions (`unhandled_exception` in `OwnerTask::Promise`) are stored and rethrown in `await_resume()`. The Platform thread itself never propagates exceptions out of `Run()`; it logs and continues.
- Channel overflow: `Emit` returns `false` when the SPSC ring is full and increments `dropCount_`. `EventPump` emits a structured log entry per drop (kind, channel index, total drop count) at info level. Clients can query `PendingCount()` and `DropCount()` and respond to back-pressure (e.g., a render thread that has stalled). Producer-side coalescing (§6.3) reduces drops for high-rate kinds like `InputMouseMove`.
- Pool exhaustion in `OwnerExecutor`: `Enqueue` falls back to a heap-allocated node and increments a counter. As §6.5 documents, this is an expected path under bursty contention, not exceptional.
- Silent caps (`kMaxChannels = 8`, `kPoolSize = 256`, `kMaxLiveWindows = 256`): when exceeded, log a structured warning and assert in debug builds. Release builds degrade gracefully (channel attach fails, executor uses heap fallback, window query falls through to slow path) rather than crashing.

## 10. Testing

Unit tests live under `Mashiro/tests/Platform/`. Tests are written with the project's existing Catch2 setup.

| Test target | Verifies |
|---|---|
| `EventChannelTest.cpp` | Single-event `co_await`, batch drain, lost-wake protocol under thread interleave (ASan + UBSan), close while waiting, ring overflow |
| `OwnerTaskTest.cpp` | Same-thread fast path produces zero suspensions, cross-thread transfer resumes correctly, exception propagation, void specialisation |
| `OwnerExecutorTest.cpp` | MPSC enqueue from many threads, drain order, pool exhaustion fallback, wake-event coalescing |
| `SeqLockTest.cpp` | Single-writer multi-reader correctness under contention; tearing detection in stress tests |
| `ManagerTraitsTest.cpp` | A deliberately-mis-annotated Manager fails to compile (verified via `try_compile` CMake probes) |
| `EventPumpTest.cpp` | Translation, sequence numbering, broadcast cardinality, bookkeeping precedes broadcast |
| `WindowManagerTest.cpp` | Create / destroy, title / size / mode mutations, `GetSize` returns post-event state after `WindowResize` is broadcast |
| `PlatformThreadIntegrationTest.cpp` | Spin up the thread, attach a channel, call `Create` from a worker coroutine, observe `WindowResize` from the channel, shut down cleanly |

The negative compile probes for `ManagerTraitsTest.cpp` follow the pattern of `cmake/ReflectionFeatureProbes.cmake`: each negative case is its own tiny TU that *must* fail; CMake asserts the compile failure.

## 11. Decisions and alternatives

| Decision | Alternative considered | Why this won |
|---|---|---|
| Sole producer = Platform thread | Per-producer-per-client SPSC (cartesian product of channels) | Cartesian explodes channel count to `O(producers × clients)`; inbox model keeps it `O(clients)` and reuses existing SPSC primitives |
| Manager as state owner, not event consumer | Pub/sub Manager subscribing to events | The user's original intent was "Platform forwards events to clients; Managers only execute requests". Pub/sub mixed two orthogonal data flows and required Manager-side dispatch tables that duplicated client-side handling |
| Phase-less main loop (Pump → Drain → Wait) | 7-phase loop with consteval phase table | No real per-frame work justified phases; phases added complexity without preventing the only real reentrancy hazard (WndProc), which is already prevented by running coroutine bodies only between Pump and Wait |
| `OwnerTask<T>` with `TransferToOwner` initial_suspend | Explicit `co_await SwitchToPlatform()` inside every method | Implicit transfer at coroutine entry is harder to forget; the contract verification ensures the return type matches the annotation |
| Bookkeeping via `[[=BookkeepFor{kind}]]` annotations + consteval table | A virtual `IEventConsumer` interface | Consteval generation produces a static `template for` switch with zero indirect calls; avoids vtables and plays well with cache-line layouts |
| Single waiter per channel (atomic handle) | Waiter list / multiplex | Channels are SPSC by construction; consumer affinity is single-threaded, so multiple waiters on one channel cannot exist |
| `SeqLock` for composite any-thread queries | Mutex / shared_mutex | Lock-free read; writer is the Platform thread (single writer); composite values fit in one or two cache lines |
| Close-as-broadcast (broadcast `Close()` on shutdown) | Per-channel sentinel `SystemEvent` | Close already wakes the waiter and is observable via `IsClosed()`; injecting a sentinel pollutes the event schema |
| 15 Managers split by scheduling mode | Single monolithic `PlatformManager` | Each Manager owns one OS resource family; small files, focused tests, independent compile units |
| `SystemEvent` is a tagged union with reflection-generated accessor | `std::variant<...>` | Variant cannot share an outer header (kind/flags/sequence/timestamp), and its discriminator + alignment depend on libc++ implementation; the union form holds `sizeof == 64` precisely. Reflection-based `As<K>()` recovers variant's only real win — type-safe accessors — without dispatch overhead |
| Producer-side coalescing of unpublished slots | In-place overwrite of already-published slots | Already-published overwrite would race with the consumer reading the slot, breaking SPSC's single-writer-of-tail invariant |
| `BatchView` non-coroutine input range | `Generator<const SystemEvent&>` | Generator allocates a coroutine frame; events are the hottest path; BatchView pops lazily from the SPSC ring with zero allocation |
| `ChunkedSlotMap` for `WindowState` storage | Fixed-size array with `count_` | Existing primitive; no silent cap; `SeqLock` array remains fixed because cross-thread query cap (256) is generous |
| High-precision timing as free functions, not a Manager | `TimingManager` | No thread affinity, no state to coordinate, no events to emit; a Manager class would be ceremony around static functions |

## 12. Examples

### 12.1 Minimal client coroutine

```cpp
Task<void> RunRenderClient(PlatformThread& platform) {
    EventChannel<> events;
    platform.AttachChannel(events);

    auto window = co_await platform.Windows().Create({
        .title = "Mashiro",
        .size  = {1920, 1080},
        .flags = Window::Flags::Resizable | Window::Flags::HighDpi,
    });
    if (!window) co_return;  // Result<WindowHandle> — error path

    while (!events.IsClosed()) {
        for (const auto& event : co_await events.NextBatch()) {
            switch (event.kind) {
                case EventKind::WindowResize: {
                    const auto& payload = event.As<EventKind::WindowResize>();
                    RecreateSwapchain(payload.width, payload.height);
                    break;
                }
                case EventKind::WindowClose:
                    platform.RequestStop();
                    break;
                default: break;
            }
        }
        if (platform.Windows().IsValid(*window)) {
            auto size = platform.Windows().GetSize(*window);  // any-thread, SeqLock
            RenderFrame(size);
        }
    }

    co_await platform.Windows().Destroy(*window);
}
```

### 12.2 Polling style (no coroutine)

```cpp
void GameTick(PlatformThread& platform, EventChannel<>& events) {
    while (auto event = events.TryReceive()) {
        gameWorld.HandleSystemEvent(*event);
    }
    gameWorld.Simulate(dt);
}
```

### 12.3 Cross-Manager call from a Manager method

```cpp
OwnerTask<Result<WindowHandle>>
WindowManager::CreateWithSurface(Window::WindowDesc desc, vk::Instance inst) {
    auto window = co_await Create(desc);     // already on platform thread → no transfer
    if (!window) co_return std::unexpected{window.error()};
    co_await platform.Surfaces().AttachVulkan(*window, inst);
    co_return *window;
}
```

## 13. Glossary

- **Platform thread:** Single OS thread that owns thread-affine OS resources (HWND, IME, clipboard, OLE DnD, system dialogs, Vulkan surfaces).
- **Client thread:** Any non-Platform thread that runs application logic — render, logic, UI, networking. Multiple are expected.
- **Worker thread:** Thread submitting an `OwnerTask` request. Same as client thread when it happens to be running coroutines.
- **Manager:** A class owning one OS resource family (windows, input, clipboard, …). Either platform-thread, dedicated-thread, or free-threaded.
- **Bookkeeping:** Manager state updates performed by `EventPump` during translation, before `Broadcast`. Distinct from event consumption.
- **EventChannel:** SPSC ring + atomic waiter handle. Platform thread is the sole producer.
- **OwnerTask\<T\>:** Coroutine return type whose body runs on the Platform thread. Result is delivered to the caller's thread.
- **OwnerExecutor:** MPSC mailbox of suspended coroutine handles awaiting Platform-thread resumption.
- **Bookkeep handler:** A private Manager method tagged `[[=Detail::BookkeepFor{EventKind::X}]]` that updates state when `X` is being broadcast.
- **Free-threaded Manager:** Holds OS state but exposes only any-thread queries; mutation events arrive via Platform-thread bookkeeping into a `SeqLock`.

---

*End of design spec.*


