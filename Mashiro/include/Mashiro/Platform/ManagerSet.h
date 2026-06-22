/**
 * @file ManagerSet.h
 * @brief Single source of truth for the Platform-thread Manager pack.
 *
 * Every Manager that participates in platform input is listed *once* here. The
 * @ref Mashiro::Platform::EventPump template parameter pack is derived from
 * this tuple, so adding or removing a Manager is a one-line change to this
 * file — no other site needs to be touched.
 *
 * @par Apartment classification
 * Each Manager carries a @c [[=ScheduleMode]] annotation in its own header.
 * EventPump partitions the pack at compile time into three apartments:
 *   - Platform-thread: @c Window, @c Input, @c Ime, @c Clipboard, @c Cursor,
 *     @c DragDrop, @c Dialog, @c Surface, @c Appearance, @c Accessibility.
 *   - Dedicated-thread: @c Gamepad, @c FileWatch.
 *   - Free-threaded: @c Display, @c Power, @c AudioDevice.
 *
 * The classification lives in each Manager's own header, *not* here, because
 * the apartment is a property of the Manager, not of the set.
 *
 * @ingroup Platform
 */
#pragma once

// Manager headers, grouped by apartment. Each Manager carries its own
// [[=ScheduleMode]] annotation; EventPump partitions the pack at compile time.
// Bring-up phase keeps grouped headers per apartment for everything that has
// not yet grown out; @c Window has graduated to its own file (per the rule
// stated below) and is included individually.

#include "Mashiro/Platform/Managers/Window.h"

#include "Mashiro/Platform/Managers/DedicatedApartment.h"
#include "Mashiro/Platform/Managers/FreeApartment.h"
#include "Mashiro/Platform/Managers/PlatformApartment.h"

#include "Mashiro/Platform/EventPump.h"

namespace Mashiro::Platform {

    /// @brief The full Manager set, declared in one place.
    using PlatformPump = EventPump<
        Window, Input, Ime, Clipboard, Cursor, DragDrop, Dialog,
        Surface, Appearance, Accessibility,
        Gamepad, FileWatch,
        Display, Power, AudioDevice>;

} // namespace Mashiro::Platform
