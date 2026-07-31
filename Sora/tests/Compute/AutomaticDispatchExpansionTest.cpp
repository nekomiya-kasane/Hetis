/**
 * @file AutomaticDispatchExpansionTest.cpp
 * @brief Transfer-inclusive automatic-dispatch physical graph expansion tests.
 * @ingroup Compute
 */

#include "PlanningTestSupport.h"

#include <Sora/Compute/Runtime/AutomaticDispatchExpansion.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;
    using namespace Sora::Test::ComputePlanning;

    void CalibrateResolution(PerformanceModel& model, SemanticHash kernel, TargetFingerprint target,
                             uint64_t nanoseconds = 10'000) {
        const ArtifactKey artifact =
            MakeArtifactKey(ExecutableKey{.lowered = kernel, .target = target, .specialization = {}});
        for (const ArtifactResolutionPath path :
             {ArtifactResolutionPath::PackageImport, ArtifactResolutionPath::PersistentImport,
              ArtifactResolutionPath::PersistentProduction, ArtifactResolutionPath::TransientProduction}) {
            REQUIRE(model.Update({.key = artifact, .path = path, .nanoseconds = nanoseconds}).has_value());
        }
    }

    constexpr uint64_t kStridedElementCount = 1'000'000;
    constexpr uint64_t kStridedBufferBytes = (kStridedElementCount - 1) * 8 + sizeof(float);
    constexpr std::array kStridedExtents{kStridedElementCount};
    constexpr std::array kStridedStrides{uint64_t{8}};
    constexpr auto kStridedFootprint =
        AffineByteFootprint::Create(kStridedExtents, kStridedStrides, sizeof(float), 0, sizeof(float), 1);
    static_assert(kStridedFootprint.has_value());

    consteval auto MakeStridedCopyProgram() {
        KernelBuilder<1, 1> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<2, 1, 1> program{domain, std::move(scalar).Finish(value),
                                              PartitionContract::Elementwise(1)};
        const auto input = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Strided, 64, {1});
        const auto output = program.AddBufferArgument(AccessMode::Write, kFloat, 1, LayoutKind::Strided, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    constexpr auto kStridedCopyProgram = MakeStridedCopyProgram();
    constexpr uint64_t kAxisSearchRows = 3;
    constexpr uint64_t kAxisSearchColumns = 1'000;
    constexpr uint64_t kAxisSearchElementCount = kAxisSearchRows * kAxisSearchColumns;
    constexpr uint64_t kAxisSearchBufferBytes = kAxisSearchElementCount * sizeof(float);
    constexpr std::array kAxisSearchExtents{kAxisSearchRows, kAxisSearchColumns};

    consteval auto MakeAxisSearchProgram() {
        KernelBuilder<1, 1> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        constexpr IndexDomain domain{.rank = 2};
        KernelProgramBuilder<2, 1, 1> program{domain, std::move(scalar).Finish(value),
                                              PartitionContract::Elementwise(domain.rank)};
        const auto input =
            program.AddBufferArgument(AccessMode::Read, kFloat, domain.rank, LayoutKind::Strided, 64, {1});
        const auto output =
            program.AddBufferArgument(AccessMode::Write, kFloat, domain.rank, LayoutKind::Strided, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    constexpr auto kAxisSearchProgram = MakeAxisSearchProgram();
    constexpr DeviceId kGpuC{2, 4};
    constexpr MemoryLocation kDeviceC{.domain = {23, 23}, .device = kGpuC, .backend = BackendKind::Vulkan};
    constexpr QueueId kQueueC{33, 33};

    [[nodiscard]] Allocation MakeSecondaryAllocation(BackendService& service, std::atomic<uint32_t>& destroyCount,
                                                     MemoryLocation location = kDeviceB) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Allocation::Adopt(service, {.location = location,
                                                      .memory = {0xB, 0xD15CA11ULL},
                                                      .properties = MemoryProperty::DeviceLocal,
                                                      .sizeBytes = kBufferBytes,
                                                      .alignment = 64,
                                                      .state = native.release(),
                                                      .destroy = DestroyAllocation});
        REQUIRE(allocation.has_value());
        return std::move(*allocation);
    }

    [[nodiscard]] Allocation MakeStridedAllocation(const BackendService& service, MemoryLocation location,
                                                   PhysicalMemoryId memory, std::atomic<uint32_t>& destroyCount) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Allocation::Adopt(service, {.location = location,
                                                      .memory = memory,
                                                      .properties = MemoryProperty::DeviceLocal,
                                                      .sizeBytes = kStridedBufferBytes,
                                                      .alignment = 64,
                                                      .state = native.release(),
                                                      .destroy = DestroyAllocation});
        REQUIRE(allocation.has_value());
        return std::move(*allocation);
    }

    [[nodiscard]] Allocation MakeAxisSearchAllocation(const BackendService& service, MemoryLocation location,
                                                      PhysicalMemoryId memory, std::atomic<uint32_t>& destroyCount) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Allocation::Adopt(service, {.location = location,
                                                      .memory = memory,
                                                      .properties = MemoryProperty::DeviceLocal,
                                                      .sizeBytes = kAxisSearchBufferBytes,
                                                      .alignment = 64,
                                                      .state = native.release(),
                                                      .destroy = DestroyAllocation});
        REQUIRE(allocation.has_value());
        return std::move(*allocation);
    }

    [[nodiscard]] Buffer MakeAxisSearchBuffer(BufferId id) {
        auto buffer = Buffer::Create({.id = id,
                                      .elementType = kFloat,
                                      .layout = LayoutKind::Contiguous,
                                      .rank = 2,
                                      .sizeBytes = kAxisSearchBufferBytes,
                                      .alignment = 64});
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    [[nodiscard]] BufferView AxisSearchView(Buffer buffer) {
        auto view = BufferView::Contiguous(std::move(buffer), kAxisSearchExtents);
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    [[nodiscard]] KernelInvocation AxisSearchInvocation(Buffer input, Buffer output) {
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(AxisSearchView(std::move(input)));
        bindings.emplace_back(AxisSearchView(std::move(output)));
        auto invocation = BindKernelProgram(kAxisSearchProgram, kAxisSearchExtents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    [[nodiscard]] Buffer MakeStridedBuffer(BufferId id) {
        auto buffer = Buffer::Create({.id = id,
                                      .elementType = kFloat,
                                      .layout = LayoutKind::Strided,
                                      .rank = 1,
                                      .sizeBytes = kStridedBufferBytes,
                                      .alignment = 64},
                                     *kStridedFootprint);
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    [[nodiscard]] BufferView StridedView(Buffer buffer) {
        auto view = BufferView::Create(std::move(buffer), {.layout = LayoutKind::Strided,
                                                           .rank = 1,
                                                           .extents = {kStridedElementCount},
                                                           .byteStrides = {8},
                                                           .componentStrideBytes = sizeof(float)});
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    struct ExpansionFixture {
        PlanningFixture planning{{.includeSecondDevice = true}};
        std::atomic<uint32_t> destroyCount{0};
        BackendService secondaryService = MakeSecondaryService();
        Buffer input = MakeBuffer({0xA, 1});
        Buffer output = MakeBuffer({0xA, 2});
        std::optional<ArtifactAvailabilitySnapshot> artifacts;
        std::shared_ptr<const PerformanceSnapshot> performance;
        std::shared_ptr<const ProgramCatalogSnapshot> packages;
        ExecutionGraph graph;
        GraphResidencyState baseline;
        DispatchCandidateSet candidates;
        AutomaticDispatchExecution execution;

        ExpansionFixture()
            : graph(PrepareGraph()),
              baseline(PrepareBaseline()),
              candidates(EnumerateCandidates()),
              execution(PlanPartition()) {}

        [[nodiscard]] ExecutionGraph PrepareGraph() {
            REQUIRE(input.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
            const auto whole = ByteRegion::Make(0, kBufferBytes);
            REQUIRE(whole.has_value());
            REQUIRE(
                input.PublishReplica(1, *whole, MakeSecondaryAllocation(secondaryService, destroyCount)).has_value());
            CalibrateKernel(planning.performance, kQueueB, Target(kGpuB));
            return MakeGraph(Invocation(input, output));
        }

        [[nodiscard]] GraphResidencyState PrepareBaseline() {
            auto frozen = FreezeGraphResidency(graph);
            REQUIRE(frozen.has_value());
            return std::move(*frozen);
        }

        [[nodiscard]] DispatchCandidateSet EnumerateCandidates() {
            auto availability = planning.artifacts.SnapshotAvailability();
            REQUIRE(availability.has_value());
            artifacts.emplace(std::move(*availability));
            performance = planning.performance.Snapshot();
            packages = FreezeProgramCatalog(planning.packages);
            const std::array devices{
                planning.device,
                RuntimeDeviceBinding{
                    .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))},
            };
            const std::array snapshots{
                planning.snapshot,
                DeviceSnapshot{.id = kGpuB,
                               .state = DeviceState::Available,
                               .generation = 1,
                               .availableMemoryBytes = uint64_t{8} << 30U},
            };
            DispatchEnumerationPolicy policy;
            policy.memory.publication = DestinationPublication::Publish;
            auto enumerated = EnumerateDispatchCandidates(
                graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots}, Services(), baseline,
                policy);
            REQUIRE(enumerated.has_value());
            REQUIRE(enumerated->placements.size() == 2);
            return std::move(*enumerated);
        }

        [[nodiscard]] AutomaticDispatchExecution PlanPartition() {
            auto planned =
                PlanAutomaticDispatch(std::get<KernelInvocation>(graph.Operation({0})), candidates, Services(),
                                      baseline, {.maximumDevices = 2, .minimumImprovementPermille = 0});
            REQUIRE(planned.has_value());
            REQUIRE(planned->UsesPartitioning());
            return std::move(*planned);
        }

        [[nodiscard]] GraphPlanningServices Services() const noexcept {
            return {.packages = *packages,
                    .resources = planning.resources.Inventory(),
                    .artifacts = *artifacts,
                    .performance = *performance};
        }
    };

    struct DeviceSetSearchFixture {
        PlanningFixture planning{{.includeAlternativeQueue = true, .includeSecondDevice = true}, false};
        std::atomic<uint32_t> destroyCount{0};
        BackendService secondaryService = MakeSecondaryService();
        Buffer input = MakeBuffer({0xF, 1});
        Buffer output = MakeBuffer({0xF, 2});
        std::optional<ArtifactAvailabilitySnapshot> artifacts;
        std::shared_ptr<const PerformanceSnapshot> performance;
        std::shared_ptr<const ProgramCatalogSnapshot> packages;
        ExecutionGraph graph;
        GraphResidencyState baseline;
        DispatchCandidateSet candidates;

        explicit DeviceSetSearchFixture(uint64_t transferPayloadNanoseconds = 160'000,
                                        uint64_t transferLatencyNanoseconds = 80'000)
            : graph(PrepareGraph(transferPayloadNanoseconds, transferLatencyNanoseconds)),
              baseline(PrepareBaseline()),
              candidates(EnumerateCandidates()) {}

        void CalibrateQueue(QueueId queue, TargetFingerprint target, uint64_t computeNanoseconds) {
            REQUIRE(
                planning.performance
                    .Update(
                        {.key = {.kernel = kCopyProgram.Hash(), .target = target, .specialization = {}, .queue = queue},
                         .workItemCount = kElementCount,
                         .computeNanoseconds = computeNanoseconds,
                         .launchNanoseconds = 100})
                    .has_value());
            CalibrateResolution(planning.performance, kCopyProgram.Hash(), target);
        }

        [[nodiscard]] ExecutionGraph PrepareGraph(uint64_t transferPayloadNanoseconds,
                                                  uint64_t transferLatencyNanoseconds) {
            REQUIRE(input.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
            const auto whole = ByteRegion::Make(0, kBufferBytes);
            REQUIRE(whole.has_value());
            REQUIRE(input.PublishReplica(1, *whole, MakeSecondaryAllocation(secondaryService, destroyCount, kDeviceC))
                        .has_value());
            CalibrateQueue(kQueue, Target(), 100'000);
            CalibrateQueue(kAlternativeQueue, Target(), 90'000);
            CalibrateQueue(kQueueB, Target(kGpuB), 80'000);
            REQUIRE(planning.performance
                        .Update({.key = {.service = kServiceB,
                                         .engine = kPeerTransfer,
                                         .source = kDevice,
                                         .destination = kDeviceB},
                                 .bytes = kBufferBytes,
                                 .payloadNanoseconds = transferPayloadNanoseconds,
                                 .latencyNanoseconds = transferLatencyNanoseconds})
                        .has_value());
            return MakeGraph(Invocation(input, output));
        }

        [[nodiscard]] GraphResidencyState PrepareBaseline() {
            auto frozen = FreezeGraphResidency(graph);
            REQUIRE(frozen.has_value());
            return std::move(*frozen);
        }

        [[nodiscard]] DispatchCandidateSet EnumerateCandidates() {
            auto availability = planning.artifacts.SnapshotAvailability();
            REQUIRE(availability.has_value());
            artifacts.emplace(std::move(*availability));
            performance = planning.performance.Snapshot();
            packages = FreezeProgramCatalog(planning.packages);
            const std::array devices{
                planning.device,
                RuntimeDeviceBinding{
                    .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))},
            };
            const std::array snapshots{
                planning.snapshot,
                DeviceSnapshot{.id = kGpuB,
                               .state = DeviceState::Available,
                               .generation = 1,
                               .availableMemoryBytes = uint64_t{8} << 30U},
            };
            DispatchEnumerationPolicy policy;
            policy.memory.publication = DestinationPublication::Publish;
            auto enumerated = EnumerateDispatchCandidates(
                graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots}, Services(), baseline,
                policy);
            REQUIRE(enumerated.has_value());
            REQUIRE(enumerated->placements.size() == 3);

            const auto placementB = std::ranges::find_if(
                enumerated->placements, [](const auto& candidate) { return candidate.placement.device.id == kGpuB; });
            REQUIRE(placementB != enumerated->placements.end());
            REQUIRE(placementB->residencyProjection < enumerated->residencies.size());
            DispatchResidencyProjection residencyC = enumerated->residencies[placementB->residencyProjection];
            residencyC.device = kGpuC;
            REQUIRE(residencyC.destination.has_value());
            residencyC.destination->location = kDeviceC;
            residencyC.inputs.clear();
            residencyC.transfer = {};
            enumerated->residencies.push_back(std::move(residencyC));

            DispatchPlacementCandidate placementC = *placementB;
            placementC.placement.device = Descriptor(kGpuC);
            placementC.placement.target = Target(kGpuC);
            placementC.placement.queue = kQueueC;
            placementC.placement.snapshot.id = kGpuC;
            placementC.placement.workItemsPerSecond = 2'694'736;
            placementC.placement.transfer = CandidateTransferCost{};
            placementC.residencyProjection = static_cast<uint32_t>(enumerated->residencies.size() - 1);
            enumerated->placements.push_back(std::move(placementC));
            return std::move(*enumerated);
        }

        [[nodiscard]] GraphPlanningServices Services() const noexcept {
            return {.packages = *packages,
                    .resources = planning.resources.Inventory(),
                    .artifacts = *artifacts,
                    .performance = *performance};
        }

        [[nodiscard]] std::expected<AutomaticDispatchExecution, AutomaticDispatchPlanningError>
        Plan(const AutomaticPartitionPolicy& policy) const noexcept {
            return PlanAutomaticDispatch(std::get<KernelInvocation>(graph.Operation({0})), candidates, Services(),
                                         baseline, policy);
        }
    };

} // namespace

TEST_CASE("Transfer-aware partition search replaces a compute-optimal device and respects its total budget",
          "[Sora][Compute][Planner][Partition][Transfer][DeviceSet]") {
    DeviceSetSearchFixture fixture;
    const AutomaticPartitionPolicy oneEvaluation{
        .maximumDevices = 2, .maximumPartitionEvaluations = 1, .minimumImprovementPermille = 0};
    const auto bounded = fixture.Plan(oneEvaluation);
    REQUIRE(bounded.has_value());
    REQUIRE_FALSE(bounded->UsesPartitioning());

    const AutomaticPartitionPolicy searchPolicy{
        .maximumDevices = 2, .maximumPartitionEvaluations = 128, .minimumImprovementPermille = 0};
    const auto first = fixture.Plan(searchPolicy);
    const auto second = fixture.Plan(searchPolicy);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->UsesPartitioning());
    REQUIRE(second->UsesPartitioning());
    REQUIRE(first->partitioned->partitionEvaluations <= searchPolicy.maximumPartitionEvaluations);
    REQUIRE(first->partitioned->shards.size() == 2);

    const auto hasDevice = [&](const AutomaticDispatchExecution& execution, DeviceId device) {
        return std::ranges::any_of(execution.partitioned->shards,
                                   [&](const auto& shard) { return shard.execution.device == device; });
    };
    REQUIRE(hasDevice(*first, kGpu));
    REQUIRE_FALSE(hasDevice(*first, kGpuB));
    REQUIRE(hasDevice(*first, kGpuC));
    REQUIRE(first->partitioned->transfers.empty());
    REQUIRE(first->partitioned->totalNanoseconds == second->partitioned->totalNanoseconds);
    REQUIRE(first->partitioned->partitionEvaluations == second->partitioned->partitionEvaluations);
    for (size_t index = 0; index < first->partitioned->shards.size(); ++index) {
        REQUIRE(first->partitioned->shards[index].execution.device ==
                second->partitioned->shards[index].execution.device);
        REQUIRE(first->partitioned->shards[index].slice == second->partitioned->shards[index].slice);
    }
    REQUIRE(first->partitioned->shards[0].execution.device != first->partitioned->shards[1].execution.device);
}

TEST_CASE("Transfer-inclusive partition planning shares its budget across axes and selects the finer cover",
          "[Sora][Compute][Planner][Partition][Transfer][Axis]") {
    PlanningFixture planning{{.includeSecondDevice = true}, false};
    REQUIRE(planning.packages.Register(kAxisSearchProgram).has_value());
    const auto calibrate = [&](QueueId queue, TargetFingerprint target) {
        REQUIRE(planning.performance
                    .Update({.key = {.kernel = kAxisSearchProgram.Hash(),
                                     .target = target,
                                     .specialization = {},
                                     .queue = queue},
                             .workItemCount = kAxisSearchElementCount,
                             .computeNanoseconds = kAxisSearchElementCount,
                             .launchNanoseconds = 0})
                    .has_value());
        CalibrateResolution(planning.performance, kAxisSearchProgram.Hash(), target);
    };
    calibrate(kQueue, Target());
    calibrate(kQueueB, Target(kGpuB));

    std::atomic<uint32_t> destroyCount{0};
    BackendService secondaryService = MakeSecondaryService();
    Buffer input = MakeAxisSearchBuffer({0xAA, 1});
    Buffer output = MakeAxisSearchBuffer({0xAA, 2});
    REQUIRE(input
                .CommitWholeWrite(
                    MakeAxisSearchAllocation(Sora::Test::TestBackendService(), kDevice, {0xAA, 3}, destroyCount))
                .has_value());
    const auto whole = ByteRegion::Make(0, kAxisSearchBufferBytes);
    REQUIRE(whole.has_value());
    REQUIRE(
        input.PublishReplica(1, *whole, MakeAxisSearchAllocation(secondaryService, kDeviceB, {0xAA, 4}, destroyCount))
            .has_value());

    ExecutionGraph graph = MakeGraph(AxisSearchInvocation(input, output));
    auto baseline = FreezeGraphResidency(graph);
    REQUIRE(baseline.has_value());
    auto artifacts = planning.artifacts.SnapshotAvailability();
    REQUIRE(artifacts.has_value());
    const auto performance = planning.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(planning.packages);
    const std::array devices{
        planning.device,
        RuntimeDeviceBinding{
            .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))},
    };
    const std::array snapshots{
        planning.snapshot,
        DeviceSnapshot{
            .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U},
    };
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = planning.resources.Inventory(),
                                         .artifacts = *artifacts,
                                         .performance = *performance};
    DispatchEnumerationPolicy enumerationPolicy;
    enumerationPolicy.memory.publication = DestinationPublication::Publish;
    auto candidates =
        EnumerateDispatchCandidates(graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    services, *baseline, enumerationPolicy);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->placements.size() == 2);

    const auto insufficient =
        PlanAutomaticDispatch(std::get<KernelInvocation>(graph.Operation({0})), *candidates, services, *baseline,
                              {.maximumDevices = 2, .maximumPartitionEvaluations = 1, .minimumImprovementPermille = 0});
    REQUIRE_FALSE(insufficient.has_value());
    REQUIRE(insufficient.error() == AutomaticDispatchPlanningError::InsufficientSearchBudget);

    auto execution =
        PlanAutomaticDispatch(std::get<KernelInvocation>(graph.Operation({0})), *candidates, services, *baseline,
                              {.maximumDevices = 2, .maximumPartitionEvaluations = 2, .minimumImprovementPermille = 0});
    REQUIRE(execution.has_value());
    REQUIRE(execution->UsesPartitioning());
    REQUIRE(execution->partitioned->axis == 1);
    REQUIRE(execution->partitioned->partitionEvaluations == 2);
    REQUIRE(execution->partitioned->transfers.empty());
    REQUIRE(execution->partitioned->shards.size() == 2);
    REQUIRE(execution->partitioned->shards[0].slice.extents[1] == 500);
    REQUIRE(execution->partitioned->shards[1].slice.extents[1] == 500);
}

