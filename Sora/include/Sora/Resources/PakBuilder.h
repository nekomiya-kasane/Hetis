/**
 * @file PakBuilder.h
 * @brief `.lpak` package construction from embedded or runtime resources.
 * @ingroup Resources
 */
#pragma once

#include "Sora/Core/Hash.h"
#include <Sora/ErrorCode.h>
#include <Sora/Resources/EmbeddedResource.h>
#include <Sora/Resources/Format.h>
#include <Sora/Resources/Wire.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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

        [[nodiscard]] static auto NormalizeUri(std::string_view uri) -> Result<std::string> {
            if (uri.empty() || uri.size() > std::numeric_limits<uint32_t>::max()) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            std::string normalized;
            normalized.reserve(uri.size());
            for (char c : uri) {
                if (c == '\0') {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
                normalized.push_back(c == '\\' ? '/' : c);
            }
            if (!ParseResourceUri(normalized).has_value()) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            return normalized;
        }

        [[nodiscard]] static constexpr auto CheckedAdd(uint64_t a, uint64_t b) -> Result<uint64_t> {
            if (b > std::numeric_limits<uint64_t>::max() - a) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            return a + b;
        }

        [[nodiscard]] static constexpr auto CheckedAlignUp(uint64_t value, uint64_t alignment) -> Result<uint64_t> {
            if (alignment == 0) {
                return value;
            }
            auto biased = CheckedAdd(value, alignment - 1);
            if (!biased) {
                return std::unexpected(biased.error());
            }
            return (*biased / alignment) * alignment;
        }

        [[nodiscard]] static constexpr auto ToSize(uint64_t value) -> Result<size_t> {
            if (value > std::numeric_limits<size_t>::max()) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            return static_cast<size_t>(value);
        }

    public:
        /** @brief Add a resource. The payload is copied into the builder. */
        auto Add(std::string_view uri, ResourceType type, std::span<const std::byte> bytes) -> VoidResult {
            auto normalized = NormalizeUri(uri);
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

        /** @brief Add an embedded resource. */
        auto Add(EmbeddedResourceView resource) -> VoidResult {
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

            std::sort(built.begin(), built.end(), [](const BuiltResource& a, const BuiltResource& b) {
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
                    return std::unexpected(aligned.error());
                }
                auto alignedSize = ToSize(*aligned);
                if (!alignedSize) {
                    return std::unexpected(alignedSize.error());
                }
                data.resize(*alignedSize, std::byte{0});
                r.entry.dataOffset = data.size();
                data.insert_range(data.end(), r.bytes);
            }

            std::vector<std::byte> entries;
            for (const auto& r : built) {
                Wire::Append(entries, r.entry);
            }

            constexpr auto headerSize = Wire::SizeOf<FileHeader>();
            constexpr auto sectionSize = Wire::SizeOf<SectionDescriptor>();
            constexpr uint16_t sectionCount = 3;

            const uint64_t sectionTableOffset = headerSize;
            auto sectionTableEnd = CheckedAdd(sectionTableOffset, sectionSize * sectionCount);
            if (!sectionTableEnd) {
                return std::unexpected(sectionTableEnd.error());
            }
            auto entriesOffset = CheckedAlignUp(*sectionTableEnd, 8);
            if (!entriesOffset) {
                return std::unexpected(entriesOffset.error());
            }
            auto entriesEnd = CheckedAdd(*entriesOffset, entries.size());
            if (!entriesEnd) {
                return std::unexpected(entriesEnd.error());
            }
            auto stringsOffset = CheckedAlignUp(*entriesEnd, 8);
            if (!stringsOffset) {
                return std::unexpected(stringsOffset.error());
            }
            auto stringsEnd = CheckedAdd(*stringsOffset, strings.size());
            if (!stringsEnd) {
                return std::unexpected(stringsEnd.error());
            }
            auto dataOffset = CheckedAlignUp(*stringsEnd, kDefaultDataAlignment);
            if (!dataOffset) {
                return std::unexpected(dataOffset.error());
            }
            auto fileSize = CheckedAdd(*dataOffset, data.size());
            if (!fileSize) {
                return std::unexpected(fileSize.error());
            }
            auto fileCapacity = ToSize(*fileSize);
            if (!fileCapacity) {
                return std::unexpected(fileCapacity.error());
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
                .checksum = Sora::Hashing::HashByteRange(data),
            };

            FileHeader header{
                .headerSize = static_cast<uint16_t>(headerSize),
                .sectionCount = sectionCount,
                .layout = static_cast<uint16_t>(IndexLayout::Sorted),
                .fileSize = *fileSize,
                .sectionTableOffset = sectionTableOffset,
                .resourceCount = built.size(),
            };

            std::vector<std::byte> file;
            file.reserve(*fileCapacity);
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
            if (file.size() != *fileSize) {
                return std::unexpected(ErrorCode::InvalidState);
            }

            header.headerHash = 0;
            header.fileHash = 0;
            std::vector<std::byte> headerBytes;
            Wire::Append(headerBytes, header);
            header.headerHash = Sora::Hashing::HashByteRange(headerBytes);
            auto writeHeader = Wire::WriteAt(file, 0, header);
            if (!writeHeader) {
                return std::unexpected(writeHeader.error());
            }
            header.fileHash = Sora::Hashing::HashByteRange(file);
            writeHeader = Wire::WriteAt(file, 0, header);
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
