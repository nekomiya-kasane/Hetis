/**
 * @file SystemAPI.cpp
 * @brief Bootstrap and initialize immutable dynamically resolved system API tables.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/PAL/Module.h>

#include <cerrno>
#include <mutex>
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
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#    include <cstdio>
#    include <dlfcn.h>
#    include <execinfo.h>
#    include <fcntl.h>
#    include <poll.h>
#    include <pthread.h>
#    include <signal.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
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

        [[nodiscard]] const ModulePtr& DbgHelpModule() noexcept {
            static const ModulePtr module = LoadSystemModule("dbghelp.dll");
            return module;
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
        static_assert(
            std::is_same_v<DbgHelpSystemAPI::SymGetLineFromAddressFunction, decltype(&::SymGetLineFromAddr64)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymGetModuleInfoFunction, decltype(&::SymGetModuleInfo64)>);
        static_assert(
            std::is_same_v<DbgHelpSystemAPI::UndecorateSymbolNameFunction, decltype(&::UnDecorateSymbolName)>);

        static_assert(
            std::is_same_v<StackTraceSystemAPI::CaptureStackBackTraceFunction, decltype(&::RtlCaptureStackBackTrace)>);
        static_assert(std::is_same_v<StackTraceSystemAPI::GetCurrentProcessFunction, decltype(&::GetCurrentProcess)>);

        static_assert(std::is_same_v<CrashSystemAPI::SetUnhandledExceptionFilterFunction,
                                     decltype(&::SetUnhandledExceptionFilter)>);

        static_assert(sizeof(WindowsSystem::GlobalMemory) == sizeof(HGLOBAL));
        static_assert(alignof(WindowsSystem::GlobalMemory) == alignof(HGLOBAL));
        static_assert(sizeof(WindowsSystem::Handle) == sizeof(HWND));
        static_assert(alignof(WindowsSystem::Handle) == alignof(HWND));

        static_assert(std::is_same_v<ClipboardSystemAPI::CloseClipboardFunction, decltype(&::CloseClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::EmptyClipboardFunction, decltype(&::EmptyClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::GetClipboardDataFunction, decltype(&::GetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::SetClipboardDataFunction, decltype(&::SetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::IsClipboardFormatAvailableFunction,
                                     decltype(&::IsClipboardFormatAvailable)>);

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

        static_assert(std::is_convertible_v<decltype(&::open), FileSystemAPI::OpenFunction>);
        static_assert(std::is_convertible_v<decltype(&::sigaction), CrashSystemAPI::SignalActionFunction>);
        static_assert(sizeof(PosixSystem::SignalSet*) == sizeof(sigset_t*));
        static_assert(alignof(PosixSystem::SignalSet*) == alignof(sigset_t*));
        static_assert(std::is_convertible_v<decltype(&::raise), CrashSystemAPI::RaiseSignalFunction>);
        static_assert(std::is_convertible_v<decltype(&::_exit), CrashSystemAPI::ImmediateExitFunction>);
        static_assert(std::is_convertible_v<decltype(&::close), FileSystemAPI::CloseFunction>);
        static_assert(std::is_convertible_v<decltype(&::read), FileSystemAPI::ReadFunction>);
        static_assert(std::is_convertible_v<decltype(&::write), FileSystemAPI::WriteFunction>);
        static_assert(std::is_convertible_v<decltype(&::pread), FileSystemAPI::ReadAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::pwrite), FileSystemAPI::WriteAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::fsync), FileSystemAPI::SyncFunction>);
        static_assert(std::is_convertible_v<decltype(&::ftruncate), FileSystemAPI::ResizeFunction>);
        static_assert(std::is_convertible_v<decltype(&::fstat), FileSystemAPI::StatFunction>);
        static_assert(std::is_convertible_v<decltype(&::mmap), FileSystemAPI::MapFunction>);
        static_assert(std::is_convertible_v<decltype(&::munmap), FileSystemAPI::UnmapFunction>);
        static_assert(std::is_convertible_v<decltype(&::msync), FileSystemAPI::SyncMapFunction>);
        static_assert(std::is_convertible_v<decltype(&::madvise), FileSystemAPI::AdviseMapFunction>);
        static_assert(std::is_convertible_v<decltype(&::rename), FileSystemAPI::RenameFunction>);
        static_assert(std::is_convertible_v<decltype(&::unlink), FileSystemAPI::UnlinkFunction>);
        static_assert(std::is_convertible_v<decltype(&::fcntl), FileSystemAPI::ControlFunction>);
        static_assert(std::is_convertible_v<decltype(&::getpid), FileSystemAPI::GetProcessIdFunction>);
        static_assert(std::is_convertible_v<decltype(&::posix_fadvise), FileSystemAPI::AdviseFileFunction>);
        static_assert(sizeof(PosixSystem::ThreadId) == sizeof(pthread_t));
        static_assert(alignof(PosixSystem::ThreadId) == alignof(pthread_t));
        static_assert(sizeof(PosixSystem::ThreadAttributes*) == sizeof(pthread_attr_t*));
        static_assert(sizeof(PosixSystem::FileOffset) == sizeof(off_t));
        static_assert(alignof(PosixSystem::FileOffset) == alignof(off_t));
        static_assert(sizeof(PosixSystem::PollCount) == sizeof(nfds_t));
#endif

    } // namespace

    int CaptureLastSystemError() noexcept {
#if defined(PLATFORM_WINDOWS)
        return static_cast<int>(::GetLastError());
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        return errno;
#else
        return 0;
#endif
    }

    const NativeErrorSystemAPI& LoadNativeErrorSystemAPI() noexcept {
        static const NativeErrorSystemAPI api = [] {
            NativeErrorSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
            if (module == nullptr) {
                return loaded;
            }
            loaded.getLastError = module->TryFindFunction<NativeErrorSystemAPI::GetLastErrorFunction>("GetLastError");
            loaded.setLastError = module->TryFindFunction<NativeErrorSystemAPI::SetLastErrorFunction>("SetLastError");
            loaded.formatMessageWide =
                module->TryFindFunction<NativeErrorSystemAPI::FormatMessageWideFunction>("FormatMessageW");
            loaded.localFree = module->TryFindFunction<NativeErrorSystemAPI::LocalFreeFunction>("LocalFree");
#endif
            return loaded;
        }();
        return api;
    }

    const EnvironmentSystemAPI& LoadEnvironmentSystemAPI() noexcept {
        static const EnvironmentSystemAPI api = [] {
            EnvironmentSystemAPI loaded{};
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
            loaded.getLastError = module->TryFindFunction<EnvironmentSystemAPI::GetLastErrorFunction>("GetLastError");
            loaded.setLastError = module->TryFindFunction<EnvironmentSystemAPI::SetLastErrorFunction>("SetLastError");
            loaded.getEnvironmentVariableWide =
                module->TryFindFunction<EnvironmentSystemAPI::GetEnvironmentVariableWideFunction>(
                    "GetEnvironmentVariableW");
            loaded.setEnvironmentVariableWide =
                module->TryFindFunction<EnvironmentSystemAPI::SetEnvironmentVariableWideFunction>(
                    "SetEnvironmentVariableW");
            loaded.getEnvironmentStringsWide =
                module->TryFindFunction<EnvironmentSystemAPI::GetEnvironmentStringsWideFunction>(
                    "GetEnvironmentStringsW");
            loaded.freeEnvironmentStringsWide =
                module->TryFindFunction<EnvironmentSystemAPI::FreeEnvironmentStringsWideFunction>(
                    "FreeEnvironmentStringsW");
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            loaded.get = module->TryFindFunction<EnvironmentSystemAPI::GetFunction>("getenv");
            loaded.set = module->TryFindFunction<EnvironmentSystemAPI::SetFunction>("setenv");
            loaded.remove = module->TryFindFunction<EnvironmentSystemAPI::RemoveFunction>("unsetenv");
#endif
            return loaded;
        }();
        return api;
    }

    const ThreadSystemAPI& LoadThreadSystemAPI() noexcept {
        static const ThreadSystemAPI api = [] {
            ThreadSystemAPI loaded{};
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
            loaded.getCurrentThreadId =
                module->TryFindFunction<ThreadSystemAPI::GetCurrentThreadIdFunction>("GetCurrentThreadId");
            loaded.getCurrentThread =
                module->TryFindFunction<ThreadSystemAPI::GetCurrentThreadFunction>("GetCurrentThread");
            loaded.setThreadDescription =
                module->TryFindFunction<ThreadSystemAPI::SetThreadDescriptionFunction>("SetThreadDescription");
            loaded.getThreadDescription =
                module->TryFindFunction<ThreadSystemAPI::GetThreadDescriptionFunction>("GetThreadDescription");
            loaded.localFree = module->TryFindFunction<ThreadSystemAPI::LocalFreeFunction>("LocalFree");
            loaded.getCurrentProcessorNumberEx =
                module->TryFindFunction<ThreadSystemAPI::GetCurrentProcessorNumberExFunction>(
                    "GetCurrentProcessorNumberEx");
            loaded.getCurrentThreadStackLimits =
                module->TryFindFunction<ThreadSystemAPI::GetCurrentThreadStackLimitsFunction>(
                    "GetCurrentThreadStackLimits");
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            loaded.pthreadSelf = module->TryFindFunction<ThreadSystemAPI::PthreadSelfFunction>("pthread_self");
            loaded.pthreadGetName =
                module->TryFindFunction<ThreadSystemAPI::PthreadGetNameFunction>("pthread_getname_np");
#    if defined(PLATFORM_LINUX)
            loaded.systemCall = module->TryFindFunction<ThreadSystemAPI::SystemCallFunction>("syscall");
            loaded.scheduleGetCpu = module->TryFindFunction<ThreadSystemAPI::ScheduleGetCpuFunction>("sched_getcpu");
            loaded.pthreadSetName =
                module->TryFindFunction<ThreadSystemAPI::PthreadSetNameFunction>("pthread_setname_np");
            loaded.pthreadGetAttributes =
                module->TryFindFunction<ThreadSystemAPI::PthreadGetAttributesFunction>("pthread_getattr_np");
            loaded.pthreadDestroyAttributes =
                module->TryFindFunction<ThreadSystemAPI::PthreadDestroyAttributesFunction>("pthread_attr_destroy");
            loaded.pthreadGetStack =
                module->TryFindFunction<ThreadSystemAPI::PthreadGetStackFunction>("pthread_attr_getstack");
#    else
            loaded.pthreadThreadId =
                module->TryFindFunction<ThreadSystemAPI::PthreadThreadIdFunction>("pthread_threadid_np");
            loaded.pthreadSetName =
                module->TryFindFunction<ThreadSystemAPI::PthreadSetNameFunction>("pthread_setname_np");
            loaded.pthreadGetStackAddress =
                module->TryFindFunction<ThreadSystemAPI::PthreadGetStackAddressFunction>("pthread_get_stackaddr_np");
            loaded.pthreadGetStackSize =
                module->TryFindFunction<ThreadSystemAPI::PthreadGetStackSizeFunction>("pthread_get_stacksize_np");
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
            loaded.openClipboard             = module->TryFindFunction<API::OpenClipboardFunction>("OpenClipboard");
            loaded.closeClipboard            = module->TryFindFunction<API::CloseClipboardFunction>("CloseClipboard");
            loaded.emptyClipboard            = module->TryFindFunction<API::EmptyClipboardFunction>("EmptyClipboard");
            loaded.getClipboardData =
                module->TryFindFunction<API::GetClipboardDataFunction>("GetClipboardData");
            loaded.setClipboardData =
                module->TryFindFunction<API::SetClipboardDataFunction>("SetClipboardData");
            loaded.isClipboardFormatAvailable =
                module->TryFindFunction<API::IsClipboardFormatAvailableFunction>("IsClipboardFormatAvailable");
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
            loaded.setFileInformation =
                module->TryFindFunction<API::SetFileInformationFunction>("SetFileInformationByHandle");
            loaded.getFileInformation =
                module->TryFindFunction<API::GetFileInformationFunction>("GetFileInformationByHandleEx");
            loaded.createFileMappingWide =
                module->TryFindFunction<API::CreateFileMappingWideFunction>("CreateFileMappingW");
            loaded.mapViewOfFile         = module->TryFindFunction<API::MapViewOfFileFunction>("MapViewOfFile");
            loaded.unmapViewOfFile       = module->TryFindFunction<API::UnmapViewOfFileFunction>("UnmapViewOfFile");
            loaded.flushViewOfFile       = module->TryFindFunction<API::FlushViewOfFileFunction>("FlushViewOfFile");
            loaded.replaceFileWide       = module->TryFindFunction<API::ReplaceFileWideFunction>("ReplaceFileW");
            loaded.moveFileWide          = module->TryFindFunction<API::MoveFileWideFunction>("MoveFileExW");
            loaded.deleteFileWide        = module->TryFindFunction<API::DeleteFileWideFunction>("DeleteFileW");
            loaded.createEventWide       = module->TryFindFunction<API::CreateEventWideFunction>("CreateEventW");
            loaded.resetEvent            = module->TryFindFunction<API::ResetEventFunction>("ResetEvent");
            loaded.waitForSingleObject =
                module->TryFindFunction<API::WaitForSingleObjectFunction>("WaitForSingleObject");
            loaded.getOverlappedResult =
                module->TryFindFunction<API::GetOverlappedResultFunction>("GetOverlappedResult");
            loaded.cancelIo              = module->TryFindFunction<API::CancelIoFunction>("CancelIoEx");
            loaded.readDirectoryChanges =
                module->TryFindFunction<API::ReadDirectoryChangesFunction>("ReadDirectoryChangesW");
            loaded.getCurrentProcessId =
                module->TryFindFunction<API::GetCurrentProcessIdFunction>("GetCurrentProcessId");
            loaded.getSystemInfo         = module->TryFindFunction<API::GetSystemInfoFunction>("GetSystemInfo");
            loaded.getCurrentProcess     = module->TryFindFunction<API::GetCurrentProcessFunction>("GetCurrentProcess");
            loaded.duplicateHandle       = module->TryFindFunction<API::DuplicateHandleFunction>("DuplicateHandle");
            // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            // clang-format off
            loaded.open                = module->TryFindFunction<API::OpenFunction>("open");
            loaded.close               = module->TryFindFunction<API::CloseFunction>("close");
            loaded.read                = module->TryFindFunction<API::ReadFunction>("read");
            loaded.write               = module->TryFindFunction<API::WriteFunction>("write");
            loaded.readAt              = module->TryFindFunction<API::ReadAtFunction>("pread");
            loaded.writeAt             = module->TryFindFunction<API::WriteAtFunction>("pwrite");
            loaded.sync                = module->TryFindFunction<API::SyncFunction>("fsync");
            loaded.resize              = module->TryFindFunction<API::ResizeFunction>("ftruncate");
            loaded.stat                = module->TryFindFunction<API::StatFunction>("fstat");
            loaded.map                 = module->TryFindFunction<API::MapFunction>("mmap");
            loaded.unmap               = module->TryFindFunction<API::UnmapFunction>("munmap");
            loaded.syncMap             = module->TryFindFunction<API::SyncMapFunction>("msync");
            loaded.adviseMap           = module->TryFindFunction<API::AdviseMapFunction>("madvise");
            loaded.rename              = module->TryFindFunction<API::RenameFunction>("rename");
            loaded.unlink              = module->TryFindFunction<API::UnlinkFunction>("unlink");
            loaded.control             = module->TryFindFunction<API::ControlFunction>("fcntl");
            loaded.systemConfiguration = module->TryFindFunction<API::SystemConfigurationFunction>("sysconf");
            loaded.duplicate           = module->TryFindFunction<API::DuplicateFunction>("dup");
            loaded.getProcessId        = module->TryFindFunction<API::GetProcessIdFunction>("getpid");
            loaded.adviseFile          = module->TryFindFunction<API::AdviseFileFunction>("posix_fadvise");
#    if defined(PLATFORM_LINUX)
            loaded.initializeNotify    = module->TryFindFunction<API::InitializeNotifyFunction>("inotify_init1");
            loaded.addNotify           = module->TryFindFunction<API::AddNotifyFunction>("inotify_add_watch");
            loaded.removeNotify        = module->TryFindFunction<API::RemoveNotifyFunction>("inotify_rm_watch");
            loaded.poll                = module->TryFindFunction<API::PollFunction>("poll");
#    elif defined(PLATFORM_MACOS)
            loaded.createQueue         = module->TryFindFunction<API::CreateQueueFunction>("kqueue");
            loaded.queueEvent          = module->TryFindFunction<API::QueueEventFunction>("kevent");
            // clang-format on
#    endif
#endif
            return loaded;
        }();
        return api;
    }

    const CrashSystemAPI& LoadCrashSystemAPI() noexcept {
        static const CrashSystemAPI api = [] {
            CrashSystemAPI loaded{};
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
            loaded.setUnhandledExceptionFilter =
                module->TryFindFunction<CrashSystemAPI::SetUnhandledExceptionFilterFunction>(
                    "SetUnhandledExceptionFilter");
            loaded.getErrorMode = module->TryFindFunction<CrashSystemAPI::GetErrorModeFunction>("GetErrorMode");
            loaded.setErrorMode = module->TryFindFunction<CrashSystemAPI::SetErrorModeFunction>("SetErrorMode");
            loaded.getCurrentProcess =
                module->TryFindFunction<CrashSystemAPI::GetCurrentProcessFunction>("GetCurrentProcess");
            loaded.werGetFlags = module->TryFindFunction<CrashSystemAPI::WerGetFlagsFunction>("WerGetFlags");
            loaded.werSetFlags = module->TryFindFunction<CrashSystemAPI::WerSetFlagsFunction>("WerSetFlags");
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            loaded.signalAction = module->TryFindFunction<CrashSystemAPI::SignalActionFunction>("sigaction");
            loaded.emptySignalSet = module->TryFindFunction<CrashSystemAPI::EmptySignalSetFunction>("sigemptyset");
            loaded.raiseSignal = module->TryFindFunction<CrashSystemAPI::RaiseSignalFunction>("raise");
            loaded.immediateExit = module->TryFindFunction<CrashSystemAPI::ImmediateExitFunction>("_exit");
#endif
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
            loaded.symGetLineFromAddress =
                module->TryFindFunction<API::SymGetLineFromAddressFunction>("SymGetLineFromAddr64");
            loaded.symGetModuleInfo =
                module->TryFindFunction<API::SymGetModuleInfoFunction>("SymGetModuleInfo64");
            loaded.undecorateSymbolName =
                module->TryFindFunction<API::UndecorateSymbolNameFunction>("UnDecorateSymbolName");
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
            StackTraceSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
            const ModulePtr& module = KernelModule();
            if (module == nullptr) {
                return loaded;
            }
            loaded.captureStackBackTrace =
                module->TryFindFunction<StackTraceSystemAPI::CaptureStackBackTraceFunction>("RtlCaptureStackBackTrace");
            loaded.getCurrentProcess =
                module->TryFindFunction<StackTraceSystemAPI::GetCurrentProcessFunction>("GetCurrentProcess");
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const PosixStackTraceNativeAPI& native = PosixStackTraceAPI();
            loaded.captureStackBackTrace =
                native.captureStackBackTrace != nullptr ? &CapturePosixStackBackTrace : nullptr;
            loaded.findDynamicSymbol = native.findDynamicSymbol != nullptr ? &FindPosixDynamicSymbol : nullptr;
#endif
            return loaded;
        }();
        return api;
    }

} // namespace Sora::PAL
