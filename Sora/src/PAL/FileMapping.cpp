/**
 * @file FileMapping.cpp
 * @brief Implement granularity-correct Windows and POSIX mapped file ranges.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/FileMapping.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/Traits/EnumTraits.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <utility>

namespace Sora::PAL {

    namespace {

        [[nodiscard]] size_t MappingGranularity(const FileSystemAPI& api) noexcept {
#if defined(PLATFORM_WINDOWS)
            WindowsSystem::SystemInfo info{};
            if (EnsureSystemAPIs(api.getSystemInfo)) {
                api.getSystemInfo(&info);
            }
            return std::max<size_t>(info.allocationGranularity, 1);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const long size = EnsureSystemAPIs(api.systemConfiguration)
                                  ? api.systemConfiguration(PosixSystem::kPageSizeConfiguration)
                                  : -1;
            return size > 0 ? static_cast<size_t>(size) : 4096;
#else
            static_cast<void>(api);
            return 1;
#endif
        }

    } // namespace

    FileMapping::~FileMapping() {
        Reset();
    }

    FileMapping::FileMapping(FileMapping&& other) noexcept
        : mappingHandle_{std::exchange(other.mappingHandle_, nullptr)},
          base_{std::exchange(other.base_, nullptr)},
          data_{std::exchange(other.data_, nullptr)},
          mappedSize_{std::exchange(other.mappedSize_, 0)},
          size_{std::exchange(other.size_, 0)},
          offset_{std::exchange(other.offset_, 0)},
          file_{std::move(other.file_)},
          access_{std::exchange(other.access_, FileMappingAccess::Read)},
          active_{std::exchange(other.active_, false)} {}

    FileMapping& FileMapping::operator=(FileMapping&& other) noexcept {
        if (this != &other) {
            Reset();
            mappingHandle_ = std::exchange(other.mappingHandle_, nullptr);
            base_ = std::exchange(other.base_, nullptr);
            data_ = std::exchange(other.data_, nullptr);
            mappedSize_ = std::exchange(other.mappedSize_, 0);
            size_ = std::exchange(other.size_, 0);
            offset_ = std::exchange(other.offset_, 0);
            file_ = std::move(other.file_);
            access_ = std::exchange(other.access_, FileMappingAccess::Read);
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    Result<FileMapping> FileMapping::Map(const File& file, FileMappingOptions options) noexcept {
        // 1. Validate the requested semantic mode and exact exposed file range.
        if (!file || !Traits::IsValidEnumValue(options.access)) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        auto fileSize = file.Size();
        if (!fileSize || options.offset > *fileSize) {
            return std::unexpected{fileSize ? ErrorCode::OutOfRange : fileSize.error()};
        }
        const std::uint64_t remaining = *fileSize - options.offset;
        const std::uint64_t requested = options.size == 0 ? remaining : options.size;
        if (requested > remaining || requested > std::numeric_limits<size_t>::max()) {
            return std::unexpected{ErrorCode::OutOfRange};
        }

        FileMapping mapping;
        mapping.offset_ = options.offset;
        mapping.size_ = static_cast<size_t>(requested);
        mapping.access_ = options.access;
        if (requested == 0) {
            mapping.active_ = true;
            return mapping;
        }
        const auto& api = LoadFileSystemAPI();

        // 2. Duplicate the source file so mapping lifetime is independent of its caller.
#if defined(PLATFORM_WINDOWS)
        const ProcessSystemAPI& processAPI = LoadProcessSystemAPI();
        if (!EnsureSystemAPIs(processAPI.getCurrentProcess, api.duplicateHandle)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        const NativeFileHandle process = processAPI.getCurrentProcess();
        NativeFileHandle duplicate = kInvalidNativeFileHandle;
        if (api.duplicateHandle(process, file.handle_, process, &duplicate, 0, WindowsSystem::kFalse,
                                WindowsSystem::kDuplicateSameAccess) == WindowsSystem::kFalse) {
            return std::unexpected{ErrorCode::IoError};
        }
        mapping.file_ = File{duplicate, false, false, {}};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.duplicate)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        const NativeFileHandle duplicate = api.duplicate(file.handle_);
        if (duplicate < 0) {
            return std::unexpected{ErrorCode::IoError};
        }
        mapping.file_ = File{duplicate, false, false, {}};
#endif

        // 3. Expand the native view downward to allocation granularity while preserving the exact exposed subrange.
        const size_t granularity = MappingGranularity(api);
        const std::uint64_t alignedOffset = options.offset - options.offset % granularity;
        const size_t delta = static_cast<size_t>(options.offset - alignedOffset);
        if (requested > std::numeric_limits<size_t>::max() - delta) {
            return std::unexpected{ErrorCode::OutOfRange};
        }
        mapping.mappedSize_ = delta + static_cast<size_t>(requested);

        // 4. Create the platform mapping and expose only the caller-requested bytes.
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.createFileMappingWide, api.mapViewOfFile, api.unmapViewOfFile, api.closeHandle)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        const WindowsSystem::DWord protection = options.access == FileMappingAccess::Read ? WindowsSystem::kPageReadOnly
                                                : options.access == FileMappingAccess::ReadWrite
                                                    ? WindowsSystem::kPageReadWrite
                                                    : WindowsSystem::kPageWriteCopy;
        const WindowsSystem::DWord desiredAccess =
            options.access == FileMappingAccess::Read        ? WindowsSystem::kFileMapRead
            : options.access == FileMappingAccess::ReadWrite ? WindowsSystem::kFileMapWrite
                                                             : WindowsSystem::kFileMapCopy;
        mapping.mappingHandle_ = api.createFileMappingWide(mapping.file_.handle_, nullptr, protection, 0, 0, nullptr);
        if (mapping.mappingHandle_ == nullptr) {
            return std::unexpected{ErrorCode::IoError};
        }
        mapping.base_ = api.mapViewOfFile(mapping.mappingHandle_, desiredAccess,
                                          static_cast<WindowsSystem::DWord>(alignedOffset >> 32),
                                          static_cast<WindowsSystem::DWord>(alignedOffset), mapping.mappedSize_);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.map, api.unmap) ||
            alignedOffset > static_cast<std::uint64_t>(std::numeric_limits<PosixSystem::FileOffset>::max())) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        int protection = PosixSystem::kProtectionRead;
        if (options.access != FileMappingAccess::Read) {
            protection |= PosixSystem::kProtectionWrite;
        }
        const int flags =
            options.access == FileMappingAccess::CopyOnWrite ? PosixSystem::kMapPrivate : PosixSystem::kMapShared;
        mapping.base_ = api.map(nullptr, mapping.mappedSize_, protection, flags, mapping.file_.handle_,
                                static_cast<PosixSystem::FileOffset>(alignedOffset));
        if (PosixSystem::IsMapFailed(mapping.base_)) {
            mapping.base_ = nullptr;
        }
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
        if (mapping.base_ == nullptr) {
            mapping.Reset();
            return std::unexpected{ErrorCode::IoError};
        }
        mapping.data_ = static_cast<std::byte*>(mapping.base_) + delta;
        mapping.active_ = true;
        return mapping;
    }

    VoidResult FileMapping::Flush(size_t offset, size_t size) const noexcept {
        if (!*this || access_ != FileMappingAccess::ReadWrite || offset > size_) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        const size_t length = size == 0 ? size_ - offset : size;
        if (length > size_ - offset || length == 0) {
            return length == 0 ? VoidResult{} : VoidResult{std::unexpected{ErrorCode::OutOfRange}};
        }
        const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
        if (!EnsureSystemAPIs(api.flushViewOfFile)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        if (api.flushViewOfFile(data_ + offset, length) == WindowsSystem::kFalse) {
            return std::unexpected{ErrorCode::IoError};
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (!EnsureSystemAPIs(api.syncMap)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        int synchronized = 0;
        do {
            synchronized = api.syncMap(base_, mappedSize_, PosixSystem::kSynchronize);
        } while (synchronized != 0 && errno == EINTR);
        if (synchronized != 0) {
            return std::unexpected{ErrorCode::IoError};
        }
#else
        return std::unexpected{ErrorCode::NotSupported};
#endif
        return file_.Flush();
    }

    VoidResult FileMapping::Advise(FileMappingAdvice advice) const noexcept {
        if (!*this || base_ == nullptr || !Traits::IsValidEnumValue(advice)) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        const auto& api = LoadFileSystemAPI();
        if (!EnsureSystemAPIs(api.adviseMap)) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        constexpr int adviceValues[]{PosixSystem::kAdviceNormal, PosixSystem::kAdviceSequential,
                                     PosixSystem::kAdviceRandom, PosixSystem::kAdviceWillNeed,
                                     PosixSystem::kAdviceDontNeed};
        return api.adviseMap(base_, mappedSize_, adviceValues[static_cast<size_t>(advice)]) == 0
                   ? VoidResult{}
                   : VoidResult{std::unexpected{ErrorCode::IoError}};
#else
        static_cast<void>(advice);
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    void FileMapping::Reset() noexcept {
        if (active_ || base_ != nullptr || mappingHandle_ != nullptr || file_) {
            const auto& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
            if (base_ != nullptr && api.unmapViewOfFile != nullptr) {
                static_cast<void>(api.unmapViewOfFile(base_));
            }
            if (mappingHandle_ != nullptr && api.closeHandle != nullptr) {
                static_cast<void>(api.closeHandle(mappingHandle_));
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (base_ != nullptr && api.unmap != nullptr) {
                static_cast<void>(api.unmap(base_, mappedSize_));
            }
#endif
        }
        [[maybe_unused]] const VoidResult closed = file_.Close();
        mappingHandle_ = nullptr;
        base_ = nullptr;
        data_ = nullptr;
        mappedSize_ = 0;
        size_ = 0;
        offset_ = 0;
        access_ = FileMappingAccess::Read;
        active_ = false;
    }

} // namespace Sora::PAL
