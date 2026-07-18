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
 * @note @ref Module owns the native dynamic-loader bootstrap and typed symbol lookup. Loader bootstrap primitives and
 * immediate last-error capture are the only system entry points called directly because they cannot resolve
 * themselves, and symbol resolution can overwrite the error state that must be captured.
 */
#pragma once

#include <Sora/Platform.h>

#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <type_traits>

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
struct kevent;
struct pollfd;
struct sigaction;
struct stat;
struct timespec;
#endif

#if defined(PLATFORM_WINDOWS)
struct _IMAGEHLP_LINE64;
struct _IMAGEHLP_MODULE64;
struct _EXCEPTION_POINTERS;
union _LARGE_INTEGER;
struct _OVERLAPPED;
struct _SECURITY_ATTRIBUTES;
struct _SYMBOL_INFO;
#endif

namespace Sora::PAL {

    /** @brief Capture the calling thread's native error slot without triggering lazy symbol resolution. */
    [[nodiscard]] int CaptureLastSystemError() noexcept;

#if defined(PLATFORM_WINDOWS)

    namespace WindowsSystem {

        using Bool = int;
        using DWord = unsigned long;
        using DWord64 = unsigned long long;
        using UInt = unsigned int;
        using HResult = long;
        using Size = uintptr_t;
        using Handle = void*;
        using GlobalMemory = void*;

        /** @brief ABI-compatible representation of Win32 @c PROCESSOR_NUMBER. */
        struct ProcessorNumber {
            uint16_t group = 0;
            uint8_t number = 0;
            uint8_t reserved = 0;
        };

        inline constexpr Bool kFalse = 0;
        inline constexpr DWord kErrorSuccess = 0;
        inline constexpr DWord kErrorEnvironmentVariableNotFound = 203;
        inline constexpr DWord kFormatMessageAllocateBuffer = 0x00000100;
        inline constexpr DWord kFormatMessageIgnoreInserts = 0x00000200;
        inline constexpr DWord kFormatMessageFromSystem = 0x00001000;
        inline constexpr DWord kGenericWrite = 0x40000000;
        inline constexpr DWord kGenericRead = 0x80000000;
        inline constexpr DWord kFileShareRead = 0x00000001;
        inline constexpr DWord kFileShareWrite = 0x00000002;
        inline constexpr DWord kFileShareDelete = 0x00000004;
        inline constexpr DWord kCreateNew = 1;
        inline constexpr DWord kCreateAlways = 2;
        inline constexpr DWord kOpenExisting = 3;
        inline constexpr DWord kOpenAlways = 4;
        inline constexpr DWord kTruncateExisting = 5;
        inline constexpr DWord kFileAttributeNormal = 0x00000080;
        inline constexpr DWord kFileFlagWriteThrough = 0x80000000;
        inline constexpr DWord kFileFlagOverlapped = 0x40000000;
        inline constexpr DWord kFileFlagNoBuffering = 0x20000000;
        inline constexpr DWord kFileFlagRandomAccess = 0x10000000;
        inline constexpr DWord kFileFlagSequentialScan = 0x08000000;
        inline constexpr DWord kFileFlagDeleteOnClose = 0x04000000;
        inline constexpr DWord kFileFlagBackupSemantics = 0x02000000;
        inline constexpr DWord kStandardErrorHandle = static_cast<DWord>(-12);
        inline constexpr UInt kMovableGlobalMemory = 0x0002;
        inline constexpr UInt kUnicodeTextClipboardFormat = 13;

        /** @brief Return whether @p result represents a successful Win32 @c HRESULT. */
        [[nodiscard]] constexpr bool Succeeded(HResult result) noexcept {
            return result >= 0;
        }

    } // namespace WindowsSystem

    /** @brief Dynamically resolved Win32 error-reporting entry points. */
    struct NativeErrorSystemAPI {
        using GetLastErrorFunction = WindowsSystem::DWord(__stdcall*)();
        using SetLastErrorFunction = void(__stdcall*)(WindowsSystem::DWord);
        using FormatMessageWideFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord, const void*,
                                                                           WindowsSystem::DWord, WindowsSystem::DWord,
                                                                           wchar_t*, WindowsSystem::DWord,
                                                                           std::va_list*);
        using LocalFreeFunction = void*(__stdcall*)(void*);

