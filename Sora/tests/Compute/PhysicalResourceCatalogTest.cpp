#include "BackendTestSupport.h"

#include <Sora/Compute/Runtime/CalibratedTransferTopology.h>
#include <Sora/Compute/Runtime/CandidateSynthesis.h>
#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <ranges>

namespace {

    constexpr Sora::Compute::DeviceId kCpu{1, 1};
    constexpr Sora::Compute::DeviceId kGpu{2, 2};
    constexpr size_t kCpuPriorIndex = static_cast<size_t>(Sora::Compute::DeviceKind::Cpu);
    constexpr size_t kDiscreteGpuPriorIndex = static_cast<size_t>(Sora::Compute::DeviceKind::DiscreteGpu);
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {10, 10}, .device = kCpu, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kDevice{
        .domain = {20, 20}, .device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan};

    [[nodiscard]] constexpr Sora::Compute::TargetFingerprint GpuTarget() noexcept {
        return {.device = kGpu,
                .backend = Sora::Compute::BackendKind::Vulkan,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                .addressBits = 64,
                .architectureId = 2,
                .compilerIdentity = {10, 11},
                .driverIdentity = {12, 13}};
    }

    [[nodiscard]] constexpr Sora::Compute::TargetFingerprint CpuTarget() noexcept {
        return {.device = kCpu,
                .backend = Sora::Compute::BackendKind::Cpu,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                .addressBits = 64,
                .architectureId = 1,
                .compilerIdentity = {5, 6},
                .driverIdentity = {7, 8}};
    }

    void DestroyOpaque(void* state) noexcept {
        delete static_cast<uint32_t*>(state);
    }

    void Submit(void*, const Sora::Compute::BackendSubmitRequest*, Sora::Compute::BackendSubmitResult*) noexcept {}

    void Allocate(void*, const Sora::Compute::BackendAllocationRequest*,
                  Sora::Compute::BackendAllocationResult*) noexcept {}

    void Transfer(void*, const Sora::Compute::BackendTransferRequest*, Sora::Compute::BackendTransferResult*) noexcept {
    }

    void Compile(void*, const Sora::Compute::BackendCompileRequest*, Sora::Compute::BackendCompileResult*) noexcept {}

    void CreatePipeline(void*, const Sora::Compute::BackendPipelineRequest*,
                        Sora::Compute::BackendPipelineResult*) noexcept {}

    void ObserveGpu(void*, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        *snapshot = {.id = kGpu, .state = Sora::Compute::DeviceState::Available, .generation = 1};
    }

    void ObserveCpu(void*, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        *snapshot = {.id = kCpu, .state = Sora::Compute::DeviceState::Available, .generation = 1};
    }

    Sora::Compute::BackendService MakeService(Sora::Compute::BackendServiceId id) {
        auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
        auto service = Sora::Compute::BackendService::Adopt(
            {.id = id, .state = new uint32_t{}, .destroy = DestroyOpaque}, std::move(code));
        REQUIRE(service.has_value());
        return *service;
    }

    Sora::Compute::DeviceObserver MakeObserver(Sora::Compute::BackendService service) {
        auto observer = Sora::Compute::DeviceObserver::Create(service, {.device = kGpu,
                                                                        .backend = Sora::Compute::BackendKind::Vulkan,
                                                                        .state = service.State(),
                                                                        .observe = ObserveGpu});
        REQUIRE(observer.has_value());
        return *observer;
    }

    Sora::Compute::DeviceObserver MakeCpuObserver(Sora::Compute::BackendService service) {
        auto observer = Sora::Compute::DeviceObserver::Create(service, {.device = kCpu,
                                                                        .backend = Sora::Compute::BackendKind::Cpu,
                                                                        .state = service.State(),
                                                                        .observe = ObserveCpu});
        REQUIRE(observer.has_value());
        return *observer;
    }

    Sora::Compute::Queue MakeQueue(Sora::Compute::BackendService service, Sora::Compute::DeviceId device,
                                   Sora::Compute::BackendKind backend, uint64_t id,
                                   Sora::Compute::QueueCapabilityMask capabilities,
                                   Sora::Compute::ArtifactKind dispatchArtifactKind,
                                   bool reportsExecutionNanoseconds = false) {
        auto queue = Sora::Compute::Queue::Adopt(service, {.id = {id, 1},
                                                           .device = device,
                                                           .backend = backend,
                                                           .capabilities = capabilities,
                                                           .dispatchArtifactKind = dispatchArtifactKind,
                                                           .state = new uint32_t{},
                                                           .destroy = DestroyOpaque,
                                                           .submit = Submit,
                                                           .reportsExecutionNanoseconds = reportsExecutionNanoseconds});
        REQUIRE(queue.has_value());
        return *queue;
    }

    Sora::Compute::Queue
    MakeQueue(uint64_t id, Sora::Compute::QueueCapabilityMask capabilities,
              Sora::Compute::ArtifactKind dispatchArtifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
              bool reportsExecutionNanoseconds = false) {
        return MakeQueue(Sora::Test::TestBackendService(), kGpu, Sora::Compute::BackendKind::Vulkan, id, capabilities,
                         dispatchArtifactKind, reportsExecutionNanoseconds);
    }

    Sora::Compute::MemoryAllocator MakeAllocator(Sora::Compute::BackendService service,
                                                 Sora::Compute::MemoryLocation location) {
        auto allocator = Sora::Compute::MemoryAllocator::Create(
            service, {.location = location,
                      .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                      .state = service.State(),
                      .allocate = Allocate});
        REQUIRE(allocator.has_value());
        return *allocator;
    }

    Sora::Compute::MemoryAllocator MakeAllocator(Sora::Compute::MemoryLocation location) {
        return MakeAllocator(Sora::Test::TestBackendService(), location);
    }

    Sora::Compute::TransferEngine MakeTransfer(Sora::Compute::BackendService destinationService,
                                               Sora::Compute::BackendServiceId sourceService, uint64_t id) {
        auto engine = Sora::Compute::TransferEngine::Create(destinationService,
                                                            {.engine = {id, 1},
                                                             .sourceService = sourceService,
                                                             .source = kHost,
                                                             .destination = kDevice,
                                                             .kind = Sora::Compute::TransferKind::Copy,
                                                             .completionDevice = kGpu,
                                                             .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                             .state = destinationService.State(),
                                                             .transfer = Transfer});
        REQUIRE(engine.has_value());
        return *engine;
    }

