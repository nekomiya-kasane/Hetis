/**
 * @file SystemAPI.cpp
 * @brief Bootstrap and initialize immutable dynamically resolved system API tables.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/PAL/ComAPI.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(PLATFORM_WINDOWS)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <Windows.h>
#    include <bcrypt.h>
#    include <iphlpapi.h>
#    include <ioringapi.h>
#    include <objbase.h>
#    include <coml2api.h>
#    include <PropIdl.h>
#    include <ole2.h>
#    include <DocObj.h>
#    include <oleauto.h>
#    include <werapi.h>
#    include <DbgHelp.h>
#    include <Psapi.h>
#    include <Shellapi.h>
#    include <TlHelp32.h>
#    include <evntrace.h>
#    include <evntcons.h>
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#    include <cstdio>
#    include <dlfcn.h>
#    include <execinfo.h>
#    include <fcntl.h>
#    include <ifaddrs.h>
#    include <poll.h>
#    include <sys/file.h>
#    include <sys/ioctl.h>
#    include <ucontext.h>
#    include <pthread.h>
#    include <signal.h>
#    include <spawn.h>
#    include <sys/mman.h>
#    include <sys/resource.h>
#    include <sys/socket.h>
#    include <sys/stat.h>
#    include <sys/statvfs.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <netdb.h>
#    include <net/if.h>
#    include <unistd.h>
#    if defined(PLATFORM_MACOS)
#        include <sys/sysctl.h>
#    endif
#    if defined(PLATFORM_LINUX)
#        if defined(__aarch64__) || defined(__riscv)
#            include <asm/hwcap.h>
#        endif
#        include <sys/auxv.h>
#        include <sys/epoll.h>
#        include <sys/eventfd.h>
#        include <sys/fanotify.h>
#        include <sys/inotify.h>
#        include <sys/prctl.h>
#        include <sys/ptrace.h>
#        include <linux/ptrace.h>
#        include <sys/sendfile.h>
#        include <sys/signalfd.h>
#        include <sys/syscall.h>
#        include <sys/timerfd.h>
#    elif defined(PLATFORM_MACOS)
#        include <libproc.h>
#        include <mach-o/dyld.h>
#    endif
#endif

#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/Unicode.h>

namespace Sora::PAL {

    namespace {

        /** @brief Load one optional native module for the process lifetime. */
        [[nodiscard]] ModulePtr LoadOptionalModule(std::initializer_list<std::string_view> names) noexcept {
            try {
                constexpr ModuleLoadOptions options{
                    .nameKind = ModuleNameKind::Stem,
                    .candidatePolicy = ModuleCandidatePolicy::NativeDecorated,
                    .cachePolicy = ModuleCachePolicy::Shared,
                    .unloadOnDestroy = false,
                };
                Result<ModulePtr> loaded = LoadModule(names, options);
                return loaded ? std::move(*loaded) : nullptr;
            } catch (...) {
                return nullptr;
            }
        }

