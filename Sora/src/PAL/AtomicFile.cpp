/**
 * @file AtomicFile.cpp
 * @brief Implement durable same-directory temporary-file publication.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/AtomicFile.h>
#include <Sora/Core/PAL/Process.h>
#include <Sora/Core/PAL/SystemAPI.h>

#include <atomic>
#include <cerrno>
#include <format>
#include <new>
#include <utility>

namespace Sora::PAL {

    namespace {

        std::atomic<std::uint64_t> gTemporarySequence = 0;

        [[nodiscard]] std::uint64_t ProcessDiscriminator() noexcept {
            const auto process = CurrentProcessId();
            return process.value_or(0);
        }

        void DeleteNativeFile(const std::filesystem::path& path) noexcept {
            if (path.empty()) {
                return;
            }
            const FileSystemAPI& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
            if (api.deleteFileWide != nullptr) {
                static_cast<void>(api.deleteFileWide(path.c_str()));
            }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (api.unlink != nullptr) {
                static_cast<void>(api.unlink(path.c_str()));
            }
#endif
        }

        [[nodiscard]] VoidResult SyncDirectory(const std::filesystem::path& directoryPath) noexcept {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            const FileSystemAPI& api = LoadFileSystemAPI();
            if (api.open == nullptr || api.close == nullptr || api.sync == nullptr) {
                return std::unexpected{ErrorCode::NotSupported};
            }
            const int directory =
                api.open(directoryPath.c_str(),
                         PosixSystem::kOpenReadOnly | PosixSystem::kOpenCloseOnExec | PosixSystem::kOpenDirectory);
            if (directory < 0) {
                return std::unexpected{ErrorCode::IoError};
            }
            int synced = 0;
            do {
                synced = api.sync(directory);
            } while (synced != 0 && errno == EINTR);
            const int closed = api.close(directory);
            return synced == 0 && closed == 0 ? VoidResult{} : VoidResult{std::unexpected{ErrorCode::IoError}};
#else
            static_cast<void>(directoryPath);
            return {};
#endif
        }

        struct ReplaceOutcome {
            VoidResult result;
            bool published = false;
        };

        [[nodiscard]] ReplaceOutcome ReplaceNativeFile(const std::filesystem::path& replacement,
                                                       const std::filesystem::path& destination,
                                                       AtomicReplaceOptions options) {
            const FileSystemAPI& api = LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
            const WindowsSystem::DWord replaceFlags = options.durable ? WindowsSystem::kReplaceWriteThrough : 0;
            if (EnsureSystemAPIs(api.replaceFileWide)) {
                if (api.replaceFileWide(destination.c_str(), replacement.c_str(), nullptr, replaceFlags, nullptr,
                                        nullptr) != WindowsSystem::kFalse) {
                    return {.result = {}, .published = true};
                }
                const auto error = CaptureLastSystemError();
                if (error != WindowsSystem::kErrorFileNotFound && error != WindowsSystem::kErrorPathNotFound) {
                    return {.result = std::unexpected{ErrorCode::IoError}, .published = false};
                }
            }
            if (!EnsureSystemAPIs(api.moveFileWide)) {
                return {.result = std::unexpected{ErrorCode::NotSupported}, .published = false};
            }
            WindowsSystem::DWord moveFlags = WindowsSystem::kMoveReplaceExisting;
            moveFlags |= options.durable ? WindowsSystem::kMoveWriteThrough : 0;
            const bool published =
                api.moveFileWide(replacement.c_str(), destination.c_str(), moveFlags) != WindowsSystem::kFalse;
            if (!published) {
                return {.result = std::unexpected{ErrorCode::IoError}, .published = false};
            }
            return {.result = VoidResult{}, .published = published};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            if (api.rename == nullptr) {
                return {.result = std::unexpected{ErrorCode::NotSupported}, .published = false};
            }
            if (api.rename(replacement.c_str(), destination.c_str()) != 0) {
                return {.result = std::unexpected{ErrorCode::IoError}, .published = false};
            }
            if (!options.durable) {
                return {.result = {}, .published = true};
            }
            const std::filesystem::path sourceParent =
                replacement.has_parent_path() ? replacement.parent_path() : std::filesystem::path{"."};
            const std::filesystem::path destinationParent =
                destination.has_parent_path() ? destination.parent_path() : std::filesystem::path{"."};
            if (auto synchronized = SyncDirectory(destinationParent); !synchronized) {
                return {.result = synchronized, .published = true};
            }
            if (sourceParent != destinationParent) {
                return {.result = SyncDirectory(sourceParent), .published = true};
            }
            return {.result = {}, .published = true};
#else
            static_cast<void>(replacement);
            static_cast<void>(destination);
            static_cast<void>(options);
            return {.result = std::unexpected{ErrorCode::NotSupported}, .published = false};
#endif
        }

    } // namespace

    VoidResult AtomicReplaceFile(const std::filesystem::path& replacement, const std::filesystem::path& destination,
                                 AtomicReplaceOptions options) noexcept {
        if (replacement.empty() || destination.empty()) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        try {
            if (options.durable) {
                auto file = File::Open(replacement, FileOpenOptions{
                                                        .access = FileAccess::Write,
                                                        .share = FileShare::Read | FileShare::Write | FileShare::Delete,
                                                    });
                if (!file) {
                    return std::unexpected{file.error()};
                }
                if (auto flushed = file->Flush(); !flushed) {
                    return flushed;
                }
                if (auto closed = file->Close(); !closed) {
                    return closed;
                }
            }
            return ReplaceNativeFile(replacement, destination, options).result;
        } catch (const std::bad_alloc&) {
            return std::unexpected{ErrorCode::OutOfMemory};
        } catch (...) {
            return std::unexpected{ErrorCode::IoError};
        }
    }

    AtomicFileWriter::~AtomicFileWriter() {
        Abort();
    }

    AtomicFileWriter::AtomicFileWriter(AtomicFileWriter&& other) noexcept
        : file_{std::move(other.file_)},
          destination_{std::move(other.destination_)},
          temporary_{std::move(other.temporary_)},
          options_{other.options_},
          prepared_{std::exchange(other.prepared_, false)} {
        other.destination_.clear();
        other.temporary_.clear();
    }

    AtomicFileWriter& AtomicFileWriter::operator=(AtomicFileWriter&& other) noexcept {
        if (this != &other) {
            Abort();
            file_ = std::move(other.file_);
            destination_ = std::move(other.destination_);
            temporary_ = std::move(other.temporary_);
            options_ = other.options_;
            prepared_ = std::exchange(other.prepared_, false);
            other.destination_.clear();
            other.temporary_.clear();
        }
        return *this;
    }

    Result<AtomicFileWriter> AtomicFileWriter::Create(const std::filesystem::path& destination,
                                                      AtomicReplaceOptions options) noexcept {
        if (destination.empty() || !destination.has_filename()) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        try {
            const FileSystemAPI& api = LoadFileSystemAPI();
            const std::uint64_t process = CurrentProcessId().value_or(0);
            for (std::uint32_t attempt = 0; attempt < 128; ++attempt) {
                const std::uint64_t sequence = gTemporarySequence.fetch_add(1, std::memory_order_relaxed);
                std::filesystem::path temporary = destination;
#if defined(PLATFORM_WINDOWS)
                temporary += std::format(L".sora-tmp-{}-{}", process, sequence);
#else
                temporary += std::format(".sora-tmp-{}-{}", process, sequence);
#endif
                auto file = File::Open(temporary, FileOpenOptions{
                                                      .access = FileAccess::Read | FileAccess::Write,
                                                      .creation = FileCreation::CreateNew,
                                                      .flags = FileOpenFlag::Positional,
                                                  });
                if (file) {
                    AtomicFileWriter writer;
                    writer.file_ = std::move(*file);
                    writer.destination_ = destination;
                    writer.temporary_ = temporary;
                    writer.options_ = options;
                    return writer;
                }
                if (file.error() != ErrorCode::AlreadyExists) {
                    return std::unexpected{file.error()};
                }
            }
            return std::unexpected{ErrorCode::ResourceExhausted};
        } catch (const std::bad_alloc&) {
            return std::unexpected{ErrorCode::OutOfMemory};
        } catch (...) {
            return std::unexpected{ErrorCode::IoError};
        }
    }

    VoidResult AtomicFileWriter::Commit() noexcept {
        if (temporary_.empty() || (!file_ && !prepared_)) {
            return std::unexpected{ErrorCode::InvalidState};
        }
        try {
            if (!prepared_) {
                if (options_.durable) {
                    if (auto flushed = file_.Flush(); !flushed) {
                        return flushed;
                    }
                }
                if (auto closed = file_.Close(); !closed) {
                    return closed;
                }
                prepared_ = true;
            }
            ReplaceOutcome replaced = ReplaceNativeFile(temporary_, destination_, options_);
            if (replaced.published) {
                temporary_.clear();
                destination_.clear();
                prepared_ = false;
            }
            return std::move(replaced.result);
        } catch (const std::bad_alloc&) {
            return std::unexpected{ErrorCode::OutOfMemory};
        } catch (...) {
            return std::unexpected{ErrorCode::IoError};
        }
    }

    void AtomicFileWriter::Abort() noexcept {
        [[maybe_unused]] const VoidResult closed = file_.Close();
        DeleteNativeFile(temporary_);
        temporary_.clear();
        destination_.clear();
        prepared_ = false;
    }

} // namespace Sora::PAL
