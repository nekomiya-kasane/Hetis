#include "BackendTestSupport.h"
#include <Sora/Compute/Memory/BufferView.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace {

    struct MockAllocationState {
        std::atomic<uint32_t>* destroyCount = nullptr;
        const Sora::Compute::Buffer* reentrantBuffer = nullptr;
        std::atomic<uint64_t>* observedVersion = nullptr;
    };

    void DestroyMockAllocation(void* opaque) noexcept {
        std::unique_ptr<MockAllocationState> state{static_cast<MockAllocationState*>(opaque)};
        if (state->reentrantBuffer != nullptr) {
            state->observedVersion->store(state->reentrantBuffer->CurrentVersion(), std::memory_order_relaxed);
        }
        state->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyTestService(void* opaque) noexcept {
        delete static_cast<uint32_t*>(opaque);
    }

    Sora::Compute::BackendService MakeTestService(Sora::Compute::BackendServiceId id) {
        auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
        auto service = Sora::Compute::BackendService::Adopt(
            {.id = id, .state = new uint32_t{}, .destroy = DestroyTestService}, std::move(code));
        REQUIRE(service.has_value());
        return *service;
    }

    constexpr Sora::Compute::MemoryLocation kHostLocation{
        .domain = {1, 1},
        .device = {1, 1},
        .backend = Sora::Compute::BackendKind::Cpu,
    };
    constexpr Sora::Compute::MemoryLocation kGpuLocation{
        .domain = {2, 3},
        .device = {2, 2},
        .backend = Sora::Compute::BackendKind::Vulkan,
    };

    Sora::Compute::Allocation MakeAllocation(Sora::Compute::MemoryLocation location, uint64_t size,
                                             std::atomic<uint32_t>& destroyCount,
                                             const Sora::Compute::Buffer* reentrantBuffer = nullptr,
                                             std::atomic<uint64_t>* observedVersion = nullptr,
                                             Sora::Compute::BackendService service = Sora::Test::TestBackendService()) {
        static std::atomic<uint64_t> nextMemoryId = 1;
        auto state = std::make_unique<MockAllocationState>(&destroyCount, reentrantBuffer, observedVersion);
        auto allocation = Sora::Compute::Allocation::Adopt(
            std::move(service), {
                                    .location = location,
                                    .memory = {nextMemoryId.fetch_add(1, std::memory_order_relaxed), 1},
                                    .properties = Sora::Compute::MemoryProperty::DeviceLocal,
                                    .sizeBytes = size,
                                    .alignment = 64,
                                    .state = state.release(),
                                    .destroy = DestroyMockAllocation,
                                });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

} // namespace

TEST_CASE("ByteRegion rejects overflow and uses half-open containment", "[Sora][Compute][Memory][Region]") {
    constexpr auto whole = Sora::Compute::ByteRegion::Make(16, 32);
    constexpr auto inside = Sora::Compute::ByteRegion::Make(24, 8);
    constexpr auto touchingEnd = Sora::Compute::ByteRegion::Make(48, 0);
    constexpr auto outside = Sora::Compute::ByteRegion::Make(40, 16);
    static_assert(whole.has_value());
    static_assert(inside.has_value());
    static_assert(whole->Contains(*inside));
    static_assert(whole->Contains(*touchingEnd));
    static_assert(!whole->Contains(*outside));

    REQUIRE_FALSE(Sora::Compute::ByteRegion::Make(UINT64_MAX - 1, 2).has_value());
    REQUIRE_FALSE(touchingEnd->Intersects(*whole));
    REQUIRE_FALSE(whole->Intersects(*touchingEnd));
}

TEST_CASE("Allocation validates payload and destroys shared state exactly once",
          "[Sora][Compute][Memory][Allocation]") {
    std::atomic<uint32_t> destroyCount = 0;
    {
        Sora::Compute::Allocation allocation = MakeAllocation(kHostLocation, 256, destroyCount);
        {
            const Sora::Compute::Allocation copy = allocation;
            REQUIRE(copy.View().state == allocation.View().state);
            REQUIRE(copy.UseCount() == 2);
            REQUIRE(copy.View().Covers(192, 64));
            REQUIRE_FALSE(copy.View().Covers(193, 64));
        }
        REQUIRE(destroyCount.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);

    auto invalidState = std::make_unique<MockAllocationState>(&destroyCount);
    const auto invalid =
        Sora::Compute::Allocation::Adopt(Sora::Test::TestBackendService(), {
                                                                               .location = kHostLocation,
                                                                               .memory = {100, 1},
                                                                               .sizeBytes = 256,
                                                                               .alignment = 48,
                                                                               .state = invalidState.release(),
                                                                               .destroy = DestroyMockAllocation,
                                                                           });
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error() == Sora::Compute::AllocationError::InvalidPayload);
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Buffer tracks current whole-buffer version and ready regional replicas",
          "[Sora][Compute][Memory][Residency]") {
    std::atomic<uint32_t> destroyCount = 0;
    {
        auto buffer = Sora::Compute::Buffer::Create({
            .id = {0xB0, 0xFF},
            .elementType = {Sora::Compute::ScalarKind::Float32, 1},
            .layout = Sora::Compute::LayoutKind::Contiguous,
            .rank = 1,
            .sizeBytes = 256,
            .alignment = 64,
        });
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->CurrentVersion() == 0);

        Sora::Compute::Allocation host = MakeAllocation(kHostLocation, 256, destroyCount);
        const auto firstVersion = buffer->CommitWholeWrite(host);
        REQUIRE(firstVersion.has_value());
        REQUIRE(*firstVersion == 1);
        REQUIRE(buffer->ReplicaCount() == 1);

        const auto middle = Sora::Compute::ByteRegion::Make(64, 64);
        REQUIRE(middle.has_value());
        auto hostLease = buffer->AcquireReplica(kHostLocation, *middle);
        REQUIRE(hostLease.has_value());
        REQUIRE(hostLease->version == 1);
        const auto hostOffset = hostLease->ResolveAllocationOffset(*middle);
        REQUIRE(hostOffset.has_value());
        REQUIRE(*hostOffset == 64);

        Sora::Compute::Allocation gpu = MakeAllocation(kGpuLocation, 256, destroyCount);
        REQUIRE(buffer->PublishReplica(1, *middle, gpu, 128).has_value());
        REQUIRE(buffer->ReplicaCount() == 2);
        auto gpuLease = buffer->AcquireReplica(kGpuLocation, *middle);
        REQUIRE(gpuLease.has_value());
        const auto gpuOffset = gpuLease->ResolveAllocationOffset(*middle);
        REQUIRE(gpuOffset.has_value());
        REQUIRE(*gpuOffset == 128);

        Sora::Compute::Allocation nextHost = MakeAllocation(kHostLocation, 256, destroyCount);
        const auto secondVersion = buffer->CommitWholeWrite(nextHost);
        REQUIRE(secondVersion.has_value());
        REQUIRE(*secondVersion == 2);
        REQUIRE(buffer->ReplicaCount() == 1);
        const auto missing = buffer->AcquireReplica(kGpuLocation, *middle);
        REQUIRE_FALSE(missing.has_value());
        REQUIRE(missing.error() == Sora::Compute::MemoryError::NotResident);

        Sora::Compute::Allocation staleGpu = MakeAllocation(kGpuLocation, 256, destroyCount);
        const auto stale = buffer->PublishReplica(1, *middle, staleGpu);
        REQUIRE_FALSE(stale.has_value());
        REQUIRE(stale.error() == Sora::Compute::MemoryError::StaleVersion);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 4);
}