TEST_CASE("Transfer-aware partition search does not require a compute-only partition crossover",
          "[Sora][Compute][Planner][Partition][Transfer][DeviceSet][Seed]") {
    DeviceSetSearchFixture fixture;
    std::vector<PlacementCandidate> computeCandidates;
    computeCandidates.reserve(fixture.candidates.placements.size());
    for (DispatchPlacementCandidate& candidate : fixture.candidates.placements) {
        candidate.placement.executable = {.mode = ArtifactResolutionMode::Live};
        candidate.placement.launchNanoseconds = candidate.placement.device.id == kGpuB ? 0 : 100'000;
        PlacementCandidate compute = candidate.placement;
        compute.transfer = CandidateTransferCost{};
        computeCandidates.push_back(std::move(compute));
    }
    const KernelInvocation& invocation = std::get<KernelInvocation>(fixture.graph.Operation({0}));
    const AutomaticPartitionPolicy policy{
        .maximumDevices = 2, .maximumPartitionEvaluations = 256, .minimumImprovementPermille = 0};
    const auto computeOnly = PlanAutomaticDispatch(invocation.Domain(), invocation.Partition(),
                                                   fixture.candidates.workload, computeCandidates, policy);
    REQUIRE(computeOnly.has_value());
    REQUIRE_FALSE(computeOnly->UsesPartitioning());

    const auto transferAware = fixture.Plan(policy);
    REQUIRE(transferAware.has_value());
    REQUIRE(transferAware->UsesPartitioning());
    REQUIRE(transferAware->partitioned->shards.size() == 2);
    REQUIRE(std::ranges::any_of(transferAware->partitioned->shards,
                                [](const auto& shard) { return shard.execution.device == kGpu; }));
    REQUIRE(std::ranges::any_of(transferAware->partitioned->shards,
                                [](const auto& shard) { return shard.execution.device == kGpuC; }));
    REQUIRE(std::ranges::none_of(transferAware->partitioned->shards,
                                 [](const auto& shard) { return shard.execution.device == kGpuB; }));
}

