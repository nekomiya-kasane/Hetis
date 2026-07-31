/**
 * @file BackendPluginAdmission.h
 * @brief Host-established trust facts for native backend plugin images.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>

#include <array>
#include <compare>
#include <cstdint>
#include <type_traits>

namespace Sora::Compute {

    inline constexpr SemanticHash kDirectBackendPluginAuthority{0x534F52415F4C4F43ULL, 0x414C5F504C554749ULL};

    /** @brief Opaque 256-bit identity of the exact native module image admitted by host policy. */
    struct BackendPluginImageIdentity {
        std::array<uint64_t, 4> words{};

        /** @brief Return whether this identity is nonzero. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return words[0] != 0 || words[1] != 0 || words[2] != 0 || words[3] != 0;
        }

        friend constexpr bool operator==(BackendPluginImageIdentity, BackendPluginImageIdentity) noexcept = default;
        friend constexpr auto operator<=>(BackendPluginImageIdentity, BackendPluginImageIdentity) noexcept = default;
    };

    /** @brief Origin of the host decision permitting one native image to execute in process. */
    enum class BackendPluginAdmissionKind : uint8_t {
        Direct,
        VerifiedImage,
    };

    /**
     * @brief Immutable native-plugin admission fact supplied by trusted host policy.
     * @details This record does not verify signatures or hash files. The admitting authority must bind @ref image to
     * the exact loaded module before constructing this value. The catalog preserves the fact without duplicating
     * operating-system signing, certificate revocation, or organizational deny-list policy.
     */
    struct BackendPluginAdmission {
        BackendPluginAdmissionKind kind = BackendPluginAdmissionKind::Direct;
        uint32_t scheme = 0;
        SemanticHash authority = kDirectBackendPluginAuthority;
        BackendPluginImageIdentity image{};
        uint64_t policyRevision = 0;

        /** @brief Record admission by trusted process code for one already authenticated image. */
        [[nodiscard]] static constexpr BackendPluginAdmission Direct(BackendPluginImageIdentity image,
                                                                     uint64_t policyRevision = 0) noexcept {
            return {.image = image, .policyRevision = policyRevision};
        }

        /** @brief Record admission granted by one external image-verification authority. */
        [[nodiscard]] static constexpr BackendPluginAdmission Verified(uint32_t scheme, SemanticHash authority,
                                                                       BackendPluginImageIdentity image,
                                                                       uint64_t policyRevision) noexcept {
            return {.kind = BackendPluginAdmissionKind::VerifiedImage,
                    .scheme = scheme,
                    .authority = authority,
                    .image = image,
                    .policyRevision = policyRevision};
        }

        /** @brief Return whether this value has one canonical admission form and nonzero image identity. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            if (!image.Valid()) {
                return false;
            }
            switch (kind) {
                case BackendPluginAdmissionKind::Direct:
                    return scheme == 0 && authority == kDirectBackendPluginAuthority;
                case BackendPluginAdmissionKind::VerifiedImage:
                    return scheme != 0 && ValidSemanticHash(authority);
            }
            return false;
        }

        friend constexpr bool operator==(BackendPluginAdmission, BackendPluginAdmission) noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<BackendPluginImageIdentity>);
    static_assert(std::is_trivially_copyable_v<BackendPluginAdmission>);

} // namespace Sora::Compute
