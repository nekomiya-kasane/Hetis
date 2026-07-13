/**
 * @file PakLayout.h
 * @brief Shared canonical `.lpak` layout and wire-writing primitives.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Core/Hash.h>
#include <Sora/Core/Memory/MemoryLayout.h>
#include <Sora/Core/Wire.h>
#include <Sora/Core/Resources/Format.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace Sora::Resources::Detail {

    /** @brief Number of sections in the canonical `.lpak` image. */
    inline constexpr uint16_t kPakSectionCount = 3;

    /** @brief Metadata section alignment used by entries and strings. */
    inline constexpr uint64_t kPakMetadataAlignment = 8;

    /** @brief Encoded payload alignment used by canonical uncompressed payload entries. */
    inline constexpr uint16_t kPakPayloadAlignmentLog2 = Sora::Log2OfPowerOfTwo(kDefaultDataAlignment);

    static_assert(kPakPayloadAlignmentLog2 <= std::numeric_limits<uint16_t>::max());

    /** @brief Canonical byte offsets and section sizes for one `.lpak` image. */
    struct PakLayout {
        uint64_t resourceCount = 0;                             /**< Number of resource entries. */
        uint64_t sectionTableOffset = Wire::SizeOf<FileHeader>(); /**< Absolute section-table offset. */
        uint64_t entriesOffset = 0;                             /**< Absolute entry section offset. */
        uint64_t entriesSize = 0;                               /**< Entry section byte size. */
        uint64_t stringsOffset = 0;                             /**< Absolute URI string-section offset. */
        uint64_t stringsSize = 0;                               /**< URI string-section byte size. */
        uint64_t dataOffset = 0;                                /**< Absolute payload section offset. */
        uint64_t dataSize = 0;                                  /**< Payload section byte size. */
        uint64_t fileSize = 0;                                  /**< Total image byte size. */
    };

    /** @brief Add two 64-bit byte counts without wraparound. */
    [[nodiscard]] constexpr std::optional<uint64_t> CheckedAdd(uint64_t a, uint64_t b) noexcept {
        if (b > std::numeric_limits<uint64_t>::max() - a) {
            return std::nullopt;
        }
        return a + b;
    }

    /** @brief Multiply two 64-bit byte counts without wraparound. */
    [[nodiscard]] constexpr std::optional<uint64_t> CheckedMul(uint64_t a, uint64_t b) noexcept {
        if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
            return std::nullopt;
        }
        return a * b;
    }

    /** @brief Stage one NUL-terminated URI in the string table and return its section-local offset. */
    [[nodiscard]] constexpr std::optional<uint32_t> PlacePakUri(uint64_t& stringsSize, uint64_t uriSize) noexcept {
        constexpr uint64_t maxOffset = std::numeric_limits<uint32_t>::max();
        if (stringsSize > maxOffset || uriSize > maxOffset) {
            return std::nullopt;
        }
        auto withText = CheckedAdd(stringsSize, uriSize);
        if (!withText) {
            return std::nullopt;
        }
        auto withTerminator = CheckedAdd(*withText, 1);
        if (!withTerminator || *withTerminator > maxOffset) {
            return std::nullopt;
        }
        const uint32_t offset = static_cast<uint32_t>(stringsSize);
        stringsSize = *withTerminator;
        return offset;
    }

    /** @brief Stage one payload in the data section and return its section-local aligned offset. */
    [[nodiscard]] constexpr std::optional<uint64_t> PlacePakPayload(uint64_t& dataSize,
                                                                    uint64_t payloadSize) noexcept {
        auto offset = Sora::TryAlignUp(dataSize, kDefaultDataAlignment);
        if (!offset) {
            return std::nullopt;
        }
        auto end = CheckedAdd(*offset, payloadSize);
        if (!end) {
            return std::nullopt;
        }
        dataSize = *end;
        return *offset;
    }

    /** @brief Compute canonical `.lpak` section offsets from precomputed string and payload section sizes. */
    [[nodiscard]] constexpr std::optional<PakLayout> MakePakLayout(uint64_t resourceCount, uint64_t stringsSize,
                                                                   uint64_t dataSize) noexcept {
        if (resourceCount == 0) {
            return std::nullopt;
        }

        constexpr uint64_t headerSize = Wire::SizeOf<FileHeader>();
        constexpr uint64_t sectionSize = Wire::SizeOf<SectionDescriptor>();
        constexpr uint64_t entrySize = Wire::SizeOf<ResourceEntry>();
        static_assert(headerSize == kFileHeaderWireSize);
        static_assert(sectionSize == kSectionDescriptorWireSize);
        static_assert(entrySize == kResourceEntryWireSize);

        auto entriesSize = CheckedMul(entrySize, resourceCount);
        auto sectionTableSize = CheckedMul(sectionSize, kPakSectionCount);
        if (!entriesSize || !sectionTableSize) {
            return std::nullopt;
        }

        auto sectionTableEnd = CheckedAdd(headerSize, *sectionTableSize);
        if (!sectionTableEnd) {
            return std::nullopt;
        }
        auto entriesOffset = Sora::TryAlignUp(*sectionTableEnd, kPakMetadataAlignment);
        if (!entriesOffset) {
            return std::nullopt;
        }
        auto entriesEnd = CheckedAdd(*entriesOffset, *entriesSize);
        if (!entriesEnd) {
            return std::nullopt;
        }
        auto stringsOffset = Sora::TryAlignUp(*entriesEnd, kPakMetadataAlignment);
        if (!stringsOffset) {
            return std::nullopt;
        }
        auto stringsEnd = CheckedAdd(*stringsOffset, stringsSize);
        if (!stringsEnd) {
            return std::nullopt;
        }
        auto dataOffset = Sora::TryAlignUp(*stringsEnd, kDefaultDataAlignment);
        if (!dataOffset) {
            return std::nullopt;
        }
        auto fileSize = CheckedAdd(*dataOffset, dataSize);
        if (!fileSize || *fileSize == 0) {
            return std::nullopt;
        }

        return PakLayout{.resourceCount = resourceCount,
                         .sectionTableOffset = headerSize,
                         .entriesOffset = *entriesOffset,
                         .entriesSize = *entriesSize,
                         .stringsOffset = *stringsOffset,
                         .stringsSize = stringsSize,
                         .dataOffset = *dataOffset,
                         .dataSize = dataSize,
                         .fileSize = *fileSize};
    }

    /** @brief Build a canonical resource entry from section-local URI and payload offsets. */
    [[nodiscard]] constexpr ResourceEntry MakePakEntry(uint64_t semanticHash, uint64_t contentHash,
                                                       uint64_t payloadOffset, uint64_t size, uint32_t uriOffset,
                                                       uint32_t uriSize, ResourceType type) noexcept {
        return ResourceEntry{.semanticHash = semanticHash,
                             .contentHash = contentHash,
                             .payloadOffset = payloadOffset,
                             .packedSize = size,
                             .unpackedSize = size,
                             .uriOffset = uriOffset,
                             .uriSize = uriSize,
                             .type = static_cast<uint16_t>(type),
                             .codec = static_cast<uint16_t>(CompressionCodec::None),
                             .flags = static_cast<uint16_t>(ResourceFlags::None),
                             .alignmentLog2 = kPakPayloadAlignmentLog2};
    }

    /** @brief Build the canonical header with @p metadataHash already applied. */
    [[nodiscard]] constexpr FileHeader MakePakHeader(const PakLayout& layout, uint64_t metadataHash = 0) noexcept {
        return FileHeader{.magic = kLpakMagic,
                          .major = kLpakMajor,
                          .minor = kLpakMinor,
                          .headerSize = static_cast<uint16_t>(Wire::SizeOf<FileHeader>()),
                          .sectionCount = kPakSectionCount,
                          .sectionSize = static_cast<uint16_t>(Wire::SizeOf<SectionDescriptor>()),
                          .entrySize = static_cast<uint16_t>(Wire::SizeOf<ResourceEntry>()),
                          .layout = static_cast<uint16_t>(IndexLayout::Sorted),
                          .reserved0 = 0,
                          .flags = 0,
                          .fileSize = layout.fileSize,
                          .sectionTableOffset = layout.sectionTableOffset,
                          .resourceCount = layout.resourceCount,
                          .metadataHash = metadataHash,
                          .reserved1 = 0};
    }

    /** @brief Build the entry section descriptor. */
    [[nodiscard]] constexpr SectionDescriptor MakePakEntriesSection(const PakLayout& layout,
                                                                    uint64_t checksum) noexcept {
        return SectionDescriptor{.kind = static_cast<uint32_t>(SectionKind::Entries),
                                 .flags = 0,
                                 .alignment = static_cast<uint32_t>(kPakMetadataAlignment),
                                 .reserved = 0,
                                 .offset = layout.entriesOffset,
                                 .size = layout.entriesSize,
                                 .checksum = checksum};
    }

    /** @brief Build the URI string section descriptor. */
    [[nodiscard]] constexpr SectionDescriptor MakePakStringsSection(const PakLayout& layout,
                                                                    uint64_t checksum) noexcept {
        return SectionDescriptor{.kind = static_cast<uint32_t>(SectionKind::Strings),
                                 .flags = 0,
                                 .alignment = static_cast<uint32_t>(kPakMetadataAlignment),
                                 .reserved = 0,
                                 .offset = layout.stringsOffset,
                                 .size = layout.stringsSize,
                                 .checksum = checksum};
    }

    /** @brief Build the payload data section descriptor. */
    [[nodiscard]] constexpr SectionDescriptor MakePakDataSection(const PakLayout& layout) noexcept {
        return SectionDescriptor{.kind = static_cast<uint32_t>(SectionKind::Data),
                                 .flags = 0,
                                 .alignment = static_cast<uint32_t>(kDefaultDataAlignment),
                                 .reserved = 0,
                                 .offset = layout.dataOffset,
                                 .size = layout.dataSize,
                                 .checksum = 0};
    }

    /** @brief Return a byte span for one package region. */
    template<typename Byte>
    [[nodiscard]] constexpr std::span<Byte> PakRegion(std::span<Byte> bytes, uint64_t offset, uint64_t size) noexcept {
        return bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
    }

    /** @brief Return the entry section bytes of @p bytes. */
    template<typename Byte>
    [[nodiscard]] constexpr auto PakEntriesRegion(std::span<Byte> bytes, const PakLayout& layout) noexcept {
        return PakRegion(bytes, layout.entriesOffset, layout.entriesSize);
    }

    /** @brief Return the URI string section bytes of @p bytes. */
    template<typename Byte>
    [[nodiscard]] constexpr auto PakStringsRegion(std::span<Byte> bytes, const PakLayout& layout) noexcept {
        return PakRegion(bytes, layout.stringsOffset, layout.stringsSize);
    }

    /** @brief Convert one byte-like value to @c std::byte. */
    template<typename Byte>
    [[nodiscard]] constexpr std::byte ToStdByte(Byte byte) noexcept {
        if constexpr (std::same_as<std::remove_cv_t<Byte>, std::byte>) {
            return byte;
        } else {
            return static_cast<std::byte>(static_cast<unsigned char>(byte));
        }
    }

    /** @brief Feed a byte-like range to an incremental hash state. */
    template<typename State, typename Byte>
    constexpr void FeedPakBytes(State& state, std::span<const Byte> bytes) noexcept {
        for (Byte byte : bytes) {
            state.FeedByte(ToStdByte(byte));
        }
    }

    /** @brief Hash the metadata regions covered by @ref FileHeader::metadataHash. */
    template<typename Byte>
    [[nodiscard]] constexpr uint64_t HashPakMetadata(std::span<const Byte> bytes, const PakLayout& layout) noexcept {
        auto state = Sora::Hashing::Fnv1a64State::Seed();
        FeedPakBytes(state, PakRegion(bytes, 0, Wire::SizeOf<FileHeader>()));
        FeedPakBytes(state, PakRegion(bytes, layout.sectionTableOffset,
                                      Wire::SizeOf<SectionDescriptor>() * kPakSectionCount));
        FeedPakBytes(state, PakEntriesRegion(bytes, layout));
        FeedPakBytes(state, PakStringsRegion(bytes, layout));
        return state.Finalize();
    }

    /** @brief Write the package header and section table with a zero metadata hash. */
    template<typename Byte>
    constexpr void WritePakPreambleUnchecked(std::span<Byte> bytes, const PakLayout& layout,
                                             uint64_t entriesChecksum, uint64_t stringsChecksum) {
        Wire::WriteUnchecked(bytes, 0, MakePakHeader(layout));
        Wire::WriteUnchecked(bytes, static_cast<size_t>(layout.sectionTableOffset),
                             MakePakEntriesSection(layout, entriesChecksum));
        Wire::WriteUnchecked(bytes, static_cast<size_t>(layout.sectionTableOffset + Wire::SizeOf<SectionDescriptor>()),
                             MakePakStringsSection(layout, stringsChecksum));
        Wire::WriteUnchecked(bytes,
                             static_cast<size_t>(layout.sectionTableOffset + Wire::SizeOf<SectionDescriptor>() * 2u),
                             MakePakDataSection(layout));
    }

    /** @brief Write one resource entry at @p entryIndex. */
    template<typename Byte>
    constexpr void WritePakEntryUnchecked(std::span<Byte> bytes, const PakLayout& layout, size_t entryIndex,
                                          const ResourceEntry& entry) {
        const uint64_t offset = layout.entriesOffset + Wire::SizeOf<ResourceEntry>() * entryIndex;
        Wire::WriteUnchecked(bytes, static_cast<size_t>(offset), entry);
    }

    /** @brief Recompute and write @ref FileHeader::metadataHash after metadata bytes have been written. */
    template<typename Byte>
    constexpr void FinalizePakHeaderUnchecked(std::span<Byte> bytes, const PakLayout& layout) {
        const uint64_t metadataHash = HashPakMetadata(std::span<const Byte>{bytes.data(), bytes.size()}, layout);
        Wire::WriteUnchecked(bytes, 0, MakePakHeader(layout, metadataHash));
    }

} // namespace Sora::Resources::Detail
