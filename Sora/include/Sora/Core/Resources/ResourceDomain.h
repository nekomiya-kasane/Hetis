/**
 * @file ResourceDomain.h
 * @brief Generic resource-domain URI aliases, static resources, and runtime override providers.
 * @ingroup Resources
 *
 * @details
 * This header provides the infrastructure for defining and managing Sora resource domains. Resource domains are typed
 * collections of resources identified by canonical URI schemes.
 *
 * @section Usage
 *
 * To define a custom resource domain:
 *
 * @code
 * namespace MyApp::Resources {
 *     struct MyDomain {
 *         static constexpr auto kDescriptor = Sora::Resources::MakeResourceDomainDescriptor(
 *             "mydomain",                              // Lowercase URI scheme (not "res")
 *             Sora::ResourceType::Texture,             // Resource type
 *             Sora::Resources::RuntimeEntryPolicy::AllowUndeclared // Runtime policy
 *         );
 *     };
 * }
 * @endcode
 *
 * The domain must satisfy the @c Sora::Concept::ResourceDomain concept, which enforces:
 * - Lowercase canonical URI scheme (RFC 3986 compliant, not "res")
 * - Known @c ResourceType semantic classification
 * - Valid @c RuntimeEntryPolicy (DeclaredOnly or AllowUndeclared)
 *
 * Resource domains enable type-safe URI resolution with compile-time validation and optional runtime provider overrides
 * controlled by @c RuntimeEntryPolicy.
 */
#pragma once

#include <Sora/Core/Traits/EnumTraits.h>
#include <Sora/ErrorCode.h>
#include <Sora/Core/Resources/ResourceModule.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Resources {

    /** @brief Policy controlling whether a runtime provider may introduce undeclared domain entries. */
    enum class RuntimeEntryPolicy : uint8_t {
        DeclaredOnly,   /**< Only allow entries declared at compile time. */
        AllowUndeclared /**< Allow entries introduced by runtime providers. */
    };

    /** @brief Atomic compile-time description of one resource URI domain. */
    struct ResourceDomainDescriptor {
        FixedString<32> scheme{}; /**< Canonical URI scheme and canonical @c res:// authority. */
        ResourceType resourceType = ResourceType::Unknown; /**< Semantic resource type carried by the domain. */
        RuntimeEntryPolicy entryPolicy = RuntimeEntryPolicy::DeclaredOnly; /**< Runtime entry admission policy. */

        constexpr bool operator==(const ResourceDomainDescriptor&) const = default;
    };

    /**
     * @brief Construct and validate one resource-domain descriptor during constant evaluation.
     * @param[in] scheme Canonical lowercase URI scheme. @c res is reserved by the resource system.
     * @param[in] resourceType Semantic type carried by resources in this domain.
     * @param[in] entryPolicy Policy for entries introduced by runtime providers.
     */
    [[nodiscard]] consteval ResourceDomainDescriptor
    MakeResourceDomainDescriptor(std::string_view scheme, ResourceType resourceType,
                                 RuntimeEntryPolicy entryPolicy = RuntimeEntryPolicy::DeclaredOnly) {
        if (scheme.size() > FixedString<32>{}.capacity()) {
            throw std::define_static_string("Sora resource domain scheme exceeds 32 characters.");
        }
        if (!Sora::IsCanonicalUriScheme(scheme)) {
            throw std::define_static_string(
                "Sora resource domain schemes must use canonical lowercase RFC 3986 spelling.");
        }
        if (scheme == "res") {
            throw std::define_static_string("Sora resource domain scheme 'res' is reserved.");
        }
        if (!IsKnownResourceType(resourceType)) {
            throw std::define_static_string("Sora resource domains must map to a known ResourceType.");
        }
        if (!Traits::IsValidEnumValue(entryPolicy)) {
            throw std::define_static_string("Sora resource domains must use a known RuntimeEntryPolicy.");
        }
        return ResourceDomainDescriptor{
            .scheme = FixedString<32>{scheme},
            .resourceType = resourceType,
            .entryPolicy = entryPolicy,
        };
    }

} // namespace Sora::Resources

namespace Sora::Concept {

    /**
     * @brief Type that owns one complete, canonical resource-domain descriptor.
     * - A domain cannot be named 'res', and must use a canonical lowercase URI scheme.
     * - A domain must use a known ResourceType
     * - A domain must use a known RuntimeEntryPolicy
     * - A domain must have a static constexpr ResourceDomainDescriptor named kDescriptor.
     */
    template<typename Domain>
    concept ResourceDomain = std::is_class_v<Domain> && requires {
        requires std::same_as<std::remove_cv_t<decltype(Domain::kDescriptor)>, Resources::ResourceDomainDescriptor>;

        requires Sora::IsCanonicalUriScheme(Domain::kDescriptor.scheme.view());
        requires Domain::kDescriptor.scheme != "res";
        requires IsKnownResourceType(Domain::kDescriptor.resourceType);
        requires Traits::IsValidEnumValue(Domain::kDescriptor.entryPolicy);
    };

} // namespace Sora::Concept

