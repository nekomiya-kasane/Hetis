/**
 * @file PlatformBackendWindows.cpp
 * @brief Win32 readiness and native-message drain backend for the Platform thread.
 *
 * This translation unit is the Win32 leaf below @ref Mashiro::Platform::EventPump. It owns the OS wake handle,
 * folds the thread message queue, external wake, and stop request into one wait point, and drains Win32 messages
 * into the platform event ingress. Manager bookkeeping and subscriber broadcast stay above this layer.
 *
 * @ingroup Platform
 */
#include "Mashiro/Platform/PlatformBackend.h"
#include "Mashiro/Core/Unicode.h"
#include "Mashiro/Platform/Common.h"
#include "Mashiro/Platform/DisplayMetrics.h"
#include "Mashiro/Platform/EventEmission.h"
#include "Mashiro/Platform/Managers/WindowManager.h"

#include <cassert>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef PLATFORM_WINDOWS

#    include "Mashiro/Platform/SystemApiHeaders.h"

namespace Mashiro {

    namespace Backend {

        void Initialize() {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            OleInitialize(nullptr);
        }

    } /* namespace Backend */

} /* namespace Mashiro */

namespace Mashiro::Platform {

    namespace Detail {

        inline constexpr DWORD kNativeMessageMask = QS_ALLINPUT;
        inline constexpr DWORD kNativeWaitFlags = MWMO_INPUTAVAILABLE;
        inline constexpr DWORD kInfiniteWait = INFINITE;
        inline constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ull;
        inline constexpr std::uint32_t kExtendedScancodePrefix = 0xE000u;

        /** @brief Return true iff @p handle names a live Win32 handle. */
        [[nodiscard]] constexpr bool HasHandle(HANDLE handle) noexcept {
            return handle != nullptr;
        }

        /** @brief Set @p handle if it exists. */
        void Signal(HANDLE handle) noexcept {
            if (HasHandle(handle)) {
                SetEvent(handle);
            }
        }

        /** @brief Reset @p handle after the current readiness edge has been observed. */
        void ClearObservedSignal(HANDLE handle) noexcept {
            if (HasHandle(handle)) {
                ResetEvent(handle);
            }
        }

        /** @brief RAII owner for the manual-reset event used by @ref PlatformBackend::Wake. */
        class WakeEvent final {
        public:
            WakeEvent() noexcept
                : handle_(CreateEventW(
                      /*lpEventAttributes=*/nullptr,
                      /*bManualReset=*/TRUE,
                      /*bInitialState=*/FALSE,
                      /*lpName=*/nullptr)) {
                assert(handle_ != nullptr && "CreateEventW failed for Platform thread wake event");
            }

            ~WakeEvent() noexcept {
                if (HasHandle(handle_)) {
                    CloseHandle(handle_);
                }
            }

            WakeEvent(const WakeEvent&) = delete;
            WakeEvent& operator=(const WakeEvent&) = delete;
            WakeEvent(WakeEvent&&) = delete;
            WakeEvent& operator=(WakeEvent&&) = delete;

            [[nodiscard]] HANDLE get() const noexcept { return handle_; }

            void Signal() const noexcept { Detail::Signal(handle_); }

            void ClearObservedSignal() const noexcept { Detail::ClearObservedSignal(handle_); }

        private:
            HANDLE handle_ = nullptr;
        };

        /** @brief Window show state remembered only to turn Win32 level messages into edge events. */
        enum class WindowShowState : std::uint8_t {
            Unknown,
            Restored,
            Minimized,
            Maximized,
        };

        /** @brief Per-HWND transient state needed by the stateless Win32 message stream. */
        struct NativeWindowState {
            HWND hwnd = nullptr;
            WindowId id = WindowId::Invalid;
            vec2 lastMousePosition{};
            bool hasMousePosition = false;
            bool trackingMouseLeave = false;
            Unicode::Utf16StreamDecoder utf16{};
            bool created = false;
            WindowShowState showState = WindowShowState::Unknown;
        };

        /** @brief Mutable context used while draining one native batch. */
        struct NativeEventContext {
            WindowManager* windows = nullptr;
            std::vector<NativeWindowState>* windowsByHandle = nullptr;
        };

        /** @brief Stop callback payload that wakes the platform wait handle. */
        struct WakeOnStop {
            HANDLE event;

            void operator()() const noexcept { Signal(event); }
        };

        [[nodiscard]] constexpr std::uint16_t LowWord(std::uintptr_t value) noexcept {
            return static_cast<std::uint16_t>(value & 0xFFFFu);
        }

        [[nodiscard]] constexpr std::uint16_t HighWord(std::uintptr_t value) noexcept {
            return static_cast<std::uint16_t>((value >> 16u) & 0xFFFFu);
        }

        [[nodiscard]] constexpr int SignedLowWord(std::uintptr_t value) noexcept {
            return static_cast<int>(static_cast<std::int16_t>(LowWord(value)));
        }

        [[nodiscard]] constexpr int SignedHighWord(std::uintptr_t value) noexcept {
            return static_cast<int>(static_cast<std::int16_t>(HighWord(value)));
        }

        [[nodiscard]] constexpr int UnsignedLowWord(std::uintptr_t value) noexcept {
            return static_cast<int>(LowWord(value));
        }

        [[nodiscard]] constexpr int UnsignedHighWord(std::uintptr_t value) noexcept {
            return static_cast<int>(HighWord(value));
        }

        [[nodiscard]] std::uint64_t TimestampNow() noexcept {
            static const std::uint64_t frequency = []() noexcept {
                LARGE_INTEGER value{};
                return QueryPerformanceFrequency(&value) ? static_cast<std::uint64_t>(value.QuadPart) : 1ull;
            }();

            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
            const std::uint64_t seconds = ticks / frequency;
            const std::uint64_t remainder = ticks % frequency;
            return seconds * kNanosecondsPerSecond + (remainder * kNanosecondsPerSecond) / frequency;
        }

        [[nodiscard]] WindowId ResolveWindowId(HWND hwnd, const WindowManager* windows) noexcept {
            if (hwnd == nullptr || windows == nullptr) {
                return WindowId::Invalid;
            }
            return windows->IdOf(static_cast<NativeWindowHandle>(hwnd));
        }

