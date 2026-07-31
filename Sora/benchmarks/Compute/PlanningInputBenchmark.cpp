/**
 * @file PlanningInputBenchmark.cpp
 * @brief Fresh-attempt baselines for immutable planning inputs and exact
 * dispatch-candidate enumeration.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/ArtifactCache.h>
#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/CalibratedTransferTopology.h>
#include <Sora/Compute/Runtime/CandidateSynthesis.h>
#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>
#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>
#include <Sora/Compute/Runtime/PipelineBindingLayout.h>

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
    constexpr size_t kArtifactCount = 64;
    constexpr uint64_t kTransferBytes = 1'000'000;
    constexpr uint64_t kElementCount = kTransferBytes / sizeof(float);
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr Sora::Compute::BackendServiceId kServiceId{100, 101};
    constexpr Sora::Compute::DeviceId kHostDevice{1, 1};
    constexpr Sora::Compute::DeviceId kGpu{2, 2};
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {10, 10}, .device = kHostDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kDevice{
        .domain = {20, 20}, .device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::QueueId kExecutableQueue{9, 1};
    constexpr Sora::Compute::QueueId kPipelineQueue{10, 1};
    constexpr Sora::Compute::SemanticHash kAlternativeCompiler{8, 9};
    constexpr Sora::Compute::SemanticHash kPipelineFactory{14, 15};
    constexpr Sora::Compute::SemanticHash kPipelineState{16, 17};
    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

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
        *snapshot = {.id = kGpu,
                     .state = Sora::Compute::DeviceState::Available,
                     .generation = 1,
                     .availableMemoryBytes = uint64_t{8} << 30U};
    }

    constexpr Sora::Compute::DeviceDescriptor DeviceDescriptor() noexcept {
        return {.id = kGpu,
                .backend = Sora::Compute::BackendKind::Vulkan,
                .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                .architectureId = 2,
                .totalMemoryBytes = uint64_t{8} << 30U,
                .maximumAllocationBytes = uint64_t{4} << 30U,
                .subgroupWidthMinimum = 1,
                .subgroupWidthMaximum = 32,
                .maximumWorkgroupInvocations = 1024};
    }

    constexpr Sora::Compute::TargetFingerprint Target(
        Sora::Compute::SemanticHash compiler = {10, 11},
        Sora::Compute::ExecutableTargetDomain domain = Sora::Compute::ExecutableTargetDomain::TargetNative) noexcept {
        return {.device = kGpu,
                .backend = Sora::Compute::BackendKind::Vulkan,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                .executableDomain = domain,
                .addressBits = 64,
                .architectureId = 2,
                .compilerIdentity = compiler,
                .driverIdentity = {12, 13}};
    }

    constexpr Sora::Compute::KernelCalibrationKey
    CandidateKey(Sora::Compute::TargetFingerprint target = Target()) noexcept {
        return {.kernel = kCopyProgram.Hash(), .target = target, .queue = kExecutableQueue};
    }

    constexpr Sora::Compute::KernelCalibrationKey PipelineCandidateKey() noexcept {
        return {.kernel = kCopyProgram.Hash(), .target = Target(), .queue = kPipelineQueue};
    }

    constexpr Sora::Compute::ArtifactKey
    ExecutableArtifact(const Sora::Compute::KernelCalibrationKey& candidate) noexcept {
        return Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = candidate.kernel, .target = candidate.target, .specialization = candidate.specialization});
    }

    constexpr Sora::Compute::PipelineKey PipelineKey() noexcept {
        constexpr auto candidate = PipelineCandidateKey();
        constexpr auto executable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = candidate.kernel, .target = candidate.target, .specialization = candidate.specialization});
        return {.executable = Sora::Compute::ArtifactIdentity(executable),
                .target = candidate.target,
                .builder = kPipelineFactory,
                .state = kPipelineState,
                .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(kCopyProgram.Arguments())};
    }

    class PlanningInputFixture final {
    public:
        PlanningInputFixture() : artifacts_(kArtifactCount) {
            auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
            auto service = Sora::Compute::BackendService::Adopt(
                {.id = kServiceId, .state = new uint32_t{}, .destroy = DestroyOpaque}, std::move(code));
            if (!service) {
                failed_ = true;
                return;
            }
            service_ = std::move(*service);
            if (!PopulateArtifacts() || !CreateCatalog(catalog_, false) ||
                !CreateCatalog(multiCompilerCatalog_, true) || !CreatePipelineCatalog() || !PopulatePerformance() ||
                !CreateGraph() || !FreezePrograms()) {
                failed_ = true;
                return;
            }
            artifactAvailability_ = artifacts_.SnapshotAvailability();
            failed_ = !artifactAvailability_;
        }

        void SnapshotArtifacts() noexcept {
            const auto snapshot = artifacts_.SnapshotAvailability();
            checksum_ += snapshot ? snapshot->Keys().size() : 0;
        }

        void ProjectTopology() noexcept {
            const auto topology = Sora::Compute::ProjectCalibratedTransferTopology(
                catalog_->Inventory(), *performanceSnapshot_, kTransferBytes);
            checksum_ += topology ? topology->Links().size() + topology->Bytes() : 0;
        }

        void SynthesizeCandidate() noexcept {
            const std::array devices{
                Sora::Compute::RuntimeDeviceBinding{.service = kServiceId,
                                                    .descriptor = DeviceDescriptor(),
                                                    .target = Sora::Compute::RuntimeTarget(Target())}};
            const std::array snapshots{Sora::Compute::DeviceSnapshot{.id = kGpu,
                                                                     .state = Sora::Compute::DeviceState::Available,
                                                                     .generation = 1,
                                                                     .availableMemoryBytes = uint64_t{8} << 30U}};
            const std::array transfers{Sora::Compute::CandidateTransferState{
                .service = kServiceId, .candidate = CandidateKey(), .bytes = kTransferBytes, .nanoseconds = 105'000}};
            const Sora::Compute::DispatchCandidateRequest request{
                .workload = {.kernelHash = kCopyProgram.Hash(),
                             .resourceDemand = kCopyProgram.ResourceDemand(),
                             .workItemCount = 4096,
                             .requiredDeviceMemoryBytes = kTransferBytes,
                             .largestAllocationBytes = kTransferBytes},
                .devices = devices,
                .snapshots = snapshots,
                .transfers = transfers,
                .requireTransferProjection = true};
            const auto candidates =
                Sora::Compute::SynthesizeDispatchCandidates(request, catalog_->Inventory(), *performanceSnapshot_);
            checksum_ +=
                candidates && !candidates->empty() ? candidates->front().transfer->nanoseconds + candidates->size() : 0;
        }

        void SynthesizePipelineCandidate() noexcept {
            const std::array devices{
                Sora::Compute::RuntimeDeviceBinding{.service = kServiceId,
                                                    .descriptor = DeviceDescriptor(),
                                                    .target = Sora::Compute::RuntimeTarget(Target())}};
            const std::array snapshots{Sora::Compute::DeviceSnapshot{.id = kGpu,
                                                                     .state = Sora::Compute::DeviceState::Available,
                                                                     .generation = 1,
                                                                     .availableMemoryBytes = uint64_t{8} << 30U}};
            const std::array artifacts{Sora::Compute::CandidateArtifactState{
                .service = kServiceId,
                .candidate = PipelineCandidateKey(),
                .executable = Sora::Compute::ArtifactResolutionMode::TransientProduction,
                .pipeline = Sora::Compute::PipelineArtifactState{
                    .key = PipelineKey(), .resolution = Sora::Compute::ArtifactResolutionMode::TransientProduction}}};
            const std::array transfers{Sora::Compute::CandidateTransferState{.service = kServiceId,
                                                                             .candidate = PipelineCandidateKey(),
                                                                             .bytes = kTransferBytes,
                                                                             .nanoseconds = 105'000}};
            const Sora::Compute::DispatchCandidateRequest request{
                .workload = {.kernelHash = kCopyProgram.Hash(),
                             .resourceDemand = kCopyProgram.ResourceDemand(),
                             .workItemCount = 4096,
                             .requiredDeviceMemoryBytes = kTransferBytes,
                             .largestAllocationBytes = kTransferBytes},
                .devices = devices,
                .snapshots = snapshots,
                .artifacts = artifacts,
                .transfers = transfers,
                .requireTransferProjection = true};
            const auto candidates = Sora::Compute::SynthesizeDispatchCandidates(request, pipelineCatalog_->Inventory(),
                                                                                *pipelinePerformanceSnapshot_);
            checksum_ += candidates && candidates->size() == 1 && candidates->front().pipeline
                             ? candidates->front().pipeline->resolution.productionNanoseconds +
                                   candidates->front().transfer->nanoseconds
                             : 0;
        }

        void SynthesizeMultiCompilerCandidate() noexcept {
            const auto alternativeTarget =
                Target(kAlternativeCompiler, Sora::Compute::ExecutableTargetDomain::BackendPortable);
            const std::array devices{
                Sora::Compute::RuntimeDeviceBinding{.service = kServiceId,
                                                    .descriptor = DeviceDescriptor(),
                                                    .target = Sora::Compute::RuntimeTarget(Target())}};
            const std::array snapshots{Sora::Compute::DeviceSnapshot{.id = kGpu,
                                                                     .state = Sora::Compute::DeviceState::Available,
                                                                     .generation = 1,
                                                                     .availableMemoryBytes = uint64_t{8} << 30U}};
            const std::array transfers{
                Sora::Compute::CandidateTransferState{.service = kServiceId,
                                                      .candidate = CandidateKey(),
                                                      .bytes = kTransferBytes,
                                                      .nanoseconds = 105'000},
                Sora::Compute::CandidateTransferState{.service = kServiceId,
                                                      .candidate = CandidateKey(alternativeTarget),
                                                      .bytes = kTransferBytes,
                                                      .nanoseconds = 105'000}};
            const Sora::Compute::DispatchCandidateRequest request{
                .workload = {.kernelHash = kCopyProgram.Hash(),
                             .resourceDemand = kCopyProgram.ResourceDemand(),
                             .workItemCount = 4096,
                             .requiredDeviceMemoryBytes = kTransferBytes,
                             .largestAllocationBytes = kTransferBytes},
                .devices = devices,
                .snapshots = snapshots,
                .transfers = transfers,
                .requireTransferProjection = true};
            const auto candidates = Sora::Compute::SynthesizeDispatchCandidates(
                request, multiCompilerCatalog_->Inventory(), *multiCompilerPerformanceSnapshot_);
            checksum_ += candidates && candidates->size() == 2
                             ? (*candidates)[0].transfer->nanoseconds + (*candidates)[1].transfer->nanoseconds +
                                   candidates->size()
                             : 0;
        }

        void EnumerateCandidate() noexcept {
            const std::array devices{
                Sora::Compute::RuntimeDeviceBinding{.service = kServiceId,
                                                    .descriptor = DeviceDescriptor(),
                                                    .target = Sora::Compute::RuntimeTarget(Target())}};
            const std::array snapshots{Sora::Compute::DeviceSnapshot{.id = kGpu,
                                                                     .state = Sora::Compute::DeviceState::Available,
                                                                     .generation = 1,
                                                                     .availableMemoryBytes = uint64_t{8} << 30U}};
            const Sora::Compute::GraphResidencyState residency;
            const auto candidates = Sora::Compute::EnumerateDispatchCandidates(
                *graph_, Sora::Compute::GraphNodeId{0},
                {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                {.packages = *programCatalog_,
                 .resources = catalog_->Inventory(),
                 .artifacts = *artifactAvailability_,
                 .performance = *performanceSnapshot_},
                residency);
            checksum_ += candidates && candidates->placements.size() == 1
                             ? candidates->placements.front().placement.transfer->nanoseconds +
                                   candidates->residencies.front().inputs.size()
                             : 0;
        }

        void EnumeratePipelineCandidate() noexcept {
            const std::array devices{
                Sora::Compute::RuntimeDeviceBinding{.service = kServiceId,
                                                    .descriptor = DeviceDescriptor(),
                                                    .target = Sora::Compute::RuntimeTarget(Target())}};
            const std::array snapshots{Sora::Compute::DeviceSnapshot{.id = kGpu,
                                                                     .state = Sora::Compute::DeviceState::Available,
                                                                     .generation = 1,
                                                                     .availableMemoryBytes = uint64_t{8} << 30U}};
            const Sora::Compute::GraphResidencyState residency;
            const auto candidates = Sora::Compute::EnumerateDispatchCandidates(
                *graph_, Sora::Compute::GraphNodeId{0},
                {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                {.packages = *programCatalog_,
                 .resources = pipelineCatalog_->Inventory(),
                 .artifacts = *artifactAvailability_,
                 .performance = *pipelinePerformanceSnapshot_},
                residency);
            checksum_ +=
                candidates && candidates->placements.size() == 1 && candidates->placements.front().placement.pipeline
                    ? candidates->placements.front().placement.pipeline->resolution.productionNanoseconds +
                          candidates->residencies.front().inputs.size()
                    : 0;
        }

        void EnumerateMultiCompilerCandidate() noexcept {
            const std::array devices{
                Sora::Compute::RuntimeDeviceBinding{.service = kServiceId,
                                                    .descriptor = DeviceDescriptor(),
                                                    .target = Sora::Compute::RuntimeTarget(Target())}};
            const std::array snapshots{Sora::Compute::DeviceSnapshot{.id = kGpu,
                                                                     .state = Sora::Compute::DeviceState::Available,
                                                                     .generation = 1,
                                                                     .availableMemoryBytes = uint64_t{8} << 30U}};
            const Sora::Compute::GraphResidencyState residency;
            const auto candidates = Sora::Compute::EnumerateDispatchCandidates(
                *graph_, Sora::Compute::GraphNodeId{0},
                {.epochSequence = 1, .devices = devices, .snapshots = snapshots},
                {.packages = *programCatalog_,
                 .resources = multiCompilerCatalog_->Inventory(),
                 .artifacts = *artifactAvailability_,
                 .performance = *multiCompilerPerformanceSnapshot_},
                residency);
            checksum_ += candidates && candidates->placements.size() == 2
                             ? candidates->placements[0].placement.transfer->nanoseconds +
                                   candidates->placements[1].placement.transfer->nanoseconds +
                                   candidates->residencies.size()
                             : 0;
        }

        [[nodiscard]] bool Failed() const noexcept {
            return failed_ || !service_ || !catalog_ || !multiCompilerCatalog_ || !pipelineCatalog_ ||
                   !performanceSnapshot_ || !multiCompilerPerformanceSnapshot_ || !pipelinePerformanceSnapshot_ ||
                   !artifactAvailability_ || !graph_;
        }

        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        [[nodiscard]] bool PopulateArtifacts() noexcept {
            for (uint64_t index = 1; index <= kArtifactCount; ++index) {
                const auto key = Sora::Compute::MakeArtifactKey(
                    Sora::Compute::LoweringKey{.module = {index, 1}, .lowerer = {30, 31}});
                auto artifact = Sora::Compute::Artifact::Adopt(
                    service_, {.key = key, .state = new uint32_t{}, .destroy = DestroyOpaque});
                if (!artifact || !artifacts_.Insert(std::move(*artifact))) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool CreateCatalog(std::optional<Sora::Compute::PhysicalResourceCatalog>& destination,
                                         bool includeAlternativeCompiler) noexcept {
            auto observer =
                Sora::Compute::DeviceObserver::Create(service_, {.device = kGpu,
                                                                 .backend = Sora::Compute::BackendKind::Vulkan,
                                                                 .state = service_.State(),
                                                                 .observe = ObserveGpu});
            auto queue =
                Sora::Compute::Queue::Adopt(service_, {.id = kExecutableQueue,
                                                       .device = kGpu,
                                                       .backend = Sora::Compute::BackendKind::Vulkan,
                                                       .capabilities = Sora::Compute::QueueCapability::Dispatch,
                                                       .state = new uint32_t{},
                                                       .destroy = DestroyOpaque,
                                                       .submit = Submit});
            auto allocator = Sora::Compute::MemoryAllocator::Create(
                service_, {.location = kDevice,
                           .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                           .state = service_.State(),
                           .allocate = Allocate});
            auto transfer = Sora::Compute::TransferEngine::Create(
                service_, {.engine = {3, 1},
                           .sourceService = service_.Id(),
                           .source = kHost,
                           .destination = kDevice,
                           .kind = Sora::Compute::TransferKind::Copy,
                           .completionDevice = kGpu,
                           .completionBackend = Sora::Compute::BackendKind::Vulkan,
                           .state = service_.State(),
                           .transfer = Transfer});
            auto compiler =
                Sora::Compute::BackendCompiler::Create(service_, {.identity = Target().compilerIdentity,
                                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                                  .executableDomain = Target().executableDomain,
                                                                  .state = service_.State(),
                                                                  .compile = Compile});
            if (!observer || !queue || !allocator || !transfer || !compiler) {
                return false;
            }
            Sora::Compute::PhysicalResourceCatalogStorage storage{
                .deviceObservers = {std::move(*observer)},
                .queues = {std::move(*queue)},
                .allocators = {std::move(*allocator)},
                .transferEngines = {std::move(*transfer)},
                .compilers = {std::move(*compiler)},
            };
            if (includeAlternativeCompiler) {
                auto alternative = Sora::Compute::BackendCompiler::Create(
                    service_, {.identity = kAlternativeCompiler,
                               .backend = Sora::Compute::BackendKind::Vulkan,
                               .executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable,
                               .state = service_.State(),
                               .compile = Compile});
                if (!alternative) {
                    return false;
                }
                storage.compilers.push_back(std::move(*alternative));
            }
            auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(std::move(storage));
            if (!catalog) {
                return false;
            }
            destination.emplace(std::move(*catalog));
            return true;
        }

        [[nodiscard]] bool CreatePipelineCatalog() noexcept {
            auto observer =
                Sora::Compute::DeviceObserver::Create(service_, {.device = kGpu,
                                                                 .backend = Sora::Compute::BackendKind::Vulkan,
                                                                 .state = service_.State(),
                                                                 .observe = ObserveGpu});
            auto queue = Sora::Compute::Queue::Adopt(
                service_, {.id = kPipelineQueue,
                           .device = kGpu,
                           .backend = Sora::Compute::BackendKind::Vulkan,
                           .capabilities = Sora::Compute::QueueCapability::Dispatch,
                           .dispatchArtifactKind = Sora::Compute::ArtifactKind::DevicePipeline,
                           .state = new uint32_t{},
                           .destroy = DestroyOpaque,
                           .submit = Submit});
            auto allocator = Sora::Compute::MemoryAllocator::Create(
                service_, {.location = kDevice,
                           .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                           .state = service_.State(),
                           .allocate = Allocate});
            auto transfer = Sora::Compute::TransferEngine::Create(
                service_, {.engine = {3, 1},
                           .sourceService = service_.Id(),
                           .source = kHost,
                           .destination = kDevice,
                           .kind = Sora::Compute::TransferKind::Copy,
                           .completionDevice = kGpu,
                           .completionBackend = Sora::Compute::BackendKind::Vulkan,
                           .state = service_.State(),
                           .transfer = Transfer});
            auto factory = Sora::Compute::BackendPipelineFactory::Create(
                service_, Target(),
                {.identity = kPipelineFactory, .state = service_.State(), .create = CreatePipeline});
            auto compiler =
                Sora::Compute::BackendCompiler::Create(service_, {.identity = Target().compilerIdentity,
                                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                                  .executableDomain = Target().executableDomain,
                                                                  .state = service_.State(),
                                                                  .compile = Compile});
            if (!observer || !queue || !allocator || !transfer || !factory || !compiler) {
                return false;
            }
            const auto configuration = factory->Configuration(kPipelineState);
            auto catalog = Sora::Compute::PhysicalResourceCatalog::Create({
                .deviceObservers = {std::move(*observer)},
                .queues = {std::move(*queue)},
                .allocators = {std::move(*allocator)},
                .transferEngines = {std::move(*transfer)},
                .compilers = {std::move(*compiler)},
                .pipelineFactories = {std::move(*factory)},
                .pipelineConfigurations = {configuration},
            });
            if (!catalog) {
                return false;
            }
            pipelineCatalog_.emplace(std::move(*catalog));
            return true;
        }

        [[nodiscard]] bool PopulatePerformance() noexcept {
            const auto observeKernel = [](Sora::Compute::PerformanceModel& model,
                                          const Sora::Compute::KernelCalibrationKey& key) noexcept {
                return model.Update(
                           {.key = key, .workItemCount = 4096, .computeNanoseconds = 2048, .launchNanoseconds = 300}) &&
                       model.Update({.key = ExecutableArtifact(key),
                                     .path = Sora::Compute::ArtifactResolutionPath::TransientProduction,
                                     .nanoseconds = 10'000});
            };
            if (!observeKernel(performance_, CandidateKey())) {
                return false;
            }
            if (!performance_.Update(
                    {.key = {.service = kServiceId, .engine = {3, 1}, .source = kHost, .destination = kDevice},
                     .bytes = kTransferBytes,
                     .payloadNanoseconds = 100'000,
                     .latencyNanoseconds = 5'000})) {
                return false;
            }
            performanceSnapshot_ = performance_.Snapshot();
            const auto alternativeTarget =
                Target(kAlternativeCompiler, Sora::Compute::ExecutableTargetDomain::BackendPortable);
            if (!observeKernel(multiCompilerPerformance_, CandidateKey()) ||
                !observeKernel(multiCompilerPerformance_, CandidateKey(alternativeTarget)) ||
                !multiCompilerPerformance_.Update(
                    {.key = {.service = kServiceId, .engine = {3, 1}, .source = kHost, .destination = kDevice},
                     .bytes = kTransferBytes,
                     .payloadNanoseconds = 100'000,
                     .latencyNanoseconds = 5'000})) {
                return false;
            }
            multiCompilerPerformanceSnapshot_ = multiCompilerPerformance_.Snapshot();
            if (!observeKernel(pipelinePerformance_, PipelineCandidateKey()) ||
                !pipelinePerformance_.Update(
                    {.key = {.service = kServiceId, .engine = {3, 1}, .source = kHost, .destination = kDevice},
                     .bytes = kTransferBytes,
                     .payloadNanoseconds = 100'000,
                     .latencyNanoseconds = 5'000}) ||
                !pipelinePerformance_.Update({.key = Sora::Compute::MakeArtifactKey(PipelineKey()),
                                              .path = Sora::Compute::ArtifactResolutionPath::TransientProduction,
                                              .nanoseconds = 20'000})) {
                return false;
            }
            pipelinePerformanceSnapshot_ = pipelinePerformance_.Snapshot();
            return performanceSnapshot_ != nullptr && multiCompilerPerformanceSnapshot_ != nullptr &&
                   pipelinePerformanceSnapshot_ != nullptr;
        }

        [[nodiscard]] bool CreateGraph() noexcept {
            if (!packages_.Register(kCopyProgram)) {
                return false;
            }
            auto input = Sora::Compute::Buffer::Create({.id = {50, 1},
                                                        .elementType = kFloat,
                                                        .layout = Sora::Compute::LayoutKind::Contiguous,
                                                        .rank = 1,
                                                        .sizeBytes = kTransferBytes,
                                                        .alignment = 64});
            auto output = Sora::Compute::Buffer::Create({.id = {50, 2},
                                                         .elementType = kFloat,
                                                         .layout = Sora::Compute::LayoutKind::Contiguous,
                                                         .rank = 1,
                                                         .sizeBytes = kTransferBytes,
                                                         .alignment = 64});
            auto allocation =
                Sora::Compute::Allocation::Adopt(service_, {.location = kHost,
                                                            .memory = {60, 1},
                                                            .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                            .sizeBytes = kTransferBytes,
                                                            .alignment = 64,
                                                            .state = new uint32_t{},
                                                            .destroy = DestroyOpaque});
            if (!input || !output || !allocation || !input->CommitWholeWrite(std::move(*allocation))) {
                return false;
            }
            constexpr std::array extents{kElementCount};
            auto inputView = Sora::Compute::BufferView::Contiguous(*input, extents);
            auto outputView = Sora::Compute::BufferView::Contiguous(*output, extents);
            if (!inputView || !outputView) {
                return false;
            }
            std::vector<Sora::Compute::KernelArgumentBinding> bindings;
            bindings.emplace_back(std::move(*inputView));
            bindings.emplace_back(std::move(*outputView));
            auto invocation = Sora::Compute::BindKernelProgram(kCopyProgram, extents, std::move(bindings));
            if (!invocation) {
                return false;
            }
            Sora::Compute::GraphBuilder builder;
            if (!builder.AddDispatch(std::move(*invocation))) {
                return false;
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

        Sora::Compute::BackendService service_{};
        Sora::Compute::ArtifactCache artifacts_;
        Sora::Compute::PortablePackageRegistry packages_{};
        std::optional<Sora::Compute::ProgramCatalogSnapshot> programCatalog_{};
        Sora::Compute::PerformanceModel performance_{};
        Sora::Compute::PerformanceModel multiCompilerPerformance_{};
        Sora::Compute::PerformanceModel pipelinePerformance_{};
        std::optional<Sora::Compute::PhysicalResourceCatalog> catalog_{};
        std::optional<Sora::Compute::PhysicalResourceCatalog> multiCompilerCatalog_{};
        std::optional<Sora::Compute::PhysicalResourceCatalog> pipelineCatalog_{};
        std::optional<Sora::Compute::ExecutionGraph> graph_{};
        std::expected<Sora::Compute::ArtifactAvailabilitySnapshot, Sora::Compute::ArtifactCacheError>
            artifactAvailability_ = std::unexpected(Sora::Compute::ArtifactCacheError::OutOfMemory);
        std::shared_ptr<const Sora::Compute::PerformanceSnapshot> performanceSnapshot_{};
        std::shared_ptr<const Sora::Compute::PerformanceSnapshot> multiCompilerPerformanceSnapshot_{};
        std::shared_ptr<const Sora::Compute::PerformanceSnapshot> pipelinePerformanceSnapshot_{};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    PlanningInputFixture fixture;
    if (fixture.Failed()) {
        return 2;
    }
    auto artifactSnapshot = [&fixture]() noexcept { fixture.SnapshotArtifacts(); };
    auto topologyProjection = [&fixture]() noexcept { fixture.ProjectTopology(); };
    auto candidateSynthesis = [&fixture]() noexcept { fixture.SynthesizeCandidate(); };
    auto multiCompilerCandidateSynthesis = [&fixture]() noexcept { fixture.SynthesizeMultiCompilerCandidate(); };
    auto pipelineCandidateSynthesis = [&fixture]() noexcept { fixture.SynthesizePipelineCandidate(); };
    auto candidateEnumeration = [&fixture]() noexcept { fixture.EnumerateCandidate(); };
    auto multiCompilerCandidateEnumeration = [&fixture]() noexcept { fixture.EnumerateMultiCompilerCandidate(); };
    auto pipelineCandidateEnumeration = [&fixture]() noexcept { fixture.EnumeratePipelineCandidate(); };
    const std::array cases{
        Sora::Benchmark::MakeCase("artifact_snapshot_64", artifactSnapshot),
        Sora::Benchmark::MakeCase("calibrated_topology_projection", topologyProjection),
        Sora::Benchmark::MakeCase("transfer_inclusive_candidate_synthesis", candidateSynthesis),
        Sora::Benchmark::MakeCase("two_compiler_candidate_synthesis", multiCompilerCandidateSynthesis),
        Sora::Benchmark::MakeCase("dispatch_candidate_enumeration", candidateEnumeration),
        Sora::Benchmark::MakeCase("two_compiler_dispatch_candidate_enumeration", multiCompilerCandidateEnumeration),
        Sora::Benchmark::MakeCase("pipeline_candidate_synthesis", pipelineCandidateSynthesis),
        Sora::Benchmark::MakeCase("pipeline_dispatch_candidate_enumeration", pipelineCandidateEnumeration),
    };
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Checksum() == 0 || fixture.Checksum() == std::numeric_limits<uint64_t>::max()) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("artifact_availability_snapshot_64,{:.6f},ns/operation", timing.MedianTime(0));
    std::println("calibrated_transfer_topology_projection,{:.6f},ns/operation", timing.MedianTime(1));
    std::println("transfer_inclusive_candidate_synthesis,{:.6f},ns/operation", timing.MedianTime(2));
    std::println("two_compiler_candidate_synthesis,{:.6f},ns/operation", timing.MedianTime(3));
    std::println("dispatch_candidate_enumeration,{:.6f},ns/operation", timing.MedianTime(4));
    std::println("two_compiler_dispatch_candidate_enumeration,{:.6f},ns/operation", timing.MedianTime(5));
    std::println("pipeline_candidate_synthesis,{:.6f},ns/operation", timing.MedianTime(6));
    std::println("pipeline_dispatch_candidate_enumeration,{:.6f},ns/operation", timing.MedianTime(7));
    std::println("artifact_snapshot_repetitions,{},count", timing.repetitions[0]);
    std::println("topology_projection_repetitions,{},count", timing.repetitions[1]);
    std::println("candidate_synthesis_repetitions,{},count", timing.repetitions[2]);
    std::println("two_compiler_candidate_synthesis_repetitions,{},count", timing.repetitions[3]);
    std::println("candidate_enumeration_repetitions,{},count", timing.repetitions[4]);
    std::println("two_compiler_candidate_enumeration_repetitions,{},count", timing.repetitions[5]);
    std::println("pipeline_candidate_synthesis_repetitions,{},count", timing.repetitions[6]);
    std::println("pipeline_candidate_enumeration_repetitions,{},count", timing.repetitions[7]);
    std::println("checksum,{},value", fixture.Checksum());
}
