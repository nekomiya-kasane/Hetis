#include <Mashiro/Platform/SystemEvent.h>

#include <catch2/catch_test_macros.hpp>

#include <Support/Meta.h>

#include <type_traits>

// clang-format off

using namespace Mashiro;

// =========================================================================
// CRTP `kind` is auto-populated from the [[=PayloadFor{...}]] annotation.
// =========================================================================

TEST_CASE("EventPayload kind is initialised from PayloadFor annotation", AUTO_TAG) {
    STATIC_REQUIRE(WindowCreateEvent{}.kind == EventKind::WindowCreate);
    STATIC_REQUIRE(WindowResizeEvent{}.kind == EventKind::WindowResize);
    STATIC_REQUIRE(WindowCloseEvent{}.kind == EventKind::WindowClose);
    STATIC_REQUIRE(KeyDownEvent{}.kind == EventKind::InputKeyDown);
    STATIC_REQUIRE(KeyUpEvent{}.kind == EventKind::InputKeyUp);
    STATIC_REQUIRE(MouseMoveEvent{}.kind == EventKind::InputMouseMove);
    STATIC_REQUIRE(MouseButtonEvent{}.kind == EventKind::InputMouseButton);
    STATIC_REQUIRE(ScrollEvent{}.kind == EventKind::InputScroll);
    STATIC_REQUIRE(FileCreatedEvent{}.kind == EventKind::FileCreated);
    STATIC_REQUIRE(FileRenamedEvent{}.kind == EventKind::FileRenamed);
    STATIC_REQUIRE(GamepadStateEvent{}.kind == EventKind::GamepadState);
    STATIC_REQUIRE(WindowDwmCompositionChangeEvent{}.kind ==
                   EventKind::WindowDwmCompositionChange);
    STATIC_REQUIRE(UrlOpenRequestEvent{}.kind == EventKind::UrlOpenRequest);
}

TEST_CASE("Designated initialisers leave kind intact", AUTO_TAG) {
    constexpr WindowResizeEvent ev{{.windowId = 7, .sequence = 42}};
    STATIC_REQUIRE(ev.kind == EventKind::WindowResize);
    STATIC_REQUIRE(ev.windowId == 7);
    STATIC_REQUIRE(ev.sequence == 42);
}

// =========================================================================
// EventHeader stays a 24-byte standard-layout reference across all builds.
// =========================================================================

TEST_CASE("EventHeader layout is fixed at 24 bytes / 8-byte aligned", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(EventHeader) == 24);
    STATIC_REQUIRE(alignof(EventHeader) == 8);
    STATIC_REQUIRE(std::is_standard_layout_v<EventHeader>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<EventHeader>);
}

TEST_CASE("Payload structs remain trivially copyable when their fields are", AUTO_TAG) {
    STATIC_REQUIRE(std::is_trivially_copyable_v<WindowResizeEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<KeyDownEvent>);
}

// =========================================================================
// Reflection traits — kind ↔ payload binding round-trip and platform filter.
// =========================================================================

TEST_CASE("Traits::KindOf recovers the payload's bound EventKind", AUTO_TAG) {
    STATIC_REQUIRE(Traits::KindOf<WindowResizeEvent>() == EventKind::WindowResize);
    STATIC_REQUIRE(Traits::KindOf<KeyDownEvent>()      == EventKind::InputKeyDown);
    STATIC_REQUIRE(Traits::KindOf<DragDropEvent>()     == EventKind::DragDrop);
}

TEST_CASE("Traits::AvailableOn filters by Platform::OnPlatform annotation", AUTO_TAG) {
    // Portable payloads are available everywhere.
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, PlatformBit::Windows>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, PlatformBit::macOS>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowResizeEvent, PlatformBit::Linux_X11>);

    // Linux-only payload (X11 expose).
    STATIC_REQUIRE(Traits::AvailableOn<WindowExposedEvent, PlatformBit::Linux_X11>);
    STATIC_REQUIRE(Traits::AvailableOn<WindowExposedEvent, PlatformBit::Linux_Wayland>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowExposedEvent, PlatformBit::Windows>);

    // Wayland-only payload.
    STATIC_REQUIRE(Traits::AvailableOn<WindowScaleChangeEvent, PlatformBit::Linux_Wayland>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowScaleChangeEvent, PlatformBit::Windows>);

    // Win32-only payload.
    STATIC_REQUIRE(Traits::AvailableOn<WindowDwmCompositionChangeEvent, PlatformBit::Windows>);
    STATIC_REQUIRE_FALSE(Traits::AvailableOn<WindowDwmCompositionChangeEvent,
                                             PlatformBit::Linux_X11>);
}

TEST_CASE("Traits::EventKindName returns the source identifier", AUTO_TAG) {
    STATIC_REQUIRE(Traits::EventKindName<EventKind::WindowResize>() == "WindowResize");
    STATIC_REQUIRE(Traits::EventKindName<EventKind::InputKeyDown>() == "InputKeyDown");
}

// clang-format on