TEST_CASE("Transfer-aware partition search removes a transfer-dominated device from a three-device compute seed",
          "[Sora][Compute][Planner][Partition][Transfer][DeviceSet][Removal]") {
    DeviceSetSearchFixture fixture;
    const AutomaticPartitionPolicy policy{
        .maximumDevices = 3, .maximumPartitionEvaluations = 128, .minimumImprovementPermille = 0};
    const auto execution = fixture.Plan(policy);
    REQUIRE(execution.has_value());
    REQUIRE(execution->UsesPartitioning());
    REQUIRE(execution->partitioned->shards.size() == 2);
    REQUIRE(std::ranges::none_of(execution->partitioned->shards,
                                 [](const auto& shard) { return shard.execution.device == kGpuB; }));
    REQUIRE(execution->partitioned->transfers.empty());
}

TEST_CASE("Transfer-aware partition search can refine and add a device whose compute seed is not immediately better",
          "[Sora][Compute][Planner][Partition][Transfer][DeviceSet][Addition]") {
    DeviceSetSearchFixture fixture{160'000, 0};
    const auto placementC = std::ranges::find_if(
        fixture.candidates.placements, [](const auto& candidate) { return candidate.placement.device.id == kGpuC; });
    REQUIRE(placementC != fixture.candidates.placements.end());
    placementC->placement.launchNanoseconds = 45'000;
    const AutomaticPartitionPolicy policy{
        .maximumDevices = 3, .maximumPartitionEvaluations = 512, .minimumImprovementPermille = 0};
    const auto execution = fixture.Plan(policy);
    REQUIRE(execution.has_value());
    REQUIRE(execution->UsesPartitioning());
    REQUIRE(execution->partitioned->partitionEvaluations <= policy.maximumPartitionEvaluations);
    REQUIRE(execution->partitioned->shards.size() == 3);
    REQUIRE(std::ranges::any_of(execution->partitioned->shards,
                                [](const auto& shard) { return shard.execution.device == kGpuC; }));
}

TEST_CASE("Automatic dispatch expansion creates a validated transfer-free two-device physical cover",
          "[Sora][Compute][Planner][Partition][Expansion]") {
    ExpansionFixture fixture;
    auto lowered = LowerAutomaticDispatchGraph(fixture.graph, fixture.Services(), {}, fixture.baseline,
                                               fixture.candidates, fixture.execution);
    REQUIRE(lowered.has_value());
    REQUIRE(lowered->UsesPartitioning());
    REQUIRE(lowered->storage.segments.size() == 2);
    REQUIRE(lowered->storage.allocations.size() == 2);
    REQUIRE(lowered->storage.edges.empty());

    const auto& first = std::get<PlannedDispatch>(lowered->storage.segments[0]);
    const auto& second = std::get<PlannedDispatch>(lowered->storage.segments[1]);
    REQUIRE(first.logicalNode == GraphNodeId{0});
    REQUIRE(second.logicalNode == GraphNodeId{0});
    REQUIRE(first.queue.device != second.queue.device);
    REQUIRE(first.slice.origin[0] == 0);
    REQUIRE(second.slice.origin[0] == kElementCount / 2);

    const PlannedAllocation& firstOutput = lowered->storage.allocations[0];
    const PlannedAllocation& secondOutput = lowered->storage.allocations[1];
    REQUIRE(firstOutput.bufferRegion.Offset() == 0);
    REQUIRE(firstOutput.bufferRegion.End() == secondOutput.bufferRegion.Offset());
    REQUIRE(secondOutput.bufferRegion.End() == kBufferBytes);
    REQUIRE(firstOutput.publication->kind == PlannedPublicationKind::PartitionedWrite);
    REQUIRE(secondOutput.publication->kind == PlannedPublicationKind::PartitionedWrite);
    REQUIRE(firstOutput.publication->partitionIndex == 0);
    REQUIRE(secondOutput.publication->partitionIndex == 1);
    REQUIRE(firstOutput.publication->partitionCount == 2);

    const auto plan = PhysicalGraphPlan::Create(std::move(lowered->storage), fixture.planning.resources);
    REQUIRE(plan.has_value());
}

TEST_CASE("Automatic dispatch expansion retains the single-device lowering below crossover",
          "[Sora][Compute][Planner][Partition][Expansion][Fallback]") {
    ExpansionFixture fixture;
    auto single = PlanAutomaticDispatch(std::get<KernelInvocation>(fixture.graph.Operation({0})), fixture.candidates,
                                        fixture.Services(), fixture.baseline,
                                        {.maximumDevices = 2, .partitionOverheadNanoseconds = 1'000'000});
    REQUIRE(single.has_value());
    REQUIRE_FALSE(single->UsesPartitioning());

    auto lowered = LowerAutomaticDispatchGraph(fixture.graph, fixture.Services(), {}, fixture.baseline,
                                               fixture.candidates, *single);
    REQUIRE(lowered.has_value());
    REQUIRE_FALSE(lowered->UsesPartitioning());
    REQUIRE(lowered->fallback == AutomaticDispatchFallback::SingleDeviceSelected);
    REQUIRE(lowered->storage.segments.size() == 1);
    REQUIRE(lowered->storage.allocations.size() == 1);
    REQUIRE(lowered->storage.allocations[0].publication->kind == PlannedPublicationKind::WholeWrite);

    const auto plan = PhysicalGraphPlan::Create(std::move(lowered->storage), fixture.planning.resources);
    REQUIRE(plan.has_value());
}

TEST_CASE("Automatic dispatch expansion rejects a forged non-strict partition cover",
          "[Sora][Compute][Planner][Partition][Expansion][Validation]") {
    ExpansionFixture fixture;
    REQUIRE(fixture.execution.partitioned.has_value());
    --fixture.execution.partitioned->shards[1].slice.origin[0];

    const auto lowered = LowerAutomaticDispatchGraph(fixture.graph, fixture.Services(), {}, fixture.baseline,
                                                     fixture.candidates, fixture.execution);
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().code == AutomaticDispatchExpansionErrorCode::InvalidRequest);
    REQUIRE(lowered.error().node == GraphNodeId{0});
}

