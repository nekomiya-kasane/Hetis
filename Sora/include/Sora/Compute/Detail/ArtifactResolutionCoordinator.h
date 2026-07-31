/**
 * @file ArtifactResolutionCoordinator.h
 * @brief Shared cache and in-flight coordination for stage-specific artifact
 * producers.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/ArtifactCache.h>
#include <Sora/Compute/ArtifactStore.h>
#include <Sora/Compute/Runtime/PortablePackageRegistry.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Sora::Compute::Detail {

    /** @brief Domain-neutral observations produced by one artifact-resolution
     * coordinator. */
    struct ArtifactResolutionStatistics {
        uint64_t memoryCacheHits = 0;
        uint64_t memoryCacheInsertFailures = 0;
        uint64_t packageArtifactHits = 0;
        uint64_t packageArtifactMisses = 0;
        uint64_t packageArtifactImportFailures = 0;
        uint64_t diskCacheHits = 0;
        uint64_t creations = 0;
        uint64_t diskCacheMisses = 0;
        uint64_t diskLoadFailures = 0;
        uint64_t artifactImportFailures = 0;
        uint64_t artifactExportFailures = 0;
        uint64_t diskStoreFailures = 0;
        uint64_t materializationLeaseAcquisitions = 0;
        uint64_t materializationLeaseContentions = 0;
        uint64_t materializationLeaseTimeouts = 0;
        uint64_t materializationLeaseFailures = 0;
        uint64_t postLeaseDiskCacheHits = 0;

        friend constexpr bool operator==(ArtifactResolutionStatistics, ArtifactResolutionStatistics) noexcept = default;
    };

    template<typename Operation, typename Result, typename... Arguments>
    concept NothrowArtifactOperation = requires(Operation&& operation, Arguments&&... arguments) {
        { std::forward<Operation>(operation)(std::forward<Arguments>(arguments)...) } noexcept -> std::same_as<Result>;
    };

    /** @brief Per-key cache, persistence, and concurrent creation coordination
     * shared by artifact stages. */
    template<typename Error>
        requires std::is_nothrow_copy_constructible_v<Error> && std::is_nothrow_copy_assignable_v<Error>
    class ArtifactResolutionCoordinator final {
        struct InFlight {
            std::condition_variable completed;
            std::optional<std::expected<Artifact, Error>> result;
        };

    public:
        /** @brief Observe @p cache without taking ownership of it. */
        explicit ArtifactResolutionCoordinator(ArtifactCache& cache, ArtifactResolutionObserver observer = {}) noexcept
            : cache_(cache), observer_(observer) {}

        /** @brief Observe @p cache and @p store without taking ownership of either
         * service. */
        ArtifactResolutionCoordinator(ArtifactCache& cache, ArtifactStore& store,
                                      ArtifactResolutionObserver observer = {}) noexcept
            : cache_(cache), store_(&store), observer_(observer) {}

        /** @brief Observe @p cache and transactional @p packages without taking
         * ownership. */
        ArtifactResolutionCoordinator(ArtifactCache& cache, const PortablePackageRegistry& packages,
                                      ArtifactResolutionObserver observer = {}) noexcept
            : cache_(cache), packages_(&packages), observer_(observer) {}

        /** @brief Observe every cache tier without taking ownership of any service.
         */
        ArtifactResolutionCoordinator(ArtifactCache& cache, ArtifactStore& store,
                                      const PortablePackageRegistry& packages,
                                      ArtifactResolutionObserver observer = {}) noexcept
            : cache_(cache), store_(&store), packages_(&packages), observer_(observer) {}

        /** @brief Observe optional persistence and transactional package tiers without taking ownership. */
        ArtifactResolutionCoordinator(ArtifactCache& cache, ArtifactStore* store,
                                      const PortablePackageRegistry& packages,
                                      ArtifactResolutionObserver observer = {}) noexcept
            : cache_(cache), store_(store), packages_(&packages), observer_(observer) {}

        ArtifactResolutionCoordinator(const ArtifactResolutionCoordinator&) = delete;
        ArtifactResolutionCoordinator& operator=(const ArtifactResolutionCoordinator&) = delete;

        /** @brief Resolve @p key while coordinating cache, persistence, and one
         * stage-specific creation. */
        template<typename Create, typename Import, typename Export>
            requires NothrowArtifactOperation<Create, std::expected<Artifact, Error>> &&
                     NothrowArtifactOperation<Import, std::expected<Artifact, Error>, std::span<const std::byte>> &&
                     NothrowArtifactOperation<Export, std::expected<std::vector<std::byte>, Error>, const Artifact&>
        [[nodiscard]] std::expected<Artifact, Error>
        Resolve(const ArtifactKey& key, BackendServiceId service, bool supportsPersistence, Error outOfMemory,
                Create&& create, Import&& import, Export&& exportArtifact) noexcept {
            const LiveArtifactKey liveKey{key, service};
            if (auto cached = cache_.Find(liveKey)) {
                memoryCacheHits_.fetch_add(1, std::memory_order_relaxed);
                return std::move(*cached);
            }

            std::shared_ptr<InFlight> state;
            try {
                std::unique_lock lock{mutex_};
                auto found = inFlight_.find(liveKey);
                if (found != inFlight_.end()) {
                    state = found->second;
                    state->completed.wait(lock, [&state] { return state->result.has_value(); });
                    return *state->result;
                }
                if (auto cached = cache_.Find(liveKey)) {
                    memoryCacheHits_.fetch_add(1, std::memory_order_relaxed);
                    return std::move(*cached);
                }
                state = std::make_shared<InFlight>();
                inFlight_.emplace(liveKey, state);
            } catch (const std::bad_alloc&) {
                return std::unexpected(outOfMemory);
            }

            const auto started = std::chrono::steady_clock::now();
            ArtifactResolutionPath path = ArtifactResolutionPath::TransientProduction;
            std::expected<Artifact, Error> result =
                ResolveUncached(key, supportsPersistence, path, std::forward<Create>(create),
                                std::forward<Import>(import), std::forward<Export>(exportArtifact));
            std::optional<ArtifactResolutionObservation> observation;
            if (result) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
                observation = {.key = key,
                               .path = path,
                               .nanoseconds = std::max(uint64_t{1}, static_cast<uint64_t>(elapsed.count()))};
                if (auto inserted = cache_.Insert(*result); !inserted) {
                    memoryCacheInsertFailures_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            {
                std::scoped_lock lock{mutex_};
                state->result = result;
                inFlight_.erase(liveKey);
            }
            state->completed.notify_all();
            if (observation) {
                observer_.Observe(*observation);
            }
            return result;
        }

        /** @brief Sample cache, creation, and persistence outcomes without blocking
         * active resolutions. */
        [[nodiscard]] ArtifactResolutionStatistics Statistics() const noexcept {
            return {
                .memoryCacheHits = memoryCacheHits_.load(std::memory_order_relaxed),
                .memoryCacheInsertFailures = memoryCacheInsertFailures_.load(std::memory_order_relaxed),
                .packageArtifactHits = packageArtifactHits_.load(std::memory_order_relaxed),
                .packageArtifactMisses = packageArtifactMisses_.load(std::memory_order_relaxed),
                .packageArtifactImportFailures = packageArtifactImportFailures_.load(std::memory_order_relaxed),
                .diskCacheHits = diskCacheHits_.load(std::memory_order_relaxed),
                .creations = creations_.load(std::memory_order_relaxed),
                .diskCacheMisses = diskCacheMisses_.load(std::memory_order_relaxed),
                .diskLoadFailures = diskLoadFailures_.load(std::memory_order_relaxed),
                .artifactImportFailures = artifactImportFailures_.load(std::memory_order_relaxed),
                .artifactExportFailures = artifactExportFailures_.load(std::memory_order_relaxed),
                .diskStoreFailures = diskStoreFailures_.load(std::memory_order_relaxed),
                .materializationLeaseAcquisitions = materializationLeaseAcquisitions_.load(std::memory_order_relaxed),
                .materializationLeaseContentions = materializationLeaseContentions_.load(std::memory_order_relaxed),
                .materializationLeaseTimeouts = materializationLeaseTimeouts_.load(std::memory_order_relaxed),
                .materializationLeaseFailures = materializationLeaseFailures_.load(std::memory_order_relaxed),
                .postLeaseDiskCacheHits = postLeaseDiskCacheHits_.load(std::memory_order_relaxed),
            };
        }

    private:
        template<typename Create, typename Import, typename Export>
        [[nodiscard]] std::expected<Artifact, Error>
        ResolveUncached(const ArtifactKey& key, bool supportsPersistence, ArtifactResolutionPath& path, Create&& create,
                        Import&& import, Export&& exportArtifact) noexcept {
            struct RejectedArtifactBytes {
                ArtifactDigest digest{};
                size_t size = 0;
            };

            if (packages_ != nullptr && supportsPersistence) {
                if (const auto packaged = packages_->FindArtifact(key)) {
                    auto imported = std::forward<Import>(import)(packaged->Bytes());
                    if (imported) {
                        packageArtifactHits_.fetch_add(1, std::memory_order_relaxed);
                        path = ArtifactResolutionPath::PackageImport;
                        return imported;
                    }
                    packageArtifactImportFailures_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    packageArtifactMisses_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            const bool persistent = store_ != nullptr && key.PersistentCacheable() && supportsPersistence;
            std::optional<ArtifactMaterializationLease> materializationLease;
            std::optional<RejectedArtifactBytes> rejectedDiskArtifact;
            if (persistent) {
                auto stored = store_->Load(key);
                if (stored) {
                    auto imported = std::forward<Import>(import)(*stored);
                    if (imported) {
                        diskCacheHits_.fetch_add(1, std::memory_order_relaxed);
                        path = ArtifactResolutionPath::PersistentImport;
                        return imported;
                    }
                    artifactImportFailures_.fetch_add(1, std::memory_order_relaxed);
                    rejectedDiskArtifact = {
                        .digest = DigestArtifactBytes(*stored),
                        .size = stored->size(),
                    };
                } else if (stored.error().code == ArtifactStoreErrorCode::NotFound) {
                    diskCacheMisses_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    diskLoadFailures_.fetch_add(1, std::memory_order_relaxed);
                }

                auto acquired = store_->AcquireMaterializationLease(key);
                if (acquired) {
                    materializationLeaseAcquisitions_.fetch_add(1, std::memory_order_relaxed);
                    if (acquired->ObservedContention()) {
                        materializationLeaseContentions_.fetch_add(1, std::memory_order_relaxed);
                    }
                    materializationLease.emplace(std::move(*acquired));

                    auto published = store_->Load(key);
                    if (published) {
                        const bool unchangedRejectedArtifact =
                            rejectedDiskArtifact && rejectedDiskArtifact->size == published->size() &&
                            rejectedDiskArtifact->digest == DigestArtifactBytes(*published);
                        if (!unchangedRejectedArtifact) {
                            auto imported = std::forward<Import>(import)(*published);
                            if (imported) {
                                diskCacheHits_.fetch_add(1, std::memory_order_relaxed);
                                postLeaseDiskCacheHits_.fetch_add(1, std::memory_order_relaxed);
                                path = ArtifactResolutionPath::PersistentImport;
                                return imported;
                            }
                            artifactImportFailures_.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (published.error().code != ArtifactStoreErrorCode::NotFound) {
                        diskLoadFailures_.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (acquired.error().code == ArtifactStoreErrorCode::LeaseTimeout) {
                    materializationLeaseTimeouts_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    materializationLeaseFailures_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            creations_.fetch_add(1, std::memory_order_relaxed);
            path =
                persistent ? ArtifactResolutionPath::PersistentProduction : ArtifactResolutionPath::TransientProduction;
            auto created = std::forward<Create>(create)();
            if (!created || !persistent) {
                return created;
            }

            auto bytes = std::forward<Export>(exportArtifact)(*created);
            if (!bytes) {
                artifactExportFailures_.fetch_add(1, std::memory_order_relaxed);
                return created;
            }
            if (auto stored = store_->Store(key, *bytes); !stored) {
                diskStoreFailures_.fetch_add(1, std::memory_order_relaxed);
            }
            return created;
        }

        ArtifactCache& cache_;
        ArtifactStore* store_ = nullptr;
        const PortablePackageRegistry* packages_ = nullptr;
        ArtifactResolutionObserver observer_{};
        std::mutex mutex_;
        std::unordered_map<LiveArtifactKey, std::shared_ptr<InFlight>, LiveArtifactKeyHash> inFlight_;
        std::atomic<uint64_t> memoryCacheHits_ = 0;
        std::atomic<uint64_t> memoryCacheInsertFailures_ = 0;
        std::atomic<uint64_t> packageArtifactHits_ = 0;
        std::atomic<uint64_t> packageArtifactMisses_ = 0;
        std::atomic<uint64_t> packageArtifactImportFailures_ = 0;
        std::atomic<uint64_t> diskCacheHits_ = 0;
        std::atomic<uint64_t> creations_ = 0;
        std::atomic<uint64_t> diskCacheMisses_ = 0;
        std::atomic<uint64_t> diskLoadFailures_ = 0;
        std::atomic<uint64_t> artifactImportFailures_ = 0;
        std::atomic<uint64_t> artifactExportFailures_ = 0;
        std::atomic<uint64_t> diskStoreFailures_ = 0;
        std::atomic<uint64_t> materializationLeaseAcquisitions_ = 0;
        std::atomic<uint64_t> materializationLeaseContentions_ = 0;
        std::atomic<uint64_t> materializationLeaseTimeouts_ = 0;
        std::atomic<uint64_t> materializationLeaseFailures_ = 0;
        std::atomic<uint64_t> postLeaseDiskCacheHits_ = 0;
    };

} // namespace Sora::Compute::Detail
