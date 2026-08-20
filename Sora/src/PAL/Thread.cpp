/**
 * @file Thread.cpp
 * @brief Implement operating-system thread metadata and current-thread introspection.
 * @ingroup PAL
 */

#include "Sora/Core/PAL/Thread.h"
#include "Sora/Core/Guard.h"
#include "Sora/Core/PAL/SystemAPI.h"
#include "Sora/Core/Unicode.h"
#include "Sora/Platform.h"

#include <array>
#include <cerrno>
#include <limits>
#include <string>

namespace Sora::PAL {

    namespace {

        [[nodiscard]] Result<void> ValidateThreadName(std::string_view name) {
            if (name.contains('\0') || !Unicode::ValidateUtf8(name)) {
                return std::unexpected(ErrorCode::InvalidThreadName);
            }
            if (name.size() > Traits::kMaxThreadNameLength) {
                return std::unexpected(ErrorCode::ThreadNameTooLong);
            }
            return {};
        }

#if defined(PLATFORM_WINDOWS)
        [[nodiscard]] ErrorCode ClassifyThreadLookupFailure() noexcept {
            const auto error = CaptureLastSystemError();
            if (error == WindowsSystem::kErrorAccessDenied) {
                return ErrorCode::PermissionDenied;
            }
            if (error == WindowsSystem::kErrorInvalidParameter) {
                return ErrorCode::ResourceNotFound;
            }
            return ErrorCode::ThreadNativeFailure;
        }

        [[nodiscard]] Result<WindowsSystem::Handle> OpenThread(NativeThreadId threadId,
                                                                WindowsSystem::DWord access) noexcept {
            if (threadId.Value() == 0 || threadId.Value() > std::numeric_limits<WindowsSystem::DWord>::max()) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            const auto& api = LoadThreadSystemAPI();
            if (!EnsureSystemAPIs(api.openThread, LoadHandleSystemAPI().closeHandle)) {
                return std::unexpected(ErrorCode::NotSupported);
            }
            const auto handle = api.openThread(access, WindowsSystem::kFalse,
                                               static_cast<WindowsSystem::DWord>(threadId.Value()));
            if (handle == nullptr || WindowsSystem::IsInvalidHandle(handle)) {
                return std::unexpected(ClassifyThreadLookupFailure());
            }
            return handle;
        }
#elif defined(PLATFORM_LINUX)
        [[nodiscard]] std::string ThreadCommPath(NativeThreadId threadId) {
            return "/proc/self/task/" + std::to_string(threadId.Value()) + "/comm";
        }

        [[nodiscard]] ErrorCode ClassifyThreadErrno(bool liveness) noexcept {
            if (errno == ENOENT || errno == ESRCH) {
                return liveness ? ErrorCode::ResourceNotFound : ErrorCode::ResourceNotFound;
            }
            if (errno == EACCES || errno == EPERM) {
                return ErrorCode::PermissionDenied;
            }
            return ErrorCode::ThreadNativeFailure;
        }

        [[nodiscard]] int OpenThreadComm(NativeThreadId threadId, int flags) noexcept {
            const auto& api = LoadFileSystemAPI();
            if (!EnsureSystemAPIs(api.open)) {
                errno = ENOSYS;
                return -1;
            }
            const std::string path = ThreadCommPath(threadId);
            int descriptor = -1;
            do {
                descriptor = api.open(path.c_str(), flags | PosixSystem::kOpenCloseOnExec);
            } while (descriptor < 0 && errno == EINTR);
            return descriptor;
        }
#endif

    } // namespace

    Result<NativeThreadId> GetCurrentNativeThreadId() noexcept {
        constexpr Result<NativeThreadId> unsupported{std::unexpected(ErrorCode::NotSupported)};
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.getCurrentThreadId)) {
            return unsupported;
        }
        const uint64_t id = api.getCurrentThreadId();
        return id != 0 ? Result<NativeThreadId>{NativeThreadId{id}} : unsupported;
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.getThreadId)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int threadId = api.getThreadId();
        return threadId > 0 ? Result<NativeThreadId>{NativeThreadId{static_cast<uint64_t>(threadId)}} : unsupported;
#elif defined(PLATFORM_MACOS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.pthreadThreadId)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        uint64_t threadId = 0;
        return api.pthreadThreadId(nullptr, &threadId) == 0 && threadId != 0
                   ? Result<NativeThreadId>{NativeThreadId{threadId}}
                   : unsupported;
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<void> SetCurrentThreadName(std::string_view name) {
        if (auto valid = ValidateThreadName(name); !valid) {
            return std::unexpected(valid.error());
        }

#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.getCurrentThread, api.setThreadDescription)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        if (auto nativeName = Unicode::Utf8ToWide(name); !nativeName) {
            return std::unexpected(ErrorCode::InvalidThreadName);
        } else if (!WindowsSystem::Succeeded(api.setThreadDescription(api.getCurrentThread(), nativeName->c_str()))) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return {};
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.pthreadSelf, api.pthreadSetName)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const std::string ownedName{name};
        const int result = api.pthreadSetName(api.pthreadSelf(), ownedName.c_str());
        if (result == ERANGE) {
            return std::unexpected(ErrorCode::ThreadNameTooLong);
        }
        return result == 0 ? Result<void>{} : Result<void>{std::unexpected(ErrorCode::ThreadNativeFailure)};