#if defined(PLATFORM_WINDOWS)
        WindowsSystem::Handle LoadWindowsLibrary(const wchar_t* name) noexcept {
            return reinterpret_cast<WindowsSystem::Handle>(::LoadLibraryW(name));
        }

        WindowsSystem::Handle GetWindowsModuleHandle(const wchar_t* name) noexcept {
            return reinterpret_cast<WindowsSystem::Handle>(::GetModuleHandleW(name));
        }

        void* FindWindowsSymbol(WindowsSystem::Handle module, const char* name) noexcept {
            return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(module), name));
        }

        WindowsSystem::Bool FreeWindowsLibrary(WindowsSystem::Handle module) noexcept {
            return ::FreeLibrary(reinterpret_cast<HMODULE>(module));
        }

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

        [[nodiscard]] const ModulePtr& Kernel32Module() noexcept {
            static const ModulePtr module = LoadSystemModule("kernel32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& Ole32Module() noexcept {
            static const ModulePtr module = LoadSystemModule("ole32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& OleAut32Module() noexcept {
            static const ModulePtr module = LoadSystemModule("oleaut32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& User32Module() noexcept {
            static const ModulePtr module = LoadSystemModule("user32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& ShcoreModule() noexcept {
            static const ModulePtr module = LoadSystemModule("shcore.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& Shell32Module() noexcept {
            static const ModulePtr module = LoadSystemModule("shell32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& DbgHelpModule() noexcept {
            static const ModulePtr module = LoadSystemModule("dbghelp.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& Advapi32Module() noexcept {
            static const ModulePtr module = LoadSystemModule("advapi32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& Ws2_32Module() noexcept {
            static const ModulePtr module = LoadSystemModule("ws2_32.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& BCryptModule() noexcept {
            static const ModulePtr module = LoadSystemModule("bcrypt.dll");
            return module;
        }

        [[nodiscard]] const ModulePtr& IphlpapiModule() noexcept {
            static const ModulePtr module = LoadSystemModule("iphlpapi.dll");
            return module;
        }

        inline constexpr GUID kKernelProcessProvider{
            0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
        inline constexpr GUID kKernelThreadProvider{
            0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
        inline constexpr GUID kSoraSchedulerSessionGuid{
            0xe837d88f, 0x5a6f, 0x4f8b, {0x9d, 0x2f, 0x62, 0xd3, 0x73, 0xb8, 0x7d, 0x26}};
        inline constexpr wchar_t kSoraSchedulerSessionName[] = L"Sora Scheduler Trace";

        [[nodiscard]] ErrorCode ClassifyTraceStatus(ULONG status) noexcept {
            switch (status) {
                case ERROR_SUCCESS:
                    return ErrorCode::Ok;
                case ERROR_ACCESS_DENIED:
                case ERROR_PRIVILEGE_NOT_HELD:
                    return ErrorCode::PermissionDenied;
                case ERROR_ALREADY_EXISTS:
                case ERROR_BUSY:
                    return ErrorCode::ResourceBusy;
                case ERROR_NOT_SUPPORTED:
                case ERROR_CALL_NOT_IMPLEMENTED:
                    return ErrorCode::NotSupported;
                default:
                    return ErrorCode::TraceCaptureFailed;
            }
        }

        struct WindowsSchedulerTraceProperties {
            EVENT_TRACE_PROPERTIES value{};
            wchar_t loggerName[std::size(kSoraSchedulerSessionName)]{};
        };
        static_assert(std::is_standard_layout_v<WindowsSchedulerTraceProperties>);

        struct WindowsSchedulerTraceSession {
            const SchedulerTraceSystemAPI* api = nullptr;
            WindowsSchedulerTraceProperties properties{};
            TRACEHANDLE producerHandle = 0;
            std::atomic<TRACEHANDLE> consumerHandle{INVALID_PROCESSTRACE_HANDLE};
            SchedulerTraceSystemAPI::EventCallback eventCallback = nullptr;
            SchedulerTraceSystemAPI::LossCallback lossCallback = nullptr;
            void* userData = nullptr;

            [[nodiscard]] EVENT_TRACE_PROPERTIES* Properties() noexcept { return &properties.value; }

            static ULONG __stdcall BufferCallback(EVENT_TRACE_LOGFILEW* log) noexcept {
                if (log != nullptr && log->Context != nullptr) {
                    auto& session = *static_cast<WindowsSchedulerTraceSession*>(log->Context);
                    if (session.lossCallback != nullptr) {
                        session.lossCallback(log->EventsLost, session.userData);
                    }
                }
                return TRUE;
            }

            static void __stdcall EventCallback(EVENT_RECORD* event) noexcept {
                if (event == nullptr || event->UserContext == nullptr) {
                    return;
                }
                auto& session = *static_cast<WindowsSchedulerTraceSession*>(event->UserContext);
                if (session.eventCallback == nullptr || (event->UserData == nullptr && event->UserDataLength != 0)) {
                    return;
                }

                NativeSchedulerTraceProvider provider = NativeSchedulerTraceProvider::Unknown;
                if (IsEqualGUID(event->EventHeader.ProviderId, kKernelThreadProvider)) {
                    provider = NativeSchedulerTraceProvider::Thread;
                } else if (IsEqualGUID(event->EventHeader.ProviderId, kKernelProcessProvider)) {
                    provider = NativeSchedulerTraceProvider::Process;
                }
                const auto pointerWidth = (event->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0
                                              ? NativeSchedulerTracePointerWidth::Bits32
                                              : NativeSchedulerTracePointerWidth::Bits64;
                const auto* payload = static_cast<const std::byte*>(event->UserData);
                const NativeSchedulerTraceRecord record{
                    .provider = provider,
                    .opcode = event->EventHeader.EventDescriptor.Opcode,
                    .pointerWidth = pointerWidth,
                    .timestampTicks = event->EventHeader.TimeStamp.QuadPart,
                    .processorIndex = event->BufferContext.ProcessorIndex,
                    .payload = std::span<const std::byte>{payload, event->UserDataLength},
                };
                session.eventCallback(record, session.userData);
            }
        };

        [[nodiscard]] ErrorCode StartWindowsSchedulerTrace(const NativeSchedulerTraceOptions& options,
                                                           SchedulerTraceSystemAPI::EventCallback eventCallback,
                                                           SchedulerTraceSystemAPI::LossCallback lossCallback,
                                                           void* userData,
                                                           SchedulerTraceSystemAPI::SessionHandle* output,
                                                           uint64_t* ticksPerSecond) noexcept {
            if (output == nullptr || ticksPerSecond == nullptr || eventCallback == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            *output = nullptr;
            *ticksPerSecond = 0;

            const SchedulerTraceSystemAPI& api = LoadSchedulerTraceSystemAPI();
            auto session
                = std::unique_ptr<WindowsSchedulerTraceSession>{new (std::nothrow) WindowsSchedulerTraceSession};
            if (session == nullptr) {
                return ErrorCode::OutOfMemory;
            }
            session->api = &api;
            session->eventCallback = eventCallback;
            session->lossCallback = lossCallback;
            session->userData = userData;
            EVENT_TRACE_PROPERTIES& properties = *session->Properties();
            properties.Wnode.BufferSize = static_cast<ULONG>(sizeof(session->properties));
            properties.Wnode.Guid = kSoraSchedulerSessionGuid;
            properties.Wnode.ClientContext = 1;
            properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            properties.BufferSize = options.bufferSizeKilobytes;
            properties.MinimumBuffers = options.minimumBuffers;
            properties.MaximumBuffers = options.maximumBuffers;
            properties.FlushTimer = 1;
            properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
            properties.EnableFlags = EVENT_TRACE_FLAG_PROCESS
                                     | EVENT_TRACE_FLAG_THREAD
                                     | EVENT_TRACE_FLAG_CSWITCH
                                     | EVENT_TRACE_FLAG_DISPATCHER;
            properties.LoggerNameOffset = static_cast<ULONG>(offsetof(WindowsSchedulerTraceProperties, loggerName));
            std::memcpy(session->properties.loggerName, kSoraSchedulerSessionName, sizeof(kSoraSchedulerSessionName));

            LARGE_INTEGER frequency{};
            if (api.queryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0) {
                return ErrorCode::TraceCaptureFailed;
            }
            *ticksPerSecond = static_cast<uint64_t>(frequency.QuadPart);

            ULONG status
                = api.startTrace(&session->producerHandle, session->properties.loggerName, session->Properties());
            if (status != ERROR_SUCCESS) {
                return ClassifyTraceStatus(status);
            }

            EVENT_TRACE_LOGFILEW log{};
            log.LoggerName = session->properties.loggerName;
            log.ProcessTraceMode
                = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
            log.EventRecordCallback = &WindowsSchedulerTraceSession::EventCallback;
            log.BufferCallback = &WindowsSchedulerTraceSession::BufferCallback;
            log.Context = session.get();
            const TRACEHANDLE consumerHandle = api.openTrace(&log);
            session->consumerHandle.store(consumerHandle, std::memory_order_release);
            if (consumerHandle == INVALID_PROCESSTRACE_HANDLE) {
                status = ::GetLastError();
                api.controlTrace(session->producerHandle, session->properties.loggerName, session->Properties(),
                                 EVENT_TRACE_CONTROL_STOP);
                session->producerHandle = 0;
                return ClassifyTraceStatus(status);
            }

            *output = session.release();
            return ErrorCode::Ok;
        }

        [[nodiscard]] ErrorCode ProcessWindowsSchedulerTrace(SchedulerTraceSystemAPI::SessionHandle handle) noexcept {
            if (handle == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            auto& session = *static_cast<WindowsSchedulerTraceSession*>(handle);
            TRACEHANDLE consumerHandle = session.consumerHandle.load(std::memory_order_acquire);
            const ULONG status = session.api->processTrace(&consumerHandle, 1, nullptr, nullptr);
            return status == ERROR_CANCELLED ? ErrorCode::Ok : ClassifyTraceStatus(status);
        }

        [[nodiscard]] ErrorCode StopWindowsSchedulerTrace(SchedulerTraceSystemAPI::SessionHandle handle,
                                                          NativeSchedulerTraceLosses* losses) noexcept {
            if (handle == nullptr || losses == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            auto& session = *static_cast<WindowsSchedulerTraceSession*>(handle);
            ULONG status = ERROR_SUCCESS;
            if (session.producerHandle != 0) {
                status = session.api->controlTrace(session.producerHandle, session.properties.loggerName,
                                                   session.Properties(), EVENT_TRACE_CONTROL_STOP);
                session.producerHandle = 0;
            }
            const EVENT_TRACE_PROPERTIES& properties = *session.Properties();
            *losses = {
                .eventsLost = properties.EventsLost,
                .logBuffersLost = properties.LogBuffersLost,
                .realtimeBuffersLost = properties.RealTimeBuffersLost,
            };
            if (status == ERROR_WMI_INSTANCE_NOT_FOUND) {
                status = ERROR_SUCCESS;
            } else if (status != ERROR_SUCCESS) {
                const TRACEHANDLE consumerHandle
                    = session.consumerHandle.exchange(INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel);
                if (consumerHandle != INVALID_PROCESSTRACE_HANDLE) {
                    session.api->closeTrace(consumerHandle);
                }
            }
            return ClassifyTraceStatus(status);
        }

        void CloseWindowsSchedulerTrace(SchedulerTraceSystemAPI::SessionHandle handle) noexcept {
            std::unique_ptr<WindowsSchedulerTraceSession> session{static_cast<WindowsSchedulerTraceSession*>(handle)};
            if (session == nullptr) {
                return;
            }
            if (session->producerHandle != 0) {
                session->api->controlTrace(session->producerHandle, session->properties.loggerName,
                                           session->Properties(), EVENT_TRACE_CONTROL_STOP);
            }
            const TRACEHANDLE consumerHandle
                = session->consumerHandle.exchange(INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel);
            if (consumerHandle != INVALID_PROCESSTRACE_HANDLE) {
                session->api->closeTrace(consumerHandle);
            }
        }

        [[nodiscard]] bool QueryWindowsParentProcessId(WindowsSystem::DWord* parent) noexcept {
            if (parent == nullptr) {
                return false;
            }
            const ProcessSystemAPI& api = LoadProcessSystemAPI();
            const HandleSystemAPI& handles = LoadHandleSystemAPI();
            if (!EnsureSystemAPIs(api.getCurrentProcessId, api.createProcessSnapshot, api.firstProcess, api.nextProcess,
                                  handles.closeHandle)) {
                return false;
            }

            WindowsSystem::Handle snapshot = api.createProcessSnapshot(TH32CS_SNAPPROCESS, 0);
            if (WindowsSystem::IsInvalidHandle(snapshot)) {
                return false;
            }
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            const WindowsSystem::DWord current = api.getCurrentProcessId();
            bool result = false;
            for (WindowsSystem::Bool found = api.firstProcess(snapshot, &entry); found != WindowsSystem::kFalse;
                 found = api.nextProcess(snapshot, &entry)) {
                if (entry.th32ProcessID == current) {
                    *parent = entry.th32ParentProcessID;
                    result = true;
                    break;
                }
            }
            handles.closeHandle(snapshot);
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
            if (!EnsureSystemAPIs(api.getCurrentProcess, api.getProcessTimes, api.getProcessMemoryInfo)) {
                return false;
            }

            WindowsSystem::Handle process = api.getCurrentProcess();
            FILETIME created{}, exited{}, kernel{}, user{};
            PROCESS_MEMORY_COUNTERS memory{};
            memory.cb = sizeof(memory);
            if (api.getProcessTimes(process, &created, &exited, &kernel, &user)
                == FALSE
                || api.getProcessMemoryInfo(process, &memory, sizeof(memory))
                == FALSE) {
                return false;
            }

            constexpr uint64_t kNanosecondsPerTick = 100;
            const uint64_t userTicks = FileTimeTicks(user);
            const uint64_t kernelTicks = FileTimeTicks(kernel);
            if (userTicks
                > std::numeric_limits<uint64_t>::max()
                / kNanosecondsPerTick
                || kernelTicks
                > std::numeric_limits<uint64_t>::max()
                / kNanosecondsPerTick) {
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
        using NativeSystemInfoFunction = void(__stdcall*)(SYSTEM_INFO*);
        static_assert(std::is_same_v<NativeSystemInfoFunction, decltype(&::GetSystemInfo)>);
        static_assert(std::is_same_v<NativeSystemInfoFunction, decltype(&::GetNativeSystemInfo)>);
        static_assert(sizeof(WindowsSystem::ProcessorNumber)                        == sizeof(PROCESSOR_NUMBER));
        static_assert(alignof(WindowsSystem::ProcessorNumber)                       == alignof(PROCESSOR_NUMBER));
        static_assert(sizeof(WindowsSystem::Overlapped)                             == sizeof(OVERLAPPED));
        static_assert(alignof(WindowsSystem::Overlapped)                            == alignof(OVERLAPPED));
        static_assert(offsetof(WindowsSystem::Overlapped, internal)                 == offsetof(OVERLAPPED, Internal));
        static_assert(offsetof(WindowsSystem::Overlapped, internalHigh)             == offsetof(OVERLAPPED, InternalHigh));
        static_assert(sizeof(WindowsSystem::FileNotifyInformationHeader)            == offsetof(FILE_NOTIFY_INFORMATION, FileName));
        static_assert(alignof(WindowsSystem::FileNotifyInformationHeader)           == alignof(FILE_NOTIFY_INFORMATION));
        static_assert(sizeof(WindowsSystem::SystemInfo)                             == sizeof(SYSTEM_INFO));
        static_assert(alignof(WindowsSystem::SystemInfo)                            == alignof(SYSTEM_INFO));
        static_assert(offsetof(WindowsSystem::SystemInfo, allocationGranularity)    == offsetof(SYSTEM_INFO, dwAllocationGranularity));
        static_assert(sizeof(WindowsSystem::MemoryStatus)                           == sizeof(MEMORYSTATUSEX));
        static_assert(alignof(WindowsSystem::MemoryStatus)                          == alignof(MEMORYSTATUSEX));
        static_assert(offsetof(WindowsSystem::MemoryStatus, totalPhysicalBytes)     == offsetof(MEMORYSTATUSEX, ullTotalPhys));
        static_assert(offsetof(WindowsSystem::MemoryStatus, availablePhysicalBytes) == offsetof(MEMORYSTATUSEX, ullAvailPhys));
        static_assert(sizeof(WindowsSystem::FileStorageInfo)                        == sizeof(FILE_STORAGE_INFO));
        static_assert(alignof(WindowsSystem::FileStorageInfo)                       == alignof(FILE_STORAGE_INFO));
        static_assert(sizeof(WindowsSystem::LargeInteger)                           == sizeof(LARGE_INTEGER));
        static_assert(alignof(WindowsSystem::LargeInteger)                          == alignof(LARGE_INTEGER));
        static_assert(sizeof(WindowsSystem::FileEndOfFileInformation)               == sizeof(FILE_END_OF_FILE_INFO));
        static_assert(WindowsSystem::kFileStorageInformation                        == FileStorageInfo);
        static_assert(WindowsSystem::kFileEndOfFileInformation                      == FileEndOfFileInfo);
        static_assert(WindowsSystem::kFileListDirectory                             == FILE_LIST_DIRECTORY);
        static_assert(WindowsSystem::kNotifyFileName                                == FILE_NOTIFY_CHANGE_FILE_NAME);
        static_assert(WindowsSystem::kNotifyDirectoryName                           == FILE_NOTIFY_CHANGE_DIR_NAME);
        static_assert(WindowsSystem::kNotifySize                                    == FILE_NOTIFY_CHANGE_SIZE);
        static_assert(WindowsSystem::kNotifyLastWrite                               == FILE_NOTIFY_CHANGE_LAST_WRITE);
        static_assert(WindowsSystem::kNotifyCreation                                == FILE_NOTIFY_CHANGE_CREATION);
        static_assert(WindowsSystem::kNotifySecurity                                == FILE_NOTIFY_CHANGE_SECURITY);
        static_assert(WindowsSystem::kActionAdded                                   == FILE_ACTION_ADDED);
        static_assert(WindowsSystem::kActionRemoved                                 == FILE_ACTION_REMOVED);
        static_assert(WindowsSystem::kActionModified                                == FILE_ACTION_MODIFIED);
        static_assert(WindowsSystem::kActionRenamedOld                              == FILE_ACTION_RENAMED_OLD_NAME);
        static_assert(WindowsSystem::kActionRenamedNew                              == FILE_ACTION_RENAMED_NEW_NAME);
        static_assert(WindowsSystem::kErrorIoPending                                == ERROR_IO_PENDING);
        static_assert(WindowsSystem::kInfinite                                      == INFINITE);
        static_assert(WindowsSystem::kWaitObject                                    == WAIT_OBJECT_0);
        static_assert(WindowsSystem::kWaitTimeout                                   == WAIT_TIMEOUT);
        static_assert(WindowsSystem::kSnapshotThreads                               == TH32CS_SNAPTHREAD);
        static_assert(WindowsSystem::kArmNeonInstructionsAvailable                  == PF_ARM_NEON_INSTRUCTIONS_AVAILABLE);
        static_assert(sizeof(WindowsSystem::StartupInfo)                            == sizeof(STARTUPINFOW));
        static_assert(alignof(WindowsSystem::StartupInfo)                           == alignof(STARTUPINFOW));
        static_assert(offsetof(WindowsSystem::StartupInfo, flags)                   == offsetof(STARTUPINFOW, dwFlags));
        static_assert(offsetof(WindowsSystem::StartupInfo, standardError)           == offsetof(STARTUPINFOW, hStdError));
        static_assert(sizeof(WindowsSystem::ProcessInformation)                     == sizeof(PROCESS_INFORMATION));
        static_assert(alignof(WindowsSystem::ProcessInformation)                    == alignof(PROCESS_INFORMATION));
        static_assert(offsetof(WindowsSystem::ProcessInformation, processId)        == offsetof(PROCESS_INFORMATION, dwProcessId));
        static_assert(WindowsSystem::kCreateNoWindow                                == CREATE_NO_WINDOW);
        static_assert(WindowsSystem::kStartUseStandardHandles                       == STARTF_USESTDHANDLES);
        static_assert(WindowsSystem::kFailCriticalErrors                            == SEM_FAILCRITICALERRORS);
        static_assert(WindowsSystem::kNoGeneralProtectionBox                        == SEM_NOGPFAULTERRORBOX);
        static_assert(WindowsSystem::kExceptionNoncontinuable                       == EXCEPTION_NONCONTINUABLE);
        static_assert(WindowsSystem::kAccessViolation                               == EXCEPTION_ACCESS_VIOLATION);
        static_assert(sizeof(WindowsSystem::SocketRuntimeDataStorage)               == sizeof(WSADATA));
        static_assert(alignof(WindowsSystem::SocketRuntimeDataStorage)              == alignof(WSADATA));
        static_assert(SocketSystemAPI::kNameInfoNumericHost                         == NI_NUMERICHOST);
        static_assert(SocketSystemAPI::kNameInfoNumericService                      == NI_NUMERICSERV);
        static_assert(SocketSystemAPI::kInvalidArgumentError                        == WSAEINVAL);

        static_assert(std::is_same_v<WindowsSystem::Bool, BOOL>);
        static_assert(std::is_same_v<WindowsSystem::Byte, BYTE>);
        static_assert(std::is_same_v<WindowsSystem::Char, CHAR>);
        static_assert(std::is_same_v<WindowsSystem::DWord, DWORD>);
        static_assert(std::is_same_v<WindowsSystem::DWord64, DWORD64>);
        static_assert(std::is_same_v<WindowsSystem::Float, FLOAT>);
        static_assert(std::is_same_v<WindowsSystem::Handle, HANDLE>);
        static_assert(std::is_same_v<WindowsSystem::HResult, HRESULT>);
        static_assert(std::is_same_v<WindowsSystem::Int, INT>);
        static_assert(std::is_same_v<WindowsSystem::Int8, INT8>);
        static_assert(std::is_same_v<WindowsSystem::Int16, INT16>);
        static_assert(std::is_same_v<WindowsSystem::Int32, INT32>);
        static_assert(std::is_same_v<WindowsSystem::Int64, INT64>);
        static_assert(std::is_same_v<WindowsSystem::Long, LONG>);
        static_assert(sizeof(WindowsSystem::Rect)           == sizeof(RECT));
        static_assert(alignof(WindowsSystem::Rect)          == alignof(RECT));
        static_assert(offsetof(WindowsSystem::Rect, left)   == offsetof(RECT, left));
        static_assert(offsetof(WindowsSystem::Rect, bottom) == offsetof(RECT, bottom));
        static_assert(sizeof(WindowsSystem::Point)          == sizeof(POINT));
        static_assert(alignof(WindowsSystem::Point)         == alignof(POINT));
        static_assert(offsetof(WindowsSystem::Point, x)     == offsetof(POINT, x));
        static_assert(offsetof(WindowsSystem::Point, y)     == offsetof(POINT, y));
        static_assert(std::is_same_v<WindowsSystem::PointerSizedLong, LONG_PTR>);
        static_assert(std::is_same_v<WindowsSystem::ConstCharPointer, LPCSTR>);
        static_assert(std::is_same_v<WindowsSystem::ConstVoidPointer, LPCVOID>);
        static_assert(std::is_same_v<WindowsSystem::ConstWideCharPointer, LPCWSTR>);
        static_assert(std::is_same_v<WindowsSystem::CharPointer, LPSTR>);
        static_assert(std::is_same_v<WindowsSystem::VoidPointer, LPVOID>);
        static_assert(std::is_same_v<WindowsSystem::WideCharPointer, LPWSTR>);
        static_assert(sizeof(WindowsSystem::WindowHandle)         == sizeof(HWND));
        static_assert(alignof(WindowsSystem::WindowHandle)        == alignof(HWND));
        static_assert(sizeof(WindowsSystem::ModuleHandle)         == sizeof(HMODULE));
        static_assert(alignof(WindowsSystem::ModuleHandle)        == alignof(HMODULE));
        static_assert(sizeof(WindowsSystem::MonitorHandle)        == sizeof(HMONITOR));
        static_assert(alignof(WindowsSystem::MonitorHandle)       == alignof(HMONITOR));
        static_assert(sizeof(WindowsSystem::DeviceContextHandle)  == sizeof(HDC));
        static_assert(alignof(WindowsSystem::DeviceContextHandle) == alignof(HDC));
        static_assert(std::is_same_v<WindowsSystem::Size, SIZE_T>);
        static_assert(std::is_same_v<WindowsSystem::UInt, UINT>);
        static_assert(std::is_same_v<WindowsSystem::UInt8, UINT8>);
        static_assert(std::is_same_v<WindowsSystem::UInt16, UINT16>);
        static_assert(std::is_same_v<WindowsSystem::UInt32, UINT32>);
        static_assert(std::is_same_v<WindowsSystem::UInt64, UINT64>);
        static_assert(std::is_same_v<WindowsSystem::ULong, ULONG>);
        static_assert(std::is_same_v<WindowsSystem::PointerSizedULong, ULONG_PTR>);
        static_assert(std::is_same_v<WindowsSystem::WChar, WCHAR>);
        static_assert(sizeof(WindowsSystem::Guid)                                == sizeof(GUID));
        static_assert(alignof(WindowsSystem::Guid)                               == alignof(GUID));
        static_assert(offsetof(WindowsSystem::Guid, data1)                       == offsetof(GUID, Data1));
        static_assert(offsetof(WindowsSystem::Guid, data4)                       == offsetof(GUID, Data4));
        static_assert(sizeof(WindowsSystem::Luid)                                == sizeof(LUID));
        static_assert(alignof(WindowsSystem::Luid)                               == alignof(LUID));
        static_assert(offsetof(WindowsSystem::Luid, highPart)                    == offsetof(LUID, HighPart));
        static_assert(sizeof(WindowsSystem::SecurityAttributes)                  == sizeof(SECURITY_ATTRIBUTES));
        static_assert(alignof(WindowsSystem::SecurityAttributes)                 == alignof(SECURITY_ATTRIBUTES));
        static_assert(offsetof(WindowsSystem::SecurityAttributes, inheritHandle) == offsetof(SECURITY_ATTRIBUTES, bInheritHandle));
        static_assert(sizeof(WindowsSystem::FileTime)                            == sizeof(FILETIME));
        static_assert(alignof(WindowsSystem::FileTime)                           == alignof(FILETIME));
        static_assert(sizeof(WindowsSystem::Unknown)                             == sizeof(void*));
        static_assert(alignof(WindowsSystem::Unknown)                            == alignof(void*));
        static_assert(sizeof(WindowsSystem::SequentialStream)                    == sizeof(void*));
        static_assert(alignof(WindowsSystem::SequentialStream)                   == alignof(void*));
        static_assert(sizeof(WindowsSystem::ThreadEntry)                         == sizeof(THREADENTRY32));
        static_assert(alignof(WindowsSystem::ThreadEntry)                        == alignof(THREADENTRY32));
        static_assert(offsetof(WindowsSystem::ThreadEntry, size)                 == offsetof(THREADENTRY32, dwSize));
        static_assert(offsetof(WindowsSystem::ThreadEntry, usageCount)           == offsetof(THREADENTRY32, cntUsage));
        static_assert(offsetof(WindowsSystem::ThreadEntry, threadId)             == offsetof(THREADENTRY32, th32ThreadID));
        static_assert(offsetof(WindowsSystem::ThreadEntry, ownerProcessId)       == offsetof(THREADENTRY32, th32OwnerProcessID));
        static_assert(offsetof(WindowsSystem::ThreadEntry, basePriority)         == offsetof(THREADENTRY32, tpBasePri));
        static_assert(offsetof(WindowsSystem::ThreadEntry, priorityDelta)        == offsetof(THREADENTRY32, tpDeltaPri));
        static_assert(offsetof(WindowsSystem::ThreadEntry, flags)                == offsetof(THREADENTRY32, dwFlags));
        static_assert(sizeof(WindowsSystem::WindowClassEx)                       == sizeof(WNDCLASSEXW));
        static_assert(alignof(WindowsSystem::WindowClassEx)                      == alignof(WNDCLASSEXW));
        static_assert(offsetof(WindowsSystem::WindowClassEx, size)               == offsetof(WNDCLASSEXW, cbSize));
        static_assert(offsetof(WindowsSystem::WindowClassEx, style)              == offsetof(WNDCLASSEXW, style));
        static_assert(offsetof(WindowsSystem::WindowClassEx, windowProcedure)    == offsetof(WNDCLASSEXW, lpfnWndProc));
        static_assert(offsetof(WindowsSystem::WindowClassEx, className)          == offsetof(WNDCLASSEXW, lpszClassName));
        static_assert(sizeof(WindowsSystem::WindowMessage)                       == sizeof(MSG));
        static_assert(alignof(WindowsSystem::WindowMessage)                      == alignof(MSG));
        static_assert(offsetof(WindowsSystem::WindowMessage, window)             == offsetof(MSG, hwnd));
        static_assert(offsetof(WindowsSystem::WindowMessage, message)            == offsetof(MSG, message));
        static_assert(offsetof(WindowsSystem::WindowMessage, wParam)             == offsetof(MSG, wParam));
        static_assert(offsetof(WindowsSystem::WindowMessage, lParam)             == offsetof(MSG, lParam));
        static_assert(offsetof(WindowsSystem::WindowMessage, time)               == offsetof(MSG, time));
        static_assert(offsetof(WindowsSystem::WindowMessage, point)              == offsetof(MSG, pt));
        static_assert(sizeof(WindowsSystem::PollDescriptor)                      == sizeof(WSAPOLLFD));
        static_assert(alignof(WindowsSystem::PollDescriptor)                     == alignof(WSAPOLLFD));
        static_assert(std::is_same_v<SocketSystemAPI::StartupFunction,                          decltype(&::WSAStartup)>);
        static_assert(std::is_same_v<SocketSystemAPI::CleanupFunction,                          decltype(&::WSACleanup)>);
        static_assert(std::is_same_v<SocketSystemAPI::GetLastErrorFunction,                     decltype(&::WSAGetLastError)>);
        static_assert(std::is_same_v<SocketSystemAPI::SetLastErrorFunction,                     decltype(&::WSASetLastError)>);
        static_assert(std::is_same_v<SocketSystemAPI::SocketFunction,                           decltype(&::socket)>);
        static_assert(std::is_same_v<SocketSystemAPI::CloseFunction,                            decltype(&::closesocket)>);
        static_assert(std::is_same_v<SocketSystemAPI::BindFunction,                             decltype(&::bind)>);
        static_assert(std::is_same_v<SocketSystemAPI::NameFunction,                             decltype(&::getsockname)>);
        static_assert(std::is_same_v<SocketSystemAPI::OptionFunction,                           decltype(&::setsockopt)>);
        static_assert(std::is_same_v<SocketSystemAPI::GetOptionFunction,                        decltype(&::getsockopt)>);
        static_assert(std::is_same_v<SocketSystemAPI::IoctlFunction,                            decltype(&::ioctlsocket)>);
        static_assert(std::is_same_v<SocketSystemAPI::GetAddressInfoFunction,                   decltype(&::getaddrinfo)>);
        static_assert(std::is_same_v<SocketSystemAPI::FreeAddressInfoFunction,                  decltype(&::freeaddrinfo)>);
        static_assert(std::is_same_v<SocketSystemAPI::GetNameInfoFunction,                      decltype(&::getnameinfo)>);
        static_assert(std::is_same_v<TcpSystemAPI::ListenFunction,                              decltype(&::listen)>);
        static_assert(std::is_same_v<TcpSystemAPI::AcceptFunction,                              decltype(&::accept)>);
        static_assert(std::is_same_v<TcpSystemAPI::ConnectFunction,                             decltype(&::connect)>);
        static_assert(std::is_same_v<TcpSystemAPI::SendFunction,                                decltype(&::send)>);
        static_assert(std::is_same_v<TcpSystemAPI::TransferFunction,                            decltype(&::recv)>);
        static_assert(std::is_same_v<TcpSystemAPI::ShutdownFunction,                            decltype(&::shutdown)>);
        static_assert(std::is_same_v<UdpSystemAPI::SendToFunction,                              decltype(&::sendto)>);
        static_assert(std::is_same_v<UdpSystemAPI::ReceiveFromFunction,                         decltype(&::recvfrom)>);
        static_assert(std::is_same_v<WinsockOverlappedSystemAPI::CreateFunction,                decltype(&::WSASocketW)>);
        static_assert(std::is_same_v<WinsockOverlappedSystemAPI::SendFunction,                  decltype(&::WSASend)>);
        static_assert(std::is_same_v<WinsockOverlappedSystemAPI::ReceiveFunction,               decltype(&::WSARecv)>);
        static_assert(std::is_same_v<WinsockOverlappedSystemAPI::SendToFunction,                decltype(&::WSASendTo)>);
        static_assert(std::is_same_v<WinsockOverlappedSystemAPI::ReceiveFromFunction,           decltype(&::WSARecvFrom)>);
        static_assert(std::is_same_v<WinsockOverlappedSystemAPI::GetResultFunction,             decltype(&::WSAGetOverlappedResult)>);
        static_assert(std::is_same_v<WinsockOverlappedSystemAPI::ControlFunction,               decltype(&::WSAIoctl)>);
        static_assert(std::is_same_v<IOCPSystemAPI::GetQueuedCompletionStatusExFunction,        decltype(&::GetQueuedCompletionStatusEx)>);
        static_assert(std::is_same_v<IOCPSystemAPI::SetFileCompletionNotificationModesFunction, decltype(&::SetFileCompletionNotificationModes)>);
        static_assert(sizeof(WindowsSystem::IoRingCreateFlags)                            == sizeof(IORING_CREATE_FLAGS));
        static_assert(sizeof(WindowsSystem::IoRingVersion)                                == sizeof(IORING_VERSION));
        static_assert(sizeof(WindowsSystem::IoRingOperation)                              == sizeof(IORING_OP_CODE));
        static_assert(sizeof(WindowsSystem::IoRingSqeFlags)                               == sizeof(IORING_SQE_FLAGS));
        static_assert(sizeof(WindowsSystem::IoRingWriteFlags)                             == sizeof(FILE_WRITE_FLAGS));
        static_assert(sizeof(WindowsSystem::IoRingFlushMode)                              == sizeof(FILE_FLUSH_MODE));
        static_assert(sizeof(WindowsSystem::IoRingCapabilities)                           == sizeof(IORING_CAPABILITIES));
        static_assert(sizeof(WindowsSystem::IoRingInfo)                                   == sizeof(IORING_INFO));
        static_assert(sizeof(WindowsSystem::IoRingHandleRef)                              == sizeof(IORING_HANDLE_REF));
        static_assert(sizeof(WindowsSystem::IoRingBufferRef)                              == sizeof(IORING_BUFFER_REF));
        static_assert(sizeof(WindowsSystem::IoRingBufferInfo)                             == sizeof(IORING_BUFFER_INFO));
        static_assert(sizeof(WindowsSystem::IoRingCompletion)                             == sizeof(IORING_CQE));
        static_assert(sizeof(WindowsSystem::IoRingHandle)                                 == sizeof(HIORING));
        static_assert(alignof(WindowsSystem::IoRingHandle)                                == alignof(HIORING));
        static_assert(offsetof(WindowsSystem::IoRingCapabilities, maxVersion)             == offsetof(IORING_CAPABILITIES, MaxVersion));
        static_assert(offsetof(WindowsSystem::IoRingCapabilities, maxSubmissionQueueSize) == offsetof(IORING_CAPABILITIES, MaxSubmissionQueueSize));
        static_assert(offsetof(WindowsSystem::IoRingCapabilities, maxCompletionQueueSize) == offsetof(IORING_CAPABILITIES, MaxCompletionQueueSize));
        static_assert(offsetof(WindowsSystem::IoRingCapabilities, featureFlags)           == offsetof(IORING_CAPABILITIES, FeatureFlags));
        static_assert(offsetof(WindowsSystem::IoRingInfo, version)                        == offsetof(IORING_INFO, IoRingVersion));
        static_assert(offsetof(WindowsSystem::IoRingInfo, flags)                          == offsetof(IORING_INFO, Flags));
        static_assert(offsetof(WindowsSystem::IoRingInfo, submissionQueueSize)            == offsetof(IORING_INFO, SubmissionQueueSize));
        static_assert(offsetof(WindowsSystem::IoRingInfo, completionQueueSize)            == offsetof(IORING_INFO, CompletionQueueSize));
        static_assert(sizeof(DisplayDpiAwareness)                                         == sizeof(DPI_AWARENESS));
        static_assert(sizeof(WindowsSystem::DpiAwarenessContext)                          == sizeof(DPI_AWARENESS_CONTEXT));
        static_assert(alignof(WindowsSystem::IoRingHandleRef)                             == alignof(IORING_HANDLE_REF));
        static_assert(alignof(WindowsSystem::IoRingBufferRef)                             == alignof(IORING_BUFFER_REF));
        static_assert(alignof(WindowsSystem::IoRingCompletion)                            == alignof(IORING_CQE));
        static_assert(offsetof(WindowsSystem::IoRingHandleRef, kind)                      == offsetof(IORING_HANDLE_REF, Kind));
        static_assert(offsetof(WindowsSystem::IoRingHandleRef, value)                     == offsetof(IORING_HANDLE_REF, Handle));
        static_assert(offsetof(WindowsSystem::IoRingBufferRef, kind)                      == offsetof(IORING_BUFFER_REF, Kind));
        static_assert(offsetof(WindowsSystem::IoRingBufferRef, value)                     == offsetof(IORING_BUFFER_REF, Buffer));

        // The public PAL uses fixed-width ABI projections so its header stays independent of the Windows SDK.
        // Keep the native declarations checked here; the layout assertions above establish projection equivalence.
        using NativeQueryIoRingCapabilitiesFunction        = HRESULT(WINAPI*)(IORING_CAPABILITIES*);
        using NativeIsIoRingOpSupportedFunction            = BOOL(WINAPI*)(HIORING, IORING_OP_CODE);
        using NativeCreateIoRingFunction                   = HRESULT(WINAPI*)(IORING_VERSION, IORING_CREATE_FLAGS, UINT32, UINT32, HIORING*);
        using NativeGetIoRingInfoFunction                  = HRESULT(WINAPI*)(HIORING, IORING_INFO*);
        using NativeSubmitIoRingFunction                   = HRESULT(WINAPI*)(HIORING, UINT32, UINT32, UINT32*);
        using NativeCloseIoRingFunction                    = HRESULT(WINAPI*)(HIORING);
        using NativePopIoRingCompletionFunction            = HRESULT(WINAPI*)(HIORING, IORING_CQE*);
        using NativeSetIoRingCompletionEventFunction       = HRESULT(WINAPI*)(HIORING, HANDLE);
        using NativeBuildIoRingCancelRequestFunction       = HRESULT(WINAPI*)(HIORING, IORING_HANDLE_REF, UINT_PTR, UINT_PTR);
        using NativeBuildIoRingReadFileFunction            = HRESULT(WINAPI*)(HIORING, IORING_HANDLE_REF, IORING_BUFFER_REF, UINT32, UINT64, UINT_PTR, IORING_SQE_FLAGS);
        using NativeBuildIoRingWriteFileFunction           = HRESULT(WINAPI*)(HIORING, IORING_HANDLE_REF, IORING_BUFFER_REF, UINT32, UINT64, FILE_WRITE_FLAGS, UINT_PTR, IORING_SQE_FLAGS);
        using NativeBuildIoRingFlushFileFunction           = HRESULT(WINAPI*)(HIORING, IORING_HANDLE_REF, FILE_FLUSH_MODE, UINT_PTR, IORING_SQE_FLAGS);
        using NativeBuildIoRingRegisterFileHandlesFunction = HRESULT(WINAPI*)(HIORING, UINT32, HANDLE const*, UINT_PTR);
        using NativeBuildIoRingRegisterBuffersFunction     = HRESULT(WINAPI*)(HIORING, UINT32, IORING_BUFFER_INFO const*, UINT_PTR);

        static_assert(std::is_same_v<NativeQueryIoRingCapabilitiesFunction,        decltype(&::QueryIoRingCapabilities)>);
        static_assert(std::is_same_v<NativeIsIoRingOpSupportedFunction,            decltype(&::IsIoRingOpSupported)>);
        static_assert(std::is_same_v<NativeCreateIoRingFunction,                   decltype(&::CreateIoRing)>);
        static_assert(std::is_same_v<NativeGetIoRingInfoFunction,                  decltype(&::GetIoRingInfo)>);
        static_assert(std::is_same_v<NativeSubmitIoRingFunction,                   decltype(&::SubmitIoRing)>);
        static_assert(std::is_same_v<NativeCloseIoRingFunction,                    decltype(&::CloseIoRing)>);
        static_assert(std::is_same_v<NativePopIoRingCompletionFunction,            decltype(&::PopIoRingCompletion)>);
        static_assert(std::is_same_v<NativeSetIoRingCompletionEventFunction,       decltype(&::SetIoRingCompletionEvent)>);
        static_assert(std::is_same_v<NativeBuildIoRingCancelRequestFunction,       decltype(&::BuildIoRingCancelRequest)>);
        static_assert(std::is_same_v<NativeBuildIoRingReadFileFunction,            decltype(&::BuildIoRingReadFile)>);
        static_assert(std::is_same_v<NativeBuildIoRingWriteFileFunction,           decltype(&::BuildIoRingWriteFile)>);
        static_assert(std::is_same_v<NativeBuildIoRingFlushFileFunction,           decltype(&::BuildIoRingFlushFile)>);
        static_assert(std::is_same_v<NativeBuildIoRingRegisterFileHandlesFunction, decltype(&::BuildIoRingRegisterFileHandles)>);
        static_assert(std::is_same_v<NativeBuildIoRingRegisterBuffersFunction,     decltype(&::BuildIoRingRegisterBuffers)>);

        static_assert(std::is_same_v<DbgHelpSystemAPI::SymSetOptionsFunction,            decltype(&::SymSetOptions)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymInitializeFunction,            decltype(&::SymInitialize)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymFromAddressFunction,           decltype(&::SymFromAddr)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymGetLineFromAddressFunction,    decltype(&::SymGetLineFromAddr64)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymGetModuleInfoFunction,         decltype(&::SymGetModuleInfo64)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::UndecorateSymbolNameFunction,     decltype(&::UnDecorateSymbolName)>);
       
        static_assert(std::is_same_v<StackTraceSystemAPI::CaptureStackBackTraceFunction, decltype(&::RtlCaptureStackBackTrace)>);

        static_assert(std::is_same_v<DebugSystemAPI::CheckRemoteDebuggerPresentFunction, decltype(&::CheckRemoteDebuggerPresent)>);
        static_assert(std::is_same_v<DebugSystemAPI::ContinueDebugEventFunction,         decltype(&::ContinueDebugEvent)>);
        static_assert(std::is_same_v<DebugSystemAPI::DebugActiveProcessFunction,         decltype(&::DebugActiveProcess)>);
        static_assert(std::is_same_v<DebugSystemAPI::DebugActiveProcessFunction,         decltype(&::DebugActiveProcessStop)>);
        static_assert(std::is_same_v<DebugSystemAPI::IsDebuggerPresentFunction,          decltype(&::IsDebuggerPresent)>);
        static_assert(std::is_same_v<DebugSystemAPI::BreakpointFunction,                 decltype(&::DebugBreak)>);
        static_assert(std::is_same_v<DebugSystemAPI::DebugBreakProcessFunction,          decltype(&::DebugBreakProcess)>);
        static_assert(std::is_same_v<DebugSystemAPI::DebugSetProcessKillOnExitFunction,  decltype(&::DebugSetProcessKillOnExit)>);
        static_assert(std::is_same_v<DebugSystemAPI::FatalExitFunction,                  decltype(&::FatalExit)>);
        static_assert(std::is_same_v<DebugSystemAPI::OutputDebugStringAnsiFunction,      decltype(&::OutputDebugStringA)>);
        static_assert(std::is_same_v<DebugSystemAPI::OutputDebugStringWideFunction,      decltype(&::OutputDebugStringW)>);
        static_assert(std::is_same_v<DebugSystemAPI::WaitForDebugEventFunction,          decltype(&::WaitForDebugEvent)>);
        static_assert(std::is_same_v<DebugSystemAPI::WaitForDebugEventFunction,          decltype(&::WaitForDebugEventEx)>);

        // Keep the COM/OLE projections aligned with the Windows SDK.  The public header intentionally uses opaque
        // PAL-owned records; these native aliases validate return types, calling conventions, and argument order here.
        using NativeCoWaitForMultipleHandlesFunction = HRESULT(WINAPI*)(DWORD, DWORD, ULONG, LPHANDLE, LPDWORD);
        using NativeCreateStreamOnHGlobalFunction    = HRESULT(WINAPI*)(HGLOBAL, BOOL, ::IStream**);
        using NativeGetHGlobalFromStreamFunction     = HRESULT(WINAPI*)(::IStream*, HGLOBAL*);
        using NativeOleCreateExFunction              = HRESULT(WINAPI*)(REFCLSID, REFIID, DWORD, DWORD, ULONG, DWORD*, LPFORMATETC, IAdviseSink*, DWORD*, LPOLECLIENTSITE, LPSTORAGE, LPVOID*);
        using NativeOleCreateFromDataExFunction      = HRESULT(WINAPI*)(LPDATAOBJECT, REFIID, DWORD, DWORD, ULONG, DWORD*, LPFORMATETC, IAdviseSink*, DWORD*, LPOLECLIENTSITE, LPSTORAGE, LPVOID*);
        using NativeOleCreateLinkExFunction          = HRESULT(WINAPI*)(LPMONIKER, REFIID, DWORD, DWORD, ULONG, DWORD*, LPFORMATETC, IAdviseSink*, DWORD*, LPOLECLIENTSITE, LPSTORAGE, LPVOID*);
        using NativeOleCreateLinkToFileExFunction    = HRESULT(WINAPI*)(LPCOLESTR, REFIID, DWORD, DWORD, ULONG, DWORD*, LPFORMATETC, IAdviseSink*, DWORD*, LPOLECLIENTSITE, LPSTORAGE, LPVOID*);
        using NativeOleCreateFromFileExFunction      = HRESULT(WINAPI*)(REFCLSID, LPCOLESTR, REFIID, DWORD, DWORD, ULONG, DWORD*, LPFORMATETC, IAdviseSink*, DWORD*, LPOLECLIENTSITE, LPSTORAGE, LPVOID*);
        using NativeVariantCopyIndFunction           = HRESULT(WINAPI*)(VARIANTARG*, const VARIANTARG*);
        using NativeVariantChangeTypeExFunction      = HRESULT(WINAPI*)(VARIANTARG*, const VARIANTARG*, LCID, USHORT, VARTYPE);
        using NativeLoadTypeLibExFunction            = HRESULT(WINAPI*)(LPCOLESTR, REGKIND, ::ITypeLib**);
        using NativeQueryPathOfRegTypeLibFunction    = HRESULT(WINAPI*)(REFGUID, USHORT, USHORT, LCID, LPBSTR);
        using NativeRegisterActiveObjectFunction     = HRESULT(WINAPI*)(::IUnknown*, REFCLSID, DWORD, DWORD*);
        using NativeRevokeActiveObjectFunction       = HRESULT(WINAPI*)(DWORD, void*);
        using NativeSetStorageTimesFunction               = HRESULT(WINAPI*)(LPCWSTR, const FILETIME*, const FILETIME*, const FILETIME*);
        using NativeCreateStorageExFunction               = HRESULT(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, STGOPTIONS*, PSECURITY_DESCRIPTOR, REFIID, void**);
        using NativeOpenStorageExFunction                 = HRESULT(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, STGOPTIONS*, PSECURITY_DESCRIPTOR, REFIID, void**);
        using NativeCreatePropertyStorageFunction         = HRESULT(WINAPI*)(::IUnknown*, REFFMTID, const CLSID*, DWORD, DWORD, ::IPropertyStorage**);
        using NativeOpenPropertyStorageFunction           = HRESULT(WINAPI*)(::IUnknown*, REFFMTID, DWORD, DWORD, ::IPropertyStorage**);
        using NativeFormatIdToPropertyStorageNameFunction = HRESULT(WINAPI*)(const FMTID*, LPOLESTR);
        using NativePropertyStorageNameToFormatIdFunction = HRESULT(WINAPI*)(LPOLESTR, FMTID*);
        using NativeReadClassStorageFunction              = HRESULT(WINAPI*)(::IStorage*, CLSID*);
        using NativeWriteClassStorageFunction             = HRESULT(WINAPI*)(::IStorage*, REFCLSID);
        using NativeReadClassStreamFunction               = HRESULT(WINAPI*)(::IStream*, CLSID*);
        using NativeWriteClassStreamFunction              = HRESULT(WINAPI*)(::IStream*, REFCLSID);
        using NativeWriteFormatUserTypeFunction           = HRESULT(WINAPI*)(::IStorage*, CLIPFORMAT, LPOLESTR);
        using NativeReadFormatUserTypeFunction            = HRESULT(WINAPI*)(::IStorage*, CLIPFORMAT*, LPOLESTR*);
        using NativePropertyVariantCopyFunction           = HRESULT(WINAPI*)(PROPVARIANT*, const PROPVARIANT*);
        using NativePropertyVariantClearFunction          = HRESULT(WINAPI*)(PROPVARIANT*);
        using NativeFreePropertyVariantArrayFunction      = HRESULT(WINAPI*)(ULONG, PROPVARIANT*);

        [[nodiscard]] constexpr bool EqualNativeGuid(const GUID& native, RefGuid projected) noexcept {
            if (native.Data1 != projected.data1 || native.Data2 != projected.data2 || native.Data3 != projected.data3) {
                return false;
            }
            for (size_t index = 0; index < std::size(native.Data4); ++index) {
                if (native.Data4[index] != projected.data4[index]) {
                    return false;
                }
            }
            return true;
        }

        static_assert(std::is_same_v<NativeCoWaitForMultipleHandlesFunction,                           decltype(&::CoWaitForMultipleHandles)>);
        static_assert(std::is_same_v<NativeCreateStreamOnHGlobalFunction,                              decltype(&::CreateStreamOnHGlobal)>);
        static_assert(std::is_same_v<NativeGetHGlobalFromStreamFunction,                               decltype(&::GetHGlobalFromStream)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::CreateDocfileFunction,                decltype(&::StgCreateDocfile)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::CreateDocfileOnLockBytesFunction,     decltype(&::StgCreateDocfileOnILockBytes)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::OpenStorageFunction,                  decltype(&::StgOpenStorage)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::OpenStorageOnLockBytesFunction,       decltype(&::StgOpenStorageOnILockBytes)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::IsStorageFileFunction,                decltype(&::StgIsStorageFile)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::IsStorageLockBytesFunction,           decltype(&::StgIsStorageILockBytes)>);
        static_assert(std::is_same_v<NativeSetStorageTimesFunction,                                    decltype(&::StgSetTimes)>);
        static_assert(std::is_same_v<NativeCreateStorageExFunction,                                    decltype(&::StgCreateStorageEx)>);
        static_assert(std::is_same_v<NativeOpenStorageExFunction,                                      decltype(&::StgOpenStorageEx)>);
        static_assert(std::is_same_v<NativeCreatePropertyStorageFunction,                              decltype(&::StgCreatePropStg)>);
        static_assert(std::is_same_v<NativeOpenPropertyStorageFunction,                                decltype(&::StgOpenPropStg)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::CreatePropertySetStorageFunction,     decltype(&::StgCreatePropSetStg)>);
        static_assert(std::is_same_v<NativeFormatIdToPropertyStorageNameFunction,                      decltype(&::FmtIdToPropStgName)>);
        static_assert(std::is_same_v<NativePropertyStorageNameToFormatIdFunction,                      decltype(&::PropStgNameToFmtId)>);
        static_assert(std::is_same_v<NativeReadClassStorageFunction,                                   decltype(&::ReadClassStg)>);
        static_assert(std::is_same_v<NativeWriteClassStorageFunction,                                  decltype(&::WriteClassStg)>);
        static_assert(std::is_same_v<NativeReadClassStreamFunction,                                    decltype(&::ReadClassStm)>);
        static_assert(std::is_same_v<NativeWriteClassStreamFunction,                                   decltype(&::WriteClassStm)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::GetGlobalFromLockBytesFunction,       decltype(&::GetHGlobalFromILockBytes)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::CreateLockBytesOnGlobalFunction,      decltype(&::CreateILockBytesOnHGlobal)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::GetConvertStorageFunction,            decltype(&::GetConvertStg)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::SetConvertStorageFunction,            decltype(&::SetConvertStg)>);
        static_assert(std::is_same_v<NativeWriteFormatUserTypeFunction,                                decltype(&::WriteFmtUserTypeStg)>);
        static_assert(std::is_same_v<NativeReadFormatUserTypeFunction,                                 decltype(&::ReadFmtUserTypeStg)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::OpenAsyncDocfileFunction,             decltype(&::StgOpenAsyncDocfileOnIFillLockBytes)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::GetFillLockBytesOnLockBytesFunction,  decltype(&::StgGetIFillLockBytesOnILockBytes)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::GetFillLockBytesOnFileFunction,       decltype(&::StgGetIFillLockBytesOnFile)>);
        static_assert(std::is_same_v<StructuredStorageSystemAPI::OpenLayoutDocfileFunction,            decltype(&::StgOpenLayoutDocfile)>);
        static_assert(std::is_same_v<NativePropertyVariantCopyFunction,                                decltype(&::PropVariantCopy)>);
        static_assert(std::is_same_v<NativePropertyVariantClearFunction,                               decltype(&::PropVariantClear)>);
        static_assert(std::is_same_v<NativeFreePropertyVariantArrayFunction,                           decltype(&::FreePropVariantArray)>);
        static_assert(EqualNativeGuid(__uuidof(::IFillLockBytes),    InterfaceIdOf<IFillLockBytes>()));
        static_assert(EqualNativeGuid(__uuidof(::IProgressNotify),   InterfaceIdOf<IProgressNotify>()));
        static_assert(EqualNativeGuid(__uuidof(::ILayoutStorage),    InterfaceIdOf<ILayoutStorage>()));
        static_assert(EqualNativeGuid(__uuidof(::IDirectWriterLock), InterfaceIdOf<IDirectWriterLock>()));
        static_assert(std::is_same_v<NativeOleCreateExFunction,              decltype(&::OleCreateEx)>);
        static_assert(std::is_same_v<NativeOleCreateFromDataExFunction,      decltype(&::OleCreateFromDataEx)>);
        static_assert(std::is_same_v<NativeOleCreateLinkExFunction,          decltype(&::OleCreateLinkEx)>);
        static_assert(std::is_same_v<NativeOleCreateLinkToFileExFunction,    decltype(&::OleCreateLinkToFileEx)>);
        static_assert(std::is_same_v<NativeOleCreateFromFileExFunction,      decltype(&::OleCreateFromFileEx)>);
        static_assert(std::is_same_v<NativeVariantCopyIndFunction,           decltype(&::VariantCopyInd)>);
        static_assert(std::is_same_v<NativeVariantChangeTypeExFunction,      decltype(&::VariantChangeTypeEx)>);
        static_assert(std::is_same_v<NativeLoadTypeLibExFunction,            decltype(&::LoadTypeLibEx)>);
        static_assert(std::is_same_v<NativeQueryPathOfRegTypeLibFunction,    decltype(&::QueryPathOfRegTypeLib)>);
        static_assert(std::is_same_v<NativeRegisterActiveObjectFunction,     decltype(&::RegisterActiveObject)>);
        static_assert(std::is_same_v<NativeRevokeActiveObjectFunction,       decltype(&::RevokeActiveObject)>);
        static_assert(sizeof(WindowsSystem::Guid)  == sizeof(GUID));
        static_assert(alignof(WindowsSystem::Guid) == alignof(GUID));
        static_assert(sizeof(Guid)                         == sizeof(GUID));
        static_assert(alignof(Guid)                        == alignof(GUID));
        static_assert(sizeof(LargeInteger)                 == sizeof(LARGE_INTEGER));
        static_assert(alignof(LargeInteger)                == alignof(LARGE_INTEGER));
        static_assert(sizeof(UnsignedLargeInteger)         == sizeof(ULARGE_INTEGER));
        static_assert(alignof(UnsignedLargeInteger)        == alignof(ULARGE_INTEGER));
        static_assert(sizeof(PointL)                       == sizeof(POINTL));
        static_assert(alignof(PointL)                      == alignof(POINTL));
        static_assert(sizeof(FileTime)                     == sizeof(FILETIME));
        static_assert(alignof(FileTime)                    == alignof(FILETIME));
        static_assert(offsetof(FileTime, lowDateTime)      == offsetof(FILETIME, dwLowDateTime));
        static_assert(offsetof(FileTime, highDateTime)     == offsetof(FILETIME, dwHighDateTime));
        static_assert(sizeof(WindowsSystem::StorageOptions)                  == sizeof(STGOPTIONS));
        static_assert(alignof(WindowsSystem::StorageOptions)                 == alignof(STGOPTIONS));
        static_assert(offsetof(WindowsSystem::StorageOptions, version)       == offsetof(STGOPTIONS, usVersion));
        static_assert(offsetof(WindowsSystem::StorageOptions, sectorSize)    == offsetof(STGOPTIONS, ulSectorSize));
        static_assert(offsetof(WindowsSystem::StorageOptions, templateFile)  == offsetof(STGOPTIONS, pwcsTemplateFile));
        static_assert(sizeof(StatStg)                         == sizeof(STATSTG));
        static_assert(alignof(StatStg)                        == alignof(STATSTG));
        static_assert(offsetof(StatStg, name)                 == offsetof(STATSTG, pwcsName));
        static_assert(offsetof(StatStg, size)                 == offsetof(STATSTG, cbSize));
        static_assert(offsetof(StatStg, modificationTime)     == offsetof(STATSTG, mtime));
        static_assert(offsetof(StatStg, creationTime)         == offsetof(STATSTG, ctime));
        static_assert(offsetof(StatStg, accessTime)           == offsetof(STATSTG, atime));
        static_assert(offsetof(StatStg, classId)              == offsetof(STATSTG, clsid));
        static_assert(sizeof(StorageLayout)                     == sizeof(::StorageLayout));
        static_assert(alignof(StorageLayout)                    == alignof(::StorageLayout));
        static_assert(offsetof(StorageLayout, layoutType)       == offsetof(::StorageLayout, LayoutType));
        static_assert(offsetof(StorageLayout, elementName)      == offsetof(::StorageLayout, pwcsElementName));
        static_assert(offsetof(StorageLayout, offset)           == offsetof(::StorageLayout, cOffset));
        static_assert(offsetof(StorageLayout, bytes)            == offsetof(::StorageLayout, cBytes));
        static_assert(sizeof(PropertySpec)                == sizeof(PROPSPEC));
        static_assert(alignof(PropertySpec)               == alignof(PROPSPEC));
        static_assert(offsetof(PropertySpec, kind)        == offsetof(PROPSPEC, ulKind));
        static_assert(offsetof(PropertySpec, value)       == offsetof(PROPSPEC, propid));
        static_assert(sizeof(PropertyVariant)             == sizeof(PROPVARIANT));
        static_assert(alignof(PropertyVariant)            == alignof(PROPVARIANT));
        static_assert(sizeof(StatPropStg)                 == sizeof(STATPROPSTG));
        static_assert(alignof(StatPropStg)                == alignof(STATPROPSTG));
        static_assert(offsetof(StatPropStg, name)         == offsetof(STATPROPSTG, lpwstrName));
        static_assert(offsetof(StatPropStg, propertyId)   == offsetof(STATPROPSTG, propid));
        static_assert(offsetof(StatPropStg, variantType)  == offsetof(STATPROPSTG, vt));
        static_assert(sizeof(StatPropSetStg)                             == sizeof(STATPROPSETSTG));
        static_assert(alignof(StatPropSetStg)                            == alignof(STATPROPSETSTG));
        static_assert(offsetof(StatPropSetStg, formatId)                 == offsetof(STATPROPSETSTG, fmtid));
        static_assert(offsetof(StatPropSetStg, classId)                  == offsetof(STATPROPSETSTG, clsid));
        static_assert(offsetof(StatPropSetStg, flags)                    == offsetof(STATPROPSETSTG, grfFlags));
        static_assert(offsetof(StatPropSetStg, modificationTime)         == offsetof(STATPROPSETSTG, mtime));
        static_assert(offsetof(StatPropSetStg, creationTime)             == offsetof(STATPROPSETSTG, ctime));
        static_assert(offsetof(StatPropSetStg, accessTime)               == offsetof(STATPROPSETSTG, atime));
        static_assert(offsetof(StatPropSetStg, operatingSystemVersion)   == offsetof(STATPROPSETSTG, dwOSVersion));
        static_assert(sizeof(IFillLockBytes)    == sizeof(::IFillLockBytes));
        static_assert(sizeof(IProgressNotify)   == sizeof(::IProgressNotify));
        static_assert(sizeof(ILayoutStorage)    == sizeof(::ILayoutStorage));
        static_assert(sizeof(IDirectWriterLock) == sizeof(::IDirectWriterLock));
        static_assert(EqualNativeGuid(__uuidof(::IOleDocument), InterfaceIdOf<IOleDocument>()));
        static_assert(EqualNativeGuid(__uuidof(::IOleDocumentView), InterfaceIdOf<IOleDocumentView>()));
        static_assert(EqualNativeGuid(__uuidof(::IOleDocumentSite), InterfaceIdOf<IOleDocumentSite>()));
        static_assert(EqualNativeGuid(__uuidof(::IEnumOleDocumentViews), InterfaceIdOf<IEnumOleDocumentViews>()));
        static_assert(EqualNativeGuid(__uuidof(::IPrint), InterfaceIdOf<IPrint>()));
        static_assert(EqualNativeGuid(__uuidof(::IContinueCallback), InterfaceIdOf<IContinueCallback>()));
        static_assert(EqualNativeGuid(__uuidof(::IOleCommandTarget), InterfaceIdOf<IOleCommandTarget>()));
        static_assert(EqualNativeGuid(__uuidof(::IZoomEvents), InterfaceIdOf<IZoomEvents>()));
        static_assert(sizeof(Rect)             == sizeof(RECT));
        static_assert(alignof(Rect)            == alignof(RECT));
        static_assert(offsetof(Rect, left)     == offsetof(RECT, left));
        static_assert(offsetof(Rect, bottom)   == offsetof(RECT, bottom));
        static_assert(sizeof(OleCommand)                       == sizeof(OLECMD));
        static_assert(alignof(OleCommand)                      == alignof(OLECMD));
        static_assert(offsetof(OleCommand, commandId)          == offsetof(OLECMD, cmdID));
        static_assert(offsetof(OleCommand, flags)              == offsetof(OLECMD, cmdf));
        static_assert(sizeof(OleCommandText)                   == sizeof(OLECMDTEXT));
        static_assert(alignof(OleCommandText)                  == alignof(OLECMDTEXT));
        static_assert(offsetof(OleCommandText, text)           == offsetof(OLECMDTEXT, rgwz));
        static_assert(sizeof(PageRange)                        == sizeof(PAGERANGE));
        static_assert(alignof(PageRange)                       == alignof(PAGERANGE));
        static_assert(offsetof(PageRange, firstPage)           == offsetof(PAGERANGE, nFromPage));
        static_assert(offsetof(PageRange, lastPage)            == offsetof(PAGERANGE, nToPage));
        static_assert(sizeof(PageSet)                          == sizeof(PAGESET));
        static_assert(alignof(PageSet)                         == alignof(PAGESET));
        static_assert(offsetof(PageSet, ranges)                == offsetof(PAGESET, rgPages));
        static_assert(sizeof(IOleDocument)          == sizeof(::IOleDocument));
        static_assert(sizeof(IOleDocumentSite)      == sizeof(::IOleDocumentSite));
        static_assert(sizeof(IOleDocumentView)      == sizeof(::IOleDocumentView));
        static_assert(sizeof(IEnumOleDocumentViews) == sizeof(::IEnumOleDocumentViews));
        static_assert(sizeof(IContinueCallback)     == sizeof(::IContinueCallback));
        static_assert(sizeof(IPrint)                == sizeof(::IPrint));
        static_assert(sizeof(IOleCommandTarget)     == sizeof(::IOleCommandTarget));
        static_assert(sizeof(IZoomEvents)           == sizeof(::IZoomEvents));
        static_assert(std::is_same_v<decltype(&::IOleDocument::CreateView),                  HRESULT (STDMETHODCALLTYPE ::IOleDocument::*)(::IOleInPlaceSite*, ::IStream*, DWORD, ::IOleDocumentView**)>);
        static_assert(std::is_same_v<decltype(&::IOleDocument::GetDocMiscStatus),            HRESULT (STDMETHODCALLTYPE ::IOleDocument::*)(DWORD*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocument::EnumViews),                   HRESULT (STDMETHODCALLTYPE ::IOleDocument::*)(::IEnumOleDocumentViews**, ::IOleDocumentView**)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentSite::ActivateMe),              HRESULT (STDMETHODCALLTYPE ::IOleDocumentSite::*)(::IOleDocumentView*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::SetInPlaceSite),           HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(::IOleInPlaceSite*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::GetInPlaceSite),           HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(::IOleInPlaceSite**)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::GetDocument),              HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(::IUnknown**)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::SetRect),                  HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(RECT*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::GetRect),                  HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(RECT*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::SetRectComplex),           HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(RECT*, RECT*, RECT*, RECT*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::Show),                     HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(BOOL)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::UIActivate),               HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(BOOL)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::Open),                     HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)()>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::CloseView),                HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(DWORD)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::SaveViewState),            HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(::IStream*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::ApplyViewState),           HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(::IStream*)>);
        static_assert(std::is_same_v<decltype(&::IOleDocumentView::Clone),                    HRESULT (STDMETHODCALLTYPE ::IOleDocumentView::*)(::IOleInPlaceSite*, ::IOleDocumentView**)>);
        static_assert(std::is_same_v<decltype(&::IEnumOleDocumentViews::Next),                HRESULT (__stdcall ::IEnumOleDocumentViews::*)(ULONG, ::IOleDocumentView**, ULONG*)>);
        static_assert(std::is_same_v<decltype(&::IEnumOleDocumentViews::Skip),                HRESULT (STDMETHODCALLTYPE ::IEnumOleDocumentViews::*)(ULONG)>);
        static_assert(std::is_same_v<decltype(&::IEnumOleDocumentViews::Reset),               HRESULT (STDMETHODCALLTYPE ::IEnumOleDocumentViews::*)()>);
        static_assert(std::is_same_v<decltype(&::IEnumOleDocumentViews::Clone),               HRESULT (STDMETHODCALLTYPE ::IEnumOleDocumentViews::*)(::IEnumOleDocumentViews**)>);
        static_assert(std::is_same_v<decltype(&::IContinueCallback::FContinue),               HRESULT (STDMETHODCALLTYPE ::IContinueCallback::*)()>);
        static_assert(std::is_same_v<decltype(&::IContinueCallback::FContinuePrinting),       HRESULT (STDMETHODCALLTYPE ::IContinueCallback::*)(LONG, LONG, wchar_t*)>);
        static_assert(std::is_same_v<decltype(&::IPrint::SetInitialPageNum),                  HRESULT (STDMETHODCALLTYPE ::IPrint::*)(LONG)>);
        static_assert(std::is_same_v<decltype(&::IPrint::GetPageInfo),                        HRESULT (STDMETHODCALLTYPE ::IPrint::*)(LONG*, LONG*)>);
        static_assert(std::is_same_v<decltype(&::IPrint::Print),                              HRESULT (__stdcall ::IPrint::*)(DWORD, DVTARGETDEVICE**, PAGESET**, STGMEDIUM*, ::IContinueCallback*, LONG, LONG*, LONG*)>);
        static_assert(std::is_same_v<decltype(&::IOleCommandTarget::QueryStatus),             HRESULT (STDMETHODCALLTYPE ::IOleCommandTarget::*)(const GUID*, ULONG, OLECMD*, OLECMDTEXT*)>);
        static_assert(std::is_same_v<decltype(&::IOleCommandTarget::Exec),                    HRESULT (STDMETHODCALLTYPE ::IOleCommandTarget::*)(const GUID*, DWORD, DWORD, VARIANT*, VARIANT*)>);
        static_assert(std::is_same_v<decltype(&::IZoomEvents::OnZoomPercentChanged),          HRESULT (STDMETHODCALLTYPE ::IZoomEvents::*)(ULONG)>);
        static_assert(sizeof(Variant)  == sizeof(VARIANT));
        static_assert(alignof(Variant) == alignof(VARIANT));

        static_assert(std::is_same_v<SchedulerTraceSystemAPI::StartTraceFunction,                 decltype(&::StartTraceW)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::ControlTraceFunction,               decltype(&::ControlTraceW)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::OpenTraceFunction,                  decltype(&::OpenTraceW)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::ProcessTraceFunction,               decltype(&::ProcessTrace)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::CloseTraceFunction,                 decltype(&::CloseTrace)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::QueryPerformanceFrequencyFunction,  decltype(&::QueryPerformanceFrequency)>);

        static_assert(std::is_same_v<ProcessSystemAPI::GetCurrentProcessIdFunction,               decltype(&::GetCurrentProcessId)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetCurrentProcessFunction,                 decltype(&::GetCurrentProcess)>);
        static_assert(std::is_same_v<ProcessSystemAPI::CreateProcessSnapshotFunction,             decltype(&::CreateToolhelp32Snapshot)>);
        static_assert(std::is_same_v<ProcessSystemAPI::ReadProcessEntryFunction,                  decltype(&::Process32FirstW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::ReadProcessEntryFunction,                  decltype(&::Process32NextW)>);
        static_assert(std::is_same_v<HandleSystemAPI::CloseHandleFunction,                        decltype(&::CloseHandle)>);
        static_assert(std::is_same_v<ProcessSystemAPI::QueryFullProcessImageNameWideFunction,     decltype(&::QueryFullProcessImageNameW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetCommandLineWideFunction,                decltype(&::GetCommandLineW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::CommandLineToArgvWideFunction,             decltype(&::CommandLineToArgvW)>);
        static_assert(std::is_same_v<LocalMemorySystemAPI::LocalFreeFunction,                     decltype(&::LocalFree)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessTimesFunction,                   decltype(&::GetProcessTimes)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessMemoryInfoFunction,              decltype(&::K32GetProcessMemoryInfo)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessIdFunction,                     decltype(&::GetProcessId)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessHandleCountFunction,             decltype(&::GetProcessHandleCount)>);
        static_assert(std::is_same_v<ProcessSystemAPI::QueryProcessCycleTimeFunction,             decltype(&::QueryProcessCycleTime)>);
        static_assert(std::is_same_v<SystemInformationSystemAPI::GetTickCount64Function,          decltype(&::GetTickCount64)>);
        static_assert(std::is_same_v<ThreadSystemAPI::OpenThreadFunction,                         decltype(&::OpenThread)>);
        static_assert(std::is_same_v<ThreadSystemAPI::GetThreadIdFunction,                       decltype(&::GetThreadId)>);
        static_assert(std::is_same_v<ThreadSystemAPI::QueryThreadCycleTimeFunction,              decltype(&::QueryThreadCycleTime)>);
        static_assert(std::is_same_v<HandleSystemAPI::DuplicateHandleFunction,                    decltype(&::DuplicateHandle)>);
        static_assert(std::is_same_v<EventSystemAPI::CreateEventWideFunction,                     decltype(&::CreateEventW)>);
        static_assert(std::is_same_v<EventSystemAPI::SetEventFunction,                            decltype(&::SetEvent)>);
        static_assert(std::is_same_v<EventSystemAPI::ResetEventFunction,                          decltype(&::ResetEvent)>);
        static_assert(std::is_same_v<FileSystemAPI::GetFileTypeFunction,                         decltype(&::GetFileType)>);
        static_assert(std::is_same_v<FileSystemAPI::SetFilePointerFunction,                      decltype(&::SetFilePointerEx)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::WaitForSingleObjectFunction,       decltype(&::WaitForSingleObject)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::WaitForSingleObjectExFunction,     decltype(&::WaitForSingleObjectEx)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::WaitForMultipleObjectsFunction,    decltype(&::WaitForMultipleObjects)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::WaitForMultipleObjectsExFunction,  decltype(&::WaitForMultipleObjectsEx)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::SignalObjectAndWaitFunction,       decltype(&::SignalObjectAndWait)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::CreateMutexExWideFunction,         decltype(&::CreateMutexExW)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::ReleaseMutexFunction,              decltype(&::ReleaseMutex)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::CreateSemaphoreExWideFunction,     decltype(&::CreateSemaphoreExW)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::ReleaseSemaphoreFunction,          decltype(&::ReleaseSemaphore)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::CreateWaitableTimerExWideFunction, decltype(&::CreateWaitableTimerExW)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::SetWaitableTimerExFunction,        decltype(&::SetWaitableTimerEx)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::CancelWaitableTimerFunction,       decltype(&::CancelWaitableTimer)>);
        static_assert(std::is_same_v<VirtualMemorySystemAPI::AllocateFunction,                    decltype(&::VirtualAlloc)>);
        static_assert(std::is_same_v<VirtualMemorySystemAPI::FreeFunction,                        decltype(&::VirtualFree)>);
        static_assert(std::is_same_v<VirtualMemorySystemAPI::ProtectFunction,                     decltype(&::VirtualProtect)>);
        static_assert(std::is_same_v<VirtualMemorySystemAPI::QueryFunction,                       decltype(&::VirtualQuery)>);
        static_assert(std::is_same_v<VirtualMemorySystemAPI::Allocate2Function,                   decltype(&::VirtualAlloc2)>);
        static_assert(std::is_same_v<VirtualMemorySystemAPI::MapViewOfFile3Function,              decltype(&::MapViewOfFile3)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::OpenProcessFunction,                decltype(&::OpenProcess)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::CreateProcessWideFunction,          decltype(&::CreateProcessW)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::GetExitCodeFunction,                decltype(&::GetExitCodeProcess)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::TerminateFunction,                  decltype(&::TerminateProcess)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::CreateJobObjectWideFunction,        decltype(&::CreateJobObjectW)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::AssignJobFunction,                  decltype(&::AssignProcessToJobObject)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::SetPriorityClassFunction,           decltype(&::SetPriorityClass)>);
        static_assert(std::is_same_v<ProcessControlSystemAPI::SetAffinityMaskFunction,            decltype(&::SetProcessAffinityMask)>);
        static_assert(std::is_same_v<ThreadControlSystemAPI::SetPriorityFunction,                 decltype(&::SetThreadPriority)>);
        static_assert(std::is_same_v<ThreadControlSystemAPI::GetPriorityFunction,                 decltype(&::GetThreadPriority)>);
        static_assert(std::is_same_v<ThreadControlSystemAPI::SetAffinityMaskFunction,             decltype(&::SetThreadAffinityMask)>);
        static_assert(std::is_same_v<ThreadControlSystemAPI::SetIdealProcessorFunction,           decltype(&::SetThreadIdealProcessor)>);
        static_assert(std::is_same_v<ThreadControlSystemAPI::SetIdealProcessorExFunction,         decltype(&::SetThreadIdealProcessorEx)>);
        static_assert(std::is_same_v<ThreadControlSystemAPI::SuspendFunction,                     decltype(&::SuspendThread)>);
        static_assert(std::is_same_v<ThreadControlSystemAPI::ResumeFunction,                      decltype(&::ResumeThread)>);
        static_assert(std::is_same_v<RandomSystemAPI::GenerateFunction,                           decltype(&::BCryptGenRandom)>);
        static_assert(std::is_same_v<NetworkInterfaceSystemAPI::GetAdaptersAddressesFunction,     decltype(&::GetAdaptersAddresses)>);
        static_assert(std::is_same_v<NetworkInterfaceSystemAPI::GetIfTable2Function,              decltype(&::GetIfTable2)>);
        static_assert(std::is_same_v<NetworkInterfaceSystemAPI::FreeMibTableFunction,             decltype(&::FreeMibTable)>);
        static_assert(std::is_same_v<NetworkInterfaceSystemAPI::NotifyAddressChangeFunction,      decltype(&::NotifyAddrChange)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::CreateDirectoryWideFunction,            decltype(&::CreateDirectoryW)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::RemoveDirectoryWideFunction,            decltype(&::RemoveDirectoryW)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::FindFirstFileWideFunction,              decltype(&::FindFirstFileW)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::FindNextFileWideFunction,               decltype(&::FindNextFileW)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::FindCloseFunction,                      decltype(&::FindClose)>);

        using NativeGetFileAttributesExWideFunction = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

        static_assert(std::is_same_v<NativeGetFileAttributesExWideFunction,                       decltype(&::GetFileAttributesExW)>);
        static_assert(sizeof(WindowsSystem::GetFileExInfoLevel) == sizeof(GET_FILEEX_INFO_LEVELS));
        static_assert(std::is_same_v<ModernFileSystemAPI::GetFinalPathNameByHandleWideFunction,   decltype(&::GetFinalPathNameByHandleW)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::CreateHardLinkWideFunction,             decltype(&::CreateHardLinkW)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::CreateSymbolicLinkWideFunction,         decltype(&::CreateSymbolicLinkW)>);
        static_assert(std::is_same_v<ModernFileSystemAPI::GetVolumeInformationWideFunction,       decltype(&::GetVolumeInformationW)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::CreateFileMappingWideFunction,         decltype(&::CreateFileMappingW)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::MapViewOfFileFunction,                 decltype(&::MapViewOfFile)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::UnmapViewOfFileFunction,               decltype(&::UnmapViewOfFile)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::FlushViewOfFileFunction,               decltype(&::FlushViewOfFile)>);

        static_assert(sizeof(WindowsSystem::GlobalMemory)  == sizeof(HGLOBAL));
        static_assert(alignof(WindowsSystem::GlobalMemory) == alignof(HGLOBAL));
        static_assert(sizeof(WindowsSystem::Handle)        == sizeof(HWND));
        static_assert(alignof(WindowsSystem::Handle)       == alignof(HWND));

        static_assert(std::is_same_v<ClipboardSystemAPI::CloseClipboardFunction,                  decltype(&::CloseClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::EmptyClipboardFunction,                  decltype(&::EmptyClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::GetClipboardDataFunction,                decltype(&::GetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::SetClipboardDataFunction,                decltype(&::SetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::IsClipboardFormatAvailableFunction,      decltype(&::IsClipboardFormatAvailable)>);

        static_assert(std::is_same_v<FileSystemAPI::GetStandardHandleFunction,                    decltype(&::GetStdHandle)>);
        static_assert(std::is_same_v<FileSystemAPI::CreateFileWideFunction,                       decltype(&::CreateFileW)>);
        static_assert(std::is_same_v<FileSystemAPI::ReadFileFunction,                             decltype(&::ReadFile)>);
        static_assert(std::is_same_v<FileSystemAPI::WriteFileFunction,                            decltype(&::WriteFile)>);
        static_assert(std::is_same_v<FileSystemAPI::FlushFileBuffersFunction,                     decltype(&::FlushFileBuffers)>);
        static_assert(std::is_same_v<FileSystemAPI::LockFileFunction,                             decltype(&::LockFileEx)>);

        static_assert(sizeof(WindowsSystem::DWord)                  == sizeof(DWORD));
        static_assert(alignof(WindowsSystem::DWord)                 == alignof(DWORD));
        static_assert(std::is_unsigned_v<WindowsSystem::DWord>      == std::is_unsigned_v<DWORD>);
        static_assert(sizeof(WindowsSystem::UInt)                   == sizeof(UINT));
        static_assert(alignof(WindowsSystem::UInt)                  == alignof(UINT));
        static_assert(std::is_unsigned_v<WindowsSystem::UInt>       == std::is_unsigned_v<UINT>);
        static_assert(sizeof(WindowsSystem::Size)                   == sizeof(SIZE_T));
        static_assert(alignof(WindowsSystem::Size)                  == alignof(SIZE_T));
        static_assert(std::is_unsigned_v<WindowsSystem::Size>       == std::is_unsigned_v<SIZE_T>);
        // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        void* OpenPosixModule(const char* name, int flags) noexcept {
            return ::dlopen(name, flags);
        }

        int ClosePosixModule(void* module) noexcept {
            return ::dlclose(module);
        }

        void* FindPosixSymbol(void* module, const char* name) noexcept {
            return ::dlsym(module, name);
        }

        using QueryPosixAddressNativeFunction = decltype(&::dladdr);

        [[nodiscard]] QueryPosixAddressNativeFunction LoadPosixAddressQuery() noexcept {
            static const QueryPosixAddressNativeFunction query = [] {
                const ModulePtr& module = CurrentProcessModule();
                if (module == nullptr) {
                    return static_cast<QueryPosixAddressNativeFunction>(nullptr);
                }
                return module->TryFindFunction<QueryPosixAddressNativeFunction>("dladdr");
            }();
            return query;
        }

        bool QueryPosixAddress(const void* address, ModuleAddressInfo* output) noexcept {
            if (address == nullptr || output == nullptr) {
                return false;
            }
            *output = {};
            Dl_info nativeInfo{};
            const QueryPosixAddressNativeFunction query = LoadPosixAddressQuery();
            if (query == nullptr || query(address, &nativeInfo) == 0) {
                return false;
            }
            *output = {
                .modulePath = nativeInfo.dli_fname,
                .moduleBase = nativeInfo.dli_fbase,
                .symbolName = nativeInfo.dli_sname,
                .symbolAddress = nativeInfo.dli_saddr,
            };
            return true;
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
            const SystemInformationSystemAPI& system = LoadSystemInformationSystemAPI();
            if (!EnsureSystemAPIs(system.systemConfiguration, file.open, file.read, file.close)) {
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
            const long pageSize = system.systemConfiguration(_SC_PAGESIZE);
            if (parsed.ec
                != std::errc{}
                || pageSize
                <= 0
                || residentPages
                > std::numeric_limits<uint64_t>::max()
                / static_cast<uint64_t>(pageSize)) {
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
            if (!TimeValueNanoseconds(usage.ru_utime, &userNanoseconds)
                || !TimeValueNanoseconds(usage.ru_stime, &kernelNanoseconds)) {
                return false;
            }

            uint64_t residentBytes = 0;
#    if defined(PLATFORM_LINUX)
            if (!LinuxResidentMemory(&residentBytes)
                || static_cast<uint64_t>(usage.ru_maxrss)
                > std::numeric_limits<uint64_t>::max()
                / 1024) {
                return false;
            }
            uint64_t peakResidentBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#    else
            if (!EnsureSystemAPIs(api.getProcessId, api.processResourceUsage)) {
                return false;
            }
            rusage_info_v2 nativeUsage{};
            if (api.processResourceUsage(api.getProcessId(), RUSAGE_INFO_V2,
                                         reinterpret_cast<rusage_info_t*>(&nativeUsage))
                != 0) {
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
        /** @brief Query a POSIX file's preferred block size through the native stat ABI. */
        [[nodiscard]] bool QueryPosixFileBlockSize(int descriptor, size_t* output) noexcept {
            if (output == nullptr) {
                return false;
            }
            const FileSystemAPI& api = LoadFileSystemAPI();
            if (api.stat == nullptr) {
                return false;
            }
            struct stat information {};
            if (api.stat(descriptor, &information) != 0 || information.st_blksize <= 0) {
                return false;
            }
            *output = static_cast<size_t>(information.st_blksize);
            return true;
        }

        /** @brief Query a POSIX file size through the native stat ABI. */
        [[nodiscard]] bool QueryPosixFileSize(int descriptor, uint64_t* output) noexcept {
            if (output == nullptr) {
                return false;
            }
            const FileSystemAPI& api = LoadFileSystemAPI();
            if (api.stat == nullptr) {
                return false;
            }
            struct stat information {};
            if (api.stat(descriptor, &information) != 0 || information.st_size < 0) {
                return false;
            }
            *output = static_cast<uint64_t>(information.st_size);
            return true;
        }

        static_assert(std::is_convertible_v<decltype(&::getpid),               ProcessSystemAPI::GetProcessIdFunction>);
        static_assert(std::is_convertible_v<decltype(&::getppid),              ProcessSystemAPI::GetParentProcessIdFunction>);
        static_assert(std::is_convertible_v<decltype(&::getrusage),            ProcessSystemAPI::GetResourceUsageFunction>);
#    if defined(PLATFORM_MACOS)
        static_assert(std::is_convertible_v<decltype(&::_NSGetExecutablePath), ProcessSystemAPI::GetExecutablePathFunction>);
        static_assert(std::is_convertible_v<decltype(&::_NSGetArgc),           ProcessSystemAPI::GetArgumentCountFunction>);
        static_assert(std::is_convertible_v<decltype(&::_NSGetArgv),           ProcessSystemAPI::GetArgumentVectorFunction>);
        static_assert(std::is_convertible_v<decltype(&::proc_pid_rusage),      ProcessSystemAPI::ProcessResourceUsageFunction>);
#    endif
        static_assert(std::is_convertible_v<decltype(&::open),                 FileSystemAPI::OpenFunction>);
        static_assert(std::is_convertible_v<decltype(&::close),                FileSystemAPI::CloseFunction>);
        static_assert(std::is_convertible_v<decltype(&::read),                 FileSystemAPI::ReadFunction>);
        static_assert(std::is_convertible_v<decltype(&::write),                FileSystemAPI::WriteFunction>);
        static_assert(std::is_convertible_v<decltype(&::pread),                FileSystemAPI::ReadAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::pwrite),               FileSystemAPI::WriteAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::fsync),                FileSystemAPI::SyncFunction>);
        static_assert(std::is_convertible_v<decltype(&::ftruncate),            FileSystemAPI::ResizeFunction>);
        static_assert(std::is_convertible_v<decltype(&::fstat),                FileSystemAPI::StatFunction>);
        static_assert(std::is_convertible_v<decltype(&::mmap),                 FileMappingSystemAPI::MapFunction>);
        static_assert(std::is_convertible_v<decltype(&::munmap),               FileMappingSystemAPI::UnmapFunction>);
        static_assert(std::is_convertible_v<decltype(&::msync),                FileMappingSystemAPI::SyncMapFunction>);
        static_assert(std::is_convertible_v<decltype(&::madvise),              FileMappingSystemAPI::AdviseMapFunction>);
        static_assert(std::is_convertible_v<decltype(&::rename),               FileSystemAPI::RenameFunction>);
        static_assert(std::is_convertible_v<decltype(&::unlink),               FileSystemAPI::UnlinkFunction>);
        static_assert(std::is_convertible_v<decltype(&::fcntl),                FileSystemAPI::ControlFunction>);
        static_assert(std::is_convertible_v<decltype(&::posix_fadvise),        FileSystemAPI::AdviseFileFunction>);
        static_assert(std::is_convertible_v<decltype(&::flock),                FileSystemAPI::LockFunction>);


        static_assert(sizeof(PosixSystem::ThreadId)          == sizeof(pthread_t));
        static_assert(sizeof(PosixSystem::ThreadAttributes*) == sizeof(pthread_attr_t*));
        static_assert(sizeof(PosixSystem::FileOffset)        == sizeof(off_t));
        static_assert(sizeof(PosixSystem::FileMode)          == sizeof(mode_t));
        static_assert(sizeof(PosixSystem::PollCount)         == sizeof(nfds_t));
        static_assert(alignof(PosixSystem::ThreadId)         == alignof(pthread_t));
        static_assert(alignof(PosixSystem::FileOffset)       == alignof(off_t));
        static_assert(alignof(PosixSystem::FileMode)         == alignof(mode_t));
        static_assert(PosixSystem::kProtectionRead   == PROT_READ);
        static_assert(PosixSystem::kProtectionWrite  == PROT_WRITE);
        static_assert(PosixSystem::kMapShared        == MAP_SHARED);
        static_assert(PosixSystem::kMapPrivate       == MAP_PRIVATE);
        static_assert(PosixSystem::kSynchronize      == MS_SYNC);
        static_assert(PosixSystem::kAdviceNormal     == MADV_NORMAL);
        static_assert(PosixSystem::kAdviceSequential == MADV_SEQUENTIAL);
        static_assert(PosixSystem::kAdviceRandom     == MADV_RANDOM);
        static_assert(PosixSystem::kAdviceWillNeed   == MADV_WILLNEED);
        static_assert(PosixSystem::kAdviceDontNeed   == MADV_DONTNEED);
        static_assert(PosixSystem::kDynamicLazy      == RTLD_LAZY);
        static_assert(PosixSystem::kDynamicNow       == RTLD_NOW);
        static_assert(PosixSystem::kDynamicLocal     == RTLD_LOCAL);
        static_assert(PosixSystem::kDynamicGlobal    == RTLD_GLOBAL);
#    if defined(PLATFORM_LINUX)
        static_assert(sizeof(PosixSystem::InotifyEventHeader)                     == sizeof(inotify_event));
        static_assert(alignof(PosixSystem::InotifyEventHeader)                    == alignof(inotify_event));
        static_assert(offsetof(PosixSystem::InotifyEventHeader, watchDescriptor)  == offsetof(inotify_event, wd));
        static_assert(offsetof(PosixSystem::InotifyEventHeader, nameLength)       == offsetof(inotify_event, len));
        static_assert(sizeof(PosixSystem::PollDescriptor)                         == sizeof(pollfd));
        static_assert(alignof(PosixSystem::PollDescriptor)                        == alignof(pollfd));
        static_assert(offsetof(PosixSystem::PollDescriptor, descriptor)           == offsetof(pollfd, fd));
        static_assert(offsetof(PosixSystem::PollDescriptor, events)               == offsetof(pollfd, events));
        static_assert(offsetof(PosixSystem::PollDescriptor, revents)              == offsetof(pollfd, revents));
        static_assert(std::is_convertible_v<decltype(&::socket),                  SocketSystemAPI::SocketFunction>);
        static_assert(std::is_convertible_v<decltype(&::close),                   SocketSystemAPI::CloseFunction>);
        static_assert(std::is_convertible_v<decltype(&::bind),                    SocketSystemAPI::BindFunction>);
        static_assert(std::is_convertible_v<decltype(&::getsockname),             SocketSystemAPI::NameFunction>);
        static_assert(std::is_convertible_v<decltype(&::setsockopt),              SocketSystemAPI::OptionFunction>);
        static_assert(std::is_convertible_v<decltype(&::getsockopt),              SocketSystemAPI::GetOptionFunction>);
        static_assert(std::is_convertible_v<decltype(&::ioctl),                   SocketSystemAPI::IoctlFunction>);
        static_assert(std::is_convertible_v<decltype(&::listen),                  TcpSystemAPI::ListenFunction>);
        static_assert(std::is_convertible_v<decltype(&::accept),                  TcpSystemAPI::AcceptFunction>);
        static_assert(std::is_convertible_v<decltype(&::connect),                 TcpSystemAPI::ConnectFunction>);
        static_assert(std::is_convertible_v<decltype(&::send),                    TcpSystemAPI::SendFunction>);
        static_assert(std::is_convertible_v<decltype(&::recv),                    TcpSystemAPI::TransferFunction>);
        static_assert(std::is_convertible_v<decltype(&::shutdown),                TcpSystemAPI::ShutdownFunction>);
        static_assert(std::is_convertible_v<decltype(&::sendto),                  UdpSystemAPI::SendToFunction>);
        static_assert(std::is_convertible_v<decltype(&::recvfrom),                UdpSystemAPI::ReceiveFromFunction>);
        static_assert(std::is_convertible_v<decltype(&::clock_gettime),           ClockSystemAPI::GetTimeFunction>);
        static_assert(std::is_convertible_v<decltype(&::clock_getres),            ClockSystemAPI::GetResolutionFunction>);
        static_assert(std::is_convertible_v<decltype(&::clock_nanosleep),         ClockSystemAPI::SleepFunction>);
        static_assert(std::is_convertible_v<decltype(&::sendmsg),                 AdvancedSocketSystemAPI::SendMessageFunction>);
        static_assert(std::is_convertible_v<decltype(&::recvmsg),                 AdvancedSocketSystemAPI::ReceiveMessageFunction>);
        static_assert(std::is_convertible_v<decltype(&::getifaddrs),              NetworkInterfaceSystemAPI::GetIfAddressesFunction>);
        static_assert(std::is_convertible_v<decltype(&::freeifaddrs),             NetworkInterfaceSystemAPI::FreeIfAddressesFunction>);
        static_assert(std::is_convertible_v<decltype(&::if_nametoindex),          NetworkInterfaceSystemAPI::IfNameToIndexFunction>);
        static_assert(std::is_convertible_v<decltype(&::if_indextoname),          NetworkInterfaceSystemAPI::IfIndexToNameFunction>);
        static_assert(std::is_convertible_v<decltype(&::fork),                    ProcessControlSystemAPI::ForkFunction>);
        static_assert(std::is_convertible_v<decltype(&::execve),                  ProcessControlSystemAPI::ExecFunction>);
        static_assert(
            std::is_convertible_v<decltype(&::execv), ProcessControlSystemAPI::ExecPathFunction>);
        static_assert(std::is_convertible_v<decltype(&::waitpid),                 ProcessControlSystemAPI::WaitFunction>);
        static_assert(sizeof(PosixSystem::ProcessId) == sizeof(::pid_t));
        static_assert(sizeof(PosixSystem::WaitId)    == sizeof(::id_t));
        static_assert(std::is_convertible_v<decltype(&::kill),                    ProcessControlSystemAPI::SignalFunction>);
        static_assert(std::is_convertible_v<decltype(&::dup2),                    FileSystemAPI::DuplicateToFunction>);
        static_assert(PosixSystem::kStandardErrorDescriptor == STDERR_FILENO);
        static_assert(PosixSystem::kAbortSignal == SIGABRT);
        static_assert(PosixSystem::kKillSignal == SIGKILL);
        static_assert(SocketSystemAPI::kNameInfoNumericHost == NI_NUMERICHOST);
        static_assert(SocketSystemAPI::kNameInfoNumericService == NI_NUMERICSERV);
        static_assert(PosixSystem::ProcessExited(0) == static_cast<bool>(WIFEXITED(0)));
        static_assert(std::is_convertible_v<decltype(&::openat),                  ModernFileSystemAPI::OpenAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::mkdirat),                 ModernFileSystemAPI::MakeDirectoryAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::unlinkat),                ModernFileSystemAPI::UnlinkAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::renameat),                ModernFileSystemAPI::RenameAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::readlinkat),              ModernFileSystemAPI::ReadLinkAtFunction>);
        static_assert(std::is_convertible_v<decltype(&::sendfile),                ModernFileSystemAPI::SendFileFunction>);
        static_assert(std::is_convertible_v<decltype(&::fallocate),               ModernFileSystemAPI::AllocateFunction>);
        static_assert(std::is_convertible_v<decltype(&::syncfs),                  ModernFileSystemAPI::SyncFileSystemFunction>);
        static_assert(std::is_convertible_v<decltype(&::mprotect),                VirtualMemorySystemAPI::ProtectFunction>);
        static_assert(std::is_convertible_v<decltype(&::mlock),                   VirtualMemorySystemAPI::LockFunction>);
        static_assert(std::is_convertible_v<decltype(&::munlock),                 VirtualMemorySystemAPI::UnlockFunction>);
#    if defined(PLATFORM_LINUX)   
        static_assert(sizeof(__ptrace_request) == sizeof(int));
        static_assert(static_cast<int>(PosixSystem::TraceRequest::TraceMe)              == PTRACE_TRACEME);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::PeekText)             == PTRACE_PEEKTEXT);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::PeekData)             == PTRACE_PEEKDATA);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::PeekUser)             == PTRACE_PEEKUSER);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::PokeText)             == PTRACE_POKETEXT);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::PokeData)             == PTRACE_POKEDATA);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::PokeUser)             == PTRACE_POKEUSER);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::Continue)             == PTRACE_CONT);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::Kill)                 == PTRACE_KILL);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SingleStep)           == PTRACE_SINGLESTEP);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::Attach)               == PTRACE_ATTACH);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::Detach)               == PTRACE_DETACH);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SystemCall)           == PTRACE_SYSCALL);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SetOptions)           == PTRACE_SETOPTIONS);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetEventMessage)      == PTRACE_GETEVENTMSG);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetSignalInfo)        == PTRACE_GETSIGINFO);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SetSignalInfo)        == PTRACE_SETSIGINFO);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetRegisterSet)       == PTRACE_GETREGSET);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SetRegisterSet)       == PTRACE_SETREGSET);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::Seize)                == PTRACE_SEIZE);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::Interrupt)            == PTRACE_INTERRUPT);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::Listen)               == PTRACE_LISTEN);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::PeekSignalInfo)       == PTRACE_PEEKSIGINFO);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetSignalMask)        == PTRACE_GETSIGMASK);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SetSignalMask)        == PTRACE_SETSIGMASK);
