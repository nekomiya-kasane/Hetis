/**
 * @file ProgramCatalog.cpp
 * @brief Immutable planning-time program catalog implementation.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/ProgramCatalog.h>

#include <limits>
#include <ranges>
#include <utility>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr ProgramCatalogError Failure(ProgramCatalogErrorCode code, SemanticHash program = {},
                                                            uint64_t detail = 0) noexcept {
            return {.code = code, .program = program, .detail = detail};
        }

        [[nodiscard]] bool SameProgram(const ProgramPackage& left, const ProgramPackage& right) noexcept {
            return left.Hash() == right.Hash() && std::ranges::equal(left.Bytes(), right.Bytes());
        }

    } // namespace

    std::expected<ProgramCatalogSnapshot, ProgramCatalogError>
    ProgramCatalogSnapshot::Create(std::shared_ptr<const PortablePackageRegistrySnapshot> admitted) noexcept {
        if (!admitted) {
            return std::unexpected(Failure(ProgramCatalogErrorCode::InvalidSnapshot));
        }
        const uint64_t ownedBytes = admitted->OwnedBytes();
        return ProgramCatalogSnapshot{std::move(admitted), std::nullopt, ownedBytes};
    }

    std::expected<ProgramCatalogSnapshot, ProgramCatalogError>
    ProgramCatalogSnapshot::WithGenerated(ProgramPackage program) const noexcept {
        const SemanticHash identity = program.Hash();
        if (!program || !ValidSemanticHash(identity) || Detail::HashBytes(program.Bytes()) != identity) {
            return std::unexpected(Failure(ProgramCatalogErrorCode::InvalidProgram, identity));
        }
        if (generated_) {
            if (generated_->Hash() != identity) {
                return std::unexpected(Failure(ProgramCatalogErrorCode::CapacityExceeded, identity));
            }
            if (!SameProgram(*generated_, program)) {
                return std::unexpected(Failure(ProgramCatalogErrorCode::ProgramIdentityCollision, identity));
            }
            return *this;
        }
        const auto existing = admitted_->Resolve(identity);
        if (existing) {
            if (!std::ranges::equal(existing->Program().Bytes(), program.Bytes())) {
                return std::unexpected(Failure(ProgramCatalogErrorCode::ProgramIdentityCollision, identity));
            }
            return *this;
        }
        if (existing.error().code != PortablePackageRegistryErrorCode::NotFound) {
            return std::unexpected(Failure(ProgramCatalogErrorCode::InvalidSnapshot, identity,
                                           static_cast<uint64_t>(existing.error().code)));
        }
        if (program.Bytes().size() > std::numeric_limits<uint64_t>::max() - admitted_->OwnedBytes()) {
            return std::unexpected(Failure(ProgramCatalogErrorCode::CapacityExceeded, identity));
        }
        const uint64_t ownedBytes = admitted_->OwnedBytes() + program.Bytes().size();
        return ProgramCatalogSnapshot{admitted_, std::move(program), ownedBytes};
    }

    std::expected<ProgramPackage, PortablePackageRegistryError>
    ProgramCatalogSnapshot::Resolve(SemanticHash program) const noexcept {
        if (generated_ && generated_->Hash() == program) {
            return *generated_;
        }
        auto admitted = admitted_->Resolve(program);
        if (!admitted) {
            return std::unexpected(admitted.error());
        }
        return admitted->Program();
    }

    std::optional<PackagedArtifactSeed> ProgramCatalogSnapshot::FindArtifact(const ArtifactKey& key) const noexcept {
        return admitted_->FindArtifact(key);
    }

    size_t ProgramCatalogSnapshot::PackageCount() const noexcept {
        return admitted_->PackageCount() + generated_.has_value();
    }

} // namespace Sora::Compute
