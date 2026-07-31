#include "PlanningTestSupport.h"

#include <Sora/Compute/Runtime/HeftPlanner.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;
    using namespace Sora::Test::ComputePlanning;

    consteval auto MakeOffsetProgram() {
        KernelBuilder<1, 3> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Binary(OpCode::Add, value, scalar.Constant(1.0F));
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<2, 1, 3> program{domain, std::move(scalar).Finish(result),
                                              PartitionContract::Elementwise(1)};
        const auto input = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(AccessMode::Write, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    consteval auto MakeAddProgram() {
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

    consteval auto MakeIndivisibleOffsetProgram() {
        KernelBuilder<1, 3> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Binary(OpCode::Add, value, scalar.Constant(1.0F));
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<2, 1, 3> program{domain, std::move(scalar).Finish(result),
                                              PartitionContract::Indivisible()};
        const auto input = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(AccessMode::Write, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    consteval auto MakeIndivisibleScaleProgram() {
        KernelBuilder<1, 3> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Binary(OpCode::Mul, value, scalar.Constant(2.0F));
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<2, 1, 3> program{domain, std::move(scalar).Finish(result),
                                              PartitionContract::Indivisible()};
        const auto input = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(AccessMode::Write, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    constexpr auto kOffsetProgram = MakeOffsetProgram();
    constexpr auto kAddProgram = MakeAddProgram();
    constexpr auto kReadWriteProgram = MakeReadWriteProgram();
    constexpr auto kIndivisibleOffsetProgram = MakeIndivisibleOffsetProgram();
    constexpr auto kIndivisibleScaleProgram = MakeIndivisibleScaleProgram();

    template<typename Program>
    KernelInvocation Bind(const Program& program, std::vector<KernelArgumentBinding> bindings) {
        constexpr std::array extents{kElementCount};
        auto invocation = BindKernelProgram(program, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    template<typename Program>
    KernelInvocation Bind(const Program& program, Buffer input, Buffer output) {
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(View(std::move(input)));
        bindings.emplace_back(View(std::move(output)));
        return Bind(program, std::move(bindings));
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

    std::expected<HeftPlanningResult, HeftPlanningError>
    DetailedPlan(const ExecutionGraph& graph, PlanningFixture& fixture, HeftPlanningPolicy policy = {},
                 bool twoServices = false,
                 std::shared_ptr<AutomaticPartitionPlanningCalibration> planningCalibration = {}) {
        const auto availability = fixture.artifacts.SnapshotAvailability();
        REQUIRE(availability.has_value());
        const auto performance = fixture.performance.Snapshot();
        const auto packages = FreezeProgramCatalog(fixture.packages);
        if (!twoServices) {
            const std::array devices{fixture.device};
            const std::array snapshots{fixture.snapshot};
            return HeftPlanner{policy, planningCalibration}.Plan(
                graph, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                {.packages = *packages,
                 .resources = fixture.resources.Inventory(),
                 .artifacts = *availability,
                 .performance = *performance});
        }
        const std::array devices{fixture.device, RuntimeDeviceBinding{.service = kServiceB,
                                                                      .descriptor = Descriptor(kGpuB),
                                                                      .target = RuntimeTarget(Target(kGpuB))}};
        const std::array snapshots{fixture.snapshot, DeviceSnapshot{.id = kGpuB,
                                                                    .state = DeviceState::Available,
                                                                    .generation = 1,
                                                                    .availableMemoryBytes = uint64_t{8} << 30U}};
        return HeftPlanner{policy, std::move(planningCalibration)}.Plan(
            graph, {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
            {.packages = *packages,
             .resources = fixture.resources.Inventory(),
             .artifacts = *availability,
             .performance = *performance});
    }

    std::expected<PhysicalGraphPlanStorage, HeftPlanningError> Plan(const ExecutionGraph& graph,
                                                                    PlanningFixture& fixture,
                                                                    HeftPlanningPolicy policy = {},
                                                                    bool twoServices = false) {
        auto result = DetailedPlan(graph, fixture, policy, twoServices);
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::move(result->storage);
    }

    HeftPlanningPolicy SmallPartitionPolicy() {
        HeftPlanningPolicy policy;
        policy.automaticPartition.maximumPartitionEvaluations = 8;
        policy.automaticPartition.minimumAmortizedBenefitNanoseconds = 0;
        policy.automaticPartitionTuning.enabled = false;
        return policy;
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

    PhysicalPlanSegmentId DispatchSegment(const PhysicalGraphPlanStorage& storage, GraphNodeId node) {
        for (uint32_t index = 0; index < storage.segments.size(); ++index) {
            const auto* dispatch = std::get_if<PlannedDispatch>(&storage.segments[index]);
            if (dispatch != nullptr && dispatch->logicalNode == node) {
                return {index};
            }
        }
        return {};
    }

    std::vector<PhysicalPlanSegmentId> DispatchSegments(const PhysicalGraphPlanStorage& storage, GraphNodeId node) {
        std::vector<PhysicalPlanSegmentId> result;
        for (uint32_t index = 0; index < storage.segments.size(); ++index) {
            const auto* dispatch = std::get_if<PlannedDispatch>(&storage.segments[index]);
            if (dispatch != nullptr && dispatch->logicalNode == node) {
                result.push_back({index});
            }
        }
        return result;
    }

    PhysicalPlanSegmentId TransferSegment(const PhysicalGraphPlanStorage& storage, GraphNodeId node,
                                          TransferEngineId engine) {
        for (uint32_t index = 0; index < storage.segments.size(); ++index) {
            const auto* transfer = std::get_if<PlannedTransfer>(&storage.segments[index]);
            if (transfer != nullptr && transfer->logicalNode == node && transfer->engine == engine) {
                return {index};
            }
        }
        return {};
    }

    bool HasEdge(const PhysicalGraphPlanStorage& storage, PhysicalPlanSegmentId predecessor,
                 PhysicalPlanSegmentId successor) {
        return std::ranges::find(storage.edges, PhysicalPlanEdge{predecessor, successor}) != storage.edges.end();
    }

    bool HasPath(const PhysicalGraphPlanStorage& storage, PhysicalPlanSegmentId predecessor,
                 PhysicalPlanSegmentId successor) {
        std::vector<PhysicalPlanSegmentId> pending{predecessor};
        std::vector<uint8_t> visited(storage.segments.size());
        while (!pending.empty()) {
            const PhysicalPlanSegmentId current = pending.back();
            pending.pop_back();
            if (current == successor) {
                return true;
            }
            if (!current.Valid() || current.index >= visited.size() || visited[current.index] != 0) {
                continue;
            }
            visited[current.index] = 1;
            for (const PhysicalPlanEdge edge : storage.edges) {
                if (edge.predecessor == current) {
                    pending.push_back(edge.successor);
                }
            }
        }
        return false;
    }

} // namespace

TEST_CASE("Automatic partition planning calibration is bounded and shape-specific",
          "[Sora][Compute][Runtime][HeftPlanner][Calibration]") {
    AutomaticPartitionPlanningCalibration calibration;
    constexpr AutomaticPartitionPlanningShape computeOnly{.axisCount = 1, .placementCount = 2};
    constexpr AutomaticPartitionPlanningShape twoInputRoutes{.axisCount = 1, .placementCount = 2, .inputRouteCount = 2};
    constexpr AutomaticPartitionPlanningShape sixteenInputRoutes{
        .axisCount = 1, .placementCount = 2, .inputRouteCount = 16};
    REQUIRE_FALSE(calibration.Snapshot().Estimate(computeOnly).has_value());
    REQUIRE_FALSE(calibration.Observe({}));
    REQUIRE(calibration.Observe({.shape = computeOnly, .evaluationCount = 4, .elapsedNanoseconds = 4'000}));
    REQUIRE(calibration.Observe({.shape = computeOnly, .evaluationCount = 2, .elapsedNanoseconds = 4'000}));

    const auto snapshot = calibration.Snapshot();
    const auto estimate = snapshot.Estimate(computeOnly);
    REQUIRE(estimate.has_value());
    REQUIRE(estimate->meanNanosecondsPerEvaluation == 1'536);
    REQUIRE(estimate->meanAbsoluteDeviationNanoseconds == 256);
    REQUIRE(estimate->GuardedNanosecondsPerEvaluation() == 2'048);
    REQUIRE(estimate->sampleCount == 2);
    REQUIRE_FALSE(snapshot.Estimate(twoInputRoutes).has_value());
    REQUIRE(calibration.Observe({.shape = twoInputRoutes, .evaluationCount = 1, .elapsedNanoseconds = 1'000}));
    REQUIRE(calibration.Observe({.shape = sixteenInputRoutes, .evaluationCount = 1, .elapsedNanoseconds = 1'000}));
    REQUIRE(calibration.Snapshot().Estimate(twoInputRoutes).has_value());
    REQUIRE(calibration.Snapshot().Estimate(sixteenInputRoutes).has_value());

    AutomaticPartitionPlanningCalibration concurrent;
    std::array<std::jthread, 4> writers;
    for (std::jthread& writer : writers) {
        writer = std::jthread([&concurrent, computeOnly] {
            for (uint32_t sample = 0; sample < 1'000; ++sample) {
                static_cast<void>(
                    concurrent.Observe({.shape = computeOnly, .evaluationCount = 4, .elapsedNanoseconds = 4'000}));
            }
        });
    }
    for (std::jthread& writer : writers) {
        writer.join();
    }
    const auto concurrentEstimate = concurrent.Snapshot().Estimate(computeOnly);
    REQUIRE(concurrentEstimate.has_value());
    REQUIRE(concurrentEstimate->meanNanosecondsPerEvaluation == 1'024);
    REQUIRE(concurrentEstimate->meanAbsoluteDeviationNanoseconds == 0);
    REQUIRE(concurrentEstimate->sampleCount == 4'000);

    AutomaticPartitionPlanningCalibration saturated;
    bool allSaturatedObservationsAccepted = true;
    for (uint32_t sample = 0; sample < 70'000; ++sample) {
        allSaturatedObservationsAccepted &= saturated.Observe(
            {.shape = computeOnly, .evaluationCount = 1, .elapsedNanoseconds = std::numeric_limits<uint64_t>::max()});
    }
    REQUIRE(allSaturatedObservationsAccepted);
    const auto saturatedEstimate = saturated.Snapshot().Estimate(computeOnly);
    REQUIRE(saturatedEstimate.has_value());
    REQUIRE(saturatedEstimate->meanNanosecondsPerEvaluation == 4'294'967'040ULL);
    REQUIRE(saturatedEstimate->meanAbsoluteDeviationNanoseconds == 0);
    REQUIRE(saturatedEstimate->sampleCount == 65'535);

    constexpr AutomaticPartitionPlanningEstimate overflowingGuard{
        .meanNanosecondsPerEvaluation = std::numeric_limits<uint64_t>::max() - 1U,
        .meanAbsoluteDeviationNanoseconds = std::numeric_limits<uint64_t>::max(),
        .sampleCount = 1};
    STATIC_REQUIRE(overflowingGuard.GuardedNanosecondsPerEvaluation() == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("HEFT resolves cold and calibrated automatic partition budgets from reusable whole cost",
          "[Sora][Compute][Runtime][HeftPlanner][Calibration][Policy]") {
    AutomaticPartitionPolicy policy;
    policy.maximumPartitionEvaluations = 64;
    HeftAutomaticPartitionTuningPolicy tuning;
    constexpr AutomaticPartitionPlanningShape shape{.axisCount = 2, .placementCount = 4, .inputRouteCount = 8};

    const auto cold = ResolveHeftAutomaticPartitionPolicy(policy, tuning, shape, std::nullopt, 120'000, 1);
    REQUIRE(cold.search);
    REQUIRE_FALSE(cold.calibrated);
    REQUIRE(cold.policy.maximumPartitionEvaluations == 8);

    const auto coldRejected = ResolveHeftAutomaticPartitionPolicy(policy, tuning, shape, std::nullopt, 20'000, 1);
    REQUIRE_FALSE(coldRejected.search);

    const auto warm = ResolveHeftAutomaticPartitionPolicy(
        policy, tuning, shape,
        AutomaticPartitionPlanningEstimate{
            .meanNanosecondsPerEvaluation = 10'000, .meanAbsoluteDeviationNanoseconds = 0, .sampleCount = 4},
        120'000, 1);
    REQUIRE(warm.search);
    REQUIRE(warm.calibrated);
    REQUIRE(warm.policy.maximumPartitionEvaluations == 3);

    const auto expensive = ResolveHeftAutomaticPartitionPolicy(
        policy, tuning, shape,
        AutomaticPartitionPlanningEstimate{
            .meanNanosecondsPerEvaluation = 20'000, .meanAbsoluteDeviationNanoseconds = 10'000, .sampleCount = 4},
        120'000, 1);
    REQUIRE_FALSE(expensive.search);

    const auto saturated = ResolveHeftAutomaticPartitionPolicy(
        policy, tuning, shape,
        AutomaticPartitionPlanningEstimate{
            .meanNanosecondsPerEvaluation = 1, .meanAbsoluteDeviationNanoseconds = 0, .sampleCount = 4},
        std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max());
    REQUIRE(saturated.search);
    REQUIRE(saturated.policy.maximumPartitionEvaluations == 64);

    tuning.enabled = false;
    policy.maximumPartitionEvaluations = 7;
    policy.minimumAmortizedBenefitNanoseconds = 100'000;
    const auto fixed = ResolveHeftAutomaticPartitionPolicy(policy, tuning, shape, std::nullopt, 60'000, 2);
    REQUIRE(fixed.search);
    REQUIRE(fixed.policy.maximumPartitionEvaluations == 7);
}

TEST_CASE("HEFT publishes partition-search timing only after the frozen planning attempt",
          "[Sora][Compute][Runtime][HeftPlanner][Calibration][Observation]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 100);
    Buffer input = MakeBuffer({93, 101});
    Buffer output = MakeBuffer({93, 102});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));
    auto calibration = std::make_shared<AutomaticPartitionPlanningCalibration>();
    const HeftPlanningPolicy policy = SmallPartitionPolicy();

    const auto first = DetailedPlan(graph, fixture, policy, true, calibration);
    REQUIRE(first.has_value());
    REQUIRE(first->statistics.partitionSearches == 1);
    REQUIRE(first->statistics.partitionEvaluationBudget == 8);
    REQUIRE(first->statistics.partitionEvaluations != 0);
    constexpr AutomaticPartitionPlanningShape shape{.axisCount = 1, .placementCount = 2, .inputRouteCount = 2};
    const auto estimate = calibration->Snapshot().Estimate(shape);
    REQUIRE(estimate.has_value());
    REQUIRE(estimate->sampleCount == 1);
    REQUIRE(estimate->meanNanosecondsPerEvaluation != 0);
    REQUIRE(PhysicalGraphPlan::Create(first->storage, fixture.resources).has_value());
}

TEST_CASE("HEFT schedules and validates a fork-join DAG in upward-rank order",
          "[Sora][Compute][Runtime][HeftPlanner][DAG][Rank]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture;
    CalibrateTransfer(fixture.performance);
    Buffer input = MakeBuffer({90, 1});
    Buffer shared = MakeBuffer({90, 2});
    Buffer shortOutput = MakeBuffer({90, 3});
    Buffer criticalA = MakeBuffer({90, 4});
    Buffer criticalB = MakeBuffer({90, 5});
    Buffer output = MakeBuffer({90, 6});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());

    GraphBuilder builder;
    const auto root = builder.AddDispatch(Invocation(input, shared));
    const auto shortBranch = builder.AddDispatch(Invocation(shared, shortOutput));
    const auto critical = builder.AddDispatch(Invocation(shared, criticalA));
    const auto criticalTail = builder.AddDispatch(Invocation(criticalA, criticalB));
    const auto join = builder.AddDispatch(Invocation(criticalB, output));
    REQUIRE(root.has_value());
    REQUIRE(shortBranch.has_value());
    REQUIRE(critical.has_value());
    REQUIRE(criticalTail.has_value());
    REQUIRE(join.has_value());
    REQUIRE(builder.AddDependency(*shortBranch, *join).has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture);
    REQUIRE(planned.has_value());
    const auto dispatches = Dispatches(*planned);
    REQUIRE(dispatches.size() == 5);
    const auto criticalPosition =
        std::ranges::find(dispatches, *critical, [](const PlannedDispatch* value) { return value->logicalNode; });
    const auto shortPosition =
        std::ranges::find(dispatches, *shortBranch, [](const PlannedDispatch* value) { return value->logicalNode; });
    REQUIRE(criticalPosition < shortPosition);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT exposes independent dispatches to distinct queues without a "
          "synthetic dependency",
          "[Sora][Compute][Runtime][HeftPlanner][Parallelism]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeAlternativeQueue = true}};
    CalibrateTransfer(fixture.performance);
    Buffer inputA = MakeBuffer({91, 1});
    Buffer outputA = MakeBuffer({91, 2});
    Buffer inputB = MakeBuffer({91, 3});
    Buffer outputB = MakeBuffer({91, 4});
    REQUIRE(inputA.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    REQUIRE(inputB.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto first = builder.AddDispatch(Invocation(inputA, outputA));
    const auto second = builder.AddDispatch(Invocation(inputB, outputB));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture);
    REQUIRE(planned.has_value());
    const auto firstSegment = DispatchSegment(*planned, *first);
    const auto secondSegment = DispatchSegment(*planned, *second);
    REQUIRE(firstSegment.Valid());
    REQUIRE(secondSegment.Valid());
    const auto& firstDispatch = std::get<PlannedDispatch>(planned->segments[firstSegment.index]);
    const auto& secondDispatch = std::get<PlannedDispatch>(planned->segments[secondSegment.index]);
    REQUIRE(firstDispatch.queue.queue != secondDispatch.queue.queue);
    REQUIRE_FALSE(HasEdge(*planned, firstSegment, secondSegment));
    REQUIRE_FALSE(HasEdge(*planned, secondSegment, firstSegment));
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT serializes independent dispatches assigned to the same exact queue",
          "[Sora][Compute][Runtime][HeftPlanner][Queue]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture;
    CalibrateTransfer(fixture.performance);
    Buffer inputA = MakeBuffer({92, 1});
    Buffer outputA = MakeBuffer({92, 2});
    Buffer inputB = MakeBuffer({92, 3});
    Buffer outputB = MakeBuffer({92, 4});
    REQUIRE(inputA.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    REQUIRE(inputB.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto first = builder.AddDispatch(Invocation(inputA, outputA));
    const auto second = builder.AddDispatch(Invocation(inputB, outputB));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture);
    REQUIRE(planned.has_value());
    const auto firstSegment = DispatchSegment(*planned, *first);
    const auto secondSegment = DispatchSegment(*planned, *second);
    REQUIRE(HasEdge(*planned, firstSegment, secondSegment));
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT serializes independent routes on the same exact transfer engine",
          "[Sora][Compute][Runtime][HeftPlanner][TransferCalendar][Contention]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture;
    CalibrateTransfer(fixture.performance);
    Buffer inputA = MakeBuffer({101, 1});
    Buffer outputA = MakeBuffer({101, 2});
    Buffer inputB = MakeBuffer({101, 3});
    Buffer outputB = MakeBuffer({101, 4});
    REQUIRE(inputA.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    REQUIRE(inputB.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto first = builder.AddDispatch(Invocation(inputA, outputA));
    const auto second = builder.AddDispatch(Invocation(inputB, outputB));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture);
    const auto repeated = Plan(*graph, fixture);
    REQUIRE(planned.has_value());
    REQUIRE(repeated.has_value());
    const auto firstTransfer = TransferSegment(*planned, *first, kTransfer);
    const auto secondTransfer = TransferSegment(*planned, *second, kTransfer);
    REQUIRE(firstTransfer.Valid());
    REQUIRE(secondTransfer.Valid());
    REQUIRE(HasEdge(*planned, firstTransfer, secondTransfer));
    REQUIRE(planned->edges == repeated->edges);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
    REQUIRE(PhysicalGraphPlan::Create(*repeated, fixture.resources).has_value());
}

TEST_CASE("Dispatch lowering stably reorders reverse transfer constraints and "
          "rejects missing hops",
          "[Sora][Compute][Runtime][DispatchGraphPlanning][TransferConstraint]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture;
    CalibrateTransfer(fixture.performance);
    Buffer inputA = MakeBuffer({104, 1});
    Buffer outputA = MakeBuffer({104, 2});
    Buffer inputB = MakeBuffer({104, 3});
    Buffer outputB = MakeBuffer({104, 4});
    REQUIRE(inputA.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    REQUIRE(inputB.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto first = builder.AddDispatch(Invocation(inputA, outputA));
    const auto second = builder.AddDispatch(Invocation(inputB, outputB));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto availability = fixture.artifacts.SnapshotAvailability();
    const auto performance = fixture.performance.Snapshot();
    const auto packages = FreezeProgramCatalog(fixture.packages);
    REQUIRE(availability.has_value());
    const std::array devices{fixture.device};
    const std::array snapshots{fixture.snapshot};
    const GraphPlanningContext context{.epochSequence = 1, .devices = devices, .snapshots = snapshots};
    const GraphPlanningServices services{.packages = *packages,
                                         .resources = fixture.resources.Inventory(),
                                         .artifacts = *availability,
                                         .performance = *performance};
    const auto baseline = FreezeGraphResidency(*graph);
    REQUIRE(baseline.has_value());
    GraphResidencyState residency = *baseline;
    std::vector<DispatchGraphSelection> selections;
    for (uint32_t index = 0; index < graph->NodeCount(); ++index) {
        const GraphNodeId node{index};
        auto candidates = EnumerateDispatchCandidates(*graph, node, context, services, residency);
        REQUIRE(candidates.has_value());
        REQUIRE_FALSE(candidates->placements.empty());
        const DispatchPlacementCandidate& candidate = candidates->placements.front();
        REQUIRE(candidate.residencyProjection < candidates->residencies.size());
        const DispatchResidencyProjection& projection = candidates->residencies[candidate.residencyProjection];
        const auto& invocation = std::get<KernelInvocation>(graph->Operation(node));
        selections.push_back(
            {.node = node,
             .workload = candidates->workload,
             .execution = DispatchGraphShardSelection{.slice = DispatchSlice::Whole(invocation.Domain()),
                                                      .placement = candidate.placement,
                                                      .residency = projection,
                                                      .geometry = candidates->geometry}});
        REQUIRE(CommitDispatchResidencySelection(node, projection, residency).has_value());
    }

    const DispatchTransferConstraint reverse{
        .predecessor = {.consumer = *second, .buffer = inputB.Describe().id, .route = 0, .hop = 0},
        .successor = {.consumer = *first, .buffer = inputA.Describe().id, .route = 0, .hop = 0}};
    const std::array reverseConstraints{reverse};
    auto lowered = LowerDispatchGraph(*graph, services, {}, *baseline, selections, reverseConstraints);
    REQUIRE(lowered.has_value());
    const auto secondTransfer = TransferSegment(*lowered, *second, kTransfer);
    const auto firstTransfer = TransferSegment(*lowered, *first, kTransfer);
    REQUIRE(secondTransfer.Valid());
    REQUIRE(firstTransfer.Valid());
    REQUIRE(secondTransfer < firstTransfer);
    REQUIRE(HasEdge(*lowered, secondTransfer, firstTransfer));
    REQUIRE(PhysicalGraphPlan::Create(*lowered, fixture.resources).has_value());

    DispatchTransferConstraint missing = reverse;
    missing.predecessor.hop = 1;
    const std::array missingConstraints{missing};
    const auto rejected = LowerDispatchGraph(*graph, services, {}, *baseline, selections, missingConstraints);
    REQUIRE(rejected.error().code == DispatchGraphPlanningErrorCode::InvalidSelection);
}

TEST_CASE("HEFT preserves parallel transfers on distinct service-scoped engines",
          "[Sora][Compute][Runtime][HeftPlanner][TransferCalendar][Parallelism]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true, .reusePrimaryTransferIdentityOnSecondService = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 10);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 100);
    ObserveTransfer(fixture.performance, kServiceB, kTransfer, kHost, kDeviceB, 100);
    Buffer inputA = MakeBuffer({102, 1});
    Buffer outputA = MakeBuffer({102, 2});
    Buffer inputB = MakeBuffer({102, 3});
    Buffer outputB = MakeBuffer({102, 4});
    REQUIRE(inputA.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    REQUIRE(inputB.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto first = builder.AddDispatch(Invocation(inputA, outputA));
    const auto second = builder.AddDispatch(Bind(kOffsetProgram, inputB, outputB));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture, {}, true);
    REQUIRE(planned.has_value());
    const auto firstTransfer = TransferSegment(*planned, *first, kTransfer);
    const auto secondTransfer = TransferSegment(*planned, *second, kTransfer);
    REQUIRE(firstTransfer.Valid());
    REQUIRE(secondTransfer.Valid());
    REQUIRE_FALSE(HasEdge(*planned, firstTransfer, secondTransfer));
    REQUIRE_FALSE(HasEdge(*planned, secondTransfer, firstTransfer));
    REQUIRE(std::get<PlannedDispatch>(planned->segments[DispatchSegment(*planned, *first).index]).queue.device == kGpu);
    REQUIRE(std::get<PlannedDispatch>(planned->segments[DispatchSegment(*planned, *second).index]).queue.device ==
            kGpuB);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT schedules a multi-hop route one exact engine at a time",
          "[Sora][Compute][Runtime][HeftPlanner][TransferCalendar][MultiHop]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10'000);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 10);
    Buffer input = MakeBuffer({103, 1});
    Buffer output = MakeBuffer({103, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Bind(kOffsetProgram, input, output));

    const auto planned = Plan(graph, fixture, {}, true);
    REQUIRE(planned.has_value());
    const auto firstHop = TransferSegment(*planned, GraphNodeId{0}, kTransfer);
    const auto secondHop = TransferSegment(*planned, GraphNodeId{0}, kPeerTransfer);
    const auto dispatch = DispatchSegment(*planned, GraphNodeId{0});
    REQUIRE(firstHop.Valid());
    REQUIRE(secondHop.Valid());
    REQUIRE(dispatch.Valid());
    REQUIRE(HasEdge(*planned, firstHop, secondHop));
    REQUIRE(HasEdge(*planned, secondHop, dispatch));
    REQUIRE(std::get<PlannedDispatch>(planned->segments[dispatch.index]).queue.device == kGpuB);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT chooses earliest finish across services and exact transfer costs",
          "[Sora][Compute][Runtime][HeftPlanner][EFT]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 100);
    Buffer input = MakeBuffer({93, 1});
    Buffer output = MakeBuffer({93, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));

    const HeftPlanningPolicy policy = SmallPartitionPolicy();
    const auto planned = Plan(graph, fixture, policy, true);
    const auto repeated = Plan(graph, fixture, policy, true);
    REQUIRE(planned.has_value());
    REQUIRE(repeated.has_value());
    const auto dispatches = Dispatches(*planned);
    REQUIRE(dispatches.size() == 2);
    REQUIRE(std::ranges::any_of(dispatches,
                                [](const PlannedDispatch* dispatch) { return dispatch->queue.device == kGpu; }));
    REQUIRE(std::ranges::any_of(dispatches,
                                [](const PlannedDispatch* dispatch) { return dispatch->queue.device == kGpuB; }));
    REQUIRE(planned->edges == repeated->edges);
    REQUIRE(Dispatches(*repeated).size() == 2);

    const auto deviceLossReplan = Plan(graph, fixture, policy, false);
    REQUIRE(deviceLossReplan.has_value());
    REQUIRE(Dispatches(*deviceLossReplan).size() == 1);
    REQUIRE(Dispatches(*deviceLossReplan).front()->queue.device == kGpu);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
    REQUIRE(PhysicalGraphPlan::Create(*deviceLossReplan, fixture.resources).has_value());
}

TEST_CASE("HEFT lowers a partitioned producer into one feasible whole consumer",
          "[Sora][Compute][Runtime][HeftPlanner][Partition][ProducerToWhole]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true, .secondaryQueueSupportsCopy = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 1'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 10);
    Buffer input = MakeBuffer({106, 1});
    Buffer intermediate = MakeBuffer({106, 2});
    Buffer output = MakeBuffer({106, 3});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(input, intermediate));
    const auto consumer = builder.AddDispatch(Bind(kOffsetProgram, intermediate, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture, SmallPartitionPolicy(), true);
    REQUIRE(planned.has_value());
    const auto producerDispatches = DispatchSegments(*planned, *producer);
    const auto consumerDispatches = DispatchSegments(*planned, *consumer);
    REQUIRE(producerDispatches.size() == 2);
    REQUIRE(consumerDispatches.size() == 1);
    REQUIRE(std::get<PlannedDispatch>(planned->segments[consumerDispatches.front().index]).queue.device == kGpuB);
    for (const PhysicalPlanSegmentId shard : producerDispatches) {
        REQUIRE(HasPath(*planned, shard, consumerDispatches.front()));
    }
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT partitions a consumer after one whole producer",
          "[Sora][Compute][Runtime][HeftPlanner][Partition][WholeToConsumer]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 10);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 1'000);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10'000);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 10);
    Buffer input = MakeBuffer({107, 1});
    Buffer intermediate = MakeBuffer({107, 2});
    Buffer output = MakeBuffer({107, 3});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(input, intermediate));
    const auto consumer = builder.AddDispatch(Bind(kOffsetProgram, intermediate, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture, SmallPartitionPolicy(), true);
    REQUIRE(planned.has_value());
    const auto producerDispatches = DispatchSegments(*planned, *producer);
    const auto consumerDispatches = DispatchSegments(*planned, *consumer);
    REQUIRE(producerDispatches.size() == 1);
    REQUIRE(consumerDispatches.size() == 2);
    for (const PhysicalPlanSegmentId shard : consumerDispatches) {
        REQUIRE(HasPath(*planned, producerDispatches.front(), shard));
    }
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT closes partitioned fork-join dependencies over every physical shard",
          "[Sora][Compute][Runtime][HeftPlanner][Partition][ForkJoin]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true, .secondaryQueueSupportsCopy = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    REQUIRE(fixture.packages.Register(kAddProgram).has_value());
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 1'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveKernel(fixture.performance, kAddProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kAddProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 10);
    Buffer input = MakeBuffer({110, 1});
    Buffer shared = MakeBuffer({110, 2});
    Buffer left = MakeBuffer({110, 3});
    Buffer right = MakeBuffer({110, 4});
    Buffer output = MakeBuffer({110, 5});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto root = builder.AddDispatch(Invocation(input, shared));
    const auto leftBranch = builder.AddDispatch(Bind(kOffsetProgram, shared, left));
    const auto rightBranch = builder.AddDispatch(Bind(kOffsetProgram, shared, right));
    std::vector<KernelArgumentBinding> joinBindings;
    joinBindings.emplace_back(View(left));
    joinBindings.emplace_back(View(right));
    joinBindings.emplace_back(View(output));
    const auto join = builder.AddDispatch(Bind(kAddProgram, std::move(joinBindings)));
    REQUIRE(root.has_value());
    REQUIRE(leftBranch.has_value());
    REQUIRE(rightBranch.has_value());
    REQUIRE(join.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture, SmallPartitionPolicy(), true);
    REQUIRE(planned.has_value());
    const auto rootDispatches = DispatchSegments(*planned, *root);
    const auto leftDispatches = DispatchSegments(*planned, *leftBranch);
    const auto rightDispatches = DispatchSegments(*planned, *rightBranch);
    const auto joinDispatches = DispatchSegments(*planned, *join);
    REQUIRE(rootDispatches.size() == 2);
    REQUIRE(leftDispatches.size() == 1);
    REQUIRE(rightDispatches.size() == 1);
    REQUIRE(joinDispatches.size() == 1);
    for (const PhysicalPlanSegmentId rootShard : rootDispatches) {
        REQUIRE(HasPath(*planned, rootShard, leftDispatches.front()));
        REQUIRE(HasPath(*planned, rootShard, rightDispatches.front()));
    }
    REQUIRE(HasPath(*planned, leftDispatches.front(), joinDispatches.front()));
    REQUIRE(HasPath(*planned, rightDispatches.front(), joinDispatches.front()));
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT rejects partitioning when exact peer-transfer contention dominates",
          "[Sora][Compute][Runtime][HeftPlanner][Partition][TransferContention]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kAddProgram).has_value());
    ObserveKernel(fixture.performance, kAddProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kAddProgram.Hash(), Target(kGpuB), kQueueB, 1'000);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 20'000);
    Buffer left = MakeBuffer({108, 1});
    Buffer right = MakeBuffer({108, 2});
    Buffer output = MakeBuffer({108, 3});
    REQUIRE(left.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    REQUIRE(right.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    std::vector<KernelArgumentBinding> bindings;
    bindings.emplace_back(View(left));
    bindings.emplace_back(View(right));
    bindings.emplace_back(View(output));
    const ExecutionGraph graph = MakeGraph(Bind(kAddProgram, std::move(bindings)));

    const auto planned = Plan(graph, fixture, SmallPartitionPolicy(), true);
    REQUIRE(planned.has_value());
    const auto dispatches = Dispatches(*planned);
    REQUIRE(dispatches.size() == 1);
    REQUIRE(dispatches.front()->queue.device == kGpu);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT applies partition overhead before accepting an otherwise faster cover",
          "[Sora][Compute][Runtime][HeftPlanner][Partition][Overhead]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 1'000);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    Buffer input = MakeBuffer({109, 1});
    Buffer output = MakeBuffer({109, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));
    HeftPlanningPolicy policy;
    policy.automaticPartition.maximumPartitionEvaluations = 8;
    policy.automaticPartition.minimumAmortizedBenefitNanoseconds = 0;
    policy.automaticPartitionTuning.enabled = false;
    policy.automaticPartition.partitionOverheadNanoseconds = 10'000;

    const auto planned = Plan(graph, fixture, policy, true);
    REQUIRE(planned.has_value());
    REQUIRE(Dispatches(*planned).size() == 1);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT evaluates partition transfers against reservations committed by earlier nodes",
          "[Sora][Compute][Runtime][HeftPlanner][Partition][CommittedTransfer]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kIndivisibleOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kIndivisibleOffsetProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kIndivisibleOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 1'000);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 880);

    Buffer isolatedInput = MakeBuffer({111, 1});
    Buffer isolatedOutput = MakeBuffer({111, 2});
    REQUIRE(isolatedInput.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    const ExecutionGraph isolated = MakeGraph(Invocation(isolatedInput, isolatedOutput));
    const HeftPlanningPolicy policy = SmallPartitionPolicy();
    const auto isolatedPlan = Plan(isolated, fixture, policy, true);
    REQUIRE(isolatedPlan.has_value());
    REQUIRE(Dispatches(*isolatedPlan).size() == 2);

    Buffer occupierInput = MakeBuffer({111, 3});
    Buffer occupierOutput = MakeBuffer({111, 4});
    Buffer candidateInput = MakeBuffer({111, 5});
    Buffer candidateOutput = MakeBuffer({111, 6});
    REQUIRE(occupierInput.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    REQUIRE(candidateInput.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    GraphBuilder builder;
    const auto occupier = builder.AddDispatch(Bind(kIndivisibleOffsetProgram, occupierInput, occupierOutput));
    const auto candidate = builder.AddDispatch(Invocation(candidateInput, candidateOutput));
    REQUIRE(occupier.has_value());
    REQUIRE(candidate.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture, policy, true);
    REQUIRE(planned.has_value());
    REQUIRE(DispatchSegments(*planned, *occupier).size() == 1);
    REQUIRE(DispatchSegments(*planned, *candidate).size() == 1);
    REQUIRE(std::get<PlannedDispatch>(planned->segments[DispatchSegments(*planned, *candidate).front().index])
                .queue.device == kGpu);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT amortizes partition search against the actual contended whole timeline",
          "[Sora][Compute][Runtime][HeftPlanner][Partition][Amortization]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kIndivisibleOffsetProgram).has_value());
    REQUIRE(fixture.packages.Register(kIndivisibleScaleProgram).has_value());
    ObserveKernel(fixture.performance, kIndivisibleOffsetProgram.Hash(), Target(), kQueue, 100'000);
    ObserveKernel(fixture.performance, kIndivisibleOffsetProgram.Hash(), Target(kGpuB), kQueueB, 200'000);
    ObserveKernel(fixture.performance, kIndivisibleScaleProgram.Hash(), Target(), kQueue, 200'000);
    ObserveKernel(fixture.performance, kIndivisibleScaleProgram.Hash(), Target(kGpuB), kQueueB, 100'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 20'000);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 20'000);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);

    Buffer leftInput = MakeBuffer({112, 1});
    Buffer leftOutput = MakeBuffer({112, 2});
    Buffer rightInput = MakeBuffer({112, 3});
    Buffer rightOutput = MakeBuffer({112, 4});
    Buffer candidateInput = MakeBuffer({112, 5});
    Buffer candidateOutput = MakeBuffer({112, 6});
    REQUIRE(leftInput.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    REQUIRE(rightInput.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    REQUIRE(candidateInput.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());

    GraphBuilder builder;
    const auto left = builder.AddDispatch(Bind(kIndivisibleOffsetProgram, leftInput, leftOutput));
    const auto right = builder.AddDispatch(Bind(kIndivisibleScaleProgram, rightInput, rightOutput));
    const auto candidate = builder.AddDispatch(Invocation(candidateInput, candidateOutput));
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    REQUIRE(candidate.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    HeftPlanningPolicy policy;
    policy.automaticPartition.minimumAmortizedBenefitNanoseconds = 50'000;
    const auto planned = Plan(*graph, fixture, policy, true);
    REQUIRE(planned.has_value());
    REQUIRE(DispatchSegments(*planned, *left).size() == 1);
    REQUIRE(DispatchSegments(*planned, *right).size() == 1);
    REQUIRE(DispatchSegments(*planned, *candidate).size() == 2);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT bounded local search escapes a greedy placement minimum within "
          "its exact budget",
          "[Sora][Compute][Runtime][HeftPlanner][LocalSearch]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 10);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 100);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 1'000);
    Buffer input = MakeBuffer({105, 1});
    Buffer intermediate = MakeBuffer({105, 2});
    Buffer output = MakeBuffer({105, 3});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(input, intermediate));
    const auto consumer = builder.AddDispatch(Bind(kOffsetProgram, intermediate, output));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto baseline = DetailedPlan(*graph, fixture, {}, true);
    REQUIRE(baseline.has_value());
    REQUIRE_FALSE(baseline->statistics.Improved());
    REQUIRE(baseline->statistics.neighborEvaluations == 0);
    REQUIRE(Dispatches(baseline->storage).front()->queue.device == kGpu);

    HeftPlanningPolicy policy;
    policy.localSearch = {.maximumPasses = 1, .maximumNeighborEvaluations = 1};
    const auto optimized = DetailedPlan(*graph, fixture, policy, true);
    const auto repeated = DetailedPlan(*graph, fixture, policy, true);
    REQUIRE(optimized.has_value());
    REQUIRE(repeated.has_value());
    REQUIRE(optimized->statistics.baselineMakespanNanoseconds == baseline->statistics.baselineMakespanNanoseconds);
    REQUIRE(optimized->statistics.Improved());
    REQUIRE(optimized->statistics.neighborEvaluations == 1);
    REQUIRE(optimized->statistics.acceptedMoves == 1);
    REQUIRE(optimized->statistics == repeated->statistics);
    REQUIRE(Dispatches(optimized->storage).front()->queue.device == kGpuB);
    REQUIRE(Dispatches(repeated->storage).front()->queue.device == kGpuB);
    REQUIRE(optimized->storage.edges == repeated->storage.edges);
    REQUIRE(PhysicalGraphPlan::Create(optimized->storage, fixture.resources).has_value());

    policy.localSearch = {.maximumPasses = 2, .maximumNeighborEvaluations = 3};
    const auto secondPass = DetailedPlan(*graph, fixture, policy, true);
    REQUIRE(secondPass.has_value());
    REQUIRE(secondPass->statistics.optimizedMakespanNanoseconds == optimized->statistics.optimizedMakespanNanoseconds);
    REQUIRE(secondPass->statistics.neighborEvaluations == 3);
    REQUIRE(secondPass->statistics.acceptedMoves == 1);
    REQUIRE(Dispatches(secondPass->storage).front()->queue.device == kGpuB);
    REQUIRE(PhysicalGraphPlan::Create(secondPass->storage, fixture.resources).has_value());

    policy.localSearch = {.maximumPasses = 1, .maximumNeighborEvaluations = 1};
    policy.localSearch.minimumImprovementNanoseconds = UINT64_MAX;
    const auto thresholded = DetailedPlan(*graph, fixture, policy, true);
    REQUIRE(thresholded.has_value());
    REQUIRE_FALSE(thresholded->statistics.Improved());
    REQUIRE(thresholded->statistics.neighborEvaluations == 1);
    REQUIRE(thresholded->statistics.acceptedMoves == 0);
    REQUIRE(Dispatches(thresholded->storage).front()->queue.device == kGpu);

    policy.localSearch = {.maximumPasses = 1, .maximumNeighborEvaluations = 0};
    const auto invalid = DetailedPlan(*graph, fixture, policy, true);
    REQUIRE(invalid.error().code == HeftPlanningErrorCode::InvalidPolicy);
}

TEST_CASE("HEFT reuses a replica materialized by an already scheduled "
          "higher-id reader",
          "[Sora][Compute][Runtime][HeftPlanner][Residency]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 10);
    ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
    ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 100);
    Buffer input = MakeBuffer({94, 1});
    Buffer shared = MakeBuffer({94, 2});
    Buffer lowerOutput = MakeBuffer({94, 3});
    Buffer higherOutput = MakeBuffer({94, 4});
    Buffer tailOutput = MakeBuffer({94, 5});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invocation(input, shared));
    const auto lowerReader = builder.AddDispatch(Bind(kOffsetProgram, shared, lowerOutput));
    const auto higherReader = builder.AddDispatch(Bind(kOffsetProgram, shared, higherOutput));
    const auto tail = builder.AddDispatch(Bind(kOffsetProgram, higherOutput, tailOutput));
    REQUIRE(producer.has_value());
    REQUIRE(lowerReader.has_value());
    REQUIRE(higherReader.has_value());
    REQUIRE(tail.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture, {}, true);
    REQUIRE(planned.has_value());
    REQUIRE(*higherReader > *lowerReader);
    const auto dispatches = Dispatches(*planned);
    const auto higherPosition =
        std::ranges::find(dispatches, *higherReader, [](const PlannedDispatch* value) { return value->logicalNode; });
    const auto lowerPosition =
        std::ranges::find(dispatches, *lowerReader, [](const PlannedDispatch* value) { return value->logicalNode; });
    REQUIRE(higherPosition < lowerPosition);
    const auto peerTransfers = std::ranges::count_if(planned->segments, [](const PhysicalPlanSegment& segment) {
        const auto* transfer = std::get_if<PlannedTransfer>(&segment);
        return transfer != nullptr && transfer->engine == kPeerTransfer;
    });
    REQUIRE(peerTransfers == 1);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT reuses a baseline replica materialized by an already scheduled higher-id reader",
          "[Sora][Compute][Runtime][HeftPlanner][Residency][Baseline]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeSecondDevice = true}, false};
    REQUIRE(fixture.packages.Register(kOffsetProgram).has_value());
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(), kQueue, 10'000);
    ObserveKernel(fixture.performance, kOffsetProgram.Hash(), Target(kGpuB), kQueueB, 10);
    ObserveTransfer(fixture.performance, kServiceB, kPeerTransfer, kDevice, kDeviceB, 100);
    Buffer input = MakeBuffer({94, 6});
    Buffer lowerOutput = MakeBuffer({94, 7});
    Buffer higherOutput = MakeBuffer({94, 8});
    Buffer tailOutput = MakeBuffer({94, 9});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    GraphBuilder builder;
    const auto lowerReader = builder.AddDispatch(Bind(kOffsetProgram, input, lowerOutput));
    const auto higherReader = builder.AddDispatch(Bind(kOffsetProgram, input, higherOutput));
    const auto tail = builder.AddDispatch(Bind(kOffsetProgram, higherOutput, tailOutput));
    REQUIRE(lowerReader.has_value());
    REQUIRE(higherReader.has_value());
    REQUIRE(tail.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto planned = Plan(*graph, fixture, {}, true);
    REQUIRE(planned.has_value());
    REQUIRE(*higherReader > *lowerReader);
    const auto dispatches = Dispatches(*planned);
    const auto higherPosition =
        std::ranges::find(dispatches, *higherReader, [](const PlannedDispatch* value) { return value->logicalNode; });
    const auto lowerPosition =
        std::ranges::find(dispatches, *lowerReader, [](const PlannedDispatch* value) { return value->logicalNode; });
    REQUIRE(higherPosition < lowerPosition);
    const auto peerTransfers = std::ranges::count_if(planned->segments, [](const PhysicalPlanSegment& segment) {
        const auto* transfer = std::get_if<PlannedTransfer>(&segment);
        return transfer != nullptr && transfer->engine == kPeerTransfer;
    });
    REQUIRE(peerTransfers == 1);
    REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
}

TEST_CASE("HEFT deterministically resolves equal-cost placements",
          "[Sora][Compute][Runtime][HeftPlanner][Determinism]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture{{.includeAlternativeQueue = true}};
    CalibrateTransfer(fixture.performance);
    Buffer input = MakeBuffer({95, 1});
    Buffer output = MakeBuffer({95, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));

    const auto first = Plan(graph, fixture);
    const auto second = Plan(graph, fixture);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(Dispatches(*first).front()->queue.queue == Dispatches(*second).front()->queue.queue);
    REQUIRE(Dispatches(*first).front()->queue.queue == std::min(kQueue, kAlternativeQueue));
    REQUIRE(first->edges == second->edges);
    REQUIRE(PhysicalGraphPlan::Create(*first, fixture.resources).has_value());
    REQUIRE(PhysicalGraphPlan::Create(*second, fixture.resources).has_value());
}

TEST_CASE("HEFT reports absent routes and runtime-compilation policy rejection",
          "[Sora][Compute][Runtime][HeftPlanner][NoCandidate]") {
    std::atomic<uint32_t> destroyCount = 0;
    PlanningFixture fixture;
    Buffer input = MakeBuffer({96, 1});
    Buffer output = MakeBuffer({96, 2});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
    const ExecutionGraph graph = MakeGraph(Invocation(input, output));

    const auto noRoute = Plan(graph, fixture);
    REQUIRE(noRoute.error().code == HeftPlanningErrorCode::NoCandidate);
    REQUIRE(noRoute.error().node == GraphNodeId{0});
    CalibrateTransfer(fixture.performance);
    HeftPlanningPolicy policy;
    policy.placement.allowRuntimeCompilation = false;
    const auto noCompilation = Plan(graph, fixture, policy);
    REQUIRE(noCompilation.error().code == HeftPlanningErrorCode::NoCandidate);
    REQUIRE(noCompilation.error().node == GraphNodeId{0});
}

TEST_CASE("HEFT rejects non-dispatch operations", "[Sora][Compute][Runtime][HeftPlanner][Validation]") {
    PlanningFixture fixture;
    constexpr auto whole = ByteRegion::Make(0, kBufferBytes);
    STATIC_REQUIRE(whole.has_value());

    SECTION("copy") {
        auto source = BufferSlice::Create(MakeBuffer({97, 1}), *whole);
        auto destination = BufferSlice::Create(MakeBuffer({97, 2}), *whole);
        REQUIRE(source.has_value());
        REQUIRE(destination.has_value());
        auto operation = CopyOperation::Create(std::move(*source), std::move(*destination));
        REQUIRE(operation.has_value());
        GraphBuilder builder;
        REQUIRE(builder.AddCopy(std::move(*operation)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        REQUIRE(Plan(*graph, fixture).error().code == HeftPlanningErrorCode::UnsupportedOperation);
    }
    SECTION("fill") {
        auto destination = BufferSlice::Create(MakeBuffer({97, 3}), *whole);
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
        REQUIRE(Plan(*graph, fixture).error().code == HeftPlanningErrorCode::UnsupportedOperation);
    }
    SECTION("external") {
        std::vector<ExternalResourceBinding> resources;
        resources.push_back({View(MakeBuffer({97, 4})), AccessMode::Read});
        auto operation = ExternalOperation::Create({97, 5}, ExternalPortability::BackendPortable, std::move(resources));
        REQUIRE(operation.has_value());
        GraphBuilder builder;
        REQUIRE(builder.AddExternal(std::move(*operation)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        REQUIRE(Plan(*graph, fixture).error().code == HeftPlanningErrorCode::UnsupportedOperation);
    }
}

TEST_CASE("HEFT accepts graph-local partial bindings and rejects duplicate and read-write bindings",
          "[Sora][Compute][Runtime][HeftPlanner][Validation]") {
    PlanningFixture fixture;
    REQUIRE(fixture.packages.Register(kAddProgram).has_value());
    REQUIRE(fixture.packages.Register(kReadWriteProgram).has_value());

    SECTION("partial") {
        std::atomic<uint32_t> destroyCount = 0;
        constexpr std::array extents{kElementCount / 2};
        Buffer inputBuffer = MakeBuffer({98, 1});
        REQUIRE(inputBuffer.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        CalibrateTransfer(fixture.performance);
        auto input = BufferView::Contiguous(inputBuffer, extents);
        auto output = BufferView::Contiguous(MakeBuffer({98, 2}), extents);
        REQUIRE(input.has_value());
        REQUIRE(output.has_value());
        const AffineByteFootprint outputFootprint = output->Footprint();
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(std::move(*input));
        bindings.emplace_back(std::move(*output));
        auto invocation = BindKernelProgram(kCopyProgram, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        const ExecutionGraph graph = MakeGraph(std::move(*invocation));
        HeftPlanningPolicy policy;
        policy.dispatch.memory.publication = DestinationPublication::GraphLocal;
        const auto planned = Plan(graph, fixture, policy);
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
    SECTION("duplicate") {
        Buffer input = MakeBuffer({98, 3});
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(View(input));
        bindings.emplace_back(View(input));
        bindings.emplace_back(View(MakeBuffer({98, 4})));
        const ExecutionGraph graph = MakeGraph(Bind(kAddProgram, std::move(bindings)));
        const auto planned = Plan(graph, fixture);
        REQUIRE(planned.error().code == HeftPlanningErrorCode::UnsupportedBinding);
        REQUIRE(planned.error().argument == KernelArgumentId{1});
    }
    SECTION("read-write") {
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(View(MakeBuffer({98, 5})));
        const ExecutionGraph graph = MakeGraph(Bind(kReadWriteProgram, std::move(bindings)));
        const auto planned = Plan(graph, fixture);
        REQUIRE(planned.error().code == HeftPlanningErrorCode::UnsupportedBinding);
        REQUIRE(planned.error().argument == KernelArgumentId{0});
    }
}

TEST_CASE("HEFT prices cold executable and pipeline materialization", "[Sora][Compute][Runtime][HeftPlanner][Cost]") {
    std::atomic<uint32_t> destroyCount = 0;
    SECTION("executable") {
        PlanningFixture fixture{{.includeSecondDevice = true}, false};
        ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1, 10'000);
        ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 100, 1);
        ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
        ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
        Buffer input = MakeBuffer({99, 1});
        Buffer output = MakeBuffer({99, 2});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));
        const auto planned = Plan(graph, fixture, {}, true);
        REQUIRE(planned.has_value());
        REQUIRE(Dispatches(*planned).front()->queue.device == kGpuB);
        REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
    }
    SECTION("pipeline") {
        PlanningFixture fixture{
            {.primaryDispatchArtifact = ArtifactKind::DevicePipeline, .includeSecondPipelineConfiguration = true}};
        CalibrateTransfer(fixture.performance);
        CalibratePipeline(fixture.performance, kPipelineStateA, 30'000);
        CalibratePipeline(fixture.performance, kPipelineStateB, 10'000);
        Buffer input = MakeBuffer({99, 3});
        Buffer output = MakeBuffer({99, 4});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));
        const auto planned = Plan(graph, fixture);
        REQUIRE(planned.has_value());
        REQUIRE(Dispatches(*planned).front()->pipeline.has_value());
        REQUIRE(Dispatches(*planned).front()->pipeline->state == kPipelineStateB);
        REQUIRE(PhysicalGraphPlan::Create(*planned, fixture.resources).has_value());
    }
    SECTION("partitioned pipeline amortization") {
        PlanningFixture fixture{{.includeSecondDevice = true,
                                 .primaryDispatchArtifact = ArtifactKind::DevicePipeline,
                                 .secondaryDispatchArtifact = ArtifactKind::DevicePipeline},
                                false};
        ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 10'000);
        ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(kGpuB), kQueueB, 10'000);
        CalibratePipeline(fixture.performance, kPipelineStateA, 1);
        CalibratePipeline(fixture.performance, kPipelineStateA, 30'000, Target(kGpuB));
        ObserveTransfer(fixture.performance, Sora::Test::TestBackendService().Id(), kTransfer, kHost, kDevice, 10);
        ObserveTransfer(fixture.performance, kServiceB, kTransferB, kHost, kDeviceB, 10);
        Buffer input = MakeBuffer({99, 5});
        Buffer output = MakeBuffer({99, 6});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));

        HeftPlanningPolicy policy = SmallPartitionPolicy();
        const auto cold = Plan(graph, fixture, policy, true);
        REQUIRE(cold.has_value());
        const auto coldDispatches = Dispatches(*cold);
        REQUIRE(coldDispatches.size() == 1);
        REQUIRE(coldDispatches.front()->queue.device == kGpu);

        policy.dispatch.expectedReuseCount = 10;
        const auto amortized = Plan(graph, fixture, policy, true);
        REQUIRE(amortized.has_value());
        const auto amortizedDispatches = Dispatches(*amortized);
        REQUIRE(amortizedDispatches.size() == 2);
        REQUIRE(std::ranges::any_of(amortizedDispatches,
                                    [](const PlannedDispatch* dispatch) { return dispatch->queue.device == kGpu; }));
        REQUIRE(std::ranges::any_of(amortizedDispatches,
                                    [](const PlannedDispatch* dispatch) { return dispatch->queue.device == kGpuB; }));
        REQUIRE(std::ranges::all_of(amortizedDispatches,
                                    [](const PlannedDispatch* dispatch) { return dispatch->pipeline.has_value(); }));
        REQUIRE(PhysicalGraphPlan::Create(*cold, fixture.resources).has_value());
        REQUIRE(PhysicalGraphPlan::Create(*amortized, fixture.resources).has_value());
    }
}

TEST_CASE("HEFT rejects rank and schedule time overflow", "[Sora][Compute][Runtime][HeftPlanner][Overflow]") {
    std::atomic<uint32_t> destroyCount = 0;
    SECTION("rank") {
        PlanningFixture fixture{{}, false};
        ObserveKernel(fixture.performance, kCopyProgram.Hash(), Target(), kQueue, 1,
                      std::numeric_limits<uint64_t>::max());
        CalibrateTransfer(fixture.performance);
        Buffer input = MakeBuffer({100, 1});
        Buffer output = MakeBuffer({100, 2});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));
        REQUIRE(Plan(graph, fixture).error().code == HeftPlanningErrorCode::CapacityExceeded);
    }
    SECTION("schedule") {
        PlanningFixture fixture;
        CalibrateTransfer(fixture.performance);
        fixture.snapshot.estimatedQueueDelayNanoseconds = std::numeric_limits<uint64_t>::max();
        Buffer input = MakeBuffer({100, 3});
        Buffer output = MakeBuffer({100, 4});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        const ExecutionGraph graph = MakeGraph(Invocation(input, output));
        REQUIRE(Plan(graph, fixture).error().code == HeftPlanningErrorCode::CapacityExceeded);
    }
}