#elif defined(PLATFORM_MACOS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.pthreadSetName)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const std::string ownedName{name};
        const int result = api.pthreadSetName(ownedName.c_str());
        if (result == ERANGE) {
            return std::unexpected(ErrorCode::ThreadNameTooLong);
        }
        return result == 0 ? Result<void>{} : Result<void>{std::unexpected(ErrorCode::ThreadNativeFailure)};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<std::string> GetCurrentThreadName() {
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        const LocalMemorySystemAPI& memoryAPI = LoadLocalMemorySystemAPI();
        if (!EnsureSystemAPIs(api.getCurrentThread, api.getThreadDescription, memoryAPI.localFree)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        wchar_t* nativeName = nullptr;
        if (!WindowsSystem::Succeeded(api.getThreadDescription(api.getCurrentThread(), &nativeName))) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        if (nativeName == nullptr) {
            return std::string{};
        }
        auto utf8 = Unicode::WideToUtf8(std::wstring_view{nativeName});
        memoryAPI.localFree(nativeName);

        if (!utf8) {
            return std::unexpected(ErrorCode::InvalidNativeThreadText);
        }
        return std::move(*utf8);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.pthreadSelf, api.pthreadGetName)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        std::array<char, Sora::kIsLinux ? 16 : 64> name{};
        if (api.pthreadGetName(api.pthreadSelf(), name.data(), name.size()) != 0) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        std::string result{name.data()};
        if (auto valid = ValidateThreadName(result); !valid) {
            return std::unexpected(valid.error());
        }
        return result;
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<void> SetThreadName(NativeThreadId threadId, std::string_view name) {
        if (threadId.Value() == 0) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        if (auto valid = ValidateThreadName(name); !valid) {
            return std::unexpected(valid.error());
        }
        if (const auto current = GetCurrentNativeThreadId(); current && *current == threadId) {
            return SetCurrentThreadName(name);
        }
#if defined(PLATFORM_WINDOWS)
        auto handle = OpenThread(threadId, WindowsSystem::kThreadSetLimitedInformation);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        const auto& handles = LoadHandleSystemAPI();
        ScopeExit close{[&handles, value = *handle] noexcept { handles.closeHandle(value); }};
        const auto& threads = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(threads.setThreadDescription)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        auto nativeName = Unicode::Utf8ToWide(name);
        if (!nativeName) {
            return std::unexpected(ErrorCode::InvalidThreadName);
        }
        return WindowsSystem::Succeeded(threads.setThreadDescription(*handle, nativeName->c_str()))
                   ? Result<void>{}
                   : Result<void>{std::unexpected(ErrorCode::ThreadNativeFailure)};
#elif defined(PLATFORM_LINUX)
        const auto& api = LoadFileSystemAPI();
        if (!EnsureSystemAPIs(api.write, api.close)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int descriptor = OpenThreadComm(threadId, PosixSystem::kOpenWriteOnly);
        if (descriptor < 0) {
            return std::unexpected(ClassifyThreadErrno(false));
        }
        ScopeExit close{[&api, descriptor] noexcept { static_cast<void>(api.close(descriptor)); }};
        Sora::ssize_t written = -1;
        do {
            written = api.write(descriptor, name.data(), name.size());
        } while (written < 0 && errno == EINTR);
        return written == static_cast<Sora::ssize_t>(name.size())
                   ? Result<void>{}
                   : Result<void>{std::unexpected(ClassifyThreadErrno(false))};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<std::string> GetThreadName(NativeThreadId threadId) {
        if (threadId.Value() == 0) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        if (const auto current = GetCurrentNativeThreadId(); current && *current == threadId) {
            return GetCurrentThreadName();
        }
#if defined(PLATFORM_WINDOWS)
        auto handle = OpenThread(threadId, WindowsSystem::kThreadQueryLimitedInformation);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        const auto& handles = LoadHandleSystemAPI();
        ScopeExit close{[&handles, value = *handle] noexcept { handles.closeHandle(value); }};
        const auto& threads = LoadThreadSystemAPI();
        const auto& memory = LoadLocalMemorySystemAPI();
        if (!EnsureSystemAPIs(threads.getThreadDescription, memory.localFree)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        wchar_t* nativeName = nullptr;
        if (!WindowsSystem::Succeeded(threads.getThreadDescription(*handle, &nativeName))) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        if (nativeName == nullptr) {
            return std::string{};
        }
        ScopeExit release{[&memory, nativeName] noexcept { memory.localFree(nativeName); }};
        auto utf8 = Unicode::WideToUtf8(std::wstring_view{nativeName});
        return utf8 ? Result<std::string>{std::move(*utf8)}
                    : Result<std::string>{std::unexpected(ErrorCode::InvalidNativeThreadText)};
#elif defined(PLATFORM_LINUX)
        const auto& api = LoadFileSystemAPI();
        if (!EnsureSystemAPIs(api.read, api.close)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int descriptor = OpenThreadComm(threadId, PosixSystem::kOpenReadOnly);
        if (descriptor < 0) {
            return std::unexpected(ClassifyThreadErrno(false));
        }
        ScopeExit close{[&api, descriptor] noexcept { static_cast<void>(api.close(descriptor)); }};
        std::array<char, 17> buffer{};
        Sora::ssize_t size = -1;
        do {
            size = api.read(descriptor, buffer.data(), buffer.size() - 1);
        } while (size < 0 && errno == EINTR);
        if (size < 0) {
            return std::unexpected(ClassifyThreadErrno(false));
        }
        std::string result{buffer.data(), static_cast<size_t>(size)};
        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
        return result;
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<bool> IsThreadAlive(NativeThreadId threadId) noexcept {
        if (threadId.Value() == 0) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
#if defined(PLATFORM_WINDOWS)
        auto handle = OpenThread(threadId, WindowsSystem::kSynchronizeAccess);
        if (!handle) {
            return handle.error() == ErrorCode::ResourceNotFound
                       ? Result<bool>{false}
                       : Result<bool>{std::unexpected(handle.error())};
        }
        const auto& handles = LoadHandleSystemAPI();
        ScopeExit close{[&handles, value = *handle] noexcept { handles.closeHandle(value); }};
        const auto& synchronization = LoadSynchronizationSystemAPI();
        if (!EnsureSystemAPIs(synchronization.waitForSingleObject)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const auto waited = synchronization.waitForSingleObject(*handle, 0);
        if (waited == WindowsSystem::kWaitTimeout) {
            return true;
        }
        if (waited == WindowsSystem::kWaitObject) {
            return false;
        }
        return std::unexpected(ErrorCode::ThreadNativeFailure);
#elif defined(PLATFORM_LINUX)
        const auto& api = LoadFileSystemAPI();
        if (!EnsureSystemAPIs(api.close)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int descriptor = OpenThreadComm(threadId, PosixSystem::kOpenReadOnly);
        if (descriptor >= 0) {
            static_cast<void>(api.close(descriptor));
            return true;
        }
        if (errno == ENOENT || errno == ESRCH) {
            return false;
        }
        return std::unexpected(ClassifyThreadErrno(true));
#elif defined(PLATFORM_MACOS)
        const auto current = GetCurrentNativeThreadId();
        return current && *current == threadId ? Result<bool>{true}
                                               : Result<bool>{std::unexpected(ErrorCode::NotSupported)};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<LogicalProcessorLocation> GetCurrentLogicalProcessor() noexcept {
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.getCurrentProcessorNumberEx)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        WindowsSystem::ProcessorNumber processor{};
        api.getCurrentProcessorNumberEx(&processor);
        return LogicalProcessorLocation{.group = processor.group, .index = processor.number};
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.scheduleGetCpu)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int processor = api.scheduleGetCpu();
        if (processor < 0) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return LogicalProcessorLocation{.group = 0, .index = static_cast<uint32_t>(processor)};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<ThreadStackBounds> GetCurrentThreadStackBounds() noexcept {
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.getCurrentThreadStackLimits)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        uintptr_t lower = 0, upper = 0;
        api.getCurrentThreadStackLimits(&lower, &upper);
        if (lower >= upper) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return ThreadStackBounds{.lower = static_cast<uintptr_t>(lower), .upper = static_cast<uintptr_t>(upper)};
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.pthreadSelf, api.pthreadGetAttributes, api.pthreadDestroyAttributes,
                              api.pthreadGetStack)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        pthread_attr_t attributes{};
        auto* opaqueAttributes = reinterpret_cast<PosixSystem::ThreadAttributes*>(&attributes);
        if (api.pthreadGetAttributes(api.pthreadSelf(), opaqueAttributes) != 0) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        struct AttributeGuard {
            PosixSystem::ThreadAttributes* value;
            ThreadSystemAPI::PthreadDestroyAttributesFunction destroy;
            ~AttributeGuard() { destroy(value); }
        } guard{opaqueAttributes, api.pthreadDestroyAttributes};

        void* stackAddress = nullptr;
        size_t stackSize = 0;
        if (api.pthreadGetStack(opaqueAttributes, &stackAddress, &stackSize) != 0 || stackAddress == nullptr) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        const uintptr_t lower = reinterpret_cast<uintptr_t>(stackAddress);
        if (stackSize == 0 || stackSize > std::numeric_limits<uintptr_t>::max() - lower) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return ThreadStackBounds{.lower = lower, .upper = lower + stackSize};
#elif defined(PLATFORM_MACOS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (!EnsureSystemAPIs(api.pthreadSelf, api.pthreadGetStackAddress, api.pthreadGetStackSize)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const auto thread = api.pthreadSelf();
        const uintptr_t upper = reinterpret_cast<uintptr_t>(api.pthreadGetStackAddress(thread));
        const size_t stackSize = api.pthreadGetStackSize(thread);
        if (upper == 0 || stackSize == 0 || stackSize > upper) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return ThreadStackBounds{.lower = upper - stackSize, .upper = upper};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

} // namespace Sora::PAL
