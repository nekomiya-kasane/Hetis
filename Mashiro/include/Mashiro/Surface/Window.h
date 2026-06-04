// nekomiya-mixed4: Modern UI Engine
// Platform Window - Cross-platform window abstraction

#pragma once

#include "neko/common/Geometry.h"
#include "neko/platform/Event.h"
#include "neko/platform/Surface.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Mashiro {

    namespace Platform {

        

    }

// ============================================================================
// Forward Declarations
// ============================================================================

class Window;
class EventLoop;

using WindowPtr = std::unique_ptr<Window>;

// ============================================================================
// Window Configuration
// ============================================================================

struct WindowConfig {
    std::string title = "Window";
    uint32_t width = 800;
    uint32_t height = 600;
    std::optional<::PointI> position = std::nullopt;
    bool resizable = true;
    bool decorated = true;
    bool transparent = false;
    bool alwaysOnTop = false;
    bool maximized = false;
    bool minimized = false;
    bool visible = true;
    std::optional<::SizeU> minSize = std::nullopt;
    std::optional<::SizeU> maxSize = std::nullopt;
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
        position = ::PointI{iX, iY};
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
        minSize = ::SizeU{iWidth, iHeight};
        return *this;
    }
    WindowConfig& WithMaxSize(uint32_t iWidth, uint32_t iHeight) {
        maxSize = ::SizeU{iWidth, iHeight};
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
    virtual void SetSize(::SizeU iSize) = 0;
    [[nodiscard]] virtual ::SizeU GetSize() const = 0;

    // Position
    virtual void SetPosition(::PointI iPosition) = 0;
    [[nodiscard]] virtual ::PointI GetPosition() const = 0;

    // Size constraints
    virtual void SetMinSize(std::optional<::SizeU> iSize) = 0;
    virtual void SetMaxSize(std::optional<::SizeU> iSize) = 0;

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

    virtual void SetCursorPosition(::PointI iPosition) = 0;
    [[nodiscard]] virtual ::PointI GetCursorPosition() const = 0;

    virtual void SetCursorScreenPosition(::PointI iPosition) = 0;
    [[nodiscard]] virtual ::PointI GetCursorScreenPosition() const = 0;

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
    virtual void SetIcon(std::span<const uint32_t> iPixels, ::SizeU iSize) = 0;

    // Utilities
    virtual void BringToFront() = 0;
    virtual void SendToBack() = 0;
    virtual void Flash() = 0;
    virtual void CenterOnScreen() = 0;

    // Screen info
    [[nodiscard]] virtual ::PointI GetScreenSize() const = 0;
    [[nodiscard]] virtual ::PointI ClientToScreen(::PointI iClientPos) const = 0;
    [[nodiscard]] virtual ::PointI ScreenToClient(::PointI iScreenPos) const = 0;

    // IME (Input Method Editor) support
    // Set the position where IME candidate window should appear (in client coordinates)
    virtual void SetIMEPosition(::PointI iPosition) = 0;

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

} // namespace neko::platform
