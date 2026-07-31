#include "BackendTestSupport.h"
#include <Sora/Compute/Backend/MemoryApi.h>
#include <Sora/Compute/Backend/TransferApi.h>
#include <Sora/Compute/Runtime/ExecutableGraph.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::DeviceId kCpuDevice{1, 1};
    constexpr Sora::Compute::DeviceId kGpuDevice{2, 2};
    constexpr Sora::Compute::MemoryDomainId kHostDomain{10, 10};
    constexpr Sora::Compute::MemoryDomainId kSharedDomain{20, 20};
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = kHostDomain, .device = kCpuDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kSharedCpu{
        .domain = kSharedDomain, .device = kCpuDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kSharedGpu{
        .domain = kSharedDomain, .device = kGpuDevice, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::MemoryLocation kDeviceLocal{
        .domain = {30, 30}, .device = kGpuDevice, .backend = Sora::Compute::BackendKind::Vulkan};

    struct MockState {
        std::atomic<uint32_t> allocateCount = 0;
        std::atomic<uint32_t> transferCount = 0;
        std::atomic<uint32_t> allocationDestroyCount = 0;
        std::atomic<uint32_t> eventDestroyCount = 0;
        std::atomic<uint32_t> observedWaitCount = 0;
        std::atomic<uint32_t> observedRegionCount = 0;
        std::atomic<uint64_t> nextMemory = 1;
        std::array<Sora::Compute::TransferRegion, 4> observedRegions{};
        bool malformedAllocation = false;
        bool malformedAlias = false;
        bool failTransfer = false;
    };

    struct NativeAllocation {
        MockState* owner = nullptr;
    };

    struct NativeEvent {
        MockState* owner = nullptr;
        Sora::Compute::EventStatus status = Sora::Compute::EventStatus::Pending;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        allocation->owner->allocationDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyEvent(void* opaque) noexcept {
        std::unique_ptr<NativeEvent> event{static_cast<NativeEvent*>(opaque)};
        event->owner->eventDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyServiceState(void* opaque) noexcept {
        delete static_cast<uint32_t*>(opaque);
    }

    Sora::Compute::BackendService MakeService(Sora::Compute::BackendServiceId id) {
        auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
        auto service = Sora::Compute::BackendService::Adopt(
            {.id = id, .state = new uint32_t{}, .destroy = DestroyServiceState}, std::move(code));
        REQUIRE(service.has_value());
        return *service;
    }

    void QueryEvent(void* opaque, Sora::Compute::BackendEventObservation* observation) noexcept {
        const auto& event = *static_cast<NativeEvent*>(opaque);
        observation->status = event.status;
        observation->error = {};
    }

    void WaitEvent(void* opaque, uint64_t, Sora::Compute::BackendEventObservation* observation) noexcept {
        auto& event = *static_cast<NativeEvent*>(opaque);
        event.status = Sora::Compute::EventStatus::Completed;
        observation->status = event.status;
        observation->error = {};
    }

    Sora::Compute::BackendEventPayload MakeEventPayload(MockState& state, Sora::Compute::DeviceId device,
                                                        Sora::Compute::BackendKind backend) {
        auto event = std::make_unique<NativeEvent>(&state);
        return {.device = device,
                .backend = backend,
                .state = event.release(),
                .destroy = DestroyEvent,
                .query = QueryEvent,
                .wait = WaitEvent};
    }

    Sora::Compute::Allocation MakeAllocation(MockState& state, Sora::Compute::BackendService service,
                                             Sora::Compute::MemoryLocation location,
                                             Sora::Compute::PhysicalMemoryId memory = {}, uint64_t memoryOffset = 0,
                                             uint64_t size = 1024) {
        if (!memory.Valid()) {
            memory = {state.nextMemory.fetch_add(1, std::memory_order_relaxed), 1};
        }
        auto native = std::make_unique<NativeAllocation>(&state);
        auto allocation =
            Sora::Compute::Allocation::Adopt(service, {
                                                          .location = location,
                                                          .memory = memory,
                                                          .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                          .memoryOffset = memoryOffset,
                                                          .sizeBytes = size,
                                                          .alignment = 64,
                                                          .state = native.release(),
                                                          .destroy = DestroyAllocation,
                                                      });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

    Sora::Compute::Allocation MakeAllocation(MockState& state, Sora::Compute::MemoryLocation location,
                                             Sora::Compute::PhysicalMemoryId memory = {}, uint64_t memoryOffset = 0,
                                             uint64_t size = 1024) {
        return MakeAllocation(state, Sora::Test::TestBackendService(), location, memory, memoryOffset, size);
    }

    Sora::Compute::Event MakeEvent(MockState& state, Sora::Compute::DeviceId device,
                                   Sora::Compute::BackendKind backend) {
        auto event = Sora::Compute::Event::Adopt(Sora::Test::TestBackendService(),
                                                 MakeEventPayload(state, device, backend), backend, device);
        REQUIRE(event.has_value());
        return *event;
    }

    void AllocateMock(void* opaque, const Sora::Compute::BackendAllocationRequest* request,
                      Sora::Compute::BackendAllocationResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        state.allocateCount.fetch_add(1, std::memory_order_relaxed);
        auto native = std::unique_ptr<NativeAllocation>{new (std::nothrow) NativeAllocation{&state}};
        if (!native) {
            result->error = {Sora::Compute::MemoryAllocationErrorCode::OutOfMemory, 0};
            return;
        }
        result->allocation = {
            .location = state.malformedAllocation ? kHost : request->location,
            .memory = {state.nextMemory.fetch_add(1, std::memory_order_relaxed), 2},
            .properties = request->requiredProperties | Sora::Compute::MemoryProperty::HostVisible,
            .sizeBytes = request->sizeBytes,
            .alignment = request->alignment,
            .state = native.release(),
            .destroy = DestroyAllocation,
        };
        result->error = {Sora::Compute::MemoryAllocationErrorCode::None, 0};
    }

    void AliasMock(void* opaque, const Sora::Compute::BackendTransferRequest* request,
                   Sora::Compute::BackendTransferResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        state.transferCount.fetch_add(1, std::memory_order_relaxed);
        auto native = std::unique_ptr<NativeAllocation>{new (std::nothrow) NativeAllocation{&state}};
        if (!native) {
            result->error = {Sora::Compute::TransferErrorCode::OutOfMemory, 0};
            return;
        }
        const Sora::Compute::TransferRegion region = request->Regions().front();
        result->imported = {
            .location = request->destination,
            .memory = state.malformedAlias ? Sora::Compute::PhysicalMemoryId{99, 99} : request->source.memory,
            .properties = request->source.properties,
            .memoryOffset = request->source.memoryOffset + region.sourceOffset,
            .sizeBytes = region.sizeBytes,
            .alignment = request->source.alignment,
            .state = native.release(),
            .destroy = DestroyAllocation,
        };
        result->error = {Sora::Compute::TransferErrorCode::None, 0};
    }

    void TransferMock(void* opaque, const Sora::Compute::BackendTransferRequest* request,
                      Sora::Compute::BackendTransferResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        state.transferCount.fetch_add(1, std::memory_order_relaxed);
        state.observedWaitCount.store(request->waitCount, std::memory_order_relaxed);
        state.observedRegionCount.store(request->regionCount, std::memory_order_relaxed);
        for (size_t index = 0; index < std::min(request->Regions().size(), state.observedRegions.size()); ++index) {
            state.observedRegions[index] = request->Regions()[index];
        }
        if (state.failTransfer) {
            result->error = {Sora::Compute::TransferErrorCode::EngineUnavailable, 41};
            return;
        }
        result->completion = MakeEventPayload(state, kGpuDevice, Sora::Compute::BackendKind::Vulkan);
        result->error = {Sora::Compute::TransferErrorCode::None, 0};
    }

} // namespace

TEST_CASE("MemoryAllocator validates requests and adopts exact physical storage identity",
          "[Sora][Compute][Backend][Memory]") {
    MockState state;
    auto allocator = Sora::Compute::MemoryAllocator::Create(
        Sora::Test::TestBackendService(), {
                                              .location = kDeviceLocal,
                                              .supportedProperties = Sora::Compute::MemoryProperty::HostVisible |
                                                                     Sora::Compute::MemoryProperty::DeviceLocal,
                                              .state = &state,
                                              .allocate = AllocateMock,
                                          });
    REQUIRE(allocator.has_value());

    auto allocation = allocator->Allocate({
        .location = kDeviceLocal,
        .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
        .preferredProperties = Sora::Compute::MemoryProperty::HostVisible,
        .sizeBytes = 4096,
        .alignment = 256,
    });
    REQUIRE(allocation.has_value());
    REQUIRE(allocation->Location() == kDeviceLocal);
    REQUIRE(allocation->SizeBytes() == 4096);
    REQUIRE(allocation->Alignment() == 256);
    REQUIRE(allocation->View().memory.Valid());
    REQUIRE(allocation->View().PhysicalRange(128, 256).has_value());
    REQUIRE(state.allocateCount.load(std::memory_order_relaxed) == 1);

    allocation = std::unexpected(Sora::Compute::MemoryAllocationError{});
    REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("MemoryAllocator rejects mismatched backend payloads and destroys them transactionally",
          "[Sora][Compute][Backend][Memory][Validation]") {
    MockState state;
    state.malformedAllocation = true;
    auto allocator = Sora::Compute::MemoryAllocator::Create(
        Sora::Test::TestBackendService(), {
                                              .location = kDeviceLocal,
                                              .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                              .state = &state,
                                              .allocate = AllocateMock,
                                          });
    REQUIRE(allocator.has_value());

    auto allocation = allocator->Allocate({.location = kDeviceLocal,
                                           .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                           .sizeBytes = 1024,
                                           .alignment = 64});
    REQUIRE_FALSE(allocation.has_value());
    REQUIRE(allocation.error().code == Sora::Compute::MemoryAllocationErrorCode::InvalidAllocation);
    REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Coherent alias import preserves physical identity and resolves the imported subrange",
          "[Sora][Compute][Backend][Transfer][Alias]") {
    MockState state;
    auto engine = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                        {
                                                            .engine = {1, 1},
                                                            .sourceService = Sora::Test::TestBackendService().Id(),
                                                            .source = kSharedCpu,
                                                            .destination = kSharedGpu,
                                                            .kind = Sora::Compute::TransferKind::CoherentAlias,
                                                            .state = &state,
                                                            .transfer = AliasMock,
                                                        });
    REQUIRE(engine.has_value());

    auto source = MakeAllocation(state, kSharedCpu, {7, 7}, 128, 1024);
    auto imported = engine->ImportAlias(source, 256, 512);
    REQUIRE(imported.has_value());
    REQUIRE(imported->Location() == kSharedGpu);
    REQUIRE(imported->View().memory == source.View().memory);
    REQUIRE(imported->View().memoryOffset == source.View().memoryOffset + 256);
    REQUIRE(imported->SizeBytes() == 512);
    REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Coherent alias import destroys a result that changes physical identity",
          "[Sora][Compute][Backend][Transfer][Alias][Validation]") {
    MockState state;
    state.malformedAlias = true;
    auto engine = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                        {
                                                            .engine = {2, 2},
                                                            .sourceService = Sora::Test::TestBackendService().Id(),
                                                            .source = kSharedCpu,
                                                            .destination = kSharedGpu,
                                                            .kind = Sora::Compute::TransferKind::CoherentAlias,
                                                            .state = &state,
                                                            .transfer = AliasMock,
                                                        });
    REQUIRE(engine.has_value());
    auto source = MakeAllocation(state, kSharedCpu);
    const auto imported = engine->ImportAlias(source, 0, 512);
    REQUIRE_FALSE(imported.has_value());
    REQUIRE(imported.error().code == Sora::Compute::TransferErrorCode::InvalidResult);
    REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Asynchronous transfer owns allocations and wait events until completion state retires",
          "[Sora][Compute][Backend][Transfer][Lifecycle]") {
    MockState state;
    auto engine = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                        {
                                                            .engine = {3, 3},
                                                            .sourceService = Sora::Test::TestBackendService().Id(),
                                                            .source = kHost,
                                                            .destination = kDeviceLocal,
                                                            .kind = Sora::Compute::TransferKind::HostBridge,
                                                            .completionDevice = kGpuDevice,
                                                            .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                            .state = &state,
                                                            .transfer = TransferMock,
                                                        });
    REQUIRE(engine.has_value());

    auto source = MakeAllocation(state, kHost);
    auto destination = MakeAllocation(state, kDeviceLocal);
    std::vector<Sora::Compute::Event> waits;
    waits.push_back(MakeEvent(state, kCpuDevice, Sora::Compute::BackendKind::Cpu));
    const std::array regions{Sora::Compute::TransferRegion{.sourceOffset = 128, .targetOffset = 256, .sizeBytes = 256},
                             Sora::Compute::TransferRegion{.sourceOffset = 512, .targetOffset = 768, .sizeBytes = 128}};
    {
        auto submission = engine->Submit(source, destination, regions, std::move(waits));
        REQUIRE(submission.has_value());
        source = {};
        destination = {};
        REQUIRE(state.observedWaitCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(state.observedRegionCount.load(std::memory_order_relaxed) == regions.size());
        REQUIRE(state.observedRegions[0] == regions[0]);
        REQUIRE(state.observedRegions[1] == regions[1]);
        REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 0);
        REQUIRE(submission->Query().status == Sora::Compute::EventStatus::Pending);
        REQUIRE(submission->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    }
    REQUIRE(state.allocationDestroyCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Asynchronous transfer accepts a source allocation owned by another backend service",
          "[Sora][Compute][Backend][Transfer][Service]") {
    MockState state;
    auto sourceService = MakeService({0x534F555243455F31ULL, 0x534552564943455FULL});
    const auto& destinationService = Sora::Test::TestBackendService();
    auto engine = Sora::Compute::TransferEngine::Create(destinationService,
                                                        {.engine = {31, 31},
                                                         .sourceService = sourceService.Id(),
                                                         .source = kHost,
                                                         .destination = kDeviceLocal,
                                                         .kind = Sora::Compute::TransferKind::Remote,
                                                         .completionDevice = kGpuDevice,
                                                         .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                         .state = &state,
                                                         .transfer = TransferMock});
    REQUIRE(engine.has_value());
    REQUIRE(engine->SourceService() == sourceService.Id());
    REQUIRE(engine->Service() == destinationService.Id());

    auto source = MakeAllocation(state, sourceService, kHost);
    auto destination = MakeAllocation(state, destinationService, kDeviceLocal);
    const std::array regions{Sora::Compute::TransferRegion{.sizeBytes = 1024}};
    auto submission = engine->Submit(source, destination, regions);
    REQUIRE(submission.has_value());
    REQUIRE(submission->Destination().View().service == destinationService.Id());
    REQUIRE(submission->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
}

TEST_CASE("Scatter gather transfer rejects empty, unordered, and overlapping target regions before callback",
          "[Sora][Compute][Backend][Transfer][ScatterGather][Validation]") {
    MockState state;
    auto engine = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                        {.engine = {32, 32},
                                                         .sourceService = Sora::Test::TestBackendService().Id(),
                                                         .source = kHost,
                                                         .destination = kDeviceLocal,
                                                         .kind = Sora::Compute::TransferKind::Remote,
                                                         .completionDevice = kGpuDevice,
                                                         .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                         .state = &state,
                                                         .transfer = TransferMock});
    REQUIRE(engine.has_value());
    auto source = MakeAllocation(state, kHost);
    auto destination = MakeAllocation(state, kDeviceLocal);

    const std::array<Sora::Compute::TransferRegion, 0> empty{};
    const std::array unordered{
        Sora::Compute::TransferRegion{.targetOffset = 512, .sizeBytes = 128},
        Sora::Compute::TransferRegion{.sourceOffset = 128, .targetOffset = 128, .sizeBytes = 128}};
    const std::array overlapping{
        Sora::Compute::TransferRegion{.targetOffset = 128, .sizeBytes = 256},
        Sora::Compute::TransferRegion{.sourceOffset = 256, .targetOffset = 256, .sizeBytes = 128}};

    for (const auto regions : {std::span<const Sora::Compute::TransferRegion>{empty},
                               std::span<const Sora::Compute::TransferRegion>{unordered},
                               std::span<const Sora::Compute::TransferRegion>{overlapping}}) {
        const auto submission = engine->Submit(source, destination, regions);
        REQUIRE_FALSE(submission.has_value());
        REQUIRE(submission.error().code == Sora::Compute::TransferErrorCode::InvalidRequest);
    }
    REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Scatter gather transfer rejects source-to-target alias cycles before callback",
          "[Sora][Compute][Backend][Transfer][ScatterGather][Validation]") {
    MockState state;
    auto engine = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                        {.engine = {33, 33},
                                                         .sourceService = Sora::Test::TestBackendService().Id(),
                                                         .source = kHost,
                                                         .destination = kDeviceLocal,
                                                         .kind = Sora::Compute::TransferKind::Remote,
                                                         .completionDevice = kGpuDevice,
                                                         .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                         .state = &state,
                                                         .transfer = TransferMock});
    REQUIRE(engine.has_value());
    constexpr Sora::Compute::PhysicalMemoryId memory{77, 77};
    auto source = MakeAllocation(state, kHost, memory, 0, 1024);
    auto destination = MakeAllocation(state, kDeviceLocal, memory, 0, 1024);
    const std::array selfOverlap{
        Sora::Compute::TransferRegion{.sourceOffset = 0, .targetOffset = 128, .sizeBytes = 256}};
    const std::array aliasCycle{
        Sora::Compute::TransferRegion{.sourceOffset = 512, .targetOffset = 0, .sizeBytes = 128},
        Sora::Compute::TransferRegion{.sourceOffset = 0, .targetOffset = 512, .sizeBytes = 128}};

    for (const auto regions : {std::span<const Sora::Compute::TransferRegion>{selfOverlap},
                               std::span<const Sora::Compute::TransferRegion>{aliasCycle}}) {
        const auto submission = engine->Submit(source, destination, regions);
        REQUIRE_FALSE(submission.has_value());
        REQUIRE(submission.error().code == Sora::Compute::TransferErrorCode::InvalidRequest);
    }
    REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Asynchronous transfer reports backend rejection without fabricating an event",
          "[Sora][Compute][Backend][Transfer][Failure]") {
    MockState state;
    state.failTransfer = true;
    auto engine = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                        {
                                                            .engine = {4, 4},
                                                            .sourceService = Sora::Test::TestBackendService().Id(),
                                                            .source = kHost,
                                                            .destination = kDeviceLocal,
                                                            .kind = Sora::Compute::TransferKind::HostBridge,
                                                            .completionDevice = kGpuDevice,
                                                            .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                            .state = &state,
                                                            .transfer = TransferMock,
                                                        });
    REQUIRE(engine.has_value());
    const std::array regions{Sora::Compute::TransferRegion{.sizeBytes = 1024}};
    const auto submission = engine->Submit(MakeAllocation(state, kHost), MakeAllocation(state, kDeviceLocal), regions);
    REQUIRE_FALSE(submission.has_value());
    REQUIRE(submission.error() ==
            Sora::Compute::TransferError{Sora::Compute::TransferErrorCode::EngineUnavailable, 41});
    REQUIRE(state.eventDestroyCount.load(std::memory_order_relaxed) == 0);
}

namespace {

    Sora::Compute::TransferEngine
    MakeTransferEngine(MockState& state, Sora::Compute::DeviceId completionDevice = kGpuDevice,
                       Sora::Compute::BackendKind completionBackend = Sora::Compute::BackendKind::Vulkan) {
        auto engine = Sora::Compute::TransferEngine::Create(
            Sora::Test::TestBackendService(), {
                                                  .engine = {40 + completionDevice.low, 40 + completionDevice.high},
                                                  .sourceService = Sora::Test::TestBackendService().Id(),
                                                  .source = kHost,
                                                  .destination = kDeviceLocal,
                                                  .kind = Sora::Compute::TransferKind::HostBridge,
                                                  .completionDevice = completionDevice,
                                                  .completionBackend = completionBackend,
                                                  .state = &state,
                                                  .transfer = TransferMock,
                                              });
        REQUIRE(engine.has_value());
        return *engine;
    }

    Sora::Compute::ExecutableTransfer MakeExecutableTransfer(MockState& state, Sora::Compute::TransferEngine engine,
                                                             std::vector<Sora::Compute::Event> waits = {}) {
        auto transfer = Sora::Compute::ExecutableTransfer::Create({
            .engine = engine,
            .source = MakeAllocation(state, kHost),
            .destination = MakeAllocation(state, kDeviceLocal),
            .regions = {{.sourceOffset = 64, .targetOffset = 128, .sizeBytes = 256},
                        {.sourceOffset = 512, .targetOffset = 512, .sizeBytes = 256}},
            .waits = std::move(waits),
        });
        REQUIRE(transfer.has_value());
        return std::move(*transfer);
    }

} // namespace

TEST_CASE("ExecutableTransfer remains inert until its physical graph is submitted",
          "[Sora][Compute][Runtime][PhysicalSegment][Transfer]") {
    MockState state;
    std::vector<Sora::Compute::Event> waits;
    waits.push_back(MakeEvent(state, kCpuDevice, Sora::Compute::BackendKind::Cpu));
    auto transfer = MakeExecutableTransfer(state, MakeTransferEngine(state), std::move(waits));
    REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 0);

    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.emplace_back(std::move(transfer));
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());
    REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 0);

    auto submission = std::move(*graph).Submit();
    REQUIRE(submission.has_value());
    REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.observedWaitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(submission->Query().status == Sora::Compute::EventStatus::Pending);
    REQUIRE(submission->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
}