namespace Sora::Resources {

    /** @brief Validated atomic descriptor owned by @p Domain. */
    template<Sora::Concept::ResourceDomain Domain>
    inline constexpr ResourceDomainDescriptor kResourceDomainDescriptor = Domain::kDescriptor;

    /** @brief Return the canonical @c res://domain resource-tree root for @p Domain. */
    template<Sora::Concept::ResourceDomain Domain, size_t Capacity = 256>
    [[nodiscard]] consteval FixedString<Capacity> ResourceDomainRootUri() {
        FixedString<Capacity> out{};
        constexpr auto& scheme = kResourceDomainDescriptor<Domain>.scheme;
        if (std::string_view{"res://"}.size() + scheme.size() > out.capacity()) {
            throw std::define_static_string("Sora resource domain root URI exceeds its fixed capacity.");
        }
        out.append("res://");
        out.append(scheme.view());
        return out;
    }

    /** @brief Return true when @p parts use @p scheme directly or as a @c res authority. */
    [[nodiscard]] constexpr bool IsResourceDomainUri(const Sora::UriParts& parts, std::string_view scheme) noexcept {
        return parts.scheme == scheme || (parts.scheme == "res" && parts.hasAuthority && parts.authority == scheme);
    }

    /** @brief Return true when @p uri uses @p scheme directly or as a @c res authority. */
    [[nodiscard]] constexpr bool IsResourceDomainUri(std::string_view uri, std::string_view scheme) noexcept {
        const auto parsed = Sora::ParseUri(uri);
        return parsed.has_value() && IsResourceDomainUri(*parsed, scheme);
    }

    /** @brief Return true when @p uri belongs to @p Domain by URI scheme or @c res authority. */
    template<Sora::Concept::ResourceDomain Domain>
    [[nodiscard]] constexpr bool IsResourceDomainUri(std::string_view uri) noexcept {
        return IsResourceDomainUri(uri, kResourceDomainDescriptor<Domain>.scheme.view());
    }

    /** @brief Validate and canonicalize @p uri from @p scheme to @c res://scheme/... identity text. */
    template<size_t Capacity = 256>
    [[nodiscard]] constexpr auto CanonicalizeResourceDomainUri(std::string_view scheme, std::string_view uri) noexcept
        -> std::expected<FixedString<Capacity>, ErrorCode> {
        const auto parsed = Sora::ParseUri(uri);
        if (!parsed || !IsResourceDomainUri(*parsed, scheme)) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        if (parsed->scheme == "res" && parsed->hasAuthority && parsed->authority == scheme) {
            if (!ParseResourceUri(uri)) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            if (uri.size() > Capacity) {
                return std::unexpected(ErrorCode::OutOfRange);
            }
            return FixedString<Capacity>{uri};
        }

        if (!parsed->hasAuthority || parsed->authority.empty() || parsed->hasQuery || parsed->hasFragment) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        FixedString<Capacity> root{};
        if (std::string_view{"res://"}.size() + scheme.size() > root.capacity()) {
            return std::unexpected(ErrorCode::OutOfRange);
        }
        root.append("res://");
        root.append(scheme);

        FixedString<Capacity> relative{};
        if (parsed->authority.size() + parsed->path.size() > relative.capacity()) {
            return std::unexpected(ErrorCode::OutOfRange);
        }
        relative.append(parsed->authority);
        relative.append(parsed->path);

        auto out = Sora::JoinUriIdentityPath<Capacity>(root.view(), relative.view());
        if (!out) {
            return std::unexpected(out.error() == Sora::UriError::TooLong ? ErrorCode::OutOfRange
                                                                          : ErrorCode::InvalidArgument);
        }
        if (!ParseResourceUri(out->view())) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        return *out;
    }

    /** @brief Validate and canonicalize @p uri to @p Domain's @c res://domain/... identity text. */
    template<Sora::Concept::ResourceDomain Domain, size_t Capacity = 256>
    [[nodiscard]] constexpr auto CanonicalizeResourceDomainUri(std::string_view uri) noexcept
        -> std::expected<FixedString<Capacity>, ErrorCode> {
        return CanonicalizeResourceDomainUri<Capacity>(kResourceDomainDescriptor<Domain>.scheme.view(), uri);
    }

