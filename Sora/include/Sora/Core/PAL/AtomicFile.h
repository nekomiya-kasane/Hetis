/**
 * @file AtomicFile.h
 * @brief Publish complete files atomically through same-directory temporary-file transactions.
 * @details @ref AtomicFileWriter creates a unique sibling of the destination, exposes its positional @ref File, then
 * commits through one native atomic replacement. Commit flushes and closes the temporary file first; durable commits
 * additionally request write-through replacement and synchronize the parent directory where the platform requires it.
 *
 * @code{.cpp}
 * auto transaction = Sora::PAL::AtomicFileWriter::Create("cache/index.bin");
 * if (!transaction) {
 *     return transaction.error();
 * }
 * if (auto write = transaction->Output().WriteAllAt(bytes, 0); !write) {
 *     return write.error();
 * }
 * return transaction->Commit();
 * @endcode
 *
 * Source and destination must reside on the same mounted file system for atomicity. A successful commit replaces the
 * destination as one namespace operation; it does not make readers holding the previous file handle observe new bytes.
 * @ingroup PAL
 */

#pragma once

#include <Sora/Core/PAL/File.h>

#include <filesystem>

namespace Sora::PAL {

    /** @brief Durability policy for atomic replacement. */
    struct AtomicReplaceOptions {
        bool durable = true; /**< Flush data and namespace updates before reporting success. */
    };

    /**
     * @brief Atomically replace @p destination with an already complete @p replacement on the same file system.
     * @details A durable replacement flushes @p replacement before publication and synchronizes affected directory
     * entries. A durability error after the rename means the new name is visible but crash persistence is uncertain.
     */
    [[nodiscard]] VoidResult AtomicReplaceFile(const std::filesystem::path& replacement,
                                               const std::filesystem::path& destination,
                                               AtomicReplaceOptions options = {}) noexcept;

    /** @brief Move-only same-directory temporary-file transaction. */
    class AtomicFileWriter {
    public:
        /** @brief Construct an inactive transaction. */
        AtomicFileWriter() noexcept = default;

        /** @brief Delete an uncommitted temporary file. */
        ~AtomicFileWriter();

        AtomicFileWriter(const AtomicFileWriter&) = delete;
        AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

        /** @brief Transfer transaction ownership from @p other. */
        AtomicFileWriter(AtomicFileWriter&& other) noexcept;

        /** @brief Abort this transaction, then transfer ownership from @p other. */
        AtomicFileWriter& operator=(AtomicFileWriter&& other) noexcept;

        /** @brief Create a unique temporary sibling for @p destination. */
        [[nodiscard]] static Result<AtomicFileWriter> Create(const std::filesystem::path& destination,
                                                             AtomicReplaceOptions options = {}) noexcept;

        /** @brief Return the temporary output file. */
        [[nodiscard]] File& Output() noexcept { return file_; }

        /** @brief Return the temporary output file. */
        [[nodiscard]] const File& Output() const noexcept { return file_; }

        /** @brief Flush, close, and atomically publish the completed temporary file. */
        [[nodiscard]] VoidResult Commit() noexcept;

        /** @brief Close and delete the temporary file; repeated calls succeed. */
        void Abort() noexcept;

        /** @brief Return whether this transaction still owns an unpublished temporary file. */
        [[nodiscard]] explicit operator bool() const noexcept { return !temporary_.empty(); }

        /** @brief Return the private temporary path for diagnostics and tests. */
        [[nodiscard]] const std::filesystem::path& TemporaryPath() const noexcept { return temporary_; }

    private:
        File file_{};
        std::filesystem::path destination_;
        std::filesystem::path temporary_;
        AtomicReplaceOptions options_{};
        bool prepared_ = false;
    };

} // namespace Sora::PAL
