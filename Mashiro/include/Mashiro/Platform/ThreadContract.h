/**
 * @file ThreadContract.h
 * @brief Compile-time scheduling annotations for Platform managers.
 *
 * Manager apartment placement is declared on the manager type with @c [[=ScheduleMode]] and read by
 * @ref Mashiro::Traits::GetScheduleMode during EventPump instantiation. The annotation is the scheduling fact;
 * event bookkeeping still uses structural @c On(const P&) @c noexcept overloads from SystemEvent.h.
 *
 * @ingroup Platform
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
            static_assert(annots.size() == 1,
                          "Manager must have exactly one ScheduleMode annotation");
            return std::meta::extract<Platform::ScheduleMode>(annots[0]).mode;
        }

    } // namespace Traits

} // namespace Mashiro
