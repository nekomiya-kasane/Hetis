/**
 * @file GlobalMemory.cpp
 * @brief Implement ownership and scoped access for platform movable global memory.
 * @ingroup PAL
 */

#include <Sora/Core/Assertion.h>
#include <Sora/Core/Assertion.h>
#include <Sora/Core/PAL/GlobalMemory.h>
#include <Sora/Core/Traits/EnumTraits.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Platform.h>

#include <utility>

namespace Sora::PAL {

    namespace {

#ifdef PLATFORM_WINDOWS
        [[nodiscard]] WindowsSystem::GlobalMemory ToNative(GlobalMemoryHandle handle) noexcept {
            return static_cast<WindowsSystem::GlobalMemory>(handle);
        }

        [[nodiscard]] GlobalMemoryHandle FromNative(WindowsSystem::GlobalMemory handle) noexcept {
            return static_cast<GlobalMemoryHandle>(handle);
        }
#endif

    } // namespace

    GlobalMemoryLock::~GlobalMemoryLock() {
        UnlockIgnoringError();
    }

    GlobalMemoryLock::GlobalMemoryLock(GlobalMemoryLock&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)},
          data_{std::exchange(other.data_, nullptr)},
          size_{std::exchange(other.size_, 0)} {}

    GlobalMemoryLock& GlobalMemoryLock::operator=(GlobalMemoryLock&& other) noexcept {
        if (this != &other) {
            UnlockIgnoringError();
            handle_ = std::exchange(other.handle_, nullptr);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    VoidResult GlobalMemoryLock::Unlock() noexcept {
        if (data_ == nullptr) {
            return {};
        }
#ifdef PLATFORM_WINDOWS
        const GlobalMemorySystemAPI& memory = LoadGlobalMemorySystemAPI();
        const NativeErrorSystemAPI& error = LoadNativeErrorSystemAPI();
        if (!EnsureSystemAPIs(memory.globalUnlock, error.setLastError, error.getLastError)) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        error.setLastError(WindowsSystem::kErrorSuccess);
        if (memory.globalUnlock(ToNative(handle_)) == 0 && error.getLastError() != WindowsSystem::kErrorSuccess) {
            return std::unexpected{ErrorCode::GlobalMemoryNativeFailure};
        }
        handle_ = nullptr;
        data_ = nullptr;
        size_ = 0;
        return {};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    void GlobalMemoryLock::Swap(GlobalMemoryLock& other) noexcept {
        std::swap(handle_, other.handle_);
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
    }

    void GlobalMemoryLock::UnlockIgnoringError() noexcept {
        if (data_ == nullptr) {
            return;
        }
#ifdef PLATFORM_WINDOWS
        if (const GlobalMemorySystemAPI& memory = LoadGlobalMemorySystemAPI(); memory.globalUnlock != nullptr) {
            memory.globalUnlock(ToNative(handle_));
        }
#endif
        handle_ = nullptr;
        data_ = nullptr;
        size_ = 0;
    }

    Result<size_t> GlobalMemoryView::Size() const noexcept {
        if (handle_ == nullptr) {
            return std::unexpected{ErrorCode::InvalidState};
        }
#ifdef PLATFORM_WINDOWS
        const GlobalMemorySystemAPI& memory = LoadGlobalMemorySystemAPI();
        const NativeErrorSystemAPI& error = LoadNativeErrorSystemAPI();
        if (!EnsureSystemAPIs(memory.globalSize, error.setLastError, error.getLastError)) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        error.setLastError(WindowsSystem::kErrorSuccess);
        const WindowsSystem::Size size = memory.globalSize(ToNative(handle_));
        if (size == 0 && error.getLastError() != WindowsSystem::kErrorSuccess) {
            return std::unexpected{ErrorCode::GlobalMemoryNativeFailure};
        }
        return static_cast<size_t>(size);
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    Result<GlobalMemoryLock> GlobalMemoryView::Lock() const noexcept {
        if (handle_ == nullptr) {
            return std::unexpected{ErrorCode::InvalidState};
        }
#ifdef PLATFORM_WINDOWS
        const GlobalMemorySystemAPI& memory = LoadGlobalMemorySystemAPI();
        if (!EnsureSystemAPIs(memory.globalLock, memory.globalUnlock)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        auto size = Size();
        if (!size) {
            return std::unexpected{size.error()};
        }
        if (*size == 0) {
            return std::unexpected{ErrorCode::InvalidState};
        }

        void* data = memory.globalLock(ToNative(handle_));
        if (data == nullptr) {
            return std::unexpected{ErrorCode::GlobalMemoryNativeFailure};
        }
        return GlobalMemoryLock{handle_, static_cast<std::byte*>(data), *size};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    OwnedGlobalMemory::~OwnedGlobalMemory() {
        if (!Verify(Reset().has_value(), "OwnedGlobalMemory destruction failed to release its native block.")) {
            return;
        }
    }

    OwnedGlobalMemory::OwnedGlobalMemory(OwnedGlobalMemory&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)} {}

    OwnedGlobalMemory& OwnedGlobalMemory::operator=(OwnedGlobalMemory&& other) noexcept {
        if (this != &other) {
            if (!Verify(Reset().has_value(), "OwnedGlobalMemory replacement failed to release its current block.")) {
                return *this;
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Result<OwnedGlobalMemory> OwnedGlobalMemory::Allocate(size_t size,
                                                          GlobalMemoryInitialization initialization) noexcept {
        if (!Traits::IsValidEnumValue(initialization)) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        if (size == 0) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }

#ifdef PLATFORM_WINDOWS
        const GlobalMemorySystemAPI& memory = LoadGlobalMemorySystemAPI();
        if (!EnsureSystemAPIs(memory.globalAllocate, memory.globalFree)) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        WindowsSystem::UInt flags = WindowsSystem::kMovableGlobalMemory;
        if (initialization == GlobalMemoryInitialization::Zeroed) {
            flags |= WindowsSystem::kZeroInitializeGlobalMemory;
        }
        WindowsSystem::GlobalMemory handle = memory.globalAllocate(flags, static_cast<WindowsSystem::Size>(size));
        if (handle == nullptr) {
            return std::unexpected{ErrorCode::OutOfMemory};
        }
        return OwnedGlobalMemory{FromNative(handle)};
#else
        static_cast<void>(initialization);
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    GlobalMemoryHandle OwnedGlobalMemory::Release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    VoidResult OwnedGlobalMemory::Reset() noexcept {
        if (!handle_) {
            return {};
        }

#ifdef PLATFORM_WINDOWS
        const GlobalMemorySystemAPI& memory = LoadGlobalMemorySystemAPI();
        if (!EnsureSystemAPIs(memory.globalFree)) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        if (auto failed = memory.globalFree(ToNative(handle_)); failed) {
            handle_ = FromNative(failed);
            return std::unexpected{ErrorCode::GlobalMemoryNativeFailure};
        }
        handle_ = nullptr;
        return {};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    void OwnedGlobalMemory::Swap(OwnedGlobalMemory& other) noexcept {
        std::swap(handle_, other.handle_);
    }

} // namespace Sora::PAL
