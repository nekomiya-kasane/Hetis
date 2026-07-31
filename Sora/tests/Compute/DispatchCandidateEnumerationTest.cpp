#include "PlanningTestSupport.h"

#include <Sora/Compute/Runtime/CompletionTransaction.h>
#include <Sora/Compute/Runtime/DispatchGraphPlanning.h>

namespace {

    using namespace Sora::Compute;
    using namespace Sora::Test::ComputePlanning;

    constexpr std::array kAffineExtents{uint64_t{256}};
    constexpr std::array kAffineStrides{uint64_t{8}};
    constexpr std::array kAffineHalfExtents{uint64_t{128}};
    constexpr auto kAffineDomain =
        AffineByteFootprint::Create(kAffineExtents, kAffineStrides, sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineFirst =
        AffineByteFootprint::Create(kAffineHalfExtents, kAffineStrides, sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineSecond =
        AffineByteFootprint::Create(kAffineHalfExtents, kAffineStrides, sizeof(float), 1024, sizeof(float), 1);
    constexpr std::array kAffinePrefixExtents{uint64_t{96}};
    constexpr std::array kAffineOverlapExtents{uint64_t{160}};
    constexpr auto kAffinePrefix =
        AffineByteFootprint::Create(kAffinePrefixExtents, kAffineStrides, sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineOverlap =
        AffineByteFootprint::Create(kAffineOverlapExtents, kAffineStrides, sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineSuffix =
        AffineByteFootprint::Create(kAffineOverlapExtents, kAffineStrides, sizeof(float), 768, sizeof(float), 1);
    constexpr auto kAffineMiddle =
        AffineByteFootprint::Create(kAffineHalfExtents, kAffineStrides, sizeof(float), 512, sizeof(float), 1);
    static_assert(kAffineDomain && kAffineFirst && kAffineSecond && kAffinePrefix && kAffineOverlap && kAffineSuffix &&
                  kAffineMiddle);

    consteval auto MakeAffineCopyProgram() {
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

    constexpr auto kAffineCopyProgram = MakeAffineCopyProgram();

    void CalibrateAffineResolution(PerformanceModel& model, TargetFingerprint target = Target()) {
        const ArtifactKey artifact = MakeArtifactKey(
            ExecutableKey{.lowered = kAffineCopyProgram.Hash(), .target = target, .specialization = {}});
        for (const ArtifactResolutionPath path :
             {ArtifactResolutionPath::PackageImport, ArtifactResolutionPath::PersistentImport,
              ArtifactResolutionPath::PersistentProduction, ArtifactResolutionPath::TransientProduction}) {
            REQUIRE(model.Update({.key = artifact, .path = path, .nanoseconds = 10'000}).has_value());
        }
    }

    [[nodiscard]] Buffer MakeAffineBuffer(BufferId id) {
        auto buffer = Buffer::Create({.id = id,
                                      .elementType = kFloat,
                                      .layout = LayoutKind::Strided,
                                      .rank = 1,
                                      .sizeBytes = kAffineDomain->BoundingRegion().End(),
                                      .alignment = 64},
                                     *kAffineDomain);
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    [[nodiscard]] Allocation MakeAffineAllocation(uint64_t sizeBytes, PhysicalMemoryId memory,
                                                  std::atomic<uint32_t>& destroyCount,
                                                  MemoryLocation location = kDevice) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Allocation::Adopt(
            Sora::Test::TestBackendService(),
            {.location = location,
             .memory = memory,
             .properties = location == kHost ? MemoryProperty::HostVisible : MemoryProperty::DeviceLocal,
             .sizeBytes = sizeBytes,
             .alignment = 64,
             .state = native.release(),
             .destroy = DestroyAllocation});
        REQUIRE(allocation.has_value());
        return std::move(*allocation);
    }

    [[nodiscard]] BufferView AffineView(Buffer buffer) {
        auto view = BufferView::Create(std::move(buffer), {.layout = LayoutKind::Strided,
                                                           .rank = 1,
                                                           .extents = {256},
                                                           .byteStrides = {8},
                                                           .componentStrideBytes = sizeof(float)});
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    [[nodiscard]] KernelInvocation AffineInvocation(Buffer input, Buffer output) {
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(AffineView(std::move(input)));
        bindings.emplace_back(AffineView(std::move(output)));
        auto invocation = BindKernelProgram(kAffineCopyProgram, kAffineExtents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    [[nodiscard]] KernelInvocation AffineFirstHalfInvocation(Buffer input, Buffer output) {
        constexpr std::array origin{uint64_t{0}};
        auto inputView = AffineView(std::move(input)).SubView(origin, kAffineHalfExtents);
        auto outputView = AffineView(std::move(output)).SubView(origin, kAffineHalfExtents);
        REQUIRE(inputView.has_value());
        REQUIRE(outputView.has_value());
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(std::move(*inputView));
        bindings.emplace_back(std::move(*outputView));
        auto invocation = BindKernelProgram(kAffineCopyProgram, kAffineHalfExtents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    [[nodiscard]] KernelInvocation AffineMiddleHalfInvocation(Buffer input, Buffer output) {
        constexpr std::array origin{uint64_t{64}};
        auto inputView = AffineView(std::move(input)).SubView(origin, kAffineHalfExtents);
        auto outputView = AffineView(std::move(output)).SubView(origin, kAffineHalfExtents);
        REQUIRE(inputView.has_value());
        REQUIRE(outputView.has_value());
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(std::move(*inputView));
        bindings.emplace_back(std::move(*outputView));
        auto invocation = BindKernelProgram(kAffineCopyProgram, kAffineHalfExtents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    void CalibrateAffineKernel(PerformanceModel& model, QueueId queue, TargetFingerprint target) {
        REQUIRE(model
                    .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                     .target = target,
                                     .specialization = {},
                                     .queue = queue},
                             .workItemCount = 256,
                             .computeNanoseconds = 256,
                             .launchNanoseconds = 100})
                    .has_value());
        CalibrateAffineResolution(model, target);
    }

    [[nodiscard]] bool HasPhysicalEdge(const PhysicalGraphPlanStorage& storage, PhysicalPlanSegmentId predecessor,
                                       PhysicalPlanSegmentId successor) noexcept {
        return std::ranges::find(storage.edges, PhysicalPlanEdge{predecessor, successor}) != storage.edges.end();
    }

    [[nodiscard]] std::vector<PhysicalPlanSegmentId> DispatchCompletions(const PhysicalGraphPlanStorage& storage,
                                                                         GraphNodeId node) {
        std::vector<PhysicalPlanSegmentId> result;
        for (uint32_t index = 0; index < storage.segments.size(); ++index) {
            const auto* dispatch = std::get_if<PlannedDispatch>(&storage.segments[index]);
            if (dispatch != nullptr && dispatch->logicalNode == node) {
                result.push_back({index});
            }
        }
        return result;
    }

    [[nodiscard]] DispatchGraphSelection WholeSelection(const ExecutionGraph& graph, GraphNodeId node,
                                                        const DispatchCandidateSet& candidates, DeviceId device,
                                                        GraphResidencyState& residency) {
        const auto placement = std::ranges::find_if(
            candidates.placements, [&](const auto& candidate) { return candidate.placement.device.id == device; });
        REQUIRE(placement != candidates.placements.end());
        REQUIRE(placement->residencyProjection < candidates.residencies.size());
        const DispatchResidencyProjection& projection = candidates.residencies[placement->residencyProjection];
        REQUIRE(CommitDispatchResidencySelection(node, projection, residency).has_value());
        const auto& invocation = std::get<KernelInvocation>(graph.Operation(node));
        return {.node = node,
                .workload = candidates.workload,
                .execution = DispatchGraphShardSelection{.slice = DispatchSlice::Whole(invocation.Domain()),
                                                         .placement = placement->placement,
                                                         .residency = projection,
                                                         .geometry = candidates.geometry}};
    }

    [[nodiscard]] DispatchGraphSelection PartitionSelection(const ExecutionGraph& graph, GraphNodeId node,
                                                            const DispatchCandidateSet& candidates,
                                                            GraphPlanningServices services,
                                                            const std::array<DeviceId, 2>& devices,
                                                            GraphResidencyState& residency) {
        const auto& invocation = std::get<KernelInvocation>(graph.Operation(node));
        const auto partition =
            ElementwisePartitionPlan::Balanced(invocation.Domain(), invocation.Partition(), 0, devices.size());
        REQUIRE(partition.has_value());
        std::vector<DispatchGraphShardSelection> shards;
        std::vector<DispatchResidencyProjection> projections;
        shards.reserve(devices.size());
        projections.reserve(devices.size());
        std::vector<DispatchMemoryBinding> logicalBindings;
        for (uint32_t index = 0; index < devices.size(); ++index) {
            const auto placement = std::ranges::find_if(candidates.placements, [&](const auto& candidate) {
                return candidate.placement.device.id == devices[index];
            });
            REQUIRE(placement != candidates.placements.end());
            REQUIRE(placement->residencyProjection < candidates.residencies.size());
            const DispatchResidencyProjection& whole = candidates.residencies[placement->residencyProjection];
            if (logicalBindings.empty()) {
                logicalBindings = whole.bindings;
            }
            auto projected =
                ProjectDispatchSliceResidency(invocation, node, partition->Shards()[index], whole, services, residency);
            REQUIRE(projected.has_value());
            const auto geometry = PlanDispatchSliceGeometry(partition->Shards()[index]);
            REQUIRE(geometry.has_value());
            projections.push_back(*projected);
            shards.push_back({.slice = partition->Shards()[index],
                              .placement = placement->placement,
                              .residency = std::move(*projected),
                              .geometry = *geometry});
        }
        REQUIRE(CommitPartitionedDispatchResidencySelection(node, logicalBindings, projections, residency).has_value());
        return {.node = node,
                .workload = candidates.workload,
                .execution = DispatchGraphPartitionSelection{
                    .axis = 0, .logicalBindings = std::move(logicalBindings), .shards = std::move(shards)}};
    }

    struct MixedDagEnvironment {
        PlanningFixture planning{{.includeSecondDevice = true, .secondaryQueueSupportsCopy = true}, false};
        std::optional<ArtifactAvailabilitySnapshot> artifacts{};
        std::shared_ptr<const PerformanceSnapshot> performance{};
        std::shared_ptr<const ProgramCatalogSnapshot> packages{};
        std::array<RuntimeDeviceBinding, 2> devices{planning.device,
                                                    RuntimeDeviceBinding{.service = kServiceB,
                                                                         .descriptor = Descriptor(kGpuB),
                                                                         .target = RuntimeTarget(Target(kGpuB))}};
        std::array<DeviceSnapshot, 2> snapshots{planning.snapshot,
                                                DeviceSnapshot{.id = kGpuB,
                                                               .state = DeviceState::Available,
                                                               .generation = 1,
                                                               .availableMemoryBytes = uint64_t{8} << 30U}};

        MixedDagEnvironment() {
            CalibrateKernel(planning.performance);
            CalibrateKernel(planning.performance, kQueueB, Target(kGpuB));
            CalibrateTransfer(planning.performance, kPeerTransfer, kDeviceB, kDevice, kServiceB);
            auto availability = planning.artifacts.SnapshotAvailability();
            REQUIRE(availability.has_value());
            artifacts.emplace(std::move(*availability));
            performance = planning.performance.Snapshot();
            packages = FreezeProgramCatalog(planning.packages);
        }

        [[nodiscard]] GraphPlanningContext Context() const noexcept {
            return {.epochSequence = 1, .devices = devices, .snapshots = snapshots};
        }

        [[nodiscard]] GraphPlanningServices Services() const noexcept {
            return {.packages = *packages,
                    .resources = planning.resources.Inventory(),
                    .artifacts = *artifacts,
                    .performance = *performance};
        }
    };

} // namespace

TEST_CASE("Shared dispatch lowering closes partitioned producer dependencies into a whole consumer",
          "[Sora][Compute][Runtime][DispatchGraphPlanning][Partition][DAG][Producer]") {
    MixedDagEnvironment environment;
    std::atomic<uint32_t> destroyCount = 0;
    Buffer seed = MakeBuffer({0xD0, 1});
    Buffer intermediate = MakeBuffer({0xD0, 2});
    Buffer output = MakeBuffer({0xD0, 3});
    REQUIRE(seed.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());

    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(seed, intermediate));
    const auto consumer = builder.AddDispatch(Invocation(intermediate, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());
    const auto baseline = FreezeGraphResidency(*graph);
    REQUIRE(baseline.has_value());
    GraphResidencyState residency = *baseline;

    const auto producerCandidates =
        EnumerateDispatchCandidates(*graph, *producer, environment.Context(), environment.Services(), residency);
    REQUIRE(producerCandidates.has_value());
    std::vector<DispatchGraphSelection> selections;
    selections.push_back(
        PartitionSelection(*graph, *producer, *producerCandidates, environment.Services(), {kGpu, kGpuB}, residency));
    const auto consumerCandidates =
        EnumerateDispatchCandidates(*graph, *consumer, environment.Context(), environment.Services(), residency);
    REQUIRE(consumerCandidates.has_value());
    selections.push_back(WholeSelection(*graph, *consumer, *consumerCandidates, kGpuB, residency));

    auto lowered = LowerDispatchGraph(*graph, environment.Services(), {}, *baseline, selections);
    if (!lowered) {
        UNSCOPED_INFO("code=" << static_cast<uint32_t>(lowered.error().code) << " node=" << lowered.error().node.index
                              << " argument=" << lowered.error().argument.index << " cause=" << lowered.error().cause);
    }
    REQUIRE(lowered.has_value());
    const auto producerCompletions = DispatchCompletions(*lowered, *producer);
    const auto consumerCompletions = DispatchCompletions(*lowered, *consumer);
    REQUIRE(producerCompletions.size() == 2);
    REQUIRE(consumerCompletions.size() == 1);
    for (const PhysicalPlanSegmentId completion : producerCompletions) {
        REQUIRE(HasPhysicalEdge(*lowered, completion, consumerCompletions.front()));
    }
    REQUIRE(PhysicalGraphPlan::Create(std::move(*lowered), environment.planning.resources).has_value());
}

TEST_CASE("Shared dispatch lowering fans one whole producer into every partitioned consumer shard",
          "[Sora][Compute][Runtime][DispatchGraphPlanning][Partition][DAG][Consumer]") {
    MixedDagEnvironment environment;
    std::atomic<uint32_t> destroyCount = 0;
    Buffer seed = MakeBuffer({0xD1, 1});
    Buffer intermediate = MakeBuffer({0xD1, 2});
    Buffer output = MakeBuffer({0xD1, 3});
    REQUIRE(seed.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());

    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(seed, intermediate));
    const auto consumer = builder.AddDispatch(Invocation(intermediate, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());
    const auto baseline = FreezeGraphResidency(*graph);
    REQUIRE(baseline.has_value());
    GraphResidencyState residency = *baseline;

    const auto producerCandidates =
        EnumerateDispatchCandidates(*graph, *producer, environment.Context(), environment.Services(), residency);
    REQUIRE(producerCandidates.has_value());
    std::vector<DispatchGraphSelection> selections;
    selections.push_back(WholeSelection(*graph, *producer, *producerCandidates, kGpu, residency));
    const auto consumerCandidates =
        EnumerateDispatchCandidates(*graph, *consumer, environment.Context(), environment.Services(), residency);
    REQUIRE(consumerCandidates.has_value());
    selections.push_back(
        PartitionSelection(*graph, *consumer, *consumerCandidates, environment.Services(), {kGpu, kGpuB}, residency));

    auto lowered = LowerDispatchGraph(*graph, environment.Services(), {}, *baseline, selections);
    REQUIRE(lowered.has_value());
    const auto producerCompletions = DispatchCompletions(*lowered, *producer);
    const auto consumerCompletions = DispatchCompletions(*lowered, *consumer);
    REQUIRE(producerCompletions.size() == 1);
    REQUIRE(consumerCompletions.size() == 2);
    for (const PhysicalPlanSegmentId completion : consumerCompletions) {
        REQUIRE(HasPhysicalEdge(*lowered, producerCompletions.front(), completion));
    }
    REQUIRE(PhysicalGraphPlan::Create(std::move(*lowered), environment.planning.resources).has_value());
}

TEST_CASE("Shared dispatch lowering preserves partition completion closure through fork and join",
          "[Sora][Compute][Runtime][DispatchGraphPlanning][Partition][DAG][ForkJoin]") {
    MixedDagEnvironment environment;
    std::atomic<uint32_t> destroyCount = 0;
    std::array<Buffer, 4> inputs{MakeBuffer({0xD2, 1}), MakeBuffer({0xD2, 2}), MakeBuffer({0xD2, 3}),
                                 MakeBuffer({0xD2, 4})};
    std::array<Buffer, 4> outputs{MakeBuffer({0xD2, 5}), MakeBuffer({0xD2, 6}), MakeBuffer({0xD2, 7}),
                                  MakeBuffer({0xD2, 8})};
    for (Buffer& input : inputs) {
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    }

    GraphBuilder builder;
    std::array<GraphNodeId, 4> nodes{};
    for (uint32_t index = 0; index < nodes.size(); ++index) {
        const auto node = builder.AddDispatch(Invocation(inputs[index], outputs[index]));
        REQUIRE(node.has_value());
        nodes[index] = *node;
    }
    REQUIRE(builder.AddDependency(nodes[0], nodes[1]).has_value());
    REQUIRE(builder.AddDependency(nodes[0], nodes[2]).has_value());
    REQUIRE(builder.AddDependency(nodes[1], nodes[3]).has_value());
    REQUIRE(builder.AddDependency(nodes[2], nodes[3]).has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());
    const auto baseline = FreezeGraphResidency(*graph);
    REQUIRE(baseline.has_value());
    GraphResidencyState residency = *baseline;

    std::vector<DispatchGraphSelection> selections;
    for (uint32_t index = 0; index < nodes.size(); ++index) {
        const auto candidates =
            EnumerateDispatchCandidates(*graph, nodes[index], environment.Context(), environment.Services(), residency);
        REQUIRE(candidates.has_value());
        if (index == 0) {
            selections.push_back(PartitionSelection(*graph, nodes[index], *candidates, environment.Services(),
                                                    {kGpu, kGpuB}, residency));
        } else {
            selections.push_back(WholeSelection(*graph, nodes[index], *candidates, kGpu, residency));
        }
    }

    auto lowered = LowerDispatchGraph(*graph, environment.Services(), {}, *baseline, selections);
    REQUIRE(lowered.has_value());
    const auto root = DispatchCompletions(*lowered, nodes[0]);
    const auto left = DispatchCompletions(*lowered, nodes[1]);
    const auto right = DispatchCompletions(*lowered, nodes[2]);
    const auto join = DispatchCompletions(*lowered, nodes[3]);
    REQUIRE(root.size() == 2);
    REQUIRE(left.size() == 1);
    REQUIRE(right.size() == 1);
    REQUIRE(join.size() == 1);
    for (const PhysicalPlanSegmentId completion : root) {
        REQUIRE(HasPhysicalEdge(*lowered, completion, left.front()));
        REQUIRE(HasPhysicalEdge(*lowered, completion, right.front()));
    }
    REQUIRE(HasPhysicalEdge(*lowered, left.front(), join.front()));
    REQUIRE(HasPhysicalEdge(*lowered, right.front(), join.front()));
    REQUIRE(PhysicalGraphPlan::Create(std::move(*lowered), environment.planning.resources).has_value());
}

TEST_CASE("Dispatch candidate enumeration joins exact routes, memory, "
          "artifacts, and calibration",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration]") {
    std::atomic<uint32_t> destroyCount = 0;
    {
        PlanningFixture fixture;
        CalibrateTransfer(fixture.performance);
        Buffer input = MakeBuffer({70, 1});
        Buffer output = MakeBuffer({70, 2});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));

        const auto candidates = Enumerate(graph, fixture);
        REQUIRE(candidates.has_value());
        REQUIRE(candidates->workload.requiredDeviceMemoryBytes == 2 * kBufferBytes);
        REQUIRE(candidates->workload.largestAllocationBytes == kBufferBytes);
        REQUIRE(candidates->residencies.size() == 1);
        REQUIRE(candidates->residencies[0].destination->location == kDevice);
        REQUIRE(candidates->residencies[0].bindings.size() == 2);
        REQUIRE(candidates->residencies[0].inputs.size() == 1);
        REQUIRE(candidates->residencies[0].inputs[0].buffer == input.Describe().id);
        REQUIRE(candidates->residencies[0].inputs[0].version == 1);
        REQUIRE(candidates->residencies[0].transfer == CandidateTransferCost{kBufferBytes, 1124});
        REQUIRE(candidates->placements.size() == 1);
        REQUIRE(candidates->placements[0].residencyProjection == 0);
        REQUIRE(candidates->placements[0].placement.transfer == CandidateTransferCost{kBufferBytes, 1124});
        REQUIRE(candidates->placements[0].placement.executable.mode == ArtifactResolutionMode::TransientProduction);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Dispatch candidate enumeration preserves zero-cost resident routes "
          "and excludes uncalibrated transfers",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency]") {
    std::atomic<uint32_t> destroyCount = 0;
    {
        PlanningFixture fixture;
        Buffer input = MakeBuffer({71, 1});
        Buffer output = MakeBuffer({71, 2});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));

        const auto uncalibrated = Enumerate(graph, fixture);
        REQUIRE(uncalibrated.has_value());
        REQUIRE(uncalibrated->residencies.empty());
        REQUIRE(uncalibrated->placements.empty());

        REQUIRE(input.PublishReplica(1, *ByteRegion::Make(0, kBufferBytes), MakeAllocation(kDevice, destroyCount))
                    .has_value());
        const auto resident = Enumerate(graph, fixture);
        REQUIRE(resident.has_value());
        REQUIRE(resident->residencies.size() == 1);
        REQUIRE(resident->residencies[0].inputs[0].AlreadyResident());
        REQUIRE(resident->residencies[0].transfer == CandidateTransferCost{});
        REQUIRE(resident->placements[0].placement.transfer == CandidateTransferCost{});
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Dispatch candidate enumeration consumes graph-produced residency and "
          "validates memory policy",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Validation]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture;
    CalibrateTransfer(fixture.performance);
    Buffer first = MakeBuffer({72, 1});
    Buffer intermediate = MakeBuffer({72, 2});
    Buffer output = MakeBuffer({72, 3});
    REQUIRE(first.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());

    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(first, intermediate));
    const auto consumer = builder.AddDispatch(Invocation(intermediate, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto availability = fixture.artifacts.SnapshotAvailability();
    REQUIRE(availability.has_value());
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const GraphPlanningContext context{.epochSequence = 1, .devices = devices, .snapshots = snapshots};
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    GraphResidencyState residency;
    REQUIRE(residency.Track(first).has_value());
    REQUIRE(residency.Track(intermediate).has_value());
    REQUIRE(residency.Track(output).has_value());

    const auto producerCandidates = EnumerateDispatchCandidates(*graph, *producer, context, services, residency);
    REQUIRE(producerCandidates.has_value());
    REQUIRE(producerCandidates->residencies.size() == 1);
    REQUIRE(
        CommitDispatchResidencySelection(*producer, producerCandidates->residencies.front(), residency).has_value());

    const auto consumerCandidates = EnumerateDispatchCandidates(*graph, *consumer, context, services, residency);
    REQUIRE(consumerCandidates.has_value());
    REQUIRE(consumerCandidates->residencies.size() == 1);
    REQUIRE(consumerCandidates->residencies.front().inputs.size() == 1);
    const auto* source =
        std::get_if<GraphLocalResidencySource>(&consumerCandidates->residencies.front().inputs.front().source);
    REQUIRE(source != nullptr);
    REQUIRE(source->producer == *producer);
    REQUIRE(consumerCandidates->residencies.front().inputs.front().AlreadyResident());

    constexpr std::array duplicatePreferences{kDevice, kDevice};
    const DispatchEnumerationPolicy invalidPolicy{.memory = {.preferredLocations = duplicatePreferences}};
    const auto invalid = EnumerateDispatchCandidates(*graph, *producer, context, services, residency, invalidPolicy);
    REQUIRE(invalid.error().code == DispatchCandidateEnumerationErrorCode::InvalidRequest);
}

TEST_CASE("Dispatch candidate enumeration routes an exact affine value assembled from symbolic shards",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Affine]") {
    PlanningFixture fixture({.primaryQueueSupportsCopy = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    REQUIRE(fixture.performance
                .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                 .target = Target(),
                                 .specialization = {},
                                 .queue = kQueue},
                         .workItemCount = 256,
                         .computeNanoseconds = 256,
                         .launchNanoseconds = 100})
                .has_value());
    CalibrateAffineResolution(fixture.performance);

    Buffer seed = MakeAffineBuffer({72, 10});
    Buffer intermediate = MakeAffineBuffer({72, 11});
    Buffer output = MakeAffineBuffer({72, 12});
    GraphBuilder builder;
    const auto producer = builder.AddDispatch(AffineInvocation(seed, intermediate));
    const auto consumer = builder.AddDispatch(AffineInvocation(intermediate, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    GraphResidencyState residency;
    REQUIRE(residency.Track(seed).has_value());
    REQUIRE(residency.Track(intermediate).has_value());
    REQUIRE(residency.Track(output).has_value());
    const auto write = [&](const AffineByteFootprint& footprint) {
        return ProducedResidencyWrite{.buffer = intermediate.Describe().id,
                                      .region = footprint.BoundingRegion(),
                                      .byteFootprint = footprint,
                                      .version = 1,
                                      .producer = *producer,
                                      .replica = {.service = Sora::Test::TestBackendService().Id(),
                                                  .location = kDevice,
                                                  .materializer = *producer}};
    };
    REQUIRE(residency.Apply({.writes = {write(*kAffineFirst), write(*kAffineSecond)}}).has_value());

    const auto availability = fixture.artifacts.SnapshotAvailability();
    REQUIRE(availability.has_value());
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const auto candidates =
        EnumerateDispatchCandidates(*graph, *consumer, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    {.packages = *packages,
                                     .resources = fixture.resources.Inventory(),
                                     .artifacts = *availability,
                                     .performance = *performance},
                                    residency);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->residencies.front().inputs.size() == 2);
    REQUIRE(candidates->residencies.front().transfer == CandidateTransferCost{});
    REQUIRE(candidates->residencies.front().inputs[0].Coverage() == ByteCoverageView::Affine(*kAffineFirst));
    REQUIRE(candidates->residencies.front().inputs[1].Coverage() == ByteCoverageView::Affine(*kAffineSecond));
    REQUIRE(CommitDispatchResidencySelection(*consumer, candidates->residencies.front(), residency).has_value());
    REQUIRE(residency.Segments(output.Describe().id).size() == 1);
    REQUIRE(residency.Segments(output.Describe().id).front().Coverage() == ByteCoverageView::Affine(*kAffineDomain));
}

TEST_CASE("Partitioned dispatch residency commits one exact heterogeneous write cover atomically",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Partition]") {
    Buffer output = MakeBuffer({72, 20});
    GraphResidencyState residency;
    REQUIRE(residency.Track(output).has_value());
    constexpr GraphNodeId node{0};
    const auto projection = [&](MemoryLocation location, ByteRegion region) {
        return DispatchResidencyProjection{
            .service = Sora::Test::TestBackendService().Id(),
            .device = location.device,
            .destination = PhysicalMemoryDescriptor{.service = Sora::Test::TestBackendService().Id(),
                                                    .location = location,
                                                    .supportedProperties = MemoryProperty::DeviceLocal},
            .bindings = {{.argument = {0},
                          .access = AccessMode::Write,
                          .buffer = output,
                          .region = region,
                          .alignment = output.Describe().alignment}},
        };
    };
    const auto firstRegion = ByteRegion::Make(kBufferBytes / 4, kBufferBytes / 4);
    const auto secondRegion = ByteRegion::Make(kBufferBytes / 2, kBufferBytes / 4);
    const auto logicalRegion = ByteRegion::Make(kBufferBytes / 4, kBufferBytes / 2);
    REQUIRE(firstRegion.has_value());
    REQUIRE(secondRegion.has_value());
    REQUIRE(logicalRegion.has_value());
    const std::array shards{projection(kDevice, *firstRegion), projection(kAlternativeDevice, *secondRegion)};
    std::array logicalBindings{shards.front().bindings.front()};
    logicalBindings.front().region = *logicalRegion;

    REQUIRE(CommitPartitionedDispatchResidencySelection(node, logicalBindings, shards, residency).has_value());
    const auto segments = residency.Segments(output.Describe().id);
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].Coverage() == ByteCoverageView::Interval(*firstRegion));
    REQUIRE(segments[1].Coverage() == ByteCoverageView::Interval(*secondRegion));
    REQUIRE(segments[0].version == 1);
    REQUIRE(segments[1].version == 1);
    REQUIRE(segments[0].producer == node);
    REQUIRE(segments[1].producer == node);
    REQUIRE(segments[0].replicas.front().location == kDevice);
    REQUIRE(segments[1].replicas.front().location == kAlternativeDevice);
}

TEST_CASE("Partitioned dispatch residency rejects overlapping or duplicate writes without partial state",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Partition][Atomic]") {
    Buffer output = MakeBuffer({72, 21});
    GraphResidencyState residency;
    REQUIRE(residency.Track(output).has_value());
    constexpr GraphNodeId node{0};
    const auto binding = [&](ByteRegion region) {
        return DispatchMemoryBinding{.argument = {0},
                                     .access = AccessMode::Write,
                                     .buffer = output,
                                     .region = region,
                                     .alignment = output.Describe().alignment};
    };
    const auto projection = [&](MemoryLocation location, std::vector<DispatchMemoryBinding> bindings) {
        return DispatchResidencyProjection{
            .service = Sora::Test::TestBackendService().Id(),
            .device = location.device,
            .destination = PhysicalMemoryDescriptor{.service = Sora::Test::TestBackendService().Id(),
                                                    .location = location,
                                                    .supportedProperties = MemoryProperty::DeviceLocal},
            .bindings = std::move(bindings),
        };
    };
    const auto firstRegion = ByteRegion::Make(0, 3 * kBufferBytes / 4);
    const auto secondRegion = ByteRegion::Make(kBufferBytes / 2, kBufferBytes / 2);
    const auto logicalRegion = ByteRegion::Make(0, kBufferBytes);
    REQUIRE(firstRegion.has_value());
    REQUIRE(secondRegion.has_value());
    REQUIRE(logicalRegion.has_value());
    const std::array logicalBindings{binding(*logicalRegion)};
    const std::array overlapping{projection(kDevice, {binding(*firstRegion)}),
                                 projection(kAlternativeDevice, {binding(*secondRegion)})};

    REQUIRE(CommitPartitionedDispatchResidencySelection(node, logicalBindings, overlapping, residency).error() ==
            GraphResidencyError::OverlappingWrites);
    REQUIRE(residency.Segments(output.Describe().id).empty());

    const auto half = ByteRegion::Make(0, kBufferBytes / 2);
    const auto suffix = ByteRegion::Make(kBufferBytes / 2, kBufferBytes / 2);
    REQUIRE(half.has_value());
    REQUIRE(suffix.has_value());
    const std::array duplicateBindings{
        projection(kDevice, {binding(*half), binding(*half)}),
        projection(kAlternativeDevice, {binding(*suffix), binding(*suffix)}),
    };
    const std::array duplicateLogicalBindings{binding(*logicalRegion), binding(*logicalRegion)};
    REQUIRE(CommitPartitionedDispatchResidencySelection(node, duplicateLogicalBindings, duplicateBindings, residency)
                .error() == GraphResidencyError::OverlappingWrites);
    REQUIRE(residency.Segments(output.Describe().id).empty());
}

TEST_CASE("Partitioned dispatch residency rejects an incomplete logical write cover",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Partition][Coverage]") {
    Buffer output = MakeBuffer({72, 22});
    GraphResidencyState residency;
    REQUIRE(residency.Track(output).has_value());
    constexpr GraphNodeId node{0};
    const auto projection = [&](MemoryLocation location, ByteRegion region) {
        return DispatchResidencyProjection{
            .service = Sora::Test::TestBackendService().Id(),
            .device = location.device,
            .destination = PhysicalMemoryDescriptor{.service = Sora::Test::TestBackendService().Id(),
                                                    .location = location,
                                                    .supportedProperties = MemoryProperty::DeviceLocal},
            .bindings = {{.argument = {0},
                          .access = AccessMode::Write,
                          .buffer = output,
                          .region = region,
                          .alignment = output.Describe().alignment}},
        };
    };
    const auto firstRegion = ByteRegion::Make(0, kBufferBytes / 4);
    const auto secondRegion = ByteRegion::Make(kBufferBytes / 2, kBufferBytes / 2);
    const auto logicalRegion = ByteRegion::Make(0, kBufferBytes);
    REQUIRE(firstRegion.has_value());
    REQUIRE(secondRegion.has_value());
    REQUIRE(logicalRegion.has_value());
    const std::array incomplete{projection(kDevice, *firstRegion), projection(kAlternativeDevice, *secondRegion)};
    std::array logicalBindings{incomplete.front().bindings.front()};
    logicalBindings.front().region = *logicalRegion;

    REQUIRE(CommitPartitionedDispatchResidencySelection(node, logicalBindings, incomplete, residency).error() ==
            GraphResidencyError::InvalidRequest);
    REQUIRE(residency.Segments(output.Describe().id).empty());
}

TEST_CASE("Dispatch candidate enumeration fills an affine produced overlay from frozen baseline residency",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Affine][Mixed]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.primaryQueueSupportsCopy = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    REQUIRE(fixture.performance
                .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                 .target = Target(),
                                 .specialization = {},
                                 .queue = kQueue},
                         .workItemCount = 256,
                         .computeNanoseconds = 256,
                         .launchNanoseconds = 100})
                .has_value());
    CalibrateAffineResolution(fixture.performance);

    Buffer seed = MakeAffineBuffer({72, 13});
    Buffer input = MakeAffineBuffer({72, 14});
    Buffer output = MakeAffineBuffer({72, 15});
    REQUIRE(input.CommitWholeWrite(MakeAffineAllocation(kAffineDomain->BoundingRegion().Size(), {81, 1}, destroyCount))
                .has_value());
    GraphBuilder builder;
    const auto producer = builder.AddDispatch(AffineFirstHalfInvocation(seed, input));
    const auto consumer = builder.AddDispatch(AffineInvocation(input, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    auto residency = FreezeGraphResidency(*graph);
    REQUIRE(residency.has_value());
    REQUIRE(residency
                ->Apply({.writes = {{.buffer = input.Describe().id,
                                     .region = kAffineFirst->BoundingRegion(),
                                     .byteFootprint = *kAffineFirst,
                                     .version = 2,
                                     .producer = *producer,
                                     .replica = {.service = Sora::Test::TestBackendService().Id(),
                                                 .location = kDevice,
                                                 .materializer = *producer}}}})
                .has_value());

    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const auto candidates =
        EnumerateDispatchCandidates(*graph, *consumer, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    {.packages = *packages,
                                     .resources = fixture.resources.Inventory(),
                                     .artifacts = *availability,
                                     .performance = *performance},
                                    *residency);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->residencies.front().inputs.size() == 2);
    REQUIRE(candidates->residencies.front().transfer == CandidateTransferCost{});
    const GraphResidencyRoute& produced = candidates->residencies.front().inputs[0];
    const GraphResidencyRoute& baseline = candidates->residencies.front().inputs[1];
    REQUIRE(std::holds_alternative<GraphLocalResidencySource>(produced.source));
    REQUIRE(std::holds_alternative<ResidentReplica>(baseline.source));
    REQUIRE(produced.version == 2);
    REQUIRE(baseline.version == 1);
    REQUIRE(produced.Coverage() == ByteCoverageView::Affine(*kAffineFirst));
    REQUIRE(baseline.Coverage() == ByteCoverageView::Affine(*kAffineSecond));
    const std::array exactCover{produced.Coverage(), baseline.Coverage()};
    REQUIRE(ExactCoverageUnion<256>(ByteCoverageView::Affine(*kAffineDomain), exactCover) ==
            CoverageEquivalence::Equal);
}

