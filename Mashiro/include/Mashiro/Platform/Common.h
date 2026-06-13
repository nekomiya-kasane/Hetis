// ReSharper disable CppClangTidyPerformanceEnumSize
#pragma once

#include <cstdint>

namespace Mashiro {

    // ============================================================================
    // Platform Capability Set
    // ============================================================================

    /**
     * @brief Bit set identifying which OS / windowing backends a payload is
     *        emitted on.
     *
     * Several events are only meaningful on a specific OS or display server
     * (e.g. Wayland fractional-scale change, Win32 IME candidate window,
     * macOS swipe-back gesture). Each payload struct carries a
     * @ref Mashiro::Platform::Anno::OnPlatform annotation declaring its
     * support set; @ref Mashiro::Traits::AvailableOn lifts that into a
     * compile-time predicate so backends can statically prune unsupported
     * payloads from their dispatch tables.
     *
     * The enum is a bitfield (`BitfieldEnum`) so callers can express e.g.
     * `Linux_X11 | Linux_Wayland` for "any Linux backend".
     */
    enum class PlatformBit : uint16_t {
        None = 0,
        Windows = 1u << 0,       ///< Win32 / DWM.
        Linux_X11 = 1u << 1,     ///< X11 / xcb backend.
        Linux_Wayland = 1u << 2, ///< wlroots / GNOME / KDE Wayland compositor.
        macOS = 1u << 3,         ///< AppKit / Cocoa.
        Android = 1u << 4,       ///< Reserved.
        iOS = 1u << 5,           ///< Reserved.
    };
    static_assert(Traits::BitfieldEnum<PlatformBit>);

    /// @name Composite platform-bit aliases (sets of @ref PlatformBit).
    /// @{
    inline constexpr PlatformBit PlatformBit_Linux =
        PlatformBit::Linux_X11 | PlatformBit::Linux_Wayland;
    inline constexpr PlatformBit PlatformBit_Desktop =
        PlatformBit::Windows | PlatformBit_Linux | PlatformBit::macOS;
    inline constexpr PlatformBit PlatformBit_All = Traits::kBitfieldMask<PlatformBit>;
    /// @}

    namespace Platform {

        /**
         * @brief Annotation marking a payload's supported platform set.
         *
         * Defaults to @ref kPlatformBit_All — i.e. portable. Specialise per
         * payload when the event only fires on a subset:
         * @code
         * struct [[=OnPlatform{PlatformBit::Windows}]] WindowDwmCompositionEvent { ... };
         * @endcode
         */
        struct OnPlatform {
            PlatformBit set = PlatformBit_All;
            constexpr bool operator==(const OnPlatform&) const = default;
        };

        /// @name Convenience platform-set markers
        /// @{
        inline constexpr OnPlatform AllPlatforms{PlatformBit_All};
        inline constexpr OnPlatform DesktopOnly{PlatformBit_Desktop};
        inline constexpr OnPlatform WindowsOnly{PlatformBit::Windows};
        inline constexpr OnPlatform LinuxOnly{PlatformBit_Linux};
        inline constexpr OnPlatform X11Only{PlatformBit::Linux_X11};
        inline constexpr OnPlatform WaylandOnly{PlatformBit::Linux_Wayland};
        inline constexpr OnPlatform MacOnly{PlatformBit::macOS};
        /// @}

    } // namespace Platform

    /// @brief Stable identifier for a window owned by the Platform thread.
    ///        Issued by `WindowManager` on creation and valid until the matching
    ///        `WindowDestroy` event is broadcast.
    enum class WindowId : uint32_t { Invalid = 0 };

    /// @brief Identifier for a connected input device (keyboard, mouse, pen, gamepad).
    enum class DeviceId : uint32_t { Invalid = 0 };

    /// @brief Identifier for a connected display / monitor.
    enum class DisplayId : uint32_t { Invalid = 0 };

    /// @brief Identifier for a touch contact point, valid for the lifetime of the contact.
    enum class TouchId : uint32_t { Invalid = 0 };

} // namespace Mashiro
