/**
 * @brief Annotations for the Platform thread.
 *
 * ```
 * ┌───────────────────────────────────────────────────────────────────────┐
 * │                        Platform Thread                                │
 * │                                                                       │
 * │  ┌──────────────┐        ┌────────────────────────┐    SPSC           │
 * │  │ Win32 Pump   │───────▶│ Unified Event Writer   │───────────▶ Client A
 * │  │ (PeekMessage)│        │ (sole producer for all │    SPSC           │
 * │  └──────────────┘        │  EventChannels)        │───────────▶ Client B
 * │        ▲                 └────────────────────────┘                   │
 * │        │ wake event             ▲                                     │
 * │        │                        │ drain                               │
 * │  ┌──────────────┐        ┌──────────────┐                             │
 * │  │ MPSC Event   │◀───────│ Dedicated    │                             │
 * │  │ Inbox        │ submit │ thread mgrs  │                             │
 * │  └──────────────┘        │ (Gamepad,    │                             │
 * │        │                 │  FileWatch)  │                             │
 * │        ▼                 └──────────────┘                             │
 * │  ┌────────────────────────────────────────────┐                       │
 * │  │ Managers (state owners on platform thread) │                       │
 * │  │  Window, Input, Ime, Clipboard, Cursor,    │◀── OwnerTask<T> ──────┤
 * │  │  DragDrop, Dialog, Surface, Appearance,    │   (cross-thread,      │
 * │  │  Accessibility                             │    co_await)          │
 * │  └────────────────────────────────────────────┘                       │
 * │        ▲                                                              │
 * │  ┌──────────────┐                                                     │
 * │  │ MPSC         │◀────── coroutine handles from any worker thread     │
 * │  │ OwnerExecutor│                                                     │
 * │  └──────────────┘                                                     │
 * └───────────────────────────────────────────────────────────────────────┘
 *
 * Free-threaded managers (any thread): Display, Power, AudioDevice
 * Free functions (not a Manager): `Mashiro::Platform::Time::*` for QPC, timer resolution, waitable
 * timers.
 * ```
 */

#pragma once

#include <cstdint>
#include <meta>

namespace Mashiro {

    namespace Platform {

        enum class ThreadDomain : uint8_t {
            Platform, // Must be executed on the Platform thread
            Any,      // Free-threaded; no transfer needed.
        };

        struct ThreadContract {
            ThreadDomain domain = ThreadDomain::Platform;
            constexpr bool operator==(const ThreadContract&) const = default;
        };

        inline constexpr ThreadContract PlatformOnly{.domain = ThreadDomain::Platform};
        inline constexpr ThreadContract AnyThread{.domain = ThreadDomain::Any};

        enum class ScheduleDomain : uint8_t {
            PlatformThread,  // Lives on Platform thread; mutators return OwnerTask<T>.
            DedicatedThread, // Owns its own thread; emits via the event inbox.
            FreeThreaded,    // Stateless or atomically-protected; callable anywhere.
        };

        struct ScheduleMode {
            ScheduleDomain mode = ScheduleDomain::PlatformThread;
            constexpr bool operator==(const ScheduleMode&) const = default;
        };

        inline constexpr ScheduleMode OnPlatformThread{.mode = ScheduleDomain::PlatformThread};
        inline constexpr ScheduleMode OnDedicatedThread{.mode = ScheduleDomain::DedicatedThread};
        inline constexpr ScheduleMode OnFreeThreaded{.mode = ScheduleDomain::FreeThreaded};

    } // namespace Platform

    namespace Traits {

        template<typename T>
        consteval auto GetScheduleMode() {
            constexpr auto annots =
                std::define_static_array(std::meta::annotations_of(^^T, ^^Platform::ScheduleMode));
            static_assert(annots.size() <= 1,
                          "Manager must have at most one ScheduleMode annotation");
            return std::meta::extract<Platform::ScheduleMode>(annots[0]).mode;
        }

    } // namespace Traits

} // namespace Mashiro
