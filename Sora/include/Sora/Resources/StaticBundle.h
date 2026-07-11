/**
 * @file StaticBundle.h
 * @brief Static resource bundles backed by `#embed` byte arrays.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Resources/EmbeddedResource.h>
#include <Sora/Resources/PakBuilder.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace Sora::Resources {

    /** @brief Fixed-size resource provider over embedded byte arrays. */
    template<size_t N>
    class StaticBundle {
        std::array<EmbeddedResourceView, N> resources_{};

        constexpr void Validate() const {
            for (size_t i = 0; i < N; ++i) {
                auto uri = ParseResourceUri(resources_[i].uri);
                if (!uri || resources_[i].hash != uri->Hash()) {
                    throw "Sora::Resources::StaticBundle: invalid URI or hash.";
                }
                if (resources_[i].size != 0 && resources_[i].data == nullptr) {
                    throw "Sora::Resources::StaticBundle: non-empty resource has null payload.";
                }
                for (size_t j = i + 1; j < N; ++j) {
                    if (resources_[i].hash == resources_[j].hash) {
                        throw "Sora::Resources::StaticBundle: duplicate resource hash.";
                    }
                }
            }
        }

    public:
        /** @brief Construct from embedded resource views. */
        constexpr explicit StaticBundle(std::array<EmbeddedResourceView, N> resources) : resources_(resources) {
            Validate();
            std::ranges::sort(resources_, [](auto a, auto b) { return a.hash < b.hash; });
        }

        /** @brief Number of resources. */
        [[nodiscard]] constexpr auto Count() const noexcept -> size_t { return N; }

        /** @brief Resource descriptors. */
        [[nodiscard]] constexpr auto Resources() const noexcept -> std::span<const EmbeddedResourceView> {
            return resources_;
        }

        /** @brief Return payload bytes by semantic hash. */
        [[nodiscard]] constexpr auto Get(uint64_t hash) const -> Result<std::span<const std::byte>> {
            auto it = std::lower_bound(resources_.begin(), resources_.end(), hash,
                                       [](EmbeddedResourceView r, uint64_t h) { return r.hash < h; });
            if (it == resources_.end() || it->hash != hash) {
                return std::unexpected(ErrorCode::ResourceNotFound);
            }
            return it->Bytes();
        }

        /** @brief Serialize this bundle to an `.lpak` byte vector. */
        [[nodiscard]] auto ToPakBytes() const -> Result<std::vector<std::byte>> {
            PakBuilder builder;
            for (auto r : resources_) {
                auto added = builder.Add(r);
                if (!added) {
                    return std::unexpected(added.error());
                }
            }
            return builder.Serialize();
        }
    };

    /** @brief Build a static bundle from embedded resource values. */
    template<typename... Rs>
    [[nodiscard]] constexpr auto MakeStaticBundle(const Rs&... resources) {
        return StaticBundle<sizeof...(Rs)>{std::array<EmbeddedResourceView, sizeof...(Rs)>{resources.View()...}};
    }

} // namespace Sora::Resources