        GetLastErrorFunction getLastError = nullptr;
        SetLastErrorFunction setLastError = nullptr;
        FormatMessageWideFunction formatMessageWide = nullptr;
        LocalFreeFunction localFree = nullptr;
    };

    /** @brief Dynamically resolved Win32 environment entry points. */
    struct EnvironmentSystemAPI {
        using GetLastErrorFunction = WindowsSystem::DWord(__stdcall*)();
        using SetLastErrorFunction = void(__stdcall*)(WindowsSystem::DWord);
        using GetEnvironmentVariableWideFunction = WindowsSystem::DWord(__stdcall*)(const wchar_t*, wchar_t*,
                                                                                    WindowsSystem::DWord);
        using SetEnvironmentVariableWideFunction = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*);
        using GetEnvironmentStringsWideFunction = wchar_t*(__stdcall*)();
        using FreeEnvironmentStringsWideFunction = WindowsSystem::Bool(__stdcall*)(wchar_t*);

        GetLastErrorFunction getLastError = nullptr;
        SetLastErrorFunction setLastError = nullptr;
        GetEnvironmentVariableWideFunction getEnvironmentVariableWide = nullptr;
        SetEnvironmentVariableWideFunction setEnvironmentVariableWide = nullptr;
        GetEnvironmentStringsWideFunction getEnvironmentStringsWide = nullptr;
        FreeEnvironmentStringsWideFunction freeEnvironmentStringsWide = nullptr;
    };

    /** @brief Dynamically resolved Win32 current-thread entry points. */
    struct ThreadSystemAPI {
        using GetCurrentThreadIdFunction = WindowsSystem::DWord(__stdcall*)();
        using GetCurrentThreadFunction = WindowsSystem::Handle(__stdcall*)();
        using SetThreadDescriptionFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, const wchar_t*);
        using GetThreadDescriptionFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, wchar_t**);
        using LocalFreeFunction = void*(__stdcall*)(void*);
        using GetCurrentProcessorNumberExFunction = void(__stdcall*)(WindowsSystem::ProcessorNumber*);
        using GetCurrentThreadStackLimitsFunction = void(__stdcall*)(uintptr_t*, uintptr_t*);

        GetCurrentThreadIdFunction getCurrentThreadId = nullptr;
        GetCurrentThreadFunction getCurrentThread = nullptr;
        SetThreadDescriptionFunction setThreadDescription = nullptr;
        GetThreadDescriptionFunction getThreadDescription = nullptr;
        LocalFreeFunction localFree = nullptr;
        GetCurrentProcessorNumberExFunction getCurrentProcessorNumberEx = nullptr;
        GetCurrentThreadStackLimitsFunction getCurrentThreadStackLimits = nullptr;
    };

    /** @brief Dynamically resolved Win32 movable-global-memory entry points. */
    struct GlobalMemorySystemAPI {
        using GlobalAllocateFunction = WindowsSystem::GlobalMemory(__stdcall*)(WindowsSystem::UInt,
                                                                               WindowsSystem::Size);
        using GlobalFreeFunction = WindowsSystem::GlobalMemory(__stdcall*)(WindowsSystem::GlobalMemory);
        using GlobalLockFunction = void*(__stdcall*)(WindowsSystem::GlobalMemory);
        using GlobalUnlockFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::GlobalMemory);
        using GlobalSizeFunction = WindowsSystem::Size(__stdcall*)(WindowsSystem::GlobalMemory);

        GlobalAllocateFunction globalAllocate = nullptr;
        GlobalFreeFunction globalFree = nullptr;
        GlobalLockFunction globalLock = nullptr;
        GlobalUnlockFunction globalUnlock = nullptr;
        GlobalSizeFunction globalSize = nullptr;
    };

    /** @brief Dynamically resolved Win32 clipboard entry points. */
    struct ClipboardSystemAPI {
        using OpenClipboardFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using CloseClipboardFunction = WindowsSystem::Bool(__stdcall*)();
        using EmptyClipboardFunction = WindowsSystem::Bool(__stdcall*)();
        using GetClipboardDataFunction = WindowsSystem::Handle(__stdcall*)(WindowsSystem::UInt);
        using SetClipboardDataFunction = WindowsSystem::Handle(__stdcall*)(WindowsSystem::UInt, WindowsSystem::Handle);
        using IsClipboardFormatAvailableFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::UInt);

        OpenClipboardFunction openClipboard = nullptr;
        CloseClipboardFunction closeClipboard = nullptr;
        EmptyClipboardFunction emptyClipboard = nullptr;
        GetClipboardDataFunction getClipboardData = nullptr;
        SetClipboardDataFunction setClipboardData = nullptr;
        IsClipboardFormatAvailableFunction isClipboardFormatAvailable = nullptr;
    };

    /** @brief Dynamically resolved Win32 file, mapping, replacement, and directory-monitoring entry points. */
    struct FileSystemAPI {
        using GetStandardHandleFunction = WindowsSystem::Handle(__stdcall*)(WindowsSystem::DWord);
        using CreateFileWideFunction = WindowsSystem::Handle(__stdcall*)(const wchar_t*, WindowsSystem::DWord,
                                                                         WindowsSystem::DWord, ::_SECURITY_ATTRIBUTES*,
                                                                         WindowsSystem::DWord, WindowsSystem::DWord,
                                                                         WindowsSystem::Handle);
        using CloseHandleFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using WriteFileFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, const void*,
                                                                  WindowsSystem::DWord, WindowsSystem::DWord*,
                                                                  ::_OVERLAPPED*);
        using FlushFileBuffersFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using ReadFileFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, void*, WindowsSystem::DWord,
                                                                  WindowsSystem::DWord*, ::_OVERLAPPED*);
        using GetFileSizeFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_LARGE_INTEGER*);
        using SetFileInformationFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, int, void*,
                                                                           WindowsSystem::DWord);
        using GetFileInformationFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, int, void*,
                                                                           WindowsSystem::DWord);
        using CreateFileMappingWideFunction = WindowsSystem::Handle(__stdcall*)(WindowsSystem::Handle,
                                                                                 ::_SECURITY_ATTRIBUTES*,
                                                                                 WindowsSystem::DWord,
                                                                                 WindowsSystem::DWord,
                                                                                 WindowsSystem::DWord, const wchar_t*);
        using MapViewOfFileFunction = void*(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord,
                                                        WindowsSystem::DWord, WindowsSystem::DWord, size_t);
        using UnmapViewOfFileFunction = WindowsSystem::Bool(__stdcall*)(const void*);
        using FlushViewOfFileFunction = WindowsSystem::Bool(__stdcall*)(const void*, size_t);
        using ReplaceFileWideFunction = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*,
                                                                         const wchar_t*, WindowsSystem::DWord, void*,
                                                                         void*);
        using MoveFileWideFunction = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*,
                                                                      WindowsSystem::DWord);
        using DeleteFileWideFunction = WindowsSystem::Bool(__stdcall*)(const wchar_t*);
        using CreateEventWideFunction = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*,
                                                                           WindowsSystem::Bool, WindowsSystem::Bool,
                                                                           const wchar_t*);
        using ResetEventFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using WaitForSingleObjectFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle,
                                                                             WindowsSystem::DWord);
        using GetOverlappedResultFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_OVERLAPPED*,
                                                                            WindowsSystem::DWord*, WindowsSystem::Bool);
        using CancelIoFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_OVERLAPPED*);
        using DirectoryCompletionFunction = void(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord,
                                                              ::_OVERLAPPED*);
        using ReadDirectoryChangesFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, void*,
                                                                              WindowsSystem::DWord,
                                                                              WindowsSystem::Bool,
                                                                              WindowsSystem::DWord,
                                                                              WindowsSystem::DWord*, ::_OVERLAPPED*,
                                                                              DirectoryCompletionFunction);
        using GetCurrentProcessIdFunction = WindowsSystem::DWord(__stdcall*)();
        using GetSystemInfoFunction = void(__stdcall*)(void*);
        using GetCurrentProcessFunction = WindowsSystem::Handle(__stdcall*)();
        using DuplicateHandleFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle,
                                                                         WindowsSystem::Handle, WindowsSystem::Handle*,
                                                                         WindowsSystem::DWord, WindowsSystem::Bool,
                                                                         WindowsSystem::DWord);

        GetStandardHandleFunction getStandardHandle = nullptr;
        CreateFileWideFunction createFileWide = nullptr;
        CloseHandleFunction closeHandle = nullptr;
        WriteFileFunction writeFile = nullptr;
        FlushFileBuffersFunction flushFileBuffers = nullptr;
        ReadFileFunction readFile = nullptr;
        GetFileSizeFunction getFileSize = nullptr;
        SetFileInformationFunction setFileInformation = nullptr;
        GetFileInformationFunction getFileInformation = nullptr;
        CreateFileMappingWideFunction createFileMappingWide = nullptr;
        MapViewOfFileFunction mapViewOfFile = nullptr;
        UnmapViewOfFileFunction unmapViewOfFile = nullptr;
        FlushViewOfFileFunction flushViewOfFile = nullptr;
        ReplaceFileWideFunction replaceFileWide = nullptr;
        MoveFileWideFunction moveFileWide = nullptr;
        DeleteFileWideFunction deleteFileWide = nullptr;
        CreateEventWideFunction createEventWide = nullptr;
        ResetEventFunction resetEvent = nullptr;
        WaitForSingleObjectFunction waitForSingleObject = nullptr;
        GetOverlappedResultFunction getOverlappedResult = nullptr;
        CancelIoFunction cancelIo = nullptr;
        ReadDirectoryChangesFunction readDirectoryChanges = nullptr;
        GetCurrentProcessIdFunction getCurrentProcessId = nullptr;
        GetSystemInfoFunction getSystemInfo = nullptr;
        GetCurrentProcessFunction getCurrentProcess = nullptr;
        DuplicateHandleFunction duplicateHandle = nullptr;
    };

    /** @brief Dynamically resolved Win32 fatal-exception entry points. */
    struct CrashSystemAPI {
        using ExceptionFilterFunction = long(__stdcall*)(::_EXCEPTION_POINTERS*);
        using SetUnhandledExceptionFilterFunction = ExceptionFilterFunction(__stdcall*)(ExceptionFilterFunction);
        using GetErrorModeFunction = WindowsSystem::DWord(__stdcall*)();
        using SetErrorModeFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord);
        using GetCurrentProcessFunction = WindowsSystem::Handle(__stdcall*)();
        using WerGetFlagsFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord*);
        using WerSetFlagsFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord);

        SetUnhandledExceptionFilterFunction setUnhandledExceptionFilter = nullptr;
        GetErrorModeFunction getErrorMode = nullptr;
        SetErrorModeFunction setErrorMode = nullptr;
        GetCurrentProcessFunction getCurrentProcess = nullptr;
        WerGetFlagsFunction werGetFlags = nullptr;
        WerSetFlagsFunction werSetFlags = nullptr;
    };

    /** @brief Dynamically resolved Windows DbgHelp entry points shared by diagnostic services. */
    struct DbgHelpSystemAPI {
        using SymSetOptionsFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord);
        using SymInitializeFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, const char*,
                                                                      WindowsSystem::Bool);
        using SymFromAddressFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord64,
                                                                       WindowsSystem::DWord64*, ::_SYMBOL_INFO*);
        using SymGetLineFromAddressFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle,
                                                                              WindowsSystem::DWord64,
                                                                              WindowsSystem::DWord*,
                                                                              ::_IMAGEHLP_LINE64*);
        using SymGetModuleInfoFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord64,
                                                                         ::_IMAGEHLP_MODULE64*);
        using UndecorateSymbolNameFunction = WindowsSystem::DWord(__stdcall*)(const char*, char*, WindowsSystem::DWord,
                                                                              WindowsSystem::DWord);

        SymSetOptionsFunction symSetOptions = nullptr;
        SymInitializeFunction symInitialize = nullptr;
        SymFromAddressFunction symFromAddress = nullptr;
        SymGetLineFromAddressFunction symGetLineFromAddress = nullptr;
        SymGetModuleInfoFunction symGetModuleInfo = nullptr;
        UndecorateSymbolNameFunction undecorateSymbolName = nullptr;
    };

    /** @brief Dynamically resolved Windows stack-capture and process entry points. */
    struct StackTraceSystemAPI {
        using CaptureStackBackTraceFunction = uint16_t(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, void**,
                                                                   WindowsSystem::DWord*);
        using GetCurrentProcessFunction = WindowsSystem::Handle(__stdcall*)();

        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        GetCurrentProcessFunction getCurrentProcess = nullptr;
    };