    Sora::Compute::TransferEngine MakeTransfer(uint64_t id) {
        return MakeTransfer(Sora::Test::TestBackendService(), Sora::Test::TestBackendService().Id(), id);
    }

    Sora::Compute::BackendCompiler
    MakeCompiler(Sora::Compute::SemanticHash identity,
                 Sora::Compute::ExecutableTargetDomain domain = Sora::Compute::ExecutableTargetDomain::TargetNative) {
        const auto& service = Sora::Test::TestBackendService();
        auto compiler = Sora::Compute::BackendCompiler::Create(service, {.identity = identity,
                                                                         .backend = Sora::Compute::BackendKind::Vulkan,
                                                                         .executableDomain = domain,
                                                                         .state = service.State(),
                                                                         .compile = Compile});
        REQUIRE(compiler.has_value());
        return *compiler;
    }

    Sora::Compute::BackendCompiler
    MakeCompiler(Sora::Compute::BackendService service, Sora::Compute::SemanticHash identity,
                 Sora::Compute::ExecutableTargetDomain domain = Sora::Compute::ExecutableTargetDomain::TargetNative) {
        auto compiler = Sora::Compute::BackendCompiler::Create(service, {.identity = identity,
                                                                         .backend = Sora::Compute::BackendKind::Vulkan,
                                                                         .executableDomain = domain,
                                                                         .state = service.State(),
                                                                         .compile = Compile});
        REQUIRE(compiler.has_value());
        return *compiler;
    }

    Sora::Compute::BackendCompiler MakeCompiler(Sora::Compute::BackendService service,
                                                Sora::Compute::BackendKind backend,
                                                Sora::Compute::SemanticHash identity) {
        auto compiler = Sora::Compute::BackendCompiler::Create(
            service, {.identity = identity,
                      .backend = backend,
                      .executableDomain = Sora::Compute::ExecutableTargetDomain::TargetNative,
                      .state = service.State(),
                      .compile = Compile});
        REQUIRE(compiler.has_value());
        return *compiler;
    }

    Sora::Compute::BackendPipelineFactory MakePipelineFactory(Sora::Compute::SemanticHash identity) {
        const auto& service = Sora::Test::TestBackendService();
        auto factory = Sora::Compute::BackendPipelineFactory::Create(
            service, GpuTarget(), {.identity = identity, .state = service.State(), .create = CreatePipeline});
        REQUIRE(factory.has_value());
        return *factory;
    }

    Sora::Compute::BackendPipelineFactory MakePipelineFactory(Sora::Compute::BackendService service,
                                                              Sora::Compute::SemanticHash identity) {
        auto factory = Sora::Compute::BackendPipelineFactory::Create(
            service, GpuTarget(), {.identity = identity, .state = service.State(), .create = CreatePipeline});
        REQUIRE(factory.has_value());
        return *factory;
    }

} // namespace

TEST_CASE("PhysicalResourceCatalog provides deterministic allocation-free "
          "resource lookup",
          "[Sora][Compute][Runtime][PhysicalResourceCatalog]") {
    auto queue2 = MakeQueue(2, Sora::Compute::QueueCapability::Dispatch | Sora::Compute::QueueCapability::Copy,
                            Sora::Compute::ArtifactKind::DeviceExecutable, true);
    auto queue1 = MakeQueue(1, Sora::Compute::QueueCapability::Copy);
    auto allocator = MakeAllocator(kDevice);
    auto transfer = MakeTransfer(3);
    auto compiler = MakeCompiler({4, 5});
    auto pipelineFactory = MakePipelineFactory({6, 7});
    const auto pipelineConfiguration = pipelineFactory.Configuration({8, 9});
    const auto service = Sora::Test::TestBackendService().Id();
    auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
        {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
         .queues = {queue2, queue1},
         .allocators = {allocator},
         .transferEngines = {transfer},
         .compilers = {compiler},
         .pipelineFactories = {pipelineFactory},
         .pipelineConfigurations = {pipelineConfiguration}});
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->FindQueue(service, Sora::Compute::BackendKind::Vulkan, kGpu, {2, 1})->View().id ==
            Sora::Compute::QueueId{2, 1});
    REQUIRE(
        catalog->SelectQueue(service, Sora::Compute::BackendKind::Vulkan, kGpu, Sora::Compute::BackendCommandKind::Copy)
            ->View()
            .id == Sora::Compute::QueueId{1, 1});
    REQUIRE(catalog->SelectQueue(service, Sora::Compute::BackendKind::Vulkan, kGpu,
                                 Sora::Compute::BackendCommandKind::Fill) == nullptr);
    REQUIRE(catalog->SelectQueue(service, Sora::Compute::BackendKind::Cuda, kGpu,
                                 Sora::Compute::BackendCommandKind::Copy) == nullptr);
    REQUIRE(catalog->FindAllocator(service, kDevice) != nullptr);
    REQUIRE(catalog->FindAllocator(service, kHost) == nullptr);
    REQUIRE(catalog->FindTransferEngine(service, {3, 1}) != nullptr);
    REQUIRE(catalog->FindCompiler(service, Sora::Compute::BackendKind::Vulkan, {4, 5}) != nullptr);
    REQUIRE(catalog->FindCompiler(service, Sora::Compute::BackendKind::Cpu, {4, 5}) == nullptr);
    REQUIRE(catalog->FindCompiler({9, 9}, Sora::Compute::BackendKind::Vulkan, {4, 5}) == nullptr);
    REQUIRE(catalog->FindPipelineFactory(service, Sora::Compute::BackendKind::Vulkan, kGpu, {6, 7}) != nullptr);
    REQUIRE(catalog->FindPipelineFactory(service, Sora::Compute::BackendKind::Vulkan, kCpu, {6, 7}) == nullptr);

    const auto& inventory = catalog->Inventory();
    REQUIRE(inventory.Queues().size() == 2);
    REQUIRE(inventory.Queues()[0].queue == Sora::Compute::QueueId{1, 1});
    REQUIRE(inventory.Queues()[0].capabilities.Supports(Sora::Compute::BackendCommandKind::Copy));
    REQUIRE_FALSE(inventory.Queues()[0].reportsExecutionNanoseconds);
    REQUIRE(inventory.Queues()[1].reportsExecutionNanoseconds);
    REQUIRE(inventory.Memories().size() == 1);
    REQUIRE(inventory.Memories()[0].service == service);
    REQUIRE(inventory.Memories()[0].location == kDevice);
    REQUIRE(inventory.Memories()[0].supportedProperties == Sora::Compute::MemoryProperty::DeviceLocal);
    REQUIRE(inventory.Transfers().size() == 1);
    REQUIRE(inventory.Transfers()[0].engine == Sora::Compute::TransferEngineId{3, 1});
    REQUIRE(inventory.Compilers().size() == 1);
    REQUIRE(inventory.Compilers()[0].service == service);
    REQUIRE(inventory.Compilers()[0].compiler == Sora::Compute::SemanticHash{4, 5});
    REQUIRE(inventory.Compilers()[0].backend == Sora::Compute::BackendKind::Vulkan);
    REQUIRE(inventory.PipelineFactories().size() == 1);
    REQUIRE(inventory.PipelineFactories()[0].factory == Sora::Compute::SemanticHash{6, 7});
    REQUIRE(inventory.PipelineConfigurations().size() == 1);
    REQUIRE(inventory.PipelineConfigurations()[0] == pipelineConfiguration);
    const auto targetConfigurations =
        inventory.PipelineConfigurationsFor(service, Sora::Compute::BackendKind::Vulkan, pipelineConfiguration.device);
    REQUIRE(targetConfigurations.size() == 1);
    REQUIRE(targetConfigurations[0] == pipelineConfiguration);
    REQUIRE(inventory.PipelineConfigurationsFor(service, Sora::Compute::BackendKind::Cuda, pipelineConfiguration.device)
                .empty());
    REQUIRE(inventory.HasPipelineConfiguration(pipelineConfiguration));
    auto missingConfiguration = pipelineConfiguration;
    missingConfiguration.state = {10, 11};
    REQUIRE_FALSE(inventory.HasPipelineConfiguration(missingConfiguration));
}

