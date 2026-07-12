/**
 * @file PakView.h
 * @brief Zero-copy `.lpak` reader with explicit sorted-index lookup.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Core/MemoryLayout.h>
#include <Sora/Core/Wire.h>
#include <Sora/ErrorCode.h>
#include <Sora/Core/Resources/Format.h>
#include <Sora/Core/Resources/ResourceId.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace Sora::Resources {

    static_assert(Wire::SizeOf<FileHeader>() == kFileHeaderWireSize);
    static_assert(Wire::SizeOf<SectionDescriptor>() == kSectionDescriptorWireSize);
    static_assert(Wire::SizeOf<ResourceEntry>() == kResourceEntryWireSize);

    /** @brief Read-only view into an `.lpak` byte image. */
    class PakView {
        std::span<const std::byte> bytes_{};
        FileHeader header_{};
        SectionDescriptor entriesSection_{};
        SectionDescriptor stringsSection_{};
        SectionDescriptor dataSection_{};
        std::vector<ResourceEntry> entries_{};
        std::vector<uint64_t> hashes_{};

        [[nodiscard]] auto SectionBytes(const SectionDescriptor& section) const -> Result<std::span<const std::byte>> {
            if (section.offset > bytes_.size() || section.size > bytes_.size() - section.offset) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            return bytes_.subspan(static_cast<size_t>(section.offset), static_cast<size_t>(section.size));
        }

        [[nodiscard]] auto ValidateSections(std::span<const SectionDescriptor> sections) const -> VoidResult {
            const auto tableSize = static_cast<uint64_t>(Wire::SizeOf<SectionDescriptor>()) * header_.sectionCount;
            const auto tableOffset = header_.sectionTableOffset;
            if (tableOffset < header_.headerSize || tableOffset > bytes_.size() ||
                tableSize > bytes_.size() - tableOffset) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }

            for (const auto& section : sections) {
                if (!Sora::IsValidAlignment(section.alignment) || !Sora::IsAligned(section.offset, section.alignment) ||
                    section.offset > bytes_.size() || section.size > bytes_.size() - section.offset) {
                    return std::unexpected(ErrorCode::ResourceCorrupted);
                }
            }

            for (size_t i = 0; i < sections.size(); ++i) {
                const auto& a = sections[i];
                if (Sora::RangesOverlap(a.offset, a.size, tableOffset, tableSize)) {
                    return std::unexpected(ErrorCode::ResourceCorrupted);
                }
                for (size_t j = i + 1; j < sections.size(); ++j) {
                    const auto& b = sections[j];
                    if (Sora::RangesOverlap(a.offset, a.size, b.offset, b.size)) {
                        return std::unexpected(ErrorCode::ResourceCorrupted);
                    }
                }
            }
            return {};
        }

        [[nodiscard]] auto ValidateEntry(const ResourceEntry& entry, std::span<const std::byte> strings) const
            -> VoidResult {
            if (!IsKnownResourceType(entry.type) || !IsKnownCompressionCodec(entry.codec) ||
                !HasOnlyKnownResourceFlags(entry.flags)) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (entry.packedSize != entry.unpackedSize) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (entry.uriOffset > strings.size() || entry.uriSize >= strings.size() - entry.uriOffset) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (strings[entry.uriOffset + entry.uriSize] != std::byte{0}) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (entry.payloadOffset < dataSection_.offset || entry.payloadOffset > bytes_.size() ||
                entry.packedSize > bytes_.size() - entry.payloadOffset ||
                entry.payloadOffset + entry.packedSize > dataSection_.offset + dataSection_.size) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }

            const auto uri = std::string_view{reinterpret_cast<const char*>(strings.data() + entry.uriOffset),
                                              static_cast<size_t>(entry.uriSize)};
            auto resourceUri = ParseResourceUri(uri);
            if (!resourceUri || resourceUri->Hash() != entry.semanticHash) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            return {};
        }

        [[nodiscard]] static constexpr bool SameEntry(const ResourceEntry& a, const ResourceEntry& b) noexcept {
            return a.semanticHash == b.semanticHash && a.contentHash == b.contentHash &&
                   a.payloadOffset == b.payloadOffset && a.packedSize == b.packedSize &&
                   a.unpackedSize == b.unpackedSize && a.uriOffset == b.uriOffset && a.uriSize == b.uriSize &&
                   a.type == b.type && a.codec == b.codec && a.flags == b.flags && a.alignmentLog2 == b.alignmentLog2;
        }

        /**
         * @brief Return the package metadata hash.
         * @details Payload bytes are excluded and @ref FileHeader::metadataHash is treated as zero.
         */
        [[nodiscard]] auto MetadataHash() const -> Result<uint64_t> {
            auto header = header_;
            header.metadataHash = 0;
            std::vector<std::byte> headerBytes;
            Wire::Append(headerBytes, header);

            const auto tableSize = static_cast<size_t>(header_.sectionSize) * header_.sectionCount;
            if (header_.sectionTableOffset > bytes_.size() || tableSize > bytes_.size() - header_.sectionTableOffset) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            auto entries = SectionBytes(entriesSection_);
            auto strings = SectionBytes(stringsSection_);
            if (!entries) {
                return std::unexpected(entries.error());
            }
            if (!strings) {
                return std::unexpected(strings.error());
            }

            auto hasher = Sora::Hashing::Hasher<>{};
            hasher.FeedBytes(headerBytes);
            hasher.FeedBytes(bytes_.subspan(static_cast<size_t>(header_.sectionTableOffset), tableSize));
            hasher.FeedBytes(*entries);
            hasher.FeedBytes(*strings);
            return hasher.Finalize();
        }

        /** @brief Return payload bytes for an entry already proven to belong to this package. */
        [[nodiscard]] auto TrustedDataOf(const ResourceEntry& entry) const -> Result<std::span<const std::byte>> {
            if (entry.payloadOffset < dataSection_.offset || entry.payloadOffset > bytes_.size() ||
                entry.packedSize > bytes_.size() - entry.payloadOffset ||
                entry.payloadOffset + entry.packedSize > dataSection_.offset + dataSection_.size) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            return bytes_.subspan(static_cast<size_t>(entry.payloadOffset), static_cast<size_t>(entry.packedSize));
        }

    public:
        /** @brief Parse and validate an `.lpak` byte image. */
        [[nodiscard]] static auto Open(std::span<const std::byte> bytes) -> Result<PakView> {
            PakView view;
            view.bytes_ = bytes;

            // 1. Load and validate the file header.
            size_t offset = 0;
            auto header = Wire::Read<FileHeader>(bytes, offset);
            if (!header) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            view.header_ = *header;

            if (view.header_.magic != kLpakMagic || view.header_.major != kLpakMajor ||
                view.header_.headerSize != Wire::SizeOf<FileHeader>() ||
                view.header_.sectionSize != Wire::SizeOf<SectionDescriptor>() ||
                view.header_.entrySize != Wire::SizeOf<ResourceEntry>()) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (view.header_.fileSize != bytes.size() || view.header_.sectionCount == 0 ||
                view.header_.sectionCount > std::numeric_limits<uint16_t>::max() / view.header_.sectionSize) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (static_cast<IndexLayout>(view.header_.layout) != IndexLayout::Sorted) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }

            bool hasEntries = false;
            bool hasStrings = false;
            bool hasData = false;
            std::vector<SectionDescriptor> sections;
            sections.reserve(view.header_.sectionCount);

            offset = static_cast<size_t>(view.header_.sectionTableOffset);
            for (uint16_t i = 0; i < view.header_.sectionCount; ++i) {
                auto section = Wire::Read<SectionDescriptor>(bytes, offset);
                if (!section) {
                    return std::unexpected(ErrorCode::ResourceCorrupted);
                }
                switch (static_cast<SectionKind>(section->kind)) {
                    case SectionKind::Entries:
                        if (hasEntries) {
                            return std::unexpected(ErrorCode::ResourceCorrupted);
                        }
                        if (auto sectionBytes = view.SectionBytes(*section);
                            !sectionBytes || Sora::Hashing::HashByteRange(*sectionBytes) != section->checksum) {
                            return std::unexpected(ErrorCode::ResourceCorrupted);
                        }
                        view.entriesSection_ = *section;
                        hasEntries = true;
                        break;
                    case SectionKind::Strings:
                        if (hasStrings) {
                            return std::unexpected(ErrorCode::ResourceCorrupted);
                        }
                        if (auto sectionBytes = view.SectionBytes(*section);
                            !sectionBytes || Sora::Hashing::HashByteRange(*sectionBytes) != section->checksum) {
                            return std::unexpected(ErrorCode::ResourceCorrupted);
                        }
                        view.stringsSection_ = *section;
                        hasStrings = true;
                        break;
                    case SectionKind::Data:
                        if (hasData) {
                            return std::unexpected(ErrorCode::ResourceCorrupted);
                        }
                        if (section->checksum != 0) {
                            return std::unexpected(ErrorCode::ResourceCorrupted);
                        }
                        view.dataSection_ = *section;
                        hasData = true;
                        break;
                    default:
                        break;
                }
                sections.push_back(*section);
            }

            if (!hasEntries || !hasStrings || !hasData) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            auto sectionValidation = view.ValidateSections(sections);
            if (!sectionValidation) {
                return std::unexpected(sectionValidation.error());
            }

            auto metadataHash = view.MetadataHash();
            if (!metadataHash || *metadataHash != view.header_.metadataHash) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (view.entriesSection_.size == 0 && view.header_.resourceCount != 0) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }

            auto entryBytes = view.SectionBytes(view.entriesSection_);
            if (!entryBytes) {
                return std::unexpected(entryBytes.error());
            }
            auto stringBytes = view.SectionBytes(view.stringsSection_);
            if (!stringBytes) {
                return std::unexpected(stringBytes.error());
            }
            constexpr auto entrySize = Wire::SizeOf<ResourceEntry>();
            if (view.header_.resourceCount > entryBytes->size() / entrySize ||
                entryBytes->size() != static_cast<size_t>(view.header_.resourceCount) * entrySize) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }

            size_t entryOffset = 0;
            view.entries_.reserve(static_cast<size_t>(view.header_.resourceCount));
            view.hashes_.reserve(static_cast<size_t>(view.header_.resourceCount));
            for (uint64_t i = 0; i < view.header_.resourceCount; ++i) {
                auto entry = Wire::Read<ResourceEntry>(*entryBytes, entryOffset);
                if (!entry) {
                    return std::unexpected(ErrorCode::ResourceCorrupted);
                }
                if (i > 0 && view.entries_.back().semanticHash >= entry->semanticHash) {
                    return std::unexpected(ErrorCode::ResourceCorrupted);
                }
                auto entryValidation = view.ValidateEntry(*entry, *stringBytes);
                if (!entryValidation) {
                    return std::unexpected(entryValidation.error());
                }
                view.hashes_.push_back(entry->semanticHash);
                view.entries_.push_back(*entry);
            }
            return view;
        }

        /** @brief Number of resource entries. */
        [[nodiscard]] auto Count() const noexcept -> size_t { return entries_.size(); }

        /** @brief File header. */
        [[nodiscard]] auto Header() const noexcept -> const FileHeader& { return header_; }

        /** @brief Entry descriptors. */
        [[nodiscard]] auto Entries() const noexcept -> std::span<const ResourceEntry> { return entries_; }

        /** @brief Find an entry descriptor by semantic hash. */
        [[nodiscard]] auto Find(uint64_t hash) const noexcept -> const ResourceEntry* {
            auto it = std::lower_bound(hashes_.begin(), hashes_.end(), hash);
            if (it == hashes_.end() || *it != hash) {
                return nullptr;
            }
            return &entries_[static_cast<size_t>(it - hashes_.begin())];
        }

        /** @brief Return payload bytes for @p entry. */
        [[nodiscard]] auto DataOf(const ResourceEntry& entry) const -> Result<std::span<const std::byte>> {
            const auto* canonical = Find(entry.semanticHash);
            if (canonical == nullptr || !SameEntry(*canonical, entry)) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            return TrustedDataOf(*canonical);
        }

        /** @brief Return resource payload by semantic hash. */
        [[nodiscard]] auto Get(uint64_t hash) const -> Result<std::span<const std::byte>> {
            auto* entry = Find(hash);
            if (!entry) {
                return std::unexpected(ErrorCode::ResourceNotFound);
            }
            return TrustedDataOf(*entry);
        }

        /** @brief Return the canonical URI string of @p entry. */
        [[nodiscard]] auto UriOf(const ResourceEntry& entry) const -> Result<std::string_view> {
            auto strings = SectionBytes(stringsSection_);
            if (!strings) {
                return std::unexpected(strings.error());
            }
            const auto* canonical = Find(entry.semanticHash);
            if (canonical == nullptr || !SameEntry(*canonical, entry)) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (entry.uriOffset > strings->size() || entry.uriSize > strings->size() - entry.uriOffset) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            auto* ptr = reinterpret_cast<const char*>(strings->data() + entry.uriOffset);
            return std::string_view{ptr, entry.uriSize};
        }
    };

} // namespace Sora::Resources
