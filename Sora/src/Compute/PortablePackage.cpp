/**
 * @file PortablePackage.cpp
 * @brief Validation and canonical lookup for packaged program artifacts.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/PortablePackage.h>

#include <algorithm>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace Sora::Compute {

    namespace {

        struct ArtifactIdentityRecord {
            SemanticHash identity{};
            ArtifactKind kind = ArtifactKind::PortableLowering;
            uint64_t artifact = 0;
        };

        [[nodiscard]] constexpr bool LessIdentityRecord(const ArtifactIdentityRecord& left,
                                                        const ArtifactIdentityRecord& right) noexcept {
            if (left.identity != right.identity) {
                return SemanticHashLess{}(left.identity, right.identity);
            }
            return std::to_underlying(left.kind) < std::to_underlying(right.kind);
        }

        [[nodiscard]] const ArtifactIdentityRecord* FindIdentity(std::span<const ArtifactIdentityRecord> identities,
                                                                 SemanticHash identity) noexcept {
            const auto found =
                std::ranges::lower_bound(identities, identity, SemanticHashLess{}, &ArtifactIdentityRecord::identity);
            return found != identities.end() && found->identity == identity ? std::addressof(*found) : nullptr;
        }

        [[nodiscard]] PortablePackageError Failure(PortablePackageErrorCode code, uint64_t artifact,
                                                   const ArtifactKey& key) noexcept {
            return {.code = code, .artifact = artifact, .kind = key.Kind(), .identity = ArtifactIdentity(key)};
        }

    } // namespace

    std::expected<PackagedArtifactSet, PortablePackageError>
    PackagedArtifactSet::Create(SemanticHash program, std::vector<PackagedArtifact> artifacts) noexcept {
        if (!ValidSemanticHash(program)) {
            return std::unexpected(PortablePackageError{PortablePackageErrorCode::InvalidProgram});
        }
        if (!std::in_range<uint64_t>(artifacts.size())) {
            return std::unexpected(PortablePackageError{PortablePackageErrorCode::CapacityExceeded});
        }

        try {
            std::ranges::sort(artifacts, [](const PackagedArtifact& left, const PackagedArtifact& right) noexcept {
                return ArtifactKeyLess{}(left.key, right.key);
            });

            std::vector<ArtifactIdentityRecord> identities;
            identities.reserve(artifacts.size());
            for (size_t index = 0; index < artifacts.size(); ++index) {
                const PackagedArtifact& artifact = artifacts[index];
                if (!artifact.key.Valid()) {
                    return std::unexpected(Failure(PortablePackageErrorCode::InvalidArtifactKey, index, artifact.key));
                }
                if (!artifact.key.PersistentCacheable()) {
                    return std::unexpected(
                        Failure(PortablePackageErrorCode::NonPersistentArtifact, index, artifact.key));
                }
                if (artifact.payload.empty()) {
                    return std::unexpected(
                        Failure(PortablePackageErrorCode::EmptyArtifactPayload, index, artifact.key));
                }
                if (index != 0 && artifact.key == artifacts[index - 1].key) {
                    return std::unexpected(Failure(PortablePackageErrorCode::DuplicateArtifact, index, artifact.key));
                }
                const SemanticHash identity = ArtifactIdentity(artifact.key);
                if (!ValidSemanticHash(identity) || identity == program) {
                    return std::unexpected(
                        Failure(PortablePackageErrorCode::ArtifactIdentityCollision, index, artifact.key));
                }
                identities.push_back({identity, artifact.key.Kind(), index});
            }

            std::ranges::sort(identities, LessIdentityRecord);
            for (size_t index = 1; index < identities.size(); ++index) {
                if (identities[index - 1].identity == identities[index].identity) {
                    const uint64_t artifact = identities[index].artifact;
                    return std::unexpected(Failure(PortablePackageErrorCode::ArtifactIdentityCollision, artifact,
                                                   artifacts[artifact].key));
                }
            }

            for (size_t index = 0; index < artifacts.size(); ++index) {
                const ArtifactKey& key = artifacts[index].key;
                const SemanticHash input = key.InputIdentity();
                const ArtifactIdentityRecord* upstream = FindIdentity(identities, input);
                bool coherent = false;
                switch (key.Kind()) {
                    case ArtifactKind::PortableLowering:
                        coherent = input == program;
                        break;
                    case ArtifactKind::DeviceExecutable:
                        coherent = input == program ||
                                   (upstream != nullptr && upstream->kind == ArtifactKind::PortableLowering);
                        break;
                    case ArtifactKind::DevicePipeline:
                        coherent = upstream != nullptr && upstream->kind == ArtifactKind::DeviceExecutable &&
                                   key.Backend() == artifacts[static_cast<size_t>(upstream->artifact)].key.Backend();
                        break;
                }
                if (!coherent) {
                    return std::unexpected(Failure(PortablePackageErrorCode::IncoherentArtifactGraph, index, key));
                }
            }
            return PackagedArtifactSet{program, std::move(artifacts)};
        } catch (const std::length_error&) {
            return std::unexpected(PortablePackageError{PortablePackageErrorCode::CapacityExceeded});
        } catch (const std::bad_alloc&) {
            return std::unexpected(PortablePackageError{PortablePackageErrorCode::OutOfMemory});
        }
    }

    std::optional<std::span<const std::byte>> PackagedArtifactSet::Find(const ArtifactKey& key) const noexcept {
        if (!key.Valid()) {
            return std::nullopt;
        }
        const auto found = std::ranges::lower_bound(artifacts_, key, ArtifactKeyLess{}, &PackagedArtifact::key);
        if (found == artifacts_.end() || found->key != key) {
            return std::nullopt;
        }
        return std::span<const std::byte>{found->payload};
    }

    std::expected<PortablePackage, PortablePackageError>
    PortablePackage::Create(ProgramPackage program, std::vector<PackagedArtifact> artifacts) noexcept {
        if (!program || !ValidSemanticHash(program.Hash()) || Detail::HashBytes(program.Bytes()) != program.Hash()) {
            return std::unexpected(PortablePackageError{PortablePackageErrorCode::InvalidProgram});
        }
        auto artifactSet = PackagedArtifactSet::Create(program.Hash(), std::move(artifacts));
        if (!artifactSet) {
            return std::unexpected(artifactSet.error());
        }
        return PortablePackage{std::move(program), std::move(*artifactSet)};
    }

} // namespace Sora::Compute
