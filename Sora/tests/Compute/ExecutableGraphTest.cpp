#include "BackendTestSupport.h"
#include <Sora/Compute/Runtime/ExecutableGraph.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::DeviceId kDeviceA{1, 1};
    constexpr Sora::Compute::DeviceId kDeviceB{2, 2};

    struct MockState {
        std::atomic<uint32_t> submitCount = 0;
        std::atomic<uint32_t> eventWaitCount = 0;
        std::atomic<bool> failNextEventWait = false;
        std::atomic<uint32_t> eventDestroyCount = 0;
        std::atomic<uint32_t> queueDestroyCount = 0;
        std::atomic<uint32_t> artifactDestroyCount = 0;
        std::array<std::atomic<uint32_t>, 4> observedWaitCounts{};
    };

    struct NativeQueue {
        MockState* owner = nullptr;
        Sora::Compute::DeviceId device{};
        uint32_t index = 0;
        bool fail = false;
    };

    struct NativeEvent {
        MockState* owner = nullptr;
        Sora::Compute::DeviceId device{};
        Sora::Compute::EventStatus status = Sora::Compute::EventStatus::Pending;
    };

    struct NativeArtifact {
        MockState* owner = nullptr;
    };

    void DestroyQueue(void* opaque) noexcept {
        std::unique_ptr<NativeQueue> queue{static_cast<NativeQueue*>(opaque)};
        queue->owner->queueDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyEvent(void* opaque) noexcept {
        std::unique_ptr<NativeEvent> event{static_cast<NativeEvent*>(opaque)};
        event->owner->eventDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyArtifact(void* opaque) noexcept {
        std::unique_ptr<NativeArtifact> artifact{static_cast<NativeArtifact*>(opaque)};
        artifact->owner->artifactDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void QueryEvent(void* opaque, Sora::Compute::BackendEventObservation* observation) noexcept {
        const auto& event = *static_cast<NativeEvent*>(opaque);
        observation->status = event.status;
        observation->error =
            event.status == Sora::Compute::EventStatus::Failed
                ? Sora::Compute::SubmissionError{Sora::Compute::SubmissionErrorCode::SubmissionFailed, 0}
                : Sora::Compute::SubmissionError{};
    }

    void WaitEvent(void* opaque, uint64_t, Sora::Compute::BackendEventObservation* observation) noexcept {
        auto& event = *static_cast<NativeEvent*>(opaque);
        event.owner->eventWaitCount.fetch_add(1, std::memory_order_relaxed);
        event.status = event.owner->failNextEventWait.exchange(false, std::memory_order_relaxed)
                           ? Sora::Compute::EventStatus::Failed
                           : Sora::Compute::EventStatus::Completed;
        observation->status = event.status;
        observation->error =
            event.status == Sora::Compute::EventStatus::Failed
                ? Sora::Compute::SubmissionError{Sora::Compute::SubmissionErrorCode::SubmissionFailed, 0}
                : Sora::Compute::SubmissionError{};
    }

    void Submit(void* opaque, const Sora::Compute::BackendSubmitRequest* request,
                Sora::Compute::BackendSubmitResult* result) noexcept {
        auto& queue = *static_cast<NativeQueue*>(opaque);
        queue.owner->submitCount.fetch_add(1, std::memory_order_relaxed);
        queue.owner->observedWaitCounts[queue.index].store(request->waitCount, std::memory_order_relaxed);
        if (queue.fail) {
            result->error = {Sora::Compute::SubmissionErrorCode::QueueUnavailable, queue.index};
            return;
        }
        auto event = std::unique_ptr<NativeEvent>{new (std::nothrow) NativeEvent{queue.owner, queue.device}};
        if (!event) {
            result->error = {Sora::Compute::SubmissionErrorCode::OutOfMemory, 0};
            return;
        }
        result->event = {.device = queue.device,
                         .backend = Sora::Compute::BackendKind::Cpu,
                         .state = event.release(),
                         .destroy = DestroyEvent,
                         .query = QueryEvent,
                         .wait = WaitEvent};
        result->error = {};
    }

    Sora::Compute::Queue MakeQueue(MockState& state, Sora::Compute::DeviceId device, uint32_t index,
                                   bool fail = false) {
        auto native = std::make_unique<NativeQueue>(&state, device, index, fail);
        auto queue = Sora::Compute::Queue::Adopt(Sora::Test::TestBackendService(),
                                                 {
                                                     .id = {index + 1, 1},
                                                     .device = device,
                                                     .backend = Sora::Compute::BackendKind::Cpu,
                                                     .capabilities = Sora::Compute::QueueCapability::Dispatch,
                                                     .state = native.release(),
                                                     .destroy = DestroyQueue,
                                                     .submit = Submit,
                                                 });
        REQUIRE(queue.has_value());
        return *queue;
    }

    Sora::Compute::Artifact MakeArtifact(MockState& state, Sora::Compute::DeviceId device, uint64_t identity) {
        auto native = std::make_unique<NativeArtifact>(&state);
        auto artifact = Sora::Compute::Artifact::Adopt(
            Sora::Test::TestBackendService(),
            {
                .key = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
                    .lowered = {identity, identity},
                    .target = Sora::Compute::ExecutableTarget({
                        .device = device,
                        .backend = Sora::Compute::BackendKind::Cpu,
                        .hostArchitecture = Sora::CpuArchitecture::X64,
                        .operatingSystem = Sora::OperatingSystem::Windows,
                        .binaryFormat = Sora::BinaryFormat::PE,
                        .byteOrder = Sora::ByteOrder::Little,
                        .abi = Sora::AbiFamily::MSVC,
                        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                        .addressBits = 64,
                        .architectureId = identity,
                        .compilerIdentity = {identity, 0x434F4D50494C4552},
                        .driverIdentity = {identity, 0x445249564552},
                    }),
                    .specialization = {},
                }),
                .device = device,
                .state = native.release(),
                .destroy = DestroyArtifact,
            });
        REQUIRE(artifact.has_value());
        return *artifact;
    }

    Sora::Compute::ExecutableBatch MakeBatch(MockState& state, Sora::Compute::DeviceId device, uint32_t index,
                                             bool fail = false) {
        Sora::Compute::Artifact artifact = MakeArtifact(state, device, index + 1);
        Sora::Compute::ExecutableBatchStorage storage{
            .queue = MakeQueue(state, device, index, fail),
            .commands = {Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                       .artifact = artifact.View(),
                                                       .workgroupCount = {1, 1, 1}}},
            .artifacts = {},
        };
        storage.artifacts.push_back(std::move(artifact));
        auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
        REQUIRE(batch.has_value());
        return std::move(*batch);
    }

} // namespace