TEST_CASE("Dispatch candidate enumeration reuses a partial affine transfer on its destination device",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Affine][Peer][Replica]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.includeSecondDevice = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    CalibrateAffineKernel(fixture.performance, kQueue, Target());
    CalibrateAffineKernel(fixture.performance, kQueueB, Target(kGpuB));
    CalibrateTransfer(fixture.performance);
    CalibrateTransfer(fixture.performance, kPeerTransfer, kDeviceB, kDevice, kServiceB);

    Buffer seed = MakeAffineBuffer({72, 30});
    Buffer intermediate = MakeAffineBuffer({72, 31});
    Buffer firstOutput = MakeAffineBuffer({72, 32});
    Buffer secondOutput = MakeAffineBuffer({72, 33});
    REQUIRE(seed.CommitWholeWrite(
                    MakeAffineAllocation(kAffineDomain->BoundingRegion().Size(), {85, 1}, destroyCount, kHost))
                .has_value());

    GraphBuilder builder;
    const auto producer = builder.AddDispatch(AffineInvocation(seed, intermediate));
    const auto firstConsumer = builder.AddDispatch(AffineFirstHalfInvocation(intermediate, firstOutput));
    const auto secondConsumer = builder.AddDispatch(AffineFirstHalfInvocation(intermediate, secondOutput));
    REQUIRE(producer.has_value());
    REQUIRE(firstConsumer.has_value());
    REQUIRE(secondConsumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    auto residency = FreezeGraphResidency(*graph);
    REQUIRE(residency.has_value());
    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    const std::array deviceA{fixture.device};
    const std::array snapshotA{fixture.snapshot};
    const std::array deviceB{RuntimeDeviceBinding{
        .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))}};
    const std::array snapshotB{DeviceSnapshot{
        .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U}};

    const auto produced =
        EnumerateDispatchCandidates(*graph, *producer, {.epochSequence = 1, .devices = deviceA, .snapshots = snapshotA},
                                    services, *residency, {.memory = {.device = kGpu}});
    REQUIRE(produced.has_value());
    REQUIRE(produced->residencies.size() == 1);
    REQUIRE(CommitDispatchResidencySelection(*producer, produced->residencies.front(), *residency).has_value());
    REQUIRE(residency->Segments(intermediate.Describe().id).size() == 1);
    REQUIRE(residency->Segments(intermediate.Describe().id).front().Coverage() ==
            ByteCoverageView::Affine(*kAffineDomain));

    const auto transferred = EnumerateDispatchCandidates(
        *graph, *firstConsumer, {.epochSequence = 1, .devices = deviceB, .snapshots = snapshotB}, services, *residency,
        {.memory = {.device = kGpuB}});
    REQUIRE(transferred.has_value());
    REQUIRE(transferred->residencies.size() == 1);
    REQUIRE(transferred->residencies.front().inputs.size() == 1);
    REQUIRE(transferred->residencies.front().inputs.front().Coverage() == ByteCoverageView::Affine(*kAffineFirst));
    REQUIRE_FALSE(transferred->residencies.front().inputs.front().AlreadyResident());
    REQUIRE(transferred->residencies.front().transfer.bytes == kAffineFirst->BoundingRegion().Size());
    REQUIRE(CommitDispatchResidencySelection(*firstConsumer, transferred->residencies.front(), *residency).has_value());

    const auto intermediateSegments = residency->Segments(intermediate.Describe().id);
    REQUIRE(intermediateSegments.size() == 2);
    const auto transferredSegment = std::ranges::find_if(intermediateSegments, [](const auto& segment) {
        return segment.Coverage() == ByteCoverageView::Affine(*kAffineFirst);
    });
    REQUIRE(transferredSegment != intermediateSegments.end());
    REQUIRE(transferredSegment->replicas.size() == 2);
    REQUIRE(std::ranges::any_of(transferredSegment->replicas, [](const GraphLocalResidencyReplica& replica) {
        return replica.service == kServiceB && replica.location == kDeviceB;
    }));

    const auto reused = EnumerateDispatchCandidates(*graph, *secondConsumer,
                                                    {.epochSequence = 1, .devices = deviceB, .snapshots = snapshotB},
                                                    services, *residency, {.memory = {.device = kGpuB}});
    REQUIRE(reused.has_value());
    REQUIRE(reused->residencies.size() == 1);
    REQUIRE(reused->residencies.front().inputs.size() == 1);
    REQUIRE(reused->residencies.front().inputs.front().AlreadyResident());
    REQUIRE(reused->residencies.front().inputs.front().Coverage() == ByteCoverageView::Affine(*kAffineFirst));
    REQUIRE(reused->residencies.front().transfer == CandidateTransferCost{});
}

