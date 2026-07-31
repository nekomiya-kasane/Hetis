/**
 * @file PortablePackageRegistry.cpp
 * @brief Transactional publication and immutable lookup for portable compute
 * packages.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/PortablePackageRegistry.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] bool SamePackage(const PortablePackage& left, const PortablePackage& right) noexcept {
            if (!std::ranges::equal(left.Program().Bytes(), right.Program().Bytes())) {
                return false;
            }
            const auto leftArtifacts = left.Artifacts().Entries();
            const auto rightArtifacts = right.Artifacts().Entries();
            if (leftArtifacts.size() != rightArtifacts.size()) {
                return false;
            }
            for (size_t index = 0; index < leftArtifacts.size(); ++index) {
                if (leftArtifacts[index].key != rightArtifacts[index].key ||
                    !std::ranges::equal(leftArtifacts[index].payload, rightArtifacts[index].payload)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::optional<uint64_t> PackageBytes(const PortablePackage& package) noexcept {
            uint64_t bytes = package.Program().Bytes().size();
            for (const PackagedArtifact& artifact : package.Artifacts().Entries()) {
                if (artifact.payload.size() > std::numeric_limits<uint64_t>::max() - bytes) {
                    return std::nullopt;
                }
                bytes += artifact.payload.size();
            }
            return bytes;
        }

        [[nodiscard]] PortablePackageRegistryError Failure(PortablePackageRegistryErrorCode code,
                                                           SemanticHash program = {}, SemanticHash artifact = {},
                                                           uint64_t detail = 0) noexcept {
            return {.code = code, .program = program, .artifact = artifact, .detail = detail};
        }

        [[nodiscard]] constexpr auto ProvenanceKey(const PortablePackageProvenance& value) noexcept {
            return std::tuple{value.kind,        value.scheme,       value.authority.low, value.authority.high,
                              value.subject.low, value.subject.high, value.policyRevision};
        }

        [[nodiscard]] constexpr bool ProvenanceLess(const PortablePackageProvenance& left,
                                                    const PortablePackageProvenance& right) noexcept {
            return ProvenanceKey(left) < ProvenanceKey(right);
        }

    } // namespace

    std::expected<PortablePackageLease, PortablePackageRegistryError>
    PortablePackageRegistrySnapshot::Resolve(SemanticHash program) const noexcept {
        const auto found = std::ranges::lower_bound(programs_, program, SemanticHashLess{}, &ProgramRecord::identity);
        if (found == programs_.end() || found->identity != program) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::NotFound, program));
        }
        return PortablePackageLease{found->generation};
    }

    std::optional<PackagedArtifactSeed>
    PortablePackageRegistrySnapshot::FindArtifact(const ArtifactKey& key) const noexcept {
        if (!key.Valid()) {
            return std::nullopt;
        }
        const auto found = std::ranges::lower_bound(artifacts_, key, ArtifactKeyLess{}, &ArtifactRecord::key);
        if (found == artifacts_.end() || found->key != key) {
            return std::nullopt;
        }
        assert(found->programIndex < programs_.size());
        const auto& generation = programs_[found->programIndex].generation;
        const auto entries = generation->owner->Artifacts().Entries();
        assert(found->packageArtifact < entries.size());
        assert(entries[found->packageArtifact].key == key);
        return PackagedArtifactSeed{entries[found->packageArtifact].payload, PortablePackageLease{generation}};
    }

    PortablePackageRegistry::PortablePackageRegistry(PortablePackageRegistryLimits limits) : limits_(limits) {
        if (!limits_.Valid()) {
            throw std::invalid_argument{"PortablePackageRegistry limits must admit at least one program"};
        }
        Publish(std::make_shared<PortablePackageRegistrySnapshot>());
    }

    std::shared_ptr<const PortablePackageRegistrySnapshot> PortablePackageRegistry::Snapshot() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
        return snapshot_.load(std::memory_order_acquire);
#else
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
#endif
    }

    void PortablePackageRegistry::Publish(std::shared_ptr<const PortablePackageRegistrySnapshot> snapshot) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
        snapshot_.store(std::move(snapshot), std::memory_order_release);
#else
        std::atomic_store_explicit(&snapshot_, std::move(snapshot), std::memory_order_release);
#endif
    }

    std::expected<PortablePackageLease, PortablePackageRegistryError>
    PortablePackageRegistry::Register(PortablePackage package, PortablePackageProvenance provenance) noexcept {
        const SemanticHash program = package.Program().Hash();
        if (!package.Program() || !ValidSemanticHash(program) || package.Artifacts().Program() != program ||
            Detail::HashBytes(package.Program().Bytes()) != program) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::InvalidPackage, program));
        }
        if (!provenance.Valid()) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::InvalidProvenance, program));
        }
        const std::optional<uint64_t> packageBytes = PackageBytes(package);
        if (!packageBytes) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::ByteCapacityExceeded, program));
        }

        try {
            std::lock_guard lock{writer_};
            const auto current = Snapshot();
            const auto existing = std::ranges::lower_bound(current->programs_, program, SemanticHashLess{},
                                                           &PortablePackageRegistrySnapshot::ProgramRecord::identity);
            if (existing != current->programs_.end() && existing->identity == program) {
                if (!std::ranges::equal(existing->generation->owner->Program().Bytes(), package.Program().Bytes())) {
                    return std::unexpected(
                        Failure(PortablePackageRegistryErrorCode::ProgramIdentityCollision, program));
                }
                if (!SamePackage(*existing->generation->owner, package)) {
                    return std::unexpected(Failure(PortablePackageRegistryErrorCode::PackageConflict, program));
                }
                const auto admission =
                    std::ranges::lower_bound(existing->generation->provenance, provenance, ProvenanceLess);
                if (admission != existing->generation->provenance.end() && *admission == provenance) {
                    return PortablePackageLease{existing->generation};
                }
                if (current->provenanceCount_ >= limits_.maximumProvenanceRecords ||
                    existing->generation->provenance.size() >= existing->generation->provenance.max_size()) {
                    return std::unexpected(
                        Failure(PortablePackageRegistryErrorCode::ProvenanceCapacityExceeded, program));
                }
                auto provenanceSet = existing->generation->provenance;
                provenanceSet.insert(provenanceSet.begin() + (admission - existing->generation->provenance.begin()),
                                     provenance);
                auto generation = std::make_shared<PortablePackageLease::Generation>(existing->generation->owner,
                                                                                     std::move(provenanceSet));
                auto next = std::make_shared<PortablePackageRegistrySnapshot>(*current);
                const size_t programIndex = static_cast<size_t>(existing - current->programs_.begin());
                next->programs_[programIndex].generation = generation;
                ++next->provenanceCount_;
                Publish(next);
                return PortablePackageLease{std::move(generation)};
            }

            const size_t artifactCount = package.Artifacts().Entries().size();
            if (current->programs_.size() >= limits_.maximumPackages ||
                current->programs_.size() >= current->programs_.max_size()) {
                return std::unexpected(Failure(PortablePackageRegistryErrorCode::PackageCapacityExceeded, program));
            }
            if (artifactCount > limits_.maximumArtifacts - current->artifacts_.size() ||
                artifactCount > current->artifacts_.max_size() - current->artifacts_.size() ||
                artifactCount > current->artifactIdentities_.max_size() - current->artifactIdentities_.size()) {
                return std::unexpected(Failure(PortablePackageRegistryErrorCode::ArtifactCapacityExceeded, program));
            }
            if (current->provenanceCount_ >= limits_.maximumProvenanceRecords) {
                return std::unexpected(Failure(PortablePackageRegistryErrorCode::ProvenanceCapacityExceeded, program));
            }
            if (*packageBytes > limits_.maximumOwnedBytes - current->ownedBytes_) {
                return std::unexpected(Failure(PortablePackageRegistryErrorCode::ByteCapacityExceeded, program));
            }

            for (const PackagedArtifact& artifact : package.Artifacts().Entries()) {
                const SemanticHash identity = ArtifactIdentity(artifact.key);
                const auto collision =
                    std::ranges::lower_bound(current->artifactIdentities_, identity, SemanticHashLess{});
                if (collision != current->artifactIdentities_.end() && *collision == identity) {
                    return std::unexpected(
                        Failure(PortablePackageRegistryErrorCode::ArtifactIdentityCollision, program, identity));
                }
            }

            auto owner = std::make_shared<PortablePackage>(std::move(package));
            auto generation = std::make_shared<PortablePackageLease::Generation>(
                std::move(owner), std::vector<PortablePackageProvenance>{provenance});
            auto next = std::make_shared<PortablePackageRegistrySnapshot>(*current);
            const size_t programIndex = static_cast<size_t>(existing - current->programs_.begin());
            next->programs_.insert(next->programs_.begin() + programIndex,
                                   PortablePackageRegistrySnapshot::ProgramRecord{program, generation});
            for (auto& artifact : next->artifacts_) {
                artifact.programIndex += artifact.programIndex >= programIndex;
            }
            const auto artifacts = generation->owner->Artifacts().Entries();
            next->artifacts_.reserve(next->artifacts_.size() + artifacts.size());
            next->artifactIdentities_.reserve(next->artifactIdentities_.size() + artifacts.size());
            for (size_t index = 0; index < artifacts.size(); ++index) {
                next->artifacts_.push_back(
                    PortablePackageRegistrySnapshot::ArtifactRecord{artifacts[index].key, programIndex, index});
                next->artifactIdentities_.push_back(ArtifactIdentity(artifacts[index].key));
            }
            std::ranges::sort(next->artifacts_, ArtifactKeyLess{},
                              &PortablePackageRegistrySnapshot::ArtifactRecord::key);
            std::ranges::sort(next->artifactIdentities_, SemanticHashLess{});
            ++next->provenanceCount_;
            next->ownedBytes_ += *packageBytes;
            Publish(next);
            return PortablePackageLease{std::move(generation)};
        } catch (const std::length_error&) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::OutOfMemory, program));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::OutOfMemory, program));
        }
    }

    std::expected<PortablePackageLease, PortablePackageRegistryError>
    PortablePackageRegistry::Resolve(SemanticHash program) const noexcept {
        return Snapshot()->Resolve(program);
    }

    std::optional<PackagedArtifactSeed> PortablePackageRegistry::FindArtifact(const ArtifactKey& key) const noexcept {
        return Snapshot()->FindArtifact(key);
    }

    std::expected<bool, PortablePackageRegistryError> PortablePackageRegistry::Erase(SemanticHash program) noexcept {
        try {
            std::lock_guard lock{writer_};
            const auto current = Snapshot();
            const auto found = std::ranges::lower_bound(current->programs_, program, SemanticHashLess{},
                                                        &PortablePackageRegistrySnapshot::ProgramRecord::identity);
            if (found == current->programs_.end() || found->identity != program) {
                return false;
            }
            const size_t programIndex = static_cast<size_t>(found - current->programs_.begin());
            auto next = std::make_shared<PortablePackageRegistrySnapshot>();
            next->programs_.reserve(current->programs_.size() - 1);
            next->artifacts_.reserve(current->artifacts_.size() -
                                     found->generation->owner->Artifacts().Entries().size());
            next->artifactIdentities_.reserve(next->artifacts_.capacity());
            for (const auto& record : current->programs_) {
                if (record.identity != program) {
                    next->programs_.push_back(record);
                }
            }
            for (const auto& record : current->artifacts_) {
                if (record.programIndex != programIndex) {
                    auto retained = record;
                    retained.programIndex -= retained.programIndex > programIndex;
                    next->artifactIdentities_.push_back(ArtifactIdentity(retained.key));
                    next->artifacts_.push_back(std::move(retained));
                }
            }
            std::ranges::sort(next->artifactIdentities_, SemanticHashLess{});
            const std::optional<uint64_t> retiredBytes = PackageBytes(*found->generation->owner);
            assert(retiredBytes && *retiredBytes <= current->ownedBytes_);
            next->ownedBytes_ = current->ownedBytes_ - *retiredBytes;
            assert(found->generation != nullptr && found->generation->provenance.size() <= current->provenanceCount_);
            next->provenanceCount_ = current->provenanceCount_ - found->generation->provenance.size();
            Publish(next);
            return true;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::OutOfMemory, program));
        }
    }

    std::expected<PortablePackageAdmissionRevocationResult, PortablePackageRegistryError>
    PortablePackageRegistry::RevokeAdmissions(PortablePackageAdmissionRevocation revocation) noexcept {
        if (!revocation.Valid()) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::InvalidRevocation));
        }

        try {
            std::lock_guard lock{writer_};
            const auto current = Snapshot();
            PortablePackageAdmissionRevocationResult result{};
            size_t artifactsErased = 0;
            for (const auto& record : current->programs_) {
                const size_t matches = static_cast<size_t>(
                    std::ranges::count_if(record.generation->provenance, [&revocation](const auto& provenance) {
                        return revocation.Matches(provenance);
                    }));
                if (matches == 0) {
                    continue;
                }
                result.provenanceRemoved += matches;
                if (matches == record.generation->provenance.size()) {
                    ++result.packagesErased;
                    artifactsErased += record.generation->owner->Artifacts().Entries().size();
                } else {
                    ++result.packagesUpdated;
                }
            }
            if (!result.Changed()) {
                return result;
            }

            auto next = std::make_shared<PortablePackageRegistrySnapshot>();
            next->programs_.reserve(current->programs_.size() - result.packagesErased);
            next->artifacts_.reserve(current->artifacts_.size() - artifactsErased);
            next->artifactIdentities_.reserve(next->artifacts_.capacity());
            std::vector<size_t> programMap(current->programs_.size(), std::numeric_limits<size_t>::max());
            next->ownedBytes_ = current->ownedBytes_;
            next->provenanceCount_ = current->provenanceCount_ - result.provenanceRemoved;

            for (size_t oldIndex = 0; oldIndex < current->programs_.size(); ++oldIndex) {
                const auto& record = current->programs_[oldIndex];
                const size_t matches = static_cast<size_t>(
                    std::ranges::count_if(record.generation->provenance, [&revocation](const auto& provenance) {
                        return revocation.Matches(provenance);
                    }));
                if (matches == record.generation->provenance.size()) {
                    const auto retiredBytes = PackageBytes(*record.generation->owner);
                    assert(retiredBytes && *retiredBytes <= next->ownedBytes_);
                    next->ownedBytes_ -= *retiredBytes;
                    continue;
                }

                auto generation = record.generation;
                if (matches != 0) {
                    std::vector<PortablePackageProvenance> retained;
                    retained.reserve(record.generation->provenance.size() - matches);
                    std::ranges::copy_if(
                        record.generation->provenance, std::back_inserter(retained),
                        [&revocation](const auto& provenance) { return !revocation.Matches(provenance); });
                    generation = std::make_shared<PortablePackageLease::Generation>(record.generation->owner,
                                                                                    std::move(retained));
                }
                programMap[oldIndex] = next->programs_.size();
                next->programs_.push_back({record.identity, std::move(generation)});
            }

            for (const auto& artifact : current->artifacts_) {
                assert(artifact.programIndex < programMap.size());
                const size_t programIndex = programMap[artifact.programIndex];
                if (programIndex == std::numeric_limits<size_t>::max()) {
                    continue;
                }
                auto retained = artifact;
                retained.programIndex = programIndex;
                next->artifactIdentities_.push_back(ArtifactIdentity(retained.key));
                next->artifacts_.push_back(std::move(retained));
            }
            std::ranges::sort(next->artifactIdentities_, SemanticHashLess{});
            Publish(next);
            return result;
        } catch (const std::length_error&) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::OutOfMemory));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::OutOfMemory));
        }
    }

    std::expected<void, PortablePackageRegistryError> PortablePackageRegistry::Clear() noexcept {
        try {
            std::lock_guard lock{writer_};
            if (Snapshot()->PackageCount() == 0) {
                return {};
            }
            Publish(std::make_shared<PortablePackageRegistrySnapshot>());
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(PortablePackageRegistryErrorCode::OutOfMemory));
        }
    }

} // namespace Sora::Compute
