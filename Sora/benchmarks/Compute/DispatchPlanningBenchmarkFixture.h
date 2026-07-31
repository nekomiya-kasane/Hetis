/**
 * @file DispatchPlanningBenchmarkFixture.h
 * @brief Shared deterministic fixture for dispatch-graph planning benchmarks.
 * @ingroup Compute
 */

#pragma once

#include <BenchmarkHarness.h>
#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/AutomaticDispatchPlanning.h>
#include <Sora/Compute/Runtime/AutomaticPartitionPlanningCalibration.h>
#include <Sora/Compute/Runtime/DispatchGraphPlanning.h>
#include <Sora/Compute/Runtime/GraphPlanner.h>
#include <Sora/Compute/Runtime/PhysicalGraphPlan.h>
#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <utility>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr uint64_t kElementCount = 1024;
    constexpr uint64_t kBufferBytes = kElementCount * sizeof(float);
    constexpr Sora::Compute::BackendServiceId kServiceA{100, 101};
    constexpr Sora::Compute::BackendServiceId kServiceB{102, 103};
    constexpr Sora::Compute::DeviceId kHostDevice{1, 1};
    constexpr Sora::Compute::DeviceId kDeviceA{2, 1};
    constexpr Sora::Compute::DeviceId kDeviceB{2, 2};
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {10, 1}, .device = kHostDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kMemoryA{
        .domain = {20, 1}, .device = kDeviceA, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::MemoryLocation kMemoryB{
        .domain = {20, 2}, .device = kDeviceB, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::QueueId kQueueA{30, 1};
    constexpr Sora::Compute::QueueId kQueueB{30, 2};
    constexpr Sora::Compute::TransferEngineId kHostToA{40, 1};
    constexpr Sora::Compute::TransferEngineId kHostToB{40, 2};
    constexpr Sora::Compute::TransferEngineId kAToB{40, 3};
    constexpr Sora::Compute::TransferEngineId kBToA{40, 4};
    constexpr Sora::Compute::SemanticHash kCompiler{50, 1};
    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

    enum class DispatchPlanningGraph : uint8_t {
        Chain,
        ResidencySensitiveChain,
        IndependentSharedTransfer,
        IndependentDistinctTransfers,
    };

    [[nodiscard]] constexpr Sora::Compute::MemoryLocation IndependentHost(uint32_t index) noexcept {
        return {.domain = {10, static_cast<uint64_t>(index) + 2},
                .device = kHostDevice,
                .backend = Sora::Compute::BackendKind::Cpu};
    }

    [[nodiscard]] constexpr Sora::Compute::TransferEngineId IndependentTransfer(uint32_t index) noexcept {
        return {40, static_cast<uint64_t>(index) + 100};
    }

    consteval auto MakeCopyProgram() {
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<2, 1, 1> program{domain, std::move(scalar).Finish(value),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto input = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                     Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    constexpr auto kCopyProgram = MakeCopyProgram();

    consteval auto MakeSumProgram() {
        Sora::Compute::KernelBuilder<2, 3> scalar;
        const auto left = scalar.AddValueParameter(kFloat);
        const auto right = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Binary(Sora::Compute::OpCode::Add, left, right);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<3, 2, 3> program{domain, std::move(scalar).Finish(result),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto leftInput = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto rightInput = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                          Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {1});
        program.BindArgument(leftInput);
        program.BindArgument(rightInput);
        return std::move(program).Finish(output);
    }

    constexpr auto kSumProgram = MakeSumProgram();

    void DestroyOpaque(void* state) noexcept {
        delete static_cast<uint32_t*>(state);
    }

    void Submit(void*, const Sora::Compute::BackendSubmitRequest*, Sora::Compute::BackendSubmitResult*) noexcept {}

    void Allocate(void*, const Sora::Compute::BackendAllocationRequest*,
                  Sora::Compute::BackendAllocationResult*) noexcept {}

    void Transfer(void*, const Sora::Compute::BackendTransferRequest*, Sora::Compute::BackendTransferResult*) noexcept {
    }

    void Compile(void*, const Sora::Compute::BackendCompileRequest*, Sora::Compute::BackendCompileResult*) noexcept {}

    void ObserveA(void*, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        *snapshot = {.id = kDeviceA,
                     .state = Sora::Compute::DeviceState::Available,
                     .generation = 1,
                     .availableMemoryBytes = uint64_t{8} << 30U};
    }

    void ObserveB(void*, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        *snapshot = {.id = kDeviceB,
                     .state = Sora::Compute::DeviceState::Available,
                     .generation = 1,
                     .availableMemoryBytes = uint64_t{8} << 30U};
    }

    [[nodiscard]] constexpr Sora::Compute::DeviceDescriptor DeviceDescriptor(Sora::Compute::DeviceId device) noexcept {
        return {.id = device,
                .backend = Sora::Compute::BackendKind::Vulkan,
                .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                .architectureId = device == kDeviceA ? 61ULL : 62ULL,
                .totalMemoryBytes = uint64_t{8} << 30U,
                .maximumAllocationBytes = uint64_t{2} << 30U,
                .subgroupWidthMinimum = 1,
                .subgroupWidthMaximum = 32,
                .maximumWorkgroupInvocations = 1024};
    }

    [[nodiscard]] constexpr Sora::Compute::TargetFingerprint Target(Sora::Compute::DeviceId device) noexcept {
        return {.device = device,
                .backend = Sora::Compute::BackendKind::Vulkan,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                .addressBits = 64,
                .architectureId = device == kDeviceA ? 61ULL : 62ULL,
                .compilerIdentity = kCompiler,
                .driverIdentity =
                    device == kDeviceA ? Sora::Compute::SemanticHash{63, 1} : Sora::Compute::SemanticHash{63, 2}};
    }

    class DispatchPlanningBenchmarkFixture final {
    public:
        DispatchPlanningBenchmarkFixture(uint32_t nodeCount, uint32_t destinationCount,
                                         DispatchPlanningGraph graph = DispatchPlanningGraph::Chain)
            : artifacts_(1), nodeCount_(nodeCount), destinationCount_(destinationCount), graphKind_(graph) {
            if (nodeCount == 0 || destinationCount == 0 || destinationCount > 2 || !CreateServices() ||
                !CreateCatalog() || !PopulatePerformance() || !CreateGraph() || !FreezePrograms()) {
                failed_ = true;
                return;
            }
            availability_ = artifacts_.SnapshotAvailability();
            performanceSnapshot_ = performance_.Snapshot();
            if (!availability_ || performanceSnapshot_ == nullptr) {
                failed_ = true;
                return;
            }
            devices_.push_back({.service = kServiceA,
                                .descriptor = DeviceDescriptor(kDeviceA),
                                .target = RuntimeTarget(Target(kDeviceA))});
            snapshots_.push_back({.id = kDeviceA,
                                  .state = Sora::Compute::DeviceState::Available,
                                  .generation = 1,
                                  .availableMemoryBytes = uint64_t{8} << 30U});
            if (destinationCount_ == 2) {
                devices_.push_back({.service = kServiceB,
                                    .descriptor = DeviceDescriptor(kDeviceB),
                                    .target = RuntimeTarget(Target(kDeviceB))});
                snapshots_.push_back({.id = kDeviceB,
                                      .state = Sora::Compute::DeviceState::Available,
                                      .generation = 1,
                                      .availableMemoryBytes = uint64_t{8} << 30U});
            }
        }

        template<typename Planner>
        [[nodiscard]] bool Validate(const Planner& planner) noexcept {
            const auto planned = Plan(planner);
            if (!planned || !Sora::Compute::PhysicalGraphPlan::Create(planned->storage, *catalog_)) {
                failed_ = true;
            }
            return !failed_;
        }

        template<typename Planner>
        [[nodiscard]] bool ValidateLocalSearch(const Planner& planner, uint32_t expectedEvaluations) noexcept {
            const auto planned = DetailedPlan(planner);
            if (!planned || planned->statistics.neighborEvaluations != expectedEvaluations ||
                !Sora::Compute::PhysicalGraphPlan::Create(planned->selection.storage, *catalog_)) {
                failed_ = true;
            }
            return !failed_;
        }

        template<typename Planner>
        [[nodiscard]] bool ValidateAutomaticTuning(const Planner& planner) noexcept {
            const auto planned = DetailedPlan(planner);
            if (!planned || planned->statistics.partitionSearches == 0 ||
                planned->statistics.partitionEvaluationBudget == 0 ||
                !Sora::Compute::PhysicalGraphPlan::Create(planned->selection.storage, *catalog_)) {
                failed_ = true;
            } else {
                automaticPartitionBudget_ = planned->statistics.partitionEvaluationBudget;
                automaticPartitionEvaluations_ = planned->statistics.partitionEvaluations;
            }
            return !failed_;
        }

        template<typename Planner>
        [[nodiscard]] bool ValidateDeferredAutomaticTuning(
            const Planner& planner, Sora::Compute::AutomaticPartitionPlanningCalibrationBatch& calibration) noexcept {
            const auto planned = DetailedPlan(planner, calibration);
            if (!planned || planned->statistics.partitionSearches == 0 ||
                planned->statistics.partitionEvaluationBudget == 0 || calibration.Observations().empty() ||
                !Sora::Compute::PhysicalGraphPlan::Create(planned->selection.storage, *catalog_)) {
                failed_ = true;
            } else {
                automaticPartitionBudget_ = planned->statistics.partitionEvaluationBudget;
                automaticPartitionEvaluations_ = planned->statistics.partitionEvaluations;
            }
            calibration.Discard();
            return !failed_;
        }

        template<typename Planner>
        void Invoke(const Planner& planner) noexcept {
            const auto planned = Plan(planner);
            if (!planned) {
                failed_ = true;
                return;
            }
            checksum_ += planned->storage.allocations.size() + planned->storage.segments.size() +
                         planned->storage.edges.size() + planned->predictedMakespanNanoseconds + 1;
        }

        template<typename Planner>
        void InvokeDeferred(const Planner& planner,
                            Sora::Compute::AutomaticPartitionPlanningCalibrationBatch& calibration) noexcept {
            const auto planned = DetailedPlan(planner, calibration);
            if (!planned) {
                failed_ = true;
                calibration.Discard();
                return;
            }
            checksum_ += planned->selection.storage.allocations.size() + planned->selection.storage.segments.size() +
                         planned->selection.storage.edges.size() + planned->selection.predictedMakespanNanoseconds + 1;
            calibration.Discard();
        }

        void InvokeAutomaticPartition(const Sora::Compute::AutomaticPartitionPolicy& policy) noexcept {
            if (nodeCount_ != 1) {
                failed_ = true;
                return;
            }
            auto residency = Sora::Compute::FreezeGraphResidency(*graph_);
            Sora::Compute::DispatchEnumerationPolicy enumerationPolicy;
            enumerationPolicy.memory.publication = Sora::Compute::DestinationPublication::Publish;
            const Sora::Compute::GraphPlanningServices services{.packages = *programCatalog_,
                                                                .resources = catalog_->Inventory(),
                                                                .artifacts = *availability_,
                                                                .performance = *performanceSnapshot_};
            if (!residency) {
                failed_ = true;
                return;
            }
            auto candidates = Sora::Compute::EnumerateDispatchCandidates(
                *graph_, {0}, {.epochSequence = 1, .devices = devices_, .snapshots = snapshots_}, services, *residency,
                enumerationPolicy);
            if (!candidates) {
                failed_ = true;
                return;
            }
            const auto& invocation = std::get<Sora::Compute::KernelInvocation>(graph_->Operation({0}));
            auto planned = Sora::Compute::PlanAutomaticDispatch(invocation, *candidates, services, *residency, policy);
            if (!planned) {
                failed_ = true;
                return;
            }
            checksum_ += planned->SelectedNanoseconds() +
                         (planned->partitioned ? planned->partitioned->partitionEvaluations : 0) + 1;
        }

        [[nodiscard]] bool PrepareMixedDagLowering() noexcept {
            if (nodeCount_ != 2 || destinationCount_ != 2) {
                failed_ = true;
                return false;
            }
            try {
                auto baseline = Sora::Compute::FreezeGraphResidency(*graph_);
                if (!baseline) {
                    failed_ = true;
                    return false;
                }
                const Sora::Compute::GraphPlanningServices services{.packages = *programCatalog_,
                                                                    .resources = catalog_->Inventory(),
                                                                    .artifacts = *availability_,
                                                                    .performance = *performanceSnapshot_};
                const Sora::Compute::GraphPlanningContext context{
                    .epochSequence = 1, .devices = devices_, .snapshots = snapshots_};
                Sora::Compute::GraphResidencyState residency = *baseline;
                auto producer = Sora::Compute::EnumerateDispatchCandidates(*graph_, {0}, context, services, residency);
                if (!producer) {
                    failed_ = true;
                    return false;
                }
                const auto& invocation = std::get<Sora::Compute::KernelInvocation>(graph_->Operation({0}));
                auto partition = Sora::Compute::ElementwisePartitionPlan::Balanced(invocation.Domain(),
                                                                                   invocation.Partition(), 0, 2);
                if (!partition) {
                    failed_ = true;
                    return false;
                }
                Sora::Compute::DispatchGraphPartitionSelection partitionSelection{.axis = 0};
                std::vector<Sora::Compute::DispatchResidencyProjection> projections;
                projections.reserve(2);
                for (uint32_t index = 0; index < 2; ++index) {
                    const Sora::Compute::DeviceId device = index == 0 ? kDeviceA : kDeviceB;
                    const auto placement = std::ranges::find_if(producer->placements, [&](const auto& candidate) {
                        return candidate.placement.device.id == device;
                    });
                    if (placement == producer->placements.end() ||
                        placement->residencyProjection >= producer->residencies.size()) {
                        failed_ = true;
                        return false;
                    }
                    const auto& whole = producer->residencies[placement->residencyProjection];
                    if (partitionSelection.logicalBindings.empty()) {
                        partitionSelection.logicalBindings = whole.bindings;
                    }
                    auto projection = Sora::Compute::ProjectDispatchSliceResidency(
                        invocation, {0}, partition->Shards()[index], whole, services, residency);
                    auto geometry = Sora::Compute::PlanDispatchSliceGeometry(partition->Shards()[index]);
                    if (!projection || !geometry) {
                        failed_ = true;
                        return false;
                    }
                    projections.push_back(*projection);
                    partitionSelection.shards.push_back({.slice = partition->Shards()[index],
                                                         .placement = placement->placement,
                                                         .residency = std::move(*projection),
                                                         .geometry = *geometry});
                }
                if (!Sora::Compute::CommitPartitionedDispatchResidencySelection({0}, partitionSelection.logicalBindings,
                                                                                projections, residency)) {
                    failed_ = true;
                    return false;
                }
                loweringSelections_.push_back(
                    {.node = {0}, .workload = producer->workload, .execution = std::move(partitionSelection)});

                auto consumer = Sora::Compute::EnumerateDispatchCandidates(*graph_, {1}, context, services, residency);
                if (!consumer) {
                    failed_ = true;
                    return false;
                }
                const auto placement = std::ranges::find_if(consumer->placements, [](const auto& candidate) {
                    return candidate.placement.device.id == kDeviceB;
                });
                if (placement == consumer->placements.end() ||
                    placement->residencyProjection >= consumer->residencies.size()) {
                    failed_ = true;
                    return false;
                }
                const auto& projection = consumer->residencies[placement->residencyProjection];
                if (!Sora::Compute::CommitDispatchResidencySelection({1}, projection, residency)) {
                    failed_ = true;
                    return false;
                }
                const auto& consumerInvocation = std::get<Sora::Compute::KernelInvocation>(graph_->Operation({1}));
                loweringSelections_.push_back(
                    {.node = {1},
                     .workload = consumer->workload,
                     .execution = Sora::Compute::DispatchGraphShardSelection{
                         .slice = Sora::Compute::DispatchSlice::Whole(consumerInvocation.Domain()),
                         .placement = placement->placement,
                         .residency = projection,
                         .geometry = consumer->geometry}});
                loweringBaseline_ = std::move(*baseline);
                const auto lowered =
                    Sora::Compute::LowerDispatchGraph(*graph_, services, {}, *loweringBaseline_, loweringSelections_);
                if (!lowered || !Sora::Compute::PhysicalGraphPlan::Create(*lowered, *catalog_)) {
                    failed_ = true;
                }
                return !failed_;
            } catch (const std::bad_alloc&) {
                failed_ = true;
                return false;
            }
        }

        void InvokeMixedDagLowering() noexcept {
            if (!loweringBaseline_ || loweringSelections_.size() != 2) {
                failed_ = true;
                return;
            }
            const Sora::Compute::GraphPlanningServices services{.packages = *programCatalog_,
                                                                .resources = catalog_->Inventory(),
                                                                .artifacts = *availability_,
                                                                .performance = *performanceSnapshot_};
            const auto lowered =
                Sora::Compute::LowerDispatchGraph(*graph_, services, {}, *loweringBaseline_, loweringSelections_);
            if (!lowered) {
                failed_ = true;
                return;
            }
            checksum_ += lowered->allocations.size() + lowered->segments.size() + lowered->edges.size() + 1;
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }
        [[nodiscard]] uint64_t AutomaticPartitionBudget() const noexcept { return automaticPartitionBudget_; }
        [[nodiscard]] uint64_t AutomaticPartitionEvaluations() const noexcept { return automaticPartitionEvaluations_; }

    private:
        [[nodiscard]] bool CreateServices() noexcept {
            auto codeA = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
            auto codeB = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
            auto serviceA = Sora::Compute::BackendService::Adopt(
                {.id = kServiceA, .state = new uint32_t{}, .destroy = DestroyOpaque}, std::move(codeA));
            auto serviceB = Sora::Compute::BackendService::Adopt(
                {.id = kServiceB, .state = new uint32_t{}, .destroy = DestroyOpaque}, std::move(codeB));
            if (!serviceA || !serviceB) {
                return false;
            }
            serviceA_ = std::move(*serviceA);
            serviceB_ = std::move(*serviceB);
            return true;
        }

        [[nodiscard]] bool CreateCatalog() noexcept {
            auto observerA =
                Sora::Compute::DeviceObserver::Create(serviceA_, {.device = kDeviceA,
                                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                                  .state = serviceA_.State(),
                                                                  .observe = ObserveA});
            auto observerB =
                Sora::Compute::DeviceObserver::Create(serviceB_, {.device = kDeviceB,
                                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                                  .state = serviceB_.State(),
                                                                  .observe = ObserveB});
            auto queueA = Sora::Compute::Queue::Adopt(
                serviceA_,
                {.id = kQueueA,
                 .device = kDeviceA,
                 .backend = Sora::Compute::BackendKind::Vulkan,
                 .capabilities = Sora::Compute::QueueCapability::Dispatch | Sora::Compute::QueueCapability::Copy,
                 .dispatchArtifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
                 .state = new uint32_t{},
                 .destroy = DestroyOpaque,
                 .submit = Submit});
            auto queueB = Sora::Compute::Queue::Adopt(
                serviceB_,
                {.id = kQueueB,
                 .device = kDeviceB,
                 .backend = Sora::Compute::BackendKind::Vulkan,
                 .capabilities = Sora::Compute::QueueCapability::Dispatch | Sora::Compute::QueueCapability::Copy,
                 .dispatchArtifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
                 .state = new uint32_t{},
                 .destroy = DestroyOpaque,
                 .submit = Submit});
            auto allocatorA = Sora::Compute::MemoryAllocator::Create(
                serviceA_, {.location = kMemoryA,
                            .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                            .state = serviceA_.State(),
                            .allocate = Allocate});
            auto allocatorB = Sora::Compute::MemoryAllocator::Create(
                serviceB_, {.location = kMemoryB,
                            .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                            .state = serviceB_.State(),
                            .allocate = Allocate});
            auto compilerA =
                Sora::Compute::BackendCompiler::Create(serviceA_, {.identity = kCompiler,
                                                                   .backend = Sora::Compute::BackendKind::Vulkan,
                                                                   .state = serviceA_.State(),
                                                                   .compile = Compile});
            auto compilerB =
                Sora::Compute::BackendCompiler::Create(serviceB_, {.identity = kCompiler,
                                                                   .backend = Sora::Compute::BackendKind::Vulkan,
                                                                   .state = serviceB_.State(),
                                                                   .compile = Compile});
            auto hostToA = MakeTransfer(serviceA_, kHostToA, kServiceA, kHost, kMemoryA, kDeviceA);
            auto hostToB = MakeTransfer(serviceB_, kHostToB, kServiceA, kHost, kMemoryB, kDeviceB);
            auto aToB = MakeTransfer(serviceB_, kAToB, kServiceA, kMemoryA, kMemoryB, kDeviceB);
            auto bToA = MakeTransfer(serviceA_, kBToA, kServiceB, kMemoryB, kMemoryA, kDeviceA);
            if (!observerA || !observerB || !queueA || !queueB || !allocatorA || !allocatorB || !compilerA ||
                !compilerB || !hostToA || !hostToB || !aToB || !bToA) {
                return false;
            }
            Sora::Compute::PhysicalResourceCatalogStorage storage{
                .deviceObservers = {std::move(*observerA), std::move(*observerB)},
                .queues = {std::move(*queueA), std::move(*queueB)},
                .allocators = {std::move(*allocatorA), std::move(*allocatorB)},
                .transferEngines = {std::move(*hostToA), std::move(*hostToB), std::move(*aToB), std::move(*bToA)},
                .compilers = {std::move(*compilerA), std::move(*compilerB)}};
            if (graphKind_ == DispatchPlanningGraph::IndependentDistinctTransfers) {
                for (uint32_t index = 0; index < nodeCount_; ++index) {
                    auto transfer = MakeTransfer(serviceA_, IndependentTransfer(index), kServiceA,
                                                 IndependentHost(index), kMemoryA, kDeviceA);
                    if (!transfer) {
                        return false;
                    }
                    storage.transferEngines.push_back(std::move(*transfer));
                }
            }
            auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(std::move(storage));
            if (!catalog) {
                return false;
            }
            catalog_.emplace(std::move(*catalog));
            return true;
        }

        [[nodiscard]] static std::expected<Sora::Compute::TransferEngine, Sora::Compute::TransferError>
        MakeTransfer(const Sora::Compute::BackendService& service, Sora::Compute::TransferEngineId engine,
                     Sora::Compute::BackendServiceId sourceService, Sora::Compute::MemoryLocation source,
                     Sora::Compute::MemoryLocation destination, Sora::Compute::DeviceId completionDevice) noexcept {
            return Sora::Compute::TransferEngine::Create(service,
                                                         {.engine = engine,
                                                          .sourceService = sourceService,
                                                          .source = source,
                                                          .destination = destination,
                                                          .kind = Sora::Compute::TransferKind::Copy,
                                                          .completionDevice = completionDevice,
                                                          .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                          .state = service.State(),
                                                          .transfer = Transfer});
        }

        [[nodiscard]] bool PopulatePerformance() noexcept {
            if (!ObserveKernel(kCopyProgram.Hash(), kDeviceA, kQueueA, 500) ||
                !ObserveKernel(kCopyProgram.Hash(), kDeviceB, kQueueB, 450)) {
                return false;
            }
            if (graphKind_ == DispatchPlanningGraph::ResidencySensitiveChain &&
                (!ObserveKernel(kSumProgram.Hash(), kDeviceA, kQueueA, 500) ||
                 !ObserveKernel(kSumProgram.Hash(), kDeviceB, kQueueB, 450))) {
                return false;
            }
            if (!ObserveTransfer(kServiceA, kHostToA, kHost, kMemoryA, 1000) ||
                !ObserveTransfer(kServiceB, kHostToB, kHost, kMemoryB, 1000) ||
                !ObserveTransfer(kServiceB, kAToB, kMemoryA, kMemoryB, 1600) ||
                !ObserveTransfer(kServiceA, kBToA, kMemoryB, kMemoryA, 1600)) {
                return false;
            }
            if (graphKind_ == DispatchPlanningGraph::IndependentDistinctTransfers) {
                for (uint32_t index = 0; index < nodeCount_; ++index) {
                    if (!ObserveTransfer(kServiceA, IndependentTransfer(index), IndependentHost(index), kMemoryA,
                                         1000)) {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool ObserveKernel(Sora::Compute::SemanticHash kernel, Sora::Compute::DeviceId device,
                                         Sora::Compute::QueueId queue, uint64_t computeNanoseconds) noexcept {
            const auto target = Target(device);
            if (!performance_.Update({.key = {.kernel = kernel, .target = target, .specialization = {}, .queue = queue},
                                      .workItemCount = kElementCount,
                                      .computeNanoseconds = computeNanoseconds,
                                      .launchNanoseconds = 100})) {
                return false;
            }
            const auto artifact = Sora::Compute::MakeArtifactKey(
                Sora::Compute::ExecutableKey{.lowered = kernel, .target = target, .specialization = {}});
            return performance_
                .Update({.key = artifact,
                         .path = Sora::Compute::ArtifactResolutionPath::TransientProduction,
                         .nanoseconds = 1000})
                .has_value();
        }

        [[nodiscard]] bool ObserveTransfer(Sora::Compute::BackendServiceId service,
                                           Sora::Compute::TransferEngineId engine, Sora::Compute::MemoryLocation source,
                                           Sora::Compute::MemoryLocation destination, uint64_t nanoseconds) noexcept {
            return performance_
                .Update({.key = {.service = service, .engine = engine, .source = source, .destination = destination},
                         .bytes = kBufferBytes,
                         .payloadNanoseconds = nanoseconds - 100,
                         .latencyNanoseconds = 100})
                .has_value();
        }

        [[nodiscard]] bool CreateGraph() noexcept {
            if (!packages_.Register(kCopyProgram)) {
                return false;
            }
            if (graphKind_ == DispatchPlanningGraph::ResidencySensitiveChain) {
                return packages_.Register(kSumProgram).has_value() && CreateResidencySensitiveChainGraph();
            }
            return graphKind_ == DispatchPlanningGraph::Chain ? CreateChainGraph() : CreateIndependentGraph();
        }

        [[nodiscard]] bool CreateChainGraph() noexcept {
            std::vector<Sora::Compute::Buffer> buffers;
            buffers.reserve(static_cast<size_t>(nodeCount_) + 1);
            for (uint32_t index = 0; index <= nodeCount_; ++index) {
                auto buffer = Sora::Compute::Buffer::Create({.id = {60, static_cast<uint64_t>(index) + 1},
                                                             .elementType = kFloat,
                                                             .layout = Sora::Compute::LayoutKind::Contiguous,
                                                             .rank = 1,
                                                             .sizeBytes = kBufferBytes,
                                                             .alignment = 64});
                if (!buffer) {
                    return false;
                }
                buffers.push_back(std::move(*buffer));
            }
            auto allocation =
                Sora::Compute::Allocation::Adopt(serviceA_, {.location = kHost,
                                                             .memory = {70, 1},
                                                             .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                             .sizeBytes = kBufferBytes,
                                                             .alignment = 64,
                                                             .state = new uint32_t{},
                                                             .destroy = DestroyOpaque});
            if (!allocation || !buffers.front().CommitWholeWrite(std::move(*allocation))) {
                return false;
            }
            Sora::Compute::GraphBuilder builder;
            constexpr std::array extents{kElementCount};
            for (uint32_t node = 0; node < nodeCount_; ++node) {
                auto input = Sora::Compute::BufferView::Contiguous(buffers[node], extents);
                auto output = Sora::Compute::BufferView::Contiguous(buffers[node + 1], extents);
                if (!input || !output) {
                    return false;
                }
                std::vector<Sora::Compute::KernelArgumentBinding> bindings;
                bindings.emplace_back(std::move(*input));
                bindings.emplace_back(std::move(*output));
                auto invocation = Sora::Compute::BindKernelProgram(kCopyProgram, extents, std::move(bindings));
                if (!invocation || !builder.AddDispatch(std::move(*invocation))) {
                    return false;
                }
            }
            auto graph = std::move(builder).Finish();
            if (!graph) {
                return false;
            }
            graph_.emplace(std::move(*graph));
            return true;
        }

        [[nodiscard]] bool CreateResidencySensitiveChainGraph() noexcept {
            if (nodeCount_ != 3) {
                return false;
            }
            std::vector<Sora::Compute::Buffer> buffers;
            buffers.reserve(4);
            for (uint32_t index = 0; index < 4; ++index) {
                auto buffer = Sora::Compute::Buffer::Create({.id = {64, static_cast<uint64_t>(index) + 1},
                                                             .elementType = kFloat,
                                                             .layout = Sora::Compute::LayoutKind::Contiguous,
                                                             .rank = 1,
                                                             .sizeBytes = kBufferBytes,
                                                             .alignment = 64});
                if (!buffer) {
                    return false;
                }
                buffers.push_back(std::move(*buffer));
            }
            auto allocation =
                Sora::Compute::Allocation::Adopt(serviceA_, {.location = kHost,
                                                             .memory = {74, 1},
                                                             .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                             .sizeBytes = kBufferBytes,
                                                             .alignment = 64,
                                                             .state = new uint32_t{},
                                                             .destroy = DestroyOpaque});
            if (!allocation || !buffers[0].CommitWholeWrite(std::move(*allocation))) {
                return false;
            }

            constexpr std::array extents{kElementCount};
            auto bindCopy = [&](Sora::Compute::Buffer input,
                                Sora::Compute::Buffer output) -> std::optional<Sora::Compute::KernelInvocation> {
                auto inputView = Sora::Compute::BufferView::Contiguous(std::move(input), extents);
                auto outputView = Sora::Compute::BufferView::Contiguous(std::move(output), extents);
                if (!inputView || !outputView) {
                    return std::nullopt;
                }
                std::vector<Sora::Compute::KernelArgumentBinding> bindings;
                bindings.emplace_back(std::move(*inputView));
                bindings.emplace_back(std::move(*outputView));
                auto invocation = Sora::Compute::BindKernelProgram(kCopyProgram, extents, std::move(bindings));
                return invocation ? std::optional{std::move(*invocation)} : std::nullopt;
            };
            auto first = bindCopy(buffers[0], buffers[1]);
            auto second = bindCopy(buffers[1], buffers[2]);
            auto left = Sora::Compute::BufferView::Contiguous(buffers[0], extents);
            auto right = Sora::Compute::BufferView::Contiguous(buffers[2], extents);
            auto output = Sora::Compute::BufferView::Contiguous(buffers[3], extents);
            if (!first || !second || !left || !right || !output) {
                return false;
            }
            std::vector<Sora::Compute::KernelArgumentBinding> sumBindings;
            sumBindings.emplace_back(std::move(*left));
            sumBindings.emplace_back(std::move(*right));
            sumBindings.emplace_back(std::move(*output));
            auto sum = Sora::Compute::BindKernelProgram(kSumProgram, extents, std::move(sumBindings));
            if (!sum) {
                return false;
            }
            Sora::Compute::GraphBuilder builder;
            if (!builder.AddDispatch(std::move(*first)) || !builder.AddDispatch(std::move(*second)) ||
                !builder.AddDispatch(std::move(*sum))) {
                return false;
            }
            auto graph = std::move(builder).Finish();
            if (!graph) {
                return false;
            }
            graph_.emplace(std::move(*graph));
            return true;
        }

        [[nodiscard]] bool CreateIndependentGraph() noexcept {
            Sora::Compute::GraphBuilder builder;
            constexpr std::array extents{kElementCount};
            for (uint32_t node = 0; node < nodeCount_; ++node) {
                auto input = Sora::Compute::Buffer::Create({.id = {65, static_cast<uint64_t>(node) * 2 + 1},
                                                            .elementType = kFloat,
                                                            .layout = Sora::Compute::LayoutKind::Contiguous,
                                                            .rank = 1,
                                                            .sizeBytes = kBufferBytes,
                                                            .alignment = 64});
                auto output = Sora::Compute::Buffer::Create({.id = {65, static_cast<uint64_t>(node) * 2 + 2},
                                                             .elementType = kFloat,
                                                             .layout = Sora::Compute::LayoutKind::Contiguous,
                                                             .rank = 1,
                                                             .sizeBytes = kBufferBytes,
                                                             .alignment = 64});
                const Sora::Compute::MemoryLocation source =
                    graphKind_ == DispatchPlanningGraph::IndependentSharedTransfer ? kHost : IndependentHost(node);
                auto allocation = Sora::Compute::Allocation::Adopt(
                    serviceA_, {.location = source,
                                .memory = {75, static_cast<uint64_t>(node) + 1},
                                .properties = Sora::Compute::MemoryProperty::HostVisible,
                                .sizeBytes = kBufferBytes,
                                .alignment = 64,
                                .state = new uint32_t{},
                                .destroy = DestroyOpaque});
                if (!input || !output || !allocation || !input->CommitWholeWrite(std::move(*allocation))) {
                    return false;
                }
                auto inputView = Sora::Compute::BufferView::Contiguous(std::move(*input), extents);
                auto outputView = Sora::Compute::BufferView::Contiguous(std::move(*output), extents);
                if (!inputView || !outputView) {
                    return false;
                }
                std::vector<Sora::Compute::KernelArgumentBinding> bindings;
                bindings.emplace_back(std::move(*inputView));
                bindings.emplace_back(std::move(*outputView));
                auto invocation = Sora::Compute::BindKernelProgram(kCopyProgram, extents, std::move(bindings));
                if (!invocation || !builder.AddDispatch(std::move(*invocation))) {
                    return false;
                }
            }
            auto graph = std::move(builder).Finish();
            if (!graph) {
                return false;
            }
            graph_.emplace(std::move(*graph));
            return true;
        }

        [[nodiscard]] bool FreezePrograms() noexcept {
            auto catalog = Sora::Compute::ProgramCatalogSnapshot::Create(packages_.Snapshot());
            if (!catalog) {
                return false;
            }
            programCatalog_.emplace(std::move(*catalog));
            return true;
        }

        template<typename Planner>
        [[nodiscard]] auto Plan(const Planner& planner) const noexcept {
            return planner(*graph_, {.epochSequence = 1, .devices = devices_, .snapshots = snapshots_},
                           {.packages = *programCatalog_,
                            .resources = catalog_->Inventory(),
                            .artifacts = *availability_,
                            .performance = *performanceSnapshot_});
        }

        template<typename Planner>
        [[nodiscard]] auto DetailedPlan(const Planner& planner) const noexcept {
            return planner.Plan(*graph_, {.epochSequence = 1, .devices = devices_, .snapshots = snapshots_},
                                {.packages = *programCatalog_,
                                 .resources = catalog_->Inventory(),
                                 .artifacts = *availability_,
                                 .performance = *performanceSnapshot_});
        }

        template<typename Planner>
        [[nodiscard]] auto
        DetailedPlan(const Planner& planner,
                     Sora::Compute::AutomaticPartitionPlanningCalibrationBatch& calibration) const noexcept {
            return planner.Plan(*graph_, {.epochSequence = 1, .devices = devices_, .snapshots = snapshots_},
                                {.packages = *programCatalog_,
                                 .resources = catalog_->Inventory(),
                                 .artifacts = *availability_,
                                 .performance = *performanceSnapshot_},
                                calibration);
        }

        Sora::Compute::BackendService serviceA_{};
        Sora::Compute::BackendService serviceB_{};
        Sora::Compute::ArtifactCache artifacts_;
        Sora::Compute::PortablePackageRegistry packages_{};
        std::optional<Sora::Compute::ProgramCatalogSnapshot> programCatalog_{};
        Sora::Compute::PerformanceModel performance_{};
        std::optional<Sora::Compute::PhysicalResourceCatalog> catalog_{};
        std::optional<Sora::Compute::ExecutionGraph> graph_{};
        std::expected<Sora::Compute::ArtifactAvailabilitySnapshot, Sora::Compute::ArtifactCacheError> availability_ =
            std::unexpected(Sora::Compute::ArtifactCacheError::OutOfMemory);
        std::shared_ptr<const Sora::Compute::PerformanceSnapshot> performanceSnapshot_{};
        std::vector<Sora::Compute::RuntimeDeviceBinding> devices_{};
        std::vector<Sora::Compute::DeviceSnapshot> snapshots_{};
        std::optional<Sora::Compute::GraphResidencyState> loweringBaseline_{};
        std::vector<Sora::Compute::DispatchGraphSelection> loweringSelections_{};
        uint64_t checksum_ = 0;
        uint64_t automaticPartitionBudget_ = 0;
        uint64_t automaticPartitionEvaluations_ = 0;
        uint32_t nodeCount_ = 0;
        uint32_t destinationCount_ = 0;
        DispatchPlanningGraph graphKind_ = DispatchPlanningGraph::Chain;
        bool failed_ = false;
    };

    template<typename Planner>
    [[nodiscard]] int RunDispatchPlanningBenchmark(const Planner& planner) {
        DispatchPlanningBenchmarkFixture chain1Destination1{1, 1};
        DispatchPlanningBenchmarkFixture chain8Destination1{8, 1};
        DispatchPlanningBenchmarkFixture chain8Destination2{8, 2};
        DispatchPlanningBenchmarkFixture chain32Destination2{32, 2};
        DispatchPlanningBenchmarkFixture residencySensitiveChain{3, 2, DispatchPlanningGraph::ResidencySensitiveChain};
        if (!chain1Destination1.Validate(planner) || !chain8Destination1.Validate(planner) ||
            !chain8Destination2.Validate(planner) || !chain32Destination2.Validate(planner) ||
            !residencySensitiveChain.Validate(planner)) {
            return 2;
        }
        auto case1x1 = [&]() noexcept { chain1Destination1.Invoke(planner); };
        auto case8x1 = [&]() noexcept { chain8Destination1.Invoke(planner); };
        auto case8x2 = [&]() noexcept { chain8Destination2.Invoke(planner); };
        auto case32x2 = [&]() noexcept { chain32Destination2.Invoke(planner); };
        auto residencyCase = [&]() noexcept { residencySensitiveChain.Invoke(planner); };
        const std::array cases{Sora::Benchmark::MakeCase("chain_1_destination_1", case1x1),
                               Sora::Benchmark::MakeCase("chain_8_destination_1", case8x1),
                               Sora::Benchmark::MakeCase("chain_8_destination_2", case8x2),
                               Sora::Benchmark::MakeCase("chain_32_destination_2", case32x2),
                               Sora::Benchmark::MakeCase("residency_sensitive_chain_3_destination_2", residencyCase)};
        const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
        const uint64_t checksum = chain1Destination1.Checksum() + chain8Destination1.Checksum() +
                                  chain8Destination2.Checksum() + chain32Destination2.Checksum() +
                                  residencySensitiveChain.Checksum();
        if (checksum == 0 || checksum == std::numeric_limits<uint64_t>::max()) {
            return 3;
        }

        std::println("metric,value,unit");
        std::println("graph_plan_selection_size,{},bytes", sizeof(Sora::Compute::GraphPlanSelection));
        std::println("chain_1_destination_1,{:.6f},ns/plan", timing.MedianTime(0));
        std::println("chain_8_destination_1,{:.6f},ns/plan", timing.MedianTime(1));
        std::println("chain_8_destination_2,{:.6f},ns/plan", timing.MedianTime(2));
        std::println("chain_32_destination_2,{:.6f},ns/plan", timing.MedianTime(3));
        std::println("residency_sensitive_chain_3_destination_2,{:.6f},ns/plan", timing.MedianTime(4));
        std::println("chain_1_destination_1_repetitions,{},count", timing.repetitions[0]);
        std::println("chain_8_destination_1_repetitions,{},count", timing.repetitions[1]);
        std::println("chain_8_destination_2_repetitions,{},count", timing.repetitions[2]);
        std::println("chain_32_destination_2_repetitions,{},count", timing.repetitions[3]);
        std::println("residency_sensitive_chain_3_destination_2_repetitions,{},count", timing.repetitions[4]);
        std::println("checksum,{},value", checksum);
        return 0;
    }

    template<typename Planner>
    [[nodiscard]] int
    RunHeftPlanningBenchmark(const Planner& wholeOnlyPlanner, const Planner& automaticPlanner,
                             const Planner& amortizedPlanner, const Planner& rejectedPartitionPlanner,
                             const Planner& oneNeighborPlanner, const Planner& eightNeighborPlanner,
                             const Planner& adaptivePlanner,
                             Sora::Compute::AutomaticPartitionPlanningCalibrationBatch& deferredCalibration) {
        DispatchPlanningBenchmarkFixture chain1Destination1{1, 1};
        DispatchPlanningBenchmarkFixture chain8Destination1{8, 1};
        DispatchPlanningBenchmarkFixture chain8Destination2{8, 2};
        DispatchPlanningBenchmarkFixture automaticChain1Destination2{1, 2};
        DispatchPlanningBenchmarkFixture automaticChain8Destination2{8, 2};
        DispatchPlanningBenchmarkFixture automaticChain32Destination2{32, 2};
        DispatchPlanningBenchmarkFixture rejectedChain1Destination2{1, 2};
        DispatchPlanningBenchmarkFixture amortizedChain1Destination2{1, 2};
        DispatchPlanningBenchmarkFixture chain8Destination2LocalSearch1{8, 2};
        DispatchPlanningBenchmarkFixture chain8Destination2LocalSearch8{8, 2};
        DispatchPlanningBenchmarkFixture adaptiveChain1Destination2{1, 2};
        DispatchPlanningBenchmarkFixture deferredAdaptiveChain1Destination2{1, 2};
        DispatchPlanningBenchmarkFixture chain32Destination2{32, 2};
        DispatchPlanningBenchmarkFixture independent8Shared{8, 1, DispatchPlanningGraph::IndependentSharedTransfer};
        DispatchPlanningBenchmarkFixture independent8Distinct{8, 1,
                                                              DispatchPlanningGraph::IndependentDistinctTransfers};
        if (!chain1Destination1.Validate(wholeOnlyPlanner) || !chain8Destination1.Validate(wholeOnlyPlanner) ||
            !chain8Destination2.Validate(wholeOnlyPlanner) || !chain32Destination2.Validate(wholeOnlyPlanner) ||
            !automaticChain1Destination2.Validate(automaticPlanner) ||
            !automaticChain8Destination2.Validate(automaticPlanner) ||
            !automaticChain32Destination2.Validate(automaticPlanner) ||
            !rejectedChain1Destination2.Validate(rejectedPartitionPlanner) ||
            !amortizedChain1Destination2.Validate(amortizedPlanner) || !independent8Shared.Validate(wholeOnlyPlanner) ||
            !independent8Distinct.Validate(wholeOnlyPlanner) ||
            !chain8Destination2LocalSearch1.ValidateLocalSearch(oneNeighborPlanner, 1) ||
            !chain8Destination2LocalSearch8.ValidateLocalSearch(eightNeighborPlanner, 8) ||
            !adaptiveChain1Destination2.ValidateAutomaticTuning(adaptivePlanner) ||
            !deferredAdaptiveChain1Destination2.ValidateDeferredAutomaticTuning(adaptivePlanner, deferredCalibration)) {
            return 2;
        }
        auto case1x1 = [&]() noexcept { chain1Destination1.Invoke(wholeOnlyPlanner); };
        auto case8x1 = [&]() noexcept { chain8Destination1.Invoke(wholeOnlyPlanner); };
        auto case8x2 = [&]() noexcept { chain8Destination2.Invoke(wholeOnlyPlanner); };
        auto case32x2 = [&]() noexcept { chain32Destination2.Invoke(wholeOnlyPlanner); };
        auto caseAutomatic1x2 = [&]() noexcept { automaticChain1Destination2.Invoke(automaticPlanner); };
        auto caseAutomatic8x2 = [&]() noexcept { automaticChain8Destination2.Invoke(automaticPlanner); };
        auto caseAutomatic32x2 = [&]() noexcept { automaticChain32Destination2.Invoke(automaticPlanner); };
        auto caseRejected1x2 = [&]() noexcept { rejectedChain1Destination2.Invoke(rejectedPartitionPlanner); };
        auto caseAmortized1x2 = [&]() noexcept { amortizedChain1Destination2.Invoke(amortizedPlanner); };
        auto caseShared = [&]() noexcept { independent8Shared.Invoke(wholeOnlyPlanner); };
        auto caseDistinct = [&]() noexcept { independent8Distinct.Invoke(wholeOnlyPlanner); };
        auto caseLocalSearch1 = [&]() noexcept { chain8Destination2LocalSearch1.Invoke(oneNeighborPlanner); };
        auto caseLocalSearch8 = [&]() noexcept { chain8Destination2LocalSearch8.Invoke(eightNeighborPlanner); };
        auto caseAdaptive1x2 = [&]() noexcept { adaptiveChain1Destination2.Invoke(adaptivePlanner); };
        auto caseDeferredAdaptive1x2 = [&]() noexcept {
            deferredAdaptiveChain1Destination2.InvokeDeferred(adaptivePlanner, deferredCalibration);
        };
        const std::array cases{
            Sora::Benchmark::MakeCase("chain_1_destination_1", case1x1),
            Sora::Benchmark::MakeCase("chain_8_destination_1", case8x1),
            Sora::Benchmark::MakeCase("chain_8_destination_2", case8x2),
            Sora::Benchmark::MakeCase("chain_32_destination_2", case32x2),
            Sora::Benchmark::MakeCase("automatic_chain_1_destination_2", caseAutomatic1x2),
            Sora::Benchmark::MakeCase("automatic_chain_8_destination_2", caseAutomatic8x2),
            Sora::Benchmark::MakeCase("automatic_chain_32_destination_2", caseAutomatic32x2),
            Sora::Benchmark::MakeCase("rejected_partition_chain_1_destination_2", caseRejected1x2),
            Sora::Benchmark::MakeCase("amortized_gate_chain_1_destination_2", caseAmortized1x2),
            Sora::Benchmark::MakeCase("independent_8_shared_transfer_engine", caseShared),
            Sora::Benchmark::MakeCase("independent_8_distinct_transfer_engines", caseDistinct),
            Sora::Benchmark::MakeCase("chain_8_destination_2_local_search_1", caseLocalSearch1),
            Sora::Benchmark::MakeCase("chain_8_destination_2_local_search_8", caseLocalSearch8),
            Sora::Benchmark::MakeCase("adaptive_reuse_1000_chain_1_destination_2", caseAdaptive1x2),
            Sora::Benchmark::MakeCase("deferred_adaptive_reuse_1000_chain_1_destination_2", caseDeferredAdaptive1x2)};
        const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
        const uint64_t checksum = chain1Destination1.Checksum() + chain8Destination1.Checksum() +
                                  chain8Destination2.Checksum() + chain32Destination2.Checksum() +
                                  automaticChain1Destination2.Checksum() + automaticChain8Destination2.Checksum() +
                                  automaticChain32Destination2.Checksum() + rejectedChain1Destination2.Checksum() +
                                  amortizedChain1Destination2.Checksum() + independent8Shared.Checksum() +
                                  independent8Distinct.Checksum() + chain8Destination2LocalSearch1.Checksum() +
                                  chain8Destination2LocalSearch8.Checksum() + adaptiveChain1Destination2.Checksum() +
                                  deferredAdaptiveChain1Destination2.Checksum();
        if (checksum == 0 || checksum == std::numeric_limits<uint64_t>::max()) {
            return 3;
        }
        const auto amortizedIncrementalCost = [&](size_t caseIndex, uint32_t evaluations) noexcept {
            std::array<double, kSamples> samples{};
            for (size_t sample = 0; sample < kSamples; ++sample) {
                samples[sample] =
                    (timing.nanosecondsPerElement[caseIndex][sample] - timing.nanosecondsPerElement[2][sample]) /
                    static_cast<double>(evaluations);
            }
            return Sora::Benchmark::Median(samples);
        };

        std::println("metric,value,unit");
        std::println("graph_plan_selection_size,{},bytes", sizeof(Sora::Compute::GraphPlanSelection));
        std::println("chain_1_destination_1,{:.6f},ns/plan", timing.MedianTime(0));
        std::println("chain_8_destination_1,{:.6f},ns/plan", timing.MedianTime(1));
        std::println("chain_8_destination_2,{:.6f},ns/plan", timing.MedianTime(2));
        std::println("chain_32_destination_2,{:.6f},ns/plan", timing.MedianTime(3));
        std::println("automatic_chain_1_destination_2,{:.6f},ns/plan", timing.MedianTime(4));
        std::println("automatic_chain_8_destination_2,{:.6f},ns/plan", timing.MedianTime(5));
        std::println("automatic_chain_32_destination_2,{:.6f},ns/plan", timing.MedianTime(6));
        std::println("rejected_partition_chain_1_destination_2,{:.6f},ns/plan", timing.MedianTime(7));
        std::println("amortized_gate_chain_1_destination_2,{:.6f},ns/plan", timing.MedianTime(8));
        std::println("independent_8_shared_transfer_engine,{:.6f},ns/plan", timing.MedianTime(9));
        std::println("independent_8_distinct_transfer_engines,{:.6f},ns/plan", timing.MedianTime(10));
        std::println("chain_8_destination_2_local_search_1,{:.6f},ns/plan", timing.MedianTime(11));
        std::println("chain_8_destination_2_local_search_8,{:.6f},ns/plan", timing.MedianTime(12));
        std::println("adaptive_reuse_1000_chain_1_destination_2,{:.6f},ns/plan", timing.MedianTime(13));
        std::println("deferred_adaptive_reuse_1000_chain_1_destination_2,{:.6f},ns/plan", timing.MedianTime(14));
        std::println("automatic_chain_8_destination_2_over_whole,{:.6f},ratio", timing.PairedRatio(5, 2));
        std::println("automatic_chain_32_destination_2_over_whole,{:.6f},ratio", timing.PairedRatio(6, 3));
        std::println("rejected_partition_over_selected_partition,{:.6f},ratio", timing.PairedRatio(7, 4));
        std::println("amortized_gate_over_forced_search,{:.6f},ratio", timing.PairedRatio(8, 4));
        std::println("adaptive_over_forced_search,{:.6f},ratio", timing.PairedRatio(13, 4));
        std::println("deferred_adaptive_over_immediate_adaptive,{:.6f},ratio", timing.PairedRatio(14, 13));
        std::println("adaptive_resolved_evaluation_budget,{},count",
                     adaptiveChain1Destination2.AutomaticPartitionBudget());
        std::println("adaptive_consumed_evaluations,{},count",
                     adaptiveChain1Destination2.AutomaticPartitionEvaluations());
        std::println("chain_8_destination_2_local_search_1_over_baseline,{:.6f},ratio", timing.PairedRatio(11, 2));
        std::println("chain_8_destination_2_local_search_8_over_baseline,{:.6f},ratio", timing.PairedRatio(12, 2));
        std::println("chain_8_destination_2_local_search_1_amortized_increment,{:.6f}"
                     ",ns/evaluation",
                     amortizedIncrementalCost(11, 1));
        std::println("chain_8_destination_2_local_search_8_amortized_increment,{:.6f}"
                     ",ns/evaluation",
                     amortizedIncrementalCost(12, 8));
        std::println("chain_1_destination_1_repetitions,{},count", timing.repetitions[0]);
        std::println("chain_8_destination_1_repetitions,{},count", timing.repetitions[1]);
        std::println("chain_8_destination_2_repetitions,{},count", timing.repetitions[2]);
        std::println("chain_32_destination_2_repetitions,{},count", timing.repetitions[3]);
        std::println("automatic_chain_1_destination_2_repetitions,{},count", timing.repetitions[4]);
        std::println("automatic_chain_8_destination_2_repetitions,{},count", timing.repetitions[5]);
        std::println("automatic_chain_32_destination_2_repetitions,{},count", timing.repetitions[6]);
        std::println("rejected_partition_chain_1_destination_2_repetitions,{},count", timing.repetitions[7]);
        std::println("amortized_gate_chain_1_destination_2_repetitions,{},count", timing.repetitions[8]);
        std::println("independent_8_shared_transfer_engine_repetitions,{},count", timing.repetitions[9]);
        std::println("independent_8_distinct_transfer_engines_repetitions,{},count", timing.repetitions[10]);
        std::println("chain_8_destination_2_local_search_1_repetitions,{},count", timing.repetitions[11]);
        std::println("chain_8_destination_2_local_search_8_repetitions,{},count", timing.repetitions[12]);
        std::println("adaptive_reuse_1000_chain_1_destination_2_repetitions,{},count", timing.repetitions[13]);
        std::println("deferred_adaptive_reuse_1000_chain_1_destination_2_repetitions,{},count", timing.repetitions[14]);
        std::println("checksum,{},value", checksum);
        return 0;
    }

} // namespace
