// SPDX-License-Identifier: MIT
//
// Tests for Mashiro::Platform::SystemEvent — the reflection-driven event
// vocabulary. Verifies the kind/payload binding, layout invariants, the
// CRTP NSDMI, the platform-filter trait, and end-to-end completeness of
// the EventKind ↔ payload mapping.

#include <Mashiro/Platform/SystemEvent.h>

#include <catch2/catch_test_macros.hpp>

#include <Support/Meta.h>

#include <array>
#include <string_view>
#include <type_traits>

// clang-format off

using namespace Mashiro;

// =========================================================================
// Section 1 — EventKind: sequential, dense, kEventKindCount agreement
// =========================================================================

TEST_CASE("EventKind is a SequentialEnum and fits in one byte", AUTO_TAG) {
    STATIC_REQUIRE(Traits::SequentialEnum<EventKind>);
    STATIC_REQUIRE(sizeof(EventKind) == 1);
    STATIC_REQUIRE(kEventKindCount == Traits::EnumeratorsCount<EventKind>);
    STATIC_REQUIRE(kEventKindCount > 0);
    STATIC_REQUIRE(static_cast<size_t>(EventKind::WindowCreate) == 0);
}

TEST_CASE("EventKind enumerator names round-trip via reflection", AUTO_TAG) {
    STATIC_REQUIRE(Traits::EventKindName<EventKind::WindowCreate>()    == "WindowCreate");
    STATIC_REQUIRE(Traits::EventKindName<EventKind::WindowResize>()    == "WindowResize");
    STATIC_REQUIRE(Traits::EventKindName<EventKind::InputKeyDown>()    == "InputKeyDown");
    STATIC_REQUIRE(Traits::EventKindName<EventKind::InputMouseMove>()  == "InputMouseMove");
    STATIC_REQUIRE(Traits::EventKindName<EventKind::FileRenamed>()     == "FileRenamed");
    STATIC_REQUIRE(Traits::EventKindName<EventKind::DeviceChange>()    == "DeviceChange");
}