#        if defined(PTRACE_SECCOMP_GET_FILTER)
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetSeccompFilter)     == PTRACE_SECCOMP_GET_FILTER);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetSeccompMetadata)   == PTRACE_SECCOMP_GET_METADATA);
#        endif
#        if defined(PTRACE_GET_SYSCALL_INFO)
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetSystemCallInfo)    == PTRACE_GET_SYSCALL_INFO);
#        endif
#        if defined(PTRACE_GET_RSEQ_CONFIGURATION)
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetRseqConfiguration) == PTRACE_GET_RSEQ_CONFIGURATION);
#        endif
#        if defined(PTRACE_SET_SYSCALL_USER_DISPATCH_CONFIG)
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SetSystemCallUserDispatchConfiguration) == PTRACE_SET_SYSCALL_USER_DISPATCH_CONFIG);
        static_assert(static_cast<int>(PosixSystem::TraceRequest::GetSystemCallUserDispatchConfiguration) == PTRACE_GET_SYSCALL_USER_DISPATCH_CONFIG);
#        endif
#        if defined(PTRACE_SET_SYSCALL_INFO)
        static_assert(static_cast<int>(PosixSystem::TraceRequest::SetSystemCallInfo)    == PTRACE_SET_SYSCALL_INFO);
#        endif
        static_assert(PosixSystem::kTraceOptionSystemCallsGood == PTRACE_O_TRACESYSGOOD);
        static_assert(PosixSystem::kTraceOptionFork            == PTRACE_O_TRACEFORK);
        static_assert(PosixSystem::kTraceOptionVFork           == PTRACE_O_TRACEVFORK);
        static_assert(PosixSystem::kTraceOptionClone           == PTRACE_O_TRACECLONE);
        static_assert(PosixSystem::kTraceOptionExec            == PTRACE_O_TRACEEXEC);
        static_assert(PosixSystem::kTraceOptionVForkDone       == PTRACE_O_TRACEVFORKDONE);
        static_assert(PosixSystem::kTraceOptionExit            == PTRACE_O_TRACEEXIT);
        static_assert(PosixSystem::kTraceOptionSeccomp         == PTRACE_O_TRACESECCOMP);
        static_assert(PosixSystem::kTraceOptionExitKill        == PTRACE_O_EXITKILL);