TEST_CASE("calibrated transfer topology joins physical edges with exact service-scoped measurements",
          "[Sora][Compute][Runtime][TransferTopology]") {
    auto allocator = MakeAllocator(kDevice);
    auto transfer = MakeTransfer(3);
    auto catalog = Sora::Compute::PhysicalResourceCatalog::Create({
        .deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
        .allocators = {allocator},
        .transferEngines = {transfer},
    });
    REQUIRE(catalog.has_value());

    const auto uncalibrated = Sora::Compute::ProjectCalibratedTransferTopology(
        catalog->Inventory(), Sora::Compute::PerformanceSnapshot::Empty(), 1'000'000);
    REQUIRE(uncalibrated.has_value());
    REQUIRE(uncalibrated->Links().empty());

    Sora::Compute::PerformanceModel model;
    const Sora::Compute::TransferCalibrationKey key{
        .service = Sora::Test::TestBackendService().Id(), .engine = {3, 1}, .source = kHost, .destination = kDevice};
    REQUIRE(model.Update({.key = key, .bytes = 1'000'000, .payloadNanoseconds = 100'000, .latencyNanoseconds = 5'000})
                .has_value());
    const auto performance = model.Snapshot();
    const auto calibrated =
        Sora::Compute::ProjectCalibratedTransferTopology(catalog->Inventory(), *performance, 1'000'000);
    REQUIRE(calibrated.has_value());
    REQUIRE(calibrated->Bytes() == 1'000'000);
    REQUIRE(calibrated->Links().size() == 1);
    REQUIRE(calibrated->Links().front().engine == Sora::Compute::TransferEngineId{3, 1});
    REQUIRE(calibrated->Links().front().bytesPerSecond == 10'000'000'000);
    REQUIRE(calibrated->Links().front().latencyNanoseconds == 5'000);

    const auto invalid = Sora::Compute::ProjectCalibratedTransferTopology(catalog->Inventory(), *performance, 0);
    REQUIRE(invalid.error() == Sora::Compute::CalibratedTransferTopologyError::InvalidRequest);
}

TEST_CASE("candidate synthesis joins immutable device, queue, artifact, and calibration facts",
          "[Sora][Compute][Runtime][CandidateSynthesis]") {
    auto queue = MakeQueue(9, Sora::Compute::QueueCapability::Dispatch);
    auto pipelineQueue =
        MakeQueue(10, Sora::Compute::QueueCapability::Dispatch, Sora::Compute::ArtifactKind::DevicePipeline);
    auto compiler = MakeCompiler({10, 11});
    auto pipelineFactory = MakePipelineFactory({30, 31});
    const auto configurationA = pipelineFactory.Configuration({32, 33});
    const auto configurationB = pipelineFactory.Configuration({34, 35});
    auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
        {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
         .queues = {queue, pipelineQueue},
         .compilers = {compiler},
         .pipelineFactories = {pipelineFactory},
         .pipelineConfigurations = {configurationB, configurationA}});
    REQUIRE(catalog.has_value());

    const Sora::Compute::DeviceDescriptor descriptor{.id = kGpu,
                                                     .backend = Sora::Compute::BackendKind::Vulkan,
                                                     .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                                                     .architectureId = 2,
                                                     .subgroupWidthMinimum = 1,
                                                     .subgroupWidthMaximum = 32,
                                                     .maximumWorkgroupInvocations = 1024};
    const Sora::Compute::TargetFingerprint target{.device = kGpu,
                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                  .hostArchitecture = Sora::CpuArchitecture::X64,
                                                  .operatingSystem = Sora::OperatingSystem::Windows,
                                                  .binaryFormat = Sora::BinaryFormat::PE,
                                                  .byteOrder = Sora::ByteOrder::Little,
                                                  .abi = Sora::AbiFamily::MSVC,
                                                  .identityStability =
                                                      Sora::Compute::TargetIdentityStability::MachineStable,
                                                  .addressBits = 64,
                                                  .architectureId = 2,
                                                  .compilerIdentity = {10, 11},
                                                  .driverIdentity = {12, 13}};
    const std::array devices{Sora::Compute::RuntimeDeviceBinding{.service = Sora::Test::TestBackendService().Id(),
                                                                 .descriptor = descriptor,
                                                                 .target = Sora::Compute::RuntimeTarget(target)}};
    const std::array snapshots{
        Sora::Compute::DeviceSnapshot{.id = kGpu, .state = Sora::Compute::DeviceState::Available, .generation = 1}};
    const Sora::Compute::KernelCalibrationKey key{
        .kernel = {20, 21}, .target = target, .specialization = {}, .queue = {9, 1}};
    const Sora::Compute::KernelCalibrationKey pipelineKey{
        .kernel = {20, 21}, .target = target, .specialization = {}, .queue = {10, 1}};
    const Sora::Compute::ArtifactKey executable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = pipelineKey.kernel, .target = pipelineKey.target, .specialization = pipelineKey.specialization});
    const Sora::Compute::PipelineKey pipelineA{.executable = Sora::Compute::ArtifactIdentity(executable),
                                               .target = target,
                                               .builder = configurationA.factory,
                                               .state = configurationA.state,
                                               .bindingLayout = {36, 37}};
    const Sora::Compute::PipelineKey pipelineB{.executable = Sora::Compute::ArtifactIdentity(executable),
                                               .target = target,
                                               .builder = configurationB.factory,
                                               .state = configurationB.state,
                                               .bindingLayout = {36, 37}};
    Sora::Compute::PerformanceModel model;
    REQUIRE(model.Update({.key = key, .workItemCount = 4096, .computeNanoseconds = 2048, .launchNanoseconds = 300})
                .has_value());
    REQUIRE(
        model.Update({.key = pipelineKey, .workItemCount = 4096, .computeNanoseconds = 2048, .launchNanoseconds = 300})
            .has_value());
    const auto kernelPerformance = model.Snapshot();
    REQUIRE(model
                .Update({.key = Sora::Compute::MakeArtifactKey(pipelineA),
                         .path = Sora::Compute::ArtifactResolutionPath::TransientProduction,
                         .nanoseconds = 20'000})
                .has_value());
    const auto performance = model.Snapshot();
    const auto service = Sora::Test::TestBackendService().Id();
    const std::array artifacts{
        Sora::Compute::CandidateArtifactState{
            .service = service, .candidate = key, .executable = Sora::Compute::ArtifactResolutionMode::Live},
        Sora::Compute::CandidateArtifactState{
            .service = service,
            .candidate = pipelineKey,
            .executable = Sora::Compute::ArtifactResolutionMode::Live,
            .pipeline =
                Sora::Compute::PipelineArtifactState{
                    .key = pipelineA, .resolution = Sora::Compute::ArtifactResolutionMode::TransientProduction}},
        Sora::Compute::CandidateArtifactState{
            .service = service,
            .candidate = pipelineKey,
            .executable = Sora::Compute::ArtifactResolutionMode::Live,
            .pipeline = Sora::Compute::PipelineArtifactState{
                .key = pipelineB, .resolution = Sora::Compute::ArtifactResolutionMode::Live}}};
    const Sora::Compute::DispatchCandidateRequest request{
        .workload = {.kernelHash = {20, 21},
                     .resourceDemand = {.floatingPointOperations = 2, .bytesRead = 8, .bytesWritten = 4},
                     .workItemCount = 4096},
        .devices = devices,
        .snapshots = snapshots,
        .artifacts = artifacts};

    constexpr Sora::Compute::PerformancePriorModel priors;
    const auto priorCandidates = Sora::Compute::SynthesizeDispatchCandidates(
        request, catalog->Inventory(), Sora::Compute::PerformanceSnapshot::Empty(), &priors);
    REQUIRE(priorCandidates.has_value());
    REQUIRE(priorCandidates->size() == 3);
    REQUIRE(std::ranges::all_of(*priorCandidates, [](const auto& candidate) {
        return candidate.kernelEvidence.kind == Sora::Compute::PerformanceEvidenceKind::Prior &&
               candidate.kernelEvidence.sampleCount == 0;
    }));

    const auto candidates = Sora::Compute::SynthesizeDispatchCandidates(request, catalog->Inventory(), *performance);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->size() == 3);
    const auto executableCandidate = std::ranges::find_if(*candidates, [](const auto& candidate) {
        return candidate.queue == Sora::Compute::QueueId{9, 1} && !candidate.pipeline;
    });
    const auto coldPipeline = std::ranges::find_if(
        *candidates, [&](const auto& candidate) { return candidate.pipeline && candidate.pipeline->key == pipelineA; });
    const auto livePipeline = std::ranges::find_if(
        *candidates, [&](const auto& candidate) { return candidate.pipeline && candidate.pipeline->key == pipelineB; });
    REQUIRE(executableCandidate != candidates->end());
    REQUIRE(executableCandidate->target == target);
    REQUIRE(executableCandidate->executable.mode == Sora::Compute::ArtifactResolutionMode::Live);
    REQUIRE(executableCandidate->workItemsPerSecond == 2'000'000'000);
    REQUIRE(executableCandidate->kernelEvidence.kind == Sora::Compute::PerformanceEvidenceKind::Observed);
    REQUIRE(executableCandidate->kernelEvidence.sampleCount == 1);
    REQUIRE(coldPipeline != candidates->end());
    REQUIRE(coldPipeline->pipeline->resolution.mode == Sora::Compute::ArtifactResolutionMode::TransientProduction);
    REQUIRE(coldPipeline->pipeline->resolution.productionNanoseconds == 20'000);
    REQUIRE(livePipeline != candidates->end());
    REQUIRE(livePipeline->pipeline->resolution.mode == Sora::Compute::ArtifactResolutionMode::Live);
    REQUIRE(livePipeline->pipeline->resolution.productionNanoseconds == 0);

    const auto withoutColdCalibration =
        Sora::Compute::SynthesizeDispatchCandidates(request, catalog->Inventory(), *kernelPerformance);
    REQUIRE(withoutColdCalibration.has_value());
    REQUIRE(withoutColdCalibration->size() == 2);
    REQUIRE(std::ranges::none_of(*withoutColdCalibration, [&](const auto& candidate) {
        return candidate.pipeline && candidate.pipeline->key == pipelineA;
    }));

    auto transferRequest = request;
    transferRequest.requireTransferProjection = true;
    const auto transfer =
        Sora::Compute::SynthesizeDispatchCandidates(transferRequest, catalog->Inventory(), *performance);
    REQUIRE(transfer.error() == Sora::Compute::CandidateSynthesisError::TransferModelRequired);

    const std::array transferCosts{
        Sora::Compute::CandidateTransferState{.service = service, .candidate = key, .bytes = 1, .nanoseconds = 700},
        Sora::Compute::CandidateTransferState{
            .service = service, .candidate = pipelineKey, .bytes = 2, .nanoseconds = 800}};
    transferRequest.transfers = transferCosts;
    const auto projected =
        Sora::Compute::SynthesizeDispatchCandidates(transferRequest, catalog->Inventory(), *performance);
    REQUIRE(projected.has_value());
    REQUIRE(projected->size() == 3);
    REQUIRE(projected->front().transfer == Sora::Compute::CandidateTransferCost{1, 700});

    const std::array duplicateTransfers{transferCosts[0], transferCosts[0]};
    transferRequest.transfers = duplicateTransfers;
    const auto duplicateTransfer =
        Sora::Compute::SynthesizeDispatchCandidates(transferRequest, catalog->Inventory(), *performance);
    REQUIRE(duplicateTransfer.error() == Sora::Compute::CandidateSynthesisError::InvalidRequest);

    auto unexpectedTransferRequest = request;
    unexpectedTransferRequest.transfers = transferCosts;
    const auto unexpectedTransfer =
        Sora::Compute::SynthesizeDispatchCandidates(unexpectedTransferRequest, catalog->Inventory(), *performance);
    REQUIRE(unexpectedTransfer.error() == Sora::Compute::CandidateSynthesisError::InvalidRequest);

    auto mismatchedDevices = devices;
    mismatchedDevices[0].target.architectureId = 99;
    auto mismatchedRequest = request;
    mismatchedRequest.devices = mismatchedDevices;
    const auto mismatched =
        Sora::Compute::SynthesizeDispatchCandidates(mismatchedRequest, catalog->Inventory(), *performance);
    REQUIRE(mismatched->empty());

    const std::array duplicateArtifacts{artifacts[0], artifacts[0]};
    auto duplicateRequest = request;
    duplicateRequest.artifacts = duplicateArtifacts;
    const auto duplicate =
        Sora::Compute::SynthesizeDispatchCandidates(duplicateRequest, catalog->Inventory(), *performance);
    REQUIRE(duplicate.error() == Sora::Compute::CandidateSynthesisError::InvalidRequest);

    auto unsupportedPipeline = pipelineA;
    unsupportedPipeline.builder = {90, 91};
    const std::array unsupportedArtifacts{
        artifacts[0], Sora::Compute::CandidateArtifactState{
                          .service = service,
                          .candidate = pipelineKey,
                          .executable = Sora::Compute::ArtifactResolutionMode::Live,
                          .pipeline = Sora::Compute::PipelineArtifactState{
                              .key = unsupportedPipeline, .resolution = Sora::Compute::ArtifactResolutionMode::Live}}};
    auto unsupportedRequest = request;
    unsupportedRequest.artifacts = unsupportedArtifacts;
    const auto unsupported =
        Sora::Compute::SynthesizeDispatchCandidates(unsupportedRequest, catalog->Inventory(), *performance);
    REQUIRE(unsupported.has_value());
    REQUIRE(unsupported->size() == 1);
    REQUIRE_FALSE(unsupported->front().pipeline.has_value());

    auto foreignArtifacts = artifacts;
    foreignArtifacts[1].service = {92, 93};
    foreignArtifacts[2].service = {92, 93};
    auto foreignRequest = request;
    foreignRequest.artifacts = foreignArtifacts;
    const auto foreign =
        Sora::Compute::SynthesizeDispatchCandidates(foreignRequest, catalog->Inventory(), *performance);
    REQUIRE(foreign.has_value());
    REQUIRE(foreign->size() == 1);
    REQUIRE_FALSE(foreign->front().pipeline.has_value());
}

TEST_CASE("candidate synthesis orders cold devices by workload and lets exact observations dominate",
          "[Sora][Compute][Runtime][CandidateSynthesis][Prior]") {
    constexpr Sora::Compute::SemanticHash kernel{120, 121};
    constexpr Sora::Compute::SemanticHash cpuCompilerId{5, 6};
    constexpr Sora::Compute::SemanticHash gpuCompilerId{10, 11};
    auto cpuService = MakeService({122, 123});
    const auto& gpuService = Sora::Test::TestBackendService();
    auto cpuQueue = MakeQueue(cpuService, kCpu, Sora::Compute::BackendKind::Cpu, 8,
                              Sora::Compute::QueueCapability::Dispatch, Sora::Compute::ArtifactKind::DeviceExecutable);
    auto gpuQueue = MakeQueue(9, Sora::Compute::QueueCapability::Dispatch);
    auto cpuCompiler = MakeCompiler(cpuService, Sora::Compute::BackendKind::Cpu, cpuCompilerId);
    auto gpuCompiler = MakeCompiler(gpuCompilerId);
    auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
        {.deviceObservers = {MakeCpuObserver(cpuService), MakeObserver(gpuService)},
         .queues = {cpuQueue, gpuQueue},
         .compilers = {cpuCompiler, gpuCompiler}});
    REQUIRE(catalog.has_value());

    constexpr Sora::Compute::DeviceDescriptor cpu{.id = kCpu,
                                                  .backend = Sora::Compute::BackendKind::Cpu,
                                                  .kind = Sora::Compute::DeviceKind::Cpu,
                                                  .architectureId = 1,
                                                  .computeUnitCount = 1,
                                                  .subgroupWidthMinimum = 1,
                                                  .subgroupWidthMaximum = 1,
                                                  .maximumWorkgroupInvocations = 1};
    constexpr Sora::Compute::DeviceDescriptor gpu{.id = kGpu,
                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                  .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                                                  .architectureId = 2,
                                                  .computeUnitCount = 1,
                                                  .subgroupWidthMinimum = 1,
                                                  .subgroupWidthMaximum = 32,
                                                  .maximumWorkgroupInvocations = 1024};
    constexpr auto cpuRuntimeTarget = Sora::Compute::RuntimeTarget(CpuTarget());
    constexpr auto gpuRuntimeTarget = Sora::Compute::RuntimeTarget(GpuTarget());
    constexpr auto cpuTarget = Sora::Compute::ComposeTarget(cpuRuntimeTarget, cpuCompilerId,
                                                            Sora::Compute::ExecutableTargetDomain::TargetNative);
    constexpr auto gpuTarget = Sora::Compute::ComposeTarget(gpuRuntimeTarget, gpuCompilerId,
                                                            Sora::Compute::ExecutableTargetDomain::TargetNative);
    constexpr Sora::Compute::KernelCalibrationKey cpuKey{.kernel = kernel, .target = cpuTarget, .queue = {8, 1}};
    constexpr Sora::Compute::KernelCalibrationKey gpuKey{.kernel = kernel, .target = gpuTarget, .queue = {9, 1}};
    const std::array devices{
        Sora::Compute::RuntimeDeviceBinding{.service = cpuService.Id(), .descriptor = cpu, .target = cpuRuntimeTarget},
        Sora::Compute::RuntimeDeviceBinding{.service = gpuService.Id(), .descriptor = gpu, .target = gpuRuntimeTarget}};
    const std::array snapshots{
        Sora::Compute::DeviceSnapshot{.id = kCpu, .state = Sora::Compute::DeviceState::Available, .generation = 1},
        Sora::Compute::DeviceSnapshot{.id = kGpu, .state = Sora::Compute::DeviceState::Available, .generation = 1}};
    const std::array artifacts{
        Sora::Compute::CandidateArtifactState{
            .service = cpuService.Id(), .candidate = cpuKey, .executable = Sora::Compute::ArtifactResolutionMode::Live},
        Sora::Compute::CandidateArtifactState{.service = gpuService.Id(),
                                              .candidate = gpuKey,
                                              .executable = Sora::Compute::ArtifactResolutionMode::Live}};
    constexpr auto priorPolicy = [] {
        Sora::Compute::PerformancePriorPolicy policy;
        auto& cpuPrior = policy.kernels[kCpuPriorIndex];
        cpuPrior.floatingPointOperationsPerSecondPerComputeUnit = 100;
        cpuPrior.memoryBytesPerSecondPerComputeUnit = 800;
        auto& gpuPrior = policy.kernels[kDiscreteGpuPriorIndex];
        gpuPrior.floatingPointOperationsPerSecondPerComputeUnit = 1'000;
        gpuPrior.memoryBytesPerSecondPerComputeUnit = 80;
        return policy;
    }();
    constexpr Sora::Compute::PerformancePriorModel priors{priorPolicy};
    Sora::Compute::DispatchCandidateRequest request{.workload = {.kernelHash = kernel,
                                                                 .resourceDemand = {.bytesRead = 4, .bytesWritten = 4},
                                                                 .workItemCount = 1'000},
                                                    .devices = devices,
                                                    .snapshots = snapshots,
                                                    .artifacts = artifacts};
    const auto findDevice = [](const auto& candidates, Sora::Compute::DeviceId device) {
        return std::ranges::find_if(candidates,
                                    [device](const auto& candidate) { return candidate.device.id == device; });
    };

    const auto copyCandidates = Sora::Compute::SynthesizeDispatchCandidates(
        request, catalog->Inventory(), Sora::Compute::PerformanceSnapshot::Empty(), &priors);
    REQUIRE(copyCandidates.has_value());
    REQUIRE(copyCandidates->size() == 2);
    const auto copyCpu = findDevice(*copyCandidates, kCpu);
    const auto copyGpu = findDevice(*copyCandidates, kGpu);
    REQUIRE(copyCpu != copyCandidates->end());
    REQUIRE(copyGpu != copyCandidates->end());
    REQUIRE(copyCpu->workItemsPerSecond > copyGpu->workItemsPerSecond);

    request.workload.resourceDemand = {.floatingPointOperations = 100, .bytesRead = 4, .bytesWritten = 4};
    const auto computeCandidates = Sora::Compute::SynthesizeDispatchCandidates(
        request, catalog->Inventory(), Sora::Compute::PerformanceSnapshot::Empty(), &priors);
    REQUIRE(computeCandidates.has_value());
    const auto computeCpu = findDevice(*computeCandidates, kCpu);
    const auto computeGpu = findDevice(*computeCandidates, kGpu);
    REQUIRE(computeCpu != computeCandidates->end());
    REQUIRE(computeGpu != computeCandidates->end());
    REQUIRE(computeGpu->workItemsPerSecond > computeCpu->workItemsPerSecond);

    Sora::Compute::PerformanceModel model;
    REQUIRE(model.Update({.key = cpuKey, .workItemCount = 1'000, .computeNanoseconds = 100}).has_value());
    const auto observedCandidates =
        Sora::Compute::SynthesizeDispatchCandidates(request, catalog->Inventory(), *model.Snapshot(), &priors);
    REQUIRE(observedCandidates.has_value());
    const auto observedCpu = findDevice(*observedCandidates, kCpu);
    const auto priorGpu = findDevice(*observedCandidates, kGpu);
    REQUIRE(observedCpu != observedCandidates->end());
    REQUIRE(priorGpu != observedCandidates->end());
    REQUIRE(observedCpu->kernelEvidence.kind == Sora::Compute::PerformanceEvidenceKind::Observed);
    REQUIRE(priorGpu->kernelEvidence.kind == Sora::Compute::PerformanceEvidenceKind::Prior);
    REQUIRE(observedCpu->workItemsPerSecond > priorGpu->workItemsPerSecond);

    auto invalidRequest = request;
    invalidRequest.workload.resourceDemand = {};
    const auto invalidSynthesis = Sora::Compute::SynthesizeDispatchCandidates(
        invalidRequest, catalog->Inventory(), Sora::Compute::PerformanceSnapshot::Empty(), &priors);
    REQUIRE(invalidSynthesis.error() == Sora::Compute::CandidateSynthesisError::InvalidRequest);
    REQUIRE(Sora::Compute::PlanExecution(invalidRequest.workload, *copyCandidates).error() ==
            Sora::Compute::PlanningError::InvalidWorkload);

    invalidRequest.workload.resourceDemand = {.bytesRead = 4, .stridedBytesRead = 8};
    const auto invalidLayoutDemand = Sora::Compute::SynthesizeDispatchCandidates(
        invalidRequest, catalog->Inventory(), Sora::Compute::PerformanceSnapshot::Empty(), &priors);
    REQUIRE(invalidLayoutDemand.error() == Sora::Compute::CandidateSynthesisError::InvalidRequest);
}

TEST_CASE("candidate synthesis enumerates canonical compiler variants on one physical device",
          "[Sora][Compute][Runtime][CandidateSynthesis][Compiler]") {
    constexpr Sora::Compute::SemanticHash portableCompiler{5, 5};
    constexpr Sora::Compute::SemanticHash nativeCompiler{10, 11};
    auto queue = MakeQueue(9, Sora::Compute::QueueCapability::Dispatch);
    auto portable = MakeCompiler(portableCompiler, Sora::Compute::ExecutableTargetDomain::BackendPortable);
    auto native = MakeCompiler(nativeCompiler, Sora::Compute::ExecutableTargetDomain::TargetNative);
    auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
        {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
         .queues = {queue},
         .compilers = {native, portable}});
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->Inventory()
                .CompilersFor(Sora::Test::TestBackendService().Id(), Sora::Compute::BackendKind::Vulkan)
                .size() == 2);

    const auto runtimeTarget = Sora::Compute::RuntimeTarget(GpuTarget());
    const auto portableTarget = Sora::Compute::ComposeTarget(runtimeTarget, portableCompiler,
                                                             Sora::Compute::ExecutableTargetDomain::BackendPortable);
    const auto nativeTarget = Sora::Compute::ComposeTarget(runtimeTarget, nativeCompiler,
                                                           Sora::Compute::ExecutableTargetDomain::TargetNative);
    constexpr Sora::Compute::SemanticHash kernel{20, 21};
    constexpr Sora::Compute::QueueId queueId{9, 1};
    const Sora::Compute::KernelCalibrationKey portableKey{.kernel = kernel, .target = portableTarget, .queue = queueId};
    const Sora::Compute::KernelCalibrationKey nativeKey{.kernel = kernel, .target = nativeTarget, .queue = queueId};

    Sora::Compute::PerformanceModel model;
    REQUIRE(model.Update({.key = portableKey, .workItemCount = 1'000, .computeNanoseconds = 1'000}).has_value());
    REQUIRE(model.Update({.key = nativeKey, .workItemCount = 1'000, .computeNanoseconds = 100}).has_value());
    const auto nativeArtifact = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = nativeKey.kernel, .target = nativeKey.target, .specialization = nativeKey.specialization});
    REQUIRE(model
                .Update({.key = nativeArtifact,
                         .path = Sora::Compute::ArtifactResolutionPath::TransientProduction,
                         .nanoseconds = 50'000})
                .has_value());
    const auto performance = model.Snapshot();
    REQUIRE(performance);

    const auto service = Sora::Test::TestBackendService().Id();
    const std::array devices{
        Sora::Compute::RuntimeDeviceBinding{.service = service,
                                            .descriptor = {.id = kGpu,
                                                           .backend = Sora::Compute::BackendKind::Vulkan,
                                                           .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                                                           .architectureId = 2,
                                                           .subgroupWidthMinimum = 1,
                                                           .subgroupWidthMaximum = 32,
                                                           .maximumWorkgroupInvocations = 1024},
                                            .target = runtimeTarget}};
    const std::array snapshots{
        Sora::Compute::DeviceSnapshot{.id = kGpu, .state = Sora::Compute::DeviceState::Available, .generation = 1}};
    const std::array artifacts{Sora::Compute::CandidateArtifactState{
        .service = service, .candidate = portableKey, .executable = Sora::Compute::ArtifactResolutionMode::Live}};
    const Sora::Compute::WorkloadDesc workload{.kernelHash = kernel,
                                               .resourceDemand = {.bytesRead = 4, .bytesWritten = 4},
                                               .workItemCount = 1'000,
                                               .expectedReuseCount = 1};
    const Sora::Compute::DispatchCandidateRequest request{
        .workload = workload, .devices = devices, .snapshots = snapshots, .artifacts = artifacts};

    const auto candidates = Sora::Compute::SynthesizeDispatchCandidates(request, catalog->Inventory(), *performance);
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->size() == 2);
    REQUIRE((*candidates)[0].target == portableTarget);
    REQUIRE((*candidates)[0].executable.mode == Sora::Compute::ArtifactResolutionMode::Live);
    REQUIRE((*candidates)[1].target == nativeTarget);
    REQUIRE((*candidates)[1].executable.mode == Sora::Compute::ArtifactResolutionMode::TransientProduction);

    const auto coldPlan = Sora::Compute::PlanExecution(workload, *candidates);
    REQUIRE(coldPlan.has_value());
    REQUIRE(coldPlan->target == portableTarget);
    auto reusedWorkload = workload;
    reusedWorkload.expectedReuseCount = 100;
    const auto reusedPlan = Sora::Compute::PlanExecution(reusedWorkload, *candidates);
    REQUIRE(reusedPlan.has_value());
    REQUIRE(reusedPlan->target == nativeTarget);

    std::array tied{(*candidates)[1], (*candidates)[0]};
    for (auto& candidate : tied) {
        candidate.executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live};
        candidate.workItemsPerSecond = 1'000'000'000;
    }
    const auto canonicalTie = Sora::Compute::PlanExecution(workload, tied);
    REQUIRE(canonicalTie.has_value());
    REQUIRE(canonicalTie->target == portableTarget);
}

TEST_CASE("PhysicalResourceCatalog rejects duplicate identities", "[Sora][Compute][Runtime][Validation]") {
    auto queue = MakeQueue(1, Sora::Compute::QueueCapability::Dispatch);
    auto allocator = MakeAllocator(kDevice);
    auto transfer = MakeTransfer(2);
    auto compiler = MakeCompiler({3, 4});
    auto pipelineFactory = MakePipelineFactory({5, 6});
    const auto pipelineConfiguration = pipelineFactory.Configuration({7, 8});

    SECTION("invalid device observer") {
        const auto catalog =
            Sora::Compute::PhysicalResourceCatalog::Create({.deviceObservers = {Sora::Compute::DeviceObserver{}}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::InvalidDeviceObserver);
    }
    SECTION("duplicate device observer") {
        auto observer = MakeObserver(Sora::Test::TestBackendService());
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create({.deviceObservers = {observer, observer}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::DuplicateDeviceObserver);
    }
    SECTION("missing device observer") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create({.queues = {queue}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::MissingDeviceObserver);
    }
    SECTION("queue") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())}, .queues = {queue, queue}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::DuplicateQueue);
    }
    SECTION("allocator") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
             .allocators = {allocator, allocator}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::DuplicateAllocator);
    }
    SECTION("transfer engine") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
             .allocators = {allocator},
             .transferEngines = {transfer, transfer}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::DuplicateTransferEngine);
    }
    SECTION("compiler") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())}, .compilers = {compiler, compiler}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::DuplicateCompiler);
    }
    SECTION("pipeline factory") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
             .pipelineFactories = {pipelineFactory, pipelineFactory}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::DuplicatePipelineFactory);
    }
    SECTION("invalid pipeline configuration") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.pipelineConfigurations = {Sora::Compute::PipelineConfiguration{}}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::InvalidPipelineConfiguration);
    }
    SECTION("duplicate pipeline configuration") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
             .pipelineFactories = {pipelineFactory},
             .pipelineConfigurations = {pipelineConfiguration, pipelineConfiguration}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::DuplicatePipelineConfiguration);
    }
    SECTION("missing pipeline factory") {
        const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
            {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())},
             .pipelineConfigurations = {pipelineConfiguration}});
        REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::MissingPipelineFactory);
    }
}