#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

    namespace PosixSystem {

        /** @brief ABI-sized POSIX thread identifier used without including @c pthread.h. */
        using ThreadId = std::conditional_t<Platform::kIsLinux, std::uintptr_t, void*>;

        /** @brief Opaque POSIX thread-attribute object used by the dynamically loaded table. */
        struct ThreadAttributes;

        /** @brief Opaque POSIX signal-set object used by the dynamically loaded table. */
        struct SignalSet;

        /** @brief POSIX file offset ABI type without including @c sys/types.h. */
        using FileOffset = std::int64_t;

        /** @brief POSIX poll descriptor-count ABI type without including @c poll.h. */
        using PollCount = std::size_t;

    } // namespace PosixSystem

    /** @brief POSIX has no additional native-error entry points beyond thread-local @c errno. */
    struct NativeErrorSystemAPI {};

    /** @brief Dynamically resolved POSIX process-environment entry points. */
    struct EnvironmentSystemAPI {
        using GetFunction = char* (*)(const char*);
        using SetFunction = int (*)(const char*, const char*, int);
        using RemoveFunction = int (*)(const char*);

        GetFunction get = nullptr;
        SetFunction set = nullptr;
        RemoveFunction remove = nullptr;
    };

    /** @brief Dynamically resolved Linux or Darwin current-thread entry points. */
    struct ThreadSystemAPI {
        using PthreadSelfFunction = PosixSystem::ThreadId (*)();
        using PthreadGetNameFunction = int (*)(PosixSystem::ThreadId, char*, size_t);

        PthreadSelfFunction pthreadSelf = nullptr;
        PthreadGetNameFunction pthreadGetName = nullptr;

#    if defined(PLATFORM_LINUX)
        using SystemCallFunction = long (*)(long, ...);
        using ScheduleGetCpuFunction = int (*)();
        using PthreadSetNameFunction = int (*)(PosixSystem::ThreadId, const char*);
        using PthreadGetAttributesFunction = int (*)(PosixSystem::ThreadId, PosixSystem::ThreadAttributes*);
        using PthreadDestroyAttributesFunction = int (*)(PosixSystem::ThreadAttributes*);
        using PthreadGetStackFunction = int (*)(const PosixSystem::ThreadAttributes*, void**, size_t*);

        SystemCallFunction systemCall = nullptr;
        ScheduleGetCpuFunction scheduleGetCpu = nullptr;
        PthreadSetNameFunction pthreadSetName = nullptr;
        PthreadGetAttributesFunction pthreadGetAttributes = nullptr;
        PthreadDestroyAttributesFunction pthreadDestroyAttributes = nullptr;
        PthreadGetStackFunction pthreadGetStack = nullptr;
#    else
        using PthreadThreadIdFunction = int (*)(PosixSystem::ThreadId, uint64_t*);
        using PthreadSetNameFunction = int (*)(const char*);
        using PthreadGetStackAddressFunction = void* (*)(PosixSystem::ThreadId);
        using PthreadGetStackSizeFunction = size_t (*)(PosixSystem::ThreadId);

        PthreadThreadIdFunction pthreadThreadId = nullptr;
        PthreadSetNameFunction pthreadSetName = nullptr;
        PthreadGetStackAddressFunction pthreadGetStackAddress = nullptr;
        PthreadGetStackSizeFunction pthreadGetStackSize = nullptr;
#    endif
    };

    /** @brief Win32 movable global memory is unavailable on POSIX. */
    struct GlobalMemorySystemAPI {};

    /** @brief Clipboard is not implemented through a stable POSIX system ABI. */
    struct ClipboardSystemAPI {};

    /** @brief Dynamically resolved POSIX file, mapping, replacement, and monitoring entry points. */
    struct FileSystemAPI {
        using OpenFunction = int (*)(const char*, int, ...);
        using CloseFunction = int (*)(int);
        using ReadFunction = Sora::ssize_t (*)(int, void*, size_t);
        using WriteFunction = Sora::ssize_t (*)(int, const void*, size_t);
        using ReadAtFunction = Sora::ssize_t (*)(int, void*, size_t, PosixSystem::FileOffset);
        using WriteAtFunction = Sora::ssize_t (*)(int, const void*, size_t, PosixSystem::FileOffset);
        using SyncFunction = int (*)(int);
        using ResizeFunction = int (*)(int, PosixSystem::FileOffset);
        using StatFunction = int (*)(int, struct stat*);
        using MapFunction = void* (*)(void*, size_t, int, int, int, PosixSystem::FileOffset);
        using UnmapFunction = int (*)(void*, size_t);
        using SyncMapFunction = int (*)(void*, size_t, int);
        using AdviseMapFunction = int (*)(void*, size_t, int);
        using RenameFunction = int (*)(const char*, const char*);
        using UnlinkFunction = int (*)(const char*);
        using ControlFunction = int (*)(int, int, ...);
        using SystemConfigurationFunction = long (*)(int);
        using DuplicateFunction = int (*)(int);
        using GetProcessIdFunction = int (*)();
        using AdviseFileFunction = int (*)(int, PosixSystem::FileOffset, PosixSystem::FileOffset, int);

        OpenFunction open = nullptr;
        CloseFunction close = nullptr;
        ReadFunction read = nullptr;
        WriteFunction write = nullptr;
        ReadAtFunction readAt = nullptr;
        WriteAtFunction writeAt = nullptr;
        SyncFunction sync = nullptr;
        ResizeFunction resize = nullptr;
        StatFunction stat = nullptr;
        MapFunction map = nullptr;
        UnmapFunction unmap = nullptr;
        SyncMapFunction syncMap = nullptr;
        AdviseMapFunction adviseMap = nullptr;
        RenameFunction rename = nullptr;
        UnlinkFunction unlink = nullptr;
        ControlFunction control = nullptr;
        SystemConfigurationFunction systemConfiguration = nullptr;
        DuplicateFunction duplicate = nullptr;
        GetProcessIdFunction getProcessId = nullptr;
        AdviseFileFunction adviseFile = nullptr;

#    if defined(PLATFORM_LINUX)
        using InitializeNotifyFunction = int (*)(int);
        using AddNotifyFunction = int (*)(int, const char*, uint32_t);
        using RemoveNotifyFunction = int (*)(int, int);
        using PollFunction = int (*)(struct pollfd*, PosixSystem::PollCount, int);

        InitializeNotifyFunction initializeNotify = nullptr;
        AddNotifyFunction addNotify = nullptr;
        RemoveNotifyFunction removeNotify = nullptr;
        PollFunction poll = nullptr;
#    elif defined(PLATFORM_MACOS)
        using CreateQueueFunction = int (*)();
        using QueueEventFunction = int (*)(int, const struct kevent*, int, struct kevent*, int,
                                            const struct timespec*);

        CreateQueueFunction createQueue = nullptr;
        QueueEventFunction queueEvent = nullptr;
#    endif
    };

    /** @brief Dynamically resolved POSIX fatal-signal entry points. */
    struct CrashSystemAPI {
        using SignalActionFunction = int (*)(int, const struct sigaction*, struct sigaction*);
        using EmptySignalSetFunction = int (*)(PosixSystem::SignalSet*);
        using RaiseSignalFunction = int (*)(int);
        using ImmediateExitFunction = void (*)(int);

        SignalActionFunction signalAction = nullptr;
        EmptySignalSetFunction emptySignalSet = nullptr;
        RaiseSignalFunction raiseSignal = nullptr;
        ImmediateExitFunction immediateExit = nullptr;
    };

    /** @brief DbgHelp is unavailable outside Windows. */
    struct DbgHelpSystemAPI {};

    /** @brief Native POSIX stack-capture and dynamic-symbol entry points before PAL normalization. */
    struct PosixStackTraceNativeAPI {
        using CaptureStackBackTraceFunction = int (*)(void**, int);
        using FindDynamicSymbolFunction = int (*)(const void*, void*);

        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        FindDynamicSymbolFunction findDynamicSymbol = nullptr;
    };

    /** @brief ABI-compatible result returned by the POSIX @c dladdr function. */
    struct DynamicSymbolInfo {
        const char* fileName = nullptr;
        void* fileBase = nullptr;
        const char* symbolName = nullptr;
        void* symbolAddress = nullptr;
    };

    /** @brief Dynamically resolved POSIX stack-capture and symbol-query entry points. */
    struct StackTraceSystemAPI {
        using CaptureStackBackTraceFunction = int (*)(void**, int);
        using FindDynamicSymbolFunction = int (*)(const void*, DynamicSymbolInfo*);

        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        FindDynamicSymbolFunction findDynamicSymbol = nullptr;
    };

