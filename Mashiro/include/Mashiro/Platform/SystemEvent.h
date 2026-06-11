/**
 * @file SystemEvent.h
 * @brief Platform-agnostic input vocabulary and system-event taxonomy.
 *
 * Defines the canonical enumerations and structures used by the input
 * pipeline before any platform-specific scancode or virtual-key translation:
 * - @ref Mashiro::KeyCode — logical keys
 * - @ref Mashiro::MouseButton — mouse buttons
 * - @ref Mashiro::Modifiers — modifier-key state
 * - @ref Mashiro::EventKind — discriminator tag for the unified event stream
 * - @ref Mashiro::PayloadFor / @ref Mashiro::SystemEvent — kind/payload binding
 *
 * Values are stable and may be persisted (e.g. in keybinding configs).
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
#include <cstdint>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Mashiro {

    // ============================================================================
    // Event Kinds
    // ============================================================================

    /**
     * @brief Discriminator tag for entries on the unified Platform event stream.
     *
     * All Platform-thread producers (Win32 pump, X11/xcb pump, Wayland
     * dispatcher, dedicated worker threads such as gamepad and file watchers)
     * funnel events through the unified writer; this tag identifies which
     * payload variant follows. Values are sequential (no holes, no manual
     * numbering) so they index per-kind dispatch tables directly — enforced
     * by the `Traits::SequentialEnum` static_assert below.
     *
     * Each entry is bound to a payload type via the @ref Mashiro::PayloadFor
     * annotation on that struct; @ref Mashiro::Traits::PayloadOf recovers the
     * mapping at compile time.
     *
     * @note Insertions must preserve sequentiality. Append new entries at the
     *       very end and never reuse freed values — persisted bindings rely
     *       on numeric stability.
     */
    enum class EventKind : uint8_t {
        // ---- Window lifecycle -------------------------------------------------
        WindowCreate,
        WindowClose,
        WindowDestroy,
        WindowResize,
        WindowMove,
        WindowFocus,
        WindowMinimize,
        WindowMaximize,
        WindowRestore,
        WindowVisibilityChange,
        WindowEnterFullscreen,
        WindowLeaveFullscreen,
        WindowDpiChange,
        WindowScaleChange,         ///< Wayland fractional-scale.
        WindowThemeChange,
        WindowOcclusionChange,     ///< macOS occlusion / Win32 cloak.
        WindowExposed,             ///< X11 Expose / damage region needs redraw.
        WindowDwmCompositionChange,///< Win32 DWM composition enabled / disabled.

        // ---- Keyboard ---------------------------------------------------------
        InputKeyDown,
        InputKeyUp,
        InputChar,                 ///< Translated text codepoint(s).
        InputKeyboardLayoutChange,

        // ---- Pointer ----------------------------------------------------------
        InputMouseEnter,
        InputMouseLeave,
        InputMouseMove,
        InputMouseButton,
        InputScroll,
        InputRawMouseMotion,       ///< Unaccelerated relative motion (FPS / DCC).

        // ---- Touch / pen ------------------------------------------------------
        InputTouch,
        InputTouchGesture,         ///< Pinch / rotate / swipe (synthesised).
        InputPen,
        InputPenProximity,
        InputPenButton,

        // ---- IME --------------------------------------------------------------
        ImeActivate,
        ImeDeactivate,
        ImeComposition,
        ImeCompositionUpdate,
        ImeCommit,
        ImeCandidateList,
        ImePreeditCursor,

        // ---- Clipboard --------------------------------------------------------
        ClipboardUpdate,
        SelectionUpdate,           ///< X11 PRIMARY / Wayland selection.

        // ---- Drag and drop ----------------------------------------------------
        DragEnter,
        DragOver,
        DragDrop,
        DragLeave,

        // ---- Display ----------------------------------------------------------
        DisplayConnect,
        DisplayDisconnect,
        DisplayChange,
        DisplayColorProfileChange,
        DisplayHdrChange,

        // ---- Power / session --------------------------------------------------
        PowerStateChange,
        PowerSuspend,
        PowerResume,
        BatteryLevelChange,
        SessionLock,
        SessionUnlock,
        SessionUserSwitch,         ///< Win32 fast user switch.
        SessionEndQuery,           ///< Logoff / shutdown about to occur.

        // ---- Appearance / accessibility ---------------------------------------
        AppearanceChange,          ///< Light / dark / accent / contrast.
        AccessibilityScreenReader,
        AccessibilityReducedMotion,
        AccessibilityHighContrast,

        // ---- Gamepad ----------------------------------------------------------
        GamepadConnect,
        GamepadDisconnect,
        GamepadState,
        GamepadBatteryChange,

        // ---- File system watcher ---------------------------------------------
        FileCreated,
        FileModified,
        FileDeleted,
        FileRenamed,
        FileAttributesChanged,
        FileWatchOverflow,         ///< Backend buffer overflowed; rescan needed.

        // ---- Misc -------------------------------------------------------------
        UrlOpenRequest,            ///< OS asked the app to open a URL/file.
        TimerTick,
        DeviceChange,              ///< Generic hot-plug (USB / monitor / audio).
    };
    static_assert(Traits::SequentialEnum<EventKind>);
    static_assert(sizeof(EventKind) == 1);

    /// @brief Number of distinct @ref EventKind values.
    inline constexpr size_t kEventKindCount = Traits::EnumeratorsCount<EventKind>;

    // ============================================================================
    // Event Payload Binding
    // ============================================================================

    /**
     * @brief Annotation attached to a payload type to bind it to an @ref EventKind.
     *
     * A payload struct carries this annotation to declare which `EventKind`
     * value identifies it on the wire; reflection recovers the binding so the
     * tag and payload layout cannot drift apart. There is exactly one payload
     * per kind, enforced by a compile-time static_assert at the bottom of
     * this header.
     *
     * @code
     * struct [[=PayloadFor{EventKind::WindowResize}]] WindowResizeEvent : EventHeader { ... };
     * @endcode
     */
    struct PayloadFor {
        EventKind kind;
        constexpr bool operator==(const PayloadFor&) const = default;
    };

    // ============================================================================
    // Event Header (layout reference)
    // ============================================================================

    /**
     * @brief Layout-equivalent view of the first 24 bytes of every payload.
     *
     * Concrete payloads do **not** derive from this type. Each
     * @ref EventPayload specialisation lays out the same fields in the same
     * order so that buffer-level code (golden-file tests, hex dumps,
     * cross-process serialisers) can interpret an opaque byte span as an
     * `EventHeader` regardless of the trailing variant.
     *
     * @note `kind` on a concrete payload is auto-populated to the value bound
     *       by the type's @ref PayloadFor annotation; this struct exists
     *       solely as a layout / documentation reference.
     */
    struct EventHeader {
        EventKind kind = EventKind{};   ///< Discriminator — see @ref PayloadFor.
        uint8_t   pad0 = 0;             ///< Reserved; keeps `flags` 16-bit aligned.
        uint16_t  flags = 0;            ///< Bitmask of @ref EventFlag values.
        uint32_t  windowId = 0;         ///< Window the event targets, or 0 for app-global.
        uint32_t  sequence = 0;         ///< Monotonic sequence number; total order + dedup.
        uint32_t  pad1 = 0;             ///< Reserved; keeps `timestamp` 8-byte aligned.
        uint64_t  timestamp = 0;        ///< High-resolution timestamp (steady clock, ns).

        constexpr bool operator==(const EventHeader&) const = default;
    };
    static_assert(std::is_trivially_copyable_v<EventHeader>);
    static_assert(std::is_standard_layout_v<EventHeader>);
    static_assert(sizeof(EventHeader) == 24, "EventHeader layout must remain stable");
    static_assert(alignof(EventHeader) == 8);

    /**
     * @brief Kind-agnostic flag bits storable in `EventHeader::flags`.
     */
    enum class EventFlag : uint8_t {
        None        = 0,
        Synthetic   = 1u << 0, ///< Generated internally rather than from the OS.
        Coalesced   = 1u << 1, ///< Multiple OS events folded into this one (e.g. mouse-move).
        Replayed    = 1u << 2, ///< Replayed from a recording or testing harness.
        Lost        = 1u << 3, ///< Marker indicating one or more events were dropped before this one.
    };
    static_assert(Traits::BitfieldEnum<EventFlag>);

    // ============================================================================
    // Common Sub-Types
    // ============================================================================

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
        Lines,  ///< Discrete notches (most desktop mice).
        Pixels, ///< High-resolution / smooth scroll.
        Pages,  ///< Page-up / page-down equivalent.
    };
    static_assert(Traits::SequentialEnum<ScrollUnit>);

    /**
     * @brief Pen / stylus button state. Combinable bit flags.
     */
    enum class PenButton : uint8_t {
        None    = 0,
        Tip     = 1u << 0, ///< Pen is in contact with the surface.
        Barrel  = 1u << 1, ///< Side button.
        Eraser  = 1u << 2, ///< Inverted (eraser-end) contact.
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
     *        @ref EventKind::FileCreated etc.).
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

    /// @brief Drag-source MIME-type fingerprint mask.
    enum class DragKind : uint16_t {
        None    = 0,
        Files   = 1u << 0,
        Text    = 1u << 1,
        Image   = 1u << 2,
        Url     = 1u << 3,
        Custom  = 1u << 4,
    };
    static_assert(Traits::BitfieldEnum<DragKind>);

    // clang-format off

    // ============================================================================
    // Event Payloads
    // ============================================================================

    /// @cond DOXYGEN_PAYLOAD_NOISE
    namespace Detail {

        /// @brief Marker base so generic code can detect payload structs at
        ///        compile time without depending on the CRTP template signature.
        struct EventPayloadBase {};

        /**
         * @brief Recover the @ref EventKind bound to a payload type via its
         *        `[[=PayloadFor{...}]]` annotation.
         *
         * Used as the NSDMI for `EventPayload<Derived>::kind` so every concrete
         * payload starts life with the correct discriminator without needing a
         * user-written constructor — the type stays an aggregate.
         *
         * @tparam Derived Concrete payload struct (the CRTP self-type).
         * @return The kind bound by exactly one @ref PayloadFor annotation.
         *
         * @note Evaluated lazily during instantiation of the NSDMI, i.e. when
         *       `Derived` has its full annotation set defined. Calls fail at
         *       compile time if the annotation is missing.
         */
        template<typename Derived>
        consteval EventKind KindFromAnnotation() {
            constexpr auto bound = Traits::Anno::Get<PayloadFor>(^^Derived);
            static_assert(bound.has_value(),
                          "Payload type must carry exactly one [[=PayloadFor{...}]] annotation");
            return bound->kind;
        }

    } // namespace Detail
    /// @endcond

    /**
     * @brief CRTP base shared by every concrete event payload.
     *
     * Lays out the fields of @ref EventHeader directly (so payloads remain
     * standard-layout and ABI-compatible with the header view) and uses a
     * reflection-driven default member initialiser to populate `kind` with the
     * value bound by the derived type's `[[=PayloadFor{...}]]` annotation.
     *
     * Because no user-defined constructor is introduced, every derived payload
     * remains an aggregate and stays designated-initialiser friendly:
     * @code
     * WindowResizeEvent ev{
     *     {.windowId = 1, .sequence = 42, .timestamp = clock.Now()},
     *     .size = {1280, 720},
     * };
     * // ev.kind is automatically EventKind::WindowResize.
     * @endcode
     *
     * The compiler resolves the `kind` initialiser at constant-evaluation time,
     * so on a release build the value is an immediate store with no call.
     *
     * @tparam Derived Concrete payload struct (the curiously-recurring self-type).
     */
    template<typename Derived>
    struct EventPayload : Detail::EventPayloadBase {
        EventKind kind = Detail::KindFromAnnotation<Derived>();
        uint8_t   pad0 = 0;             ///< Reserved; keeps `flags` 16-bit aligned.
        uint16_t  flags = 0;            ///< Bitmask of @ref EventFlag values.
        uint32_t  windowId = 0;         ///< Window the event targets, or 0 for app-global.
        uint32_t  sequence = 0;         ///< Monotonic sequence number; total order + dedup.
        uint32_t  pad1 = 0;             ///< Reserved; keeps `timestamp` 8-byte aligned.
        uint64_t  timestamp = 0;        ///< High-resolution timestamp (steady clock, ns).

        constexpr bool operator==(const EventPayload&) const = default;
    };

    // ---- Window lifecycle ---------------------------------------------------

    /// @brief Window has been created and is ready for first draw.
    struct [[=PayloadFor{EventKind::WindowCreate}]] 
    WindowCreateEvent : EventPayload<WindowCreateEvent> {
        ivec2 size{};       ///< Initial client size in pixels.
        float dpiScale = 1; ///< Initial DPI scale factor.
    };

    /// @brief OS requested the window to close (X / Cmd-Q / Alt-F4).
    struct [[=PayloadFor{EventKind::WindowClose}]] 
    WindowCloseEvent : EventPayload<WindowCloseEvent> {};

    /// @brief Window has been destroyed; this is the last event for `windowId`.
    struct [[=PayloadFor{EventKind::WindowDestroy}]] 
    WindowDestroyEvent : EventPayload<WindowDestroyEvent> {};

    /// @brief Client area has been resized.
    struct [[=PayloadFor{EventKind::WindowResize}]] 
    WindowResizeEvent : EventPayload<WindowResizeEvent> {
        ivec2 size{};
        bool  isMinimised = false;
    };

    /// @brief Window has been moved on the desktop.
    struct [[=PayloadFor{EventKind::WindowMove}]] 
    WindowMoveEvent : EventPayload<WindowMoveEvent> {
        ivec2 position{};
    };

    /// @brief Window gained or lost keyboard focus.
    struct [[=PayloadFor{EventKind::WindowFocus}]] 
    WindowFocusEvent : EventPayload<WindowFocusEvent> {
        bool focused = false;
    };

    struct [[=PayloadFor{EventKind::WindowMinimize}]]
    WindowMinimizeEvent : EventPayload<WindowMinimizeEvent> {};

    struct [[=PayloadFor{EventKind::WindowMaximize}]]
    WindowMaximizeEvent : EventPayload<WindowMaximizeEvent> {};

    struct [[=PayloadFor{EventKind::WindowRestore}]]
    WindowRestoreEvent : EventPayload<WindowRestoreEvent> {};

    /// @brief Window visibility has toggled (`ShowWindow`, `xdg_toplevel.configure`).
    struct [[=PayloadFor{EventKind::WindowVisibilityChange}]] 
    WindowVisibilityChangeEvent : EventPayload<WindowVisibilityChangeEvent> {
        bool visible = false;
    };

    struct [[=PayloadFor{EventKind::WindowEnterFullscreen}]] 
    WindowEnterFullscreenEvent : EventPayload<WindowEnterFullscreenEvent> {
        DisplayId display = DisplayId::Invalid;
    };
    struct [[=PayloadFor{EventKind::WindowLeaveFullscreen}]] 
    WindowLeaveFullscreenEvent : EventPayload<WindowLeaveFullscreenEvent> {};

    /// @brief DPI / scale of the window's monitor changed (per-monitor DPI on Win10+).
    struct [[=PayloadFor{EventKind::WindowDpiChange}]] 
    WindowDpiChangeEvent : EventPayload<WindowDpiChangeEvent> {
        float oldScale = 1;
        float newScale = 1;
    };

    /// @brief Wayland fractional-scale factor change.
    struct [[=PayloadFor{EventKind::WindowScaleChange}, =Platform::WaylandOnly]] 
    WindowScaleChangeEvent : EventPayload<WindowScaleChangeEvent> {
        float scale = 1; ///< Multiples of 120 / 120, surfaced as float.
    };

    /// @brief OS theme applied to the window (decorations, system menu).
    struct [[=PayloadFor{EventKind::WindowThemeChange}]] 
    WindowThemeChangeEvent : EventPayload<WindowThemeChangeEvent> {
        AppearanceTheme theme = AppearanceTheme::Unknown;
    };

    /// @brief Window occluded / unoccluded (macOS visibility, Win32 cloak / DWM).
    struct [[=PayloadFor{EventKind::WindowOcclusionChange}]] 
    WindowOcclusionChangeEvent : EventPayload<WindowOcclusionChangeEvent> {
        bool occluded = false;
    };

    /// @brief X11 `Expose` / Wayland frame damage — region must be redrawn.
    struct [[=PayloadFor{EventKind::WindowExposed}, =Platform::LinuxOnly]] 
    WindowExposedEvent : EventPayload<WindowExposedEvent> {
        ivec2 origin{};
        ivec2 extent{};
    };

    /// @brief Win32 DWM composition state toggled (`WM_DWMCOMPOSITIONCHANGED`).
    struct [[=PayloadFor{EventKind::WindowDwmCompositionChange}, =Platform::WindowsOnly]] 
    WindowDwmCompositionChangeEvent : EventPayload<WindowDwmCompositionChangeEvent> {
        bool compositionEnabled = false;
    };

    // ---- Keyboard -----------------------------------------------------------

    /**
     * @brief Logical key identifier.
     *
     * Letter and digit values intentionally match their ASCII code points so a
     * `KeyCode` can round-trip with printable input where applicable. All other
     * keys occupy a private range starting at 256.
     */
    enum class KeyCode : uint32_t {
        Unknown = 0,

        // Letters (ASCII uppercase)
        A = 'A',
        B = 'B',
        C = 'C',
        D = 'D',
        E = 'E',
        F = 'F',
        G = 'G',
        H = 'H',
        I = 'I',
        J = 'J',
        K = 'K',
        L = 'L',
        M = 'M',
        N = 'N',
        O = 'O',
        P = 'P',
        Q = 'Q',
        R = 'R',
        S = 'S',
        T = 'T',
        U = 'U',
        V = 'V',
        W = 'W',
        X = 'X',
        Y = 'Y',
        Z = 'Z',

        // Numbers (ASCII digits)
        Num0 = '0',
        Num1 = '1',
        Num2 = '2',
        Num3 = '3',
        Num4 = '4',
        Num5 = '5',
        Num6 = '6',
        Num7 = '7',
        Num8 = '8',
        Num9 = '9',

        // Function keys
        F1 = 256,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,

        // Navigation / modifiers / whitespace
        Escape,
        Tab,
        CapsLock,
        Shift,
        Control,
        Alt,
        Super,
        Space,
        Enter,
        Backspace,
        Delete,
        Insert,
        Home,
        End,
        PageUp,
        PageDown,
        Left,
        Right,
        Up,
        Down,

        // Punctuation
        Comma,
        Period,
        Slash,
        Semicolon,
        Quote,
        BracketLeft,
        BracketRight,
        Backslash,
        Minus,
        Equal,
        Grave,
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

        /// @return `true` iff no modifier bits are set.
        [[nodiscard]] constexpr bool IsNone() const { return !shift && !ctrl && !alt && !super; }

        /// @name Common modifier combinations
        /// @{
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
        /// @}
    };

    /// @brief Physical scancode that produced this key event (USB HID Usage ID, OS native).
    enum class Scancode : uint32_t { Unknown = 0 };

    /**
     * @brief Common payload skeleton for both key-down and key-up.
     *
     * CRTP-parameterised on the concrete event so the inherited
     * @ref EventPayload<Derived>::kind initialiser still resolves to the
     * derived type's `[[=PayloadFor{...}]]` annotation.
     */
    template<typename Derived>
    struct KeyEventBase : EventPayload<Derived> {
        KeyCode   code     = KeyCode::Unknown; ///< Logical key.
        Scancode  scancode = Scancode::Unknown;///< Physical key (layout-independent).
        Modifiers mods{};                      ///< Modifier-key state at event time.
        DeviceId  device   = DeviceId::Invalid;///< Originating keyboard.
        bool      repeat   = false;            ///< OS auto-repeat (key-down only).
    };

    struct [[=PayloadFor{EventKind::InputKeyDown}]]
    KeyDownEvent : KeyEventBase<KeyDownEvent> {};

    struct [[=PayloadFor{EventKind::InputKeyUp}]]
    KeyUpEvent : KeyEventBase<KeyUpEvent> {};

    /// @brief Translated text input — one or more codepoints from a single keystroke.
    struct [[=PayloadFor{EventKind::InputChar}]] 
    CharEvent : EventPayload<CharEvent> {
        std::array<char32_t, 4> codepoints{}; ///< UTF-32, zero-terminated when shorter than 4.
        uint8_t   count = 0;                  ///< Number of valid entries in `codepoints`.
        Modifiers mods{};
    };

    /// @brief Active keyboard layout / input source changed.
    struct [[=PayloadFor{EventKind::InputKeyboardLayoutChange}]]
    KeyboardLayoutChangeEvent : EventPayload<KeyboardLayoutChangeEvent> {
        std::string layoutId; ///< OS identifier (e.g. "en-US", "ja-JP-IME").
    };

    // ---- Pointer ------------------------------------------------------------

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
        Button4 = 3, ///< Typically "back" on five-button mice.
        Button5 = 4, ///< Typically "forward" on five-button mice.
    };

    struct [[=PayloadFor{EventKind::InputMouseEnter}]]
    MouseEnterEvent : EventPayload<MouseEnterEvent> {};

    struct [[=PayloadFor{EventKind::InputMouseLeave}]]
    MouseLeaveEvent : EventPayload<MouseLeaveEvent> {};

    /**
     * @brief Pointer moved within the window's client area.
     */
    struct [[=PayloadFor{EventKind::InputMouseMove}]] 
    MouseMoveEvent : EventPayload<MouseMoveEvent> {
        vec2     position{};                 ///< Client-space, sub-pixel precision.
        vec2     delta{};                    ///< Movement since previous event.
        Modifiers mods{};
        DeviceId device = DeviceId::Invalid;
    };

    /**
     * @brief Pointer button pressed or released.
     */
    struct [[=PayloadFor{EventKind::InputMouseButton}]] 
    MouseButtonEvent : EventPayload<MouseButtonEvent> {
        vec2        position{};
        MouseButton button = MouseButton::Left;
        bool        pressed = false;
        uint8_t     clickCount = 1; ///< 1=single, 2=double, 3=triple, etc.
        Modifiers   mods{};
        DeviceId    device = DeviceId::Invalid;
    };

    /// @brief Mouse wheel / trackpad scroll.
    struct [[=PayloadFor{EventKind::InputScroll}]] 
    ScrollEvent : EventPayload<ScrollEvent> {
        vec2       position{};
        vec2       delta{};        ///< +Y = scroll up, +X = scroll right.
        ScrollUnit unit = ScrollUnit::Lines;
        bool       inertial = false;///< macOS / precision touchpad inertial phase.
        Modifiers  mods{};
        DeviceId   device = DeviceId::Invalid;
    };

    /// @brief Raw, unaccelerated relative mouse motion (`WM_INPUT` / xinput2 / libinput).
    struct [[=PayloadFor{EventKind::InputRawMouseMotion}]] 
    RawMouseMotionEvent : EventPayload<RawMouseMotionEvent> {
        ivec2    delta{};
        DeviceId device = DeviceId::Invalid;
    };

    // ---- Touch / pen --------------------------------------------------------

    /// @brief Single-finger touch contact event.
    struct [[=PayloadFor{EventKind::InputTouch}]] 
    TouchEvent : EventPayload<TouchEvent> {
        TouchId    id = TouchId::Invalid;
        TouchPhase phase = TouchPhase::Began;
        vec2       position{};
        vec2       size{};      ///< Contact ellipse size in pixels (0 if unsupported).
        float      pressure = 0;///< Normalised [0,1].
    };

    /// @brief OS-synthesised gesture from the trackpad / touch driver.
    struct [[=PayloadFor{EventKind::InputTouchGesture}]]
    TouchGestureEvent : EventPayload<TouchGestureEvent> {
        enum class Kind : uint8_t { Pinch, Rotate, Swipe, Pan, Tap, LongPress };
        Kind  gesture  = Kind::Pinch; ///< Renamed from `kind` to avoid shadowing the EventPayload discriminator.
        vec2  position{};
        vec2  delta;      ///< Pan/swipe direction in pixels.
        float scale = 1;    ///< Pinch scale factor.
        float rotation = 0; ///< Rotation in radians.
    };

    /// @brief Stylus / pen contact event with full Wintab/Pointer-Input metadata.
    struct [[=PayloadFor{EventKind::InputPen}]] 
    PenEvent : EventPayload<PenEvent> {
        TouchPhase phase = TouchPhase::Began;
        vec2       position{};
        float      pressure = 0;     ///< Normalised [0,1].
        float      tiltX = 0;        ///< Radians, +X axis tilt.
        float      tiltY = 0;        ///< Radians, +Y axis tilt.
        float      twist = 0;        ///< Barrel rotation in radians.
        float      tangentialPressure = 0;
        DeviceId   device = DeviceId::Invalid;
    };

    /// @brief Pen entered or left the digitiser hover range.
    struct [[=PayloadFor{EventKind::InputPenProximity}]] 
    PenProximityEvent : EventPayload<PenProximityEvent> {
        bool     entering = true;
        DeviceId device = DeviceId::Invalid;
    };

    /// @brief Pen barrel button / eraser-tip state changed.
    struct [[=PayloadFor{EventKind::InputPenButton}]] 
    PenButtonEvent : EventPayload<PenButtonEvent> {
        PenButton buttons = PenButton::None;
        bool      pressed = false;
        DeviceId  device = DeviceId::Invalid;
    };

    // ---- IME ----------------------------------------------------------------

    struct [[=PayloadFor{EventKind::ImeActivate}]]   
    ImeActivateEvent : EventPayload<ImeActivateEvent> {};

    struct [[=PayloadFor{EventKind::ImeDeactivate}]] 
    ImeDeactivateEvent : EventPayload<ImeDeactivateEvent> {};

    /// @brief IME composition started.
    struct [[=PayloadFor{EventKind::ImeComposition}]] 
    ImeCompositionEvent : EventPayload<ImeCompositionEvent> {
        std::string preedit; ///< UTF-8 preedit / composition string.
        uint32_t    caret = 0;
    };

    /// @brief IME composition contents changed.
    struct [[=PayloadFor{EventKind::ImeCompositionUpdate}]] 
    ImeCompositionUpdateEvent : EventPayload<ImeCompositionUpdateEvent> {
        std::string preedit;
        uint32_t    caret = 0;
        uint32_t    selectionStart = 0;
        uint32_t    selectionEnd = 0;
    };

    /// @brief IME committed a final string.
    struct [[=PayloadFor{EventKind::ImeCommit}]] 
    ImeCommitEvent : EventPayload<ImeCommitEvent> {
        std::string text; ///< UTF-8 finalised string.
    };

    /// @brief IME candidate-window contents updated.
    struct [[=PayloadFor{EventKind::ImeCandidateList}]] 
    ImeCandidateListEvent : EventPayload<ImeCandidateListEvent> {
        std::vector<std::string> candidates; ///< UTF-8 candidates.
        uint32_t selected = 0;               ///< Currently-highlighted index.
        uint32_t pageStart = 0;
        uint32_t pageSize = 0;
    };

    /// @brief Where the host should anchor the candidate window (in client space).
    struct [[=PayloadFor{EventKind::ImePreeditCursor}]] 
    ImePreeditCursorEvent : EventPayload<ImePreeditCursorEvent> {
        ivec2 caretOrigin{}; ///< Client-space pixel origin.
        uvec2 caretSize{};
    };

    // ---- Clipboard ----------------------------------------------------------

    /// @brief OS clipboard contents changed.
    struct [[=PayloadFor{EventKind::ClipboardUpdate}]] 
    ClipboardUpdateEvent : EventPayload<ClipboardUpdateEvent> {
        DragKind kinds = DragKind::None; ///< Bitmask of available MIME categories.
    };

    /// @brief X11 PRIMARY / Wayland selection changed (separate from clipboard).
    struct [[=PayloadFor{EventKind::SelectionUpdate}, =Platform::LinuxOnly]] 
    SelectionUpdateEvent : EventPayload<SelectionUpdateEvent> {
        DragKind kinds = DragKind::None;
    };

    // ---- Drag and drop ------------------------------------------------------

    struct DragPayloadInfo {
        DragKind                 kinds = DragKind::None;
        std::vector<std::string> mimeTypes;
        size_t                   itemCount = 0;
    };

    struct [[=PayloadFor{EventKind::DragEnter}]] 
    DragEnterEvent : EventPayload<DragEnterEvent> {
        vec2            position{};
        DragPayloadInfo payload;
    };

    struct [[=PayloadFor{EventKind::DragOver}]] 
    DragOverEvent : EventPayload<DragOverEvent> {
        vec2 position{};
    };

    struct [[=PayloadFor{EventKind::DragDrop}]]
    DragDropEvent : EventPayload<DragDropEvent> {
        vec2                      position{};
        std::vector<std::string>  paths;       ///< For `Files` payloads, decoded local paths.
        std::string               text;        ///< For `Text` payloads.
        std::vector<std::byte>    blob;        ///< For `Custom` / `Image` payloads, raw bytes.
        DragKind                  drag = DragKind::None; ///< Renamed from `kind` to avoid shadowing the discriminator.
    };

    struct [[=PayloadFor{EventKind::DragLeave}]] 
    DragLeaveEvent : EventPayload<DragLeaveEvent> {};

    // ---- Display ------------------------------------------------------------

    struct [[=PayloadFor{EventKind::DisplayConnect}]] 
    DisplayConnectEvent : EventPayload<DisplayConnectEvent> {
        DisplayId   display = DisplayId::Invalid;
        std::string name;
    };

    struct [[=PayloadFor{EventKind::DisplayDisconnect}]] 
    DisplayDisconnectEvent : EventPayload<DisplayDisconnectEvent> {
        DisplayId display = DisplayId::Invalid;
    };

    /// @brief A display's mode, position, or topology changed.
    struct [[=PayloadFor{EventKind::DisplayChange}]] 
    DisplayChangeEvent : EventPayload<DisplayChangeEvent> {
        DisplayId display = DisplayId::Invalid;
        uvec2     resolution{};
        float     refreshHz = 0;
        float     dpiScale = 1;
    };

    /// @brief ICC / colour profile change (typically per-display on macOS / Win10+).
    struct [[=PayloadFor{EventKind::DisplayColorProfileChange}]] 
    DisplayColorProfileChangeEvent : EventPayload<DisplayColorProfileChangeEvent> {
        DisplayId   display = DisplayId::Invalid;
        std::string profileName;
    };

    /// @brief HDR enable / disable / metadata change.
    struct [[=PayloadFor{EventKind::DisplayHdrChange}]] 
    DisplayHdrChangeEvent : EventPayload<DisplayHdrChangeEvent> {
        DisplayId display = DisplayId::Invalid;
        bool      hdrEnabled = false;
        float     maxLuminanceNits = 0;
    };

    // ---- Power / session ----------------------------------------------------

    struct [[=PayloadFor{EventKind::PowerStateChange}]] 
    PowerStateChangeEvent : EventPayload<PowerStateChangeEvent> {
        PowerSource source = PowerSource::Unknown;
        float       batteryLevel = 1; ///< Normalised [0,1]; 1 if non-battery.
    };

    struct [[=PayloadFor{EventKind::PowerSuspend}]] 
    PowerSuspendEvent : EventPayload<PowerSuspendEvent> {};

    struct [[=PayloadFor{EventKind::PowerResume}]]  
    PowerResumeEvent : EventPayload<PowerResumeEvent> {};

    struct [[=PayloadFor{EventKind::BatteryLevelChange}]] 
    BatteryLevelChangeEvent : EventPayload<BatteryLevelChangeEvent> {
        float    level = 1;
        bool     charging = false;
        uint32_t secondsRemaining = 0; ///< 0 if unknown.
    };

    struct [[=PayloadFor{EventKind::SessionLock}]]   
    SessionLockEvent : EventPayload<SessionLockEvent> {};

    struct [[=PayloadFor{EventKind::SessionUnlock}]] 
    SessionUnlockEvent : EventPayload<SessionUnlockEvent> {};

    /// @brief Win32 fast user switch.
    struct [[=PayloadFor{EventKind::SessionUserSwitch}, =Platform::WindowsOnly]] 
    SessionUserSwitchEvent : EventPayload<SessionUserSwitchEvent> {};

    /// @brief Logoff or shutdown imminent — last chance to persist work.
    struct [[=PayloadFor{EventKind::SessionEndQuery}]] 
    SessionEndQueryEvent : EventPayload<SessionEndQueryEvent> {
        bool isShutdown = false;
        bool isCritical = false;
    };

    // ---- Appearance / accessibility ----------------------------------------

    struct [[=PayloadFor{EventKind::AppearanceChange}]] 
    AppearanceChangeEvent : EventPayload<AppearanceChangeEvent> {
        AppearanceTheme theme = AppearanceTheme::Unknown;
        uint32_t        accentColorRgba = 0;
    };

    struct [[=PayloadFor{EventKind::AccessibilityScreenReader}]] 
    AccessibilityScreenReaderEvent : EventPayload<AccessibilityScreenReaderEvent> {
        bool enabled = false;
    };

    struct [[=PayloadFor{EventKind::AccessibilityReducedMotion}]] 
    AccessibilityReducedMotionEvent : EventPayload<AccessibilityReducedMotionEvent> {
        bool enabled = false;
    };

    struct [[=PayloadFor{EventKind::AccessibilityHighContrast}]] 
    AccessibilityHighContrastEvent : EventPayload<AccessibilityHighContrastEvent> {
        bool enabled = false;
    };

    // ---- Gamepad ------------------------------------------------------------

    struct [[=PayloadFor{EventKind::GamepadConnect}]] 
    GamepadConnectEvent : EventPayload<GamepadConnectEvent> {
        DeviceId    device = DeviceId::Invalid;
        std::string name;
        uint32_t    vendorId = 0;
        uint32_t    productId = 0;
    };

    struct [[=PayloadFor{EventKind::GamepadDisconnect}]] 
    GamepadDisconnectEvent : EventPayload<GamepadDisconnectEvent> {
        DeviceId device = DeviceId::Invalid;
    };

    /// @brief Snapshot of a gamepad's full state.
    struct [[=PayloadFor{EventKind::GamepadState}]] 
    GamepadStateEvent : EventPayload<GamepadStateEvent> {
        DeviceId             device = DeviceId::Invalid;
        std::array<float, 6> axes{};    ///< LX, LY, RX, RY, LT, RT (normalised).
        uint32_t             buttons = 0;///< Bit mask, layout = vendor-defined map.
    };

    struct [[=PayloadFor{EventKind::GamepadBatteryChange}]] 
    GamepadBatteryChangeEvent : EventPayload<GamepadBatteryChangeEvent> {
        DeviceId device = DeviceId::Invalid;
        float    level = 1;
        bool     charging = false;
    };

    // ---- File system watcher ------------------------------------------------

    /**
     * @brief Common payload skeleton for file-system change events.
     *
     * CRTP-parameterised on the concrete event so the inherited `kind` is
     * resolved from the derived type's annotation rather than this base.
     */
    template<typename Derived>
    struct FileEventBase : EventPayload<Derived> {
        std::string path;
    };

    struct [[=PayloadFor{EventKind::FileCreated}]]
    FileCreatedEvent  : FileEventBase<FileCreatedEvent> {};

    struct [[=PayloadFor{EventKind::FileDeleted}]]
    FileDeletedEvent  : FileEventBase<FileDeletedEvent> {};

    struct [[=PayloadFor{EventKind::FileModified}]]
    FileModifiedEvent : FileEventBase<FileModifiedEvent> {
        FileChangeFlags changes = FileChangeFlags::None;
    };

    struct [[=PayloadFor{EventKind::FileRenamed}]]
    FileRenamedEvent : EventPayload<FileRenamedEvent> {
        std::string oldPath;
        std::string newPath;
    };

    struct [[=PayloadFor{EventKind::FileAttributesChanged}]]
    FileAttributesChangedEvent : FileEventBase<FileAttributesChangedEvent> {
        FileChangeFlags changes = FileChangeFlags::None;
    };

    /// @brief Watcher could not keep up — caller must rescan the watched root.
    struct [[=PayloadFor{EventKind::FileWatchOverflow}]] 
    FileWatchOverflowEvent : EventPayload<FileWatchOverflowEvent> {
        std::string root;
    };

    // ---- Misc ---------------------------------------------------------------

    /// @brief OS asked the application to handle an external URL or document.
    struct [[=PayloadFor{EventKind::UrlOpenRequest}]] 
    UrlOpenRequestEvent : EventPayload<UrlOpenRequestEvent> {
        std::string url; ///< UTF-8 URL or local path.
    };

    /// @brief Coalesced periodic timer tick (for clients that opt in).
    struct [[=PayloadFor{EventKind::TimerTick}]] 
    TimerTickEvent : EventPayload<TimerTickEvent> {
        uint32_t timerId = 0;
        uint64_t periodNs = 0;
    };

    /// @brief Generic device hot-plug (USB / monitor / audio endpoint).
    struct [[=PayloadFor{EventKind::DeviceChange}]] 
    DeviceChangeEvent : EventPayload<DeviceChangeEvent> {
        DeviceId    device = DeviceId::Invalid;
        std::string description;
        bool        attached = true;
    };

    // ============================================================================
    // Reflection Traits — Kind ↔ Payload Mapping
    // ============================================================================

    namespace Traits {

        /** @brief Concept: `T` is a system-event payload (derives from
         *         @ref Mashiro::Detail::EventPayloadBase).
         */
        template<typename T>
        concept SystemEventPayload = std::is_base_of_v<::Mashiro::Detail::EventPayloadBase, T>;

        /**
         * @brief Recover the @ref EventKind that payload `T` is bound to.
         *
         * @tparam T A payload struct annotated with exactly one @ref PayloadFor.
         * @return The bound `EventKind`.
         *
         * Compile-time. Asserts that the binding annotation exists.
         */
        template<SystemEventPayload T>
        consteval EventKind KindOf() {
            constexpr auto bound = ::Mashiro::Traits::Anno::Get<PayloadFor>(^^T);
            static_assert(bound.has_value(),
                          "Payload must carry a PayloadFor annotation");
            return bound->kind;
        }

        /**
         * @brief Recover a payload's supported @ref PlatformBit set.
         *
         * Returns @ref PlatformBit_All when the payload carries no
         * @ref Platform::OnPlatform annotation (i.e. portable).
         */
        template<SystemEventPayload T>
        consteval PlatformBit PlatformsOf() {
            constexpr auto on = ::Mashiro::Traits::Anno::Get<Platform::OnPlatform>(^^T);
            return on ? on->set : PlatformBit_All;
        }

        /**
         * @brief Compile-time predicate: payload `T` is emitted on platform `P`.
         *
         * @tparam T Payload type.
         * @tparam P Single-bit platform (e.g. `PlatformBit::Windows`).
         */
        template<SystemEventPayload T, PlatformBit P>
        inline constexpr bool AvailableOn = (PlatformsOf<T>() & P) != PlatformBit::None;

        /// @brief The unqualified type name of payload `T` (`"WindowResizeEvent"` etc.).
        template<SystemEventPayload T>
        consteval std::string_view PayloadTypeName() {
            return std::meta::identifier_of(^^T);
        }

        /// @brief The textual name of an @ref EventKind enumerator.
        template<EventKind K>
        consteval std::string_view EventKindName() {
            return ::Mashiro::Traits::EnumeratorName<EventKind, K>();
        }

    } // namespace Traits

    // ============================================================================
    // Compile-Time Completeness Check
    // ============================================================================

    /// @cond DOXYGEN_INTERNAL
    namespace Detail {

        /// @brief `true` for class / struct types that carry a @ref PayloadFor
        ///        annotation. Function / variable templates and other
        ///        non-class members of `Mashiro` are skipped — `annotations_of`
        ///        cannot be applied to template entities or non-types.
        consteval bool IsBoundPayload(std::meta::info m) {
            if (std::meta::is_template(m)) return false;
            if (!std::meta::is_type(m)) return false;
            if (!std::meta::is_class_type(m)) return false;
            if (!std::meta::is_complete_type(m)) return false;
            return ::Mashiro::Traits::Anno::Has<PayloadFor>(m);
        }

        /**
         * @brief Build a histogram (one entry per @ref EventKind) of how many
         *        payload structs claim each kind. Should be all-ones at the
         *        end of the header.
         *
         * The `is_template / is_type / is_class_type / is_complete_type`
         * cascade screens out non-class members of `Mashiro` (free functions,
         * function templates such as `MakePoint<T,N>` from Geom.h, variable
         * templates, namespaces, etc.). `annotations_of` cannot be applied to
         * those entities; the cascade is what makes the namespace-wide sweep
         * portable.
         */
        consteval std::array<uint8_t, kEventKindCount> BuildKindCoverage() {
            std::array<uint8_t, kEventKindCount> seen{};
            template for (constexpr auto m :
                          std::define_static_array(
                              std::meta::members_of(^^Mashiro,
                                                    std::meta::access_context::unchecked()))) {
                if constexpr (!std::meta::is_template(m) &&
                               std::meta::is_type(m) &&
                               std::meta::is_class_type(m) &&
                               std::meta::is_complete_type(m)) {
                    if constexpr (::Mashiro::Traits::Anno::Has<PayloadFor>(m)) {
                        constexpr auto bound =
                            ::Mashiro::Traits::Anno::Get<PayloadFor>(m);
                        ++seen[static_cast<size_t>(bound->kind)];
                    }
                }
            }
            return seen;
        }

        consteval bool AllKindsCovered() {
            constexpr auto cov = BuildKindCoverage();
            for (size_t i = 0; i < cov.size(); ++i)
                if (cov[i] != 1) return false;
            return true;
        }

    } // namespace Detail
    /// @endcond

    static_assert(Detail::AllKindsCovered(),
                  "Every EventKind must have exactly one payload struct annotated "
                  "[[=PayloadFor{...}]] in namespace Mashiro");

    // clang-format on

} // namespace Mashiro