TEST_CASE("Dispatch graph lowering reuses a produced affine part stored in a prior input assembly",
          "[Sora][Compute][Runtime][DispatchGraphPlanning][Residency][Affine][Assembly][Reuse]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture(
        {.includeSecondDevice = true, .primaryQueueSupportsCopy = true, .secondaryQueueSupportsCopy = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    CalibrateAffineKernel(fixture.performance, kQueue, Target());
    CalibrateAffineKernel(fixture.performance, kQueueB, Target(kGpuB));
    CalibrateTransfer(fixture.performance);
    CalibrateTransfer(fixture.performance, kPeerTransfer, kDeviceB, kDevice, kServiceB);

    Buffer seed = MakeAffineBuffer({72, 34});
    Buffer intermediate = MakeAffineBuffer({72, 35});
    Buffer firstOutput = MakeAffineBuffer({72, 36});
    Buffer secondOutput = MakeAffineBuffer({72, 37});
    REQUIRE(seed.CommitWholeWrite(
                    MakeAffineAllocation(kAffineDomain->BoundingRegion().Size(), {86, 1}, destroyCount, kDevice))
                .has_value());
    REQUIRE(intermediate
                .CommitWholeWrite(
                    MakeAffineAllocation(kAffineDomain->BoundingRegion().Size(), {86, 2}, destroyCount, kDevice))
                .has_value());

    GraphBuilder builder;
    const auto producer = builder.AddDispatch(AffineFirstHalfInvocation(seed, intermediate));
    const auto firstConsumer = builder.AddDispatch(AffineInvocation(intermediate, firstOutput));
    const auto secondConsumer = builder.AddDispatch(AffineInvocation(intermediate, secondOutput));
    REQUIRE(producer.has_value());
    REQUIRE(firstConsumer.has_value());
    REQUIRE(secondConsumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    auto residency = FreezeGraphResidency(*graph);
    REQUIRE(residency.has_value());
    const GraphResidencyState baseline = *residency;
    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    const std::array deviceA{fixture.device};
    const std::array snapshotA{fixture.snapshot};
    const std::array deviceB{RuntimeDeviceBinding{
        .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))}};
    const std::array snapshotB{DeviceSnapshot{
        .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U}};

    const auto produced =
        EnumerateDispatchCandidates(*graph, *producer, {.epochSequence = 1, .devices = deviceA, .snapshots = snapshotA},
                                    services, *residency, {.memory = {.device = kGpu}});
    REQUIRE(produced.has_value());
    REQUIRE(produced->placements.size() == 1);
    REQUIRE(CommitDispatchResidencySelection(*producer, produced->residencies.front(), *residency).has_value());

    const auto first = EnumerateDispatchCandidates(*graph, *firstConsumer,
                                                   {.epochSequence = 1, .devices = deviceB, .snapshots = snapshotB},
                                                   services, *residency, {.memory = {.device = kGpuB}});
    REQUIRE(first.has_value());
    REQUIRE(first->placements.size() == 1);
    REQUIRE(first->residencies.front().inputs.size() == 2);
    REQUIRE(std::ranges::none_of(first->residencies.front().inputs, &GraphResidencyRoute::AlreadyResident));
    REQUIRE(CommitDispatchResidencySelection(*firstConsumer, first->residencies.front(), *residency).has_value());

    const auto second = EnumerateDispatchCandidates(*graph, *secondConsumer,
                                                    {.epochSequence = 1, .devices = deviceB, .snapshots = snapshotB},
                                                    services, *residency, {.memory = {.device = kGpuB}});
    REQUIRE(second.has_value());
    REQUIRE(second->placements.size() == 1);
    REQUIRE(second->residencies.front().inputs.size() == 2);
    const auto reusedRoute = std::ranges::find_if(second->residencies.front().inputs, [](const auto& route) {
        return route.Coverage() == ByteCoverageView::Affine(*kAffineFirst);
    });
    REQUIRE(reusedRoute != second->residencies.front().inputs.end());
    REQUIRE(reusedRoute->AlreadyResident());

    const auto selection = [&](GraphNodeId node, const DispatchCandidateSet& candidates) {
        const DispatchPlacementCandidate& placement = candidates.placements.front();
        const auto& invocation = std::get<KernelInvocation>(graph->Operation(node));
        return DispatchGraphSelection{
            .node = node,
            .workload = candidates.workload,
            .execution = DispatchGraphShardSelection{.slice = DispatchSlice::Whole(invocation.Domain()),
                                                     .placement = placement.placement,
                                                     .residency = candidates.residencies[placement.residencyProjection],
                                                     .geometry = candidates.geometry}};
    };
    const std::array selections{selection(*producer, *produced), selection(*firstConsumer, *first),
                                selection(*secondConsumer, *second)};
    auto lowered = LowerDispatchGraph(*graph, services, {}, baseline, selections);
    if (!lowered) {
        UNSCOPED_INFO("code=" << static_cast<uint32_t>(lowered.error().code) << " node=" << lowered.error().node.index
                              << " argument=" << lowered.error().argument.index << " cause=" << lowered.error().cause);
    }
    REQUIRE(lowered.has_value());

    const auto reusedAssembly = std::ranges::find_if(lowered->segments, [&](const PhysicalPlanSegment& segment) {
        const auto* assembly = std::get_if<PlannedAssembly>(&segment);
        return assembly != nullptr && assembly->logicalNode == *secondConsumer;
    });
    REQUIRE(reusedAssembly != lowered->segments.end());
    const auto& assembly = std::get<PlannedAssembly>(*reusedAssembly);
    const auto reusedPart = std::ranges::find_if(assembly.parts, [](const PlannedAssemblyPart& part) {
        return part.Coverage() == ByteCoverageView::Affine(*kAffineFirst);
    });
    REQUIRE(reusedPart != assembly.parts.end());
    REQUIRE(reusedPart->source.has_value());
    const auto* priorAssembly = std::get_if<PlannedAllocationRef>(&*reusedPart->source);
    REQUIRE(priorAssembly != nullptr);
    REQUIRE(lowered->allocations[priorAssembly->allocation.index].purpose ==
            PlannedAllocationPurpose::DispatchInputAssembly);

    auto invalidSelections = selections;
    auto& invalidResidency = std::get<DispatchGraphShardSelection>(invalidSelections.front().execution).residency;
    const auto partialOutput = std::ranges::find_if(
        invalidResidency.bindings, [](const auto& binding) { return binding.access == AccessMode::Write; });
    REQUIRE(partialOutput != invalidResidency.bindings.end());
    partialOutput->publication = DestinationPublication::Publish;
    const auto invalidPublication = LowerDispatchGraph(*graph, services, {}, baseline, invalidSelections);
    REQUIRE_FALSE(invalidPublication.has_value());
    REQUIRE(invalidPublication.error().code == DispatchGraphPlanningErrorCode::UnsupportedBinding);
    REQUIRE(invalidPublication.error().node == *producer);
    REQUIRE(invalidPublication.error().argument == partialOutput->argument);

    REQUIRE(PhysicalGraphPlan::Create(std::move(*lowered), fixture.resources).has_value());
}

