/**
 * @file Format.h
 * @brief Sora resource package wire-format declarations.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Core/Flags.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Sora::Resources {

    /** @brief `.lpak` magic, encoded as little-endian bytes `LPAK`. */
    inline constexpr uint32_t kLpakMagic =
        uint32_t{'L'} | (uint32_t{'P'} << 8) | (uint32_t{'A'} << 16) | (uint32_t{'K'} << 24);

    /** @brief Major format version for Sora resource packages. */
    inline constexpr uint16_t kLpakMajor = 1;

    /** @brief Minor format version for Sora resource packages. */
    inline constexpr uint16_t kLpakMinor = 0;

    /** @brief Default payload alignment. */
    inline constexpr uint64_t kDefaultDataAlignment = 4096;

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
               static_cast<uint16_t>(type) <= static_cast<uint16_t>(ResourceType::Data);
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
    /** @brief File header for `.lpak`. Every field is little-endian on disk. */
    struct FileHeader {
        uint32_t magic = kLpakMagic;
        uint16_t major = kLpakMajor;
        uint16_t minor = kLpakMinor;
        uint16_t headerSize = 0;
        uint16_t sectionCount = 0;
        uint16_t layout = static_cast<uint16_t>(IndexLayout::Sorted);
        uint16_t reserved0 = 0;
        uint32_t flags = 0;
        uint64_t fileSize = 0;
        uint64_t sectionTableOffset = 0;
        uint64_t resourceCount = 0;
        uint64_t headerHash = 0;
        uint64_t fileHash = 0;
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
        uint64_t dataOffset = 0;
        uint64_t packedSize = 0;
        uint64_t unpackedSize = 0;
        uint32_t uriOffset = 0;
        uint32_t uriSize = 0;
        uint16_t type = static_cast<uint16_t>(ResourceType::Unknown);
        uint16_t codec = static_cast<uint16_t>(CompressionCodec::None);
        uint16_t flags = static_cast<uint16_t>(ResourceFlags::None);
        uint16_t reserved = 0;
    };

    static_assert(std::is_trivially_copyable_v<FileHeader>);
    static_assert(std::is_trivially_copyable_v<SectionDescriptor>);
    static_assert(std::is_trivially_copyable_v<ResourceEntry>);
    static_assert(std::endian::native == std::endian::little, "Sora currently supports little-endian hosts only.");

} // namespace Sora::Resources