TEST_CASE("ExecutableGraph injects same-device predecessor events without host blocking",
          "[Sora][Compute][Runtime][ExecutableGraph][DeviceWait]") {
    MockState state;
    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
    storage.segments.push_back(MakeBatch(state, kDeviceA, 1));
    storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::DeviceWait});
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());

    auto submission = std::move(*graph).Submit();
    REQUIRE(submission.has_value());
    REQUIRE(submission->SegmentCount() == 2);
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(state.observedWaitCounts[0].load(std::memory_order_relaxed) == 0);
    REQUIRE(state.observedWaitCounts[1].load(std::memory_order_relaxed) == 1);
    REQUIRE(state.eventWaitCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(submission->Query().status == Sora::Compute::EventStatus::Pending);
    REQUIRE(submission->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
}

TEST_CASE("ExecutableGraph resolves cross-device host dependencies before successor submission",
          "[Sora][Compute][Runtime][ExecutableGraph][HostCompletion]") {
    MockState state;
    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
    storage.segments.push_back(MakeBatch(state, kDeviceB, 1));
    storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::HostCompletion});
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());

    auto submission = std::move(*graph).Submit();
    REQUIRE(submission.has_value());
    REQUIRE(state.eventWaitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.observedWaitCounts[1].load(std::memory_order_relaxed) == 0);
    REQUIRE(submission->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
}