#else

    struct NativeErrorSystemAPI {};
    struct EnvironmentSystemAPI {};
    struct ThreadSystemAPI {};
    struct GlobalMemorySystemAPI {};
    struct ClipboardSystemAPI {};
    struct FileSystemAPI {};
    struct CrashSystemAPI {};
    struct DbgHelpSystemAPI {};
    struct StackTraceSystemAPI {};

#endif

    /** @brief Exclusive access token for the process-global, non-thread-safe DbgHelp API. */
    class LockedDbgHelpSystemAPI {
    public:
        LockedDbgHelpSystemAPI(const LockedDbgHelpSystemAPI&) = delete;
        LockedDbgHelpSystemAPI& operator=(const LockedDbgHelpSystemAPI&) = delete;
        LockedDbgHelpSystemAPI(LockedDbgHelpSystemAPI&&) noexcept = default;
        LockedDbgHelpSystemAPI& operator=(LockedDbgHelpSystemAPI&&) noexcept = default;

        /** @brief Access DbgHelp entry points while this token owns the process-global lock. */
        [[nodiscard]] const DbgHelpSystemAPI& operator*() const noexcept { return *api_; }

        /** @brief Access DbgHelp entry points while this token owns the process-global lock. */
        [[nodiscard]] const DbgHelpSystemAPI* operator->() const noexcept { return api_; }

    private:
        friend LockedDbgHelpSystemAPI LockDbgHelpSystemAPI();

        LockedDbgHelpSystemAPI(std::mutex& mutex, const DbgHelpSystemAPI& api) : lock_{mutex}, api_{&api} {}

        std::unique_lock<std::mutex> lock_;
        const DbgHelpSystemAPI* api_ = nullptr;
    };

    /** @brief Load and return the immutable native error-reporting function table. */
    [[nodiscard]] const NativeErrorSystemAPI& LoadNativeErrorSystemAPI() noexcept;

    /** @brief Load and return the immutable process-environment function table. */
    [[nodiscard]] const EnvironmentSystemAPI& LoadEnvironmentSystemAPI() noexcept;

    /** @brief Load and return the immutable current-thread function table. */
    [[nodiscard]] const ThreadSystemAPI& LoadThreadSystemAPI() noexcept;

    /** @brief Load and return the immutable movable-global-memory function table. */
    [[nodiscard]] const GlobalMemorySystemAPI& LoadGlobalMemorySystemAPI() noexcept;

    /** @brief Load and return the immutable clipboard function table. */
    [[nodiscard]] const ClipboardSystemAPI& LoadClipboardSystemAPI() noexcept;

    /** @brief Load and return the immutable native file-system function table. */
    [[nodiscard]] const FileSystemAPI& LoadFileSystemAPI() noexcept;

    /** @brief Load and return the immutable crash-output function table. */
    [[nodiscard]] const CrashSystemAPI& LoadCrashSystemAPI() noexcept;

    /** @brief Lock and return exclusive access to the immutable Windows DbgHelp function table. */
    [[nodiscard]] LockedDbgHelpSystemAPI LockDbgHelpSystemAPI();

    /** @brief Initialize the process-global DbgHelp symbol handler once. */
    [[nodiscard]] bool InitializeDbgHelpSystemAPI() noexcept;

    /** @brief Load and return the immutable native stack-trace function table. */
    [[nodiscard]] const StackTraceSystemAPI& LoadStackTraceSystemAPI() noexcept;

} // namespace Sora::PAL
