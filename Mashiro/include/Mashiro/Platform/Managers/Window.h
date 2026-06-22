/**
 * @file Window.h
 * @brief Platform-thread Window Manager: HWND ↔ WindowId registry and lifecycle.
 *
 * @par Responsibility
 * @c Window is the single source of truth for the (native handle, @c WindowId,
 * DPI, last-known size) tuple of every window that the Platform thread owns.
 * Every other component on the Platform thread that needs to translate between
 * a native handle (HWND, @c xdg_toplevel*, NSWindow*) and the project-internal
 * @ref WindowId reads it back through one of the four query methods below;
 * conversely, the OS-input bridge (@c PlatformBackendWindows.cpp's WndProc-side
 * translator, in particular) feeds new handles in via @ref Adopt and removes
 * them via @ref Withdraw.
 *
 * @par Two-step registration
 * The platform-native side knows the HWND *before* the corresponding
 * @ref WindowCreateEvent has been built (the HWND is the parameter to
 * @c WM_NCCREATE; the event needs the HWND mapped to a @c WindowId so that the
 * @c WindowSpecificEvent base can be filled in). The flow is therefore:
 *
 *   1. WndProc receives @c WM_NCCREATE → calls @ref Adopt to mint a @c WindowId.
 *   2. The translator constructs a @ref WindowCreateEvent carrying that id and
 *      enqueues it for broadcast.
 *   3. EventPump's bookkeep stage invokes @ref On(const WindowCreateEvent&), which
 *      cross-checks that the id is registered (defensive — never a runtime fault
 *      in correct code; it catches translator bugs in @c assert builds) and
 *      stores the captured size and DPI for downstream queries.
 *   4. Other platform-thread Managers see the broadcast next, by which point
 *      @ref Lookup already returns the entry.
 *
 * Destruction is symmetric: @ref On(const WindowDestroyEvent&) calls
 * @ref Withdraw after every other bookkeep recipient has had a chance to read
 * the entry. @ref WindowDestroyEvent is the *last* event for that @c WindowId
 * (per the comment on the payload itself).
 *
 * @par Identity policy
 * @c WindowId is monotonic and never reused — even after @ref Withdraw, the id
 * stays burned. Reuse would force every cached @c WindowId to carry a
 * generation counter to detect a stale identifier referring to a different
 * window; banning reuse outright sidesteps that overhead at a cost of 32 bits
 * per destroyed window. The counter wraps after ≈ 4 × 10⁹ windows, which is
 * outside any reasonable application's lifetime; an exhausted allocator
 * @c std::terminate's rather than wrap silently.
 *
 * @par Storage
 * Windows are normally a handful per process (the high water mark for desktop
 * applications stays well under 32). A flat @c std::vector<Entry> with linear
 * lookup beats @c std::unordered_map at this scale: better cache behaviour,
 * no hash dependency, no per-entry allocation, and the @c O(N) cost is
 * dominated by predictable branch behaviour. The decision is reconsidered the
 * day a project actually opens hundreds of windows; an indexed table keyed by
 * @c WindowId.value can be added without breaking the public contract.
 *
 * @par Native handle type
 * Stored as @c NativeWindowHandle (an alias for @c void*) so this header does
 * not drag @c <windows.h> into every translation unit that includes
 * @c PlatformApartment.h. The Win32 backend casts to @c HWND at the boundary;
 * the Wayland/X11 backend will use the same alias for @c xdg_toplevel*.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Math/Types.h"
#include "Mashiro/Platform/Common.h"
#include "Mashiro/Platform/SystemEvent.h"
#include "Mashiro/Platform/ThreadContract.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>     // std::terminate
#include <vector>

namespace Mashiro::Platform {

    /// @brief Type-erased native window handle (HWND, xdg_toplevel*, NSWindow*, …).
    ///
    /// Aliased rather than left as raw @c void* so call sites read with
    /// intent. Backends cast to the OS-native type at the API boundary.
    using NativeWindowHandle = void*;

    /// @brief Platform-thread window registry and identity allocator.
    ///
    /// All members must be invoked on the Platform thread. Concurrency safety
    /// is provided by the @c [[=OnPlatformThread]] apartment promise; the class
    /// itself locks nothing.
    struct [[=OnPlatformThread]] Window {

        // --------------------------------------------------------------
        // Registration — driven by the OS-input translator.
        // --------------------------------------------------------------

        /// @brief Mint a new @c WindowId for @p native and store it.
        ///
        /// Called from the Win32 WndProc on @c WM_NCCREATE (resp. the Wayland
        /// equivalent) *before* a @ref WindowCreateEvent is enqueued. Returns
        /// the freshly-minted id so the translator can write it into the event
        /// payload.
        ///
        /// @pre @p native is a non-null OS handle the calling thread will
        ///      keep alive at least until @ref Withdraw is called.
        /// @pre @p native is not currently registered. Registering the same
        ///      handle twice is a translator bug; this is checked in debug.
        [[nodiscard]] WindowId Adopt(NativeWindowHandle native) noexcept {
            assert(native != nullptr && "Window::Adopt requires a non-null handle");
            assert(IdOf(native) == WindowId::Invalid && "Window::Adopt: handle already registered");
            const std::uint32_t v = ++nextId_;
            if (v == 0) std::terminate();   // 32-bit wrap = ≥ 4·10⁹ windows; a programming error.
            const auto id = static_cast<WindowId>(v);
            entries_.push_back(Entry{ .id = id, .native = native });
            return id;
        }

        /// @brief Remove the entry for @p id. Idempotent on a missing id.
        ///
        /// Invoked from @ref On(const WindowDestroyEvent&); also exposed for
        /// translators that need to clean up after a creation that the OS
        /// rolled back before the @ref WindowCreateEvent was even broadcast.
        void Withdraw(WindowId id) noexcept {
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->id == id) { entries_.erase(it); return; }
            }
        }

        // --------------------------------------------------------------
        // Bookkeep stage — invoked by EventPump on every relevant payload.
        // --------------------------------------------------------------

        /// @brief Capture initial size and DPI; the entry is already present
        ///        from @ref Adopt at the WndProc level.
        void On(const Event::WindowCreateEvent& ev) noexcept {
            if (auto* e = Find(ev.windowId)) {
                e->size     = ev.size;
                e->dpiScale = ev.dpiScale;
            }
        }

        /// @brief Update cached size after a confirmed resize.
        ///
        /// Stored so a worker thread that asks "what is the current pixel size
        /// of window N?" can answer from the registry without round-tripping
        /// through the OS — backed by the bookkeep-before-broadcast invariant.
        void On(const Event::WindowResizeEvent& ev) noexcept {
            if (auto* e = Find(ev.windowId); e && !ev.isMinimised) e->size = ev.size;
        }

        /// @brief Update cached DPI scale on per-monitor DPI change.
        void On(const Event::WindowDpiChangeEvent& ev) noexcept {
            if (auto* e = Find(ev.windowId)) e->dpiScale = ev.newScale;
        }

        /// @brief Final entry removal. Last event for @p ev.windowId.
        void On(const Event::WindowDestroyEvent& ev) noexcept { Withdraw(ev.windowId); }

        // --------------------------------------------------------------
        // Queries — Platform thread only.
        // --------------------------------------------------------------

        /// @brief @c WindowId for @p native, or @ref WindowId::Invalid if not registered.
        ///
        /// Used by the OS-input translator to fill the @c windowId field of a
        /// @c WindowSpecificEvent before broadcast.
        [[nodiscard]] WindowId IdOf(NativeWindowHandle native) const noexcept {
            for (const auto& e : entries_) if (e.native == native) return e.id;
            return WindowId::Invalid;
        }

        /// @brief Native handle for @p id, or @c nullptr if not registered.
        [[nodiscard]] NativeWindowHandle HandleOf(WindowId id) const noexcept {
            for (const auto& e : entries_) if (e.id == id) return e.native;
            return nullptr;
        }

        /// @brief Last-broadcast pixel size for @p id, or @c {} if not registered.
        [[nodiscard]] ivec2 SizeOf(WindowId id) const noexcept {
            for (const auto& e : entries_) if (e.id == id) return e.size;
            return {};
        }

        /// @brief Last-broadcast DPI scale for @p id, or @c 1.0f if not registered.
        [[nodiscard]] float DpiScaleOf(WindowId id) const noexcept {
            for (const auto& e : entries_) if (e.id == id) return e.dpiScale;
            return 1.0f;
        }

        /// @brief Number of currently-registered windows.
        [[nodiscard]] std::size_t Count() const noexcept { return entries_.size(); }

    private:
        struct Entry {
            WindowId           id       = WindowId::Invalid;
            NativeWindowHandle native   = nullptr;
            ivec2              size{};
            float              dpiScale = 1.0f;
        };

        Entry* Find(WindowId id) noexcept {
            for (auto& e : entries_) if (e.id == id) return &e;
            return nullptr;
        }

        std::vector<Entry> entries_{};
        std::uint32_t      nextId_ = 0; // ++nextId_ before use; first id == 1.
    };

} // namespace Mashiro::Platform
