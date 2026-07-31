#include "PlanningTestSupport.h"

#include <Sora/Compute/Runtime/LinearChainPlanner.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <ranges>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;
    using namespace Sora::Test::ComputePlanning;

    consteval auto MakeOffsetProgram() {
        KernelBuilder<1, 3> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        const auto one = scalar.Constant(1.0F);
        const auto result = scalar.Binary(OpCode::Add, value, one);
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<2, 1, 3> program{domain, std::move(scalar).Finish(result),
                                              PartitionContract::Elementwise(1)};
        const auto input = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(AccessMode::Write, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    constexpr auto kOffsetProgram = MakeOffsetProgram();

    consteval auto MakeReadWriteProgram() {
        KernelBuilder<1, 1> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<1, 1, 1> program{domain, std::move(scalar).Finish(value),
                                              PartitionContract::Elementwise(1)};
        const auto data = program.AddBufferArgument(AccessMode::ReadWrite, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        program.BindArgument(data);
        return std::move(program).Finish(data);
    }

    constexpr auto kReadWriteProgram = MakeReadWriteProgram();

    consteval auto MakeSumProgram() {
        KernelBuilder<2, 3> scalar;
        const auto left = scalar.AddValueParameter(kFloat);
        const auto right = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Binary(OpCode::Add, left, right);
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<3, 2, 3> program{domain, std::move(scalar).Finish(result),
                                              PartitionContract::Elementwise(1)};
        const auto leftInput = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        const auto rightInput = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(AccessMode::Write, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        program.BindArgument(leftInput);
        program.BindArgument(rightInput);
        return std::move(program).Finish(output);
    }

    constexpr auto kSumProgram = MakeSumProgram();

    template<typename Program>
    KernelInvocation Bind(const Program& program, Buffer input, Buffer output) {
        constexpr std::array extents{kElementCount};
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(View(std::move(input)));
        bindings.emplace_back(View(std::move(output)));
        auto invocation = BindKernelProgram(program, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    template<typename Program>
    KernelInvocation Bind(const Program& program, Buffer left, Buffer right, Buffer output) {
        constexpr std::array extents{kElementCount};
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(View(std::move(left)));
        bindings.emplace_back(View(std::move(right)));
        bindings.emplace_back(View(std::move(output)));
        auto invocation = BindKernelProgram(program, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    void ObserveKernel(PerformanceModel& model, SemanticHash kernel, TargetFingerprint target, QueueId queue,
                       uint64_t computeNanoseconds, uint64_t productionNanoseconds = 1) {
        REQUIRE(model
                    .Update({.key = {.kernel = kernel, .target = target, .specialization = {}, .queue = queue},
                             .workItemCount = kElementCount,
                             .computeNanoseconds = computeNanoseconds,
                             .launchNanoseconds = 1})
                    .has_value());
        const ArtifactKey artifact =
            MakeArtifactKey(ExecutableKey{.lowered = kernel, .target = target, .specialization = {}});
        for (const ArtifactResolutionPath path :
             {ArtifactResolutionPath::PackageImport, ArtifactResolutionPath::PersistentImport,
              ArtifactResolutionPath::PersistentProduction, ArtifactResolutionPath::TransientProduction}) {
            REQUIRE(model.Update({.key = artifact, .path = path, .nanoseconds = productionNanoseconds}).has_value());
        }
    }

    void ObserveTransfer(PerformanceModel& model, BackendServiceId service, TransferEngineId engine,
                         MemoryLocation source, MemoryLocation destination, uint64_t nanoseconds) {
        REQUIRE(nanoseconds > 1);
        REQUIRE(
            model
                .Update({.key = {.service = service, .engine = engine, .source = source, .destination = destination},
                         .bytes = kBufferBytes,
                         .payloadNanoseconds = nanoseconds - 1,
                         .latencyNanoseconds = 1})
                .has_value());
    }

    ExecutionGraph MakeChain(Buffer input, Buffer intermediate, Buffer output) {
        GraphBuilder builder;
        REQUIRE(builder.AddDispatch(Bind(kCopyProgram, std::move(input), intermediate)).has_value());
        REQUIRE(builder.AddDispatch(Bind(kOffsetProgram, std::move(intermediate), std::move(output))).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    ExecutionGraph MakeResidencySensitiveChain(Buffer input, Buffer first, Buffer second, Buffer output) {
        GraphBuilder builder;
        REQUIRE(builder.AddDispatch(Bind(kCopyProgram, input, first)).has_value());
        REQUIRE(builder.AddDispatch(Bind(kOffsetProgram, std::move(first), second)).has_value());
        REQUIRE(
            builder.AddDispatch(Bind(kSumProgram, std::move(input), std::move(second), std::move(output))).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    std::vector<const PlannedDispatch*> Dispatches(const PhysicalGraphPlanStorage& storage) {
        std::vector<const PlannedDispatch*> result;
        for (const PhysicalPlanSegment& segment : storage.segments) {
            if (const auto* dispatch = std::get_if<PlannedDispatch>(&segment)) {
                result.push_back(dispatch);
            }
        }
        return result;
    }

    std::expected<PhysicalGraphPlanStorage, LinearChainPlanningError>
    PlanSingleService(const ExecutionGraph& graph, PlanningFixture& fixture, LinearChainPlanningPolicy policy = {}) {
        const auto availability = fixture.artifacts.SnapshotAvailability();
        REQUIRE(availability.has_value());
        const auto performance = fixture.performance.Snapshot();
        const auto packages = FreezeProgramCatalog(fixture.packages);
        const std::array devices{fixture.device};
        const std::array snapshots{fixture.snapshot};
        return LinearChainPlanner{policy}(graph, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                          {.packages = *packages,
                                           .resources = fixture.resources.Inventory(),
                                           .artifacts = *availability,
                                           .performance = *performance});
    }

    std::expected<PhysicalGraphPlanStorage, LinearChainPlanningError>
    PlanTwoServices(const ExecutionGraph& graph, PlanningFixture& fixture, LinearChainPlanningPolicy policy = {}) {
        const auto availability = fixture.artifacts.SnapshotAvailability();
        REQUIRE(availability.has_value());
        const auto performance = fixture.performance.Snapshot();
        const auto packages = FreezeProgramCatalog(fixture.packages);
        const std::array devices{fixture.device, RuntimeDeviceBinding{.service = kServiceB,
                                                                      .descriptor = Descriptor(kGpuB),
                                                                      .target = RuntimeTarget(Target(kGpuB))}};
        const std::array snapshots{fixture.snapshot, DeviceSnapshot{.id = kGpuB,
                                                                    .state = DeviceState::Available,
                                                                    .generation = 1,
                                                                    .availableMemoryBytes = uint64_t{8} << 30U}};
        return LinearChainPlanner{policy}(graph, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                          {.packages = *packages,
                                           .resources = fixture.resources.Inventory(),
                                           .artifacts = *availability,
                                           .performance = *performance});
    }

} // namespace

TEST_CASE("Linear chain DP selects the global path instead of the greedy first "
          "placement",
          "[Sora][Compute][Runtime][LinearChainPlanner][DP]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());

    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 10);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 20);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 1);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 100);

    Buffer input = MakeBuffer({80, 1});
    Buffer intermediate = MakeBuffer({80, 2});
    Buffer output = MakeBuffer({80, 3});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeChain(input, intermediate, output);

    const auto planned = PlanTwoServices(graph, fixture);
    REQUIRE(planned.has_value());
    const auto dispatches = Dispatches(*planned);
    REQUIRE(dispatches.size() == 2);
    REQUIRE(dispatches[0]->queue.device == kGpuB);
    REQUIRE(dispatches[1]->queue.device == kGpuB);
    REQUIRE(dispatches[0]->queue.queue == kQueueB);
    REQUIRE(dispatches[1]->queue.queue == kQueueB);

    auto validated = PhysicalGraphPlan::Create(*planned, fixture.resources);
    REQUIRE(validated.has_value());
}

TEST_CASE("Linear chain frontier preserves baseline replicas that change a later transfer cost",
          "[Sora][Compute][Runtime][LinearChainPlanner][DP][ResidencyState]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    REQUIRE(fixture.packages.Register(kSumProgram).has_value());

    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 121);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 1);
    ObserveKernel(fixture.performance, kSumProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kSumProgram.Hash(), Target(kGpuB), kQueueB, 1);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 100);

    Buffer input = MakeBuffer({88, 1});
    Buffer first = MakeBuffer({88, 2});
    Buffer second = MakeBuffer({88, 3});
    Buffer output = MakeBuffer({88, 4});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeResidencySensitiveChain(input, first, second, output);

    const auto planned = PlanTwoServices(graph, fixture);
    REQUIRE(planned.has_value());
    const auto dispatches = Dispatches(*planned);
    REQUIRE(dispatches.size() == 3);
    REQUIRE(std::ranges::all_of(dispatches,
                                [](const PlannedDispatch* dispatch) { return dispatch->queue.device == kGpuB; }));
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("Linear chain planner deterministically resolves equal-cost queue "
          "placements",
          "[Sora][Compute][Runtime][LinearChainPlanner][Determinism]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeAlternativeQueue = true}};
    CalibrateTransfer(fixture.performance);
    Buffer input = MakeBuffer({81, 1});
    Buffer output = MakeBuffer({81, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));

    const auto first = PlanSingleService(graph, fixture);
    const auto second = PlanSingleService(graph, fixture);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto firstDispatches = Dispatches(*first);
    const auto secondDispatches = Dispatches(*second);
    REQUIRE(firstDispatches.size() == 1);
    REQUIRE(secondDispatches.size() == 1);
    const QueueId expected = kQueue < kAlternativeQueue ? kQueue : kAlternativeQueue;
    REQUIRE(firstDispatches.front()->queue.queue == expected);
    REQUIRE(secondDispatches.front()->queue.queue == expected);
    REQUIRE(PhysicalGraphPlan::Create(*first, fixture.resources).has_value());
    REQUIRE(PhysicalGraphPlan::Create(*second, fixture.resources).has_value());
}

TEST_CASE("Linear chain planner reports absent placements and "
          "runtime-compilation policy rejection",
          "[Sora][Compute][Runtime][LinearChainPlanner][NoCandidate]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture;
    Buffer input = MakeBuffer({82, 1});
    Buffer output = MakeBuffer({82, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));

    const auto noRoute = PlanSingleService(graph, fixture);
    REQUIRE(noRoute.error().code == LinearChainPlanningErrorCode::NoCandidate);
    REQUIRE(noRoute.error().node == GraphNodeId{0});

    CalibrateTransfer(fixture.performance);
    LinearChainPlanningPolicy policy;
    policy.placement.allowRuntimeCompilation = false;
    const auto noCompilation = PlanSingleService(graph, fixture, policy);
    REQUIRE(noCompilation.error().code == LinearChainPlanningErrorCode::NoCandidate);
    REQUIRE(noCompilation.error().node == GraphNodeId{0});
}

TEST_CASE("Linear chain planner rejects invalid and overflowing exact-frontier capacities",
          "[Sora][Compute][Runtime][LinearChainPlanner][Capacity]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 1);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    Buffer input = MakeBuffer({89, 1});
    Buffer first = MakeBuffer({89, 2});
    Buffer second = MakeBuffer({89, 3});
    Buffer output = MakeBuffer({89, 4});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeResidencySensitiveChain(input, first, second, output);

    LinearChainPlanningPolicy invalid;
    invalid.maximumFrontierStates = 0;
    const auto invalidResult = PlanTwoServices(graph, fixture, invalid);
    REQUIRE_FALSE(invalidResult.has_value());
    REQUIRE(invalidResult.error().code == LinearChainPlanningErrorCode::InvalidPolicy);

    LinearChainPlanningPolicy bounded;
    bounded.maximumFrontierStates = 1;
    const auto overflow = PlanTwoServices(graph, fixture, bounded);
    REQUIRE_FALSE(overflow.has_value());
    REQUIRE(overflow.error().code == LinearChainPlanningErrorCode::CapacityExceeded);
    REQUIRE(overflow.error().node == GraphNodeId{0});
    REQUIRE(overflow.error().cause == 1);
}

TEST_CASE("Linear chain planner rejects non-chain dependency frontiers",
          "[Sora][Compute][Runtime][LinearChainPlanner][Validation]") {
    PlanningFixture fixture;
    GraphBuilder builder;
    REQUIRE(builder.AddDispatch(Invocation(MakeBuffer({83, 1}), MakeBuffer({83, 2}))).has_value());
    REQUIRE(builder.AddDispatch(Invocation(MakeBuffer({83, 3}), MakeBuffer({83, 4}))).has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = PlanSingleService(*graph, fixture);
    REQUIRE(planned.error().code == LinearChainPlanningErrorCode::NotLinearChain);
}

TEST_CASE("Linear chain planner rejects copy, fill, and external operation nodes",
          "[Sora][Compute][Runtime][LinearChainPlanner][Validation]") {
    PlanningFixture fixture;
    constexpr auto whole = ByteRegion::Make(0, kBufferBytes);
    STATIC_REQUIRE(whole.has_value());

    SECTION("copy") {
        auto source = BufferSlice::Create(MakeBuffer({84, 1}), *whole);
        auto destination = BufferSlice::Create(MakeBuffer({84, 2}), *whole);
        REQUIRE(source.has_value());
        REQUIRE(destination.has_value());
        auto operation = CopyOperation::Create(std::move(*source), std::move(*destination));
        REQUIRE(operation.has_value());
        GraphBuilder builder;
        REQUIRE(builder.AddCopy(std::move(*operation)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        REQUIRE(PlanSingleService(*graph, fixture).error().code == LinearChainPlanningErrorCode::UnsupportedOperation);
    }

    SECTION("fill") {
        auto destination = BufferSlice::Create(MakeBuffer({84, 3}), *whole);
        REQUIRE(destination.has_value());
        constexpr std::array bytes{std::byte{0}};
        constexpr auto pattern = FillPattern::Create(bytes);
        STATIC_REQUIRE(pattern.has_value());
        auto operation = FillOperation::Create(std::move(*destination), *pattern);
        REQUIRE(operation.has_value());
        GraphBuilder builder;
        REQUIRE(builder.AddFill(std::move(*operation)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        REQUIRE(PlanSingleService(*graph, fixture).error().code == LinearChainPlanningErrorCode::UnsupportedOperation);
    }

    SECTION("external") {
        std::vector<ExternalResourceBinding> resources;
        resources.push_back({View(MakeBuffer({84, 4})), AccessMode::Read});
        auto operation = ExternalOperation::Create({84, 5}, ExternalPortability::BackendPortable, std::move(resources));
        REQUIRE(operation.has_value());
        GraphBuilder builder;
        REQUIRE(builder.AddExternal(std::move(*operation)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        REQUIRE(PlanSingleService(*graph, fixture).error().code == LinearChainPlanningErrorCode::UnsupportedOperation);
    }
}

TEST_CASE("Linear chain planner accepts graph-local partial bindings and rejects read-write buffer bindings",
          "[Sora][Compute][Runtime][LinearChainPlanner][Validation]") {
    PlanningFixture fixture;

    SECTION("partial") {
        std::atomic<uint32_t> destroyCount = 0;
        constexpr std::array extents{kElementCount / 2};
        Buffer inputBuffer = MakeBuffer({85, 1});
        REQUIRE(inputBuffer.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        CalibrateTransfer(fixture.performance);
        auto input = BufferView::Contiguous(inputBuffer, extents);
        auto output = BufferView::Contiguous(MakeBuffer({85, 2}), extents);
        REQUIRE(input.has_value());
        REQUIRE(output.has_value());
        const AffineByteFootprint outputFootprint = output->Footprint();
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(std::move(*input));
        bindings.emplace_back(std::move(*output));
        auto invocation = BindKernelProgram(kCopyProgram, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        const ExecutionGraph graph = MakeGraph(std::move(*invocation));
        LinearChainPlanningPolicy policy;
        policy.dispatch.memory.publication = DestinationPublication::GraphLocal;
        const auto planned = PlanSingleService(graph, fixture, policy);
        REQUIRE(planned.has_value());
        const auto dispatches = Dispatches(*planned);
        REQUIRE(dispatches.size() == 1);
        const PlannedAllocationId outputAllocation = dispatches.front()->bufferStorage[1].output;
        REQUIRE(outputAllocation.Valid());
        const PlannedAllocation& outputState = planned->allocations[outputAllocation.index];
        const ByteCoverageView expectedCoverage = ByteCoverageView::Affine(outputFootprint);
        REQUIRE(outputState.Coverage().Contains(expectedCoverage) == CoverageContainment::Yes);
        REQUIRE(expectedCoverage.Contains(outputState.Coverage()) == CoverageContainment::Yes);
        REQUIRE_FALSE(outputState.publication.has_value());
        REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
    }

    SECTION("read-write") {
        REQUIRE(fixture.packages.Register(kReadWriteProgram).has_value());
        constexpr std::array extents{kElementCount};
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(View(MakeBuffer({85, 3})));
        auto invocation = BindKernelProgram(kReadWriteProgram, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        const ExecutionGraph graph = MakeGraph(std::move(*invocation));
        const auto planned = PlanSingleService(graph, fixture);
        REQUIRE(planned.error().code == LinearChainPlanningErrorCode::UnsupportedBinding);
        REQUIRE(planned.error().argument == KernelArgumentId{0});
    }
}

TEST_CASE("Linear chain planner prices cold executable materialization in "
          "device selection",
          "[Sora][Compute][Runtime][LinearChainPlanner][Cost]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1, 10'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 100, 1);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    Buffer input = MakeBuffer({86, 1});
    Buffer output = MakeBuffer({86, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));

    const auto planned = PlanTwoServices(graph, fixture);
    REQUIRE(planned.has_value());
    const auto dispatches = Dispatches(*planned);
    REQUIRE(dispatches.size() == 1);
    REQUIRE(dispatches.front()->queue.device == kGpuB);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("Linear chain planner prices cold pipeline materialization in "
          "configuration selection",
          "[Sora][Compute][Runtime][LinearChainPlanner][Cost][Pipeline]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{
        {.primaryDispatchArtifact = ArtifactKind::DevicePipeline, .includeSecondPipelineConfiguration = true}};
    CalibrateTransfer(fixture.performance);
    CalibratePipeline(fixture.performance, kPipelineStateA, 30'000);
    CalibratePipeline(fixture.performance, kPipelineStateB, 10'000);
    Buffer input = MakeBuffer({87, 1});
    Buffer output = MakeBuffer({87, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));

    const auto planned = PlanSingleService(graph, fixture);
    REQUIRE(planned.has_value());
    const auto dispatches = Dispatches(*planned);
    REQUIRE(dispatches.size() == 1);
    REQUIRE(dispatches.front()->pipeline.has_value());
    REQUIRE(dispatches.front()->pipeline->state == kPipelineStateB);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}
