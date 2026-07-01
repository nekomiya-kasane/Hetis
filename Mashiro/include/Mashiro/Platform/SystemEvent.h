/**
 * @file SystemEvent.h
 * @brief Platform-neutral system-event payload vocabulary.
 *
 * SystemEvent is the value layer below Platform::EventPump. It names facts already translated out of platform-native
 * messages, but it does not own scheduling, wakeup, coroutine, or subscriber semantics. The payload type is the
 * discriminator; shared properties such as window affinity and capture time are expressed by mixin bases and concepts.
 *
 * Variant materialisation is compile-time: reflection scans @ref Mashiro::Event, selects leaf classes deriving from
 * @ref Mashiro::Event::EventPayloadBase, and splices them into @c std::variant. Platform availability uses strong
 * annotations read at compile time; manager bookkeeping is a structural @c noexcept protocol.
 *
 * @ingroup Platform
 */

#pragma once

#include "Mashiro/Core/Flags.h"
#include "Mashiro/Core/TypeTraits.h"
#include "Mashiro/Math/Types.h"
#include "Mashiro/Geom/Geom.h"
#include "Mashiro/Platform/Common.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <variant>

namespace Mashiro {

    inline namespace Event {

        /** @name Payload Foundations @{ */

        /**
         * @brief Empty marker base for every system-event payload.
         *
         * Inherited (directly or via @ref WindowSpecificEvent /
         * @ref TimestampedEvent) by every struct that participates in
         * @ref Mashiro::SystemEvent. Used by the variant materialiser
         * to discover payload types via reflection without an annotation.
         *
         * The defaulted @c operator== exists so derived payloads that default
         * their own @c operator== are not implicitly deleted by the absence of
         * a base comparator (the marker contributes zero bytes, so the
         * comparison is genuinely vacuous).
         */
        struct EventPayloadBase {
            constexpr bool operator==(const EventPayloadBase&) const = default;
        };

        /**
         * @brief Mixin: payload targets a specific window.
         *
         * Inherited by every event whose meaning is window-scoped: lifecycle,
         * input, IME, drag-drop, and focus. Events that are global to the
         * application (display, power, gamepad, file watcher, clipboard) do not
         * inherit this; there is no @c windowId sentinel for them.
         */
        struct WindowSpecificEvent : EventPayloadBase {
            WindowId windowId = WindowId::Invalid; /**< Window the event targets. */
            constexpr bool operator==(const WindowSpecificEvent&) const = default;
        };

        /**
         * @brief Mixin: payload carries a high-resolution capture timestamp.
         *
         * Inherited by events whose downstream consumers need sub-millisecond
         * timing: input latency measurement, IME composition timeouts, gesture
         * velocity, and timer ticks. Events at the seconds / minutes timescale
         * (theme change, display reconfiguration, power state) do not carry it.
         *
         * Stamped by the producer immediately before broadcast; the value is a
         * monotonic steady clock reading expressed in nanoseconds (typically QPC
         * on Windows, @c clock_gettime(CLOCK_MONOTONIC) on Linux).
         */
        struct TimestampedEvent : EventPayloadBase {
            uint64_t timestamp = 0; /**< Monotonic steady-clock reading, ns. */
            constexpr bool operator==(const TimestampedEvent&) const = default;
        };

        /** @} */

        /** @name Shared Event Vocabulary @{ */

        /**
         * @brief Touch / pen contact phase.
         */
        enum class TouchPhase : uint8_t {
            Began,
            Moved,
            Stationary,
            Ended,
            Cancelled,
        };
        static_assert(Traits::SequentialEnum<TouchPhase>);

        /**
         * @brief Mouse-wheel / trackpad scroll units.
         */
        enum class ScrollUnit : uint8_t {
            Lines, /**< Discrete notches (most desktop mice). */
            Pixels, /**< High-resolution / smooth scroll. */
            Pages, /**< Page-up / page-down equivalent. */
        };
        static_assert(Traits::SequentialEnum<ScrollUnit>);

        /**
         * @brief Pen / stylus button state. Combinable bit flags.
         */
        enum class PenButton : uint8_t {
            None    = 0,
            Tip     = 1u << 0, /**< Pen is in contact with the surface. */
            Barrel  = 1u << 1, /**< Side button. */
            Eraser  = 1u << 2, /**< Inverted (eraser-end) contact. */
        };
        static_assert(Traits::BitfieldEnum<PenButton>);

        /**
         * @brief OS-level appearance theme.
         */
        enum class AppearanceTheme : uint8_t {
            Unknown,
            Light,
            Dark,
            HighContrastLight,
            HighContrastDark,
        };
        static_assert(Traits::SequentialEnum<AppearanceTheme>);

        /**
         * @brief AC / battery power-source state.
         */
        enum class PowerSource : uint8_t {
            Unknown,
            AC,
            Battery,
            UPS,
        };
        static_assert(Traits::SequentialEnum<PowerSource>);

        /**
         * @brief File-system change classification (per-event detail beyond
         *        the @ref FileCreatedEvent / @ref FileDeletedEvent variant
         *        membership).
         */
        enum class FileChangeFlags : uint16_t {
            None        = 0,
            Content     = 1u << 0,
            Size        = 1u << 1,
            Attributes  = 1u << 2,
            Permissions = 1u << 3,
            Owner       = 1u << 4,
        };
        static_assert(Traits::BitfieldEnum<FileChangeFlags>);

        /** @brief Drag-source MIME-type fingerprint mask. */
        enum class DragKind : uint16_t {
            None    = 0,
            Files   = 1u << 0,
            Text    = 1u << 1,
            Image   = 1u << 2,
            Url     = 1u << 3,
            Custom  = 1u << 4,
        };
        static_assert(Traits::BitfieldEnum<DragKind>);


        /** @} */

        /** @name Event Payloads @{ */

        inline namespace Wind {

            /** @brief Window has been created and is ready for first draw. */
            struct WindowCreateEvent : WindowSpecificEvent {
                ivec2 size{}; /**< Initial client size in pixels. */
                float dpiScale = 1; /**< Initial DPI scale factor. */
            };

            /** @brief OS requested the window to close (X / Cmd-Q / Alt-F4). */
            struct WindowCloseEvent : WindowSpecificEvent {};

            /** @brief Window has been destroyed; this is the last event for @c windowId. */
            struct WindowDestroyEvent : WindowSpecificEvent {};

            /** @brief Client area has been resized. */
            struct WindowResizeEvent : WindowSpecificEvent, TimestampedEvent {
                ivec2 size{};
                bool  isMinimised = false;
            };

            /** @brief Window has been moved on the desktop. */
            struct WindowMoveEvent : WindowSpecificEvent {
                ivec2 position{};
            };

            /** @brief Window gained or lost keyboard focus. */
            struct WindowFocusEvent : WindowSpecificEvent {
                bool focused = false;
            };

            /** @brief Window has been iconified / minimized, maximized, or restored. */
            struct WindowMinimizeEvent : WindowSpecificEvent {};
            struct WindowMaximizeEvent : WindowSpecificEvent {};
            struct WindowRestoreEvent : WindowSpecificEvent {};

            /** @brief Window visibility has toggled via @c ShowWindow or @c xdg_toplevel.configure. */
            struct WindowVisibilityChangeEvent : WindowSpecificEvent {
                bool visible = false;
            };

            struct WindowEnterFullscreenEvent : WindowSpecificEvent {
                DisplayId display = DisplayId::Invalid;
            };
            struct WindowLeaveFullscreenEvent : WindowSpecificEvent {};

            /** @brief DPI / scale of the window's monitor changed (per-monitor DPI on Win10+). */
            struct WindowDpiChangeEvent : WindowSpecificEvent {
                float oldScale = 1;
                float newScale = 1;
            };

            /** @brief Wayland fractional-scale factor change. */
            struct [[=Platform::WaylandOnly]] WindowScaleChangeEvent : WindowSpecificEvent {
                float scale = 1; /**< Multiples of 120 / 120, surfaced as float. */
            };

            /** @brief OS theme applied to the window (decorations, system menu). */
            struct WindowThemeChangeEvent : WindowSpecificEvent {
                AppearanceTheme theme = AppearanceTheme::Unknown;
            };

            /** @brief Window occluded / unoccluded (macOS visibility, Win32 cloak / DWM). */
            struct WindowOcclusionChangeEvent : WindowSpecificEvent {
                bool occluded = false;
            };

            /** @brief X11 @c Expose / Wayland frame damage; region must be redrawn. */
            struct [[=Platform::LinuxOnly]] WindowExposedEvent : WindowSpecificEvent {
                ivec2 origin{};
                ivec2 extent{};
            };

            /** @brief Win32 DWM composition state toggled by @c WM_DWMCOMPOSITIONCHANGED. */
            struct [[=Platform::WindowsOnly]] WindowDwmCompositionChangeEvent : WindowSpecificEvent {
                bool compositionEnabled = false;
            };

        } /* namespace Wind */

        /**
         * @brief Logical key identifier.
         *
         * Letter and digit values intentionally match their ASCII code points so a
         * @c KeyCode can round-trip with printable input where applicable. All other
         * keys occupy a private range starting at 256.
         */
        enum class KeyCode : uint32_t {
            Unknown = 0,

            /* Letters (ASCII uppercase) */
            A = 'A', B = 'B', C = 'C', D = 'D', E = 'E', F = 'F', G = 'G', H = 'H', I = 'I', J = 'J', K = 'K',
            L = 'L', M = 'M', N = 'N', O = 'O', P = 'P', Q = 'Q', R = 'R', S = 'S', T = 'T', U = 'U', V = 'V',
            W = 'W', X = 'X', Y = 'Y', Z = 'Z',

            /* Numbers (ASCII digits) */
            Num0 = '0', Num1 = '1', Num2 = '2', Num3 = '3', Num4 = '4', Num5 = '5', Num6 = '6', Num7 = '7',
            Num8 = '8', Num9 = '9',

            /* Function keys */
            F1 = 256, F2, F3, F4, F5, F6, F7, F8,
            F9, F10, F11, F12, F13, F14, F15, F16,
            F17, F18, F19, F20, F21, F22, F23, F24,

            /* Navigation / modifiers / whitespace */
            Escape, Tab, CapsLock, Shift, Control, Alt, Super, Space, Enter, Backspace, Delete, Insert, Home,
            End, PageUp, PageDown, Left, Right, Up, Down,

            /* Punctuation */
            Comma, Period, Slash, Semicolon, Quote, BracketLeft, BracketRight, Backslash, Minus, Equal, Grave,
        };

        /**
         * @brief Bit-packed modifier-key state accompanying an input event.
         *
         * Bitfield layout keeps the struct at one byte while remaining
         * structured-binding friendly. Use the static factory methods to express
         * common combinations in a readable way.
         */
        struct Modifiers {
            bool shift : 1 = false;
            bool ctrl : 1 = false;
            bool alt : 1 = false;
            bool super : 1 = false;

            /** @return @c true iff no modifier bits are set. */
            [[nodiscard]] constexpr bool IsNone() const { return !shift && !ctrl && !alt && !super; }

            /** @name Common modifier combinations @{ */
            static constexpr Modifiers None() { return {}; }
            static constexpr Modifiers Ctrl() {
                return {.shift = false, .ctrl = true, .alt = false, .super = false};
            }
            static constexpr Modifiers Alt() {
                return {.shift = false, .ctrl = false, .alt = true, .super = false};
            }
            static constexpr Modifiers Shift() {
                return {.shift = true, .ctrl = false, .alt = false, .super = false};
            }
            static constexpr Modifiers Super() {
                return {.shift = false, .ctrl = false, .alt = false, .super = true};
            }
            static constexpr Modifiers CtrlShift() {
                return {.shift = true, .ctrl = true, .alt = false, .super = false};
            }
            static constexpr Modifiers CtrlAlt() {
                return {.shift = false, .ctrl = true, .alt = true, .super = false};
            }
            static constexpr Modifiers CtrlAltShift() {
                return {.shift = true, .ctrl = true, .alt = true, .super = false};
            }
            /** @} */
        };

        /** @brief Physical scancode that produced this key event (USB HID Usage ID, OS native). */
        enum class Scancode : uint32_t { Unknown = 0 };

        inline namespace Keyboard {

            /**
             * @brief Common payload skeleton for both key-down and key-up.
             *
             * Inherits @ref WindowSpecificEvent and @ref TimestampedEvent so the
             * derived @c KeyDownEvent / @c KeyUpEvent types pick up @c windowId,
             * @c timestamp, and the @ref EventPayloadBase marker
             * automatically. The payload type itself is the discriminator on
             * @ref Mashiro::SystemEvent; there is no separate enum tag.
             */
            struct KeyEventBase : WindowSpecificEvent, TimestampedEvent {
                KeyCode   code     = KeyCode::Unknown; /**< Logical key. */
                Scancode  scancode = Scancode::Unknown; /**< Physical key (layout-independent). */
                Modifiers mods{}; /**< Modifier-key state at event time. */
                DeviceId  device   = DeviceId::Invalid; /**< Originating keyboard. */
                bool      repeat   = false; /**< OS auto-repeat (key-down only). */
            };

            struct KeyDownEvent : KeyEventBase {};
            struct KeyUpEvent : KeyEventBase {};

            /** @brief Translated text input from a single keystroke. */
            struct CharEvent : WindowSpecificEvent {
                std::array<char32_t, 4> codepoints{}; /**< UTF-32, zero-terminated when shorter than 4. */
                uint8_t   count = 0; /**< Number of valid entries in @c codepoints. */
                Modifiers mods{};
            };

            /** @brief Active keyboard layout / input source changed. */
            struct KeyboardLayoutChangeEvent : WindowSpecificEvent {
                std::string layoutId; /**< OS identifier (e.g. "en-US", "ja-JP-IME"). */
            };

        } /* namespace Keyboard */

        /**
         * @brief Logical mouse button identifier.
         *
         * Values are dense (0..4) so they can index small per-button arrays
         * directly without a translation table.
         */
        enum class MouseButton : uint8_t {
            Left = 0,
            Right = 1,
            Middle = 2,
            Button4 = 3, /**< Typically "back" on five-button mice. */
            Button5 = 4, /**< Typically "forward" on five-button mice. */
        };

        inline namespace Mouse {

            struct MouseEnterEvent : WindowSpecificEvent {};
            struct MouseLeaveEvent : WindowSpecificEvent {};

            /**
             * @brief Pointer moved within the window's client area.
             */
            struct MouseMoveEvent : WindowSpecificEvent, TimestampedEvent {
                vec2     position{}; /**< Client-space, sub-pixel precision. */
                vec2     delta{}; /**< Movement since previous event. */
                Modifiers mods{};
                DeviceId device = DeviceId::Invalid;
            };

            /**
             * @brief Pointer button pressed or released.
             */
            struct MouseButtonEvent : WindowSpecificEvent, TimestampedEvent {
                vec2        position{};
                MouseButton button = MouseButton::Left;
                bool        pressed = false;
                uint8_t     clickCount = 1; /**< 1=single, 2=double, 3=triple, etc. */
                Modifiers   mods{};
                DeviceId    device = DeviceId::Invalid;
            };

            /** @brief Mouse wheel / trackpad scroll. */
            struct ScrollEvent : WindowSpecificEvent, TimestampedEvent {
                vec2       position{};
                vec2       delta{}; /**< +Y = scroll up, +X = scroll right. */
                ScrollUnit unit = ScrollUnit::Lines;
                bool       inertial = false; /**< macOS / precision touchpad inertial phase. */
                Modifiers  mods{};
                DeviceId   device = DeviceId::Invalid;
            };

            /** @brief Raw, unaccelerated relative mouse motion from @c WM_INPUT, xinput2, or libinput. */
            struct RawMouseMotionEvent : WindowSpecificEvent, TimestampedEvent {
                ivec2    delta{};
                DeviceId device = DeviceId::Invalid;
            };

        } /* namespace Mouse */

        inline namespace Touch {

            /** @brief Single-finger touch contact event. */
            struct TouchEvent : WindowSpecificEvent, TimestampedEvent {
                TouchId    id = TouchId::Invalid;
                TouchPhase phase = TouchPhase::Began;
                vec2       position{};
                vec2       size{}; /**< Contact ellipse size in pixels (0 if unsupported). */
                float      pressure = 0; /**< Normalised [0,1]. */
            };

            /** @brief OS-synthesised gesture from the trackpad / touch driver. */
            struct TouchGestureEvent : WindowSpecificEvent, TimestampedEvent {
                enum class Kind : uint8_t { Pinch, Rotate, Swipe, Pan, Tap, LongPress };
                Kind  gesture  = Kind::Pinch; /**< Sub-tag distinguishing the gesture kind. */
                vec2  position{};
                vec2  delta; /**< Pan/swipe direction in pixels. */
                float scale = 1; /**< Pinch scale factor. */
                float rotation = 0; /**< Rotation in radians. */
            };

        } /* namespace Touch */

        inline namespace Pen {

            /** @brief Stylus / pen contact event with full Wintab/Pointer-Input metadata. */
            struct PenEvent : WindowSpecificEvent, TimestampedEvent {
                TouchPhase phase = TouchPhase::Began;
                vec2       position{};
                float      pressure = 0; /**< Normalised [0,1]. */
                float      tiltX = 0; /**< Radians, +X axis tilt. */
                float      tiltY = 0; /**< Radians, +Y axis tilt. */
                float      twist = 0; /**< Barrel rotation in radians. */
                float      tangentialPressure = 0;
                DeviceId   device = DeviceId::Invalid;
            };

            /** @brief Pen entered or left the digitiser hover range. */
            struct PenProximityEvent : WindowSpecificEvent, TimestampedEvent {
                bool     entering = true;
                DeviceId device = DeviceId::Invalid;
            };

            /** @brief Pen barrel button / eraser-tip state changed. */
            struct PenButtonEvent : WindowSpecificEvent, TimestampedEvent {
                PenButton buttons = PenButton::None;
                bool      pressed = false;
                DeviceId  device = DeviceId::Invalid;
            };

        } /* namespace Pen */

        inline namespace IME {

            struct ImeActivateEvent : WindowSpecificEvent {};
            struct ImeDeactivateEvent : WindowSpecificEvent {};

            /** @brief IME composition started. */
            struct ImeCompositionEvent : WindowSpecificEvent, TimestampedEvent {
                std::string preedit; /**< UTF-8 preedit / composition string. */
                uint32_t    caret = 0;
            };

            /** @brief IME composition contents changed. */
            struct ImeCompositionUpdateEvent : WindowSpecificEvent, TimestampedEvent {
                std::string preedit;
                uint32_t    caret = 0;
                uint32_t    selectionStart = 0;
                uint32_t    selectionEnd = 0;
            };

            /** @brief IME committed a final string. */
            struct ImeCommitEvent : WindowSpecificEvent, TimestampedEvent {
                std::string text; /**< UTF-8 finalised string. */
            };

            /** @brief IME candidate-window contents updated. */
            struct ImeCandidateListEvent : WindowSpecificEvent {
                std::vector<std::string> candidates; /**< UTF-8 candidates. */
                uint32_t selected = 0; /**< Currently-highlighted index. */
                uint32_t pageStart = 0;
                uint32_t pageSize = 0;
            };

            /** @brief Where the host should anchor the candidate window (in client space). */
            struct ImePreeditCursorEvent : WindowSpecificEvent {
                ivec2 caretOrigin{}; /**< Client-space pixel origin. */
                uvec2 caretSize{};
            };

        } /* namespace IME */

        inline namespace Clipboard {

            /**
             * @brief OS clipboard contents changed.
             *
             * Clipboard is a process-global view of system state, so this payload carries no @c windowId even when a
             * platform notifies a hidden listener window.
             */
            struct ClipboardUpdateEvent : EventPayloadBase {
                DragKind kinds = DragKind::None; /**< Bitmask of available MIME categories. */
            };

            /** @brief X11 PRIMARY / Wayland selection changed (separate from clipboard). */
            struct [[=Platform::LinuxOnly]] SelectionUpdateEvent : WindowSpecificEvent {
                DragKind kinds = DragKind::None;
            };

        } /* namespace Clipboard */

        inline namespace DnD {

            struct DragPayloadInfo {
                DragKind                 kinds = DragKind::None;
                std::vector<std::string> mimeTypes;
                size_t                   itemCount = 0;
            };

            struct DragEnterEvent : WindowSpecificEvent {
                vec2            position{};
                DragPayloadInfo payload;
            };

            struct DragOverEvent : WindowSpecificEvent {
                vec2 position{};
            };

            struct DragDropEvent : WindowSpecificEvent {
                vec2                      position{};
                std::vector<std::string>  paths; /**< For @c Files payloads, decoded local paths. */
                std::string               text; /**< For @c Text payloads. */
                std::vector<std::byte>    blob; /**< For @c Custom / @c Image payloads, raw bytes. */
                DragKind                  drag = DragKind::None; /**< MIME-category fingerprint. */
            };

            struct DragLeaveEvent : WindowSpecificEvent {};

        } /* namespace DnD */

        inline namespace Display {

            struct DisplayConnectEvent : EventPayloadBase {
                DisplayId   display = DisplayId::Invalid;
                std::string name;
            };

            struct DisplayDisconnectEvent : EventPayloadBase {
                DisplayId display = DisplayId::Invalid;
            };

            /** @brief A display's mode, position, or topology changed. */
            struct DisplayChangeEvent : EventPayloadBase {
                DisplayId display = DisplayId::Invalid;
                uvec2     resolution{};
                float     refreshHz = 0;
                float     dpiScale = 1;
            };

            /** @brief ICC / colour profile change (typically per-display on macOS / Win10+). */
            struct DisplayColorProfileChangeEvent : EventPayloadBase {
                DisplayId   display = DisplayId::Invalid;
                std::string profileName;
            };

            /** @brief HDR enable / disable / metadata change. */
            struct DisplayHdrChangeEvent : EventPayloadBase {
                DisplayId display = DisplayId::Invalid;
                bool      hdrEnabled = false;
                float     maxLuminanceNits = 0;
            };

        } /* namespace Display */

        inline namespace Power {

            struct PowerStateChangeEvent : EventPayloadBase {
                PowerSource source = PowerSource::Unknown;
                float       batteryLevel = 1; /**< Normalised [0,1]; 1 if non-battery. */
            };

            struct PowerSuspendEvent : EventPayloadBase {};
            struct PowerResumeEvent  : EventPayloadBase {};

            struct BatteryLevelChangeEvent : EventPayloadBase {
                float    level = 1;
                bool     charging = false;
                uint32_t secondsRemaining = 0; /**< 0 if unknown. */
            };

        } /* namespace Power */

        inline namespace Session {

            struct SessionLockEvent   : EventPayloadBase {};
            struct SessionUnlockEvent : EventPayloadBase {};

            /** @brief Win32 fast user switch. */
            struct [[=Platform::WindowsOnly]] SessionUserSwitchEvent : EventPayloadBase {};

            /** @brief Logoff or shutdown imminent; last chance to persist work. */
            struct SessionEndQueryEvent : EventPayloadBase {
                bool isShutdown = false;
                bool isCritical = false;
            };

        } /* namespace Session */

        inline namespace Theme {

            struct AppearanceChangeEvent : EventPayloadBase {
                AppearanceTheme theme = AppearanceTheme::Unknown;
                uint32_t        accentColorRgba = 0;
            };

        } /* namespace Theme */

        inline namespace A13y {

            struct AccessibilityScreenReaderEvent : EventPayloadBase {
                bool enabled = false;
            };
            struct AccessibilityReducedMotionEvent : EventPayloadBase {
                bool enabled = false;
            };
            struct AccessibilityHighContrastEvent : EventPayloadBase {
                bool enabled = false;
            };

        } /* namespace A13y */

        inline namespace Gamepad {

            struct GamepadConnectEvent : EventPayloadBase {
                DeviceId    device = DeviceId::Invalid;
                std::string name;
                uint32_t    vendorId = 0;
                uint32_t    productId = 0;
            };

            struct GamepadDisconnectEvent : EventPayloadBase {
                DeviceId device = DeviceId::Invalid;
            };

            /** @brief Snapshot of a gamepad's full state. */
            struct GamepadStateEvent : TimestampedEvent {
                DeviceId             device = DeviceId::Invalid;
                std::array<float, 6> axes{}; /**< LX, LY, RX, RY, LT, RT (normalised). */
                uint32_t             buttons = 0; /**< Bit mask, layout = vendor-defined map. */
            };

            struct GamepadBatteryChangeEvent : EventPayloadBase {
                DeviceId device = DeviceId::Invalid;
                float    level = 1;
                bool     charging = false;
            };

        } /* namespace Gamepad */

        inline namespace File {

            struct FileSpecificEvent : EventPayloadBase {
                std::string path;
            };

            struct FileCreatedEvent : FileSpecificEvent {};
            struct FileDeletedEvent : FileSpecificEvent {};

            struct FileModifiedEvent : FileSpecificEvent {
                FileChangeFlags changes = FileChangeFlags::None;
            };

            struct FileRenamedEvent : FileSpecificEvent {
                std::string newPath;
            };

            struct FileAttributesChangedEvent : FileSpecificEvent {
                FileChangeFlags changes = FileChangeFlags::None;
            };

            /** @brief Watcher could not keep up; caller must rescan the watched root. */
            struct FileWatchOverflowEvent : FileSpecificEvent {};

        } /* namespace File */

        inline namespace Misc {

            /** @brief OS asked the application to handle an external URL or document. */
            struct UrlOpenRequestEvent : EventPayloadBase {
                std::string url; /**< UTF-8 URL or local path. */
            };

            /** @brief Coalesced periodic timer tick (for clients that opt in). */
            struct TimerTickEvent : TimestampedEvent {
                uint32_t timerId = 0;
                uint64_t periodNs = 0;
            };

            /** @brief Generic device hot-plug (USB / monitor / audio endpoint). */
            struct DeviceChangeEvent : EventPayloadBase {
                DeviceId    device = DeviceId::Invalid;
                std::string description;
                bool        attached = true;
            };

        } /* namespace Misc */

        /** @} */

    } /* namespace Event */

