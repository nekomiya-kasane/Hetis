/**
 * @file PlatformApartment.h
 * @brief Platform-thread-apartment Manager skeletons (Window split out).
 *
 * @c Window has graduated to its own header (@c Managers/Window.h) — it owns the
 * @c WindowId allocator, the HWND/xdg-surface registry, and the lifecycle book-
 * keeping that the rest of the Platform stack reads back through. The Managers
 * remaining in this file are the bring-up stubs that have not yet been wired up;
 * each one will move out into its own header on the same path the moment its
 * implementation grows past a few overloads (per the @c ManagerSet.h note).
 *
 * Each Manager declares:
 *   - the @c [[=ScheduleMode]] annotation that puts it in the Platform apartment;
 *   - zero or more @c On(const P&) overloads — the canonical bookkeep convention
 *     established in @c SystemEvent.h. EventPump's compile-time table picks them
 *     up via @c Mashiro::Traits::Event::HandlesBookkeep without any registry,
 *     enum, or annotation on the overload itself.
 */
#pragma once

#include "Mashiro/Platform/Managers/Window.h"
#include "Mashiro/Platform/SystemEvent.h"
#include "Mashiro/Platform/ThreadContract.h"

namespace Mashiro::Platform {

    /// @brief Aggregates keyboard / mouse / pen / touch state per window.
    ///
    /// Bookkeep contract is empty until the v1 Win32 translator wires actual
    /// payloads through. The apartment annotation is what EventPump cares about
    /// — once @c On overloads land here they are picked up automatically.
    struct [[=OnPlatformThread]] Input {};

    /// @brief Composition window, candidate list, IME-on/off state.
    struct [[=OnPlatformThread]] Ime {};

    /// @brief Owns the system clipboard mirror and clipboard-format negotiations.
    struct [[=OnPlatformThread]] Clipboard {};

    /// @brief Cursor visibility / shape / capture state per window.
    struct [[=OnPlatformThread]] Cursor {};

    /// @brief In-flight drag state, accept/reject feedback, drop targets.
    struct [[=OnPlatformThread]] DragDrop {};

    /// @brief Native modal dialog lifecycle (file open / save / message box).
    struct [[=OnPlatformThread]] Dialog {};

    /// @brief Surface (swapchain handle / DPI / colour space) per window.
    ///
    /// Surface tracks DPI/extent on a per-window basis and reuses @c Window's
    /// registry indirectly via the broadcast — it does not need its own HWND
    /// table. The resize handler is a stub until the swapchain bridge lands.
    struct [[=OnPlatformThread]] Surface {
        void On(const Event::WindowResizeEvent&) noexcept {}
    };

    /// @brief Theme / accent colour / reduced-motion state.
    struct [[=OnPlatformThread]] Appearance {};

    /// @brief Screen-reader / high-contrast / a11y-tree liaison.
    struct [[=OnPlatformThread]] Accessibility {};

} // namespace Mashiro::Platform