TEST_CASE("Automatic dispatch expansion rejects a mismatched destination identity",
          "[Sora][Compute][Planner][Partition][Expansion][Identity]") {
    ExpansionFixture fixture;
    REQUIRE(fixture.execution.partitioned.has_value());
    const uint32_t candidateIndex = fixture.execution.partitioned->shards[0].execution.candidateIndex;
    REQUIRE(candidateIndex < fixture.candidates.placements.size());
    const uint32_t residencyIndex = fixture.candidates.placements[candidateIndex].residencyProjection;
    REQUIRE(residencyIndex < fixture.candidates.residencies.size());
    REQUIRE(fixture.candidates.residencies[residencyIndex].destination.has_value());
    fixture.candidates.residencies[residencyIndex].destination->location.device = kHostDevice;

    const auto lowered = LowerAutomaticDispatchGraph(fixture.graph, fixture.Services(), {}, fixture.baseline,
                                                     fixture.candidates, fixture.execution);
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().code == AutomaticDispatchExpansionErrorCode::CandidateMismatch);
    REQUIRE(lowered.error().node == GraphNodeId{0});
}

TEST_CASE("Automatic dispatch expansion rejects forged transfer timing provenance",
          "[Sora][Compute][Planner][Partition][Transfer][Validation]") {
    ExpansionFixture fixture;
    REQUIRE(fixture.execution.partitioned.has_value());
    ++fixture.execution.partitioned->shards.front().inputReadyNanoseconds;

    const auto lowered = LowerAutomaticDispatchGraph(fixture.graph, fixture.Services(), {}, fixture.baseline,
                                                     fixture.candidates, fixture.execution);
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().code == AutomaticDispatchExpansionErrorCode::CandidateMismatch);
    REQUIRE(lowered.error().node == GraphNodeId{0});
}

