/**
 * @file ArtifactStore.h
 * @brief Validated, content-addressed persistence for portable compute artifacts.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>
#include <Sora/Core/PAL/InterprocessFileLock.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable 256-bit digest used for artifact addressing and accidental-corruption detection. */
    struct ArtifactDigest {
        std::array<uint64_t, 4> words{};

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return words[0] != 0 || words[1] != 0 || words[2] != 0 || words[3] != 0;
        }

        friend constexpr bool operator==(ArtifactDigest, ArtifactDigest) noexcept = default;
    };

    /** @brief Compute a deterministic 256-bit non-cryptographic integrity digest. */
    [[nodiscard]] constexpr ArtifactDigest DigestArtifactBytes(std::span<const std::byte> bytes) noexcept {
        std::array<uint64_t, 4> state{0x6A09E667F3BCC909ULL, 0xBB67AE8584CAA73BULL, 0x3C6EF372FE94F82BULL,
                                      0xA54FF53A5F1D36F1ULL};
        constexpr std::array<uint64_t, 4> primes{0x100000001B3ULL, 0x9E3779B185EBCA87ULL, 0xC2B2AE3D27D4EB4FULL,
                                                 0x165667B19E3779F9ULL};
        for (const std::byte byte : bytes) {
            const uint64_t value = std::to_integer<uint8_t>(byte);
            for (size_t lane = 0; lane < state.size(); ++lane) {
                state[lane] ^= value + (lane * 0x9DU);
                state[lane] *= primes[lane];
                state[lane] = std::rotl(state[lane], static_cast<int>(11 + lane * 7));
            }
        }
        for (size_t lane = 0; lane < state.size(); ++lane) {
            state[lane] ^= static_cast<uint64_t>(bytes.size()) + (lane << 32U);
            state[lane] ^= state[lane] >> 30U;
            state[lane] *= 0xBF58476D1CE4E5B9ULL;
            state[lane] ^= state[lane] >> 27U;
            state[lane] *= 0x94D049BB133111EBULL;
            state[lane] ^= state[lane] >> 31U;
        }
        return {state};
    }

    /** @brief Stable ArtifactStore failure independent of a concrete filesystem API. */
    enum class ArtifactStoreErrorCode : uint8_t {
        InvalidConfiguration,
        InvalidKey,
        NonPersistentIdentity,
        NotFound,
        UnsupportedVersion,
        Corrupted,
        PayloadTooLarge,
        CapacityExceeded,
        LeaseContended,
        LeaseTimeout,
        IoError,
        OutOfMemory,
    };

    /** @brief ArtifactStore failure with a format offset, size, or nested PAL error identity. */
    struct ArtifactStoreError {
        ArtifactStoreErrorCode code = ArtifactStoreErrorCode::IoError;
        uint64_t detail = 0;

        friend constexpr bool operator==(ArtifactStoreError, ArtifactStoreError) noexcept = default;
    };

    /** @brief Immutable construction policy for one disk artifact namespace. */
    struct ArtifactStoreConfig {
        std::filesystem::path root;
        uint64_t maximumPayloadBytes = uint64_t{1} << 30U;
        uint64_t maximumStorageBytes = uint64_t{8} << 30U;
        uint64_t maximumEntryCount = 4096;
        uint64_t reconciliationPublicationInterval = 256;
        uint64_t reconciliationByteInterval = uint64_t{64} << 20U;
        uint32_t materializationLeaseStripes = 4096;
        std::chrono::milliseconds materializationLeaseTimeout = std::chrono::minutes{5};
        std::chrono::milliseconds leasePollInterval = std::chrono::milliseconds{1};
        bool durableWrites = true;
    };

    /** @brief Owning cross-process lease for materializing one bounded artifact-key stripe. */
    class ArtifactMaterializationLease final {
    public:
        ArtifactMaterializationLease(const ArtifactMaterializationLease&) = delete;
        ArtifactMaterializationLease& operator=(const ArtifactMaterializationLease&) = delete;
        ArtifactMaterializationLease(ArtifactMaterializationLease&&) noexcept = default;
        ArtifactMaterializationLease& operator=(ArtifactMaterializationLease&&) noexcept = default;

        /** @brief Return whether acquisition observed an existing owner before succeeding. */
        [[nodiscard]] bool ObservedContention() const noexcept { return observedContention_; }

    private:
        friend class ArtifactStore;

        ArtifactMaterializationLease(PAL::InterprocessFileLock lock, bool observedContention) noexcept
            : lock_(std::move(lock)), observedContention_(observedContention) {}

        PAL::InterprocessFileLock lock_;
        bool observedContention_ = false;
    };

    /** @brief Result of one deterministic oldest-publication-first cache maintenance pass. */
    struct ArtifactStoreMaintenance {
        uint64_t scannedEntries = 0;
        uint64_t scannedBytes = 0;
        uint64_t removedEntries = 0;
        uint64_t removedBytes = 0;
        uint64_t retainedEntries = 0;
        uint64_t retainedBytes = 0;

        friend constexpr bool operator==(ArtifactStoreMaintenance, ArtifactStoreMaintenance) noexcept = default;
    };

    /**
     * @brief Disk cache for backend-produced byte artifacts.
     * @details Entries use canonical little-endian headers, exact key words, payload digests, and same-directory
     * atomic publication. Runtime handles and C++ object representations are never serialized.
     */
    class ArtifactStore final {
    public:
        ArtifactStore(const ArtifactStore&) = delete;
        ArtifactStore& operator=(const ArtifactStore&) = delete;
        ArtifactStore(ArtifactStore&&) noexcept = default;
        ArtifactStore& operator=(ArtifactStore&&) noexcept = default;

        /** @brief Validate @p config and create its root directory when absent. */
        [[nodiscard]] static std::expected<ArtifactStore, ArtifactStoreError>
        Create(ArtifactStoreConfig config) noexcept;

        /** @brief Load and validate the exact entry for @p key. */
        [[nodiscard]] std::expected<std::vector<std::byte>, ArtifactStoreError>
        Load(const ArtifactKey& key) const noexcept;

        /** @brief Atomically publish @p payload for @p key after validating the persistence gate. */
        [[nodiscard]] std::expected<void, ArtifactStoreError> Store(const ArtifactKey& key,
                                                                    std::span<const std::byte> payload) const noexcept;

        /** @brief Acquire the bounded cross-process materialization stripe for @p key or time out. */
        [[nodiscard]] std::expected<ArtifactMaterializationLease, ArtifactStoreError>
        AcquireMaterializationLease(const ArtifactKey& key) const noexcept;

        /** @brief Enforce configured total-byte and entry-count retention budgets. */
        [[nodiscard]] std::expected<ArtifactStoreMaintenance, ArtifactStoreError> Maintain() const noexcept;

        /** @brief Return the deterministic entry path, primarily for diagnostics and integrity tests. */
        [[nodiscard]] std::expected<std::filesystem::path, ArtifactStoreError>
        EntryPath(const ArtifactKey& key) const noexcept;

        /** @brief Return this store's root namespace. */
        [[nodiscard]] const std::filesystem::path& Root() const noexcept { return config_.root; }

    private:
        struct MaintenanceState {
            std::mutex mutex;
            uint64_t estimatedEntries = 0;
            uint64_t estimatedBytes = 0;
            uint64_t publicationsSinceReconciliation = 0;
            uint64_t bytesSinceReconciliation = 0;
        };

        ArtifactStore(ArtifactStoreConfig config, std::unique_ptr<MaintenanceState> maintenance) noexcept
            : config_(std::move(config)), maintenance_(std::move(maintenance)) {}

        [[nodiscard]] std::expected<ArtifactStoreMaintenance, ArtifactStoreError>
        MaintainLocked(const std::filesystem::path* protectedEntry) const noexcept;

        [[nodiscard]] std::expected<std::filesystem::path, ArtifactStoreError>
        MaterializationLeasePath(const ArtifactKey& key) const noexcept;

        [[nodiscard]] std::expected<std::optional<PAL::InterprocessFileLock>, ArtifactStoreError>
        TryAcquireMaintenanceLease() const noexcept;

        [[nodiscard]] std::expected<void, ArtifactStoreError> AccountPublication(const std::filesystem::path& path,
                                                                                 std::optional<uint64_t> previousSize,
                                                                                 uint64_t publishedSize) const noexcept;

        ArtifactStoreConfig config_;
        std::unique_ptr<MaintenanceState> maintenance_;
    };

} // namespace Sora::Compute