TEST_CASE("ExecutableGraph injects device waits between asynchronous transfer segments",
          "[Sora][Compute][Runtime][PhysicalSegment][Transfer][DeviceWait]") {
    MockState state;
    auto engine = MakeTransferEngine(state);
    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.emplace_back(MakeExecutableTransfer(state, engine));
    storage.segments.emplace_back(MakeExecutableTransfer(state, engine));
    storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::DeviceWait});
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());

    auto submission = std::move(*graph).Submit();
    REQUIRE(submission.has_value());
    REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(state.observedWaitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(submission->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
}

TEST_CASE("ExecutableGraph rejects cross-domain transfer device waits and maps transfer rejection",
          "[Sora][Compute][Runtime][PhysicalSegment][Transfer][Validation]") {
    MockState state;
    {
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.emplace_back(MakeExecutableTransfer(state, MakeTransferEngine(state)));
        storage.segments.emplace_back(
            MakeExecutableTransfer(state, MakeTransferEngine(state, kCpuDevice, Sora::Compute::BackendKind::Cpu)));
        storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::DeviceWait});
        const auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::ExecutableGraphErrorCode::IncompatibleDeviceWait);
        REQUIRE(state.transferCount.load(std::memory_order_relaxed) == 0);
    }
    {
        state.failTransfer = true;
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.emplace_back(MakeExecutableTransfer(state, MakeTransferEngine(state)));
        auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE(graph.has_value());
        const auto submission = std::move(*graph).Submit();
        REQUIRE_FALSE(submission.has_value());
        REQUIRE(submission.error().error.code == Sora::Compute::SubmissionErrorCode::QueueUnavailable);
    }
}
