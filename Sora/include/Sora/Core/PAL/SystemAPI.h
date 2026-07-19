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

#include <Sora/Core/FixedString.h>
#include <Sora/Platform.h>

#include <concepts>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <type_traits>

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
struct kevent;
struct pollfd;
struct rusage;
struct sigaction;
struct stat;
struct timespec;
#endif

#if defined(PLATFORM_WINDOWS)
struct _IMAGEHLP_LINE64;
struct _IMAGEHLP_MODULE64;
struct _EXCEPTION_POINTERS;
struct _FILETIME;
struct _PROCESS_MEMORY_COUNTERS;
union _LARGE_INTEGER;
struct _OVERLAPPED;
struct _SECURITY_ATTRIBUTES;
struct _SYMBOL_INFO;
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

    } // namespace $

    /** @brief Platform-normalized counters produced by native process-accounting adapters. */
    struct ProcessUsageCounters {
        uint64_t userCpuNanoseconds = 0;
        uint64_t kernelCpuNanoseconds = 0;
        uint64_t residentMemoryBytes = 0;
        uint64_t peakResidentMemoryBytes = 0;
    };

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
        /** @brief Opaque native type used by the existing Win32 file API table. */
        using NativeOverlapped = ::_OVERLAPPED;
        using NativeLargeInteger = ::_LARGE_INTEGER;

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
         * transfer sizes should then respect the effective physical-sector size and any additional alignment offsets
         * reported below.
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

        /** @brief ABI-compatible signed 64-bit integer returned by Win32 file-size APIs. */
        struct LargeInteger {
            int64_t quadPart = 0;
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

    /** @brief Capture the calling thread's native error slot without triggering lazy symbol resolution. */
    [[nodiscard]] SystemError CaptureLastSystemError() noexcept;

    /** @brief Dynamically resolved Win32 error-reporting entry points. */
    struct NativeErrorSystemAPI {
        // clang-format off
        using GetLastErrorFunction      = WindowsSystem::DWord(__stdcall*)();
        using SetLastErrorFunction      = void(__stdcall*)(WindowsSystem::DWord);
        using FormatMessageWideFunction = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord, const void*, WindowsSystem::DWord, WindowsSystem::DWord, wchar_t*, WindowsSystem::DWord, std::va_list*);
        using LocalFreeFunction         = void*(__stdcall*)(void*);

        [[= $::Syscall{"GetLastError"}]]
        GetLastErrorFunction      getLastError      = nullptr;
        [[= $::Syscall{"SetLastError"}]]
        SetLastErrorFunction      setLastError      = nullptr;
        [[= $::Syscall{"FormatMessageW"}]]
        FormatMessageWideFunction formatMessageWide = nullptr;
        [[= $::Syscall{"LocalFree"}]]
        LocalFreeFunction         localFree         = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 environment entry points. */
    struct EnvironmentSystemAPI {
        // clang-format off
        using GetLastErrorFunction               = WindowsSystem::DWord(__stdcall*)();
        using SetLastErrorFunction               = void(__stdcall*)(WindowsSystem::DWord);
        using GetEnvironmentVariableWideFunction = WindowsSystem::DWord(__stdcall*)(const wchar_t*, wchar_t*, WindowsSystem::DWord);
        using SetEnvironmentVariableWideFunction = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*);
        using GetEnvironmentStringsWideFunction  = wchar_t*(__stdcall*)();
        using FreeEnvironmentStringsWideFunction = WindowsSystem::Bool(__stdcall*)(wchar_t*);

        [[= $::Syscall{"GetLastError"}]]
        GetLastErrorFunction                    getLastError                    = nullptr;
        [[= $::Syscall{"SetLastError"}]]
        SetLastErrorFunction                    setLastError                    = nullptr;
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
        using CloseHandleFunction                   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using QueryFullProcessImageNameWideFunction = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, wchar_t*, WindowsSystem::DWord*);
        using GetCommandLineWideFunction            = wchar_t*(__stdcall*)();
        using CommandLineToArgvWideFunction         = wchar_t**(__stdcall*)(const wchar_t*, int*);
        using LocalFreeFunction                     = void*(__stdcall*)(void*);
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
        [[= $::Syscall{"CloseHandle"}]]
        CloseHandleFunction                   closeHandle                    = nullptr;
        [[= $::Syscall{"QueryFullProcessImageNameW"}]]
        QueryFullProcessImageNameWideFunction queryFullProcessImageNameWide = nullptr;
        [[= $::Syscall{"GetCommandLineW"}]]
        GetCommandLineWideFunction            getCommandLineWide            = nullptr;
        [[= $::Syscall{"CommandLineToArgvW"}]]
        CommandLineToArgvWideFunction         commandLineToArgvWide         = nullptr;
        [[= $::Syscall{"LocalFree"}]]
        LocalFreeFunction                     localFree                     = nullptr;
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
        using SetThreadDescriptionFunction        = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, const wchar_t*);
        using GetThreadDescriptionFunction        = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, wchar_t**);
        using LocalFreeFunction                   = void*(__stdcall*)(void*);
        using GetCurrentProcessorNumberExFunction = void(__stdcall*)(WindowsSystem::ProcessorNumber*);
        using GetCurrentThreadStackLimitsFunction = void(__stdcall*)(uintptr_t*, uintptr_t*);

        [[= $::Syscall{"GetCurrentThreadId"}]]
        GetCurrentThreadIdFunction          getCurrentThreadId          = nullptr;
        [[= $::Syscall{"GetCurrentThread"}]]
        GetCurrentThreadFunction            getCurrentThread            = nullptr;
        [[= $::Syscall{"SetThreadDescription"}]]
        SetThreadDescriptionFunction        setThreadDescription        = nullptr;
        [[= $::Syscall{"GetThreadDescription"}]]
        GetThreadDescriptionFunction        getThreadDescription        = nullptr;
        [[= $::Syscall{"LocalFree"}]]
        LocalFreeFunction                   localFree                   = nullptr;
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

    /** @brief Dynamically resolved Win32 file, mapping, replacement, and directory-monitoring entry points. */
    struct FileSystemAPI {
        // clang-format off
        using GetStandardHandleFunction     = WindowsSystem::Handle(__stdcall*)(WindowsSystem::DWord);
        using CreateFileWideFunction        = WindowsSystem::Handle(__stdcall*)(const wchar_t*, WindowsSystem::DWord, WindowsSystem::DWord, ::_SECURITY_ATTRIBUTES*, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::Handle);
        using CloseHandleFunction           = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using WriteFileFunction             = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, const void*, WindowsSystem::DWord, WindowsSystem::DWord*, ::_OVERLAPPED*);
        using FlushFileBuffersFunction      = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using ReadFileFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, void*, WindowsSystem::DWord, WindowsSystem::DWord*, ::_OVERLAPPED*);
        using GetFileSizeFunction           = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_LARGE_INTEGER*);
        using SetFileInformationFunction    = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, int, void*, WindowsSystem::DWord);
        using GetFileInformationFunction    = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, int, void*, WindowsSystem::DWord);
        using CreateFileMappingWideFunction = WindowsSystem::Handle(__stdcall*)(WindowsSystem::Handle, ::_SECURITY_ATTRIBUTES*, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord, const wchar_t*);
        using MapViewOfFileFunction         = void*(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord, WindowsSystem::DWord, WindowsSystem::DWord, size_t);
        using UnmapViewOfFileFunction       = WindowsSystem::Bool(__stdcall*)(const void*);
        using FlushViewOfFileFunction       = WindowsSystem::Bool(__stdcall*)(const void*, size_t);
        using ReplaceFileWideFunction       = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*, WindowsSystem::DWord, void*, void*);
        using MoveFileWideFunction          = WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*, WindowsSystem::DWord);
        using DeleteFileWideFunction        = WindowsSystem::Bool(__stdcall*)(const wchar_t*);
        using CreateEventWideFunction       = WindowsSystem::Handle(__stdcall*)(::_SECURITY_ATTRIBUTES*, WindowsSystem::Bool, WindowsSystem::Bool, const wchar_t*);
        using ResetEventFunction            = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle);
        using WaitForSingleObjectFunction   = WindowsSystem::DWord(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord);
        using GetOverlappedResultFunction   = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_OVERLAPPED*, WindowsSystem::DWord*, WindowsSystem::Bool);
        using CancelIoFunction              = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, ::_OVERLAPPED*);
        using DirectoryCompletionFunction   = void(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, ::_OVERLAPPED*);
        using ReadDirectoryChangesFunction  = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, void*, WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord, WindowsSystem::DWord*, ::_OVERLAPPED*, DirectoryCompletionFunction);
        using GetSystemInfoFunction         = void(__stdcall*)(void*);
        using GetCurrentProcessFunction     = WindowsSystem::Handle(__stdcall*)();
        using DuplicateHandleFunction       = WindowsSystem::Bool(__stdcall*)(WindowsSystem::Handle, WindowsSystem::Handle, WindowsSystem::Handle, WindowsSystem::Handle*, WindowsSystem::DWord, WindowsSystem::Bool, WindowsSystem::DWord);

        [[= $::Syscall{"GetStdHandle"}]]
        GetStandardHandleFunction     getStandardHandle     = nullptr;
        [[= $::Syscall{"CreateFileW"}]]
        CreateFileWideFunction        createFileWide        = nullptr;
        [[= $::Syscall{"CloseHandle"}]]
        CloseHandleFunction           closeHandle           = nullptr;
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
        [[= $::Syscall{"CreateFileMappingW"}]]
        CreateFileMappingWideFunction createFileMappingWide = nullptr;
        [[= $::Syscall{"MapViewOfFile"}]]
        MapViewOfFileFunction         mapViewOfFile         = nullptr;
        [[= $::Syscall{"UnmapViewOfFile"}]]
        UnmapViewOfFileFunction       unmapViewOfFile       = nullptr;
        [[= $::Syscall{"FlushViewOfFile"}]]
        FlushViewOfFileFunction       flushViewOfFile       = nullptr;
        [[= $::Syscall{"ReplaceFileW"}]]
        ReplaceFileWideFunction       replaceFileWide       = nullptr;
        [[= $::Syscall{"MoveFileExW"}]]
        MoveFileWideFunction          moveFileWide          = nullptr;
        [[= $::Syscall{"DeleteFileW"}]]
        DeleteFileWideFunction        deleteFileWide        = nullptr;
        [[= $::Syscall{"CreateEventW"}]]
        CreateEventWideFunction       createEventWide       = nullptr;
        [[= $::Syscall{"ResetEvent"}]]
        ResetEventFunction            resetEvent            = nullptr;
        [[= $::Syscall{"WaitForSingleObject"}]]
        WaitForSingleObjectFunction   waitForSingleObject   = nullptr;
        [[= $::Syscall{"GetOverlappedResult"}]]
        GetOverlappedResultFunction   getOverlappedResult   = nullptr;
        [[= $::Syscall{"CancelIoEx"}]]
        CancelIoFunction              cancelIo              = nullptr;
        [[= $::Syscall{"ReadDirectoryChangesW"}]]
        ReadDirectoryChangesFunction  readDirectoryChanges  = nullptr;
        [[= $::Syscall{"GetSystemInfo"}]]
        GetSystemInfoFunction         getSystemInfo         = nullptr;
        [[= $::Syscall{"GetCurrentProcess"}]]
        GetCurrentProcessFunction     getCurrentProcess     = nullptr;
        [[= $::Syscall{"DuplicateHandle"}]]
        DuplicateHandleFunction       duplicateHandle       = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved Win32 fatal-exception entry points. */
    struct CrashSystemAPI {
        // clang-format off
        using ExceptionFilterFunction              = long(__stdcall*)(::_EXCEPTION_POINTERS*);
        using SetUnhandledExceptionFilterFunction  = ExceptionFilterFunction(__stdcall*)(ExceptionFilterFunction);
        using GetErrorModeFunction                 = WindowsSystem::DWord(__stdcall*)();
        using SetErrorModeFunction                 = WindowsSystem::DWord(__stdcall*)(WindowsSystem::DWord);
        using GetCurrentProcessFunction            = WindowsSystem::Handle(__stdcall*)();
        using WerGetFlagsFunction                  = WindowsSystem::HResult(__stdcall*)(WindowsSystem::Handle, WindowsSystem::DWord*);
        using WerSetFlagsFunction                  = WindowsSystem::HResult(__stdcall*)(WindowsSystem::DWord);

        [[= $::Syscall{"SetUnhandledExceptionFilter"}]]
        SetUnhandledExceptionFilterFunction  setUnhandledExceptionFilter  = nullptr;
        [[= $::Syscall{"GetErrorMode"}]]
        GetErrorModeFunction                 getErrorMode                 = nullptr;
        [[= $::Syscall{"SetErrorMode"}]]
        SetErrorModeFunction                 setErrorMode                 = nullptr;
        [[= $::Syscall{"GetCurrentProcess"}]]
        GetCurrentProcessFunction            getCurrentProcess            = nullptr;
        [[= $::Syscall{"WerGetFlags"}]]
        WerGetFlagsFunction                  werGetFlags                  = nullptr;
        [[= $::Syscall{"WerSetFlags"}]]
        WerSetFlagsFunction                  werSetFlags                  = nullptr;
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

    /** @brief Dynamically resolved Windows stack-capture and process entry points. */
    struct StackTraceSystemAPI {
        // clang-format off
        using CaptureStackBackTraceFunction = uint16_t(__stdcall*)(WindowsSystem::DWord, WindowsSystem::DWord, void**, WindowsSystem::DWord*);
        using GetCurrentProcessFunction     = WindowsSystem::Handle(__stdcall*)();

        [[= $::Syscall{"RtlCaptureStackBackTrace"}]]
        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        [[= $::Syscall{"GetCurrentProcess"}]]
        GetCurrentProcessFunction     getCurrentProcess     = nullptr;
        // clang-format on
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

        /** @brief POSIX creation-mode ABI type without including @c sys/types.h. */
        using FileMode = std::conditional_t<Platform::kIsLinux, uint32_t, uint16_t>;

        /** @brief POSIX poll descriptor-count ABI type without including @c poll.h. */
        using PollCount = size_t;

        /** @brief Opaque Darwin process-accounting buffer pointer used by @c proc_pid_rusage. */
        using ResourceUsageInfo = void*;

        /** @brief Opaque native poll descriptor used by the POSIX file API table. */
        using NativePollDescriptor = ::pollfd;

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
            inline constexpr int kOpenReadOnly = 0x00000000;
            inline constexpr int kOpenWriteOnly = 0x00000001;
            inline constexpr int kOpenReadWrite = 0x00000002;
            inline constexpr int kOpenCreate = 0x00000040;
            inline constexpr int kOpenExclusive = 0x00000080;
            inline constexpr int kOpenTruncate = 0x00000200;
            inline constexpr int kOpenDirectory = 0x00010000;
            inline constexpr int kOpenDirect = 0x00004000;
            inline constexpr int kOpenSync = 0x00101000;
            inline constexpr int kPageSizeConfiguration = 30;
            inline constexpr int kFileAdviceSequential = 2;
            inline constexpr int kFileAdviceRandom = 1;
            // clang-format on
#    elif defined(PLATFORM_MACOS)
            // clang-format off
            inline constexpr int kOpenReadOnly = 0x00000000;
            inline constexpr int kOpenWriteOnly = 0x00000001;
            inline constexpr int kOpenReadWrite = 0x00000002;
            inline constexpr int kOpenCreate = 0x00000200;
            inline constexpr int kOpenExclusive = 0x00000800;
            inline constexpr int kOpenTruncate = 0x00000400;
            inline constexpr int kOpenDirectory = 0x00100000;
            inline constexpr int kOpenSync = 0x00000080;
            inline constexpr int kNoCacheControl = 48;
            inline constexpr int kFullSyncControl = 51;
            inline constexpr int kPageSizeConfiguration = 29;
            // clang-format on
#    endif

        } // namespace File

        /** @brief POSIX virtual-memory protection, mapping, and synchronization constants. */
        inline namespace Mapping {

            // clang-format off
            inline constexpr int kProtectionRead = 0x1;
            inline constexpr int kProtectionWrite = 0x2;
            inline constexpr int kMapShared = 0x1;
            inline constexpr int kMapPrivate = 0x2;
            inline constexpr int kSynchronize = 0x4;
            inline constexpr int kAdviceNormal = 0;
            inline constexpr int kAdviceRandom = 1;
            inline constexpr int kAdviceSequential = 2;
            inline constexpr int kAdviceWillNeed = 3;
            inline constexpr int kAdviceDontNeed = 4;
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
            inline constexpr int kDynamicLocal = 0x00000004;
#    if defined(PLATFORM_LINUX)
            inline constexpr int kDynamicGlobal = 0x00000100;
#    else
            inline constexpr int kDynamicGlobal = 0x00000008;
#    endif

        } // namespace Module

    } // namespace PosixSystem

    /** @brief POSIX has no additional native-error entry points beyond thread-local @c errno. */
    struct NativeErrorSystemAPI {};

    /** @brief Dynamically resolved POSIX process-environment entry points. */
    struct EnvironmentSystemAPI {
        // clang-format off
        using GetFunction    = char* (*)(const char*);
        using SetFunction    = int (*)(const char*, const char*, int);
        using RemoveFunction = int (*)(const char*);

        [[= $::Syscall{"getenv"}]]
        GetFunction    get    = nullptr;
        [[= $::Syscall{"setenv"}]]
        SetFunction    set    = nullptr;
        [[= $::Syscall{"unsetenv"}]]
        RemoveFunction remove = nullptr;
        // clang-format on
    };

    /** @brief Dynamically resolved module-loader operations with normalized POSIX handle semantics. */
    struct ModuleSystemAPI {
        // clang-format off
        using OpenFunction   = void* (*)(const char*, int) noexcept;
        using CloseFunction  = int (*)(void*) noexcept;
        using FindSymbolFunction = void* (*)(void*, const char*) noexcept;

        [[= $::Syscall{"dlopen"}]]
        OpenFunction        open        = nullptr;
        [[= $::Syscall{"dlclose"}]]
        CloseFunction       close       = nullptr;
        [[= $::Syscall{"dlsym"}]]
        FindSymbolFunction  findSymbol  = nullptr;
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
        using SystemCallFunction               = long (*)(long, ...);
        using ScheduleGetCpuFunction           = int (*)();
        using PthreadSetNameFunction           = int (*)(PosixSystem::ThreadId, const char*);
        using PthreadGetAttributesFunction     = int (*)(PosixSystem::ThreadId, PosixSystem::ThreadAttributes*);
        using PthreadDestroyAttributesFunction = int (*)(PosixSystem::ThreadAttributes*);
        using PthreadGetStackFunction          = int (*)(const PosixSystem::ThreadAttributes*, void**, size_t*);

        [[= $::Syscall{"syscall"}]]
        SystemCallFunction               systemCall               = nullptr;
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
        using MapFunction                 = void* (*)(void*, size_t, int, int, int, PosixSystem::FileOffset);
        using UnmapFunction               = int (*)(void*, size_t);
        using SyncMapFunction             = int (*)(void*, size_t, int);
        using AdviseMapFunction           = int (*)(void*, size_t, int);
        using RenameFunction              = int (*)(const char*, const char*);
        using UnlinkFunction              = int (*)(const char*);
        using ControlFunction             = int (*)(int, int, ...);
        using SystemConfigurationFunction = long (*)(int);
        using QueryFileBlockSizeFunction  = bool (*)(int, size_t*) noexcept;
        using QueryFileSizeFunction       = bool (*)(int, uint64_t*) noexcept;
        using DuplicateFunction           = int (*)(int);
        using AdviseFileFunction          = int (*)(int, PosixSystem::FileOffset, PosixSystem::FileOffset, int);

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
        [[= $::Syscall{"mmap"}]]
        MapFunction                 map                 = nullptr;
        [[= $::Syscall{"munmap"}]]
        UnmapFunction               unmap               = nullptr;
        [[= $::Syscall{"msync"}]]
        SyncMapFunction             syncMap             = nullptr;
        [[= $::Syscall{"madvise"}]]
        AdviseMapFunction           adviseMap           = nullptr;
        [[= $::Syscall{"rename"}]]
        RenameFunction              rename              = nullptr;
        [[= $::Syscall{"unlink"}]]
        UnlinkFunction              unlink              = nullptr;
        [[= $::Syscall{"fcntl"}]]
        ControlFunction             control             = nullptr;
        [[= $::Syscall{"sysconf"}]]
        SystemConfigurationFunction systemConfiguration = nullptr;
        [[= $::Syscall{"fstat"}]]
        QueryFileBlockSizeFunction  queryFileBlockSize  = nullptr;
        [[= $::Syscall{"fstat"}]]
        QueryFileSizeFunction       queryFileSize       = nullptr;
        [[= $::Syscall{"dup"}]]
        DuplicateFunction           duplicate           = nullptr;
        [[= $::Syscall{"posix_fadvise"}]]
        AdviseFileFunction          adviseFile          = nullptr;

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

    /** @brief Dynamically resolved POSIX fatal-signal entry points. */
    struct CrashSystemAPI {
        // clang-format off
        using SignalActionFunction   = int (*)(int, const struct sigaction*, struct sigaction*);
        using EmptySignalSetFunction = int (*)(PosixSystem::SignalSet*);
        using RaiseSignalFunction    = int (*)(int);
        using ImmediateExitFunction  = void (*)(int);

        [[= $::Syscall{"sigaction"}]]
        SignalActionFunction   signalAction   = nullptr;
        [[= $::Syscall{"sigemptyset"}]]
        EmptySignalSetFunction emptySignalSet = nullptr;
        [[= $::Syscall{"raise"}]]
        RaiseSignalFunction    raiseSignal    = nullptr;
        [[= $::Syscall{"_exit"}]]
        ImmediateExitFunction  immediateExit  = nullptr;
        // clang-format on
    };

    /** @brief DbgHelp is unavailable outside Windows. */
    struct DbgHelpSystemAPI {};

    /** @brief Native POSIX stack-capture and dynamic-symbol entry points before PAL normalization. */
    struct PosixStackTraceNativeAPI {
        // clang-format off
        using CaptureStackBackTraceFunction = int (*)(void**, int);
        using FindDynamicSymbolFunction     = int (*)(const void*, void*);

        [[= $::Syscall{"backtrace"}]]
        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        [[= $::Syscall{"dladdr"}]]
        FindDynamicSymbolFunction     findDynamicSymbol     = nullptr;
        // clang-format on
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
        // clang-format off
        using CaptureStackBackTraceFunction = int (*)(void**, int);
        using FindDynamicSymbolFunction     = int (*)(const void*, DynamicSymbolInfo*);

        [[= $::Syscall{"backtrace"}]]
        CaptureStackBackTraceFunction captureStackBackTrace = nullptr;
        [[= $::Syscall{"dladdr"}]]
        FindDynamicSymbolFunction     findDynamicSymbol     = nullptr;
        // clang-format on
    };

#else

    struct NativeErrorSystemAPI {};
    struct EnvironmentSystemAPI {};
    struct ModuleSystemAPI {};
    struct ProcessSystemAPI {};
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
        LockedDbgHelpSystemAPI(LockedDbgHelpSystemAPI&&) noexcept = delete;
        LockedDbgHelpSystemAPI& operator=(LockedDbgHelpSystemAPI&&) noexcept = delete;

        /** @brief Access DbgHelp entry points while this token owns the process-global lock. */
        [[nodiscard]] const DbgHelpSystemAPI& operator*() const noexcept { return api_; }

        /** @brief Access DbgHelp entry points while this token owns the process-global lock. */
        [[nodiscard]] const DbgHelpSystemAPI* operator->() const noexcept { return &api_; }

    private:
        friend LockedDbgHelpSystemAPI LockDbgHelpSystemAPI();

        LockedDbgHelpSystemAPI(std::mutex& mutex, const DbgHelpSystemAPI& api) : lock_{mutex}, api_{api} {}

        std::unique_lock<std::mutex> lock_;
        const DbgHelpSystemAPI& api_;
    };

    /** @brief Load and return the immutable native error-reporting function table. */
    [[nodiscard]] const NativeErrorSystemAPI& LoadNativeErrorSystemAPI() noexcept;

    /** @brief Load and return the immutable process-environment function table. */
    [[nodiscard]] const EnvironmentSystemAPI& LoadEnvironmentSystemAPI() noexcept;

    /** @brief Load and return the immutable normalized dynamic-module function table. */
    [[nodiscard]] const ModuleSystemAPI& LoadModuleSystemAPI() noexcept;

    /** @brief Load and return the immutable current-process introspection function table. */
    [[nodiscard]] const ProcessSystemAPI& LoadProcessSystemAPI() noexcept;

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

    /** @brief Ensure that all provided system APIs are loaded and valid. */
    template<typename... Ts>
        requires(sizeof...(Ts) > 0 && (std::is_function_v<std::remove_pointer_t<Ts>> && ...))
    [[nodiscard]] bool EnsureSystemAPIs(Ts&... apis) noexcept {
        return (apis && ...);
    }

} // namespace Sora::PAL
