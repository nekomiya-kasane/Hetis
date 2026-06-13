// SPDX-License-Identifier: MIT
//
// Tests for Mashiro::Platform::SystemEvent — the reflection-driven event
// vocabulary. Verifies the marker base + mixins (EventPayloadBase /
// WindowSpecificEvent / TimestampedEvent), the platform-filter trait, the
// cross-cutting accessors (NameOf / WindowOf / TimestampOf), the variant
// materialisation, and convention-based bookkeep dispatch.
//
// There is intentionally no `EventKind` enum and no `[[=PayloadFor{...}]]`
// annotation: the payload type *is* the discriminator on `SystemEvent`.

#include <Mashiro/Platform/SystemEvent.h>

#include <catch2/catch_test_macros.hpp>

#include <Support/Meta.h>

#include <array>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

// clang-format off

using namespace Mashiro;

// =========================================================================
// Section 1 — Categorisation enums (TouchPhase / ScrollUnit / …)
// =========================================================================

TEST_CASE("Categorisation enums round-trip via reflection", AUTO_TAG) {
    STATIC_REQUIRE(Traits::EnumeratorName<TouchPhase, TouchPhase::Began>()       == "Began");
    STATIC_REQUIRE(Traits::EnumeratorName<TouchPhase, TouchPhase::Cancelled>()   == "Cancelled");
    STATIC_REQUIRE(Traits::EnumeratorName<ScrollUnit, ScrollUnit::Pixels>()      == "Pixels");
    STATIC_REQUIRE(Traits::EnumeratorName<AppearanceTheme,
                                          AppearanceTheme::HighContrastDark>() == "HighContrastDark");
    STATIC_REQUIRE(Traits::EnumeratorName<PowerSource, PowerSource::UPS>()       == "UPS");
}

// =========================================================================
// Section 2 — Bitfield enums: operator| / & / mask / static categorisation
// =========================================================================

TEST_CASE("PenButton, FileChangeFlags, DragKind, PlatformBit are BitfieldEnums", AUTO_TAG) {
    STATIC_REQUIRE(Traits::BitfieldEnum<PenButton>);
    STATIC_REQUIRE(Traits::BitfieldEnum<FileChangeFlags>);
    STATIC_REQUIRE(Traits::BitfieldEnum<DragKind>);
    STATIC_REQUIRE(Traits::BitfieldEnum<PlatformBit>);
}

TEST_CASE("BitfieldEnum operators compose without explicit casts", AUTO_TAG) {
    constexpr auto rwx = PenButton::Tip | PenButton::Barrel | PenButton::Eraser;
    STATIC_REQUIRE((rwx & PenButton::Tip)    == PenButton::Tip);
    STATIC_REQUIRE((rwx & PenButton::Barrel) == PenButton::Barrel);
    STATIC_REQUIRE((rwx & PenButton::Eraser) == PenButton::Eraser);

    constexpr auto cleared = rwx & ~PenButton::Eraser;
    STATIC_REQUIRE((cleared & PenButton::Eraser) == PenButton::None);

    constexpr auto allMask = Traits::kBitfieldMask<PenButton>;
    STATIC_REQUIRE((allMask & PenButton::Tip)    == PenButton::Tip);
    STATIC_REQUIRE((allMask & PenButton::Barrel) == PenButton::Barrel);
    STATIC_REQUIRE((allMask & PenButton::Eraser) == PenButton::Eraser);
}

TEST_CASE("Composite PlatformBit aliases match expected component sets", AUTO_TAG) {
    STATIC_REQUIRE((PlatformBit_Linux & PlatformBit::Linux_X11)     == PlatformBit::Linux_X11);
    STATIC_REQUIRE((PlatformBit_Linux & PlatformBit::Linux_Wayland) == PlatformBit::Linux_Wayland);
    STATIC_REQUIRE((PlatformBit_Linux & PlatformBit::Windows)       == PlatformBit::None);

    STATIC_REQUIRE((PlatformBit_Desktop & PlatformBit::Windows) == PlatformBit::Windows);
    STATIC_REQUIRE((PlatformBit_Desktop & PlatformBit::macOS)   == PlatformBit::macOS);

    STATIC_REQUIRE((PlatformBit_All & PlatformBit::Android) == PlatformBit::Android);
    STATIC_REQUIRE((PlatformBit_All & PlatformBit::iOS)     == PlatformBit::iOS);
}

