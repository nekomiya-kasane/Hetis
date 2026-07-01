/**
 * @file WindowManager.h
 * @brief Platform-thread registry for native windows and WindowId ownership.
 *
 * WindowManager is the Platform-thread source of truth for the native handle, WindowId, last client size, and DPI
 * scale of every window owned by the process. Backends use it while translating native messages; other managers read
 * the resulting state after EventPump's bookkeep stage.
 *
 * @par Registration order
 * Native systems usually expose the native handle before a WindowCreateEvent can be emitted. The backend therefore
 * adopts the handle first, emits WindowCreateEvent with the assigned WindowId, and lets EventPump call
 * @ref On(const Event::WindowCreateEvent&) before subscriber broadcast.
 *
 * @par Identity
 * WindowId values are monotonic and never reused. This avoids generation counters on every application-side window
 * reference; exhaustion is treated as a design-invariant violation and terminates rather than wrapping silently.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Math/Types.h"
#include "Mashiro/Platform/Common.h"
#include "Mashiro/Platform/SystemEvent.h"
#include "Mashiro/Platform/ThreadContract.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Mashiro::Platform {

    /** @brief Type-erased native window handle: HWND, xdg_toplevel*, NSWindow*, or equivalent. */
    using NativeWindowHandle = void*;

    /**
     * @brief Platform-thread window registry and identity allocator.
     *
     * All members run on the Platform thread. The class does not lock; thread-affinity is declared by the
     * @c [[=OnPlatformThread]] annotation and enforced by routing through EventPump / PlatformThread.
     */
    struct [[=OnPlatformThread]] WindowManager {
        /** @brief Mint a WindowId for @p native and register the native handle. */
        [[nodiscard]] WindowId Adopt(NativeWindowHandle native) noexcept {
            assert(native != nullptr && "WindowManager::Adopt requires a non-null handle");
            assert(IdOf(native) == WindowId::Invalid && "WindowManager::Adopt: handle already registered");
            const std::uint32_t value = ++nextId_;
            if (value == 0) {
                std::terminate();
            }
            const auto id = static_cast<WindowId>(value);
            entries_.push_back(Entry{.id = id, .native = native});
            return id;
        }

        /** @brief Remove @p id if it is still registered. */
        void Withdraw(WindowId id) noexcept {
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->id == id) {
                    entries_.erase(it);
                    return;
                }
            }
        }

        /** @brief Capture initial size and DPI after native creation is translated. */
        void On(const Event::WindowCreateEvent& ev) noexcept {
            if (auto* entry = Find(ev.windowId)) {
                entry->size = ev.size;
                entry->dpiScale = ev.dpiScale;
            }
        }

        /** @brief Update cached client size after a non-minimised resize. */
        void On(const Event::WindowResizeEvent& ev) noexcept {
            if (auto* entry = Find(ev.windowId); entry != nullptr && !ev.isMinimised) {
                entry->size = ev.size;
            }
        }

        /** @brief Update cached DPI scale after a per-monitor DPI transition. */
        void On(const Event::WindowDpiChangeEvent& ev) noexcept {
            if (auto* entry = Find(ev.windowId)) {
                entry->dpiScale = ev.newScale;
            }
        }

        /** @brief Withdraw the final lifecycle event's window id. */
        void On(const Event::WindowDestroyEvent& ev) noexcept { Withdraw(ev.windowId); }

        /** @brief Return the WindowId assigned to @p native, or WindowId::Invalid. */
        [[nodiscard]] WindowId IdOf(NativeWindowHandle native) const noexcept {
            for (const auto& entry : entries_) {
                if (entry.native == native) {
                    return entry.id;
                }
            }
            return WindowId::Invalid;
        }

        /** @brief Return the native handle for @p id, or nullptr when absent. */
        [[nodiscard]] NativeWindowHandle HandleOf(WindowId id) const noexcept {
            for (const auto& entry : entries_) {
                if (entry.id == id) {
                    return entry.native;
                }
            }
            return nullptr;
        }

        /** @brief Return the last broadcast client size for @p id, or zero size when absent. */
        [[nodiscard]] ivec2 SizeOf(WindowId id) const noexcept {
            for (const auto& entry : entries_) {
                if (entry.id == id) {
                    return entry.size;
                }
            }
            return {};
        }

        /** @brief Return the last broadcast DPI scale for @p id, or 1.0 when absent. */
        [[nodiscard]] float DpiScaleOf(WindowId id) const noexcept {
            for (const auto& entry : entries_) {
                if (entry.id == id) {
                    return entry.dpiScale;
                }
            }
            return 1.0f;
        }

        /** @brief Number of currently registered windows. */
        [[nodiscard]] std::size_t Count() const noexcept { return entries_.size(); }

    private:
        struct Entry {
            WindowId id = WindowId::Invalid;
            NativeWindowHandle native = nullptr;
            ivec2 size{};
            float dpiScale = 1.0f;
        };

        /** @brief Return the mutable entry for @p id, or nullptr. */
        [[nodiscard]] Entry* Find(WindowId id) noexcept {
            for (auto& entry : entries_) {
                if (entry.id == id) {
                    return &entry;
                }
            }
            return nullptr;
        }

        std::vector<Entry> entries_{};
        std::uint32_t nextId_ = 0;
    };

} /* namespace Mashiro::Platform */