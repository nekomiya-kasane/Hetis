/**
 * @file SystemAPI.cpp
 * @brief Bootstrap and initialize immutable dynamically resolved system API tables.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/Unicode.h>


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
#    include <Windows.h>
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
#    include <poll.h>
#    include <sys/file.h>
#    include <ucontext.h>
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

        /** @brief Load one optional window-system module for the process lifetime. */
        [[nodiscard]] ModulePtr LoadWindowModule(std::initializer_list<std::string_view> names) noexcept {
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
            auto session =
                std::unique_ptr<WindowsSchedulerTraceSession>{new (std::nothrow) WindowsSchedulerTraceSession};
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
            properties.EnableFlags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_CSWITCH |
                                     EVENT_TRACE_FLAG_DISPATCHER;
            properties.LoggerNameOffset = static_cast<ULONG>(offsetof(WindowsSchedulerTraceProperties, loggerName));
            std::memcpy(session->properties.loggerName, kSoraSchedulerSessionName, sizeof(kSoraSchedulerSessionName));

            LARGE_INTEGER frequency{};
            if (api.queryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0) {
                return ErrorCode::TraceCaptureFailed;
            }
            *ticksPerSecond = static_cast<uint64_t>(frequency.QuadPart);

            ULONG status =
                api.startTrace(&session->producerHandle, session->properties.loggerName, session->Properties());
            if (status != ERROR_SUCCESS) {
                return ClassifyTraceStatus(status);
            }

            EVENT_TRACE_LOGFILEW log{};
            log.LoggerName = session->properties.loggerName;
            log.ProcessTraceMode =
                PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
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
                const TRACEHANDLE consumerHandle =
                    session.consumerHandle.exchange(INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel);
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
            const TRACEHANDLE consumerHandle =
                session->consumerHandle.exchange(INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel);
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
        static_assert(sizeof(WindowsSystem::ProcessorNumber)                     == sizeof(PROCESSOR_NUMBER));
        static_assert(alignof(WindowsSystem::ProcessorNumber)                    == alignof(PROCESSOR_NUMBER));
        static_assert(sizeof(WindowsSystem::Overlapped)                          == sizeof(OVERLAPPED));
        static_assert(alignof(WindowsSystem::Overlapped)                         == alignof(OVERLAPPED));
        static_assert(offsetof(WindowsSystem::Overlapped, internal)              == offsetof(OVERLAPPED, Internal));
        static_assert(offsetof(WindowsSystem::Overlapped, internalHigh)          == offsetof(OVERLAPPED, InternalHigh));
        static_assert(sizeof(WindowsSystem::FileNotifyInformationHeader)         == offsetof(FILE_NOTIFY_INFORMATION, FileName));
        static_assert(alignof(WindowsSystem::FileNotifyInformationHeader)        == alignof(FILE_NOTIFY_INFORMATION));
        static_assert(sizeof(WindowsSystem::SystemInfo)                          == sizeof(SYSTEM_INFO));
        static_assert(alignof(WindowsSystem::SystemInfo)                         == alignof(SYSTEM_INFO));
        static_assert(offsetof(WindowsSystem::SystemInfo, allocationGranularity) == offsetof(SYSTEM_INFO, dwAllocationGranularity));
        static_assert(sizeof(WindowsSystem::FileStorageInfo)                     == sizeof(FILE_STORAGE_INFO));
        static_assert(alignof(WindowsSystem::FileStorageInfo)                    == alignof(FILE_STORAGE_INFO));
        static_assert(sizeof(WindowsSystem::LargeInteger)                        == sizeof(LARGE_INTEGER));
        static_assert(alignof(WindowsSystem::LargeInteger)                       == alignof(LARGE_INTEGER));
        static_assert(sizeof(WindowsSystem::FileEndOfFileInformation)            == sizeof(FILE_END_OF_FILE_INFO));
        static_assert(WindowsSystem::kFileStorageInformation                     == FileStorageInfo);
        static_assert(WindowsSystem::kFileEndOfFileInformation                   == FileEndOfFileInfo);
        static_assert(WindowsSystem::kFileListDirectory                          == FILE_LIST_DIRECTORY);
        static_assert(WindowsSystem::kNotifyFileName                             == FILE_NOTIFY_CHANGE_FILE_NAME);
        static_assert(WindowsSystem::kNotifyDirectoryName                        == FILE_NOTIFY_CHANGE_DIR_NAME);
        static_assert(WindowsSystem::kNotifySize                                 == FILE_NOTIFY_CHANGE_SIZE);
        static_assert(WindowsSystem::kNotifyLastWrite                            == FILE_NOTIFY_CHANGE_LAST_WRITE);
        static_assert(WindowsSystem::kNotifyCreation                             == FILE_NOTIFY_CHANGE_CREATION);
        static_assert(WindowsSystem::kNotifySecurity                             == FILE_NOTIFY_CHANGE_SECURITY);
        static_assert(WindowsSystem::kActionAdded                                == FILE_ACTION_ADDED);
        static_assert(WindowsSystem::kActionRemoved                              == FILE_ACTION_REMOVED);
        static_assert(WindowsSystem::kActionModified                             == FILE_ACTION_MODIFIED);
        static_assert(WindowsSystem::kActionRenamedOld                           == FILE_ACTION_RENAMED_OLD_NAME);
        static_assert(WindowsSystem::kActionRenamedNew                           == FILE_ACTION_RENAMED_NEW_NAME);
        static_assert(WindowsSystem::kErrorIoPending                             == ERROR_IO_PENDING);
        static_assert(WindowsSystem::kInfinite                                   == INFINITE);
        static_assert(WindowsSystem::kWaitObject                                 == WAIT_OBJECT_0);
        static_assert(WindowsSystem::kWaitTimeout                                == WAIT_TIMEOUT);
        static_assert(WindowsSystem::kSnapshotThreads                            == TH32CS_SNAPTHREAD);

        static_assert(std::is_same_v<WindowsSystem::Bool,    BOOL>);
        static_assert(std::is_same_v<WindowsSystem::DWord,   DWORD>);
        static_assert(std::is_same_v<WindowsSystem::DWord64, DWORD64>);
        static_assert(std::is_same_v<WindowsSystem::UInt,    UINT>);
        static_assert(std::is_same_v<WindowsSystem::HResult, HRESULT>);
        static_assert(std::is_same_v<WindowsSystem::Long,    LONG>);
        static_assert(sizeof(WindowsSystem::ThreadEntry) == sizeof(THREADENTRY32));
        static_assert(alignof(WindowsSystem::ThreadEntry) == alignof(THREADENTRY32));
        static_assert(offsetof(WindowsSystem::ThreadEntry, size) == offsetof(THREADENTRY32, dwSize));
        static_assert(offsetof(WindowsSystem::ThreadEntry, usageCount) == offsetof(THREADENTRY32, cntUsage));
        static_assert(offsetof(WindowsSystem::ThreadEntry, threadId) == offsetof(THREADENTRY32, th32ThreadID));
        static_assert(offsetof(WindowsSystem::ThreadEntry, ownerProcessId) == offsetof(THREADENTRY32, th32OwnerProcessID));
        static_assert(offsetof(WindowsSystem::ThreadEntry, basePriority) == offsetof(THREADENTRY32, tpBasePri));
        static_assert(offsetof(WindowsSystem::ThreadEntry, priorityDelta) == offsetof(THREADENTRY32, tpDeltaPri));
        static_assert(offsetof(WindowsSystem::ThreadEntry, flags) == offsetof(THREADENTRY32, dwFlags));
        static_assert(sizeof(WindowsSystem::WindowClassEx) == sizeof(WNDCLASSEXW));
        static_assert(alignof(WindowsSystem::WindowClassEx) == alignof(WNDCLASSEXW));
        static_assert(offsetof(WindowsSystem::WindowClassEx, size) == offsetof(WNDCLASSEXW, cbSize));
        static_assert(offsetof(WindowsSystem::WindowClassEx, style) == offsetof(WNDCLASSEXW, style));
        static_assert(offsetof(WindowsSystem::WindowClassEx, windowProcedure) == offsetof(WNDCLASSEXW, lpfnWndProc));
        static_assert(offsetof(WindowsSystem::WindowClassEx, className) == offsetof(WNDCLASSEXW, lpszClassName));
        static_assert(sizeof(WindowsSystem::WindowMessage) == sizeof(MSG));
        static_assert(alignof(WindowsSystem::WindowMessage) == alignof(MSG));
        static_assert(offsetof(WindowsSystem::WindowMessage, window) == offsetof(MSG, hwnd));
        static_assert(offsetof(WindowsSystem::WindowMessage, message) == offsetof(MSG, message));
        static_assert(offsetof(WindowsSystem::WindowMessage, wParam) == offsetof(MSG, wParam));
        static_assert(offsetof(WindowsSystem::WindowMessage, lParam) == offsetof(MSG, lParam));
        static_assert(offsetof(WindowsSystem::WindowMessage, time) == offsetof(MSG, time));
        static_assert(offsetof(WindowsSystem::WindowMessage, point) == offsetof(MSG, pt));

        static_assert(std::is_same_v<DbgHelpSystemAPI::SymSetOptionsFunction,                   decltype(&::SymSetOptions)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymInitializeFunction,                   decltype(&::SymInitialize)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymFromAddressFunction,                  decltype(&::SymFromAddr)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymGetLineFromAddressFunction,           decltype(&::SymGetLineFromAddr64)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::SymGetModuleInfoFunction,                decltype(&::SymGetModuleInfo64)>);
        static_assert(std::is_same_v<DbgHelpSystemAPI::UndecorateSymbolNameFunction,            decltype(&::UnDecorateSymbolName)>);
       
        static_assert(std::is_same_v<StackTraceSystemAPI::CaptureStackBackTraceFunction,        decltype(&::RtlCaptureStackBackTrace)>);

        static_assert(std::is_same_v<SchedulerTraceSystemAPI::StartTraceFunction, decltype(&::StartTraceW)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::ControlTraceFunction, decltype(&::ControlTraceW)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::OpenTraceFunction, decltype(&::OpenTraceW)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::ProcessTraceFunction, decltype(&::ProcessTrace)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::CloseTraceFunction, decltype(&::CloseTrace)>);
        static_assert(std::is_same_v<SchedulerTraceSystemAPI::QueryPerformanceFrequencyFunction, decltype(&::QueryPerformanceFrequency)>);

        static_assert(std::is_same_v<ProcessSystemAPI::GetCurrentProcessIdFunction,             decltype(&::GetCurrentProcessId)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetCurrentProcessFunction,               decltype(&::GetCurrentProcess)>);
        static_assert(std::is_same_v<ProcessSystemAPI::CreateProcessSnapshotFunction,           decltype(&::CreateToolhelp32Snapshot)>);
        static_assert(std::is_same_v<ProcessSystemAPI::ReadProcessEntryFunction,                decltype(&::Process32FirstW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::ReadProcessEntryFunction,                decltype(&::Process32NextW)>);
        static_assert(std::is_same_v<HandleSystemAPI::CloseHandleFunction,                      decltype(&::CloseHandle)>);
        static_assert(std::is_same_v<ProcessSystemAPI::QueryFullProcessImageNameWideFunction,   decltype(&::QueryFullProcessImageNameW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetCommandLineWideFunction,              decltype(&::GetCommandLineW)>);
        static_assert(std::is_same_v<ProcessSystemAPI::CommandLineToArgvWideFunction,           decltype(&::CommandLineToArgvW)>);
        static_assert(std::is_same_v<LocalMemorySystemAPI::LocalFreeFunction,                   decltype(&::LocalFree)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessTimesFunction,                 decltype(&::GetProcessTimes)>);
        static_assert(std::is_same_v<ProcessSystemAPI::GetProcessMemoryInfoFunction,            decltype(&::K32GetProcessMemoryInfo)>);
        static_assert(std::is_same_v<ThreadSystemAPI::OpenThreadFunction,                       decltype(&::OpenThread)>);
        static_assert(std::is_same_v<HandleSystemAPI::DuplicateHandleFunction,                  decltype(&::DuplicateHandle)>);
        static_assert(std::is_same_v<EventSystemAPI::CreateEventWideFunction,                   decltype(&::CreateEventW)>);
        static_assert(std::is_same_v<EventSystemAPI::SetEventFunction,                          decltype(&::SetEvent)>);
        static_assert(std::is_same_v<EventSystemAPI::ResetEventFunction,                        decltype(&::ResetEvent)>);
        static_assert(std::is_same_v<SynchronizationSystemAPI::WaitForSingleObjectFunction,     decltype(&::WaitForSingleObject)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::CreateFileMappingWideFunction,       decltype(&::CreateFileMappingW)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::MapViewOfFileFunction,               decltype(&::MapViewOfFile)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::UnmapViewOfFileFunction,             decltype(&::UnmapViewOfFile)>);
        static_assert(std::is_same_v<FileMappingSystemAPI::FlushViewOfFileFunction,             decltype(&::FlushViewOfFile)>);

        static_assert(sizeof(WindowsSystem::GlobalMemory)  == sizeof(HGLOBAL));
        static_assert(alignof(WindowsSystem::GlobalMemory) == alignof(HGLOBAL));
        static_assert(sizeof(WindowsSystem::Handle)        == sizeof(HWND));
        static_assert(alignof(WindowsSystem::Handle)       == alignof(HWND));

        static_assert(std::is_same_v<ClipboardSystemAPI::CloseClipboardFunction,                decltype(&::CloseClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::EmptyClipboardFunction,                decltype(&::EmptyClipboard)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::GetClipboardDataFunction,              decltype(&::GetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::SetClipboardDataFunction,              decltype(&::SetClipboardData)>);
        static_assert(std::is_same_v<ClipboardSystemAPI::IsClipboardFormatAvailableFunction,    decltype(&::IsClipboardFormatAvailable)>);

        static_assert(std::is_same_v<FileSystemAPI::GetStandardHandleFunction,                  decltype(&::GetStdHandle)>);
        static_assert(std::is_same_v<FileSystemAPI::CreateFileWideFunction,                     decltype(&::CreateFileW)>);
        static_assert(std::is_same_v<FileSystemAPI::ReadFileFunction,                           decltype(&::ReadFile)>);
        static_assert(std::is_same_v<FileSystemAPI::WriteFileFunction,                          decltype(&::WriteFile)>);
        static_assert(std::is_same_v<FileSystemAPI::FlushFileBuffersFunction,                   decltype(&::FlushFileBuffers)>);
        static_assert(std::is_same_v<FileSystemAPI::LockFileFunction,                           decltype(&::LockFileEx)>);

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
            if (!EnsureSystemAPIs(api.getProcessId, api.processResourceUsage)) {
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
        static_assert(std::is_convertible_v<decltype(&::flock),         FileSystemAPI::LockFunction>);


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
        static_assert(PosixSystem::kOpenNonBlocking                               == O_NONBLOCK);
        static_assert(PosixSystem::kOpenCloseOnExec                               == O_CLOEXEC);
        static_assert(PosixSystem::kOpenReadOnly                                  == O_RDONLY);
        static_assert(PosixSystem::kOpenWriteOnly                                 == O_WRONLY);
        static_assert(PosixSystem::kOpenReadWrite                                 == O_RDWR);
        static_assert(PosixSystem::kOpenCreate                                    == O_CREAT);
        static_assert(PosixSystem::kOpenExclusive                                 == O_EXCL);
        static_assert(PosixSystem::kOpenTruncate                                  == O_TRUNC);
        static_assert(PosixSystem::kOpenDirectory                                 == O_DIRECTORY);
        static_assert(PosixSystem::kOpenDirect                                    == O_DIRECT);
        static_assert(PosixSystem::kOpenSync                                      == O_SYNC);
        static_assert(PosixSystem::kPageSizeConfiguration                         == _SC_PAGESIZE);
        static_assert(PosixSystem::kFileAdviceSequential                          == POSIX_FADV_SEQUENTIAL);
        static_assert(PosixSystem::kFileAdviceRandom                              == POSIX_FADV_RANDOM);
        static_assert(PosixSystem::kEventModify                                   == IN_MODIFY);
        static_assert(PosixSystem::kEventAttrib                                   == IN_ATTRIB);
        static_assert(PosixSystem::kEventCloseWrite                               == IN_CLOSE_WRITE);
        static_assert(PosixSystem::kEventMovedFrom                                == IN_MOVED_FROM);
        static_assert(PosixSystem::kEventMovedTo                                  == IN_MOVED_TO);
        static_assert(PosixSystem::kEventCreate                                   == IN_CREATE);
        static_assert(PosixSystem::kEventDelete                                   == IN_DELETE);
        static_assert(PosixSystem::kEventDeleteSelf                               == IN_DELETE_SELF);
        static_assert(PosixSystem::kEventMoveSelf                                 == IN_MOVE_SELF);
        static_assert(PosixSystem::kEventQueueOverflow                            == IN_Q_OVERFLOW);
        static_assert(PosixSystem::kEventIgnored                                  == IN_IGNORED);
        static_assert(PosixSystem::kEventIsDirectory                              == IN_ISDIR);
        static_assert(PosixSystem::kPollInput                                     == POLLIN);
        static_assert(PosixSystem::kPollError                                     == POLLERR);
        static_assert(PosixSystem::kPollHangup                                    == POLLHUP);
        static_assert(PosixSystem::kPollInvalid                                   == POLLNVAL);
#    elif defined(PLATFORM_MACOS)
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
                loaded.setUnhandledExceptionFilter = kernelModule->TryFindFunction<WindowsCrashNativeAPI::SetUnhandledExceptionFilterFunction>("SetUnhandledExceptionFilter");
                loaded.addVectoredExceptionHandler = kernelModule->TryFindFunction<WindowsCrashNativeAPI::AddVectoredExceptionHandlerFunction>("AddVectoredExceptionHandler");
                loaded.removeVectoredExceptionHandler = kernelModule->TryFindFunction<WindowsCrashNativeAPI::RemoveVectoredExceptionHandlerFunction>("RemoveVectoredExceptionHandler");
                loaded.raiseException              = kernelModule->TryFindFunction<WindowsCrashNativeAPI::RaiseExceptionFunction>("RaiseException");
                loaded.getErrorMode                = kernelModule->TryFindFunction<WindowsCrashNativeAPI::GetErrorModeFunction>("GetErrorMode");
                loaded.setErrorMode                = kernelModule->TryFindFunction<WindowsCrashNativeAPI::SetErrorModeFunction>("SetErrorMode");
                loaded.werGetFlags                 = kernelModule->TryFindFunction<WindowsCrashNativeAPI::WerGetFlagsFunction>("WerGetFlags");
                loaded.werSetFlags                 = kernelModule->TryFindFunction<WindowsCrashNativeAPI::WerSetFlagsFunction>("WerSetFlags");
                // clang-format on
                return loaded;
            }();
            return api;
        }

        inline constexpr WindowsSystem::DWord kSuppressedErrorModeFlags =
            SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
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
                .instructionPointer =
                    WindowsInstructionPointer(exception != nullptr ? exception->ContextRecord : nullptr),
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
            if (registration.addedWerFlags != 0 && registration.process != nullptr && api.werGetFlags != nullptr &&
                api.werSetFlags != nullptr) {
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
                if (registration.addedWerFlags != 0 &&
                    !WindowsSystem::Succeeded(api.werSetFlags(currentWerFlags | registration.addedWerFlags))) {
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
            WindowsCrashRegistration* registration =
                gNativeCrashRegistry.active.exchange(nullptr, std::memory_order_acq_rel);
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
            PosixCrashRegistration* registration =
                gNativeCrashRegistry.active.exchange(nullptr, std::memory_order_acq_rel);
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
                loaded.duplicateHandle =
                    module->TryFindFunction<HandleSystemAPI::DuplicateHandleFunction>("DuplicateHandle");
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
                loaded.waitForSingleObject = module->TryFindFunction<
                    SynchronizationSystemAPI::WaitForSingleObjectFunction>("WaitForSingleObject");
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
                loaded.createFileMappingWide = module->TryFindFunction<
                    FileMappingSystemAPI::CreateFileMappingWideFunction>("CreateFileMappingW");
                loaded.mapViewOfFile =
                    module->TryFindFunction<FileMappingSystemAPI::MapViewOfFileFunction>("MapViewOfFile");
                loaded.unmapViewOfFile =
                    module->TryFindFunction<FileMappingSystemAPI::UnmapViewOfFileFunction>("UnmapViewOfFile");
                loaded.flushViewOfFile =
                    module->TryFindFunction<FileMappingSystemAPI::FlushViewOfFileFunction>("FlushViewOfFile");
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                loaded.map = module->TryFindFunction<FileMappingSystemAPI::MapFunction>("mmap");
                loaded.unmap = module->TryFindFunction<FileMappingSystemAPI::UnmapFunction>("munmap");
                loaded.syncMap = module->TryFindFunction<FileMappingSystemAPI::SyncMapFunction>("msync");
                loaded.adviseMap = module->TryFindFunction<FileMappingSystemAPI::AdviseMapFunction>("madvise");
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
                loaded.createEventWide = module->TryFindFunction<EventSystemAPI::CreateEventWideFunction>("CreateEventW");
                loaded.setEvent = module->TryFindFunction<EventSystemAPI::SetEventFunction>("SetEvent");
                loaded.resetEvent = module->TryFindFunction<EventSystemAPI::ResetEventFunction>("ResetEvent");
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
                loaded.createIoCompletionPort = module->TryFindFunction<IOCPSystemAPI::CreateIoCompletionPortFunction>("CreateIoCompletionPort");
                loaded.getQueuedCompletionStatus = module->TryFindFunction<IOCPSystemAPI::GetQueuedCompletionStatusFunction>("GetQueuedCompletionStatus");
                loaded.postQueuedCompletionStatus = module->TryFindFunction<IOCPSystemAPI::PostQueuedCompletionStatusFunction>("PostQueuedCompletionStatus");
            }
#endif
            return loaded;
        }();
        return api;
    }

    const IORingSystemAPI& LoadIORingSystemAPI() noexcept {
        static const IORingSystemAPI api = [] {
            IORingSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
            if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                loaded.createIoRing = module->TryFindFunction<IORingSystemAPI::CreateIoRingFunction>("CreateIoRing");
                loaded.closeIoRing = module->TryFindFunction<IORingSystemAPI::CloseIoRingFunction>("CloseIoRing");
                loaded.submitIoRing = module->TryFindFunction<IORingSystemAPI::SubmitIoRingFunction>("SubmitIoRing");
                loaded.popIoRingCompletion = module->TryFindFunction<IORingSystemAPI::PopIoRingCompletionFunction>("PopIoRingCompletion");
            }
#elif defined(PLATFORM_LINUX)
            if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                loaded.queueInit = module->TryFindFunction<IORingSystemAPI::QueueInitFunction>("io_uring_queue_init");
                loaded.queueExit = module->TryFindFunction<IORingSystemAPI::QueueExitFunction>("io_uring_queue_exit");
                loaded.submit = module->TryFindFunction<IORingSystemAPI::SubmitFunction>("io_uring_submit");
                loaded.waitCqe = module->TryFindFunction<IORingSystemAPI::WaitCqeFunction>("io_uring_wait_cqe");
                loaded.peekCqe = module->TryFindFunction<IORingSystemAPI::PeekCqeFunction>("io_uring_peek_cqe");
                loaded.cqeSeen = module->TryFindFunction<IORingSystemAPI::CqeSeenFunction>("io_uring_cqe_seen");
            }
#endif
            return loaded;
        }();
        return api;
    }

    const SystemInformationSystemAPI& LoadSystemInformationSystemAPI() noexcept {
        static const SystemInformationSystemAPI api = [] {
            SystemInformationSystemAPI loaded{};
#if defined(PLATFORM_WINDOWS)
            if (const ModulePtr& module = Kernel32Module(); module != nullptr) {
                loaded.getSystemInfo =
                    module->TryFindFunction<SystemInformationSystemAPI::GetSystemInfoFunction>("GetSystemInfo");
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (const ModulePtr& module = CurrentProcessModule(); module != nullptr) {
                loaded.systemConfiguration = module->TryFindFunction<
                    SystemInformationSystemAPI::SystemConfigurationFunction>("sysconf");
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
                loaded.commandLineToArgvWide =
                    shell->TryFindFunction<ProcessSystemAPI::CommandLineToArgvWideFunction>("CommandLineToArgvW");
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const ModulePtr& module = CurrentProcessModule();
            if (module != nullptr) {
                // clang-format off
                loaded.getProcessId         = module->TryFindFunction<ProcessSystemAPI::GetProcessIdFunction>("getpid");
                loaded.getParentProcessId   = module->TryFindFunction<ProcessSystemAPI::GetParentProcessIdFunction>("getppid");
                loaded.getResourceUsage     = module->TryFindFunction<ProcessSystemAPI::GetResourceUsageFunction>("getrusage");
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
            const ModulePtr module = LoadWindowModule({"X11", "libX11.so.6"});
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
            const ModulePtr module = LoadWindowModule({"xcb", "libxcb.so.1"});
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
            const ModulePtr module = LoadWindowModule({"wayland-client", "libwayland-client.so.0"});
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
            loaded.getRegistry             = module->TryFindFunction<API::GetRegistryFunction>("wl_display_get_registry");
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
            if (api_.symGetLineFromAddress(process, nativeAddress, &displacement, &line) != FALSE &&
                line.FileName != nullptr) {
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
