/**
 * @file PipelineService.h
 * @brief Cache-aware, per-key deduplicated driver pipeline service.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Pipeline.h>
#include <Sora/Compute/Detail/ArtifactResolutionCoordinator.h>

#include <cstdint>
#include <expected>

namespace Sora::Compute {

    /** @brief Monotonic observations of pipeline coordination and persistent-cache
     * behavior. */
    struct PipelineServiceStatistics {
        uint64_t memoryCacheHits = 0;
        uint64_t memoryCacheInsertFailures = 0;
        uint64_t packageArtifactHits = 0;
        uint64_t packageArtifactMisses = 0;
        uint64_t packageArtifactImportFailures = 0;
        uint64_t diskCacheHits = 0;
        uint64_t pipelineCreations = 0;
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

        friend constexpr bool operator==(PipelineServiceStatistics, PipelineServiceStatistics) noexcept = default;
    };

    /** @brief Coordinates cache lookup and at-most-once concurrent pipeline
     * creation for each exact key. */
    class PipelineService final {
    public:
        /** @brief Observe @p cache without taking ownership of it. */
        explicit PipelineService(ArtifactCache& cache, ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, observer) {}

        /** @brief Observe @p cache and @p store without taking ownership of either
         * service. */
        PipelineService(ArtifactCache& cache, ArtifactStore& store, ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, store, observer) {}

        /** @brief Observe @p cache and transactional @p packages without taking
         * ownership. */
        PipelineService(ArtifactCache& cache, const PortablePackageRegistry& packages,
                        ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, packages, observer) {}

        /** @brief Observe every cache tier without taking ownership of any service.
         */
        PipelineService(ArtifactCache& cache, ArtifactStore& store, const PortablePackageRegistry& packages,
                        ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, store, packages, observer) {}

        /** @brief Observe transactional packages and an optional persistent store without taking ownership. */
        PipelineService(ArtifactCache& cache, ArtifactStore* store, const PortablePackageRegistry& packages,
                        ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, store, packages, observer) {}

        PipelineService(const PipelineService&) = delete;
        PipelineService& operator=(const PipelineService&) = delete;

        /** @brief Resolve from cache or create exactly once while concurrent callers
         * share the result. */
        [[nodiscard]] std::expected<Artifact, PipelineError>
        Resolve(const BackendPipelineFactory& factory, const Artifact& executable, PipelineStateKey state) noexcept;

        /** @brief Sample cache, pipeline creation, and persistence outcomes. */
        [[nodiscard]] PipelineServiceStatistics Statistics() const noexcept;

    private:
        Detail::ArtifactResolutionCoordinator<PipelineError> coordinator_;
    };

} // namespace Sora::Compute
