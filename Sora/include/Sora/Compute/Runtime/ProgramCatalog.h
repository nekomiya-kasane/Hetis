/**
 * @file ProgramCatalog.h
 * @brief Immutable planning-time resolution of admitted and submission-derived programs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/PortablePackageRegistry.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace Sora::Compute {

    /** @brief Stable failure while constructing an immutable planning program catalog. */
    enum class ProgramCatalogErrorCode : uint8_t {
        InvalidSnapshot,
        InvalidProgram,
        ProgramIdentityCollision,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Program-catalog failure located at one canonical program identity. */
    struct ProgramCatalogError {
        ProgramCatalogErrorCode code = ProgramCatalogErrorCode::InvalidSnapshot;
        SemanticHash program{};
        uint64_t detail = 0;

        friend constexpr bool operator==(ProgramCatalogError, ProgramCatalogError) noexcept = default;
    };

    /**
     * @brief Immutable program-resolution surface for one planning transaction.
     * @details The admitted registry snapshot owns trust and packaged artifacts. Generated programs are scoped to this
     * catalog and carry no independent admission, so they cannot outlive or bypass the submission's frozen trust view.
     */
    class ProgramCatalogSnapshot final {
    public:
        /** @brief Construct a catalog over one admitted snapshot without generated state. */
        [[nodiscard]] static std::expected<ProgramCatalogSnapshot, ProgramCatalogError>
        Create(std::shared_ptr<const PortablePackageRegistrySnapshot> admitted) noexcept;

        /** @brief Add one generated program while preserving immutability of this catalog. */
        [[nodiscard]] std::expected<ProgramCatalogSnapshot, ProgramCatalogError>
        WithGenerated(ProgramPackage program) const noexcept;

        /** @brief Resolve one owning canonical program from generated state first, then admitted packages. */
        [[nodiscard]] std::expected<ProgramPackage, PortablePackageRegistryError>
        Resolve(SemanticHash program) const noexcept;

        /** @brief Find one admitted exact-key artifact seed while retaining its package owner. */
        [[nodiscard]] std::optional<PackagedArtifactSeed> FindArtifact(const ArtifactKey& key) const noexcept;

        /** @brief Return the number of admitted and generated programs visible to planning. */
        [[nodiscard]] size_t PackageCount() const noexcept;

        /** @brief Return the number of submission-derived programs visible to planning. */
        [[nodiscard]] size_t GeneratedProgramCount() const noexcept { return generated_.has_value(); }

        /** @brief Return the structural generated-program capacity of one optimization transaction. */
        [[nodiscard]] static consteval size_t GeneratedProgramCapacity() noexcept { return 1; }

        /** @brief Return the number of admitted exact artifact seeds. */
        [[nodiscard]] size_t ArtifactCount() const noexcept { return admitted_->ArtifactCount(); }

        /** @brief Return the number of admitted provenance identities. */
        [[nodiscard]] size_t ProvenanceCount() const noexcept { return admitted_->ProvenanceCount(); }

        /** @brief Return canonical bytes retained by admitted and generated programs. */
        [[nodiscard]] uint64_t OwnedBytes() const noexcept { return ownedBytes_; }

    private:
        ProgramCatalogSnapshot(std::shared_ptr<const PortablePackageRegistrySnapshot> admitted,
                               std::optional<ProgramPackage> generated, uint64_t ownedBytes) noexcept
            : admitted_(std::move(admitted)), generated_(std::move(generated)), ownedBytes_(ownedBytes) {}

        std::shared_ptr<const PortablePackageRegistrySnapshot> admitted_{};
        std::optional<ProgramPackage> generated_{};
        uint64_t ownedBytes_ = 0;
    };

} // namespace Sora::Compute
