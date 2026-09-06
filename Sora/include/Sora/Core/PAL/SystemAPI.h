/**
 * @file SystemAPI.h
 * @brief Dynamically resolved operating-system entry points used by the platform abstraction layer.
 * @ingroup PAL
 * @anchor SoraPALSystemAPIHeader
 *
 * @section SoraPALSystemAPITableOfContents API table directory
 *
 * @par Common tables
 * - @ref Sora::PAL::SocketSystemAPI
 * - @ref Sora::PAL::TcpSystemAPI
 * - @ref Sora::PAL::UdpSystemAPI
 * - @ref Sora::PAL::ClockSystemAPI
 * - @ref Sora::PAL::AdvancedSocketSystemAPI
 * - @ref Sora::PAL::NetworkInterfaceSystemAPI
 * - @ref Sora::PAL::ProcessControlSystemAPI
 * - @ref Sora::PAL::ModernFileSystemAPI
 * - @ref Sora::PAL::RandomSystemAPI
 * - @ref Sora::PAL::VirtualMemorySystemAPI
 * - @ref Sora::PAL::FileMappingSystemAPI
 * - @ref Sora::PAL::EnvironmentSystemAPI
 * - @ref Sora::PAL::ModuleSystemAPI
 * - @ref Sora::PAL::ProcessSystemAPI
 * - @ref Sora::PAL::ThreadSystemAPI
 * - @ref Sora::PAL::ThreadControlSystemAPI
 * - @ref Sora::PAL::GlobalMemorySystemAPI
 * - @ref Sora::PAL::ClipboardSystemAPI
 * - @ref Sora::PAL::DisplaySystemAPI
 * - @ref Sora::PAL::FileSystemAPI
 *
 * @par Windows tables
 * - @ref Sora::PAL::NativeErrorSystemAPI
 * - @ref Sora::PAL::StructuredExceptionSystemAPI
 * - @ref Sora::PAL::LocalMemorySystemAPI
 * - @ref Sora::PAL::HandleSystemAPI
 * - @ref Sora::PAL::SynchronizationSystemAPI
 * - @ref Sora::PAL::EventSystemAPI
 * - @ref Sora::PAL::IOCPSystemAPI
 * - @ref Sora::PAL::WindowsIORingSystemAPI
 * - @ref Sora::PAL::WinsockOverlappedSystemAPI
 * - @ref Sora::PAL::Win32WindowSystemAPI
 * - @ref Sora::PAL::SystemInformationSystemAPI
 * - @ref Sora::PAL::DebugSystemAPI
 * - @ref Sora::PAL::COMSystemAPI
 * - @ref Sora::PAL::OleSystemAPI
 * - @ref Sora::PAL::OleAutomationSystemAPI
 * - @ref Sora::PAL::SchedulerTraceSystemAPI
 * - @ref Sora::PAL::DbgHelpSystemAPI
 * - @ref Sora::PAL::StackTraceSystemAPI
 *
 * @par Linux and POSIX tables
 * - @ref Sora::PAL::EpollSystemAPI
 * - @ref Sora::PAL::EventFdSystemAPI
 * - @ref Sora::PAL::TimerFdSystemAPI
 * - @ref Sora::PAL::SignalFdSystemAPI
 * - @ref Sora::PAL::FanotifySystemAPI
 * - @ref Sora::PAL::LinuxSyscallSystemAPI
 * - @ref Sora::PAL::PerfEventSystemAPI
 * - @ref Sora::PAL::LinuxIoUringSystemAPI
 * - @ref Sora::PAL::X11WindowSystemAPI
 * - @ref Sora::PAL::XcbWindowSystemAPI
 * - @ref Sora::PAL::WaylandWindowSystemAPI
 *
 * @details The accessors in this file initialize immutable, process-lifetime function tables on first use. Callers
 * must test optional entry points before invoking them because an older operating system may not export them.
 *
 * @code{.cpp}
 * #if defined(PLATFORM_WINDOWS)
 * const Sora::PAL::ThreadSystemAPI& api = Sora::PAL::LoadThreadSystemAPI();
 * if (api.getCurrentThread != nullptr && api.setThreadDescription != nullptr) {
 *     api.setThreadDescription(api.getCurrentThread(), L"worker");
 * }
 * #endif
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
 * tables and preserves the chosen backend in @c NativeWindowView.
 * @note All connection, window, surface, and module lifetimes remain owned by the caller; PAL tables are non-owning
 * views over dynamically resolved entry points.
 *
 * @note @c Module owns the native dynamic-loader bootstrap and typed symbol lookup. Loader bootstrap primitives and
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

// Public PAL contract: no operating-system SDK/POSIX headers may be included here. Keep native headers in
// SystemAPI.cpp and expose only opaque declarations, PAL-owned records, and ABI-stable scalar aliases.
struct addrinfo;
struct sockaddr;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
struct epoll_event;
struct ifaddrs;
struct itimerspec;
struct kevent;
struct mmsghdr;
struct msghdr;
struct pollfd;
struct rusage;
struct stat;
struct statvfs;
struct sysinfo;
struct timespec;
#    if defined(PLATFORM_LINUX)
struct fanotify_event_metadata;
struct io_uring;
struct io_uring_cqe;
struct __kernel_timespec;
#    endif
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

struct _DEBUG_EVENT;
struct _EXCEPTION_POINTERS;
struct IUnknown;
struct IStream;
struct IStorage;
struct IDataObject;
struct IDropSource;
struct IDropTarget;
struct ITypeLib;
struct ITypeInfo;
struct IDispatch;
struct IRecordInfo;
struct tagSAFEARRAY;
struct tagVARIANT;
struct tagSTGMEDIUM;
struct _IMAGEHLP_LINE64;
struct _IMAGEHLP_MODULE64;
struct _EVENT_TRACE_LOGFILEW;
struct _EVENT_TRACE_PROPERTIES;
struct _FILETIME;
struct _MEMORY_BASIC_INFORMATION;
struct _PROCESS_MEMORY_COUNTERS;
struct _PROCESS_INFORMATION;
struct _REASON_CONTEXT;
struct _STARTUPINFOW;
struct _WIN32_FILE_ATTRIBUTE_DATA;
struct _WIN32_FIND_DATAW;
struct MEM_EXTENDED_PARAMETER;
struct _IP_ADAPTER_ADDRESSES_LH;
struct _MIB_IF_TABLE2;
union _LARGE_INTEGER;
struct _OVERLAPPED;
struct _OVERLAPPED_ENTRY;
struct _PROCESSOR_NUMBER;
struct _SECURITY_ATTRIBUTES;
struct _SYMBOL_INFO;
struct _WSABUF;
struct _WSAPROTOCOL_INFOW;
struct WSAData;
struct tagPROCESSENTRY32W;
#endif

namespace Sora::PAL {

    namespace $ {

        struct Macro {
            enum class Type : uint8_t { Mask, Serial };
            FixedString<256> value = "";
            Type type = Type::Serial;
        };

        /** @brief Associate a function-table entry with one native symbol or syscall operation. */
        struct Syscall {
            FixedString<256> name = ""; /**< Case-sensitive export or syscall operation name. */
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

    /** @brief DPI awareness mode understood by the Windows display adapter. */
    enum class DisplayDpiAwareness : int {
        Invalid = -1,
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
        Unknown = 0, /**< No native window ABI has been selected. */
        Win32,       /**< Win32 HWND and USER32 message-queue ABI. */
        Xlib,        /**< Xlib Display and Window ABI over the X11 protocol. */
        Xcb,         /**< XCB connection and window-id ABI over the X11 protocol. */
        Wayland,     /**< Wayland display, proxy, and surface ABI. */
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

        /** @brief Return @ref IsValid so a view can be tested directly in a condition. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }
    };

    /** @brief Capabilities that a native window backend may expose without implying semantic equivalence. */
    enum class WindowSystemFeature : uint64_t {
        None = 0,                          /**< No optional capability. */
        WindowCreation = uint64_t{1} << 0, /**< Create and destroy top-level native windows. */
        EventQueue = uint64_t{1} << 1,     /**< Poll or dispatch native window-system events. */
        WindowPosition = uint64_t{1} << 2, /**< Move a native window in global or parent coordinates. */
        WindowResize = uint64_t{1} << 3,   /**< Resize a native window. */
        WindowState = uint64_t{1} << 4,    /**< Map, minimize, maximize, or otherwise change window state. */
        PointerCapture = uint64_t{1} << 5, /**< Capture, confine, or warp the pointing device. */
        TextInput = uint64_t{1} << 6,      /**< Translate native key events through a text-input method. */
        VulkanSurface = uint64_t{1} << 7,  /**< Supply handles required by the matching Vulkan WSI extension. */
        DisplayFd = uint64_t{1} << 8,      /**< Expose a pollable display-connection file descriptor. */
    };

    /** @brief Raw feature mask for one dynamically loaded native window backend. */
    struct WindowSystemCapabilities {
        Platform::WindowSystem windowSystem = Platform::WindowSystem::Unknown;
        NativeWindowBackend backend = NativeWindowBackend::Unknown;
        uint64_t features = 0;

        /**
         * @brief Return whether every bit in @p feature is declared by this backend.
         * @param[in] feature Feature bit or bitwise-combined mask to test.
         * @return `true` when all requested bits are present; `None` is not considered a capability.
         */
        [[nodiscard]] constexpr bool Supports(WindowSystemFeature feature) const noexcept {
            const auto requested = std::to_underlying(feature);
            return requested != 0 && (features & requested) == requested;
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
        // GET_FILEEX_INFO_LEVELS is an anonymous SDK enum; int preserves its Win32 ABI without importing Windows.h.
        using GetFileExInfoLevel = int;
        using WindowMessageId = uint32_t;
        using WindowProcedureResult = intptr_t;
        using Socket = uintptr_t;
        using TimerApcRoutine = void(__stdcall*)(void*, DWord, DWord);
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

        /** @brief Layout-compatible Win32 @c RECT record; values are left, top, right, and bottom coordinates. */
        struct Rect {
            Long left = 0;
            Long top = 0;
            Long right = 0;
            Long bottom = 0;
        };

        /** @brief Layout-compatible Win32 @c POINT record. */
        struct Point {
            Long x = 0;
            Long y = 0;
        };

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
            Point point{};
        };

        /** @brief ABI-compatible Winsock readiness descriptor. */
        struct PollDescriptor {
            Socket descriptor = static_cast<Socket>(-1);
            int16_t events = 0;
            int16_t revents = 0;
        };
        using NativePollDescriptor = PollDescriptor;

        /** @brief ABI-compatible Windows I/O ring creation flags. */
        struct IoRingCreateFlags {
            uint32_t required = 0;
            uint32_t advisory = 0;
        };

        /** @brief Opaque handle returned by @c CreateIoRing and consumed by ring operations. */
        using IoRingHandle = void*;

        /** @brief ABI-compatible Windows I/O ring version and operation enumerations. */
        using IoRingVersion = uint32_t;
        using IoRingOperation = uint32_t;
        using IoRingSqeFlags = uint32_t;
        using IoRingWriteFlags = uint32_t;
        using IoRingFlushMode = uint32_t;

        /** @brief ABI-compatible Windows I/O ring handle reference. */
        struct IoRingHandleRef {
            uint32_t kind = 0;
            union {
                Handle handle;
                uint32_t index;
            } value{};
        };

        /** @brief ABI-compatible registered Windows I/O ring buffer reference. */
        struct IoRingRegisteredBuffer {
            uint32_t index = 0;
            uint32_t offset = 0;
        };

        /** @brief ABI-compatible Windows I/O ring buffer reference. */
        struct IoRingBufferRef {
            uint32_t kind = 0;
            union {
                void* address;
                IoRingRegisteredBuffer registeredBuffer;
            } value{};
        };

        /** @brief ABI-compatible Windows I/O ring registered-buffer descriptor. */
        struct IoRingBufferInfo {
            void* address = nullptr;
            uint32_t length = 0;
        };

        /** @brief ABI-compatible Windows I/O ring completion entry. */
        struct IoRingCompletion {
            uintptr_t userData = 0;
            HResult result = 0;
            uintptr_t information = 0;
        };

        /** @brief ABI-compatible Windows I/O ring capability record. */
        struct IoRingCapabilities {
            IoRingVersion maxVersion = 0;
            uint32_t maxSubmissionQueueSize = 0;
            uint32_t maxCompletionQueueSize = 0;
            uint32_t featureFlags = 0;
        };

        /** @brief ABI-compatible Windows I/O ring information record. */
        struct IoRingInfo {
            IoRingVersion version = 0;
            IoRingCreateFlags flags{};
            uint32_t submissionQueueSize = 0;
            uint32_t completionQueueSize = 0;
        };

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
        using ClipCursorFunction          = WindowsSystem::Bool(__stdcall*)(const WindowsSystem::Rect*);
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

        /** @brief Return whether the minimum Wayland transport symbols were resolved. */
        [[nodiscard]] constexpr bool Available() const noexcept {
            return connect != nullptr && disconnect != nullptr && dispatch != nullptr && flush != nullptr &&
                   getFileDescriptor != nullptr && proxyAddListener != nullptr && proxyDestroy != nullptr &&
                   proxyMarshalFlags != nullptr;
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

        /** @brief ABI-compatible Win32 physical-memory status without exposing the Windows SDK. */
        struct MemoryStatus {
            DWord length = sizeof(MemoryStatus);
            DWord memoryLoadPercent = 0;
            uint64_t totalPhysicalBytes = 0;
            uint64_t availablePhysicalBytes = 0;
            uint64_t totalPageFileBytes = 0;
            uint64_t availablePageFileBytes = 0;
            uint64_t totalVirtualBytes = 0;
            uint64_t availableVirtualBytes = 0;
            uint64_t availableExtendedVirtualBytes = 0;
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
            Rect monitor{};
            Rect work{};
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

            /** @brief Processor-feature selector consumed by @c SystemInformationSystemAPI. */
            inline constexpr DWord kArmNeonInstructionsAvailable = 19;

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
                inline constexpr int kFileStorageInformation             = 16;
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
                inline constexpr DWord kNotifySize           = 0x00000008;
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

    /** @brief Dynamically resolved socket-runtime, endpoint, option, and readiness operations. */
    struct SocketSystemAPI {
        // clang-format off
        using StartupFunction         = int(__stdcall*)(uint16_t, WSAData*);
        using CleanupFunction         = int(__stdcall*)();
        using GetLastErrorFunction    = int(__stdcall*)();
        using SetLastErrorFunction    = void(__stdcall*)(int);
        using SocketFunction          = WindowsSystem::Socket(__stdcall*)(int, int, int);
        using CloseFunction           = int(__stdcall*)(WindowsSystem::Socket);
        using BindFunction            = int(__stdcall*)(WindowsSystem::Socket, const sockaddr*, int);
        using NameFunction            = int(__stdcall*)(WindowsSystem::Socket, sockaddr*, int*);
        using OptionFunction          = int(__stdcall*)(WindowsSystem::Socket, int, int, const char*, int);
        using GetOptionFunction       = int(__stdcall*)(WindowsSystem::Socket, int, int, char*, int*);
        using IoctlFunction           = int(__stdcall*)(WindowsSystem::Socket, long, unsigned long*);
        using PollFunction            = int(__stdcall*)(WindowsSystem::NativePollDescriptor*, unsigned long, int);
        using GetAddressInfoFunction  = int(__stdcall*)(const char*, const char*, const addrinfo*, addrinfo**);
        using FreeAddressInfoFunction = void(__stdcall*)(addrinfo*);
        using GetNameInfoFunction     = int(__stdcall*)(const sockaddr*, int, char*, unsigned long, char*, unsigned long,
                                                    int);

        [[= $::Syscall{"WSAStartup"}]]       StartupFunction         startup         = nullptr;
        [[= $::Syscall{"WSACleanup"}]]       CleanupFunction         cleanup         = nullptr;
        [[= $::Syscall{"WSAGetLastError"}]]  GetLastErrorFunction    getLastError    = nullptr;
        [[= $::Syscall{"WSASetLastError"}]]  SetLastErrorFunction    setLastError    = nullptr;
        [[= $::Syscall{"socket"}]]           SocketFunction          create          = nullptr;
        [[= $::Syscall{"closesocket"}]]      CloseFunction           close           = nullptr;
        [[= $::Syscall{"bind"}]]             BindFunction            bind            = nullptr;
        [[= $::Syscall{"getsockname"}]]      NameFunction            getSocketName   = nullptr;
        [[= $::Syscall{"getpeername"}]]      NameFunction            getPeerName     = nullptr;
        [[= $::Syscall{"setsockopt"}]]       OptionFunction          setOption       = nullptr;
        [[= $::Syscall{"getsockopt"}]]       GetOptionFunction       getOption       = nullptr;
        [[= $::Syscall{"ioctlsocket"}]]      IoctlFunction           ioctl           = nullptr;
        [[= $::Syscall{"WSAPoll"}]]          PollFunction            poll            = nullptr;
        [[= $::Syscall{"getaddrinfo"}]]      GetAddressInfoFunction  getAddressInfo  = nullptr;
        [[= $::Syscall{"freeaddrinfo"}]]     FreeAddressInfoFunction freeAddressInfo = nullptr;
        [[= $::Syscall{"getnameinfo"}]]      GetNameInfoFunction     getNameInfo     = nullptr;
        // clang-format on

        /** @brief Address families understood by the socket table. */
        enum class Family : int { Unspecified = 0, IPv4 = 2, IPv6 = 23 };

        /** @brief Socket kinds understood by the transport-specific tables. */
        enum class Type : int { Stream = 1, Datagram = 2 };

        /** @brief Native transport protocol identifiers. */
        enum class Protocol : int { Any = 0, Tcp = 6, Udp = 17 };

        /** @brief Shutdown directions accepted by @ref TcpSystemAPI::shutdown. */
        enum class Shutdown : int { Receive = 0, Send = 1, Both = 2 };

        // clang-format off
        inline static constexpr int kPollReadable              = 0x0100;
        inline static constexpr int kPollWritable              = 0x0010;
        inline static constexpr int kPollError                 = 0x0001;
        inline static constexpr int kPollHangup                = 0x0002;
        inline static constexpr int kPollInvalid               = 0x0004;
        inline static constexpr int kOptionSocket              = 0xffff;
        inline static constexpr int kOptionReuseAddress        = 0x0004;
        inline static constexpr int kOptionKeepAlive           = 0x0008;
        inline static constexpr int kAddressInfoPassive        = 0x0001;
        inline static constexpr int kAddressInfoNumericHost    = 0x0004;
        inline static constexpr int kAddressInfoNumericService = 0x0008;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows overlapped Winsock operations. */
    struct WinsockOverlappedSystemAPI {
        // clang-format off
        using CompletionRoutine   = void(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, ::_OVERLAPPED*, WindowsSystem::DWord);
        using CreateFunction      = WindowsSystem::Socket(__stdcall*)(int, int, int, ::_WSAPROTOCOL_INFOW*, WindowsSystem::UInt, WindowsSystem::DWord);
        using SendFunction        = int(__stdcall*)(WindowsSystem::Socket, ::_WSABUF*, WindowsSystem::DWord, WindowsSystem::DWord*, WindowsSystem::DWord, ::_OVERLAPPED*, CompletionRoutine);
        using ReceiveFunction     = int(__stdcall*)(WindowsSystem::Socket, ::_WSABUF*, WindowsSystem::DWord, WindowsSystem::DWord*, WindowsSystem::DWord*, ::_OVERLAPPED*, CompletionRoutine);
        using SendToFunction      = int(__stdcall*)(WindowsSystem::Socket, ::_WSABUF*, WindowsSystem::DWord, WindowsSystem::DWord*, WindowsSystem::DWord, const sockaddr*, int, ::_OVERLAPPED*, CompletionRoutine);
        using ReceiveFromFunction = int(__stdcall*)(WindowsSystem::Socket, ::_WSABUF*, WindowsSystem::DWord, WindowsSystem::DWord*, WindowsSystem::DWord*, sockaddr*, int*, ::_OVERLAPPED*, CompletionRoutine);
        using GetResultFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Socket, ::_OVERLAPPED*, WindowsSystem::DWord*, WindowsSystem::Bool, WindowsSystem::DWord*);
        using ControlFunction     = int(__stdcall*)(WindowsSystem::Socket, WindowsSystem::DWord, void*, WindowsSystem::DWord, void*, WindowsSystem::DWord, WindowsSystem::DWord*, ::_OVERLAPPED*, CompletionRoutine);

        [[= $::Syscall{"WSASocketW"}]]             
        CreateFunction      create      = nullptr;
        [[= $::Syscall{"WSASend"}]]                
        SendFunction        send        = nullptr;
        [[= $::Syscall{"WSARecv"}]]                
        ReceiveFunction     receive     = nullptr;
        [[= $::Syscall{"WSASendTo"}]]              
        SendToFunction      sendTo      = nullptr;
        [[= $::Syscall{"WSARecvFrom"}]]            
        ReceiveFromFunction receiveFrom = nullptr;
        [[= $::Syscall{"WSAGetOverlappedResult"}]] 
        GetResultFunction   getResult   = nullptr;
        [[= $::Syscall{"WSAIoctl"}]]               
        ControlFunction     control     = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved monotonic and wall-clock operations. */
    struct ClockSystemAPI {
        // clang-format off
        using QueryCounterFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::NativeLargeInteger*);
        using QueryFrequencyFunction       = WindowsSystem::Bool(__stdcall*)(WindowsSystem::NativeLargeInteger*);
        using GetPreciseSystemTimeFunction = void(__stdcall*)(::_FILETIME*);

        [[= $::Syscall{"QueryPerformanceCounter"}]]        
        QueryCounterFunction         queryCounter         = nullptr;
        [[= $::Syscall{"QueryPerformanceFrequency"}]]      
        QueryFrequencyFunction       queryFrequency       = nullptr;
        [[= $::Syscall{"GetSystemTimePreciseAsFileTime"}]] 
        GetPreciseSystemTimeFunction getPreciseSystemTime = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved connection-oriented byte-stream operations. */
    struct TcpSystemAPI {
        // clang-format off
        using ListenFunction   = int(__stdcall*)(WindowsSystem::Socket, int);
        using AcceptFunction   = WindowsSystem::Socket(__stdcall*)(WindowsSystem::Socket, sockaddr*, int*);
        using ConnectFunction  = int(__stdcall*)(WindowsSystem::Socket, const sockaddr*, int);
        using TransferFunction = int(__stdcall*)(WindowsSystem::Socket, char*, int, int);
        using SendFunction     = int(__stdcall*)(WindowsSystem::Socket, const char*, int, int);
        using ShutdownFunction = int(__stdcall*)(WindowsSystem::Socket, int);

        [[= $::Syscall{"listen"}]]   
        ListenFunction   listen   = nullptr;
        [[= $::Syscall{"accept"}]]   
        AcceptFunction   accept   = nullptr;
        [[= $::Syscall{"connect"}]]  
        ConnectFunction  connect  = nullptr;
        [[= $::Syscall{"send"}]]     
        SendFunction     send     = nullptr;
        [[= $::Syscall{"recv"}]]     
        TransferFunction receive  = nullptr;
        [[= $::Syscall{"shutdown"}]] 
        ShutdownFunction shutdown = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved connectionless datagram operations. */
    struct UdpSystemAPI {
        // clang-format off
        using SendToFunction      = int(__stdcall*)(WindowsSystem::Socket, const char*, int, int, const sockaddr*, int);
        using ReceiveFromFunction = int(__stdcall*)(WindowsSystem::Socket, char*, int, int, sockaddr*, int*);

        [[= $::Syscall{"sendto"}]]   
        SendToFunction      sendTo      = nullptr;
        [[= $::Syscall{"recvfrom"}]] 
        ReceiveFromFunction receiveFrom = nullptr;
        // clang-format on
    };

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
        using WaitForSingleObjectFunction       = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord);
        using WaitForSingleObjectExFunction     = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, WindowsSystem::Bool);
        using WaitForMultipleObjectsFunction    = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord, const WindowsSystem::Handle*, WindowsSystem::Bool, WindowsSystem::DWord);
        using WaitForMultipleObjectsExFunction  = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord, const WindowsSystem::Handle*, WindowsSystem::Bool, WindowsSystem::DWord, WindowsSystem::Bool);
        using SignalObjectAndWaitFunction       = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle, WindowsSystem::DWord, WindowsSystem::Bool);
        using CreateMutexExWideFunction         = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*, const wchar_t*, WindowsSystem::DWord, WindowsSystem::DWord);
        using ReleaseMutexFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using CreateSemaphoreExWideFunction     = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*, long, long, const wchar_t*, WindowsSystem::DWord, WindowsSystem::DWord);
        using ReleaseSemaphoreFunction          = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, long, long*);
        using CreateWaitableTimerExWideFunction = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*, const wchar_t*, WindowsSystem::DWord, WindowsSystem::DWord);
        using SetWaitableTimerExFunction        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, const ::_LARGE_INTEGER*, long, WindowsSystem::TimerApcRoutine, void*, ::_REASON_CONTEXT*, WindowsSystem::DWord);
        using CancelWaitableTimerFunction       = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);

        [[= $::Syscall{"WaitForSingleObject"}]]
        WaitForSingleObjectFunction       waitForSingleObject       = nullptr;
        [[= $::Syscall{"WaitForSingleObjectEx"}]]
        WaitForSingleObjectExFunction     waitForSingleObjectEx     = nullptr;
        [[= $::Syscall{"WaitForMultipleObjects"}]]
        WaitForMultipleObjectsFunction    waitForMultipleObjects    = nullptr;
        [[= $::Syscall{"WaitForMultipleObjectsEx"}]]
        WaitForMultipleObjectsExFunction  waitForMultipleObjectsEx  = nullptr;
        [[= $::Syscall{"SignalObjectAndWait"}]]
        SignalObjectAndWaitFunction       signalObjectAndWait       = nullptr;
        [[= $::Syscall{"CreateMutexExW"}]]
        CreateMutexExWideFunction         createMutexExWide         = nullptr;
        [[= $::Syscall{"ReleaseMutex"}]]
        ReleaseMutexFunction              releaseMutex              = nullptr;
        [[= $::Syscall{"CreateSemaphoreExW"}]]
        CreateSemaphoreExWideFunction     createSemaphoreExWide     = nullptr;
        [[= $::Syscall{"ReleaseSemaphore"}]]
        ReleaseSemaphoreFunction          releaseSemaphore          = nullptr;
        [[= $::Syscall{"CreateWaitableTimerExW"}]]
        CreateWaitableTimerExWideFunction createWaitableTimerExWide = nullptr;
        [[= $::Syscall{"SetWaitableTimerEx"}]]
        SetWaitableTimerExFunction        setWaitableTimerEx        = nullptr;
        [[= $::Syscall{"CancelWaitableTimer"}]]
        CancelWaitableTimerFunction       cancelWaitableTimer       = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 virtual-memory reservation and protection operations. */
    struct VirtualMemorySystemAPI {
        // clang-format off
        using AllocateFunction       = void*(__stdcall*)(void*, size_t, WindowsSystem::DWord, WindowsSystem::DWord);
        using FreeFunction           = WindowsSystem::Bool(__stdcall*)(void*, size_t, WindowsSystem::DWord);
        using ProtectFunction        = WindowsSystem::Bool(__stdcall*)(void*, size_t, WindowsSystem::DWord, WindowsSystem::DWord*);
        using QueryFunction          = size_t(__stdcall*)(const void*, ::_MEMORY_BASIC_INFORMATION*, size_t);
        using Allocate2Function      = void*(__stdcall*)(WindowsSystem::Handle, void*, uint64_t, WindowsSystem::DWord, WindowsSystem::DWord, ::MEM_EXTENDED_PARAMETER*, WindowsSystem::DWord);
        using MapViewOfFile3Function = void*(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle, void*, uint64_t, size_t, WindowsSystem::DWord, WindowsSystem::DWord, ::MEM_EXTENDED_PARAMETER*, WindowsSystem::DWord);

        [[= $::Syscall{"VirtualAlloc"}]] 
        AllocateFunction allocate = nullptr;
        [[= $::Syscall{"VirtualFree"}]] 
        FreeFunction free = nullptr;
        [[= $::Syscall{"VirtualProtect"}]] 
        ProtectFunction protect = nullptr;
        [[= $::Syscall{"VirtualQuery"}]] 
        QueryFunction query = nullptr;
        [[= $::Syscall{"VirtualAlloc2"}]] 
        Allocate2Function allocate2 = nullptr;
        [[= $::Syscall{"MapViewOfFile3"}]] 
        MapViewOfFile3Function mapViewOfFile3 = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 event-object operations. */
    struct EventSystemAPI {
        // clang-format off
        using CreateEventWideFunction = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*, WindowsSystem::Bool, WindowsSystem::Bool, const wchar_t*);
        using SetEventFunction        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using ResetEventFunction      = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);

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
        using CreateIoCompletionPortFunction             = WindowsSystem::Handle(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle, WindowsSystem::Size, WindowsSystem::DWord);
        using GetQueuedCompletionStatusFunction          = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord*, WindowsSystem::Size*, ::_OVERLAPPED**, WindowsSystem::DWord);
        using GetQueuedCompletionStatusExFunction        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_OVERLAPPED_ENTRY*, WindowsSystem::ULong, WindowsSystem::ULong*, WindowsSystem::DWord, WindowsSystem::Bool);
        using PostQueuedCompletionStatusFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, WindowsSystem::Size, ::_OVERLAPPED*);
        using SetFileCompletionNotificationModesFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Byte);

        [[= $::Syscall{"CreateIoCompletionPort"}]]
        CreateIoCompletionPortFunction             createIoCompletionPort             = nullptr;
        [[= $::Syscall{"GetQueuedCompletionStatus"}]]
        GetQueuedCompletionStatusFunction          getQueuedCompletionStatus          = nullptr;
        [[= $::Syscall{"GetQueuedCompletionStatusEx"}]]
        GetQueuedCompletionStatusExFunction        getQueuedCompletionStatusEx        = nullptr;
        [[= $::Syscall{"PostQueuedCompletionStatus"}]]
        PostQueuedCompletionStatusFunction         postQueuedCompletionStatus         = nullptr;
        [[= $::Syscall{"SetFileCompletionNotificationModes"}]]
        SetFileCompletionNotificationModesFunction setFileCompletionNotificationModes = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows I/O ring operations. */
    struct WindowsIORingSystemAPI {
        // clang-format off
        using QueryCapabilitiesFunction    = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingCapabilities*);
        using IsOperationSupportedFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::IoRingOperation);
        using CreateIoRingFunction         = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingVersion, WindowsSystem::IoRingCreateFlags, uint32_t, uint32_t, WindowsSystem::IoRingHandle*);
        using GetIoRingInfoFunction        = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::IoRingInfo*);
        using SubmitIoRingFunction         = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, uint32_t, uint32_t, uint32_t*);
        using CloseIoRingFunction          = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle);
        using PopIoRingCompletionFunction  = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::IoRingCompletion*);
        using SetCompletionEventFunction   = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::Handle);
        using BuildCancelFunction          = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::IoRingHandleRef, uintptr_t, uintptr_t);
        using BuildReadFunction            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::IoRingHandleRef, WindowsSystem::IoRingBufferRef, uint32_t, uint64_t, uintptr_t, WindowsSystem::IoRingSqeFlags);
        using BuildWriteFunction           = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::IoRingHandleRef, WindowsSystem::IoRingBufferRef, uint32_t, uint64_t, WindowsSystem::IoRingWriteFlags, uintptr_t, WindowsSystem::IoRingSqeFlags);
        using BuildFlushFunction           = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, WindowsSystem::IoRingHandleRef, WindowsSystem::IoRingFlushMode, uintptr_t, WindowsSystem::IoRingSqeFlags);
        using BuildRegisterFilesFunction   = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, uint32_t, WindowsSystem::Handle const*, uintptr_t);
        using BuildRegisterBuffersFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::IoRingHandle, uint32_t, WindowsSystem::IoRingBufferInfo const*, uintptr_t);

        [[= $::Syscall{"QueryIoRingCapabilities"}]]
        QueryCapabilitiesFunction    queryCapabilities    = nullptr;
        [[= $::Syscall{"IsIoRingOpSupported"}]]
        IsOperationSupportedFunction isOperationSupported = nullptr;
        [[= $::Syscall{"CreateIoRing"}]]
        CreateIoRingFunction         createIoRing         = nullptr;
        [[= $::Syscall{"GetIoRingInfo"}]]
        GetIoRingInfoFunction        getIoRingInfo        = nullptr;
        [[= $::Syscall{"CloseIoRing"}]]
        CloseIoRingFunction          closeIoRing          = nullptr;
        [[= $::Syscall{"SubmitIoRing"}]]
        SubmitIoRingFunction         submitIoRing         = nullptr;
        [[= $::Syscall{"PopIoRingCompletion"}]]
        PopIoRingCompletionFunction  popIoRingCompletion  = nullptr;
        [[= $::Syscall{"SetIoRingCompletionEvent"}]]
        SetCompletionEventFunction   setCompletionEvent   = nullptr;
        [[= $::Syscall{"BuildIoRingCancelRequest"}]]
        BuildCancelFunction          buildCancel          = nullptr;
        [[= $::Syscall{"BuildIoRingReadFile"}]]
        BuildReadFunction            buildRead            = nullptr;
        [[= $::Syscall{"BuildIoRingWriteFile"}]]
        BuildWriteFunction           buildWrite           = nullptr;
        [[= $::Syscall{"BuildIoRingFlushFile"}]]
        BuildFlushFunction           buildFlush           = nullptr;
        [[= $::Syscall{"BuildIoRingRegisterFileHandles"}]]
        BuildRegisterFilesFunction   buildRegisterFiles   = nullptr;
        [[= $::Syscall{"BuildIoRingRegisterBuffers"}]]
        BuildRegisterBuffersFunction buildRegisterBuffers = nullptr;
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
        // clang-format off
        using GetSystemInfoFunction          = void(__stdcall*)(void*);
        using GlobalMemoryStatusFunction     = WindowsSystem::Bool(__stdcall*)(WindowsSystem::MemoryStatus*);
        using IsProcessorFeatureFunction     = WindowsSystem::Bool(__stdcall*)(WindowsSystem::DWord);

        [[= $::Syscall{"GetSystemInfo"}]]
        GetSystemInfoFunction      getSystemInfo      = nullptr;
        [[= $::Syscall{"GlobalMemoryStatusEx"}]]
        GlobalMemoryStatusFunction globalMemoryStatus = nullptr;
        [[= $::Syscall{"IsProcessorFeaturePresent"}]]
        IsProcessorFeatureFunction isProcessorFeature = nullptr;
        // clang-format on
    };

    /** @brief Indivisible documented Win32 debugger entry points. */
    struct DebugSystemAPI {
        // clang-format off
        using CheckRemoteDebuggerPresentFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Bool*);
        using ContinueDebugEventFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord);
        using DebugActiveProcessFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::DWord);
        using DebugBreakProcessFunction          = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using DebugSetProcessKillOnExitFunction  = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Bool);
        using FatalExitFunction                  = void(__stdcall*)(int);
        using IsDebuggerPresentFunction          = WindowsSystem::Bool(__stdcall*)();
        using BreakpointFunction                 = void(__stdcall*)();
        using OutputDebugStringAnsiFunction      = void(__stdcall*)(const char*);
        using OutputDebugStringWideFunction      = void(__stdcall*)(const wchar_t*);
        using WaitForDebugEventFunction          = WindowsSystem::Bool(__stdcall*)(::_DEBUG_EVENT*,
                                                                                    WindowsSystem::DWord);

        [[= $::Syscall{"CheckRemoteDebuggerPresent"}]]
        CheckRemoteDebuggerPresentFunction checkRemoteDebuggerPresent = nullptr;
        [[= $::Syscall{"ContinueDebugEvent"}]]
        ContinueDebugEventFunction         continueDebugEvent          = nullptr;
        [[= $::Syscall{"DebugActiveProcess"}]]
        DebugActiveProcessFunction         debugActiveProcess          = nullptr;
        [[= $::Syscall{"DebugActiveProcessStop"}]]
        DebugActiveProcessFunction         debugActiveProcessStop      = nullptr;
        [[= $::Syscall{"IsDebuggerPresent"}]]
        IsDebuggerPresentFunction          isDebuggerPresent           = nullptr;
        [[= $::Syscall{"DebugBreak"}]]
        BreakpointFunction                 breakpoint                  = nullptr;
        [[= $::Syscall{"DebugBreakProcess"}]]
        DebugBreakProcessFunction          breakpointProcess           = nullptr;
        [[= $::Syscall{"DebugSetProcessKillOnExit"}]]
        DebugSetProcessKillOnExitFunction  setProcessKillOnExit        = nullptr;
        [[= $::Syscall{"FatalExit"}]]
        FatalExitFunction                  fatalExit                    = nullptr;
        [[= $::Syscall{"OutputDebugStringA"}]]
        OutputDebugStringAnsiFunction      outputDebugStringAnsi       = nullptr;
        [[= $::Syscall{"OutputDebugStringW"}]]
        OutputDebugStringWideFunction      outputDebugStringWide       = nullptr;
        [[= $::Syscall{"WaitForDebugEvent"}]]
        WaitForDebugEventFunction          waitForDebugEvent           = nullptr;
        [[= $::Syscall{"WaitForDebugEventEx"}]]
        WaitForDebugEventFunction          waitForDebugEventExtended   = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved COM runtime, activation, marshaling, identity, allocator, and stream operations. */
    struct COMSystemAPI {
        using InitializeFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using InitializeExFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::DWord);
        using InitializeSecurityFunction
            = WindowsSystem::HResult(__stdcall*)(void*, long, void*, void*, WindowsSystem::DWord, WindowsSystem::DWord,
                                                 void*, WindowsSystem::DWord, void*);
        using UninitializeFunction = void(__stdcall*)();
        using GetApartmentTypeFunction = WindowsSystem::HResult(__stdcall*)(int*, int*);
        using GetCallerThreadIdFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord*);
        using GetContextTokenFunction = WindowsSystem::HResult(__stdcall*)(uintptr_t*);
        using GetObjectContextFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefInterfaceId, void**);
        using GetClassObjectFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, WindowsSystem::DWord, void*,
                                                 WindowsSystem::RefInterfaceId, void**);
        using CreateInstanceFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, void*, WindowsSystem::DWord,
                                                 WindowsSystem::RefInterfaceId, void**);
        using CreateInstanceExFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, void*, WindowsSystem::DWord, void*,
                                                 WindowsSystem::DWord, void*);
        using CreateInstanceFromAppFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, void*, WindowsSystem::DWord, void*,
                                                 WindowsSystem::DWord, void*);
        using RegisterClassObjectFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, void*, WindowsSystem::DWord,
                                                 WindowsSystem::DWord, WindowsSystem::DWord*);
        using RevokeClassObjectFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord);
        using ResumeSuspendClassObjectsFunction = WindowsSystem::HResult(__stdcall*)();
        using GetMallocFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord, void**);
        using CreateStandardMallocFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord, void**);
        using TaskMemAllocFunction = void*(__stdcall*)(size_t);
        using TaskMemReallocFunction = void*(__stdcall*)(void*, size_t);
        using TaskMemFreeFunction = void(__stdcall*)(void*);
        using CreateStreamOnGlobalFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Bool, void**);
        using GetGlobalFromStreamFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::Handle*);
        using MarshalSizeMaxFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::ULong*, WindowsSystem::RefInterfaceId, void*,
                                                 WindowsSystem::DWord, void*, WindowsSystem::DWord);
        using MarshalInterfaceFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, void*, WindowsSystem::DWord,
                                                 void*, WindowsSystem::DWord);
        using UnmarshalInterfaceFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, void**);
        using ReleaseMarshalDataFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using MarshalInterThreadInterfaceFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefInterfaceId, void*, void**);
        using GetInterfaceAndReleaseStreamFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, void**);
        using CreateFreeThreadedMarshalerFunction = WindowsSystem::HResult(__stdcall*)(void*, void**);
        using DisconnectObjectFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::DWord);
        using LockObjectExternalFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::Bool, WindowsSystem::Bool);
        using QueryProxyBlanketFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::DWord*, WindowsSystem::DWord*, wchar_t**,
                                                 WindowsSystem::DWord*, WindowsSystem::DWord*, void**,
                                                 WindowsSystem::DWord*);
        using SetProxyBlanketFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::DWord, WindowsSystem::DWord, wchar_t*,
                                                 WindowsSystem::DWord, WindowsSystem::DWord, void*,
                                                 WindowsSystem::DWord);
        using CancelCallFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord, WindowsSystem::ULong);
        using TestCancelFunction = WindowsSystem::HResult(__stdcall*)();
        using EnableDisableCallCancellationFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using CreateGuidFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Guid*);
        using StringFromGuidFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefGuid, wchar_t**);
        using GuidFromStringFunction = WindowsSystem::HResult(__stdcall*)(const wchar_t*, WindowsSystem::Guid*);
        using StringFromGuid2Function = int(__stdcall*)(WindowsSystem::RefGuid, wchar_t*, int);
        using ProgIdFromClsidFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, wchar_t**);
        using ClsidFromProgIdFunction = WindowsSystem::HResult(__stdcall*)(const wchar_t*, WindowsSystem::ClassId*);
        using WaitForMultipleHandlesFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::ULong,
                                                 WindowsSystem::Handle*, WindowsSystem::DWord*);
        using FreeUnusedLibrariesFunction = void(__stdcall*)();
        using FreeUnusedLibrariesExFunction = void(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord);

        // clang-format off
        [[= $::Syscall{"CoInitialize"}]]
        InitializeFunction                          initialize = nullptr;
        [[= $::Syscall{"CoInitializeEx"}]]
        InitializeExFunction                        initializeEx = nullptr;
        [[= $::Syscall{"CoInitializeSecurity"}]]
        InitializeSecurityFunction                  initializeSecurity = nullptr;
        [[= $::Syscall{"CoUninitialize"}]]
        UninitializeFunction                         uninitialize = nullptr;
        [[= $::Syscall{"CoGetApartmentType"}]]
        GetApartmentTypeFunction                    getApartmentType = nullptr;
        [[= $::Syscall{"CoGetCallerTID"}]]
        GetCallerThreadIdFunction                   getCallerThreadId = nullptr;
        [[= $::Syscall{"CoGetContextToken"}]]
        GetContextTokenFunction                    getContextToken = nullptr;
        [[= $::Syscall{"CoGetObjectContext"}]]
        GetObjectContextFunction                    getObjectContext = nullptr;
        [[= $::Syscall{"CoGetClassObject"}]]
        GetClassObjectFunction                     getClassObject = nullptr;
        [[= $::Syscall{"CoCreateInstance"}]]
        CreateInstanceFunction                     createInstance = nullptr;
        [[= $::Syscall{"CoCreateInstanceEx"}]]
        CreateInstanceExFunction                   createInstanceEx = nullptr;
        [[= $::Syscall{"CoCreateInstanceFromApp"}]]
        CreateInstanceFromAppFunction              createInstanceFromApp = nullptr;
        [[= $::Syscall{"CoRegisterClassObject"}]]
        RegisterClassObjectFunction                registerClassObject = nullptr;
        [[= $::Syscall{"CoRevokeClassObject"}]]
        RevokeClassObjectFunction                  revokeClassObject = nullptr;
        [[= $::Syscall{"CoResumeClassObjects"}]]
        ResumeSuspendClassObjectsFunction          resumeClassObjects = nullptr;
        [[= $::Syscall{"CoSuspendClassObjects"}]]
        ResumeSuspendClassObjectsFunction          suspendClassObjects = nullptr;
        [[= $::Syscall{"CoGetMalloc"}]]
        GetMallocFunction                          getMalloc = nullptr;
        [[= $::Syscall{"CoCreateStandardMalloc"}]]
        CreateStandardMallocFunction               createStandardMalloc = nullptr;
        [[= $::Syscall{"CoTaskMemAlloc"}]]
        TaskMemAllocFunction                       taskMemAlloc = nullptr;
        [[= $::Syscall{"CoTaskMemRealloc"}]]
        TaskMemReallocFunction                     taskMemRealloc = nullptr;
        [[= $::Syscall{"CoTaskMemFree"}]]
        TaskMemFreeFunction                        taskMemFree = nullptr;
        [[= $::Syscall{"CreateStreamOnHGlobal"}]]
        CreateStreamOnGlobalFunction               createStreamOnGlobal = nullptr;
        [[= $::Syscall{"GetHGlobalFromStream"}]]
        GetGlobalFromStreamFunction                getGlobalFromStream = nullptr;
        [[= $::Syscall{"CoGetMarshalSizeMax"}]]
        MarshalSizeMaxFunction                     getMarshalSizeMax = nullptr;
        [[= $::Syscall{"CoMarshalInterface"}]]
        MarshalInterfaceFunction                   marshalInterface = nullptr;
        [[= $::Syscall{"CoUnmarshalInterface"}]]
        UnmarshalInterfaceFunction                 unmarshalInterface = nullptr;
        [[= $::Syscall{"CoReleaseMarshalData"}]]
        ReleaseMarshalDataFunction                 releaseMarshalData = nullptr;
        [[= $::Syscall{"CoMarshalInterThreadInterfaceInStream"}]]
        MarshalInterThreadInterfaceFunction        marshalInterThreadInterface = nullptr;
        [[= $::Syscall{"CoGetInterfaceAndReleaseStream"}]]
        GetInterfaceAndReleaseStreamFunction       getInterfaceAndReleaseStream = nullptr;
        [[= $::Syscall{"CoCreateFreeThreadedMarshaler"}]]
        CreateFreeThreadedMarshalerFunction        createFreeThreadedMarshaler = nullptr;
        [[= $::Syscall{"CoDisconnectObject"}]]
        DisconnectObjectFunction                   disconnectObject = nullptr;
        [[= $::Syscall{"CoLockObjectExternal"}]]
        LockObjectExternalFunction                 lockObjectExternal = nullptr;
        [[= $::Syscall{"CoQueryProxyBlanket"}]]
        QueryProxyBlanketFunction                   queryProxyBlanket = nullptr;
        [[= $::Syscall{"CoSetProxyBlanket"}]]
        SetProxyBlanketFunction                     setProxyBlanket = nullptr;
        [[= $::Syscall{"CoCancelCall"}]]
        CancelCallFunction                          cancelCall = nullptr;
        [[= $::Syscall{"CoTestCancel"}]]
        TestCancelFunction                          testCancel = nullptr;
        [[= $::Syscall{"CoEnableCallCancellation"}]]
        EnableDisableCallCancellationFunction       enableCallCancellation = nullptr;
        [[= $::Syscall{"CoDisableCallCancellation"}]]
        EnableDisableCallCancellationFunction       disableCallCancellation = nullptr;
        [[= $::Syscall{"CoCreateGuid"}]]
        CreateGuidFunction                         createGuid = nullptr;
        [[= $::Syscall{"StringFromCLSID"}]]
        StringFromGuidFunction                     stringFromClsid = nullptr;
        [[= $::Syscall{"CLSIDFromString"}]]
        GuidFromStringFunction                     clsidFromString = nullptr;
        [[= $::Syscall{"StringFromIID"}]]
        StringFromGuidFunction                     stringFromIid = nullptr;
        [[= $::Syscall{"IIDFromString"}]]
        GuidFromStringFunction                     iidFromString = nullptr;
        [[= $::Syscall{"StringFromGUID2"}]]
        StringFromGuid2Function                    stringFromGuid2 = nullptr;
        [[= $::Syscall{"ProgIDFromCLSID"}]]
        ProgIdFromClsidFunction                    progIdFromClsid = nullptr;
        [[= $::Syscall{"CLSIDFromProgID"}]]
        ClsidFromProgIdFunction                    clsidFromProgId = nullptr;
        [[= $::Syscall{"CoWaitForMultipleHandles"}]]
        WaitForMultipleHandlesFunction             waitForMultipleHandles = nullptr;
        [[= $::Syscall{"CoFreeUnusedLibraries"}]]
        FreeUnusedLibrariesFunction                freeUnusedLibraries = nullptr;
        [[= $::Syscall{"CoFreeUnusedLibrariesEx"}]]
        FreeUnusedLibrariesExFunction              freeUnusedLibrariesEx = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved OLE object, persistence, data-transfer, drag/drop, and clipboard operations. */
    struct OleSystemAPI {
        using InitializeFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using UninitializeFunction = void(__stdcall*)();
        using BuildVersionFunction = WindowsSystem::DWord(__stdcall*)();
        using QueryDataFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using CreateObjectFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, WindowsSystem::RefInterfaceId,
                                                 WindowsSystem::DWord, void*, void*, void*, void**);
        using CreateFromDataFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, WindowsSystem::DWord, void*,
                                                 void*, void*, void**);
        using CreateLinkFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, WindowsSystem::DWord, void*,
                                                 void*, void*, void**);
        using CreateLinkToFileFunction
            = WindowsSystem::HResult(__stdcall*)(const wchar_t*, WindowsSystem::RefInterfaceId, WindowsSystem::DWord,
                                                 void*, void*, void*, void**);
        using CreateObjectExFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, WindowsSystem::RefInterfaceId,
                                                 WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::ULong,
                                                 WindowsSystem::DWord*, void*, void*, WindowsSystem::DWord*, void*,
                                                 void*, void**);
        using CreateFromDataExFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, WindowsSystem::DWord,
                                                 WindowsSystem::DWord, WindowsSystem::ULong, WindowsSystem::DWord*,
                                                 void*, void*, WindowsSystem::DWord*, void*, void*, void**);
        using CreateLinkExFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, WindowsSystem::DWord,
                                                 WindowsSystem::DWord, WindowsSystem::ULong, WindowsSystem::DWord*,
                                                 void*, void*, WindowsSystem::DWord*, void*, void*, void**);
        using CreateLinkToFileExFunction
            = WindowsSystem::HResult(__stdcall*)(const wchar_t*, WindowsSystem::RefInterfaceId, WindowsSystem::DWord,
                                                 WindowsSystem::DWord, WindowsSystem::ULong, WindowsSystem::DWord*,
                                                 void*, void*, WindowsSystem::DWord*, void*, void*, void**);
        using CreateFromFileFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, const wchar_t*,
                                                 WindowsSystem::RefInterfaceId, WindowsSystem::DWord, void*, void*,
                                                 void*, void**);
        using CreateFromFileExFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, const wchar_t*,
                                                 WindowsSystem::RefInterfaceId, WindowsSystem::DWord,
                                                 WindowsSystem::DWord, WindowsSystem::ULong, WindowsSystem::DWord*,
                                                 void*, void*, WindowsSystem::DWord*, void*, void*, void**);
        using LoadFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, void*, void**);
        using SaveFunction = WindowsSystem::HResult(__stdcall*)(void*, void*, WindowsSystem::Bool);
        using LoadFromStreamFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefInterfaceId, void**);
        using SaveToStreamFunction = WindowsSystem::HResult(__stdcall*)(void*, void*);
        using SetObjectStateFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::Bool);
        using RegisterDragDropFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::WindowHandle, void*);
        using RevokeDragDropFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::WindowHandle);
        using DoDragDropFunction
            = WindowsSystem::HResult(__stdcall*)(void*, void*, WindowsSystem::DWord, WindowsSystem::DWord*);
        using SetClipboardFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using GetClipboardFunction = WindowsSystem::HResult(__stdcall*)(void**);
        using FlushClipboardFunction = WindowsSystem::HResult(__stdcall*)();
        using IsCurrentClipboardFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using DrawFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::DWord, WindowsSystem::DeviceContextHandle,
                                                 const WindowsSystem::Rect*);
        using IsRunningFunction = WindowsSystem::Bool(__stdcall*)(void*);
        using LockRunningFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::Bool, WindowsSystem::Bool);
        using ReleaseStorageMediumFunction = void(__stdcall*)(void*);
        using CreateAdviseHolderFunction = WindowsSystem::HResult(__stdcall*)(void**);
        using CreateDataCacheFunction = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefClassId,
                                                                           WindowsSystem::RefInterfaceId, void**);

        // clang-format off
        [[= $::Syscall{"OleInitialize"}]]
        InitializeFunction                    initialize = nullptr;
        [[= $::Syscall{"OleUninitialize"}]]
        UninitializeFunction                  uninitialize = nullptr;
        [[= $::Syscall{"OleBuildVersion"}]]
        BuildVersionFunction                  buildVersion = nullptr;
        [[= $::Syscall{"OleQueryLinkFromData"}]]
        QueryDataFunction                     queryLinkFromData = nullptr;
        [[= $::Syscall{"OleQueryCreateFromData"}]]
        QueryDataFunction                     queryCreateFromData = nullptr;
        [[= $::Syscall{"OleCreate"}]]
        CreateObjectFunction                  create = nullptr;
        [[= $::Syscall{"OleCreateEx"}]]
        CreateObjectExFunction                createEx = nullptr;
        [[= $::Syscall{"OleCreateFromData"}]]
        CreateFromDataFunction               createFromData = nullptr;
        [[= $::Syscall{"OleCreateFromDataEx"}]]
        CreateFromDataExFunction             createFromDataEx = nullptr;
        [[= $::Syscall{"OleCreateLink"}]]
        CreateLinkFunction                   createLink = nullptr;
        [[= $::Syscall{"OleCreateLinkEx"}]]
        CreateLinkExFunction                  createLinkEx = nullptr;
        [[= $::Syscall{"OleCreateLinkToFile"}]]
        CreateLinkToFileFunction              createLinkToFile = nullptr;
        [[= $::Syscall{"OleCreateLinkToFileEx"}]]
        CreateLinkToFileExFunction            createLinkToFileEx = nullptr;
        [[= $::Syscall{"OleCreateFromFile"}]]
        CreateFromFileFunction                createFromFile = nullptr;
        [[= $::Syscall{"OleCreateFromFileEx"}]]
        CreateFromFileExFunction              createFromFileEx = nullptr;
        [[= $::Syscall{"OleLoad"}]]
        LoadFunction                          load = nullptr;
        [[= $::Syscall{"OleSave"}]]
        SaveFunction                          save = nullptr;
        [[= $::Syscall{"OleLoadFromStream"}]]
        LoadFromStreamFunction                loadFromStream = nullptr;
        [[= $::Syscall{"OleSaveToStream"}]]
        SaveToStreamFunction                  saveToStream = nullptr;
        [[= $::Syscall{"OleSetContainedObject"}]]
        SetObjectStateFunction                setContainedObject = nullptr;
        [[= $::Syscall{"OleNoteObjectVisible"}]]
        SetObjectStateFunction                noteObjectVisible = nullptr;
        [[= $::Syscall{"RegisterDragDrop"}]]
        RegisterDragDropFunction              registerDragDrop = nullptr;
        [[= $::Syscall{"RevokeDragDrop"}]]
        RevokeDragDropFunction                revokeDragDrop = nullptr;
        [[= $::Syscall{"DoDragDrop"}]]
        DoDragDropFunction                    doDragDrop = nullptr;
        [[= $::Syscall{"OleSetClipboard"}]]
        SetClipboardFunction                  setClipboard = nullptr;
        [[= $::Syscall{"OleGetClipboard"}]]
        GetClipboardFunction                  getClipboard = nullptr;
        [[= $::Syscall{"OleFlushClipboard"}]]
        FlushClipboardFunction                flushClipboard = nullptr;
        [[= $::Syscall{"OleIsCurrentClipboard"}]]
        IsCurrentClipboardFunction            isCurrentClipboard = nullptr;
        [[= $::Syscall{"OleDraw"}]]
        DrawFunction                          draw = nullptr;
        [[= $::Syscall{"OleRun"}]]
        QueryDataFunction                     run = nullptr;
        [[= $::Syscall{"OleIsRunning"}]]
        IsRunningFunction                     isRunning = nullptr;
        [[= $::Syscall{"OleLockRunning"}]]
        LockRunningFunction                   lockRunning = nullptr;
        [[= $::Syscall{"ReleaseStgMedium"}]]
        ReleaseStorageMediumFunction          releaseStorageMedium = nullptr;
        [[= $::Syscall{"CreateOleAdviseHolder"}]]
        CreateAdviseHolderFunction            createOleAdviseHolder = nullptr;
        [[= $::Syscall{"CreateDataAdviseHolder"}]]
        CreateAdviseHolderFunction            createDataAdviseHolder = nullptr;
        [[= $::Syscall{"CreateDataCache"}]]
        CreateDataCacheFunction               createDataCache = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved OLE Automation values, active objects, and type-library operations. */
    struct OleAutomationSystemAPI {
        using AllocStringFunction = WindowsSystem::AutomationString(__stdcall*)(const wchar_t*);
        using AllocStringLengthFunction = WindowsSystem::AutomationString(__stdcall*)(const wchar_t*, unsigned int);
        using ReallocStringLengthFunction
            = WindowsSystem::Bool(__stdcall*)(WindowsSystem::AutomationString*, const wchar_t*, unsigned int);
        using FreeStringFunction = void(__stdcall*)(WindowsSystem::AutomationString);
        using StringLengthFunction = unsigned int(__stdcall*)(WindowsSystem::AutomationString);
        using SafeArrayCreateFunction = void*(__stdcall*)(unsigned short, unsigned int, void*);
        using SafeArrayCreateVectorFunction = void*(__stdcall*)(unsigned short, long, unsigned long);
        using SafeArrayDestroyFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using SafeArrayCopyFunction = WindowsSystem::HResult(__stdcall*)(void*, void**);
        using SafeArrayAccessDataFunction = WindowsSystem::HResult(__stdcall*)(void*, void**);
        using SafeArrayUnaccessDataFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using SafeArrayGetElementFunction = WindowsSystem::HResult(__stdcall*)(void*, long*, void*);
        using SafeArrayPutElementFunction = WindowsSystem::HResult(__stdcall*)(void*, long*, void*);
        using SafeArrayGetBoundFunction = WindowsSystem::HResult(__stdcall*)(void*, unsigned int, long*);
        using SafeArrayLockFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using SafeArrayUnlockFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using SafeArrayGetDimensionFunction = unsigned int(__stdcall*)(void*);
        using VariantInitFunction = void(__stdcall*)(void*);
        using VariantOperationFunction = WindowsSystem::HResult(__stdcall*)(void*);
        using VariantCopyFunction = WindowsSystem::HResult(__stdcall*)(void*, const void*);
        using VariantCopyIndFunction = WindowsSystem::HResult(__stdcall*)(void*, const void*);
        using VariantChangeTypeFunction
            = WindowsSystem::HResult(__stdcall*)(void*, const void*, unsigned short, unsigned short);
        using VariantChangeTypeExFunction = WindowsSystem::HResult(__stdcall*)(void*, const void*, WindowsSystem::DWord,
                                                                               unsigned short, unsigned short);
        using RegisterActiveObjectFunction
            = WindowsSystem::HResult(__stdcall*)(void*, WindowsSystem::RefClassId, WindowsSystem::DWord,
                                                 WindowsSystem::DWord*);
        using RevokeActiveObjectFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord, void*);
        using GetActiveObjectFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefClassId, void*, void**);
        using LoadTypeLibraryFunction = WindowsSystem::HResult(__stdcall*)(const wchar_t*, void**);
        using LoadTypeLibraryExFunction = WindowsSystem::HResult(__stdcall*)(const wchar_t*, int, void**);
        using LoadRegisteredTypeLibraryFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefGuid, unsigned short, unsigned short,
                                                 WindowsSystem::DWord, void**);
        using QueryTypeLibraryPathFunction
            = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefGuid, unsigned short, unsigned short,
                                                 WindowsSystem::DWord, wchar_t**);

        // clang-format off
        [[= $::Syscall{"SysAllocString"}]]
        AllocStringFunction                    allocString = nullptr;
        [[= $::Syscall{"SysAllocStringLen"}]]
        AllocStringLengthFunction              allocStringLen = nullptr;
        [[= $::Syscall{"SysReAllocStringLen"}]]
        ReallocStringLengthFunction            reallocStringLen = nullptr;
        [[= $::Syscall{"SysFreeString"}]]
        FreeStringFunction                     freeString = nullptr;
        [[= $::Syscall{"SysStringLen"}]]
        StringLengthFunction                   stringLength = nullptr;
        [[= $::Syscall{"SysStringByteLen"}]]
        StringLengthFunction                   stringByteLength = nullptr;
        [[= $::Syscall{"SafeArrayCreate"}]]
        SafeArrayCreateFunction                safeArrayCreate = nullptr;
        [[= $::Syscall{"SafeArrayCreateVector"}]]
        SafeArrayCreateVectorFunction          safeArrayCreateVector = nullptr;
        [[= $::Syscall{"SafeArrayDestroy"}]]
        SafeArrayDestroyFunction               safeArrayDestroy = nullptr;
        [[= $::Syscall{"SafeArrayCopy"}]]
        SafeArrayCopyFunction                  safeArrayCopy = nullptr;
        [[= $::Syscall{"SafeArrayAccessData"}]]
        SafeArrayAccessDataFunction            safeArrayAccessData = nullptr;
        [[= $::Syscall{"SafeArrayUnaccessData"}]]
        SafeArrayUnaccessDataFunction          safeArrayUnaccessData = nullptr;
        [[= $::Syscall{"SafeArrayGetElement"}]]
        SafeArrayGetElementFunction            safeArrayGetElement = nullptr;
        [[= $::Syscall{"SafeArrayPutElement"}]]
        SafeArrayPutElementFunction            safeArrayPutElement = nullptr;
        [[= $::Syscall{"SafeArrayGetLBound"}]]
        SafeArrayGetBoundFunction              safeArrayGetLowerBound = nullptr;
        [[= $::Syscall{"SafeArrayGetUBound"}]]
        SafeArrayGetBoundFunction              safeArrayGetUpperBound = nullptr;
        [[= $::Syscall{"SafeArrayLock"}]]
        SafeArrayLockFunction                 safeArrayLock = nullptr;
        [[= $::Syscall{"SafeArrayUnlock"}]]
        SafeArrayUnlockFunction               safeArrayUnlock = nullptr;
        [[= $::Syscall{"SafeArrayGetDim"}]]
        SafeArrayGetDimensionFunction         safeArrayGetDimension = nullptr;
        [[= $::Syscall{"VariantInit"}]]
        VariantInitFunction                   variantInit = nullptr;
        [[= $::Syscall{"VariantClear"}]]
        VariantOperationFunction              variantClear = nullptr;
        [[= $::Syscall{"VariantCopy"}]]
        VariantCopyFunction                   variantCopy = nullptr;
        [[= $::Syscall{"VariantCopyInd"}]]
        VariantCopyIndFunction                variantCopyInd = nullptr;
        [[= $::Syscall{"VariantChangeType"}]]
        VariantChangeTypeFunction             variantChangeType = nullptr;
        [[= $::Syscall{"VariantChangeTypeEx"}]]
        VariantChangeTypeExFunction           variantChangeTypeEx = nullptr;
        [[= $::Syscall{"RegisterActiveObject"}]]
        RegisterActiveObjectFunction          registerActiveObject = nullptr;
        [[= $::Syscall{"RevokeActiveObject"}]]
        RevokeActiveObjectFunction            revokeActiveObject = nullptr;
        [[= $::Syscall{"GetActiveObject"}]]
        GetActiveObjectFunction               getActiveObject = nullptr;
        [[= $::Syscall{"LoadTypeLib"}]]
        LoadTypeLibraryFunction               loadTypeLibrary = nullptr;
        [[= $::Syscall{"LoadTypeLibEx"}]]
        LoadTypeLibraryExFunction             loadTypeLibraryEx = nullptr;
        [[= $::Syscall{"LoadRegTypeLib"}]]
        LoadRegisteredTypeLibraryFunction     loadRegisteredTypeLibrary = nullptr;
        [[= $::Syscall{"QueryPathOfRegTypeLib"}]]
        QueryTypeLibraryPathFunction          queryPathOfRegisteredTypeLibrary = nullptr;
        // clang-format on
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
        using LoadLibraryWideFunction        = WindowsSystem::Handle (*)(const wchar_t*) noexcept;
        using GetModuleHandleWideFunction    = WindowsSystem::Handle (*)(const wchar_t*) noexcept;
        using FindSymbolFunction             = void* (*)(WindowsSystem::Handle, const char*) noexcept;
        using FreeLibraryFunction            = WindowsSystem::Bool (*)(WindowsSystem::Handle) noexcept;

        [[= $::Syscall{"LoadLibraryW"}]]
        LoadLibraryWideFunction      loadLibraryWide     = nullptr;
        [[= $::Syscall{"GetModuleHandleW"}]]
        GetModuleHandleWideFunction  getModuleHandleWide = nullptr;
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

    /** @brief Dynamically resolved Win32 process creation, lifecycle, job, and scheduling operations. */
    struct ProcessControlSystemAPI {
        // clang-format off
        using OpenProcessFunction          = WindowsSystem::Handle(__stdcall*)(WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord);
        using CreateProcessWideFunction    = WindowsSystem::Bool(__stdcall*)(const wchar_t*, wchar_t*, ::_SECURITY_ATTRIBUTES*, ::_SECURITY_ATTRIBUTES*, WindowsSystem::Bool, WindowsSystem::DWord, void*, const wchar_t*, ::_STARTUPINFOW*, ::_PROCESS_INFORMATION*);
        using GetExitCodeFunction          = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord*);
        using TerminateFunction            = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::UInt);
        using CreateJobObjectWideFunction  = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*, const wchar_t*);
        using AssignJobFunction            = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle);
        using SetPriorityClassFunction     = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord);
        using SetAffinityMaskFunction      = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, uintptr_t);

        [[= $::Syscall{"OpenProcess"}]]
        OpenProcessFunction          openProcess          = nullptr;
        [[= $::Syscall{"CreateProcessW"}]]
        CreateProcessWideFunction    createProcessWide    = nullptr;
        [[= $::Syscall{"GetExitCodeProcess"}]]
        GetExitCodeFunction          getExitCode          = nullptr;
        [[= $::Syscall{"TerminateProcess"}]]
        TerminateFunction            terminate            = nullptr;
        [[= $::Syscall{"CreateJobObjectW"}]]
        CreateJobObjectWideFunction  createJobObjectWide  = nullptr;
        [[= $::Syscall{"AssignProcessToJobObject"}]]
        AssignJobFunction            assignProcessToJob   = nullptr;
        [[= $::Syscall{"SetPriorityClass"}]]
        SetPriorityClassFunction     setPriorityClass     = nullptr;
        [[= $::Syscall{"SetProcessAffinityMask"}]]
        SetAffinityMaskFunction      setProcessAffinityMask = nullptr;
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

    /** @brief Dynamically resolved Win32 thread scheduling and lifecycle operations. */
    struct ThreadControlSystemAPI {
        // clang-format off
        using SetPriorityFunction         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, int);
        using GetPriorityFunction         = int(__stdcall*)(WindowsSystem::Handle);
        using SetAffinityMaskFunction     = uintptr_t(__stdcall*)(WindowsSystem::Handle, uintptr_t);
        using SetIdealProcessorFunction   = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord);
        using SetIdealProcessorExFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_PROCESSOR_NUMBER*, ::_PROCESSOR_NUMBER*);
        using SuspendFunction             = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle);
        using ResumeFunction              = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle);

        [[= $::Syscall{"SetThreadPriority"}]]
        SetPriorityFunction         setPriority         = nullptr;
        [[= $::Syscall{"GetThreadPriority"}]]
        GetPriorityFunction         getPriority         = nullptr;
        [[= $::Syscall{"SetThreadAffinityMask"}]]
        SetAffinityMaskFunction     setAffinityMask     = nullptr;
        [[= $::Syscall{"SetThreadIdealProcessor"}]]
        SetIdealProcessorFunction   setIdealProcessor   = nullptr;
        [[= $::Syscall{"SetThreadIdealProcessorEx"}]]
        SetIdealProcessorExFunction setIdealProcessorEx = nullptr;
        [[= $::Syscall{"SuspendThread"}]]
        SuspendFunction             suspend             = nullptr;
        [[= $::Syscall{"ResumeThread"}]]
        ResumeFunction              resume              = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows CNG random-byte generation operation. */
    struct RandomSystemAPI {
        // clang-format off
        using GenerateFunction = WindowsSystem::Long(__stdcall*)(void*, unsigned char*, WindowsSystem::ULong, WindowsSystem::ULong);

        [[= $::Syscall{"BCryptGenRandom"}]]
        GenerateFunction generate = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Windows network-interface enumeration operations. */
    struct NetworkInterfaceSystemAPI {
        // clang-format off
        using GetAdaptersAddressesFunction = WindowsSystem::ULong(__stdcall*)(WindowsSystem::ULong, WindowsSystem::ULong, void*, ::_IP_ADAPTER_ADDRESSES_LH*, WindowsSystem::ULong*);
        using GetIfTable2Function          = WindowsSystem::ULong(__stdcall*)(::_MIB_IF_TABLE2**);
        using FreeMibTableFunction         = void(__stdcall*)(void*);
        using NotifyAddressChangeFunction  = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle*, ::_OVERLAPPED*);

        [[= $::Syscall{"GetAdaptersAddresses"}]]
        GetAdaptersAddressesFunction getAdaptersAddresses = nullptr;
        [[= $::Syscall{"GetIfTable2"}]]
        GetIfTable2Function          getIfTable2          = nullptr;
        [[= $::Syscall{"FreeMibTable"}]]
        FreeMibTableFunction         freeMibTable         = nullptr;
        [[= $::Syscall{"NotifyAddrChange"}]]
        NotifyAddressChangeFunction  notifyAddressChange  = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 directory, metadata, link, and volume operations. */
    struct ModernFileSystemAPI {
        // clang-format off
        using CreateDirectoryWideFunction          = WindowsSystem::Bool(__stdcall*)(const wchar_t*, ::_SECURITY_ATTRIBUTES*);
        using RemoveDirectoryWideFunction          = WindowsSystem::Bool(__stdcall*)(const wchar_t*);
        using FindFirstFileWideFunction            = WindowsSystem::Handle(__stdcall*)(const wchar_t*, ::_WIN32_FIND_DATAW*);
        using FindNextFileWideFunction             = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_WIN32_FIND_DATAW*);
        using FindCloseFunction                    = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using GetFileAttributesExWideFunction      = WindowsSystem::Bool(__stdcall*)(const wchar_t*, WindowsSystem::GetFileExInfoLevel, void*);
        using GetFinalPathNameByHandleWideFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle, wchar_t*, WindowsSystem::DWord, WindowsSystem::DWord);
        using CreateHardLinkWideFunction           = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*, ::_SECURITY_ATTRIBUTES*);
        using CreateSymbolicLinkWideFunction       = WindowsSystem::Byte(__stdcall*)(const wchar_t*, const wchar_t*, WindowsSystem::DWord);
        using GetVolumeInformationWideFunction     = WindowsSystem::Bool(__stdcall*)(const wchar_t*, wchar_t*, WindowsSystem::DWord, WindowsSystem::DWord*, WindowsSystem::DWord*, WindowsSystem::DWord*, wchar_t*, WindowsSystem::DWord);

        [[= $::Syscall{"CreateDirectoryW"}]]
        CreateDirectoryWideFunction          createDirectoryWide          = nullptr;
        [[= $::Syscall{"RemoveDirectoryW"}]]
        RemoveDirectoryWideFunction          removeDirectoryWide          = nullptr;
        [[= $::Syscall{"FindFirstFileW"}]]
        FindFirstFileWideFunction            findFirstFileWide            = nullptr;
        [[= $::Syscall{"FindNextFileW"}]]
        FindNextFileWideFunction             findNextFileWide             = nullptr;
        [[= $::Syscall{"FindClose"}]]
        FindCloseFunction                    findClose                    = nullptr;
        [[= $::Syscall{"GetFileAttributesExW"}]]
        GetFileAttributesExWideFunction      getFileAttributesExWide      = nullptr;
        [[= $::Syscall{"GetFinalPathNameByHandleW"}]]
        GetFinalPathNameByHandleWideFunction getFinalPathNameByHandleWide = nullptr;
        [[= $::Syscall{"CreateHardLinkW"}]]
        CreateHardLinkWideFunction           createHardLinkWide           = nullptr;
        [[= $::Syscall{"CreateSymbolicLinkW"}]]
        CreateSymbolicLinkWideFunction       createSymbolicLinkWide       = nullptr;
        [[= $::Syscall{"GetVolumeInformationW"}]]
        GetVolumeInformationWideFunction     getVolumeInformationWide     = nullptr;
        // clang-format on
    };

    /** @brief Linux-only advanced socket operations are unavailable on Win32. */
    struct AdvancedSocketSystemAPI {};
    /** @brief Linux-only fanotify operations are unavailable on Win32. */
    struct FanotifySystemAPI {};
    /** @brief Linux syscall adapters are unavailable on Win32. */
    struct LinuxSyscallSystemAPI {};
    /** @brief Linux performance-event adapters are unavailable on Win32. */
    struct PerfEventSystemAPI {};

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
        using EnumDisplayMonitorsCallback                   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::MonitorHandle, WindowsSystem::DeviceContextHandle, const WindowsSystem::Rect*, WindowsSystem::LParam);
        using EnumDisplayMonitorsFunction                   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::DeviceContextHandle, const WindowsSystem::Rect*, EnumDisplayMonitorsCallback, WindowsSystem::LParam);
        using GetMonitorInfoWideFunction                    = WindowsSystem::Bool(__stdcall*)(WindowsSystem::MonitorHandle, WindowsSystem::MonitorInfo*);
        using MonitorFromPointFunction                      = WindowsSystem::MonitorHandle(__stdcall*)(WindowsSystem::Point, WindowsSystem::DWord);
        using MonitorFromWindowFunction                     = WindowsSystem::MonitorHandle(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::DWord);
        using GetWindowRectFunction                         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::Rect*);
        using GetClientRectFunction                         = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::Rect*);
        using TranslatePointFunction                        = WindowsSystem::Bool(__stdcall*)(WindowsSystem::WindowHandle, WindowsSystem::Point*);
        using GetCursorPositionFunction                     = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Point*);
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
        using GetAwarenessFromDpiAwarenessContextFunction   = DisplayDpiAwareness(__stdcall*)(WindowsSystem::DpiAwarenessContext);
        using AdjustWindowRectExForDpiFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Rect*, WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord, WindowsSystem::UInt);
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

        /** @brief Native POSIX process identifier without including @c sys/types.h. */
        using ProcessId = int32_t;

        /** @brief Native POSIX wait-object identifier without including @c sys/types.h. */
        using WaitId = uint32_t;

        /** @brief Native POSIX wait-object category without including @c sys/wait.h. */
        using WaitIdType = int32_t;

        /** @brief Opaque POSIX spawn file-actions object owned by the caller. */
        struct SpawnFileActions;

        /** @brief Opaque POSIX spawn attributes object owned by the caller. */
        struct SpawnAttributes;

        /** @brief Opaque POSIX signal-information object owned by the caller or operating system. */
        struct SignalInfo;

        /** @brief Opaque POSIX signal-action object owned by the caller. */
        struct SignalAction;

        /** @brief Opaque POSIX signal set owned by the caller. */
        struct SignalSet;

        /** @brief ABI-sized POSIX thread identifier used without including @c pthread.h. */
        using ThreadId = std::conditional_t<Platform::kIsMacOS, void*, std::uintptr_t>;

        /** @brief Opaque POSIX thread-attribute object used by the dynamically loaded table. */
        struct ThreadAttributes;

        /** @brief POSIX file offset ABI type without including @c sys/types.h. */
        using FileOffset = std::int64_t;

        /** @brief POSIX creation-mode ABI type without including @c sys/types.h. */
        using FileMode = std::conditional_t<Platform::kIsMacOS, uint16_t, uint32_t>;

        /** @brief POSIX poll descriptor-count ABI type without including @c poll.h. */
        using PollCount = std::conditional_t<Platform::kIsMacOS, uint32_t, size_t>;
        using SocketLength = uint32_t;
        using ByteCount = std::ptrdiff_t;

        /** @brief Opaque Darwin process-accounting buffer pointer used by @c proc_pid_rusage. */
        using ResourceUsageInfo = void*;

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
#    if defined(__aarch64__)
             inline constexpr int kOpenDirectory          = 0x00004000;
             inline constexpr int kOpenDirect             = 0x00010000;
