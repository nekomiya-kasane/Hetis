# Platform Thread Infrastructure — Design Spec

**Status:** Draft for review
**Date:** 2026-06-11
**Author:** Mashiro Engine team
**Scope:** `Mashiro::Platform` namespace; new sources under `Mashiro/include/Mashiro/Platform/` and `Mashiro/src/Platform/`.

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

Free-threaded managers (any thread): Display, Power, AudioDevice, Timing
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

There are no phases. Order is fixed: pump → drain inbox → drain executor → wait. WndProc reentrancy is contained because Platform-thread-side Manager mutations always go through the executor and only run between Pump and Wait, never inside `DispatchMessage`.

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

A trivially-copyable, fixed-size canonical event. Sized to one cache line so emission is one `memcpy`.

```cpp
namespace Mashiro::Platform {

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

    using WindowId = uint32_t;

    // Per-kind payloads (declared once each; full set in header):
    struct WindowResizePayload { WindowId window; uint32_t width, height; bool minimized; };
    struct KeyPayload          { WindowId window; uint16_t scancode, vkey; uint8_t mods; bool repeat; };
    // ... (DragPayload, ImePayload, GamepadStatePayload, FileChangePayload, etc.)

    struct SystemEvent {
        EventKind kind;
        uint16_t  flags;
        uint32_t  sequence;   // monotonic, assigned by EventPump
        uint64_t  timestamp;  // QPC ticks
        union Payload {
            WindowResizePayload resize;
            KeyPayload          key;
            // ... one branch per EventKind
            alignas(8) uint8_t  raw[48];
        } payload;
    };
    static_assert(std::is_trivially_copyable_v<SystemEvent>);
    static_assert(sizeof(SystemEvent) == 64);

} // namespace Mashiro::Platform
```

Variable-length data (IME composition strings, file paths, clipboard blobs) is stored in a side `SpscByteRing` per channel; the event carries an offset + length into that ring.

### 6.3 `Mashiro/Platform/EventChannel.h`

Awaitable SPSC channel. The Platform thread is the sole producer for every channel. A consumer coroutine on a client thread can `co_await channel.Next()` (single event) or `co_await channel.NextBatch()` (drain-as-generator). One pending waiter per channel — guaranteed by SPSC consumer-side affinity.

```cpp
namespace Mashiro::Platform {

    template<uint32_t Capacity = 4096>
    class EventChannel {
    public:
        // Producer (Platform thread only)
        bool Emit(const SystemEvent& event) noexcept;     // push + wake waiter
        uint32_t EmitBatch(std::span<const SystemEvent>) noexcept;
        void Close() noexcept;

        // Consumer (client thread)
        std::optional<SystemEvent> TryReceive() noexcept;
        auto Next()      noexcept -> NextAwaiter;         // suspends until event or close
        auto NextBatch() noexcept -> BatchAwaiter;        // suspends, returns Generator drain
        bool IsClosed()  const noexcept;

    private:
        SpscQueue<SystemEvent, Capacity>            ring_;
        std::atomic<std::coroutine_handle<>>        waiter_{nullptr};
        std::atomic<bool>                           closed_{false};

        void WakeConsumer() noexcept; // exchange(nullptr) + resume
    };

} // namespace Mashiro::Platform
```

Wake protocol (lost-wake-free):

