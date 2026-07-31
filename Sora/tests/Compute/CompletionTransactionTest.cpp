#include "BackendTestSupport.h"
#include <Sora/Compute/Runtime/CompletionTransaction.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {1, 1}, .device = {1, 1}, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kDevice{
        .domain = {2, 2}, .device = {2, 2}, .backend = Sora::Compute::BackendKind::Vulkan};

    struct NativeAllocation {
        std::atomic<uint32_t>* destroyCount = nullptr;
        const Sora::Compute::Buffer* reentrantBuffer = nullptr;
        std::atomic<uint64_t>* observedVersion = nullptr;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        if (allocation->reentrantBuffer != nullptr) {
            allocation->observedVersion->store(allocation->reentrantBuffer->CurrentVersion(),
                                               std::memory_order_relaxed);
        }
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

    Sora::Compute::Allocation
    MakeAllocation(std::atomic<uint32_t>& destroyCount, Sora::Compute::MemoryLocation location,
                   Sora::Compute::PhysicalMemoryId memory, const Sora::Compute::Buffer* reentrantBuffer = nullptr,
                   std::atomic<uint64_t>* observedVersion = nullptr, uint64_t alignment = 64) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount, reentrantBuffer, observedVersion);
        auto allocation = Sora::Compute::Allocation::Adopt(Sora::Test::TestBackendService(),
                                                           {
                                                               .location = location,
                                                               .memory = memory,
                                                               .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                               .sizeBytes = 256,
                                                               .alignment = alignment,
                                                               .state = native.release(),
                                                               .destroy = DestroyAllocation,
                                                           });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

    uint64_t Initialize(Sora::Compute::Buffer& buffer, std::atomic<uint32_t>& destroyCount,
                        Sora::Compute::PhysicalMemoryId memory) {
        auto version = buffer.CommitWholeWrite(MakeAllocation(destroyCount, kHost, memory));
        REQUIRE(version.has_value());
        return *version;
    }

    constexpr auto kWhole = Sora::Compute::ByteRegion::Make(0, 256);
    static_assert(kWhole.has_value());

} // namespace

TEST_CASE("CompletionTransaction atomically publishes current-version replicas across buffers",
          "[Sora][Compute][Runtime][Publication]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto first = MakeBuffer({1, 1});
    auto second = MakeBuffer({1, 2});
    const uint64_t firstVersion = Initialize(first, destroyCount, {1, 1});
    const uint64_t secondVersion = Initialize(second, destroyCount, {2, 2});
    auto firstDevice = MakeAllocation(destroyCount, kDevice, {3, 3});
    auto secondDevice = MakeAllocation(destroyCount, kDevice, {4, 4});

    Sora::Compute::CompletionTransaction transaction;
    REQUIRE(transaction.AddReplica(first, firstVersion, *kWhole, firstDevice).has_value());
    REQUIRE(transaction.AddReplica(second, secondVersion, *kWhole, secondDevice).has_value());
    REQUIRE(transaction.ActionCount() == 2);
    REQUIRE(std::move(transaction).Commit().has_value());
    REQUIRE(first.AcquireReplica(kDevice, *kWhole).has_value());
    REQUIRE(second.AcquireReplica(kDevice, *kWhole).has_value());
}

TEST_CASE("CompletionTransaction rejects stale input without partially publishing earlier actions",
          "[Sora][Compute][Runtime][Publication][Atomicity]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto first = MakeBuffer({2, 1});
    auto second = MakeBuffer({2, 2});
    const uint64_t firstVersion = Initialize(first, destroyCount, {10, 10});
    const uint64_t secondVersion = Initialize(second, destroyCount, {11, 11});

    Sora::Compute::CompletionTransaction transaction;
    REQUIRE(transaction.AddReplica(first, firstVersion, *kWhole, MakeAllocation(destroyCount, kDevice, {12, 12}))
                .has_value());
    REQUIRE(transaction.AddReplica(second, secondVersion + 1, *kWhole, MakeAllocation(destroyCount, kDevice, {13, 13}))
                .has_value());
    const auto committed = std::move(transaction).Commit();
    REQUIRE_FALSE(committed.has_value());
    REQUIRE(committed.error().code == Sora::Compute::PublicationErrorCode::StaleVersion);
    REQUIRE_FALSE(first.AcquireReplica(kDevice, *kWhole).has_value());
    REQUIRE_FALSE(second.AcquireReplica(kDevice, *kWhole).has_value());
    REQUIRE(first.CurrentVersion() == firstVersion);
    REQUIRE(second.CurrentVersion() == secondVersion);
}