    /** @name Reflection Traits @{ */

    namespace Traits {

        inline namespace Event {

            /** @brief Concept satisfied by every type eligible for @ref Mashiro::SystemEvent. */
            template<typename T>
            concept SystemEventPayload = std::is_class_v<T> && std::is_base_of_v<EventPayloadBase, T>;

            /** @brief Concept satisfied by payloads that target one window. */
            template<typename T>
            concept WindowScoped = std::is_base_of_v<WindowSpecificEvent, T>;

            /** @brief Concept satisfied by payloads that carry capture time. */
            template<typename T>
            concept Timestamped = std::is_base_of_v<TimestampedEvent, T>;

            /**
             * @brief Return the compile-time platform bitset declared for @p T.
             *
             * Payloads without @ref Platform::OnPlatform annotations are portable by default.
             */
            template<SystemEventPayload T>
            consteval PlatformBit PlatformsOf() {
                constexpr auto on = Mashiro::Traits::Anno::Get<Platform::OnPlatform>(^^T);
                return on ? on->set : PlatformBit_All;
            }

            /** @brief Compile-time predicate for whether payload @p T is available on platform @p P. */
            template<SystemEventPayload T, PlatformBit P>
            inline constexpr bool AvailableOn = (PlatformsOf<T>() & P) != PlatformBit::None;

            /** @brief Return the unqualified payload type name. */
            template<SystemEventPayload T>
            consteval std::string_view PayloadTypeName() {
                return std::meta::identifier_of(^^T);
            }

            /**
             * @brief Concept for manager participation in the bookkeep stage.
             *
             * A Platform-thread manager participates for payload @p P by exposing:
             * @code{.cpp}
             * void On(const P&) noexcept;
             * @endcode
             * The convention is structural: no registration table, no marker base on the manager, and no runtime
             * dispatch table. The @c noexcept requirement belongs to the hot path because EventPump bookkeeps on the
             * owner thread immediately before subscriber broadcast.
             */
            template<typename M, typename P>
            concept HandlesBookkeep = SystemEventPayload<P> && requires(M& m, const P& p) {
                { m.On(p) } noexcept -> std::same_as<void>;
            };

        } /* namespace Event */

    } /* namespace Traits */