1. `await_ready()` returns true if `!ring_.Empty() || closed_`.
2. `await_suspend(h)` stores `h` into `waiter_` (release).
3. After the store, re-check `!ring_.Empty() || closed_`. If true, attempt to reclaim the handle via `compare_exchange_strong(h, nullptr)`. Success → return `false` (don't suspend). Failure → producer already took the handle and will resume — stay suspended.
4. Producer's `Emit()` does `TryPush()`, then `waiter_.exchange(nullptr)` → `resume()` if non-null.

`BatchAwaiter::await_resume()` returns a `Generator<const SystemEvent&>` that lazily drains all currently available events; integrates with range-based `for`.

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

Implementation: intrusive Treiber stack of pre-allocated nodes (no heap on hot path). Pool size `kPoolSize = 256` — enough for thousands of in-flight calls because nodes are freed as soon as the handle is resumed. If the pool is exhausted, `Enqueue` falls back to a heap node (cold path, instrumented).

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
    // kind X is being broadcast. The dispatch is generated by `template for`.
    struct BookkeepFor { EventKind kind; bool operator==(const BookkeepFor&) const = default; };

    template<typename Manager>
    consteval auto BuildBookkeepTable();
    // returns a std::define_static_array of {EventKind, std::meta::info}.

} // namespace Mashiro::Platform::Detail
```

`EventPump::DispatchBookkeep<Manager>(mgr, event)` is `template for`-expanded over `BuildBookkeepTable<Manager>()`, becoming a static switch with zero virtual calls.

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
        void SubmitExternal(const SystemEvent& event) noexcept;

    private:
        static constexpr size_t kMaxChannels = 8;
        EventChannel<>* channels_[kMaxChannels]{};
        uint8_t channelCount_ = 0;

        MpscQueue<SystemEvent> externalInbox_;
        uint32_t nextSequence_ = 0;

        void Broadcast(const SystemEvent& event) noexcept;
        template<typename M> void DispatchBookkeep(M& mgr, const SystemEvent& event) noexcept;
        std::optional<SystemEvent> TranslateWin32(/* MSG */) noexcept;  // platform-specific impl
    };

} // namespace Mashiro::Platform
```

Per-event order on the Platform thread: translate → assign sequence + timestamp → dispatch bookkeep to all managers (`template for`) → broadcast to all channels. This guarantees that when a client reads an event from its channel, every Manager's any-thread query already reflects the post-event state.

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

// Shutdown
for each channel: channel.Close();
gamepadMgr_.Stop();
fileWatchMgr_.Stop();
```

## 7. Managers

Sixteen Managers split by scheduling mode. Each is a separate header under `Platform/Managers/`. Public APIs follow the patterns above; only the responsibilities and the OS APIs they wrap differ.

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
        [[=Detail::BookkeepFor{EventKind::WindowResize}]]   void OnPump_WindowResize(const SystemEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowClose}]]    void OnPump_WindowClose(const SystemEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowMove}]]     void OnPump_WindowMove(const SystemEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowDpiChange}]]void OnPump_WindowDpiChange(const SystemEvent&) noexcept;
        [[=Detail::BookkeepFor{EventKind::WindowFocus}]]    void OnPump_WindowFocus(const SystemEvent&) noexcept;

        struct State { void* hwnd = nullptr; bool alive = true; };
        static constexpr size_t kMax = 64;
        State                   state_[kMax]{};
        SeqLock<Window::WindowDesc> descs_[kMax];
        uint32_t count_ = 0;
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
- Any-thread queries (`GetSize`, `GetDesc`) return values, not `Result`. They never fail; they read from `SeqLock<T>` and return the cached state. An invalid `WindowHandle` returns a default-constructed value; callers must check `IsValid` first if they need certainty.
- Coroutine exceptions (`unhandled_exception` in `OwnerTask::Promise`) are stored and rethrown in `await_resume()`. The Platform thread itself never propagates exceptions out of `Run()`; it logs and continues.
- Channel overflow: `Emit` returns `false` when the SPSC ring is full. `EventPump` increments a per-channel `dropCount_` and continues. Clients can query `PendingCount()` and respond to back-pressure (e.g., a render thread that has stalled). High-rate event kinds (mouse moves) coalesce on emission: if the most recent event in the ring is the same kind for the same window, payload is overwritten.
- Pool exhaustion in `OwnerExecutor`: `Enqueue` falls back to a heap-allocated node; the slow path is instrumented with a counter for diagnostics.

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
                case EventKind::WindowResize:
                    RecreateSwapchain(event.payload.resize.width,
                                      event.payload.resize.height);
                    break;
                case EventKind::WindowClose:
                    platform.RequestStop();
                    break;
                default: break;
            }
        }
        auto size = platform.Windows().GetSize(*window);  // any-thread, SeqLock
        RenderFrame(size);
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