// =========================================================================
// Section 3 — Modifiers bitfield + factories
// =========================================================================

TEST_CASE("Modifiers factories produce expected one-shot states", AUTO_TAG) {
    STATIC_REQUIRE(Modifiers::None().IsNone());
    STATIC_REQUIRE_FALSE(Modifiers::Ctrl().IsNone());
    STATIC_REQUIRE(Modifiers::Ctrl().ctrl);
    STATIC_REQUIRE(Modifiers::Shift().shift);
    STATIC_REQUIRE(Modifiers::Alt().alt);
    STATIC_REQUIRE(Modifiers::Super().super);

    constexpr auto cs = Modifiers::CtrlShift();
    STATIC_REQUIRE(cs.ctrl && cs.shift && !cs.alt && !cs.super);

    constexpr auto cas = Modifiers::CtrlAltShift();
    STATIC_REQUIRE(cas.ctrl && cas.alt && cas.shift && !cas.super);
}

// =========================================================================
// Section 4 — KeyCode preserves ASCII identity for letters and digits
// =========================================================================

TEST_CASE("KeyCode letters and digits map to ASCII code points", AUTO_TAG) {
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::A) == uint32_t{'A'});
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Z) == uint32_t{'Z'});
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Num0) == uint32_t{'0'});
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Num9) == uint32_t{'9'});
    // Function and named keys live above 256 so they don't collide with ASCII.
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::F1)     == 256);
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Escape) >  256);
}

// =========================================================================
// Section 5 — Marker base + mixins: opt-in via inheritance
// =========================================================================

TEST_CASE("EventPayloadBase is an empty marker; mixins inherit it", AUTO_TAG) {
    STATIC_REQUIRE(std::is_empty_v<Event::EventPayloadBase>);
    STATIC_REQUIRE(std::is_base_of_v<Event::EventPayloadBase, WindowSpecificEvent>);
    STATIC_REQUIRE(std::is_base_of_v<Event::EventPayloadBase, TimestampedEvent>);
    STATIC_REQUIRE(WindowSpecificEvent{}.windowId  == WindowId::Invalid);
    STATIC_REQUIRE(TimestampedEvent{}.timestamp    == 0);
}

TEST_CASE("SystemEventPayload concept identifies every payload, rejects anything else", AUTO_TAG) {
    STATIC_REQUIRE(Traits::SystemEventPayload<WindowResizeEvent>);
    STATIC_REQUIRE(Traits::SystemEventPayload<KeyDownEvent>);
    STATIC_REQUIRE(Traits::SystemEventPayload<DisplayChangeEvent>);
    STATIC_REQUIRE(Traits::SystemEventPayload<FileCreatedEvent>);
    STATIC_REQUIRE(Traits::SystemEventPayload<TimerTickEvent>);

    // Plain types are not payloads — anything that doesn't derive from the
    // marker base is excluded.
    STATIC_REQUIRE_FALSE(Traits::SystemEventPayload<int>);
    STATIC_REQUIRE_FALSE(Traits::SystemEventPayload<Modifiers>);
    STATIC_REQUIRE_FALSE(Traits::SystemEventPayload<Event::DragPayloadInfo>);
}