TEST_CASE("Dispatch candidate enumeration normalizes overlapping ready replicas into an exact affine route cover",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Affine][Overlap]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.primaryQueueSupportsCopy = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    REQUIRE(fixture.performance
                .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                 .target = Target(),
                                 .specialization = {},
                                 .queue = kQueue},
                         .workItemCount = 256,
                         .computeNanoseconds = 256,
                         .launchNanoseconds = 100})
                .has_value());
    CalibrateAffineResolution(fixture.performance);

    Buffer input = MakeAffineBuffer({72, 16});
    Buffer output = MakeAffineBuffer({72, 17});
    const std::array partition{
        PartitionedWritePart{.region = kAffinePrefix->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffinePrefix->BoundingRegion().Size(), {82, 1}, destroyCount),
                             .byteFootprint = *kAffinePrefix},
        PartitionedWritePart{.region = kAffineSuffix->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffineSuffix->BoundingRegion().Size(), {82, 2}, destroyCount),
                             .byteFootprint = *kAffineSuffix},
    };
    CompletionTransaction publication;
    REQUIRE(publication.AddPartitionedWrite(input, 0, partition).has_value());
    REQUIRE(std::move(publication).Commit().has_value());
    REQUIRE(input
                .PublishReplica(1, *kAffineOverlap,
                                MakeAffineAllocation(kAffineOverlap->BoundingRegion().Size(), {82, 3}, destroyCount))
                .has_value());

    const ExecutionGraph graph = MakeGraph(AffineInvocation(input, output));
    const auto residency = FreezeGraphResidency(graph);
    REQUIRE(residency.has_value());
    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const auto candidates =
        EnumerateDispatchCandidates(graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    {.packages = *packages,
                                     .resources = fixture.resources.Inventory(),
                                     .artifacts = *availability,
                                     .performance = *performance},
                                    *residency);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->residencies.front().inputs.size() == 2);
    const GraphResidencyRoute& prefix = candidates->residencies.front().inputs[0];
    const GraphResidencyRoute& suffix = candidates->residencies.front().inputs[1];
    REQUIRE(prefix.Coverage() == ByteCoverageView::Affine(*kAffineOverlap));
    REQUIRE(suffix.byteFootprint.has_value());
    REQUIRE(suffix.byteFootprint->Extents()[0] == 96);
    REQUIRE(suffix.byteFootprint->OffsetBytes() == 1280);
    const std::array exactCover{prefix.Coverage(), suffix.Coverage()};
    REQUIRE(ExactCoverageUnion<256>(ByteCoverageView::Affine(*kAffineDomain), exactCover) ==
            CoverageEquivalence::Equal);
}

