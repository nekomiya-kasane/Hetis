/**
 * @file SystemAPI.h
 * @brief Dynamically resolved operating-system entry points used by the platform abstraction layer.
 * @ingroup PAL
 *
 * @details The accessors in this file initialize immutable, process-lifetime function tables on first use. Callers
 * must test optional entry points before invoking them because an older operating system may not export them.
 *
 * @code{.cpp}
 * const Sora::PAL::ThreadSystemAPI& api = Sora::PAL::LoadThreadSystemAPI();
 * if (api.getCurrentThread != nullptr && api.setThreadDescription != nullptr) {
 *     api.setThreadDescription(api.getCurrentThread(), L"worker");
 * }
 * @endcode
 *
 * @par Window-system selection
 * @code{.cpp}
 * using namespace Sora::PAL;
 * #if defined(PLATFORM_WINDOWS)
 * const auto& windows = LoadWin32WindowSystemAPI();
 * if (windows.Available()) {
 *     const auto capabilities = windows.DeclaredCapabilities();
 *     // Pass NativeWindowView{Platform::WindowSystem::Win32, NativeWindowBackend::Win32, ...} to WSI.
 * }
 * #elif defined(PLATFORM_LINUX) && !defined(__ANDROID__)
 * const auto& xlib = LoadX11WindowSystemAPI();
 * const auto& xcb = LoadXcbWindowSystemAPI();
 * const auto& wayland = LoadWaylandWindowSystemAPI();
 * if (xlib.Available() || xcb.Available() || wayland.Available()) {
 *     // Prefer the backend whose connection was established by the application.
 * }
 * #endif
 * @endcode
 *
 * @note Linux exposes Xlib, XCB, and Wayland as separate client ABIs. Xlib and XCB both speak the X11 protocol,
 * but their connection objects and event ownership rules cannot be mixed. The PAL therefore exposes independent
 * tables and preserves the chosen backend in @ref NativeWindowView.
 *
 * @note @ref Module owns the native dynamic-loader bootstrap and typed symbol lookup. Loader bootstrap primitives and
 * immediate last-error capture are the only system entry points called directly because they cannot resolve
 * themselves, and symbol resolution can overwrite the error state that must be captured.
 */
#pragma once

#include <Sora/Core/FixedString.h>
#include <Sora/ErrorCode.h>
#include <Sora/Platform.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

struct addrinfo;
struct sockaddr;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
struct kevent;
struct pollfd;
struct rusage;
struct stat;
struct timespec;
#endif

#if defined(PLATFORM_LINUX) && !defined(__ANDROID__)
struct _XDisplay;
union _XEvent;
struct _XIC;
struct _XIM;
struct _XKeyEvent;
struct _XSetWindowAttributes;
struct _XVisual;
struct _XWindowAttributes;
struct _XrmHashBucketRec;
struct wl_display;
struct wl_interface;
struct wl_proxy;
struct wl_registry;
struct wl_surface;
struct xcb_connection_t;
struct xcb_generic_event_t;
struct xcb_generic_error_t;
struct xcb_intern_atom_reply_t;
struct xcb_setup_t;
#endif

#if defined(PLATFORM_WINDOWS)

struct _EXCEPTION_POINTERS;
struct _IMAGEHLP_LINE64;
struct _IMAGEHLP_MODULE64;
struct _EVENT_TRACE_LOGFILEW;
struct _EVENT_TRACE_PROPERTIES;
struct _FILETIME;
struct _PROCESS_MEMORY_COUNTERS;
union _LARGE_INTEGER;
struct _OVERLAPPED;
struct _SECURITY_ATTRIBUTES;
struct _SYMBOL_INFO;
struct _WSABUF;
struct tagPROCESSENTRY32W;
#endif

namespace Sora::PAL {

    namespace $ {

        struct Macro {
            enum class Type : uint8_t { Mask, Serial };
            FixedString<256> value = "";
            Type type = Type::Serial;
        };

        /** @brief Associate a function-table entry with one exact native symbol that it exposes or delegates to. */
        struct Syscall {
            FixedString<256> name = ""; /**< Case-sensitive export name passed to the native symbol resolver. */
            Platform::OperatingSystem os = Platform::kOperatingSystem; /**< Operating system that exports the symbol. */
        };

        /** @brief Associate a function-table entry with one exact native variable that it exposes or delegates to. */
        struct Var {
            FixedString<256> name = ""; /**< Case-sensitive export name passed to the native symbol resolver. */
            Platform::OperatingSystem os = Platform::kOperatingSystem; /**< Operating system that exports the symbol. */
        };

    } // namespace $

    /** @brief Platform-normalized counters produced by native process-accounting adapters. */
    struct ProcessUsageCounters {
        uint64_t userCpuNanoseconds = 0;
        uint64_t kernelCpuNanoseconds = 0;
        uint64_t residentMemoryBytes = 0;
        uint64_t peakResidentMemoryBytes = 0;
    };

    /** @brief Platform-normalized metadata delivered from a native fatal exception or signal. */
    struct NativeCrashContext {
        std::string_view reason{};        /**< Static native exception or signal name. */
        uint32_t code = 0;                /**< Native exception code or signal number. */
        uintptr_t faultAddress = 0;       /**< Faulting memory address when available. */
        uintptr_t instructionPointer = 0; /**< Native instruction pointer when available. */
    };

    /** @brief Allocation-free callback invoked by the normalized native crash adapter. */
    using NativeCrashCallback = void (*)(const NativeCrashContext& context, void* userData) noexcept;

    /** @brief Native scheduler provider family after platform GUID or tracepoint normalization. */
    enum class NativeSchedulerTraceProvider : uint8_t {
        Process,
        Thread,
        Unknown,
    };

    /** @brief Pointer width used by pointer-sized fields in one native event payload. */
    enum class NativeSchedulerTracePointerWidth : uint8_t {
        Bits32 = 4,
        Bits64 = 8,
    };

    /** @brief Platform-neutral native scheduler record valid only for the duration of its callback. */
    struct NativeSchedulerTraceRecord {
        NativeSchedulerTraceProvider provider = NativeSchedulerTraceProvider::Unknown;
        uint8_t opcode = 0;
        NativeSchedulerTracePointerWidth pointerWidth = NativeSchedulerTracePointerWidth::Bits64;
        int64_t timestampTicks = 0;
        uint32_t processorIndex = 0;
        std::span<const std::byte> payload{};
    };

    /** @brief Native buffering parameters for the scheduler trace transport. */
    struct NativeSchedulerTraceOptions {
        uint32_t bufferSizeKilobytes = 64;
        uint32_t minimumBuffers = 64;
        uint32_t maximumBuffers = 256;
    };

    /** @brief Native transport loss totals captured when the session stops. */
    struct NativeSchedulerTraceLosses {
        uint64_t eventsLost = 0;
        uint64_t logBuffersLost = 0;
        uint64_t realtimeBuffersLost = 0;
    };

    /** @brief Platform-normalized module and symbol information for one native address. */
    struct ModuleAddressInfo {
        const char* modulePath = nullptr; /**< Native module path containing the address, when available. */
        void* moduleBase = nullptr;       /**< Native load address of the containing module, when available. */
        const char* symbolName = nullptr; /**< Native symbol spelling, when available. */
        void* symbolAddress = nullptr;    /**< Native start address of the resolved symbol, when available. */
    };

    /** @brief Owned, platform-neutral result of resolving one address through Windows DbgHelp. */
    struct DbgHelpAddressInfo {
        std::string symbol;      /**< Native symbol spelling, or empty when unavailable. */
        std::string modulePath;  /**< Native module image path, or empty when unavailable. */
        std::string sourceFile;  /**< Source file path, or empty when unavailable. */
        uint32_t sourceLine = 0; /**< One-based source line, or zero when unavailable. */
        uint64_t offset = 0;     /**< Byte displacement from the resolved symbol start. */
    };

    /** @brief Signed desktop coordinate rectangle used by display and windowing adapters. */
    struct DisplayRect {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
    };

    /** @brief Signed desktop coordinate point used by display and windowing adapters. */
    struct DisplayPoint {
        int32_t x = 0;
        int32_t y = 0;
    };

    /** @brief DPI awareness mode understood by the Windows display adapter. */
    enum class DisplayDpiAwareness : int {
        Unaware = 0,
        SystemAware,
        PerMonitorAware,
    };

    /** @brief Fallback policy used when a point or window is not on a monitor. */
    enum class DisplayMonitorSelection : uint32_t {
        Null = 0,
        Primary = 1,
        Nearest = 2,
    };

    /** @brief DPI quantity requested from the legacy per-monitor DPI query. */
    enum class DisplayDpiType : int {
        Effective = 0,
        Angular = 1,
        Raw = 2,
    };

    /** @brief Concrete native client ABI carried by a non-owning window view. */
    enum class NativeWindowBackend : uint8_t {
        Unknown = 0,
        Win32,
        Xlib,
        Xcb,
        Wayland,
    };

    /** @brief Non-owning native view required by a window backend or Vulkan WSI adapter. */
    struct NativeWindowView {
        Platform::WindowSystem windowSystem = Platform::WindowSystem::Unknown;
        NativeWindowBackend backend = NativeWindowBackend::Unknown;
        void* displayConnection = nullptr; /**< HWND uses null; X11/XCB/Wayland use their connection object. */
        uintptr_t window = 0;              /**< HWND/X11/XCB identifier, or an opaque native-window pointer. */
        void* surface = nullptr;           /**< Wayland wl_surface or another backend-specific surface object. */

        /** @brief Return whether the view satisfies the native-handle invariant for its concrete backend. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            switch (backend) {
                case NativeWindowBackend::Win32:
                    return windowSystem == Platform::WindowSystem::Win32 && window != 0 && displayConnection == nullptr;
                case NativeWindowBackend::Xlib:
                case NativeWindowBackend::Xcb:
                    return windowSystem == Platform::WindowSystem::X11 && displayConnection != nullptr && window != 0;
                case NativeWindowBackend::Wayland:
                    return windowSystem == Platform::WindowSystem::Wayland && displayConnection != nullptr &&
                           surface != nullptr;
                case NativeWindowBackend::Unknown:
                    return false;
            }
            return false;
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }
    };

    /** @brief Capabilities that a native window backend may expose without implying semantic equivalence. */
    enum class WindowSystemFeature : uint64_t {
        None = 0,
        WindowCreation = uint64_t{1} << 0,
        EventQueue = uint64_t{1} << 1,
        WindowPosition = uint64_t{1} << 2,
        WindowResize = uint64_t{1} << 3,
        WindowState = uint64_t{1} << 4,
        PointerCapture = uint64_t{1} << 5,
        TextInput = uint64_t{1} << 6,
        VulkanSurface = uint64_t{1} << 7,
        DisplayFd = uint64_t{1} << 8,
    };

    /** @brief Raw feature mask for one dynamically loaded native window backend. */
    struct WindowSystemCapabilities {
        Platform::WindowSystem windowSystem = Platform::WindowSystem::Unknown;
        NativeWindowBackend backend = NativeWindowBackend::Unknown;
        uint64_t features = 0;

        [[nodiscard]] constexpr bool Supports(WindowSystemFeature feature) const noexcept {
            return (features & std::to_underlying(feature)) != 0;
        }
    };

#if defined(PLATFORM_WINDOWS)

    /** @brief ABI-only Win32 declarations used by @ref Win32WindowSystemAPI without including Windows.h. */
    namespace WindowsSystem {

        using Bool = int;
        using Byte = uint8_t;
        using Char = char;
        using DWord = unsigned long;
        using DWord64 = unsigned long long;
        using Float = float;
        using Handle = void*;
        using HResult = long;
        using Int = int;
        using Int8 = int8_t;
        using Int16 = int16_t;
        using Int32 = int32_t;
        using Int64 = int64_t;
        using Long = long;
        using PointerSizedLong = intptr_t;
        using ConstCharPointer = const char*;
        using ConstVoidPointer = const void*;
        using ConstWideCharPointer = const wchar_t*;
        using CharPointer = char*;
        using VoidPointer = void*;
        using WideCharPointer = wchar_t*;
        using RpcInterfaceHandle = void*;
        using AutomationString = wchar_t*;
        using WindowHandle = void*;
        using ModuleHandle = void*;
        using MonitorHandle = void*;
        using DeviceContextHandle = void*;
        using Size = size_t;
        using UInt = unsigned int;
        using UInt8 = uint8_t;
        using UInt16 = uint16_t;
        using UInt32 = uint32_t;
        using UInt64 = uint64_t;
        using ULong = unsigned long;
        using PointerSizedULong = uintptr_t;
        using WChar = wchar_t;
        using WindowAtom = uint16_t;
        using WindowMessageId = uint32_t;
        using WindowProcedureResult = intptr_t;
        using Socket = uintptr_t;
        using WindowProcedure = WindowProcedureResult(__stdcall*)(WindowHandle, WindowMessageId, uintptr_t, intptr_t);

        /** @brief Layout-compatible Win32 globally unique identifier. */
        struct Guid {
            uint32_t data1;
            uint16_t data2;
            uint16_t data3;
            uint8_t data4[8];
        };

        using InterfaceId = Guid;
        using ClassId = Guid;
        using Uuid = Guid;
        using RefGuid = const Guid&;
        using RefInterfaceId = const InterfaceId&;
        using RefClassId = const ClassId&;

        /** @brief Layout-compatible Win32 locally unique identifier. */
        struct Luid {
            DWord lowPart;
            Long highPart;
        };

        /** @brief Layout-compatible signed 64-bit Win32 integer union. */
        union LargeInteger {
            struct {
                DWord lowPart;
                Long highPart;
            };
            int64_t quadPart;
        };

        using Rect = DisplayRect;
        using Point = DisplayPoint;

        /** @brief Layout-compatible Win32 security-attributes record. */
        struct SecurityAttributes {
            DWord length;
            void* securityDescriptor;
            Bool inheritHandle;
        };

        /** @brief Layout-compatible Win32 file timestamp. */
        struct FileTime {
            DWord lowDateTime;
            DWord highDateTime;
        };

        /** @brief ABI-compatible COM root interface. */
        struct Unknown {
            virtual HResult __stdcall QueryInterface(RefInterfaceId iid, void** object) = 0;
            virtual ULong __stdcall AddRef() = 0;
            virtual ULong __stdcall Release() = 0;
        };

        /** @brief ABI-compatible COM sequential byte-stream interface. */
        struct SequentialStream : Unknown {
            virtual HResult __stdcall Read(void* destination, ULong bytes, ULong* bytesRead) = 0;
            virtual HResult __stdcall Write(const void* source, ULong bytes, ULong* bytesWritten) = 0;
        };

        struct Stream;
        struct PropertyBag;
        struct MemoryAllocator;

        template<size_t Size>
        struct EnumInteger;

        template<>
        struct EnumInteger<1> {
            using Type = int8_t;
        };

        template<>
        struct EnumInteger<2> {
            using Type = int16_t;
        };

        template<>
        struct EnumInteger<4> {
            using Type = int32_t;
        };

        template<>
        struct EnumInteger<8> {
            using Type = int64_t;
        };

        template<typename T>
        struct EnumFlagSizedInteger {
            using Type = typename EnumInteger<sizeof(T)>::Type;
        };

        /** @brief Layout-compatible Win32 @c WNDCLASSEXW record. */
        struct WindowClassEx {
            uint32_t size = sizeof(WindowClassEx);
            DWord style = 0;
            WindowProcedure windowProcedure = nullptr;
            int32_t classExtraBytes = 0;
            int32_t windowExtraBytes = 0;
            Handle instance = nullptr;
            Handle icon = nullptr;
            Handle cursor = nullptr;
            Handle backgroundBrush = nullptr;
            const wchar_t* menuName = nullptr;
            const wchar_t* className = nullptr;
            Handle smallIcon = nullptr;
        };

        /** @brief Layout-compatible Win32 @c MSG record used by message-pump entry points. */
        struct WindowMessage {
            WindowHandle window = nullptr;
            WindowMessageId message = 0;
            uintptr_t wParam = 0;
            intptr_t lParam = 0;
            DWord time = 0;
            DisplayPoint point{};
        };

        /** @brief ABI-compatible Winsock readiness descriptor. */
        struct PollDescriptor {
            Socket descriptor = static_cast<Socket>(-1);
            int16_t events = 0;
            int16_t revents = 0;
        };
        using NativePollDescriptor = PollDescriptor;

    } // namespace WindowsSystem