        [[nodiscard]] NativeWindowState* FindState(NativeEventContext& context, HWND hwnd) noexcept {
            if (context.windowsByHandle == nullptr) {
                return nullptr;
            }
            for (auto& state : *context.windowsByHandle) {
                if (state.hwnd == hwnd) {
                    return &state;
                }
            }
            return nullptr;
        }

        [[nodiscard]] NativeWindowState* StateFor(NativeEventContext& context, HWND hwnd, WindowId id) noexcept {
            if (context.windowsByHandle == nullptr) {
                return nullptr;
            }
            if (auto* state = FindState(context, hwnd)) {
                state->id = id;
                return state;
            }
            context.windowsByHandle->push_back(NativeWindowState{.hwnd = hwnd, .id = id});
            return &context.windowsByHandle->back();
        }

        void EraseState(NativeEventContext& context, HWND hwnd) noexcept {
            if (context.windowsByHandle == nullptr) {
                return;
            }
            for (auto it = context.windowsByHandle->begin(); it != context.windowsByHandle->end(); ++it) {
                if (it->hwnd == hwnd) {
                    context.windowsByHandle->erase(it);
                    return;
                }
            }
        }

        template<class Payload, class Fill>
        [[nodiscard]] bool EmitWindowPayload(const MSG& message, NativeEventContext& context,
                                             SystemEventConsumerRef consume, Fill&& fill) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            Payload payload = MakeWindowPayload<Payload>(id);
            std::forward<Fill>(fill)(payload);
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        template<class Payload>
        [[nodiscard]] bool EmitWindowPayload(const MSG& message, NativeEventContext& context,
                                             SystemEventConsumerRef consume) noexcept {
            return EmitWindowPayload<Payload>(message, context, consume, [](Payload&) noexcept {});
        }

        [[nodiscard]] Modifiers CurrentModifiers() noexcept {
            return Modifiers{
                .shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                .ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                .alt = (GetKeyState(VK_MENU) & 0x8000) != 0,
                .super = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0,
            };
        }

        [[nodiscard]] Modifiers MouseModifiers(WPARAM state) noexcept {
            auto mods = CurrentModifiers();
            mods.shift = mods.shift || ((state & MK_SHIFT) != 0);
            mods.ctrl = mods.ctrl || ((state & MK_CONTROL) != 0);
            return mods;
        }

        [[nodiscard]] constexpr KeyCode KeyCodeFromVirtualKey(WPARAM virtualKey) noexcept {
            if (virtualKey >= 'A' && virtualKey <= 'Z') {
                return static_cast<KeyCode>(virtualKey);
            }
            if (virtualKey >= '0' && virtualKey <= '9') {
                return static_cast<KeyCode>(virtualKey);
            }

            switch (virtualKey) {
            case VK_F1:
                return KeyCode::F1;
            case VK_F2:
                return KeyCode::F2;
            case VK_F3:
                return KeyCode::F3;
            case VK_F4:
                return KeyCode::F4;
            case VK_F5:
                return KeyCode::F5;
            case VK_F6:
                return KeyCode::F6;
            case VK_F7:
                return KeyCode::F7;
            case VK_F8:
                return KeyCode::F8;
            case VK_F9:
                return KeyCode::F9;
            case VK_F10:
                return KeyCode::F10;
            case VK_F11:
                return KeyCode::F11;
            case VK_F12:
                return KeyCode::F12;
            case VK_F13:
                return KeyCode::F13;
            case VK_F14:
                return KeyCode::F14;
            case VK_F15:
                return KeyCode::F15;
            case VK_F16:
                return KeyCode::F16;
            case VK_F17:
                return KeyCode::F17;
            case VK_F18:
                return KeyCode::F18;
            case VK_F19:
                return KeyCode::F19;
            case VK_F20:
                return KeyCode::F20;
            case VK_F21:
                return KeyCode::F21;
            case VK_F22:
                return KeyCode::F22;
            case VK_F23:
                return KeyCode::F23;
            case VK_F24:
                return KeyCode::F24;
            case VK_ESCAPE:
                return KeyCode::Escape;
            case VK_TAB:
                return KeyCode::Tab;
            case VK_CAPITAL:
                return KeyCode::CapsLock;
            case VK_SHIFT:
            case VK_LSHIFT:
            case VK_RSHIFT:
                return KeyCode::Shift;
            case VK_CONTROL:
            case VK_LCONTROL:
            case VK_RCONTROL:
                return KeyCode::Control;
            case VK_MENU:
            case VK_LMENU:
            case VK_RMENU:
                return KeyCode::Alt;
            case VK_LWIN:
            case VK_RWIN:
                return KeyCode::Super;
            case VK_SPACE:
                return KeyCode::Space;
            case VK_RETURN:
                return KeyCode::Enter;
            case VK_BACK:
                return KeyCode::Backspace;
            case VK_DELETE:
                return KeyCode::Delete;
            case VK_INSERT:
                return KeyCode::Insert;
            case VK_HOME:
                return KeyCode::Home;
            case VK_END:
                return KeyCode::End;
            case VK_PRIOR:
                return KeyCode::PageUp;
            case VK_NEXT:
                return KeyCode::PageDown;
            case VK_LEFT:
                return KeyCode::Left;
            case VK_RIGHT:
                return KeyCode::Right;
            case VK_UP:
                return KeyCode::Up;
            case VK_DOWN:
                return KeyCode::Down;
            case VK_OEM_COMMA:
                return KeyCode::Comma;
            case VK_OEM_PERIOD:
                return KeyCode::Period;
            case VK_OEM_2:
                return KeyCode::Slash;
            case VK_OEM_1:
                return KeyCode::Semicolon;
            case VK_OEM_7:
                return KeyCode::Quote;
            case VK_OEM_4:
                return KeyCode::BracketLeft;
            case VK_OEM_6:
                return KeyCode::BracketRight;
            case VK_OEM_5:
                return KeyCode::Backslash;
            case VK_OEM_MINUS:
                return KeyCode::Minus;
            case VK_OEM_PLUS:
                return KeyCode::Equal;
            case VK_OEM_3:
                return KeyCode::Grave;
            default:
                return KeyCode::Unknown;
            }
        }

