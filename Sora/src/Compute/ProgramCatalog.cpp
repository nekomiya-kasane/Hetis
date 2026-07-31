/**
 * @file ProgramCatalog.cpp
 * @brief Immutable planning-time program catalog implementation.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/ProgramCatalog.h>

#include <algorithm>
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
    ProgramCatalogSnapshot::Create(std::shared_ptr<const PortablePackageRegistrySnapshot> admitted,
                                   ProgramCatalogLimits limits) noexcept {
        if (!admitted || !limits.Valid()) {
            return std::unexpected(Failure(ProgramCatalogErrorCode::InvalidSnapshot));
        }
        const uint64_t ownedBytes = admitted->OwnedBytes();
        return ProgramCatalogSnapshot{std::move(admitted), {}, limits, 0, ownedBytes};
    }

    std::expected<ProgramCatalogSnapshot, ProgramCatalogError>
    ProgramCatalogSnapshot::WithGenerated(std::vector<ProgramPackage> programs) const noexcept {
        try {
            std::vector<ProgramPackage> combined = generated_;
            uint64_t generatedBytes = generatedBytes_;
            const size_t availablePrograms = limits_.maximumGeneratedPrograms - combined.size();
            combined.reserve(combined.size() + std::min(availablePrograms, programs.size()));
            for (ProgramPackage& program : programs) {
                const SemanticHash identity = program.Hash();
                if (!program || !ValidSemanticHash(identity) || Detail::HashBytes(program.Bytes()) != identity) {
                    return std::unexpected(Failure(ProgramCatalogErrorCode::InvalidProgram, identity));
                }

                const auto generated =
                    std::ranges::lower_bound(combined, identity, SemanticHashLess{}, &ProgramPackage::Hash);
                if (generated != combined.end() && generated->Hash() == identity) {
                    if (!SameProgram(*generated, program)) {
                        return std::unexpected(Failure(ProgramCatalogErrorCode::ProgramIdentityCollision, identity));
                    }
                    continue;
                }

                const auto admitted = admitted_->Resolve(identity);
                if (admitted) {
                    if (!std::ranges::equal(admitted->Program().Bytes(), program.Bytes())) {
                        return std::unexpected(Failure(ProgramCatalogErrorCode::ProgramIdentityCollision, identity));
                    }
                    continue;
                }
                if (admitted.error().code != PortablePackageRegistryErrorCode::NotFound) {
                    return std::unexpected(Failure(ProgramCatalogErrorCode::InvalidSnapshot, identity,
                                                   static_cast<uint64_t>(admitted.error().code)));
                }
                if (combined.size() == limits_.maximumGeneratedPrograms ||
                    program.Bytes().size() > limits_.maximumGeneratedBytes - generatedBytes) {
                    return std::unexpected(Failure(ProgramCatalogErrorCode::CapacityExceeded, identity));
                }
                generatedBytes += program.Bytes().size();
                combined.insert(generated, std::move(program));
            }
            if (generatedBytes > std::numeric_limits<uint64_t>::max() - admitted_->OwnedBytes()) {
                return std::unexpected(Failure(ProgramCatalogErrorCode::CapacityExceeded));
            }
            return ProgramCatalogSnapshot{admitted_, std::move(combined), limits_, generatedBytes,
                                          admitted_->OwnedBytes() + generatedBytes};
        } catch (const std::length_error&) {
            return std::unexpected(Failure(ProgramCatalogErrorCode::CapacityExceeded));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(ProgramCatalogErrorCode::OutOfMemory));
        }
    }

    std::expected<ProgramPackage, PortablePackageRegistryError>
    ProgramCatalogSnapshot::Resolve(SemanticHash program) const noexcept {
        const auto generated = std::ranges::lower_bound(generated_, program, SemanticHashLess{}, &ProgramPackage::Hash);
        if (generated != generated_.end() && generated->Hash() == program) {
            return *generated;
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
        return admitted_->PackageCount() + generated_.size();
    }

} // namespace Sora::Compute
