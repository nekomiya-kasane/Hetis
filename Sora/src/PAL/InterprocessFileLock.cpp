/**
 * @file InterprocessFileLock.cpp
 * @brief Implement crash-released exclusive file locking on Win32 and POSIX.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/InterprocessFileLock.h>

#include <Sora/Core/PAL/NativeError.h>
#include <Sora/Core/PAL/SystemAPI.h>

#include <cerrno>
#include <utility>

namespace Sora::PAL {

    Result<std::optional<InterprocessFileLock>>
    InterprocessFileLock::TryAcquire(const std::filesystem::path& path) noexcept {
#if defined(PLATFORM_WINDOWS)
        constexpr FileShare share = FileShare::Read | FileShare::Write;
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        constexpr FileShare share = FileShare::Read | FileShare::Write | FileShare::Delete;
#else
        constexpr FileShare share = FileShare::None;
#endif
        auto opened = File::Open(path, {
                                           .access = FileAccess::Read | FileAccess::Write,
                                           .creation = FileCreation::OpenOrCreate,
                                           .share = share,
                                           .flags = FileOpenFlag::None,
                                       });
        if (!opened) {
            return std::unexpected(opened.error());
        }

        const FileSystemAPI& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.lockFile)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        WindowsSystem::Overlapped range{};
        constexpr WindowsSystem::DWord flags =
            WindowsSystem::kLockFileExclusive | WindowsSystem::kLockFileFailImmediately;
        const bool acquired =
            api.lockFile(opened->handle_, flags, 0, 1, 0, reinterpret_cast<WindowsSystem::NativeOverlapped*>(&range)) !=
            WindowsSystem::kFalse;
        if (!acquired) {
            if (CaptureLastSystemError() == WindowsSystem::kErrorLockViolation) {
                return std::optional<InterprocessFileLock>{};
            }
            return std::unexpected(ErrorCode::IoError);
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.lock)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        int locked = 0;
        do {
            locked = api.lock(opened->handle_, PosixSystem::kLockExclusive | PosixSystem::kLockNonBlocking);
        } while (locked != 0 && errno == EINTR);
        if (locked != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return std::optional<InterprocessFileLock>{};
            }
            return std::unexpected(ErrorCode::IoError);
        }
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
        return std::optional<InterprocessFileLock>{InterprocessFileLock{std::move(*opened)}};
    }

} // namespace Sora::PAL