TEST_CASE("Dispatch candidate enumeration clips baseline shards that cross an affine request boundary",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Affine][Intersection]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.primaryQueueSupportsCopy = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    REQUIRE(fixture.performance
                .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                 .target = Target(),
                                 .specialization = {},
                                 .queue = kQueue},
                         .workItemCount = 128,
                         .computeNanoseconds = 128,
                         .launchNanoseconds = 100})
                .has_value());
    CalibrateAffineResolution(fixture.performance);

    Buffer input = MakeAffineBuffer({72, 18});
    Buffer output = MakeAffineBuffer({72, 19});
    const std::array partition{
        PartitionedWritePart{.region = kAffinePrefix->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffinePrefix->BoundingRegion().Size(), {83, 1}, destroyCount),
                             .byteFootprint = *kAffinePrefix},
        PartitionedWritePart{.region = kAffineSuffix->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffineSuffix->BoundingRegion().Size(), {83, 2}, destroyCount),
                             .byteFootprint = *kAffineSuffix},
    };
    CompletionTransaction publication;
    REQUIRE(publication.AddPartitionedWrite(input, 0, partition).has_value());
    REQUIRE(std::move(publication).Commit().has_value());

    const ExecutionGraph graph = MakeGraph(AffineMiddleHalfInvocation(input, output));
    const auto residency = FreezeGraphResidency(graph);
    REQUIRE(residency.has_value());
    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const auto candidates =
        EnumerateDispatchCandidates(graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    {.packages = *packages,
                                     .resources = fixture.resources.Inventory(),
                                     .artifacts = *availability,
                                     .performance = *performance},
                                    *residency);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->residencies.front().inputs.size() == 2);
    const GraphResidencyRoute& suffix = candidates->residencies.front().inputs[0];
    const GraphResidencyRoute& prefix = candidates->residencies.front().inputs[1];
    REQUIRE(prefix.byteFootprint.has_value());
    REQUIRE(prefix.byteFootprint->Extents()[0] == 32);
    REQUIRE(prefix.byteFootprint->OffsetBytes() == 512);
    REQUIRE(suffix.byteFootprint.has_value());
    REQUIRE(suffix.byteFootprint->Extents()[0] == 96);
    REQUIRE(suffix.byteFootprint->OffsetBytes() == 768);
    const std::array exactCover{prefix.Coverage(), suffix.Coverage()};
    REQUIRE(ExactCoverageUnion<256>(ByteCoverageView::Affine(*kAffineMiddle), exactCover) ==
            CoverageEquivalence::Equal);
}

TEST_CASE("Dispatch candidate enumeration prefers a local affine shard cover over a transferable whole replica",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Affine][Cost]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.primaryQueueSupportsCopy = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    REQUIRE(fixture.performance
                .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                 .target = Target(),
                                 .specialization = {},
                                 .queue = kQueue},
                         .workItemCount = 256,
                         .computeNanoseconds = 256,
                         .launchNanoseconds = 100})
                .has_value());
    CalibrateAffineResolution(fixture.performance);
    CalibrateTransfer(fixture.performance);

    Buffer input = MakeAffineBuffer({72, 20});
    Buffer output = MakeAffineBuffer({72, 21});
    const std::array partition{
        PartitionedWritePart{.region = kAffinePrefix->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffinePrefix->BoundingRegion().Size(), {84, 1}, destroyCount),
                             .byteFootprint = *kAffinePrefix},
        PartitionedWritePart{.region = kAffineSuffix->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffineSuffix->BoundingRegion().Size(), {84, 2}, destroyCount),
                             .byteFootprint = *kAffineSuffix},
    };
    CompletionTransaction publication;
    REQUIRE(publication.AddPartitionedWrite(input, 0, partition).has_value());
    REQUIRE(std::move(publication).Commit().has_value());
    REQUIRE(
        input
            .PublishReplica(1, *kAffineDomain,
                            MakeAffineAllocation(kAffineDomain->BoundingRegion().Size(), {84, 3}, destroyCount, kHost))
            .has_value());

    const ExecutionGraph graph = MakeGraph(AffineInvocation(input, output));
    const auto candidates = Enumerate(graph, fixture);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->residencies.front().inputs.size() == 2);
    REQUIRE(candidates->residencies.front().transfer == CandidateTransferCost{});
    REQUIRE(std::ranges::all_of(candidates->residencies.front().inputs, &GraphResidencyRoute::AlreadyResident));
    const std::array exactCover{candidates->residencies.front().inputs[0].Coverage(),
                                candidates->residencies.front().inputs[1].Coverage()};
    REQUIRE(ExactCoverageUnion<256>(ByteCoverageView::Affine(*kAffineDomain), exactCover) ==
            CoverageEquivalence::Equal);
}

