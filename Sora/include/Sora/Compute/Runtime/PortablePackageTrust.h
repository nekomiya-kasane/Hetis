/**
 * @file PortablePackageTrust.h
 * @brief Detached trust verification and immutable admission provenance for portable compute packages.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace Sora::Compute {

    inline constexpr SemanticHash kDirectPackageAuthority{0x534F52414C4F4341ULL, 0x4C5041434B414745ULL};
    inline constexpr SemanticHash kTrustedArchiveAuthority{0x534F524154525553ULL, 0x5445444152434856ULL};

    /** @brief Detached trust evidence over canonical archive bytes. */
    struct PortablePackageDetachedSignature {
        uint32_t scheme = 0;
        std::span<const std::byte> keyIdentity{};
        std::span<const std::byte> signature{};

        /** @brief Return whether all provider-selected signature fields are present. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return scheme != 0 && !keyIdentity.empty() && !signature.empty();
        }
    };

    /** @brief Stable failure reported by an external cryptographic trust provider. */
    enum class PortablePackageTrustError : uint8_t {
        UnsupportedScheme,
        UnknownKey,
        InvalidSignature,
        RevokedKey,
        ProviderFailure,
    };

    /** @brief Authenticated principal and policy generation returned by a trust provider. */
    struct PortablePackageTrustGrant {
        uint32_t scheme = 0;
        SemanticHash authority{};
        SemanticHash subject{};
        uint64_t policyRevision = 0;

        /** @brief Return whether this grant names a complete authenticated principal. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return scheme != 0 && ValidSemanticHash(authority) && ValidSemanticHash(subject);
        }

        friend constexpr bool operator==(PortablePackageTrustGrant, PortablePackageTrustGrant) noexcept = default;
    };

    /** @brief Domain-separated verification request for one detached archive signature. */
    struct PortablePackageVerificationRequest {
        std::string_view context{};
        std::span<const std::byte> archive{};
        PortablePackageDetachedSignature signature{};
    };

    inline constexpr std::string_view kPortablePackageSignatureContext = "Sora.Compute.PortablePackageArchive.v1";

    /** @brief Stateless or stateful provider that authenticates detached package trust evidence without throwing. */
    template<typename Verifier>
    concept PortablePackageSignatureVerifier =
        requires(const Verifier& verifier, const PortablePackageVerificationRequest& request) {
            {
                verifier.Verify(request)
            } noexcept -> std::same_as<std::expected<PortablePackageTrustGrant, PortablePackageTrustError>>;
        };

    /** @brief Admission path retained independently of canonical package content identity. */
    enum class PortablePackageAdmissionKind : uint8_t {
        Direct,
        TrustedArchive,
        DetachedSignature,
    };

    /** @brief Fixed-size immutable audit identity for one successful package admission. */
    struct PortablePackageProvenance {
        PortablePackageAdmissionKind kind = PortablePackageAdmissionKind::Direct;
        uint32_t scheme = 0;
        SemanticHash authority = kDirectPackageAuthority;
        SemanticHash subject = kDirectPackageAuthority;
        uint64_t policyRevision = 0;

        /** @brief Construct provenance for a package supplied directly by trusted process code. */
        [[nodiscard]] static constexpr PortablePackageProvenance Direct() noexcept { return {}; }

        /** @brief Construct provenance for an archive authenticated by its enclosing transport or local policy. */
        [[nodiscard]] static constexpr PortablePackageProvenance TrustedArchive() noexcept {
            return {.kind = PortablePackageAdmissionKind::TrustedArchive,
                    .authority = kTrustedArchiveAuthority,
                    .subject = kTrustedArchiveAuthority};
        }

        /** @brief Construct provenance from one authenticated detached-signature grant. */
        [[nodiscard]] static constexpr PortablePackageProvenance Verified(PortablePackageTrustGrant grant) noexcept {
            return {.kind = PortablePackageAdmissionKind::DetachedSignature,
                    .scheme = grant.scheme,
                    .authority = grant.authority,
                    .subject = grant.subject,
                    .policyRevision = grant.policyRevision};
        }

        /** @brief Return whether the admission kind and authenticated identity are coherent. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            switch (kind) {
                case PortablePackageAdmissionKind::Direct:
                    return scheme == 0 && authority == kDirectPackageAuthority && subject == kDirectPackageAuthority &&
                           policyRevision == 0;
                case PortablePackageAdmissionKind::TrustedArchive:
                    return scheme == 0 && authority == kTrustedArchiveAuthority &&
                           subject == kTrustedArchiveAuthority && policyRevision == 0;
                case PortablePackageAdmissionKind::DetachedSignature:
                    return scheme != 0 && ValidSemanticHash(authority) && ValidSemanticHash(subject);
            }
            return false;
        }

        friend constexpr bool operator==(PortablePackageProvenance, PortablePackageProvenance) noexcept = default;
    };

    /** @brief Principal granularity selected by one detached-admission revocation transaction. */
    enum class PortablePackageAdmissionRevocationScope : uint8_t {
        Authority,
        Subject,
    };

    /** @brief Canonical predicate removing current detached admissions through one policy revision. */
    struct PortablePackageAdmissionRevocation {
        PortablePackageAdmissionRevocationScope scope = PortablePackageAdmissionRevocationScope::Authority;
        uint32_t scheme = 0;
        SemanticHash authority{};
        SemanticHash subject{};
        uint64_t throughPolicyRevision = std::numeric_limits<uint64_t>::max();

        /** @brief Revoke every current subject admitted by @p authority through @p revision. */
        [[nodiscard]] static constexpr PortablePackageAdmissionRevocation
        ForAuthority(uint32_t scheme, SemanticHash authority,
                     uint64_t revision = std::numeric_limits<uint64_t>::max()) noexcept {
            return {.scope = PortablePackageAdmissionRevocationScope::Authority,
                    .scheme = scheme,
                    .authority = authority,
                    .throughPolicyRevision = revision};
        }

        /** @brief Revoke one current subject admitted by @p authority through @p revision. */
        [[nodiscard]] static constexpr PortablePackageAdmissionRevocation
        ForSubject(uint32_t scheme, SemanticHash authority, SemanticHash subject,
                   uint64_t revision = std::numeric_limits<uint64_t>::max()) noexcept {
            return {.scope = PortablePackageAdmissionRevocationScope::Subject,
                    .scheme = scheme,
                    .authority = authority,
                    .subject = subject,
                    .throughPolicyRevision = revision};
        }

        /** @brief Return whether this value has one canonical authority or subject scope. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            if (scheme == 0 || !ValidSemanticHash(authority)) {
                return false;
            }
            switch (scope) {
                case PortablePackageAdmissionRevocationScope::Authority:
                    return subject == SemanticHash{};
                case PortablePackageAdmissionRevocationScope::Subject:
                    return ValidSemanticHash(subject);
            }
            return false;
        }

        /** @brief Return whether @p provenance is removed by this current-generation transaction. */
        [[nodiscard]] constexpr bool Matches(const PortablePackageProvenance& provenance) const noexcept {
            return Valid() && provenance.kind == PortablePackageAdmissionKind::DetachedSignature &&
                   provenance.scheme == scheme && provenance.authority == authority &&
                   provenance.policyRevision <= throughPolicyRevision &&
                   (scope == PortablePackageAdmissionRevocationScope::Authority || provenance.subject == subject);
        }

        friend constexpr bool operator==(PortablePackageAdmissionRevocation,
                                         PortablePackageAdmissionRevocation) noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<PortablePackageTrustGrant>);
    static_assert(std::is_trivially_copyable_v<PortablePackageProvenance>);
    static_assert(std::is_trivially_copyable_v<PortablePackageAdmissionRevocation>);

} // namespace Sora::Compute