TEST_CASE("SystemEvent variant excludes abstract bases and includes only leaves", AUTO_TAG) {
    // Concrete leaves are present; abstract bases (WindowSpecificEvent,
    // TimestampedEvent, KeyEventBase, FileSpecificEvent) are filtered out by
    // the "is-not-a-base-of-another-candidate" leaf rule in the variant
    // materialiser, so they cannot be constructed as a SystemEvent.
    STATIC_REQUIRE_FALSE(std::is_constructible_v<SystemEvent, WindowSpecificEvent>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<SystemEvent, TimestampedEvent>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<SystemEvent, Event::KeyEventBase>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<SystemEvent, Event::FileSpecificEvent>);

    STATIC_REQUIRE(std::is_constructible_v<SystemEvent, WindowResizeEvent>);
    STATIC_REQUIRE(std::is_constructible_v<SystemEvent, KeyDownEvent>);
    STATIC_REQUIRE(std::is_constructible_v<SystemEvent, FileCreatedEvent>);
    STATIC_REQUIRE(std::is_constructible_v<SystemEvent, TimerTickEvent>);
}

TEST_CASE("WindowScoped concept identifies window-targeting payloads", AUTO_TAG) {
    STATIC_REQUIRE(Traits::WindowScoped<WindowResizeEvent>);
    STATIC_REQUIRE(Traits::WindowScoped<KeyDownEvent>);
    STATIC_REQUIRE(Traits::WindowScoped<MouseMoveEvent>);
    STATIC_REQUIRE(Traits::WindowScoped<DragDropEvent>);
    STATIC_REQUIRE(Traits::WindowScoped<ImeCommitEvent>);

    STATIC_REQUIRE_FALSE(Traits::WindowScoped<DisplayChangeEvent>);
    STATIC_REQUIRE_FALSE(Traits::WindowScoped<PowerStateChangeEvent>);
    STATIC_REQUIRE_FALSE(Traits::WindowScoped<GamepadStateEvent>);
    STATIC_REQUIRE_FALSE(Traits::WindowScoped<FileCreatedEvent>);
    STATIC_REQUIRE_FALSE(Traits::WindowScoped<ClipboardUpdateEvent>);
    STATIC_REQUIRE_FALSE(Traits::WindowScoped<AppearanceChangeEvent>);
}

TEST_CASE("Timestamped concept identifies time-sensitive payloads", AUTO_TAG) {
    STATIC_REQUIRE(Traits::Timestamped<KeyDownEvent>);
    STATIC_REQUIRE(Traits::Timestamped<MouseMoveEvent>);
    STATIC_REQUIRE(Traits::Timestamped<TouchEvent>);
    STATIC_REQUIRE(Traits::Timestamped<TouchGestureEvent>);
    STATIC_REQUIRE(Traits::Timestamped<PenEvent>);
    STATIC_REQUIRE(Traits::Timestamped<ImeCompositionEvent>);
    STATIC_REQUIRE(Traits::Timestamped<GamepadStateEvent>);
    STATIC_REQUIRE(Traits::Timestamped<TimerTickEvent>);
    STATIC_REQUIRE(Traits::Timestamped<WindowResizeEvent>);

    STATIC_REQUIRE_FALSE(Traits::Timestamped<WindowFocusEvent>);
    STATIC_REQUIRE_FALSE(Traits::Timestamped<DisplayChangeEvent>);
    STATIC_REQUIRE_FALSE(Traits::Timestamped<PowerStateChangeEvent>);
    STATIC_REQUIRE_FALSE(Traits::Timestamped<FileCreatedEvent>);
    STATIC_REQUIRE_FALSE(Traits::Timestamped<ClipboardUpdateEvent>);
    STATIC_REQUIRE_FALSE(Traits::Timestamped<AppearanceChangeEvent>);
}

// =========================================================================
// Section 6 — Mixin defaults + base-class composition
// =========================================================================

