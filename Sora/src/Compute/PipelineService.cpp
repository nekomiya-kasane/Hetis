/**
 * @file PipelineService.cpp
 * @brief Cache-aware driver pipeline coordination.
 * @ingroup Compute
 */

#include <Sora/Compute/PipelineService.h>

namespace Sora::Compute {

    std::expected<Artifact, PipelineError> PipelineService::Resolve(const BackendPipelineFactory& factory,
                                                                    const Artifact& executable,
                                                                    PipelineStateKey state) noexcept {
        const ArtifactKey key = factory.ExpectedKey(executable, state);
        if (!factory.Accepts(executable, state)) {
            return std::unexpected(PipelineError{PipelineErrorCode::InvalidRequest, 0});
        }
        return coordinator_.Resolve(
            key, factory.Service(), factory.SupportsPersistentArtifacts(),
            PipelineError{PipelineErrorCode::OutOfMemory, 0},
            [&factory, &executable, state]() noexcept { return factory.CreatePipeline(executable, state); },
            [&factory, &executable, state](std::span<const std::byte> bytes) noexcept {
                return factory.Import(executable, state, bytes);
            },
            [&factory](const Artifact& pipeline) noexcept { return factory.Export(pipeline); });
    }

    PipelineServiceStatistics PipelineService::Statistics() const noexcept {
        const Detail::ArtifactResolutionStatistics statistics = coordinator_.Statistics();
        return {
            .memoryCacheHits = statistics.memoryCacheHits,
            .memoryCacheInsertFailures = statistics.memoryCacheInsertFailures,
            .packageArtifactHits = statistics.packageArtifactHits,
            .packageArtifactMisses = statistics.packageArtifactMisses,
            .packageArtifactImportFailures = statistics.packageArtifactImportFailures,
            .diskCacheHits = statistics.diskCacheHits,
            .pipelineCreations = statistics.creations,
            .diskCacheMisses = statistics.diskCacheMisses,
            .diskLoadFailures = statistics.diskLoadFailures,
            .artifactImportFailures = statistics.artifactImportFailures,
            .artifactExportFailures = statistics.artifactExportFailures,
            .diskStoreFailures = statistics.diskStoreFailures,
            .materializationLeaseAcquisitions = statistics.materializationLeaseAcquisitions,
            .materializationLeaseContentions = statistics.materializationLeaseContentions,
            .materializationLeaseTimeouts = statistics.materializationLeaseTimeouts,
            .materializationLeaseFailures = statistics.materializationLeaseFailures,
            .postLeaseDiskCacheHits = statistics.postLeaseDiskCacheHits,
        };
    }

} // namespace Sora::Compute