    /** @brief Dynamically resolved Win32 window-class, lifecycle, input, and message-pump entry points. */
    struct Win32WindowSystemAPI {
        // clang-format off
        using RegisterClassExWideFunction = WindowsSystem::WindowAtom(__stdcall*)(const WindowsSystem::WindowClassEx*);
        using UnregisterClassWideFunction = WindowsSystem::Bool(__stdcall*)(const wchar_t*, WindowsSystem::Handle);
        using CreateWindowExWideFunction  = WindowsSystem::WindowHandle(__stdcall*)(WindowsSystem::DWord, const wchar_t*, const wchar_t*, WindowsSystem::DWord, int, int, int, int, WindowsSystem::WindowHandle, WindowsSystem::Handle, WindowsSystem::Handle, void*);
        using DestroyWindowFunction       = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle);
        using ShowWindowFunction          = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, int);
        using UpdateWindowFunction        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle);
        using SetWindowTextWideFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, const wchar_t*);
        using GetWindowLongPtrFunction    = intptr_t(__stdcall*)(WindowsSystem::WindowHandle, int);
        using SetWindowLongPtrFunction    = intptr_t(__stdcall*)(WindowsSystem::WindowHandle, int, intptr_t);
        using WindowProcedureFunction     = WindowsSystem::WindowProcedureResult(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::WindowMessageId, uintptr_t, intptr_t);
        using PeekMessageFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowMessage*, WindowsSystem::WindowHandle, WindowsSystem::WindowMessageId, WindowsSystem::WindowMessageId, WindowsSystem::DWord);
        using GetMessageFunction          = int(__stdcall*)(WindowsSystem::WindowMessage*, WindowsSystem::WindowHandle, WindowsSystem::WindowMessageId, WindowsSystem::WindowMessageId);
        using TranslateMessageFunction    = WindowsSystem::Bool(__stdcall*)(const WindowsSystem::WindowMessage*);
        using DispatchMessageFunction     = WindowsSystem::WindowProcedureResult(__stdcall*)(const WindowsSystem::WindowMessage*);
        using PostQuitMessageFunction     = void(__stdcall*)(int);
        using PostMessageFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::WindowMessageId, uintptr_t, intptr_t);
        using SendMessageFunction         = WindowsSystem::WindowProcedureResult(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::WindowMessageId, uintptr_t, intptr_t);
        using SetWindowPositionFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::WindowHandle, int, int, int, int, WindowsSystem::DWord);
        using SetCaptureFunction          = WindowsSystem::WindowHandle(__stdcall*)(WindowsSystem::WindowHandle);
        using ReleaseCaptureFunction      = WindowsSystem::Bool(__stdcall*)();
        using SetCursorFunction           = WindowsSystem::Handle(__stdcall*)(WindowsSystem::Handle);
        using ShowCursorFunction          = int(__stdcall*)(WindowsSystem::Bool);
        using SetCursorPositionFunction   = WindowsSystem::Bool(__stdcall*)(int, int);
        using ClipCursorFunction          = WindowsSystem::Bool(__stdcall*)(const DisplayRect*);
        using SetFocusFunction            = WindowsSystem::WindowHandle(__stdcall*)(WindowsSystem::WindowHandle);
        using GetFocusFunction            = WindowsSystem::WindowHandle(__stdcall*)();
        using SetForegroundWindowFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle);
        using IsWindowFunction            = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle);
        using IsWindowVisibleFunction     = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle);
        using IsIconicFunction            = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle);
        using IsZoomedFunction            = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle);
        using WaitMessageFunction         = WindowsSystem::Bool(__stdcall*)();
        using MsgWaitForMultipleObjectsExFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord, const WindowsSystem::Handle*, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord);

        [[= $::Syscall{"RegisterClassExW"}]]            RegisterClassExWideFunction registerClassExWide = nullptr;
        [[= $::Syscall{"UnregisterClassW"}]]            UnregisterClassWideFunction unregisterClassWide = nullptr;
        [[= $::Syscall{"CreateWindowExW"}]]             CreateWindowExWideFunction createWindowExWide = nullptr;
        [[= $::Syscall{"DestroyWindow"}]]               DestroyWindowFunction destroyWindow = nullptr;
        [[= $::Syscall{"ShowWindow"}]]                  ShowWindowFunction showWindow = nullptr;
        [[= $::Syscall{"UpdateWindow"}]]                UpdateWindowFunction updateWindow = nullptr;
        [[= $::Syscall{"SetWindowTextW"}]]              SetWindowTextWideFunction setWindowTextWide = nullptr;
        [[= $::Syscall{"GetWindowLongPtrW"}]]           GetWindowLongPtrFunction getWindowLongPtr = nullptr;
        [[= $::Syscall{"SetWindowLongPtrW"}]]           SetWindowLongPtrFunction setWindowLongPtr = nullptr;
        [[= $::Syscall{"DefWindowProcW"}]]              WindowProcedureFunction defWindowProc = nullptr;
        [[= $::Syscall{"PeekMessageW"}]]                PeekMessageFunction peekMessage = nullptr;
        [[= $::Syscall{"GetMessageW"}]]                 GetMessageFunction getMessage = nullptr;
        [[= $::Syscall{"TranslateMessage"}]]            TranslateMessageFunction translateMessage = nullptr;
        [[= $::Syscall{"DispatchMessageW"}]]            DispatchMessageFunction dispatchMessage = nullptr;
        [[= $::Syscall{"PostQuitMessage"}]]             PostQuitMessageFunction postQuitMessage = nullptr;
        [[= $::Syscall{"PostMessageW"}]]                PostMessageFunction postMessage = nullptr;
        [[= $::Syscall{"SendMessageW"}]]                SendMessageFunction sendMessage = nullptr;
        [[= $::Syscall{"SetWindowPos"}]]                SetWindowPositionFunction setWindowPosition = nullptr;
        [[= $::Syscall{"SetCapture"}]]                  SetCaptureFunction setCapture = nullptr;
        [[= $::Syscall{"ReleaseCapture"}]]              ReleaseCaptureFunction releaseCapture = nullptr;
        [[= $::Syscall{"SetCursor"}]]                   SetCursorFunction setCursor = nullptr;
        [[= $::Syscall{"ShowCursor"}]]                  ShowCursorFunction showCursor = nullptr;
        [[= $::Syscall{"SetCursorPos"}]]                SetCursorPositionFunction setCursorPosition = nullptr;
        [[= $::Syscall{"ClipCursor"}]]                  ClipCursorFunction clipCursor = nullptr;
        [[= $::Syscall{"SetFocus"}]]                    SetFocusFunction setFocus = nullptr;
        [[= $::Syscall{"GetFocus"}]]                    GetFocusFunction getFocus = nullptr;
        [[= $::Syscall{"SetForegroundWindow"}]]         SetForegroundWindowFunction setForegroundWindow = nullptr;
        [[= $::Syscall{"IsWindow"}]]                    IsWindowFunction isWindow = nullptr;
        [[= $::Syscall{"IsWindowVisible"}]]             IsWindowVisibleFunction isWindowVisible = nullptr;
        [[= $::Syscall{"IsIconic"}]]                    IsIconicFunction isIconic = nullptr;
        [[= $::Syscall{"IsZoomed"}]]                    IsZoomedFunction isZoomed = nullptr;
        [[= $::Syscall{"WaitMessage"}]]                 WaitMessageFunction waitMessage = nullptr;
        [[= $::Syscall{"MsgWaitForMultipleObjectsEx"}]] MsgWaitForMultipleObjectsExFunction msgWaitForMultipleObjectsEx = nullptr;
        // clang-format on

        /** @brief Return the Win32 contract represented by this table, independent of optional symbol availability. */
        [[nodiscard]] static constexpr WindowSystemCapabilities DeclaredCapabilities() noexcept {
            return {.windowSystem = Platform::WindowSystem::Win32,
                    .backend = NativeWindowBackend::Win32,
                    .features = std::to_underlying(WindowSystemFeature::WindowCreation) |
                                std::to_underlying(WindowSystemFeature::EventQueue) |
                                std::to_underlying(WindowSystemFeature::WindowPosition) |
                                std::to_underlying(WindowSystemFeature::WindowResize) |
                                std::to_underlying(WindowSystemFeature::WindowState) |
                                std::to_underlying(WindowSystemFeature::PointerCapture) |
                                std::to_underlying(WindowSystemFeature::TextInput) |
                                std::to_underlying(WindowSystemFeature::VulkanSurface)};
        }

        /** @brief Return whether the minimum window lifecycle and message-pump symbols were resolved. */
        [[nodiscard]] constexpr bool Available() const noexcept {
            return registerClassExWide != nullptr && createWindowExWide != nullptr && destroyWindow != nullptr &&
                   defWindowProc != nullptr && peekMessage != nullptr && translateMessage != nullptr &&
                   dispatchMessage != nullptr;
        }
    };

#else

    /** @brief Win32 window entry points are unavailable on non-Windows targets. */
    struct Win32WindowSystemAPI {};

#endif