TEST_CASE("Automatic dispatch expansion rejects forged shard binding provenance",
          "[Sora][Compute][Planner][Partition][Binding][Validation]") {
    ExpansionFixture fixture;
    REQUIRE(fixture.execution.partitioned.has_value());
    REQUIRE_FALSE(fixture.execution.partitioned->shards.front().residency.bindings.empty());
    auto whole = ByteRegion::Make(0, kBufferBytes);
    REQUIRE(whole.has_value());
    fixture.execution.partitioned->shards.front().residency.bindings.front().region = *whole;

    const auto lowered = LowerAutomaticDispatchGraph(fixture.graph, fixture.Services(), {}, fixture.baseline,
                                                     fixture.candidates, fixture.execution);
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().code == AutomaticDispatchExpansionErrorCode::CandidateMismatch);
    REQUIRE(lowered.error().node == GraphNodeId{0});
}

TEST_CASE("Automatic dispatch expansion rejects forged shard route provenance",
          "[Sora][Compute][Planner][Partition][Route][Validation]") {
    ExpansionFixture fixture;
    REQUIRE(fixture.execution.partitioned.has_value());
    REQUIRE_FALSE(fixture.execution.partitioned->shards.front().residency.inputs.empty());
    auto whole = ByteRegion::Make(0, kBufferBytes);
    REQUIRE(whole.has_value());
    fixture.execution.partitioned->shards.front().residency.inputs.front().region = *whole;

    const auto lowered = LowerAutomaticDispatchGraph(fixture.graph, fixture.Services(), {}, fixture.baseline,
                                                     fixture.candidates, fixture.execution);
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().code == AutomaticDispatchExpansionErrorCode::CandidateMismatch);
    REQUIRE(lowered.error().node == GraphNodeId{0});
}