TEST_CASE("Dispatch graph lowering assembles a published affine partition cover before whole-value dispatch",
          "[Sora][Compute][Runtime][DispatchGraphPlanning][Residency][Affine][Assembly]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.includeCopyOnlyQueue = true}, false);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    REQUIRE(fixture.performance
                .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                 .target = Target(),
                                 .specialization = {},
                                 .queue = kQueue},
                         .workItemCount = 256,
                         .computeNanoseconds = 256,
                         .launchNanoseconds = 100})
                .has_value());
    CalibrateAffineResolution(fixture.performance);

    Buffer input = MakeAffineBuffer({72, 20});
    Buffer output = MakeAffineBuffer({72, 21});
    const std::array parts{
        PartitionedWritePart{.region = kAffineFirst->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffineFirst->BoundingRegion().Size(), {80, 1}, destroyCount),
                             .byteFootprint = *kAffineFirst},
        PartitionedWritePart{.region = kAffineSecond->BoundingRegion(),
                             .allocation =
                                 MakeAffineAllocation(kAffineSecond->BoundingRegion().Size(), {80, 2}, destroyCount),
                             .byteFootprint = *kAffineSecond},
    };
    CompletionTransaction publication;
    REQUIRE(publication.AddPartitionedWrite(input, 0, parts).has_value());
    REQUIRE(std::move(publication).Commit().has_value());
    const ExecutionGraph graph = MakeGraph(AffineInvocation(input, output));
    const auto baseline = FreezeGraphResidency(graph);
    REQUIRE(baseline.has_value());
    const BufferResidencySnapshot* ready = baseline->ReadySnapshot(input.Describe().id);
    REQUIRE(ready != nullptr);
    REQUIRE(baseline->Segments(input.Describe().id).empty());
    REQUIRE(ready->replicas.size() == 2);
    REQUIRE(ready->replicas[0].Coverage() == ByteCoverageView::Affine(*kAffineFirst));
    REQUIRE(ready->replicas[1].Coverage() == ByteCoverageView::Affine(*kAffineSecond));
    const std::array readyCoverages{ready->replicas[0].Coverage(), ready->replicas[1].Coverage()};
    REQUIRE(ExactCoverageUnion<256>(ByteCoverageView::Affine(*kAffineDomain), readyCoverages) ==
            CoverageEquivalence::Equal);
    const auto wholeReady = PlanResidency(*ready,
                                          {.service = Sora::Test::TestBackendService().Id(),
                                           .destination = kDevice,
                                           .region = kAffineDomain->BoundingRegion(),
                                           .coverage = ByteCoverageView::Affine(*kAffineDomain)},
                                          {});
    REQUIRE_FALSE(wholeReady.has_value());
    REQUIRE(wholeReady.error() == ResidencyPlanningError::NoSourceReplica);
    REQUIRE(PlanResidency(*ready,
                          {.service = Sora::Test::TestBackendService().Id(),
                           .destination = kDevice,
                           .region = kAffineFirst->BoundingRegion(),
                           .coverage = ByteCoverageView::Affine(*kAffineFirst)},
                          {})
                .has_value());
    REQUIRE(PlanResidency(*ready,
                          {.service = Sora::Test::TestBackendService().Id(),
                           .destination = kDevice,
                           .region = kAffineSecond->BoundingRegion(),
                           .coverage = ByteCoverageView::Affine(*kAffineSecond)},
                          {})
                .has_value());

    {
        PlanningFixture noCopyFixture({}, false);
        REQUIRE(noCopyFixture.packages.Register(kAffineCopyProgram).has_value());
        CalibrateAffineKernel(noCopyFixture.performance, kQueue, Target());
        const auto noCopyAvailability = noCopyFixture.artifacts.SnapshotAvailability();
        const auto noCopyPerformance = noCopyFixture.performance.Snapshot();
        const auto noCopyPackages = FreezeProgramCatalog(noCopyFixture.packages);
        REQUIRE(noCopyAvailability.has_value());
        const std::array noCopyDevices{noCopyFixture.device};
        const std::array noCopySnapshots{noCopyFixture.snapshot};
        const GraphPlanningContext noCopyContext{
            .epochSequence = 1, .devices = noCopyDevices, .snapshots = noCopySnapshots};
        const GraphPlanningServices noCopyServices{.packages = *noCopyPackages,
                                                   .resources = noCopyFixture.resources.Inventory(),
                                                   .artifacts = *noCopyAvailability,
                                                   .performance = *noCopyPerformance};
        const auto noCopyResources = EnumerateDispatchResourceCandidates(graph, {0}, noCopyContext, noCopyServices);
        REQUIRE(noCopyResources.has_value());
        REQUIRE(noCopyResources->candidates.size() == 1);
        const auto noCopyCandidates = EnumerateDispatchCandidates(graph, {0}, noCopyContext, noCopyServices, *baseline);
        REQUIRE(noCopyCandidates.has_value());
        REQUIRE(noCopyCandidates->residencies.empty());
        REQUIRE(noCopyCandidates->placements.empty());

        const DispatchResourceCandidate& resource = noCopyResources->candidates.front();
        const DispatchResidencyProjection destination{.service = resource.placement.service,
                                                      .device = resource.placement.device.id,
                                                      .destination = resource.destination,
                                                      .bindings = noCopyResources->bindings};
        const auto& invocation = std::get<KernelInvocation>(graph.Operation({0}));
        const auto projected = ProjectDispatchSliceResidency(invocation, {0}, DispatchSlice::Whole(invocation.Domain()),
                                                             destination, noCopyServices, *baseline);
        REQUIRE_FALSE(projected.has_value());
        REQUIRE(projected.error().code == DispatchCandidateEnumerationErrorCode::InputAssemblyUnavailable);
    }

    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    const auto resourceCandidates = EnumerateDispatchResourceCandidates(
        graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots}, services);
    REQUIRE(resourceCandidates.has_value());
    REQUIRE(resourceCandidates->bindings.size() == 2);
    REQUIRE(resourceCandidates->bindings.front().byteFootprint == kAffineDomain);
    REQUIRE(resourceCandidates->candidates.size() == 1);
    REQUIRE(resourceCandidates->candidates.front().destination.has_value());
    REQUIRE(resourceCandidates->candidates.front().destination->location == kDevice);
    const auto candidates = EnumerateDispatchCandidates(
        graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots}, services, *baseline);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->residencies.front().inputs.size() == 2);
    REQUIRE(candidates->placements.size() == 1);
    const DispatchPlacementCandidate& candidate = candidates->placements.front();
    DispatchResidencyProjection reversedResidency = candidates->residencies[candidate.residencyProjection];
    std::ranges::reverse(reversedResidency.inputs);
    const auto& invocation = std::get<KernelInvocation>(graph.Operation({0}));
    const std::array selections{DispatchGraphSelection{
        .node = {0},
        .workload = candidates->workload,
        .execution = DispatchGraphShardSelection{.slice = DispatchSlice::Whole(invocation.Domain()),
                                                 .placement = candidate.placement,
                                                 .residency = std::move(reversedResidency),
                                                 .geometry = candidates->geometry}}};

    auto forgedBinding = selections;
    auto& forgedBindingShard = std::get<DispatchGraphShardSelection>(forgedBinding.front().execution);
    forgedBindingShard.residency.bindings.front().region = kAffineFirst->BoundingRegion();
    forgedBindingShard.residency.bindings.front().byteFootprint = *kAffineFirst;
    const auto rejectedBinding = LowerDispatchGraph(graph, services, {}, *baseline, forgedBinding);
    REQUIRE_FALSE(rejectedBinding.has_value());
    REQUIRE(rejectedBinding.error().code == DispatchGraphPlanningErrorCode::InvalidSelection);

    auto incompleteRoutes = selections;
    std::get<DispatchGraphShardSelection>(incompleteRoutes.front().execution).residency.inputs.pop_back();
    const auto rejectedRoutes = LowerDispatchGraph(graph, services, {}, *baseline, incompleteRoutes);
    REQUIRE_FALSE(rejectedRoutes.has_value());
    REQUIRE(rejectedRoutes.error().code == DispatchGraphPlanningErrorCode::InvalidSelection);

    auto lowered = LowerDispatchGraph(graph, services, {}, *baseline, selections);
    REQUIRE(lowered.has_value());
    REQUIRE(lowered->allocations.size() == 2);
    REQUIRE(lowered->segments.size() == 2);
    REQUIRE(lowered->allocations.front().purpose == PlannedAllocationPurpose::DispatchInputAssembly);
    const auto* assembly = std::get_if<PlannedAssembly>(&lowered->segments.front());
    REQUIRE(assembly != nullptr);
    REQUIRE(assembly->queue.queue == kAlternativeQueue);
    REQUIRE(assembly->parts.size() == 2);
    REQUIRE(assembly->parts[0].region.Offset() < assembly->parts[1].region.Offset());
    const auto* dispatch = std::get_if<PlannedDispatch>(&lowered->segments.back());
    REQUIRE(dispatch != nullptr);
    REQUIRE(dispatch->queue.queue == kQueue);
    REQUIRE(dispatch->bufferStorage.front().input.has_value());
    REQUIRE(std::get<PlannedAllocationRef>(*dispatch->bufferStorage.front().input).allocation == assembly->destination);
    REQUIRE(PhysicalGraphPlan::Create(std::move(*lowered), fixture.resources).has_value());
}

TEST_CASE("Dispatch graph lowering direct-fills one assembly from independent remote routes",
          "[Sora][Compute][Runtime][DispatchGraphPlanning][Residency][Affine][DirectFill]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.primaryQueueSupportsCopy = false}, false);
    CalibrateTransfer(fixture.performance);
    REQUIRE(fixture.packages.Register(kAffineCopyProgram).has_value());
    REQUIRE(fixture.performance
                .Update({.key = {.kernel = kAffineCopyProgram.Hash(),
                                 .target = Target(),
                                 .specialization = {},
                                 .queue = kQueue},
                         .workItemCount = 256,
                         .computeNanoseconds = 256,
                         .launchNanoseconds = 100})
                .has_value());
    CalibrateAffineResolution(fixture.performance);

    Buffer input = MakeAffineBuffer({72, 22});
    Buffer output = MakeAffineBuffer({72, 23});
    const std::array parts{
        PartitionedWritePart{
            .region = kAffineFirst->BoundingRegion(),
            .allocation = MakeAffineAllocation(kAffineFirst->BoundingRegion().Size(), {81, 1}, destroyCount, kHost),
            .byteFootprint = *kAffineFirst},
        PartitionedWritePart{
            .region = kAffineSecond->BoundingRegion(),
            .allocation = MakeAffineAllocation(kAffineSecond->BoundingRegion().Size(), {81, 2}, destroyCount, kHost),
            .byteFootprint = *kAffineSecond},
    };
    CompletionTransaction publication;
    REQUIRE(publication.AddPartitionedWrite(input, 0, parts).has_value());
    REQUIRE(std::move(publication).Commit().has_value());
    const ExecutionGraph graph = MakeGraph(AffineInvocation(input, output));
    const auto baseline = FreezeGraphResidency(graph);
    REQUIRE(baseline.has_value());

    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    const auto candidates = EnumerateDispatchCandidates(
        graph, {0}, {.epochSequence = 1, .devices = devices, .snapshots = snapshots}, services, *baseline);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->residencies.front().inputs.size() == 2);
    REQUIRE(std::ranges::none_of(candidates->residencies.front().inputs, &GraphResidencyRoute::AlreadyResident));
    REQUIRE(candidates->placements.size() == 1);
    const DispatchPlacementCandidate& candidate = candidates->placements.front();
    const auto& invocation = std::get<KernelInvocation>(graph.Operation({0}));
    const std::array selections{DispatchGraphSelection{
        .node = {0},
        .workload = candidates->workload,
        .execution = DispatchGraphShardSelection{.slice = DispatchSlice::Whole(invocation.Domain()),
                                                 .placement = candidate.placement,
                                                 .residency = candidates->residencies[candidate.residencyProjection],
                                                 .geometry = candidates->geometry}}};

    auto lowered = LowerDispatchGraph(graph, services, {}, *baseline, selections);
    REQUIRE(lowered.has_value());
    REQUIRE(lowered->allocations.size() == 2);
    REQUIRE(lowered->segments.size() == 4);
    REQUIRE(lowered->allocations.front().purpose == PlannedAllocationPurpose::DispatchInputAssembly);
    for (uint32_t index = 0; index < 2; ++index) {
        const auto* transfer = std::get_if<PlannedTransfer>(&lowered->segments[index]);
        REQUIRE(transfer != nullptr);
        REQUIRE(transfer->target == PlannedTransferTarget::AssemblyPart);
        REQUIRE(transfer->destination == PlannedAllocationId{0});
    }
    const auto* assembly = std::get_if<PlannedAssembly>(&lowered->segments[2]);
    REQUIRE(assembly != nullptr);
    REQUIRE(assembly->destination == PlannedAllocationId{0});
    REQUIRE(assembly->parts.size() == 2);
    REQUIRE(std::ranges::all_of(assembly->parts, [](const PlannedAssemblyPart& part) {
        return !part.source && part.transferProducer.Valid();
    }));
    REQUIRE(std::ranges::find(lowered->edges, PhysicalPlanEdge{{0}, {2}}) != lowered->edges.end());
    REQUIRE(std::ranges::find(lowered->edges, PhysicalPlanEdge{{1}, {2}}) != lowered->edges.end());
    REQUIRE(std::ranges::find(lowered->edges, PhysicalPlanEdge{{0}, {1}}) == lowered->edges.end());
    REQUIRE(std::ranges::find(lowered->edges, PhysicalPlanEdge{{1}, {0}}) == lowered->edges.end());
    const auto* dispatch = std::get_if<PlannedDispatch>(&lowered->segments[3]);
    REQUIRE(dispatch != nullptr);
    REQUIRE(dispatch->bufferStorage.front().input.has_value());
    REQUIRE(std::get<PlannedAllocationRef>(*dispatch->bufferStorage.front().input) ==
            PlannedAllocationRef{.allocation = {0}, .readyAfter = {2}});
    REQUIRE(PhysicalGraphPlan::Create(std::move(*lowered), fixture.resources).has_value());
}

