/**
 * @file CompilerService.cpp
 * @brief Cache-aware runtime compiler coordination.
 * @ingroup Compute
 */

#include <Sora/Compute/CompilerService.h>

namespace Sora::Compute {

    std::expected<Artifact, CompileError> CompilerService::Resolve(const BackendCompiler& compiler,
                                                                   const BackendCompileRequest& request) noexcept {
        const ArtifactKey key = request.ExpectedKey();
        if (!compiler.Accepts(request)) {
            return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
        }
        return coordinator_.Resolve(
            key, compiler.Service(), compiler.SupportsPersistentArtifacts(),
            CompileError{CompileErrorCode::OutOfMemory, 0},
            [&compiler, &request]() noexcept { return compiler.Compile(request); },
            [&compiler, &request](std::span<const std::byte> bytes) noexcept {
                return compiler.Import(request, bytes);
            },
            [&compiler](const Artifact& artifact) noexcept { return compiler.Export(artifact); });
    }

    CompilerServiceStatistics CompilerService::Statistics() const noexcept {
        const Detail::ArtifactResolutionStatistics statistics = coordinator_.Statistics();
        return {
            .memoryCacheHits = statistics.memoryCacheHits,
            .memoryCacheInsertFailures = statistics.memoryCacheInsertFailures,
            .packageArtifactHits = statistics.packageArtifactHits,
            .packageArtifactMisses = statistics.packageArtifactMisses,
            .packageArtifactImportFailures = statistics.packageArtifactImportFailures,
            .diskCacheHits = statistics.diskCacheHits,
            .compilations = statistics.creations,
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