        [[nodiscard]] Scancode ScancodeFromKeyLParam(LPARAM value) noexcept {
            const auto bits = static_cast<std::uintptr_t>(value);
            std::uint32_t scancode = static_cast<std::uint32_t>((bits >> 16u) & 0xFFu);
            if ((bits & (std::uintptr_t{1} << 24u)) != 0) {
                scancode |= kExtendedScancodePrefix;
            }
            return static_cast<Scancode>(scancode);
        }

        [[nodiscard]] bool IsRepeatKeyDown(LPARAM value) noexcept {
            const auto bits = static_cast<std::uintptr_t>(value);
            return (bits & (std::uintptr_t{1} << 30u)) != 0;
        }

        template<class Payload>
        [[nodiscard]] bool EmitKeyPayload(const MSG& message, NativeEventContext& context,
                                          SystemEventConsumerRef consume) noexcept {
            return EmitWindowPayload<Payload>(message, context, consume, [&](Payload& payload) noexcept {
                payload.timestamp = TimestampNow();
                payload.code = KeyCodeFromVirtualKey(message.wParam);
                payload.scancode = ScancodeFromKeyLParam(message.lParam);
                payload.mods = CurrentModifiers();
                if constexpr (std::same_as<Payload, Event::KeyDownEvent>) {
                    payload.repeat = IsRepeatKeyDown(message.lParam);
                }
            });
        }

        void EmitCharCodePoint(WindowId id, char32_t codepoint, SystemEventConsumerRef consume) noexcept {
            auto payload = MakeWindowPayload<Event::CharEvent>(id);
            payload.codepoints[0] = Unicode::SanitizeScalarValue(codepoint);
            payload.count = 1;
            payload.mods = CurrentModifiers();
            EmitSystemEvent(consume, std::move(payload));
        }

        [[nodiscard]] bool EmitUtf16Char(const MSG& message, NativeEventContext& context,
                                         SystemEventConsumerRef consume) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            auto* state = StateFor(context, message.hwnd, id);
            const auto codeUnit = static_cast<std::uint16_t>(message.wParam & 0xFFFFu);
            Unicode::Utf16DecodeResult decoded{};
            if (state != nullptr) {
                decoded = state->utf16.Push(codeUnit);
            } else {
                Unicode::Utf16StreamDecoder decoder{};
                decoded = decoder.Push(codeUnit);
            }

            for (std::uint8_t i = 0; i < decoded.count; ++i) {
                EmitCharCodePoint(id, decoded.codepoints[i], consume);
            }
            return true;
        }

