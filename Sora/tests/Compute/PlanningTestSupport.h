/**
 * @file PlanningTestSupport.h
 * @brief Shared physical-planning catalog, program, residency, and calibration
 * fixtures.
 */
#pragma once

#include "BackendTestSupport.h"

#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>
#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>
#include <Sora/Compute/Runtime/PipelineBindingLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace Sora::Test::ComputePlanning {

    using namespace Sora::Compute;

    constexpr BackendServiceId kServiceB{0x534F52415F544553ULL, 0x545F4241434B454FULL};
    constexpr DeviceId kHostDevice{1, 1};
    constexpr DeviceId kGpu{2, 2};
    constexpr DeviceId kGpuB{2, 3};
    constexpr MemoryLocation kHost{.domain = {10, 10}, .device = kHostDevice, .backend = BackendKind::Cpu};
    constexpr MemoryLocation kDevice{.domain = {20, 20}, .device = kGpu, .backend = BackendKind::Vulkan};
    constexpr MemoryLocation kAlternativeDevice{.domain = {21, 21}, .device = kGpu, .backend = BackendKind::Vulkan};
    constexpr MemoryLocation kDeviceB{.domain = {22, 22}, .device = kGpuB, .backend = BackendKind::Vulkan};
    constexpr QueueId kQueue{30, 30};
    constexpr QueueId kAlternativeQueue{31, 31};
    constexpr QueueId kQueueB{32, 32};
    constexpr TransferEngineId kTransfer{40, 40};
    constexpr TransferEngineId kAlternativeTransfer{41, 41};
    constexpr TransferEngineId kPeerTransfer{42, 42};
    constexpr TransferEngineId kTransferB{43, 43};
    constexpr SemanticHash kCompiler{50, 50};
    constexpr SemanticHash kAlternativeCompiler{49, 49};
    constexpr SemanticHash kPipelineFactory{51, 51};
    constexpr SemanticHash kPipelineStateA{52, 52};
    constexpr SemanticHash kPipelineStateB{53, 53};
    constexpr ValueType kFloat{ScalarKind::Float32, 1};
    constexpr uint64_t kElementCount = 256;
    constexpr uint64_t kBufferBytes = kElementCount * sizeof(float);

    consteval auto MakeCopyProgram() {
        KernelBuilder<1, 1> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        constexpr IndexDomain domain{.rank = 1};
        KernelProgramBuilder<2, 1, 1> program{domain, std::move(scalar).Finish(value),
                                              PartitionContract::Elementwise(1)};
        const auto input = program.AddBufferArgument(AccessMode::Read, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        const auto output = program.AddBufferArgument(AccessMode::Write, kFloat, 1, LayoutKind::Contiguous, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    constexpr auto kCopyProgram = MakeCopyProgram();

    struct NativeAllocation {
        std::atomic<uint32_t>* destroyCount = nullptr;
    };

    inline void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        allocation->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    inline void Submit(void*, const BackendSubmitRequest*, BackendSubmitResult*) noexcept {}
    inline void Allocate(void*, const BackendAllocationRequest*, BackendAllocationResult*) noexcept {}
    inline void Transfer(void*, const BackendTransferRequest*, BackendTransferResult*) noexcept {}
    inline void CreatePipeline(void*, const BackendPipelineRequest*, BackendPipelineResult*) noexcept {}
    inline void Compile(void*, const BackendCompileRequest*, BackendCompileResult*) noexcept {}
    inline void ImportArtifact(void*, const BackendArtifactImportRequest*, BackendCompileResult*) noexcept {}
    inline void ExportArtifact(void*, const BackendArtifactExportRequest*, BackendArtifactExportResult*) noexcept {}

    inline void DestroySecondaryService(void* state) noexcept {
        delete static_cast<uint32_t*>(state);
    }

    inline BackendService MakeSecondaryService() {
        auto code = BackendCodeLease::Retain(std::make_shared<uint32_t>());
        auto service = BackendService::Adopt(
            {.id = kServiceB, .state = new uint32_t{}, .destroy = DestroySecondaryService}, std::move(code));
        REQUIRE(service.has_value());
        return std::move(*service);
    }

    inline void Observe(void*, DeviceSnapshot* snapshot) noexcept {
        *snapshot = {
            .id = kGpu, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U};
    }

    inline void ObserveB(void*, DeviceSnapshot* snapshot) noexcept {
        *snapshot = {
            .id = kGpuB, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U};
    }

    inline Allocation MakeAllocation(MemoryLocation location, std::atomic<uint32_t>& destroyCount) {
        static std::atomic<uint64_t> nextMemory = 1;
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation =
            Allocation::Adopt(Sora::Test::TestBackendService(),
                              {.location = location,
                               .memory = {nextMemory.fetch_add(1, std::memory_order_relaxed), 0xD15CA11ULL},
                               .properties = location == kDevice ? MemoryPropertyMask{MemoryProperty::DeviceLocal}
                                                                 : MemoryPropertyMask{MemoryProperty::HostVisible},
                               .sizeBytes = kBufferBytes,
                               .alignment = 64,
                               .state = native.release(),
                               .destroy = DestroyAllocation});
        REQUIRE(allocation.has_value());
        return std::move(*allocation);
    }

    inline Buffer MakeBuffer(BufferId id) {
        auto buffer = Buffer::Create({.id = id,
                                      .elementType = kFloat,
                                      .layout = LayoutKind::Contiguous,
                                      .rank = 1,
                                      .sizeBytes = kBufferBytes,
                                      .alignment = 64});
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    inline BufferView View(Buffer buffer) {
        constexpr std::array extents{kElementCount};
        auto view = BufferView::Contiguous(std::move(buffer), extents);
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    inline KernelInvocation Invocation(Buffer input, Buffer output) {
        constexpr std::array extents{kElementCount};
        std::vector<KernelArgumentBinding> bindings;
        bindings.emplace_back(View(std::move(input)));
        bindings.emplace_back(View(std::move(output)));
        auto invocation = BindKernelProgram(kCopyProgram, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    inline DeviceDescriptor Descriptor(DeviceId device = kGpu) {
        return {.id = device,
                .backend = BackendKind::Vulkan,
                .kind = DeviceKind::DiscreteGpu,
                .architectureId = device == kGpu ? 61ULL : 62ULL,
                .totalMemoryBytes = uint64_t{8} << 30U,
                .maximumAllocationBytes = uint64_t{2} << 30U,
                .subgroupWidthMinimum = 1,
                .subgroupWidthMaximum = 32,
                .maximumWorkgroupInvocations = 1024};
    }

    inline TargetFingerprint Target(DeviceId device = kGpu, SemanticHash compiler = kCompiler,
                                    ExecutableTargetDomain domain = ExecutableTargetDomain::TargetNative) {
        return {.device = device,
                .backend = BackendKind::Vulkan,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = TargetIdentityStability::MachineStable,
                .executableDomain = domain,
                .addressBits = 64,
                .architectureId = device == kGpu ? 61ULL : 62ULL,
                .compilerIdentity = compiler,
                .driverIdentity = device == kGpu ? SemanticHash{62, 62} : SemanticHash{63, 63}};
    }

    struct CatalogOptions {
        bool includeAlternativeQueue = false;
        bool includeCopyOnlyQueue = false;
        bool includeAlternativeMemory = false;
        bool includeSecondDevice = false;
        bool reusePrimaryTransferIdentityOnSecondService = false;
        bool primaryQueueSupportsCopy = false;
        bool secondaryQueueSupportsCopy = false;
        bool includeAlternativeCompiler = false;
        bool persistentPrimaryCompiler = false;
        bool includePackagedExecutable = false;
        ArtifactKind primaryDispatchArtifact = ArtifactKind::DeviceExecutable;
        ArtifactKind secondaryDispatchArtifact = ArtifactKind::DeviceExecutable;
        bool includeSecondPipelineConfiguration = false;
    };

    inline PhysicalResourceCatalog MakeCatalog(CatalogOptions options = {}) {
        const BackendService& service = Sora::Test::TestBackendService();
        auto observer = DeviceObserver::Create(
            service, {.device = kGpu, .backend = BackendKind::Vulkan, .state = service.State(), .observe = Observe});
        auto primaryQueue =
            Queue::Adopt(service, {.id = kQueue,
                                   .device = kGpu,
                                   .backend = BackendKind::Vulkan,
                                   .capabilities = options.primaryQueueSupportsCopy
                                                       ? QueueCapability::Dispatch | QueueCapability::Copy
                                                       : QueueCapability::Dispatch,
                                   .dispatchArtifactKind = options.primaryDispatchArtifact,
                                   .state = new uint32_t{},
                                   .destroy = [](void* state) noexcept { delete static_cast<uint32_t*>(state); },
                                   .submit = Submit});
        auto allocator = MemoryAllocator::Create(service, {.location = kDevice,
                                                           .supportedProperties = MemoryProperty::DeviceLocal,
                                                           .state = service.State(),
                                                           .allocate = Allocate});
        REQUIRE(allocator.has_value());
        auto transfer = TransferEngine::Create(service, {.engine = kTransfer,
                                                         .sourceService = service.Id(),
                                                         .source = kHost,
                                                         .destination = kDevice,
                                                         .kind = TransferKind::Copy,
                                                         .completionDevice = kGpu,
                                                         .completionBackend = BackendKind::Vulkan,
                                                         .state = service.State(),
                                                         .transfer = Transfer});
        REQUIRE(observer.has_value());
        REQUIRE(primaryQueue.has_value());
        REQUIRE(allocator.has_value());
        REQUIRE(transfer.has_value());
        auto compiler = BackendCompiler::Create(
            service, {.identity = kCompiler,
                      .backend = BackendKind::Vulkan,
                      .state = service.State(),
                      .compile = Compile,
                      .importArtifact = options.persistentPrimaryCompiler ? ImportArtifact : nullptr,
                      .exportArtifact = options.persistentPrimaryCompiler ? ExportArtifact : nullptr});
        REQUIRE(compiler.has_value());

        PhysicalResourceCatalogStorage storage{.deviceObservers = {std::move(*observer)},
                                               .queues = {std::move(*primaryQueue)},
                                               .allocators = {std::move(*allocator)},
                                               .transferEngines = {std::move(*transfer)},
                                               .compilers = {std::move(*compiler)}};
        if (options.includeAlternativeCompiler) {
            auto alternativeCompiler =
                BackendCompiler::Create(service, {.identity = kAlternativeCompiler,
                                                  .backend = BackendKind::Vulkan,
                                                  .executableDomain = ExecutableTargetDomain::BackendPortable,
                                                  .state = service.State(),
                                                  .compile = Compile});
            REQUIRE(alternativeCompiler.has_value());
            storage.compilers.push_back(std::move(*alternativeCompiler));
        }
        if (options.primaryDispatchArtifact == ArtifactKind::DevicePipeline) {
            auto factory = BackendPipelineFactory::Create(
                service, Target(kGpu),
                {.identity = kPipelineFactory, .state = service.State(), .create = CreatePipeline});
            REQUIRE(factory.has_value());
            storage.pipelineConfigurations.push_back(factory->Configuration(kPipelineStateA));
            if (options.includeSecondPipelineConfiguration) {
                storage.pipelineConfigurations.push_back(factory->Configuration(kPipelineStateB));
            }
            storage.pipelineFactories.push_back(std::move(*factory));
        }
        if (options.includeAlternativeQueue || options.includeCopyOnlyQueue) {
            auto queue =
                Queue::Adopt(service, {.id = kAlternativeQueue,
                                       .device = kGpu,
                                       .backend = BackendKind::Vulkan,
                                       .capabilities = options.includeCopyOnlyQueue ? QueueCapability::Copy
                                                                                    : QueueCapability::Dispatch,
                                       .state = new uint32_t{},
                                       .destroy = [](void* state) noexcept { delete static_cast<uint32_t*>(state); },
                                       .submit = Submit});
            REQUIRE(queue.has_value());
            storage.queues.push_back(std::move(*queue));
        }
        if (options.includeAlternativeMemory) {
            auto alternativeAllocator =
                MemoryAllocator::Create(service, {.location = kAlternativeDevice,
                                                  .supportedProperties = MemoryProperty::DeviceLocal,
                                                  .state = service.State(),
                                                  .allocate = Allocate});
            auto alternativeTransfer = TransferEngine::Create(service, {.engine = kAlternativeTransfer,
                                                                        .sourceService = service.Id(),
                                                                        .source = kHost,
                                                                        .destination = kAlternativeDevice,
                                                                        .kind = TransferKind::Copy,
                                                                        .completionDevice = kGpu,
                                                                        .completionBackend = BackendKind::Vulkan,
                                                                        .state = service.State(),
                                                                        .transfer = Transfer});
            REQUIRE(alternativeAllocator.has_value());
            REQUIRE(alternativeTransfer.has_value());
            storage.allocators.push_back(std::move(*alternativeAllocator));
            storage.transferEngines.push_back(std::move(*alternativeTransfer));
        }
        if (options.includeSecondDevice) {
            BackendService serviceB = MakeSecondaryService();
            auto observerB = DeviceObserver::Create(
                serviceB,
                {.device = kGpuB, .backend = BackendKind::Vulkan, .state = serviceB.State(), .observe = ObserveB});
            auto queueB =
                Queue::Adopt(serviceB, {.id = kQueueB,
                                        .device = kGpuB,
                                        .backend = BackendKind::Vulkan,
                                        .capabilities = options.secondaryQueueSupportsCopy
                                                            ? QueueCapability::Dispatch | QueueCapability::Copy
                                                            : QueueCapability::Dispatch,
                                        .dispatchArtifactKind = options.secondaryDispatchArtifact,
                                        .state = new uint32_t{},
                                        .destroy = [](void* state) noexcept { delete static_cast<uint32_t*>(state); },
                                        .submit = Submit});
            auto allocatorB = MemoryAllocator::Create(serviceB, {.location = kDeviceB,
                                                                 .supportedProperties = MemoryProperty::DeviceLocal,
                                                                 .state = serviceB.State(),
                                                                 .allocate = Allocate});
            auto peerTransfer = TransferEngine::Create(serviceB, {.engine = kPeerTransfer,
                                                                  .sourceService = service.Id(),
                                                                  .source = kDevice,
                                                                  .destination = kDeviceB,
                                                                  .kind = TransferKind::Copy,
                                                                  .completionDevice = kGpuB,
                                                                  .completionBackend = BackendKind::Vulkan,
                                                                  .state = serviceB.State(),
                                                                  .transfer = Transfer});
            auto transferB = TransferEngine::Create(
                serviceB, {.engine = options.reusePrimaryTransferIdentityOnSecondService ? kTransfer : kTransferB,
                           .sourceService = service.Id(),
                           .source = kHost,
                           .destination = kDeviceB,
                           .kind = TransferKind::Copy,
                           .completionDevice = kGpuB,
                           .completionBackend = BackendKind::Vulkan,
                           .state = serviceB.State(),
                           .transfer = Transfer});
            REQUIRE(observerB.has_value());
            REQUIRE(queueB.has_value());
            REQUIRE(allocatorB.has_value());
            REQUIRE(peerTransfer.has_value());
            REQUIRE(transferB.has_value());
            auto compilerB = BackendCompiler::Create(
                serviceB,
                {.identity = kCompiler, .backend = BackendKind::Vulkan, .state = serviceB.State(), .compile = Compile});
            REQUIRE(compilerB.has_value());
            if (options.secondaryDispatchArtifact == ArtifactKind::DevicePipeline) {
                auto factoryB = BackendPipelineFactory::Create(
                    serviceB, Target(kGpuB),
                    {.identity = kPipelineFactory, .state = serviceB.State(), .create = CreatePipeline});
                REQUIRE(factoryB.has_value());
                storage.pipelineConfigurations.push_back(factoryB->Configuration(kPipelineStateA));
                if (options.includeSecondPipelineConfiguration) {
                    storage.pipelineConfigurations.push_back(factoryB->Configuration(kPipelineStateB));
                }
                storage.pipelineFactories.push_back(std::move(*factoryB));
            }
            storage.deviceObservers.push_back(std::move(*observerB));
            storage.queues.push_back(std::move(*queueB));
            storage.allocators.push_back(std::move(*allocatorB));
            storage.transferEngines.push_back(std::move(*peerTransfer));
            storage.transferEngines.push_back(std::move(*transferB));
            storage.compilers.push_back(std::move(*compilerB));
        }
        auto catalog = PhysicalResourceCatalog::Create(std::move(storage));
        REQUIRE(catalog.has_value());
        return std::move(*catalog);
    }

    inline ExecutionGraph MakeGraph(KernelInvocation invocation) {
        GraphBuilder builder;
        REQUIRE(builder.AddDispatch(std::move(invocation)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    inline void CalibrateKernel(PerformanceModel& model, QueueId queue = kQueue, TargetFingerprint target = Target()) {
        const KernelCalibrationKey key{
            .kernel = kCopyProgram.Hash(), .target = target, .specialization = {}, .queue = queue};
        REQUIRE(
            model
                .Update(
                    {.key = key, .workItemCount = kElementCount, .computeNanoseconds = 256, .launchNanoseconds = 100})
                .has_value());
        const ArtifactKey artifact =
            MakeArtifactKey(ExecutableKey{.lowered = key.kernel, .target = key.target, .specialization = {}});
        for (const ArtifactResolutionPath path :
             {ArtifactResolutionPath::PackageImport, ArtifactResolutionPath::PersistentImport,
              ArtifactResolutionPath::PersistentProduction, ArtifactResolutionPath::TransientProduction}) {
            REQUIRE(model.Update({.key = artifact, .path = path, .nanoseconds = 10'000}).has_value());
        }
    }

    inline void CalibrateTransfer(PerformanceModel& model, TransferEngineId engine = kTransfer,
                                  MemoryLocation destination = kDevice, MemoryLocation source = kHost,
                                  BackendServiceId service = Sora::Test::TestBackendService().Id()) {
        const TransferCalibrationKey key{
            .service = service, .engine = engine, .source = source, .destination = destination};
        REQUIRE(model.Update({.key = key, .bytes = kBufferBytes, .payloadNanoseconds = 1024, .latencyNanoseconds = 100})
                    .has_value());
    }

    inline PipelineKey PipelineFor(SemanticHash state, TargetFingerprint target = Target()) {
        const ArtifactKey executable =
            MakeArtifactKey(ExecutableKey{.lowered = kCopyProgram.Hash(), .target = target, .specialization = {}});
        return {.executable = ArtifactIdentity(executable),
                .target = target,
                .builder = kPipelineFactory,
                .state = state,
                .bindingLayout = PipelineBindingLayoutIdentity(kCopyProgram.Arguments())};
    }

    inline void CalibratePipeline(PerformanceModel& model, SemanticHash state, uint64_t nanoseconds,
                                  TargetFingerprint target = Target()) {
        const ArtifactKey artifact = MakeArtifactKey(PipelineFor(state, target));
        for (const ArtifactResolutionPath path :
             {ArtifactResolutionPath::PackageImport, ArtifactResolutionPath::PersistentImport,
              ArtifactResolutionPath::PersistentProduction, ArtifactResolutionPath::TransientProduction}) {
            REQUIRE(model.Update({.key = artifact, .path = path, .nanoseconds = nanoseconds}).has_value());
        }
    }

    inline void CachePipeline(ArtifactCache& cache, SemanticHash state) {
        auto artifact =
            Artifact::Adopt(Sora::Test::TestBackendService(),
                            {.key = MakeArtifactKey(PipelineFor(state)),
                             .device = kGpu,
                             .state = new uint32_t{},
                             .destroy = [](void* value) noexcept { delete static_cast<uint32_t*>(value); }});
        REQUIRE(artifact.has_value());
        REQUIRE(cache.Insert(std::move(*artifact)).has_value());
    }

    inline void CacheExecutable(ArtifactCache& cache, TargetFingerprint target) {
        const auto key =
            MakeArtifactKey(ExecutableKey{.lowered = kCopyProgram.Hash(), .target = target, .specialization = {}});
        auto artifact = Artifact::Adopt(
            Sora::Test::TestBackendService(),
            {.key = key, .device = target.device, .state = new uint32_t{}, .destroy = [](void* value) noexcept {
                 delete static_cast<uint32_t*>(value);
             }});
        REQUIRE(artifact.has_value());
        REQUIRE(cache.Insert(std::move(*artifact)).has_value());
    }

    [[nodiscard]] inline std::shared_ptr<const ProgramCatalogSnapshot>
    FreezeProgramCatalog(const PortablePackageRegistry& registry) {
        auto catalog = ProgramCatalogSnapshot::Create(registry.Snapshot());
        REQUIRE(catalog.has_value());
        return std::make_shared<const ProgramCatalogSnapshot>(std::move(*catalog));
    }

    struct PlanningFixture {
        PortablePackageRegistry packages{};
        PhysicalResourceCatalog resources;
        ArtifactCache artifacts{8};
        PerformanceModel performance{};
        RuntimeDeviceBinding device{.service = Sora::Test::TestBackendService().Id(),
                                    .descriptor = Descriptor(),
                                    .target = RuntimeTarget(Target())};
        DeviceSnapshot snapshot{
            .id = kGpu, .state = DeviceState::Available, .generation = 1, .availableMemoryBytes = uint64_t{8} << 30U};

        explicit PlanningFixture(CatalogOptions options = {}, bool calibrateDefault = true)
            : resources(MakeCatalog(options)) {
            if (options.includePackagedExecutable) {
                auto program = ProgramPackage::Create(kCopyProgram);
                REQUIRE(program.has_value());
                const ArtifactKey executable = MakeArtifactKey(
                    ExecutableKey{.lowered = kCopyProgram.Hash(), .target = Target(), .specialization = {}});
                std::vector packaged{PackagedArtifact{.key = executable, .payload = {std::byte{0x53}}}};
                auto package = PortablePackage::Create(std::move(*program), std::move(packaged));
                REQUIRE(package.has_value());
                REQUIRE(packages.Register(std::move(*package)).has_value());
            } else {
                REQUIRE(packages.Register(kCopyProgram).has_value());
            }
            if (calibrateDefault) {
                CalibrateKernel(performance);
            }
            if (calibrateDefault && options.includeAlternativeQueue) {
                CalibrateKernel(performance, kAlternativeQueue);
            }
        }
    };

    inline auto Enumerate(const ExecutionGraph& graph, PlanningFixture& fixture,
                          const DispatchEnumerationPolicy& policy = {}) {
        const auto availability = fixture.artifacts.SnapshotAvailability();
        REQUIRE(availability.has_value());
        const auto performance = fixture.performance.Snapshot();
        const auto packages = FreezeProgramCatalog(fixture.packages);
        const std::array devices{fixture.device};
        const std::array snapshots{fixture.snapshot};
        const auto residency = FreezeGraphResidency(graph);
        REQUIRE(residency.has_value());
        return EnumerateDispatchCandidates(graph, GraphNodeId{0},
                                           {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                                           {.packages = *packages,
                                            .resources = fixture.resources.Inventory(),
                                            .artifacts = *availability,
                                            .performance = *performance},
                                           *residency, policy);
    }

} // namespace Sora::Test::ComputePlanning