TEST_CASE("PhysicalResourceCatalog isolates equal provider identities by backend service",
          "[Sora][Compute][Runtime][PhysicalResourceCatalog][Service]") {
    constexpr Sora::Compute::SemanticHash identity{11, 12};
    const auto& firstService = Sora::Test::TestBackendService();
    auto secondService = MakeService({13, 14});
    auto firstCompiler = MakeCompiler(identity);
    auto secondCompiler = MakeCompiler(secondService, identity);
    auto firstFactory = MakePipelineFactory(identity);
    auto secondFactory = MakePipelineFactory(secondService, identity);
    const auto firstConfiguration = firstFactory.Configuration({15, 16});
    const auto secondConfiguration = secondFactory.Configuration({15, 16});

    auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
        {.deviceObservers = {MakeObserver(firstService), MakeObserver(secondService)},
         .compilers = {firstCompiler, secondCompiler},
         .pipelineFactories = {firstFactory, secondFactory},
         .pipelineConfigurations = {secondConfiguration, firstConfiguration}});
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->FindCompiler(firstService.Id(), Sora::Compute::BackendKind::Vulkan, identity)->Service() ==
            firstService.Id());
    REQUIRE(catalog->FindCompiler(secondService.Id(), Sora::Compute::BackendKind::Vulkan, identity)->Service() ==
            secondService.Id());
    REQUIRE(catalog->FindPipelineFactory(firstService.Id(), Sora::Compute::BackendKind::Vulkan, kGpu, identity)
                ->Service() == firstService.Id());
    REQUIRE(catalog->FindPipelineFactory(secondService.Id(), Sora::Compute::BackendKind::Vulkan, kGpu, identity)
                ->Service() == secondService.Id());
    const auto& inventory = catalog->Inventory();
    const auto firstConfigurations =
        inventory.PipelineConfigurationsFor(firstService.Id(), Sora::Compute::BackendKind::Vulkan, kGpu);
    const auto secondConfigurations =
        inventory.PipelineConfigurationsFor(secondService.Id(), Sora::Compute::BackendKind::Vulkan, kGpu);
    REQUIRE(firstConfigurations.size() == 1);
    REQUIRE(firstConfigurations[0] == firstConfiguration);
    REQUIRE(secondConfigurations.size() == 1);
    REQUIRE(secondConfigurations[0] == secondConfiguration);
}