        [[nodiscard]] bool EmitUnicodeChar(const MSG& message, NativeEventContext& context,
                                           SystemEventConsumerRef consume) noexcept {
            if (message.wParam == UNICODE_NOCHAR) {
                return true;
            }

            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }
            EmitCharCodePoint(id, static_cast<char32_t>(message.wParam), consume);
            return true;
        }

        [[nodiscard]] ivec2 SignedPointFromLParam(LPARAM value) noexcept {
            const auto bits = static_cast<std::uintptr_t>(value);
            return ivec2{.x = SignedLowWord(bits), .y = SignedHighWord(bits)};
        }

        [[nodiscard]] ivec2 UnsignedSizeFromLParam(LPARAM value) noexcept {
            const auto bits = static_cast<std::uintptr_t>(value);
            return ivec2{.x = UnsignedLowWord(bits), .y = UnsignedHighWord(bits)};
        }

        [[nodiscard]] vec2 ClientPositionFromLParam(LPARAM value) noexcept {
            const auto point = SignedPointFromLParam(value);
            return vec2{.x = static_cast<float>(point.x), .y = static_cast<float>(point.y)};
        }

        [[nodiscard]] vec2 ClientPositionFromScreenLParam(HWND hwnd, LPARAM value) noexcept {
            const auto point = SignedPointFromLParam(value);
            POINT nativePoint{};
            nativePoint.x = point.x;
            nativePoint.y = point.y;
            if (hwnd != nullptr) {
                ScreenToClient(hwnd, &nativePoint);
            }
            return vec2{.x = static_cast<float>(nativePoint.x), .y = static_cast<float>(nativePoint.y)};
        }
        void TrackMouseLeave(HWND hwnd) noexcept {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(TRACKMOUSEEVENT);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = hwnd;
            tracking.dwHoverTime = HOVER_DEFAULT;
            TrackMouseEvent(&tracking);
        }

        [[nodiscard]] bool EmitMouseMove(const MSG& message, NativeEventContext& context,
                                         SystemEventConsumerRef consume) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            const vec2 position = ClientPositionFromLParam(message.lParam);
            vec2 delta{};
            if (auto* state = StateFor(context, message.hwnd, id)) {
                if (!state->trackingMouseLeave) {
                    state->trackingMouseLeave = true;
                    TrackMouseLeave(message.hwnd);
                    EmitSystemEvent(consume, MakeWindowPayload<Event::MouseEnterEvent>(id));
                }
                if (state->hasMousePosition) {
                    delta.x = position.x - state->lastMousePosition.x;
                    delta.y = position.y - state->lastMousePosition.y;
                }
                state->lastMousePosition = position;
                state->hasMousePosition = true;
            }

            auto payload = MakeWindowPayload<Event::MouseMoveEvent>(id);
            payload.timestamp = TimestampNow();
            payload.position = position;
            payload.delta = delta;
            payload.mods = MouseModifiers(message.wParam);
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        [[nodiscard]] bool EmitMouseLeave(const MSG& message, NativeEventContext& context,
                                          SystemEventConsumerRef consume) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            if (auto* state = FindState(context, message.hwnd)) {
                state->trackingMouseLeave = false;
                state->hasMousePosition = false;
            }
            EmitSystemEvent(consume, MakeWindowPayload<Event::MouseLeaveEvent>(id));
            return true;
        }

        /** @brief Decoded Win32 mouse-button message. */
        struct MouseButtonMessage {
            MouseButton button = MouseButton::Left;
            bool pressed = false;
            std::uint8_t clickCount = 1;
        };

        [[nodiscard]] MouseButtonMessage MouseButtonFromMessage(const MSG& message) noexcept {
            switch (message.message) {
            case WM_LBUTTONDOWN:
                return {.button = MouseButton::Left, .pressed = true, .clickCount = 1};
            case WM_LBUTTONUP:
                return {.button = MouseButton::Left, .pressed = false, .clickCount = 1};
            case WM_LBUTTONDBLCLK:
                return {.button = MouseButton::Left, .pressed = true, .clickCount = 2};
            case WM_RBUTTONDOWN:
                return {.button = MouseButton::Right, .pressed = true, .clickCount = 1};
            case WM_RBUTTONUP:
                return {.button = MouseButton::Right, .pressed = false, .clickCount = 1};
            case WM_RBUTTONDBLCLK:
                return {.button = MouseButton::Right, .pressed = true, .clickCount = 2};
            case WM_MBUTTONDOWN:
                return {.button = MouseButton::Middle, .pressed = true, .clickCount = 1};
            case WM_MBUTTONUP:
                return {.button = MouseButton::Middle, .pressed = false, .clickCount = 1};
            case WM_MBUTTONDBLCLK:
                return {.button = MouseButton::Middle, .pressed = true, .clickCount = 2};
            case WM_XBUTTONDOWN:
                return {.button = HighWord(message.wParam) == XBUTTON2 ? MouseButton::Button5 : MouseButton::Button4,
                        .pressed = true,
                        .clickCount = 1};
            case WM_XBUTTONUP:
                return {.button = HighWord(message.wParam) == XBUTTON2 ? MouseButton::Button5 : MouseButton::Button4,
                        .pressed = false,
                        .clickCount = 1};
            case WM_XBUTTONDBLCLK:
                return {.button = HighWord(message.wParam) == XBUTTON2 ? MouseButton::Button5 : MouseButton::Button4,
                        .pressed = true,
                        .clickCount = 2};
            default:
                return {};
            }
        }

        [[nodiscard]] bool EmitMouseButton(const MSG& message, NativeEventContext& context,
                                           SystemEventConsumerRef consume) noexcept {
            return EmitWindowPayload<Event::MouseButtonEvent>(message, context, consume, [&](auto& payload) noexcept {
                const MouseButtonMessage button = MouseButtonFromMessage(message);
                payload.timestamp = TimestampNow();
                payload.position = ClientPositionFromLParam(message.lParam);
                payload.button = button.button;
                payload.pressed = button.pressed;
                payload.clickCount = button.clickCount;
                payload.mods = MouseModifiers(message.wParam);
            });
        }

        [[nodiscard]] bool EmitScroll(const MSG& message, NativeEventContext& context,
                                      SystemEventConsumerRef consume) noexcept {
            return EmitWindowPayload<Event::ScrollEvent>(message, context, consume, [&](auto& payload) noexcept {
                const float amount =
                    static_cast<float>(SignedHighWord(message.wParam)) / static_cast<float>(WHEEL_DELTA);
                payload.timestamp = TimestampNow();
                payload.position = ClientPositionFromScreenLParam(message.hwnd, message.lParam);
                payload.delta =
                    message.message == WM_MOUSEHWHEEL ? vec2{.x = amount, .y = 0.0f} : vec2{.x = 0.0f, .y = amount};
                payload.unit = ScrollUnit::Lines;
                payload.mods = MouseModifiers(message.wParam);
            });
        }

        [[nodiscard]] WindowShowState ShowStateFromSizeMessage(WPARAM value) noexcept {
            switch (value) {
            case SIZE_MINIMIZED:
                return WindowShowState::Minimized;
            case SIZE_MAXIMIZED:
                return WindowShowState::Maximized;
            case SIZE_RESTORED:
                return WindowShowState::Restored;
            default:
                return WindowShowState::Unknown;
            }
        }

        void EmitShowTransition(WindowId id, WindowShowState before, WindowShowState after,
                                SystemEventConsumerRef consume) noexcept {
            if (after == WindowShowState::Minimized) {
                EmitSystemEvent(consume, MakeWindowPayload<Event::WindowMinimizeEvent>(id));
            } else if (after == WindowShowState::Maximized) {
                EmitSystemEvent(consume, MakeWindowPayload<Event::WindowMaximizeEvent>(id));
            } else if (after == WindowShowState::Restored &&
                       (before == WindowShowState::Minimized || before == WindowShowState::Maximized)) {
                EmitSystemEvent(consume, MakeWindowPayload<Event::WindowRestoreEvent>(id));
            }
        }

        [[nodiscard]] bool EmitWindowResize(const MSG& message, NativeEventContext& context,
                                            SystemEventConsumerRef consume) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            auto resize = MakeWindowPayload<Event::WindowResizeEvent>(id);
            resize.timestamp = TimestampNow();
            resize.size = UnsignedSizeFromLParam(message.lParam);
            resize.isMinimised = message.wParam == SIZE_MINIMIZED;
            EmitSystemEvent(consume, std::move(resize));

            const WindowShowState after = ShowStateFromSizeMessage(message.wParam);
            if (after == WindowShowState::Unknown) {
                return true;
            }

            if (auto* state = StateFor(context, message.hwnd, id)) {
                const WindowShowState before = state->showState;
                state->showState = after;
                EmitShowTransition(id, before, after, consume);
            } else if (after == WindowShowState::Minimized || after == WindowShowState::Maximized) {
                EmitShowTransition(id, WindowShowState::Unknown, after, consume);
            }
            return true;
        }

        [[nodiscard]] bool EmitWindowDestroy(const MSG& message, NativeEventContext& context,
                                             SystemEventConsumerRef consume) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                EraseState(context, message.hwnd);
                return false;
            }

            EmitSystemEvent(consume, MakeWindowPayload<Event::WindowDestroyEvent>(id));
            EraseState(context, message.hwnd);
            return true;
        }

        [[nodiscard]] bool EmitWindowDpiChange(const MSG& message, NativeEventContext& context,
                                               SystemEventConsumerRef consume) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid || context.windows == nullptr) {
                return false;
            }

            auto payload = MakeWindowPayload<Event::WindowDpiChangeEvent>(id);
            payload.oldScale = context.windows->DpiScaleOf(id);
            payload.newScale = DpiScaleFromDpi(LowWord(message.wParam));
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        [[nodiscard]] bool DwmCompositionEnabled() noexcept {
            BOOL enabled = FALSE;
            return SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled == TRUE;
        }

        [[nodiscard]] bool EmitDwmCompositionChange(const MSG& message, NativeEventContext& context,
                                                    SystemEventConsumerRef consume) noexcept {
            return EmitWindowPayload<Event::WindowDwmCompositionChangeEvent>(
                message, context, consume,
                [](auto& payload) noexcept { payload.compositionEnabled = DwmCompositionEnabled(); });
        }
        [[nodiscard]] bool ReadDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& out) noexcept {
            DWORD size = sizeof(out);
            return RegGetValueW(root, subkey, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
        }

        [[nodiscard]] bool SystemWindowColorIsLight() noexcept {
            const COLORREF color = GetSysColor(COLOR_WINDOW);
            const int luma = static_cast<int>(GetRValue(color)) * 299 + static_cast<int>(GetGValue(color)) * 587 +
                             static_cast<int>(GetBValue(color)) * 114;
            return luma >= 128000;
        }

        [[nodiscard]] AppearanceTheme CurrentAppearanceTheme() noexcept {
            HIGHCONTRASTW highContrast{};
            highContrast.cbSize = sizeof(highContrast);
            const bool highContrastOn =
                SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
                ((highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0);

            constexpr const wchar_t* key = LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
            DWORD appsUseLightTheme = 1;
            const bool light = ReadDword(HKEY_CURRENT_USER, key, L"AppsUseLightTheme", appsUseLightTheme)
                                   ? appsUseLightTheme != 0
                                   : SystemWindowColorIsLight();

            if (highContrastOn) {
                return light ? AppearanceTheme::HighContrastLight : AppearanceTheme::HighContrastDark;
            }
            return light ? AppearanceTheme::Light : AppearanceTheme::Dark;
        }

        [[nodiscard]] std::uint32_t CurrentAccentColorRgba() noexcept {
            DWORD argb = 0;
            BOOL opaque = FALSE;
            if (!SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) {
                return 0;
            }

            const std::uint32_t a = (argb >> 24u) & 0xFFu;
            const std::uint32_t r = (argb >> 16u) & 0xFFu;
            const std::uint32_t g = (argb >> 8u) & 0xFFu;
            const std::uint32_t b = argb & 0xFFu;
            return (r << 24u) | (g << 16u) | (b << 8u) | a;
        }

        void EmitAppearanceChange(SystemEventConsumerRef consume) noexcept {
            Event::AppearanceChangeEvent payload{};
            payload.theme = CurrentAppearanceTheme();
            payload.accentColorRgba = CurrentAccentColorRgba();
            EmitSystemEvent(consume, std::move(payload));
        }

        [[nodiscard]] std::string CurrentKeyboardLayoutName() {
            wchar_t name[KL_NAMELENGTH]{};
            return GetKeyboardLayoutNameW(name) ? Unicode::Utf16ToUtf8(std::wstring_view{name}) : std::string{};
        }

        [[nodiscard]] DragKind CurrentClipboardKinds() noexcept {
            DragKind kinds = DragKind::None;
            if (IsClipboardFormatAvailable(CF_HDROP)) {
                kinds |= DragKind::Files;
            }
            if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) {
                kinds |= DragKind::Text;
            }
            if (IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB) ||
                IsClipboardFormatAvailable(CF_DIBV5)) {
                kinds |= DragKind::Image;
            }
            return kinds;
        }

        void EmitClipboardUpdate(SystemEventConsumerRef consume) noexcept {
            Event::ClipboardUpdateEvent payload{};
            payload.kinds = CurrentClipboardKinds();
            EmitSystemEvent(consume, std::move(payload));
        }

        [[nodiscard]] PowerSource PowerSourceOf(const SYSTEM_POWER_STATUS& status) noexcept {
            switch (status.ACLineStatus) {
            case 0:
                return PowerSource::Battery;
            case 1:
                return PowerSource::AC;
            default:
                return PowerSource::Unknown;
            }
        }

        [[nodiscard]] float BatteryLevelOf(const SYSTEM_POWER_STATUS& status) noexcept {
            return status.BatteryLifePercent == 255 ? 1.0f : static_cast<float>(status.BatteryLifePercent) / 100.0f;
        }

        [[nodiscard]] std::uint32_t BatterySecondsRemainingOf(const SYSTEM_POWER_STATUS& status) noexcept {
            return status.BatteryLifeTime == static_cast<DWORD>(-1) ? 0u : status.BatteryLifeTime;
        }

        void EmitPowerSnapshot(SystemEventConsumerRef consume) noexcept {
            SYSTEM_POWER_STATUS status{};
            if (!GetSystemPowerStatus(&status)) {
                EmitSystemEvent(consume, Event::PowerStateChangeEvent{});
                return;
            }

            Event::PowerStateChangeEvent power{};
            power.source = PowerSourceOf(status);
            power.batteryLevel = BatteryLevelOf(status);
            EmitSystemEvent(consume, std::move(power));

            if (status.BatteryLifePercent != 255) {
                Event::BatteryLevelChangeEvent battery{};
                battery.level = BatteryLevelOf(status);
                battery.charging = (status.BatteryFlag & 0x08u) != 0;
                battery.secondsRemaining = BatterySecondsRemainingOf(status);
                EmitSystemEvent(consume, std::move(battery));
            }
        }

        [[nodiscard]] bool EmitPowerBroadcast(const MSG& message, SystemEventConsumerRef consume) noexcept {
            switch (message.wParam) {
            case PBT_APMSUSPEND:
                EmitSystemEvent(consume, Event::PowerSuspendEvent{});
                return true;
            case PBT_APMRESUMECRITICAL:
            case PBT_APMRESUMESUSPEND:
            case PBT_APMRESUMEAUTOMATIC:
                EmitSystemEvent(consume, Event::PowerResumeEvent{});
                EmitPowerSnapshot(consume);
                return true;
            case PBT_APMPOWERSTATUSCHANGE:
            case PBT_POWERSETTINGCHANGE:
                EmitPowerSnapshot(consume);
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] bool EmitSessionEndQuery(const MSG& message, SystemEventConsumerRef consume) noexcept {
            const auto flags = static_cast<std::uintptr_t>(message.lParam);
            Event::SessionEndQueryEvent payload{};
            payload.isShutdown = (flags & static_cast<std::uintptr_t>(ENDSESSION_LOGOFF)) == 0;
            payload.isCritical = (flags & static_cast<std::uintptr_t>(ENDSESSION_CRITICAL)) != 0;
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        [[nodiscard]] bool EmitSessionChange(const MSG& message, SystemEventConsumerRef consume) noexcept {
            switch (message.wParam) {
            case WTS_SESSION_LOCK:
                EmitSystemEvent(consume, Event::SessionLockEvent{});
                return true;
            case WTS_SESSION_UNLOCK:
                EmitSystemEvent(consume, Event::SessionUnlockEvent{});
                return true;
            case WTS_SESSION_LOGON:
            case WTS_SESSION_LOGOFF:
                EmitSystemEvent(consume, Event::SessionUserSwitchEvent{});
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] bool EmitDisplayChange(const MSG& message, SystemEventConsumerRef consume) noexcept {
            const auto bits = static_cast<std::uintptr_t>(message.lParam);
            Event::DisplayChangeEvent payload{};
            payload.resolution = uvec2{
                .x = static_cast<std::uint32_t>(UnsignedLowWord(bits)),
                .y = static_cast<std::uint32_t>(UnsignedHighWord(bits)),
            };
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        [[nodiscard]] bool EmitDeviceChange(const MSG& message, SystemEventConsumerRef consume) noexcept {
            switch (message.wParam) {
            case DBT_DEVICEARRIVAL: {
                Event::DeviceChangeEvent payload{};
                payload.attached = true;
                EmitSystemEvent(consume, std::move(payload));
                return true;
            }
            case DBT_DEVICEREMOVECOMPLETE: {
                Event::DeviceChangeEvent payload{};
                payload.attached = false;
                EmitSystemEvent(consume, std::move(payload));
                return true;
            }
            default:
                return false;
            }
        }

        [[nodiscard]] ivec2 ClientSizeOf(HWND hwnd) noexcept {
            RECT rect{};
            if (hwnd == nullptr || !GetClientRect(hwnd, &rect)) {
                return {};
            }
            return ivec2{.x = static_cast<int>(rect.right - rect.left), .y = static_cast<int>(rect.bottom - rect.top)};
        }

        [[nodiscard]] std::uint32_t DpiOf(HWND hwnd) noexcept {
            return hwnd != nullptr ? GetDpiForWindow(hwnd) : static_cast<std::uint32_t>(kDesktopDpiBase);
        }

        [[nodiscard]] bool EmitWindowCreate(const MSG& message, NativeEventContext& context,
                                            SystemEventConsumerRef consume) noexcept {
            if (message.hwnd == nullptr || context.windows == nullptr) {
                return false;
            }

            WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                id = context.windows->Adopt(static_cast<NativeWindowHandle>(message.hwnd));
            }

            auto* state = StateFor(context, message.hwnd, id);
            if (state != nullptr && state->created) {
                return true;
            }
            if (state != nullptr) {
                state->created = true;
            }

            auto payload = MakeWindowPayload<Event::WindowCreateEvent>(id);
            payload.size = ClientSizeOf(message.hwnd);
            payload.dpiScale = DpiScaleFromDpi(DpiOf(message.hwnd));
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        [[nodiscard]] std::string ImeCompositionString(HWND hwnd, DWORD kind) {
            HIMC input = ImmGetContext(hwnd);
            if (input == nullptr) {
                return {};
            }

            const LONG bytes = ImmGetCompositionStringW(input, kind, nullptr, 0);
            if (bytes <= 0) {
                ImmReleaseContext(hwnd, input);
                return {};
            }

            std::wstring wide(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(input, kind, wide.data(), static_cast<DWORD>(bytes));
            ImmReleaseContext(hwnd, input);
            return Unicode::Utf16ToUtf8(std::wstring_view{wide.data(), wide.size()});
        }

        [[nodiscard]] std::uint32_t ImeCursorPosition(HWND hwnd) noexcept {
            HIMC input = ImmGetContext(hwnd);
            if (input == nullptr) {
                return 0;
            }
            const LONG position = ImmGetCompositionStringW(input, GCS_CURSORPOS, nullptr, 0);
            ImmReleaseContext(hwnd, input);
            return position > 0 ? static_cast<std::uint32_t>(position) : 0u;
        }

        [[nodiscard]] bool EmitImeComposition(const MSG& message, NativeEventContext& context,
                                              SystemEventConsumerRef consume) noexcept {
            bool emitted = false;
            if ((message.lParam & GCS_COMPSTR) != 0) {
                emitted |=
                    EmitWindowPayload<Event::ImeCompositionUpdateEvent>(message, context, consume, [&](auto& payload) {
                        payload.timestamp = TimestampNow();
                        payload.preedit = ImeCompositionString(message.hwnd, GCS_COMPSTR);
                        payload.caret = ImeCursorPosition(message.hwnd);
                    });
            }
            if ((message.lParam & GCS_RESULTSTR) != 0) {
                emitted |= EmitWindowPayload<Event::ImeCommitEvent>(message, context, consume, [&](auto& payload) {
                    payload.timestamp = TimestampNow();
                    payload.text = ImeCompositionString(message.hwnd, GCS_RESULTSTR);
                });
            }
            return emitted;
        }

        [[nodiscard]] DWORD FirstCandidateListIndex(LPARAM value) noexcept {
            const auto bits = static_cast<std::uintptr_t>(value);
            for (DWORD i = 0; i < 32; ++i) {
                if ((bits & (std::uintptr_t{1} << i)) != 0) {
                    return i;
                }
            }
            return 0;
        }

        [[nodiscard]] bool EmitImeCandidateList(const MSG& message, NativeEventContext& context,
                                                SystemEventConsumerRef consume) noexcept {
            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            HIMC input = ImmGetContext(message.hwnd);
            if (input == nullptr) {
                return false;
            }

            const DWORD listIndex = FirstCandidateListIndex(message.lParam);
            const DWORD bytes = ImmGetCandidateListW(input, listIndex, nullptr, 0);
            if (bytes == 0) {
                ImmReleaseContext(message.hwnd, input);
                return false;
            }

            std::vector<std::byte> storage(bytes);
            auto* list = reinterpret_cast<CANDIDATELIST*>(storage.data());
            ImmGetCandidateListW(input, listIndex, list, bytes);
            ImmReleaseContext(message.hwnd, input);

            auto payload = MakeWindowPayload<Event::ImeCandidateListEvent>(id);
            payload.selected = list->dwSelection;
            payload.pageStart = list->dwPageStart;
            payload.pageSize = list->dwPageSize;
            payload.candidates.reserve(list->dwCount);
            const auto* base = reinterpret_cast<const char*>(storage.data());
            for (DWORD i = 0; i < list->dwCount; ++i) {
                const auto* text = reinterpret_cast<const wchar_t*>(base + list->dwOffset[i]);
                payload.candidates.push_back(Unicode::Utf16ToUtf8(std::wstring_view{text}));
            }
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        [[nodiscard]] bool EmitRawInput(const MSG& message, NativeEventContext& context,
                                        SystemEventConsumerRef consume) noexcept {
            UINT bytes = 0;
            auto input = reinterpret_cast<HRAWINPUT>(message.lParam);
            if (GetRawInputData(input, RID_INPUT, nullptr, &bytes, sizeof(RAWINPUTHEADER)) != 0 || bytes == 0) {
                return false;
            }

            std::vector<std::byte> storage(bytes);
            if (GetRawInputData(input, RID_INPUT, storage.data(), &bytes, sizeof(RAWINPUTHEADER)) != bytes) {
                return false;
            }

            const auto* raw = reinterpret_cast<const RAWINPUT*>(storage.data());
            if (raw->header.dwType != RIM_TYPEMOUSE || (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
                return false;
            }

            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            auto payload = MakeWindowPayload<Event::RawMouseMotionEvent>(id);
            payload.timestamp = TimestampNow();
            payload.delta = ivec2{.x = raw->data.mouse.lLastX, .y = raw->data.mouse.lLastY};
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        /** @brief RAII owner for an HDROP payload delivered by WM_DROPFILES. */
        class DropHandle final {
        public:
            explicit DropHandle(HDROP handle) noexcept : handle_(handle) {}

            ~DropHandle() noexcept {
                if (handle_ != nullptr) {
                    DragFinish(handle_);
                }
            }

            DropHandle(const DropHandle&) = delete;
            DropHandle& operator=(const DropHandle&) = delete;
            DropHandle(DropHandle&&) = delete;
            DropHandle& operator=(DropHandle&&) = delete;

            [[nodiscard]] HDROP get() const noexcept { return handle_; }

        private:
            HDROP handle_ = nullptr;
        };
        [[nodiscard]] std::vector<std::string> DropFilePaths(HDROP drop) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            std::vector<std::string> paths;
            paths.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                const UINT chars = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(static_cast<std::size_t>(chars) + 1u, L'\0');
                DragQueryFileW(drop, i, path.data(), chars + 1);
                path.resize(chars);
                paths.push_back(Unicode::Utf16ToUtf8(std::wstring_view{path.data(), path.size()}));
            }
            return paths;
        }

        [[nodiscard]] bool EmitDropFiles(const MSG& message, NativeEventContext& context,
                                         SystemEventConsumerRef consume) noexcept {
            DropHandle drop{reinterpret_cast<HDROP>(message.wParam)};
            if (drop.get() == nullptr) {
                return false;
            }

            const WindowId id = ResolveWindowId(message.hwnd, context.windows);
            if (id == WindowId::Invalid) {
                return false;
            }

            POINT point{};
            DragQueryPoint(drop.get(), &point);
            auto payload = MakeWindowPayload<Event::DragDropEvent>(id);
            payload.position = vec2{.x = static_cast<float>(point.x), .y = static_cast<float>(point.y)};
            payload.paths = DropFilePaths(drop.get());
            payload.drag = DragKind::Files;
            EmitSystemEvent(consume, std::move(payload));
            return true;
        }

        [[nodiscard]] bool HighContrastEnabled() noexcept {
            HIGHCONTRASTW highContrast{};
            highContrast.cbSize = sizeof(highContrast);
            return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
                   ((highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0);
        }

        [[nodiscard]] bool ScreenReaderEnabled() noexcept {
            BOOL enabled = FALSE;
            return SystemParametersInfoW(SPI_GETSCREENREADER, 0, &enabled, 0) && enabled == TRUE;
        }

        [[nodiscard]] bool ReducedMotionEnabled() noexcept {
            BOOL animation = TRUE;
            return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0) && animation == FALSE;
        }

        void EmitAccessibilitySnapshot(SystemEventConsumerRef consume) noexcept {
            Event::AccessibilityScreenReaderEvent screenReader{};
            screenReader.enabled = ScreenReaderEnabled();
            EmitSystemEvent(consume, std::move(screenReader));

            Event::AccessibilityReducedMotionEvent reducedMotion{};
            reducedMotion.enabled = ReducedMotionEnabled();
            EmitSystemEvent(consume, std::move(reducedMotion));

            Event::AccessibilityHighContrastEvent highContrast{};
            highContrast.enabled = HighContrastEnabled();
            EmitSystemEvent(consume, std::move(highContrast));
        }

        void EmitAppearanceMessages(const MSG& message, NativeEventContext& context,
                                    SystemEventConsumerRef consume) noexcept {
            EmitAppearanceChange(consume);
            (void)EmitWindowPayload<Event::WindowThemeChangeEvent>(
                message, context, consume, [](auto& payload) { payload.theme = CurrentAppearanceTheme(); });
            if (message.message == WM_SETTINGCHANGE) {
                EmitAccessibilitySnapshot(consume);
            }
        }

        void EmitTimerTick(const MSG& message, SystemEventConsumerRef consume) noexcept {
            Event::TimerTickEvent payload{};
            payload.timestamp = TimestampNow();
            payload.timerId = static_cast<std::uint32_t>(message.wParam);
            EmitSystemEvent(consume, std::move(payload));
        }

        /**
         * @brief Emit a mapped @ref SystemEvent for @p message when the platform schema defines one.
         *
         * Window-scoped messages require a live @ref WindowId resolved from @p context. This prevents invalid
         * native handles from crossing the platform boundary as sentinel-valued semantic events.
         */
        [[nodiscard]] bool EmitMappedEvent(const MSG& message, NativeEventContext& context,
                                           SystemEventConsumerRef consume) noexcept {
            switch (message.message) {
            case WM_NCCREATE:
            case WM_CREATE:
                return EmitWindowCreate(message, context, consume);
            case WM_CLOSE:
                return EmitWindowPayload<Event::WindowCloseEvent>(message, context, consume);
            case WM_DESTROY:
                return EmitWindowDestroy(message, context, consume);
            case WM_NCDESTROY:
                EraseState(context, message.hwnd);
                return true;
            case WM_SIZE:
                return EmitWindowResize(message, context, consume);
            case WM_MOVE:
                return EmitWindowPayload<Event::WindowMoveEvent>(
                    message, context, consume,
                    [&](auto& payload) noexcept { payload.position = SignedPointFromLParam(message.lParam); });
            case WM_SETFOCUS:
                return EmitWindowPayload<Event::WindowFocusEvent>(
                    message, context, consume, [](auto& payload) noexcept { payload.focused = true; });
            case WM_KILLFOCUS:
                return EmitWindowPayload<Event::WindowFocusEvent>(
                    message, context, consume, [](auto& payload) noexcept { payload.focused = false; });
            case WM_SHOWWINDOW:
                return EmitWindowPayload<Event::WindowVisibilityChangeEvent>(
                    message, context, consume,
                    [&](auto& payload) noexcept { payload.visible = message.wParam != FALSE; });
            case WM_DPICHANGED:
                return EmitWindowDpiChange(message, context, consume);
            case WM_DWMCOMPOSITIONCHANGED:
                return EmitDwmCompositionChange(message, context, consume);
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                return EmitKeyPayload<Event::KeyDownEvent>(message, context, consume);
            case WM_KEYUP:
            case WM_SYSKEYUP:
                return EmitKeyPayload<Event::KeyUpEvent>(message, context, consume);
            case WM_CHAR:
            case WM_SYSCHAR:
                return EmitUtf16Char(message, context, consume);
            case WM_UNICHAR:
                return EmitUnicodeChar(message, context, consume);
            case WM_INPUTLANGCHANGE:
                return EmitWindowPayload<Event::KeyboardLayoutChangeEvent>(
                    message, context, consume, [](auto& payload) { payload.layoutId = CurrentKeyboardLayoutName(); });
            case WM_IME_SETCONTEXT:
                if (message.wParam != FALSE) {
                    return EmitWindowPayload<Event::ImeActivateEvent>(message, context, consume);
                }
                return EmitWindowPayload<Event::ImeDeactivateEvent>(message, context, consume);
            case WM_IME_STARTCOMPOSITION:
                return EmitWindowPayload<Event::ImeCompositionEvent>(
                    message, context, consume, [](auto& payload) { payload.timestamp = TimestampNow(); });
            case WM_IME_COMPOSITION:
                return EmitImeComposition(message, context, consume);
            case WM_IME_ENDCOMPOSITION:
                return true;
            case WM_IME_NOTIFY:
                return message.wParam == IMN_OPENCANDIDATE || message.wParam == IMN_CHANGECANDIDATE
                           ? EmitImeCandidateList(message, context, consume)
                           : false;
            case WM_INPUT:
                return EmitRawInput(message, context, consume);
            case WM_MOUSEMOVE:
                return EmitMouseMove(message, context, consume);
            case WM_MOUSELEAVE:
                return EmitMouseLeave(message, context, consume);
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
                return EmitMouseButton(message, context, consume);
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return EmitScroll(message, context, consume);
            case WM_DROPFILES:
                return EmitDropFiles(message, context, consume);
            case WM_THEMECHANGED:
            case WM_SYSCOLORCHANGE:
            case WM_SETTINGCHANGE:
            case WM_DWMCOLORIZATIONCOLORCHANGED:
                EmitAppearanceMessages(message, context, consume);
                return true;
            case WM_CLIPBOARDUPDATE:
                EmitClipboardUpdate(consume);
                return true;
            case WM_DISPLAYCHANGE:
                return EmitDisplayChange(message, consume);
            case WM_POWERBROADCAST:
                return EmitPowerBroadcast(message, consume);
            case WM_QUERYENDSESSION:
                return EmitSessionEndQuery(message, consume);
            case WM_WTSSESSION_CHANGE:
                return EmitSessionChange(message, consume);
            case WM_DEVICECHANGE:
                return EmitDeviceChange(message, consume);
            case WM_TIMER:
                EmitTimerTick(message, consume);
                return true;
            default:
                return false;
            }
        }

    } /* namespace Detail */

    /** @brief Win32 state owned by @ref PlatformBackend. */
    struct PlatformBackend::Impl {
        Detail::WakeEvent wakeEvent;
        WindowManager* windows = nullptr;
        std::vector<Detail::NativeWindowState> windowsByHandle{};
    };

    PlatformBackend::PlatformBackend() : impl_(std::make_unique<Impl>()) {}
    PlatformBackend::~PlatformBackend() = default;

    void PlatformBackend::Wake() const noexcept {
        impl_->wakeEvent.Signal();
    }

    void PlatformBackend::AttachWindowRegistry(WindowManager& windows) const noexcept {
        impl_->windows = &windows;
    }

    void PlatformBackend::WaitForAnySource(stdexec::inplace_stop_token stop) const noexcept {
        auto wake = impl_->wakeEvent.get();
        stdexec::inplace_stop_callback<Detail::WakeOnStop> stopWake{stop, Detail::WakeOnStop{wake}};

        const DWORD handleCount = Detail::HasHandle(wake) ? 1u : 0u;
        const DWORD result = MsgWaitForMultipleObjectsEx(handleCount, &wake, Detail::kInfiniteWait,
                                                         Detail::kNativeMessageMask, Detail::kNativeWaitFlags);

        impl_->wakeEvent.ClearObservedSignal();
        (void)result;
    }

    void PlatformBackend::DrainNative(SystemEventConsumerRef consume) const noexcept {
        Detail::NativeEventContext context{
            .windows = impl_->windows,
            .windowsByHandle = &impl_->windowsByHandle,
        };

        MSG message{};
        while (PeekMessageW(&message, /*hWnd=*/nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return;
            }
            if (Detail::EmitMappedEvent(message, context, consume)) {
                continue;
            }
        }
    }

} /* namespace Mashiro::Platform */

#endif