TEST_CASE("CompletionTransaction commits one terminal whole write and rejects contradictory actions",
          "[Sora][Compute][Runtime][Publication][Write]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({3, 1});
    auto target = MakeAllocation(destroyCount, kDevice, {20, 20});
    Sora::Compute::CompletionTransaction write;
    REQUIRE(write.AddWholeWrite(buffer, 0, target).has_value());
    REQUIRE(std::move(write).Commit().has_value());
    REQUIRE(buffer.CurrentVersion() == 1);
    REQUIRE(buffer.AcquireReplica(kDevice, *kWhole).has_value());

    Sora::Compute::CompletionTransaction conflict;
    REQUIRE(conflict.AddReplica(buffer, 1, *kWhole, MakeAllocation(destroyCount, kHost, {21, 21})).has_value());
    const auto wholeWrite = conflict.AddWholeWrite(buffer, 1, MakeAllocation(destroyCount, kHost, {22, 22}));
    REQUIRE_FALSE(wholeWrite.has_value());
    REQUIRE(wholeWrite.error().code == Sora::Compute::PublicationErrorCode::ConflictingActions);
}

TEST_CASE("CompletionTransaction retires replaced allocations after releasing every buffer lock",
          "[Sora][Compute][Runtime][Publication][Reentrant]") {
    std::atomic<uint32_t> destroyCount = 0;
    std::atomic<uint64_t> observedVersion = 0;
    auto buffer = MakeBuffer({4, 1});
    auto old = MakeAllocation(destroyCount, kHost, {30, 30}, &buffer, &observedVersion);
    REQUIRE(buffer.CommitWholeWrite(std::move(old)).has_value());

    Sora::Compute::CompletionTransaction transaction;
    REQUIRE(transaction.AddWholeWrite(buffer, 1, MakeAllocation(destroyCount, kDevice, {31, 31})).has_value());
    REQUIRE(std::move(transaction).Commit().has_value());
    REQUIRE(buffer.CurrentVersion() == 2);
    REQUIRE(observedVersion.load(std::memory_order_relaxed) == 2);
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("CompletionTransaction rejects publication after its backend service retires",
          "[Sora][Compute][Runtime][Publication][DeviceGeneration]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({5, 1});
    const uint64_t version = Initialize(buffer, destroyCount, {40, 40});

    Sora::Compute::CompletionTransaction transaction;
    REQUIRE(
        transaction.AddReplica(buffer, version, *kWhole, MakeAllocation(destroyCount, kDevice, {41, 41})).has_value());
    const auto invalidated = buffer.InvalidateService(Sora::Test::TestBackendService().Id());
    REQUIRE(invalidated.has_value());
    REQUIRE(*invalidated == 1);

    const auto committed = std::move(transaction).Commit();
    REQUIRE_FALSE(committed.has_value());
    REQUIRE(committed.error().code == Sora::Compute::PublicationErrorCode::RetiredService);
    REQUIRE(buffer.ReplicaCount() == 0);
    REQUIRE(buffer.CurrentVersion() == version);
}

TEST_CASE("CompletionTransaction jointly publishes a strict partition cover as one logical version",
          "[Sora][Compute][Runtime][Publication][Partition]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({6, 1});
    const std::array parts{
        Sora::Compute::PartitionedWritePart{.region = *Sora::Compute::ByteRegion::Make(0, 96),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {50, 50})},
        Sora::Compute::PartitionedWritePart{.region = *Sora::Compute::ByteRegion::Make(96, 160),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {51, 51})},
    };

    Sora::Compute::CompletionTransaction transaction;
    REQUIRE(transaction.AddPartitionedWrite(buffer, 0, parts).has_value());
    REQUIRE(transaction.ActionCount() == 2);
    REQUIRE(std::move(transaction).Commit().has_value());
    REQUIRE(buffer.CurrentVersion() == 1);
    REQUIRE(buffer.AcquireReplica(kDevice, parts[0].region).has_value());
    REQUIRE(buffer.AcquireReplica(kDevice, parts[1].region).has_value());
    REQUIRE_FALSE(buffer.AcquireReplica(kDevice, *kWhole).has_value());
}

