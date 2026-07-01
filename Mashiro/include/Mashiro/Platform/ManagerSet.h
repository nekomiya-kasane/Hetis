/**
 * @file ManagerSet.h
 * @brief Single source of truth for the Platform-thread manager pack.
 *
 * Every manager that participates in platform input is listed exactly once here. EventPump derives its template
 * parameter pack from this alias, then partitions managers at compile time from each type's @c [[=ScheduleMode]]
 * annotation.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Platform/Managers/WindowManager.h"

#include "Mashiro/Platform/Managers/DedicatedApartment.h"
#include "Mashiro/Platform/Managers/FreeApartment.h"
#include "Mashiro/Platform/Managers/PlatformApartment.h"

#include "Mashiro/Platform/EventPump.h"

namespace Mashiro::Platform {

    /** @brief Platform-thread event pump instantiated with the complete manager set. */
    using PlatformPump = EventPump<
        WindowManager, Input, Ime, Clipboard, Cursor, DragDrop, Dialog,
        Surface, Appearance, Accessibility,
        Gamepad, FileWatch,
        Display, Power, AudioDevice>;

} /* namespace Mashiro::Platform */