/**
 * @file ProgramCatalog.h
 * @brief Immutable planning-time resolution of admitted and submission-derived programs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/PortablePackageRegistry.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

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

    /** @brief Explicit submission budget for generated programs retained beside one admitted snapshot. */
    struct ProgramCatalogLimits {
        size_t maximumGeneratedPrograms = 64;
        uint64_t maximumGeneratedBytes = uint64_t{256} * 1'024 * 1'024;

        /** @brief Return whether generated counts can be represented by the public diagnostics. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return maximumGeneratedPrograms <= std::numeric_limits<uint32_t>::max();
        }

        friend constexpr bool operator==(ProgramCatalogLimits, ProgramCatalogLimits) noexcept = default;
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
        Create(std::shared_ptr<const PortablePackageRegistrySnapshot> admitted,
               ProgramCatalogLimits limits = {}) noexcept;

        /** @brief Transactionally extend generated state while preserving immutability of this catalog. */
        [[nodiscard]] std::expected<ProgramCatalogSnapshot, ProgramCatalogError>
        WithGenerated(std::vector<ProgramPackage> programs) const noexcept;

        /** @brief Resolve one owning canonical program from generated state first, then admitted packages. */
        [[nodiscard]] std::expected<ProgramPackage, PortablePackageRegistryError>
        Resolve(SemanticHash program) const noexcept;

        /** @brief Find one admitted exact-key artifact seed while retaining its package owner. */
        [[nodiscard]] std::optional<PackagedArtifactSeed> FindArtifact(const ArtifactKey& key) const noexcept;

        /** @brief Return the number of admitted and generated programs visible to planning. */
        [[nodiscard]] size_t PackageCount() const noexcept;

        /** @brief Return the number of submission-derived programs visible to planning. */
        [[nodiscard]] size_t GeneratedProgramCount() const noexcept { return generated_.size(); }

        /** @brief Return the explicit generated-program capacity of this planning transaction. */
        [[nodiscard]] size_t GeneratedProgramCapacity() const noexcept { return limits_.maximumGeneratedPrograms; }

        /** @brief Return unused generated-program slots in this planning transaction. */
        [[nodiscard]] size_t AvailableGeneratedProgramCapacity() const noexcept {
            return limits_.maximumGeneratedPrograms - generated_.size();
        }

        /** @brief Return canonical bytes retained only by submission-derived programs. */
        [[nodiscard]] uint64_t GeneratedBytes() const noexcept { return generatedBytes_; }

        /** @brief Return unused canonical-byte budget for submission-derived programs. */
        [[nodiscard]] uint64_t AvailableGeneratedBytes() const noexcept {
            return limits_.maximumGeneratedBytes - generatedBytes_;
        }

        /** @brief Return the number of admitted exact artifact seeds. */
        [[nodiscard]] size_t ArtifactCount() const noexcept { return admitted_->ArtifactCount(); }

        /** @brief Return the number of admitted provenance identities. */
        [[nodiscard]] size_t ProvenanceCount() const noexcept { return admitted_->ProvenanceCount(); }

        /** @brief Return canonical bytes retained by admitted and generated programs. */
        [[nodiscard]] uint64_t OwnedBytes() const noexcept { return ownedBytes_; }

    private:
        ProgramCatalogSnapshot(std::shared_ptr<const PortablePackageRegistrySnapshot> admitted,
                               std::vector<ProgramPackage> generated, ProgramCatalogLimits limits,
                               uint64_t generatedBytes, uint64_t ownedBytes) noexcept
            : admitted_(std::move(admitted)),
              generated_(std::move(generated)),
              limits_(limits),
              generatedBytes_(generatedBytes),
              ownedBytes_(ownedBytes) {}

        std::shared_ptr<const PortablePackageRegistrySnapshot> admitted_{};
        std::vector<ProgramPackage> generated_{};
        ProgramCatalogLimits limits_{};
        uint64_t generatedBytes_ = 0;
        uint64_t ownedBytes_ = 0;
    };

    static_assert(std::is_trivially_copyable_v<ProgramCatalogLimits>);

} // namespace Sora::Compute
