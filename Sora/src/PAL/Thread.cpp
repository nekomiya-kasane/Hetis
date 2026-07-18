/**
 * @file Thread.cpp
 * @brief Implement operating-system thread metadata and current-thread introspection.
 * @ingroup PAL
 */

#include "Sora/Core/PAL/Thread.h"
#include "Sora/Core/PAL/SystemAPI.h"
#include "Sora/Core/Unicode.h"
#include "Sora/Platform.h"

#include <array>
#include <cerrno>
#include <limits>

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

    } // namespace

    Result<uint64_t> CurrentNativeThreadId() noexcept {
        constexpr Result<uint64_t> unsupported{std::unexpected(ErrorCode::NotSupported)};
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        return api.getCurrentThreadId ? Result<uint64_t>{static_cast<uint64_t>(api.getCurrentThreadId())} : unsupported;
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.systemCall == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const long threadId = api.systemCall(SYS_gettid);
        return threadId > 0 ? Result<uint64_t>{static_cast<uint64_t>(threadId)} : unsupported;
#elif defined(PLATFORM_MACOS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.pthreadThreadId == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        uint64_t threadId = 0;
        return api.pthreadThreadId(nullptr, &threadId) ? Result<uint64_t>{threadId} : unsupported;
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
        if (api.getCurrentThread == nullptr || api.setThreadDescription == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        } else if (auto nativeName = Unicode::Utf8ToWide(name); !nativeName) {
            return std::unexpected(ErrorCode::InvalidThreadName);
        } else if (!WindowsSystem::Succeeded(api.setThreadDescription(api.getCurrentThread(), nativeName->c_str()))) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return {};
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.pthreadSelf == nullptr || api.pthreadSetName == nullptr) {
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
        if (api.pthreadSetName == nullptr) {
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

    Result<std::string> CurrentThreadName() {
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.getCurrentThread == nullptr || api.getThreadDescription == nullptr || api.localFree == nullptr) {
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
        api.localFree(nativeName);

        if (!utf8) {
            return std::unexpected(ErrorCode::InvalidNativeThreadText);
        }
        return std::move(*utf8);
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.pthreadSelf == nullptr || api.pthreadGetName == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        std::array<char, 16> name{};
        if (api.pthreadGetName(api.pthreadSelf(), name.data(), name.size()) != 0) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return ValidateThreadName(name.data());
#elif defined(PLATFORM_MACOS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.pthreadSelf == nullptr || api.pthreadGetName == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        std::array<char, 64> name{};
        if (api.pthreadGetName(api.pthreadSelf(), name.data(), name.size()) != 0) {
            return std::unexpected(ErrorCode::ThreadNativeFailure);
        }
        return ValidateThreadName(name.data());
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<LogicalProcessorLocation> CurrentLogicalProcessor() noexcept {
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.getCurrentProcessorNumberEx == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        WindowsSystem::ProcessorNumber processor{};
        api.getCurrentProcessorNumberEx(&processor);
        return LogicalProcessorLocation{.group = processor.group, .index = processor.number};
#elif defined(PLATFORM_LINUX)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.scheduleGetCpu == nullptr) {
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

    Result<ThreadStackBounds> CurrentThreadStackBounds() noexcept {
#if defined(PLATFORM_WINDOWS)
        const ThreadSystemAPI& api = LoadThreadSystemAPI();
        if (api.getCurrentThreadStackLimits == nullptr) {
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
        if (api.pthreadSelf == nullptr || api.pthreadGetAttributes == nullptr ||
            api.pthreadDestroyAttributes == nullptr || api.pthreadGetStack == nullptr) {
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
        if (api.pthreadSelf == nullptr || api.pthreadGetStackAddress == nullptr || api.pthreadGetStackSize == nullptr) {
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
