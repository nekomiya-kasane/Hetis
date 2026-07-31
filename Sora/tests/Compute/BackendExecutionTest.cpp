#include "BackendTestSupport.h"
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/CompilerService.h>
#include <Sora/Compute/Execution/RecoveringSubmitSender.h>
#include <Sora/Compute/Execution/SubmitSender.h>
#include <Sora/Compute/Runtime/ExecutableGraph.h>
#include <Sora/Compute/Memory/Buffer.h>
#include <Sora/Compute/Planner.h>

#include <catch2/catch_test_macros.hpp>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

namespace {

    constexpr Sora::Compute::DeviceId kDevice{0xC0, 0xDE};
    constexpr Sora::Compute::DeviceId kSurvivorDevice{0xC0, 0xDF};
    constexpr Sora::Compute::BackendServiceId kSurvivorService{0x5355525649564F52, 0x5F53455256494345};
    constexpr Sora::Compute::MemoryLocation kSourceLocation{
        .domain = {0xA0, 0xA0}, .device = kDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kLocation{
        .domain = {0xC0, 0xDE},
        .device = kDevice,
        .backend = Sora::Compute::BackendKind::Cpu,
    };
    constexpr std::array kModuleBytes{std::byte{0x53}, std::byte{0x4B}, std::byte{0x49}, std::byte{0x52}};
    constexpr Sora::Compute::BackendCompileRequest kCompileRequest{
        .moduleData = kModuleBytes.data(),
        .moduleSize = kModuleBytes.size(),
        .moduleHash = Sora::Compute::Detail::HashBytes(kModuleBytes),
        .target = {.device = kDevice,
                   .backend = Sora::Compute::BackendKind::Cpu,
                   .hostArchitecture = Sora::CpuArchitecture::X64,
                   .operatingSystem = Sora::OperatingSystem::Windows,
                   .binaryFormat = Sora::BinaryFormat::PE,
                   .byteOrder = Sora::ByteOrder::Little,
                   .abi = Sora::AbiFamily::MSVC,
                   .addressBits = 64,
                   .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
                   .architectureId = 0x7838365F3634,
                   .compilerIdentity = {3, 4},
                   .driverIdentity = {5, 6}},
        .specialization = {.shape = {7, 8}, .layout = {9, 10}, .tuning = {11, 12}},
    };
    constexpr Sora::Compute::ArtifactKey kArtifactKey = kCompileRequest.ExpectedKey();

    struct MockQueueState {
        std::atomic<uint32_t> compileCount = 0;
        std::atomic<uint32_t> submitCount = 0;
        std::atomic<uint32_t> lastCommandCount = 0;
        std::atomic<uint32_t> lastBindingCount = 0;
        std::atomic<uint32_t> queueDestroyCount = 0;
        std::atomic<uint32_t> lastScalarBindingCount = 0;
        std::atomic<uint64_t> lastScalarDataSize = 0;
        std::atomic<uint32_t> eventDestroyCount = 0;
        std::atomic<uint32_t> artifactDestroyCount = 0;
        std::atomic<uint32_t> allocationDestroyCount = 0;
        std::atomic<uint64_t> nextMemoryId = 1;
        uint64_t executionNanoseconds = 0;
        bool returnWrongDevice = false;
        bool returnMalformedObservation = false;
        bool returnTimedPendingObservation = false;
        bool returnInvalidSubmitError = false;
        bool returnInvalidObservationError = false;
        bool failSubmit = false;
        bool completeEventsOnSubmit = false;
        Sora::Compute::SubmissionErrorCode failCode = Sora::Compute::SubmissionErrorCode::QueueUnavailable;
        bool returnPendingOnWait = false;
        stdexec::inplace_stop_source* stopOnWait = nullptr;
    };

    struct MockQueueNative {
        MockQueueState* owner = nullptr;
        Sora::Compute::DeviceId device{};
    };

    struct MockEventNative {
        MockQueueState* owner = nullptr;
        Sora::Compute::EventStatus status = Sora::Compute::EventStatus::Pending;
    };

    struct MockResourceNative {
        std::atomic<uint32_t>* destroyCount = nullptr;
    };

    void DestroyQueue(void* opaque) noexcept {
        std::unique_ptr<MockQueueNative> queue{static_cast<MockQueueNative*>(opaque)};
        queue->owner->queueDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyEvent(void* opaque) noexcept {
        std::unique_ptr<MockEventNative> event{static_cast<MockEventNative*>(opaque)};
        event->owner->eventDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyResource(void* opaque) noexcept {
        std::unique_ptr<MockResourceNative> resource{static_cast<MockResourceNative*>(opaque)};
        resource->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    void QueryEvent(void* opaque, Sora::Compute::BackendEventObservation* observation) noexcept {
        auto& event = *static_cast<MockEventNative*>(opaque);
        if (event.owner->returnInvalidObservationError) {
            observation->status = Sora::Compute::EventStatus::Failed;
            observation->error = {static_cast<Sora::Compute::SubmissionErrorCode>(UINT8_MAX), 8};
            return;
        }
        if (event.owner->returnMalformedObservation) {
            observation->status = Sora::Compute::EventStatus::Completed;
            observation->error = {Sora::Compute::SubmissionErrorCode::SubmissionFailed, 7};
            return;
        }
        if (event.owner->returnTimedPendingObservation) {
            observation->status = Sora::Compute::EventStatus::Pending;
            observation->error = {};
            observation->executionNanoseconds = 7;
            return;
        }
        observation->status = event.status;
        observation->error = {};
        observation->executionNanoseconds =
            event.status == Sora::Compute::EventStatus::Completed ? event.owner->executionNanoseconds : 0;
    }

    void WaitEvent(void* opaque, uint64_t, Sora::Compute::BackendEventObservation* observation) noexcept {
        auto& event = *static_cast<MockEventNative*>(opaque);
        if (event.owner->returnPendingOnWait) {
            event.owner->returnPendingOnWait = false;
            observation->status = Sora::Compute::EventStatus::Pending;
            observation->error = {};
            return;
        }
        if (event.owner->stopOnWait != nullptr) {
            event.owner->stopOnWait->request_stop();
        }
        event.status = Sora::Compute::EventStatus::Completed;
        observation->status = event.status;
        observation->error = {};
        observation->executionNanoseconds = event.owner->executionNanoseconds;
    }

    void CompileArtifactMock(void* opaque, const Sora::Compute::BackendCompileRequest* request,
                             Sora::Compute::BackendCompileResult* result) noexcept {
        auto& state = *static_cast<MockQueueState*>(opaque);
        state.compileCount.fetch_add(1, std::memory_order_relaxed);
        auto artifact =
            std::unique_ptr<MockResourceNative>{new (std::nothrow) MockResourceNative{&state.artifactDestroyCount}};
        if (!artifact) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->ExpectedKey(),
                           .device = request->target.device,
                           .state = artifact.release(),
                           .destroy = DestroyResource};
        result->error = {};
    }

    Sora::Compute::BackendCompiler MakeCompiler(MockQueueState& state) {
        auto compiler = Sora::Compute::BackendCompiler::Create(Sora::Test::TestBackendService(),
                                                               {.identity = kCompileRequest.target.compilerIdentity,
                                                                .backend = kCompileRequest.target.backend,
                                                                .state = &state,
                                                                .compile = CompileArtifactMock});
        REQUIRE(compiler.has_value());
        return *compiler;
    }

    void SubmitMock(void* opaque, const Sora::Compute::BackendSubmitRequest* request,
                    Sora::Compute::BackendSubmitResult* result) noexcept {
        const auto& native = *static_cast<MockQueueNative*>(opaque);
        auto& queue = *native.owner;
        queue.submitCount.fetch_add(1, std::memory_order_relaxed);
        queue.lastCommandCount.store(request->commandCount, std::memory_order_relaxed);
        queue.lastBindingCount.store(request->bindingCount, std::memory_order_relaxed);
        queue.lastScalarBindingCount.store(request->scalarBindingCount, std::memory_order_relaxed);
        queue.lastScalarDataSize.store(request->scalarDataSize, std::memory_order_relaxed);
        if (queue.returnInvalidSubmitError) {
            result->error = {static_cast<Sora::Compute::SubmissionErrorCode>(UINT8_MAX), 42};
            return;
        }
        if (queue.failSubmit) {
            result->error = {queue.failCode, 41};
            return;
        }
        auto event = std::unique_ptr<MockEventNative>{new (std::nothrow) MockEventNative{
            &queue, queue.completeEventsOnSubmit ? Sora::Compute::EventStatus::Completed
                                                 : Sora::Compute::EventStatus::Pending}};
        if (!event) {
            result->error = {Sora::Compute::SubmissionErrorCode::OutOfMemory, 0};
            return;
        }
        result->event = {.device = queue.returnWrongDevice ? Sora::Compute::DeviceId{9, 9} : native.device,
                         .backend = Sora::Compute::BackendKind::Cpu,
                         .state = event.release(),
                         .destroy = DestroyEvent,
                         .query = QueryEvent,
                         .wait = WaitEvent};
        result->error = {};
    }

    Sora::Compute::Queue
    MakeQueue(MockQueueState& state,
              Sora::Compute::QueueCapabilityMask capabilities = Sora::Compute::QueueCapability::Dispatch |
                                                                Sora::Compute::QueueCapability::Copy |
                                                                Sora::Compute::QueueCapability::Fill,
              const Sora::Compute::BackendService& service = Sora::Test::TestBackendService(),
              Sora::Compute::DeviceId device = kDevice, bool reportsExecutionNanoseconds = false) {
        auto native = std::make_unique<MockQueueNative>(&state, device);
        auto queue =
            Sora::Compute::Queue::Adopt(service, {
                                                     .id = {1, 1},
                                                     .device = device,
                                                     .backend = Sora::Compute::BackendKind::Cpu,
                                                     .capabilities = capabilities,
                                                     .state = native.release(),
                                                     .destroy = DestroyQueue,
                                                     .submit = SubmitMock,
                                                     .reportsExecutionNanoseconds = reportsExecutionNanoseconds,
                                                 });
        REQUIRE(queue.has_value());
        return *queue;
    }

    Sora::Compute::Event MakeEvent(MockQueueState& state, Sora::Compute::DeviceId device) {
        auto native = std::make_unique<MockEventNative>(&state);
        auto event = Sora::Compute::Event::Adopt(Sora::Test::TestBackendService(),
                                                 {.device = device,
                                                  .backend = Sora::Compute::BackendKind::Cpu,
                                                  .state = native.release(),
                                                  .destroy = DestroyEvent,
                                                  .query = QueryEvent,
                                                  .wait = WaitEvent},
                                                 Sora::Compute::BackendKind::Cpu, device);
        REQUIRE(event.has_value());
        return *event;
    }

    Sora::Compute::Artifact
    MakeArtifact(MockQueueState& state, const Sora::Compute::BackendService& service = Sora::Test::TestBackendService(),
                 Sora::Compute::ArtifactKey key = kArtifactKey, Sora::Compute::DeviceId device = kDevice) {
        auto native = std::make_unique<MockResourceNative>(&state.artifactDestroyCount);
        Sora::Compute::ArtifactPayload payload{
            .key = key,
            .device = device,
            .state = native.release(),
            .destroy = DestroyResource,
        };
        auto artifact = Sora::Compute::Artifact::Adopt(service, payload);
        REQUIRE(artifact.has_value());
        return *artifact;
    }

    Sora::Compute::Allocation
    MakeAllocation(MockQueueState& state, Sora::Compute::MemoryLocation location = kLocation,
                   Sora::Compute::PhysicalMemoryId memory = {}, uint64_t memoryOffset = 0,
                   const Sora::Compute::BackendService& service = Sora::Test::TestBackendService()) {
        if (!memory.Valid()) {
            memory = {state.nextMemoryId.fetch_add(1, std::memory_order_relaxed), 3};
        }
        auto native = std::make_unique<MockResourceNative>(&state.allocationDestroyCount);
        auto allocation =
            Sora::Compute::Allocation::Adopt(service, {
                                                          .location = location,
                                                          .memory = memory,
                                                          .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                          .memoryOffset = memoryOffset,
                                                          .sizeBytes = 1024,
                                                          .alignment = 64,
                                                          .state = native.release(),
                                                          .destroy = DestroyResource,
                                                      });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

    Sora::Compute::ExecutableBatch
    MakeBatch(MockQueueState& state, const Sora::Compute::BackendService& service = Sora::Test::TestBackendService(),
              Sora::Compute::DeviceId device = kDevice) {
        Sora::Compute::BackendCompileRequest request = kCompileRequest;
        request.target.device = device;
        Sora::Compute::MemoryLocation location = kLocation;
        location.device = device;
        Sora::Compute::Artifact artifact = MakeArtifact(state, service, request.ExpectedKey(), device);
        Sora::Compute::Allocation allocation = MakeAllocation(state, location, {}, 0, service);
        Sora::Compute::ExecutableBatchStorage storage{
            .queue = MakeQueue(state,
                               Sora::Compute::QueueCapability::Dispatch | Sora::Compute::QueueCapability::Copy |
                                   Sora::Compute::QueueCapability::Fill,
                               service, device),
            .commands = {Sora::Compute::BackendCommand{
                .kind = Sora::Compute::BackendCommandKind::Dispatch,
                .firstBinding = 0,
                .bindingCount = 1,
                .artifact = artifact.View(),
                .workgroupCount = {8, 1, 1},
            }},
            .bindings = {Sora::Compute::BackendBinding{allocation.View(), 0, 1024, 0,
                                                       Sora::Compute::AccessMode::ReadWrite}},
            .artifacts = {},
            .allocations = {},
        };
        storage.artifacts.push_back(std::move(artifact));
        storage.allocations.push_back(std::move(allocation));
        auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
        REQUIRE(batch.has_value());
        return std::move(*batch);
    }

    Sora::Compute::ExecutableGraph
    MakeGraph(MockQueueState& state, const Sora::Compute::BackendService& service = Sora::Test::TestBackendService(),
              Sora::Compute::DeviceId device = kDevice) {
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.emplace_back(MakeBatch(state, service, device));
        auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    struct KernelTelemetryState {
        std::atomic<uint32_t> observationCount = 0;
        Sora::Compute::KernelPerformanceObservation observation{};
    };

    void ObserveKernelTelemetry(void* opaque, const Sora::Compute::KernelPerformanceObservation& observation) noexcept {
        auto& state = *static_cast<KernelTelemetryState*>(opaque);
        state.observationCount.fetch_add(1, std::memory_order_relaxed);
        state.observation = observation;
    }

    struct RecoveryBackendState {};

    void DestroyRecoveryBackendState(void* opaque) noexcept {
        delete static_cast<RecoveryBackendState*>(opaque);
    }

    Sora::Compute::BackendService MakeRecoveryBackendService(Sora::Compute::BackendServiceId id) {
        auto state = std::make_unique<RecoveryBackendState>();
        auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<RecoveryBackendState>());
        auto service = Sora::Compute::BackendService::Adopt(
            {.id = id, .state = state.get(), .destroy = DestroyRecoveryBackendState}, std::move(code));
        REQUIRE(service.has_value());
        static_cast<void>(state.release());
        return std::move(*service);
    }

    struct RecoveryObserverState {
        Sora::Compute::DeviceId device = kDevice;
        std::atomic<Sora::Compute::DeviceState> state = Sora::Compute::DeviceState::Available;
    };

    void ObserveRecoveryDevice(void* opaque, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        const auto& state = *static_cast<const RecoveryObserverState*>(opaque);
        *snapshot = {.id = state.device, .state = state.state.load(std::memory_order_acquire), .generation = 1};
    }

    constexpr Sora::Compute::DeviceDescriptor RecoveryDeviceDescriptor(Sora::Compute::DeviceId device) noexcept {
        return {.id = device,
                .backend = Sora::Compute::BackendKind::Cpu,
                .kind = Sora::Compute::DeviceKind::Cpu,
                .architectureId = device.low,
                .subgroupWidthMinimum = 1,
                .subgroupWidthMaximum = 1,
                .maximumWorkgroupInvocations = 1024};
    }

    constexpr Sora::Compute::TargetFingerprint RecoveryTarget(Sora::Compute::DeviceDescriptor device) noexcept {
        return {.device = device.id,
                .backend = device.backend,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                .addressBits = 64,
                .features = device.features,
                .architectureId = device.architectureId,
                .compilerIdentity = {51, 1},
                .driverIdentity = {51, 2}};
    }

    Sora::Compute::DeviceObserver MakeRecoveryObserver(const Sora::Compute::BackendService& service,
                                                       RecoveryObserverState& state) {
        auto observer = Sora::Compute::DeviceObserver::Create(service, {.device = state.device,
                                                                        .backend = Sora::Compute::BackendKind::Cpu,
                                                                        .state = &state,
                                                                        .observe = ObserveRecoveryDevice});
        REQUIRE(observer.has_value());
        return std::move(*observer);
    }

    std::unique_ptr<Sora::Compute::RuntimeRecoveryCoordinator> MakeRecoveryCoordinator(RecoveryObserverState& state) {
        Sora::Compute::PhysicalResourceCatalogStorage storage;
        storage.deviceObservers.push_back(MakeRecoveryObserver(Sora::Test::TestBackendService(), state));
        auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(std::move(storage));
        REQUIRE(catalog.has_value());
        const auto descriptor = RecoveryDeviceDescriptor(state.device);
        auto epoch =
            Sora::Compute::RuntimeEpoch::Create(1, std::move(*catalog),
                                                {{.service = Sora::Test::TestBackendService().Id(),
                                                  .descriptor = descriptor,
                                                  .target = Sora::Compute::RuntimeTarget(RecoveryTarget(descriptor))}});
        REQUIRE(epoch.has_value());
        auto coordinator = Sora::Compute::RuntimeRecoveryCoordinator::Create(std::move(*epoch));
        REQUIRE(coordinator.has_value());
        return std::move(*coordinator);
    }

    std::unique_ptr<Sora::Compute::RuntimeRecoveryCoordinator>
    MakeRecoveryCoordinator(RecoveryObserverState& failedState, const Sora::Compute::BackendService& survivor,
                            RecoveryObserverState& survivorState) {
        Sora::Compute::PhysicalResourceCatalogStorage storage;
        storage.deviceObservers.push_back(MakeRecoveryObserver(Sora::Test::TestBackendService(), failedState));
        storage.deviceObservers.push_back(MakeRecoveryObserver(survivor, survivorState));
        auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(std::move(storage));
        REQUIRE(catalog.has_value());
        const auto failedDescriptor = RecoveryDeviceDescriptor(failedState.device);
        const auto survivorDescriptor = RecoveryDeviceDescriptor(survivorState.device);
        auto epoch = Sora::Compute::RuntimeEpoch::Create(
            1, std::move(*catalog),
            {{.service = Sora::Test::TestBackendService().Id(),
              .descriptor = failedDescriptor,
              .target = Sora::Compute::RuntimeTarget(RecoveryTarget(failedDescriptor))},
             {.service = survivor.Id(),
              .descriptor = survivorDescriptor,
              .target = Sora::Compute::RuntimeTarget(RecoveryTarget(survivorDescriptor))}});
        REQUIRE(epoch.has_value());
        auto coordinator = Sora::Compute::RuntimeRecoveryCoordinator::Create(std::move(*epoch));
        REQUIRE(coordinator.has_value());
        return std::move(*coordinator);
    }

    std::expected<Sora::Compute::RuntimeEpoch, Sora::Compute::RuntimeRecoveryError>
    MakeReplacementEpoch(uint64_t sequence, const Sora::Compute::BackendService& service, RecoveryObserverState& state,
                         Sora::Compute::BackendServiceId failedService) noexcept {
        try {
            auto observer = Sora::Compute::DeviceObserver::Create(service, {.device = state.device,
                                                                            .backend = Sora::Compute::BackendKind::Cpu,
                                                                            .state = &state,
                                                                            .observe = ObserveRecoveryDevice});
            if (!observer) {
                return std::unexpected(Sora::Compute::RuntimeRecoveryError{
                    .code = Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed,
                    .service = failedService,
                    .expectedSequence = sequence});
            }
            Sora::Compute::PhysicalResourceCatalogStorage storage;
            storage.deviceObservers.push_back(std::move(*observer));
            auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(std::move(storage));
            if (!catalog) {
                return std::unexpected(Sora::Compute::RuntimeRecoveryError{
                    .code = Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed,
                    .service = failedService,
                    .expectedSequence = sequence});
            }
            const auto descriptor = RecoveryDeviceDescriptor(state.device);
            auto epoch = Sora::Compute::RuntimeEpoch::Create(
                sequence, std::move(*catalog),
                {{.service = service.Id(),
                  .descriptor = descriptor,
                  .target = Sora::Compute::RuntimeTarget(RecoveryTarget(descriptor))}});
            if (!epoch) {
                return std::unexpected(Sora::Compute::RuntimeRecoveryError{
                    .code = Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed,
                    .service = failedService,
                    .expectedSequence = sequence});
            }
            return std::move(*epoch);
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                Sora::Compute::RuntimeRecoveryError{.code = Sora::Compute::RuntimeRecoveryErrorCode::OutOfMemory,
                                                    .service = failedService,
                                                    .expectedSequence = sequence});
        }
    }

    std::expected<Sora::Compute::RuntimeEpoch, Sora::Compute::RuntimeRecoveryError>
    RejectRecovery(const Sora::Compute::RuntimeRecoveryRequest& request) noexcept {
        return std::unexpected(
            Sora::Compute::RuntimeRecoveryError{.code = Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed,
                                                .service = request.failedService,
                                                .expectedSequence = request.sequence});
    }

    template<typename Error, stdexec::sender Sender>
    Error WaitForRecoveringError(Sender sender) {
        std::optional<Error> observed;
        auto recovered = std::move(sender) | stdexec::upon_error([&observed](Error error) noexcept {
                             observed.emplace(std::move(error));
                             return Sora::Compute::EventObservation{};
                         });
        auto completed = stdexec::sync_wait(std::move(recovered));
        REQUIRE(completed.has_value());
        REQUIRE(observed.has_value());
        return std::move(*observed);
    }

} // namespace

TEST_CASE("Queue and Event share ownership and destroy backend state exactly once",
          "[Sora][Compute][Backend][Lifecycle]") {
    MockQueueState state{};
    {
        Sora::Compute::Queue queue = MakeQueue(state);
        {
            const Sora::Compute::Queue copy = queue;
            REQUIRE(copy.View().state == queue.View().state);
            REQUIRE(copy.UseCount() == 2);
        }
        REQUIRE(state.queueDestroyCount.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(state.queueDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Queue validates and submits one physical dispatch batch", "[Sora][Compute][Backend][Submit]") {
    MockQueueState state{};
    {
        Sora::Compute::Queue queue = MakeQueue(state);
        Sora::Compute::Artifact artifact = MakeArtifact(state);
        Sora::Compute::Allocation allocation = MakeAllocation(state);
        const std::array bindings{
            Sora::Compute::BackendBinding{allocation.View(), 0, 1024, 0, Sora::Compute::AccessMode::ReadWrite},
        };
        const std::array commands{
            Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                          .firstBinding = 0,
                                          .bindingCount = 1,
                                          .artifact = artifact.View(),
                                          .workgroupCount = {8, 1, 1}},
        };
        const Sora::Compute::BackendSubmitRequest request{
            .commands = commands.data(),
            .commandCount = static_cast<uint32_t>(commands.size()),
            .bindings = bindings.data(),
            .bindingCount = static_cast<uint32_t>(bindings.size()),
        };

        auto submitted = queue.Submit(request);
        REQUIRE(submitted.has_value());
        REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(submitted->Query().status == Sora::Compute::EventStatus::Pending);
        REQUIRE(submitted->Wait(1'000'000).status == Sora::Compute::EventStatus::Completed);
        {
            const Sora::Compute::Event copy = *submitted;
            REQUIRE(copy.UseCount() == 2);
        }
        REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.queueDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.artifactDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Queue accepts a commandless event join and rejects orphan command data",
          "[Sora][Compute][Backend][Submit]") {
    MockQueueState state{};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Event wait = MakeEvent(state, kDevice);
    const Sora::Compute::EventView waitView = wait.View();
    const Sora::Compute::BackendSubmitRequest joinRequest{.waits = &waitView, .waitCount = 1};

    auto joined = queue.Submit(joinRequest);
    REQUIRE(joined.has_value());
    REQUIRE(state.lastCommandCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(state.lastBindingCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(joined->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);

    Sora::Compute::Allocation allocation = MakeAllocation(state);
    const Sora::Compute::BackendBinding binding{allocation.View(), 0, 16, 0, Sora::Compute::AccessMode::Read};
    const Sora::Compute::BackendSubmitRequest invalidRequest{.bindings = &binding, .bindingCount = 1};
    const auto invalid = queue.Submit(invalidRequest);
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("ExecutableBatch models an owning signal-only event join", "[Sora][Compute][Execution][Submit]") {
    MockQueueState state{};
    {
        Sora::Compute::ExecutableBatchStorage storage{
            .queue = MakeQueue(state),
            .allocations = {MakeAllocation(state)},
            .waits = {MakeEvent(state, kDevice)},
            .signalOnly = true,
        };
        auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
        REQUIRE(batch.has_value());
        REQUIRE(batch->Request().commandCount == 0);
        REQUIRE(batch->Request().waitCount == 1);

        auto submitted = std::move(*batch).Submit();
        REQUIRE(submitted.has_value());
        REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
        REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Queue rejects malformed commands before calling the backend", "[Sora][Compute][Backend][Validation]") {
    MockQueueState state{};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    const std::array commands{
        Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                      .artifact = artifact.View(),
                                      .workgroupCount = {0, 1, 1}},
    };
    const Sora::Compute::BackendSubmitRequest request{
        .commands = commands.data(),
        .commandCount = static_cast<uint32_t>(commands.size()),
    };

    const auto submitted = queue.Submit(request);
    REQUIRE_FALSE(submitted.has_value());
    REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Queue rejects an allocation owned by another device before submission",
          "[Sora][Compute][Backend][Validation]") {
    MockQueueState state{};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    constexpr Sora::Compute::MemoryLocation foreignLocation{
        .domain = kLocation.domain,
        .device = {0xFA, 0x11},
        .backend = Sora::Compute::BackendKind::Cpu,
    };
    Sora::Compute::Allocation allocation = MakeAllocation(state, foreignLocation);
    const std::array bindings{
        Sora::Compute::BackendBinding{allocation.View(), 0, 1024, 0, Sora::Compute::AccessMode::Read},
    };
    const std::array commands{
        Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                      .firstBinding = 0,
                                      .bindingCount = 1,
                                      .artifact = artifact.View(),
                                      .workgroupCount = {1, 1, 1}},
    };
    const Sora::Compute::BackendSubmitRequest request{
        .commands = commands.data(),
        .commandCount = static_cast<uint32_t>(commands.size()),
        .bindings = bindings.data(),
        .bindingCount = static_cast<uint32_t>(bindings.size()),
    };

    const auto submitted = queue.Submit(request);
    REQUIRE_FALSE(submitted.has_value());
    REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Queue rejects a wait event owned by another device before submission",
          "[Sora][Compute][Backend][Validation]") {
    MockQueueState state{};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    Sora::Compute::Event foreign = MakeEvent(state, {0xFA, 0x12});
    const std::array commands{
        Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                      .artifact = artifact.View(),
                                      .workgroupCount = {1, 1, 1}},
    };
    const std::array waits{foreign.View()};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = commands.data(),
        .commandCount = static_cast<uint32_t>(commands.size()),
        .waits = waits.data(),
        .waitCount = static_cast<uint32_t>(waits.size()),
    };

    const auto submitted = queue.Submit(request);
    REQUIRE_FALSE(submitted.has_value());
    REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Queue submits affine buffer and scalar bindings without backend-private side channels",
          "[Sora][Compute][Backend][Binding]") {
    MockQueueState state;
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    Sora::Compute::Allocation allocation = MakeAllocation(state);
    Sora::Compute::BackendBinding buffer{allocation.View(),
                                         0,
                                         256,
                                         0,
                                         Sora::Compute::AccessMode::Read,
                                         Sora::Compute::BackendBindingKind::AffineElements,
                                         {Sora::Compute::ScalarKind::Float32, 1},
                                         Sora::Compute::LayoutKind::Contiguous,
                                         1};
    buffer.extents[0] = 64;
    buffer.byteStrides[0] = 4;
    buffer.componentStrideBytes = 4;
    constexpr std::array scalarData{std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0x3F}};
    constexpr std::array scalarBindings{
        Sora::Compute::BackendScalarBinding{0, 4, 1, {Sora::Compute::ScalarKind::Float32, 1}}};
    const std::array bindings{buffer};
    const std::array commands{Sora::Compute::BackendCommand{
        .kind = Sora::Compute::BackendCommandKind::Dispatch,
        .firstBinding = 0,
        .bindingCount = 1,
        .firstScalarBinding = 0,
        .scalarBindingCount = 1,
        .artifact = artifact.View(),
        .workgroupCount = {1, 1, 1},
    }};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = commands.data(),
        .commandCount = static_cast<uint32_t>(commands.size()),
        .bindings = bindings.data(),
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .scalarData = scalarData.data(),
        .scalarDataSize = scalarData.size(),
        .scalarBindings = scalarBindings.data(),
        .scalarBindingCount = static_cast<uint32_t>(scalarBindings.size()),
    };

    const auto event = queue.Submit(request);
    REQUIRE(event.has_value());
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.lastScalarBindingCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.lastScalarDataSize.load(std::memory_order_relaxed) == 4);
}

TEST_CASE("Queue rejects non-canonical scalar bytes before backend submission",
          "[Sora][Compute][Backend][Binding][Validation]") {
    MockQueueState state;
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    constexpr std::array scalarData{std::byte{2}};
    constexpr std::array scalarBindings{
        Sora::Compute::BackendScalarBinding{0, 1, 0, {Sora::Compute::ScalarKind::Bool, 1}}};
    const std::array commands{Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                            .firstScalarBinding = 0,
                                                            .scalarBindingCount = 1,
                                                            .artifact = artifact.View()}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = commands.data(),
        .commandCount = static_cast<uint32_t>(commands.size()),
        .scalarData = scalarData.data(),
        .scalarDataSize = scalarData.size(),
        .scalarBindings = scalarBindings.data(),
        .scalarBindingCount = static_cast<uint32_t>(scalarBindings.size()),
    };

    const auto event = queue.Submit(request);
    REQUIRE_FALSE(event.has_value());
    REQUIRE(event.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Queue accepts a sixteen-byte fill pattern", "[Sora][Compute][Backend][Fill]") {
    MockQueueState state;
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Allocation allocation = MakeAllocation(state);
    Sora::Compute::BackendCommand fill{.kind = Sora::Compute::BackendCommandKind::Fill,
                                       .target = allocation.View(),
                                       .sizeBytes = 256,
                                       .fillPatternSize = 16};
    for (uint8_t index = 0; index < 16; ++index) {
        fill.fillPattern[index] = index;
    }
    const Sora::Compute::BackendSubmitRequest request{.commands = &fill, .commandCount = 1};
    const auto event = queue.Submit(request);
    REQUIRE(event.has_value());
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Queue detects overlap across distinct handles of the same physical memory",
          "[Sora][Compute][Backend][Copy][Alias]") {
    MockQueueState state;
    Sora::Compute::Queue queue = MakeQueue(state);
    constexpr Sora::Compute::PhysicalMemoryId memory{0xAA, 0xBB};
    Sora::Compute::Allocation source = MakeAllocation(state, kLocation, memory, 0);
    Sora::Compute::Allocation target = MakeAllocation(state, kLocation, memory, 128);
    const Sora::Compute::BackendCommand copy{.kind = Sora::Compute::BackendCommandKind::Copy,
                                             .source = source.View(),
                                             .target = target.View(),
                                             .sizeBytes = 256};
    const Sora::Compute::BackendSubmitRequest request{.commands = &copy, .commandCount = 1};
    const auto event = queue.Submit(request);
    REQUIRE_FALSE(event.has_value());
    REQUIRE(event.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Queue transactionally destroys an event with the wrong identity", "[Sora][Compute][Backend][Validation]") {
    MockQueueState state{.returnWrongDevice = true};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    const std::array commands{
        Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                      .artifact = artifact.View(),
                                      .workgroupCount = {1, 1, 1}},
    };
    const Sora::Compute::BackendSubmitRequest request{
        .commands = commands.data(),
        .commandCount = static_cast<uint32_t>(commands.size()),
    };

    const auto submitted = queue.Submit(request);
    REQUIRE_FALSE(submitted.has_value());
    REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidEvent);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Event normalizes malformed backend observations", "[Sora][Compute][Backend][Event]") {
    MockQueueState state{.returnMalformedObservation = true};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    const std::array commands{
        Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                      .artifact = artifact.View(),
                                      .workgroupCount = {1, 1, 1}},
    };
    const Sora::Compute::BackendSubmitRequest request{
        .commands = commands.data(),
        .commandCount = static_cast<uint32_t>(commands.size()),
    };

    auto submitted = queue.Submit(request);
    REQUIRE(submitted.has_value());
    const Sora::Compute::EventObservation observation = submitted->Query();
    REQUIRE(observation.status == Sora::Compute::EventStatus::Failed);
    REQUIRE(observation.error.code == Sora::Compute::SubmissionErrorCode::InvalidEvent);
}

TEST_CASE("Event rejects execution duration before completion", "[Sora][Compute][Backend][Event][Timing]") {
    MockQueueState state{.returnTimedPendingObservation = true};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    const Sora::Compute::BackendCommand command{
        .kind = Sora::Compute::BackendCommandKind::Dispatch, .artifact = artifact.View(), .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{.commands = &command, .commandCount = 1};

    auto submitted = queue.Submit(request);
    REQUIRE(submitted.has_value());
    const Sora::Compute::EventObservation observation = submitted->Query();
    REQUIRE(observation.status == Sora::Compute::EventStatus::Failed);
    REQUIRE(observation.error.code == Sora::Compute::SubmissionErrorCode::InvalidEvent);
    REQUIRE(observation.executionNanoseconds == 0);
}

TEST_CASE("PhysicalSubmission publishes completed kernel telemetry exactly once",
          "[Sora][Compute][Runtime][PhysicalSegment][Telemetry]") {
    constexpr uint64_t executionNanoseconds = 321;
    MockQueueState state{.executionNanoseconds = executionNanoseconds};
    KernelTelemetryState telemetry;
    Sora::Compute::PhysicalSegment segment{MakeBatch(state),
                                           {.key = {.kernel = kCompileRequest.moduleHash,
                                                    .target = kCompileRequest.target,
                                                    .specialization = kCompileRequest.specialization,
                                                    .queue = {1, 1}},
                                            .workItemCount = 8,
                                            .deterministic = true},
                                           {.context = &telemetry, .observe = ObserveKernelTelemetry}};

    auto submitted = std::move(segment).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    REQUIRE(submitted->Query().status == Sora::Compute::EventStatus::Completed);
    REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(telemetry.observationCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(telemetry.observation.key.kernel == kCompileRequest.moduleHash);
    REQUIRE(telemetry.observation.workItemCount == 8);
    REQUIRE(telemetry.observation.computeNanoseconds == executionNanoseconds);
    REQUIRE(telemetry.observation.deterministic);
}

TEST_CASE("PhysicalSubmission publishes completed kernel telemetry once under concurrent observation",
          "[Sora][Compute][Runtime][PhysicalSegment][Telemetry][Concurrency]") {
    constexpr size_t observerCount = 16;
    constexpr uint64_t executionNanoseconds = 654;
    MockQueueState state{.executionNanoseconds = executionNanoseconds, .completeEventsOnSubmit = true};
    KernelTelemetryState telemetry;
    Sora::Compute::PhysicalSegment segment{MakeBatch(state),
                                           {.key = {.kernel = kCompileRequest.moduleHash,
                                                    .target = kCompileRequest.target,
                                                    .specialization = kCompileRequest.specialization,
                                                    .queue = {1, 1}},
                                            .workItemCount = 16,
                                            .deterministic = true},
                                           {.context = &telemetry, .observe = ObserveKernelTelemetry}};

    auto submitted = std::move(segment).Submit();
    REQUIRE(submitted.has_value());
    std::barrier start{static_cast<ptrdiff_t>(observerCount)};
    std::array<std::atomic_bool, observerCount> completed{};
    std::array<std::jthread, observerCount> observers;
    for (size_t index = 0; index < observerCount; ++index) {
        observers[index] = std::jthread([&submitted, &start, &completed, index] {
            start.arrive_and_wait();
            completed[index].store(submitted->Query().status == Sora::Compute::EventStatus::Completed,
                                   std::memory_order_relaxed);
        });
    }
    for (auto& observer : observers) {
        observer.join();
    }
    for (const auto& result : completed) {
        REQUIRE(result.load(std::memory_order_relaxed));
    }
    REQUIRE(telemetry.observationCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(telemetry.observation.computeNanoseconds == executionNanoseconds);
}

TEST_CASE("Queue exposes backend execution timing capability without changing command semantics",
          "[Sora][Compute][Backend][Queue][Timing]") {
    MockQueueState state;
    const Sora::Compute::Queue untimed = MakeQueue(state);
    const Sora::Compute::Queue timed =
        MakeQueue(state,
                  Sora::Compute::QueueCapability::Dispatch | Sora::Compute::QueueCapability::Copy |
                      Sora::Compute::QueueCapability::Fill,
                  Sora::Test::TestBackendService(), kDevice, true);

    REQUIRE_FALSE(untimed.View().reportsExecutionNanoseconds);
    REQUIRE(timed.View().reportsExecutionNanoseconds);
    REQUIRE(timed.View().capabilities == untimed.View().capabilities);

    auto invalidNative = std::make_unique<MockQueueNative>(&state, kDevice);
    auto invalid = Sora::Compute::Queue::Adopt(Sora::Test::TestBackendService(),
                                               {.id = {2, 2},
                                                .device = kDevice,
                                                .backend = Sora::Compute::BackendKind::Cpu,
                                                .capabilities = Sora::Compute::QueueCapability::Dispatch,
                                                .state = invalidNative.release(),
                                                .destroy = DestroyQueue,
                                                .submit = SubmitMock,
                                                .reportsExecutionNanoseconds = 2});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error() == Sora::Compute::QueueError::InvalidPayload);
    REQUIRE(state.queueDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Queue and Event reject error codes outside the backend ABI domain", "[Sora][Compute][Backend][Validation]") {
    {
        MockQueueState state{.returnInvalidSubmitError = true};
        Sora::Compute::Queue queue = MakeQueue(state);
        Sora::Compute::Artifact artifact = MakeArtifact(state);
        const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                    .artifact = artifact.View(),
                                                    .workgroupCount = {1, 1, 1}};
        const Sora::Compute::BackendSubmitRequest request{.commands = &command, .commandCount = 1};
        const auto submitted = queue.Submit(request);
        REQUIRE_FALSE(submitted.has_value());
        REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidEvent);
    }
    {
        MockQueueState state{.returnInvalidObservationError = true};
        Sora::Compute::Queue queue = MakeQueue(state);
        Sora::Compute::Artifact artifact = MakeArtifact(state);
        const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                    .artifact = artifact.View(),
                                                    .workgroupCount = {1, 1, 1}};
        const Sora::Compute::BackendSubmitRequest request{.commands = &command, .commandCount = 1};
        auto submitted = queue.Submit(request);
        REQUIRE(submitted.has_value());
        const auto observation = submitted->Query();
        REQUIRE(observation.status == Sora::Compute::EventStatus::Failed);
        REQUIRE(observation.error.code == Sora::Compute::SubmissionErrorCode::InvalidEvent);
    }
}

TEST_CASE("Queue rejects explicit dispatch slices with non-canonical inactive axes",
          "[Sora][Compute][Backend][Validation][Partition]") {
    MockQueueState state{};
    Sora::Compute::Queue queue = MakeQueue(state);
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .artifact = artifact.View(),
                                                .dispatchExtents = {16, 0, 0},
                                                .globalExtents = {32, 1, 0},
                                                .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{.commands = &command, .commandCount = 1};

    const auto submitted = queue.Submit(request);
    REQUIRE_FALSE(submitted.has_value());
    REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("ExecutableBatch rejects physical views without owning leases", "[Sora][Compute][Execution][Validation]") {
    MockQueueState state{};
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    Sora::Compute::ExecutableBatchStorage storage{
        .queue = MakeQueue(state),
        .commands = {Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                   .artifact = artifact.View(),
                                                   .workgroupCount = {1, 1, 1}}},
    };
    const auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
    REQUIRE_FALSE(batch.has_value());
    REQUIRE(batch.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
}

TEST_CASE("ExecutableBatch rejects a forged artifact view with an owned state",
          "[Sora][Compute][Execution][Validation]") {
    MockQueueState state{};
    Sora::Compute::Artifact artifact = MakeArtifact(state);
    auto forged = artifact.View();
    ++forged.device.low;
    Sora::Compute::ExecutableBatchStorage storage{
        .queue = MakeQueue(state),
        .commands = {Sora::Compute::BackendCommand{
            .kind = Sora::Compute::BackendCommandKind::Dispatch, .artifact = forged, .workgroupCount = {1, 1, 1}}},
        .artifacts = {artifact},
    };
    const auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
    REQUIRE_FALSE(batch.has_value());
    REQUIRE(batch.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
}

TEST_CASE("SubmitOn completes with a value and retains every lease until completion",
          "[Sora][Compute][Execution][Sender]") {
    MockQueueState state{};
    exec::static_thread_pool pool{1};
    auto result = stdexec::sync_wait(Sora::Compute::SubmitOn(pool.get_scheduler(), MakeBatch(state)));
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.artifactDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SubmitOn exposes backend submission failure on the typed error channel",
          "[Sora][Compute][Execution][Sender]") {
    MockQueueState state{.failSubmit = true};
    exec::static_thread_pool pool{1};
    std::optional<Sora::Compute::SubmissionError> observedError;
    auto sender = Sora::Compute::SubmitOn(pool.get_scheduler(), MakeBatch(state)) |
                  stdexec::upon_error([&observedError](Sora::Compute::SubmissionError error) noexcept {
                      observedError = error;
                      return Sora::Compute::EventObservation{};
                  });
    const auto result = stdexec::sync_wait(std::move(sender));
    REQUIRE(result.has_value());
    REQUIRE(observedError.has_value());
    REQUIRE(observedError->code == Sora::Compute::SubmissionErrorCode::QueueUnavailable);
    REQUIRE(observedError->detail == 41);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SubmitOn honors stop before submission", "[Sora][Compute][Execution][Sender]") {
    MockQueueState state{};
    exec::static_thread_pool pool{1};
    stdexec::inplace_stop_source stopSource;
    stopSource.request_stop();
    auto sender = Sora::Compute::SubmitOn(pool.get_scheduler(), MakeBatch(state)) |
                  stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stopSource.get_token()});
    const auto result = stdexec::sync_wait(std::move(sender));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("SubmitOn drains accepted work before reporting stop", "[Sora][Compute][Execution][Sender]") {
    MockQueueState state{};
    exec::static_thread_pool pool{1};
    stdexec::inplace_stop_source stopSource;
    state.stopOnWait = &stopSource;
    auto sender = Sora::Compute::SubmitOn(pool.get_scheduler(), MakeBatch(state)) |
                  stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stopSource.get_token()});
    const auto result = stdexec::sync_wait(std::move(sender));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SubmitOn completes an executable graph on the value channel", "[Sora][Compute][Execution][Sender][Graph]") {
    MockQueueState state{};
    exec::static_thread_pool pool{1};
    auto result = stdexec::sync_wait(Sora::Compute::SubmitOn(pool.get_scheduler(), MakeGraph(state)));
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SubmitOn exposes executable graph submission failure with its physical node",
          "[Sora][Compute][Execution][Sender][Graph]") {
    MockQueueState state{.failSubmit = true};
    exec::static_thread_pool pool{1};
    std::optional<Sora::Compute::GraphSubmissionError> observedError;
    auto sender = Sora::Compute::SubmitOn(pool.get_scheduler(), MakeGraph(state)) |
                  stdexec::upon_error([&observedError](Sora::Compute::GraphSubmissionError error) noexcept {
                      observedError = error;
                      return Sora::Compute::EventObservation{};
                  });
    const auto result = stdexec::sync_wait(std::move(sender));
    REQUIRE(result.has_value());
    REQUIRE(observedError.has_value());
    REQUIRE(observedError->node == 0);
    REQUIRE(observedError->error.code == Sora::Compute::SubmissionErrorCode::QueueUnavailable);
    REQUIRE(observedError->error.detail == 41);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SubmitOn honors stop before executable graph submission", "[Sora][Compute][Execution][Sender][Graph]") {
    MockQueueState state{};
    exec::static_thread_pool pool{1};
    stdexec::inplace_stop_source stopSource;
    stopSource.request_stop();
    auto sender = Sora::Compute::SubmitOn(pool.get_scheduler(), MakeGraph(state)) |
                  stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stopSource.get_token()});
    const auto result = stdexec::sync_wait(std::move(sender));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("SubmitOn drains an accepted executable graph before reporting stop",
          "[Sora][Compute][Execution][Sender][Graph]") {
    MockQueueState state{};
    exec::static_thread_pool pool{1};
    stdexec::inplace_stop_source stopSource;
    state.stopOnWait = &stopSource;
    auto sender = Sora::Compute::SubmitOn(pool.get_scheduler(), MakeGraph(state)) |
                  stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stopSource.get_token()});
    const auto result = stdexec::sync_wait(std::move(sender));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SubmitOn retains a batch across a spurious pending infinite wait",
          "[Sora][Compute][Execution][Sender][Validation]") {
    MockQueueState state{.returnPendingOnWait = true};
    exec::static_thread_pool pool{1};
    const auto result = stdexec::sync_wait(Sora::Compute::SubmitOn(pool.get_scheduler(), MakeBatch(state)));
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SubmitOn retains a graph across a spurious pending infinite wait",
          "[Sora][Compute][Execution][Sender][Graph][Validation]") {
    MockQueueState state{.returnPendingOnWait = true};
    exec::static_thread_pool pool{1};
    const auto result = stdexec::sync_wait(Sora::Compute::SubmitOn(pool.get_scheduler(), MakeGraph(state)));
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("RecoveringSubmitOn builds a fresh graph after transient queue unavailability",
          "[Sora][Compute][Execution][Sender][Recovery]") {
    MockQueueState state{};
    RecoveryObserverState observerState{};
    auto coordinator = MakeRecoveryCoordinator(observerState);
    std::atomic<uint32_t> factoryCalls = 0;
    std::atomic<bool> observedPreviousFailure = false;
    auto factory = [&](const Sora::Compute::GraphAttemptContext& context) noexcept
        -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
        const uint32_t call = factoryCalls.fetch_add(1, std::memory_order_relaxed);
        state.failSubmit = call == 0;
        if (call == 1) {
            observedPreviousFailure.store(context.previousSubmission != nullptr &&
                                              context.previousSubmission->error.code ==
                                                  Sora::Compute::SubmissionErrorCode::QueueUnavailable,
                                          std::memory_order_relaxed);
        }
        return MakeGraph(state);
    };
    auto provider = [](const Sora::Compute::RuntimeRecoveryRequest& request) noexcept
        -> std::expected<Sora::Compute::RuntimeEpoch, Sora::Compute::RuntimeRecoveryError> {
        return std::unexpected(
            Sora::Compute::RuntimeRecoveryError{.code = Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed,
                                                .service = request.failedService,
                                                .expectedSequence = request.sequence});
    };

    exec::static_thread_pool pool{1};
    auto completed = stdexec::sync_wait(Sora::Compute::RecoveringSubmitOn(pool.get_scheduler(), *coordinator,
                                                                          {.maximumAttempts = 2}, factory, provider));
    REQUIRE(completed.has_value());
    REQUIRE(std::get<0>(*completed).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(factoryCalls.load(std::memory_order_relaxed) == 2);
    REQUIRE(observedPreviousFailure.load(std::memory_order_relaxed));
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("RecoveringSubmitOn rebuilds the runtime epoch and rematerializes on a survivor service",
          "[Sora][Compute][Execution][Sender][Recovery][DeviceLoss]") {
    MockQueueState state{.failCode = Sora::Compute::SubmissionErrorCode::DeviceLost};
    RecoveryObserverState failedState{};
    RecoveryObserverState survivorState{.device = kSurvivorDevice};
    Sora::Compute::BackendService survivor = MakeRecoveryBackendService(kSurvivorService);
    auto coordinator = MakeRecoveryCoordinator(failedState, survivor, survivorState);
    std::atomic<uint32_t> factoryCalls = 0;
    std::atomic<uint32_t> providerCalls = 0;
    std::atomic<bool> observedReplacement = false;

    auto factory = [&](const Sora::Compute::GraphAttemptContext& context) noexcept
        -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
        const uint32_t call = factoryCalls.fetch_add(1, std::memory_order_relaxed);
        if (call == 0) {
            state.failSubmit = true;
            failedState.state.store(Sora::Compute::DeviceState::Lost, std::memory_order_release);
            return MakeGraph(state, Sora::Test::TestBackendService(), kDevice);
        }
        state.failSubmit = false;
        const bool replacementValid =
            context.epoch && context.epoch->Sequence() == 2 && context.epoch->Devices().size() == 1 &&
            context.epoch->Devices()[0].service == survivor.Id() && context.previousSubmission != nullptr &&
            context.previousSubmission->error.code == Sora::Compute::SubmissionErrorCode::DeviceLost;
        observedReplacement.store(replacementValid, std::memory_order_relaxed);
        return MakeGraph(state, survivor, kSurvivorDevice);
    };
    auto provider = [&](const Sora::Compute::RuntimeRecoveryRequest& request) noexcept
        -> std::expected<Sora::Compute::RuntimeEpoch, Sora::Compute::RuntimeRecoveryError> {
        providerCalls.fetch_add(1, std::memory_order_relaxed);
        return MakeReplacementEpoch(request.sequence, survivor, survivorState, request.failedService);
    };

    exec::static_thread_pool pool{1};
    auto completed = stdexec::sync_wait(Sora::Compute::RecoveringSubmitOn(pool.get_scheduler(), *coordinator,
                                                                          {.maximumAttempts = 2}, factory, provider));
    REQUIRE(completed.has_value());
    REQUIRE(std::get<0>(*completed).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(factoryCalls.load(std::memory_order_relaxed) == 2);
    REQUIRE(providerCalls.load(std::memory_order_relaxed) == 1);
    REQUIRE(observedReplacement.load(std::memory_order_relaxed));
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 2);
    const auto current = coordinator->Current();
    REQUIRE(current->Sequence() == 2);
    REQUIRE(current->Devices().size() == 1);
    REQUIRE(current->Devices()[0].service == survivor.Id());
}

TEST_CASE("RecoveringSubmitOn preserves every typed terminal failure",
          "[Sora][Compute][Execution][Sender][Recovery][Error]") {
    using Error = Sora::Compute::RecoveringGraphError<Sora::Compute::ExecutableGraphError>;

    SECTION("invalid policy") {
        MockQueueState state{};
        RecoveryObserverState observerState{};
        auto coordinator = MakeRecoveryCoordinator(observerState);
        std::atomic<uint32_t> factoryCalls = 0;
        auto factory = [&](const Sora::Compute::GraphAttemptContext&) noexcept
            -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
            factoryCalls.fetch_add(1, std::memory_order_relaxed);
            return MakeGraph(state);
        };
        exec::static_thread_pool pool{1};
        Error error = WaitForRecoveringError<Error>(Sora::Compute::RecoveringSubmitOn(
            pool.get_scheduler(), *coordinator, {.maximumAttempts = 0}, factory, RejectRecovery));
        REQUIRE(std::holds_alternative<Sora::Compute::InvalidGraphRecoveryPolicy>(error));
        REQUIRE(std::get<Sora::Compute::InvalidGraphRecoveryPolicy>(error).policy.maximumAttempts == 0);
        REQUIRE(factoryCalls.load(std::memory_order_relaxed) == 0);
    }

    SECTION("attempt build failure") {
        RecoveryObserverState observerState{};
        auto coordinator = MakeRecoveryCoordinator(observerState);
        observerState.state.store(Sora::Compute::DeviceState::Unavailable, std::memory_order_release);
        std::atomic<uint32_t> factoryCalls = 0;
        std::atomic<bool> observedFreshSnapshot = false;
        auto factory = [&](const Sora::Compute::GraphAttemptContext& context) noexcept
            -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
            factoryCalls.fetch_add(1, std::memory_order_relaxed);
            observedFreshSnapshot.store(
                context.devices.size() == 1 && context.snapshots.size() == 1 &&
                    context.snapshots[0].state == Sora::Compute::DeviceState::Unavailable && context.epoch &&
                    context.epoch->Snapshots()[0].state == Sora::Compute::DeviceState::Available,
                std::memory_order_relaxed);
            return std::unexpected(Sora::Compute::ExecutableGraphError{
                .code = Sora::Compute::ExecutableGraphErrorCode::InvalidEdge, .predecessor = 3, .successor = 4});
        };
        exec::static_thread_pool pool{1};
        Error error = WaitForRecoveringError<Error>(Sora::Compute::RecoveringSubmitOn(
            pool.get_scheduler(), *coordinator, {.maximumAttempts = 2}, factory, RejectRecovery));
        REQUIRE(std::holds_alternative<Sora::Compute::GraphAttemptBuildFailure<Sora::Compute::ExecutableGraphError>>(
            error));
        const auto& failure =
            std::get<Sora::Compute::GraphAttemptBuildFailure<Sora::Compute::ExecutableGraphError>>(error);
        REQUIRE(failure.attempt == 0);
        REQUIRE(failure.error.code == Sora::Compute::ExecutableGraphErrorCode::InvalidEdge);
        REQUIRE(failure.error.predecessor == 3);
        REQUIRE(failure.error.successor == 4);
        REQUIRE(factoryCalls.load(std::memory_order_relaxed) == 1);
        REQUIRE(observedFreshSnapshot.load(std::memory_order_relaxed));
    }

    SECTION("non-retryable submission failure") {
        MockQueueState state{.failSubmit = true, .failCode = Sora::Compute::SubmissionErrorCode::OutOfMemory};
        RecoveryObserverState observerState{};
        auto coordinator = MakeRecoveryCoordinator(observerState);
        auto factory = [&](const Sora::Compute::GraphAttemptContext&) noexcept
            -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
            return MakeGraph(state);
        };
        exec::static_thread_pool pool{1};
        Error error = WaitForRecoveringError<Error>(Sora::Compute::RecoveringSubmitOn(
            pool.get_scheduler(), *coordinator, {.maximumAttempts = 2}, factory, RejectRecovery));
        REQUIRE(std::holds_alternative<Sora::Compute::TerminalGraphSubmissionFailure>(error));
        const auto& failure = std::get<Sora::Compute::TerminalGraphSubmissionFailure>(error);
        REQUIRE(failure.attempt == 0);
        REQUIRE(failure.error.node == 0);
        REQUIRE(failure.error.error.code == Sora::Compute::SubmissionErrorCode::OutOfMemory);
        REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);
    }

    SECTION("runtime reconstruction failure") {
        MockQueueState state{.failSubmit = true, .failCode = Sora::Compute::SubmissionErrorCode::DeviceLost};
        RecoveryObserverState failedState{};
        RecoveryObserverState survivorState{.device = kSurvivorDevice};
        Sora::Compute::BackendService survivor = MakeRecoveryBackendService(kSurvivorService);
        auto coordinator = MakeRecoveryCoordinator(failedState, survivor, survivorState);
        auto factory = [&](const Sora::Compute::GraphAttemptContext&) noexcept
            -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
            failedState.state.store(Sora::Compute::DeviceState::Lost, std::memory_order_release);
            return MakeGraph(state);
        };
        exec::static_thread_pool pool{1};
        Error error = WaitForRecoveringError<Error>(Sora::Compute::RecoveringSubmitOn(
            pool.get_scheduler(), *coordinator, {.maximumAttempts = 2}, factory, RejectRecovery));
        REQUIRE(std::holds_alternative<Sora::Compute::GraphRuntimeRecoveryFailure>(error));
        const auto& failure = std::get<Sora::Compute::GraphRuntimeRecoveryFailure>(error);
        REQUIRE(failure.attempt == 0);
        REQUIRE(failure.error.code == Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed);
        REQUIRE(failure.error.expectedSequence == 2);
        REQUIRE(coordinator->Current()->Sequence() == 1);
        REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 1);

        state.failSubmit = false;
        std::atomic<bool> observedReplacement = false;
        auto replacementFactory = [&](const Sora::Compute::GraphAttemptContext& context) noexcept
            -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
            observedReplacement.store(
                context.epoch && context.epoch->Sequence() == 2 && context.epoch->Devices().size() == 1 &&
                    context.epoch->Devices()[0].service == survivor.Id() && context.previousSubmission == nullptr,
                std::memory_order_relaxed);
            return MakeGraph(state, survivor, kSurvivorDevice);
        };
        auto replacementProvider = [&](const Sora::Compute::RuntimeRecoveryRequest& request) noexcept
            -> std::expected<Sora::Compute::RuntimeEpoch, Sora::Compute::RuntimeRecoveryError> {
            return MakeReplacementEpoch(request.sequence, survivor, survivorState, request.failedService);
        };
        auto completed = stdexec::sync_wait(Sora::Compute::RecoveringSubmitOn(
            pool.get_scheduler(), *coordinator, {.maximumAttempts = 2}, replacementFactory, replacementProvider));
        REQUIRE(completed.has_value());
        REQUIRE(observedReplacement.load(std::memory_order_relaxed));
        REQUIRE(coordinator->Current()->Sequence() == 2);
        REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 2);
    }

    SECTION("recovery disabled for a pre-existing loss") {
        MockQueueState state{};
        RecoveryObserverState observerState{};
        auto coordinator = MakeRecoveryCoordinator(observerState);
        observerState.state.store(Sora::Compute::DeviceState::Lost, std::memory_order_release);
        std::atomic<uint32_t> factoryCalls = 0;
        auto factory = [&](const Sora::Compute::GraphAttemptContext&) noexcept
            -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
            factoryCalls.fetch_add(1, std::memory_order_relaxed);
            return MakeGraph(state);
        };
        exec::static_thread_pool pool{1};
        Error error = WaitForRecoveringError<Error>(Sora::Compute::RecoveringSubmitOn(
            pool.get_scheduler(), *coordinator, {.maximumAttempts = 2, .recoverDeviceLoss = false}, factory,
            RejectRecovery));
        REQUIRE(std::holds_alternative<Sora::Compute::GraphRuntimeObservationFailure>(error));
        const auto& failure = std::get<Sora::Compute::GraphRuntimeObservationFailure>(error);
        REQUIRE(failure.attempt == 0);
        REQUIRE(failure.error.code == Sora::Compute::RuntimeEpochErrorCode::DeviceLost);
        REQUIRE(factoryCalls.load(std::memory_order_relaxed) == 0);
    }
}

