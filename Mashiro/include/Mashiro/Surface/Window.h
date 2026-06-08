#pragma once

#include "Mashiro/Core/TypeTraits.h"
#include "Mashiro/Core/Flags.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Mashiro {

    /// @brief 2D point with integer coordinates (screen/pixel space).
    struct PointI { int32_t x = 0, y = 0; };

    /// @brief 2D size with unsigned dimensions (pixels).
    struct SizeU { uint32_t w = 0, h = 0; };

    /// @brief Placeholder event type (forward declaration for the event system).
    struct Event {};

    namespace Platform {

        namespace Window {

            // Window style flags
            enum class Style : uint32_t {
                None = 0,
                Titled = 1 << 0,
                Closable = 1 << 1,
                Minimizable = 1 << 2,
                Maximizable = 1 << 3,
                Resizable = 1 << 4,
                Borderless = 1 << 5,
                Transparent = 1 << 6,
            };
            static_assert(Mashiro::Traits::BitfieldEnum<Style>);

            // Window display mode
            enum class DisplayMode : uint8_t {
                Windowed,
                Fullscreen,
                BorderlessFullscreen,
                ExclusiveFullscreen,
            };

            // Window visibility state
            enum class VisibilityState : uint8_t {
                Hidden,
                Visible,
                Minimized,
                Maximized,
            };

            // DPI awareness mode
            enum class DpiMode : uint8_t {
                Unaware,
                SystemAware,
                PerMonitorAware,
                PerMonitorAwareV2,
            };

            // Color space for window surface
            enum class ColorSpace : uint8_t {
                Srgb,
                LinearSrgb,
                Hdr10,
                DolbyVision,
                ScRgb,
                AdobeRgb,
                DciP3,
            };

            // Window icon specification
            struct WindowIcon {
                std::span<const std::byte> pixels;
                uint32_t width = 0;
                uint32_t height = 0;
                uint8_t bpp = 32; // bits per pixel
            };

            // Monitor/display identifier
            struct MonitorId {
                uint32_t index = 0;
                explicit operator bool() const { return index != UINT32_MAX; }
                static constexpr MonitorId Primary() { return {0}; }
                static constexpr MonitorId Invalid() { return {UINT32_MAX}; }
            };

            // Complete window descriptor
            struct WindowDesc {
                // Basic properties
                std::string title = "Mashiro Window";
                uint32_t width = 1280;
                uint32_t height = 720;

                // Position (nullopt = system default / centered)
                std::optional<Mashiro::PointI> position = std::nullopt;

                // Size constraints
                std::optional<Mashiro::SizeU> minSize = std::nullopt;
                std::optional<Mashiro::SizeU> maxSize = std::nullopt;

                // Style and behavior
                Style style = Style::Titled | Style::Closable | Style::Minimizable | Style::Resizable;
                DisplayMode displayMode = DisplayMode::Windowed;
                VisibilityState initialState = VisibilityState::Visible;

                // DPI and scaling
                DpiMode dpiMode = DpiMode::PerMonitorAwareV2;
                float contentScale = 1.0f;

                // Rendering properties
                ColorSpace colorSpace = ColorSpace::Srgb;
                bool vsync = true;
                uint8_t swapchainBuffers = 2;

                // Visual properties
                float opacity = 1.0f;
                bool alwaysOnTop = false;
                bool taskbarVisible = true;
                bool acceptDropFiles = false;

                // Input behavior
                bool cursorVisible = true;
                bool cursorConfined = false;
                bool rawMouseInput = false;
                bool keyboardGrab = false;

                // Icon (optional)
                std::optional<WindowIcon> icon = std::nullopt;
                std::optional<WindowIcon> smallIcon = std::nullopt;

                // Multi-monitor
                MonitorId preferredMonitor = MonitorId::Primary();

                // Parent window for child/popup windows
                void* parentHandle = nullptr;

                // Platform-specific hints (opaque, forwarded to backend)
                std::span<const std::pair<std::string_view, std::string_view>> platformHints = {};
            };

        } // namespace Window

    } // namespace Platform

    // ============================================================================
    // Forward Declarations
    // ============================================================================

    class Window;
    class EventLoop;
    class Surface;

    using WindowPtr = std::unique_ptr<Window>;

    // ============================================================================
    // Window Configuration
    // ============================================================================

    struct WindowConfig {
        std::string title = "Window";
        uint32_t width = 800;
        uint32_t height = 600;
        std::optional<Mashiro::PointI> position = std::nullopt;
        bool resizable = true;
        bool decorated = true;
        bool transparent = false;
        bool alwaysOnTop = false;
        bool maximized = false;
        bool minimized = false;
        bool visible = true;
        std::optional<Mashiro::SizeU> minSize = std::nullopt;
        std::optional<Mashiro::SizeU> maxSize = std::nullopt;
        Window* ownerWindow = nullptr; // Owner window for cascading destruction

        // Optional custom event callback (if not set, events go to EventPump)
        std::function<void(const Event&)> eventCallback = nullptr;

        WindowConfig& WithTitle(std::string iTitle) {
            title = std::move(iTitle);
            return *this;
        }
        WindowConfig& WithSize(uint32_t iWidth, uint32_t iHeight) {
            width = iWidth;
            height = iHeight;
            return *this;
        }
        WindowConfig& WithPosition(int32_t iX, int32_t iY) {
            position = Mashiro::PointI{iX, iY};
            return *this;
        }
        WindowConfig& WithResizable(bool iValue) {
            resizable = iValue;
            return *this;
        }
        WindowConfig& WithDecorated(bool iValue) {
            decorated = iValue;
            return *this;
        }
        WindowConfig& WithTransparent(bool iValue) {
            transparent = iValue;
            return *this;
        }
        WindowConfig& WithAlwaysOnTop(bool iValue) {
            alwaysOnTop = iValue;
            return *this;
        }
        WindowConfig& WithMaximized(bool iValue) {
            maximized = iValue;
            return *this;
        }
        WindowConfig& WithVisible(bool iValue) {
            visible = iValue;
            return *this;
        }
        WindowConfig& WithMinSize(uint32_t iWidth, uint32_t iHeight) {
            minSize = Mashiro::SizeU{iWidth, iHeight};
            return *this;
        }
        WindowConfig& WithMaxSize(uint32_t iWidth, uint32_t iHeight) {
            maxSize = Mashiro::SizeU{iWidth, iHeight};
            return *this;
        }
        WindowConfig& WithOwnerWindow(Window* iOwner) {
            ownerWindow = iOwner;
            return *this;
        }
        WindowConfig& WithEventCallback(std::function<void(const Event&)> iCallback) {
            eventCallback = std::move(iCallback);
            return *this;
        }
    };

    // ============================================================================
    // Cursor Types
    // ============================================================================

    enum class CursorType {
        Arrow,
        IBeam,
        Crosshair,
        Hand,
        ResizeNS,
        ResizeEW,
        ResizeNESW,
        ResizeNWSE,
        Move,
        NotAllowed,
        Wait,
        Progress,
        Hidden,
    };

    // ============================================================================
    // Window State
    // ============================================================================

    enum class WindowState {
        Normal,
        Minimized,
        Maximized,
        Fullscreen,
    };

    // ============================================================================
    // Window Interface
    // ============================================================================

    class Window {
    public:
        virtual ~Window() = default;

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&) noexcept = default;
        Window& operator=(Window&&) noexcept = default;

        // Title
        virtual void SetTitle(std::string_view iTitle) = 0;
        [[nodiscard]] virtual std::string GetTitle() const = 0;

        // Size
        virtual void SetSize(Mashiro::SizeU iSize) = 0;
        [[nodiscard]] virtual Mashiro::SizeU GetSize() const = 0;

        // Position
        virtual void SetPosition(Mashiro::PointI iPosition) = 0;
        [[nodiscard]] virtual Mashiro::PointI GetPosition() const = 0;

        // Size constraints
        virtual void SetMinSize(std::optional<Mashiro::SizeU> iSize) = 0;
        virtual void SetMaxSize(std::optional<Mashiro::SizeU> iSize) = 0;

        // Visibility
        virtual void SetVisible(bool iVisible) = 0;
        [[nodiscard]] virtual bool IsVisible() const = 0;

        // State
        virtual void SetState(WindowState iState) = 0;
        [[nodiscard]] virtual WindowState GetState() const = 0;

        // Properties
        virtual void SetResizable(bool iResizable) = 0;
        [[nodiscard]] virtual bool IsResizable() const = 0;

        virtual void SetDecorated(bool iDecorated) = 0;
        [[nodiscard]] virtual bool IsDecorated() const = 0;

        virtual void SetAlwaysOnTop(bool iAlwaysOnTop) = 0;
        [[nodiscard]] virtual bool IsAlwaysOnTop() const = 0;

        // Cursor
        virtual void SetCursor(CursorType iCursor) = 0;
        [[nodiscard]] virtual CursorType GetCursor() const = 0;

        virtual void SetCursorVisible(bool iVisible) = 0;
        [[nodiscard]] virtual bool IsCursorVisible() const = 0;

        virtual void SetCursorGrab(bool iGrab) = 0;
        [[nodiscard]] virtual bool IsCursorGrabbed() const = 0;

        virtual void SetCursorPosition(Mashiro::PointI iPosition) = 0;
        [[nodiscard]] virtual Mashiro::PointI GetCursorPosition() const = 0;

        virtual void SetCursorScreenPosition(Mashiro::PointI iPosition) = 0;
        [[nodiscard]] virtual Mashiro::PointI GetCursorScreenPosition() const = 0;

        // Focus
        virtual void Focus() = 0;
        [[nodiscard]] virtual bool IsFocused() const = 0;

        // Redraw
        virtual void RequestRedraw() = 0;

        // Close
        virtual void Close() = 0;
        [[nodiscard]] virtual bool ShouldClose() const = 0;

        // Surface
        [[nodiscard]] virtual Surface& GetSurface() = 0;
        [[nodiscard]] virtual const Surface& GetSurface() const = 0;

        // Opacity
        virtual void SetOpacity(float iOpacity) = 0;
        [[nodiscard]] virtual float GetOpacity() const = 0;

        // Icon
        virtual void SetIcon(std::span<const uint32_t> iPixels, Mashiro::SizeU iSize) = 0;

        // Utilities
        virtual void BringToFront() = 0;
        virtual void SendToBack() = 0;
        virtual void Flash() = 0;
        virtual void CenterOnScreen() = 0;

        // Screen info
        [[nodiscard]] virtual Mashiro::PointI GetScreenSize() const = 0;
        [[nodiscard]] virtual Mashiro::PointI ClientToScreen(Mashiro::PointI iClientPos) const = 0;
        [[nodiscard]] virtual Mashiro::PointI ScreenToClient(Mashiro::PointI iScreenPos) const = 0;

        // IME (Input Method Editor) support
        // Set the position where IME candidate window should appear (in client coordinates)
        virtual void SetIMEPosition(Mashiro::PointI iPosition) = 0;

        // Native handle
        [[nodiscard]] virtual void* GetNativeHandle() const = 0;

        // Unique ID
        [[nodiscard]] virtual uint64_t GetId() const = 0;

        // Convenience
        [[nodiscard]] bool IsOpen() const { return !ShouldClose(); }

        // ========================================================================
        // Event Handling
        // ========================================================================

        using EventCallback = std::function<void(const Event&)>;

        // Set a callback to receive all window events
        void SetEventCallback(EventCallback iCallback) { eventCallback_ = std::move(iCallback); }

        // Dispatch an event to the callback (called by platform backend)
        void DispatchEvent(const Event& iEvent) {
            if (eventCallback_) {
                eventCallback_(iEvent);
            }
        }

    protected:
        Window() = default;

    private:
        EventCallback eventCallback_;
    };

    // ============================================================================
    // Window Handle (lightweight reference)
    // ============================================================================

    class WindowHandle {
    public:
        WindowHandle() = default;
        explicit WindowHandle(Window* iWindow) : window_(iWindow) {}

        [[nodiscard]] Window* Get() const { return window_; }
        Window* operator->() const { return window_; }
        Window& operator*() const { return *window_; }
        explicit operator bool() const { return window_ != nullptr; }

        bool operator==(const WindowHandle& iOther) const { return window_ == iOther.window_; }

    private:
        Window* window_ = nullptr;
    };

} // namespace Mashiro