TEST_CASE("Mixin fields default-construct to documented sentinels", AUTO_TAG) {
    STATIC_REQUIRE(WindowResizeEvent{}.windowId  == WindowId::Invalid);
    STATIC_REQUIRE(WindowResizeEvent{}.timestamp == 0);
    STATIC_REQUIRE(KeyDownEvent{}.windowId       == WindowId::Invalid);
    STATIC_REQUIRE(KeyDownEvent{}.timestamp      == 0);
    STATIC_REQUIRE(WindowCreateEvent{}.windowId  == WindowId::Invalid);
    STATIC_REQUIRE(GamepadStateEvent{}.timestamp == 0);
}

TEST_CASE("KeyEventBase mixin fields propagate to derived events", AUTO_TAG) {
    KeyDownEvent kd{};
    kd.windowId  = WindowId{3};
    kd.timestamp = 1'234'000ULL;
    kd.code      = KeyCode::Enter;
    kd.repeat    = true;
    REQUIRE(kd.code      == KeyCode::Enter);
    REQUIRE(kd.repeat);
    REQUIRE(kd.windowId  == WindowId{3});
    REQUIRE(kd.timestamp == 1'234'000ULL);
}

TEST_CASE("FileSpecificEvent base inherits the marker but no mixin", AUTO_TAG) {
    STATIC_REQUIRE_FALSE(Traits::WindowScoped<FileCreatedEvent>);
    STATIC_REQUIRE_FALSE(Traits::Timestamped<FileCreatedEvent>);
    FileCreatedEvent c{};
    c.path = "/tmp/x";
    REQUIRE(c.path == "/tmp/x");
}

// =========================================================================
// Section 7 — Trivial-copyability holds for plain payloads (no std::string)
// =========================================================================

TEST_CASE("Plain payloads remain trivially copyable", AUTO_TAG) {
    STATIC_REQUIRE(std::is_trivially_copyable_v<WindowResizeEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<WindowFocusEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<KeyDownEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<MouseButtonEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<ScrollEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<TouchEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<PenEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<TimerTickEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<GamepadStateEvent>);
}

// =========================================================================
// Section 8 — Reflection traits — type name / availability / platforms
// =========================================================================

TEST_CASE("Traits::PayloadTypeName returns the unqualified struct name", AUTO_TAG) {
    STATIC_REQUIRE(Traits::PayloadTypeName<WindowResizeEvent>() == "WindowResizeEvent");
    STATIC_REQUIRE(Traits::PayloadTypeName<KeyDownEvent>()      == "KeyDownEvent");
    STATIC_REQUIRE(Traits::PayloadTypeName<UrlOpenRequestEvent>()== "UrlOpenRequestEvent");
}

TEST_CASE("Traits::AvailableOn filters by Platform::OnPlatform annotation", AUTO_TAG) {
    // Portable payloads — present on every platform.
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, PlatformBit::Windows>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, PlatformBit::macOS>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, PlatformBit::Linux_X11>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, PlatformBit::Linux_Wayland>);

    // Linux-only — both X11 and Wayland.
    STATIC_REQUIRE(Traits::AvailableOn<WindowExposedEvent, PlatformBit::Linux_X11>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowExposedEvent, PlatformBit::Linux_Wayland>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowExposedEvent, PlatformBit::Windows>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowExposedEvent, PlatformBit::macOS>);

    // Wayland-only.
    STATIC_REQUIRE(Traits::AvailableOn<WindowScaleChangeEvent, PlatformBit::Linux_Wayland>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowScaleChangeEvent, PlatformBit::Linux_X11>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowScaleChangeEvent, PlatformBit::Windows>);

    // Win32-only.
    STATIC_REQUIRE(Traits::AvailableOn<WindowDwmCompositionChangeEvent, PlatformBit::Windows>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowDwmCompositionChangeEvent, PlatformBit::Linux_X11>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowDwmCompositionChangeEvent, PlatformBit::macOS>);

    // Linux-only Selection (PRIMARY) and Win32-only SessionUserSwitch.
    STATIC_REQUIRE(Traits::AvailableOn<SelectionUpdateEvent,    PlatformBit::Linux_X11>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<SelectionUpdateEvent, PlatformBit::Windows>);
    STATIC_REQUIRE(Traits::AvailableOn<SessionUserSwitchEvent, PlatformBit::Windows>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<SessionUserSwitchEvent, PlatformBit::macOS>);
}