#if defined(PLATFORM_LINUX) && !defined(__ANDROID__)

    /** @brief Opaque Xlib ABI types kept out of the public header's dependency graph. */
    using X11Display = ::_XDisplay;
    using X11Event = ::_XEvent;
    using X11InputContext = ::_XIC;
    using X11InputMethod = ::_XIM;
    using X11KeyEvent = ::_XKeyEvent;
    using X11KeySym = unsigned long;
    using X11ResourceDatabase = ::_XrmHashBucketRec;
    using X11Status = int;
    using X11SetWindowAttributes = ::_XSetWindowAttributes;
    using X11Visual = ::_XVisual;
    using X11WindowAttributes = ::_XWindowAttributes;
    using X11Window = unsigned long;
    using X11Atom = unsigned long;
    using X11Cursor = unsigned long;

    /** @brief Dynamically resolved Xlib window, event, focus, and pointer entry points. */
    struct X11WindowSystemAPI {
        // clang-format off
        using OpenDisplayFunction          = X11Display* (*)(const char*);
        using CloseDisplayFunction         = int (*)(X11Display*);
        using DefaultScreenFunction        = int (*)(X11Display*);
        using RootWindowFunction           = X11Window (*)(X11Display*, int);
        using DefaultVisualFunction        = X11Visual* (*)(X11Display*, int);
        using DefaultDepthFunction         = int (*)(X11Display*, int);
        using CreateSimpleWindowFunction   = X11Window (*)(X11Display*, X11Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long, unsigned long);
        using CreateWindowFunction         = X11Window (*)(X11Display*, X11Window, int, int, unsigned, unsigned, unsigned, int, unsigned, X11Visual*, unsigned long, X11SetWindowAttributes*);
        using DestroyWindowFunction        = int (*)(X11Display*, X11Window);
        using MapWindowFunction            = int (*)(X11Display*, X11Window);
        using UnmapWindowFunction          = int (*)(X11Display*, X11Window);
        using MoveWindowFunction           = int (*)(X11Display*, X11Window, int, int);
        using ResizeWindowFunction         = int (*)(X11Display*, X11Window, unsigned, unsigned);
        using MoveResizeWindowFunction     = int (*)(X11Display*, X11Window, int, int, unsigned, unsigned);
        using SelectInputFunction          = int (*)(X11Display*, X11Window, long);
        using StoreNameFunction            = int (*)(X11Display*, X11Window, const char*);
        using InternAtomFunction           = X11Atom (*)(X11Display*, const char*, int);
        using SetWmProtocolsFunction       = int (*)(X11Display*, X11Window, X11Atom*, int);
        using PendingFunction              = int (*)(X11Display*);
        using NextEventFunction            = int (*)(X11Display*, X11Event*);
        using CheckMaskEventFunction       = int (*)(X11Display*, long, X11Event*);
        using ConnectionNumberFunction     = int (*)(X11Display*);
        using FlushFunction                = int (*)(X11Display*);
        using SyncFunction                 = int (*)(X11Display*, int);
        using FreeFunction                 = int (*)(void*);
        using GetWindowAttributesFunction  = int (*)(X11Display*, X11Window, X11WindowAttributes*);
        using SetInputFocusFunction        = int (*)(X11Display*, X11Window, int, unsigned long);
        using IconifyWindowFunction        = int (*)(X11Display*, X11Window, int);
        using RaiseWindowFunction          = int (*)(X11Display*, X11Window);
        using GrabPointerFunction          = int (*)(X11Display*, X11Window, int, unsigned, int, int, X11Window, X11Cursor, unsigned long);
        using UngrabPointerFunction        = int (*)(X11Display*, unsigned long);
        using WarpPointerFunction          = int (*)(X11Display*, X11Window, X11Window, int, int, unsigned, unsigned, int, int);
        using CreateFontCursorFunction     = X11Cursor (*)(X11Display*, unsigned);
        using DefineCursorFunction         = int (*)(X11Display*, X11Window, X11Cursor);
        using UndefineCursorFunction       = int (*)(X11Display*, X11Window);
        using FreeCursorFunction           = int (*)(X11Display*, X11Cursor);
        using OpenInputMethodFunction      = X11InputMethod* (*)(X11Display*, X11ResourceDatabase*, char*, char*);
        using CloseInputMethodFunction     = int (*)(X11InputMethod*);
        using CreateInputContextFunction   = X11InputContext* (*)(X11InputMethod*, ...);
        using DestroyInputContextFunction  = void (*)(X11InputContext*);
        using FilterEventFunction          = int (*)(X11Event*, X11Window);
        using Utf8LookupStringFunction     = int (*)(X11InputContext*, X11KeyEvent*, char*, int, X11KeySym*, X11Status*);

        [[= $::Syscall{"XOpenDisplay"}]]         OpenDisplayFunction openDisplay = nullptr;
        [[= $::Syscall{"XCloseDisplay"}]]        CloseDisplayFunction closeDisplay = nullptr;
        [[= $::Syscall{"XDefaultScreen"}]]       DefaultScreenFunction defaultScreen = nullptr;
        [[= $::Syscall{"XRootWindow"}]]          RootWindowFunction rootWindow = nullptr;
        [[= $::Syscall{"XDefaultVisual"}]]       DefaultVisualFunction defaultVisual = nullptr;
        [[= $::Syscall{"XDefaultDepth"}]]        DefaultDepthFunction defaultDepth = nullptr;
        [[= $::Syscall{"XCreateSimpleWindow"}]]  CreateSimpleWindowFunction createSimpleWindow = nullptr;
        [[= $::Syscall{"XCreateWindow"}]]        CreateWindowFunction createWindow = nullptr;
        [[= $::Syscall{"XDestroyWindow"}]]       DestroyWindowFunction destroyWindow = nullptr;
        [[= $::Syscall{"XMapWindow"}]]           MapWindowFunction mapWindow = nullptr;
        [[= $::Syscall{"XUnmapWindow"}]]         UnmapWindowFunction unmapWindow = nullptr;
        [[= $::Syscall{"XMoveWindow"}]]          MoveWindowFunction moveWindow = nullptr;
        [[= $::Syscall{"XResizeWindow"}]]        ResizeWindowFunction resizeWindow = nullptr;
        [[= $::Syscall{"XMoveResizeWindow"}]]    MoveResizeWindowFunction moveResizeWindow = nullptr;
        [[= $::Syscall{"XSelectInput"}]]         SelectInputFunction selectInput = nullptr;
        [[= $::Syscall{"XStoreName"}]]           StoreNameFunction storeName = nullptr;
        [[= $::Syscall{"XInternAtom"}]]          InternAtomFunction internAtom = nullptr;
        [[= $::Syscall{"XSetWMProtocols"}]]      SetWmProtocolsFunction setWmProtocols = nullptr;
        [[= $::Syscall{"XPending"}]]             PendingFunction pending = nullptr;
        [[= $::Syscall{"XNextEvent"}]]           NextEventFunction nextEvent = nullptr;
        [[= $::Syscall{"XCheckMaskEvent"}]]      CheckMaskEventFunction checkMaskEvent = nullptr;
        [[= $::Syscall{"XConnectionNumber"}]]    ConnectionNumberFunction connectionNumber = nullptr;
        [[= $::Syscall{"XFlush"}]]               FlushFunction flush = nullptr;
        [[= $::Syscall{"XSync"}]]                SyncFunction sync = nullptr;
        [[= $::Syscall{"XFree"}]]                FreeFunction free = nullptr;
        [[= $::Syscall{"XGetWindowAttributes"}]] GetWindowAttributesFunction getWindowAttributes = nullptr;
        [[= $::Syscall{"XSetInputFocus"}]]       SetInputFocusFunction setInputFocus = nullptr;
        [[= $::Syscall{"XIconifyWindow"}]]       IconifyWindowFunction iconifyWindow = nullptr;
        [[= $::Syscall{"XRaiseWindow"}]]         RaiseWindowFunction raiseWindow = nullptr;
        [[= $::Syscall{"XGrabPointer"}]]         GrabPointerFunction grabPointer = nullptr;
        [[= $::Syscall{"XUngrabPointer"}]]       UngrabPointerFunction ungrabPointer = nullptr;
        [[= $::Syscall{"XWarpPointer"}]]         WarpPointerFunction warpPointer = nullptr;
        [[= $::Syscall{"XCreateFontCursor"}]]    CreateFontCursorFunction createFontCursor = nullptr;
        [[= $::Syscall{"XDefineCursor"}]]        DefineCursorFunction defineCursor = nullptr;
        [[= $::Syscall{"XUndefineCursor"}]]      UndefineCursorFunction undefineCursor = nullptr;
        [[= $::Syscall{"XFreeCursor"}]]          FreeCursorFunction freeCursor = nullptr;
        [[= $::Syscall{"XOpenIM"}]]              OpenInputMethodFunction openInputMethod = nullptr;
        [[= $::Syscall{"XCloseIM"}]]             CloseInputMethodFunction closeInputMethod = nullptr;
        [[= $::Syscall{"XCreateIC"}]]            CreateInputContextFunction createInputContext = nullptr;
        [[= $::Syscall{"XDestroyIC"}]]           DestroyInputContextFunction destroyInputContext = nullptr;
        [[= $::Syscall{"XFilterEvent"}]]         FilterEventFunction filterEvent = nullptr;
        [[= $::Syscall{"Xutf8LookupString"}]]    Utf8LookupStringFunction utf8LookupString = nullptr;
        // clang-format on

        /** @brief Return the Xlib contract represented by this table, independent of optional symbol availability. */
        [[nodiscard]] static constexpr WindowSystemCapabilities DeclaredCapabilities() noexcept {
            return {.windowSystem = Platform::WindowSystem::X11,
                    .backend = NativeWindowBackend::Xlib,
                    .features = std::to_underlying(WindowSystemFeature::WindowCreation) |
                                std::to_underlying(WindowSystemFeature::EventQueue) |
                                std::to_underlying(WindowSystemFeature::WindowPosition) |
                                std::to_underlying(WindowSystemFeature::WindowResize) |
                                std::to_underlying(WindowSystemFeature::WindowState) |
                                std::to_underlying(WindowSystemFeature::PointerCapture) |
                                std::to_underlying(WindowSystemFeature::TextInput) |
                                std::to_underlying(WindowSystemFeature::VulkanSurface) |
                                std::to_underlying(WindowSystemFeature::DisplayFd)};
        }

        /** @brief Return whether the minimum Xlib connection, lifecycle, and event symbols were resolved. */
        [[nodiscard]] constexpr bool Available() const noexcept {
            return openDisplay != nullptr && closeDisplay != nullptr && createSimpleWindow != nullptr &&
                   destroyWindow != nullptr && mapWindow != nullptr && pending != nullptr && nextEvent != nullptr &&
                   connectionNumber != nullptr && flush != nullptr;
        }
    };

    using XcbConnection = ::xcb_connection_t;
    using XcbWindow = uint32_t;
    using XcbAtom = uint32_t;
    using XcbSetup = ::xcb_setup_t;
    using XcbGenericEvent = ::xcb_generic_event_t;
    using XcbGenericError = ::xcb_generic_error_t;
    using XcbInternAtomReply = ::xcb_intern_atom_reply_t;

    /** @brief ABI-compatible XCB cookies returned by requests that do not need a generated header. */
    struct XcbVoidCookie {
        uint32_t sequence = 0;
    };

    /** @brief ABI-compatible XCB intern-atom request cookie. */
    struct XcbInternAtomCookie {
        uint32_t sequence = 0;
    };

    /** @brief Dynamically resolved XCB connection, window, property, and event entry points.
     *
     * @details XCB is a distinct client ABI even though its server-side protocol is X11. Keep its native connection
     * and window identifiers separate from the Xlib table so Vulkan WSI selection remains unambiguous.
     */
    struct XcbWindowSystemAPI {
        // clang-format off
        using ConnectFunction                 = XcbConnection* (*)(const char*, int*);
        using DisconnectFunction              = void (*)(XcbConnection*);
        using ConnectionHasErrorFunction      = int (*)(XcbConnection*);
        using GetSetupFunction                = const XcbSetup* (*)(XcbConnection*);
        using GenerateIdFunction              = XcbWindow (*)(XcbConnection*);
        using CreateWindowFunction            = XcbVoidCookie (*)(XcbConnection*, uint8_t, XcbWindow, XcbWindow, int16_t, int16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, const uint32_t*);
        using ChangePropertyFunction          = XcbVoidCookie (*)(XcbConnection*, uint8_t, XcbWindow, XcbAtom, XcbAtom, uint8_t, uint32_t, const void*);
        using InternAtomFunction              = XcbInternAtomCookie (*)(XcbConnection*, uint8_t, uint16_t, const char*);
        using InternAtomReplyFunction         = XcbInternAtomReply* (*)(XcbConnection*, XcbInternAtomCookie, XcbGenericError**);
        using DestroyWindowFunction           = XcbVoidCookie (*)(XcbConnection*, XcbWindow);
        using MapWindowFunction               = XcbVoidCookie (*)(XcbConnection*, XcbWindow);
        using UnmapWindowFunction             = XcbVoidCookie (*)(XcbConnection*, XcbWindow);
        using ConfigureWindowFunction         = XcbVoidCookie (*)(XcbConnection*, XcbWindow, uint16_t, const uint32_t*);
        using PollForEventFunction            = XcbGenericEvent* (*)(XcbConnection*);
        using WaitForEventFunction            = XcbGenericEvent* (*)(XcbConnection*);
        using FlushFunction                   = int (*)(XcbConnection*);
        using GetFileDescriptorFunction       = int (*)(XcbConnection*);
        using GetMaximumRequestLengthFunction = uint32_t (*)(XcbConnection*);

        [[= $::Syscall{"xcb_connect"}]]                    ConnectFunction connect = nullptr;
        [[= $::Syscall{"xcb_disconnect"}]]                 DisconnectFunction disconnect = nullptr;
        [[= $::Syscall{"xcb_connection_has_error"}]]       ConnectionHasErrorFunction connectionHasError = nullptr;
        [[= $::Syscall{"xcb_get_setup"}]]                  GetSetupFunction getSetup = nullptr;
        [[= $::Syscall{"xcb_generate_id"}]]                GenerateIdFunction generateId = nullptr;
        [[= $::Syscall{"xcb_create_window"}]]              CreateWindowFunction createWindow = nullptr;
        [[= $::Syscall{"xcb_change_property"}]]            ChangePropertyFunction changeProperty = nullptr;
        [[= $::Syscall{"xcb_intern_atom"}]]                InternAtomFunction internAtom = nullptr;
        [[= $::Syscall{"xcb_intern_atom_reply"}]]          InternAtomReplyFunction internAtomReply = nullptr;
        [[= $::Syscall{"xcb_destroy_window"}]]             DestroyWindowFunction destroyWindow = nullptr;
        [[= $::Syscall{"xcb_map_window"}]]                 MapWindowFunction mapWindow = nullptr;
        [[= $::Syscall{"xcb_unmap_window"}]]               UnmapWindowFunction unmapWindow = nullptr;
        [[= $::Syscall{"xcb_configure_window"}]]           ConfigureWindowFunction configureWindow = nullptr;
        [[= $::Syscall{"xcb_poll_for_event"}]]             PollForEventFunction pollForEvent = nullptr;
        [[= $::Syscall{"xcb_wait_for_event"}]]             WaitForEventFunction waitForEvent = nullptr;
        [[= $::Syscall{"xcb_flush"}]]                      FlushFunction flush = nullptr;
        [[= $::Syscall{"xcb_get_file_descriptor"}]]        GetFileDescriptorFunction getFileDescriptor = nullptr;
        [[= $::Syscall{"xcb_get_maximum_request_length"}]] GetMaximumRequestLengthFunction getMaximumRequestLength = nullptr;
        // clang-format on

        /** @brief Return the XCB contract represented by this table, independent of optional symbol availability. */
        [[nodiscard]] static constexpr WindowSystemCapabilities DeclaredCapabilities() noexcept {
            return {.windowSystem = Platform::WindowSystem::X11,
                    .backend = NativeWindowBackend::Xcb,
                    .features = std::to_underlying(WindowSystemFeature::WindowCreation) |
                                std::to_underlying(WindowSystemFeature::EventQueue) |
                                std::to_underlying(WindowSystemFeature::WindowPosition) |
                                std::to_underlying(WindowSystemFeature::WindowResize) |
                                std::to_underlying(WindowSystemFeature::VulkanSurface) |
                                std::to_underlying(WindowSystemFeature::DisplayFd)};
        }

        /** @brief Return whether the minimum XCB connection, lifecycle, and event symbols were resolved. */
        [[nodiscard]] constexpr bool Available() const noexcept {
            return connect != nullptr && disconnect != nullptr && connectionHasError != nullptr &&
                   generateId != nullptr && createWindow != nullptr && destroyWindow != nullptr &&
                   pollForEvent != nullptr && flush != nullptr && getFileDescriptor != nullptr;
        }
    };

    using WaylandDisplay = ::wl_display;
    using WaylandInterface = ::wl_interface;
    using WaylandProxy = ::wl_proxy;
    using WaylandRegistry = ::wl_registry;
    using WaylandSurface = ::wl_surface;

    /** @brief Dynamically resolved Wayland client transport entry points.
     *
     * @details This table deliberately stops at libwayland-client. xdg-shell, xkb, fractional-scale, and other
     * protocol objects are generated by the concrete backend and must not be fabricated as transport functions here.
     */
    struct WaylandWindowSystemAPI {
        // clang-format off
        using ConnectFunction                 = WaylandDisplay* (*)(const char*);
        using ConnectFdFunction               = WaylandDisplay* (*)(int);
        using DisconnectFunction              = void (*)(WaylandDisplay*);
        using DispatchFunction                = int (*)(WaylandDisplay*);
        using DispatchPendingFunction         = int (*)(WaylandDisplay*);
        using RoundtripFunction               = int (*)(WaylandDisplay*);
        using FlushFunction                   = int (*)(WaylandDisplay*);
        using GetFileDescriptorFunction       = int (*)(WaylandDisplay*);
        using PrepareReadFunction             = int (*)(WaylandDisplay*);
        using CancelReadFunction              = void (*)(WaylandDisplay*);
        using ReadEventsFunction              = int (*)(WaylandDisplay*);
        using GetRegistryFunction             = WaylandRegistry* (*)(WaylandDisplay*);
        using ProxyAddListenerFunction        = int (*)(WaylandProxy*, void (**)(void), void*);
        using ProxyDestroyFunction            = void (*)(WaylandProxy*);
        using ProxyMarshalFlagsFunction       = WaylandProxy* (*)(WaylandProxy*, uint32_t, const WaylandInterface*, uint32_t, uint32_t, ...);
        using ProxyGetVersionFunction         = uint32_t (*)(const WaylandProxy*);
        using ProxySetUserDataFunction        = void (*)(WaylandProxy*, void*);
        using ProxyGetUserDataFunction        = void* (*)(const WaylandProxy*);
        using DisplayGetErrorFunction         = int (*)(WaylandDisplay*);
        using DisplayGetProtocolErrorFunction = uint32_t (*)(WaylandDisplay*, const WaylandInterface**, uint32_t*);

        [[= $::Syscall{"wl_display_connect"}]]
        ConnectFunction                 connect                 = nullptr;
        [[= $::Syscall{"wl_display_connect_to_fd"}]]
        ConnectFdFunction               connectToFd             = nullptr;
        [[= $::Syscall{"wl_display_disconnect"}]]
        DisconnectFunction              disconnect              = nullptr;
        [[= $::Syscall{"wl_display_dispatch"}]]
        DispatchFunction                dispatch                = nullptr;
        [[= $::Syscall{"wl_display_dispatch_pending"}]]
        DispatchPendingFunction         dispatchPending         = nullptr;
        [[= $::Syscall{"wl_display_roundtrip"}]]
        RoundtripFunction               roundtrip               = nullptr;
        [[= $::Syscall{"wl_display_flush"}]]
        FlushFunction                   flush                   = nullptr;
        [[= $::Syscall{"wl_display_get_fd"}]]
        GetFileDescriptorFunction       getFileDescriptor       = nullptr;
        [[= $::Syscall{"wl_display_prepare_read"}]]
        PrepareReadFunction             prepareRead             = nullptr;
        [[= $::Syscall{"wl_display_cancel_read"}]]
        CancelReadFunction              cancelRead              = nullptr;
        [[= $::Syscall{"wl_display_read_events"}]]
        ReadEventsFunction              readEvents              = nullptr;
        [[= $::Syscall{"wl_display_get_registry"}]]
        GetRegistryFunction             getRegistry             = nullptr;
        [[= $::Syscall{"wl_proxy_add_listener"}]]
        ProxyAddListenerFunction        proxyAddListener        = nullptr;
        [[= $::Syscall{"wl_proxy_destroy"}]]
        ProxyDestroyFunction            proxyDestroy            = nullptr;
        [[= $::Syscall{"wl_proxy_marshal_flags"}]]
        ProxyMarshalFlagsFunction       proxyMarshalFlags       = nullptr;
        [[= $::Syscall{"wl_proxy_get_version"}]]
        ProxyGetVersionFunction         proxyGetVersion         = nullptr;
        [[= $::Syscall{"wl_proxy_set_user_data"}]]
        ProxySetUserDataFunction        proxySetUserData        = nullptr;
        [[= $::Syscall{"wl_proxy_get_user_data"}]]
        ProxyGetUserDataFunction        proxyGetUserData        = nullptr;
        [[= $::Syscall{"wl_display_get_error"}]]
        DisplayGetErrorFunction         displayGetError         = nullptr;
        [[= $::Syscall{"wl_display_get_protocol_error"}]]
        DisplayGetProtocolErrorFunction displayGetProtocolError = nullptr;
        // clang-format on

        /** @brief Return the Wayland transport contract represented by this table. */
        [[nodiscard]] static constexpr WindowSystemCapabilities DeclaredCapabilities() noexcept {
            return {.windowSystem = Platform::WindowSystem::Wayland,
                    .backend = NativeWindowBackend::Wayland,
                    .features = std::to_underlying(WindowSystemFeature::EventQueue) |
                                std::to_underlying(WindowSystemFeature::DisplayFd)};
        }

        /** @brief Return whether the minimum Wayland transport and registry symbols were resolved. */
        [[nodiscard]] constexpr bool Available() const noexcept {
            return connect != nullptr && disconnect != nullptr && dispatch != nullptr && flush != nullptr &&
                   getFileDescriptor != nullptr && getRegistry != nullptr && proxyAddListener != nullptr &&
                   proxyDestroy != nullptr && proxyMarshalFlags != nullptr;
        }
    };

#else

    /** @brief Xlib is unavailable on non-Linux targets. */
    struct X11WindowSystemAPI {};
    /** @brief XCB is unavailable on non-Linux targets. */
    struct XcbWindowSystemAPI {};
    /** @brief Wayland is unavailable on non-Linux targets. */
    struct WaylandWindowSystemAPI {};

#endif

