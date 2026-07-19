/**
 * @file FileWatcher.h
 * @brief Observe directory changes as a lossy hint stream with explicit overflow and rescan semantics.
 * @details Native file monitors may merge events, reorder independent names, or overflow bounded kernel buffers.
 * Consumers must treat @ref FileChangeKind::RescanRequired as an instruction to rebuild state from the directory.
 * Rename events carry both paths when the native backend correlates them within one batch.
 *
 * @code{.cpp}
 * auto watcher = Sora::PAL::FileWatcher::Open("shaders", {.recursive = true});
 * if (!watcher) {
 *     return watcher.error();
 * }
 * auto changes = watcher->Wait(std::chrono::seconds{1});
 * if (!changes) {
 *     return changes.error();
 * }
 * for (const Sora::PAL::FileChange& change : *changes) {
 *     ReloadAffectedShader(change);
 * }
 * @endcode
 *
 * @ref Wait is synchronous and must not run concurrently on the same watcher. Compose it with a dedicated scheduler
 * when blocking is acceptable; a future IOCP/io_uring sender backend should expose completion and cancellation without
 * wrapping this blocking call in an unstructured coroutine.
 * @ingroup PAL
 */

#pragma once

#include <Sora/Core/Flags.h>
#include <Sora/ErrorCode.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace Sora::PAL {

    /** @brief Native change categories requested from a watcher backend. */
    enum class FileWatchFilter : std::uint8_t {
        None = 0,               /**< No event category. */
        FileName = 1 << 0,      /**< File creation, removal, and rename. */
        DirectoryName = 1 << 1, /**< Directory creation, removal, and rename. */
        Size = 1 << 2,          /**< File-size changes. */
        LastWrite = 1 << 3,     /**< Last-write timestamp changes. */
        Creation = 1 << 4,      /**< Creation timestamp changes where available. */
        Security = 1 << 5       /**< Security descriptor changes where available. */
    };

    /** @brief Watch root and native filtering policy. */
    struct FileWatchOptions {
        bool recursive = false; /**< Include descendants when the backend can maintain recursive watches. */
        FileWatchFilter filter = FileWatchFilter::FileName | FileWatchFilter::DirectoryName | FileWatchFilter::Size |
                                 FileWatchFilter::LastWrite; /**< Requested categories. */
    };

    /** @brief Semantic change kind delivered by @ref FileWatcher. */
    enum class FileChangeKind : std::uint8_t {
        Added,         /**< A directory entry appeared. */
        Removed,       /**< A directory entry disappeared. */
        Modified,      /**< Content or selected metadata changed. */
        Renamed,       /**< A directory entry moved; @ref FileChange::oldPath may be empty. */
        RescanRequired /**< Events were lost or the backend reports only coarse invalidation. */
    };

    /** @brief One normalized file-monitor event. */
    struct FileChange {
        FileChangeKind kind = FileChangeKind::Modified; /**< Normalized operation. */
        std::filesystem::path path;                     /**< Current or affected absolute path. */
        std::filesystem::path oldPath;                  /**< Previous path for correlated renames. */
        bool directory = false;                         /**< Best-effort directory classification. */
    };

    /** @brief Move-only native directory monitor. */
    class FileWatcher {
    public:
        /** @brief Construct an inactive watcher. */
        FileWatcher() noexcept = default;

        /** @brief Stop monitoring and release native resources. */
        ~FileWatcher();

        FileWatcher(const FileWatcher&) = delete;
        FileWatcher& operator=(const FileWatcher&) = delete;

        /** @brief Transfer native monitoring state from @p other. */
        FileWatcher(FileWatcher&& other) noexcept;

        /** @brief Stop this watcher, then transfer state from @p other. */
        FileWatcher& operator=(FileWatcher&& other) noexcept;

        /** @brief Open a monitor rooted at @p directory. */
        [[nodiscard]] static Result<FileWatcher> Open(const std::filesystem::path& directory,
                                                      FileWatchOptions options = {}) noexcept;

        /**
         * @brief Wait up to @p timeout for one native batch and normalize it.
         * @return Empty vector on timeout, normalized events on success, or an I/O error.
         */
        [[nodiscard]] Result<std::vector<FileChange>>
        Wait(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

        /** @brief Return whether this object owns active native monitoring state. */
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

        /** @brief Return the absolute or caller-supplied watch root. */
        [[nodiscard]] Result<std::filesystem::path> Root() const noexcept;

    private:
        struct State;

        explicit FileWatcher(std::unique_ptr<State> state) noexcept;

        std::unique_ptr<State> state_;
    };

} // namespace Sora::PAL