TEST_CASE("Traits::PlatformsOf reports the raw bit set", AUTO_TAG) {
    STATIC_REQUIRE(Traits::PlatformsOf<WindowResizeEvent>()                == PlatformBit_All);
    STATIC_REQUIRE(Traits::PlatformsOf<WindowExposedEvent>()               == PlatformBit_Linux);
    STATIC_REQUIRE(Traits::PlatformsOf<WindowScaleChangeEvent>()           == PlatformBit::Linux_Wayland);
    STATIC_REQUIRE(Traits::PlatformsOf<WindowDwmCompositionChangeEvent>()  == PlatformBit::Windows);
    STATIC_REQUIRE(Traits::PlatformsOf<SessionUserSwitchEvent>()           == PlatformBit::Windows);
}

// =========================================================================
// Section 9 — TouchGestureEvent::Kind nested enum
// =========================================================================

TEST_CASE("TouchGestureEvent carries a typed Kind subenum", AUTO_TAG) {
    using GK = TouchGestureEvent::Kind;
    STATIC_REQUIRE(Traits::SequentialEnum<GK>);
    TouchGestureEvent g{};
    g.gesture = GK::Pinch;
    g.scale   = 1.5f;
    REQUIRE(g.gesture == GK::Pinch);
    REQUIRE(g.scale   == 1.5f);
}

// =========================================================================
// Section 10 — SystemEvent variant + cross-cutting accessors
// =========================================================================

TEST_CASE("SystemEvent is a std::variant whose alternatives are the payload types", AUTO_TAG) {
    STATIC_REQUIRE(std::variant_size_v<SystemEvent> > 0);

    SystemEvent ev{WindowResizeEvent{}};
    REQUIRE(std::holds_alternative<WindowResizeEvent>(ev));
    ev = KeyDownEvent{};
    REQUIRE(std::holds_alternative<KeyDownEvent>(ev));
    ev = DisplayChangeEvent{};
    REQUIRE(std::holds_alternative<DisplayChangeEvent>(ev));
}

TEST_CASE("NameOf(SystemEvent) returns the active alternative's type name", AUTO_TAG) {
    SystemEvent ev{WindowResizeEvent{}};
    REQUIRE(Mashiro::NameOf(ev) == "WindowResizeEvent");

    ev = KeyDownEvent{};
    REQUIRE(Mashiro::NameOf(ev) == "KeyDownEvent");

    ev = DisplayChangeEvent{};
    REQUIRE(Mashiro::NameOf(ev) == "DisplayChangeEvent");
}

TEST_CASE("WindowOf(SystemEvent) returns the windowId only for window-scoped events", AUTO_TAG) {
    KeyDownEvent kd{};
    kd.windowId = WindowId{42};
    SystemEvent ev{kd};
    REQUIRE(Mashiro::WindowOf(ev) == std::optional<WindowId>{WindowId{42}});

    ev = DisplayChangeEvent{};
    REQUIRE(Mashiro::WindowOf(ev) == std::nullopt);

    ev = ClipboardUpdateEvent{};
    REQUIRE(Mashiro::WindowOf(ev) == std::nullopt);
}

