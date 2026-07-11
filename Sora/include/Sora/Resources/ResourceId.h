/**
 * @file ResourceId.h
 * @brief Canonical resource URI identity and stable resource hashes.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Core/FixedString.h>
#include <Sora/Core/Hash.h>
#include <Sora/Core/Uri.h>
#include <Sora/Resources/Format.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Sora::Resources {

    /** @brief Runtime resource identity: canonical URI, type, and 64-bit semantic hash. */
    struct ResourceId {
        uint64_t hash = 0;
        ResourceType type = ResourceType::Unknown;
        std::string_view uri{};

        constexpr bool operator==(const ResourceId&) const noexcept = default;
    };

    /** @brief Parsed, canonical resource URI view backed by the caller-owned URI text. */
    struct ResourceUriView {
        Sora::ParsedUri parsed{}; /**< Generic parsed URI constrained by resource identity rules. */

        /** @brief Return the original canonical URI text. */
        [[nodiscard]] constexpr std::string_view view() const noexcept { return parsed.view(); }
        [[nodiscard]] constexpr std::string_view View() const noexcept { return parsed.view(); }

        /** @brief Return the URI scheme, always @c res for valid resource URIs. */
        [[nodiscard]] constexpr std::string_view Scheme() const noexcept { return parsed.Scheme(); }

        /** @brief Return the resource authority segment, such as @c shader in @c res://shader/fullscreen.wgsl. */
        [[nodiscard]] constexpr std::string_view Authority() const noexcept { return parsed.Authority(); }

        /** @brief Return the resource path component, beginning with @c / when non-empty. */
        [[nodiscard]] constexpr std::string_view Path() const noexcept { return parsed.Path(); }

        /** @brief Return this value as a generic URI view. */
        [[nodiscard]] constexpr Sora::UriView Uri() const noexcept { return Sora::UriView{view()}; }

        /** @brief Return the stable resource identity hash. */
        [[nodiscard]] constexpr uint64_t Hash() const noexcept { return parsed.Hash(); }
    };

    /**
     * @brief Parse @p uri as a canonical resource identity URI.
     *
     * @details Resource identity is intentionally stricter than generic URI syntax: it must be an absolute
     * @c res://... URI, must not contain Windows separators, and must not carry query or fragment components. Runtime
     * request parameters belong to higher-level open calls, not to package identity hashes.
     */
    [[nodiscard]] constexpr std::optional<ResourceUriView> ParseResourceUri(std::string_view uri) noexcept {
        auto parsed = Sora::ParseUriView(uri);
        if (!parsed || parsed->Scheme() != "res" || !parsed->HasAuthority() || parsed->Authority().empty() ||
            parsed->HasQuery() || parsed->HasAnchor()) {
            return std::nullopt;
        }
        if (uri.find('\\') != std::string_view::npos) {
            return std::nullopt;
        }
        return ResourceUriView{.parsed = *parsed};
    }

    /** @brief True when @p uri uses the canonical resource identity form. */
    [[nodiscard]] constexpr bool IsCanonicalUri(std::string_view uri) noexcept {
        return ParseResourceUri(uri).has_value();
    }

    /** @brief Compile-time FNV-1a hash used for resource URI identity. */
    [[nodiscard]] constexpr uint64_t HashUri(std::string_view uri) noexcept {
        if (uri.find('\\') == std::string_view::npos) {
            return Sora::UriHash(uri);
        }
        auto state = Sora::Hashing::Fnv1a64State::Seed();
        for (char c : uri) {
            const auto normalized = static_cast<unsigned char>(c == '\\' ? '/' : c);
            const std::byte b{normalized};
            state.Feed(std::span<const std::byte>{&b, 1});
        }
        return state.Finalize();
    }

    /** @brief Compile-time resource identity carrier. */
    template<auto Uri, ResourceType Type = ResourceType::Raw>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    struct StaticResourceId {
        static_assert(IsCanonicalUri(Uri.view()),
                      "Static resource URI must use canonical res://path form and '/' separators.");

        inline static constexpr auto kUri = Uri;
        inline static constexpr ResourceType kType = Type;
        inline static constexpr uint64_t kHash = HashUri(Uri.view());

        [[nodiscard]] static constexpr ResourceId Runtime() noexcept {
            return ResourceId{.hash = kHash, .type = Type, .uri = Uri.view()};
        }
    };

} // namespace Sora::Resources