#        if defined(PTRACE_O_SUSPEND_SECCOMP)
        static_assert(PosixSystem::kTraceOptionSuspendSeccomp  == PTRACE_O_SUSPEND_SECCOMP);
        static_assert(PosixSystem::kTraceOptionMask            == PTRACE_O_MASK);
#        endif
        static_assert(PosixSystem::kTraceEventFork             == PTRACE_EVENT_FORK);
        static_assert(PosixSystem::kTraceEventVFork            == PTRACE_EVENT_VFORK);
        static_assert(PosixSystem::kTraceEventClone            == PTRACE_EVENT_CLONE);
        static_assert(PosixSystem::kTraceEventExec             == PTRACE_EVENT_EXEC);
        static_assert(PosixSystem::kTraceEventVForkDone        == PTRACE_EVENT_VFORK_DONE);
        static_assert(PosixSystem::kTraceEventExit             == PTRACE_EVENT_EXIT);
        static_assert(PosixSystem::kTraceEventSeccomp          == PTRACE_EVENT_SECCOMP);
        static_assert(PosixSystem::kTraceEventStop             == PTRACE_EVENT_STOP);
        static_assert(std::is_convertible_v<decltype(&::accept4),                 AdvancedSocketSystemAPI::Accept4Function>);
        static_assert(std::is_convertible_v<decltype(&::sendmmsg),                AdvancedSocketSystemAPI::SendMessagesFunction>);
        static_assert(std::is_convertible_v<decltype(&::recvmmsg),                AdvancedSocketSystemAPI::ReceiveMessagesFunction>);
        static_assert(std::is_convertible_v<decltype(&::fanotify_init),           FanotifySystemAPI::InitializeFunction>);
        static_assert(std::is_convertible_v<decltype(&::fanotify_mark),           FanotifySystemAPI::MarkFunction>);
        static_assert(std::is_convertible_v<decltype(&::getauxval),               SystemInformationSystemAPI::AuxiliaryValueFunction>);
        static_assert(std::is_convertible_v<decltype(&::prctl),                   SystemInformationSystemAPI::ProcessControlFunction>);
        static_assert(std::is_convertible_v<decltype(&::statvfs),                 SystemInformationSystemAPI::FileSystemStatisticsFunction>);
#    endif   
        static_assert(std::is_convertible_v<decltype(&::epoll_create1),           EpollSystemAPI::CreateFunction>);
        static_assert(std::is_convertible_v<decltype(&::epoll_ctl),               EpollSystemAPI::ControlFunction>);
        static_assert(std::is_convertible_v<decltype(&::epoll_wait),              EpollSystemAPI::WaitFunction>);
        static_assert(std::is_convertible_v<decltype(&::eventfd),                 EventFdSystemAPI::CreateFunction>);
        static_assert(std::is_convertible_v<decltype(&::timerfd_create),          TimerFdSystemAPI::CreateFunction>);
        static_assert(std::is_convertible_v<decltype(&::timerfd_settime),         TimerFdSystemAPI::SetTimeFunction>);
        static_assert(std::is_convertible_v<decltype(&::timerfd_gettime),         TimerFdSystemAPI::GetTimeFunction>);
        static_assert(PosixSystem::kOpenNonBlocking       == O_NONBLOCK);
        static_assert(PosixSystem::kOpenCloseOnExec       == O_CLOEXEC);
        static_assert(PosixSystem::kOpenReadOnly          == O_RDONLY);
        static_assert(PosixSystem::kOpenWriteOnly         == O_WRONLY);
        static_assert(PosixSystem::kOpenReadWrite         == O_RDWR);
        static_assert(PosixSystem::kOpenCreate            == O_CREAT);
        static_assert(PosixSystem::kOpenExclusive         == O_EXCL);
        static_assert(PosixSystem::kOpenTruncate          == O_TRUNC);
        static_assert(PosixSystem::kOpenDirectory         == O_DIRECTORY);
        static_assert(PosixSystem::kOpenDirect            == O_DIRECT);
        static_assert(PosixSystem::kOpenSync              == O_SYNC);
        static_assert(PosixSystem::kPageSizeConfiguration == _SC_PAGESIZE);
        static_assert(PosixSystem::kPhysicalPageCountConfiguration == _SC_PHYS_PAGES);
        static_assert(PosixSystem::kAvailablePhysicalPageCountConfiguration == _SC_AVPHYS_PAGES);
#        if defined(__aarch64__)
        static_assert(PosixSystem::kHardwareCapabilities     == AT_HWCAP);
        static_assert(PosixSystem::kHardwareCapabilities2    == AT_HWCAP2);
        static_assert(PosixSystem::kArmFloatingPoint         == HWCAP_FP);
        static_assert(PosixSystem::kArmAdvancedSimd          == HWCAP_ASIMD);
        static_assert(PosixSystem::kArmScalableVector        == HWCAP_SVE);
        static_assert(PosixSystem::kArmScalableVector2       == HWCAP2_SVE2);
        static_assert(PosixSystem::kGetScalableVectorLength  == PR_SVE_GET_VL);
        static_assert(PosixSystem::kScalableVectorLengthMask == PR_SVE_VL_LEN_MASK);
#        endif
        static_assert(PosixSystem::kFileAdviceSequential  == POSIX_FADV_SEQUENTIAL);
        static_assert(PosixSystem::kFileAdviceRandom      == POSIX_FADV_RANDOM);
        static_assert(PosixSystem::kEventModify           == IN_MODIFY);
        static_assert(PosixSystem::kEventAttrib           == IN_ATTRIB);
        static_assert(PosixSystem::kEventCloseWrite       == IN_CLOSE_WRITE);
        static_assert(PosixSystem::kEventMovedFrom        == IN_MOVED_FROM);
        static_assert(PosixSystem::kEventMovedTo          == IN_MOVED_TO);
        static_assert(PosixSystem::kEventCreate           == IN_CREATE);
        static_assert(PosixSystem::kEventDelete           == IN_DELETE);
        static_assert(PosixSystem::kEventDeleteSelf       == IN_DELETE_SELF);
        static_assert(PosixSystem::kEventMoveSelf         == IN_MOVE_SELF);
        static_assert(PosixSystem::kEventQueueOverflow    == IN_Q_OVERFLOW);
        static_assert(PosixSystem::kEventIgnored          == IN_IGNORED);
        static_assert(PosixSystem::kEventIsDirectory      == IN_ISDIR);
        static_assert(PosixSystem::kPollInput             == POLLIN);
        static_assert(PosixSystem::kPollError             == POLLERR);
        static_assert(PosixSystem::kPollHangup            == POLLHUP);
        static_assert(PosixSystem::kPollInvalid           == POLLNVAL);
#    elif defined(PLATFORM_MACOS)
        static_assert(std::is_convertible_v<decltype(&::sysctlbyname), SystemInformationSystemAPI::SysctlByNameFunction>);
        static_assert(sizeof(PosixSystem::KernelEvent)                 == sizeof(kevent));
        static_assert(alignof(PosixSystem::KernelEvent)                == alignof(kevent));
        static_assert(offsetof(PosixSystem::KernelEvent, identifier)   == offsetof(kevent, ident));
        static_assert(offsetof(PosixSystem::KernelEvent, filter)       == offsetof(kevent, filter));
        static_assert(offsetof(PosixSystem::KernelEvent, flags)        == offsetof(kevent, flags));
        static_assert(offsetof(PosixSystem::KernelEvent, filterFlags)  == offsetof(kevent, fflags));
        static_assert(offsetof(PosixSystem::KernelEvent, data)         == offsetof(kevent, data));
        static_assert(offsetof(PosixSystem::KernelEvent, userData)     == offsetof(kevent, udata));
        static_assert(sizeof(PosixSystem::TimeSpec)                    == sizeof(timespec));
        static_assert(alignof(PosixSystem::TimeSpec)                   == alignof(timespec));
        static_assert(PosixSystem::kOpenEventOnly                      == O_EVTONLY);
        static_assert(PosixSystem::kOpenCloseOnExec                    == O_CLOEXEC);
        static_assert(PosixSystem::kOpenReadOnly                       == O_RDONLY);
        static_assert(PosixSystem::kOpenWriteOnly                      == O_WRONLY);
        static_assert(PosixSystem::kOpenReadWrite                      == O_RDWR);
        static_assert(PosixSystem::kOpenCreate                         == O_CREAT);
        static_assert(PosixSystem::kOpenExclusive                      == O_EXCL);
        static_assert(PosixSystem::kOpenTruncate                       == O_TRUNC);
        static_assert(PosixSystem::kOpenDirectory                      == O_DIRECTORY);
        static_assert(PosixSystem::kOpenSync                           == O_SYNC);
        static_assert(PosixSystem::kNoCacheControl                     == F_NOCACHE);
        static_assert(PosixSystem::kFullSyncControl                    == F_FULLFSYNC);
        static_assert(PosixSystem::kPageSizeConfiguration              == _SC_PAGESIZE);
        static_assert(PosixSystem::kFilterVnode                        == EVFILT_VNODE);
        static_assert(PosixSystem::kEventAdd                           == EV_ADD);
        static_assert(PosixSystem::kEventClear                         == EV_CLEAR);
        static_assert(PosixSystem::kNoteWrite                          == NOTE_WRITE);
        static_assert(PosixSystem::kNoteDelete                         == NOTE_DELETE);
        static_assert(PosixSystem::kNoteRename                         == NOTE_RENAME);
        static_assert(PosixSystem::kNoteExtend                         == NOTE_EXTEND);
        static_assert(PosixSystem::kNoteAttribute                      == NOTE_ATTRIB);
#    endif
        // clang-format on
#endif

#if defined(PLATFORM_LINUX)
        using NativeEpollWaitMaskedFunction = decltype(&::epoll_pwait);
        using NativeEpollWaitMaskedTimeFunction
            = int (*)(int, ::epoll_event*, int, const ::timespec*, const ::sigset_t*);
        using NativeSignalFdFunction = decltype(&::signalfd);
        using NativeIoUringSubmitAndWaitTimeoutFunction
            = int (*)(::io_uring*, ::io_uring_cqe**, unsigned, ::__kernel_timespec*, ::sigset_t*);
        using NativeIoUringWaitCompletionsFunction
            = int (*)(::io_uring*, ::io_uring_cqe**, unsigned, ::__kernel_timespec*, ::sigset_t*);

        NativeEpollWaitMaskedFunction gNativeEpollWaitMasked = nullptr;
        NativeEpollWaitMaskedTimeFunction gNativeEpollWaitMaskedTime = nullptr;
        NativeSignalFdFunction gNativeSignalFd = nullptr;
        NativeIoUringSubmitAndWaitTimeoutFunction gNativeIoUringSubmitAndWaitTimeout = nullptr;
        NativeIoUringWaitCompletionsFunction gNativeIoUringWaitCompletions = nullptr;

        int EpollWaitMaskedAdapter(int descriptor, ::epoll_event* events, int eventCount, int timeoutMilliseconds,
                                   const PosixSystem::SignalSet* mask) noexcept {
            return gNativeEpollWaitMasked(descriptor, events, eventCount, timeoutMilliseconds,
                                          reinterpret_cast<const ::sigset_t*>(mask));
        }

        int EpollWaitMaskedTimeAdapter(int descriptor, ::epoll_event* events, int eventCount, const ::timespec* timeout,
                                       const PosixSystem::SignalSet* mask) noexcept {
            return gNativeEpollWaitMaskedTime(descriptor, events, eventCount, timeout,
                                              reinterpret_cast<const ::sigset_t*>(mask));
        }

        int SignalFdAdapter(int descriptor, const PosixSystem::SignalSet* mask, int flags) noexcept {
            return gNativeSignalFd(descriptor, reinterpret_cast<const ::sigset_t*>(mask), flags);
        }

        int IoUringSubmitAndWaitTimeoutAdapter(::io_uring* ring, ::io_uring_cqe** completion, unsigned waitCount,
                                               ::__kernel_timespec* timeout, PosixSystem::SignalSet* mask) noexcept {
            return gNativeIoUringSubmitAndWaitTimeout(ring, completion, waitCount, timeout,
                                                      reinterpret_cast<::sigset_t*>(mask));
        }

        int IoUringWaitCompletionsAdapter(::io_uring* ring, ::io_uring_cqe** completion, unsigned waitCount,
                                          ::__kernel_timespec* timeout, PosixSystem::SignalSet* mask) noexcept {
            return gNativeIoUringWaitCompletions(ring, completion, waitCount, timeout,
                                                 reinterpret_cast<::sigset_t*>(mask));
        }

        template<typename... Arguments>
        [[nodiscard]] long InvokeLinuxSyscall(long number, Arguments... arguments) noexcept {
            const auto syscallFunction = LoadLinuxSyscallSystemAPI().syscall;
            if (syscallFunction == nullptr) {
                errno = ENOSYS;
                return -1;
            }
            return syscallFunction(number, arguments...);
        }

#    if defined(SYS_openat2)
        int OpenAt2ViaSyscall(int directoryDescriptor, const char* path, const void* how, size_t size) noexcept {
            return static_cast<int>(InvokeLinuxSyscall(SYS_openat2, directoryDescriptor, path, how, size));
        }
#    endif

#    if defined(SYS_tgkill)
        int ThreadKillViaSyscall(int processId, int threadId, int signal) noexcept {
            return static_cast<int>(InvokeLinuxSyscall(SYS_tgkill, processId, threadId, signal));
        }
#    endif

#    if defined(SYS_pidfd_open)
        int ProcessDescriptorOpenViaSyscall(int processId, unsigned int flags) noexcept {
            return static_cast<int>(InvokeLinuxSyscall(SYS_pidfd_open, processId, flags));
        }
#    endif

