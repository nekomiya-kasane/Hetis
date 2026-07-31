/**
 * @file CompilerService.h
 * @brief Cache-aware, per-key deduplicated runtime compilation service.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend.h>
#include <Sora/Compute/Detail/ArtifactResolutionCoordinator.h>

#include <expected>

namespace Sora::Compute {

    /** @brief Monotonic observations of compiler coordination and persistent-cache
     * behavior. */
    struct CompilerServiceStatistics {
        uint64_t memoryCacheHits = 0;
        uint64_t memoryCacheInsertFailures = 0;
        uint64_t packageArtifactHits = 0;
        uint64_t packageArtifactMisses = 0;
        uint64_t packageArtifactImportFailures = 0;
        uint64_t diskCacheHits = 0;
        uint64_t compilations = 0;
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

        friend constexpr bool operator==(CompilerServiceStatistics, CompilerServiceStatistics) noexcept = default;
    };

    /** @brief Coordinates cache lookup and at-most-once concurrent compilation for
     * each artifact key. */
    class CompilerService final {
    public:
        /** @brief Observe @p cache without taking ownership of it. */
        explicit CompilerService(ArtifactCache& cache, ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, observer) {}

        /** @brief Observe @p cache and @p store without taking ownership of either
         * service. */
        CompilerService(ArtifactCache& cache, ArtifactStore& store, ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, store, observer) {}

        /** @brief Observe @p cache and transactional @p packages without taking
         * ownership. */
        CompilerService(ArtifactCache& cache, const PortablePackageRegistry& packages,
                        ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, packages, observer) {}

        /** @brief Observe every cache tier without taking ownership of any service.
         */
        CompilerService(ArtifactCache& cache, ArtifactStore& store, const PortablePackageRegistry& packages,
                        ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, store, packages, observer) {}

        /** @brief Observe transactional packages and an optional persistent store without taking ownership. */
        CompilerService(ArtifactCache& cache, ArtifactStore* store, const PortablePackageRegistry& packages,
                        ArtifactResolutionObserver observer = {}) noexcept
            : coordinator_(cache, store, packages, observer) {}

        CompilerService(const CompilerService&) = delete;
        CompilerService& operator=(const CompilerService&) = delete;

        /** @brief Resolve from cache or compile exactly once while concurrent callers
         * wait for the same result. */
        [[nodiscard]] std::expected<Artifact, CompileError> Resolve(const BackendCompiler& compiler,
                                                                    const BackendCompileRequest& request) noexcept;

        /** @brief Sample cache, compilation, and persistence outcomes without
         * blocking active resolutions. */
        [[nodiscard]] CompilerServiceStatistics Statistics() const noexcept;

    private:
        Detail::ArtifactResolutionCoordinator<CompileError> coordinator_;
    };

} // namespace Sora::Compute
