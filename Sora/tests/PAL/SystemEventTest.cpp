/**
 * @file SystemEventTest.cpp
 * @brief Verify the reflection-driven PAL event vocabulary and structural bookkeeping dispatch.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/SystemEvent.h>

#include <catch2/catch_test_macros.hpp>

#include <Support/Meta.h>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

using namespace Sora::PAL;
using namespace Sora::PAL::Event;

TEST_CASE("Event flag enums satisfy the shared Sora bitfield concept", AUTO_TAG) {
    STATIC_REQUIRE(Sora::Concept::BitfieldEnum<PenButton>);
    STATIC_REQUIRE(Sora::Concept::BitfieldEnum<FileChangeFlags>);
    STATIC_REQUIRE(Sora::Concept::BitfieldEnum<DragKind>);
}

TEST_CASE("BitfieldEnum operators compose without explicit casts", AUTO_TAG) {
    constexpr auto rwx = PenButton::Tip | PenButton::Barrel | PenButton::Eraser;
    STATIC_REQUIRE((rwx & PenButton::Tip) == PenButton::Tip);
    STATIC_REQUIRE((rwx & PenButton::Barrel) == PenButton::Barrel);
    STATIC_REQUIRE((rwx & PenButton::Eraser) == PenButton::Eraser);

    constexpr auto allMask = Sora::Traits::BitfieldMask<PenButton>;
    STATIC_REQUIRE((allMask & PenButton::Tip) == PenButton::Tip);
    STATIC_REQUIRE((allMask & PenButton::Barrel) == PenButton::Barrel);
    STATIC_REQUIRE((allMask & PenButton::Eraser) == PenButton::Eraser);
}

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

TEST_CASE("KeyCode letters and digits map to ASCII code points", AUTO_TAG) {
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::A) == uint32_t{'A'});
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Z) == uint32_t{'Z'});
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Num0) == uint32_t{'0'});
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Num9) == uint32_t{'9'});
    // Function and named keys live above 256 so they don't collide with ASCII.
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::F1) == 256);
    STATIC_REQUIRE(static_cast<uint32_t>(KeyCode::Escape) > 256);
}

TEST_CASE("EventPayloadBase is an empty marker; mixins inherit it", AUTO_TAG) {
    STATIC_REQUIRE(std::is_empty_v<Event::EventPayloadBase>);
    STATIC_REQUIRE(std::is_base_of_v<Event::EventPayloadBase, WindowSpecificEvent>);
    STATIC_REQUIRE(std::is_base_of_v<Event::EventPayloadBase, TimestampedEvent>);
    STATIC_REQUIRE(WindowSpecificEvent{}.window == 0);
    STATIC_REQUIRE(TimestampedEvent{}.timestamp == 0);
}

TEST_CASE("SystemEventPayload concept identifies every payload, rejects anything else", AUTO_TAG) {
    STATIC_REQUIRE(Concept::SystemEventPayload<WindowResizeEvent>);
    STATIC_REQUIRE(Concept::SystemEventPayload<KeyDownEvent>);
    STATIC_REQUIRE(Concept::SystemEventPayload<DisplayChangeEvent>);
    STATIC_REQUIRE(Concept::SystemEventPayload<FileCreatedEvent>);
    STATIC_REQUIRE(Concept::SystemEventPayload<TimerTickEvent>);

    // Plain types are not payloads; anything that doesn't derive from the
    // marker base is excluded.
    STATIC_REQUIRE_FALSE(Concept::SystemEventPayload<int>);
    STATIC_REQUIRE_FALSE(Concept::SystemEventPayload<Modifiers>);
    STATIC_REQUIRE_FALSE(Concept::SystemEventPayload<Event::DragPayloadInfo>);
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
    STATIC_REQUIRE(Concept::WindowScoped<WindowResizeEvent>);
    STATIC_REQUIRE(Concept::WindowScoped<KeyDownEvent>);
    STATIC_REQUIRE(Concept::WindowScoped<MouseMoveEvent>);
    STATIC_REQUIRE(Concept::WindowScoped<DragDropEvent>);
    STATIC_REQUIRE(Concept::WindowScoped<ImeCommitEvent>);

    STATIC_REQUIRE_FALSE(Concept::WindowScoped<DisplayChangeEvent>);
    STATIC_REQUIRE_FALSE(Concept::WindowScoped<PowerStateChangeEvent>);
    STATIC_REQUIRE_FALSE(Concept::WindowScoped<GamepadStateEvent>);
    STATIC_REQUIRE_FALSE(Concept::WindowScoped<FileCreatedEvent>);
    STATIC_REQUIRE_FALSE(Concept::WindowScoped<ClipboardUpdateEvent>);
    STATIC_REQUIRE_FALSE(Concept::WindowScoped<AppearanceChangeEvent>);
}

TEST_CASE("Timestamped concept identifies time-sensitive payloads", AUTO_TAG) {
    STATIC_REQUIRE(Concept::Timestamped<KeyDownEvent>);
    STATIC_REQUIRE(Concept::Timestamped<MouseMoveEvent>);
    STATIC_REQUIRE(Concept::Timestamped<TouchEvent>);
    STATIC_REQUIRE(Concept::Timestamped<TouchGestureEvent>);
    STATIC_REQUIRE(Concept::Timestamped<PenEvent>);
    STATIC_REQUIRE(Concept::Timestamped<ImeCompositionEvent>);
    STATIC_REQUIRE(Concept::Timestamped<GamepadStateEvent>);
    STATIC_REQUIRE(Concept::Timestamped<TimerTickEvent>);
    STATIC_REQUIRE(Concept::Timestamped<WindowResizeEvent>);

    STATIC_REQUIRE_FALSE(Concept::Timestamped<WindowFocusEvent>);
    STATIC_REQUIRE_FALSE(Concept::Timestamped<DisplayChangeEvent>);
    STATIC_REQUIRE_FALSE(Concept::Timestamped<PowerStateChangeEvent>);
    STATIC_REQUIRE_FALSE(Concept::Timestamped<FileCreatedEvent>);
    STATIC_REQUIRE_FALSE(Concept::Timestamped<ClipboardUpdateEvent>);
    STATIC_REQUIRE_FALSE(Concept::Timestamped<AppearanceChangeEvent>);
}

TEST_CASE("Mixin fields default-construct to documented sentinels", AUTO_TAG) {
    STATIC_REQUIRE(WindowResizeEvent{}.window == 0);
    STATIC_REQUIRE(WindowResizeEvent{}.timestamp == 0);
    STATIC_REQUIRE(KeyDownEvent{}.window == 0);
    STATIC_REQUIRE(KeyDownEvent{}.timestamp == 0);
    STATIC_REQUIRE(WindowCreateEvent{}.window == 0);
    STATIC_REQUIRE(GamepadStateEvent{}.timestamp == 0);
}

TEST_CASE("KeyEventBase mixin fields propagate to derived events", AUTO_TAG) {
    KeyDownEvent kd{};
    kd.window = 3;
    kd.timestamp = 1'234'000ULL;
    kd.code = KeyCode::Enter;
    kd.repeat = true;
    REQUIRE(kd.code == KeyCode::Enter);
    REQUIRE(kd.repeat);
    REQUIRE(kd.window == 3);
    REQUIRE(kd.timestamp == 1'234'000ULL);
}

TEST_CASE("FileSpecificEvent base inherits the marker but no mixin", AUTO_TAG) {
    STATIC_REQUIRE_FALSE(Concept::WindowScoped<FileCreatedEvent>);
    STATIC_REQUIRE_FALSE(Concept::Timestamped<FileCreatedEvent>);
    FileCreatedEvent c{};
    c.path = "/tmp/x";
    REQUIRE(c.path == "/tmp/x");
}

TEST_CASE("Platform object identities use one portable integer representation", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<decltype(WindowSpecificEvent{}.window), uint64_t>);
    STATIC_REQUIRE(std::same_as<decltype(DisplayChangeEvent{}.display), uint64_t>);
    STATIC_REQUIRE(std::same_as<decltype(KeyDownEvent{}.device), uint64_t>);
    STATIC_REQUIRE(std::same_as<decltype(TouchEvent{}.contact), uint64_t>);
    STATIC_REQUIRE(WindowSpecificEvent{}.window == 0);
    STATIC_REQUIRE(DisplayChangeEvent{}.display == 0);
    STATIC_REQUIRE(KeyDownEvent{}.device == 0);
    STATIC_REQUIRE(TouchEvent{}.contact == 0);

    WindowResizeEvent resize{};
    resize.window = 17;
    DisplayChangeEvent display{};
    display.display = 17;
    REQUIRE(resize.window == display.display);
}

TEST_CASE("Event geometry distinguishes points, vectors, and rectangles", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<decltype(WindowMoveEvent{}.position), Sora::Math::Point2i>);
    STATIC_REQUIRE(std::same_as<decltype(WindowExposedEvent{}.damage), Sora::Math::Rect2i>);
    STATIC_REQUIRE(std::same_as<decltype(ImePreeditCursorEvent{}.caretBounds), Sora::Math::Rect2i>);
    STATIC_REQUIRE(std::same_as<decltype(MouseMoveEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(MouseMoveEvent{}.delta), Sora::Math::Vec2f>);
    STATIC_REQUIRE(std::same_as<decltype(MouseButtonEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(ScrollEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(ScrollEvent{}.delta), Sora::Math::Vec2f>);
    STATIC_REQUIRE(std::same_as<decltype(TouchEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(TouchEvent{}.size), Sora::Math::Vec2f>);
    STATIC_REQUIRE(std::same_as<decltype(TouchGestureEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(TouchGestureEvent{}.delta), Sora::Math::Vec2f>);
    STATIC_REQUIRE(std::same_as<decltype(PenEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(DragEnterEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(DragOverEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(DragDropEvent{}.position), Sora::Math::Point2f>);
    STATIC_REQUIRE(std::same_as<decltype(DisplayChangeEvent{}.bounds), Sora::Math::Rect2i>);

    constexpr Sora::Math::Rect2i secondaryDisplay{Sora::Math::Point2i{-1920, 0}, Sora::Math::Vec2i{1920, 1080}};
    STATIC_REQUIRE(secondaryDisplay.Base() == Sora::Math::Point2i{-1920, 0});
    STATIC_REQUIRE(secondaryDisplay.Extent() == Sora::Math::Vec2i{1920, 1080});

    constexpr ImePreeditCursorEvent preedit{
        {},
        Sora::Math::Rect2i{Sora::Math::Point2i{-2, 12}, Sora::Math::Vec2i{2, 18}},
    };
    STATIC_REQUIRE(preedit.caretBounds.Base() == Sora::Math::Point2i{-2, 12});
    STATIC_REQUIRE(preedit.caretBounds.Extent() == Sora::Math::Vec2i{2, 18});
}

TEST_CASE("Plain payloads remain trivially copyable", AUTO_TAG) {
    STATIC_REQUIRE(std::is_trivially_copyable_v<WindowResizeEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<WindowFocusEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<KeyDownEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<MouseButtonEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<ScrollEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<TouchEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<PenEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<DisplayChangeEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<TimerTickEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<GamepadStateEvent>);
}

TEST_CASE("Meta::PayloadTypeName returns the unqualified struct name", AUTO_TAG) {
    STATIC_REQUIRE(Meta::PayloadTypeName<WindowResizeEvent>() == "WindowResizeEvent");
    STATIC_REQUIRE(Meta::PayloadTypeName<KeyDownEvent>() == "KeyDownEvent");
    STATIC_REQUIRE(Meta::PayloadTypeName<UrlOpenRequestEvent>() == "UrlOpenRequestEvent");
}

TEST_CASE("Availability reuses Platform operating-system and window-system vocabulary", AUTO_TAG) {
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, Sora::Platform::OperatingSystem::Windows>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, Sora::Platform::OperatingSystem::macOS>);

    STATIC_REQUIRE(Traits::AvailableOn<WindowExposedEvent, Sora::Platform::OperatingSystem::Linux,
                                       Sora::Platform::WindowSystem::X11>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowExposedEvent, Sora::Platform::OperatingSystem::Windows>);

    STATIC_REQUIRE(Traits::AvailableOn<WindowScaleChangeEvent, Sora::Platform::OperatingSystem::Linux,
                                       Sora::Platform::WindowSystem::Wayland>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowScaleChangeEvent, Sora::Platform::OperatingSystem::Linux,
                                             Sora::Platform::WindowSystem::X11>);

    STATIC_REQUIRE(Traits::AvailableOn<WindowDwmCompositionChangeEvent, Sora::Platform::OperatingSystem::Windows>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowDwmCompositionChangeEvent, Sora::Platform::OperatingSystem::Linux>);
    STATIC_REQUIRE(Meta::OperatingSystemOf<SessionUserSwitchEvent>() == Sora::Platform::OperatingSystem::Windows);
    STATIC_REQUIRE(Meta::WindowSystemOf<WindowScaleChangeEvent>() == Sora::Platform::WindowSystem::Wayland);
}

TEST_CASE("TouchGestureEvent carries a typed Kind subenum", AUTO_TAG) {
    using GK = TouchGestureEvent::Kind;
    STATIC_REQUIRE(Sora::Concept::SequentialEnum<GK>);
    TouchGestureEvent g{};
    g.gesture = GK::Pinch;
    g.scale = 1.5f;
    REQUIRE(g.gesture == GK::Pinch);
    REQUIRE(g.scale == 1.5f);
}

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
    REQUIRE(Sora::PAL::NameOf(ev) == "WindowResizeEvent");

    ev = KeyDownEvent{};
    REQUIRE(Sora::PAL::NameOf(ev) == "KeyDownEvent");

    ev = DisplayChangeEvent{};
    REQUIRE(Sora::PAL::NameOf(ev) == "DisplayChangeEvent");
}

TEST_CASE("WindowOf(SystemEvent) returns the window identity only for window-scoped events", AUTO_TAG) {
    KeyDownEvent kd{};
    kd.window = 42;
    SystemEvent ev{kd};
    REQUIRE(Sora::PAL::WindowOf(ev) == std::optional<uint64_t>{42});

    ev = DisplayChangeEvent{};
    REQUIRE(Sora::PAL::WindowOf(ev) == std::nullopt);

    ev = ClipboardUpdateEvent{};
    REQUIRE(Sora::PAL::WindowOf(ev) == std::nullopt);
}

TEST_CASE("TimestampOf(SystemEvent) returns the ns timestamp only when carried", AUTO_TAG) {
    MouseMoveEvent mm{};
    mm.timestamp = 9'999'888'777ULL;
    SystemEvent ev{mm};
    REQUIRE(Sora::PAL::TimestampOf(ev) == std::optional<uint64_t>{9'999'888'777ULL});

    ev = WindowFocusEvent{};
    REQUIRE(Sora::PAL::TimestampOf(ev) == std::nullopt);

    ev = DisplayChangeEvent{};
    REQUIRE(Sora::PAL::TimestampOf(ev) == std::nullopt);
}

namespace {

    struct ResizeBookkeeper {
        int resizeCount = 0;
        int focusCount = 0;
        Sora::Math::Vec2i lastSize{};
        bool lastFocused = false;

        void On(const WindowResizeEvent& ev) noexcept {
            ++resizeCount;
            lastSize = ev.size;
        }
        void On(const WindowFocusEvent& ev) noexcept {
            ++focusCount;
            lastFocused = ev.focused;
        }
    };

    struct ThrowingResizeBookkeeper {
        void On(const WindowResizeEvent&) {}
    };

    struct InertManager {
        int callCount = 0;
    };

} // namespace

TEST_CASE("HandlesBookkeeping concept accepts only noexcept structural handlers", AUTO_TAG) {
    using namespace Concept;
    STATIC_REQUIRE(HandlesBookkeeping<ResizeBookkeeper, WindowResizeEvent>);
    STATIC_REQUIRE(HandlesBookkeeping<ResizeBookkeeper, WindowFocusEvent>);

    STATIC_REQUIRE_FALSE(HandlesBookkeeping<ResizeBookkeeper, KeyDownEvent>);
    STATIC_REQUIRE_FALSE(HandlesBookkeeping<ResizeBookkeeper, DisplayChangeEvent>);
    STATIC_REQUIRE_FALSE(HandlesBookkeeping<ResizeBookkeeper, MouseMoveEvent>);

    STATIC_REQUIRE_FALSE(HandlesBookkeeping<ThrowingResizeBookkeeper, WindowResizeEvent>);
    STATIC_REQUIRE_FALSE(HandlesBookkeeping<InertManager, WindowResizeEvent>);
    STATIC_REQUIRE_FALSE(HandlesBookkeeping<InertManager, KeyDownEvent>);

    STATIC_REQUIRE_FALSE(HandlesBookkeeping<ResizeBookkeeper, int>);
}

TEST_CASE("DispatchBookkeeping routes payloads to matching On overloads", AUTO_TAG) {
    ResizeBookkeeper mgr{};

    WindowResizeEvent rz{};
    rz.size = {800, 600};
    SystemEvent ev{rz};
    DispatchBookkeeping(mgr, ev);
    REQUIRE(mgr.resizeCount == 1);
    REQUIRE(mgr.lastSize[0] == 800);
    REQUIRE(mgr.lastSize[1] == 600);
    REQUIRE(mgr.focusCount == 0);

    WindowFocusEvent fc{};
    fc.focused = true;
    ev = fc;
    DispatchBookkeeping(mgr, ev);
    REQUIRE(mgr.focusCount == 1);
    REQUIRE(mgr.lastFocused);
    REQUIRE(mgr.resizeCount == 1);
}

TEST_CASE("DispatchBookkeeping ignores payloads the manager does not handle", AUTO_TAG) {
    ResizeBookkeeper mgr{};

    SystemEvent ev{KeyDownEvent{}};
    DispatchBookkeeping(mgr, ev);
    ev = DisplayChangeEvent{};
    DispatchBookkeeping(mgr, ev);
    ev = MouseMoveEvent{};
    DispatchBookkeeping(mgr, ev);

    REQUIRE(mgr.resizeCount == 0);
    REQUIRE(mgr.focusCount == 0);
}

TEST_CASE("DispatchBookkeeping on a manager with no handlers compiles and is a no-op", AUTO_TAG) {
    InertManager mgr{};
    SystemEvent ev{WindowResizeEvent{}};
    DispatchBookkeeping(mgr, ev);
    ev = KeyDownEvent{};
    DispatchBookkeeping(mgr, ev);
    REQUIRE(mgr.callCount == 0);
}