#    if defined(SYS_pidfd_send_signal)
        int ProcessDescriptorSendSignalViaSyscall(int processDescriptor, int signal,
                                                  PosixSystem::SignalInfo* information, unsigned int flags) noexcept {
            return static_cast<int>(InvokeLinuxSyscall(SYS_pidfd_send_signal, processDescriptor, signal,
                                                       reinterpret_cast<::siginfo_t*>(information), flags));
        }
#    endif

#    if defined(SYS_perf_event_open)
        long OpenPerfEventViaSyscall(void* attributes, int processId, int cpu, int groupFileDescriptor,
                                     unsigned long flags) noexcept {
            return InvokeLinuxSyscall(SYS_perf_event_open, attributes, processId, cpu, groupFileDescriptor, flags);
        }
#    endif
#endif

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        using NativeSpawnFunction = decltype(&::posix_spawn);
        using NativeWaitIdFunction = decltype(&::waitid);
        using NativeSignalActionFunction = decltype(&::sigaction);

        NativeSpawnFunction gNativeSpawn = nullptr;
        NativeWaitIdFunction gNativeWaitId = nullptr;
        NativeSignalActionFunction gNativeSignalAction = nullptr;

        int SpawnAdapter(PosixSystem::ProcessId* process, const char* path,
                         const PosixSystem::SpawnFileActions* fileActions,
                         const PosixSystem::SpawnAttributes* attributes, char* const arguments[],
                         char* const environment[]) noexcept {
            return gNativeSpawn(reinterpret_cast<pid_t*>(process), path,
                                reinterpret_cast<const posix_spawn_file_actions_t*>(fileActions),
                                reinterpret_cast<const posix_spawnattr_t*>(attributes), arguments, environment);
        }

        int WaitIdAdapter(PosixSystem::WaitIdType type, PosixSystem::WaitId identifier,
                          PosixSystem::SignalInfo* information, int options) noexcept {
            return gNativeWaitId(static_cast<idtype_t>(type), static_cast<id_t>(identifier),
                                 reinterpret_cast<siginfo_t*>(information), options);
        }

        int SignalActionAdapter(int signalNumber, const PosixSystem::SignalAction* action,
                                PosixSystem::SignalAction* previousAction) noexcept {
            return gNativeSignalAction(signalNumber, reinterpret_cast<const struct sigaction*>(action),
                                       reinterpret_cast<struct sigaction*>(previousAction));
        }

        static_assert(std::is_convertible_v<decltype(&SpawnAdapter), ProcessControlSystemAPI::SpawnFunction>);
        static_assert(std::is_convertible_v<decltype(&WaitIdAdapter), ProcessControlSystemAPI::WaitIdFunction>);
        static_assert(
            std::is_convertible_v<decltype(&SignalActionAdapter), ProcessControlSystemAPI::SignalActionFunction>);
#endif

