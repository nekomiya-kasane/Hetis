/**
 * @file Format.h
 * @brief Sora resource package wire-format declarations.
 * @ingroup Resources
 *
 * @details `.lpak` is optimized for the common case where payload bytes dominate file size and metadata is small.
 * Package opening validates and deserializes metadata once, then resource lookup returns immutable spans directly into
 * the backing byte image. Payload bytes are intentionally not scanned during mount; the operating system can keep mmap
 * access lazy and page-granular.
 *
 * @verbatim
 * .lpak
 * +----------------------+ 0
 * | FileHeader           | fixed wire header, metadata hash field is zeroed while hashing
 * +----------------------+ header.sectionTableOffset
 * | SectionDescriptor[]  | entries, strings, data; descriptor bytes are metadata
 * +----------------------+
 * | padding              | 8-byte metadata alignment
 * +----------------------+
 * | ResourceEntry[]      | sorted by semanticHash when layout == Sorted
 * +----------------------+
 * | padding              | 8-byte metadata alignment
 * +----------------------+
 * | URI string table     | NUL-terminated canonical res://... strings
 * +----------------------+
 * | padding              | kDefaultDataAlignment
 * +----------------------+ first payloadOffset
 * | payload bytes        | each payload may be page-aligned and is never copied by PakView
 * +----------------------+
 * @endverbatim
 *
 * | Region | Access pattern | Integrity checked by `PakView::Open` | Runtime payload cost |
 * |---|---|---|---|
 * | Header | One reflected little-endian read | Magic, version, sizes, and metadata hash | None |
 * | Section table | Reflected little-endian reads | Bounds, alignment, overlap, and metadata hash | None |
 * | Entries | Reflected reads into a compact vector | Sorted order, URI references, metadata hash | None |
 * | Strings | Borrowed span | Canonical URI text and metadata hash | None |
 * | Payload | Borrowed span by absolute file offset | Bounds during mount; content hash is advisory | Zero-copy |
 */
#pragma once

#include <Sora/Core/Flags.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Sora::Resources {

    /** @brief `.lpak` magic, encoded as little-endian bytes `LPAK`. */
    inline constexpr uint32_t kLpakMagic =
        uint32_t{'L'} | (uint32_t{'P'} << 8) | (uint32_t{'A'} << 16) | (uint32_t{'K'} << 24);

    /** @brief Major format version for the canonical Sora resource package format. */
    inline constexpr uint16_t kLpakMajor = 1;

    /** @brief Minor format version for Sora resource packages. */
    inline constexpr uint16_t kLpakMinor = 0;

    /** @brief Default payload alignment. */
    inline constexpr uint64_t kDefaultDataAlignment = 4096;

    /** @brief Canonical little-endian wire size of @ref FileHeader. */
    inline constexpr uint16_t kFileHeaderWireSize = 64;

    /** @brief Canonical little-endian wire size of @ref SectionDescriptor. */
    inline constexpr uint16_t kSectionDescriptorWireSize = 40;

    /** @brief Canonical little-endian wire size of @ref ResourceEntry. */
    inline constexpr uint16_t kResourceEntryWireSize = 56;

    /** @brief Coarse semantic class of a resource. */
    enum class ResourceType : uint16_t {
        Unknown = 0,
        Image = 1,
        Font = 2,
        I18n = 3,
        Bytecode = 4,
        Raw = 5,
        Shader = 6,
        Model = 7,
        Material = 8,
        Scene = 9,
        UiLayout = 10,
        Config = 11,
        Audio = 12,
        Data = 13,
        Settings = 14,
        Max = std::numeric_limits<uint16_t>::max()
    };

    /** @brief Payload codec encoded by the canonical package format. */
    enum class CompressionCodec : uint16_t { None = 0, Max = std::numeric_limits<uint16_t>::max() };

    /** @brief Per-entry storage flags encoded by the canonical package format. */
    enum class ResourceFlags : uint16_t { None = 0, Max = std::numeric_limits<uint16_t>::max() };

    /** @brief Physical lookup layout encoded by an `.lpak` file. */
    enum class IndexLayout : uint16_t { Sorted = 0, Max = std::numeric_limits<uint16_t>::max() };

    /** @brief Logical section kind in the package section table. */
    enum class SectionKind : uint32_t {
        Entries = 1,
        Strings = 2,
        Data = 3,
        Max = std::numeric_limits<uint32_t>::max()
    };

    /** @brief Return whether @p type is a stable, currently supported resource type. */
    [[nodiscard]] constexpr bool IsKnownResourceType(ResourceType type) noexcept {
        return type != ResourceType::Unknown &&
               static_cast<uint16_t>(type) <= static_cast<uint16_t>(ResourceType::Settings);
    }

    /** @brief Return whether @p type is a stable, currently supported encoded resource type. */
    [[nodiscard]] constexpr bool IsKnownResourceType(uint16_t type) noexcept {
        return IsKnownResourceType(static_cast<ResourceType>(type));
    }

    /** @brief Return whether @p codec is a supported payload codec. */
    [[nodiscard]] constexpr bool IsKnownCompressionCodec(uint16_t codec) noexcept {
        return codec == static_cast<uint16_t>(CompressionCodec::None);
    }

    /** @brief Return whether @p flags contains only supported resource flags. */
    [[nodiscard]] constexpr bool HasOnlyKnownResourceFlags(uint16_t flags) noexcept {
        return flags == static_cast<uint16_t>(ResourceFlags::None);
    }
    /** @brief File header for `.lpak`. Every field is little-endian on disk and independent of C++ object padding. */
    struct FileHeader {
        uint32_t magic = kLpakMagic;
        uint16_t major = kLpakMajor;
        uint16_t minor = kLpakMinor;
        uint16_t headerSize = 0;
        uint16_t sectionCount = 0;
        uint16_t sectionSize = 0;
        uint16_t entrySize = 0;
        uint16_t layout = static_cast<uint16_t>(IndexLayout::Sorted);
        uint16_t reserved0 = 0;
        uint32_t flags = 0;
        uint64_t fileSize = 0;
        uint64_t sectionTableOffset = 0;
        uint64_t resourceCount = 0;
        uint64_t metadataHash = 0;
        uint64_t reserved1 = 0;
    };

    /** @brief Section descriptor for `.lpak`. Every field is little-endian on disk. */
    struct SectionDescriptor {
        uint32_t kind = 0;
        uint32_t flags = 0;
        uint32_t alignment = 1;
        uint32_t reserved = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t checksum = 0;
    };

    /** @brief Resource entry descriptor for `.lpak`. Every field is little-endian on disk. */
    struct ResourceEntry {
        uint64_t semanticHash = 0;
        uint64_t contentHash = 0;
        uint64_t payloadOffset = 0;
        uint64_t packedSize = 0;
        uint64_t unpackedSize = 0;
        uint32_t uriOffset = 0;
        uint32_t uriSize = 0;
        uint16_t type = static_cast<uint16_t>(ResourceType::Unknown);
        uint16_t codec = static_cast<uint16_t>(CompressionCodec::None);
        uint16_t flags = static_cast<uint16_t>(ResourceFlags::None);
        uint16_t alignmentLog2 = 0;
    };

    static_assert(std::is_trivially_copyable_v<FileHeader> && sizeof(FileHeader) == kFileHeaderWireSize);
    static_assert(std::is_trivially_copyable_v<SectionDescriptor> &&
                  sizeof(SectionDescriptor) == kSectionDescriptorWireSize);
    static_assert(std::is_trivially_copyable_v<ResourceEntry> && sizeof(ResourceEntry) == kResourceEntryWireSize);

} // namespace Sora::Resources