TEST_CASE("ExecutableGraph rejects incompatible device waits and contradictory edge modes",
          "[Sora][Compute][Runtime][ExecutableGraph][Validation]") {
    MockState state;
    {
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
        storage.segments.push_back(MakeBatch(state, kDeviceB, 1));
        storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::DeviceWait});
        const auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::ExecutableGraphErrorCode::IncompatibleDeviceWait);
    }
    {
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.push_back(MakeBatch(state, kDeviceA, 2));
        storage.segments.push_back(MakeBatch(state, kDeviceA, 3));
        storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::DeviceWait});
        storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::HostCompletion});
        const auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::ExecutableGraphErrorCode::InvalidEdge);
    }
}

TEST_CASE("ExecutableGraph drains accepted predecessors when a later queue rejects submission",
          "[Sora][Compute][Runtime][ExecutableGraph][Failure]") {
    MockState state;
    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
    storage.segments.push_back(MakeBatch(state, kDeviceA, 1, true));
    storage.edges.push_back({{0}, {1}, Sora::Compute::ExecutableDependencyKind::DeviceWait});
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());

    const auto submission = std::move(*graph).Submit();
    REQUIRE_FALSE(submission.has_value());
    REQUIRE(submission.error().node == 1);
    REQUIRE(submission.error().error ==
            Sora::Compute::SubmissionError{Sora::Compute::SubmissionErrorCode::QueueUnavailable, 1});
    REQUIRE(state.submitCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(state.eventWaitCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("ExecutableGraph terminal wait drains every accepted segment before reporting failure",
          "[Sora][Compute][Runtime][ExecutableGraph][Failure][Terminal]") {
    MockState state;
    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
    storage.segments.push_back(MakeBatch(state, kDeviceA, 1));
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());

    auto submission = std::move(*graph).Submit();
    REQUIRE(submission.has_value());
    REQUIRE(state.eventWaitCount.load(std::memory_order_relaxed) == 0);

    state.failNextEventWait.store(true, std::memory_order_relaxed);
    const auto observation = submission->WaitUntilTerminal();
    REQUIRE(observation.status == Sora::Compute::EventStatus::Failed);
    REQUIRE(observation.error.code == Sora::Compute::SubmissionErrorCode::SubmissionFailed);
    REQUIRE(state.eventWaitCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(submission->Query() == observation);
}

namespace {

    constexpr Sora::Compute::MemoryLocation kHostMemory{
        .domain = {10, 1}, .device = kDeviceA, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kDeviceMemory{
        .domain = {10, 2}, .device = kDeviceA, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr auto kWholeBuffer = Sora::Compute::ByteRegion::Make(0, 256);
    static_assert(kWholeBuffer.has_value());

    struct NativeAllocation {
        std::atomic<uint32_t>* destroyCount = nullptr;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        allocation->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    Sora::Compute::Buffer MakeBuffer(Sora::Compute::BufferId id) {
        auto buffer = Sora::Compute::Buffer::Create({
            .id = id,
            .elementType = {Sora::Compute::ScalarKind::Float32, 1},
            .layout = Sora::Compute::LayoutKind::Contiguous,
            .rank = 1,
            .sizeBytes = 256,
            .alignment = 64,
        });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Sora::Compute::Allocation MakeAllocation(std::atomic<uint32_t>& destroyCount,
                                             Sora::Compute::MemoryLocation location,
                                             Sora::Compute::PhysicalMemoryId memory) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Sora::Compute::Allocation::Adopt(Sora::Test::TestBackendService(),
                                                           {
                                                               .location = location,
                                                               .memory = memory,
                                                               .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                               .sizeBytes = 256,
                                                               .alignment = 64,
                                                               .state = native.release(),
                                                               .destroy = DestroyAllocation,
                                                           });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

} // namespace

TEST_CASE("ExecutableGraph publishes completion transaction only after all segments complete",
          "[Sora][Compute][Runtime][ExecutableGraph][Publication]") {
    MockState state;
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({10, 1});
    Sora::Compute::CompletionTransaction completion;
    REQUIRE(completion.AddWholeWrite(buffer, 0, MakeAllocation(destroyCount, kDeviceMemory, {10, 1})).has_value());

    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
    storage.completion.emplace(std::move(completion));
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());
    auto submission = std::move(*graph).Submit();
    REQUIRE(submission.has_value());

    REQUIRE(buffer.CurrentVersion() == 0);
    REQUIRE_FALSE(buffer.AcquireReplica(kDeviceMemory, *kWholeBuffer).has_value());
    REQUIRE(submission->Query().status == Sora::Compute::EventStatus::Pending);
    REQUIRE(submission->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(buffer.CurrentVersion() == 1);
    REQUIRE(buffer.AcquireReplica(kDeviceMemory, *kWholeBuffer).has_value());
}

TEST_CASE("GraphSubmission destructor drains accepted work and publishes successful results",
          "[Sora][Compute][Runtime][ExecutableGraph][Publication][Lifetime]") {
    MockState state;
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({10, 2});
    Sora::Compute::CompletionTransaction completion;
    REQUIRE(completion.AddWholeWrite(buffer, 0, MakeAllocation(destroyCount, kDeviceMemory, {10, 2})).has_value());

    {
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
        storage.completion.emplace(std::move(completion));
        auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE(graph.has_value());
        auto submission = std::move(*graph).Submit();
        REQUIRE(submission.has_value());
        REQUIRE(buffer.CurrentVersion() == 0);
    }

    REQUIRE(state.eventWaitCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(buffer.CurrentVersion() == 1);
    REQUIRE(buffer.AcquireReplica(kDeviceMemory, *kWholeBuffer).has_value());
}

TEST_CASE("ExecutableGraph submission failure discards its uncommitted completion transaction",
          "[Sora][Compute][Runtime][ExecutableGraph][Publication][Failure]") {
    MockState state;
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({10, 3});
    {
        Sora::Compute::CompletionTransaction completion;
        REQUIRE(completion.AddWholeWrite(buffer, 0, MakeAllocation(destroyCount, kDeviceMemory, {10, 3})).has_value());
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
        storage.segments.push_back(MakeBatch(state, kDeviceA, 1, true));
        storage.completion.emplace(std::move(completion));
        auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE(graph.has_value());
        const auto submission = std::move(*graph).Submit();
        REQUIRE_FALSE(submission.has_value());
        REQUIRE(buffer.CurrentVersion() == 0);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE_FALSE(buffer.AcquireReplica(kDeviceMemory, *kWholeBuffer).has_value());
}

TEST_CASE("ExecutableGraph rejects empty completion transactions and reports stale publication",
          "[Sora][Compute][Runtime][ExecutableGraph][Publication][Validation]") {
    MockState state;
    {
        Sora::Compute::ExecutableGraphStorage storage;
        storage.segments.push_back(MakeBatch(state, kDeviceA, 0));
        storage.completion.emplace();
        const auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::ExecutableGraphErrorCode::InvalidCompletionTransaction);
    }

    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({10, 4});
    Sora::Compute::CompletionTransaction completion;
    REQUIRE(completion.AddWholeWrite(buffer, 0, MakeAllocation(destroyCount, kDeviceMemory, {10, 4})).has_value());
    Sora::Compute::ExecutableGraphStorage storage;
    storage.segments.push_back(MakeBatch(state, kDeviceA, 1));
    storage.completion.emplace(std::move(completion));
    auto graph = Sora::Compute::ExecutableGraph::Create(std::move(storage));
    REQUIRE(graph.has_value());
    REQUIRE(buffer.CommitWholeWrite(MakeAllocation(destroyCount, kHostMemory, {10, 5})).has_value());
    auto submission = std::move(*graph).Submit();
    REQUIRE(submission.has_value());

    const auto observation = submission->Wait(UINT64_MAX);
    REQUIRE(observation.status == Sora::Compute::EventStatus::Failed);
    REQUIRE(observation.error.code == Sora::Compute::SubmissionErrorCode::PublicationFailed);
    REQUIRE((observation.error.detail >> 32) == std::to_underlying(Sora::Compute::PublicationErrorCode::StaleVersion));
    REQUIRE(static_cast<uint32_t>(observation.error.detail) == 0);
    REQUIRE(submission->Query() == observation);
    REQUIRE(buffer.CurrentVersion() == 1);
    REQUIRE_FALSE(buffer.AcquireReplica(kDeviceMemory, *kWholeBuffer).has_value());
}
