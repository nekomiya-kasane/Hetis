#include <Sora/Compute/Runtime/CandidateSynthesis.h>

#include <algorithm>
#include <memory>
#include <new>
#include <ranges>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr bool TargetMatches(const RuntimeDeviceBinding& binding) noexcept {
            return binding.service.Valid() && ValidDeviceDescriptor(binding.descriptor) && binding.target.Valid() &&
                   binding.target.device == binding.descriptor.id &&
                   binding.target.backend == binding.descriptor.backend &&
                   binding.target.features == binding.descriptor.features &&
                   binding.target.architectureId == binding.descriptor.architectureId;
        }

        [[nodiscard]] constexpr ExecutableKey ExecutableFor(const KernelCalibrationKey& candidate) noexcept {
            return {
                .lowered = candidate.kernel, .target = candidate.target, .specialization = candidate.specialization};
        }

        [[nodiscard]] constexpr bool PipelineMatchesCandidate(const PipelineKey& pipeline,
                                                              const KernelCalibrationKey& candidate) noexcept {
            const ArtifactKey executable = MakeArtifactKey(ExecutableFor(candidate));
            return executable.Valid() && pipeline.executable == ArtifactIdentity(executable) &&
                   pipeline.target == PipelineTarget(candidate.target) && MakeArtifactKey(pipeline).Valid();
        }

        [[nodiscard]] constexpr bool SameArtifactIdentity(const CandidateArtifactState& left,
                                                          const CandidateArtifactState& right) noexcept {
            if (left.service != right.service || left.candidate != right.candidate ||
                left.pipeline.has_value() != right.pipeline.has_value()) {
                return false;
            }
            return !left.pipeline || left.pipeline->key == right.pipeline->key;
        }

        [[nodiscard]] constexpr bool SameCandidate(const CandidateArtifactState& left,
                                                   const CandidateArtifactState& right) noexcept {
            return left.service == right.service && left.candidate == right.candidate;
        }

        [[nodiscard]] constexpr bool SameTransferIdentity(const CandidateTransferState& left,
                                                          const CandidateTransferState& right) noexcept {
            return left.service == right.service && left.candidate == right.candidate;
        }

        [[nodiscard]] ArtifactResolutionMode ExecutableResolutionFor(std::span<const CandidateArtifactState> artifacts,
                                                                     BackendServiceId service,
                                                                     const KernelCalibrationKey& key) noexcept {
            const auto found = std::ranges::find_if(artifacts, [&](const CandidateArtifactState& state) {
                return state.service == service && state.candidate == key;
            });
            return found == artifacts.end() ? ArtifactResolutionMode::TransientProduction : found->executable;
        }

        [[nodiscard]] constexpr PerformanceEvidence ObservedEvidence(uint32_t sampleCount) noexcept {
            const uint64_t confidence = static_cast<uint64_t>(sampleCount) * 1'000 / (sampleCount + 4ULL);
            return {.kind = PerformanceEvidenceKind::Observed,
                    .confidencePermille = static_cast<uint16_t>(std::max<uint64_t>(confidence, 1)),
                    .sampleCount = sampleCount};
        }

        [[nodiscard]] constexpr PerformanceEvidence PriorEvidence(const PerformancePriorModel& priors,
                                                                  BackendKind backend) noexcept {
            const auto policy = priors.Policy();
            const auto& prior = policy.artifacts[static_cast<size_t>(std::to_underlying(backend))];
            return {.kind = PerformanceEvidenceKind::Prior,
                    .confidencePermille = prior.confidencePermille,
                    .sampleCount = 0};
        }

        [[nodiscard]] std::optional<ArtifactResolutionForecast>
        ResolutionForecast(const PerformanceSnapshot& performance, const ArtifactKey& key, ArtifactResolutionMode mode,
                           BackendKind backend, bool pipeline, const PerformancePriorModel* priors) noexcept {
            if (mode == ArtifactResolutionMode::Live) {
                return ArtifactResolutionForecast{.mode = mode};
            }
            if (mode == ArtifactResolutionMode::Persistent) {
                if (const auto estimate = performance.EstimatePersistentResolution(key)) {
                    return ArtifactResolutionForecast{
                        .mode = mode,
                        .importNanoseconds = estimate->importNanoseconds,
                        .productionNanoseconds = estimate->productionNanoseconds,
                        .evidence = ObservedEvidence(estimate->importWeight + estimate->productionWeight)};
                }
                if (priors != nullptr) {
                    if (const auto estimate = priors->ForecastPersistent(backend, pipeline)) {
                        return ArtifactResolutionForecast{.mode = mode,
                                                          .importNanoseconds = estimate->importNanoseconds,
                                                          .productionNanoseconds = estimate->productionNanoseconds,
                                                          .evidence = PriorEvidence(*priors, backend)};
                    }
                }
                return std::nullopt;
            }
            const ArtifactResolutionPath path = mode == ArtifactResolutionMode::PackageImport
                                                    ? ArtifactResolutionPath::PackageImport
                                                    : ArtifactResolutionPath::TransientProduction;
            if (const auto estimate = performance.Estimate(key, path)) {
                return ArtifactResolutionForecast{
                    .mode = mode,
                    .importNanoseconds = mode == ArtifactResolutionMode::PackageImport ? estimate->nanoseconds : 0,
                    .productionNanoseconds =
                        mode == ArtifactResolutionMode::TransientProduction ? estimate->nanoseconds : 0,
                    .evidence = ObservedEvidence(estimate->sampleCount)};
            }
            if (priors == nullptr) {
                return std::nullopt;
            }
            const auto prior = priors->ForecastArtifact(backend, path, pipeline);
            if (!prior || !prior->Valid()) {
                return std::nullopt;
            }
            return ArtifactResolutionForecast{
                .mode = mode,
                .importNanoseconds = mode == ArtifactResolutionMode::PackageImport ? prior->nanoseconds : 0,
                .productionNanoseconds = mode == ArtifactResolutionMode::TransientProduction ? prior->nanoseconds : 0,
                .evidence = PriorEvidence(*priors, backend)};
        }

        [[nodiscard]] const CandidateTransferState* TransferFor(std::span<const CandidateTransferState> transfers,
                                                                BackendServiceId service,
                                                                const KernelCalibrationKey& key) noexcept {
            const auto found = std::ranges::find_if(transfers, [&](const CandidateTransferState& state) {
                return state.service == service && state.candidate == key;
            });
            return found == transfers.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] bool ValidRoute(const ResidencyRoute& route) noexcept {
            if (!route.buffer.Valid() || !route.source.allocation || !route.destinationService.Valid() ||
                !route.destination.Valid() || route.region.Size() == 0 || route.version == 0 ||
                !route.source.Covers(route.version, route.region)) {
                return false;
            }
            BackendServiceId service = route.source.allocation.View().service;
            MemoryLocation location = route.source.allocation.Location();
            for (const TransferLink& hop : route.hops) {
                if (!hop.Valid() || hop.sourceService != service || hop.source != location ||
                    route.region.Size() > hop.maximumTransferBytes) {
                    return false;
                }
                service = hop.service;
                location = hop.destination;
            }
            return service == route.destinationService && location == route.destination &&
                   (route.hops.empty() || route.estimatedNanoseconds != 0);
        }

        [[nodiscard]] bool ValidRoute(const GraphResidencyRoute& route) noexcept {
            return route.Valid();
        }

        template<typename Route>
        [[nodiscard]] std::expected<CandidateTransferState, CandidateSynthesisError>
        ProjectTransferCost(BackendServiceId service, const KernelCalibrationKey& candidate,
                            std::span<const Route> routes) noexcept {
            if (!service.Valid() || !candidate.Valid()) {
                return std::unexpected(CandidateSynthesisError::InvalidRequest);
            }
            CandidateTransferState projection{.service = service, .candidate = candidate};
            for (const Route& route : routes) {
                if (!ValidRoute(route) || route.destinationService != service) {
                    return std::unexpected(CandidateSynthesisError::InvalidRequest);
                }
                if (route.AlreadyResident()) {
                    continue;
                }
                if (route.region.Size() > UINT64_MAX - projection.bytes ||
                    route.estimatedNanoseconds > UINT64_MAX - projection.nanoseconds) {
                    return std::unexpected(CandidateSynthesisError::InvalidRequest);
                }
                projection.bytes += route.region.Size();
                projection.nanoseconds += route.estimatedNanoseconds;
            }
            return projection;
        }

    } // namespace

    std::expected<CandidateTransferState, CandidateSynthesisError>
    ProjectCandidateTransferCost(BackendServiceId service, const KernelCalibrationKey& candidate,
                                 std::span<const ResidencyRoute> routes) noexcept {
        return ProjectTransferCost(service, candidate, routes);
    }

    std::expected<CandidateTransferState, CandidateSynthesisError>
    ProjectCandidateTransferCost(BackendServiceId service, const KernelCalibrationKey& candidate,
                                 std::span<const GraphResidencyRoute> routes) noexcept {
        return ProjectTransferCost(service, candidate, routes);
    }

    std::expected<std::vector<PlacementCandidate>, CandidateSynthesisError>
    SynthesizeDispatchCandidates(const DispatchCandidateRequest& request, const PhysicalResourceInventory& resources,
                                 const PerformanceSnapshot& performance, const PerformancePriorModel* priors) noexcept {
        if (!request.workload.Valid()) {
            return std::unexpected(CandidateSynthesisError::InvalidRequest);
        }
        if (request.devices.size() != request.snapshots.size()) {
            return std::unexpected(CandidateSynthesisError::RuntimeContextMismatch);
        }
        for (size_t index = 0; index < request.artifacts.size(); ++index) {
            const CandidateArtifactState& state = request.artifacts[index];
            if (!state.service.Valid() || !state.candidate.Valid() || !ValidArtifactResolutionMode(state.executable) ||
                (state.pipeline && (!ValidArtifactResolutionMode(state.pipeline->resolution) ||
                                    !PipelineMatchesCandidate(state.pipeline->key, state.candidate)))) {
                return std::unexpected(CandidateSynthesisError::InvalidRequest);
            }
            for (size_t prior = 0; prior < index; ++prior) {
                if (SameArtifactIdentity(request.artifacts[prior], state) ||
                    (SameCandidate(request.artifacts[prior], state) &&
                     request.artifacts[prior].executable != state.executable)) {
                    return std::unexpected(CandidateSynthesisError::InvalidRequest);
                }
            }
        }
        for (size_t index = 0; index < request.transfers.size(); ++index) {
            if (!request.transfers[index].service.Valid() || !request.transfers[index].candidate.Valid()) {
                return std::unexpected(CandidateSynthesisError::InvalidRequest);
            }
            for (size_t prior = 0; prior < index; ++prior) {
                if (SameTransferIdentity(request.transfers[prior], request.transfers[index])) {
                    return std::unexpected(CandidateSynthesisError::InvalidRequest);
                }
            }
        }
        if (request.requireTransferProjection && request.transfers.empty()) {
            return std::unexpected(CandidateSynthesisError::TransferModelRequired);
        }
        if (!request.requireTransferProjection && !request.transfers.empty()) {
            return std::unexpected(CandidateSynthesisError::InvalidRequest);
        }

        try {
            std::vector<PlacementCandidate> candidates;
            candidates.reserve(std::max(resources.Queues().size(), request.artifacts.size()));
            for (size_t deviceIndex = 0; deviceIndex < request.devices.size(); ++deviceIndex) {
                const RuntimeDeviceBinding& binding = request.devices[deviceIndex];
                const DeviceSnapshot& snapshot = request.snapshots[deviceIndex];
                if (!TargetMatches(binding) || !ValidDeviceSnapshot(snapshot) || snapshot.id != binding.descriptor.id ||
                    snapshot.state != DeviceState::Available ||
                    !binding.descriptor.features.Contains(request.workload.requiredFeatures) ||
                    snapshot.availableMemoryBytes < request.workload.requiredDeviceMemoryBytes ||
                    binding.descriptor.maximumAllocationBytes < request.workload.largestAllocationBytes) {
                    continue;
                }
                const std::optional<KernelPerformanceForecast> priorForecast =
                    priors != nullptr ? priors->ForecastKernel(binding.descriptor, request.workload.resourceDemand,
                                                               request.workload.deterministic)
                                      : std::nullopt;
                for (const PhysicalCompilerDescriptor& compiler :
                     resources.CompilersFor(binding.service, binding.descriptor.backend)) {
                    const TargetFingerprint target =
                        ComposeTarget(binding.target, compiler.compiler, compiler.executableDomain);
                    if (!target.Valid()) {
                        continue;
                    }
                    for (const PhysicalQueueDescriptor& queue :
                         resources.QueuesFor(binding.service, binding.descriptor.backend, binding.descriptor.id)) {
                        if (!queue.queue.Valid() || !queue.capabilities.Supports(BackendCommandKind::Dispatch)) {
                            continue;
                        }
                        const KernelCalibrationKey key{.kernel = request.workload.kernelHash,
                                                       .target = target,
                                                       .specialization = request.specialization,
                                                       .queue = queue.queue};
                        std::optional<KernelPerformanceForecast> forecast;
                        if (const auto estimate = performance.Estimate(key, request.workload.workItemCount)) {
                            forecast = KernelPerformanceForecast{.estimate = *estimate,
                                                                 .evidence = ObservedEvidence(estimate->sampleCount)};
                        } else {
                            forecast = priorForecast;
                        }
                        if (!forecast || !forecast->Valid()) {
                            continue;
                        }
                        const ArtifactKey executableKey = MakeArtifactKey(ExecutableFor(key));
                        const ArtifactResolutionMode executableMode =
                            ExecutableResolutionFor(request.artifacts, binding.service, key);
                        const auto executable = ResolutionForecast(performance, executableKey, executableMode,
                                                                   binding.descriptor.backend, false, priors);
                        if (!executable) {
                            continue;
                        }
                        const CandidateTransferState* transfer = TransferFor(request.transfers, binding.service, key);
                        if (request.requireTransferProjection && transfer == nullptr) {
                            continue;
                        }
                        const auto append = [&](std::optional<PipelinePlacement> pipeline) {
                            candidates.push_back(PlacementCandidate{
                                .service = binding.service,
                                .device = binding.descriptor,
                                .target = target,
                                .specialization = request.specialization,
                                .queue = queue.queue,
                                .snapshot = snapshot,
                                .executable = *executable,
                                .pipeline = std::move(pipeline),
                                .workItemsPerSecond = forecast->estimate.workItemsPerSecond,
                                .transfer = transfer != nullptr ? std::optional{CandidateTransferCost{
                                                                      transfer->bytes, transfer->nanoseconds}}
                                                                : std::optional<CandidateTransferCost>{},
                                .launchNanoseconds = forecast->estimate.launchNanoseconds,
                                .synchronizationNanoseconds = forecast->estimate.synchronizationNanoseconds,
                                .deterministic = forecast->estimate.deterministic,
                                .kernelEvidence = forecast->evidence});
                        };
                        if (queue.dispatchArtifactKind == ArtifactKind::DeviceExecutable) {
                            append(std::nullopt);
                            continue;
                        }
                        if (queue.dispatchArtifactKind != ArtifactKind::DevicePipeline) {
                            continue;
                        }
                        for (const CandidateArtifactState& state : request.artifacts) {
                            if (state.service != binding.service || state.candidate != key || !state.pipeline) {
                                continue;
                            }
                            const PipelineConfiguration configuration{.service = state.service,
                                                                      .factory = state.pipeline->key.builder,
                                                                      .state = state.pipeline->key.state,
                                                                      .device = state.pipeline->key.target.device,
                                                                      .backend = state.pipeline->key.target.backend};
                            if (!resources.HasPipelineConfiguration(configuration)) {
                                continue;
                            }
                            const auto resolution = ResolutionForecast(
                                performance, MakeArtifactKey(state.pipeline->key), state.pipeline->resolution,
                                binding.descriptor.backend, true, priors);
                            if (!resolution) {
                                continue;
                            }
                            append(PipelinePlacement{.key = state.pipeline->key, .resolution = *resolution});
                        }
                    }
                }
            }
            return candidates;
        } catch (const std::bad_alloc&) {
            return std::unexpected(CandidateSynthesisError::OutOfMemory);
        }
    }

} // namespace Sora::Compute
