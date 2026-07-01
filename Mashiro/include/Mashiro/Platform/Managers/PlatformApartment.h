/**
 * @file PlatformApartment.h
 * @brief Platform-thread manager declarations that have not yet grown into dedicated headers.
 *
 * WindowManager already owns its own header because it carries identity, native-handle lookup, and lifecycle
 * bookkeeping. The remaining managers are lightweight bring-up declarations; each manager moves to its own header
 * when it gains real state or more than trivial bookkeep overloads.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Platform/Managers/WindowManager.h"
#include "Mashiro/Platform/SystemEvent.h"
#include "Mashiro/Platform/ThreadContract.h"

namespace Mashiro::Platform {

    /**
     * @brief Aggregates keyboard, mouse, pen, and touch state per window.
     *
     * The apartment annotation is the scheduling fact EventPump needs; matching @c On overloads are discovered
     * structurally by @c Traits::Event::HandlesBookkeep.
     */
    struct [[=OnPlatformThread]] Input {};

    /** @brief Composition window, candidate list, and IME activation state. */
    struct [[=OnPlatformThread]] Ime {};

    /** @brief System clipboard mirror and clipboard-format negotiations. */
    struct [[=OnPlatformThread]] Clipboard {};

    /** @brief Cursor visibility, shape, and capture state per window. */
    struct [[=OnPlatformThread]] Cursor {};

    /** @brief Drag session state, accept/reject feedback, and drop targets. */
    struct [[=OnPlatformThread]] DragDrop {};

    /** @brief Native modal dialog lifecycle, including file-open, save, and message boxes. */
    struct [[=OnPlatformThread]] Dialog {};

    /**
     * @brief Surface state per window.
     *
     * Surface consumes window lifecycle facts through EventPump broadcast and does not need a separate native-handle
     * table. The resize hook is a stub until the swapchain bridge lands.
     */
    struct [[=OnPlatformThread]] Surface {
        void On(const Event::WindowResizeEvent&) noexcept {}
    };

    /** @brief Theme, accent colour, and reduced-motion state. */
    struct [[=OnPlatformThread]] Appearance {};

    /** @brief Screen-reader, high-contrast, and accessibility-tree liaison. */
    struct [[=OnPlatformThread]] Accessibility {};

} /* namespace Mashiro::Platform */