TEST_CASE("RecoveringSubmitOn reports the final retryable error at its exact attempt bound",
          "[Sora][Compute][Execution][Sender][Recovery][Bounded]") {
    MockQueueState state{.failSubmit = true};
    RecoveryObserverState observerState{};
    auto coordinator = MakeRecoveryCoordinator(observerState);
    std::atomic<uint32_t> factoryCalls = 0;
    auto factory = [&](const Sora::Compute::GraphAttemptContext&) noexcept
        -> std::expected<Sora::Compute::ExecutableGraph, Sora::Compute::ExecutableGraphError> {
        factoryCalls.fetch_add(1, std::memory_order_relaxed);
        return MakeGraph(state);
    };
    auto provider = [](const Sora::Compute::RuntimeRecoveryRequest& request) noexcept
        -> std::expected<Sora::Compute::RuntimeEpoch, Sora::Compute::RuntimeRecoveryError> {
        return std::unexpected(
            Sora::Compute::RuntimeRecoveryError{.code = Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed,
                                                .service = request.failedService,
                                                .expectedSequence = request.sequence});
    };
    using Error = Sora::Compute::RecoveringGraphError<Sora::Compute::ExecutableGraphError>;
    exec::static_thread_pool pool{1};
    Error error = WaitForRecoveringError<Error>(Sora::Compute::RecoveringSubmitOn(
        pool.get_scheduler(), *coordinator, {.maximumAttempts = 2}, factory, provider));
    REQUIRE(std::holds_alternative<Sora::Compute::GraphRetryLimitExceeded>(error));
    REQUIRE(std::get<Sora::Compute::GraphRetryLimitExceeded>(error).attempts == 2);
    REQUIRE(factoryCalls.load(std::memory_order_relaxed) == 2);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Portable compute components compose from compile through residency publication",
          "[Sora][Compute][Execution][EndToEnd]") {
    MockQueueState state{};
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    const auto compiler = MakeCompiler(state);

    auto compiled = compilerService.Resolve(compiler, kCompileRequest);
    if (!compiled) {
        INFO("compile error code=" << static_cast<uint32_t>(compiled.error().code)
                                   << " detail=" << compiled.error().detail);
    }
    REQUIRE(compiled.has_value());
    auto cached = compilerService.Resolve(compiler, kCompileRequest);
    REQUIRE(cached.has_value());
    REQUIRE(cached->View().state == compiled->View().state);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);

    constexpr Sora::Compute::WorkloadDesc workload{
        .kernelHash = kCompileRequest.moduleHash,
        .requiredFeatures = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
        .resourceDemand = {.floatingPointOperations = 2, .bytesRead = 8, .bytesWritten = 4},
        .workItemCount = 256,
        .requiredDeviceMemoryBytes = 2048,
        .largestAllocationBytes = 1024,
        .expectedReuseCount = 8,
    };
    constexpr std::array candidates{
        Sora::Compute::PlacementCandidate{.service = {1, 1},
                                          .device = {.id = kDevice,
                                                     .backend = Sora::Compute::BackendKind::Cpu,
                                                     .kind = Sora::Compute::DeviceKind::Cpu,
                                                     .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
                                                     .architectureId = 0x7838365F3634,
                                                     .totalMemoryBytes = 1ULL << 30,
                                                     .maximumAllocationBytes = 1ULL << 29,
                                                     .computeUnitCount = 8},
                                          .snapshot = {.id = kDevice,
                                                       .state = Sora::Compute::DeviceState::Available,
                                                       .generation = 1,
                                                       .availableMemoryBytes = 1ULL << 29},
                                          .executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live},
                                          .workItemsPerSecond = 1'000'000,
                                          .transfer = Sora::Compute::CandidateTransferCost{1024, 1'024},
                                          .launchNanoseconds = 1'000},
        Sora::Compute::PlacementCandidate{
            .service = {2, 2},
            .device = {.id = {0xD0, 0xD0},
                       .backend = Sora::Compute::BackendKind::Cpu,
                       .kind = Sora::Compute::DeviceKind::Cpu,
                       .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
                       .maximumAllocationBytes = 1ULL << 29},
            .snapshot = {.id = {0xD0, 0xD0},
                         .state = Sora::Compute::DeviceState::Available,
                         .generation = 1,
                         .availableMemoryBytes = 1ULL << 29},
            .executable = {.mode = Sora::Compute::ArtifactResolutionMode::TransientProduction,
                           .productionNanoseconds = 1'000'000},
            .workItemsPerSecond = 100'000,
            .transfer = Sora::Compute::CandidateTransferCost{1024, 10'240},
            .launchNanoseconds = 10'000},
    };
    constexpr auto plan = Sora::Compute::PlanExecution(workload, candidates);
    static_assert(plan.has_value());
    static_assert(plan->candidateIndex == 0);
    constexpr auto refreshed = [](auto candidate) {
        candidate.snapshot.observationSequence = 99;
        return candidate;
    }(candidates[0]);
    static_assert(Sora::Compute::ValidateExecutionPlan(*plan, refreshed).has_value());
    constexpr auto replaced = [](auto candidate) {
        ++candidate.snapshot.generation;
        return candidate;
    }(candidates[0]);
    static_assert(Sora::Compute::ValidateExecutionPlan(*plan, replaced).error() ==
                  Sora::Compute::ExecutionPlanValidationError::GenerationChanged);

    auto input = Sora::Compute::Buffer::Create({.id = {21, 22},
                                                .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                                                .layout = Sora::Compute::LayoutKind::Contiguous,
                                                .rank = 1,
                                                .sizeBytes = 1024,
                                                .alignment = 64});
    REQUIRE(input.has_value());
    Sora::Compute::Allocation source = MakeAllocation(state, kSourceLocation);
    Sora::Compute::Allocation target = MakeAllocation(state);
    Sora::Compute::Allocation output = MakeAllocation(state);
    auto version = input->CommitWholeWrite(source);
    REQUIRE(version.has_value());
    const auto whole = Sora::Compute::ByteRegion::Make(0, 1024);
    REQUIRE(whole.has_value());
    auto sourceReplica = input->AcquireReplica(kSourceLocation, *whole);
    REQUIRE(sourceReplica.has_value());

    Sora::Compute::ExecutableBatchStorage storage{
        .queue = MakeQueue(state),
        .commands = {Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Copy,
                                                   .source = sourceReplica->allocation.View(),
                                                   .target = target.View(),
                                                   .sizeBytes = 1024},
                     Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                   .firstBinding = 0,
                                                   .bindingCount = 2,
                                                   .artifact = compiled->View(),
                                                   .workgroupCount = {4, 1, 1}}},
        .bindings = {Sora::Compute::BackendBinding{target.View(), 0, 1024, 0, Sora::Compute::AccessMode::Read},
                     Sora::Compute::BackendBinding{output.View(), 0, 1024, 1, Sora::Compute::AccessMode::Write}},
        .artifacts = {*compiled},
        .allocations = {sourceReplica->allocation, target, output},
    };
    auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
    REQUIRE(batch.has_value());
    exec::static_thread_pool pool{1};
    const auto completed = stdexec::sync_wait(Sora::Compute::SubmitOn(pool.get_scheduler(), std::move(*batch)));
    REQUIRE(completed.has_value());
    REQUIRE(state.lastCommandCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(state.lastBindingCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(input->PublishReplica(*version, *whole, target).has_value());
    REQUIRE(input->AcquireReplica(kLocation, *whole).has_value());
}

TEST_CASE("Queue capabilities reject unsupported commands before backend submission",
          "[Sora][Compute][Backend][Queue][Capability]") {
    MockQueueState state;
    {
        Sora::Compute::Queue queue = MakeQueue(state, Sora::Compute::QueueCapability::Dispatch);
        Sora::Compute::Allocation source = MakeAllocation(state, kLocation);
        Sora::Compute::Allocation target = MakeAllocation(state, kLocation);
        const Sora::Compute::BackendCommand copy{
            .kind = Sora::Compute::BackendCommandKind::Copy,
            .source = source.View(),
            .target = target.View(),
            .sizeBytes = 256,
        };
        const Sora::Compute::BackendSubmitRequest request{.commands = &copy, .commandCount = 1};
        const auto submitted = queue.Submit(request);
        REQUIRE_FALSE(submitted.has_value());
        REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
        REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
    }
    {
        Sora::Compute::Artifact artifact = MakeArtifact(state);
        Sora::Compute::ExecutableBatchStorage storage{
            .queue = MakeQueue(state, Sora::Compute::QueueCapability::Copy),
            .commands = {Sora::Compute::BackendCommand{
                .kind = Sora::Compute::BackendCommandKind::Dispatch,
                .artifact = artifact.View(),
                .workgroupCount = {1, 1, 1},
            }},
            .artifacts = {},
        };
        storage.artifacts.push_back(std::move(artifact));
        const auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
        REQUIRE_FALSE(batch.has_value());
        REQUIRE(batch.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
        REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 0);
    }
}