TEST_CASE("Automatic dispatch expansion forms a validated exact partitioned affine cover",
          "[Sora][Compute][Planner][Partition][Expansion][Footprint]") {
    PlanningFixture planning{{.includeSecondDevice = true}};
    REQUIRE(planning.packages.Register(kStridedCopyProgram).has_value());
    const auto calibrate = [&](QueueId queue, TargetFingerprint target) {
        REQUIRE(planning.performance
                    .Update({.key = {.kernel = kStridedCopyProgram.Hash(),
                                     .target = target,
                                     .specialization = {},
                                     .queue = queue},
                             .workItemCount = kStridedElementCount,
                             .computeNanoseconds = 256,
                             .launchNanoseconds = 100})
                    .has_value());
        CalibrateResolution(planning.performance, kStridedCopyProgram.Hash(), target);
    };
    calibrate(kQueue, Target());
    calibrate(kQueueB, Target(kGpuB));

    std::atomic<uint32_t> destroyCount{0};
    BackendService secondaryService = MakeSecondaryService();
    Buffer input = MakeStridedBuffer({0xB, 1});
    Buffer output = MakeStridedBuffer({0xB, 2});
    REQUIRE(
        input.CommitWholeWrite(MakeStridedAllocation(Sora::Test::TestBackendService(), kDevice, {0xC, 1}, destroyCount))
            .has_value());
    REQUIRE(input
                .PublishReplica(1, *kStridedFootprint,
                                MakeStridedAllocation(secondaryService, kDeviceB, {0xC, 2}, destroyCount))
                .has_value());

    std::vector<KernelArgumentBinding> bindings;
    bindings.emplace_back(StridedView(input));
    bindings.emplace_back(StridedView(output));
    constexpr std::array extents{kStridedElementCount};
    auto invocation = BindKernelProgram(kStridedCopyProgram, extents, std::move(bindings));
    REQUIRE(invocation.has_value());
    ExecutionGraph graph = MakeGraph(std::move(*invocation));
    auto baseline = FreezeGraphResidency(graph);
    REQUIRE(baseline.has_value());

    auto artifacts = planning.artifacts.SnapshotAvailability();
    REQUIRE(artifacts.has_value());
    const auto performance = planning.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(planning.packages);
    const std::array devices{
        planning.device,
        RuntimeDeviceBinding{
            .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))},
    };
    const std::array snapshots{
        planning.snapshot,
        DeviceSnapshot{
            .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U},
    };
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = planning.resources.Inventory(),
                                         .artifacts = *artifacts,
                                         .performance = *performance};
    DispatchEnumerationPolicy enumerationPolicy;
    enumerationPolicy.memory.publication = DestinationPublication::Publish;
    auto candidates =
        EnumerateDispatchCandidates(graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    services, *baseline, enumerationPolicy);
    REQUIRE(candidates.has_value());
    auto execution = PlanAutomaticDispatch(std::get<KernelInvocation>(graph.Operation({0})), *candidates, services,
                                           *baseline, {.maximumDevices = 2, .minimumImprovementPermille = 0});
    REQUIRE(execution.has_value());
    REQUIRE(execution->UsesPartitioning());

    auto lowered = LowerAutomaticDispatchGraph(graph, services, {}, *baseline, *candidates, std::move(*execution));
    REQUIRE(lowered.has_value());
    REQUIRE(lowered->UsesPartitioning());
    REQUIRE(lowered->fallback == AutomaticDispatchFallback::None);
    REQUIRE(lowered->storage.segments.size() == 2);
    REQUIRE(lowered->storage.allocations.size() == 2);
    const auto plan = PhysicalGraphPlan::Create(std::move(lowered->storage), planning.resources);
    REQUIRE(plan.has_value());
}

