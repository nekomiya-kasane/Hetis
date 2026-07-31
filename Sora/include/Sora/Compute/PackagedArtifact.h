/**
 * @file PackagedArtifact.h
 * @brief Validated exact-key artifact seeds carried by portable program packages.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable failure while constructing an immutable packaged-artifact catalog. */
    enum class PortablePackageErrorCode : uint8_t {
        InvalidProgram,
        InvalidArtifactKey,
        NonPersistentArtifact,
        EmptyArtifactPayload,
        DuplicateArtifact,
        ArtifactIdentityCollision,
        IncoherentArtifactGraph,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Portable-package failure located at one canonical artifact and semantic identity. */
    struct PortablePackageError {
        PortablePackageErrorCode code = PortablePackageErrorCode::InvalidProgram;
        uint64_t artifact = UINT64_MAX;
        ArtifactKind kind = ArtifactKind::PortableLowering;
        SemanticHash identity{};

        friend constexpr bool operator==(PortablePackageError, PortablePackageError) noexcept = default;
    };

    /** @brief Owning backend-private bytes indexed by their complete persistent artifact key. */
    struct PackagedArtifact {
        ArtifactKey key{};
        std::vector<std::byte> payload{};
    };

    /** @brief Immutable canonical artifact set proven coherent with one root program identity. */
    class PackagedArtifactSet final {
    public:
        PackagedArtifactSet() = delete;

        /** @brief Validate, canonicalize, and own @p artifacts rooted at @p program. */
        [[nodiscard]] static std::expected<PackagedArtifactSet, PortablePackageError>
        Create(SemanticHash program, std::vector<PackagedArtifact> artifacts) noexcept;

        /** @brief Return the root canonical program identity. */
        [[nodiscard]] SemanticHash Program() const noexcept { return program_; }

        /** @brief Return all artifacts in canonical stage-and-key order. */
        [[nodiscard]] std::span<const PackagedArtifact> Entries() const noexcept { return artifacts_; }

        /** @brief Find backend bytes only for the complete exact @p key. */
        [[nodiscard]] std::optional<std::span<const std::byte>> Find(const ArtifactKey& key) const noexcept;

    private:
        PackagedArtifactSet(SemanticHash program, std::vector<PackagedArtifact> artifacts) noexcept
            : program_(program), artifacts_(std::move(artifacts)) {}

        SemanticHash program_{};
        std::vector<PackagedArtifact> artifacts_{};
    };

} // namespace Sora::Compute