TEST_CASE("Buffer invalidates replicas by backend service while preserving the logical version",
          "[Sora][Compute][Memory][Residency][DeviceGeneration]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto secondService = MakeTestService({0x5345434F4E445F4DULL, 1});
    auto buffer = Sora::Compute::Buffer::Create({.id = {0xD0, 0xD1},
                                                 .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                                                 .layout = Sora::Compute::LayoutKind::Contiguous,
                                                 .rank = 1,
                                                 .sizeBytes = 256,
                                                 .alignment = 64});
    REQUIRE(buffer.has_value());
    auto committed = buffer->CommitWholeWrite(MakeAllocation(kHostLocation, 256, destroyCount));
    REQUIRE(committed.has_value());
    constexpr auto whole = Sora::Compute::ByteRegion::Make(0, 256);
    static_assert(whole.has_value());
    REQUIRE(buffer
                ->PublishReplica(*committed, *whole,
                                 MakeAllocation(kGpuLocation, 256, destroyCount, nullptr, nullptr, secondService))
                .has_value());
    REQUIRE(buffer->ReplicaCount() == 2);

    const auto invalidated = buffer->InvalidateService(Sora::Test::TestBackendService().Id());
    REQUIRE(invalidated.has_value());
    REQUIRE(*invalidated == 1);
    REQUIRE(buffer->ReplicaCount() == 1);
    REQUIRE(buffer->CurrentVersion() == *committed);
    REQUIRE_FALSE(buffer->AcquireReplica(kHostLocation, *whole).has_value());
    REQUIRE(buffer->AcquireReplica(kGpuLocation, *whole).has_value());
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);

    REQUIRE_FALSE(
        buffer->PublishReplica(*committed, *whole, MakeAllocation(kHostLocation, 256, destroyCount)).has_value());
    REQUIRE(buffer->ReplicaCount() == 1);
}