    /** @} */

    /** @cond DOXYGEN_INTERNAL */
    namespace Detail {

        /** @brief Return whether reflected entity @p m is a complete payload candidate. */
        consteval bool DerivesFromEventBase(std::meta::info m) {
            if (std::meta::is_template(m)) return false;
            if (!std::meta::is_type(m)) return false;
            if (!std::meta::is_class_type(m)) return false;
            if (!std::meta::is_complete_type(m)) return false;
            return std::meta::extract<bool>(std::meta::substitute(
                ^^std::is_base_of_v, {^^Event::EventPayloadBase, m}));
        }

        /** @brief Recursively collect payload candidates declared in @p ns and its nested namespaces. */
        consteval void CollectEventPayloads(std::meta::info ns, std::vector<std::meta::info>& out) {
            for (auto m : std::meta::members_of(ns, std::meta::access_context::unchecked())) {
                if (std::meta::is_namespace(m)) {
                    CollectEventPayloads(m, out);
                } else if (DerivesFromEventBase(m)) {
                    bool seen = false;
                    for (auto e : out) {
                        if (e == m) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) out.push_back(m);
                }
            }
        }

        /** @brief Collect @c std::monostate plus every leaf payload reachable from @ref Mashiro::Event. */
        consteval auto GetAllEventTypes() {
            std::vector<std::meta::info> candidates{};
            candidates.push_back(^^std::monostate);
            CollectEventPayloads(^^Mashiro::Event, candidates);

            std::vector<std::meta::info> baseTypes{};
            for (auto t : candidates) {
                for (auto b : std::meta::bases_of(t, std::meta::access_context::unchecked())) {
                    baseTypes.push_back(std::meta::type_of(b));
                }
            }

            std::vector<std::meta::info> leaves{};
            for (auto t : candidates) {
                bool isBase = false;
                for (auto b : baseTypes) {
                    if (b == t) {
                        isBase = true;
                        break;
                    }
                }
                if (!isBase) leaves.push_back(t);
            }
            return leaves;
        }