#    else
             inline constexpr int kOpenDirectory          = 0x00010000;
             inline constexpr int kOpenDirect             = 0x00004000;
#    endif
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

        /** @brief Native CPU-discovery selectors and masks consumed by upper-layer capability composition. */
        inline namespace CpuDiscovery {

#    if defined(PLATFORM_LINUX)
            inline constexpr unsigned long kHardwareCapabilities = 16;
            inline constexpr unsigned long kHardwareCapabilities2 = 26;
            inline constexpr int kPhysicalPageCountConfiguration = 85;
            inline constexpr int kAvailablePhysicalPageCountConfiguration = 86;
#        if defined(__aarch64__)
            inline constexpr unsigned long kArmFloatingPoint = 1UL << 0;
            inline constexpr unsigned long kArmAdvancedSimd = 1UL << 1;
            inline constexpr unsigned long kArmScalableVector = 1UL << 22;
            inline constexpr unsigned long kArmScalableVector2 = 1UL << 1;
            inline constexpr int kGetScalableVectorLength = 51;
            inline constexpr int kScalableVectorLengthMask = 0xFFFF;
#        elif defined(__riscv)
            inline constexpr unsigned long kRiscVDoublePrecision = 1UL << 3;
            inline constexpr unsigned long kRiscVSinglePrecision = 1UL << 5;
            inline constexpr unsigned long kRiscVVector = 1UL << 21;
#        endif
#    endif

        } // namespace CpuDiscovery

        inline namespace Debugging {

#    if defined(PLATFORM_LINUX)
            /** @brief Architecture-independent Linux @c ptrace request identifiers. */
            enum class TraceRequest : int {
                // clang-format off
                TraceMe              = 0,
                PeekText             = 1,
                PeekData             = 2,
                PeekUser             = 3,
                PokeText             = 4,
                PokeData             = 5,
                PokeUser             = 6,
                Continue             = 7,
                Kill                 = 8,
                SingleStep           = 9,
                Attach               = 16,
                Detach               = 17,
                SystemCall           = 24,
                SetOptions           = 0x4200,
                GetEventMessage      = 0x4201,
                GetSignalInfo        = 0x4202,
                SetSignalInfo        = 0x4203,
                GetRegisterSet       = 0x4204,
                SetRegisterSet       = 0x4205,
                Seize                = 0x4206,
                Interrupt            = 0x4207,
                Listen               = 0x4208,
                PeekSignalInfo       = 0x4209,
                GetSignalMask        = 0x420A,
                SetSignalMask        = 0x420B,
                GetSeccompFilter     = 0x420C,
                GetSeccompMetadata   = 0x420D,
                GetSystemCallInfo    = 0x420E,
                GetRseqConfiguration = 0x420F,
                SetSystemCallUserDispatchConfiguration = 0x4210,
                GetSystemCallUserDispatchConfiguration = 0x4211,
                SetSystemCallInfo = 0x4212,
                // clang-format on
            };

            // clang-format off
            inline constexpr uint32_t kTraceOptionSystemCallsGood = 0x00000001;
            inline constexpr uint32_t kTraceOptionFork            = 0x00000002;
            inline constexpr uint32_t kTraceOptionVFork           = 0x00000004;
            inline constexpr uint32_t kTraceOptionClone           = 0x00000008;
            inline constexpr uint32_t kTraceOptionExec            = 0x00000010;
            inline constexpr uint32_t kTraceOptionVForkDone       = 0x00000020;
            inline constexpr uint32_t kTraceOptionExit            = 0x00000040;
            inline constexpr uint32_t kTraceOptionSeccomp         = 0x00000080;
            inline constexpr uint32_t kTraceOptionExitKill        = 0x00100000;
            inline constexpr uint32_t kTraceOptionSuspendSeccomp  = 0x00200000;
            inline constexpr uint32_t kTraceOptionMask            = 0x003000FF;

            inline constexpr int kTraceEventFork      = 1;
            inline constexpr int kTraceEventVFork     = 2;
            inline constexpr int kTraceEventClone     = 3;
            inline constexpr int kTraceEventExec      = 4;
            inline constexpr int kTraceEventVForkDone = 5;
            inline constexpr int kTraceEventExit      = 6;
            inline constexpr int kTraceEventSeccomp   = 7;
            inline constexpr int kTraceEventStop      = 128;
            // clang-format on
#    endif

            /** @brief Signal number used by the native debugger breakpoint trap. */
            inline constexpr int kBreakpointSignal = 5;

        } // namespace Debugging

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

            inline constexpr int kDynamicLazy = 0x00000001;
            inline constexpr int kDynamicNow = 0x00000002;