TEST_CASE("CompletionTransaction rejects partition gaps before mutating logical state",
          "[Sora][Compute][Runtime][Publication][Partition][Validation]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer({7, 1});
    const std::array parts{
        Sora::Compute::PartitionedWritePart{.region = *Sora::Compute::ByteRegion::Make(0, 96),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {60, 60})},
        Sora::Compute::PartitionedWritePart{.region = *Sora::Compute::ByteRegion::Make(100, 156),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {61, 61})},
    };

    Sora::Compute::CompletionTransaction transaction;
    const auto added = transaction.AddPartitionedWrite(buffer, 0, parts);
    REQUIRE_FALSE(added.has_value());
    REQUIRE(added.error().code == Sora::Compute::PublicationErrorCode::InvalidAction);
    REQUIRE(transaction.ActionCount() == 0);
    REQUIRE(buffer.CurrentVersion() == 0);

    const std::array misalignedParts{
        Sora::Compute::PartitionedWritePart{.region = *Sora::Compute::ByteRegion::Make(0, 128),
                                            .allocation =
                                                MakeAllocation(destroyCount, kDevice, {62, 62}, nullptr, nullptr, 32)},
        Sora::Compute::PartitionedWritePart{.region = *Sora::Compute::ByteRegion::Make(128, 128),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {63, 63})},
    };
    Sora::Compute::CompletionTransaction misaligned;
    const auto rejectedAlignment = misaligned.AddPartitionedWrite(buffer, 0, misalignedParts);
    REQUIRE_FALSE(rejectedAlignment.has_value());
    REQUIRE(rejectedAlignment.error().code == Sora::Compute::PublicationErrorCode::InvalidAction);
    REQUIRE(misaligned.ActionCount() == 0);
}

TEST_CASE("CompletionTransaction distinguishes partition capacity from invalid coverage",
          "[Sora][Compute][Runtime][Publication][Partition][Capacity]") {
    auto buffer = MakeBuffer({7, 2});
    const std::array<Sora::Compute::PartitionedWritePart, 257> parts{};
    Sora::Compute::CompletionTransaction transaction;
    const auto added = transaction.AddPartitionedWrite(buffer, 0, parts);
    REQUIRE_FALSE(added.has_value());
    REQUIRE(added.error().code == Sora::Compute::PublicationErrorCode::CapacityExceeded);
    REQUIRE(transaction.ActionCount() == 0);
    REQUIRE(buffer.CurrentVersion() == 0);
}

TEST_CASE("CompletionTransaction advances an affine buffer only for an exact sparse partition union",
          "[Sora][Compute][Runtime][Publication][Partition][Affine]") {
    constexpr std::array extents{uint64_t{8}};
    constexpr std::array strides{uint64_t{8}};
    constexpr auto logical = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    static_assert(logical.has_value());
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = Sora::Compute::Buffer::Create({.id = {8, 1},
                                                 .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                                                 .layout = Sora::Compute::LayoutKind::Strided,
                                                 .rank = 1,
                                                 .sizeBytes = 64,
                                                 .alignment = 64},
                                                *logical);
    REQUIRE(buffer.has_value());

    constexpr std::array shardExtents{uint64_t{4}};
    constexpr auto first = Sora::Compute::AffineByteFootprint::Create(shardExtents, strides, 4, 0, 4, 1);
    constexpr auto second = Sora::Compute::AffineByteFootprint::Create(shardExtents, strides, 4, 32, 4, 1);
    static_assert(first.has_value() && second.has_value());
    const std::array parts{
        Sora::Compute::PartitionedWritePart{.region = first->BoundingRegion(),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {70, 70}),
                                            .byteFootprint = *first},
        Sora::Compute::PartitionedWritePart{.region = second->BoundingRegion(),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {71, 71}),
                                            .byteFootprint = *second},
    };

    Sora::Compute::CompletionTransaction transaction;
    REQUIRE(transaction.AddPartitionedWrite(*buffer, 0, parts).has_value());
    REQUIRE(std::move(transaction).Commit().has_value());
    REQUIRE(buffer->CurrentVersion() == 1);
    REQUIRE(buffer->AcquireReplica(kDevice, Sora::Compute::ByteCoverageView::Affine(*first)).has_value());
    REQUIRE(buffer->AcquireReplica(kDevice, Sora::Compute::ByteCoverageView::Affine(*second)).has_value());
    REQUIRE_FALSE(buffer->AcquireReplica(kDevice, *Sora::Compute::ByteRegion::Make(4, 4)).has_value());

    constexpr std::array incompleteExtents{uint64_t{3}};
    constexpr auto incomplete = Sora::Compute::AffineByteFootprint::Create(incompleteExtents, strides, 4, 32, 4, 1);
    static_assert(incomplete.has_value());
    const std::array incompleteParts{
        Sora::Compute::PartitionedWritePart{.region = first->BoundingRegion(),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {72, 72}),
                                            .byteFootprint = *first},
        Sora::Compute::PartitionedWritePart{.region = incomplete->BoundingRegion(),
                                            .allocation = MakeAllocation(destroyCount, kDevice, {73, 73}),
                                            .byteFootprint = *incomplete},
    };
    Sora::Compute::CompletionTransaction rejected;
    const auto added = rejected.AddPartitionedWrite(*buffer, 1, incompleteParts);
    REQUIRE_FALSE(added.has_value());
    REQUIRE(added.error().code == Sora::Compute::PublicationErrorCode::InvalidAction);
    REQUIRE(buffer->CurrentVersion() == 1);
}