    /** @brief Normalize a canonical resource URI or arbitrary domain alias to resource identity text. */
    [[nodiscard]] inline auto NormalizeResourceReference(std::string_view uri) -> Result<std::string> {
        if (auto normalized = NormalizeResourceUri(uri)) {
            return normalized;
        }

        const auto parsed = Sora::ParseUri(uri);
        if (!parsed || parsed->scheme == "res") {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        auto canonical = CanonicalizeResourceDomainUri(parsed->scheme, uri);
        if (!canonical) {
            return std::unexpected(canonical.error());
        }
        return std::string(canonical->view());
    }

    /** @brief Normalize a canonical resource URI or @p Domain alias to resource identity text. */
    template<Sora::Concept::ResourceDomain Domain>
    [[nodiscard]] inline auto NormalizeResourceReference(std::string_view uri) -> Result<std::string> {
        auto canonical = CanonicalizeResourceDomainUri<Domain>(uri);
        if (!canonical) {
            return std::unexpected(canonical.error());
        }
        return std::string(canonical->view());
    }

    /** @brief Validate and canonicalize a domain URI literal at compile time. */
    template<Sora::Concept::ResourceDomain Domain, auto Uri>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    [[nodiscard]] consteval FixedString<256> CanonicalResourceDomainUri() {
        auto resourceUri = CanonicalizeResourceDomainUri<Domain>(Uri.view());
        if (!resourceUri) {
            throw std::define_static_string("Sora domain URI must identify a canonical resource.");
        }
        return *resourceUri;
    }

    /** @brief Compose @c domain://identity from one canonical relative identity path. */
    template<Sora::Concept::ResourceDomain Domain, auto Identity>
        requires Sora::Concept::FixedStringLike<decltype(Identity)>
    [[nodiscard]] consteval FixedString<256> ComposeResourceDomainUri() {
        if (!Sora::IsCanonicalRelativeUriIdentityPath(Identity.view())) {
            throw std::define_static_string("Sora resource domain identity must be a canonical relative URI path.");
        }

        FixedString<256> out{};
        out.append(kResourceDomainDescriptor<Domain>.scheme.view());
        out.append("://");
        out.append(Identity.view());
        return out;
    }

    /** @brief Compose @c domain://module/entry from canonical relative identity fragments. */
    template<Sora::Concept::ResourceDomain Domain, auto Module, auto Entry>
        requires Sora::Concept::FixedStringLike<decltype(Module)> && Sora::Concept::FixedStringLike<decltype(Entry)>
    [[nodiscard]] consteval FixedString<256> ComposeResourceDomainUri() {
        if (!Sora::IsCanonicalRelativeUriIdentityPath(Module.view()) ||
            !Sora::IsCanonicalRelativeUriIdentityPath(Entry.view())) {
            throw std::define_static_string("Sora resource domain fragments must be canonical relative URI paths.");
        }

        FixedString<256> out{};
        out.append(kResourceDomainDescriptor<Domain>.scheme.view());
        out.append("://");
        out.append(Module.view());
        out.push_back('/');
        out.append(Entry.view());
        return out;
    }

    /** @brief Compile-time domain resource identity with precomputed canonical URI and semantic hash. */
    template<Sora::Concept::ResourceDomain Domain, auto Uri>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    struct StaticDomainResourceId {
        inline static constexpr auto kDomainUri = Uri;                                         /**< Domain alias URI. */
        inline static constexpr auto kResourceUri = CanonicalResourceDomainUri<Domain, Uri>(); /**< Canonical URI. */
        inline static constexpr ResourceType kType = kResourceDomainDescriptor<Domain>.resourceType;
        inline static constexpr uint64_t kHash = HashUri(kResourceUri.view()); /**< Canonical URI hash. */

        /** @brief Return this identity as a runtime resource id. */
        [[nodiscard]] static constexpr ResourceId Runtime() noexcept {
            return ResourceId{.hash = kHash, .type = kType, .uri = kResourceUri.view()};
        }
    };

    /** @brief Static resource in @p Domain exposed as canonical resource bytes. */
    template<Sora::Concept::ResourceDomain Domain, auto Uri, const auto& Bytes>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    struct StaticDomainResource {
        inline static constexpr auto kId = StaticDomainResourceId<Domain, Uri>{};
        inline static constexpr auto kDomainUri = kId.kDomainUri;
        inline static constexpr auto kResourceUri = kId.kResourceUri;
        inline static constexpr auto& kBytes = Bytes;
        using Resource = StaticResource<kResourceUri, kId.kType, Bytes>;

        /** @brief Return this object as a sparse ABI descriptor. */
        [[nodiscard]] static constexpr ModuleResourceEntry Entry() noexcept { return Resource::Entry(); }
    };

    /** @brief Static domain resource addressed by separate module and entry fragments. */
    template<Sora::Concept::ResourceDomain Domain, auto Module, auto Entry, const auto& Bytes>
    using StaticDomainResourceFor =
        StaticDomainResource<Domain, ComposeResourceDomainUri<Domain, Module, Entry>(), Bytes>;

    /** @brief Provider-side table for runtime-updatable payloads indexed by a resource domain. */
    template<Sora::Concept::ResourceDomain Domain>
    class RuntimeDomainResourceProvider {
    public:
        RuntimeDomainResourceProvider() = default;
        RuntimeDomainResourceProvider(const RuntimeDomainResourceProvider&) = delete;
        RuntimeDomainResourceProvider& operator=(const RuntimeDomainResourceProvider&) = delete;

        /** @brief Admit @p uri as a runtime override when @p Domain requires declaration. */
        [[nodiscard]] VoidResult DeclareOverride(std::string_view uri) {
            auto normalized = NormalizeResourceReference<Domain>(uri);
            if (!normalized) {
                return std::unexpected(normalized.error());
            }

            std::unique_lock lock{mutex_};
            const uint64_t hash = HashUri(*normalized);
            if (!std::ranges::contains(declaredOverrideHashes_, hash)) {
                declaredOverrideHashes_.push_back(hash);
            }
            return {};
        }

        /** @brief Admit @p StaticResource as a runtime override. */
        template<typename StaticResource>
        [[nodiscard]] VoidResult DeclareOverride() {
            return DeclareOverride(StaticResource::kResourceUri.view());
        }

        /** @brief Add or replace one runtime payload, subject to @p Domain's entry admission policy. */
        [[nodiscard]] VoidResult SetBytes(std::string_view uri, std::span<const std::byte> bytes) {
            auto normalized = NormalizeResourceReference<Domain>(uri);
            if (!normalized) {
                return std::unexpected(normalized.error());
            }

            std::unique_lock lock{mutex_};
            const uint64_t hash = HashUri(*normalized);
            if (auto it = std::ranges::find(payloads_, hash, &RuntimePayload::hash); it != payloads_.end()) {
                it->bytes.assign(bytes.begin(), bytes.end());
                RebuildDescriptorsLocked();
                return {};
            }

            if constexpr (kResourceDomainDescriptor<Domain>.entryPolicy == RuntimeEntryPolicy::DeclaredOnly) {
                if (!std::ranges::contains(declaredOverrideHashes_, hash)) {
                    return std::unexpected(ErrorCode::InvalidArgument);
                }
            }

            payloads_.push_back(RuntimePayload{.uri = std::move(*normalized), .hash = hash});
            payloads_.back().bytes.assign(bytes.begin(), bytes.end());
            RebuildDescriptorsLocked();
            return {};
        }

        /** @brief Add or replace one runtime payload from UTF-8 text. */
        [[nodiscard]] VoidResult SetText(std::string_view uri, std::string_view text) {
            return SetBytes(uri, std::as_bytes(std::span{text.data(), text.size()}));
        }

        /** @brief Return the current non-owning ABI descriptor for @ref ResourceRegistry::AddProvider. */
        [[nodiscard]] ResourceProvider ProviderDescriptor(int32_t priority = 0) noexcept {
            return ResourceProvider{.context = this,
                                    .entries = descriptors_.data(),
                                    .count = descriptors_.size(),
                                    .priority = priority,
                                    .open = OpenPayload};
        }

    private:
        struct RuntimePayload {
            std::string uri{};
            uint64_t hash = 0;
            std::vector<std::byte> bytes{};
        };

        void RebuildDescriptorsLocked() {
            descriptors_.clear();
            descriptors_.reserve(payloads_.size());
            for (const RuntimePayload& payload : payloads_) {
                descriptors_.push_back(
                    ModuleResourceEntry{.semanticHash = payload.hash,
                                        .contentHash = Sora::Hashing::HashByteRange(payload.bytes),
                                        .uri = payload.uri.data(),
                                        .uriSize = static_cast<uint32_t>(payload.uri.size()),
                                        .type = static_cast<uint16_t>(kResourceDomainDescriptor<Domain>.resourceType)});
            }
        }

        static ErrorCode OpenPayload(void* context, uint64_t hash, ResourcePayload* out) noexcept {
            if (context == nullptr || out == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            auto* self = static_cast<RuntimeDomainResourceProvider*>(context);
            static thread_local std::vector<std::byte> payload;
            std::shared_lock lock{self->mutex_};
            const auto it = std::ranges::find(self->payloads_, hash, &RuntimePayload::hash);
            if (it == self->payloads_.end()) {
                return ErrorCode::ResourceNotFound;
            }
            payload = it->bytes;
            out->data = reinterpret_cast<const unsigned char*>(payload.data());
            out->size = payload.size();
            return ErrorCode::Ok;
        }

        mutable std::shared_mutex mutex_{};
        std::vector<uint64_t> declaredOverrideHashes_{};
        std::vector<RuntimePayload> payloads_{};
        std::vector<ModuleResourceEntry> descriptors_{};
    };

} // namespace Sora::Resources