        /** @brief Call @c mgr.On(payload) iff @p mgr handles payload type @p P. */
        template<typename M, typename P>
        inline void InvokeBookkeep(M& mgr, const P& payload) noexcept {
            if constexpr (Traits::Event::HandlesBookkeep<M, P>) {
                mgr.On(payload);
            }
        }

    } /* namespace Detail */
    /** @endcond */

    /**
     * @brief Sum type over every leaf system-event payload.
     *
     * The active alternative is the discriminator. Clients should dispatch with @c std::visit or
     * @c std::holds_alternative<T>; @c std::variant::index is declaration-order dependent and not persistent.
     */
    using SystemEvent = [:std::meta::substitute(^^std::variant, Detail::GetAllEventTypes()):];

    /** @brief Return the unqualified type name of the active event payload. */
    [[nodiscard]] inline std::string_view NameOf(const SystemEvent& e) noexcept {
        return std::visit(
            [](const auto& payload) noexcept -> std::string_view {
                using T = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<T, std::monostate>) return "monostate";
                else return Traits::Event::PayloadTypeName<T>();
            },
            e);
    }

    /** @brief Return the target window when the active payload is window-scoped. */
    [[nodiscard]] inline std::optional<WindowId> WindowOf(const SystemEvent& e) noexcept {
        return std::visit(
            [](const auto& payload) noexcept -> std::optional<WindowId> {
                using T = std::remove_cvref_t<decltype(payload)>;
                if constexpr (Traits::Event::WindowScoped<T>) return payload.windowId;
                else return std::nullopt;
            },
            e);
    }

    /** @brief Return the capture timestamp when the active payload carries one. */
    [[nodiscard]] inline std::optional<uint64_t> TimestampOf(const SystemEvent& e) noexcept {
        return std::visit(
            [](const auto& payload) noexcept -> std::optional<uint64_t> {
                using T = std::remove_cvref_t<decltype(payload)>;
                if constexpr (Traits::Event::Timestamped<T>) return payload.timestamp;
                else return std::nullopt;
            },
            e);
    }

    /**
     * @brief Visit @p e and call @c mgr.On(payload) when @p mgr handles the active payload type.
     *
     * This helper is the public form of the bookkeep edge used by EventPump: it is type-driven, direct-call, and pruned
     * by @c if @c constexpr for payloads outside a manager's responsibility.
     */
    template<typename M>
    inline void DispatchBookkeep(M& mgr, const SystemEvent& e) noexcept {
        std::visit(
            [&mgr](const auto& payload) noexcept {
                Detail::InvokeBookkeep(mgr, payload);
            },
            e);
    }

} /* namespace Mashiro */
