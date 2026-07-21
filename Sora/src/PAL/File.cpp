/**
 * @file File.cpp
 * @brief Implement native file ownership, positional I/O, durability, and Direct I/O contracts.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/File.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/Traits/EnumTraits.h>

#include <algorithm>
#include <cerrno>
#include <limits>

namespace Sora::PAL {

    namespace {

        [[nodiscard]] bool IsValidFileHandle(NativeFileHandle handle) noexcept {
#if defined(PLATFORM_WINDOWS)
            return handle != nullptr && !WindowsSystem::IsInvalidHandle(handle);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            return handle >= 0;
#else
            static_cast<void>(handle);
            return false;
#endif
        }

        [[nodiscard]] DirectIORequirements QueryDirectRequirements(NativeFileHandle handle,
                                                                   const FileSystemAPI& api) noexcept {
#if defined(PLATFORM_WINDOWS)
            WindowsSystem::FileStorageInfo info{};
            if (EnsureSystemAPIs(api.getFileInformation)) {
                if (api.getFileInformation(handle, WindowsSystem::kFileStorageInformation, &info, sizeof(info)) !=
                    WindowsSystem::kFalse) {
                    const size_t logical = std::max<size_t>(info.logicalBytesPerSector, 1);
                    const size_t physical = std::max<size_t>(info.physicalBytesPerSectorForPerformance, logical);
                    return {.memoryAlignment = physical, .offsetAlignment = logical, .sizeAlignment = logical};
                }
            }
            return {.memoryAlignment = 4096, .offsetAlignment = 4096, .sizeAlignment = 4096};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            size_t blockSize = 0;
            if (EnsureSystemAPIs(api.queryFileBlockSize) && api.queryFileBlockSize(handle, &blockSize)) {
                const size_t block = std::max<size_t>(blockSize, 512);
                return {.memoryAlignment = block, .offsetAlignment = block, .sizeAlignment = block};
            }
            return {.memoryAlignment = 4096, .offsetAlignment = 4096, .sizeAlignment = 4096};
#else
            static_cast<void>(handle);
            static_cast<void>(api);
            return {};
#endif
        }

#if defined(PLATFORM_WINDOWS)
        class ThreadIOEvent {
        public:
            ThreadIOEvent() noexcept {
                const auto& api = LoadFileSystemAPI();
                if (EnsureSystemAPIs(api.createEventWide)) {
                    handle_ = api.createEventWide(nullptr, WindowsSystem::kFalse, WindowsSystem::kFalse, nullptr);
                }
            }

            ~ThreadIOEvent() {
                const auto& api = LoadFileSystemAPI();
                if (IsValidFileHandle(handle_) && EnsureSystemAPIs(api.closeHandle)) {
                    static_cast<void>(api.closeHandle(handle_));
                }
            }

            ThreadIOEvent(const ThreadIOEvent&) = delete;
            ThreadIOEvent& operator=(const ThreadIOEvent&) = delete;

            [[nodiscard]] NativeFileHandle Prepare() noexcept {
                const auto& api = LoadFileSystemAPI();
                if (IsValidFileHandle(handle_) && EnsureSystemAPIs(api.resetEvent) &&
                    api.resetEvent(handle_) != WindowsSystem::kFalse) {
                    return handle_;
                }
                return {};
            }

        private:
            NativeFileHandle handle_ = kInvalidNativeFileHandle;
        };

        [[nodiscard]] NativeFileHandle PrepareThreadIOEvent() noexcept {
            thread_local ThreadIOEvent event;
            return event.Prepare();
        }
#endif

    } // namespace

    bool DirectIORequirements::Accepts(const void* buffer, std::uint64_t offset, size_t size) const noexcept {
        if (memoryAlignment == 0 || offsetAlignment == 0 || sizeAlignment == 0) {
            return false;
        }
        if (buffer == nullptr && size != 0) {
            return false;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(buffer);
        return address % memoryAlignment == 0 && offset % offsetAlignment == 0 && size % sizeAlignment == 0;
    }

    BorrowedFile::operator bool() const noexcept {
        return IsValidFileHandle(handle_);
    }

    bool BorrowedFile::Write(std::span<const std::byte> bytes) const noexcept {
        if (!*this) {
            return false;
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.writeFile)) {
            return false;
        }

        // 1. Bound each native request to the Win32 transfer-count width.
        while (!bytes.empty()) {
            const size_t request = std::min<size_t>(bytes.size(), std::numeric_limits<WindowsSystem::DWord>::max());
            WindowsSystem::DWord written = 0;
            if (api.writeFile(handle_, bytes.data(), static_cast<WindowsSystem::DWord>(request), &written, nullptr) ==
                    WindowsSystem::kFalse ||
                written == 0) {
                return false;
            }
            bytes = bytes.subspan(written);
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.write)) {
            return false;
        }

        // 1. Complete partial writes and retry only interruptible failures.
        while (!bytes.empty()) {
            const size_t request =
                std::min<size_t>(bytes.size(), static_cast<size_t>(std::numeric_limits<Sora::ssize_t>::max()));
            const Sora::ssize_t written = api.write(handle_, bytes.data(), request);
            if (written > 0) {
                bytes = bytes.subspan(static_cast<size_t>(written));
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
#else
        static_cast<void>(bytes);
        return false;
#endif
        return true;
    }

    void BorrowedFile::Flush() const noexcept {
        if (!*this) {
            return;
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (EnsureSystemAPIs(api.flushFileBuffers)) {
            static_cast<void>(api.flushFileBuffers(handle_));
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (EnsureSystemAPIs(api.sync)) {
            while (api.sync(handle_) != 0 && errno == EINTR) {
            }
        }
#endif
    }

    File::~File() {
        [[maybe_unused]] const VoidResult closed = Close();
    }

    File::File(File&& other) noexcept
        : handle_{std::exchange(other.handle_, kInvalidNativeFileHandle)},
          direct_{std::exchange(other.direct_, false)},
          positional_{std::exchange(other.positional_, false)},
          directRequirements_{std::exchange(other.directRequirements_, {})} {}

    File& File::operator=(File&& other) noexcept {
        if (this != &other) {
            [[maybe_unused]] const VoidResult closed = Close();
            handle_ = std::exchange(other.handle_, kInvalidNativeFileHandle);
            direct_ = std::exchange(other.direct_, false);
            positional_ = std::exchange(other.positional_, false);
            directRequirements_ = std::exchange(other.directRequirements_, {});
        }
        return *this;
    }

    File::operator bool() const noexcept {
        return IsValidFileHandle(handle_);
    }

    Result<File> File::Open(const std::filesystem::path& path, FileOpenOptions options) noexcept {
        const bool truncates =
            options.creation == FileCreation::CreateAlways || options.creation == FileCreation::TruncateExisting;
        if (path.empty() || IsEmpty(options.access) || !IsValidFlagSet(options.access) ||
            !IsValidFlagSet(options.share) || !IsValidFlagSet(options.flags) ||
            !Traits::IsValidEnumValue(options.creation) || (truncates && !HasAny(options.access, FileAccess::Write)) ||
            HasAll(options.flags, FileOpenFlag::Sequential | FileOpenFlag::Random)) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        const FileSystemAPI& api = LoadFileSystemAPI();
        NativeFileHandle handle = kInvalidNativeFileHandle;
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.createFileWide, api.closeHandle)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        WindowsSystem::DWord access = 0;
        access |= HasAny(options.access, FileAccess::Read) ? WindowsSystem::kGenericRead : 0;
        access |= HasAny(options.access, FileAccess::Write) ? WindowsSystem::kGenericWrite : 0;
        WindowsSystem::DWord share = 0;
        share |= HasAny(options.share, FileShare::Read) ? WindowsSystem::kFileShareRead : 0;
        share |= HasAny(options.share, FileShare::Write) ? WindowsSystem::kFileShareWrite : 0;
        share |= HasAny(options.share, FileShare::Delete) ? WindowsSystem::kFileShareDelete : 0;
        WindowsSystem::DWord creation = WindowsSystem::kOpenExisting;
        switch (options.creation) {
            case FileCreation::OpenExisting:
                creation = WindowsSystem::kOpenExisting;
                break;
            case FileCreation::CreateNew:
                creation = WindowsSystem::kCreateNew;
                break;
            case FileCreation::OpenOrCreate:
                creation = WindowsSystem::kOpenAlways;
                break;
            case FileCreation::CreateAlways:
                creation = WindowsSystem::kCreateAlways;
                break;
            case FileCreation::TruncateExisting:
                creation = WindowsSystem::kTruncateExisting;
                break;
        }
        WindowsSystem::DWord flags = WindowsSystem::kFileAttributeNormal;
        flags |= HasAny(options.flags, FileOpenFlag::Direct) ? WindowsSystem::kFileFlagNoBuffering : 0;
        flags |= HasAny(options.flags, FileOpenFlag::WriteThrough) ? WindowsSystem::kFileFlagWriteThrough : 0;
        flags |= HasAny(options.flags, FileOpenFlag::Sequential) ? WindowsSystem::kFileFlagSequentialScan : 0;
        flags |= HasAny(options.flags, FileOpenFlag::Random) ? WindowsSystem::kFileFlagRandomAccess : 0;
        flags |= HasAny(options.flags, FileOpenFlag::DeleteOnClose) ? WindowsSystem::kFileFlagDeleteOnClose : 0;
        flags |= HasAny(options.flags, FileOpenFlag::Positional) ? WindowsSystem::kFileFlagOverlapped : 0;
        handle = api.createFileWide(path.c_str(), access, share, nullptr, creation, flags, nullptr);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.open, api.close)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        if ((options.permissions & ~std::uint32_t{07777}) != 0) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        constexpr FileShare kAllSharing = FileShare::Read | FileShare::Write | FileShare::Delete;
        if (options.share != kAllSharing) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        int flags = PosixSystem::kOpenCloseOnExec;
        if (HasAll(options.access, FileAccess::Read | FileAccess::Write)) {
            flags |= PosixSystem::kOpenReadWrite;
        } else {
            flags |=
                HasAny(options.access, FileAccess::Write) ? PosixSystem::kOpenWriteOnly : PosixSystem::kOpenReadOnly;
        }
        switch (options.creation) {
            case FileCreation::OpenExisting:
                break;
            case FileCreation::CreateNew:
                flags |= PosixSystem::kOpenCreate | PosixSystem::kOpenExclusive;
                break;
            case FileCreation::OpenOrCreate:
                flags |= PosixSystem::kOpenCreate;
                break;
            case FileCreation::CreateAlways:
                flags |= PosixSystem::kOpenCreate | PosixSystem::kOpenTruncate;
                break;
            case FileCreation::TruncateExisting:
                flags |= PosixSystem::kOpenTruncate;
                break;
        }
#    if defined(PLATFORM_LINUX)
        flags |= HasAny(options.flags, FileOpenFlag::Direct) ? PosixSystem::kOpenDirect : 0;
#    endif
        flags |= HasAny(options.flags, FileOpenFlag::WriteThrough) ? PosixSystem::kOpenSync : 0;
        handle = api.open(path.c_str(), flags, static_cast<PosixSystem::FileMode>(options.permissions));
        if (IsValidFileHandle(handle) && HasAny(options.flags, FileOpenFlag::DeleteOnClose)) {
            if (api.unlink == nullptr) {
                static_cast<void>(api.close(handle));
                return std::unexpected{ErrorCode::NotSupported};
            }
            if (api.unlink(path.c_str()) != 0) {
                static_cast<void>(api.close(handle));
                return std::unexpected{ErrorCode::IoError};
            }
        }
#    if defined(PLATFORM_MACOS)
        if (IsValidFileHandle(handle) && HasAny(options.flags, FileOpenFlag::Direct)) {
            if (api.control == nullptr || api.control(handle, PosixSystem::kNoCacheControl, 1) != 0) {
                static_cast<void>(api.close(handle));
                return std::unexpected{ErrorCode::NotSupported};
            }
        }
#    endif
#    if defined(PLATFORM_LINUX)
        if (IsValidFileHandle(handle) && api.adviseFile != nullptr) {
            if (HasAny(options.flags, FileOpenFlag::Sequential)) {
                static_cast<void>(api.adviseFile(handle, 0, 0, PosixSystem::kFileAdviceSequential));
            } else if (HasAny(options.flags, FileOpenFlag::Random)) {
                static_cast<void>(api.adviseFile(handle, 0, 0, PosixSystem::kFileAdviceRandom));
            }
        }
#    endif
#else
        static_cast<void>(path);
        static_cast<void>(options);
        static_cast<void>(api);
#endif
        if (!IsValidFileHandle(handle)) {
#if defined(PLATFORM_WINDOWS)
            const auto error = CaptureLastSystemError();
            if (error == WindowsSystem::kErrorFileExists || error == WindowsSystem::kErrorAlreadyExists) {
                return std::unexpected{ErrorCode::AlreadyExists};
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (errno == EEXIST) {
                return std::unexpected{ErrorCode::AlreadyExists};
            }
#endif
            return std::unexpected{ErrorCode::IoError};
        }
        const bool direct = HasAny(options.flags, FileOpenFlag::Direct);
        const bool positional = HasAny(options.flags, FileOpenFlag::Positional);
        const DirectIORequirements requirements =
            direct ? QueryDirectRequirements(handle, api) : DirectIORequirements{};
        return File{handle, direct, positional, requirements};
    }

    Result<size_t> File::ReadAt(std::span<std::byte> destination, std::uint64_t offset) const noexcept {
        // 1. Validate object state and the complete Direct I/O transfer contract.
        if (!*this || !ValidateTransfer(destination.data(), offset, destination.size())) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        if (destination.empty()) {
            return 0;
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!positional_ || !EnsureSystemAPIs(api.readFile, api.getOverlappedResult)) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        // 2. Issue one bounded positional request with a reusable thread-local completion event.
        WindowsSystem::Overlapped overlapped{};
        overlapped.offset = static_cast<WindowsSystem::DWord>(offset);
        overlapped.offsetHigh = static_cast<WindowsSystem::DWord>(offset >> 32);
        overlapped.event = PrepareThreadIOEvent();
        if (overlapped.event == nullptr) {
            return std::unexpected{ErrorCode::IoError};
        }
        const size_t request = std::min<size_t>(destination.size(), std::numeric_limits<WindowsSystem::DWord>::max());
        WindowsSystem::DWord read = 0;
        if (api.readFile(handle_, destination.data(), static_cast<WindowsSystem::DWord>(request), &read,
                         reinterpret_cast<WindowsSystem::NativeOverlapped*>(&overlapped)) == WindowsSystem::kFalse) {

            // 3. Complete pending I/O synchronously and normalize end-of-file to a zero-byte read.
            const auto error = CaptureLastSystemError();
            if (error == WindowsSystem::kErrorIoPending) {
                if (api.getOverlappedResult(handle_, reinterpret_cast<WindowsSystem::NativeOverlapped*>(&overlapped),
                                            &read, true) == WindowsSystem::kFalse) {
                    return CaptureLastSystemError() == WindowsSystem::kErrorHandleEof
                               ? Result<size_t>{0}
                               : Result<size_t>{std::unexpected{ErrorCode::IoError}};
                }
            } else if (error == WindowsSystem::kErrorHandleEof) {
                return 0;
            } else {
                return std::unexpected{ErrorCode::IoError};
            }
        }
        return static_cast<size_t>(read);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!positional_ || !EnsureSystemAPIs(api.readAt) ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<PosixSystem::FileOffset>::max())) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        // 2. Issue one bounded positional request and retry interrupted calls.
        const size_t request =
            std::min<size_t>(destination.size(), static_cast<size_t>(std::numeric_limits<Sora::ssize_t>::max()));
        Sora::ssize_t read = 0;
        do {
            read = api.readAt(handle_, destination.data(), request, static_cast<PosixSystem::FileOffset>(offset));
        } while (read < 0 && errno == EINTR);
        return read < 0 ? Result<size_t>{std::unexpected{ErrorCode::IoError}}
                        : Result<size_t>{static_cast<size_t>(read)};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    VoidResult File::ReadAllAt(std::span<std::byte> destination, std::uint64_t offset) const noexcept {
        // 1. Reject ranges whose final absolute offset cannot be represented.
        if (destination.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
            return std::unexpected{ErrorCode::OutOfRange};
        }

        // 2. Consume partial native reads until the destination is full or end-of-file is observed.
        while (!destination.empty()) {
            auto read = ReadAt(destination, offset);
            if (!read) {
                return std::unexpected{read.error()};
            }
            if (*read == 0) {
                return std::unexpected{ErrorCode::DataTruncated};
            }
            destination = destination.subspan(*read);
            offset += *read;
        }
        return {};
    }

    Result<size_t> File::WriteAt(std::span<const std::byte> source, std::uint64_t offset) const noexcept {
        // 1. Validate object state and the complete Direct I/O transfer contract.
        if (!*this || !ValidateTransfer(source.data(), offset, source.size())) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        if (source.empty()) {
            return 0;
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!positional_ || !EnsureSystemAPIs(api.writeFile, api.getOverlappedResult)) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        // 2. Issue one bounded positional request with a reusable thread-local completion event.
        WindowsSystem::Overlapped overlapped{};
        overlapped.offset = static_cast<WindowsSystem::DWord>(offset);
        overlapped.offsetHigh = static_cast<WindowsSystem::DWord>(offset >> 32);
        overlapped.event = PrepareThreadIOEvent();
        if (overlapped.event == nullptr) {
            return std::unexpected{ErrorCode::IoError};
        }
        const size_t request = std::min<size_t>(source.size(), std::numeric_limits<WindowsSystem::DWord>::max());
        WindowsSystem::DWord written = 0;
        if (api.writeFile(handle_, source.data(), static_cast<WindowsSystem::DWord>(request), &written,
                          reinterpret_cast<WindowsSystem::NativeOverlapped*>(&overlapped)) == WindowsSystem::kFalse) {

            // 3. Complete pending I/O synchronously and report any terminal native failure.
            const auto error = CaptureLastSystemError();
            if (error != WindowsSystem::kErrorIoPending ||
                api.getOverlappedResult(handle_, reinterpret_cast<WindowsSystem::NativeOverlapped*>(&overlapped),
                                        &written, true) == WindowsSystem::kFalse) {
                return std::unexpected{ErrorCode::IoError};
            }
        }
        return static_cast<size_t>(written);

#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!positional_ || !EnsureSystemAPIs(api.writeAt) ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<PosixSystem::FileOffset>::max())) {
            return std::unexpected{ErrorCode::NotSupported};
        }

        // 2. Issue one bounded positional request and retry interrupted calls.
        const size_t request =
            std::min<size_t>(source.size(), static_cast<size_t>(std::numeric_limits<Sora::ssize_t>::max()));
        Sora::ssize_t written = 0;
        do {
            written = api.writeAt(handle_, source.data(), request, static_cast<PosixSystem::FileOffset>(offset));
        } while (written < 0 && errno == EINTR);
        return written < 0 ? Result<size_t>{std::unexpected{ErrorCode::IoError}}
                           : Result<size_t>{static_cast<size_t>(written)};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    VoidResult File::WriteAllAt(std::span<const std::byte> source, std::uint64_t offset) const noexcept {
        // 1. Reject ranges whose final absolute offset cannot be represented.
        if (source.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
            return std::unexpected{ErrorCode::OutOfRange};
        }

        // 2. Consume partial native writes until the source is exhausted.
        while (!source.empty()) {
            auto written = WriteAt(source, offset);
            if (!written) {
                return std::unexpected{written.error()};
            }
            if (*written == 0) {
                return std::unexpected{ErrorCode::IoError};
            }
            source = source.subspan(*written);
            offset += *written;
        }
        return {};
    }

    Result<std::uint64_t> File::Size() const noexcept {
        if (!*this) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.getFileSize)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        WindowsSystem::LargeInteger size{};
        return api.getFileSize(handle_, reinterpret_cast<WindowsSystem::NativeLargeInteger*>(&size)) !=
                       WindowsSystem::kFalse
                   ? Result<std::uint64_t>{static_cast<std::uint64_t>(size.quadPart)}
                   : Result<std::uint64_t>{std::unexpected{ErrorCode::IoError}};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        std::uint64_t size = 0;
        if (!EnsureSystemAPIs(api.queryFileSize)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        return api.queryFileSize(handle_, &size) ? Result<std::uint64_t>{size}
                                                 : Result<std::uint64_t>{std::unexpected{ErrorCode::IoError}};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    VoidResult File::Resize(std::uint64_t size) const noexcept {
        if (!*this) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.setFileInformation)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected{ErrorCode::OutOfRange};
        }
        WindowsSystem::FileEndOfFileInformation info{};
        info.endOfFile.quadPart = static_cast<std::int64_t>(size);
        if (api.setFileInformation(handle_, WindowsSystem::kFileEndOfFileInformation, &info, sizeof(info)) ==
            WindowsSystem::kFalse) {
            return VoidResult{std::unexpected{ErrorCode::IoError}};
        }
        return VoidResult{};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.resize)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        if (size > static_cast<std::uint64_t>(std::numeric_limits<PosixSystem::FileOffset>::max())) {
            return std::unexpected{ErrorCode::OutOfRange};
        }
        if (api.resize(handle_, static_cast<PosixSystem::FileOffset>(size)) != 0) {
            return VoidResult{std::unexpected{ErrorCode::IoError}};
        }
        return VoidResult{};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    VoidResult File::Flush() const noexcept {
        if (!*this) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.flushFileBuffers)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        return api.flushFileBuffers(handle_) != WindowsSystem::kFalse ? VoidResult{}
                                                                      : VoidResult{std::unexpected{ErrorCode::IoError}};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#    if defined(PLATFORM_MACOS)
        // 1. Prefer Darwin's full-device synchronization when available.
        if (EnsureSystemAPIs(api.control)) {
            int result = 0;
            do {
                result = api.control(handle_, PosixSystem::kFullSyncControl);
            } while (result != 0 && errno == EINTR);
            if (result == 0) {
                return {};
            }
        }
#    endif

        // 2. Fall back to the portable descriptor synchronization primitive.
        if (!EnsureSystemAPIs(api.sync)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        int result = 0;
        do {
            result = api.sync(handle_);
        } while (result != 0 && errno == EINTR);
        return result == 0 ? VoidResult{} : VoidResult{std::unexpected{ErrorCode::IoError}};
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    VoidResult File::Close() noexcept {
        if (!*this) {
            return {};
        }
        const auto& api = LoadFileSystemAPI();
        bool closed = false;
#if defined(PLATFORM_WINDOWS)
        closed = EnsureSystemAPIs(api.closeHandle) && api.closeHandle(handle_) != WindowsSystem::kFalse;
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (EnsureSystemAPIs(api.close)) {
            closed = api.close(handle_) == 0;
        }
#endif

        // 1. Disarm ownership even when native close reports failure; retrying an indeterminate handle is unsafe.
        handle_ = kInvalidNativeFileHandle;
        direct_ = false;
        positional_ = false;
        directRequirements_ = {};
        return closed ? VoidResult{} : VoidResult{std::unexpected{ErrorCode::IoError}};
    }

    bool File::ValidateTransfer(const void* buffer, std::uint64_t offset, size_t size) const noexcept {
        return !direct_ || directRequirements_.Accepts(buffer, offset, size);
    }

    BorrowedFile NativeStandardErrorFile() noexcept {
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.getStandardHandle, api.writeFile, api.flushFileBuffers)) {
            return {};
        }
        return BorrowedFile{api.getStandardHandle(WindowsSystem::kStandardErrorHandle)};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.write, api.sync)) {
            return {};
        }
        return BorrowedFile{2};
#else
        return {};
#endif
    }

} // namespace Sora::PAL