#    if defined(PLATFORM_MACOS)
            inline constexpr int kDynamicLocal = 0x00000004;
            inline constexpr int kDynamicGlobal = 0x00000008;
#    else
            inline constexpr int kDynamicLocal = 0;
            inline constexpr int kDynamicGlobal = 0x00000100;
#    endif

        } // namespace Module

    } // namespace PosixSystem

    /** @brief Dynamically resolved POSIX socket-runtime, endpoint, option, and readiness operations. */
    struct SocketSystemAPI {
        // clang-format off
        using SocketFunction          = PosixSystem::Socket (*)(int, int, int);
        using CloseFunction           = int (*)(PosixSystem::Socket);
        using BindFunction            = int (*)(PosixSystem::Socket, const sockaddr*, PosixSystem::SocketLength);
        using NameFunction            = int (*)(PosixSystem::Socket, sockaddr*, PosixSystem::SocketLength*);
        using OptionFunction          = int (*)(PosixSystem::Socket, int, int, const void*, PosixSystem::SocketLength);
        using GetOptionFunction       = int (*)(PosixSystem::Socket, int, int, void*, PosixSystem::SocketLength*);
        using IoctlFunction           = int (*)(PosixSystem::Socket, unsigned long, ...);
        using PollFunction            = int (*)(PosixSystem::NativePollDescriptor*, PosixSystem::PollCount, int);
        using GetAddressInfoFunction  = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
        using FreeAddressInfoFunction = void (*)(addrinfo*);
        using GetNameInfoFunction     = int (*)(const sockaddr*, PosixSystem::SocketLength, char*, PosixSystem::SocketLength, char*, PosixSystem::SocketLength, int);

        [[= $::Syscall{"socket"}]]       
        SocketFunction          create          = nullptr;
        [[= $::Syscall{"close"}]]        
        CloseFunction           close           = nullptr;
        [[= $::Syscall{"bind"}]]         
        BindFunction            bind            = nullptr;
        [[= $::Syscall{"getsockname"}]]  
        NameFunction            getSocketName   = nullptr;
        [[= $::Syscall{"getpeername"}]]  
        NameFunction            getPeerName     = nullptr;
        [[= $::Syscall{"setsockopt"}]]   
        OptionFunction          setOption       = nullptr;
        [[= $::Syscall{"getsockopt"}]]   
        GetOptionFunction       getOption       = nullptr;
        [[= $::Syscall{"ioctl"}]]        
        IoctlFunction           ioctl           = nullptr;
        [[= $::Syscall{"poll"}]]         
        PollFunction            poll            = nullptr;
        [[= $::Syscall{"getaddrinfo"}]]  
        GetAddressInfoFunction  getAddressInfo  = nullptr;
        [[= $::Syscall{"freeaddrinfo"}]] 
        FreeAddressInfoFunction freeAddressInfo = nullptr;
        [[= $::Syscall{"getnameinfo"}]]  
        GetNameInfoFunction     getNameInfo     = nullptr;
        // clang-format on

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
        // clang-format off
        using ListenFunction   = int (*)(PosixSystem::Socket, int);
        using AcceptFunction   = PosixSystem::Socket (*)(PosixSystem::Socket, sockaddr*, PosixSystem::SocketLength*);
        using ConnectFunction  = int (*)(PosixSystem::Socket, const sockaddr*, PosixSystem::SocketLength);
        using TransferFunction = PosixSystem::ByteCount (*)(PosixSystem::Socket, void*, size_t, int);
        using SendFunction     = PosixSystem::ByteCount (*)(PosixSystem::Socket, const void*, size_t, int);
        using ShutdownFunction = int (*)(PosixSystem::Socket, int);

        [[= $::Syscall{"listen"}]]   
        ListenFunction   listen   = nullptr;
        [[= $::Syscall{"accept"}]]   
        AcceptFunction   accept   = nullptr;
        [[= $::Syscall{"connect"}]]  
        ConnectFunction  connect  = nullptr;
        [[= $::Syscall{"send"}]]     
        SendFunction     send     = nullptr;
        [[= $::Syscall{"recv"}]]     
        TransferFunction receive  = nullptr;
        [[= $::Syscall{"shutdown"}]] 
        ShutdownFunction shutdown = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX UDP datagram operations. */
    struct UdpSystemAPI {
        // clang-format off
        using SendToFunction      = PosixSystem::ByteCount (*)(PosixSystem::Socket, const void*, size_t, int,
                                                          const sockaddr*, PosixSystem::SocketLength);
        using ReceiveFromFunction = PosixSystem::ByteCount (*)(PosixSystem::Socket, void*, size_t, int, sockaddr*,
                                                               PosixSystem::SocketLength*);

        [[= $::Syscall{"sendto"}]]   
        SendToFunction      sendTo      = nullptr;
        [[= $::Syscall{"recvfrom"}]] 
        ReceiveFromFunction receiveFrom = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX monotonic-clock operations. */
    struct ClockSystemAPI {
        // clang-format off
        using GetTimeFunction       = int (*)(int, ::timespec*);
        using GetResolutionFunction = int (*)(int, ::timespec*);
        using SleepFunction         = int (*)(int, int, const ::timespec*, ::timespec*);

        [[= $::Syscall{"clock_gettime"}]]   
        GetTimeFunction       getTime       = nullptr;
        [[= $::Syscall{"clock_getres"}]]    
        GetResolutionFunction getResolution = nullptr;
        [[= $::Syscall{"clock_nanosleep"}]] 
        SleepFunction         sleep         = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Linux advanced socket-message and batch datagram operations. */
    struct AdvancedSocketSystemAPI {
        // clang-format off
        using Accept4Function         = PosixSystem::Socket (*)(PosixSystem::Socket, sockaddr*, PosixSystem::SocketLength*, int);
        using SendMessageFunction     = PosixSystem::ByteCount (*)(PosixSystem::Socket, const ::msghdr*, int);
        using ReceiveMessageFunction  = PosixSystem::ByteCount (*)(PosixSystem::Socket, ::msghdr*, int);
        using SendMessagesFunction    = int (*)(PosixSystem::Socket, ::mmsghdr*, unsigned int, int);
#    if defined(__x86_64__)
        using ReceiveMessagesFunction = int (*)(PosixSystem::Socket, ::mmsghdr*, unsigned int, int, const ::timespec*);
#    else
        using ReceiveMessagesFunction = int (*)(PosixSystem::Socket, ::mmsghdr*, unsigned int, int, ::timespec*);
#    endif

        [[= $::Syscall{"accept4"}]] 
        Accept4Function accept4 = nullptr;
        [[= $::Syscall{"sendmsg"}]] 
        SendMessageFunction sendMessage = nullptr;
        [[= $::Syscall{"recvmsg"}]] 
        ReceiveMessageFunction receiveMessage = nullptr;
        [[= $::Syscall{"sendmmsg"}]] 
        SendMessagesFunction sendMessages = nullptr;
        [[= $::Syscall{"recvmmsg"}]]
        ReceiveMessagesFunction receiveMessages = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX interface-name and address enumeration operations. */
    struct NetworkInterfaceSystemAPI {
        // clang-format off
        using GetIfAddressesFunction  = int (*)(::ifaddrs**);
        using FreeIfAddressesFunction = void (*)(::ifaddrs*);
        using IfNameToIndexFunction   = unsigned int (*)(const char*);
        using IfIndexToNameFunction   = char* (*)(unsigned int, char*);

        [[= $::Syscall{"getifaddrs"}]] 
        GetIfAddressesFunction getIfAddresses = nullptr;
        [[= $::Syscall{"freeifaddrs"}]] 
        FreeIfAddressesFunction freeIfAddresses = nullptr;
        [[= $::Syscall{"if_nametoindex"}]] 
        IfNameToIndexFunction ifNameToIndex = nullptr;
        [[= $::Syscall{"if_indextoname"}]] 
        IfIndexToNameFunction ifIndexToName = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX process creation, waiting, and signal-control operations. */
    struct ProcessControlSystemAPI {
        // clang-format off
        using ForkFunction                    = int (*)();
        using ExecFunction                    = int (*)(const char*, char* const[], char* const[]);
        using SpawnFunction                   = int (*)(PosixSystem::ProcessId*, const char*, const PosixSystem::SpawnFileActions*, const PosixSystem::SpawnAttributes*, char* const[], char* const[]);
        using WaitFunction                    = int (*)(PosixSystem::ProcessId, int*, int);
        using WaitIdFunction                  = int (*)(PosixSystem::WaitIdType, PosixSystem::WaitId, PosixSystem::SignalInfo*, int);
        using SignalFunction                  = int (*)(PosixSystem::ProcessId, int);
        using ThreadSignalFunction            = int (*)(PosixSystem::ProcessId, PosixSystem::ProcessId, int);
        using RaiseSignalFunction             = int (*)(int);
        using SignalActionFunction            = int (*)(int, const PosixSystem::SignalAction*, PosixSystem::SignalAction*);
        using ProcessDescriptorOpenFunction   = int (*)(PosixSystem::ProcessId, unsigned int);
        using ProcessDescriptorSignalFunction = int (*)(int, int, PosixSystem::SignalInfo*, unsigned int);

        [[= $::Syscall{"fork"}]] 
        ForkFunction fork = nullptr;
        [[= $::Syscall{"execve"}]] 
        ExecFunction exec = nullptr;
        [[= $::Syscall{"posix_spawn"}]] 
        SpawnFunction spawn = nullptr;
        [[= $::Syscall{"waitpid"}]] 
        WaitFunction wait = nullptr;
        [[= $::Syscall{"waitid"}]] 
        WaitIdFunction waitId = nullptr;
        [[= $::Syscall{"kill"}]] 
        SignalFunction kill = nullptr;
        [[= $::Syscall{"tgkill"}]] 
        ThreadSignalFunction threadKill = nullptr;
        [[= $::Syscall{"raise"}]]
        RaiseSignalFunction raiseSignal = nullptr;
        [[= $::Syscall{"sigaction"}]] 
        SignalActionFunction signalAction = nullptr;
        [[= $::Syscall{"pidfd_open"}]] 
        ProcessDescriptorOpenFunction processDescriptorOpen = nullptr;
        [[= $::Syscall{"pidfd_send_signal"}]] 
        ProcessDescriptorSignalFunction processDescriptorSendSignal = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX modern path-relative and zero-copy file operations. */
    struct ModernFileSystemAPI {
        // clang-format off
        using OpenAtFunction          = int (*)(int, const char*, int, ...);
        using OpenAt2Function         = int (*)(int, const char*, const void*, size_t);
        using MakeDirectoryAtFunction = int (*)(int, const char*, PosixSystem::FileMode);
        using UnlinkAtFunction        = int (*)(int, const char*, int);
        using RenameAtFunction        = int (*)(int, const char*, int, const char*);
        using ReadLinkAtFunction      = PosixSystem::ByteCount (*)(int, const char*, char*, size_t);
        using StatxFunction           = int (*)(int, const char*, int, unsigned int, void*);
        using CopyFileRangeFunction   = PosixSystem::ByteCount (*)(int, PosixSystem::FileOffset*, int, PosixSystem::FileOffset*, size_t, unsigned int);
        using SendFileFunction        = PosixSystem::ByteCount (*)(int, int, PosixSystem::FileOffset*, size_t);
        using AllocateFunction        = int (*)(int, int, PosixSystem::FileOffset, PosixSystem::FileOffset);
        using SyncFileSystemFunction  = int (*)(int);
        using MemoryFileFunction      = int (*)(const char*, unsigned int);

        [[= $::Syscall{"openat"}]] 
        OpenAtFunction openAt = nullptr;
        /** @brief Invoke the Linux @c openat2 syscall through the generic libc syscall transport. */
        [[= $::Syscall{"openat2"}]] 
        OpenAt2Function openAt2 = nullptr;
        [[= $::Syscall{"mkdirat"}]] 
        MakeDirectoryAtFunction makeDirectoryAt = nullptr;
        [[= $::Syscall{"unlinkat"}]] 
        UnlinkAtFunction unlinkAt = nullptr;
        [[= $::Syscall{"renameat"}]] 
        RenameAtFunction renameAt = nullptr;
        [[= $::Syscall{"readlinkat"}]] 
        ReadLinkAtFunction readLinkAt = nullptr;
        [[= $::Syscall{"statx"}]] 
        StatxFunction statx = nullptr;
        [[= $::Syscall{"copy_file_range"}]] 
        CopyFileRangeFunction copyFileRange = nullptr;
        [[= $::Syscall{"sendfile"}]] 
        SendFileFunction sendFile = nullptr;
        [[= $::Syscall{"fallocate"}]] 
        AllocateFunction allocate = nullptr;
        [[= $::Syscall{"syncfs"}]] 
        SyncFileSystemFunction syncFileSystem = nullptr;
        [[= $::Syscall{"memfd_create"}]] 
        MemoryFileFunction memoryFile = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX random-byte generation operation. */
    struct RandomSystemAPI {
        // clang-format off
        using GetRandomFunction = PosixSystem::ByteCount (*)(void*, size_t, unsigned int);

        [[= $::Syscall{"getrandom"}]] 
        GetRandomFunction getRandom = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved POSIX virtual-memory protection and pinning operations. */
    struct VirtualMemorySystemAPI {
        // clang-format off
        using ProtectFunction = int (*)(void*, size_t, int);
        using RemapFunction   = void* (*)(void*, size_t, size_t, int, ...);
        using LockFunction    = int (*)(const void*, size_t);
        using UnlockFunction  = int (*)(const void*, size_t);

        [[= $::Syscall{"mprotect"}]] 
        ProtectFunction protect = nullptr;
        [[= $::Syscall{"mremap"}]] 
        RemapFunction remap = nullptr;
        [[= $::Syscall{"mlock"}]] 
        LockFunction lock = nullptr;
        [[= $::Syscall{"munlock"}]] 
        UnlockFunction unlock = nullptr;
        // clang-format on
    };

#    if defined(PLATFORM_LINUX)

    /** @brief Dynamically resolved Linux file-notification operations. */
    struct FanotifySystemAPI {
        // clang-format off
        using InitializeFunction = int (*)(unsigned int, unsigned int);
        using MarkFunction       = int (*)(int, unsigned int, uint64_t, int, const char*);

        [[= $::Syscall{"fanotify_init"}]] 
        InitializeFunction initialize = nullptr;
        [[= $::Syscall{"fanotify_mark"}]] 
        MarkFunction mark = nullptr;
        // clang-format on
    };

    /** @brief Explicit Linux syscall entry used by syscall-only facilities such as perf events. */
    struct LinuxSyscallSystemAPI {
        // clang-format off
        using SyscallFunction = long (*)(long, ...);

        [[= $::Syscall{"syscall"}]] 
        SyscallFunction syscall = nullptr;
        // clang-format on
    };

    /** @brief Syscall adapter boundary for Linux performance-counter creation. */
    struct PerfEventSystemAPI {
        using OpenFunction = long (*)(void*, int, int, int, unsigned long);

        [[= $::Syscall{"perf_event_open"}]] 
        OpenFunction open = nullptr;
    };

    /** @brief Dynamically resolved Linux epoll readiness operations. */
    struct EpollSystemAPI {
        // clang-format off
        using CreateFunction         = int (*)(int);
        using ControlFunction        = int (*)(int, int, int, ::epoll_event*);
        using WaitFunction           = int (*)(int, ::epoll_event*, int, int);
        using WaitMaskedFunction     = int (*)(int, ::epoll_event*, int, int, const PosixSystem::SignalSet*);
        using WaitMaskedTimeFunction = int (*)(int, ::epoll_event*, int, const ::timespec*, const PosixSystem::SignalSet*);

        [[= $::Syscall{"epoll_create1"}]] 
        CreateFunction         create         = nullptr;
        [[= $::Syscall{"epoll_ctl"}]]     
        ControlFunction        control        = nullptr;
        [[= $::Syscall{"epoll_wait"}]]    
        WaitFunction           wait           = nullptr;
        [[= $::Syscall{"epoll_pwait"}]]   
        WaitMaskedFunction     waitMasked     = nullptr;
        [[= $::Syscall{"epoll_pwait2"}]]  
        WaitMaskedTimeFunction waitMaskedTime = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Linux event-counter descriptor creation. */
    struct EventFdSystemAPI {
#    if defined(__x86_64__)
        using CreateFunction = int (*)(int, int);
#    else
        using CreateFunction = int (*)(unsigned int, int);
#    endif
        [[= $::Syscall{"eventfd"}]] 
        CreateFunction create = nullptr;
    };

    /** @brief Dynamically resolved Linux timer-descriptor operations. */
    struct TimerFdSystemAPI {
        // clang-format off
        using CreateFunction  = int (*)(int, int);
        using SetTimeFunction = int (*)(int, int, const ::itimerspec*, ::itimerspec*);
        using GetTimeFunction = int (*)(int, ::itimerspec*);

        [[= $::Syscall{"timerfd_create"}]]  
        CreateFunction  create  = nullptr;
        [[= $::Syscall{"timerfd_settime"}]] 
        SetTimeFunction setTime = nullptr;
        [[= $::Syscall{"timerfd_gettime"}]] 
        GetTimeFunction getTime = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Linux signal-descriptor creation. */
    struct SignalFdSystemAPI {
        using CreateFunction = int (*)(int, const PosixSystem::SignalSet*, int);

        [[= $::Syscall{"signalfd"}]] CreateFunction create = nullptr;
    };

#    else

    /** @brief Linux fanotify is unavailable on non-Linux POSIX targets. */ 
    struct FanotifySystemAPI {};
    /** @brief Linux syscall adapters are unavailable on non-Linux POSIX targets. */ 
    struct LinuxSyscallSystemAPI {};
    /** @brief Linux perf-event adapters are unavailable on non-Linux POSIX targets. */ 
    struct PerfEventSystemAPI {};
    /** @brief Linux epoll adapters are unavailable on non-Linux POSIX targets. */ 
    struct EpollSystemAPI {};
    /** @brief Linux eventfd adapters are unavailable on non-Linux POSIX targets. */ 
    struct EventFdSystemAPI {};
    /** @brief Linux timerfd adapters are unavailable on non-Linux POSIX targets. */ 
    struct TimerFdSystemAPI {};
    /** @brief Linux signalfd adapters are unavailable on non-Linux POSIX targets. */ 
    struct SignalFdSystemAPI {};

#    endif

    /** @brief POSIX has no additional native-error entry points beyond thread-local @c errno. */
    struct NativeErrorSystemAPI {};
    /** @brief Structured exception entry points are unavailable on POSIX targets. */
    struct StructuredExceptionSystemAPI {};
    /** @brief Win32 local-memory entry points are unavailable on POSIX targets. */ 
    struct LocalMemorySystemAPI {};
    /** @brief Win32 handle entry points are unavailable on POSIX targets. */ 
    struct HandleSystemAPI {};
    /** @brief Win32 synchronization entry points are unavailable on POSIX targets. */
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
        // clang-format off
        using SystemConfigurationFunction  = long (*)(int);

        [[= $::Syscall{"sysconf"}]]
        SystemConfigurationFunction systemConfiguration = nullptr;

#    if defined(PLATFORM_LINUX)
        using AuxiliaryValueFunction        = unsigned long (*)(unsigned long);
        using ProcessControlFunction        = int (*)(int, ...);
        using FileSystemStatisticsFunction  = int (*)(const char*, ::statvfs*);

        [[= $::Syscall{"getauxval"}]]
        AuxiliaryValueFunction        auxiliaryValue       = nullptr;
        [[= $::Syscall{"prctl"}]]
        ProcessControlFunction        processControl       = nullptr;
        [[= $::Syscall{"statvfs"}]]
        FileSystemStatisticsFunction  fileSystemStatistics = nullptr;
#    elif defined(PLATFORM_MACOS)
        using SysctlByNameFunction = int (*)(const char*, void*, size_t*, void*, size_t);

        [[= $::Syscall{"sysctlbyname"}]]
        SysctlByNameFunction sysctlByName = nullptr;
#    endif
        // clang-format on
    };

    /** @brief Indivisible POSIX debugger and process-tracing entry points. */
    struct DebugSystemAPI {
#    if defined(PLATFORM_LINUX)
        using TraceProcessFunction = long (*)(PosixSystem::TraceRequest, ...);
#    endif
#    if defined(PLATFORM_LINUX)
        [[= $::Syscall{"ptrace"}]] 
        TraceProcessFunction traceProcess = nullptr;
#    endif
    };

    /** @brief Windows COM entry points are unavailable on POSIX targets. */
    struct COMSystemAPI {};

    /** @brief Windows OLE entry points are unavailable on POSIX targets. */
    struct OleSystemAPI {};

    /** @brief Windows OLE Automation entry points are unavailable on POSIX targets. */
    struct OleAutomationSystemAPI {};

    /** @brief Win32 event objects are unavailable on POSIX targets. */
    struct EventSystemAPI {};

    /** @brief Win32 I/O completion ports are unavailable on POSIX targets. */
    struct IOCPSystemAPI {};

    /** @brief Dynamically resolved liburing exports; SQE preparation remains a typed header adapter. */
    struct LinuxIoUringSystemAPI {
#    if defined(PLATFORM_LINUX)
        // clang-format off
        using QueueInitFunction            = int (*)(unsigned, ::io_uring*, unsigned);
        using QueueExitFunction            = void (*)(::io_uring*);
        using SubmitFunction               = int (*)(::io_uring*);
        using SubmitAndWaitFunction        = int (*)(::io_uring*, unsigned);
        using SubmitAndWaitTimeoutFunction = int (*)(::io_uring*, ::io_uring_cqe**, unsigned, ::__kernel_timespec*, PosixSystem::SignalSet*);
        using PeekBatchCompletionFunction  = unsigned (*)(::io_uring*, ::io_uring_cqe**, unsigned);
        using WaitCompletionsFunction      = int (*)(::io_uring*, ::io_uring_cqe**, unsigned, ::__kernel_timespec*, PosixSystem::SignalSet*);
        using RegisterBuffersFunction      = int (*)(::io_uring*, const void*, unsigned);
        using UnregisterBuffersFunction    = int (*)(::io_uring*);
        using RegisterFilesFunction        = int (*)(::io_uring*, const int*, unsigned);
        using UnregisterFilesFunction      = int (*)(::io_uring*);
        using RegisterEventFunction        = int (*)(::io_uring*, int);
        using UnregisterEventFunction      = int (*)(::io_uring*);

        [[= $::Syscall{"io_uring_queue_init"}]]
        QueueInitFunction            queueInit            = nullptr;
        [[= $::Syscall{"io_uring_queue_exit"}]]
        QueueExitFunction            queueExit            = nullptr;
        [[= $::Syscall{"io_uring_submit"}]]
        SubmitFunction               submit               = nullptr;
        [[= $::Syscall{"io_uring_submit_and_wait"}]]
        SubmitAndWaitFunction        submitAndWait        = nullptr;
        [[= $::Syscall{"io_uring_submit_and_wait_timeout"}]]
        SubmitAndWaitTimeoutFunction submitAndWaitTimeout = nullptr;
        [[= $::Syscall{"io_uring_peek_batch_cqe"}]]
        PeekBatchCompletionFunction  peekBatchCompletion  = nullptr;
        [[= $::Syscall{"io_uring_wait_cqes"}]]
        WaitCompletionsFunction      waitCompletions      = nullptr;
        [[= $::Syscall{"io_uring_register_buffers"}]]
        RegisterBuffersFunction      registerBuffers      = nullptr;
        [[= $::Syscall{"io_uring_unregister_buffers"}]]
        UnregisterBuffersFunction    unregisterBuffers    = nullptr;
        [[= $::Syscall{"io_uring_register_files"}]]
        RegisterFilesFunction        registerFiles        = nullptr;
        [[= $::Syscall{"io_uring_unregister_files"}]]
        UnregisterFilesFunction      unregisterFiles      = nullptr;
        [[= $::Syscall{"io_uring_register_eventfd"}]]
        RegisterEventFunction        registerEvent        = nullptr;
        [[= $::Syscall{"io_uring_unregister_eventfd"}]]
        UnregisterEventFunction      unregisterEvent      = nullptr;
        // clang-format on
#    endif
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
        using ProcessResourceUsageFunction   = int (*)(int, int, PosixSystem::ResourceUsageInfo);
        
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

    /** @brief POSIX thread-control extensions are intentionally owned by the thread runtime. */
    struct ThreadControlSystemAPI {};

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

    /** @brief Socket entry points are unavailable on this target. */ 
    struct SocketSystemAPI {};
    /** @brief TCP entry points are unavailable on this target. */ 
    struct TcpSystemAPI {};
    /** @brief UDP entry points are unavailable on this target. */ 
    struct UdpSystemAPI {};
    /** @brief Advanced socket entry points are unavailable on this target. */ 
    struct AdvancedSocketSystemAPI {};
    /** @brief Network-interface entry points are unavailable on this target. */ 
    struct NetworkInterfaceSystemAPI {};
    /** @brief Winsock overlapped entry points are unavailable on this target. */ 
    struct WinsockOverlappedSystemAPI {};
    /** @brief Clock entry points are unavailable on this target. */ 
    struct ClockSystemAPI {};
    /** @brief epoll entry points are unavailable on this target. */ 
    struct EpollSystemAPI {};
    /** @brief eventfd entry points are unavailable on this target. */ 
    struct EventFdSystemAPI {};
    /** @brief timerfd entry points are unavailable on this target. */ 
    struct TimerFdSystemAPI {};
    /** @brief signalfd entry points are unavailable on this target. */ 
    struct SignalFdSystemAPI {};
    /** @brief Windows I/O ring entry points are unavailable on this target. */ 
    struct WindowsIORingSystemAPI {};
    /** @brief Linux io_uring entry points are unavailable on this target. */ 
    struct LinuxIoUringSystemAPI {};
    /** @brief Native-error entry points are unavailable on this target. */ 
    struct NativeErrorSystemAPI {};
    /** @brief Structured exception entry points are unavailable on this target. */
    struct StructuredExceptionSystemAPI {};
    /** @brief Local-memory entry points are unavailable on this target. */ 
    struct LocalMemorySystemAPI {};
    /** @brief Native handle entry points are unavailable on this target. */ 
    struct HandleSystemAPI {};
    /** @brief Synchronization entry points are unavailable on this target. */ 
    struct SynchronizationSystemAPI {};
    /** @brief Virtual-memory entry points are unavailable on this target. */ 
    struct VirtualMemorySystemAPI {};
    /** @brief File-mapping entry points are unavailable on this target. */ 
    struct FileMappingSystemAPI {};
    /** @brief Event entry points are unavailable on this target. */ 
    struct EventSystemAPI {};
    /** @brief I/O completion-port entry points are unavailable on this target. */ 
    struct IOCPSystemAPI {};
    /** @brief System-information entry points are unavailable on this target. */ 
    struct SystemInformationSystemAPI {};
    /** @brief Debugger system calls are unavailable on this target. */
    struct DebugSystemAPI {};
    /** @brief COM entry points are unavailable on this target. */
    struct COMSystemAPI {};
    /** @brief OLE entry points are unavailable on this target. */
    struct OleSystemAPI {};
    /** @brief OLE Automation entry points are unavailable on this target. */
    struct OleAutomationSystemAPI {};
    /** @brief Environment entry points are unavailable on this target. */
    struct EnvironmentSystemAPI {};
    /** @brief Module entry points are unavailable on this target. */ 
    struct ModuleSystemAPI {};
    /** @brief Process entry points are unavailable on this target. */ 
    struct ProcessSystemAPI {};
    /** @brief Process-control entry points are unavailable on this target. */ 
    struct ProcessControlSystemAPI {};
    /** @brief Thread entry points are unavailable on this target. */ 
    struct ThreadSystemAPI {};
    /** @brief Thread-control entry points are unavailable on this target. */ 
    struct ThreadControlSystemAPI {};
    /** @brief Random-byte entry points are unavailable on this target. */ 
    struct RandomSystemAPI {};
    /** @brief Global-memory entry points are unavailable on this target. */ 
    struct GlobalMemorySystemAPI {};
    /** @brief Clipboard entry points are unavailable on this target. */ 
    struct ClipboardSystemAPI {};
    /** @brief Display entry points are unavailable on this target. */ 
    struct DisplaySystemAPI {};
    /** @brief File-system entry points are unavailable on this target. */ 
    struct FileSystemAPI {};
    /** @brief Modern file-system entry points are unavailable on this target. */ 
    struct ModernFileSystemAPI {};
    /** @brief fanotify entry points are unavailable on this target. */ 
    struct FanotifySystemAPI {};
    /** @brief Linux syscall adapters are unavailable on this target. */ 
    struct LinuxSyscallSystemAPI {};
    /** @brief perf-event adapters are unavailable on this target. */ 
    struct PerfEventSystemAPI {};
    /** @brief DbgHelp entry points are unavailable on this target. */ 
    struct DbgHelpSystemAPI {};
    /** @brief Stack-trace entry points are unavailable on this target. */ 
    struct StackTraceSystemAPI {};

#endif

    /** @brief Native system error type depending on the platform. */
#if defined(PLATFORM_WINDOWS)
    using SystemError = WindowsSystem::DWord;
#else
    using SystemError = int;
#endif

    /** @brief Capture the calling thread's native error slot without triggering lazy symbol resolution. */
    [[nodiscard]] SystemError CaptureLastSystemError() noexcept;

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

        /** @brief Load and return the immutable native virtual-memory function table. */
        [[nodiscard]] const VirtualMemorySystemAPI& LoadVirtualMemorySystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 file-mapping function table. */
        [[nodiscard]] const FileMappingSystemAPI& LoadFileMappingSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 event function table. */
        [[nodiscard]] const EventSystemAPI& LoadEventSystemAPI() noexcept;

        /** @brief Load and return the immutable Win32 I/O completion port function table. */
        [[nodiscard]] const IOCPSystemAPI& LoadIOCPSystemAPI() noexcept;

#if defined(PLATFORM_WINDOWS)
        /** @brief Load and return the immutable Windows I/O ring function table. */
        [[nodiscard]] const WindowsIORingSystemAPI& LoadWindowsIORingSystemAPI() noexcept;
#elif defined(PLATFORM_LINUX)
        /** @brief Load and return the immutable liburing export table. */
        [[nodiscard]] const LinuxIoUringSystemAPI& LoadLinuxIoUringSystemAPI() noexcept;
#endif

        /** @brief Load and return the immutable system-information function table. */
        [[nodiscard]] const SystemInformationSystemAPI& LoadSystemInformationSystemAPI() noexcept;

        /** @brief Load and return the immutable native debugger entry-point table. */
        [[nodiscard]] const DebugSystemAPI& LoadDebugSystemAPI() noexcept;

        /** @brief Load and return the immutable COM apartment and activation function table. */
        [[nodiscard]] const COMSystemAPI& LoadCOMSystemAPI() noexcept;

        /** @brief Load and return the immutable OLE object-model function table. */
        [[nodiscard]] const OleSystemAPI& LoadOleSystemAPI() noexcept;

        /** @brief Load and return the immutable OLE Automation function table. */
        [[nodiscard]] const OleAutomationSystemAPI& LoadOleAutomationSystemAPI() noexcept;

        /** @brief Load and return the immutable process-environment function table. */
        [[nodiscard]] const EnvironmentSystemAPI& LoadEnvironmentSystemAPI() noexcept;

        /** @brief Load and return the immutable normalized dynamic-module function table. */
        [[nodiscard]] const ModuleSystemAPI& LoadModuleSystemAPI() noexcept;

        /** @brief Load and return the immutable current-process introspection function table. */
        [[nodiscard]] const ProcessSystemAPI& LoadProcessSystemAPI() noexcept;

        /** @brief Load and return the immutable process-control function table. */
        [[nodiscard]] const ProcessControlSystemAPI& LoadProcessControlSystemAPI() noexcept;

        /** @brief Load and return the immutable current-thread function table. */
        [[nodiscard]] const ThreadSystemAPI& LoadThreadSystemAPI() noexcept;

        /** @brief Load and return the immutable thread-control function table. */
        [[nodiscard]] const ThreadControlSystemAPI& LoadThreadControlSystemAPI() noexcept;

        /** @brief Load and return the immutable cryptographic random-byte function table. */
        [[nodiscard]] const RandomSystemAPI& LoadRandomSystemAPI() noexcept;

        /** @brief Load and return the immutable native scheduler-tracing function table. */
        [[nodiscard]] const SchedulerTraceSystemAPI& LoadSchedulerTraceSystemAPI() noexcept;

        /** @brief Load and return the immutable socket-runtime function table. */
        [[nodiscard]] const SocketSystemAPI& LoadSocketSystemAPI() noexcept;

        /** @brief Load and return the immutable TCP stream function table. */
        [[nodiscard]] const TcpSystemAPI& LoadTcpSystemAPI() noexcept;

        /** @brief Load and return the immutable UDP datagram function table. */
        [[nodiscard]] const UdpSystemAPI& LoadUdpSystemAPI() noexcept;

        /** @brief Load and return the immutable native clock function table. */
        [[nodiscard]] const ClockSystemAPI& LoadClockSystemAPI() noexcept;

        /** @brief Load and return the immutable advanced socket-message function table. */
        [[nodiscard]] const AdvancedSocketSystemAPI& LoadAdvancedSocketSystemAPI() noexcept;

        /** @brief Load and return the immutable network-interface function table. */
        [[nodiscard]] const NetworkInterfaceSystemAPI& LoadNetworkInterfaceSystemAPI() noexcept;

#if defined(PLATFORM_WINDOWS)
        /** @brief Load and return the immutable overlapped Winsock function table. */
        [[nodiscard]] const WinsockOverlappedSystemAPI& LoadWinsockOverlappedSystemAPI() noexcept;
#elif defined(PLATFORM_LINUX)
        /** @brief Load and return the immutable Linux epoll function table. */
        [[nodiscard]] const EpollSystemAPI& LoadEpollSystemAPI() noexcept;

        /** @brief Load and return the immutable Linux eventfd function table. */
        [[nodiscard]] const EventFdSystemAPI& LoadEventFdSystemAPI() noexcept;

        /** @brief Load and return the immutable Linux timerfd function table. */
        [[nodiscard]] const TimerFdSystemAPI& LoadTimerFdSystemAPI() noexcept;

        /** @brief Load and return the immutable Linux signalfd function table. */
        [[nodiscard]] const SignalFdSystemAPI& LoadSignalFdSystemAPI() noexcept;
#endif

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

        /** @brief Load and return the immutable modern file-system function table. */
        [[nodiscard]] const ModernFileSystemAPI& LoadModernFileSystemAPI() noexcept;

        /** @brief Load and return the immutable Linux fanotify function table. */
        [[nodiscard]] const FanotifySystemAPI& LoadFanotifySystemAPI() noexcept;

        /** @brief Load and return the immutable Linux syscall entry table. */
        [[nodiscard]] const LinuxSyscallSystemAPI& LoadLinuxSyscallSystemAPI() noexcept;

        /** @brief Load and return the immutable Linux performance-event syscall adapter table. */
        [[nodiscard]] const PerfEventSystemAPI& LoadPerfEventSystemAPI() noexcept;

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
