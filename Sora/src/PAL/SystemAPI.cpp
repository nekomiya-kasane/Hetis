/**
 * @file SystemAPI.cpp
 * @brief Bootstrap and initialize immutable dynamically resolved system API tables.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/PAL/Module.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <limits>
#include <mutex>
#include <string_view>
#include <type_traits>

#if defined(PLATFORM_WINDOWS)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#    include <DbgHelp.h>
#    include <Psapi.h>
#    include <Shellapi.h>
#    include <TlHelp32.h>
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#    include <cstdio>
#    include <dlfcn.h>
#    include <execinfo.h>
#    include <fcntl.h>
#    include <poll.h>
#    include <pthread.h>
#    include <signal.h>
#    include <sys/mman.h>
#    include <sys/resource.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
#    if defined(PLATFORM_MACOS)
#        include <libproc.h>
#        include <mach-o/dyld.h>
#    endif
#endif

namespace Sora::PAL {

    namespace {

#if defined(PLATFORM_WINDOWS)
        /** @brief Load one exact system module and retain it for the process lifetime. */
        [[nodiscard]] ModulePtr LoadSystemModule(std::string_view name) noexcept {
            try {
                constexpr ModuleLoadOptions options{
                    .nameKind = ModuleNameKind::ExactPath,
                    .candidatePolicy = ModuleCandidatePolicy::ExactOnly,
                    .cachePolicy = ModuleCachePolicy::Private,
                    .unloadOnDestroy = false,
                };
                Result<ModulePtr> loaded = LoadModule({name}, options);
                return loaded ? std::move(*loaded) : nullptr;
            } catch (...) {
                return nullptr;
            }
        }

        [[nodiscard]] const ModulePtr& KernelModule() noexcept {
            static const ModulePtr module = LoadSystemModule("kernel32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& ClipboardModule() noexcept {
            static const ModulePtr module = LoadSystemModule("user32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& ShellModule() noexcept {
            static const ModulePtr module = LoadSystemModule("shell32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& DbgHelpModule() noexcept {
            static const ModulePtr module = LoadSystemModule("dbghelp.dll");
            return module;
        }

        [[nodiscard]] bool QueryWindowsParentProcessId(WindowsSystem::DWord* parent) noexcept {
            if (parent == nullptr) {
                return false;
            }
            const ProcessSystemAPI& api = LoadProcessSystemAPI();
            if (api.getCurrentProcessId == nullptr || api.createProcessSnapshot == nullptr ||
                api.firstProcess == nullptr || api.nextProcess == nullptr || api.closeHandle == nullptr) {
                return false;
            }

            HANDLE snapshot = api.createProcessSnapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE) {
                return false;
            }
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            const DWORD current = api.getCurrentProcessId();
            bool result = false;
            for (BOOL found = api.firstProcess(snapshot, &entry); found != FALSE;
                 found = api.nextProcess(snapshot, &entry)) {
                if (entry.th32ProcessID == current) {
                    *parent = entry.th32ParentProcessID;
                    result = true;
                    break;
                }
            }
            api.closeHandle(snapshot);
            return result;
        }

        [[nodiscard]] uint64_t FileTimeTicks(const FILETIME& value) noexcept {
            return static_cast<uint64_t>(value.dwLowDateTime) | (static_cast<uint64_t>(value.dwHighDateTime) << 32);
        }

        [[nodiscard]] bool CaptureWindowsProcessUsage(ProcessUsageCounters* output) noexcept {
            if (output == nullptr) {
                return false;
            }
            const ProcessSystemAPI& api = LoadProcessSystemAPI();
            if (api.getCurrentProcess == nullptr || api.getProcessTimes == nullptr ||
                api.getProcessMemoryInfo == nullptr) {
                return false;
            }

            HANDLE process = api.getCurrentProcess();
            FILETIME created{}, exited{}, kernel{}, user{};
            PROCESS_MEMORY_COUNTERS memory{};
            memory.cb = sizeof(memory);
            if (api.getProcessTimes(process, &created, &exited, &kernel, &user) == FALSE ||
                api.getProcessMemoryInfo(process, &memory, sizeof(memory)) == FALSE) {
                return false;
            }

            constexpr uint64_t kNanosecondsPerTick = 100;
            const uint64_t userTicks = FileTimeTicks(user);
            const uint64_t kernelTicks = FileTimeTicks(kernel);
            if (userTicks > std::numeric_limits<uint64_t>::max() / kNanosecondsPerTick ||
                kernelTicks > std::numeric_limits<uint64_t>::max() / kNanosecondsPerTick) {
                return false;
            }
            *output = {
                .userCpuNanoseconds = userTicks * kNanosecondsPerTick,
                .kernelCpuNanoseconds = kernelTicks * kNanosecondsPerTick,
                .residentMemoryBytes = static_cast<uint64_t>(memory.WorkingSetSize),
                .peakResidentMemoryBytes = static_cast<uint64_t>(memory.PeakWorkingSetSize),
            };
            return true;
        }

        // clang-format off
        static_assert(sizeof(WindowsSystem::ProcessorNumber) == sizeof(PROCESSOR_NUMBER));
        static_assert(alignof(WindowsSystem::ProcessorNumber) == alignof(PROCESSOR_NUMBER));

        static_assert(std::is_same_v<WindowsSystem::Bool, BOOL>);
        static_assert(std::is_same_v<WindowsSystem::DWord, DWORD>);
        static_assert(std::is_same_v<WindowsSystem::DWord64, DWORD64>);
        static_assert(std::is_same_v<WindowsSystem::UInt, UINT>);
        static_assert(std::is_same_v<WindowsSystem::HResult, HRESULT>);

        static_assert(std::is_same_v<DbgHelpSystemAPI::SymSetOptionsFunction, decltype(&::SymSetOptions)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymInitializeFunction, decltype(&::SymInitialize)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymFromAddressFunction, decltype(&::SymFromAddr)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymGetLineFromAddressFunction, decltype(&::SymGetLineFromAddr64)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymGetModuleInfoFunction, decltype(&::SymGetModuleInfo64)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::UndecorateSymbolNameFunction, decltype(&::UnDecorateSymbolName)>);

        static_assert(std::is_same_v<StackTraceSystemAPI::CaptureStackBackTraceFunction, decltype(&::RtlCaptureStackBackTrace)>);
        static_assert(std::is_same_v<StackTraceSystemAPI::GetCurrentProcessFunction, decltype(&::GetCurrentProcess)>);

        static_assert(std::is_same_v<CrashSystemAPI::SetUnhandledExceptionFilterFunction, decltype(&::SetUnhandledExceptionFilter)>);

        static_assert(std::is_same_v<ProcessSystemAPI::GetCurrentProcessIdFunction, decltype(&::GetCurrentProcessId)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetCurrentProcessFunction, decltype(&::GetCurrentProcess)>);
        static_assert(std::is_same_v<ProcessSystemAPI::CreateProcessSnapshotFunction, decltype(&::CreateToolhelp32Snapshot)>);
        static_assert(std::is_same_v<ProcessSystemAPI::ReadProcessEntryFunction, decltype(&::Process32FirstW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::ReadProcessEntryFunction, decltype(&::Process32NextW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::CloseHandleFunction, decltype(&::CloseHandle)>);
        static_assert(std::is_same_v<ProcessSystemAPI::QueryFullProcessImageNameWideFunction, decltype(&::QueryFullProcessImageNameW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetCommandLineWideFunction, decltype(&::GetCommandLineW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::CommandLineToArgvWideFunction, decltype(&::CommandLineToArgvW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::LocalFreeFunction, decltype(&::LocalFree)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessTimesFunction, decltype(&::GetProcessTimes)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessMemoryInfoFunction, decltype(&::K32GetProcessMemoryInfo)>);

        static_assert(sizeof(WindowsSystem::GlobalMemory) == sizeof(HGLOBAL));
        static_assert(alignof(WindowsSystem::GlobalMemory) == alignof(HGLOBAL));
        static_assert(sizeof(WindowsSystem::Handle) == sizeof(HWND));
        static_assert(alignof(WindowsSystem::Handle) == alignof(HWND));

        static_assert(std::is_same_v<ClipboardSystemAPI::CloseClipboardFunction, decltype(&::CloseClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::EmptyClipboardFunction, decltype(&::EmptyClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::GetClipboardDataFunction, decltype(&::GetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::SetClipboardDataFunction, decltype(&::SetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::IsClipboardFormatAvailableFunction, decltype(&::IsClipboardFormatAvailable)>);

        static_assert(std::is_same_v<FileSystemAPI::GetStandardHandleFunction, decltype(&::GetStdHandle)>);
        static_assert(std::is_same_v<FileSystemAPI::CreateFileWideFunction, decltype(&::CreateFileW)>);
        static_assert(std::is_same_v<FileSystemAPI::CloseHandleFunction, decltype(&::CloseHandle)>);
        static_assert(std::is_same_v<FileSystemAPI::ReadFileFunction, decltype(&::ReadFile)>);
        static_assert(std::is_same_v<FileSystemAPI::WriteFileFunction, decltype(&::WriteFile)>);
        static_assert(std::is_same_v<FileSystemAPI::FlushFileBuffersFunction, decltype(&::FlushFileBuffers)>);

        static_assert(sizeof(WindowsSystem::DWord) == sizeof(DWORD));
        static_assert(alignof(WindowsSystem::DWord) == alignof(DWORD));
        static_assert(std::is_unsigned_v<WindowsSystem::DWord> == std::is_unsigned_v<DWORD>);
        static_assert(sizeof(WindowsSystem::UInt) == sizeof(UINT));
        static_assert(alignof(WindowsSystem::UInt) == alignof(UINT));
        static_assert(std::is_unsigned_v<WindowsSystem::UInt> == std::is_unsigned_v<UINT>);
        static_assert(sizeof(WindowsSystem::Size) == sizeof(SIZE_T));
        static_assert(alignof(WindowsSystem::Size) == alignof(SIZE_T));
        static_assert(std::is_unsigned_v<WindowsSystem::Size> == std::is_unsigned_v<SIZE_T>);
        // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        [[nodiscard]] const PosixStackTraceNativeAPI& PosixStackTraceAPI() noexcept {
            static const PosixStackTraceNativeAPI api = [] {
                PosixStackTraceNativeAPI loaded{};
                const ModulePtr& module = CurrentProcessModule();
                if (module == nullptr) {
                    return loaded;
                }
                loaded.captureStackBackTrace =
                    module->TryFindFunction<PosixStackTraceNativeAPI::CaptureStackBackTraceFunction>("backtrace");
                loaded.findDynamicSymbol =
                    module->TryFindFunction<PosixStackTraceNativeAPI::FindDynamicSymbolFunction>("dladdr");
                return loaded;
            }();
            return api;
        }

        int CapturePosixStackBackTrace(void** frames, int count) {
            const auto capture = PosixStackTraceAPI().captureStackBackTrace;
            return capture != nullptr ? capture(frames, count) : 0;
        }

        int FindPosixDynamicSymbol(const void* address, DynamicSymbolInfo* info) {
            const auto find = PosixStackTraceAPI().findDynamicSymbol;
            if (find == nullptr || info == nullptr) {
                return 0;
            }
            Dl_info nativeInfo{};
            const int result = find(address, &nativeInfo);
            if (result != 0) {
                *info = {
                    .fileName = nativeInfo.dli_fname,
                    .fileBase = nativeInfo.dli_fbase,
                    .symbolName = nativeInfo.dli_sname,
                    .symbolAddress = nativeInfo.dli_saddr,
                };
            }
            return result;
        }

        /** @brief Convert a native POSIX @c timeval to a checked nanosecond count. */
        [[nodiscard]] bool TimeValueNanoseconds(const timeval& value, uint64_t* output) noexcept {
            constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000;
            if (output == nullptr || value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1'000'000) {
                return false;
            }
            const uint64_t seconds = static_cast<uint64_t>(value.tv_sec);
            const uint64_t nanoseconds = static_cast<uint64_t>(value.tv_usec) * 1'000;
            if (seconds > (std::numeric_limits<uint64_t>::max() - nanoseconds) / kNanosecondsPerSecond) {
                return false;
            }
            *output = seconds * kNanosecondsPerSecond + nanoseconds;
            return true;
        }

#    if defined(PLATFORM_LINUX)
        [[nodiscard]] bool LinuxResidentMemory(uint64_t* output) noexcept {
            if (output == nullptr) {
                return false;
            }
            const FileSystemAPI& file = LoadFileSystemAPI();
            if (file.systemConfiguration == nullptr || file.open == nullptr || file.read == nullptr ||
                file.close == nullptr) {
                return false;
            }

            const int descriptor = file.open("/proc/self/statm", O_RDONLY);
            if (descriptor < 0) {
                return false;
            }
            std::array<char, 128> text{};
            Sora::ssize_t size = 0;
            do {
                size = file.read(descriptor, text.data(), text.size());
            } while (size < 0 && errno == EINTR);
            const bool closed = file.close(descriptor) == 0;
            if (size <= 0 || !closed) {
                return false;
            }

            const char* current = text.data();
            const char* end = current + size;
            uint64_t virtualPages = 0;
            auto parsed = std::from_chars(current, end, virtualPages);
            if (parsed.ec != std::errc{}) {
                return false;
            }
            current = parsed.ptr;
            while (current != end && *current == ' ') {
                ++current;
            }
            uint64_t residentPages = 0;
            parsed = std::from_chars(current, end, residentPages);
            const long pageSize = file.systemConfiguration(_SC_PAGESIZE);
            if (parsed.ec != std::errc{} || pageSize <= 0 ||
                residentPages > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(pageSize)) {
                return false;
            }
            *output = residentPages * static_cast<uint64_t>(pageSize);
            return true;
        }
#    endif

        [[nodiscard]] bool CapturePosixProcessUsage(ProcessUsageCounters* output) noexcept {
            if (output == nullptr) {
                return false;
            }
            const ProcessSystemAPI& api = LoadProcessSystemAPI();
            if (api.getResourceUsage == nullptr) {
                return false;
            }

            rusage usage{};
            if (api.getResourceUsage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
                return false;
            }
            uint64_t userNanoseconds = 0;
            uint64_t kernelNanoseconds = 0;
            if (!TimeValueNanoseconds(usage.ru_utime, &userNanoseconds) ||
                !TimeValueNanoseconds(usage.ru_stime, &kernelNanoseconds)) {
                return false;
            }

            uint64_t residentBytes = 0;
#    if defined(PLATFORM_LINUX)
            if (!LinuxResidentMemory(&residentBytes) ||
                static_cast<uint64_t>(usage.ru_maxrss) > std::numeric_limits<uint64_t>::max() / 1024) {
                return false;
            }
            uint64_t peakResidentBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#    else
            if (api.getProcessId == nullptr || api.processResourceUsage == nullptr) {
                return false;
            }
            rusage_info_v2 nativeUsage{};
            if (api.processResourceUsage(api.getProcessId(), RUSAGE_INFO_V2,
                                         reinterpret_cast<rusage_info_t*>(&nativeUsage)) != 0) {
                return false;
            }
            residentBytes = nativeUsage.ri_resident_size;
            uint64_t peakResidentBytes = static_cast<uint64_t>(usage.ru_maxrss);
#    endif
            if (peakResidentBytes < residentBytes) {
                peakResidentBytes = residentBytes;
            }
            *output = {
                .userCpuNanoseconds = userNanoseconds,
                .kernelCpuNanoseconds = kernelNanoseconds,
                .residentMemoryBytes = residentBytes,
                .peakResidentMemoryBytes = peakResidentBytes,
            };
            return true;
        }

        // clang-format off
        static_assert(std::is_convertible_v<decltype(&::getpid),    ProcessSystemAPI::GetProcessIdFunction>);
        static_assert(std::is_convertible_v<decltype(&::getppid),   ProcessSystemAPI::GetParentProcessIdFunction>);
        static_assert(std::is_convertible_v<decltype(&::getrusage), ProcessSystemAPI::GetResourceUsageFunction>);
#    if defined(PLATFORM_MACOS)
        static_assert(std::is_convertible_v<decltype(&::_NSGetExecutablePath), ProcessSystemAPI::GetExecutablePathFunction>);
        static_assert(std::is_convertible_v<decltype(&::_NSGetArgc),           ProcessSystemAPI::GetArgumentCountFunction>);
        static_assert(std::is_convertible_v<decltype(&::_NSGetArgv),           ProcessSystemAPI::GetArgumentVectorFunction>);
        static_assert(std::is_convertible_v<decltype(&::proc_pid_rusage),      ProcessSystemAPI::ProcessResourceUsageFunction>);
#    endif
        static_assert(std::is_convertible_v<decltype(&::open),          FileSystemAPI::OpenFunction>);
        static_assert(std::is_convertible_v<decltype(&::close),         FileSystemAPI::CloseFunction>);
        static_assert(std::is_convertible_v<decltype(&::read),          FileSystemAPI::ReadFunction>);
        static_assert(std::is_convertible_v<decltype(&::write),         FileSystemAPI::WriteFunction>);
        static_assert(std::is_convertible_v<decltype(&::pread),         FileSystemAPI::ReadAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::pwrite),        FileSystemAPI::WriteAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::fsync),         FileSystemAPI::SyncFunction>);
        static_assert(std::is_convertible_v<decltype(&::ftruncate),     FileSystemAPI::ResizeFunction>);
        static_assert(std::is_convertible_v<decltype(&::fstat),         FileSystemAPI::StatFunction>);
        static_assert(std::is_convertible_v<decltype(&::mmap),          FileSystemAPI::MapFunction>);
        static_assert(std::is_convertible_v<decltype(&::munmap),        FileSystemAPI::UnmapFunction>);
        static_assert(std::is_convertible_v<decltype(&::msync),         FileSystemAPI::SyncMapFunction>);
        static_assert(std::is_convertible_v<decltype(&::madvise),       FileSystemAPI::AdviseMapFunction>);
        static_assert(std::is_convertible_v<decltype(&::rename),        FileSystemAPI::RenameFunction>);
        static_assert(std::is_convertible_v<decltype(&::unlink),        FileSystemAPI::UnlinkFunction>);
        static_assert(std::is_convertible_v<decltype(&::fcntl),         FileSystemAPI::ControlFunction>);
        static_assert(std::is_convertible_v<decltype(&::posix_fadvise), FileSystemAPI::AdviseFileFunction>);

        static_assert(std::is_convertible_v<decltype(&::sigaction), CrashSystemAPI::SignalActionFunction>);
        static_assert(std::is_convertible_v<decltype(&::raise),     CrashSystemAPI::RaiseSignalFunction>);
        static_assert(std::is_convertible_v<decltype(&::_exit),     CrashSystemAPI::ImmediateExitFunction>);

        static_assert(sizeof(PosixSystem::SignalSet*)        == sizeof(sigset_t*));
        static_assert(sizeof(PosixSystem::ThreadId)          == sizeof(pthread_t));
        static_assert(sizeof(PosixSystem::ThreadAttributes*) == sizeof(pthread_attr_t*));
        static_assert(sizeof(PosixSystem::FileOffset)        == sizeof(off_t));
        static_assert(sizeof(PosixSystem::PollCount)         == sizeof(nfds_t));
        
        static_assert(alignof(PosixSystem::SignalSet*)       == alignof(sigset_t*));
        static_assert(alignof(PosixSystem::ThreadId)         == alignof(pthread_t));
        static_assert(alignof(PosixSystem::FileOffset)       == alignof(off_t));
        // clang-format on
#endif

    } // namespace

    uint64_t CaptureLastSystemError() noexcept {
#if defined(PLATFORM_WINDOWS)
        return static_cast<uint64_t>(::GetLastError());
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        return std::bit_cast<uint64_t>(errno);
#else
        return 0;
#endif
    }

    const NativeErrorSystemAPI& LoadNativeErrorSystemAPI() noexcept {
        static const NativeErrorSystemAPI api = [] {
            using API = NativeErrorSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
            if (module == nullptr) {
                return loaded;
            }
            // clang-format off
            loaded.getLastError      = module->TryFindFunction<API::GetLastErrorFunction>("GetLastError");
            loaded.setLastError      = module->TryFindFunction<API::SetLastErrorFunction>("SetLastError");
            loaded.formatMessageWide = module->TryFindFunction<API::FormatMessageWideFunction>("FormatMessageW");
            loaded.localFree         = module->TryFindFunction<API::LocalFreeFunction>("LocalFree");
            // clang-format on
#endif
            return loaded;
        }();
        return api;
    }

    const EnvironmentSystemAPI& LoadEnvironmentSystemAPI() noexcept {
        static const EnvironmentSystemAPI api = [] {
            using API = EnvironmentSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const ModulePtr& module = CurrentProcessModule();
#endif
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (module == nullptr) {
                return loaded;
            }
#endif
#if defined(PLATFORM_WINDOWS)
            // clang-format off
            loaded.getLastError                = module->TryFindFunction<API::GetLastErrorFunction>("GetLastError");
            loaded.setLastError                = module->TryFindFunction<API::SetLastErrorFunction>("SetLastError");
            loaded.getEnvironmentVariableWide  = module->TryFindFunction<API::GetEnvironmentVariableWideFunction>("GetEnvironmentVariableW");
            loaded.setEnvironmentVariableWide  = module->TryFindFunction<API::SetEnvironmentVariableWideFunction>("SetEnvironmentVariableW");
            loaded.getEnvironmentStringsWide   = module->TryFindFunction<API::GetEnvironmentStringsWideFunction>("GetEnvironmentStringsW");
            loaded.freeEnvironmentStringsWide  = module->TryFindFunction<API::FreeEnvironmentStringsWideFunction>("FreeEnvironmentStringsW");
            // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            // clang-format off
            loaded.get    = module->TryFindFunction<API::GetFunction>("getenv");
            loaded.set    = module->TryFindFunction<API::SetFunction>("setenv");
            loaded.remove = module->TryFindFunction<API::RemoveFunction>("unsetenv");
            // clang-format on
#endif
            return loaded;
        }();
        return api;
    }

    const ProcessSystemAPI& LoadProcessSystemAPI() noexcept {
        static const ProcessSystemAPI api = [] {
            using API = ProcessSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
            if (module != nullptr) {
                // clang-format off
                loaded.getCurrentProcessId           = module->TryFindFunction<API::GetCurrentProcessIdFunction>("GetCurrentProcessId");
                loaded.getCurrentProcess             = module->TryFindFunction<API::GetCurrentProcessFunction>("GetCurrentProcess");
                loaded.createProcessSnapshot         = module->TryFindFunction<API::CreateProcessSnapshotFunction>("CreateToolhelp32Snapshot");
                loaded.firstProcess                  = module->TryFindFunction<API::ReadProcessEntryFunction>("Process32FirstW");
                loaded.nextProcess                   = module->TryFindFunction<API::ReadProcessEntryFunction>("Process32NextW");
                loaded.closeHandle                   = module->TryFindFunction<API::CloseHandleFunction>("CloseHandle");
                loaded.queryFullProcessImageNameWide = module->TryFindFunction<API::QueryFullProcessImageNameWideFunction>("QueryFullProcessImageNameW");
                loaded.getCommandLineWide            = module->TryFindFunction<API::GetCommandLineWideFunction>("GetCommandLineW");
                loaded.localFree                     = module->TryFindFunction<API::LocalFreeFunction>("LocalFree");
                loaded.getProcessTimes               = module->TryFindFunction<API::GetProcessTimesFunction>("GetProcessTimes");
                loaded.getProcessMemoryInfo          = module->TryFindFunction<API::GetProcessMemoryInfoFunction>("K32GetProcessMemoryInfo");
                // clang-format on
            }
            if (loaded.getCurrentProcessId != nullptr && loaded.createProcessSnapshot != nullptr &&
                loaded.firstProcess != nullptr && loaded.nextProcess != nullptr && loaded.closeHandle != nullptr) {
                loaded.queryParentProcessId = QueryWindowsParentProcessId;
            }
            if (loaded.getCurrentProcess != nullptr && loaded.getProcessTimes != nullptr &&
                loaded.getProcessMemoryInfo != nullptr) {
                loaded.captureUsage = CaptureWindowsProcessUsage;
            }

            const ModulePtr& shell = ShellModule();
            if (shell != nullptr) {
                loaded.commandLineToArgvWide =
                    shell->TryFindFunction<ProcessSystemAPI::CommandLineToArgvWideFunction>("CommandLineToArgvW");
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const ModulePtr& module = CurrentProcessModule();
            if (module != nullptr) {
                loaded.getProcessId = module->TryFindFunction<ProcessSystemAPI::GetProcessIdFunction>("getpid");
                loaded.getParentProcessId =
                    module->TryFindFunction<ProcessSystemAPI::GetParentProcessIdFunction>("getppid");
                loaded.getResourceUsage =
                    module->TryFindFunction<ProcessSystemAPI::GetResourceUsageFunction>("getrusage");
#    if defined(PLATFORM_MACOS)
                loaded.getExecutablePath =
                    module->TryFindFunction<ProcessSystemAPI::GetExecutablePathFunction>("_NSGetExecutablePath");
                loaded.getArgumentCount =
                    module->TryFindFunction<ProcessSystemAPI::GetArgumentCountFunction>("_NSGetArgc");
                loaded.getArgumentVector =
                    module->TryFindFunction<ProcessSystemAPI::GetArgumentVectorFunction>("_NSGetArgv");
                loaded.processResourceUsage =
                    module->TryFindFunction<ProcessSystemAPI::ProcessResourceUsageFunction>("proc_pid_rusage");
#    endif
            }
#    if defined(PLATFORM_LINUX)
            if (loaded.getResourceUsage != nullptr && LoadFileSystemAPI().systemConfiguration != nullptr) {
                loaded.captureUsage = CapturePosixProcessUsage;
            }
#    else
            if (loaded.getResourceUsage != nullptr && loaded.getProcessId != nullptr &&
                loaded.processResourceUsage != nullptr) {
                loaded.captureUsage = CapturePosixProcessUsage;
            }
#    endif
#endif
            return loaded;
        }();
        return api;
    }

    const ThreadSystemAPI& LoadThreadSystemAPI() noexcept {
        static const ThreadSystemAPI api = [] {
            using API = ThreadSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const ModulePtr& module = CurrentProcessModule();
#endif
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (module == nullptr) {
                return loaded;
            }
#endif
#if defined(PLATFORM_WINDOWS)
            // clang-format off
            loaded.getCurrentThreadId          = module->TryFindFunction<API::GetCurrentThreadIdFunction>("GetCurrentThreadId");
            loaded.getCurrentThread            = module->TryFindFunction<API::GetCurrentThreadFunction>("GetCurrentThread");
            loaded.setThreadDescription        = module->TryFindFunction<API::SetThreadDescriptionFunction>("SetThreadDescription");
            loaded.getThreadDescription        = module->TryFindFunction<API::GetThreadDescriptionFunction>("GetThreadDescription");
            loaded.localFree                   = module->TryFindFunction<API::LocalFreeFunction>("LocalFree");
            loaded.getCurrentProcessorNumberEx = module->TryFindFunction<API::GetCurrentProcessorNumberExFunction>("GetCurrentProcessorNumberEx");
            loaded.getCurrentThreadStackLimits = module->TryFindFunction<API::GetCurrentThreadStackLimitsFunction>("GetCurrentThreadStackLimits");
            // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            // clang-format off
            loaded.pthreadSelf                 = module->TryFindFunction<API::PthreadSelfFunction>("pthread_self");
            loaded.pthreadGetName              = module->TryFindFunction<API::PthreadGetNameFunction>("pthread_getname_np");
            // clang-format on
#    if defined(PLATFORM_LINUX)
            // clang-format off
            loaded.systemCall                  = module->TryFindFunction<API::SystemCallFunction>("syscall");
            loaded.scheduleGetCpu              = module->TryFindFunction<API::ScheduleGetCpuFunction>("sched_getcpu");
            loaded.pthreadSetName              = module->TryFindFunction<API::PthreadSetNameFunction>("pthread_setname_np");
            loaded.pthreadGetAttributes        = module->TryFindFunction<API::PthreadGetAttributesFunction>("pthread_getattr_np");
            loaded.pthreadDestroyAttributes    = module->TryFindFunction<API::PthreadDestroyAttributesFunction>("pthread_attr_destroy");
            loaded.pthreadGetStack             = module->TryFindFunction<API::PthreadGetStackFunction>("pthread_attr_getstack");
            // clang-format on
#    else
            // clang-format off
            loaded.pthreadThreadId             = module->TryFindFunction<API::PthreadThreadIdFunction>("pthread_threadid_np");
            loaded.pthreadSetName              = module->TryFindFunction<API::PthreadSetNameFunction>("pthread_setname_np");
            loaded.pthreadGetStackAddress      = module->TryFindFunction<API::PthreadGetStackAddressFunction>("pthread_get_stackaddr_np");
            loaded.pthreadGetStackSize         = module->TryFindFunction<API::PthreadGetStackSizeFunction>("pthread_get_stacksize_np");
            // clang-format on
#    endif
#endif
            return loaded;
        }();
        return api;
    }

    const GlobalMemorySystemAPI& LoadGlobalMemorySystemAPI() noexcept {
        static const GlobalMemorySystemAPI api = [] {
            using API = GlobalMemorySystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
            if (module == nullptr) {
                return loaded;
            }

            // clang-format off
            loaded.globalAllocate = module->TryFindFunction<API::GlobalAllocateFunction>("GlobalAlloc");
            loaded.globalFree     = module->TryFindFunction<API::GlobalFreeFunction>("GlobalFree");
            loaded.globalLock     = module->TryFindFunction<API::GlobalLockFunction>("GlobalLock");
            loaded.globalUnlock   = module->TryFindFunction<API::GlobalUnlockFunction>("GlobalUnlock");
            loaded.globalSize     = module->TryFindFunction<API::GlobalSizeFunction>("GlobalSize");
            // clang-format on
#endif
            return loaded;
        }();
        return api;
    }

    const ClipboardSystemAPI& LoadClipboardSystemAPI() noexcept {
        static const ClipboardSystemAPI api = [] {
            using API = ClipboardSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = ClipboardModule();
            if (module == nullptr) {
                return loaded;
            }

            // clang-format off
            loaded.openClipboard              = module->TryFindFunction<API::OpenClipboardFunction>("OpenClipboard");
            loaded.closeClipboard             = module->TryFindFunction<API::CloseClipboardFunction>("CloseClipboard");
            loaded.emptyClipboard             = module->TryFindFunction<API::EmptyClipboardFunction>("EmptyClipboard");
            loaded.getClipboardData           = module->TryFindFunction<API::GetClipboardDataFunction>("GetClipboardData");
            loaded.setClipboardData           = module->TryFindFunction<API::SetClipboardDataFunction>("SetClipboardData");
            loaded.isClipboardFormatAvailable = module->TryFindFunction<API::IsClipboardFormatAvailableFunction>("IsClipboardFormatAvailable");
            // clang-format on
#endif
            return loaded;
        }();
        return api;
    }

    const FileSystemAPI& LoadFileSystemAPI() noexcept {
        static const FileSystemAPI api = [] {
            using API = FileSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const ModulePtr& module = CurrentProcessModule();
#endif
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (module == nullptr) {
                return loaded;
            }
#endif
#if defined(PLATFORM_WINDOWS)
            // clang-format off
            loaded.getStandardHandle     = module->TryFindFunction<API::GetStandardHandleFunction>("GetStdHandle");
            loaded.createFileWide        = module->TryFindFunction<API::CreateFileWideFunction>("CreateFileW");
            loaded.closeHandle           = module->TryFindFunction<API::CloseHandleFunction>("CloseHandle");
            loaded.readFile              = module->TryFindFunction<API::ReadFileFunction>("ReadFile");
            loaded.writeFile             = module->TryFindFunction<API::WriteFileFunction>("WriteFile");
            loaded.flushFileBuffers      = module->TryFindFunction<API::FlushFileBuffersFunction>("FlushFileBuffers");
            loaded.getFileSize           = module->TryFindFunction<API::GetFileSizeFunction>("GetFileSizeEx");
            loaded.setFileInformation    = module->TryFindFunction<API::SetFileInformationFunction>("SetFileInformationByHandle");
            loaded.getFileInformation    = module->TryFindFunction<API::GetFileInformationFunction>("GetFileInformationByHandleEx");
            loaded.createFileMappingWide = module->TryFindFunction<API::CreateFileMappingWideFunction>("CreateFileMappingW");
            loaded.mapViewOfFile         = module->TryFindFunction<API::MapViewOfFileFunction>("MapViewOfFile");
            loaded.unmapViewOfFile       = module->TryFindFunction<API::UnmapViewOfFileFunction>("UnmapViewOfFile");
            loaded.flushViewOfFile       = module->TryFindFunction<API::FlushViewOfFileFunction>("FlushViewOfFile");
            loaded.replaceFileWide       = module->TryFindFunction<API::ReplaceFileWideFunction>("ReplaceFileW");
            loaded.moveFileWide          = module->TryFindFunction<API::MoveFileWideFunction>("MoveFileExW");
            loaded.deleteFileWide        = module->TryFindFunction<API::DeleteFileWideFunction>("DeleteFileW");
            loaded.createEventWide       = module->TryFindFunction<API::CreateEventWideFunction>("CreateEventW");
            loaded.resetEvent            = module->TryFindFunction<API::ResetEventFunction>("ResetEvent");
            loaded.waitForSingleObject   = module->TryFindFunction<API::WaitForSingleObjectFunction>("WaitForSingleObject");
            loaded.getOverlappedResult   = module->TryFindFunction<API::GetOverlappedResultFunction>("GetOverlappedResult");
            loaded.cancelIo              = module->TryFindFunction<API::CancelIoFunction>("CancelIoEx");
            loaded.readDirectoryChanges  = module->TryFindFunction<API::ReadDirectoryChangesFunction>("ReadDirectoryChangesW");
            loaded.getSystemInfo         = module->TryFindFunction<API::GetSystemInfoFunction>("GetSystemInfo");
            loaded.getCurrentProcess     = module->TryFindFunction<API::GetCurrentProcessFunction>("GetCurrentProcess");
            loaded.duplicateHandle       = module->TryFindFunction<API::DuplicateHandleFunction>("DuplicateHandle");
            // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            // clang-format off
            loaded.open                  = module->TryFindFunction<API::OpenFunction>("open");
            loaded.close                 = module->TryFindFunction<API::CloseFunction>("close");
            loaded.read                  = module->TryFindFunction<API::ReadFunction>("read");
            loaded.write                 = module->TryFindFunction<API::WriteFunction>("write");
            loaded.readAt                = module->TryFindFunction<API::ReadAtFunction>("pread");
            loaded.writeAt               = module->TryFindFunction<API::WriteAtFunction>("pwrite");
            loaded.sync                  = module->TryFindFunction<API::SyncFunction>("fsync");
            loaded.resize                = module->TryFindFunction<API::ResizeFunction>("ftruncate");
            loaded.stat                  = module->TryFindFunction<API::StatFunction>("fstat");
            loaded.map                   = module->TryFindFunction<API::MapFunction>("mmap");
            loaded.unmap                 = module->TryFindFunction<API::UnmapFunction>("munmap");
            loaded.syncMap               = module->TryFindFunction<API::SyncMapFunction>("msync");
            loaded.adviseMap             = module->TryFindFunction<API::AdviseMapFunction>("madvise");
            loaded.rename                = module->TryFindFunction<API::RenameFunction>("rename");
            loaded.unlink                = module->TryFindFunction<API::UnlinkFunction>("unlink");
            loaded.control               = module->TryFindFunction<API::ControlFunction>("fcntl");
            loaded.systemConfiguration   = module->TryFindFunction<API::SystemConfigurationFunction>("sysconf");
            loaded.duplicate             = module->TryFindFunction<API::DuplicateFunction>("dup");
            loaded.adviseFile            = module->TryFindFunction<API::AdviseFileFunction>("posix_fadvise");
#    if defined(PLATFORM_LINUX)
            loaded.initializeNotify      = module->TryFindFunction<API::InitializeNotifyFunction>("inotify_init1");
            loaded.addNotify             = module->TryFindFunction<API::AddNotifyFunction>("inotify_add_watch");
            loaded.removeNotify          = module->TryFindFunction<API::RemoveNotifyFunction>("inotify_rm_watch");
            loaded.poll                  = module->TryFindFunction<API::PollFunction>("poll");
#    elif defined(PLATFORM_MACOS)
            loaded.createQueue           = module->TryFindFunction<API::CreateQueueFunction>("kqueue");
            loaded.queueEvent            = module->TryFindFunction<API::QueueEventFunction>("kevent");
            // clang-format on
#    endif
#endif
            return loaded;
        }();
        return api;
    }

    const CrashSystemAPI& LoadCrashSystemAPI() noexcept {
        static const CrashSystemAPI api = [] {
            using API = CrashSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const ModulePtr& module = CurrentProcessModule();
#endif
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (module == nullptr) {
                return loaded;
            }
#endif
            // clang-format off
#if defined(PLATFORM_WINDOWS)
            loaded.setUnhandledExceptionFilter = module->TryFindFunction<API::SetUnhandledExceptionFilterFunction>("SetUnhandledExceptionFilter");
            loaded.getErrorMode                = module->TryFindFunction<API::GetErrorModeFunction>("GetErrorMode");
            loaded.setErrorMode                = module->TryFindFunction<API::SetErrorModeFunction>("SetErrorMode");
            loaded.getCurrentProcess           = module->TryFindFunction<API::GetCurrentProcessFunction>("GetCurrentProcess");
            loaded.werGetFlags                 = module->TryFindFunction<API::WerGetFlagsFunction>("WerGetFlags");
            loaded.werSetFlags                 = module->TryFindFunction<API::WerSetFlagsFunction>("WerSetFlags");
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            loaded.signalAction   = module->TryFindFunction<API::SignalActionFunction>("sigaction");
            loaded.emptySignalSet = module->TryFindFunction<API::EmptySignalSetFunction>("sigemptyset");
            loaded.raiseSignal    = module->TryFindFunction<API::RaiseSignalFunction>("raise");
            loaded.immediateExit  = module->TryFindFunction<API::ImmediateExitFunction>("_exit");
#endif
            // clang-format on
            return loaded;
        }();
        return api;
    }

    static const DbgHelpSystemAPI& LoadDbgHelpSystemAPI() noexcept {
        static const DbgHelpSystemAPI api = [] {
            using API = DbgHelpSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = DbgHelpModule();
            if (module == nullptr) {
                return loaded;
            }
            // clang-format off
            loaded.symSetOptions         = module->TryFindFunction<API::SymSetOptionsFunction>("SymSetOptions");
            loaded.symInitialize         = module->TryFindFunction<API::SymInitializeFunction>("SymInitialize");
            loaded.symFromAddress        = module->TryFindFunction<API::SymFromAddressFunction>("SymFromAddr");
            loaded.symGetLineFromAddress = module->TryFindFunction<API::SymGetLineFromAddressFunction>("SymGetLineFromAddr64");
            loaded.symGetModuleInfo      = module->TryFindFunction<API::SymGetModuleInfoFunction>("SymGetModuleInfo64");
            loaded.undecorateSymbolName  = module->TryFindFunction<API::UndecorateSymbolNameFunction>("UnDecorateSymbolName");
            // clang-format on
#endif
            return loaded;
        }();
        return api;
    }

    LockedDbgHelpSystemAPI LockDbgHelpSystemAPI() {
        static std::mutex mutex;
        return LockedDbgHelpSystemAPI{mutex, LoadDbgHelpSystemAPI()};
    }

    bool InitializeDbgHelpSystemAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
        static std::once_flag flag;
        static bool initialized = false;
        try {
            std::call_once(flag, [] {
                LockedDbgHelpSystemAPI dbgHelp = LockDbgHelpSystemAPI();
                const StackTraceSystemAPI& stackTrace = LoadStackTraceSystemAPI();
                if (dbgHelp->symSetOptions == nullptr || dbgHelp->symInitialize == nullptr ||
                    stackTrace.getCurrentProcess == nullptr) {
                    return;
                }
                dbgHelp->symSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                initialized = dbgHelp->symInitialize(stackTrace.getCurrentProcess(), nullptr, TRUE) != FALSE;
            });
        } catch (...) {
            return false;
        }
        return initialized;
#else
        return false;
#endif
    }

    const StackTraceSystemAPI& LoadStackTraceSystemAPI() noexcept {
        static const StackTraceSystemAPI api = [] {
            using API = StackTraceSystemAPI;
            API loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
            if (module == nullptr) {
                return loaded;
            }

            // clang-format off
            loaded.captureStackBackTrace = module->TryFindFunction<API::CaptureStackBackTraceFunction>("RtlCaptureStackBackTrace");
            loaded.getCurrentProcess     = module->TryFindFunction<API::GetCurrentProcessFunction>("GetCurrentProcess");
            // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const PosixStackTraceNativeAPI& native = PosixStackTraceAPI();
            // clang-format off
            loaded.captureStackBackTrace = native.captureStackBackTrace != nullptr ? &CapturePosixStackBackTrace : nullptr;
            loaded.findDynamicSymbol     = native.findDynamicSymbol != nullptr ? &FindPosixDynamicSymbol : nullptr;
            // clang-format on
#endif
            return loaded;
        }();
        return api;
    }

} // namespace Sora::PAL
