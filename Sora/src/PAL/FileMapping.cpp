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
          api_{std::exchange(other.api_, nullptr)},
          access_{std::exchange(other.access_, FileMappingAccess::Read)} {}

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
            api_ = std::exchange(other.api_, nullptr);
            access_ = std::exchange(other.access_, FileMappingAccess::Read);
        }
        return *this;
    }

    Result<FileMapping> FileMapping::Map(const File& file, FileMappingOptions options) noexcept {
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
        mapping.api_ = file.api_;
        mapping.offset_ = options.offset;
        mapping.size_ = static_cast<size_t>(requested);
        mapping.access_ = options.access;
        if (requested == 0) {
            return mapping;
        }
#if defined(PLATFORM_WINDOWS)
        if (file.api_->getCurrentProcess == nullptr || file.api_->duplicateHandle == nullptr) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        const NativeFileHandle process = file.api_->getCurrentProcess();
        NativeFileHandle duplicate = kInvalidNativeFileHandle;
        if (file.api_->duplicateHandle(process, file.handle_, process, &duplicate, 0, WindowsSystem::kFalse,
                                       WindowsSystem::kDuplicateSameAccess) == WindowsSystem::kFalse) {
            return std::unexpected{ErrorCode::IoError};
        }
        mapping.file_ = File{duplicate, file.api_, false, false, {}};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (file.api_->duplicate == nullptr) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        const NativeFileHandle duplicate = file.api_->duplicate(file.handle_);
        if (duplicate < 0) {
            return std::unexpected{ErrorCode::IoError};
        }
        mapping.file_ = File{duplicate, file.api_, false, false, {}};
#endif
        const size_t granularity = MappingGranularity(*file.api_);
        const std::uint64_t alignedOffset = options.offset - options.offset % granularity;
        const size_t delta = static_cast<size_t>(options.offset - alignedOffset);
        if (requested > std::numeric_limits<size_t>::max() - delta) {
            return std::unexpected{ErrorCode::OutOfRange};
        }
        mapping.mappedSize_ = delta + static_cast<size_t>(requested);

#if defined(PLATFORM_WINDOWS)
        if (file.api_->createFileMappingWide == nullptr || file.api_->mapViewOfFile == nullptr ||
            file.api_->unmapViewOfFile == nullptr || file.api_->closeHandle == nullptr) {
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
        mapping.mappingHandle_ =
            file.api_->createFileMappingWide(mapping.file_.handle_, nullptr, protection, 0, 0, nullptr);
        if (mapping.mappingHandle_ == nullptr) {
            return std::unexpected{ErrorCode::IoError};
        }
        mapping.base_ = file.api_->mapViewOfFile(mapping.mappingHandle_, desiredAccess,
                                                 static_cast<WindowsSystem::DWord>(alignedOffset >> 32),
                                                 static_cast<WindowsSystem::DWord>(alignedOffset), mapping.mappedSize_);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (file.api_->map == nullptr || file.api_->unmap == nullptr ||
            alignedOffset > static_cast<std::uint64_t>(std::numeric_limits<PosixSystem::FileOffset>::max())) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        int protection = PosixSystem::kProtectionRead;
        if (options.access != FileMappingAccess::Read) {
            protection |= PosixSystem::kProtectionWrite;
        }
        const int flags =
            options.access == FileMappingAccess::CopyOnWrite ? PosixSystem::kMapPrivate : PosixSystem::kMapShared;
        mapping.base_ = file.api_->map(nullptr, mapping.mappedSize_, protection, flags, mapping.file_.handle_,
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
#if defined(PLATFORM_WINDOWS)
        if (api_->flushViewOfFile == nullptr ||
            api_->flushViewOfFile(data_ + offset, length) == WindowsSystem::kFalse) {
            return std::unexpected{ErrorCode::IoError};
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (api_->syncMap == nullptr) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        int synchronized = 0;
        do {
            synchronized = api_->syncMap(base_, mappedSize_, PosixSystem::kSynchronize);
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
        if (api_->adviseMap == nullptr) {
            return std::unexpected{ErrorCode::NotSupported};
        }
        constexpr int adviceValues[]{PosixSystem::kAdviceNormal, PosixSystem::kAdviceSequential,
                                     PosixSystem::kAdviceRandom, PosixSystem::kAdviceWillNeed,
                                     PosixSystem::kAdviceDontNeed};
        return api_->adviseMap(base_, mappedSize_, adviceValues[static_cast<size_t>(advice)]) == 0
                   ? VoidResult{}
                   : VoidResult{std::unexpected{ErrorCode::IoError}};
#else
        static_cast<void>(advice);
        return std::unexpected{ErrorCode::NotSupported};
#endif
    }

    void FileMapping::Reset() noexcept {
        if (api_ != nullptr) {
#if defined(PLATFORM_WINDOWS)
            if (base_ != nullptr && api_->unmapViewOfFile != nullptr) {
                static_cast<void>(api_->unmapViewOfFile(base_));
            }
            if (mappingHandle_ != nullptr && api_->closeHandle != nullptr) {
                static_cast<void>(api_->closeHandle(mappingHandle_));
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (base_ != nullptr && api_->unmap != nullptr) {
                static_cast<void>(api_->unmap(base_, mappedSize_));
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
        api_ = nullptr;
        access_ = FileMappingAccess::Read;
    }

} // namespace Sora::PAL