TEST_CASE("Automatic dispatch replans exact shard transfer routes and lowers their physical hops",
          "[Sora][Compute][Planner][Partition][Transfer]") {
    PlanningFixture planning{{.includeSecondDevice = true}, false};
    const auto calibrate = [&](QueueId queue, TargetFingerprint target) {
        REQUIRE(
            planning.performance
                .Update({.key = {.kernel = kCopyProgram.Hash(), .target = target, .specialization = {}, .queue = queue},
                         .workItemCount = kElementCount,
                         .computeNanoseconds = 100'000,
                         .launchNanoseconds = 100})
                .has_value());
        CalibrateResolution(planning.performance, kCopyProgram.Hash(), target);
    };
    calibrate(kQueue, Target());
    calibrate(kQueueB, Target(kGpuB));
    CalibrateTransfer(planning.performance, kPeerTransfer, kDeviceB, kDevice, kServiceB);

    std::atomic<uint32_t> destroyCount{0};
    Buffer input = MakeBuffer({0xE, 1});
    Buffer output = MakeBuffer({0xE, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    ExecutionGraph graph = MakeGraph(Invocation(input, output));
    auto baseline = FreezeGraphResidency(graph);
    REQUIRE(baseline.has_value());

    auto artifacts = planning.artifacts.SnapshotAvailability();
    REQUIRE(artifacts.has_value());
    const auto performance = planning.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(planning.packages);
    const std::array devices{
        planning.device,
        RuntimeDeviceBinding{
            .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))},
    };
    const std::array snapshots{
        planning.snapshot,
        DeviceSnapshot{
            .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U},
    };
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = planning.resources.Inventory(),
                                         .artifacts = *artifacts,
                                         .performance = *performance};
    DispatchEnumerationPolicy enumerationPolicy;
    enumerationPolicy.memory.publication = DestinationPublication::Publish;
    auto candidates =
        EnumerateDispatchCandidates(graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    services, *baseline, enumerationPolicy);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->placements.size() == 2);
    auto execution = PlanAutomaticDispatch(std::get<KernelInvocation>(graph.Operation({0})), *candidates, services,
                                           *baseline, {.maximumDevices = 2, .minimumImprovementPermille = 0});
    REQUIRE(execution.has_value());
    REQUIRE(execution->UsesPartitioning());
    REQUIRE(execution->partitioned->transfers.size() == 1);

    const auto transferred = std::ranges::find_if(execution->partitioned->shards,
                                                  [](const auto& shard) { return shard.execution.device == kGpuB; });
    REQUIRE(transferred != execution->partitioned->shards.end());
    REQUIRE(transferred->slice.extents[0] < kElementCount / 2);
    REQUIRE(transferred->residency.inputs.size() == 1);
    REQUIRE_FALSE(transferred->residency.inputs.front().AlreadyResident());
    REQUIRE(transferred->residency.inputs.front().region.Size() == transferred->slice.extents[0] * sizeof(float));
    REQUIRE(transferred->residency.inputs.front().estimatedNanoseconds == 608);
    REQUIRE(transferred->inputReadyNanoseconds == 608);

    auto lowered = LowerAutomaticDispatchGraph(graph, services, {}, *baseline, *candidates, *execution);
    REQUIRE(lowered.has_value());
    REQUIRE(lowered->UsesPartitioning());
    REQUIRE(std::ranges::count_if(lowered->storage.segments, [](const PhysicalPlanSegment& segment) {
                return std::holds_alternative<PlannedTransfer>(segment);
            }) == 1);
    const auto plan = PhysicalGraphPlan::Create(std::move(lowered->storage), planning.resources);
    REQUIRE(plan.has_value());
}