TEST_CASE("Dispatch candidate enumeration partitions produced slices from "
          "frozen snapshot gaps",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency]["
          "Interval]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture({.primaryQueueSupportsCopy = true});
    CalibrateTransfer(fixture.performance);
    Buffer input = MakeBuffer({72, 4});
    Buffer output = MakeBuffer({72, 5});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const auto producedRegion = ByteRegion::Make(256, 256);
    REQUIRE(producedRegion.has_value());
    auto slice = BufferSlice::Create(input, *producedRegion);
    REQUIRE(slice.has_value());
    constexpr std::array patternBytes{std::byte{0}};
    const auto pattern = FillPattern::Create(patternBytes);
    REQUIRE(pattern.has_value());
    auto fill = FillOperation::Create(std::move(*slice), *pattern);
    REQUIRE(fill.has_value());

    GraphBuilder builder;
    const auto producer = builder.AddFill(std::move(*fill));
    const auto consumer = builder.AddDispatch(Invocation(input, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    GraphResidencyState residency;
    REQUIRE(residency.Track(input).has_value());
    const GraphResidencyUpdate update{.writes = {{.buffer = input.Describe().id,
                                                  .region = *producedRegion,
                                                  .version = 2,
                                                  .producer = *producer,
                                                  .replica = {.service = Sora::Test::TestBackendService().Id(),
                                                              .location = kDevice,
                                                              .materializer = *producer}}}};
    REQUIRE(residency.Apply(update).has_value());
    const auto availability = fixture.artifacts.SnapshotAvailability();
    REQUIRE(availability.has_value());
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const auto candidates =
        EnumerateDispatchCandidates(*graph, *consumer, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                    {.packages = *packages,
                                     .resources = fixture.resources.Inventory(),
                                     .artifacts = *availability,
                                     .performance = *performance},
                                    residency);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    const auto& routes = candidates->residencies.front().inputs;
    REQUIRE(routes.size() == 3);
    REQUIRE(routes[0].region == *ByteRegion::Make(0, 256));
    REQUIRE(std::holds_alternative<ResidentReplica>(routes[0].source));
    REQUIRE(routes[1].region == *producedRegion);
    REQUIRE(std::holds_alternative<GraphLocalResidencySource>(routes[1].source));
    REQUIRE(routes[1].AlreadyResident());
    REQUIRE(routes[2].region == *ByteRegion::Make(512, 512));
    REQUIRE(std::holds_alternative<ResidentReplica>(routes[2].source));
}

TEST_CASE("Dispatch candidate enumeration routes produced values across devices and "
          "reuses fanout replicas",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Peer]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}};
    CalibrateTransfer(fixture.performance);
    CalibrateTransfer(fixture.performance, kPeerTransfer, kDeviceB, kDevice, kServiceB);
    CalibrateKernel(fixture.performance, kQueueB, Target(kGpuB));
    Buffer input = MakeBuffer({72, 6});
    Buffer intermediate = MakeBuffer({72, 7});
    Buffer firstOutput = MakeBuffer({72, 8});
    Buffer secondOutput = MakeBuffer({72, 9});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());

    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(input, intermediate));
    const auto firstConsumer = builder.AddDispatch(Invocation(intermediate, firstOutput));
    const auto secondConsumer = builder.AddDispatch(Invocation(intermediate, secondOutput));
    REQUIRE(producer.has_value());
    REQUIRE(firstConsumer.has_value());
    REQUIRE(secondConsumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    GraphResidencyState residency;
    REQUIRE(residency.Track(input).has_value());
    REQUIRE(residency.Track(intermediate).has_value());
    REQUIRE(residency.Track(firstOutput).has_value());
    REQUIRE(residency.Track(secondOutput).has_value());
    const auto availability = fixture.artifacts.SnapshotAvailability();
    REQUIRE(availability.has_value());
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    const std::array deviceA{fixture.device};
    const std::array snapshotA{fixture.snapshot};
    const std::array deviceB{RuntimeDeviceBinding{
        .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))}};
    const std::array snapshotB{DeviceSnapshot{
        .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U}};

    const auto produced =
        EnumerateDispatchCandidates(*graph, *producer, {.epochSequence = 1, .devices = deviceA, .snapshots = snapshotA},
                                    services, residency, {.memory = {.device = kGpu}});
    REQUIRE(produced.has_value());
    REQUIRE(produced->residencies.size() == 1);
    REQUIRE(CommitDispatchResidencySelection(*producer, produced->residencies.front(), residency).has_value());

    const auto transferred = EnumerateDispatchCandidates(
        *graph, *firstConsumer, {.epochSequence = 1, .devices = deviceB, .snapshots = snapshotB}, services, residency,
        {.memory = {.device = kGpuB}});
    REQUIRE(transferred.has_value());
    REQUIRE(transferred->residencies.size() == 1);
    REQUIRE(transferred->residencies.front().inputs.size() == 1);
    REQUIRE(transferred->residencies.front().inputs.front().hops.size() == 1);
    REQUIRE(transferred->residencies.front().inputs.front().hops.front().engine == kPeerTransfer);
    REQUIRE(transferred->residencies.front().transfer.bytes == kBufferBytes);
    REQUIRE(CommitDispatchResidencySelection(*firstConsumer, transferred->residencies.front(), residency).has_value());

    const auto reused = EnumerateDispatchCandidates(*graph, *secondConsumer,
                                                    {.epochSequence = 1, .devices = deviceB, .snapshots = snapshotB},
                                                    services, residency, {.memory = {.device = kGpuB}});
    REQUIRE(reused.has_value());
    REQUIRE(reused->residencies.size() == 1);
    REQUIRE(reused->residencies.front().inputs.size() == 1);
    REQUIRE(reused->residencies.front().inputs.front().AlreadyResident());
    const auto* reusedSource =
        std::get_if<GraphLocalResidencySource>(&reused->residencies.front().inputs.front().source);
    REQUIRE(reusedSource != nullptr);
    REQUIRE(reusedSource->producer == *producer);
    REQUIRE(reusedSource->replica.materializer == *firstConsumer);
    REQUIRE(reused->residencies.front().transfer == CandidateTransferCost{});
}

TEST_CASE("Dispatch candidate enumeration reuses one baseline transfer across graph consumers and lowering",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Residency][Baseline][Peer]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}};
    CalibrateTransfer(fixture.performance, kPeerTransfer, kDeviceB, kDevice, kServiceB);
    CalibrateKernel(fixture.performance, kQueueB, Target(kGpuB));
    Buffer input = MakeBuffer({72, 10});
    Buffer firstOutput = MakeBuffer({72, 11});
    Buffer secondOutput = MakeBuffer({72, 12});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());

    GraphBuilder builder;
    const auto firstConsumer = builder.AddDispatch(Invocation(input, firstOutput));
    const auto secondConsumer = builder.AddDispatch(Invocation(input, secondOutput));
    REQUIRE(firstConsumer.has_value());
    REQUIRE(secondConsumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    auto residency = FreezeGraphResidency(*graph);
    REQUIRE(residency.has_value());
    const GraphResidencyState baseline = *residency;
    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    const std::array devices{RuntimeDeviceBinding{
        .service = kServiceB, .descriptor = Descriptor(kGpuB), .target = RuntimeTarget(Target(kGpuB))}};
    const std::array snapshots{DeviceSnapshot{
        .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U}};
    const GraphPlanningContext context{.epochSequence = 1, .devices = devices, .snapshots = snapshots};
    const DispatchEnumerationPolicy policy{.memory = {.device = kGpuB}};

    const auto first = EnumerateDispatchCandidates(*graph, *firstConsumer, context, services, *residency, policy);
    REQUIRE(first.has_value());
    REQUIRE(first->residencies.size() == 1);
    REQUIRE(first->residencies.front().inputs.size() == 1);
    REQUIRE(first->residencies.front().inputs.front().hops.size() == 1);
    REQUIRE(std::holds_alternative<ResidentReplica>(first->residencies.front().inputs.front().source));
    REQUIRE(CommitDispatchResidencySelection(*firstConsumer, first->residencies.front(), *residency).has_value());

    const auto second = EnumerateDispatchCandidates(*graph, *secondConsumer, context, services, *residency, policy);
    REQUIRE(second.has_value());
    REQUIRE(second->residencies.size() == 1);
    REQUIRE(second->residencies.front().inputs.size() == 1);
    REQUIRE(second->residencies.front().inputs.front().AlreadyResident());
    const auto* reused = std::get_if<GraphLocalResidencySource>(&second->residencies.front().inputs.front().source);
    REQUIRE(reused != nullptr);
    REQUIRE_FALSE(reused->producer.has_value());
    REQUIRE(reused->replica.materializer == *firstConsumer);
    REQUIRE(second->residencies.front().transfer == CandidateTransferCost{});

    const auto selection = [&](GraphNodeId node, const DispatchCandidateSet& candidates) {
        const DispatchPlacementCandidate& placement = candidates.placements.front();
        const auto& invocation = std::get<KernelInvocation>(graph->Operation(node));
        return DispatchGraphSelection{
            .node = node,
            .workload = candidates.workload,
            .execution = DispatchGraphShardSelection{.slice = DispatchSlice::Whole(invocation.Domain()),
                                                     .placement = placement.placement,
                                                     .residency = candidates.residencies[placement.residencyProjection],
                                                     .geometry = candidates.geometry}};
    };
    const std::array selections{selection(*firstConsumer, *first), selection(*secondConsumer, *second)};
    auto lowered = LowerDispatchGraph(*graph, services, {}, baseline, selections);
    REQUIRE(lowered.has_value());

    const PlannedTransfer* transfer = nullptr;
    PhysicalPlanSegmentId transferId{};
    const PlannedDispatch* secondDispatch = nullptr;
    for (uint32_t index = 0; index < lowered->segments.size(); ++index) {
        if (const auto* candidate = std::get_if<PlannedTransfer>(&lowered->segments[index]);
            candidate != nullptr && candidate->buffer == input.Describe().id) {
            REQUIRE(transfer == nullptr);
            transfer = candidate;
            transferId = {index};
        }
        if (const auto* candidate = std::get_if<PlannedDispatch>(&lowered->segments[index]);
            candidate != nullptr && candidate->logicalNode == *secondConsumer) {
            secondDispatch = candidate;
        }
    }
    REQUIRE(transfer != nullptr);
    REQUIRE(secondDispatch != nullptr);
    REQUIRE(secondDispatch->bufferStorage.front().input.has_value());
    REQUIRE(std::get<PlannedAllocationRef>(*secondDispatch->bufferStorage.front().input) ==
            PlannedAllocationRef{.allocation = transfer->destination, .readyAfter = transferId});
    REQUIRE(PhysicalGraphPlan::Create(std::move(*lowered), fixture.resources).has_value());
}

