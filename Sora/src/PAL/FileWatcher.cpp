/**
 * @file FileWatcher.cpp
 * @brief Implement bounded native directory monitoring and normalized rescan semantics.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/FileWatcher.h>
#include <Sora/Core/PAL/SystemAPI.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Sora::PAL {

    struct FileWatcher::State {
        inline static constexpr size_t kBufferSize = size_t{64} * 1024;

        std::filesystem::path root;
        FileWatchOptions options{};
        const FileSystemAPI* api = nullptr;
        alignas(std::max_align_t) std::array<std::byte, kBufferSize> buffer{};
#if defined(PLATFORM_WINDOWS)
        WindowsSystem::Handle directory = nullptr;
        WindowsSystem::Handle event = nullptr;
#elif defined(PLATFORM_LINUX)
        int notify = -1;
        std::unordered_map<int, std::filesystem::path> roots;
#elif defined(PLATFORM_MACOS)
        int queue = -1;
        int directory = -1;
#endif

        ~State() {
            if (api == nullptr) {
                return;
            }
#if defined(PLATFORM_WINDOWS)
            if (directory != nullptr && !WindowsSystem::IsInvalidHandle(directory) && api->closeHandle != nullptr) {
                static_cast<void>(api->closeHandle(directory));
            }
            if (event != nullptr && api->closeHandle != nullptr) {
                static_cast<void>(api->closeHandle(event));
            }
#elif defined(PLATFORM_LINUX)
            if (notify >= 0 && api->close != nullptr) {
                static_cast<void>(api->close(notify));
            }
#elif defined(PLATFORM_MACOS)
            if (directory >= 0 && api->close != nullptr) {
                static_cast<void>(api->close(directory));
            }
            if (queue >= 0 && api->close != nullptr) {
                static_cast<void>(api->close(queue));
            }
#endif
        }
    };

    namespace {

#if defined(PLATFORM_WINDOWS)
        [[nodiscard]] WindowsSystem::DWord WindowsWatchMask(FileWatchFilter filter) noexcept {
            WindowsSystem::DWord mask = 0;
            mask |= HasAny(filter, FileWatchFilter::FileName) ? WindowsSystem::kNotifyFileName : 0;
            mask |= HasAny(filter, FileWatchFilter::DirectoryName) ? WindowsSystem::kNotifyDirectoryName : 0;
            mask |= HasAny(filter, FileWatchFilter::Size) ? WindowsSystem::kNotifySize : 0;
            mask |= HasAny(filter, FileWatchFilter::LastWrite) ? WindowsSystem::kNotifyLastWrite : 0;
            mask |= HasAny(filter, FileWatchFilter::Creation) ? WindowsSystem::kNotifyCreation : 0;
            mask |= HasAny(filter, FileWatchFilter::Security) ? WindowsSystem::kNotifySecurity : 0;
            return mask;
        }

        template<typename State>
        void ParseWindowsChanges(State& state, WindowsSystem::DWord byteCount, std::vector<FileChange>& changes) {
            std::optional<std::filesystem::path> oldRename;
            WindowsSystem::DWord offset = 0;
            constexpr WindowsSystem::DWord kHeaderSize = sizeof(WindowsSystem::FileNotifyInformationHeader);
            while (offset <= byteCount && byteCount - offset >= kHeaderSize) {
                const auto* info =
                    reinterpret_cast<const WindowsSystem::FileNotifyInformationHeader*>(state.buffer.data() + offset);
                if (info->fileNameLength > byteCount - offset - kHeaderSize ||
                    info->fileNameLength % sizeof(wchar_t) != 0) {
                    changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                    return;
                }
                const auto* fileName = reinterpret_cast<const wchar_t*>(state.buffer.data() + offset + kHeaderSize);
                const std::filesystem::path path =
                    state.root / std::wstring_view{fileName, info->fileNameLength / sizeof(wchar_t)};
                switch (info->action) {
                    case WindowsSystem::kActionAdded:
                        changes.push_back({.kind = FileChangeKind::Added, .path = path});
                        break;
                    case WindowsSystem::kActionRemoved:
                        changes.push_back({.kind = FileChangeKind::Removed, .path = path});
                        break;
                    case WindowsSystem::kActionModified:
                        changes.push_back({.kind = FileChangeKind::Modified, .path = path});
                        break;
                    case WindowsSystem::kActionRenamedOld:
                        oldRename = path;
                        break;
                    case WindowsSystem::kActionRenamedNew:
                        changes.push_back({.kind = FileChangeKind::Renamed,
                                           .path = path,
                                           .oldPath = oldRename.value_or(std::filesystem::path{})});
                        oldRename.reset();
                        break;
                    default:
                        changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                        break;
                }
                if (info->nextEntryOffset == 0) {
                    break;
                }
                if (info->nextEntryOffset < kHeaderSize || info->nextEntryOffset > byteCount - offset) {
                    changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                    return;
                }
                offset += info->nextEntryOffset;
            }
            if (oldRename) {
                changes.push_back({.kind = FileChangeKind::Renamed, .oldPath = std::move(*oldRename)});
            }
        }

        template<typename State>
        void CancelWindowsRead(State& state, WindowsSystem::Overlapped& overlapped) noexcept {
            WindowsSystem::DWord ignored = 0;
            auto* native = reinterpret_cast<WindowsSystem::NativeOverlapped*>(&overlapped);
            static_cast<void>(state.api->cancelIo(state.directory, native));
            static_cast<void>(state.api->getOverlappedResult(state.directory, native, &ignored, true));
        }
#elif defined(PLATFORM_LINUX)
        [[nodiscard]] uint32_t LinuxWatchMask(FileWatchFilter filter) noexcept {
            uint32_t mask = 0;
            if (HasAny(filter,
                       FileWatchFilter::FileName | FileWatchFilter::DirectoryName | FileWatchFilter::Creation)) {
                mask |= PosixSystem::kEventCreate | PosixSystem::kEventDelete | PosixSystem::kEventMovedFrom |
                        PosixSystem::kEventMovedTo | PosixSystem::kEventDeleteSelf | PosixSystem::kEventMoveSelf;
            }
            if (HasAny(filter, FileWatchFilter::Size | FileWatchFilter::LastWrite)) {
                mask |= PosixSystem::kEventModify | PosixSystem::kEventCloseWrite;
            }
            if (HasAny(filter, FileWatchFilter::Creation | FileWatchFilter::Security)) {
                mask |= PosixSystem::kEventAttrib;
            }
            return mask;
        }

        template<typename State>
        [[nodiscard]] bool AddLinuxWatch(State& state, const std::filesystem::path& directory) {
            const int descriptor =
                state.api->addNotify(state.notify, directory.c_str(), LinuxWatchMask(state.options.filter));
            if (descriptor < 0) {
                return false;
            }
            state.roots.insert_or_assign(descriptor, directory);
            return true;
        }

        template<typename State>
        [[nodiscard]] bool AddLinuxTree(State& state, const std::filesystem::path& directory) {
            if (!AddLinuxWatch(state, directory)) {
                return false;
            }
            if (!state.options.recursive) {
                return true;
            }
            std::error_code error;
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            std::filesystem::recursive_directory_iterator iterator{directory, options, error};
            const std::filesystem::recursive_directory_iterator end;
            while (!error && iterator != end) {
                if (iterator->is_directory(error) && !error && !AddLinuxWatch(state, iterator->path())) {
                    return false;
                }
                iterator.increment(error);
            }
            return !error;
        }

        using PendingLinuxRenames = std::unordered_map<uint32_t, std::pair<std::filesystem::path, bool>>;

        [[nodiscard]] bool WantsName(FileWatchFilter filter, bool directory) noexcept {
            return HasAny(filter, directory ? FileWatchFilter::DirectoryName : FileWatchFilter::FileName);
        }

        [[nodiscard]] bool IsPathWithin(const std::filesystem::path& path,
                                        const std::filesystem::path& directory) noexcept {
            auto pathPart = path.begin();
            for (auto directoryPart = directory.begin(); directoryPart != directory.end();
                 ++directoryPart, ++pathPart) {
                if (pathPart == path.end() || *pathPart != *directoryPart) {
                    return false;
                }
            }
            return true;
        }

        template<typename State>
        void RemoveLinuxTree(State& state, const std::filesystem::path& directory) {
            for (auto watch = state.roots.begin(); watch != state.roots.end();) {
                if (!IsPathWithin(watch->second, directory)) {
                    ++watch;
                    continue;
                }
                if (state.api->removeNotify != nullptr) {
                    static_cast<void>(state.api->removeNotify(state.notify, watch->first));
                }
                watch = state.roots.erase(watch);
            }
        }

        template<typename State>
        void ParseLinuxChanges(State& state, size_t byteCount, PendingLinuxRenames& oldRenames,
                               std::vector<FileChange>& changes) {
            size_t offset = 0;
            constexpr size_t kHeaderSize = sizeof(PosixSystem::InotifyEventHeader);
            while (offset + kHeaderSize <= byteCount) {
                const auto* event =
                    reinterpret_cast<const PosixSystem::InotifyEventHeader*>(state.buffer.data() + offset);
                if (event->nameLength > byteCount - offset - kHeaderSize) {
                    changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                    return;
                }
                const char* name = reinterpret_cast<const char*>(state.buffer.data() + offset + kHeaderSize);
                size_t nameLength = 0;
                while (nameLength < event->nameLength && name[nameLength] != '\0') {
                    ++nameLength;
                }
                if ((event->mask & PosixSystem::kEventQueueOverflow) != 0) {
                    changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                } else if (const auto root = state.roots.find(event->watchDescriptor); root != state.roots.end()) {
                    const std::filesystem::path path =
                        nameLength == 0 ? root->second : root->second / std::string{name, nameLength};
                    const bool directory = (event->mask & PosixSystem::kEventIsDirectory) != 0;
                    if ((event->mask & PosixSystem::kEventIgnored) != 0) {
                        state.roots.erase(root);
                        changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                        offset += kHeaderSize + event->nameLength;
                        continue;
                    }
                    if ((event->mask & (PosixSystem::kEventDeleteSelf | PosixSystem::kEventMoveSelf)) != 0) {
                        changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                    }
                    if ((event->mask & PosixSystem::kEventMovedFrom) != 0) {
                        oldRenames.insert_or_assign(event->cookie, std::pair{path, directory});
                    } else if ((event->mask & PosixSystem::kEventMovedTo) != 0) {
                        auto old = oldRenames.find(event->cookie);
                        if (directory && state.options.recursive && !AddLinuxTree(state, path)) {
                            changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                        }
                        if (WantsName(state.options.filter, directory)) {
                            changes.push_back({
                                .kind = FileChangeKind::Renamed,
                                .path = path,
                                .oldPath = old != oldRenames.end() ? old->second.first : std::filesystem::path{},
                                .directory = directory,
                            });
                        }
                        if (old != oldRenames.end()) {
                            oldRenames.erase(old);
                        }
                    } else if ((event->mask & PosixSystem::kEventCreate) != 0) {
                        if (directory && state.options.recursive && !AddLinuxTree(state, path)) {
                            changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state.root});
                        }
                        if (WantsName(state.options.filter, directory)) {
                            changes.push_back({.kind = FileChangeKind::Added, .path = path, .directory = directory});
                        }
                    } else if ((event->mask & (PosixSystem::kEventDelete | PosixSystem::kEventDeleteSelf)) != 0) {
                        if (WantsName(state.options.filter, directory)) {
                            changes.push_back({.kind = FileChangeKind::Removed, .path = path, .directory = directory});
                        }
                    } else {
                        changes.push_back({.kind = FileChangeKind::Modified, .path = path, .directory = directory});
                    }
                }
                offset += kHeaderSize + event->nameLength;
            }
        }
#endif

    } // namespace

    FileWatcher::~FileWatcher() = default;
    FileWatcher::FileWatcher(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
    FileWatcher::FileWatcher(FileWatcher&& other) noexcept = default;
    FileWatcher& FileWatcher::operator=(FileWatcher&& other) noexcept = default;

    Result<FileWatcher> FileWatcher::Open(const std::filesystem::path& directory, FileWatchOptions options) noexcept {
        if (directory.empty() || IsEmpty(options.filter) || !IsValidFlagSet(options.filter)) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        try {
            auto state = std::make_unique<State>();
            state->root = directory;
            state->options = options;
            state->api = &LoadFileSystemAPI();
#if defined(PLATFORM_WINDOWS)
            if (state->api->createFileWide == nullptr || state->api->closeHandle == nullptr ||
                state->api->createEventWide == nullptr || state->api->readDirectoryChanges == nullptr) {
                return std::unexpected{ErrorCode::NotSupported};
            }
            state->directory = state->api->createFileWide(
                directory.c_str(), WindowsSystem::kFileListDirectory,
                WindowsSystem::kFileShareRead | WindowsSystem::kFileShareWrite | WindowsSystem::kFileShareDelete,
                nullptr, WindowsSystem::kOpenExisting,
                WindowsSystem::kFileFlagBackupSemantics | WindowsSystem::kFileFlagOverlapped, nullptr);
            if (WindowsSystem::IsInvalidHandle(state->directory)) {
                return std::unexpected{ErrorCode::IoError};
            }
            state->event = state->api->createEventWide(nullptr, true, false, nullptr);
            if (state->event == nullptr) {
                return std::unexpected{ErrorCode::IoError};
            }
#elif defined(PLATFORM_LINUX)
            if (state->api->initializeNotify == nullptr || state->api->addNotify == nullptr ||
                state->api->poll == nullptr || state->api->read == nullptr) {
                return std::unexpected{ErrorCode::NotSupported};
            }
            state->notify = state->api->initializeNotify(PosixSystem::kOpenNonBlocking | PosixSystem::kOpenCloseOnExec);
            if (state->notify < 0 || !AddLinuxTree(*state, directory)) {
                return std::unexpected{ErrorCode::IoError};
            }
#elif defined(PLATFORM_MACOS)
            if (options.recursive || state->api->createQueue == nullptr || state->api->queueEvent == nullptr ||
                state->api->open == nullptr) {
                return std::unexpected{ErrorCode::NotSupported};
            }
            state->directory =
                state->api->open(directory.c_str(), PosixSystem::kOpenEventOnly | PosixSystem::kOpenCloseOnExec);
            state->queue = state->api->createQueue();
            if (state->directory < 0 || state->queue < 0) {
                return std::unexpected{ErrorCode::IoError};
            }
            PosixSystem::KernelEvent event{};
            uint32_t nativeFilter = 0;
            nativeFilter |= HasAny(options.filter, FileWatchFilter::FileName | FileWatchFilter::DirectoryName |
                                                       FileWatchFilter::Creation)
                                ? PosixSystem::kNoteWrite | PosixSystem::kNoteDelete | PosixSystem::kNoteRename
                                : 0;
            nativeFilter |= HasAny(options.filter, FileWatchFilter::Size | FileWatchFilter::LastWrite)
                                ? PosixSystem::kNoteWrite | PosixSystem::kNoteExtend
                                : 0;
            nativeFilter |= HasAny(options.filter, FileWatchFilter::Security) ? PosixSystem::kNoteAttribute : 0;
            event.identifier = static_cast<uintptr_t>(state->directory);
            event.filter = PosixSystem::kFilterVnode;
            event.flags = PosixSystem::kEventAdd | PosixSystem::kEventClear;
            event.filterFlags = nativeFilter;
            if (state->api->queueEvent(state->queue, nullptr, 0,
                                       reinterpret_cast<PosixSystem::NativeKernelEvent*>(&event), 1, nullptr) < 0) {
                return std::unexpected{ErrorCode::IoError};
            }
#else
            return std::unexpected{ErrorCode::NotSupported};
#endif
            return FileWatcher{std::move(state)};
        } catch (const std::bad_alloc&) {
            return std::unexpected{ErrorCode::OutOfMemory};
        } catch (...) {
            return std::unexpected{ErrorCode::IoError};
        }
    }

    Result<std::vector<FileChange>> FileWatcher::Wait(std::chrono::milliseconds timeout) noexcept {
        if (state_ == nullptr || timeout.count() < 0) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        try {
            std::vector<FileChange> changes;
#if defined(PLATFORM_WINDOWS)
            if (state_->api->resetEvent == nullptr || state_->api->waitForSingleObject == nullptr ||
                state_->api->getOverlappedResult == nullptr || state_->api->cancelIo == nullptr) {
                return std::unexpected{ErrorCode::NotSupported};
            }
            static_assert(State::kBufferSize <= std::numeric_limits<WindowsSystem::DWord>::max());
            WindowsSystem::Overlapped overlapped{};
            overlapped.event = state_->event;
            if (state_->api->resetEvent(state_->event) == WindowsSystem::kFalse) {
                return std::unexpected{ErrorCode::IoError};
            }
            WindowsSystem::DWord byteCount = 0;
            const auto issued = state_->api->readDirectoryChanges(
                state_->directory, state_->buffer.data(), static_cast<WindowsSystem::DWord>(state_->buffer.size()),
                state_->options.recursive, WindowsWatchMask(state_->options.filter), nullptr,
                reinterpret_cast<WindowsSystem::NativeOverlapped*>(&overlapped), nullptr);
            if (issued == WindowsSystem::kFalse && CaptureLastSystemError() != WindowsSystem::kErrorIoPending) {
                return std::unexpected{ErrorCode::IoError};
            }
            const auto milliseconds = timeout == std::chrono::milliseconds::max()
                                          ? WindowsSystem::kInfinite
                                          : static_cast<WindowsSystem::DWord>(
                                                std::min<std::int64_t>(timeout.count(), WindowsSystem::kInfinite - 1));
            const WindowsSystem::DWord waited = state_->api->waitForSingleObject(state_->event, milliseconds);
            if (waited == WindowsSystem::kWaitTimeout) {
                CancelWindowsRead(*state_, overlapped);
                return changes;
            }
            if (waited != WindowsSystem::kWaitObject) {
                CancelWindowsRead(*state_, overlapped);
                return std::unexpected{ErrorCode::IoError};
            }
            if (state_->api->getOverlappedResult(state_->directory,
                                                 reinterpret_cast<WindowsSystem::NativeOverlapped*>(&overlapped),
                                                 &byteCount, false) == WindowsSystem::kFalse) {
                CancelWindowsRead(*state_, overlapped);
                return std::unexpected{ErrorCode::IoError};
            }
            if (byteCount == 0) {
                changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state_->root});
            } else {
                ParseWindowsChanges(*state_, byteCount, changes);
            }
#elif defined(PLATFORM_LINUX)
            PosixSystem::PollDescriptor descriptor{
                .descriptor = state_->notify, .events = PosixSystem::kPollInput, .revents = 0};
            const int milliseconds = timeout == std::chrono::milliseconds::max()
                                         ? -1
                                         : static_cast<int>(std::min<std::int64_t>(timeout.count(), INT_MAX));
            int polled = 0;
            do {
                polled = state_->api->poll(reinterpret_cast<PosixSystem::NativePollDescriptor*>(&descriptor), 1,
                                           milliseconds);
            } while (polled < 0 && errno == EINTR);
            if (polled == 0) {
                return changes;
            }
            if (polled < 0) {
                return std::unexpected{ErrorCode::IoError};
            }
            if ((descriptor.revents &
                 (PosixSystem::kPollError | PosixSystem::kPollHangup | PosixSystem::kPollInvalid)) != 0) {
                return std::unexpected{ErrorCode::IoError};
            }
            PendingLinuxRenames oldRenames;
            for (;;) {
                Sora::ssize_t bytes = 0;
                do {
                    bytes = state_->api->read(state_->notify, state_->buffer.data(), state_->buffer.size());
                } while (bytes < 0 && errno == EINTR);
                if (bytes > 0) {
                    ParseLinuxChanges(*state_, static_cast<size_t>(bytes), oldRenames, changes);
                    continue;
                }
                if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    return std::unexpected{ErrorCode::IoError};
                }
                break;
            }
            for (auto& [cookie, old] : oldRenames) {
                static_cast<void>(cookie);
                if (old.second && state_->options.recursive) {
                    RemoveLinuxTree(*state_, old.first);
                }
                if (WantsName(state_->options.filter, old.second)) {
                    changes.push_back({
                        .kind = FileChangeKind::Renamed,
                        .oldPath = std::move(old.first),
                        .directory = old.second,
                    });
                }
            }
#elif defined(PLATFORM_MACOS)
            PosixSystem::TimeSpec nativeTimeout{};
            const PosixSystem::NativeTimeSpec* timeoutPointer = nullptr;
            if (timeout != std::chrono::milliseconds::max()) {
                nativeTimeout.seconds = timeout.count() / 1000;
                nativeTimeout.nanoseconds = timeout.count() % 1000 * 1'000'000;
                timeoutPointer = reinterpret_cast<PosixSystem::NativeTimeSpec*>(&nativeTimeout);
            }
            PosixSystem::KernelEvent event{};
            const int count =
                state_->api->queueEvent(state_->queue, nullptr, 0,
                                        reinterpret_cast<PosixSystem::NativeKernelEvent*>(&event), 1, timeoutPointer);
            if (count < 0) {
                return std::unexpected{ErrorCode::IoError};
            }
            if (count != 0) {
                changes.push_back({.kind = FileChangeKind::RescanRequired, .path = state_->root});
            }
#else
            return std::unexpected{ErrorCode::NotSupported};
#endif
            return changes;
        } catch (const std::bad_alloc&) {
            return std::unexpected{ErrorCode::OutOfMemory};
        } catch (...) {
            return std::unexpected{ErrorCode::IoError};
        }
    }

    Result<std::filesystem::path> FileWatcher::Root() const noexcept {
        return state_ ? state_->root : std::filesystem::path{};
    }

} // namespace Sora::PAL