#if defined(PLATFORM_WINDOWS)

    namespace WindowsSystem {

        using TraceHandle = uint64_t;
        using GlobalMemory = void*;
        /** @brief Opaque native type used by the existing Win32 file API table. */
        using NativeOverlapped = ::_OVERLAPPED;
        using NativeLargeInteger = ::_LARGE_INTEGER;
        using DpiAwarenessContext = void*;
        using LParam = intptr_t;

        /** @brief Layout-compatible Win32 thread snapshot entry without exposing the Windows SDK. */
        struct ThreadEntry {
            DWord size = sizeof(ThreadEntry);
            DWord usageCount = 0;
            DWord threadId = 0;
            DWord ownerProcessId = 0;
            Long basePriority = 0;
            Long priorityDelta = 0;
            DWord flags = 0;
        };

        inline constexpr DWord kSnapshotThreads = 0x00000004;

        /** @brief Layout used by Win32 asynchronous file operations without including the Windows SDK. */
        struct Overlapped {
            uintptr_t internal = 0;
            uintptr_t internalHigh = 0;
            union {
                struct {
                    DWord offset;
                    DWord offsetHigh;
                };
                void* pointer;
            };
            Handle event = nullptr;
        };

        /** @brief Fixed prefix of a variable-length Win32 directory notification record. */
        struct FileNotifyInformationHeader {
            DWord nextEntryOffset = 0;
            DWord action = 0;
            DWord fileNameLength = 0;
        };

        /** @brief ABI-compatible Win32 system information used for allocation-granularity queries. */
        struct SystemInfo {
            union {
                DWord oemIdentifier;
                struct {
                    uint16_t processorArchitecture;
                    uint16_t reserved;
                };
            };
            DWord pageSize;
            void* minimumApplicationAddress;
            void* maximumApplicationAddress;
            uintptr_t activeProcessorMask;
            DWord processorCount;
            DWord processorType;
            DWord allocationGranularity;
            uint16_t processorLevel;
            uint16_t processorRevision;
        };

        /**
         * @brief ABI-compatible Win32 storage information used for direct-I/O alignment.
         *
         * All size and offset fields are measured in bytes. For example, a typical 512e disk may report @c
         * logicalBytesPerSector as 512 and the physical-sector fields as 4096; direct-I/O buffers, offsets, and
         * transfer sizes should then respect the effective physical-sector size and any additional alignment
         * offsets reported below.
         */
        struct FileStorageInfo {
            /** Bytes in a logical sector exposed to software; commonly 512 or 4096. */
            uint32_t logicalBytesPerSector;
            /** Smallest physical unit written atomically; for example, 4096 on a 512e disk. */
            uint32_t physicalBytesPerSectorForAtomicity;
            /** Preferred physical transfer unit for best performance; commonly 4096. */
            uint32_t physicalBytesPerSectorForPerformance;
            /** Atomicity unit enforced by the file system; for example, 4096 bytes. */
            uint32_t fileSystemEffectivePhysicalBytesPerSectorForAtomicity;
            /** Win32 storage-property bit flags describing alignment and partitioning support. */
            DWord flags;
            /** Byte adjustment needed to align a file offset to a sector boundary; often 0. */
            DWord byteOffsetForSectorAlignment;
            /** Byte adjustment needed to align storage to the partition boundary; often 0. */
            DWord byteOffsetForPartitionAlignment;
        };

        /** @brief ABI-compatible Win32 monitor information record. */
        struct MonitorInfo {
            DWord size = sizeof(MonitorInfo);
            DisplayRect monitor{};
            DisplayRect work{};
            DWord flags = 0;
        };

        /** @brief ABI-compatible end-of-file information passed to Win32 file-information APIs. */
        struct FileEndOfFileInformation {
            LargeInteger endOfFile{};
        };

        /** @brief ABI-compatible representation of Win32 @c PROCESSOR_NUMBER. */
        struct ProcessorNumber {
            uint16_t group = 0;
            uint8_t number = 0;
            uint8_t reserved = 0;
        };

        /** @brief Win32 constants exposed without including the Windows SDK headers. */
        inline namespace Constant {

            /** @brief Boolean constants shared by Win32 APIs using the @c BOOL ABI type. */
            inline namespace Common {

                // clang-format off
                [[= PAL::$::Macro{"FALSE"}]]
                inline constexpr Bool kFalse = 0;
                [[= PAL::$::Macro{"TRUE"}]]
                inline constexpr Bool kTrue  = 1;
                // clang-format on

            } // namespace Common

            /** @brief Status and formatting flags consumed by @c NativeErrorSystemAPI. */
            inline namespace NativeError {

                // clang-format off
                [[= PAL::$::Macro{"ERROR_SUCCESS"}]]
                inline constexpr DWord kErrorSuccess                     = 0;
                [[= PAL::$::Macro{"FORMAT_MESSAGE_ALLOCATE_BUFFER", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFormatMessageAllocateBuffer      = 0x00000100;
                [[= PAL::$::Macro{"FORMAT_MESSAGE_IGNORE_INSERTS", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFormatMessageIgnoreInserts       = 0x00000200;
                [[= PAL::$::Macro{"FORMAT_MESSAGE_FROM_STRING", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFormatMessageFromString          = 0x00000400;
                [[= PAL::$::Macro{"FORMAT_MESSAGE_FROM_HMODULE", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFormatMessageFromModule          = 0x00000800;
                [[= PAL::$::Macro{"FORMAT_MESSAGE_FROM_SYSTEM", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFormatMessageFromSystem          = 0x00001000;
                [[= PAL::$::Macro{"ERROR_FILE_NOT_FOUND"}]]
                inline constexpr DWord kErrorFileNotFound                = 2;
                [[= PAL::$::Macro{"ERROR_PATH_NOT_FOUND"}]]
                inline constexpr DWord kErrorPathNotFound                = 3;
                [[= PAL::$::Macro{"ERROR_NO_MORE_FILES"}]]
                inline constexpr DWord kErrorNoMoreFiles                 = 18;
                [[= PAL::$::Macro{"ERROR_HANDLE_EOF"}]]
                inline constexpr DWord kErrorHandleEof                   = 38;
                [[= PAL::$::Macro{"ERROR_FILE_EXISTS"}]]
                inline constexpr DWord kErrorFileExists                  = 80;
                [[= PAL::$::Macro{"ERROR_ALREADY_EXISTS"}]]
                inline constexpr DWord kErrorAlreadyExists               = 183;
                inline constexpr int kFileEndOfFileInformation            = 6;
                [[= PAL::$::Macro{"FORMAT_MESSAGE_ARGUMENT_ARRAY", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFormatMessageArgumentArray       = 0x00002000;
                [[= PAL::$::Macro{"FORMAT_MESSAGE_MAX_WIDTH_MASK", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFormatMessageMaximumWidthMask    = 0x000000FF;
                // clang-format on

            } // namespace NativeError

            /** @brief Error codes consumed by @c EnvironmentSystemAPI. */
            inline namespace Environment {

                // clang-format off
                [[= PAL::$::Macro{"ERROR_INSUFFICIENT_BUFFER"}]]
                inline constexpr DWord kErrorInsufficientBuffer          = 122;
                [[= PAL::$::Macro{"ERROR_ENVVAR_NOT_FOUND"}]]
                inline constexpr DWord kErrorEnvironmentVariableNotFound = 203;
                // clang-format on

            } // namespace Environment

            /** @brief Access, sharing, creation, attribute, and handle constants consumed by @c FileSystemAPI. */
            inline namespace File {

                // clang-format off
                [[= PAL::$::Macro{"GENERIC_READ",      PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kGenericRead                      = 0x80000000;
                [[= PAL::$::Macro{"GENERIC_WRITE",     PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kGenericWrite                     = 0x40000000;
                [[= PAL::$::Macro{"GENERIC_EXECUTE",   PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kGenericExecute                   = 0x20000000;
                [[= PAL::$::Macro{"GENERIC_ALL",       PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kGenericAll                       = 0x10000000;
                [[= PAL::$::Macro{"FILE_SHARE_READ",   PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileShareRead                    = 0x00000001;
                [[= PAL::$::Macro{"FILE_SHARE_WRITE",  PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileShareWrite                   = 0x00000002;
                [[= PAL::$::Macro{"FILE_SHARE_DELETE", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileShareDelete                  = 0x00000004;
                [[= PAL::$::Macro{"CREATE_NEW"}]]
                inline constexpr DWord kCreateNew                        = 1;
                [[= PAL::$::Macro{"CREATE_ALWAYS"}]]
                inline constexpr DWord kCreateAlways                     = 2;
                [[= PAL::$::Macro{"OPEN_EXISTING"}]]
                inline constexpr DWord kOpenExisting                     = 3;
                [[= PAL::$::Macro{"OPEN_ALWAYS"}]]
                inline constexpr DWord kOpenAlways                       = 4;
                [[= PAL::$::Macro{"TRUNCATE_EXISTING"}]]
                inline constexpr DWord kTruncateExisting                 = 5;
                [[= PAL::$::Macro{"FILE_ATTRIBUTE_NORMAL", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileAttributeNormal              = 0x00000080;
                [[= PAL::$::Macro{"FILE_FLAG_WRITE_THROUGH", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagWriteThrough             = 0x80000000;
                [[= PAL::$::Macro{"FILE_FLAG_OVERLAPPED", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagOverlapped               = 0x40000000;
                [[= PAL::$::Macro{"FILE_FLAG_NO_BUFFERING", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagNoBuffering              = 0x20000000;
                [[= PAL::$::Macro{"FILE_FLAG_RANDOM_ACCESS", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagRandomAccess             = 0x10000000;
                [[= PAL::$::Macro{"FILE_FLAG_SEQUENTIAL_SCAN", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagSequentialScan           = 0x08000000;
                [[= PAL::$::Macro{"FILE_FLAG_DELETE_ON_CLOSE", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagDeleteOnClose            = 0x04000000;
                [[= PAL::$::Macro{"FILE_FLAG_BACKUP_SEMANTICS", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagBackupSemantics          = 0x02000000;
                [[= PAL::$::Macro{"FILE_LIST_DIRECTORY", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileListDirectory                = 0x00000001;
                [[= PAL::$::Macro{"FILE_STORAGE_INFO", PAL::$::Macro::Type::Mask}]]
                inline constexpr int kFileStorageInformation              = 16;
                [[= PAL::$::Macro{"FILE_FLAG_POSIX_SEMANTICS", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagPosixSemantics           = 0x01000000;
                [[= PAL::$::Macro{"FILE_FLAG_SESSION_AWARE", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagSessionAware             = 0x00800000;
                [[= PAL::$::Macro{"FILE_FLAG_OPEN_REPARSE_POINT", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagOpenReparsePoint         = 0x00200000;
                [[= PAL::$::Macro{"FILE_FLAG_OPEN_NO_RECALL", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagOpenNoRecall             = 0x00100000;
                [[= PAL::$::Macro{"FILE_FLAG_FIRST_PIPE_INSTANCE", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kFileFlagFirstPipeInstance        = 0x00080000;
                [[= PAL::$::Macro{"STD_INPUT_HANDLE"}]]
                inline constexpr DWord kStandardInputHandle              = static_cast<DWord>(-10);
                [[= PAL::$::Macro{"STD_OUTPUT_HANDLE"}]]
                inline constexpr DWord kStandardOutputHandle             = static_cast<DWord>(-11);
                [[= PAL::$::Macro{"STD_ERROR_HANDLE"}]]
                inline constexpr DWord kStandardErrorHandle              = static_cast<DWord>(-12);
                [[= PAL::$::Macro{"LOCKFILE_FAIL_IMMEDIATELY", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kLockFileFailImmediately          = 0x00000001;
                [[= PAL::$::Macro{"LOCKFILE_EXCLUSIVE_LOCK", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kLockFileExclusive                = 0x00000002;
                [[= PAL::$::Macro{"ERROR_LOCK_VIOLATION"}]]
                inline constexpr DWord kErrorLockViolation               = 33;
                // clang-format on

            } // namespace File

            /** @brief Directory-notification masks, actions, statuses, and wait results. */
            inline namespace FileWatch {

                // clang-format off
                [[= PAL::$::Macro{"FILE_NOTIFY_CHANGE_FILE_NAME", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kNotifyFileName       = 0x00000001;
                [[= PAL::$::Macro{"FILE_NOTIFY_CHANGE_DIR_NAME", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kNotifyDirectoryName  = 0x00000002;
                [[= PAL::$::Macro{"FILE_NOTIFY_CHANGE_SIZE", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kNotifySize            = 0x00000008;
                [[= PAL::$::Macro{"FILE_NOTIFY_CHANGE_LAST_WRITE", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kNotifyLastWrite      = 0x00000010;
                [[= PAL::$::Macro{"FILE_NOTIFY_CHANGE_CREATION", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kNotifyCreation       = 0x00000040;
                [[= PAL::$::Macro{"FILE_NOTIFY_CHANGE_SECURITY", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kNotifySecurity       = 0x00000100;
                [[= PAL::$::Macro{"FILE_ACTION_ADDED"}]]
                inline constexpr DWord kActionAdded          = 1;
                [[= PAL::$::Macro{"FILE_ACTION_REMOVED"}]]
                inline constexpr DWord kActionRemoved        = 2;
                [[= PAL::$::Macro{"FILE_ACTION_MODIFIED"}]]
                inline constexpr DWord kActionModified       = 3;
                [[= PAL::$::Macro{"FILE_ACTION_RENAMED_OLD_NAME"}]]
                inline constexpr DWord kActionRenamedOld     = 4;
                [[= PAL::$::Macro{"FILE_ACTION_RENAMED_NEW_NAME"}]]
                inline constexpr DWord kActionRenamedNew     = 5;
                [[= PAL::$::Macro{"ERROR_IO_PENDING"}]]
                inline constexpr DWord kErrorIoPending       = 997;
                [[= PAL::$::Macro{"INFINITE"}]]
                inline constexpr DWord kInfinite             = 0xFFFFFFFFu;
                [[= PAL::$::Macro{"WAIT_OBJECT_0"}]]
                inline constexpr DWord kWaitObject           = 0;
                [[= PAL::$::Macro{"WAIT_TIMEOUT"}]]
                inline constexpr DWord kWaitTimeout          = 258;
                // clang-format on

                /** @brief Return whether @p handle is the Win32 invalid-handle sentinel. */
                [[nodiscard]] inline bool IsInvalidHandle(Handle handle) noexcept {
                    return reinterpret_cast<uintptr_t>(handle) == static_cast<uintptr_t>(-1);
                }

            } // namespace FileWatch

            /** @brief Win32 thread access rights and lookup errors used by thread metadata queries. */
            inline namespace Thread {

                inline constexpr DWord kThreadSetLimitedInformation = 0x00000400;
                inline constexpr DWord kThreadQueryLimitedInformation = 0x00000800;
                inline constexpr DWord kSynchronizeAccess = 0x00100000;
                inline constexpr DWord kErrorAccessDenied = 5;
                inline constexpr DWord kErrorInvalidParameter = 87;

            } // namespace Thread

            /** @brief Monitor selection flags and the Windows reference DPI. */
            inline namespace Display {
                inline constexpr DWord kMonitorDefaultNull = 0;
                inline constexpr DWord kMonitorDefaultPrimary = 1;
                inline constexpr DWord kMonitorDefaultNearest = 2;
                inline constexpr UInt kDefaultDpi = 96;
            } // namespace Display

            /** @brief Event, IOCP, and Windows I/O ring constants. */
            inline namespace AsyncIO {
                inline constexpr DWord kIoCompletionKeyNone = 0;
                inline constexpr DWord kIoRingVersion1 = 1;
                inline constexpr DWord kIoRingVersion3 = 3;
                inline constexpr DWord kIoRingSubmissionQueueSize = 0x0000FFFF;
                inline constexpr DWord kIoRingCompletionQueueSize = 0x0000FFFF;
            } // namespace AsyncIO

            /** @brief File replacement and mapping constants. */
            inline namespace FileMapping {

                // clang-format off
                [[= PAL::$::Macro{"REPLACEFILE_WRITE_THROUGH", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kReplaceWriteThrough = 0x00000001;
                [[= PAL::$::Macro{"MOVEFILE_REPLACE_EXISTING", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kMoveReplaceExisting = 0x00000001;
                [[= PAL::$::Macro{"MOVEFILE_WRITE_THROUGH", PAL::$::Macro::Type::Mask}]]
                inline constexpr DWord kMoveWriteThrough    = 0x00000008;
                [[= PAL::$::Macro{"PAGE_READONLY"}]]
                inline constexpr DWord kPageReadOnly        = 0x00000002;
                [[= PAL::$::Macro{"PAGE_READWRITE"}]]
                inline constexpr DWord kPageReadWrite       = 0x00000004;
                [[= PAL::$::Macro{"PAGE_WRITECOPY"}]]
                inline constexpr DWord kPageWriteCopy       = 0x00000008;
                [[= PAL::$::Macro{"FILE_MAP_COPY"}]]
                inline constexpr DWord kFileMapCopy         = 0x00000001;
                [[= PAL::$::Macro{"FILE_MAP_WRITE"}]]
                inline constexpr DWord kFileMapWrite        = 0x00000002;
                [[= PAL::$::Macro{"FILE_MAP_READ"}]]
                inline constexpr DWord kFileMapRead         = 0x00000004;
                [[= PAL::$::Macro{"DUPLICATE_SAME_ACCESS"}]]
                inline constexpr DWord kDuplicateSameAccess = 0x00000002;
                // clang-format on

            } // namespace FileMapping

            /** @brief Allocation flags consumed by @c GlobalMemorySystemAPI. */
            inline namespace GlobalMemoryBehavior {

                // clang-format off
                [[= PAL::$::Macro{"GMEM_FIXED",    PAL::$::Macro::Type::Mask}]]
                inline constexpr UInt kFixedGlobalMemory          = 0x0000;
                [[= PAL::$::Macro{"GMEM_MOVEABLE", PAL::$::Macro::Type::Mask}]]
                inline constexpr UInt kMovableGlobalMemory        = 0x0002;
                [[= PAL::$::Macro{"GMEM_ZEROINIT", PAL::$::Macro::Type::Mask}]]
                inline constexpr UInt kZeroInitializeGlobalMemory = 0x0040;
                // clang-format on

            } // namespace GlobalMemoryBehavior

            /** @brief Standard clipboard formats consumed by @c ClipboardSystemAPI. */
            inline namespace Clipboard {

                // clang-format off
                [[= PAL::$::Macro{"CF_TEXT"}]]
                inline constexpr UInt kTextClipboardFormat        = 1;
                [[= PAL::$::Macro{"CF_OEMTEXT"}]]
                inline constexpr UInt kOemTextClipboardFormat     = 7;
                [[= PAL::$::Macro{"CF_UNICODETEXT"}]]
                inline constexpr UInt kUnicodeTextClipboardFormat = 13;
                // clang-format on

            } // namespace Clipboard

        } // namespace Constant

        /** @brief Return whether @p result represents a successful Win32 @c HRESULT. */
        [[nodiscard]] constexpr bool Succeeded(HResult result) noexcept {
            return result >= 0;
        }

    } // namespace WindowsSystem

    /** @brief Native system error type depending on the platform. */
    using SystemError = std::conditional_t<Sora::Platform::kIsWindows, WindowsSystem::DWord, int>;

    /** @brief Dynamically resolved socket-runtime, endpoint, option, and readiness operations. */
    struct SocketSystemAPI {
#if defined(PLATFORM_WINDOWS)
        using StartupFunction       = int(__stdcall*)(uint16_t, void*);
        using CleanupFunction       = int(__stdcall*)();
        using SocketFunction        = WindowsSystem::Socket(__stdcall*)(int, int, int);
        using CloseFunction         = int(__stdcall*)(WindowsSystem::Socket);
        using BindFunction          = int(__stdcall*)(WindowsSystem::Socket, const sockaddr*, int);
        using NameFunction          = int(__stdcall*)(WindowsSystem::Socket, sockaddr*, int*);
        using OptionFunction        = int(__stdcall*)(WindowsSystem::Socket, int, int, const char*, int);
        using GetOptionFunction     = int(__stdcall*)(WindowsSystem::Socket, int, int, char*, int*);
        using IoctlFunction         = int(__stdcall*)(WindowsSystem::Socket, long, unsigned long*);
        using PollFunction          = int(__stdcall*)(WindowsSystem::NativePollDescriptor*, unsigned long, int);
        using GetAddressInfoFunction =
            int(__stdcall*)(const char*, const char*, const addrinfo*, addrinfo**);
        using FreeAddressInfoFunction = void(__stdcall*)(addrinfo*);
        using GetNameInfoFunction =
            int(__stdcall*)(const sockaddr*, int, char*, unsigned long, char*, unsigned long, int);
#else
        using StartupFunction       = int(*)(uint16_t, void*);
        using CleanupFunction       = int(*)();
        using SocketFunction        = PosixSystem::Socket(*)(int, int, int);
        using CloseFunction         = int(*)(PosixSystem::Socket);
        using BindFunction          = int(*)(PosixSystem::Socket, const sockaddr*, PosixSystem::SocketLength);
        using NameFunction          = int(*)(PosixSystem::Socket, sockaddr*, PosixSystem::SocketLength*);
        using OptionFunction =
            int(*)(PosixSystem::Socket, int, int, const void*, PosixSystem::SocketLength);
        using GetOptionFunction     = int(*)(PosixSystem::Socket, int, int, void*, PosixSystem::SocketLength*);
        using IoctlFunction         = int(*)(PosixSystem::Socket, unsigned long, void*);
        using PollFunction          = int(*)(PosixSystem::NativePollDescriptor*, PosixSystem::PollCount, int);
        using GetAddressInfoFunction = int(*)(const char*, const char*, const addrinfo*, addrinfo**);
        using FreeAddressInfoFunction = void(*)(addrinfo*);
        using GetNameInfoFunction =
            int(*)(const sockaddr*, PosixSystem::SocketLength, char*, size_t, char*, size_t, int);
#endif

        StartupFunction startup = nullptr;       /**< Initialize the process socket runtime; Windows only. */
        CleanupFunction cleanup = nullptr;       /**< Release the process socket runtime; Windows only. */
        SocketFunction create = nullptr;         /**< Create one native socket. */
        CloseFunction close = nullptr;           /**< Close one native socket. */
        BindFunction bind = nullptr;             /**< Bind one local endpoint. */
        NameFunction getSocketName = nullptr;    /**< Query the local endpoint assigned to a socket. */
        NameFunction getPeerName = nullptr;      /**< Query the connected peer endpoint. */
        OptionFunction setOption = nullptr;      /**< Set one socket option. */
        GetOptionFunction getOption = nullptr;   /**< Query one socket option. */
        IoctlFunction ioctl = nullptr;           /**< Apply a native non-blocking or control operation. */
        PollFunction poll = nullptr;             /**< Wait for readiness on a bounded descriptor array. */
        GetAddressInfoFunction getAddressInfo = nullptr; /**< Resolve a numeric or named endpoint. */
        FreeAddressInfoFunction freeAddressInfo = nullptr; /**< Release an address-resolution list. */
        GetNameInfoFunction getNameInfo = nullptr; /**< Format one native endpoint as host/service text. */

        /** @brief Address families understood by the socket table. */
        enum class Family : int { Unspecified = 0, IPv4 = 2, IPv6 = 23 };

        /** @brief Socket kinds understood by the transport-specific tables. */
        enum class Type : int { Stream = 1, Datagram = 2 };

        /** @brief Native transport protocol identifiers. */
        enum class Protocol : int { Any = 0, Tcp = 6, Udp = 17 };

        /** @brief Shutdown directions accepted by @ref TcpSystemAPI::shutdown. */
        enum class Shutdown : int { Receive = 0, Send = 1, Both = 2 };

        inline static constexpr int kPollReadable = 0x0100;
        inline static constexpr int kPollWritable = 0x0010;
        inline static constexpr int kPollError = 0x0001;
        inline static constexpr int kPollHangup = 0x0002;
        inline static constexpr int kPollInvalid = 0x0004;
        inline static constexpr int kOptionSocket = 0xffff;
        inline static constexpr int kOptionReuseAddress = 0x0004;
        inline static constexpr int kOptionKeepAlive = 0x0008;
        inline static constexpr int kAddressInfoPassive = 0x0001;
        inline static constexpr int kAddressInfoNumericHost = 0x0004;
        inline static constexpr int kAddressInfoNumericService = 0x0008;
    };

    /** @brief Dynamically resolved connection-oriented byte-stream operations. */
    struct TcpSystemAPI {
#if defined(PLATFORM_WINDOWS)
        using ListenFunction = int(__stdcall*)(WindowsSystem::Socket, int);
        using AcceptFunction = WindowsSystem::Socket(__stdcall*)(WindowsSystem::Socket, sockaddr*, int*);
        using ConnectFunction = int(__stdcall*)(WindowsSystem::Socket, const sockaddr*, int);
        using TransferFunction = int(__stdcall*)(WindowsSystem::Socket, char*, int, int);
        using SendFunction = int(__stdcall*)(WindowsSystem::Socket, const char*, int, int);
        using ShutdownFunction = int(__stdcall*)(WindowsSystem::Socket, int);
#else
        using ListenFunction = int(*)(PosixSystem::Socket, int);
        using AcceptFunction =
            PosixSystem::Socket(*)(PosixSystem::Socket, sockaddr*, PosixSystem::SocketLength*);
        using ConnectFunction = int(*)(PosixSystem::Socket, const sockaddr*, PosixSystem::SocketLength);
        using TransferFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, void*, size_t, int);
        using SendFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, const void*, size_t, int);
        using ShutdownFunction = int(*)(PosixSystem::Socket, int);
#endif

        ListenFunction listen = nullptr;       /**< Mark a stream socket as a passive listener. */
        AcceptFunction accept = nullptr;       /**< Accept one pending stream connection. */
        ConnectFunction connect = nullptr;     /**< Establish one stream connection. */
        SendFunction send = nullptr;           /**< Send bytes; partial writes are reported explicitly. */
        TransferFunction receive = nullptr;    /**< Receive bytes; zero denotes an orderly peer close. */
        ShutdownFunction shutdown = nullptr;   /**< Disable one or both directions of a stream. */
    };

    /** @brief Dynamically resolved connectionless datagram operations. */
    struct UdpSystemAPI {
#if defined(PLATFORM_WINDOWS)
        using SendToFunction = int(__stdcall*)(WindowsSystem::Socket, const char*, int, int, const sockaddr*, int);
        using ReceiveFromFunction = int(__stdcall*)(WindowsSystem::Socket, char*, int, int, sockaddr*, int*);
#else
        using SendToFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, const void*, size_t, int,
                                                         const sockaddr*, PosixSystem::SocketLength);
        using ReceiveFromFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, void*, size_t, int,
                                                              sockaddr*, PosixSystem::SocketLength*);
#endif

        SendToFunction sendTo = nullptr;              /**< Send one datagram to an explicit endpoint. */
        ReceiveFromFunction receiveFrom = nullptr;   /**< Receive one datagram and return its source endpoint. */
    };

    /** @brief Capture the calling thread's native error slot without triggering lazy symbol resolution. */
    [[nodiscard]] SystemError CaptureLastSystemError() noexcept;

    /** @brief Dynamically resolved Win32 error-reporting entry points. */
    struct NativeErrorSystemAPI {
        // clang-format off
        using GetLastErrorFunction      = WindowsSystem::DWord(__stdcall*)();
        using SetLastErrorFunction      = void(__stdcall*)(WindowsSystem::DWord);
        using FormatMessageWideFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord, const void*, WindowsSystem::DWord, WindowsSystem::DWord, wchar_t*, WindowsSystem::DWord, std::va_list*);

        [[= $::Syscall{"GetLastError"}]]
        GetLastErrorFunction      getLastError      = nullptr;
        [[= $::Syscall{"SetLastError"}]]
        SetLastErrorFunction      setLastError      = nullptr;
        [[= $::Syscall{"FormatMessageW"}]]
        FormatMessageWideFunction formatMessageWide = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows structured-exception and process-exception-policy operations. */
    struct StructuredExceptionSystemAPI {
        // clang-format off
        using ExceptionFilterFunction                = WindowsSystem::Long(__stdcall*)(_EXCEPTION_POINTERS*);
        using SetUnhandledExceptionFilterFunction    = ExceptionFilterFunction(__stdcall*)(ExceptionFilterFunction);
        using AddVectoredExceptionHandlerFunction    = void*(__stdcall*)(WindowsSystem::UInt, void*);
        using RemoveVectoredExceptionHandlerFunction = WindowsSystem::Bool(__stdcall*)(void*);
        using RaiseExceptionFunction                 = void(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord, const WindowsSystem::DWord*);
        using GetErrorModeFunction                   = WindowsSystem::DWord(__stdcall*)();
        using SetErrorModeFunction                   = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord);
        using WerGetFlagsFunction                    = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord*);
        using WerSetFlagsFunction                    = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord);

        [[= $::Syscall{"SetUnhandledExceptionFilter"}]]
        SetUnhandledExceptionFilterFunction      setUnhandledExceptionFilter    = nullptr;
        [[= $::Syscall{"AddVectoredExceptionHandler"}]]
        AddVectoredExceptionHandlerFunction      addVectoredExceptionHandler    = nullptr;
        [[= $::Syscall{"RemoveVectoredExceptionHandler"}]]
        RemoveVectoredExceptionHandlerFunction   removeVectoredExceptionHandler = nullptr;
        [[= $::Syscall{"RaiseException"}]]
        RaiseExceptionFunction                   raiseException                 = nullptr;
        [[= $::Syscall{"GetErrorMode"}]]
        GetErrorModeFunction                     getErrorMode                   = nullptr;
        [[= $::Syscall{"SetErrorMode"}]]
        SetErrorModeFunction                     setErrorMode                   = nullptr;
        [[= $::Syscall{"WerGetFlags"}]]
        WerGetFlagsFunction                      werGetFlags                    = nullptr;
        [[= $::Syscall{"WerSetFlags"}]]
        WerSetFlagsFunction                      werSetFlags                    = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 local-memory operations. */
    struct LocalMemorySystemAPI {
        // clang-format off
        using LocalFreeFunction = void*(__stdcall*)(void*);

        [[= $::Syscall{"LocalFree"}]]
        LocalFreeFunction localFree = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved operations common to Win32 kernel handles. */
    struct HandleSystemAPI {
        // clang-format off
        using CloseHandleFunction     = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using DuplicateHandleFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle, WindowsSystem::Handle, WindowsSystem::Handle*, WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord);

        [[= $::Syscall{"CloseHandle"}]]
        CloseHandleFunction     closeHandle     = nullptr;
        [[= $::Syscall{"DuplicateHandle"}]]
        DuplicateHandleFunction duplicateHandle = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 event synchronization operations. */
    struct SynchronizationSystemAPI {
        // clang-format off
        using WaitForSingleObjectFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord);

        [[= $::Syscall{"WaitForSingleObject"}]]
        WaitForSingleObjectFunction waitForSingleObject = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 event-object operations. */
    struct EventSystemAPI {
        // clang-format off
        using CreateEventWideFunction = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*, WindowsSystem::Bool, WindowsSystem::Bool, const wchar_t*);
        using SetEventFunction        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using ResetEventFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);

        [[= $::Syscall{"CreateEventW"}]]
        CreateEventWideFunction createEventWide = nullptr;
        [[= $::Syscall{"SetEvent"}]]
        SetEventFunction        setEvent        = nullptr;
        [[= $::Syscall{"ResetEvent"}]]
        ResetEventFunction      resetEvent      = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows I/O completion-port operations. */
    struct IOCPSystemAPI {
        // clang-format off
        using CreateIoCompletionPortFunction      = WindowsSystem::Handle(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle, WindowsSystem::Size, WindowsSystem::DWord);
        using GetQueuedCompletionStatusFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord*, WindowsSystem::Size*, ::_OVERLAPPED**, WindowsSystem::DWord);
        using PostQueuedCompletionStatusFunction  = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, WindowsSystem::Size, ::_OVERLAPPED*);

        [[= $::Syscall{"CreateIoCompletionPort"}]]
        CreateIoCompletionPortFunction     createIoCompletionPort      = nullptr;
        [[= $::Syscall{"GetQueuedCompletionStatus"}]]
        GetQueuedCompletionStatusFunction  getQueuedCompletionStatus   = nullptr;
        [[= $::Syscall{"PostQueuedCompletionStatus"}]]
        PostQueuedCompletionStatusFunction postQueuedCompletionStatus = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows I/O ring operations. */
    struct IORingSystemAPI {
        // clang-format off
        using CreateIoRingFunction         = WindowsSystem::HResult(__stdcall*)(WindowsSystem::UInt, WindowsSystem::UInt, void**);
        using CloseIoRingFunction          = void(__stdcall*)(void*);
        using SubmitIoRingFunction         = WindowsSystem::HResult(__stdcall*)(void*);
        using PopIoRingCompletionFunction = WindowsSystem::HResult(__stdcall*)(void*, void*);

        [[= $::Syscall{"CreateIoRing"}]]
        CreateIoRingFunction         createIoRing         = nullptr;
        [[= $::Syscall{"CloseIoRing"}]]
        CloseIoRingFunction          closeIoRing          = nullptr;
        [[= $::Syscall{"SubmitIoRing"}]]
        SubmitIoRingFunction         submitIoRing         = nullptr;
        [[= $::Syscall{"PopIoRingCompletion"}]]
        PopIoRingCompletionFunction popIoRingCompletion = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 file-backed virtual-memory mapping operations. */
    struct FileMappingSystemAPI {
        // clang-format off
        using CreateFileMappingWideFunction = WindowsSystem::Handle(__stdcall*)(WindowsSystem::Handle, ::_SECURITY_ATTRIBUTES*, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord, const wchar_t*);
        using MapViewOfFileFunction         = void*(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord, size_t);
        using UnmapViewOfFileFunction       = WindowsSystem::Bool(__stdcall*)(const void*);
        using FlushViewOfFileFunction       = WindowsSystem::Bool(__stdcall*)(const void*, size_t);

        [[= $::Syscall{"CreateFileMappingW"}]]
        CreateFileMappingWideFunction createFileMappingWide = nullptr;
        [[= $::Syscall{"MapViewOfFile"}]]
        MapViewOfFileFunction         mapViewOfFile         = nullptr;
        [[= $::Syscall{"UnmapViewOfFile"}]]
        UnmapViewOfFileFunction       unmapViewOfFile       = nullptr;
        [[= $::Syscall{"FlushViewOfFile"}]]
        FlushViewOfFileFunction       flushViewOfFile       = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 system-information queries. */
    struct SystemInformationSystemAPI {
        using GetSystemInfoFunction = void(__stdcall*)(void*);

        [[= $::Syscall{"GetSystemInfo"}]] GetSystemInfoFunction getSystemInfo = nullptr;
    };

    /** @brief Dynamically resolved Win32 environment entry points. */
    struct EnvironmentSystemAPI {
        // clang-format off
        using GetEnvironmentVariableWideFunction = WindowsSystem::DWord(__stdcall*)(const wchar_t*, wchar_t*, WindowsSystem::DWord);
        using SetEnvironmentVariableWideFunction = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*);
        using GetEnvironmentStringsWideFunction  = wchar_t*(__stdcall*)();
        using FreeEnvironmentStringsWideFunction = WindowsSystem::Bool(__stdcall*)(wchar_t*);

        [[= $::Syscall{"GetEnvironmentVariableW"}]]
        GetEnvironmentVariableWideFunction      getEnvironmentVariableWide      = nullptr;
        [[= $::Syscall{"SetEnvironmentVariableW"}]]
        SetEnvironmentVariableWideFunction      setEnvironmentVariableWide      = nullptr;
        [[= $::Syscall{"GetEnvironmentStringsW"}]]
        GetEnvironmentStringsWideFunction       getEnvironmentStringsWide       = nullptr;
        [[= $::Syscall{"FreeEnvironmentStringsW"}]]
        FreeEnvironmentStringsWideFunction      freeEnvironmentStringsWide      = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved module-loader operations with normalized PAL handle semantics. */
    struct ModuleSystemAPI {
        // clang-format off
        using LoadLibraryWideFunction       = WindowsSystem::Handle (*)(const wchar_t*) noexcept;
        using GetModuleHandleWideFunction   = WindowsSystem::Handle (*)(const wchar_t*) noexcept;
        using FindSymbolFunction             = void* (*)(WindowsSystem::Handle, const char*) noexcept;
        using FreeLibraryFunction            = WindowsSystem::Bool (*)(WindowsSystem::Handle) noexcept;

        [[= $::Syscall{"LoadLibraryW"}]]
        LoadLibraryWideFunction     loadLibraryWide     = nullptr;
        [[= $::Syscall{"GetModuleHandleW"}]]
        GetModuleHandleWideFunction getModuleHandleWide = nullptr;
        [[= $::Syscall{"GetProcAddress"}]]
        FindSymbolFunction           findSymbol          = nullptr;
        [[= $::Syscall{"FreeLibrary"}]]
        FreeLibraryFunction          freeLibrary         = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved and normalized Win32 current-process introspection operations. */
    struct ProcessSystemAPI {
        // clang-format off
        using GetCurrentProcessIdFunction           = WindowsSystem::DWord(__stdcall*)();
        using GetCurrentProcessFunction             = WindowsSystem::Handle(__stdcall*)();
        using CreateProcessSnapshotFunction         = WindowsSystem::Handle(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord);
        using ReadProcessEntryFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::tagPROCESSENTRY32W*);
        using ReadThreadEntryFunction               = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::ThreadEntry*);
        using QueryFullProcessImageNameWideFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, wchar_t*, WindowsSystem::DWord*);
        using GetCommandLineWideFunction            = wchar_t*(__stdcall*)();
        using CommandLineToArgvWideFunction         = wchar_t**(__stdcall*)(const wchar_t*, int*);
        using GetProcessTimesFunction               = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_FILETIME*, ::_FILETIME*, ::_FILETIME*, ::_FILETIME*);
        using GetProcessMemoryInfoFunction          = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_PROCESS_MEMORY_COUNTERS*, WindowsSystem::DWord);
        using QueryParentProcessIdFunction          = bool (*)(WindowsSystem::DWord*) noexcept;
        using CaptureUsageFunction                  = bool (*)(ProcessUsageCounters*) noexcept;

        [[= $::Syscall{"GetCurrentProcessId"}]]
        GetCurrentProcessIdFunction           getCurrentProcessId           = nullptr;
        [[= $::Syscall{"GetCurrentProcess"}]]
        GetCurrentProcessFunction             getCurrentProcess             = nullptr;
        [[= $::Syscall{"CreateToolhelp32Snapshot"}]]
        CreateProcessSnapshotFunction         createProcessSnapshot         = nullptr;
        [[= $::Syscall{"Process32FirstW"}]]
        ReadProcessEntryFunction              firstProcess                  = nullptr;
        [[= $::Syscall{"Process32NextW"}]]
        ReadProcessEntryFunction              nextProcess                   = nullptr;
        [[= $::Syscall{"Thread32First"}]]
        ReadThreadEntryFunction               firstThread                   = nullptr;
        [[= $::Syscall{"Thread32Next"}]]
        ReadThreadEntryFunction               nextThread                    = nullptr;
        [[= $::Syscall{"QueryFullProcessImageNameW"}]]
        QueryFullProcessImageNameWideFunction queryFullProcessImageNameWide = nullptr;
        [[= $::Syscall{"GetCommandLineW"}]]
        GetCommandLineWideFunction            getCommandLineWide            = nullptr;
        [[= $::Syscall{"CommandLineToArgvW"}]]
        CommandLineToArgvWideFunction         commandLineToArgvWide         = nullptr;
        [[= $::Syscall{"GetProcessTimes"}]]
        GetProcessTimesFunction               getProcessTimes               = nullptr;
        [[= $::Syscall{"K32GetProcessMemoryInfo"}]]
        GetProcessMemoryInfoFunction          getProcessMemoryInfo          = nullptr;
        QueryParentProcessIdFunction          queryParentProcessId          = nullptr;
        CaptureUsageFunction                  captureUsage                  = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 current-thread entry points. */
    struct ThreadSystemAPI {
        // clang-format off
        using GetCurrentThreadIdFunction          = WindowsSystem::DWord(__stdcall*)();
        using GetCurrentThreadFunction            = WindowsSystem::Handle(__stdcall*)();
        using OpenThreadFunction                  = WindowsSystem::Handle(__stdcall*)(WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord);
        using SetThreadDescriptionFunction        = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, const wchar_t*);
        using GetThreadDescriptionFunction        = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, wchar_t**);
        using GetCurrentProcessorNumberExFunction = void(__stdcall*)(WindowsSystem::ProcessorNumber*);
        using GetCurrentThreadStackLimitsFunction = void(__stdcall*)(uintptr_t*, uintptr_t*);

        [[= $::Syscall{"GetCurrentThreadId"}]]
        GetCurrentThreadIdFunction          getCurrentThreadId          = nullptr;
        [[= $::Syscall{"GetCurrentThread"}]]
        GetCurrentThreadFunction            getCurrentThread            = nullptr;
        [[= $::Syscall{"OpenThread"}]]
        OpenThreadFunction                  openThread                  = nullptr;
        [[= $::Syscall{"SetThreadDescription"}]]
        SetThreadDescriptionFunction        setThreadDescription        = nullptr;
        [[= $::Syscall{"GetThreadDescription"}]]
        GetThreadDescriptionFunction        getThreadDescription        = nullptr;
        [[= $::Syscall{"GetCurrentProcessorNumberEx"}]]
        GetCurrentProcessorNumberExFunction getCurrentProcessorNumberEx = nullptr;
        [[= $::Syscall{"GetCurrentThreadStackLimits"}]]
        GetCurrentThreadStackLimitsFunction getCurrentThreadStackLimits = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 movable-global-memory entry points. */
    struct GlobalMemorySystemAPI {
        // clang-format off
        using GlobalAllocateFunction = WindowsSystem::GlobalMemory(__stdcall*)(WindowsSystem::UInt, WindowsSystem::Size);
        using GlobalFreeFunction     = WindowsSystem::GlobalMemory(__stdcall*)(WindowsSystem::GlobalMemory);
        using GlobalLockFunction     = void*(__stdcall*)(WindowsSystem::GlobalMemory);
        using GlobalUnlockFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::GlobalMemory);
        using GlobalSizeFunction     = WindowsSystem::Size(__stdcall*)(WindowsSystem::GlobalMemory);

        [[= $::Syscall{"GlobalAlloc"}]]
        GlobalAllocateFunction globalAllocate = nullptr;
        [[= $::Syscall{"GlobalFree"}]]
        GlobalFreeFunction     globalFree     = nullptr;
        [[= $::Syscall{"GlobalLock"}]]
        GlobalLockFunction     globalLock     = nullptr;
        [[= $::Syscall{"GlobalUnlock"}]]
        GlobalUnlockFunction   globalUnlock   = nullptr;
        [[= $::Syscall{"GlobalSize"}]]
        GlobalSizeFunction     globalSize     = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 clipboard entry points. */
    struct ClipboardSystemAPI {
        // clang-format off
        using OpenClipboardFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using CloseClipboardFunction             = WindowsSystem::Bool(__stdcall*)();
        using EmptyClipboardFunction             = WindowsSystem::Bool(__stdcall*)();
        using GetClipboardDataFunction           = WindowsSystem::Handle(__stdcall*)(WindowsSystem::UInt);
        using SetClipboardDataFunction           = WindowsSystem::Handle(__stdcall*)(WindowsSystem::UInt, WindowsSystem::Handle);
        using IsClipboardFormatAvailableFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::UInt);

        [[= $::Syscall{"OpenClipboard"}]]
        OpenClipboardFunction              openClipboard              = nullptr;
        [[= $::Syscall{"CloseClipboard"}]]
        CloseClipboardFunction             closeClipboard             = nullptr;
        [[= $::Syscall{"EmptyClipboard"}]]
        EmptyClipboardFunction             emptyClipboard             = nullptr;
        [[= $::Syscall{"GetClipboardData"}]]
        GetClipboardDataFunction           getClipboardData           = nullptr;
        [[= $::Syscall{"SetClipboardData"}]]
        SetClipboardDataFunction           setClipboardData           = nullptr;
        [[= $::Syscall{"IsClipboardFormatAvailable"}]]
        IsClipboardFormatAvailableFunction isClipboardFormatAvailable = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved display, monitor, geometry, and Windows DPI operations. */
    struct DisplaySystemAPI {
        // clang-format off
        using EnumDisplayMonitorsCallback                   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::MonitorHandle, WindowsSystem::DeviceContextHandle, const DisplayRect*, WindowsSystem::LParam);
        using EnumDisplayMonitorsFunction                   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::DeviceContextHandle, const DisplayRect*, EnumDisplayMonitorsCallback, WindowsSystem::LParam);
        using GetMonitorInfoWideFunction                    = WindowsSystem::Bool(__stdcall*)(WindowsSystem::MonitorHandle, WindowsSystem::MonitorInfo*);
        using MonitorFromPointFunction                      = WindowsSystem::MonitorHandle(__stdcall*)(DisplayPoint, WindowsSystem::DWord);
        using MonitorFromWindowFunction                     = WindowsSystem::MonitorHandle(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::DWord);
        using GetWindowRectFunction                         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, DisplayRect*);
        using GetClientRectFunction                         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, DisplayRect*);
        using TranslatePointFunction                        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, DisplayPoint*);
        using GetCursorPositionFunction                     = WindowsSystem::Bool(__stdcall*)(DisplayPoint*);
        using GetSystemMetricsFunction                      = int(__stdcall*)(int);
        using GetSystemMetricsForDpiFunction                = int(__stdcall*)(int, WindowsSystem::UInt);
        using GetDpiForWindowFunction                       = WindowsSystem::UInt(__stdcall*)(WindowsSystem::WindowHandle);
        using GetDpiForSystemFunction                       = WindowsSystem::UInt(__stdcall*)();
        using GetSystemDpiForProcessFunction                = WindowsSystem::UInt(__stdcall*)(WindowsSystem::Handle);
        using GetWindowDpiAwarenessContextFunction          = WindowsSystem::DpiAwarenessContext(__stdcall*)(WindowsSystem::WindowHandle);
        using GetDpiForMonitorFunction                      = WindowsSystem::HResult(__stdcall*)(WindowsSystem::MonitorHandle, int, WindowsSystem::UInt*, WindowsSystem::UInt*);
        using SetProcessDpiAwarenessContextFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::DpiAwarenessContext);
        using SetThreadDpiAwarenessContextFunction          = WindowsSystem::DpiAwarenessContext(__stdcall*)(WindowsSystem::DpiAwarenessContext);
        using GetThreadDpiAwarenessContextFunction          = WindowsSystem::DpiAwarenessContext(__stdcall*)();
        using GetAwarenessFromDpiAwarenessContextFunction   = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DpiAwarenessContext, int*);
        using AdjustWindowRectExForDpiFunction              = WindowsSystem::Bool(__stdcall*)(DisplayRect*, WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord, WindowsSystem::UInt);
        using SetProcessDpiAwarenessFunction                = WindowsSystem::HResult(__stdcall*)(int);
        using GetProcessDpiAwarenessFunction                = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, int*);

        [[= $::Syscall{"EnumDisplayMonitors"}]]
        EnumDisplayMonitorsFunction                 enumDisplayMonitors                 = nullptr;
        [[= $::Syscall{"GetMonitorInfoW"}]]
        GetMonitorInfoWideFunction                  getMonitorInfoWide                  = nullptr;
        [[= $::Syscall{"MonitorFromPoint"}]]
        MonitorFromPointFunction                    monitorFromPoint                    = nullptr;
        [[= $::Syscall{"MonitorFromWindow"}]]
        MonitorFromWindowFunction                   monitorFromWindow                   = nullptr;
        [[= $::Syscall{"GetWindowRect"}]]
        GetWindowRectFunction                       getWindowRect                       = nullptr;
        [[= $::Syscall{"GetClientRect"}]]
        GetClientRectFunction                       getClientRect                       = nullptr;
        [[= $::Syscall{"ClientToScreen"}]]
        TranslatePointFunction                      clientToScreen                      = nullptr;
        [[= $::Syscall{"ScreenToClient"}]]
        TranslatePointFunction                      screenToClient                      = nullptr;
        [[= $::Syscall{"GetCursorPos"}]]
        GetCursorPositionFunction                   getCursorPosition                   = nullptr;
        [[= $::Syscall{"GetSystemMetrics"}]]
        GetSystemMetricsFunction                    getSystemMetrics                    = nullptr;
        [[= $::Syscall{"GetSystemMetricsForDpi"}]]
        GetSystemMetricsForDpiFunction              getSystemMetricsForDpi              = nullptr;
        [[= $::Syscall{"GetDpiForWindow"}]]
        GetDpiForWindowFunction                     getDpiForWindow                     = nullptr;
        [[= $::Syscall{"GetDpiForSystem"}]]
        GetDpiForSystemFunction                     getDpiForSystem                     = nullptr;
        [[= $::Syscall{"GetSystemDpiForProcess"}]]
        GetSystemDpiForProcessFunction              getSystemDpiForProcess              = nullptr;
        [[= $::Syscall{"GetWindowDpiAwarenessContext"}]]
        GetWindowDpiAwarenessContextFunction        getWindowDpiAwarenessContext        = nullptr;
        [[= $::Syscall{"GetDpiForMonitor"}]]
        GetDpiForMonitorFunction                    getDpiForMonitor                    = nullptr;
        [[= $::Syscall{"SetProcessDpiAwarenessContext"}]]
        SetProcessDpiAwarenessContextFunction       setProcessDpiAwarenessContext       = nullptr;
        [[= $::Syscall{"SetThreadDpiAwarenessContext"}]]
        SetThreadDpiAwarenessContextFunction        setThreadDpiAwarenessContext        = nullptr;
        [[= $::Syscall{"GetThreadDpiAwarenessContext"}]]
        GetThreadDpiAwarenessContextFunction        getThreadDpiAwarenessContext        = nullptr;
        [[= $::Syscall{"GetAwarenessFromDpiAwarenessContext"}]]
        GetAwarenessFromDpiAwarenessContextFunction getAwarenessFromDpiAwarenessContext = nullptr;
        [[= $::Syscall{"AdjustWindowRectExForDpi"}]]
        AdjustWindowRectExForDpiFunction            adjustWindowRectExForDpi            = nullptr;
        [[= $::Syscall{"SetProcessDpiAwareness"}]]
        SetProcessDpiAwarenessFunction              setProcessDpiAwareness              = nullptr;
        [[= $::Syscall{"GetProcessDpiAwareness"}]]
        GetProcessDpiAwarenessFunction              getProcessDpiAwareness              = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 file, mapping, replacement, and directory-monitoring entry points. */
    struct FileSystemAPI {
        // clang-format off
        using GetStandardHandleFunction     = WindowsSystem::Handle(__stdcall*)(WindowsSystem::DWord);
        using CreateFileWideFunction        = WindowsSystem::Handle(__stdcall*)(const wchar_t*, WindowsSystem::DWord, WindowsSystem::DWord, ::_SECURITY_ATTRIBUTES*, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::Handle);
        using WriteFileFunction             = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, const void*, WindowsSystem::DWord, WindowsSystem::DWord*, ::_OVERLAPPED*);
        using FlushFileBuffersFunction      = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using ReadFileFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, void*, WindowsSystem::DWord, WindowsSystem::DWord*, ::_OVERLAPPED*);
        using GetFileSizeFunction           = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_LARGE_INTEGER*);
        using SetFileInformationFunction    = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, int, void*, WindowsSystem::DWord);
        using GetFileInformationFunction    = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, int, void*, WindowsSystem::DWord);
        using ReplaceFileWideFunction       = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*, WindowsSystem::DWord, void*, void*);
        using MoveFileWideFunction          = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*, WindowsSystem::DWord);
        using DeleteFileWideFunction        = WindowsSystem::Bool(__stdcall*)(const wchar_t*);
        using GetOverlappedResultFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_OVERLAPPED*, WindowsSystem::DWord*, WindowsSystem::Bool);
        using CancelIoFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_OVERLAPPED*);
        using LockFileFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord, ::_OVERLAPPED*);
        using DirectoryCompletionFunction   = void(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, ::_OVERLAPPED*);
        using ReadDirectoryChangesFunction  = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, void*, WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord, WindowsSystem::DWord*, ::_OVERLAPPED*, DirectoryCompletionFunction);

        [[= $::Syscall{"GetStdHandle"}]]
        GetStandardHandleFunction     getStandardHandle     = nullptr;
        [[= $::Syscall{"CreateFileW"}]]
        CreateFileWideFunction        createFileWide        = nullptr;
        [[= $::Syscall{"WriteFile"}]]
        WriteFileFunction             writeFile             = nullptr;
        [[= $::Syscall{"FlushFileBuffers"}]]
        FlushFileBuffersFunction      flushFileBuffers      = nullptr;
        [[= $::Syscall{"ReadFile"}]]
        ReadFileFunction              readFile              = nullptr;
        [[= $::Syscall{"GetFileSizeEx"}]]
        GetFileSizeFunction           getFileSize           = nullptr;
        [[= $::Syscall{"SetFileInformationByHandle"}]]
        SetFileInformationFunction    setFileInformation    = nullptr;
        [[= $::Syscall{"GetFileInformationByHandleEx"}]]
        GetFileInformationFunction    getFileInformation    = nullptr;
        [[= $::Syscall{"ReplaceFileW"}]]
        ReplaceFileWideFunction       replaceFileWide       = nullptr;
        [[= $::Syscall{"MoveFileExW"}]]
        MoveFileWideFunction          moveFileWide          = nullptr;
        [[= $::Syscall{"DeleteFileW"}]]
        DeleteFileWideFunction        deleteFileWide        = nullptr;
        [[= $::Syscall{"GetOverlappedResult"}]]
        GetOverlappedResultFunction   getOverlappedResult   = nullptr;
        [[= $::Syscall{"CancelIoEx"}]]
        CancelIoFunction              cancelIo              = nullptr;
        [[= $::Syscall{"LockFileEx"}]]
        LockFileFunction              lockFile              = nullptr;
        [[= $::Syscall{"ReadDirectoryChangesW"}]]
        ReadDirectoryChangesFunction  readDirectoryChanges  = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows DbgHelp entry points shared by diagnostic services. */
    struct DbgHelpSystemAPI {
        // clang-format off
        using SymSetOptionsFunction           = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord);
        using SymInitializeFunction           = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, const char*, WindowsSystem::Bool);
        using SymFromAddressFunction          = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord64, WindowsSystem::DWord64*, ::_SYMBOL_INFO*);
        using SymGetLineFromAddressFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord64, WindowsSystem::DWord*, ::_IMAGEHLP_LINE64*);
        using SymGetModuleInfoFunction        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord64, ::_IMAGEHLP_MODULE64*);
        using UndecorateSymbolNameFunction    = WindowsSystem::DWord(__stdcall*)(const char*, char*, WindowsSystem::DWord, WindowsSystem::DWord);

        [[= $::Syscall{"SymSetOptions"}]]
        SymSetOptionsFunction         symSetOptions         = nullptr;
        [[= $::Syscall{"SymInitialize"}]]
        SymInitializeFunction         symInitialize         = nullptr;
        [[= $::Syscall{"SymFromAddr"}]]
        SymFromAddressFunction        symFromAddress        = nullptr;
        [[= $::Syscall{"SymGetLineFromAddr64"}]]
        SymGetLineFromAddressFunction symGetLineFromAddress = nullptr;
        [[= $::Syscall{"SymGetModuleInfo64"}]]
        SymGetModuleInfoFunction      symGetModuleInfo      = nullptr;
        [[= $::Syscall{"UnDecorateSymbolName"}]]
        UndecorateSymbolNameFunction  undecorateSymbolName  = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows current-thread stack-capture entry point. */
    struct StackTraceSystemAPI {
        // clang-format off
        using CaptureStackBackTraceFunction = uint16_t(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, void**, WindowsSystem::DWord*);

        [[= $::Syscall{"RtlCaptureStackBackTrace"}]]
        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        // clang-format on
    };

#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

    namespace PosixSystem {

        /** @brief Native POSIX socket descriptor. */
        using Socket = int;

        /** @brief ABI-sized POSIX thread identifier used without including @c pthread.h. */
        using ThreadId = std::conditional_t<Platform::kIsLinux, std::uintptr_t, void*>;

        /** @brief Opaque POSIX thread-attribute object used by the dynamically loaded table. */
        struct ThreadAttributes;

        /** @brief POSIX file offset ABI type without including @c sys/types.h. */
        using FileOffset = std::int64_t;

        /** @brief POSIX creation-mode ABI type without including @c sys/types.h. */
        using FileMode = std::conditional_t<Platform::kIsLinux, uint32_t, uint16_t>;

        /** @brief POSIX poll descriptor-count ABI type without including @c poll.h. */
        using PollCount = size_t;
        using SocketLength = uint32_t;
        using ByteCount = std::ptrdiff_t;

        /** @brief Opaque Darwin process-accounting buffer pointer used by @c proc_pid_rusage. */
        using ResourceUsageInfo = void*;

        /** @brief Opaque native poll descriptor used by the POSIX file API table. */
        /** @brief Opaque native kqueue event used by the Darwin file API table. */
        using NativeKernelEvent = ::kevent;

        /** @brief Opaque native timeout record used by the Darwin file API table. */
        using NativeTimeSpec = ::timespec;

        /** @brief ABI-compatible fixed prefix of a Linux inotify record. */
        struct InotifyEventHeader {
            int32_t watchDescriptor = -1;
            uint32_t mask = 0;
            uint32_t cookie = 0;
            uint32_t nameLength = 0;
        };

        /** @brief ABI-compatible poll descriptor used by the Linux watcher backend. */
        struct PollDescriptor {
            int descriptor = -1;
            int16_t events = 0;
            int16_t revents = 0;
        };
        using NativePollDescriptor = PollDescriptor;

        /** @brief ABI-compatible kqueue event record used by the Darwin watcher backend. */
        struct KernelEvent {
            uintptr_t identifier = 0;
            int16_t filter = 0;
            uint16_t flags = 0;
            uint32_t filterFlags = 0;
            int64_t data = 0;
            void* userData = nullptr;
        };

        /** @brief ABI-compatible timeout record used by the Darwin watcher backend. */
        struct TimeSpec {
            int64_t seconds = 0;
            int64_t nanoseconds = 0;
        };

        /** @brief Platform-specific watcher flags and event masks. */
        inline namespace FileWatch {

#    if defined(PLATFORM_LINUX)
            // clang-format off
            inline constexpr int      kOpenNonBlocking    = 0x00000800;
            inline constexpr int      kOpenCloseOnExec    = 0x00080000;
            inline constexpr uint32_t kEventModify        = 0x00000002;
            inline constexpr uint32_t kEventAttrib        = 0x00000004;
            inline constexpr uint32_t kEventCloseWrite    = 0x00000008;
            inline constexpr uint32_t kEventMovedFrom     = 0x00000040;
            inline constexpr uint32_t kEventMovedTo       = 0x00000080;
            inline constexpr uint32_t kEventCreate        = 0x00000100;
            inline constexpr uint32_t kEventDelete        = 0x00000200;
            inline constexpr uint32_t kEventDeleteSelf    = 0x00000400;
            inline constexpr uint32_t kEventMoveSelf      = 0x00000800;
            inline constexpr uint32_t kEventQueueOverflow = 0x00004000;
            inline constexpr uint32_t kEventIgnored       = 0x00008000;
            inline constexpr uint32_t kEventIsDirectory   = 0x40000000;
            inline constexpr int16_t  kPollInput          = 0x0001;
            inline constexpr int16_t  kPollError          = 0x0008;
            inline constexpr int16_t  kPollHangup         = 0x0010;
            inline constexpr int16_t  kPollInvalid        = 0x0020;
            // clang-format on
#    elif defined(PLATFORM_MACOS)
            // clang-format off
            inline constexpr int      kOpenEventOnly      = 0x00008000;
            inline constexpr int      kOpenCloseOnExec    = 0x01000000;
            inline constexpr int16_t  kFilterVnode        = -4;
            inline constexpr uint16_t kEventAdd           = 0x0001;
            inline constexpr uint16_t kEventClear         = 0x0020;
            inline constexpr uint32_t kNoteWrite          = 0x00000002;
            inline constexpr uint32_t kNoteDelete         = 0x00000001;
            inline constexpr uint32_t kNoteRename         = 0x00000020;
            inline constexpr uint32_t kNoteExtend         = 0x00000004;
            inline constexpr uint32_t kNoteAttribute      = 0x00000008;
            // clang-format on
#    endif

        } // namespace FileWatch

        /** @brief POSIX file-opening and mapping constants used by PAL implementations. */
        inline namespace File {

#    if defined(PLATFORM_LINUX)
            // clang-format off
            inline constexpr int kOpenReadOnly           = 0x00000000;
            inline constexpr int kOpenWriteOnly          = 0x00000001;
            inline constexpr int kOpenReadWrite          = 0x00000002;
            inline constexpr int kOpenCreate             = 0x00000040;
            inline constexpr int kOpenExclusive          = 0x00000080;
            inline constexpr int kOpenTruncate           = 0x00000200;
            inline constexpr int kOpenDirectory          = 0x00010000;
            inline constexpr int kOpenDirect             = 0x00004000;
            inline constexpr int kOpenSync               = 0x00101000;
            inline constexpr int kPageSizeConfiguration  = 30;
            inline constexpr int kFileAdviceSequential   = 2;
            inline constexpr int kFileAdviceRandom       = 1;
            inline constexpr int kLockExclusive          = 2;
            inline constexpr int kLockNonBlocking        = 4;
            // clang-format on
#    elif defined(PLATFORM_MACOS)
            // clang-format off
            inline constexpr int kOpenReadOnly           = 0x00000000;
            inline constexpr int kOpenWriteOnly          = 0x00000001;
            inline constexpr int kOpenReadWrite          = 0x00000002;
            inline constexpr int kOpenCreate             = 0x00000200;
            inline constexpr int kOpenExclusive          = 0x00000800;
            inline constexpr int kOpenTruncate           = 0x00000400;
            inline constexpr int kOpenDirectory          = 0x00100000;
            inline constexpr int kOpenSync               = 0x00000080;
            inline constexpr int kNoCacheControl         = 48;
            inline constexpr int kFullSyncControl        = 51;
            inline constexpr int kPageSizeConfiguration  = 29;
            inline constexpr int kLockExclusive          = 2;
            inline constexpr int kLockNonBlocking        = 4;
            // clang-format on
#    endif

        } // namespace File

        /** @brief POSIX virtual-memory protection, mapping, and synchronization constants. */
        inline namespace Mapping {

            // clang-format off
            inline constexpr int kProtectionRead   = 0x1;
            inline constexpr int kProtectionWrite  = 0x2;
            inline constexpr int kMapShared        = 0x1;
            inline constexpr int kMapPrivate       = 0x2;
            inline constexpr int kSynchronize      = 0x4;
            inline constexpr int kAdviceNormal     = 0;
            inline constexpr int kAdviceRandom     = 1;
            inline constexpr int kAdviceSequential = 2;
            inline constexpr int kAdviceWillNeed   = 3;
            inline constexpr int kAdviceDontNeed   = 4;
            // clang-format on

            /** @brief Return whether @p address is the POSIX failed-mapping sentinel. */
            [[nodiscard]] inline bool IsMapFailed(void* address) noexcept {
                return reinterpret_cast<uintptr_t>(address) == static_cast<uintptr_t>(-1);
            }

        } // namespace Mapping

        /** @brief POSIX dynamic-loader binding and visibility flags. */
        inline namespace Module {

            inline constexpr int kDynamicLazy   = 0x00000001;
            inline constexpr int kDynamicNow    = 0x00000002;
            inline constexpr int kDynamicLocal  = 0x00000004;
            inline constexpr int kDynamicGlobal = kIsLinux ? 0x00000100 : 0x00000008;

        } // namespace Module

    } // namespace PosixSystem

    /** @brief Dynamically resolved POSIX socket-runtime, endpoint, option, and readiness operations. */
    struct SocketSystemAPI {
        using StartupFunction = int(*)(uint16_t, void*);
        using CleanupFunction = int(*)();
        using SocketFunction = PosixSystem::Socket(*)(int, int, int);
        using CloseFunction = int(*)(PosixSystem::Socket);
        using BindFunction = int(*)(PosixSystem::Socket, const sockaddr*, PosixSystem::SocketLength);
        using NameFunction = int(*)(PosixSystem::Socket, sockaddr*, PosixSystem::SocketLength*);
        using OptionFunction = int(*)(PosixSystem::Socket, int, int, const void*, PosixSystem::SocketLength);
        using GetOptionFunction = int(*)(PosixSystem::Socket, int, int, void*, PosixSystem::SocketLength*);
        using IoctlFunction = int(*)(PosixSystem::Socket, unsigned long, void*);
        using PollFunction = int(*)(PosixSystem::NativePollDescriptor*, PosixSystem::PollCount, int);
        using GetAddressInfoFunction = int(*)(const char*, const char*, const addrinfo*, addrinfo**);
        using FreeAddressInfoFunction = void(*)(addrinfo*);
        using GetNameInfoFunction =
            int(*)(const sockaddr*, PosixSystem::SocketLength, char*, size_t, char*, size_t, int);

        StartupFunction startup = nullptr; /**< POSIX requires no process socket-runtime initialization. */
        CleanupFunction cleanup = nullptr; /**< POSIX requires no process socket-runtime cleanup. */
        SocketFunction create = nullptr;
        CloseFunction close = nullptr;
        BindFunction bind = nullptr;
        NameFunction getSocketName = nullptr;
        NameFunction getPeerName = nullptr;
        OptionFunction setOption = nullptr;
        GetOptionFunction getOption = nullptr;
        IoctlFunction ioctl = nullptr;
        PollFunction poll = nullptr;
        GetAddressInfoFunction getAddressInfo = nullptr;
        FreeAddressInfoFunction freeAddressInfo = nullptr;
        GetNameInfoFunction getNameInfo = nullptr;

        enum class Family : int {
            Unspecified = 0,
            IPv4 = 2,
#    if defined(PLATFORM_LINUX)
            IPv6 = 10,
#    else
            IPv6 = 30,
#    endif
        };
        enum class Type : int { Stream = 1, Datagram = 2 };
        enum class Protocol : int { Any = 0, Tcp = 6, Udp = 17 };
        enum class Shutdown : int { Receive = 0, Send = 1, Both = 2 };

        inline static constexpr int kPollReadable = 0x0001;
        inline static constexpr int kPollWritable = 0x0004;
        inline static constexpr int kPollError = 0x0008;
        inline static constexpr int kPollHangup = 0x0010;
        inline static constexpr int kPollInvalid = 0x0020;
#    if defined(PLATFORM_LINUX)
        inline static constexpr int kOptionSocket = 1;
        inline static constexpr int kOptionReuseAddress = 2;
        inline static constexpr int kOptionKeepAlive = 9;
        inline static constexpr int kAddressInfoNumericService = 0x0400;
#    else
        inline static constexpr int kOptionSocket = 0xffff;
        inline static constexpr int kOptionReuseAddress = 0x0004;
        inline static constexpr int kOptionKeepAlive = 0x0008;
        inline static constexpr int kAddressInfoNumericService = 0x1000;
#    endif
        inline static constexpr int kAddressInfoPassive = 0x0001;
        inline static constexpr int kAddressInfoNumericHost = 0x0004;
    };

    /** @brief Dynamically resolved POSIX TCP byte-stream operations. */
    struct TcpSystemAPI {
        using ListenFunction = int(*)(PosixSystem::Socket, int);
        using AcceptFunction =
            PosixSystem::Socket(*)(PosixSystem::Socket, sockaddr*, PosixSystem::SocketLength*);
        using ConnectFunction = int(*)(PosixSystem::Socket, const sockaddr*, PosixSystem::SocketLength);
        using TransferFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, void*, size_t, int);
        using SendFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, const void*, size_t, int);
        using ShutdownFunction = int(*)(PosixSystem::Socket, int);

        ListenFunction listen = nullptr;
        AcceptFunction accept = nullptr;
        ConnectFunction connect = nullptr;
        SendFunction send = nullptr;
        TransferFunction receive = nullptr;
        ShutdownFunction shutdown = nullptr;
    };

    /** @brief Dynamically resolved POSIX UDP datagram operations. */
    struct UdpSystemAPI {
        using SendToFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, const void*, size_t, int,
                                                         const sockaddr*, PosixSystem::SocketLength);
        using ReceiveFromFunction = PosixSystem::ByteCount(*)(PosixSystem::Socket, void*, size_t, int, sockaddr*,
                                                              PosixSystem::SocketLength*);

        SendToFunction sendTo = nullptr;
        ReceiveFromFunction receiveFrom = nullptr;
    };

    /** @brief POSIX has no additional native-error entry points beyond thread-local @c errno. */
    struct NativeErrorSystemAPI {};
    struct StructuredExceptionSystemAPI {};
    struct LocalMemorySystemAPI {};
    struct HandleSystemAPI {};
    struct SynchronizationSystemAPI {};
    struct FileMappingSystemAPI {
        // clang-format off
        using MapFunction       = void* (*)(void*, size_t, int, int, int, PosixSystem::FileOffset);
        using UnmapFunction     = int (*)(void*, size_t);
        using SyncMapFunction   = int (*)(void*, size_t, int);
        using AdviseMapFunction = int (*)(void*, size_t, int);

        [[= $::Syscall{"mmap"}]]
        MapFunction       map       = nullptr;
        [[= $::Syscall{"munmap"}]]
        UnmapFunction     unmap     = nullptr;
        [[= $::Syscall{"msync"}]]
        SyncMapFunction   syncMap   = nullptr;
        [[= $::Syscall{"madvise"}]]
        AdviseMapFunction adviseMap = nullptr;
        // clang-format on
    };
    struct SystemInformationSystemAPI {
        using SystemConfigurationFunction = long (*)(int);

        [[= $::Syscall{"sysconf"}]] SystemConfigurationFunction systemConfiguration = nullptr;
    };

    /** @brief Win32 event objects are unavailable on POSIX targets. */
    struct EventSystemAPI {};

    /** @brief Win32 I/O completion ports are unavailable on POSIX targets. */
    struct IOCPSystemAPI {};

    /** @brief Dynamically resolved Linux io_uring submission/completion operations. */
    struct IORingSystemAPI {
        // clang-format off
        using QueueInitFunction = int (*)(unsigned, void*, unsigned);
        using QueueExitFunction = void (*)(void*);
        using SubmitFunction    = int (*)(void*);
        using WaitCqeFunction   = int (*)(void*, void**);
        using PeekCqeFunction   = int (*)(void*, void**);
        using CqeSeenFunction   = void (*)(void*, void*);

        QueueInitFunction queueInit = nullptr;
        QueueExitFunction queueExit = nullptr;
        SubmitFunction    submit    = nullptr;
        WaitCqeFunction   waitCqe   = nullptr;
        PeekCqeFunction   peekCqe   = nullptr;
        CqeSeenFunction   cqeSeen   = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX process-environment entry points. */
    struct EnvironmentSystemAPI {
        // clang-format off
        using EnvironmentPointer     = char***;
        using GetEnvironmentFunction = EnvironmentPointer (*)();
        using GetFunction            = char* (*)(const char*);
        using SetFunction            = int (*)(const char*, const char*, int);
        using RemoveFunction         = int (*)(const char*);

        [[= $::Syscall{"getenv"}]]
        GetFunction            get            = nullptr;
        [[= $::Syscall{"setenv"}]]
        SetFunction            set            = nullptr;
        [[= $::Syscall{"unsetenv"}]]
        RemoveFunction         remove         = nullptr;
        [[= $::Var{"environ"}]]
        EnvironmentPointer     environment    = nullptr;
        [[= $::Syscall{"_NSGetEnviron"}]]
        GetEnvironmentFunction getEnvironment = nullptr;
        // clang-format on

        /** @brief Return the current native environment-entry array through the available platform ABI. */
        [[nodiscard]] char** EnvironmentEntries() const noexcept {
            EnvironmentPointer pointer = getEnvironment != nullptr ? getEnvironment() : environment;
            return pointer != nullptr ? *pointer : nullptr;
        }
    };

    /** @brief Dynamically resolved module-loader operations with normalized POSIX handle semantics. */
    struct ModuleSystemAPI {
        // clang-format off
        using OpenFunction         = void* (*)(const char*, int) noexcept;
        using CloseFunction        = int (*)(void*) noexcept;
        using FindSymbolFunction   = void* (*)(void*, const char*) noexcept;
        using QueryAddressFunction = bool (*)(const void*, ModuleAddressInfo*) noexcept;

        [[= $::Syscall{"dlopen"}]]
        OpenFunction         open         = nullptr;
        [[= $::Syscall{"dlclose"}]]
        CloseFunction        close        = nullptr;
        [[= $::Syscall{"dlsym"}]]
        FindSymbolFunction   findSymbol   = nullptr;
        [[= $::Syscall{"dladdr"}]]
        QueryAddressFunction queryAddress = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved and normalized POSIX current-process introspection operations. */
    struct ProcessSystemAPI {
        // clang-format off
        using GetProcessIdFunction       = int (*)();
        using GetParentProcessIdFunction = int (*)();
        using GetResourceUsageFunction   = int (*)(int, ::rusage*);
        using CaptureUsageFunction       = bool (*)(ProcessUsageCounters*) noexcept;

        [[= $::Syscall{"getpid"}]]
        GetProcessIdFunction       getProcessId       = nullptr;
        [[= $::Syscall{"getppid"}]]
        GetParentProcessIdFunction getParentProcessId = nullptr;
        [[= $::Syscall{"getrusage"}]]
        GetResourceUsageFunction   getResourceUsage   = nullptr;
        [[= $::Syscall{"getrusage"}]]
#    if defined(PLATFORM_LINUX)
        [[= $::Syscall{"sysconf"}]]
        [[= $::Syscall{"open"}]]
        [[= $::Syscall{"read"}]]
        [[= $::Syscall{"close"}]]
#    else
        [[= $::Syscall{"getpid"}]]
        [[= $::Syscall{"proc_pid_rusage"}]]
#    endif
        CaptureUsageFunction       captureUsage       = nullptr;
        
#    if defined(PLATFORM_MACOS)
        using GetExecutablePathFunction      = int (*)(char*, uint32_t*);
        using GetArgumentCountFunction       = int* (*)();
        using GetArgumentVectorFunction      = char*** (*)();
        using ProcessResourceUsageFunction   = int (*)(int, int, PosixSystem::ResourceUsageInfo*);
        
        [[= $::Syscall{"_NSGetExecutablePath"}]]
        GetExecutablePathFunction      getExecutablePath      = nullptr;
        [[= $::Syscall{"_NSGetArgc"}]]
        GetArgumentCountFunction       getArgumentCount       = nullptr;
        [[= $::Syscall{"_NSGetArgv"}]]
        GetArgumentVectorFunction      getArgumentVector      = nullptr;
        [[= $::Syscall{"proc_pid_rusage"}]]
        ProcessResourceUsageFunction   processResourceUsage   = nullptr;
#    endif
        // clang-format on
    };
    /** @brief Dynamically resolved Linux or Darwin current-thread entry points. */
    struct ThreadSystemAPI {
        // clang-format off
        using PthreadSelfFunction    = PosixSystem::ThreadId (*)();
        using PthreadGetNameFunction = int (*)(PosixSystem::ThreadId, char*, size_t);

        [[= $::Syscall{"pthread_self"}]]
        PthreadSelfFunction    pthreadSelf    = nullptr;
        [[= $::Syscall{"pthread_getname_np"}]]
        PthreadGetNameFunction pthreadGetName = nullptr;

#    if defined(PLATFORM_LINUX)
        using GetThreadIdFunction              = int (*)();
        using ScheduleGetCpuFunction           = int (*)();
        using PthreadSetNameFunction           = int (*)(PosixSystem::ThreadId, const char*);
        using PthreadGetAttributesFunction     = int (*)(PosixSystem::ThreadId, PosixSystem::ThreadAttributes*);
        using PthreadDestroyAttributesFunction = int (*)(PosixSystem::ThreadAttributes*);
        using PthreadGetStackFunction          = int (*)(const PosixSystem::ThreadAttributes*, void**, size_t*);

        [[= $::Syscall{"gettid"}]]
        GetThreadIdFunction              getThreadId              = nullptr;
        [[= $::Syscall{"sched_getcpu"}]]
        ScheduleGetCpuFunction           scheduleGetCpu           = nullptr;
        [[= $::Syscall{"pthread_setname_np"}]]
        PthreadSetNameFunction           pthreadSetName           = nullptr;
        [[= $::Syscall{"pthread_getattr_np"}]]
        PthreadGetAttributesFunction     pthreadGetAttributes     = nullptr;
        [[= $::Syscall{"pthread_attr_destroy"}]]
        PthreadDestroyAttributesFunction pthreadDestroyAttributes = nullptr;
        [[= $::Syscall{"pthread_attr_getstack"}]]
        PthreadGetStackFunction          pthreadGetStack          = nullptr;
#    else
        using PthreadThreadIdFunction          = int (*)(PosixSystem::ThreadId, uint64_t*);
        using PthreadSetNameFunction           = int (*)(const char*);
        using PthreadGetStackAddressFunction   = void* (*)(PosixSystem::ThreadId);
        using PthreadGetStackSizeFunction      = size_t (*)(PosixSystem::ThreadId);
        
        [[= $::Syscall{"pthread_threadid_np"}]]
        PthreadThreadIdFunction          pthreadThreadId          = nullptr;
        [[= $::Syscall{"pthread_setname_np"}]]
        PthreadSetNameFunction           pthreadSetName           = nullptr;
        [[= $::Syscall{"pthread_get_stackaddr_np"}]]
        PthreadGetStackAddressFunction   pthreadGetStackAddress   = nullptr;
        [[= $::Syscall{"pthread_get_stacksize_np"}]]
        PthreadGetStackSizeFunction      pthreadGetStackSize      = nullptr;
#    endif
        // clang-format on
    };

    /** @brief Win32 movable global memory is unavailable on POSIX. */
    struct GlobalMemorySystemAPI {};

    /** @brief Clipboard is not implemented through a stable POSIX system ABI. */
    struct ClipboardSystemAPI {};

    /** @brief Display system API entry points. */
    struct DisplaySystemAPI {};

    /** @brief Dynamically resolved POSIX file, mapping, replacement, and monitoring entry points. */
    struct FileSystemAPI {
        // clang-format off
        using OpenFunction                = int (*)(const char*, int, ...);
        using CloseFunction               = int (*)(int);
        using ReadFunction                = Sora::ssize_t (*)(int, void*, size_t);
        using WriteFunction               = Sora::ssize_t (*)(int, const void*, size_t);
        using ReadAtFunction              = Sora::ssize_t (*)(int, void*, size_t, PosixSystem::FileOffset);
        using WriteAtFunction             = Sora::ssize_t (*)(int, const void*, size_t, PosixSystem::FileOffset);
        using SyncFunction                = int (*)(int);
        using ResizeFunction              = int (*)(int, PosixSystem::FileOffset);
        using StatFunction                = int (*)(int, struct stat*);
        using RenameFunction              = int (*)(const char*, const char*);
        using UnlinkFunction              = int (*)(const char*);
        using ControlFunction             = int (*)(int, int, ...);
        using QueryFileBlockSizeFunction  = bool (*)(int, size_t*) noexcept;
        using QueryFileSizeFunction       = bool (*)(int, uint64_t*) noexcept;
        using DuplicateFunction           = int (*)(int);
        using AdviseFileFunction          = int (*)(int, PosixSystem::FileOffset, PosixSystem::FileOffset, int);
        using LockFunction                = int (*)(int, int);

        [[= $::Syscall{"open"}]]
        OpenFunction                open                = nullptr;
        [[= $::Syscall{"close"}]]
        CloseFunction               close               = nullptr;
        [[= $::Syscall{"read"}]]
        ReadFunction                read                = nullptr;
        [[= $::Syscall{"write"}]]
        WriteFunction               write               = nullptr;
        [[= $::Syscall{"pread"}]]
        ReadAtFunction              readAt              = nullptr;
        [[= $::Syscall{"pwrite"}]]
        WriteAtFunction             writeAt             = nullptr;
        [[= $::Syscall{"fsync"}]]
        SyncFunction                sync                = nullptr;
        [[= $::Syscall{"ftruncate"}]]
        ResizeFunction              resize              = nullptr;
        [[= $::Syscall{"fstat"}]]
        StatFunction                stat                = nullptr;
        [[= $::Syscall{"rename"}]]
        RenameFunction              rename              = nullptr;
        [[= $::Syscall{"unlink"}]]
        UnlinkFunction              unlink              = nullptr;
        [[= $::Syscall{"fcntl"}]]
        ControlFunction             control             = nullptr;
        [[= $::Syscall{"fstat"}]]
        QueryFileBlockSizeFunction  queryFileBlockSize  = nullptr;
        [[= $::Syscall{"fstat"}]]
        QueryFileSizeFunction       queryFileSize       = nullptr;
        [[= $::Syscall{"dup"}]]
        DuplicateFunction           duplicate           = nullptr;
        [[= $::Syscall{"posix_fadvise"}]]
        AdviseFileFunction          adviseFile          = nullptr;
        [[= $::Syscall{"flock"}]]
        LockFunction                lock                = nullptr;

#    if defined(PLATFORM_LINUX)
        using InitializeNotifyFunction = int (*)(int);
        using AddNotifyFunction        = int (*)(int, const char*, uint32_t);
        using RemoveNotifyFunction     = int (*)(int, int);
        using PollFunction             = int (*)(struct pollfd*, PosixSystem::PollCount, int);

        [[= $::Syscall{"inotify_init1"}]]
        InitializeNotifyFunction initializeNotify = nullptr;
        [[= $::Syscall{"inotify_add_watch"}]]
        AddNotifyFunction        addNotify        = nullptr;
        [[= $::Syscall{"inotify_rm_watch"}]]
        RemoveNotifyFunction     removeNotify     = nullptr;
        [[= $::Syscall{"poll"}]]
        PollFunction             poll             = nullptr;
#    elif defined(PLATFORM_MACOS)
        using CreateQueueFunction = int (*)();
        using QueueEventFunction  = int (*)(int, const struct kevent*, int, struct kevent*, int, const struct timespec*);

        [[= $::Syscall{"kqueue"}]]
        CreateQueueFunction createQueue = nullptr;
        [[= $::Syscall{"kevent"}]]
        QueueEventFunction  queueEvent  = nullptr;
#    endif
        // clang-format on
    };

    /** @brief DbgHelp is unavailable outside Windows. */
    struct DbgHelpSystemAPI {};

    /** @brief Dynamically resolved POSIX current-thread stack-capture entry point. */
    struct StackTraceSystemAPI {
        // clang-format off
        using CaptureStackBackTraceFunction = int (*)(void**, int);

        [[= $::Syscall{"backtrace"}]]
        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        // clang-format on
    };

#else

    struct SocketSystemAPI {};
    struct TcpSystemAPI {};
    struct UdpSystemAPI {};
    struct NativeErrorSystemAPI {};
    struct StructuredExceptionSystemAPI {};
    struct LocalMemorySystemAPI {};
    struct HandleSystemAPI {};
    struct SynchronizationSystemAPI {};
    struct FileMappingSystemAPI {};
    struct EventSystemAPI {};
    struct IOCPSystemAPI {};
    struct IORingSystemAPI {};
    struct SystemInformationSystemAPI {};
    struct EnvironmentSystemAPI {};
    struct ModuleSystemAPI {};
    struct ProcessSystemAPI {};
    struct ThreadSystemAPI {};
    struct GlobalMemorySystemAPI {};
    struct ClipboardSystemAPI {};
    struct DisplaySystemAPI {};
    struct FileSystemAPI {};
    struct DbgHelpSystemAPI {};
    struct StackTraceSystemAPI {};

#endif

    /** @brief Normalized scheduler-tracing transport implemented by the platform-specific system adapter. */
    struct SchedulerTraceSystemAPI {
        // clang-format off
        using SessionHandle   = void*;
        using EventCallback   = void (*)(const NativeSchedulerTraceRecord&, void*) noexcept;
        using LossCallback    = void (*)(uint64_t eventsLost, void*) noexcept;
        using StartFunction   = ErrorCode (*)(const NativeSchedulerTraceOptions&, EventCallback, LossCallback, void*, SessionHandle*, uint64_t*) noexcept;
        using ProcessFunction = ErrorCode (*)(SessionHandle) noexcept;
        using StopFunction    = ErrorCode (*)(SessionHandle, NativeSchedulerTraceLosses*) noexcept;
        using CloseFunction   = void (*)(SessionHandle) noexcept;

#if defined(PLATFORM_WINDOWS)
        using StartTraceFunction                = WindowsSystem::DWord(__stdcall*)(WindowsSystem::TraceHandle*, const wchar_t*, ::_EVENT_TRACE_PROPERTIES*);
        using ControlTraceFunction              = WindowsSystem::DWord(__stdcall*)(WindowsSystem::TraceHandle, const wchar_t*, ::_EVENT_TRACE_PROPERTIES*, WindowsSystem::DWord);
        using OpenTraceFunction                 = WindowsSystem::TraceHandle(__stdcall*)(::_EVENT_TRACE_LOGFILEW*);
        using ProcessTraceFunction              = WindowsSystem::DWord(__stdcall*)(WindowsSystem::TraceHandle*, WindowsSystem::DWord, ::_FILETIME*, ::_FILETIME*);
        using CloseTraceFunction                = WindowsSystem::DWord(__stdcall*)(WindowsSystem::TraceHandle);
        using QueryPerformanceFrequencyFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::NativeLargeInteger*);

        [[= $::Syscall{"StartTraceW"}]]
        StartTraceFunction                startTrace                = nullptr;
        [[= $::Syscall{"ControlTraceW"}]]
        ControlTraceFunction              controlTrace              = nullptr;
        [[= $::Syscall{"OpenTraceW"}]]
        OpenTraceFunction                 openTrace                 = nullptr;
        [[= $::Syscall{"ProcessTrace"}]]
        ProcessTraceFunction              processTrace              = nullptr;
        [[= $::Syscall{"CloseTrace"}]]
        CloseTraceFunction                closeTrace                = nullptr;
        [[= $::Syscall{"QueryPerformanceFrequency"}]]
        QueryPerformanceFrequencyFunction queryPerformanceFrequency = nullptr;
#endif

        StartFunction   start   = nullptr;     /**< Create and start one native producer and real-time consumer. */
        ProcessFunction process = nullptr;     /**< Block while dispatching native records to the event callback. */
        StopFunction    stop    = nullptr;     /**< Stop collection and capture final native loss totals. */
        CloseFunction   close   = nullptr;     /**< Close the consumer and destroy the opaque native session. */
        // clang-format on
    };

    /** @brief Exclusive access token for the process-global, non-thread-safe DbgHelp API. */
    class LockedDbgHelpSystemAPI {
    public:
        LockedDbgHelpSystemAPI(const LockedDbgHelpSystemAPI&) = delete;
        LockedDbgHelpSystemAPI& operator=(const LockedDbgHelpSystemAPI&) = delete;
        LockedDbgHelpSystemAPI(LockedDbgHelpSystemAPI&&) noexcept = delete;
        LockedDbgHelpSystemAPI& operator=(LockedDbgHelpSystemAPI&&) noexcept = delete;

        /** @brief Access DbgHelp entry points while this token owns the process-global lock. */
        [[nodiscard]] const DbgHelpSystemAPI& operator*() const noexcept { return api_; }

        /** @brief Access DbgHelp entry points while this token owns the process-global lock. */
        [[nodiscard]] const DbgHelpSystemAPI* operator->() const noexcept { return &api_; }

        /** @brief Resolve @p address in @p process into owned platform-neutral symbol information. */
        [[nodiscard]] bool ResolveAddress(void* process, uintptr_t address, DbgHelpAddressInfo& output) const;

    private:
        friend LockedDbgHelpSystemAPI LockDbgHelpSystemAPI();

        LockedDbgHelpSystemAPI(std::mutex& mutex, const DbgHelpSystemAPI& api) : lock_{mutex}, api_{api} {}

        std::unique_lock<std::mutex> lock_;
        const DbgHelpSystemAPI& api_;
    };

    inline namespace API {

        /** @brief Load and return the immutable native error-reporting function table. */
        [[nodiscard]] const NativeErrorSystemAPI& LoadNativeErrorSystemAPI() noexcept;

        /** @brief Load and return the immutable structured-exception function table. */
        [[nodiscard]] const StructuredExceptionSystemAPI& LoadStructuredExceptionSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 local-memory function table. */
        [[nodiscard]] const LocalMemorySystemAPI& LoadLocalMemorySystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 kernel-handle function table. */
        [[nodiscard]] const HandleSystemAPI& LoadHandleSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 synchronization function table. */
        [[nodiscard]] const SynchronizationSystemAPI& LoadSynchronizationSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 file-mapping function table. */
        [[nodiscard]] const FileMappingSystemAPI& LoadFileMappingSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 event function table. */
        [[nodiscard]] const EventSystemAPI& LoadEventSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 I/O completion port function table. */
        [[nodiscard]] const IOCPSystemAPI& LoadIOCPSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 I/O ring function table. */
        [[nodiscard]] const IORingSystemAPI& LoadIORingSystemAPI() noexcept;

        /** @brief Load and return the immutable system-information function table. */
        [[nodiscard]] const SystemInformationSystemAPI& LoadSystemInformationSystemAPI() noexcept;

        /** @brief Load and return the immutable process-environment function table. */
        [[nodiscard]] const EnvironmentSystemAPI& LoadEnvironmentSystemAPI() noexcept;

        /** @brief Load and return the immutable normalized dynamic-module function table. */
        [[nodiscard]] const ModuleSystemAPI& LoadModuleSystemAPI() noexcept;

        /** @brief Load and return the immutable current-process introspection function table. */
        [[nodiscard]] const ProcessSystemAPI& LoadProcessSystemAPI() noexcept;

        /** @brief Load and return the immutable current-thread function table. */
        [[nodiscard]] const ThreadSystemAPI& LoadThreadSystemAPI() noexcept;

        /** @brief Load and return the immutable native scheduler-tracing function table. */
        [[nodiscard]] const SchedulerTraceSystemAPI& LoadSchedulerTraceSystemAPI() noexcept;

        /** @brief Load and return the immutable socket-runtime function table. */
        [[nodiscard]] const SocketSystemAPI& LoadSocketSystemAPI() noexcept;

        /** @brief Load and return the immutable TCP stream function table. */
        [[nodiscard]] const TcpSystemAPI& LoadTcpSystemAPI() noexcept;

        /** @brief Load and return the immutable UDP datagram function table. */
        [[nodiscard]] const UdpSystemAPI& LoadUdpSystemAPI() noexcept;

        /** @brief Load and return the immutable movable-global-memory function table. */
        [[nodiscard]] const GlobalMemorySystemAPI& LoadGlobalMemorySystemAPI() noexcept;

        /** @brief Load and return the immutable clipboard function table. */
        [[nodiscard]] const ClipboardSystemAPI& LoadClipboardSystemAPI() noexcept;

        /** @brief Load and return the immutable display, monitor, and DPI function table. */
        [[nodiscard]] const DisplaySystemAPI& LoadDisplaySystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 window and message-pump function table. */
        [[nodiscard]] const Win32WindowSystemAPI& LoadWin32WindowSystemAPI() noexcept;

        /** @brief Load and return the immutable Xlib window and event function table. */
        [[nodiscard]] const X11WindowSystemAPI& LoadX11WindowSystemAPI() noexcept;

        /** @brief Load and return the immutable XCB window and event function table. */
        [[nodiscard]] const XcbWindowSystemAPI& LoadXcbWindowSystemAPI() noexcept;

        /** @brief Load and return the immutable Wayland client transport function table. */
        [[nodiscard]] const WaylandWindowSystemAPI& LoadWaylandWindowSystemAPI() noexcept;

        /** @brief Load and return the immutable native file-system function table. */
        [[nodiscard]] const FileSystemAPI& LoadFileSystemAPI() noexcept;

    } // namespace API

    /**
     * @brief Install the process-global native crash adapter.
     * @param callback Allocation-free callback invoked from the native fatal path.
     * @param userData Opaque state passed unchanged to @p callback.
     * @param suppressDialogs Suppress operating-system crash dialogs when supported.
     * @return @c true if the adapter was installed; otherwise @c false.
     */
    [[nodiscard]] bool InstallNativeCrashHandler(NativeCrashCallback callback, void* userData,
                                                 bool suppressDialogs) noexcept;

    /** @brief Restore the native crash handlers and process policy replaced by Sora. */
    void UninstallNativeCrashHandler() noexcept;

    /** @brief Lock and return exclusive access to the immutable Windows DbgHelp function table. */
    [[nodiscard]] LockedDbgHelpSystemAPI LockDbgHelpSystemAPI();

    /** @brief Initialize the process-global DbgHelp symbol handler once. */
    [[nodiscard]] bool InitializeDbgHelpSystemAPI() noexcept;

    /** @brief Load and return the immutable native stack-trace function table. */
    [[nodiscard]] const StackTraceSystemAPI& LoadStackTraceSystemAPI() noexcept;

    /** @brief Ensure that all provided system APIs are loaded and valid. */
    template<typename... Ts>
        requires(sizeof...(Ts) > 0 && (std::is_function_v<std::remove_pointer_t<Ts>> && ...))
    [[nodiscard]] bool EnsureSystemAPIs(Ts&... apis) noexcept {
        return (apis && ...);
    }

} // namespace Sora::PAL
