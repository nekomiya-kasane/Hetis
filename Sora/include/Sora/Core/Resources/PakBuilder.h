/**
 * @file PakBuilder.h
 * @brief `.lpak` package construction from runtime or borrowed resource bytes.
 * @ingroup Resources
 */
#pragma once

#include "Sora/Core/Hash.h"
#include <Sora/ErrorCode.h>
#include <Sora/Core/Resources/Format.h>
#include <Sora/Core/Resources/PakLayout.h>
#include <Sora/Core/Resources/ResourceBytes.h>
#include <Sora/Core/Resources/ResourceId.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
                uint64_t semanticHash = 0;
                uint64_t contentHash = 0;
                uint64_t payloadOffset = 0;
                uint32_t uriOffset = 0;
                uint32_t uriSize = 0;
                ResourceType type = ResourceType::Raw;
                std::string_view uri{};
                std::span<const std::byte> bytes{};
            };

            std::vector<BuiltResource> built;
            built.reserve(resources_.size());

            for (const auto& r : resources_) {
                built.push_back(BuiltResource{.semanticHash = HashUri(r.uri),
                                              .contentHash = Sora::Hashing::HashByteRange(r.bytes),
                                              .type = r.type,
                                              .uri = r.uri,
                                              .bytes = r.bytes});
            }

            std::ranges::sort(built, [](const BuiltResource& a, const BuiltResource& b) {
                return a.semanticHash < b.semanticHash;
            });

            for (size_t i = 1; i < built.size(); ++i) {
                if (built[i - 1].semanticHash == built[i].semanticHash) {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
            }

            uint64_t stringsSize = 0;
            uint64_t dataSize = 0;
            for (auto& r : built) {
                auto uriOffset = Detail::PlacePakUri(stringsSize, r.uri.size());
                auto payloadOffset = Detail::PlacePakPayload(dataSize, r.bytes.size());
                if (!uriOffset || !payloadOffset) {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
                r.uriOffset = *uriOffset;
                r.uriSize = static_cast<uint32_t>(r.uri.size());
                r.payloadOffset = *payloadOffset;
            }

            auto layout = Detail::MakePakLayout(built.size(), stringsSize, dataSize);
            if (!layout || layout->fileSize > std::numeric_limits<size_t>::max()) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }

            std::vector<std::byte> file(static_cast<size_t>(layout->fileSize), std::byte{0});
            auto bytes = std::span<std::byte>{file};
            for (size_t i = 0; i < built.size(); ++i) {
                const auto& r = built[i];
                const auto entry = Detail::MakePakEntry(r.semanticHash, r.contentHash,
                                                        layout->dataOffset + r.payloadOffset, r.bytes.size(),
                                                        r.uriOffset, r.uriSize, r.type);
                Detail::WritePakEntryUnchecked(bytes, *layout, i, entry);

                auto uriOut = Detail::PakRegion(bytes, layout->stringsOffset + r.uriOffset, r.uriSize);
                for (size_t j = 0; j < r.uri.size(); ++j) {
                    uriOut[j] = static_cast<std::byte>(static_cast<unsigned char>(r.uri[j]));
                }

                auto payloadOut = Detail::PakRegion(bytes, layout->dataOffset + r.payloadOffset, r.bytes.size());
                std::ranges::copy(r.bytes, payloadOut.begin());
            }

            const uint64_t entriesChecksum =
                Sora::Hashing::HashByteRange(Detail::PakEntriesRegion(std::span<const std::byte>{file}, *layout));
            const uint64_t stringsChecksum =
                Sora::Hashing::HashByteRange(Detail::PakStringsRegion(std::span<const std::byte>{file}, *layout));
            Detail::WritePakPreambleUnchecked(bytes, *layout, entriesChecksum, stringsChecksum);
            Detail::FinalizePakHeaderUnchecked(bytes, *layout);
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
