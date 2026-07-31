/**
 * @file PipelineServiceTest.cpp
 * @brief Pipeline ABI, cache coordination, persistence, lifecycle, and dispatch
 * tests.
 */

#include <Sora/Compute/Backend.h>
#include <Sora/Compute/Backend/Pipeline.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/PipelineService.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>

namespace {

    constexpr Sora::Compute::DeviceId kDevice{0x56554C4B414E, 1};
    constexpr Sora::Compute::SemanticHash kFactoryIdentity{0x504950454C494E45, 0x464143544F525931};
    constexpr Sora::Compute::TargetFingerprint kTarget{
        .device = kDevice,
        .backend = Sora::Compute::BackendKind::Vulkan,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable,
        .addressBits = 64,
        .features = Sora::Compute::DeviceFeature::Int64,
        .architectureId = 0x766B5F746573745F,
        .compilerIdentity = {0x534C414E47, 1},
        .driverIdentity = {0x445249564552, 1},
    };
    constexpr Sora::Compute::PipelineStateKey kPipelineState{
        .state = {0x454E545259, 0x5354415445},
        .bindingLayout = {0x42494E44494E47, 0x4C41594F5554},
    };

    consteval auto MakeProgram() {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> builder{
            domain, std::move(scalar).Finish(index), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, domain.rank);
        builder.BindLogicalIndex(0);
        return std::move(builder).Finish(output);
    }

    constexpr auto kProgram = MakeProgram();
    constexpr Sora::Compute::ExecutableKey kExecutableKey{
        .lowered = kProgram.Hash(),
        .target = kTarget,
        .specialization = {.shape = {1, 2}, .layout = {3, 4}, .tuning = {5, 6}},
    };

    struct LifecycleTrace {
        std::atomic<uint32_t> createCount = 0;
        std::atomic<uint32_t> importCount = 0;
        std::atomic<uint32_t> exportCount = 0;
        std::atomic<uint32_t> compilerExportCount = 0;
        std::atomic<uint32_t> bytesDestroyCount = 0;
        std::atomic<uint32_t> executableDestroyCount = 0;
        std::atomic<uint32_t> pipelineDestroyCount = 0;
        std::atomic<uint32_t> serviceDestroyCount = 0;
        std::atomic<uint32_t> moduleDestroyCount = 0;
        std::atomic<uint32_t> queueSubmitCount = 0;
        std::atomic<uint32_t> queueDestroyCount = 0;
        std::atomic<uint32_t> eventDestroyCount = 0;
        std::atomic<uint32_t> orderingViolationCount = 0;
        std::atomic<bool> executableAlive = false;
        std::atomic<bool> serviceAlive = true;
        std::atomic<bool> codeAlive = true;
        bool returnWrongKey = false;
        bool delayCreation = false;
    };

    struct SimulatedModule {
        LifecycleTrace* trace = nullptr;