TEST_CASE("Buffer releases retired allocations after publishing the new version",
          "[Sora][Compute][Memory][Residency]") {
    std::atomic<uint32_t> destroyCount = 0;
    std::atomic<uint64_t> observedVersion = 0;
    {
        auto buffer = Sora::Compute::Buffer::Create({
            .id = {0xBE, 0xEF},
            .elementType = {Sora::Compute::ScalarKind::Float32, 1},
            .layout = Sora::Compute::LayoutKind::Contiguous,
            .rank = 1,
            .sizeBytes = 256,
            .alignment = 64,
        });
        REQUIRE(buffer.has_value());
        {
            Sora::Compute::Allocation first =
                MakeAllocation(kHostLocation, 256, destroyCount, &*buffer, &observedVersion);
            const auto committed = buffer->CommitWholeWrite(first);
            REQUIRE(committed.has_value());
        }
        REQUIRE(destroyCount.load(std::memory_order_relaxed) == 0);

        {
            Sora::Compute::Allocation second = MakeAllocation(kHostLocation, 256, destroyCount);
            const auto committed = buffer->CommitWholeWrite(second);
            REQUIRE(committed.has_value());
            REQUIRE(*committed == 2);
        }
        REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(observedVersion.load(std::memory_order_relaxed) == 2);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Buffer rejects invalid descriptors and undersized allocations", "[Sora][Compute][Memory][Validation]") {
    auto invalid = Sora::Compute::Buffer::Create({
        .id = {},
        .elementType = {Sora::Compute::ScalarKind::Float32, 1},
        .layout = Sora::Compute::LayoutKind::Contiguous,
        .rank = 1,
        .sizeBytes = 64,
        .alignment = 64,
    });
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error() == Sora::Compute::MemoryError::InvalidDescriptor);

    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = Sora::Compute::Buffer::Create({
        .id = {1, 0},
        .elementType = {Sora::Compute::ScalarKind::Float32, 1},
        .layout = Sora::Compute::LayoutKind::Contiguous,
        .rank = 1,
        .sizeBytes = 128,
        .alignment = 64,
    });
    REQUIRE(buffer.has_value());
    Sora::Compute::Allocation tooSmall = MakeAllocation(kHostLocation, 64, destroyCount);
    const auto whole = Sora::Compute::ByteRegion::Make(0, 128);
    REQUIRE(whole.has_value());
    const auto unpublished = buffer->PublishReplica(0, *whole, tooSmall);
    REQUIRE_FALSE(unpublished.has_value());
    REQUIRE(unpublished.error() == Sora::Compute::MemoryError::StaleVersion);

    const auto committed = buffer->CommitWholeWrite(tooSmall);
    REQUIRE_FALSE(committed.has_value());
    REQUIRE(committed.error() == Sora::Compute::MemoryError::InvalidAllocation);
}

TEST_CASE("Buffer owns exact affine logical coverage independently of physical padding",
          "[Sora][Compute][Memory][Coverage]") {
    constexpr Sora::Compute::ValueType vec3{Sora::Compute::ScalarKind::Float32, 3};
    constexpr Sora::Compute::BufferDesc desc{.id = {7, 9},
                                             .elementType = vec3,
                                             .layout = Sora::Compute::LayoutKind::StructureOfArrays,
                                             .rank = 1,
                                             .sizeBytes = 256,
                                             .alignment = 64};
    constexpr std::array extents{uint64_t{8}};
    constexpr std::array strides{uint64_t{4}};
    constexpr auto logical = Sora::Compute::AffineByteFootprint::Create(extents, strides, 96, 0, 4, 3);
    static_assert(logical.has_value());

    const auto implicit = Sora::Compute::Buffer::Create(desc);
    REQUIRE_FALSE(implicit.has_value());
    REQUIRE(implicit.error() == Sora::Compute::MemoryError::InvalidDescriptor);

    constexpr auto wrongType = Sora::Compute::AffineByteFootprint::Create(extents, strides, 96, 0, 4, 2);
    static_assert(wrongType.has_value());
    REQUIRE_FALSE(Sora::Compute::Buffer::Create(desc, *wrongType).has_value());

    std::optional<Sora::Compute::BufferResidencySnapshot> retained;
    {
        auto buffer = Sora::Compute::Buffer::Create(desc, *logical);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->LogicalCoverage().OccupiedByteCount() == 96);

        auto wholeView = Sora::Compute::BufferView::StructureOfArrays(*buffer, extents, 0, 96);
        REQUIRE(wholeView.has_value());
        constexpr std::array origin{uint64_t{2}};
        constexpr std::array subExtents{uint64_t{4}};
        REQUIRE(wholeView->SubView(origin, subExtents).has_value());

        auto paddingView = Sora::Compute::BufferView::StructureOfArrays(*buffer, extents, 32, 96);
        REQUIRE_FALSE(paddingView.has_value());
        REQUIRE(paddingView.error() == Sora::Compute::BufferViewError::OutsideLogicalCoverage);

        auto snapshot = buffer->SnapshotResidency();
        REQUIRE(snapshot.has_value());
        retained.emplace(std::move(*snapshot));
    }

    REQUIRE(retained.has_value());
    const auto coverage = retained->LogicalCoverage();
    REQUIRE(coverage.Valid());
    REQUIRE(coverage.OccupiedByteCount() == 96);
    const auto regions = coverage.ProjectCanonicalRegions<3>();
    REQUIRE(regions.has_value());
    REQUIRE(regions->Size() == 3);
}
