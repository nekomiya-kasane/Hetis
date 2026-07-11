/**
 * @file EmbeddedResource.h
 * @brief `#embed`-oriented static resource declarations.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Resources/ResourceId.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace Sora::Resources {

    /** @brief Non-owning view of a compile-time or static resource payload. */
    struct EmbeddedResourceView : ResourceId {
        const unsigned char* data = nullptr;
        size_t size = 0;

        /** @brief Payload as immutable bytes. */
        [[nodiscard]] constexpr auto Bytes() const noexcept -> std::span<const std::byte> {
            if (size == 0) {
                return {};
            }
            return std::as_bytes(std::span{data, size});
        }
    };

    /**
     * @brief Static resource backed by a byte array, typically produced with `#embed`.
     * @tparam Uri Canonical resource URI.
     * @tparam Type Resource type.
     * @tparam N Number of embedded bytes.
     */
    template<auto Uri, ResourceType Type, size_t N>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    struct EmbeddedResource {
        const unsigned char* data = nullptr;

        inline static constexpr auto kId = StaticResourceId<Uri, Type>{};
        inline static constexpr size_t kSize = N;

        /** @brief Construct from an embedded byte array. */
        constexpr explicit EmbeddedResource(const unsigned char (&bytes)[N]) noexcept : data(bytes) {}

        /** @brief Runtime metadata and byte view. */
        [[nodiscard]] constexpr auto View() const noexcept -> EmbeddedResourceView {
            EmbeddedResourceView ret{};
            ret.hash = kId.kHash;
            ret.type = Type;
            ret.uri = Uri.view();
            ret.data = data;
            ret.size = N;
            return ret;
        }
    };

    /** @brief Create an embedded resource from a static byte array. */
    template<auto Uri, ResourceType Type = ResourceType::Raw, size_t N>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    [[nodiscard]] constexpr auto MakeEmbeddedResource(const unsigned char (&bytes)[N]) noexcept {
        return EmbeddedResource<Uri, Type, N>{bytes};
    }

    namespace $ {

        /** @brief Annotation payload for reflected resource declarations. */
        struct Resource {
            FixedString<256> uri{};                    /**< Canonical URI override, or empty to infer from scope. */
            ResourceType type = ResourceType::Unknown; /**< Semantic type override, or @c Unknown to infer. */
            constexpr bool operator==(const Resource&) const = default;
        };

    } // namespace $

} // namespace Sora::Resources
