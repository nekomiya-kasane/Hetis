/**
 * @file File.h
 * @brief Own native files with explicit open, positional I/O, durability, and Direct I/O semantics.
 * @details @ref File complements rather than replaces @c std::filesystem. Continue using the standard library for
 * paths, traversal, metadata queries, and lexical operations; use this header when native opening flags, positional
 * I/O, crash-safe borrowed writes, write-through behavior, or Direct I/O alignment are part of the contract.
 *
 * @code{.cpp}
 * using namespace Sora::PAL;
 * auto opened = File::Open("mesh.bin", FileOpenOptions{
 *     .access = FileAccess::Read | FileAccess::Write,
 *     .creation = FileCreation::OpenOrCreate,
 * });
 * if (!opened) {
 *     return opened.error();
 * }
 * File file = std::move(*opened);
 * const std::array bytes{std::byte{1}, std::byte{2}};
 * if (auto written = file.WriteAllAt(bytes, 0); !written) {
 *     return written.error();
 * }
 * return file.Flush();
 * @endcode
 *
 * Position-independent operations may run concurrently on disjoint buffers and offsets. Sequential borrowed writes
 * share the operating-system file cursor and require external ordering. A borrowed view never extends ownership.
 * @ingroup PAL
 */

#pragma once

#include <Sora/Core/Flags.h>
#include <Sora/ErrorCode.h>
#include <Sora/Platform.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Sora::PAL {

    struct FileSystemAPI;
    class FileMapping;
    class FileWatcher;
    class OwnedNativeCrashStream;

    /** @brief Platform-native file handle type. */
    using NativeFileHandle = std::conditional_t<Platform::kIsWindows, void*, int>;

    /** @brief Platform-native invalid file-handle sentinel. */
    inline constexpr NativeFileHandle kInvalidNativeFileHandle = []<typename Handle>() -> Handle {
        if constexpr (std::is_pointer_v<Handle>) {
            return nullptr;
        } else {
            return -1;
        }
    }.template operator()<NativeFileHandle>();

    /** @brief File capabilities requested when opening a handle. */
    enum class FileAccess : std::uint8_t {
        None = 0,      /**< No data access; invalid for @ref File::Open. */
        Read = 1 << 0, /**< Permit reads. */
        Write = 1 << 1 /**< Permit writes. */
    };

    /** @brief Concurrent operations permitted through independently opened handles where the platform enforces them. */
    enum class FileShare : std::uint8_t {
        None = 0,       /**< Request exclusive access where supported. */
        Read = 1 << 0,  /**< Permit concurrent readers. */
        Write = 1 << 1, /**< Permit concurrent writers. */
        Delete = 1 << 2 /**< Permit rename or deletion while open. */
    };

    /** @brief Creation policy applied atomically by @ref File::Open. */
    enum class FileCreation : std::uint8_t {
        OpenExisting,    /**< Fail when the path does not exist. */
        CreateNew,       /**< Create a new file and fail when it already exists. */
        OpenOrCreate,    /**< Open an existing file or create a new one. */
        CreateAlways,    /**< Create a new file or truncate an existing one. */
        TruncateExisting /**< Truncate an existing file and fail when it does not exist. */
    };

    /** @brief Native file policies not represented by @c std::fstream. */
    enum class FileOpenFlag : std::uint16_t {
        None = 0,               /**< No additional policy. */
        Direct = 1 << 0,        /**< Bypass the operating-system page cache when supported. */
        WriteThrough = 1 << 1,  /**< Request write-through behavior for data writes. */
        Sequential = 1 << 2,    /**< Hint predominantly forward access. */
        Random = 1 << 3,        /**< Hint non-sequential access. */
        DeleteOnClose = 1 << 4, /**< Remove the directory entry after the final handle closes where supported. */
        Positional = 1 << 5     /**< Enable independent offset I/O and disable shared-cursor borrowed writes. */
    };

    /** @brief Complete policy for opening a regular file. */
    struct FileOpenOptions {
        FileAccess access = FileAccess::Read;               /**< Required read/write capabilities. */
        FileCreation creation = FileCreation::OpenExisting; /**< Atomic creation/truncation behavior. */
        FileShare share = FileShare::Read | FileShare::Write | FileShare::Delete; /**< Permitted sharing. */
        FileOpenFlag flags = FileOpenFlag::Positional; /**< Direct, durability, and access-pattern policies. */
        std::uint32_t permissions = 0644;              /**< POSIX creation mode; ignored on Windows. */
    };

    /** @brief Alignment contract imposed on Direct I/O buffers, offsets, and transfer sizes. */
    struct DirectIORequirements {
        size_t memoryAlignment = 1; /**< Minimum address alignment. */
        size_t offsetAlignment = 1; /**< Required file-offset multiple. */
        size_t sizeAlignment = 1;   /**< Required transfer-size multiple. */

        /** @brief Return whether @p buffer, @p offset, and @p size satisfy this contract. */
        [[nodiscard]] bool Accepts(const void* buffer, std::uint64_t offset, size_t size) const noexcept;
    };

    /** @brief Trivially copyable non-owning view over an open file and its pre-resolved native API table. */
    class BorrowedFile {
    public:
        /** @brief Construct an invalid view. */
        constexpr BorrowedFile() noexcept = default;

        /** @brief Return whether this view has a valid handle and API table. */
        [[nodiscard]] explicit operator bool() const noexcept;

        /** @brief Write every byte using the shared native file cursor, retrying partial and interruptible writes. */
        [[nodiscard]] bool Write(std::span<const std::byte> bytes) const noexcept;

        /** @brief Write every character byte using the shared native file cursor. */
        [[nodiscard]] bool Write(std::string_view text) const noexcept { return Write(std::as_bytes(std::span{text})); }

        /** @brief Best-effort synchronization of native buffers with the storage device. */
        void Flush() const noexcept;

        friend constexpr bool operator==(BorrowedFile, BorrowedFile) noexcept = default;

    private:
        friend class File;
        friend class FileMapping;
        friend class FileWatcher;
        friend class OwnedNativeCrashStream;
        friend BorrowedFile NativeStandardErrorFile() noexcept;

        constexpr BorrowedFile(NativeFileHandle handle, const FileSystemAPI* api) noexcept : handle_{handle} {}

        NativeFileHandle handle_ = kInvalidNativeFileHandle;
    };

    /** @brief Move-only owner of a regular native file handle. */
    class File {
    public:
        /** @brief Construct without an owned handle. */
        constexpr File() noexcept = default;

        /** @brief Close the owned handle when valid. */
        ~File();

        File(const File&) = delete;
        File& operator=(const File&) = delete;

        /** @brief Transfer ownership from @p other. */
        File(File&& other) noexcept;

        /** @brief Close the current handle, then transfer ownership from @p other. */
        File& operator=(File&& other) noexcept;

        /** @brief Open @p path with an explicit native policy. */
        [[nodiscard]] static Result<File> Open(const std::filesystem::path& path,
                                               FileOpenOptions options = {}) noexcept;

        /** @brief Return whether this object owns a valid native handle. */
        [[nodiscard]] explicit operator bool() const noexcept;

        /** @brief Return a non-owning view valid until close, move, or destruction. */
        [[nodiscard]] BorrowedFile Borrow() const noexcept {
            return positional_ ? BorrowedFile{} : BorrowedFile{handle_, api_};
        }

        /** @brief Read up to @p destination.size() bytes at absolute @p offset; zero bytes denotes end-of-file. */
        [[nodiscard]] Result<size_t> ReadAt(std::span<std::byte> destination, std::uint64_t offset) const noexcept;

        /**
         * @brief Fill @p destination from absolute @p offset.
         * @return Success, or @ref ErrorCode::DataTruncated when end-of-file is reached first.
         */
        [[nodiscard]] VoidResult ReadAllAt(std::span<std::byte> destination, std::uint64_t offset) const noexcept;

        /** @brief Write up to @p source.size() bytes at absolute @p offset. */
        [[nodiscard]] Result<size_t> WriteAt(std::span<const std::byte> source, std::uint64_t offset) const noexcept;

        /** @brief Write all bytes at absolute @p offset or return the first failure. */
        [[nodiscard]] VoidResult WriteAllAt(std::span<const std::byte> source, std::uint64_t offset) const noexcept;

        /** @brief Return the current file size in bytes. */
        [[nodiscard]] Result<std::uint64_t> Size() const noexcept;

        /** @brief Set the file size to @p size bytes. */
        [[nodiscard]] VoidResult Resize(std::uint64_t size) const noexcept;

        /** @brief Synchronize file data and required metadata with the storage device. */
        [[nodiscard]] VoidResult Flush() const noexcept;

        /** @brief Close the handle now; repeated calls succeed. */
        [[nodiscard]] VoidResult Close() noexcept;

        /** @brief Return whether this file was opened for Direct I/O. */
        [[nodiscard]] bool IsDirect() const noexcept { return direct_; }

        /** @brief Return Direct I/O constraints, or byte alignment for buffered files. */
        [[nodiscard]] DirectIORequirements DirectRequirements() const noexcept { return directRequirements_; }

    private:
        friend class FileMapping;
        friend class FileWatcher;
        friend class OwnedNativeCrashStream;

        File(NativeFileHandle handle, bool direct, bool positional, DirectIORequirements requirements) noexcept
            : handle_{handle}, direct_{direct}, positional_{positional}, directRequirements_{requirements} {}

        [[nodiscard]] bool ValidateTransfer(const void* buffer, std::uint64_t offset, size_t size) const noexcept;

        NativeFileHandle handle_ = kInvalidNativeFileHandle;
        bool direct_ = false;
        bool positional_ = false;
        DirectIORequirements directRequirements_{};
    };

    /** @brief Return a crash-safe borrowed view of the process standard-error file. */
    [[nodiscard]] BorrowedFile NativeStandardErrorFile() noexcept;

} // namespace Sora::PAL
