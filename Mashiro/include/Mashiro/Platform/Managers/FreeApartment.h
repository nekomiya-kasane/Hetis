/**
 * @file FreeApartment.h
 * @brief Free-threaded Manager skeletons (@c Display, @c Power, @c AudioDevice).
 *
 * Free-threaded Managers carry no per-thread state and expose pure-query
 * accessors only. EventPump does not Bookkeep through them and does not call
 * @c Start on them — they are listed in @c ManagerSet.h purely so the Apartment
 * partition is exhaustive and the static_assert in @c GetScheduleMode<M>() can
 * verify that every Manager has a @c [[=ScheduleMode]] annotation.
 */
#pragma once

#include "Mashiro/Platform/ThreadContract.h"

namespace Mashiro::Platform {

    /// @brief Display enumeration / DPI / refresh-rate query — atomic snapshot.
    struct [[=OnFreeThreaded]] Display {};

    /// @brief AC/battery power source — atomic snapshot.
    struct [[=OnFreeThreaded]] Power {};

    /// @brief Default audio render / capture endpoint — atomic snapshot.
    struct [[=OnFreeThreaded]] AudioDevice {};

} // namespace Mashiro::Platform
