#include "BackendTestSupport.h"
#include <Sora/Compute/Runtime/RuntimeInvalidationRegistry.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

    using namespace Sora::Compute;

    struct ResourceState {
        std::atomic<uint32_t>* destroyCount = nullptr;
        RuntimeInvalidationRegistry* registry = nullptr;
        BackendServiceId service{};
        std::atomic<bool>* observedRetirement = nullptr;
    };

    void DestroyResource(void* opaque) noexcept {
        std::unique_ptr<ResourceState> state{static_cast<ResourceState*>(opaque)};
        if (state->registry != nullptr) {
            state->observedRetirement->store(state->registry->IsServiceRetired(state->service),
                                             std::memory_order_relaxed);
        }
        state->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyService(void* opaque) noexcept {
        delete static_cast<uint32_t*>(opaque);
    }

    BackendService MakeService(BackendServiceId id) {
        auto code = BackendCodeLease::Retain(std::make_shared<uint32_t>());
        auto service =
            BackendService::Adopt({.id = id, .state = new uint32_t{}, .destroy = DestroyService}, std::move(code));
        REQUIRE(service.has_value());
        return *service;
    }

    Buffer MakeBuffer(BufferId id) {
        auto buffer = Buffer::Create({.id = id,
                                      .elementType = {ScalarKind::Float32, 1},
                                      .layout = LayoutKind::Contiguous,
                                      .rank = 1,
                                      .sizeBytes = 256,
                                      .alignment = 64});
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Allocation MakeAllocation(const BackendService& service, MemoryLocation location,
                              std::atomic<uint32_t>& destroyCount, RuntimeInvalidationRegistry* registry = nullptr,
                              std::atomic<bool>* observedRetirement = nullptr) {
        static std::atomic<uint64_t> nextMemory = 1;
        auto state = std::make_unique<ResourceState>(&destroyCount, registry, service.Id(), observedRetirement);
        auto allocation = Allocation::Adopt(service, {
                                                         .location = location,
                                                         .memory = {nextMemory.fetch_add(1), 1},
                                                         .properties = MemoryProperty::DeviceLocal,
                                                         .sizeBytes = 256,
                                                         .alignment = 64,
                                                         .state = state.release(),
                                                         .destroy = DestroyResource,
                                                     });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

    Artifact MakeArtifact(const BackendService& service, uint64_t identity, std::atomic<uint32_t>& destroyCount,
                          RuntimeInvalidationRegistry* registry = nullptr,
                          std::atomic<bool>* observedRetirement = nullptr) {
        auto state = std::make_unique<ResourceState>(&destroyCount, registry, service.Id(), observedRetirement);
        auto artifact = Artifact::Adopt(service, {
                                                     .key = MakeArtifactKey(LoweringKey{
                                                         .module = {identity, 1},
                                                         .lowerer = {2, 3},
                                                     }),
                                                     .state = state.release(),
                                                     .destroy = DestroyResource,
                                                 });
        REQUIRE(artifact.has_value());
        return *artifact;
    }

    constexpr MemoryLocation kFirstLocation{
        .domain = {1, 1},
        .device = {1, 2},
        .backend = BackendKind::Cpu,
    };
    constexpr MemoryLocation kSecondLocation{
        .domain = {2, 1},
        .device = {2, 2},
        .backend = BackendKind::Vulkan,
    };
    constexpr auto kWholeBuffer = ByteRegion::Make(0, 256);
    static_assert(kWholeBuffer.has_value());

} // namespace

TEST_CASE("WeakBuffer retains identity without retaining logical-buffer lifetime",
          "[Sora][Compute][Runtime][Invalidation]") {
    WeakBuffer weak;
    {
        Buffer buffer = MakeBuffer({1, 1});
        weak = buffer.Weak();
        REQUIRE(weak.Identity() == BufferId{1, 1});
        REQUIRE_FALSE(weak.Expired());
        REQUIRE(weak.Lock());
        REQUIRE(weak.SharesOwnership(buffer.Weak()));
    }
    REQUIRE(weak.Expired());
    REQUIRE_FALSE(weak.Lock());
}

TEST_CASE("Runtime invalidation registration is idempotent and rejects "
          "identity aliasing",
          "[Sora][Compute][Runtime][Invalidation][Identity]") {
    ArtifactCache cache{4};
    RuntimeInvalidationRegistry registry;
    Buffer first = MakeBuffer({2, 1});
    const Buffer alias = first;
    Buffer conflict = MakeBuffer({2, 1});

    const auto firstCacheRegistration = registry.Register(cache);
    REQUIRE(firstCacheRegistration.has_value());
    REQUIRE(*firstCacheRegistration);
    const auto secondCacheRegistration = registry.Register(cache);
    REQUIRE(secondCacheRegistration.has_value());
    REQUIRE_FALSE(*secondCacheRegistration);
    const auto firstBufferRegistration = registry.Register(first);
    REQUIRE(firstBufferRegistration.has_value());
    REQUIRE(*firstBufferRegistration);
    const auto aliasRegistration = registry.Register(alias);
    REQUIRE(aliasRegistration.has_value());
    REQUIRE_FALSE(*aliasRegistration);
    const auto rejected = registry.Register(conflict);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == RuntimeInvalidationErrorCode::ConflictingBufferIdentity);
    REQUIRE(registry.CacheCount() == 1);
    REQUIRE(registry.BufferCount() == 1);
}

TEST_CASE("Runtime invalidation retires only resources owned by the failed service",
          "[Sora][Compute][Runtime][Invalidation][DeviceGeneration]") {
    const auto& firstService = Sora::Test::TestBackendService();
    BackendService secondService = MakeService({0x5345434F4E445F52ULL, 1});
    std::atomic<uint32_t> artifactDestroyCount = 0;
    std::atomic<uint32_t> allocationDestroyCount = 0;
    ArtifactCache cache{4};
    RuntimeInvalidationRegistry registry;
    Buffer buffer = MakeBuffer({3, 1});

    Artifact firstArtifact = MakeArtifact(firstService, 11, artifactDestroyCount);
    Artifact secondArtifact = MakeArtifact(secondService, 12, artifactDestroyCount);
    const ArtifactKey firstKey = firstArtifact.Key();
    const ArtifactKey secondKey = secondArtifact.Key();
    REQUIRE(cache.Insert(firstArtifact).has_value());
    REQUIRE(cache.Insert(secondArtifact).has_value());
    firstArtifact = {};
    secondArtifact = {};

    Allocation firstAllocation = MakeAllocation(firstService, kFirstLocation, allocationDestroyCount);
    auto version = buffer.CommitWholeWrite(firstAllocation);
    REQUIRE(version.has_value());
    firstAllocation = {};
    Allocation secondAllocation = MakeAllocation(secondService, kSecondLocation, allocationDestroyCount);
    REQUIRE(buffer.PublishReplica(*version, *kWholeBuffer, secondAllocation).has_value());
    secondAllocation = {};

    REQUIRE(registry.Register(cache).has_value());
    REQUIRE(registry.Register(buffer).has_value());
    auto invalidated = registry.InvalidateService(firstService.Id());
    REQUIRE(invalidated.has_value());
    REQUIRE(invalidated->newlyRetired);
    REQUIRE(invalidated->cachesVisited == 1);
    REQUIRE(invalidated->artifactsRetired == 1);
    REQUIRE(invalidated->buffersVisited == 1);
    REQUIRE(invalidated->replicasRetired == 1);
    REQUIRE(cache.Size() == 1);
    REQUIRE_FALSE(cache.Find({firstKey, firstService.Id()}).has_value());
    REQUIRE(cache.Find({secondKey, secondService.Id()}).has_value());
    REQUIRE(buffer.ReplicaCount() == 1);
    REQUIRE(buffer.AcquireReplica(kSecondLocation, *kWholeBuffer).has_value());
    REQUIRE(artifactDestroyCount.load() == 1);
    REQUIRE(allocationDestroyCount.load() == 1);

    Artifact staleArtifact = MakeArtifact(firstService, 13, artifactDestroyCount);
    auto artifactPublication = cache.Insert(staleArtifact);
    REQUIRE_FALSE(artifactPublication.has_value());
    REQUIRE(artifactPublication.error() == ArtifactCacheError::RetiredService);
    Allocation staleAllocation = MakeAllocation(firstService, kFirstLocation, allocationDestroyCount);
    auto replicaPublication = buffer.PublishReplica(*version, *kWholeBuffer, staleAllocation);
    REQUIRE_FALSE(replicaPublication.has_value());
    REQUIRE(replicaPublication.error() == MemoryError::RetiredService);
}

TEST_CASE("Registration after retirement applies the published tombstone "
          "before returning",
          "[Sora][Compute][Runtime][Invalidation][Linearization]") {
    const auto& service = Sora::Test::TestBackendService();
    std::atomic<uint32_t> destroyCount = 0;
    ArtifactCache cache{2};
    RuntimeInvalidationRegistry registry;
    REQUIRE(registry.InvalidateService(service.Id()).has_value());

    Artifact artifact = MakeArtifact(service, 21, destroyCount);
    REQUIRE(cache.Insert(artifact).has_value());
    artifact = {};
    const auto cacheRegistration = registry.Register(cache);
    REQUIRE(cacheRegistration.has_value());
    REQUIRE(*cacheRegistration);
    REQUIRE(cache.Size() == 0);

    Buffer buffer = MakeBuffer({4, 1});
    Allocation allocation = MakeAllocation(service, kFirstLocation, destroyCount);
    REQUIRE(buffer.CommitWholeWrite(allocation).has_value());
    allocation = {};
    const auto bufferRegistration = registry.Register(buffer);
    REQUIRE(bufferRegistration.has_value());
    REQUIRE(*bufferRegistration);
    REQUIRE(buffer.ReplicaCount() == 0);
    REQUIRE(destroyCount.load() == 2);
}

TEST_CASE("Concurrent registration and retirement converge to no "
          "failed-service replica",
          "[Sora][Compute][Runtime][Invalidation][Concurrency]") {
    const auto& service = Sora::Test::TestBackendService();
    for (uint64_t iteration = 0; iteration < 64; ++iteration) {
        std::atomic<uint32_t> destroyCount = 0;
        RuntimeInvalidationRegistry registry;
        Buffer buffer = MakeBuffer({100 + iteration, 1});
        Allocation allocation = MakeAllocation(service, kFirstLocation, destroyCount);
        REQUIRE(buffer.CommitWholeWrite(allocation).has_value());
        allocation = {};
        std::barrier start{2};
        std::atomic<bool> registrationSucceeded = false;
        std::atomic<bool> invalidationSucceeded = false;

        std::jthread registerThread([&] {
            start.arrive_and_wait();
            registrationSucceeded.store(registry.Register(buffer).has_value());
        });
        std::jthread invalidationThread([&] {
            start.arrive_and_wait();
            invalidationSucceeded.store(registry.InvalidateService(service.Id()).has_value());
        });
        registerThread.join();
        invalidationThread.join();

        REQUIRE(registrationSucceeded.load());
        REQUIRE(invalidationSucceeded.load());
        REQUIRE(buffer.ReplicaCount() == 0);
        REQUIRE(destroyCount.load() == 1);
    }
}

TEST_CASE("Backend resource destruction may re-enter the invalidation registry",
          "[Sora][Compute][Runtime][Invalidation][Reentrancy]") {
    const auto& service = Sora::Test::TestBackendService();
    std::atomic<uint32_t> destroyCount = 0;
    std::atomic<bool> observedRetirement = false;
    ArtifactCache cache{1};
    RuntimeInvalidationRegistry registry;
    Buffer buffer = MakeBuffer({5, 1});

    Artifact artifact = MakeArtifact(service, 31, destroyCount, &registry, &observedRetirement);
    REQUIRE(cache.Insert(artifact).has_value());
    artifact = {};
    REQUIRE(registry.Register(cache).has_value());
    REQUIRE(registry.Register(buffer).has_value());

    auto invalidated = registry.InvalidateService(service.Id());
    REQUIRE(invalidated.has_value());
    REQUIRE(destroyCount.load() == 1);
    REQUIRE(observedRetirement.load());
}