TEST_CASE("Generic Traits::EnumeratorName covers every payload-categorisation enum", AUTO_TAG) {
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

TEST_CASE("EventFlag, PenButton, FileChangeFlags, DragKind, PlatformBit are BitfieldEnums", AUTO_TAG) {
    STATIC_REQUIRE(Traits::BitfieldEnum<EventFlag>);
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
// Section 5 — EventHeader layout (24 bytes, 8-byte aligned, std-layout)
// =========================================================================

TEST_CASE("EventHeader layout is fixed at 24 bytes / 8-byte aligned", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(EventHeader)  == 24);
    STATIC_REQUIRE(alignof(EventHeader) == 8);
    STATIC_REQUIRE(std::is_standard_layout_v<EventHeader>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<EventHeader>);
}

TEST_CASE("EventHeader compares structurally", AUTO_TAG) {
    constexpr EventHeader a{.kind = EventKind::WindowCreate, .windowId = 1};
    constexpr EventHeader b{.kind = EventKind::WindowCreate, .windowId = 1};
    constexpr EventHeader c{.kind = EventKind::WindowCreate, .windowId = 2};
    STATIC_REQUIRE(a == b);
    STATIC_REQUIRE_FALSE(a == c);
}

// =========================================================================
// Section 6 — CRTP `kind` is auto-populated from PayloadFor
// =========================================================================

TEST_CASE("EventPayload kind is initialised from PayloadFor annotation", AUTO_TAG) {
    // Window lifecycle
    STATIC_REQUIRE(WindowCreateEvent{}.kind                    == EventKind::WindowCreate);
    STATIC_REQUIRE(WindowResizeEvent{}.kind                    == EventKind::WindowResize);
    STATIC_REQUIRE(WindowCloseEvent{}.kind                     == EventKind::WindowClose);
    STATIC_REQUIRE(WindowDestroyEvent{}.kind                   == EventKind::WindowDestroy);
    STATIC_REQUIRE(WindowMoveEvent{}.kind                      == EventKind::WindowMove);
    STATIC_REQUIRE(WindowFocusEvent{}.kind                     == EventKind::WindowFocus);
    STATIC_REQUIRE(WindowDpiChangeEvent{}.kind                 == EventKind::WindowDpiChange);
    STATIC_REQUIRE(WindowOcclusionChangeEvent{}.kind           == EventKind::WindowOcclusionChange);
    STATIC_REQUIRE(WindowExposedEvent{}.kind                   == EventKind::WindowExposed);
    STATIC_REQUIRE(WindowDwmCompositionChangeEvent{}.kind      == EventKind::WindowDwmCompositionChange);

    // Keyboard
    STATIC_REQUIRE(KeyDownEvent{}.kind              == EventKind::InputKeyDown);
    STATIC_REQUIRE(KeyUpEvent{}.kind                == EventKind::InputKeyUp);
    STATIC_REQUIRE(CharEvent{}.kind                 == EventKind::InputChar);
    STATIC_REQUIRE(KeyboardLayoutChangeEvent{}.kind == EventKind::InputKeyboardLayoutChange);

    // Pointer / touch / pen
    STATIC_REQUIRE(MouseEnterEvent{}.kind     == EventKind::InputMouseEnter);
    STATIC_REQUIRE(MouseMoveEvent{}.kind      == EventKind::InputMouseMove);
    STATIC_REQUIRE(MouseButtonEvent{}.kind    == EventKind::InputMouseButton);
    STATIC_REQUIRE(ScrollEvent{}.kind         == EventKind::InputScroll);
    STATIC_REQUIRE(RawMouseMotionEvent{}.kind == EventKind::InputRawMouseMotion);
    STATIC_REQUIRE(TouchEvent{}.kind          == EventKind::InputTouch);
    STATIC_REQUIRE(TouchGestureEvent{}.kind   == EventKind::InputTouchGesture);
    STATIC_REQUIRE(PenEvent{}.kind            == EventKind::InputPen);
    STATIC_REQUIRE(PenProximityEvent{}.kind   == EventKind::InputPenProximity);
    STATIC_REQUIRE(PenButtonEvent{}.kind      == EventKind::InputPenButton);

    // IME / clipboard / drag-drop
    STATIC_REQUIRE(ImeCommitEvent{}.kind        == EventKind::ImeCommit);
    STATIC_REQUIRE(ImeCandidateListEvent{}.kind == EventKind::ImeCandidateList);
    STATIC_REQUIRE(ClipboardUpdateEvent{}.kind  == EventKind::ClipboardUpdate);
    STATIC_REQUIRE(SelectionUpdateEvent{}.kind  == EventKind::SelectionUpdate);
    STATIC_REQUIRE(DragDropEvent{}.kind         == EventKind::DragDrop);

    // Display / power / accessibility / gamepad / files / misc
    STATIC_REQUIRE(DisplayHdrChangeEvent{}.kind         == EventKind::DisplayHdrChange);
    STATIC_REQUIRE(PowerStateChangeEvent{}.kind         == EventKind::PowerStateChange);
    STATIC_REQUIRE(AccessibilityScreenReaderEvent{}.kind== EventKind::AccessibilityScreenReader);
    STATIC_REQUIRE(GamepadStateEvent{}.kind             == EventKind::GamepadState);
    STATIC_REQUIRE(FileCreatedEvent{}.kind              == EventKind::FileCreated);
    STATIC_REQUIRE(FileRenamedEvent{}.kind              == EventKind::FileRenamed);
    STATIC_REQUIRE(FileWatchOverflowEvent{}.kind        == EventKind::FileWatchOverflow);
    STATIC_REQUIRE(UrlOpenRequestEvent{}.kind           == EventKind::UrlOpenRequest);
    STATIC_REQUIRE(TimerTickEvent{}.kind                == EventKind::TimerTick);
    STATIC_REQUIRE(DeviceChangeEvent{}.kind             == EventKind::DeviceChange);
}

TEST_CASE("Designated initialisers leave kind intact", AUTO_TAG) {
    constexpr WindowResizeEvent ev{{.windowId = 7, .sequence = 42, .timestamp = 1'000'000ULL}};
    STATIC_REQUIRE(ev.kind == EventKind::WindowResize);
    STATIC_REQUIRE(ev.windowId  == 7);
    STATIC_REQUIRE(ev.sequence  == 42);
    STATIC_REQUIRE(ev.timestamp == 1'000'000ULL);
}

TEST_CASE("KeyEventBase CRTP propagates the derived kind", AUTO_TAG) {
    STATIC_REQUIRE(KeyDownEvent{}.kind == EventKind::InputKeyDown);
    STATIC_REQUIRE(KeyUpEvent{}.kind   == EventKind::InputKeyUp);

    constexpr KeyDownEvent kd{{{.windowId = 3}, .code = KeyCode::Enter, .repeat = true}};
    STATIC_REQUIRE(kd.kind     == EventKind::InputKeyDown);
    STATIC_REQUIRE(kd.code     == KeyCode::Enter);
    STATIC_REQUIRE(kd.repeat);
    STATIC_REQUIRE(kd.windowId == 3);
}

TEST_CASE("FileEventBase CRTP applies to file-system events", AUTO_TAG) {
    STATIC_REQUIRE(FileCreatedEvent{}.kind          == EventKind::FileCreated);
    STATIC_REQUIRE(FileDeletedEvent{}.kind          == EventKind::FileDeleted);
    STATIC_REQUIRE(FileModifiedEvent{}.kind         == EventKind::FileModified);
    STATIC_REQUIRE(FileAttributesChangedEvent{}.kind== EventKind::FileAttributesChanged);
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
// Section 8 — Reflection traits — kind / type-name / availability
// =========================================================================

TEST_CASE("Traits::KindOf<T>() agrees with the value injected into kind", AUTO_TAG) {
    STATIC_REQUIRE(Traits::KindOf<WindowResizeEvent>() == EventKind::WindowResize);
    STATIC_REQUIRE(Traits::KindOf<KeyDownEvent>()      == EventKind::InputKeyDown);
    STATIC_REQUIRE(Traits::KindOf<DragDropEvent>()     == EventKind::DragDrop);
    STATIC_REQUIRE(Traits::KindOf<TimerTickEvent>()    == EventKind::TimerTick);
    STATIC_REQUIRE(Traits::KindOf<TimerTickEvent>()    == TimerTickEvent{}.kind);
}

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
    // Portable → Mask of all known platforms.
    STATIC_REQUIRE(Traits::PlatformsOf<WindowResizeEvent>() == PlatformBit_All);
    STATIC_REQUIRE(Traits::PlatformsOf<WindowExposedEvent>()           == PlatformBit_Linux);
    STATIC_REQUIRE(Traits::PlatformsOf<WindowScaleChangeEvent>()       == PlatformBit::Linux_Wayland);
    STATIC_REQUIRE(Traits::PlatformsOf<WindowDwmCompositionChangeEvent>() == PlatformBit::Windows);
    STATIC_REQUIRE(Traits::PlatformsOf<SessionUserSwitchEvent>()       == PlatformBit::Windows);
}

// =========================================================================
// Section 9 — TouchGestureEvent::Kind nested enum
// =========================================================================

TEST_CASE("TouchGestureEvent carries a typed Kind subenum", AUTO_TAG) {
    using GK = TouchGestureEvent::Kind;
    STATIC_REQUIRE(Traits::SequentialEnum<GK>);
    constexpr TouchGestureEvent g{{}, .gesture = GK::Pinch, .scale = 1.5f};
    STATIC_REQUIRE(g.kind    == EventKind::InputTouchGesture); // outer discriminator
    STATIC_REQUIRE(g.gesture == GK::Pinch);                    // gesture sub-tag
    STATIC_REQUIRE(g.scale   == 1.5f);
}

// =========================================================================
// Section 10 — Completeness sanity
//
// `SystemEvent.h` already carries `static_assert(Detail::AllKindsCovered())`
// at file scope, so a missing payload binding is a compile error before this
// test ever runs. We instead spot-check via `Traits::KindOf<T>()` for a
// representative slice — if any kind ever loses its payload, KindOf fails to
// instantiate and surfaces here.
// =========================================================================

TEST_CASE("Spot-check KindOf round-trip across the EventKind taxonomy", AUTO_TAG) {
    STATIC_REQUIRE(Traits::KindOf<WindowCreateEvent>()      == EventKind::WindowCreate);
    STATIC_REQUIRE(Traits::KindOf<KeyDownEvent>()           == EventKind::InputKeyDown);
    STATIC_REQUIRE(Traits::KindOf<ScrollEvent>()            == EventKind::InputScroll);
    STATIC_REQUIRE(Traits::KindOf<DragDropEvent>()          == EventKind::DragDrop);
    STATIC_REQUIRE(Traits::KindOf<DisplayHdrChangeEvent>()  == EventKind::DisplayHdrChange);
    STATIC_REQUIRE(Traits::KindOf<GamepadStateEvent>()      == EventKind::GamepadState);
    STATIC_REQUIRE(Traits::KindOf<FileWatchOverflowEvent>() == EventKind::FileWatchOverflow);
    STATIC_REQUIRE(Traits::KindOf<DeviceChangeEvent>()      == EventKind::DeviceChange);
}

// clang-format on
