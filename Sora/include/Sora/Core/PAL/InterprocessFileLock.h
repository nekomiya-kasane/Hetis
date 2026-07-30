/**
 * @file InterprocessFileLock.h
 * @brief Own one crash-released exclusive file lock shared across processes.
 * @ingroup PAL
 */
#pragma once

#include <Sora/Core/PAL/File.h>

#include <filesystem>
#include <optional>

namespace Sora::PAL {

    /**
     * @brief Move-only exclusive advisory lock whose ownership is tied to one native file handle.
     * @details The lock is released when this object closes its handle or when the owning process exits. The directory
     * entry is intentionally retained because deleting and recreating a lock file can split contenders across inodes.
     */
    class InterprocessFileLock final {
    public:
        /** @brief Construct an inactive lock. */
        InterprocessFileLock() noexcept = default;

        InterprocessFileLock(const InterprocessFileLock&) = delete;
        InterprocessFileLock& operator=(const InterprocessFileLock&) = delete;

        /** @brief Transfer lock ownership from @p other. */
        InterprocessFileLock(InterprocessFileLock&&) noexcept = default;

        /** @brief Release the current lock, then transfer ownership from @p other. */
        InterprocessFileLock& operator=(InterprocessFileLock&&) noexcept = default;

        /**
         * @brief Try to acquire an exclusive lock on @p path without waiting.
         * @return An owning lock, an empty optional when another process owns it, or a native failure.
         */
        [[nodiscard]] static Result<std::optional<InterprocessFileLock>>
        TryAcquire(const std::filesystem::path& path) noexcept;

        /** @brief Return whether this object owns the exclusive native lock. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(file_); }

    private:
        explicit InterprocessFileLock(File file) noexcept : file_(std::move(file)) {}

        File file_;
    };

} // namespace Sora::PAL
