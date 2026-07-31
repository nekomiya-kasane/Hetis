/**
 * @file PortablePackageRegistry.h
 * @brief Transactional immutable registry for canonical programs and exact
 * artifact seeds.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/PortablePackage.h>
#include <Sora/Compute/Runtime/PortablePackageTrust.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Hard ownership budgets applied atomically to every registry
     * publication. */
    struct PortablePackageRegistryLimits {
        size_t maximumPackages = 4'096;
        size_t maximumArtifacts = 65'536;
        size_t maximumProvenanceRecords = 65'536;
        uint64_t maximumOwnedBytes = uint64_t{4} * 1'024 * 1'024 * 1'024;

        /** @brief Return whether every registry budget admits at least one non-empty
         * program. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return maximumPackages != 0 && maximumProvenanceRecords != 0 && maximumOwnedBytes != 0;
        }

        friend constexpr bool operator==(PortablePackageRegistryLimits,
                                         PortablePackageRegistryLimits) noexcept = default;
    };

    /** @brief Stable failure while resolving or transactionally publishing portable
     * packages. */
    enum class PortablePackageRegistryErrorCode : uint8_t {
        InvalidPackage,
        NotFound,
        ProgramIdentityCollision,
        PackageConflict,
        ArtifactIdentityCollision,
        InvalidProvenance,
        InvalidRevocation,
        PackageCapacityExceeded,
        ArtifactCapacityExceeded,
        ProvenanceCapacityExceeded,
        ByteCapacityExceeded,
        OutOfMemory,
    };

    /** @brief Registry failure located at one program and optional artifact
     * identity. */
    struct PortablePackageRegistryError {
        PortablePackageRegistryErrorCode code = PortablePackageRegistryErrorCode::InvalidPackage;
        SemanticHash program{};
        SemanticHash artifact{};
        uint64_t detail = 0;

        friend constexpr bool operator==(PortablePackageRegistryError, PortablePackageRegistryError) noexcept = default;
    };

    /** @brief Exact effect of one atomic current-generation admission revocation. */
    struct PortablePackageAdmissionRevocationResult {
        size_t provenanceRemoved = 0;
        size_t packagesUpdated = 0;
        size_t packagesErased = 0;

        /** @brief Return whether the transaction published a new registry generation. */
        [[nodiscard]] constexpr bool Changed() const noexcept { return provenanceRemoved != 0; }

        friend constexpr bool operator==(PortablePackageAdmissionRevocationResult,
                                         PortablePackageAdmissionRevocationResult) noexcept = default;
    };

    /** @brief Owning lease for one immutable portable package retained across erase
     * and clear. */
    class PortablePackageLease final {
    public:
        PortablePackageLease() noexcept = default;

        /** @brief Return whether this lease owns one validated portable package. */
        [[nodiscard]] explicit operator bool() const noexcept { return generation_ != nullptr; }

        /** @brief Return the canonical program carried by this package. @pre This
         * lease is non-empty. */
        [[nodiscard]] const ProgramPackage& Program() const noexcept {
            assert(generation_ != nullptr);
            return generation_->owner->Program();
        }

        /** @brief Return the complete validated package. @pre This lease is
         * non-empty. */
        [[nodiscard]] const PortablePackage& Package() const noexcept {
            assert(generation_ != nullptr);
            return *generation_->owner;
        }

        /** @brief Return every canonical admission identity known to this immutable lease generation. */
        [[nodiscard]] std::span<const PortablePackageProvenance> Provenance() const noexcept {
            return generation_ != nullptr ? std::span<const PortablePackageProvenance>{generation_->provenance}
                                          : std::span<const PortablePackageProvenance>{};
        }

        /** @brief Return the number of owners retaining this immutable admission generation. */
        [[nodiscard]] long UseCount() const noexcept { return generation_.use_count(); }

    private:
        friend class PortablePackageRegistrySnapshot;
        friend class PortablePackageRegistry;

        struct Generation final {
            std::shared_ptr<const PortablePackage> owner{};
            std::vector<PortablePackageProvenance> provenance{};
        };

        explicit PortablePackageLease(std::shared_ptr<const Generation> generation) noexcept
            : generation_(std::move(generation)) {}

        std::shared_ptr<const Generation> generation_{};
    };

    /** @brief Exact backend-private bytes with an owner lease spanning the complete
     * import call. */
    class PackagedArtifactSeed final {
    public:
        PackagedArtifactSeed() = delete;

        /** @brief Return backend-private persistent bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return bytes_; }

        /** @brief Return the package lease retaining @ref Bytes. */
        [[nodiscard]] const PortablePackageLease& Owner() const noexcept { return owner_; }

    private:
        friend class PortablePackageRegistrySnapshot;

        PackagedArtifactSeed(std::span<const std::byte> bytes, PortablePackageLease owner) noexcept
            : bytes_(bytes), owner_(std::move(owner)) {}

        std::span<const std::byte> bytes_{};
        PortablePackageLease owner_{};
    };

    /** @brief One immutable, internally coherent program-and-artifact registry
     * generation. */
    class PortablePackageRegistrySnapshot final {
        struct ProgramRecord {
            SemanticHash identity{};
            std::shared_ptr<const PortablePackageLease::Generation> generation{};
        };

        struct ArtifactRecord {
            ArtifactKey key{};
            size_t programIndex = 0;
            size_t packageArtifact = 0;
        };

    public:
        PortablePackageRegistrySnapshot() = default;

        /** @brief Resolve one owning package lease by canonical program identity. */
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageRegistryError>
        Resolve(SemanticHash program) const noexcept;

        /** @brief Find one complete exact-key artifact while retaining its package
         * owner. */
        [[nodiscard]] std::optional<PackagedArtifactSeed> FindArtifact(const ArtifactKey& key) const noexcept;

        /** @brief Return the number of canonical program packages in this generation.
         */
        [[nodiscard]] size_t PackageCount() const noexcept { return programs_.size(); }

        /** @brief Return the number of exact artifact seeds in this generation. */
        [[nodiscard]] size_t ArtifactCount() const noexcept { return artifacts_.size(); }

        /** @brief Return the number of canonical admission identities in this generation. */
        [[nodiscard]] size_t ProvenanceCount() const noexcept { return provenanceCount_; }

        /** @brief Return canonical program and artifact payload bytes owned by this
         * generation. */
        [[nodiscard]] uint64_t OwnedBytes() const noexcept { return ownedBytes_; }

    private:
        friend class PortablePackageRegistry;

        std::vector<ProgramRecord> programs_{};
        std::vector<ArtifactRecord> artifacts_{};        ///< Exact-key hot lookup index.
        std::vector<SemanticHash> artifactIdentities_{}; ///< Publication-only collision index.
        size_t provenanceCount_ = 0;
        uint64_t ownedBytes_ = 0;
    };

    /**
     * @brief Serialized-writer registry publishing immutable reader snapshots.
     * @details Registration, erase, and clear have strong transaction semantics.
     * Readers acquire one generation and perform binary searches without taking the
     * writer mutex or allocating storage.
     */
    class PortablePackageRegistry final {
    public:
        /** @brief Construct an empty registry. @throws std::invalid_argument if @p limits are invalid. */
        explicit PortablePackageRegistry(PortablePackageRegistryLimits limits = {});

        PortablePackageRegistry(const PortablePackageRegistry&) = delete;
        PortablePackageRegistry& operator=(const PortablePackageRegistry&) = delete;

        /** @brief Validate, serialize, and atomically register one program with no
         * AOT artifact seeds. */
        template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageRegistryError>
        Register(const KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>& program,
                 PortablePackageProvenance provenance = PortablePackageProvenance::Direct()) noexcept {
            auto canonical = ProgramPackage::Create(program);
            if (!canonical) {
                PortablePackageRegistryErrorCode code = PortablePackageRegistryErrorCode::InvalidPackage;
                if (canonical.error().code == ProgramPackageErrorCode::CapacityExceeded) {
                    code = PortablePackageRegistryErrorCode::ByteCapacityExceeded;
                } else if (canonical.error().code == ProgramPackageErrorCode::OutOfMemory) {
                    code = PortablePackageRegistryErrorCode::OutOfMemory;
                }
                return std::unexpected(PortablePackageRegistryError{
                    .code = code,
                    .program = canonical.error().program,
                    .detail = canonical.error().detail,
                });
            }
            auto package = PortablePackage::Create(std::move(*canonical), {});
            if (!package) {
                PortablePackageRegistryErrorCode code = PortablePackageRegistryErrorCode::InvalidPackage;
                if (package.error().code == PortablePackageErrorCode::CapacityExceeded) {
                    code = PortablePackageRegistryErrorCode::ArtifactCapacityExceeded;
                } else if (package.error().code == PortablePackageErrorCode::OutOfMemory) {
                    code = PortablePackageRegistryErrorCode::OutOfMemory;
                }
                return std::unexpected(PortablePackageRegistryError{
                    .code = code,
                    .program = program.Hash(),
                    .artifact = package.error().identity,
                    .detail = static_cast<uint64_t>(package.error().code),
                });
            }
            return Register(std::move(*package), provenance);
        }

        /** @brief Atomically register @p package or return the existing lease for an
         * exact duplicate. */
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageRegistryError>
        Register(PortablePackage package,
                 PortablePackageProvenance provenance = PortablePackageProvenance::Direct()) noexcept;

        /** @brief Resolve from one atomically acquired immutable generation. */
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageRegistryError>
        Resolve(SemanticHash program) const noexcept;

        /** @brief Find an exact artifact seed from one atomically acquired immutable
         * generation. */
        [[nodiscard]] std::optional<PackagedArtifactSeed> FindArtifact(const ArtifactKey& key) const noexcept;

        /** @brief Remove one package in a new generation; existing snapshots and
         * leases remain valid. */
        [[nodiscard]] std::expected<bool, PortablePackageRegistryError> Erase(SemanticHash program) noexcept;

        /**
         * @brief Atomically remove matching detached admissions from the current generation.
         * @details Packages losing their final admission are erased. Existing snapshots and leases remain valid.
         * The external trust policy remains responsible for rejecting matching future admissions.
         */
        [[nodiscard]] std::expected<PortablePackageAdmissionRevocationResult, PortablePackageRegistryError>
        RevokeAdmissions(PortablePackageAdmissionRevocation revocation) noexcept;

        /** @brief Publish a new empty generation; existing snapshots and leases
         * remain valid. */
        [[nodiscard]] std::expected<void, PortablePackageRegistryError> Clear() noexcept;

        /** @brief Acquire one immutable registry generation. */
        [[nodiscard]] std::shared_ptr<const PortablePackageRegistrySnapshot> Snapshot() const noexcept;

        /** @brief Return immutable ownership budgets. */
        [[nodiscard]] PortablePackageRegistryLimits Limits() const noexcept { return limits_; }

    private:
        void Publish(std::shared_ptr<const PortablePackageRegistrySnapshot> snapshot) noexcept;

        PortablePackageRegistryLimits limits_{};
        mutable std::mutex writer_{};
#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<const PortablePackageRegistrySnapshot>> snapshot_{};
#else
        std::shared_ptr<const PortablePackageRegistrySnapshot> snapshot_{};
#endif
    };

    static_assert(std::is_trivially_copyable_v<PortablePackageRegistryLimits>);
    static_assert(std::is_trivially_copyable_v<PortablePackageRegistryError>);
    static_assert(std::is_trivially_copyable_v<PortablePackageAdmissionRevocationResult>);

} // namespace Sora::Compute