#if defined(PLATFORM_WINDOWS)
        using WindowsCrashNativeAPI = StructuredExceptionSystemAPI;

        [[nodiscard]] const WindowsCrashNativeAPI& LoadWindowsCrashNativeAPI() noexcept {
            static const WindowsCrashNativeAPI api = [] {
                WindowsCrashNativeAPI loaded{};
                const ModulePtr& kernelModule = Kernel32Module();
                if (kernelModule == nullptr) {
                    return loaded;
                }
                // clang-format off
                loaded.setUnhandledExceptionFilter    = kernelModule->TryFindFunction<WindowsCrashNativeAPI::SetUnhandledExceptionFilterFunction>("SetUnhandledExceptionFilter");
                loaded.addVectoredExceptionHandler    = kernelModule->TryFindFunction<WindowsCrashNativeAPI::AddVectoredExceptionHandlerFunction>("AddVectoredExceptionHandler");
                loaded.removeVectoredExceptionHandler = kernelModule->TryFindFunction<WindowsCrashNativeAPI::RemoveVectoredExceptionHandlerFunction>("RemoveVectoredExceptionHandler");
                loaded.raiseException                 = kernelModule->TryFindFunction<WindowsCrashNativeAPI::RaiseExceptionFunction>("RaiseException");
                loaded.getErrorMode                   = kernelModule->TryFindFunction<WindowsCrashNativeAPI::GetErrorModeFunction>("GetErrorMode");
                loaded.setErrorMode                   = kernelModule->TryFindFunction<WindowsCrashNativeAPI::SetErrorModeFunction>("SetErrorMode");
                loaded.werGetFlags                    = kernelModule->TryFindFunction<WindowsCrashNativeAPI::WerGetFlagsFunction>("WerGetFlags");
                loaded.werSetFlags                    = kernelModule->TryFindFunction<WindowsCrashNativeAPI::WerSetFlagsFunction>("WerSetFlags");
                // clang-format on
                return loaded;
            }();
            return api;
        }

        inline constexpr WindowsSystem::DWord kSuppressedErrorModeFlags
            = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
        inline constexpr WindowsSystem::DWord kWerNoUiFlag = 32;
        inline constexpr WindowsSystem::HResult kWerFlagsNotInitialized = -2147023728L;

        struct WindowsCrashRegistration {
            NativeCrashCallback callback = nullptr;
            void* userData = nullptr;
            WindowsCrashNativeAPI::ExceptionFilterFunction previousFilter = nullptr;
            WindowsSystem::DWord addedErrorModeFlags = 0;
            WindowsSystem::DWord addedWerFlags = 0;
            WindowsSystem::Handle process = nullptr;
            bool suppressDialogs = false;
            bool nativeHandlerInstalled = false;
        };

        struct WindowsCrashRegistry {
            std::mutex mutex;
            WindowsCrashRegistration registration;
            std::atomic<WindowsCrashRegistration*> active{nullptr};
        };

        WindowsCrashRegistry gNativeCrashRegistry;
        static_assert(decltype(WindowsCrashRegistry::active)::is_always_lock_free);

        [[nodiscard]] constexpr std::string_view ExceptionName(WindowsSystem::DWord code) noexcept {
            switch (code) {
                case EXCEPTION_ACCESS_VIOLATION:
                    return "ACCESS_VIOLATION";
                case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
                    return "ARRAY_BOUNDS_EXCEEDED";
                case EXCEPTION_DATATYPE_MISALIGNMENT:
                    return "DATATYPE_MISALIGNMENT";
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:
                    return "FLOAT_DIVIDE_BY_ZERO";
                case EXCEPTION_FLT_INVALID_OPERATION:
                    return "FLOAT_INVALID_OPERATION";
                case EXCEPTION_ILLEGAL_INSTRUCTION:
                    return "ILLEGAL_INSTRUCTION";
                case EXCEPTION_IN_PAGE_ERROR:
                    return "IN_PAGE_ERROR";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                    return "INTEGER_DIVIDE_BY_ZERO";
                case EXCEPTION_INT_OVERFLOW:
                    return "INTEGER_OVERFLOW";
                case EXCEPTION_STACK_OVERFLOW:
                    return "STACK_OVERFLOW";
                default:
                    return "UNKNOWN_STRUCTURED_EXCEPTION";
            }
        }

        [[nodiscard]] uintptr_t WindowsInstructionPointer(const CONTEXT* context) noexcept {
            if (context == nullptr) {
                return 0;
            }
#    if defined(_M_X64) || defined(__x86_64__)
            return static_cast<uintptr_t>(context->Rip);
#    elif defined(_M_IX86) || defined(__i386__)
            return static_cast<uintptr_t>(context->Eip);
#    elif defined(_M_ARM64) || defined(__aarch64__)
            return static_cast<uintptr_t>(context->Pc);
#    else
            return 0;
#    endif
        }

        WindowsSystem::Long __stdcall DispatchWindowsCrash(EXCEPTION_POINTERS* exception) noexcept {
            WindowsCrashRegistration* registration = gNativeCrashRegistry.active.load(std::memory_order_acquire);
            if (registration == nullptr || registration->callback == nullptr) {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const EXCEPTION_RECORD* record = exception != nullptr ? exception->ExceptionRecord : nullptr;
            const WindowsSystem::DWord code = record != nullptr ? record->ExceptionCode : 0;
            const NativeCrashContext context{
                .reason = ExceptionName(code),
                .code = code,
                .faultAddress = record != nullptr ? reinterpret_cast<uintptr_t>(record->ExceptionAddress) : 0,
                .instructionPointer
                = WindowsInstructionPointer(exception != nullptr ? exception->ContextRecord : nullptr),
            };
            registration->callback(context, registration->userData);

            if (registration->suppressDialogs) {
                return EXCEPTION_EXECUTE_HANDLER;
            }
            if (registration->previousFilter != nullptr) {
                return registration->previousFilter(exception);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        void RestoreWindowsCrashPolicy(WindowsCrashRegistration& registration,
                                       const WindowsCrashNativeAPI& api) noexcept {
            if (registration.addedErrorModeFlags != 0 && api.getErrorMode != nullptr && api.setErrorMode != nullptr) {
                const WindowsSystem::DWord currentMode = api.getErrorMode();
                api.setErrorMode(currentMode & ~registration.addedErrorModeFlags);
            }
            if (registration.addedWerFlags
                != 0
                && registration.process
                != nullptr
                && api.werGetFlags
                != nullptr
                && api.werSetFlags
                != nullptr) {
                WindowsSystem::DWord currentFlags = 0;
                if (WindowsSystem::Succeeded(api.werGetFlags(registration.process, &currentFlags))) {
                    api.werSetFlags(currentFlags & ~registration.addedWerFlags);
                }
            }
            registration.addedErrorModeFlags = 0;
            registration.addedWerFlags = 0;
            registration.process = nullptr;
        }

        bool InstallNativeCrashHandlerImpl(NativeCrashCallback callback, void* userData,
                                           bool suppressDialogs) noexcept {
            if (callback == nullptr) {
                return false;
            }

            std::lock_guard lock{gNativeCrashRegistry.mutex};
            if (gNativeCrashRegistry.active.load(std::memory_order_relaxed) != nullptr) {
                return false;
            }

            const WindowsCrashNativeAPI& api = LoadWindowsCrashNativeAPI();
            if (api.setUnhandledExceptionFilter == nullptr) {
                return false;
            }

            WindowsCrashRegistration& registration = gNativeCrashRegistry.registration;
            registration = {
                .callback = callback,
                .userData = userData,
                .suppressDialogs = suppressDialogs,
            };

            if (suppressDialogs) {
                const ProcessSystemAPI& processAPI = LoadProcessSystemAPI();
                if (!EnsureSystemAPIs(api.getErrorMode, api.setErrorMode, api.werGetFlags, api.werSetFlags,
                                      processAPI.getCurrentProcess)) {
                    registration = {};
                    return false;
                }

                registration.process = processAPI.getCurrentProcess();
                if (registration.process == nullptr) {
                    registration = {};
                    return false;
                }

                const WindowsSystem::DWord currentMode = api.getErrorMode();
                registration.addedErrorModeFlags = kSuppressedErrorModeFlags & ~currentMode;
                if (registration.addedErrorModeFlags != 0) {
                    api.setErrorMode(currentMode | registration.addedErrorModeFlags);
                }

                WindowsSystem::DWord currentWerFlags = 0;
                const WindowsSystem::HResult result = api.werGetFlags(registration.process, &currentWerFlags);
                if (!WindowsSystem::Succeeded(result) && result != kWerFlagsNotInitialized) {
                    RestoreWindowsCrashPolicy(registration, api);
                    registration = {};
                    return false;
                }

                registration.addedWerFlags = kWerNoUiFlag & ~currentWerFlags;
                if (registration.addedWerFlags
                    != 0
                    && !WindowsSystem::Succeeded(api.werSetFlags(currentWerFlags | registration.addedWerFlags))) {
                    RestoreWindowsCrashPolicy(registration, api);
                    registration = {};
                    return false;
                }
            }

            gNativeCrashRegistry.active.store(&registration, std::memory_order_release);
            registration.previousFilter = api.setUnhandledExceptionFilter(DispatchWindowsCrash);
            registration.nativeHandlerInstalled = true;
            return true;
        }

        void UninstallNativeCrashHandlerImpl() noexcept {
            std::lock_guard lock{gNativeCrashRegistry.mutex};
            WindowsCrashRegistration* registration
                = gNativeCrashRegistry.active.exchange(nullptr, std::memory_order_acq_rel);
            if (registration == nullptr) {
                return;
            }

            const WindowsCrashNativeAPI& api = LoadWindowsCrashNativeAPI();
            if (registration->nativeHandlerInstalled && api.setUnhandledExceptionFilter != nullptr) {
                const auto displacedFilter = api.setUnhandledExceptionFilter(registration->previousFilter);
                if (displacedFilter != DispatchWindowsCrash) {
                    api.setUnhandledExceptionFilter(displacedFilter);
                }
            }
            RestoreWindowsCrashPolicy(*registration, api);
            *registration = {};
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        struct PosixCrashNativeAPI {
            // clang-format off
            using SignalActionFunction   = decltype(&::sigaction);
            using EmptySignalSetFunction = decltype(&::sigemptyset);
            using RaiseSignalFunction    = decltype(&::raise);
            using ImmediateExitFunction  = decltype(&::_exit);

            SignalActionFunction   signalAction   = nullptr;
            EmptySignalSetFunction emptySignalSet = nullptr;
            RaiseSignalFunction    raiseSignal    = nullptr;
            ImmediateExitFunction  immediateExit  = nullptr;
            // clang-format on
        };

        [[nodiscard]] const PosixCrashNativeAPI& LoadPosixCrashNativeAPI() noexcept {
            static const PosixCrashNativeAPI api = [] {
                PosixCrashNativeAPI loaded{};
                const ModulePtr& module = CurrentProcessModule();
                if (module == nullptr) {
                    return loaded;
                }
                // clang-format off
                loaded.signalAction   = module->TryFindFunction<PosixCrashNativeAPI::SignalActionFunction>("sigaction");
                loaded.emptySignalSet = module->TryFindFunction<PosixCrashNativeAPI::EmptySignalSetFunction>("sigemptyset");
                loaded.raiseSignal    = module->TryFindFunction<PosixCrashNativeAPI::RaiseSignalFunction>("raise");
                loaded.immediateExit  = module->TryFindFunction<PosixCrashNativeAPI::ImmediateExitFunction>("_exit");
                // clang-format on
                return loaded;
            }();
            return api;
        }

        inline constexpr auto kFatalSignals = std::to_array<int>({
            SIGABRT,
            SIGFPE,
            SIGILL,
            SIGSEGV,
#    ifdef SIGBUS
            SIGBUS,
#    endif
        });

        struct PosixCrashRegistration {
            NativeCrashCallback callback = nullptr;
            void* userData = nullptr;
            PosixCrashNativeAPI::RaiseSignalFunction raiseSignal = nullptr;
            PosixCrashNativeAPI::ImmediateExitFunction immediateExit = nullptr;
            std::array<struct sigaction, kFatalSignals.size()> previousActions{};
            size_t installedCount = 0;
        };

        struct PosixCrashRegistry {
            std::mutex mutex;
            PosixCrashRegistration registration;
            std::atomic<PosixCrashRegistration*> active{nullptr};
        };

        PosixCrashRegistry gNativeCrashRegistry;
        static_assert(decltype(PosixCrashRegistry::active)::is_always_lock_free);

        [[nodiscard]] constexpr std::string_view SignalName(int signalNumber) noexcept {
            switch (signalNumber) {
                case SIGABRT:
                    return "SIGABRT";
                case SIGFPE:
                    return "SIGFPE";
                case SIGILL:
                    return "SIGILL";
                case SIGSEGV:
                    return "SIGSEGV";
#    ifdef SIGBUS
                case SIGBUS:
                    return "SIGBUS";
#    endif
                default:
                    return "UNKNOWN_SIGNAL";
            }
        }

        [[nodiscard]] uintptr_t PosixInstructionPointer(const void* nativeContext) noexcept {
            if (nativeContext == nullptr) {
                return 0;
            }
            const auto* context = static_cast<const ucontext_t*>(nativeContext);
#    if defined(PLATFORM_MACOS) && defined(__x86_64__)
            return static_cast<uintptr_t>(context->uc_mcontext->__ss.__rip);
#    elif defined(PLATFORM_MACOS) && defined(__aarch64__)
            return static_cast<uintptr_t>(context->uc_mcontext->__ss.__pc);
#    elif defined(__x86_64__) && defined(REG_RIP)
            return static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
#    elif defined(__i386__) && defined(REG_EIP)
            return static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_EIP]);
#    elif defined(__aarch64__)
            return static_cast<uintptr_t>(context->uc_mcontext.pc);
#    else
            return 0;
#    endif
        }

        void DispatchPosixCrash(int signalNumber, siginfo_t* signalInfo, void* nativeContext) noexcept {
            PosixCrashRegistration* registration = gNativeCrashRegistry.active.load(std::memory_order_acquire);
            if (registration == nullptr || registration->callback == nullptr) {
                return;
            }

            const NativeCrashContext context{
                .reason = SignalName(signalNumber),
                .code = static_cast<uint32_t>(signalNumber),
                .faultAddress = signalInfo != nullptr ? reinterpret_cast<uintptr_t>(signalInfo->si_addr) : 0,
                .instructionPointer = PosixInstructionPointer(nativeContext),
            };
            registration->callback(context, registration->userData);
            if (registration->raiseSignal(signalNumber) != 0) {
                registration->immediateExit(128 + signalNumber);
            }
        }

        bool InstallNativeCrashHandlerImpl(NativeCrashCallback callback, void* userData,
                                           bool suppressDialogs) noexcept {
            static_cast<void>(suppressDialogs);
            if (callback == nullptr) {
                return false;
            }

            std::lock_guard lock{gNativeCrashRegistry.mutex};
            if (gNativeCrashRegistry.active.load(std::memory_order_relaxed) != nullptr) {
                return false;
            }

            const PosixCrashNativeAPI& api = LoadPosixCrashNativeAPI();
            if (!EnsureSystemAPIs(api.signalAction, api.emptySignalSet, api.raiseSignal, api.immediateExit)) {
                return false;
            }

            PosixCrashRegistration& registration = gNativeCrashRegistry.registration;
            registration = {
                .callback = callback,
                .userData = userData,
                .raiseSignal = api.raiseSignal,
                .immediateExit = api.immediateExit,
            };

            struct sigaction action{};
            if (api.emptySignalSet(&action.sa_mask) != 0) {
                registration = {};
                return false;
            }
            action.sa_flags = SA_SIGINFO | SA_RESETHAND;
            action.sa_sigaction = DispatchPosixCrash;

            gNativeCrashRegistry.active.store(&registration, std::memory_order_release);
            for (int signalNumber : kFatalSignals) {
                const size_t index = registration.installedCount;
                if (api.signalAction(signalNumber, &action, &registration.previousActions[index]) != 0) {
                    while (registration.installedCount != 0) {
                        --registration.installedCount;
                        api.signalAction(kFatalSignals[registration.installedCount],
                                         &registration.previousActions[registration.installedCount], nullptr);
                    }
                    gNativeCrashRegistry.active.store(nullptr, std::memory_order_release);
                    registration = {};
                    return false;
                }
                ++registration.installedCount;
            }
            return true;
        }

        void UninstallNativeCrashHandlerImpl() noexcept {
            std::lock_guard lock{gNativeCrashRegistry.mutex};
            PosixCrashRegistration* registration
                = gNativeCrashRegistry.active.exchange(nullptr, std::memory_order_acq_rel);
            if (registration == nullptr) {
                return;
            }

            const PosixCrashNativeAPI& api = LoadPosixCrashNativeAPI();
            while (registration->installedCount != 0) {
                --registration->installedCount;
                api.signalAction(kFatalSignals[registration->installedCount],
                                 &registration->previousActions[registration->installedCount], nullptr);
            }
            *registration = {};
        }
#else
        bool InstallNativeCrashHandlerImpl(NativeCrashCallback, void*, bool) noexcept {
            return false;
        }

        void UninstallNativeCrashHandlerImpl() noexcept {}
#endif
    } // namespace

    SystemError CaptureLastSystemError() noexcept {
#if defined(PLATFORM_WINDOWS)
        return static_cast<WindowsSystem::DWord>(::GetLastError());
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        return static_cast<int>(errno);
#else
        // TODO
        return 0;
#endif
    }

    inline namespace API {

        const NativeErrorSystemAPI& LoadNativeErrorSystemAPI() noexcept {
            static const NativeErrorSystemAPI api = [] {
                using API = NativeErrorSystemAPI;
                API loaded{};
#if defined(PLATFORM_WINDOWS)
                const ModulePtr& module = Kernel32Module();
                if (module == nullptr) {
                    return loaded;
                }
                // clang-format off
            loaded.getLastError      = module->TryFindFunction<API::GetLastErrorFunction>("GetLastError");
            loaded.setLastError      = module->TryFindFunction<API::SetLastErrorFunction>("SetLastError");
            loaded.formatMessageWide = module->TryFindFunction<API::FormatMessageWideFunction>("FormatMessageW");
                // clang-format on
#endif
                return loaded;
            }();
            return api;
        }

        const StructuredExceptionSystemAPI& LoadStructuredExceptionSystemAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
            return LoadWindowsCrashNativeAPI();
#else
            static const StructuredExceptionSystemAPI api{};
            return api;
#endif
        }

        const LocalMemorySystemAPI& LoadLocalMemorySystemAPI() noexcept {
            static const LocalMemorySystemAPI api = [] {
                LocalMemorySystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    loaded.localFree = module->TryFindFunction<LocalMemorySystemAPI::LocalFreeFunction>("LocalFree");
                }
#endif
                return loaded;
            }();
            return api;
        }

        const HandleSystemAPI& LoadHandleSystemAPI() noexcept {
            static const HandleSystemAPI api = [] {
                HandleSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    loaded.closeHandle = module->TryFindFunction<HandleSystemAPI::CloseHandleFunction>("CloseHandle");
                    loaded.duplicateHandle
                        = module->TryFindFunction<HandleSystemAPI::DuplicateHandleFunction>("DuplicateHandle");
                }
#endif
                return loaded;
            }();
            return api;
        }

        const SynchronizationSystemAPI& LoadSynchronizationSystemAPI() noexcept {
            static const SynchronizationSystemAPI api = [] {
                SynchronizationSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                loaded.waitForSingleObject       = module->TryFindFunction<SynchronizationSystemAPI::WaitForSingleObjectFunction>("WaitForSingleObject");
                loaded.waitForSingleObjectEx     = module->TryFindFunction<SynchronizationSystemAPI::WaitForSingleObjectExFunction>("WaitForSingleObjectEx");
                loaded.waitForMultipleObjects    = module->TryFindFunction<SynchronizationSystemAPI::WaitForMultipleObjectsFunction>("WaitForMultipleObjects");
                loaded.waitForMultipleObjectsEx  = module->TryFindFunction<SynchronizationSystemAPI::WaitForMultipleObjectsExFunction>("WaitForMultipleObjectsEx");
                loaded.signalObjectAndWait       = module->TryFindFunction<SynchronizationSystemAPI::SignalObjectAndWaitFunction>("SignalObjectAndWait");
                loaded.createMutexExWide         = module->TryFindFunction<SynchronizationSystemAPI::CreateMutexExWideFunction>("CreateMutexExW");
                loaded.releaseMutex              = module->TryFindFunction<SynchronizationSystemAPI::ReleaseMutexFunction>("ReleaseMutex");
                loaded.createSemaphoreExWide     = module->TryFindFunction<SynchronizationSystemAPI::CreateSemaphoreExWideFunction>("CreateSemaphoreExW");
                loaded.releaseSemaphore          = module->TryFindFunction<SynchronizationSystemAPI::ReleaseSemaphoreFunction>("ReleaseSemaphore");
                loaded.createWaitableTimerExWide = module->TryFindFunction<SynchronizationSystemAPI::CreateWaitableTimerExWideFunction>("CreateWaitableTimerExW");
                loaded.setWaitableTimerEx        = module->TryFindFunction<SynchronizationSystemAPI::SetWaitableTimerExFunction>("SetWaitableTimerEx");
                loaded.cancelWaitableTimer       = module->TryFindFunction<SynchronizationSystemAPI::CancelWaitableTimerFunction>("CancelWaitableTimer");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const FileMappingSystemAPI& LoadFileMappingSystemAPI() noexcept {
            static const FileMappingSystemAPI api = [] {
                FileMappingSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                loaded.createFileMappingWide = module->TryFindFunction<FileMappingSystemAPI::CreateFileMappingWideFunction>("CreateFileMappingW");
                loaded.mapViewOfFile         = module->TryFindFunction<FileMappingSystemAPI::MapViewOfFileFunction>("MapViewOfFile");
                loaded.unmapViewOfFile       = module->TryFindFunction<FileMappingSystemAPI::UnmapViewOfFileFunction>("UnmapViewOfFile");
                loaded.flushViewOfFile       = module->TryFindFunction<FileMappingSystemAPI::FlushViewOfFileFunction>("FlushViewOfFile");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                loaded.map       = module->TryFindFunction<FileMappingSystemAPI::MapFunction>("mmap");
                loaded.unmap     = module->TryFindFunction<FileMappingSystemAPI::UnmapFunction>("munmap");
                loaded.syncMap   = module->TryFindFunction<FileMappingSystemAPI::SyncMapFunction>("msync");
                loaded.adviseMap = module->TryFindFunction<FileMappingSystemAPI::AdviseMapFunction>("madvise");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const VirtualMemorySystemAPI& LoadVirtualMemorySystemAPI() noexcept {
            static const VirtualMemorySystemAPI api = [] {
                VirtualMemorySystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                loaded.allocate       = module->TryFindFunction<VirtualMemorySystemAPI::AllocateFunction>("VirtualAlloc");
                loaded.free           = module->TryFindFunction<VirtualMemorySystemAPI::FreeFunction>("VirtualFree");
                loaded.protect        = module->TryFindFunction<VirtualMemorySystemAPI::ProtectFunction>("VirtualProtect");
                loaded.query          = module->TryFindFunction<VirtualMemorySystemAPI::QueryFunction>("VirtualQuery");
                loaded.allocate2      = module->TryFindFunction<VirtualMemorySystemAPI::Allocate2Function>("VirtualAlloc2");
                loaded.mapViewOfFile3 = module->TryFindFunction<VirtualMemorySystemAPI::MapViewOfFile3Function>("MapViewOfFile3");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                loaded.protect = module->TryFindFunction<VirtualMemorySystemAPI::ProtectFunction>("mprotect");
                loaded.remap   = module->TryFindFunction<VirtualMemorySystemAPI::RemapFunction>("mremap");
                loaded.lock    = module->TryFindFunction<VirtualMemorySystemAPI::LockFunction>("mlock");
                loaded.unlock  = module->TryFindFunction<VirtualMemorySystemAPI::UnlockFunction>("munlock");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const EventSystemAPI& LoadEventSystemAPI() noexcept {
            static const EventSystemAPI api = [] {
                EventSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                loaded.createEventWide = module->TryFindFunction<EventSystemAPI::CreateEventWideFunction>("CreateEventW");
                loaded.setEvent        = module->TryFindFunction<EventSystemAPI::SetEventFunction>("SetEvent");
                loaded.resetEvent      = module->TryFindFunction<EventSystemAPI::ResetEventFunction>("ResetEvent");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const IOCPSystemAPI& LoadIOCPSystemAPI() noexcept {
            static const IOCPSystemAPI api = [] {
                IOCPSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                loaded.createIoCompletionPort              = module->TryFindFunction<IOCPSystemAPI::CreateIoCompletionPortFunction>("CreateIoCompletionPort");
                loaded.getQueuedCompletionStatus           = module->TryFindFunction<IOCPSystemAPI::GetQueuedCompletionStatusFunction>("GetQueuedCompletionStatus");
                loaded.getQueuedCompletionStatusEx         = module->TryFindFunction<IOCPSystemAPI::GetQueuedCompletionStatusExFunction>("GetQueuedCompletionStatusEx");
                loaded.postQueuedCompletionStatus          = module->TryFindFunction<IOCPSystemAPI::PostQueuedCompletionStatusFunction>("PostQueuedCompletionStatus");
                loaded.setFileCompletionNotificationModes  = module->TryFindFunction<IOCPSystemAPI::SetFileCompletionNotificationModesFunction>("SetFileCompletionNotificationModes");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

#if defined(PLATFORM_WINDOWS)
        const WindowsIORingSystemAPI& LoadWindowsIORingSystemAPI() noexcept {
            static const WindowsIORingSystemAPI api = [] {
                using API = WindowsIORingSystemAPI;
                API loaded{};
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                loaded.queryCapabilities    = module->TryFindFunction<API::QueryCapabilitiesFunction>("QueryIoRingCapabilities");
                loaded.isOperationSupported = module->TryFindFunction<API::IsOperationSupportedFunction>("IsIoRingOpSupported");
                loaded.createIoRing         = module->TryFindFunction<API::CreateIoRingFunction>("CreateIoRing");
                loaded.getIoRingInfo        = module->TryFindFunction<API::GetIoRingInfoFunction>("GetIoRingInfo");
                loaded.submitIoRing         = module->TryFindFunction<API::SubmitIoRingFunction>("SubmitIoRing");
                loaded.closeIoRing          = module->TryFindFunction<API::CloseIoRingFunction>("CloseIoRing");
                loaded.popIoRingCompletion  = module->TryFindFunction<API::PopIoRingCompletionFunction>("PopIoRingCompletion");
                loaded.setCompletionEvent   = module->TryFindFunction<API::SetCompletionEventFunction>("SetIoRingCompletionEvent");
                loaded.buildCancel          = module->TryFindFunction<API::BuildCancelFunction>("BuildIoRingCancelRequest");
                loaded.buildRead            = module->TryFindFunction<API::BuildReadFunction>("BuildIoRingReadFile");
                loaded.buildWrite           = module->TryFindFunction<API::BuildWriteFunction>("BuildIoRingWriteFile");
                loaded.buildFlush           = module->TryFindFunction<API::BuildFlushFunction>("BuildIoRingFlushFile");
                loaded.buildRegisterFiles   = module->TryFindFunction<API::BuildRegisterFilesFunction>("BuildIoRingRegisterFileHandles");
                loaded.buildRegisterBuffers = module->TryFindFunction<API::BuildRegisterBuffersFunction>("BuildIoRingRegisterBuffers");
                    // clang-format on
                }
                return loaded;
            }();
            return api;
        }
#elif defined(PLATFORM_LINUX)
        const LinuxIoUringSystemAPI& LoadLinuxIoUringSystemAPI() noexcept {
            static const LinuxIoUringSystemAPI api = [] {
                using API = LinuxIoUringSystemAPI;
                API loaded{};
                const ModulePtr module = LoadOptionalModule({"uring", "liburing.so.2", "liburing.so.1"});
                if (module == nullptr) {
                    return loaded;
                }
                // clang-format off
            loaded.queueInit                   = module->TryFindFunction<API::QueueInitFunction>("io_uring_queue_init");
            loaded.queueExit                   = module->TryFindFunction<API::QueueExitFunction>("io_uring_queue_exit");
            loaded.submit                      = module->TryFindFunction<API::SubmitFunction>("io_uring_submit");
            loaded.submitAndWait               = module->TryFindFunction<API::SubmitAndWaitFunction>("io_uring_submit_and_wait");
            gNativeIoUringSubmitAndWaitTimeout = module->TryFindFunction<NativeIoUringSubmitAndWaitTimeoutFunction>("io_uring_submit_and_wait_timeout");
            loaded.submitAndWaitTimeout        = gNativeIoUringSubmitAndWaitTimeout != nullptr ? &IoUringSubmitAndWaitTimeoutAdapter : nullptr;
            loaded.peekBatchCompletion         = module->TryFindFunction<API::PeekBatchCompletionFunction>("io_uring_peek_batch_cqe");
            gNativeIoUringWaitCompletions      = module->TryFindFunction<NativeIoUringWaitCompletionsFunction>("io_uring_wait_cqes");
            loaded.waitCompletions             = gNativeIoUringWaitCompletions != nullptr ? &IoUringWaitCompletionsAdapter : nullptr;
            loaded.registerBuffers             = module->TryFindFunction<API::RegisterBuffersFunction>("io_uring_register_buffers");
            loaded.unregisterBuffers           = module->TryFindFunction<API::UnregisterBuffersFunction>("io_uring_unregister_buffers");
            loaded.registerFiles               = module->TryFindFunction<API::RegisterFilesFunction>("io_uring_register_files");
            loaded.unregisterFiles             = module->TryFindFunction<API::UnregisterFilesFunction>("io_uring_unregister_files");
            loaded.registerEvent               = module->TryFindFunction<API::RegisterEventFunction>("io_uring_register_eventfd");
            loaded.unregisterEvent             = module->TryFindFunction<API::UnregisterEventFunction>("io_uring_unregister_eventfd");
                // clang-format on
                return loaded;
            }();
            return api;
        }
#endif

        const SystemInformationSystemAPI& LoadSystemInformationSystemAPI() noexcept {
            static const SystemInformationSystemAPI api = [] {
                SystemInformationSystemAPI loaded{};
                // clang-format off
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    loaded.getSystemInfo         = module->TryFindFunction<SystemInformationSystemAPI::GetSystemInfoFunction>("GetSystemInfo");
                    loaded.globalMemoryStatus    = module->TryFindFunction<SystemInformationSystemAPI::GlobalMemoryStatusFunction>("GlobalMemoryStatusEx");
                    loaded.isProcessorFeature    = module->TryFindFunction<SystemInformationSystemAPI::IsProcessorFeatureFunction>("IsProcessorFeaturePresent");
                    loaded.getNativeSystemInfo  = module->TryFindFunction<SystemInformationSystemAPI::GetNativeSystemInfoFunction>("GetNativeSystemInfo");
                    loaded.getTickCount64       = module->TryFindFunction<SystemInformationSystemAPI::GetTickCount64Function>("GetTickCount64");
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    loaded.systemConfiguration   = module->TryFindFunction<SystemInformationSystemAPI::SystemConfigurationFunction>("sysconf");
#    if defined(PLATFORM_LINUX)
                    loaded.auxiliaryValue        = module->TryFindFunction<SystemInformationSystemAPI::AuxiliaryValueFunction>("getauxval");
                    loaded.processControl        = module->TryFindFunction<SystemInformationSystemAPI::ProcessControlFunction>("prctl");
                    loaded.fileSystemStatistics  = module->TryFindFunction<SystemInformationSystemAPI::FileSystemStatisticsFunction>("statvfs");
#    elif defined(PLATFORM_MACOS)
                    loaded.sysctlByName          = module->TryFindFunction<SystemInformationSystemAPI::SysctlByNameFunction>("sysctlbyname");
#    endif
                }
#endif
                // clang-format on
                return loaded;
            }();
            return api;
        }

        const DebugSystemAPI& LoadDebugSystemAPI() noexcept {
            static const DebugSystemAPI api = [] {
                using API = DebugSystemAPI;
                API loaded{};
                // clang-format off
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    loaded.checkRemoteDebuggerPresent = module->TryFindFunction<API::CheckRemoteDebuggerPresentFunction>("CheckRemoteDebuggerPresent");
                    loaded.continueDebugEvent         = module->TryFindFunction<API::ContinueDebugEventFunction>("ContinueDebugEvent");
                    loaded.debugActiveProcess         = module->TryFindFunction<API::DebugActiveProcessFunction>("DebugActiveProcess");
                    loaded.debugActiveProcessStop     = module->TryFindFunction<API::DebugActiveProcessFunction>("DebugActiveProcessStop");
                    loaded.isDebuggerPresent          = module->TryFindFunction<API::IsDebuggerPresentFunction>("IsDebuggerPresent");
                    loaded.breakpoint                 = module->TryFindFunction<API::BreakpointFunction>("DebugBreak");
                    loaded.breakpointProcess          = module->TryFindFunction<API::DebugBreakProcessFunction>("DebugBreakProcess");
                    loaded.setProcessKillOnExit       = module->TryFindFunction<API::DebugSetProcessKillOnExitFunction>("DebugSetProcessKillOnExit");
                    loaded.fatalExit                  = module->TryFindFunction<API::FatalExitFunction>("FatalExit");
                    loaded.outputDebugStringAnsi      = module->TryFindFunction<API::OutputDebugStringAnsiFunction>("OutputDebugStringA");
                    loaded.outputDebugStringWide      = module->TryFindFunction<API::OutputDebugStringWideFunction>("OutputDebugStringW");
                    loaded.waitForDebugEvent          = module->TryFindFunction<API::WaitForDebugEventFunction>("WaitForDebugEvent");
                    loaded.waitForDebugEventExtended  = module->TryFindFunction<API::WaitForDebugEventFunction>("WaitForDebugEventEx");
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
#    if defined(PLATFORM_LINUX)
                    loaded.traceProcess               = module->TryFindFunction<API::TraceProcessFunction>("ptrace");
#    endif
                }
#endif
                // clang-format on
                return loaded;
            }();
            return api;
        }

        const COMSystemAPI& LoadCOMSystemAPI() noexcept {
            static const COMSystemAPI api = [] {
                COMSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Ole32Module(); module != nullptr) {
                    // clang-format off
                    loaded.initialize                    = module->TryFindFunction<COMSystemAPI::InitializeFunction>("CoInitialize");
                    loaded.initializeEx                  = module->TryFindFunction<COMSystemAPI::InitializeExFunction>("CoInitializeEx");
                    loaded.initializeSecurity            = module->TryFindFunction<COMSystemAPI::InitializeSecurityFunction>("CoInitializeSecurity");
                    loaded.uninitialize                  = module->TryFindFunction<COMSystemAPI::UninitializeFunction>("CoUninitialize");
                    loaded.getApartmentType              = module->TryFindFunction<COMSystemAPI::GetApartmentTypeFunction>("CoGetApartmentType");
                    loaded.getCallerThreadId             = module->TryFindFunction<COMSystemAPI::GetCallerThreadIdFunction>("CoGetCallerTID");
                    loaded.getContextToken               = module->TryFindFunction<COMSystemAPI::GetContextTokenFunction>("CoGetContextToken");
                    loaded.getObjectContext              = module->TryFindFunction<COMSystemAPI::GetObjectContextFunction>("CoGetObjectContext");
                    loaded.getClassObject                = module->TryFindFunction<COMSystemAPI::GetClassObjectFunction>("CoGetClassObject");
                    loaded.createInstance                = module->TryFindFunction<COMSystemAPI::CreateInstanceFunction>("CoCreateInstance");
                    loaded.createInstanceEx              = module->TryFindFunction<COMSystemAPI::CreateInstanceExFunction>("CoCreateInstanceEx");
                    loaded.createInstanceFromApp         = module->TryFindFunction<COMSystemAPI::CreateInstanceFromAppFunction>("CoCreateInstanceFromApp");
                    loaded.registerClassObject           = module->TryFindFunction<COMSystemAPI::RegisterClassObjectFunction>("CoRegisterClassObject");
                    loaded.revokeClassObject             = module->TryFindFunction<COMSystemAPI::RevokeClassObjectFunction>("CoRevokeClassObject");
                    loaded.resumeClassObjects            = module->TryFindFunction<COMSystemAPI::ResumeSuspendClassObjectsFunction>("CoResumeClassObjects");
                    loaded.suspendClassObjects           = module->TryFindFunction<COMSystemAPI::ResumeSuspendClassObjectsFunction>("CoSuspendClassObjects");
                    loaded.getMalloc                     = module->TryFindFunction<COMSystemAPI::GetMallocFunction>("CoGetMalloc");
                    loaded.createStandardMalloc          = module->TryFindFunction<COMSystemAPI::CreateStandardMallocFunction>("CoCreateStandardMalloc");
                    loaded.taskMemAlloc                  = module->TryFindFunction<COMSystemAPI::TaskMemAllocFunction>("CoTaskMemAlloc");
                    loaded.taskMemRealloc                = module->TryFindFunction<COMSystemAPI::TaskMemReallocFunction>("CoTaskMemRealloc");
                    loaded.taskMemFree                   = module->TryFindFunction<COMSystemAPI::TaskMemFreeFunction>("CoTaskMemFree");
                    loaded.createStreamOnGlobal          = module->TryFindFunction<COMSystemAPI::CreateStreamOnGlobalFunction>("CreateStreamOnHGlobal");
                    loaded.getGlobalFromStream           = module->TryFindFunction<COMSystemAPI::GetGlobalFromStreamFunction>("GetHGlobalFromStream");
                    loaded.getMarshalSizeMax             = module->TryFindFunction<COMSystemAPI::MarshalSizeMaxFunction>("CoGetMarshalSizeMax");
                    loaded.marshalInterface              = module->TryFindFunction<COMSystemAPI::MarshalInterfaceFunction>("CoMarshalInterface");
                    loaded.unmarshalInterface            = module->TryFindFunction<COMSystemAPI::UnmarshalInterfaceFunction>("CoUnmarshalInterface");
                    loaded.releaseMarshalData            = module->TryFindFunction<COMSystemAPI::ReleaseMarshalDataFunction>("CoReleaseMarshalData");
                    loaded.marshalInterThreadInterface   = module->TryFindFunction<COMSystemAPI::MarshalInterThreadInterfaceFunction>("CoMarshalInterThreadInterfaceInStream");
                    loaded.getInterfaceAndReleaseStream  = module->TryFindFunction<COMSystemAPI::GetInterfaceAndReleaseStreamFunction>("CoGetInterfaceAndReleaseStream");
                    loaded.createFreeThreadedMarshaler   = module->TryFindFunction<COMSystemAPI::CreateFreeThreadedMarshalerFunction>("CoCreateFreeThreadedMarshaler");
                    loaded.disconnectObject              = module->TryFindFunction<COMSystemAPI::DisconnectObjectFunction>("CoDisconnectObject");
                    loaded.lockObjectExternal            = module->TryFindFunction<COMSystemAPI::LockObjectExternalFunction>("CoLockObjectExternal");
                    loaded.queryProxyBlanket             = module->TryFindFunction<COMSystemAPI::QueryProxyBlanketFunction>("CoQueryProxyBlanket");
                    loaded.setProxyBlanket               = module->TryFindFunction<COMSystemAPI::SetProxyBlanketFunction>("CoSetProxyBlanket");
                    loaded.cancelCall                    = module->TryFindFunction<COMSystemAPI::CancelCallFunction>("CoCancelCall");
                    loaded.testCancel                    = module->TryFindFunction<COMSystemAPI::TestCancelFunction>("CoTestCancel");
                    loaded.enableCallCancellation        = module->TryFindFunction<COMSystemAPI::EnableDisableCallCancellationFunction>("CoEnableCallCancellation");
                    loaded.disableCallCancellation       = module->TryFindFunction<COMSystemAPI::EnableDisableCallCancellationFunction>("CoDisableCallCancellation");
                    loaded.createGuid                    = module->TryFindFunction<COMSystemAPI::CreateGuidFunction>("CoCreateGuid");
                    loaded.stringFromClsid               = module->TryFindFunction<COMSystemAPI::StringFromGuidFunction>("StringFromCLSID");
                    loaded.clsidFromString               = module->TryFindFunction<COMSystemAPI::GuidFromStringFunction>("CLSIDFromString");
                    loaded.stringFromIid                 = module->TryFindFunction<COMSystemAPI::StringFromGuidFunction>("StringFromIID");
                    loaded.iidFromString                 = module->TryFindFunction<COMSystemAPI::GuidFromStringFunction>("IIDFromString");
                    loaded.stringFromGuid2               = module->TryFindFunction<COMSystemAPI::StringFromGuid2Function>("StringFromGUID2");
                    loaded.progIdFromClsid               = module->TryFindFunction<COMSystemAPI::ProgIdFromClsidFunction>("ProgIDFromCLSID");
                    loaded.clsidFromProgId               = module->TryFindFunction<COMSystemAPI::ClsidFromProgIdFunction>("CLSIDFromProgID");
                    loaded.waitForMultipleHandles        = module->TryFindFunction<COMSystemAPI::WaitForMultipleHandlesFunction>("CoWaitForMultipleHandles");
                    loaded.freeUnusedLibraries           = module->TryFindFunction<COMSystemAPI::FreeUnusedLibrariesFunction>("CoFreeUnusedLibraries");
                    loaded.freeUnusedLibrariesEx         = module->TryFindFunction<COMSystemAPI::FreeUnusedLibrariesExFunction>("CoFreeUnusedLibrariesEx");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const OleSystemAPI& LoadOleSystemAPI() noexcept {
            static const OleSystemAPI api = [] {
                OleSystemAPI loaded{};
                // clang-format off
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Ole32Module(); module != nullptr) {
                    loaded.initialize                      = module->TryFindFunction<OleSystemAPI::InitializeFunction>("OleInitialize");
                    loaded.uninitialize                    = module->TryFindFunction<OleSystemAPI::UninitializeFunction>("OleUninitialize");
                    loaded.buildVersion                    = module->TryFindFunction<OleSystemAPI::BuildVersionFunction>("OleBuildVersion");
                    loaded.queryLinkFromData               = module->TryFindFunction<OleSystemAPI::QueryDataFunction>("OleQueryLinkFromData");
                    loaded.queryCreateFromData             = module->TryFindFunction<OleSystemAPI::QueryDataFunction>("OleQueryCreateFromData");
                    loaded.create                          = module->TryFindFunction<OleSystemAPI::CreateObjectFunction>("OleCreate");
                    loaded.createEx                        = module->TryFindFunction<OleSystemAPI::CreateObjectExFunction>("OleCreateEx");
                    loaded.createFromData                  = module->TryFindFunction<OleSystemAPI::CreateFromDataFunction>("OleCreateFromData");
                    loaded.createFromDataEx                = module->TryFindFunction<OleSystemAPI::CreateFromDataExFunction>("OleCreateFromDataEx");
                    loaded.createLink                      = module->TryFindFunction<OleSystemAPI::CreateLinkFunction>("OleCreateLink");
                    loaded.createLinkEx                    = module->TryFindFunction<OleSystemAPI::CreateLinkExFunction>("OleCreateLinkEx");
                    loaded.createLinkToFile                = module->TryFindFunction<OleSystemAPI::CreateLinkToFileFunction>("OleCreateLinkToFile");
                    loaded.createLinkToFileEx              = module->TryFindFunction<OleSystemAPI::CreateLinkToFileExFunction>("OleCreateLinkToFileEx");
                    loaded.createFromFile                  = module->TryFindFunction<OleSystemAPI::CreateFromFileFunction>("OleCreateFromFile");
                    loaded.createFromFileEx                = module->TryFindFunction<OleSystemAPI::CreateFromFileExFunction>("OleCreateFromFileEx");
                    loaded.load                            = module->TryFindFunction<OleSystemAPI::LoadFunction>("OleLoad");
                    loaded.save                            = module->TryFindFunction<OleSystemAPI::SaveFunction>("OleSave");
                    loaded.loadFromStream                  = module->TryFindFunction<OleSystemAPI::LoadFromStreamFunction>("OleLoadFromStream");
                    loaded.saveToStream                    = module->TryFindFunction<OleSystemAPI::SaveToStreamFunction>("OleSaveToStream");
                    loaded.setContainedObject              = module->TryFindFunction<OleSystemAPI::SetObjectStateFunction>("OleSetContainedObject");
                    loaded.noteObjectVisible               = module->TryFindFunction<OleSystemAPI::SetObjectStateFunction>("OleNoteObjectVisible");
                    loaded.registerDragDrop                = module->TryFindFunction<OleSystemAPI::RegisterDragDropFunction>("RegisterDragDrop");
                    loaded.revokeDragDrop                  = module->TryFindFunction<OleSystemAPI::RevokeDragDropFunction>("RevokeDragDrop");
                    loaded.doDragDrop                      = module->TryFindFunction<OleSystemAPI::DoDragDropFunction>("DoDragDrop");
                    loaded.setClipboard                    = module->TryFindFunction<OleSystemAPI::SetClipboardFunction>("OleSetClipboard");
                    loaded.getClipboard                    = module->TryFindFunction<OleSystemAPI::GetClipboardFunction>("OleGetClipboard");
                    loaded.flushClipboard                  = module->TryFindFunction<OleSystemAPI::FlushClipboardFunction>("OleFlushClipboard");
                    loaded.isCurrentClipboard              = module->TryFindFunction<OleSystemAPI::IsCurrentClipboardFunction>("OleIsCurrentClipboard");
                    loaded.draw                            = module->TryFindFunction<OleSystemAPI::DrawFunction>("OleDraw");
                    loaded.run                             = module->TryFindFunction<OleSystemAPI::QueryDataFunction>("OleRun");
                    loaded.isRunning                       = module->TryFindFunction<OleSystemAPI::IsRunningFunction>("OleIsRunning");
                    loaded.lockRunning                     = module->TryFindFunction<OleSystemAPI::LockRunningFunction>("OleLockRunning");
                    loaded.releaseStorageMedium            = module->TryFindFunction<OleSystemAPI::ReleaseStorageMediumFunction>("ReleaseStgMedium");
                    loaded.createOleAdviseHolder           = module->TryFindFunction<OleSystemAPI::CreateAdviseHolderFunction>("CreateOleAdviseHolder");
                    loaded.createDataAdviseHolder          = module->TryFindFunction<OleSystemAPI::CreateAdviseHolderFunction>("CreateDataAdviseHolder");
                    loaded.createDataCache                 = module->TryFindFunction<OleSystemAPI::CreateDataCacheFunction>("CreateDataCache");
                }
#endif
                // clang-format on
                return loaded;
            }();
            return api;
        }

        const OleAutomationSystemAPI& LoadOleAutomationSystemAPI() noexcept {
            static const OleAutomationSystemAPI api = [] {
                OleAutomationSystemAPI loaded{};
                // clang-format off
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = OleAut32Module(); module != nullptr) {
                    loaded.allocString                      = module->TryFindFunction<OleAutomationSystemAPI::AllocStringFunction>("SysAllocString");
                    loaded.allocStringLen                   = module->TryFindFunction<OleAutomationSystemAPI::AllocStringLengthFunction>("SysAllocStringLen");
                    loaded.reallocStringLen                 = module->TryFindFunction<OleAutomationSystemAPI::ReallocStringLengthFunction>("SysReAllocStringLen");
                    loaded.freeString                       = module->TryFindFunction<OleAutomationSystemAPI::FreeStringFunction>("SysFreeString");
                    loaded.stringLength                     = module->TryFindFunction<OleAutomationSystemAPI::StringLengthFunction>("SysStringLen");
                    loaded.stringByteLength                 = module->TryFindFunction<OleAutomationSystemAPI::StringLengthFunction>("SysStringByteLen");
                    loaded.safeArrayCreate                  = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayCreateFunction>("SafeArrayCreate");
                    loaded.safeArrayCreateVector            = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayCreateVectorFunction>("SafeArrayCreateVector");
                    loaded.safeArrayDestroy                 = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayDestroyFunction>("SafeArrayDestroy");
                    loaded.safeArrayCopy                    = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayCopyFunction>("SafeArrayCopy");
                    loaded.safeArrayAccessData              = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayAccessDataFunction>("SafeArrayAccessData");
                    loaded.safeArrayUnaccessData            = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayUnaccessDataFunction>("SafeArrayUnaccessData");
                    loaded.safeArrayGetElement              = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayGetElementFunction>("SafeArrayGetElement");
                    loaded.safeArrayPutElement              = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayPutElementFunction>("SafeArrayPutElement");
                    loaded.safeArrayGetLowerBound           = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayGetBoundFunction>("SafeArrayGetLBound");
                    loaded.safeArrayGetUpperBound           = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayGetBoundFunction>("SafeArrayGetUBound");
                    loaded.safeArrayLock                    = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayLockFunction>("SafeArrayLock");
                    loaded.safeArrayUnlock                  = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayUnlockFunction>("SafeArrayUnlock");
                    loaded.safeArrayGetDimension            = module->TryFindFunction<OleAutomationSystemAPI::SafeArrayGetDimensionFunction>("SafeArrayGetDim");
                    loaded.variantInit                      = module->TryFindFunction<OleAutomationSystemAPI::VariantInitFunction>("VariantInit");
                    loaded.variantClear                     = module->TryFindFunction<OleAutomationSystemAPI::VariantOperationFunction>("VariantClear");
                    loaded.variantCopy                      = module->TryFindFunction<OleAutomationSystemAPI::VariantCopyFunction>("VariantCopy");
                    loaded.variantCopyInd                   = module->TryFindFunction<OleAutomationSystemAPI::VariantCopyIndFunction>("VariantCopyInd");
                    loaded.variantChangeType                = module->TryFindFunction<OleAutomationSystemAPI::VariantChangeTypeFunction>("VariantChangeType");
                    loaded.variantChangeTypeEx              = module->TryFindFunction<OleAutomationSystemAPI::VariantChangeTypeExFunction>("VariantChangeTypeEx");
                    loaded.registerActiveObject             = module->TryFindFunction<OleAutomationSystemAPI::RegisterActiveObjectFunction>("RegisterActiveObject");
                    loaded.revokeActiveObject               = module->TryFindFunction<OleAutomationSystemAPI::RevokeActiveObjectFunction>("RevokeActiveObject");
                    loaded.getActiveObject                  = module->TryFindFunction<OleAutomationSystemAPI::GetActiveObjectFunction>("GetActiveObject");
                    loaded.loadTypeLibrary                  = module->TryFindFunction<OleAutomationSystemAPI::LoadTypeLibraryFunction>("LoadTypeLib");
                    loaded.loadTypeLibraryEx                = module->TryFindFunction<OleAutomationSystemAPI::LoadTypeLibraryExFunction>("LoadTypeLibEx");
                    loaded.loadRegisteredTypeLibrary        = module->TryFindFunction<OleAutomationSystemAPI::LoadRegisteredTypeLibraryFunction>("LoadRegTypeLib");
                    loaded.queryPathOfRegisteredTypeLibrary = module->TryFindFunction<OleAutomationSystemAPI::QueryTypeLibraryPathFunction>("QueryPathOfRegTypeLib");
                }
#endif
                // clang-format on
                return loaded;
            }();
            return api;
        }

        const StructuredStorageSystemAPI& LoadStructuredStorageSystemAPI() noexcept {
            static const StructuredStorageSystemAPI api = [] {
                StructuredStorageSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Ole32Module(); module != nullptr) {
                    loaded.createDocfile = module->TryFindFunction<StructuredStorageSystemAPI::CreateDocfileFunction>(
                        "StgCreateDocfile");
                    loaded.createDocfileOnLockBytes
                        = module->TryFindFunction<StructuredStorageSystemAPI::CreateDocfileOnLockBytesFunction>(
                            "StgCreateDocfileOnILockBytes");
                    loaded.openStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::OpenStorageFunction>("StgOpenStorage");
                    loaded.openStorageOnLockBytes
                        = module->TryFindFunction<StructuredStorageSystemAPI::OpenStorageOnLockBytesFunction>(
                            "StgOpenStorageOnILockBytes");
                    loaded.isStorageFile = module->TryFindFunction<StructuredStorageSystemAPI::IsStorageFileFunction>(
                        "StgIsStorageFile");
                    loaded.isStorageLockBytes
                        = module->TryFindFunction<StructuredStorageSystemAPI::IsStorageLockBytesFunction>(
                            "StgIsStorageILockBytes");
                    loaded.setStorageTimes
                        = module->TryFindFunction<StructuredStorageSystemAPI::SetStorageTimesFunction>("StgSetTimes");
                    loaded.createStorageEx
                        = module->TryFindFunction<StructuredStorageSystemAPI::CreateStorageExFunction>(
                            "StgCreateStorageEx");
                    loaded.openStorageEx = module->TryFindFunction<StructuredStorageSystemAPI::OpenStorageExFunction>(
                        "StgOpenStorageEx");
                    loaded.createPropertyStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::CreatePropertyStorageFunction>(
                            "StgCreatePropStg");
                    loaded.openPropertyStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::OpenPropertyStorageFunction>(
                            "StgOpenPropStg");
                    loaded.createPropertySetStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::CreatePropertySetStorageFunction>(
                            "StgCreatePropSetStg");
                    loaded.formatIdToPropertyStorageName
                        = module->TryFindFunction<StructuredStorageSystemAPI::FormatIdToPropertyStorageNameFunction>(
                            "FmtIdToPropStgName");
                    loaded.propertyStorageNameToFormatId
                        = module->TryFindFunction<StructuredStorageSystemAPI::PropertyStorageNameToFormatIdFunction>(
                            "PropStgNameToFmtId");
                    loaded.readClassStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::ReadClassStorageFunction>("ReadClassStg");
                    loaded.writeClassStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::WriteClassStorageFunction>(
                            "WriteClassStg");
                    loaded.readClassStream
                        = module->TryFindFunction<StructuredStorageSystemAPI::ReadClassStreamFunction>("ReadClassStm");
                    loaded.writeClassStream
                        = module->TryFindFunction<StructuredStorageSystemAPI::WriteClassStreamFunction>(
                            "WriteClassStm");
                    loaded.getGlobalFromLockBytes
                        = module->TryFindFunction<StructuredStorageSystemAPI::GetGlobalFromLockBytesFunction>(
                            "GetHGlobalFromILockBytes");
                    loaded.createLockBytesOnGlobal
                        = module->TryFindFunction<StructuredStorageSystemAPI::CreateLockBytesOnGlobalFunction>(
                            "CreateILockBytesOnHGlobal");
                    loaded.getConvertStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::GetConvertStorageFunction>(
                            "GetConvertStg");
                    loaded.setConvertStorage
                        = module->TryFindFunction<StructuredStorageSystemAPI::SetConvertStorageFunction>(
                            "SetConvertStg");
                    loaded.writeFormatUserType
                        = module->TryFindFunction<StructuredStorageSystemAPI::WriteFormatUserTypeFunction>(
                            "WriteFmtUserTypeStg");
                    loaded.readFormatUserType
                        = module->TryFindFunction<StructuredStorageSystemAPI::ReadFormatUserTypeFunction>(
                            "ReadFmtUserTypeStg");
                    loaded.openAsyncDocfile
                        = module->TryFindFunction<StructuredStorageSystemAPI::OpenAsyncDocfileFunction>(
                            "StgOpenAsyncDocfileOnIFillLockBytes");
                    loaded.getFillLockBytesOnLockBytes
                        = module->TryFindFunction<StructuredStorageSystemAPI::GetFillLockBytesOnLockBytesFunction>(
                            "StgGetIFillLockBytesOnILockBytes");
                    loaded.getFillLockBytesOnFile
                        = module->TryFindFunction<StructuredStorageSystemAPI::GetFillLockBytesOnFileFunction>(
                            "StgGetIFillLockBytesOnFile");
                    loaded.openLayoutDocfile
                        = module->TryFindFunction<StructuredStorageSystemAPI::OpenLayoutDocfileFunction>(
                            "StgOpenLayoutDocfile");
                    loaded.copyPropertyVariant
                        = module->TryFindFunction<StructuredStorageSystemAPI::PropertyVariantCopyFunction>(
                            "PropVariantCopy");
                    loaded.clearPropertyVariant
                        = module->TryFindFunction<StructuredStorageSystemAPI::PropertyVariantClearFunction>(
                            "PropVariantClear");
                    loaded.freePropertyVariantArray
                        = module->TryFindFunction<StructuredStorageSystemAPI::FreePropertyVariantArrayFunction>(
                            "FreePropVariantArray");
                }
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
                const ModulePtr& module = Kernel32Module();
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
            loaded.getEnvironmentVariableWide  = module->TryFindFunction<API::GetEnvironmentVariableWideFunction>("GetEnvironmentVariableW");
            loaded.setEnvironmentVariableWide  = module->TryFindFunction<API::SetEnvironmentVariableWideFunction>("SetEnvironmentVariableW");
            loaded.getEnvironmentStringsWide   = module->TryFindFunction<API::GetEnvironmentStringsWideFunction>("GetEnvironmentStringsW");
            loaded.freeEnvironmentStringsWide  = module->TryFindFunction<API::FreeEnvironmentStringsWideFunction>("FreeEnvironmentStringsW");
                // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                // clang-format off
            loaded.get            = module->TryFindFunction<API::GetFunction>("getenv");
            loaded.set            = module->TryFindFunction<API::SetFunction>("setenv");
            loaded.remove         = module->TryFindFunction<API::RemoveFunction>("unsetenv");
            loaded.environment    = module->TryFindSymbol<char**>("environ");
            loaded.getEnvironment = module->TryFindFunction<API::GetEnvironmentFunction>("_NSGetEnviron");
                // clang-format on
#endif
                return loaded;
            }();
            return api;
        }

        const ModuleSystemAPI& LoadModuleSystemAPI() noexcept {
            static const ModuleSystemAPI api = [] {
                ModuleSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                // clang-format off
            loaded.loadLibraryWide     = LoadWindowsLibrary;
            loaded.getModuleHandleWide = GetWindowsModuleHandle;
            loaded.findSymbol          = FindWindowsSymbol;
            loaded.freeLibrary         = FreeWindowsLibrary;
                // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                // clang-format off
            loaded.open         = OpenPosixModule;
            loaded.close        = ClosePosixModule;
            loaded.findSymbol   = FindPosixSymbol;
            loaded.queryAddress = QueryPosixAddress;
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
                const ModulePtr& module = Kernel32Module();
                if (module != nullptr) {
                    // clang-format off
                loaded.getCurrentProcessId           = module->TryFindFunction<API::GetCurrentProcessIdFunction>("GetCurrentProcessId");
                loaded.getCurrentProcess             = module->TryFindFunction<API::GetCurrentProcessFunction>("GetCurrentProcess");
                loaded.createProcessSnapshot         = module->TryFindFunction<API::CreateProcessSnapshotFunction>("CreateToolhelp32Snapshot");
                loaded.firstProcess                  = module->TryFindFunction<API::ReadProcessEntryFunction>("Process32FirstW");
                loaded.nextProcess                   = module->TryFindFunction<API::ReadProcessEntryFunction>("Process32NextW");
                loaded.firstThread                   = module->TryFindFunction<API::ReadThreadEntryFunction>("Thread32First");
                loaded.nextThread                    = module->TryFindFunction<API::ReadThreadEntryFunction>("Thread32Next");
                loaded.queryFullProcessImageNameWide = module->TryFindFunction<API::QueryFullProcessImageNameWideFunction>("QueryFullProcessImageNameW");
                loaded.getCommandLineWide            = module->TryFindFunction<API::GetCommandLineWideFunction>("GetCommandLineW");
                loaded.getProcessTimes               = module->TryFindFunction<API::GetProcessTimesFunction>("GetProcessTimes");
                loaded.getProcessMemoryInfo          = module->TryFindFunction<API::GetProcessMemoryInfoFunction>("K32GetProcessMemoryInfo");
                loaded.getProcessId                  = module->TryFindFunction<API::GetProcessIdFunction>("GetProcessId");
                loaded.getProcessHandleCount         = module->TryFindFunction<API::GetProcessHandleCountFunction>("GetProcessHandleCount");
                loaded.queryProcessCycleTime        = module->TryFindFunction<API::QueryProcessCycleTimeFunction>("QueryProcessCycleTime");
                    // clang-format on
                }
                if (EnsureSystemAPIs(loaded.getCurrentProcessId, loaded.createProcessSnapshot, loaded.firstProcess,
                                     loaded.nextProcess, Sora::PAL::API::LoadHandleSystemAPI().closeHandle)) {
                    loaded.queryParentProcessId = QueryWindowsParentProcessId;
                }
                if (EnsureSystemAPIs(loaded.getCurrentProcess, loaded.getProcessTimes, loaded.getProcessMemoryInfo)) {
                    loaded.captureUsage = CaptureWindowsProcessUsage;
                }

                const ModulePtr& shell = Shell32Module();
                if (shell != nullptr) {
                    loaded.commandLineToArgvWide
                        = shell->TryFindFunction<ProcessSystemAPI::CommandLineToArgvWideFunction>("CommandLineToArgvW");
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                const ModulePtr& module = CurrentProcessModule();
                if (module != nullptr) {
                    // clang-format off
                loaded.getProcessId          = module->TryFindFunction<ProcessSystemAPI::GetProcessIdFunction>("getpid");
                loaded.getParentProcessId    = module->TryFindFunction<ProcessSystemAPI::GetParentProcessIdFunction>("getppid");
                loaded.getResourceUsage      = module->TryFindFunction<ProcessSystemAPI::GetResourceUsageFunction>("getrusage");
#    if defined(PLATFORM_MACOS)
                loaded.getExecutablePath     = module->TryFindFunction<ProcessSystemAPI::GetExecutablePathFunction>("_NSGetExecutablePath");
                loaded.getArgumentCount      = module->TryFindFunction<ProcessSystemAPI::GetArgumentCountFunction>("_NSGetArgc");
                loaded.getArgumentVector     = module->TryFindFunction<ProcessSystemAPI::GetArgumentVectorFunction>("_NSGetArgv");
                loaded.processResourceUsage  = module->TryFindFunction<ProcessSystemAPI::ProcessResourceUsageFunction>("proc_pid_rusage");
#    endif
                    // clang-format on
                }
#    if defined(PLATFORM_LINUX)
                const auto& systemAPI = LoadSystemInformationSystemAPI();
                if (EnsureSystemAPIs(loaded.getResourceUsage, systemAPI.systemConfiguration)) {
                    loaded.captureUsage = CapturePosixProcessUsage;
                }
#    else
                if (EnsureSystemAPIs(loaded.getResourceUsage, loaded.getProcessId, loaded.processResourceUsage)) {
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
                const ModulePtr& module = Kernel32Module();
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
            loaded.getCurrentThreadId          = module->TryFindFunction<API::GetCurrentThreadIdFunction>("GetCurrentThreadId");
            loaded.getCurrentThread            = module->TryFindFunction<API::GetCurrentThreadFunction>("GetCurrentThread");
            loaded.getThreadId                 = module->TryFindFunction<API::GetThreadIdFunction>("GetThreadId");
            loaded.queryThreadCycleTime        = module->TryFindFunction<API::QueryThreadCycleTimeFunction>("QueryThreadCycleTime");
            loaded.openThread                  = module->TryFindFunction<API::OpenThreadFunction>("OpenThread");
            loaded.setThreadDescription        = module->TryFindFunction<API::SetThreadDescriptionFunction>("SetThreadDescription");
            loaded.getThreadDescription        = module->TryFindFunction<API::GetThreadDescriptionFunction>("GetThreadDescription");
            loaded.getCurrentProcessorNumberEx = module->TryFindFunction<API::GetCurrentProcessorNumberExFunction>("GetCurrentProcessorNumberEx");
            loaded.getCurrentThreadStackLimits = module->TryFindFunction<API::GetCurrentThreadStackLimitsFunction>("GetCurrentThreadStackLimits");
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            loaded.pthreadSelf                 = module->TryFindFunction<API::PthreadSelfFunction>("pthread_self");
            loaded.pthreadGetName              = module->TryFindFunction<API::PthreadGetNameFunction>("pthread_getname_np");
#    if defined(PLATFORM_LINUX)
            loaded.getThreadId                 = module->TryFindFunction<API::GetThreadIdFunction>("gettid");
            loaded.scheduleGetCpu              = module->TryFindFunction<API::ScheduleGetCpuFunction>("sched_getcpu");
            loaded.pthreadSetName              = module->TryFindFunction<API::PthreadSetNameFunction>("pthread_setname_np");
            loaded.pthreadGetAttributes        = module->TryFindFunction<API::PthreadGetAttributesFunction>("pthread_getattr_np");
            loaded.pthreadDestroyAttributes    = module->TryFindFunction<API::PthreadDestroyAttributesFunction>("pthread_attr_destroy");
            loaded.pthreadGetStack             = module->TryFindFunction<API::PthreadGetStackFunction>("pthread_attr_getstack");
#    else
            loaded.pthreadThreadId             = module->TryFindFunction<API::PthreadThreadIdFunction>("pthread_threadid_np");
            loaded.pthreadSetName              = module->TryFindFunction<API::PthreadSetNameFunction>("pthread_setname_np");
            loaded.pthreadGetStackAddress      = module->TryFindFunction<API::PthreadGetStackAddressFunction>("pthread_get_stackaddr_np");
            loaded.pthreadGetStackSize         = module->TryFindFunction<API::PthreadGetStackSizeFunction>("pthread_get_stacksize_np");
#    endif
#endif
                // clang-format on
                return loaded;
            }();
            return api;
        }

        const ProcessControlSystemAPI& LoadProcessControlSystemAPI() noexcept {
            static const ProcessControlSystemAPI api = [] {
                using API = ProcessControlSystemAPI;
                API loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                    loaded.openProcess            = module->TryFindFunction<API::OpenProcessFunction>("OpenProcess");
                    loaded.createProcessWide      = module->TryFindFunction<API::CreateProcessWideFunction>("CreateProcessW");
                    loaded.getExitCode            = module->TryFindFunction<API::GetExitCodeFunction>("GetExitCodeProcess");
                    loaded.terminate              = module->TryFindFunction<API::TerminateFunction>("TerminateProcess");
                    loaded.createJobObjectWide    = module->TryFindFunction<API::CreateJobObjectWideFunction>("CreateJobObjectW");
                    loaded.assignProcessToJob     = module->TryFindFunction<API::AssignJobFunction>("AssignProcessToJobObject");
                    loaded.setPriorityClass       = module->TryFindFunction<API::SetPriorityClassFunction>("SetPriorityClass");
                    loaded.setProcessAffinityMask = module->TryFindFunction<API::SetAffinityMaskFunction>("SetProcessAffinityMask");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.fork         = module->TryFindFunction<API::ForkFunction>("fork");
                    loaded.exec         = module->TryFindFunction<API::ExecFunction>("execve");
                    loaded.execPath     = module->TryFindFunction<API::ExecPathFunction>("execv");
                    gNativeSpawn        = module->TryFindFunction<NativeSpawnFunction>("posix_spawn");
                    loaded.spawn        = gNativeSpawn != nullptr ? &SpawnAdapter : nullptr;
                    loaded.wait         = module->TryFindFunction<API::WaitFunction>("waitpid");
                    gNativeWaitId       = module->TryFindFunction<NativeWaitIdFunction>("waitid");
                    loaded.waitId       = gNativeWaitId != nullptr ? &WaitIdAdapter : nullptr;
                    loaded.kill         = module->TryFindFunction<API::SignalFunction>("kill");
                    loaded.raiseSignal  = module->TryFindFunction<API::RaiseSignalFunction>("raise");
                    gNativeSignalAction = module->TryFindFunction<NativeSignalActionFunction>("sigaction");
                    loaded.signalAction = gNativeSignalAction != nullptr ? &SignalActionAdapter : nullptr;
                    // clang-format on
                }
#    if defined(PLATFORM_LINUX)
#        if defined(SYS_tgkill)
                if (LoadLinuxSyscallSystemAPI().syscall != nullptr) {
                    loaded.threadKill = &ThreadKillViaSyscall;
                }
#        endif
#        if defined(SYS_pidfd_open)
                if (LoadLinuxSyscallSystemAPI().syscall != nullptr) {
                    loaded.processDescriptorOpen = &ProcessDescriptorOpenViaSyscall;
                }
#        endif
#        if defined(SYS_pidfd_send_signal)
                if (LoadLinuxSyscallSystemAPI().syscall != nullptr) {
                    loaded.processDescriptorSendSignal = &ProcessDescriptorSendSignalViaSyscall;
                }
#        endif
#    endif
#endif

                return loaded;
            }();
            return api;
        }

        const ThreadControlSystemAPI& LoadThreadControlSystemAPI() noexcept {
            static const ThreadControlSystemAPI api = [] {
                using API = ThreadControlSystemAPI;
                API loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                    loaded.setPriority         = module->TryFindFunction<API::SetPriorityFunction>("SetThreadPriority");
                    loaded.getPriority         = module->TryFindFunction<API::GetPriorityFunction>("GetThreadPriority");
                    loaded.setAffinityMask     = module->TryFindFunction<API::SetAffinityMaskFunction>("SetThreadAffinityMask");
                    loaded.setIdealProcessor   = module->TryFindFunction<API::SetIdealProcessorFunction>("SetThreadIdealProcessor");
                    loaded.setIdealProcessorEx = module->TryFindFunction<API::SetIdealProcessorExFunction>("SetThreadIdealProcessorEx");
                    loaded.suspend             = module->TryFindFunction<API::SuspendFunction>("SuspendThread");
                    loaded.resume              = module->TryFindFunction<API::ResumeFunction>("ResumeThread");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const RandomSystemAPI& LoadRandomSystemAPI() noexcept {
            static const RandomSystemAPI api = [] {
                RandomSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = BCryptModule(); module != nullptr) {
                    loaded.generate = module->TryFindFunction<RandomSystemAPI::GenerateFunction>("BCryptGenRandom");
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    loaded.getRandom = module->TryFindFunction<RandomSystemAPI::GetRandomFunction>("getrandom");
                }
#endif
                return loaded;
            }();
            return api;
        }

        const SocketSystemAPI& LoadSocketSystemAPI() noexcept {
            static const SocketSystemAPI api = [] {
                SocketSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                const ModulePtr& module = Ws2_32Module();
                if (module != nullptr) {
                    using API = SocketSystemAPI;
                    // clang-format off
                    loaded.startup         = module->TryFindFunction<API::StartupFunction>("WSAStartup");
                    loaded.cleanup         = module->TryFindFunction<API::CleanupFunction>("WSACleanup");
                    loaded.getLastError    = module->TryFindFunction<API::GetLastErrorFunction>("WSAGetLastError");
                    loaded.setLastError    = module->TryFindFunction<API::SetLastErrorFunction>("WSASetLastError");
                    loaded.create          = module->TryFindFunction<API::SocketFunction>("socket");
                    loaded.close           = module->TryFindFunction<API::CloseFunction>("closesocket");
                    loaded.bind            = module->TryFindFunction<API::BindFunction>("bind");
                    loaded.getSocketName   = module->TryFindFunction<API::NameFunction>("getsockname");
                    loaded.getPeerName     = module->TryFindFunction<API::NameFunction>("getpeername");
                    loaded.setOption       = module->TryFindFunction<API::OptionFunction>("setsockopt");
                    loaded.getOption       = module->TryFindFunction<API::GetOptionFunction>("getsockopt");
                    loaded.ioctl           = module->TryFindFunction<API::IoctlFunction>("ioctlsocket");
                    loaded.poll            = module->TryFindFunction<API::PollFunction>("WSAPoll");
                    loaded.getAddressInfo  = module->TryFindFunction<API::GetAddressInfoFunction>("getaddrinfo");
                    loaded.freeAddressInfo = module->TryFindFunction<API::FreeAddressInfoFunction>("freeaddrinfo");
                    loaded.getNameInfo     = module->TryFindFunction<API::GetNameInfoFunction>("getnameinfo");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                const ModulePtr& module = CurrentProcessModule();
                if (module != nullptr) {
                    using API = SocketSystemAPI;
                    // clang-format off
                    loaded.create          = module->TryFindFunction<API::SocketFunction>("socket");
                    loaded.close           = module->TryFindFunction<API::CloseFunction>("close");
                    loaded.bind            = module->TryFindFunction<API::BindFunction>("bind");
                    loaded.getSocketName   = module->TryFindFunction<API::NameFunction>("getsockname");
                    loaded.getPeerName     = module->TryFindFunction<API::NameFunction>("getpeername");
                    loaded.setOption       = module->TryFindFunction<API::OptionFunction>("setsockopt");
                    loaded.getOption       = module->TryFindFunction<API::GetOptionFunction>("getsockopt");
                    loaded.ioctl           = module->TryFindFunction<API::IoctlFunction>("ioctl");
                    loaded.poll            = module->TryFindFunction<API::PollFunction>("poll");
                    loaded.getAddressInfo  = module->TryFindFunction<API::GetAddressInfoFunction>("getaddrinfo");
                    loaded.freeAddressInfo = module->TryFindFunction<API::FreeAddressInfoFunction>("freeaddrinfo");
                    loaded.getNameInfo     = module->TryFindFunction<API::GetNameInfoFunction>("getnameinfo");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const TcpSystemAPI& LoadTcpSystemAPI() noexcept {
            static const TcpSystemAPI api = [] {
                TcpSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                const ModulePtr& module = Ws2_32Module();
                if (module != nullptr) {
                    using API = TcpSystemAPI;
                    // clang-format off
                    loaded.listen   = module->TryFindFunction<API::ListenFunction>("listen");
                    loaded.accept   = module->TryFindFunction<API::AcceptFunction>("accept");
                    loaded.connect  = module->TryFindFunction<API::ConnectFunction>("connect");
                    loaded.send     = module->TryFindFunction<API::SendFunction>("send");
                    loaded.receive  = module->TryFindFunction<API::TransferFunction>("recv");
                    loaded.shutdown = module->TryFindFunction<API::ShutdownFunction>("shutdown");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                const ModulePtr& module = CurrentProcessModule();
                if (module != nullptr) {
                    using API = TcpSystemAPI;
                    // clang-format off
                    loaded.listen   = module->TryFindFunction<API::ListenFunction>("listen");
                    loaded.accept   = module->TryFindFunction<API::AcceptFunction>("accept");
                    loaded.connect  = module->TryFindFunction<API::ConnectFunction>("connect");
                    loaded.send     = module->TryFindFunction<API::SendFunction>("send");
                    loaded.receive  = module->TryFindFunction<API::TransferFunction>("recv");
                    loaded.shutdown = module->TryFindFunction<API::ShutdownFunction>("shutdown");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const UdpSystemAPI& LoadUdpSystemAPI() noexcept {
            static const UdpSystemAPI api = [] {
                UdpSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                const ModulePtr& module = Ws2_32Module();
                if (module != nullptr) {
                    using API = UdpSystemAPI;
                    loaded.sendTo = module->TryFindFunction<API::SendToFunction>("sendto");
                    loaded.receiveFrom = module->TryFindFunction<API::ReceiveFromFunction>("recvfrom");
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                const ModulePtr& module = CurrentProcessModule();
                if (module != nullptr) {
                    using API = UdpSystemAPI;
                    loaded.sendTo = module->TryFindFunction<API::SendToFunction>("sendto");
                    loaded.receiveFrom = module->TryFindFunction<API::ReceiveFromFunction>("recvfrom");
                }
#endif
                return loaded;
            }();
            return api;
        }

        const ClockSystemAPI& LoadClockSystemAPI() noexcept {
            static const ClockSystemAPI api = [] {
                using API = ClockSystemAPI;
                API loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                    loaded.queryCounter         = module->TryFindFunction<API::QueryCounterFunction>("QueryPerformanceCounter");
                    loaded.queryFrequency       = module->TryFindFunction<API::QueryFrequencyFunction>("QueryPerformanceFrequency");
                    loaded.getPreciseSystemTime = module->TryFindFunction<API::GetPreciseSystemTimeFunction>("GetSystemTimePreciseAsFileTime");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.getTime       = module->TryFindFunction<API::GetTimeFunction>("clock_gettime");
                    loaded.getResolution = module->TryFindFunction<API::GetResolutionFunction>("clock_getres");
                    loaded.sleep         = module->TryFindFunction<API::SleepFunction>("clock_nanosleep");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const AdvancedSocketSystemAPI& LoadAdvancedSocketSystemAPI() noexcept {
            static const AdvancedSocketSystemAPI api = [] {
                using API = AdvancedSocketSystemAPI;
                API loaded{};
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.accept4         = module->TryFindFunction<API::Accept4Function>("accept4");
                    loaded.sendMessage     = module->TryFindFunction<API::SendMessageFunction>("sendmsg");
                    loaded.receiveMessage  = module->TryFindFunction<API::ReceiveMessageFunction>("recvmsg");
                    loaded.sendMessages    = module->TryFindFunction<API::SendMessagesFunction>("sendmmsg");
                    loaded.receiveMessages = module->TryFindFunction<API::ReceiveMessagesFunction>("recvmmsg");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const NetworkInterfaceSystemAPI& LoadNetworkInterfaceSystemAPI() noexcept {
            static const NetworkInterfaceSystemAPI api = [] {
                using API = NetworkInterfaceSystemAPI;
                API loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = IphlpapiModule(); module != nullptr) {
                    // clang-format off
                    loaded.getAdaptersAddresses = module->TryFindFunction<API::GetAdaptersAddressesFunction>("GetAdaptersAddresses");
                    loaded.getIfTable2          = module->TryFindFunction<API::GetIfTable2Function>("GetIfTable2");
                    loaded.freeMibTable         = module->TryFindFunction<API::FreeMibTableFunction>("FreeMibTable");
                    loaded.notifyAddressChange  = module->TryFindFunction<API::NotifyAddressChangeFunction>("NotifyAddrChange");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.getIfAddresses  = module->TryFindFunction<API::GetIfAddressesFunction>("getifaddrs");
                    loaded.freeIfAddresses = module->TryFindFunction<API::FreeIfAddressesFunction>("freeifaddrs");
                    loaded.ifNameToIndex   = module->TryFindFunction<API::IfNameToIndexFunction>("if_nametoindex");
                    loaded.ifIndexToName   = module->TryFindFunction<API::IfIndexToNameFunction>("if_indextoname");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

#if defined(PLATFORM_WINDOWS)
        const WinsockOverlappedSystemAPI& LoadWinsockOverlappedSystemAPI() noexcept {
            static const WinsockOverlappedSystemAPI api = [] {
                using API = WinsockOverlappedSystemAPI;
                API loaded{};
                const ModulePtr& module = Ws2_32Module();
                if (module == nullptr) {
                    return loaded;
                }
                // clang-format off
            loaded.create      = module->TryFindFunction<API::CreateFunction>("WSASocketW");
            loaded.send        = module->TryFindFunction<API::SendFunction>("WSASend");
            loaded.receive     = module->TryFindFunction<API::ReceiveFunction>("WSARecv");
            loaded.sendTo      = module->TryFindFunction<API::SendToFunction>("WSASendTo");
            loaded.receiveFrom = module->TryFindFunction<API::ReceiveFromFunction>("WSARecvFrom");
            loaded.getResult   = module->TryFindFunction<API::GetResultFunction>("WSAGetOverlappedResult");
            loaded.control     = module->TryFindFunction<API::ControlFunction>("WSAIoctl");
                // clang-format on
                return loaded;
            }();
            return api;
        }
#elif defined(PLATFORM_LINUX)
        const EpollSystemAPI& LoadEpollSystemAPI() noexcept {
            static const EpollSystemAPI api = [] {
                using API = EpollSystemAPI;
                API loaded{};
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.create              = module->TryFindFunction<API::CreateFunction>("epoll_create1");
                    loaded.control             = module->TryFindFunction<API::ControlFunction>("epoll_ctl");
                    loaded.wait                = module->TryFindFunction<API::WaitFunction>("epoll_wait");
                    gNativeEpollWaitMasked     = module->TryFindFunction<NativeEpollWaitMaskedFunction>("epoll_pwait");
                    gNativeEpollWaitMaskedTime = module->TryFindFunction<NativeEpollWaitMaskedTimeFunction>("epoll_pwait2");
                    loaded.waitMasked          = gNativeEpollWaitMasked != nullptr ? &EpollWaitMaskedAdapter : nullptr;
                    loaded.waitMaskedTime      = gNativeEpollWaitMaskedTime != nullptr ? &EpollWaitMaskedTimeAdapter : nullptr;
                    // clang-format on
                }
                return loaded;
            }();
            return api;
        }

        const EventFdSystemAPI& LoadEventFdSystemAPI() noexcept {
            static const EventFdSystemAPI api = [] {
                EventFdSystemAPI loaded{};
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    loaded.create = module->TryFindFunction<EventFdSystemAPI::CreateFunction>("eventfd");
                }
                return loaded;
            }();
            return api;
        }

        const TimerFdSystemAPI& LoadTimerFdSystemAPI() noexcept {
            static const TimerFdSystemAPI api = [] {
                using API = TimerFdSystemAPI;
                API loaded{};
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    loaded.create = module->TryFindFunction<API::CreateFunction>("timerfd_create");
                    loaded.setTime = module->TryFindFunction<API::SetTimeFunction>("timerfd_settime");
                    loaded.getTime = module->TryFindFunction<API::GetTimeFunction>("timerfd_gettime");
                }
                return loaded;
            }();
            return api;
        }

        const SignalFdSystemAPI& LoadSignalFdSystemAPI() noexcept {
            static const SignalFdSystemAPI api = [] {
                SignalFdSystemAPI loaded{};
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    gNativeSignalFd = module->TryFindFunction<NativeSignalFdFunction>("signalfd");
                    loaded.create = gNativeSignalFd != nullptr ? &SignalFdAdapter : nullptr;
                }
                return loaded;
            }();
            return api;
        }
#endif

        const SchedulerTraceSystemAPI& LoadSchedulerTraceSystemAPI() noexcept {
            static const SchedulerTraceSystemAPI api = [] {
                SchedulerTraceSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                const ModulePtr& tracing = Advapi32Module();
                const ModulePtr& kernel = Kernel32Module();
                if (tracing != nullptr) {
                    // clang-format off
                loaded.startTrace   = tracing->TryFindFunction<SchedulerTraceSystemAPI::StartTraceFunction>("StartTraceW");
                loaded.controlTrace = tracing->TryFindFunction<SchedulerTraceSystemAPI::ControlTraceFunction>("ControlTraceW");
                loaded.openTrace    = tracing->TryFindFunction<SchedulerTraceSystemAPI::OpenTraceFunction>("OpenTraceW");
                loaded.processTrace = tracing->TryFindFunction<SchedulerTraceSystemAPI::ProcessTraceFunction>("ProcessTrace");
                loaded.closeTrace   = tracing->TryFindFunction<SchedulerTraceSystemAPI::CloseTraceFunction>("CloseTrace");
                    // clang-format on
                }
                if (kernel != nullptr) {
                    // clang-format off
                loaded.queryPerformanceFrequency = kernel->TryFindFunction<SchedulerTraceSystemAPI::QueryPerformanceFrequencyFunction>("QueryPerformanceFrequency");
                    // clang-format on
                }
                if (EnsureSystemAPIs(loaded.startTrace, loaded.controlTrace, loaded.openTrace, loaded.processTrace,
                                     loaded.closeTrace, loaded.queryPerformanceFrequency)) {
                    loaded.start = StartWindowsSchedulerTrace;
                    loaded.process = ProcessWindowsSchedulerTrace;
                    loaded.stop = StopWindowsSchedulerTrace;
                    loaded.close = CloseWindowsSchedulerTrace;
                }
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
                const ModulePtr& module = Kernel32Module();
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
                const ModulePtr& module = User32Module();
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

        const DisplaySystemAPI& LoadDisplaySystemAPI() noexcept {
            static const DisplaySystemAPI api = [] {
                using API = DisplaySystemAPI;
                API loaded{};
#if defined(PLATFORM_WINDOWS)
                const ModulePtr& user32 = User32Module();
                if (user32 != nullptr) {
                    // clang-format off
                    loaded.enumDisplayMonitors                 = user32->TryFindFunction<API::EnumDisplayMonitorsFunction>("EnumDisplayMonitors");
                    loaded.getMonitorInfoWide                  = user32->TryFindFunction<API::GetMonitorInfoWideFunction>("GetMonitorInfoW");
                    loaded.monitorFromPoint                    = user32->TryFindFunction<API::MonitorFromPointFunction>("MonitorFromPoint");
                    loaded.monitorFromWindow                   = user32->TryFindFunction<API::MonitorFromWindowFunction>("MonitorFromWindow");
                    loaded.getWindowRect                       = user32->TryFindFunction<API::GetWindowRectFunction>("GetWindowRect");
                    loaded.getClientRect                       = user32->TryFindFunction<API::GetClientRectFunction>("GetClientRect");
                    loaded.clientToScreen                      = user32->TryFindFunction<API::TranslatePointFunction>("ClientToScreen");
                    loaded.screenToClient                      = user32->TryFindFunction<API::TranslatePointFunction>("ScreenToClient");
                    loaded.getCursorPosition                   = user32->TryFindFunction<API::GetCursorPositionFunction>("GetCursorPos");
                    loaded.getSystemMetrics                    = user32->TryFindFunction<API::GetSystemMetricsFunction>("GetSystemMetrics");
                    loaded.getSystemMetricsForDpi              = user32->TryFindFunction<API::GetSystemMetricsForDpiFunction>("GetSystemMetricsForDpi");
                    loaded.getDpiForWindow                     = user32->TryFindFunction<API::GetDpiForWindowFunction>("GetDpiForWindow");
                    loaded.getDpiForSystem                     = user32->TryFindFunction<API::GetDpiForSystemFunction>("GetDpiForSystem");
                    loaded.getSystemDpiForProcess              = user32->TryFindFunction<API::GetSystemDpiForProcessFunction>("GetSystemDpiForProcess");
                    loaded.getWindowDpiAwarenessContext        = user32->TryFindFunction<API::GetWindowDpiAwarenessContextFunction>("GetWindowDpiAwarenessContext");
                    loaded.setProcessDpiAwarenessContext       = user32->TryFindFunction<API::SetProcessDpiAwarenessContextFunction>("SetProcessDpiAwarenessContext");
                    loaded.setThreadDpiAwarenessContext        = user32->TryFindFunction<API::SetThreadDpiAwarenessContextFunction>("SetThreadDpiAwarenessContext");
                    loaded.getThreadDpiAwarenessContext        = user32->TryFindFunction<API::GetThreadDpiAwarenessContextFunction>("GetThreadDpiAwarenessContext");
                    loaded.getAwarenessFromDpiAwarenessContext = user32->TryFindFunction<API::GetAwarenessFromDpiAwarenessContextFunction>("GetAwarenessFromDpiAwarenessContext");
                    loaded.adjustWindowRectExForDpi            = user32->TryFindFunction<API::AdjustWindowRectExForDpiFunction>("AdjustWindowRectExForDpi");
                    // clang-format on
                }
                const ModulePtr& shcore = ShcoreModule();
                if (shcore != nullptr) {
                    // clang-format off
                    loaded.getDpiForMonitor       = shcore->TryFindFunction<API::GetDpiForMonitorFunction>("GetDpiForMonitor");
                    loaded.setProcessDpiAwareness = shcore->TryFindFunction<API::SetProcessDpiAwarenessFunction>("SetProcessDpiAwareness");
                    loaded.getProcessDpiAwareness = shcore->TryFindFunction<API::GetProcessDpiAwarenessFunction>("GetProcessDpiAwareness");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const Win32WindowSystemAPI& LoadWin32WindowSystemAPI() noexcept {
            static const Win32WindowSystemAPI api = [] {
                using API = Win32WindowSystemAPI;
                API loaded{};
#if defined(PLATFORM_WINDOWS)
                const ModulePtr& user32 = User32Module();
                if (user32 == nullptr) {
                    return loaded;
                }
                // clang-format off
                loaded.registerClassExWide         = user32->TryFindFunction<API::RegisterClassExWideFunction>("RegisterClassExW");
                loaded.unregisterClassWide         = user32->TryFindFunction<API::UnregisterClassWideFunction>("UnregisterClassW");
                loaded.createWindowExWide          = user32->TryFindFunction<API::CreateWindowExWideFunction>("CreateWindowExW");
                loaded.destroyWindow               = user32->TryFindFunction<API::DestroyWindowFunction>("DestroyWindow");
                loaded.showWindow                  = user32->TryFindFunction<API::ShowWindowFunction>("ShowWindow");
                loaded.updateWindow                = user32->TryFindFunction<API::UpdateWindowFunction>("UpdateWindow");
                loaded.setWindowTextWide           = user32->TryFindFunction<API::SetWindowTextWideFunction>("SetWindowTextW");
                loaded.getWindowLongPtr            = user32->TryFindFunction<API::GetWindowLongPtrFunction>("GetWindowLongPtrW");
                loaded.setWindowLongPtr            = user32->TryFindFunction<API::SetWindowLongPtrFunction>("SetWindowLongPtrW");
                loaded.defWindowProc               = user32->TryFindFunction<API::WindowProcedureFunction>("DefWindowProcW");
                loaded.peekMessage                 = user32->TryFindFunction<API::PeekMessageFunction>("PeekMessageW");
                loaded.getMessage                  = user32->TryFindFunction<API::GetMessageFunction>("GetMessageW");
                loaded.translateMessage            = user32->TryFindFunction<API::TranslateMessageFunction>("TranslateMessage");
                loaded.dispatchMessage             = user32->TryFindFunction<API::DispatchMessageFunction>("DispatchMessageW");
                loaded.postQuitMessage             = user32->TryFindFunction<API::PostQuitMessageFunction>("PostQuitMessage");
                loaded.postMessage                 = user32->TryFindFunction<API::PostMessageFunction>("PostMessageW");
                loaded.sendMessage                 = user32->TryFindFunction<API::SendMessageFunction>("SendMessageW");
                loaded.setWindowPosition           = user32->TryFindFunction<API::SetWindowPositionFunction>("SetWindowPos");
                loaded.setCapture                  = user32->TryFindFunction<API::SetCaptureFunction>("SetCapture");
                loaded.releaseCapture              = user32->TryFindFunction<API::ReleaseCaptureFunction>("ReleaseCapture");
                loaded.setCursor                   = user32->TryFindFunction<API::SetCursorFunction>("SetCursor");
                loaded.showCursor                  = user32->TryFindFunction<API::ShowCursorFunction>("ShowCursor");
                loaded.setCursorPosition           = user32->TryFindFunction<API::SetCursorPositionFunction>("SetCursorPos");
                loaded.clipCursor                  = user32->TryFindFunction<API::ClipCursorFunction>("ClipCursor");
                loaded.setFocus                    = user32->TryFindFunction<API::SetFocusFunction>("SetFocus");
                loaded.getFocus                    = user32->TryFindFunction<API::GetFocusFunction>("GetFocus");
                loaded.setForegroundWindow         = user32->TryFindFunction<API::SetForegroundWindowFunction>("SetForegroundWindow");
                loaded.isWindow                    = user32->TryFindFunction<API::IsWindowFunction>("IsWindow");
                loaded.isWindowVisible             = user32->TryFindFunction<API::IsWindowVisibleFunction>("IsWindowVisible");
                loaded.isIconic                    = user32->TryFindFunction<API::IsIconicFunction>("IsIconic");
                loaded.isZoomed                    = user32->TryFindFunction<API::IsZoomedFunction>("IsZoomed");
                loaded.waitMessage                 = user32->TryFindFunction<API::WaitMessageFunction>("WaitMessage");
                loaded.msgWaitForMultipleObjectsEx = user32->TryFindFunction<API::MsgWaitForMultipleObjectsExFunction>("MsgWaitForMultipleObjectsEx");
                // clang-format on
#endif
                return loaded;
            }();
            return api;
        }

        const X11WindowSystemAPI& LoadX11WindowSystemAPI() noexcept {
            static const X11WindowSystemAPI api = [] {
                using API = X11WindowSystemAPI;
                API loaded{};
#if defined(PLATFORM_LINUX) && !defined(__ANDROID__)
                const ModulePtr module = LoadOptionalModule({"X11", "libX11.so.6"});
                if (module == nullptr) {
                    return loaded;
                }
                // clang-format off
                loaded.openDisplay           = module->TryFindFunction<API::OpenDisplayFunction>("XOpenDisplay");
                loaded.closeDisplay          = module->TryFindFunction<API::CloseDisplayFunction>("XCloseDisplay");
                loaded.defaultScreen         = module->TryFindFunction<API::DefaultScreenFunction>("XDefaultScreen");
                loaded.rootWindow            = module->TryFindFunction<API::RootWindowFunction>("XRootWindow");
                loaded.defaultVisual         = module->TryFindFunction<API::DefaultVisualFunction>("XDefaultVisual");
                loaded.defaultDepth          = module->TryFindFunction<API::DefaultDepthFunction>("XDefaultDepth");
                loaded.createSimpleWindow    = module->TryFindFunction<API::CreateSimpleWindowFunction>("XCreateSimpleWindow");
                loaded.createWindow          = module->TryFindFunction<API::CreateWindowFunction>("XCreateWindow");
                loaded.destroyWindow         = module->TryFindFunction<API::DestroyWindowFunction>("XDestroyWindow");
                loaded.mapWindow             = module->TryFindFunction<API::MapWindowFunction>("XMapWindow");
                loaded.unmapWindow           = module->TryFindFunction<API::UnmapWindowFunction>("XUnmapWindow");
                loaded.moveWindow            = module->TryFindFunction<API::MoveWindowFunction>("XMoveWindow");
                loaded.resizeWindow          = module->TryFindFunction<API::ResizeWindowFunction>("XResizeWindow");
                loaded.moveResizeWindow      = module->TryFindFunction<API::MoveResizeWindowFunction>("XMoveResizeWindow");
                loaded.selectInput           = module->TryFindFunction<API::SelectInputFunction>("XSelectInput");
                loaded.storeName             = module->TryFindFunction<API::StoreNameFunction>("XStoreName");
                loaded.internAtom            = module->TryFindFunction<API::InternAtomFunction>("XInternAtom");
                loaded.setWmProtocols        = module->TryFindFunction<API::SetWmProtocolsFunction>("XSetWMProtocols");
                loaded.pending               = module->TryFindFunction<API::PendingFunction>("XPending");
                loaded.nextEvent             = module->TryFindFunction<API::NextEventFunction>("XNextEvent");
                loaded.checkMaskEvent        = module->TryFindFunction<API::CheckMaskEventFunction>("XCheckMaskEvent");
                loaded.connectionNumber      = module->TryFindFunction<API::ConnectionNumberFunction>("XConnectionNumber");
                loaded.flush                 = module->TryFindFunction<API::FlushFunction>("XFlush");
                loaded.sync                  = module->TryFindFunction<API::SyncFunction>("XSync");
                loaded.free                  = module->TryFindFunction<API::FreeFunction>("XFree");
                loaded.getWindowAttributes   = module->TryFindFunction<API::GetWindowAttributesFunction>("XGetWindowAttributes");
                loaded.setInputFocus         = module->TryFindFunction<API::SetInputFocusFunction>("XSetInputFocus");
                loaded.iconifyWindow         = module->TryFindFunction<API::IconifyWindowFunction>("XIconifyWindow");
                loaded.raiseWindow           = module->TryFindFunction<API::RaiseWindowFunction>("XRaiseWindow");
                loaded.grabPointer           = module->TryFindFunction<API::GrabPointerFunction>("XGrabPointer");
                loaded.ungrabPointer         = module->TryFindFunction<API::UngrabPointerFunction>("XUngrabPointer");
                loaded.warpPointer           = module->TryFindFunction<API::WarpPointerFunction>("XWarpPointer");
                loaded.createFontCursor      = module->TryFindFunction<API::CreateFontCursorFunction>("XCreateFontCursor");
                loaded.defineCursor          = module->TryFindFunction<API::DefineCursorFunction>("XDefineCursor");
                loaded.undefineCursor        = module->TryFindFunction<API::UndefineCursorFunction>("XUndefineCursor");
                loaded.freeCursor            = module->TryFindFunction<API::FreeCursorFunction>("XFreeCursor");
                loaded.openInputMethod       = module->TryFindFunction<API::OpenInputMethodFunction>("XOpenIM");
                loaded.closeInputMethod      = module->TryFindFunction<API::CloseInputMethodFunction>("XCloseIM");
                loaded.createInputContext    = module->TryFindFunction<API::CreateInputContextFunction>("XCreateIC");
                loaded.destroyInputContext   = module->TryFindFunction<API::DestroyInputContextFunction>("XDestroyIC");
                loaded.filterEvent           = module->TryFindFunction<API::FilterEventFunction>("XFilterEvent");
                loaded.utf8LookupString      = module->TryFindFunction<API::Utf8LookupStringFunction>("Xutf8LookupString");
                // clang-format on
#endif
                return loaded;
            }();
            return api;
        }

        const XcbWindowSystemAPI& LoadXcbWindowSystemAPI() noexcept {
            static const XcbWindowSystemAPI api = [] {
                using API = XcbWindowSystemAPI;
                API loaded{};
#if defined(PLATFORM_LINUX) && !defined(__ANDROID__)
                const ModulePtr module = LoadOptionalModule({"xcb", "libxcb.so.1"});
                if (module == nullptr) {
                    return loaded;
                }
                // clang-format off
                loaded.connect                 = module->TryFindFunction<API::ConnectFunction>("xcb_connect");
                loaded.disconnect              = module->TryFindFunction<API::DisconnectFunction>("xcb_disconnect");
                loaded.connectionHasError      = module->TryFindFunction<API::ConnectionHasErrorFunction>("xcb_connection_has_error");
                loaded.getSetup                = module->TryFindFunction<API::GetSetupFunction>("xcb_get_setup");
                loaded.generateId              = module->TryFindFunction<API::GenerateIdFunction>("xcb_generate_id");
                loaded.createWindow            = module->TryFindFunction<API::CreateWindowFunction>("xcb_create_window");
                loaded.changeProperty          = module->TryFindFunction<API::ChangePropertyFunction>("xcb_change_property");
                loaded.internAtom              = module->TryFindFunction<API::InternAtomFunction>("xcb_intern_atom");
                loaded.internAtomReply         = module->TryFindFunction<API::InternAtomReplyFunction>("xcb_intern_atom_reply");
                loaded.destroyWindow           = module->TryFindFunction<API::DestroyWindowFunction>("xcb_destroy_window");
                loaded.mapWindow               = module->TryFindFunction<API::MapWindowFunction>("xcb_map_window");
                loaded.unmapWindow             = module->TryFindFunction<API::UnmapWindowFunction>("xcb_unmap_window");
                loaded.configureWindow         = module->TryFindFunction<API::ConfigureWindowFunction>("xcb_configure_window");
                loaded.pollForEvent            = module->TryFindFunction<API::PollForEventFunction>("xcb_poll_for_event");
                loaded.waitForEvent            = module->TryFindFunction<API::WaitForEventFunction>("xcb_wait_for_event");
                loaded.flush                   = module->TryFindFunction<API::FlushFunction>("xcb_flush");
                loaded.getFileDescriptor       = module->TryFindFunction<API::GetFileDescriptorFunction>("xcb_get_file_descriptor");
                loaded.getMaximumRequestLength = module->TryFindFunction<API::GetMaximumRequestLengthFunction>("xcb_get_maximum_request_length");
                // clang-format on
#endif
                return loaded;
            }();
            return api;
        }

        const WaylandWindowSystemAPI& LoadWaylandWindowSystemAPI() noexcept {
            static const WaylandWindowSystemAPI api = [] {
                using API = WaylandWindowSystemAPI;
                API loaded{};
#if defined(PLATFORM_LINUX) && !defined(__ANDROID__)
                const ModulePtr module = LoadOptionalModule({"wayland-client", "libwayland-client.so.0"});
                if (module == nullptr) {
                    return loaded;
                }
                // clang-format off
                loaded.connect                 = module->TryFindFunction<API::ConnectFunction>("wl_display_connect");
                loaded.connectToFd             = module->TryFindFunction<API::ConnectFdFunction>("wl_display_connect_to_fd");
                loaded.disconnect              = module->TryFindFunction<API::DisconnectFunction>("wl_display_disconnect");
                loaded.dispatch                = module->TryFindFunction<API::DispatchFunction>("wl_display_dispatch");
                loaded.dispatchPending         = module->TryFindFunction<API::DispatchPendingFunction>("wl_display_dispatch_pending");
                loaded.roundtrip               = module->TryFindFunction<API::RoundtripFunction>("wl_display_roundtrip");
                loaded.flush                   = module->TryFindFunction<API::FlushFunction>("wl_display_flush");
                loaded.getFileDescriptor       = module->TryFindFunction<API::GetFileDescriptorFunction>("wl_display_get_fd");
                loaded.prepareRead             = module->TryFindFunction<API::PrepareReadFunction>("wl_display_prepare_read");
                loaded.cancelRead              = module->TryFindFunction<API::CancelReadFunction>("wl_display_cancel_read");
                loaded.readEvents              = module->TryFindFunction<API::ReadEventsFunction>("wl_display_read_events");
                loaded.proxyAddListener        = module->TryFindFunction<API::ProxyAddListenerFunction>("wl_proxy_add_listener");
                loaded.proxyDestroy            = module->TryFindFunction<API::ProxyDestroyFunction>("wl_proxy_destroy");
                loaded.proxyMarshalFlags       = module->TryFindFunction<API::ProxyMarshalFlagsFunction>("wl_proxy_marshal_flags");
                loaded.proxyGetVersion         = module->TryFindFunction<API::ProxyGetVersionFunction>("wl_proxy_get_version");
                loaded.proxySetUserData        = module->TryFindFunction<API::ProxySetUserDataFunction>("wl_proxy_set_user_data");
                loaded.proxyGetUserData        = module->TryFindFunction<API::ProxyGetUserDataFunction>("wl_proxy_get_user_data");
                loaded.displayGetError         = module->TryFindFunction<API::DisplayGetErrorFunction>("wl_display_get_error");
                loaded.displayGetProtocolError = module->TryFindFunction<API::DisplayGetProtocolErrorFunction>("wl_display_get_protocol_error");
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
                const ModulePtr& module = Kernel32Module();
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
                loaded.readFile              = module->TryFindFunction<API::ReadFileFunction>("ReadFile");
                loaded.writeFile             = module->TryFindFunction<API::WriteFileFunction>("WriteFile");
                loaded.flushFileBuffers      = module->TryFindFunction<API::FlushFileBuffersFunction>("FlushFileBuffers");
                loaded.getFileSize           = module->TryFindFunction<API::GetFileSizeFunction>("GetFileSizeEx");
                loaded.setFileInformation    = module->TryFindFunction<API::SetFileInformationFunction>("SetFileInformationByHandle");
                loaded.getFileInformation    = module->TryFindFunction<API::GetFileInformationFunction>("GetFileInformationByHandleEx");
                loaded.replaceFileWide       = module->TryFindFunction<API::ReplaceFileWideFunction>("ReplaceFileW");
                loaded.moveFileWide          = module->TryFindFunction<API::MoveFileWideFunction>("MoveFileExW");
                loaded.deleteFileWide        = module->TryFindFunction<API::DeleteFileWideFunction>("DeleteFileW");
                loaded.getOverlappedResult   = module->TryFindFunction<API::GetOverlappedResultFunction>("GetOverlappedResult");
                loaded.getFileType            = module->TryFindFunction<API::GetFileTypeFunction>("GetFileType");
                loaded.setFilePointer         = module->TryFindFunction<API::SetFilePointerFunction>("SetFilePointerEx");
                loaded.cancelIo              = module->TryFindFunction<API::CancelIoFunction>("CancelIoEx");
                loaded.lockFile              = module->TryFindFunction<API::LockFileFunction>("LockFileEx");
                loaded.readDirectoryChanges  = module->TryFindFunction<API::ReadDirectoryChangesFunction>("ReadDirectoryChangesW");
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
                loaded.rename                = module->TryFindFunction<API::RenameFunction>("rename");
                loaded.unlink                = module->TryFindFunction<API::UnlinkFunction>("unlink");
                loaded.control               = module->TryFindFunction<API::ControlFunction>("fcntl");
                loaded.queryFileBlockSize    = loaded.stat != nullptr ? &QueryPosixFileBlockSize : nullptr;
                loaded.queryFileSize         = loaded.stat != nullptr ? &QueryPosixFileSize : nullptr;
                loaded.duplicate             = module->TryFindFunction<API::DuplicateFunction>("dup");
                loaded.duplicateTo           = module->TryFindFunction<API::DuplicateToFunction>("dup2");
                loaded.adviseFile            = module->TryFindFunction<API::AdviseFileFunction>("posix_fadvise");
                loaded.lock                  = module->TryFindFunction<API::LockFunction>("flock");
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

        const ModernFileSystemAPI& LoadModernFileSystemAPI() noexcept {
            static const ModernFileSystemAPI api = [] {
                ModernFileSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
                if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                    // clang-format off
                    loaded.createDirectoryWide          = module->TryFindFunction<ModernFileSystemAPI::CreateDirectoryWideFunction>("CreateDirectoryW");
                    loaded.removeDirectoryWide          = module->TryFindFunction<ModernFileSystemAPI::RemoveDirectoryWideFunction>("RemoveDirectoryW");
                    loaded.findFirstFileWide            = module->TryFindFunction<ModernFileSystemAPI::FindFirstFileWideFunction>("FindFirstFileW");
                    loaded.findNextFileWide             = module->TryFindFunction<ModernFileSystemAPI::FindNextFileWideFunction>("FindNextFileW");
                    loaded.findClose                    = module->TryFindFunction<ModernFileSystemAPI::FindCloseFunction>("FindClose");
                    loaded.getFileAttributesExWide      = module->TryFindFunction<ModernFileSystemAPI::GetFileAttributesExWideFunction>("GetFileAttributesExW");
                    loaded.getFinalPathNameByHandleWide = module->TryFindFunction<ModernFileSystemAPI::GetFinalPathNameByHandleWideFunction>("GetFinalPathNameByHandleW");
                    loaded.createHardLinkWide           = module->TryFindFunction<ModernFileSystemAPI::CreateHardLinkWideFunction>("CreateHardLinkW");
                    loaded.createSymbolicLinkWide       = module->TryFindFunction<ModernFileSystemAPI::CreateSymbolicLinkWideFunction>("CreateSymbolicLinkW");
                    loaded.getVolumeInformationWide     = module->TryFindFunction<ModernFileSystemAPI::GetVolumeInformationWideFunction>("GetVolumeInformationW");
                    // clang-format on
                }
#elif defined(PLATFORM_LINUX)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.openAt          = module->TryFindFunction<ModernFileSystemAPI::OpenAtFunction>("openat");
#    if defined(SYS_openat2)
                    loaded.openAt2         = &OpenAt2ViaSyscall;
#    endif
                    loaded.makeDirectoryAt = module->TryFindFunction<ModernFileSystemAPI::MakeDirectoryAtFunction>("mkdirat");
                    loaded.unlinkAt        = module->TryFindFunction<ModernFileSystemAPI::UnlinkAtFunction>("unlinkat");
                    loaded.renameAt        = module->TryFindFunction<ModernFileSystemAPI::RenameAtFunction>("renameat");
                    loaded.readLinkAt      = module->TryFindFunction<ModernFileSystemAPI::ReadLinkAtFunction>("readlinkat");
                    loaded.statx           = module->TryFindFunction<ModernFileSystemAPI::StatxFunction>("statx");
                    loaded.copyFileRange   = module->TryFindFunction<ModernFileSystemAPI::CopyFileRangeFunction>("copy_file_range");
                    loaded.sendFile        = module->TryFindFunction<ModernFileSystemAPI::SendFileFunction>("sendfile");
                    loaded.allocate        = module->TryFindFunction<ModernFileSystemAPI::AllocateFunction>("fallocate");
                    loaded.syncFileSystem  = module->TryFindFunction<ModernFileSystemAPI::SyncFileSystemFunction>("syncfs");
                    loaded.memoryFile      = module->TryFindFunction<ModernFileSystemAPI::MemoryFileFunction>("memfd_create");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const FanotifySystemAPI& LoadFanotifySystemAPI() noexcept {
            static const FanotifySystemAPI api = [] {
                FanotifySystemAPI loaded{};
#if defined(PLATFORM_LINUX)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.initialize = module->TryFindFunction<FanotifySystemAPI::InitializeFunction>("fanotify_init");
                    loaded.mark       = module->TryFindFunction<FanotifySystemAPI::MarkFunction>("fanotify_mark");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const LinuxSyscallSystemAPI& LoadLinuxSyscallSystemAPI() noexcept {
            static const LinuxSyscallSystemAPI api = [] {
                LinuxSyscallSystemAPI loaded{};
#if defined(PLATFORM_LINUX)
                if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                    // clang-format off
                    loaded.syscall = module->TryFindFunction<LinuxSyscallSystemAPI::SyscallFunction>("syscall");
                    // clang-format on
                }
#endif
                return loaded;
            }();
            return api;
        }

        const PerfEventSystemAPI& LoadPerfEventSystemAPI() noexcept {
            static const PerfEventSystemAPI api = [] {
                PerfEventSystemAPI loaded{};
#if defined(PLATFORM_LINUX)
#    if defined(SYS_perf_event_open)
                if (LoadLinuxSyscallSystemAPI().syscall != nullptr) {
                    loaded.open = &OpenPerfEventViaSyscall;
                }
#    endif
#endif
                return loaded;
            }();
            return api;
        }

    } // namespace API

    bool InstallNativeCrashHandler(NativeCrashCallback callback, void* userData, bool suppressDialogs) noexcept {
        return InstallNativeCrashHandlerImpl(callback, userData, suppressDialogs);
    }

    void UninstallNativeCrashHandler() noexcept {
        UninstallNativeCrashHandlerImpl();
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

    bool LockedDbgHelpSystemAPI::ResolveAddress(void* process, uintptr_t address, DbgHelpAddressInfo& output) const {
        output = {};
#if defined(PLATFORM_WINDOWS)
        if (process == nullptr || address == 0) {
            return false;
        }

        bool resolved = false;
        const auto nativeAddress = static_cast<WindowsSystem::DWord64>(address);

        if (PAL::EnsureSystemAPIs(api_.symFromAddress)) {
            alignas(SYMBOL_INFO) thread_local std::array<std::byte, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage;
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            WindowsSystem::DWord64 displacement = 0;
            if (api_.symFromAddress(process, nativeAddress, &displacement, symbol) != FALSE) {
                output.symbol.assign(symbol->Name, symbol->NameLen);
                output.offset = displacement;
                resolved = true;
            }
        }

        if (PAL::EnsureSystemAPIs(api_.symGetLineFromAddress)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            WindowsSystem::DWord displacement = 0;
            if (api_.symGetLineFromAddress(process, nativeAddress, &displacement, &line)
                != FALSE
                && line.FileName
                != nullptr) {
                output.sourceFile = line.FileName;
                output.sourceLine = line.LineNumber;
                resolved = true;
            }
        }

        if (PAL::EnsureSystemAPIs(api_.symGetModuleInfo)) {
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            if (api_.symGetModuleInfo(process, nativeAddress, &module) != FALSE && module.ImageName[0] != 0) {
                output.modulePath = module.ImageName;
                resolved = true;
            }
        }
        return resolved;
#else
        static_cast<void>(process);
        static_cast<void>(address);
        return false;
#endif
    }

    bool InitializeDbgHelpSystemAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
        static std::once_flag flag;
        static bool initialized = false;
        try {
            std::call_once(flag, [] {
                LockedDbgHelpSystemAPI dbgHelp = LockDbgHelpSystemAPI();
                const ProcessSystemAPI& process = Sora::PAL::API::LoadProcessSystemAPI();
                if (!EnsureSystemAPIs(dbgHelp->symSetOptions, dbgHelp->symInitialize, process.getCurrentProcess)) {
                    return;
                }
                dbgHelp->symSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                initialized = dbgHelp->symInitialize(process.getCurrentProcess(), nullptr, TRUE) != FALSE;
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
            const ModulePtr& module = Kernel32Module();
            if (module == nullptr) {
                return loaded;
            }
            // clang-format off
            loaded.captureStackBackTrace = module->TryFindFunction<API::CaptureStackBackTraceFunction>("RtlCaptureStackBackTrace");
            // clang-format on
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const ModulePtr& module = CurrentProcessModule();
            if (module == nullptr) {
                return loaded;
            }
            // clang-format off
            loaded.captureStackBackTrace = module->TryFindFunction<API::CaptureStackBackTraceFunction>("backtrace");
            // clang-format on
#endif
            return loaded;
        }();
        return api;
    }

} // namespace Sora::PAL