        ~SimulatedModule() {
            if (trace->serviceAlive.load(std::memory_order_acquire)) {
                trace->orderingViolationCount.fetch_add(1, std::memory_order_relaxed);
            }
            trace->codeAlive.store(false, std::memory_order_release);
            trace->moduleDestroyCount.fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct NativeService {
        LifecycleTrace* trace = nullptr;
    };

    enum class NativeArtifactKind : uint8_t {
        Executable,
        Pipeline,
    };

    struct NativeArtifact {
        LifecycleTrace* trace = nullptr;
        NativeArtifactKind kind = NativeArtifactKind::Executable;
    };

    struct NativeBytes {
        LifecycleTrace* trace = nullptr;
        std::array<std::byte, 4> bytes{};
    };

    struct NativeQueue {
        LifecycleTrace* trace = nullptr;
    };

    struct NativeEvent {
        LifecycleTrace* trace = nullptr;
    };

    void DestroyService(void* opaque) noexcept {
        std::unique_ptr<NativeService> service{static_cast<NativeService*>(opaque)};
        LifecycleTrace& trace = *service->trace;
        if (!trace.codeAlive.load(std::memory_order_acquire)) {
            trace.orderingViolationCount.fetch_add(1, std::memory_order_relaxed);
        }
        trace.serviceAlive.store(false, std::memory_order_release);
        trace.serviceDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyArtifact(void* opaque) noexcept {
        std::unique_ptr<NativeArtifact> artifact{static_cast<NativeArtifact*>(opaque)};
        LifecycleTrace& trace = *artifact->trace;
        if (!trace.serviceAlive.load(std::memory_order_acquire) || !trace.codeAlive.load(std::memory_order_acquire)) {
            trace.orderingViolationCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (artifact->kind == NativeArtifactKind::Executable) {
            trace.executableAlive.store(false, std::memory_order_release);
            trace.executableDestroyCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            trace.pipelineDestroyCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void DestroyBytes(void* opaque) noexcept {
        std::unique_ptr<NativeBytes> bytes{static_cast<NativeBytes*>(opaque)};
        bytes->trace->bytesDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyQueue(void* opaque) noexcept {
        std::unique_ptr<NativeQueue> queue{static_cast<NativeQueue*>(opaque)};
        queue->trace->queueDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyEvent(void* opaque) noexcept {
        std::unique_ptr<NativeEvent> event{static_cast<NativeEvent*>(opaque)};
        event->trace->eventDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void QueryEvent(void*, Sora::Compute::BackendEventObservation* observation) noexcept {
        observation->status = Sora::Compute::EventStatus::Completed;
        observation->error = {};
    }

    void WaitEvent(void* opaque, uint64_t, Sora::Compute::BackendEventObservation* observation) noexcept {
        QueryEvent(opaque, observation);
    }

    void CreatePipeline(void* opaque, const Sora::Compute::BackendPipelineRequest* request,
                        Sora::Compute::BackendPipelineResult* result) noexcept {
        auto& service = *static_cast<NativeService*>(opaque);
        LifecycleTrace& trace = *service.trace;
        trace.createCount.fetch_add(1, std::memory_order_relaxed);
        if (trace.delayCreation) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto* executable = static_cast<const NativeArtifact*>(request->executable.state);
        if (executable == nullptr || executable->kind != NativeArtifactKind::Executable ||
            !trace.executableAlive.load(std::memory_order_acquire)) {
            result->error = {Sora::Compute::PipelineErrorCode::UnsupportedExecutable, 0};
            return;
        }
        auto pipeline = std::unique_ptr<NativeArtifact>{
            new (std::nothrow) NativeArtifact{.trace = &trace, .kind = NativeArtifactKind::Pipeline}};
        if (!pipeline) {
            result->error = {Sora::Compute::PipelineErrorCode::OutOfMemory, 0};
            return;
        }
        Sora::Compute::ArtifactKey key = request->ExpectedKey();
        if (trace.returnWrongKey) {
            auto wrong = request->pipeline;
            ++wrong.state.low;
            key = Sora::Compute::MakeArtifactKey(wrong);
        }
        result->payload = {.key = key,
                           .device = request->pipeline.target.device,
                           .state = pipeline.release(),
                           .destroy = DestroyArtifact};
        result->error = {};
    }

    void ImportPipeline(void* opaque, const Sora::Compute::BackendPipelineImportRequest* request,
                        Sora::Compute::BackendPipelineResult* result) noexcept {
        auto& service = *static_cast<NativeService*>(opaque);
        LifecycleTrace& trace = *service.trace;
        trace.importCount.fetch_add(1, std::memory_order_relaxed);
        constexpr std::array expected{std::byte{0x56}, std::byte{0x4B}, std::byte{0x50}, std::byte{0x43}};
        if (request->pipelineRequest == nullptr || !std::ranges::equal(request->ArtifactBytes(), expected)) {
            result->error = {Sora::Compute::PipelineErrorCode::ArtifactImportFailed, 0};
            return;
        }
        auto pipeline = std::unique_ptr<NativeArtifact>{
            new (std::nothrow) NativeArtifact{.trace = &trace, .kind = NativeArtifactKind::Pipeline}};
        if (!pipeline) {
            result->error = {Sora::Compute::PipelineErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->pipelineRequest->ExpectedKey(),
                           .device = request->pipelineRequest->pipeline.target.device,
                           .state = pipeline.release(),
                           .destroy = DestroyArtifact};
        result->error = {};
    }

    void ExportPipeline(void* opaque, const Sora::Compute::BackendArtifactExportRequest* request,
                        Sora::Compute::BackendPipelineExportResult* result) noexcept {
        auto& service = *static_cast<NativeService*>(opaque);
        LifecycleTrace& trace = *service.trace;
        trace.exportCount.fetch_add(1, std::memory_order_relaxed);
        const auto* pipeline = static_cast<const NativeArtifact*>(request->artifact.state);
        if (pipeline == nullptr || pipeline->kind != NativeArtifactKind::Pipeline || pipeline->trace != &trace) {
            result->error = {Sora::Compute::PipelineErrorCode::InvalidArtifact, 0};
            return;
        }
        auto bytes = std::unique_ptr<NativeBytes>{new (std::nothrow) NativeBytes{
            .trace = &trace,
            .bytes = {std::byte{0x56}, std::byte{0x4B}, std::byte{0x50}, std::byte{0x43}},
        }};
        if (!bytes) {
            result->error = {Sora::Compute::PipelineErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.data = bytes->bytes.data(),
                           .size = bytes->bytes.size(),
                           .state = bytes.release(),
                           .destroy = DestroyBytes};
        result->error = {};
    }

    void CompileExecutable(void*, const Sora::Compute::BackendCompileRequest*,
                           Sora::Compute::BackendCompileResult*) noexcept {}

    void ImportExecutable(void*, const Sora::Compute::BackendArtifactImportRequest*,
                          Sora::Compute::BackendCompileResult*) noexcept {}

    void ExportExecutable(void* opaque, const Sora::Compute::BackendArtifactExportRequest*,
                          Sora::Compute::BackendArtifactExportResult*) noexcept {
        auto& service = *static_cast<NativeService*>(opaque);
        service.trace->compilerExportCount.fetch_add(1, std::memory_order_relaxed);
    }

    void Submit(void* opaque, const Sora::Compute::BackendSubmitRequest*,
                Sora::Compute::BackendSubmitResult* result) noexcept {
        auto& queue = *static_cast<NativeQueue*>(opaque);
        queue.trace->queueSubmitCount.fetch_add(1, std::memory_order_relaxed);
        auto event = std::unique_ptr<NativeEvent>{new (std::nothrow) NativeEvent{queue.trace}};
        if (!event) {
            result->error = {Sora::Compute::SubmissionErrorCode::OutOfMemory, 0};
            return;
        }
        result->event = {.device = kDevice,
                         .backend = Sora::Compute::BackendKind::Vulkan,
                         .state = event.release(),
                         .destroy = DestroyEvent,
                         .query = QueryEvent,
                         .wait = WaitEvent};
        result->error = {};
    }

    [[nodiscard]] Sora::Compute::BackendService MakeService(LifecycleTrace& trace, Sora::Compute::BackendServiceId id) {
        auto module = std::make_shared<SimulatedModule>(&trace);
        auto code = Sora::Compute::BackendCodeLease::Retain(module);
        auto native = std::make_unique<NativeService>(&trace);
        auto service = Sora::Compute::BackendService::Adopt(
            {.id = id, .state = native.release(), .destroy = DestroyService}, std::move(code));
        REQUIRE(service.has_value());
        return *service;
    }

    [[nodiscard]] Sora::Compute::Artifact MakeExecutable(const Sora::Compute::BackendService& service,
                                                         LifecycleTrace& trace,
                                                         Sora::Compute::ExecutableKey key = kExecutableKey) {
        auto native = std::make_unique<NativeArtifact>(&trace, NativeArtifactKind::Executable);
        trace.executableAlive.store(true, std::memory_order_release);
        auto artifact = Sora::Compute::Artifact::Adopt(service, {.key = Sora::Compute::MakeArtifactKey(key),
                                                                 .device = kDevice,
                                                                 .state = native.release(),
                                                                 .destroy = DestroyArtifact});
        REQUIRE(artifact.has_value());
        return *artifact;
    }

    [[nodiscard]] Sora::Compute::BackendPipelineFactory
    MakeFactory(const Sora::Compute::BackendService& service, LifecycleTrace& trace,
                Sora::Compute::DeviceId device = kDevice,
                Sora::Compute::BackendKind backend = Sora::Compute::BackendKind::Vulkan,
                Sora::Compute::SemanticHash identity = kFactoryIdentity) {
        auto target = kTarget;
        target.device = device;
        target.backend = backend;
        auto factory = Sora::Compute::BackendPipelineFactory::Create(service, target,
                                                                     {.identity = identity,
                                                                      .state = service.State(),
                                                                      .create = CreatePipeline,
                                                                      .importPipeline = ImportPipeline,
                                                                      .exportPipeline = ExportPipeline});
        REQUIRE(factory.has_value());
        static_cast<void>(trace);
        return *factory;
    }

    [[nodiscard]] Sora::Compute::Queue MakePipelineQueue(const Sora::Compute::BackendService& service,
                                                         LifecycleTrace& trace) {
        auto native = std::make_unique<NativeQueue>(&trace);
        auto queue =
            Sora::Compute::Queue::Adopt(service, {.id = {1, 1},
                                                  .device = kDevice,
                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                  .capabilities = Sora::Compute::QueueCapability::Dispatch,
                                                  .dispatchArtifactKind = Sora::Compute::ArtifactKind::DevicePipeline,
                                                  .state = native.release(),
                                                  .destroy = DestroyQueue,
                                                  .submit = Submit});
        REQUIRE(queue.has_value());
        return *queue;
    }

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<uint64_t> sequence = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ =
                std::filesystem::temp_directory_path() / std::format("sora-pipeline-service-test-{}-{}", stamp,
                                                                     sequence.fetch_add(1, std::memory_order_relaxed));
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

} // namespace

TEST_CASE("Pipeline factory derives an exact key and rejects foreign resources",
          "[Sora][Compute][Pipeline][Identity][Validation]") {
    LifecycleTrace trace;
    const auto service = MakeService(trace, {1, 1});
    const auto executable = MakeExecutable(service, trace);
    const auto factory = MakeFactory(service, trace);
    const Sora::Compute::ArtifactKey expected = factory.ExpectedKey(executable, kPipelineState);

    REQUIRE(expected.Valid());
    REQUIRE(expected.Kind() == Sora::Compute::ArtifactKind::DevicePipeline);
    REQUIRE(expected.Backend() == Sora::Compute::BackendKind::Vulkan);
    REQUIRE(factory.Device() == kDevice);
    REQUIRE(factory.Target() == kTarget);
    REQUIRE(expected.ProducerIdentity() == kFactoryIdentity);
    REQUIRE(expected.Words().size() == 18);
    REQUIRE(executable.View().kind == Sora::Compute::ArtifactKind::DeviceExecutable);
    REQUIRE(executable.View().identity == Sora::Compute::ArtifactIdentity(executable.Key()));

    const Sora::Compute::PipelineStateKey invalidState{};
    REQUIRE_FALSE(factory.Accepts(executable, invalidState));
    REQUIRE_FALSE(factory.ExpectedKey(executable, invalidState).Valid());

    LifecycleTrace foreignServiceTrace;
    const auto foreignService = MakeService(foreignServiceTrace, {2, 2});
    const auto foreignServiceFactory = MakeFactory(foreignService, foreignServiceTrace);
    REQUIRE_FALSE(foreignServiceFactory.Accepts(executable, kPipelineState));

    const auto foreignDeviceFactory = MakeFactory(service, trace, {kDevice.low + 1, kDevice.high});
    REQUIRE_FALSE(foreignDeviceFactory.Accepts(executable, kPipelineState));

    auto cpuKey = kExecutableKey;
    auto cpuTarget = kTarget;
    cpuTarget.backend = Sora::Compute::BackendKind::Cpu;
    cpuKey.target = Sora::Compute::ExecutableTarget(cpuTarget);
    const auto cpuExecutable = MakeExecutable(service, trace, cpuKey);
    REQUIRE_FALSE(factory.Accepts(cpuExecutable, kPipelineState));

    auto pipeline = factory.CreatePipeline(executable, kPipelineState);
    REQUIRE(pipeline.has_value());
    REQUIRE_FALSE(factory.Export(executable).has_value());
    REQUIRE_FALSE(foreignDeviceFactory.Export(*pipeline).has_value());
    const auto foreignIdentityFactory = MakeFactory(service, trace, kDevice, Sora::Compute::BackendKind::Vulkan,
                                                    {kFactoryIdentity.low + 1, kFactoryIdentity.high});
    REQUIRE_FALSE(foreignIdentityFactory.Export(*pipeline).has_value());

    auto compiler = Sora::Compute::BackendCompiler::Create(
        service, {.identity = kTarget.compilerIdentity,
                  .backend = Sora::Compute::BackendKind::Vulkan,
                  .executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable,
                  .state = service.State(),
                  .compile = CompileExecutable,
                  .importArtifact = ImportExecutable,
                  .exportArtifact = ExportExecutable});
    REQUIRE(compiler.has_value());
    const auto rejectedPipeline = compiler->Export(*pipeline);
    REQUIRE_FALSE(rejectedPipeline.has_value());
    REQUIRE(rejectedPipeline.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
    REQUIRE(trace.exportCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.compilerExportCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Pipeline factory rejects a mismatched artifact key transactionally",
          "[Sora][Compute][Pipeline][Validation]") {
    LifecycleTrace trace;
    trace.returnWrongKey = true;
    const auto service = MakeService(trace, {3, 3});
    const auto executable = MakeExecutable(service, trace);
    const auto factory = MakeFactory(service, trace);

    const auto result = factory.CreatePipeline(executable, kPipelineState);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == Sora::Compute::PipelineErrorCode::InvalidArtifact);
    REQUIRE(trace.createCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.pipelineDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("PipelineService deduplicates concurrent creation and retains the "
          "executable during callbacks",
          "[Sora][Compute][Pipeline][Concurrency]") {
    constexpr size_t callerCount = 8;
    LifecycleTrace trace;
    trace.delayCreation = true;
    const auto service = MakeService(trace, {4, 4});
    const auto executable = MakeExecutable(service, trace);
    const auto factory = MakeFactory(service, trace);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::PipelineService pipelineService{cache};
    std::barrier start{callerCount};
    std::array<std::optional<std::expected<Sora::Compute::Artifact, Sora::Compute::PipelineError>>, callerCount>
        results;
    std::array<std::jthread, callerCount> callers;

    for (size_t index = 0; index < callerCount; ++index) {
        callers[index] = std::jthread{[&, index] {
            start.arrive_and_wait();
            results[index] = pipelineService.Resolve(factory, executable, kPipelineState);
        }};
    }
    for (std::jthread& caller : callers) {
        caller.join();
    }

    REQUIRE(trace.createCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.executableAlive.load(std::memory_order_acquire));
    REQUIRE(cache.Size() == 1);
    void* expectedState = nullptr;
    for (const auto& result : results) {
        REQUIRE(result.has_value());
        REQUIRE(result->has_value());
        if (expectedState == nullptr) {
            expectedState = result->value().View().state;
        }
        REQUIRE(result->value().View().state == expectedState);
    }
    REQUIRE(pipelineService.Statistics().pipelineCreations == 1);
}

TEST_CASE("PipelineService isolates live handles from distinct backend services",
          "[Sora][Compute][Pipeline][Cache][Service]") {
    LifecycleTrace firstTrace;
    LifecycleTrace secondTrace;
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::PipelineService pipelineService{cache};
    const auto firstService = MakeService(firstTrace, {10, 10});
    const auto firstExecutable = MakeExecutable(firstService, firstTrace);
    const auto firstFactory = MakeFactory(firstService, firstTrace);
    auto first = pipelineService.Resolve(firstFactory, firstExecutable, kPipelineState);
    REQUIRE(first.has_value());

    const auto secondService = MakeService(secondTrace, {11, 11});
    const auto secondExecutable = MakeExecutable(secondService, secondTrace);
    const auto secondFactory = MakeFactory(secondService, secondTrace);
    auto second = pipelineService.Resolve(secondFactory, secondExecutable, kPipelineState);
    REQUIRE(second.has_value());

    REQUIRE(first->Key() == second->Key());
    REQUIRE(first->View().service != second->View().service);
    REQUIRE(first->View().state != second->View().state);
    REQUIRE(firstTrace.createCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(secondTrace.createCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(cache.Size() == 2);
}

TEST_CASE("PipelineService repairs backend-invalid persistent bytes and "
          "imports after restart",
          "[Sora][Compute][Pipeline][Persistence]") {
    TemporaryDirectory temporary;
    auto createdStore = Sora::Compute::ArtifactStore::Create(
        {.root = temporary.Path(), .maximumPayloadBytes = 4096, .durableWrites = false});
    REQUIRE(createdStore.has_value());
    auto store = std::move(*createdStore);

    LifecycleTrace firstTrace;
    Sora::Compute::ArtifactKey key;
    {
        const auto service = MakeService(firstTrace, {5, 5});
        const auto executable = MakeExecutable(service, firstTrace);
        const auto factory = MakeFactory(service, firstTrace);
        key = factory.ExpectedKey(executable, kPipelineState);
        Sora::Compute::ArtifactCache cache{2};
        Sora::Compute::PipelineService pipelineService{cache, store};
        REQUIRE(pipelineService.Resolve(factory, executable, kPipelineState).has_value());
        REQUIRE(pipelineService.Statistics().diskCacheMisses == 1);
        REQUIRE(pipelineService.Statistics().pipelineCreations == 1);
        REQUIRE(firstTrace.exportCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(firstTrace.bytesDestroyCount.load(std::memory_order_relaxed) == 1);
    }

    constexpr std::array invalidBytes{std::byte{0x42}, std::byte{0x41}, std::byte{0x44}};
    REQUIRE(store.Store(key, invalidBytes).has_value());
    LifecycleTrace repairTrace;
    {
        const auto service = MakeService(repairTrace, {6, 6});
        const auto executable = MakeExecutable(service, repairTrace);
        const auto factory = MakeFactory(service, repairTrace);
        Sora::Compute::ArtifactCache cache{2};
        Sora::Compute::PipelineService pipelineService{cache, store};
        REQUIRE(pipelineService.Resolve(factory, executable, kPipelineState).has_value());
        REQUIRE(pipelineService.Statistics().artifactImportFailures == 1);
        REQUIRE(pipelineService.Statistics().pipelineCreations == 1);
        REQUIRE(repairTrace.importCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(repairTrace.exportCount.load(std::memory_order_relaxed) == 1);
    }

    LifecycleTrace restartTrace;
    {
        const auto service = MakeService(restartTrace, {7, 7});
        const auto executable = MakeExecutable(service, restartTrace);
        const auto factory = MakeFactory(service, restartTrace);
        Sora::Compute::ArtifactCache cache{2};
        Sora::Compute::PipelineService pipelineService{cache, store};
        REQUIRE(pipelineService.Resolve(factory, executable, kPipelineState).has_value());
        REQUIRE(pipelineService.Statistics().diskCacheHits == 1);
        REQUIRE(pipelineService.Statistics().pipelineCreations == 0);
        REQUIRE(restartTrace.importCount.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("PipelineService imports an exact packaged pipeline before driver creation",
          "[Sora][Compute][Pipeline][PortablePackage]") {
    LifecycleTrace trace;
    const auto service = MakeService(trace, {12, 12});
    const auto executable = MakeExecutable(service, trace);
    const auto factory = MakeFactory(service, trace);
    const auto pipelineKey = factory.ExpectedKey(executable, kPipelineState);
    REQUIRE(pipelineKey.Valid());

    auto program = Sora::Compute::ProgramPackage::Create(kProgram);
    REQUIRE(program.has_value());
    auto package = Sora::Compute::PortablePackage::Create(
        std::move(*program),
        {{.key = executable.Key(), .payload = {std::byte{0x45}}},
         {.key = pipelineKey, .payload = {std::byte{0x56}, std::byte{0x4B}, std::byte{0x50}, std::byte{0x43}}}});
    REQUIRE(package.has_value());
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(std::move(*package)).has_value());

    Sora::Compute::ArtifactCache cache{2};
    Sora::Compute::PipelineService pipelineService{cache, packages};
    const auto resolved = pipelineService.Resolve(factory, executable, kPipelineState);
    REQUIRE(resolved.has_value());
    REQUIRE(trace.importCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.createCount.load(std::memory_order_relaxed) == 0);
    const auto statistics = pipelineService.Statistics();
    REQUIRE(statistics.packageArtifactHits == 1);
    REQUIRE(statistics.packageArtifactMisses == 0);
    REQUIRE(statistics.pipelineCreations == 0);
}

TEST_CASE("A pipeline artifact pins service state and callback code through "
          "final destruction",
          "[Sora][Compute][Pipeline][Lifecycle]") {
    LifecycleTrace trace;
    Sora::Compute::Artifact escaped;
    {
        const auto service = MakeService(trace, {8, 8});
        const auto executable = MakeExecutable(service, trace);
        const auto factory = MakeFactory(service, trace);
        auto pipeline = factory.CreatePipeline(executable, kPipelineState);
        REQUIRE(pipeline.has_value());
        escaped = *pipeline;
    }

    REQUIRE(trace.executableDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.pipelineDestroyCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.serviceDestroyCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.moduleDestroyCount.load(std::memory_order_relaxed) == 0);
    escaped = {};
    REQUIRE(trace.pipelineDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.serviceDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.moduleDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.orderingViolationCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("A pipeline queue accepts pipeline artifacts and rejects executable "
          "artifacts",
          "[Sora][Compute][Pipeline][Queue]") {
    LifecycleTrace trace;
    const auto service = MakeService(trace, {9, 9});
    const auto executable = MakeExecutable(service, trace);
    const auto factory = MakeFactory(service, trace);
    auto pipeline = factory.CreatePipeline(executable, kPipelineState);
    REQUIRE(pipeline.has_value());
    const auto queue = MakePipelineQueue(service, trace);

    Sora::Compute::BackendCommand command{
        .kind = Sora::Compute::BackendCommandKind::Dispatch, .artifact = pipeline->View(), .workgroupCount = {8, 1, 1}};
    Sora::Compute::BackendSubmitRequest request{.commands = &command, .commandCount = 1};
    auto submitted = queue.Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->Query().status == Sora::Compute::EventStatus::Completed);
    REQUIRE(trace.queueSubmitCount.load(std::memory_order_relaxed) == 1);

    command.artifact = executable.View();
    const auto rejected = queue.Submit(request);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(trace.queueSubmitCount.load(std::memory_order_relaxed) == 1);
}
