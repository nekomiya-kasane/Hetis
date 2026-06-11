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

        enum class ScheduleMode : uint8_t {
            PlatformThread,  // Lives on Platform thread; mutators return OwnerTask<T>.
            DedicatedThread, // Owns its own thread; emits via the event inbox.
            FreeThreaded,    // Stateless or atomically-protected; callable anywhere.
        };

        struct ManagerScheduleMode {
            ScheduleMode mode = ScheduleMode::PlatformThread;
            constexpr bool operator==(const ManagerScheduleMode&) const = default;
        };

        inline constexpr ManagerScheduleMode OnPlatformThread{.mode = ScheduleMode::PlatformThread};
        inline constexpr ManagerScheduleMode OnDedicatedThread{.mode =
                                                                   ScheduleMode::DedicatedThread};
        inline constexpr ManagerScheduleMode OnFreeThreaded{.mode = ScheduleMode::FreeThreaded};

    } // namespace Platform

    namespace Traits {

        template<typename T>
        consteval auto GetScheduleMode() {
            constexpr auto annots = std::define_static_array(
                std::meta::annotations_of(^^T, ^^Platform::ManagerScheduleMode));
            static_assert(annots.size() <= 1,
                          "Manager must have at most one ManagerScheduleMode annotation");
            return std::meta::extract<Platform::ManagerScheduleMode>(annots[0]).mode;
        }

    } // namespace Traits

} // namespace Mashiro
