/**
 * @file PakBuilder.h
 * @brief `.lpak` package construction from runtime or borrowed resource bytes.
 * @ingroup Resources
 */
#pragma once

#include "Sora/Core/Hash.h"
#include <Sora/Core/MemoryLayout.h>
#include <Sora/Core/Wire.h>
#include <Sora/ErrorCode.h>
#include <Sora/Resources/Format.h>
#include <Sora/Resources/ResourceBytes.h>
#include <Sora/Resources/ResourceId.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Sora::Resources {

    /** @brief Mutable builder for `.lpak` packages. */
    class PakBuilder {
        struct PendingResource {
            std::string uri;
            ResourceType type = ResourceType::Raw;
            std::vector<std::byte> bytes;
        };

        std::vector<PendingResource> resources_;

    public:
        /** @brief Add a resource. The payload is copied into the builder. */
        auto Add(std::string_view uri, ResourceType type, std::span<const std::byte> bytes) -> VoidResult {
            auto normalized = NormalizeResourceUri(uri);
            if (!normalized || !IsKnownResourceType(type)) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            resources_.push_back(PendingResource{
                .uri = std::move(*normalized),
                .type = type,
                .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()),
            });
            return {};
        }

        /** @brief Add a borrowed resource byte view. The payload is copied into the builder. */
        auto Add(ResourceBytesView resource) -> VoidResult {
            if (resource.size != 0 && resource.data == nullptr) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            return Add(resource.uri, resource.type, resource.Bytes());
        }

        /** @brief Number of resources currently staged. */
        [[nodiscard]] auto Count() const noexcept -> size_t { return resources_.size(); }

        /** @brief Serialize the package into an owned byte vector. */
        [[nodiscard]] auto Serialize() const -> Result<std::vector<std::byte>> {
            if (resources_.empty()) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            struct BuiltResource {
                ResourceEntry entry{};
                std::string_view uri{};
                std::span<const std::byte> bytes{};
            };

            std::vector<BuiltResource> built;
            built.reserve(resources_.size());

            for (const auto& r : resources_) {
                ResourceEntry entry{};
                entry.semanticHash = HashUri(r.uri);
                entry.contentHash = Sora::Hashing::HashByteRange(r.bytes);
                entry.packedSize = r.bytes.size();
                entry.unpackedSize = r.bytes.size();
                entry.type = static_cast<uint16_t>(r.type);
                entry.codec = static_cast<uint16_t>(CompressionCodec::None);
                built.push_back(BuiltResource{.entry = entry, .uri = r.uri, .bytes = r.bytes});
            }

            std::ranges::sort(built, [](const BuiltResource& a, const BuiltResource& b) {
                return a.entry.semanticHash < b.entry.semanticHash;
            });

            for (size_t i = 1; i < built.size(); ++i) {
                if (built[i - 1].entry.semanticHash == built[i].entry.semanticHash) {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
            }

            std::vector<std::byte> strings;
            for (auto& r : built) {
                if (strings.size() > std::numeric_limits<uint32_t>::max() ||
                    r.uri.size() > std::numeric_limits<uint32_t>::max() ||
                    r.uri.size() + 1 > std::numeric_limits<uint32_t>::max() - strings.size()) {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
                r.entry.uriOffset = static_cast<uint32_t>(strings.size());
                r.entry.uriSize = static_cast<uint32_t>(r.uri.size());
                strings.append_range(r.uri | std::views::transform([](auto c) {
                                         return static_cast<std::byte>(static_cast<unsigned char>(c));
                                     }));
                strings.push_back(std::byte{0});
            }

            std::vector<std::byte> data;
            for (auto& r : built) {
                auto aligned = CheckedAlignUp(data.size(), kDefaultDataAlignment);
                if (!aligned) {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
                data.resize(static_cast<size_t>(*aligned), std::byte{0});
                r.entry.payloadOffset = data.size();
                data.insert_range(data.end(), r.bytes);
            }

            constexpr auto headerSize = Wire::SizeOf<FileHeader>();
            constexpr auto sectionSize = Wire::SizeOf<SectionDescriptor>();
            constexpr auto entrySize = Wire::SizeOf<ResourceEntry>();
            constexpr uint16_t sectionCount = 3;
            static_assert(headerSize == kFileHeaderWireSize);
            static_assert(sectionSize == kSectionDescriptorWireSize);
            static_assert(entrySize == kResourceEntryWireSize);

            auto entriesSize = entrySize * built.size();
            if (!entriesSize) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            const uint64_t sectionTableOffset = headerSize;
            auto sectionTableEnd = sectionTableOffset + sectionSize * sectionCount;
            if (!sectionTableEnd) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            auto entriesOffset = CheckedAlignUp(sectionTableEnd, 8);
            if (!entriesOffset) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            auto entriesEnd = *entriesOffset + entriesSize;
            if (!entriesEnd) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            auto stringsOffset = CheckedAlignUp(entriesEnd, 8);
            if (!stringsOffset) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            auto stringsEnd = *stringsOffset + strings.size();
            if (!stringsEnd) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            auto dataOffset = CheckedAlignUp(stringsEnd, kDefaultDataAlignment);
            if (!dataOffset) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }

            for (auto& r : built) {
                auto absolutePayloadOffset = *dataOffset + r.entry.payloadOffset;
                if (!absolutePayloadOffset) {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
                r.entry.payloadOffset = absolutePayloadOffset;
                r.entry.alignmentLog2 = 12;
            }

            std::vector<std::byte> entries;
            for (const auto& r : built) {
                Wire::Append(entries, r.entry);
            }

            auto fileSize = *dataOffset + data.size();
            if (!fileSize) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            auto fileCapacity = static_cast<size_t>(fileSize);
            if (!fileCapacity) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }

            SectionDescriptor entrySection{
                .kind = static_cast<uint32_t>(SectionKind::Entries),
                .alignment = 8,
                .offset = *entriesOffset,
                .size = entries.size(),
                .checksum = Sora::Hashing::HashByteRange(entries),
            };
            SectionDescriptor stringSection{
                .kind = static_cast<uint32_t>(SectionKind::Strings),
                .alignment = 8,
                .offset = *stringsOffset,
                .size = strings.size(),
                .checksum = Sora::Hashing::HashByteRange(strings),
            };
            SectionDescriptor dataSection{
                .kind = static_cast<uint32_t>(SectionKind::Data),
                .alignment = static_cast<uint32_t>(kDefaultDataAlignment),
                .offset = *dataOffset,
                .size = data.size(),
                .checksum = 0,
            };

            FileHeader header{
                .headerSize = static_cast<uint16_t>(headerSize),
                .sectionCount = sectionCount,
                .sectionSize = static_cast<uint16_t>(sectionSize),
                .entrySize = static_cast<uint16_t>(entrySize),
                .layout = static_cast<uint16_t>(IndexLayout::Sorted),
                .fileSize = static_cast<uint32_t>(fileSize),
                .sectionTableOffset = sectionTableOffset,
                .resourceCount = built.size(),
            };

            std::vector<std::byte> file;
            file.reserve(fileCapacity);
            Wire::Append(file, header);
            Wire::Append(file, entrySection);
            Wire::Append(file, stringSection);
            Wire::Append(file, dataSection);
            file.resize(static_cast<size_t>(*entriesOffset), std::byte{0});
            file.insert(file.end(), entries.begin(), entries.end());
            file.resize(static_cast<size_t>(*stringsOffset), std::byte{0});
            file.insert(file.end(), strings.begin(), strings.end());
            file.resize(static_cast<size_t>(*dataOffset), std::byte{0});
            file.insert(file.end(), data.begin(), data.end());
            if (file.size() != static_cast<size_t>(fileSize)) {
                return std::unexpected(ErrorCode::InvalidState);
            }

            auto hasher = Sora::Hashing::Hasher<>{};
            hasher.FeedBytes(std::span<const std::byte>{file}.subspan(0, headerSize));
            hasher.FeedBytes(std::span<const std::byte>{file}.subspan(static_cast<size_t>(sectionTableOffset),
                                                                      sectionSize * sectionCount));
            hasher.FeedBytes(
                std::span<const std::byte>{file}.subspan(static_cast<size_t>(*entriesOffset), entries.size()));
            hasher.FeedBytes(
                std::span<const std::byte>{file}.subspan(static_cast<size_t>(*stringsOffset), strings.size()));
            header.metadataHash = hasher.Finalize();
            auto writeHeader = Wire::WriteAt(file, 0, header);
            if (!writeHeader) {
                return std::unexpected(writeHeader.error());
            }
            return file;
        }

        /** @brief Serialize and write the package to disk. */
        [[nodiscard]] auto Write(const std::filesystem::path& output) const -> VoidResult {
            auto bytes = Serialize();
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            std::ofstream out(output, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected(ErrorCode::IoError);
            }
            out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            if (!out) {
                return std::unexpected(ErrorCode::IoError);
            }
            return {};
        }
    };

} // namespace Sora::Resources