TEST_CASE("Dispatch candidate enumeration shares residency across queues and "
          "honors memory preference order",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Policy]") {
    std::atomic<uint32_t> destroyCount = 0;
    {
        PlanningFixture fixture{{.includeAlternativeQueue = true, .includeAlternativeMemory = true}};
        CalibrateTransfer(fixture.performance);
        CalibrateTransfer(fixture.performance, kAlternativeTransfer, kAlternativeDevice);
        Buffer input = MakeBuffer({73, 1});
        Buffer output = MakeBuffer({73, 2});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));
        constexpr std::array preferences{kAlternativeDevice, kDevice};

        const auto candidates = Enumerate(
            graph, fixture,
            {.memory = {.preferredLocations = preferences, .requiredProperties = MemoryProperty::DeviceLocal}});
        REQUIRE(candidates.has_value());
        REQUIRE(candidates->residencies.size() == 1);
        REQUIRE(candidates->residencies[0].destination->location == kAlternativeDevice);
        REQUIRE(candidates->placements.size() == 2);
        REQUIRE(candidates->placements[0].residencyProjection == 0);
        REQUIRE(candidates->placements[1].residencyProjection == 0);
        REQUIRE(candidates->placements[0].placement.transfer == candidates->placements[1].placement.transfer);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Dispatch candidate enumeration expands compiler variants before queue placement",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Compiler]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeAlternativeCompiler = true}};
    const TargetFingerprint portableTarget =
        Target(kGpu, kAlternativeCompiler, ExecutableTargetDomain::BackendPortable);
    CalibrateKernel(fixture.performance, kQueue, portableTarget);
    CalibrateTransfer(fixture.performance);
    CacheExecutable(fixture.artifacts, portableTarget);

    Buffer input = MakeBuffer({73, 3});
    Buffer output = MakeBuffer({73, 4});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));
    const auto candidates = Enumerate(graph, fixture);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->residencies.size() == 1);
    REQUIRE(candidates->placements.size() == 2);
    REQUIRE(candidates->placements[0].placement.target == portableTarget);
    REQUIRE(candidates->placements[0].placement.executable.mode == ArtifactResolutionMode::Live);
    REQUIRE(candidates->placements[1].placement.target == Target());
    REQUIRE(candidates->placements[1].placement.executable.mode == ArtifactResolutionMode::TransientProduction);
    REQUIRE(candidates->placements[0].residencyProjection == candidates->placements[1].residencyProjection);
    REQUIRE(candidates->placements[0].placement.transfer == candidates->placements[1].placement.transfer);

    std::array placements{candidates->placements[0].placement, candidates->placements[1].placement};
    const auto selected = PlanExecution(candidates->workload, placements);
    REQUIRE(selected.has_value());
    REQUIRE(selected->target == portableTarget);
}

TEST_CASE("Exact packaged executable remains eligible when runtime compilation is forbidden",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][PortablePackage][Policy]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.persistentPrimaryCompiler = true, .includePackagedExecutable = true}};
    CalibrateTransfer(fixture.performance);
    Buffer input = MakeBuffer({73, 5});
    Buffer output = MakeBuffer({73, 6});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());

    const ExecutionGraph graph = MakeGraph(Invocation(input, output));
    const auto candidates = Enumerate(graph, fixture);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->placements.size() == 1);
    REQUIRE(candidates->placements.front().placement.executable.mode == ArtifactResolutionMode::PackageImport);
    REQUIRE(candidates->placements.front().placement.executable.importNanoseconds == 10'000);

    const std::array placement{candidates->placements.front().placement};
    const auto importOnly =
        PlanExecution(candidates->workload, placement, {.allowArtifactImport = true, .allowRuntimeCompilation = false});
    REQUIRE(importOnly.has_value());
    const auto importForbidden =
        PlanExecution(candidates->workload, placement, {.allowArtifactImport = false, .allowRuntimeCompilation = true});
    REQUIRE(importForbidden.error() == PlanningError::NoCandidate);
}

TEST_CASE("Graph residency freezing rejects ambiguous logical identity",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Identity]") {
    std::atomic<uint32_t> destroyCount = 0;
    Buffer input = MakeBuffer({74, 1});
    Buffer conflictingOutput = MakeBuffer({74, 1});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph conflictingGraph = MakeGraph(Invocation(input, conflictingOutput));
    const auto conflicting = FreezeGraphResidency(conflictingGraph);
    REQUIRE_FALSE(conflicting.has_value());
    REQUIRE(conflicting.error().code == GraphResidencySnapshotErrorCode::ConflictingBufferIdentity);
    REQUIRE(conflicting.error().node == GraphNodeId{0});
    REQUIRE(conflicting.error().buffer == input.Describe().id);
}

TEST_CASE("Dispatch candidate enumeration expands and prices exact pipeline "
          "configurations",
          "[Sora][Compute][Runtime][DispatchCandidateEnumeration][Pipeline]") {
    std::atomic<uint32_t> destroyCount = 0;
    Buffer pipelineInput = MakeBuffer({74, 2});
    Buffer pipelineOutput = MakeBuffer({74, 3});
    REQUIRE(pipelineInput.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph pipelineGraph = MakeGraph(Invocation(pipelineInput, pipelineOutput));
    PlanningFixture pipelineFixture{
        {.primaryDispatchArtifact = ArtifactKind::DevicePipeline, .includeSecondPipelineConfiguration = true}};
    CalibrateTransfer(pipelineFixture.performance);
    const auto uncalibrated = Enumerate(pipelineGraph, pipelineFixture);
    REQUIRE(uncalibrated.has_value());
    REQUIRE(uncalibrated->residencies.empty());
    REQUIRE(uncalibrated->placements.empty());

    CalibratePipeline(pipelineFixture.performance, kPipelineStateA, 20'000);
    CalibratePipeline(pipelineFixture.performance, kPipelineStateB, 30'000);
    const auto cold = Enumerate(pipelineGraph, pipelineFixture);
    REQUIRE(cold.has_value());
    REQUIRE(cold->residencies.size() == 1);
    REQUIRE(cold->placements.size() == 2);
    const auto coldA = std::ranges::find_if(cold->placements, [](const DispatchPlacementCandidate& candidate) {
        return candidate.placement.pipeline && candidate.placement.pipeline->key == PipelineFor(kPipelineStateA);
    });
    const auto coldB = std::ranges::find_if(cold->placements, [](const DispatchPlacementCandidate& candidate) {
        return candidate.placement.pipeline && candidate.placement.pipeline->key == PipelineFor(kPipelineStateB);
    });
    REQUIRE(coldA != cold->placements.end());
    REQUIRE(coldB != cold->placements.end());
    REQUIRE(coldA->placement.pipeline->resolution.mode == ArtifactResolutionMode::TransientProduction);
    REQUIRE(coldA->placement.pipeline->resolution.productionNanoseconds == 20'000);
    REQUIRE(coldB->placement.pipeline->resolution.productionNanoseconds == 30'000);
    REQUIRE(coldA->residencyProjection == coldB->residencyProjection);

    CachePipeline(pipelineFixture.artifacts, kPipelineStateB);
    const auto mixed = Enumerate(pipelineGraph, pipelineFixture);
    REQUIRE(mixed.has_value());
    REQUIRE(mixed->placements.size() == 2);
    const auto live = std::ranges::find_if(mixed->placements, [](const DispatchPlacementCandidate& candidate) {
        return candidate.placement.pipeline && candidate.placement.pipeline->key == PipelineFor(kPipelineStateB);
    });
    REQUIRE(live != mixed->placements.end());
    REQUIRE(live->placement.pipeline->resolution.mode == ArtifactResolutionMode::Live);
    REQUIRE(live->placement.pipeline->resolution.productionNanoseconds == 0);
}

TEST_CASE("Dispatch geometry represents ranks one through three and rejects "
          "invalid or overflowing domains",
          "[Sora][Compute][Invocation][DispatchGeometry]") {
    const auto rank1 = PlanDispatchGeometry({.rank = 1, .extents = {65}, .workItemCount = 65});
    const auto rank2 = PlanDispatchGeometry({.rank = 2, .extents = {9, 17}, .workItemCount = 153});
    const auto rank3 = PlanDispatchGeometry({.rank = 3, .extents = {5, 9, 17}, .workItemCount = 765});
    REQUIRE(rank1.has_value());
    REQUIRE(rank2.has_value());
    REQUIRE(rank3.has_value());
    REQUIRE(*rank1 == DispatchGeometry{.workgroupCount = {2, 1, 1}});
    REQUIRE(*rank2 == DispatchGeometry{.workgroupCount = {2, 3, 1}});
    REQUIRE(*rank3 == DispatchGeometry{.workgroupCount = {2, 3, 5}});

    REQUIRE(PlanDispatchGeometry({}).error() == DispatchGeometryError::InvalidDomain);
    REQUIRE(PlanDispatchGeometry({.rank = 1, .extents = {0}, .workItemCount = 1}).error() ==
            DispatchGeometryError::InvalidDomain);
    REQUIRE(PlanDispatchGeometry({.rank = 2, .extents = {2, 3}, .workItemCount = 5}).error() ==
            DispatchGeometryError::InvalidDomain);
    constexpr uint64_t overflowingExtent = (uint64_t{UINT32_MAX} + 1) * 64;
    REQUIRE(
        PlanDispatchGeometry({.rank = 1, .extents = {overflowingExtent}, .workItemCount = overflowingExtent}).error() ==
        DispatchGeometryError::CapacityExceeded);
}
