/**
 * @file PlatformBackend.h
 * @brief Native OS bridge: blocking wait for any platform input source, cross-thread
 *        wake, and translation of native messages into @ref Mashiro::SystemEvent.
 *
 * The backend is the only component that talks to OS APIs (Win32 message pump,
 * Wayland fd, evdev, ...). It exposes three orthogonal capabilities:
 *
 *  - @ref WaitForAnySource — block the calling thread (the Platform thread)
 *    until at least one source has work: native message queue is non-empty,
 *    @ref Wake has been called from another thread, or the supplied stop-token
 *    has fired. This is one *coalesced* wait, not three separate waits — a
 *    fragmented wait would force tail latency on whichever source got polled
 *    last, which on Windows means @c MsgWaitForMultipleObjectsEx, on Linux
 *    @c epoll on the eventfd plus the display-server fd.
 *
 *  - @ref Wake — coalescing wake-up callable from any thread. Win32 path
 *    @c SetEvent's a manual-reset wake @c HANDLE that participates in the
 *    @c MsgWaitForMultipleObjectsEx wait alongside the native queue; Linux
 *    path writes to an eventfd that participates in the @c epoll wait.
 *    Both are O(1), allocation-free, and idempotent under coalescing — the
 *    rationale (vs. @c PostMessage(WM_NULL) on Win32) is documented inline
 *    in @c PlatformBackendWindows.cpp.
 *
 *  - @ref EnumerateNative — drain the native queue into a sink. Sink is a
 *    type-erased callback (function-pointer + opaque state) so the backend's
 *    implementation can live in a separate translation unit; templating it
 *    would force every backend translation unit to expose its body to every
 *    caller.
 *
 * @par Platform splits
 * The implementation lives in @c PlatformBackendWindows.cpp (Win32) and
 * @c PlatformBackendLinux.cpp (X11/Wayland). The header itself is platform-
 * agnostic — selection is by the build system, not by macros at the call site.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Platform/SystemEvent.h"

#include <stdexec/execution.hpp>

#include <memory>
#include <utility>

namespace Mashiro {

    /// @brief Process-wide one-shot platform initialisation hooks.
    ///
    /// Called once on the Platform thread before any window/HWND is created.
    /// The implementation is in the per-OS backend translation unit
    /// (@c PlatformBackendWindows.cpp / @c PlatformBackendLinux.cpp); a single
    /// declaration here is the call-site contract — every TU that needs to
    /// invoke it picks it up from this header rather than carrying its own.
    namespace Backend {
        void Initialize();
    }

} // namespace Mashiro

namespace Mashiro::Platform {

    /// @brief Type-erased sink for one drained native event.
    ///
    /// Carries a free function plus an opaque state pointer (the EventPump
    /// instance, in practice). Equivalent to @c std::function<void(SystemEvent)>
    /// but allocation-free — the function pointer is set once per drain pass.
    struct NativeEventSink {
        void (*invoke)(void* state, SystemEvent ev) noexcept = nullptr;
        void* state                                          = nullptr;

        void operator()(SystemEvent ev) const noexcept {
            invoke(state, std::move(ev));
        }
    };

    /**
     * @brief Native OS bridge for the Platform thread.
     *
     * @par Threading
     * @ref WaitForAnySource and @ref EnumerateNative are Platform-thread-only.
     * @ref Wake is callable from any thread.
     */
    class PlatformBackend final {
    public:
        PlatformBackend();
        ~PlatformBackend();

        PlatformBackend(const PlatformBackend&)            = delete;
        PlatformBackend& operator=(const PlatformBackend&) = delete;
        PlatformBackend(PlatformBackend&&)                 = delete;
        PlatformBackend& operator=(PlatformBackend&&)      = delete;

        /// @brief Block until any source is ready or @p stop fires.
        ///
        /// Returns when the native queue has at least one message, when
        /// @ref Wake has been called since the last wait started, or when
        /// @p stop has @c stop_requested(). The function is @em level-triggered
        /// on the native queue: if the caller does not drain it before the next
        /// wait, the next wait returns immediately.
        void WaitForAnySource(stdexec::inplace_stop_token stop) noexcept;

        /// @brief Coalescing wake-up. Safe from any thread, signal-safe on Linux.
        ///
        /// Multiple concurrent calls collapse to one wake at the wait site —
        /// the backend buffers exactly one pending wake. Producers therefore do
        /// not need to throttle their wake calls.
        void Wake() noexcept;

        /// @brief Drain native messages, feeding each into @p sink.
        ///
        /// The sink is invoked synchronously on the Platform thread for every
        /// translated message. Non-translatable native messages (timer ticks,
        /// internal pings) are consumed silently. Drains until the native queue
        /// is observed empty; concurrent producers (other apps, the OS) may add
        /// more after that point — the next @ref WaitForAnySource will see them.
        void EnumerateNative(NativeEventSink sink) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace Mashiro::Platform