TEST_CASE("TimestampOf(SystemEvent) returns the ns timestamp only when carried", AUTO_TAG) {
    MouseMoveEvent mm{};
    mm.timestamp = 9'999'888'777ULL;
    SystemEvent ev{mm};
    REQUIRE(Mashiro::TimestampOf(ev) == std::optional<uint64_t>{9'999'888'777ULL});

    ev = WindowFocusEvent{};
    REQUIRE(Mashiro::TimestampOf(ev) == std::nullopt);

    ev = DisplayChangeEvent{};
    REQUIRE(Mashiro::TimestampOf(ev) == std::nullopt);
}

// =========================================================================
// Section 11 — Convention-based bookkeep dispatch
//
// `DispatchBookkeep<M>(mgr, ev)` visits the active alternative and, for any
// payload type `P` for which `mgr.On(p)` is well-formed, instantiates and
// calls the matching arm. Payloads the manager does not handle are pruned
// at compile time via `if constexpr`, so dispatch is zero-cost on
// alternatives outside the manager's responsibility.
// =========================================================================

namespace {

    struct ResizeBookkeeper {
        int   resizeCount = 0;
        int   focusCount  = 0;
        ivec2 lastSize{};
        bool  lastFocused = false;

        void On(const WindowResizeEvent& ev) noexcept {
            ++resizeCount;
            lastSize = ev.size;
        }
        void On(const WindowFocusEvent& ev) noexcept {
            ++focusCount;
            lastFocused = ev.focused;
        }
    };

    struct InertManager {
        int callCount = 0;
    };

} // namespace

TEST_CASE("HandlesBookkeep concept is purely structural", AUTO_TAG) {
    using namespace Traits;
    STATIC_REQUIRE(HandlesBookkeep<ResizeBookkeeper, WindowResizeEvent>);
    STATIC_REQUIRE(HandlesBookkeep<ResizeBookkeeper, WindowFocusEvent>);

    STATIC_REQUIRE_FALSE(HandlesBookkeep<ResizeBookkeeper, KeyDownEvent>);
    STATIC_REQUIRE_FALSE(HandlesBookkeep<ResizeBookkeeper, DisplayChangeEvent>);
    STATIC_REQUIRE_FALSE(HandlesBookkeep<ResizeBookkeeper, MouseMoveEvent>);

    STATIC_REQUIRE_FALSE(HandlesBookkeep<InertManager, WindowResizeEvent>);
    STATIC_REQUIRE_FALSE(HandlesBookkeep<InertManager, KeyDownEvent>);

    STATIC_REQUIRE_FALSE(HandlesBookkeep<ResizeBookkeeper, int>);
}

TEST_CASE("DispatchBookkeep routes payloads to matching On overloads", AUTO_TAG) {
    ResizeBookkeeper mgr{};

    WindowResizeEvent rz{};
    rz.size = {800, 600};
    SystemEvent ev{rz};
    DispatchBookkeep(mgr, ev);
    REQUIRE(mgr.resizeCount == 1);
    REQUIRE(mgr.lastSize.x  == 800);
    REQUIRE(mgr.lastSize.y  == 600);
    REQUIRE(mgr.focusCount  == 0);

    WindowFocusEvent fc{};
    fc.focused = true;
    ev = fc;
    DispatchBookkeep(mgr, ev);
    REQUIRE(mgr.focusCount   == 1);
    REQUIRE(mgr.lastFocused  == true);
    REQUIRE(mgr.resizeCount  == 1);
}

TEST_CASE("DispatchBookkeep ignores payloads the manager does not handle", AUTO_TAG) {
    ResizeBookkeeper mgr{};

    SystemEvent ev{KeyDownEvent{}};
    DispatchBookkeep(mgr, ev);
    ev = DisplayChangeEvent{};
    DispatchBookkeep(mgr, ev);
    ev = MouseMoveEvent{};
    DispatchBookkeep(mgr, ev);

    REQUIRE(mgr.resizeCount == 0);
    REQUIRE(mgr.focusCount  == 0);
}

TEST_CASE("DispatchBookkeep on a manager with no handlers compiles and is a no-op", AUTO_TAG) {
    InertManager mgr{};
    SystemEvent ev{WindowResizeEvent{}};
    DispatchBookkeep(mgr, ev);
    ev = KeyDownEvent{};
    DispatchBookkeep(mgr, ev);
    REQUIRE(mgr.callCount == 0);
}

// clang-format on
