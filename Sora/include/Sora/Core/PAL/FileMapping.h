/**
 * @file FileMapping.h
 * @brief Map file ranges into virtual memory with explicit sharing, mutability, flushing, and access advice.
 * @details A mapping borrows its source @ref File only while it is created; the native mapping remains valid after the
 * file owner is moved or closed. The requested offset need not match the operating-system allocation granularity:
 * @ref FileMapping aligns the native view downward and exposes only the exact requested byte range.
 *
 * @code{.cpp}
 * auto file = Sora::PAL::File::Open("scene.lpak");
 * if (!file) {
 *     return file.error();
 * }
 * auto mapping = Sora::PAL::FileMapping::Map(*file, {.offset = 128, .size = 4096});
 * if (!mapping) {
 *     return mapping.error();
 * }
 * ParsePackage(mapping->Bytes());
 * @endcode
 * @ingroup PAL
 */

#pragma once

#include <Sora/Core/PAL/File.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Sora::PAL {

    /** @brief Memory protection and sharing policy for a file mapping. */
    enum class FileMappingAccess : std::uint8_t {
        Read,       /**< Shared read-only view. */
        ReadWrite,  /**< Shared writable view whose changes may be flushed to the file. */
        CopyOnWrite /**< Private writable view whose changes are never committed to the file. */
    };

    /** @brief Kernel paging hint for an existing mapping. */
    enum class FileMappingAdvice : std::uint8_t {
        Normal,     /**< No specialized access pattern. */
        Sequential, /**< Predominantly increasing addresses. */
        Random,     /**< Non-sequential page access. */
        WillNeed,   /**< Pages are expected soon. */
        DontNeed    /**< Pages need not remain resident. */
    };

    /** @brief Exact file range and sharing policy requested from @ref FileMapping::Map. */
    struct FileMappingOptions {
        FileMappingAccess access = FileMappingAccess::Read; /**< Protection and sharing mode. */
        uint64_t offset = 0;                                /**< First exposed file byte. */
        size_t size = 0; /**< Exposed byte count; zero maps from @ref offset to current end-of-file. */
    };

    /** @brief Move-only owner of one native mapped file range. */
    class FileMapping {
    public:
        /** @brief Construct without a mapping. */
        constexpr FileMapping() noexcept = default;

        /** @brief Unmap the native view and release mapping resources. */
        ~FileMapping();

        FileMapping(const FileMapping&) = delete;
        FileMapping& operator=(const FileMapping&) = delete;

        /** @brief Transfer mapping ownership from @p other. */
        FileMapping(FileMapping&& other) noexcept;

        /** @brief Replace this mapping by moving @p other. */
        FileMapping& operator=(FileMapping&& other) noexcept;

        /** @brief Map the range selected by @p options from @p file. */
        [[nodiscard]] static Result<FileMapping> Map(const File& file, FileMappingOptions options = {}) noexcept;

        /** @brief Return whether this object represents a successfully created mapping, including an empty mapping. */
        [[nodiscard]] explicit operator bool() const noexcept { return active_; }

        /** @brief Return the exact requested bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return {data_, size_}; }

        /** @brief Return mutable bytes, or an empty span for read-only mappings. */
        [[nodiscard]] std::span<std::byte> WritableBytes() noexcept {
            return access_ == FileMappingAccess::Read ? std::span<std::byte>{} : std::span{data_, size_};
        }

        /** @brief Return the exposed file offset. */
        [[nodiscard]] uint64_t Offset() const noexcept { return offset_; }

        /** @brief Return the exposed byte count. */
        [[nodiscard]] size_t Size() const noexcept { return size_; }

        /** @brief Flush a subrange of a shared writable mapping and synchronize the underlying file. */
        [[nodiscard]] VoidResult Flush(size_t offset = 0, size_t size = 0) const noexcept;

        /** @brief Submit a best-effort paging hint for the exposed range. */
        [[nodiscard]] VoidResult Advise(FileMappingAdvice advice) const noexcept;

        /** @brief Release this mapping now; repeated calls succeed. */
        void Reset() noexcept;

    private:
        void* mappingHandle_ = nullptr;
        void* base_ = nullptr;
        std::byte* data_ = nullptr;
        size_t mappedSize_ = 0;
        size_t size_ = 0;
        uint64_t offset_ = 0;
        File file_{};
        FileMappingAccess access_ = FileMappingAccess::Read;
        bool active_ = false;
    };

} // namespace Sora::PAL