TEST_CASE("PhysicalResourceCatalog service-scopes equal allocator and transfer identities",
          "[Sora][Compute][Runtime][PhysicalResourceCatalog][Service]") {
    const auto& firstService = Sora::Test::TestBackendService();
    auto secondService = MakeService({21, 22});
    auto firstAllocator = MakeAllocator(firstService, kDevice);
    auto secondAllocator = MakeAllocator(secondService, kDevice);
    auto firstTransfer = MakeTransfer(firstService, firstService.Id(), 23);
    auto secondTransfer = MakeTransfer(secondService, firstService.Id(), 23);

    auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
        {.deviceObservers = {MakeObserver(firstService), MakeObserver(secondService)},
         .allocators = {secondAllocator, firstAllocator},
         .transferEngines = {secondTransfer, firstTransfer}});
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->FindAllocator(firstService.Id(), kDevice)->Service() == firstService.Id());
    REQUIRE(catalog->FindAllocator(secondService.Id(), kDevice)->Service() == secondService.Id());
    REQUIRE(catalog->FindTransferEngine(firstService.Id(), {23, 1})->Service() == firstService.Id());
    REQUIRE(catalog->FindTransferEngine(secondService.Id(), {23, 1})->Service() == secondService.Id());
    REQUIRE(catalog->Inventory().Memories().size() == 2);
    REQUIRE(catalog->Inventory().Transfers().size() == 2);
}

TEST_CASE("PhysicalResourceCatalog requires observers for both transfer endpoint services",
          "[Sora][Compute][Runtime][PhysicalResourceCatalog][Service][Validation]") {
    const auto& destinationService = Sora::Test::TestBackendService();
    auto sourceService = MakeService({24, 25});
    auto transfer = MakeTransfer(destinationService, sourceService.Id(), 26);
    const auto catalog =
        Sora::Compute::PhysicalResourceCatalog::Create({.deviceObservers = {MakeObserver(destinationService)},
                                                        .allocators = {MakeAllocator(destinationService, kDevice)},
                                                        .transferEngines = {transfer}});
    REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::MissingDeviceObserver);
}

TEST_CASE("PhysicalResourceCatalog requires allocation for non-alias transfer "
          "destinations",
          "[Sora][Compute][Runtime][Topology]") {
    const auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(
        {.deviceObservers = {MakeObserver(Sora::Test::TestBackendService())}, .transferEngines = {MakeTransfer(9)}});
    REQUIRE(catalog.error() == Sora::Compute::PhysicalResourceCatalogError::MissingDestinationAllocator);
}
