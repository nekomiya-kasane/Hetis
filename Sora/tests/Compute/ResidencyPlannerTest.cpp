#include "BackendTestSupport.h"
#include <Sora/Compute/Memory/ResidencyPlanner.h>
#include <Sora/Compute/Runtime/CandidateSynthesis.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace {

    constexpr Sora::Compute::BackendServiceId kService{0x534F52415F544553ULL, 0x545F4241434B454EULL};
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {1, 1}, .device = {10, 10}, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kStaging{
        .domain = {2, 2}, .device = {20, 20}, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::MemoryLocation kGpu{
        .domain = {3, 3}, .device = {20, 20}, .backend = Sora::Compute::BackendKind::Vulkan};

    constexpr Sora::Compute::KernelCalibrationKey kCandidate{
        .kernel = {30, 30},
        .target = {.device = {20, 20},
                   .backend = Sora::Compute::BackendKind::Vulkan,
                   .hostArchitecture = Sora::CpuArchitecture::X64,
                   .operatingSystem = Sora::OperatingSystem::Windows,
                   .binaryFormat = Sora::BinaryFormat::PE,
                   .byteOrder = Sora::ByteOrder::Little,
                   .abi = Sora::AbiFamily::MSVC,
                   .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                   .addressBits = 64,
                   .architectureId = 31,
                   .compilerIdentity = {32, 32},
                   .driverIdentity = {33, 33}},
        .queue = {34, 34}};

    struct NativeAllocation {
        std::atomic<uint32_t>* destroyCount = nullptr;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        allocation->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    Sora::Compute::Allocation MakeAllocation(Sora::Compute::MemoryLocation location,
                                             std::atomic<uint32_t>& destroyCount) {
        static std::atomic<uint64_t> nextMemoryId = 1;
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Sora::Compute::Allocation::Adopt(
            Sora::Test::TestBackendService(), {
                                                  .location = location,
                                                  .memory = {nextMemoryId.fetch_add(1, std::memory_order_relaxed), 2},
                                                  .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                  .sizeBytes = 4096,
                                                  .alignment = 64,
                                                  .state = native.release(),
                                                  .destroy = DestroyAllocation,
                                              });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

    Sora::Compute::Buffer MakeHostBuffer(std::atomic<uint32_t>& destroyCount) {
        auto buffer = Sora::Compute::Buffer::Create({
            .id = {0xB0, 0x01},
            .elementType = {Sora::Compute::ScalarKind::Float32, 1},
            .layout = Sora::Compute::LayoutKind::Contiguous,
            .rank = 1,
            .sizeBytes = 4096,
            .alignment = 64,
        });
        REQUIRE(buffer.has_value());
        auto committed = buffer->CommitWholeWrite(MakeAllocation(kHost, destroyCount));
        REQUIRE(committed.has_value());
        return *buffer;
    }

    constexpr std::array kTopology{
        Sora::Compute::TransferLink{.sourceService = kService,
                                    .service = kService,
                                    .engine = {1, 1},
                                    .source = kHost,
                                    .destination = kGpu,
                                    .kind = Sora::Compute::TransferKind::HostBridge,
                                    .bytesPerSecond = 100'000'000,
                                    .latencyNanoseconds = 1'000},
        Sora::Compute::TransferLink{.sourceService = kService,
                                    .service = kService,
                                    .engine = {2, 2},
                                    .source = kHost,
                                    .destination = kStaging,
                                    .kind = Sora::Compute::TransferKind::HostBridge,
                                    .bytesPerSecond = 1'000'000'000,
                                    .latencyNanoseconds = 100},
        Sora::Compute::TransferLink{.sourceService = kService,
                                    .service = kService,
                                    .engine = {3, 3},
                                    .source = kStaging,
                                    .destination = kGpu,
                                    .kind = Sora::Compute::TransferKind::Copy,
                                    .bytesPerSecond = 1'000'000'000,
                                    .latencyNanoseconds = 100},
    };

} // namespace

TEST_CASE("Residency planner selects a lower-cost multi-hop route and retains the source lease",
          "[Sora][Compute][Memory][Planner]") {
    std::atomic<uint32_t> destroyCount = 0;
    {
        Sora::Compute::Buffer buffer = MakeHostBuffer(destroyCount);
        const auto region = Sora::Compute::ByteRegion::Make(0, 1024);
        REQUIRE(region.has_value());
        auto snapshot = buffer.SnapshotResidency();
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->version == 1);
        REQUIRE(snapshot->replicas.size() == 1);

        auto route = Sora::Compute::PlanResidency(
            *snapshot, Sora::Compute::ResidencyRequest{.service = kService, .destination = kGpu, .region = *region},
            kTopology);
        REQUIRE(route.has_value());
        REQUIRE_FALSE(route->AlreadyResident());
        REQUIRE(route->source.allocation.Location() == kHost);
        REQUIRE(route->hops.size() == 2);
        REQUIRE(route->hops[0].destination == kStaging);
        REQUIRE(route->hops[1].destination == kGpu);
        REQUIRE(route->estimatedNanoseconds == 2'248);
        const auto projection =
            Sora::Compute::ProjectCandidateTransferCost(Sora::Test::TestBackendService().Id(), kCandidate,
                                                        std::span<const Sora::Compute::ResidencyRoute>{&*route, 1});
        REQUIRE(projection.has_value());
        REQUIRE(projection->bytes == 1024);
        REQUIRE(projection->nanoseconds == 2'248);

        auto discontinuous = *route;
        discontinuous.hops[1].source = kHost;
        const auto invalid = Sora::Compute::ProjectCandidateTransferCost(
            Sora::Test::TestBackendService().Id(), kCandidate,
            std::span<const Sora::Compute::ResidencyRoute>{&discontinuous, 1});
        REQUIRE(invalid.error() == Sora::Compute::CandidateSynthesisError::InvalidRequest);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Residency planner returns an existing destination replica without a transfer",
          "[Sora][Compute][Memory][Planner]") {
    std::atomic<uint32_t> destroyCount = 0;
    {
        Sora::Compute::Buffer buffer = MakeHostBuffer(destroyCount);
        const auto region = Sora::Compute::ByteRegion::Make(512, 1024);
        REQUIRE(region.has_value());
        REQUIRE(buffer.PublishReplica(1, *region, MakeAllocation(kGpu, destroyCount), 512).has_value());
        auto snapshot = buffer.SnapshotResidency();
        REQUIRE(snapshot.has_value());

        auto route = Sora::Compute::PlanResidency(
            *snapshot, Sora::Compute::ResidencyRequest{.service = kService, .destination = kGpu, .region = *region},
            kTopology);
        REQUIRE(route.has_value());
        REQUIRE(route->AlreadyResident());
        REQUIRE(route->source.allocation.Location() == kGpu);
        REQUIRE(route->estimatedNanoseconds == 0);
        const auto projection =
            Sora::Compute::ProjectCandidateTransferCost(Sora::Test::TestBackendService().Id(), kCandidate,
                                                        std::span<const Sora::Compute::ResidencyRoute>{&*route, 1});
        REQUIRE(projection.has_value());
        REQUIRE(projection->bytes == 0);
        REQUIRE(projection->nanoseconds == 0);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Candidate transfer projection rejects aggregate duration overflow",
          "[Sora][Compute][Memory][Planner][Overflow]") {
    std::atomic<uint32_t> destroyCount = 0;
    Sora::Compute::Buffer buffer = MakeHostBuffer(destroyCount);
    auto snapshot = buffer.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    const auto region = Sora::Compute::ByteRegion::Make(0, 64);
    REQUIRE(region.has_value());
    auto route = Sora::Compute::PlanResidency(
        *snapshot, Sora::Compute::ResidencyRequest{.service = kService, .destination = kGpu, .region = *region},
        kTopology);
    REQUIRE(route.has_value());
    route->estimatedNanoseconds = std::numeric_limits<uint64_t>::max() - 1;
    const std::array routes{*route, *route};

    const auto projection =
        Sora::Compute::ProjectCandidateTransferCost(Sora::Test::TestBackendService().Id(), kCandidate, routes);
    REQUIRE(projection.error() == Sora::Compute::CandidateSynthesisError::InvalidRequest);
}

TEST_CASE("Residency planner breaks equal-cost routes by stable transfer-engine identity",
          "[Sora][Compute][Memory][Planner]") {
    std::atomic<uint32_t> destroyCount = 0;
    Sora::Compute::Buffer buffer = MakeHostBuffer(destroyCount);
    const auto region = Sora::Compute::ByteRegion::Make(0, 64);
    REQUIRE(region.has_value());
    auto snapshot = buffer.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    constexpr std::array equalCostTopology{
        Sora::Compute::TransferLink{.sourceService = kService,
                                    .service = kService,
                                    .engine = {9, 9},
                                    .source = kHost,
                                    .destination = kGpu,
                                    .kind = Sora::Compute::TransferKind::HostBridge,
                                    .bytesPerSecond = 1'000'000'000,
                                    .latencyNanoseconds = 100},
        Sora::Compute::TransferLink{.sourceService = kService,
                                    .service = kService,
                                    .engine = {1, 1},
                                    .source = kHost,
                                    .destination = kGpu,
                                    .kind = Sora::Compute::TransferKind::HostBridge,
                                    .bytesPerSecond = 1'000'000'000,
                                    .latencyNanoseconds = 100},
    };

    auto route = Sora::Compute::PlanResidency(
        *snapshot, Sora::Compute::ResidencyRequest{.service = kService, .destination = kGpu, .region = *region},
        equalCostTopology);
    REQUIRE(route.has_value());
    REQUIRE(route->hops.size() == 1);
    REQUIRE(route->hops.front().engine == Sora::Compute::TransferEngineId{1, 1});
}

TEST_CASE("Residency planner distinguishes missing sources, missing routes, and invalid topology",
          "[Sora][Compute][Memory][Planner][Validation]") {
    const auto region = Sora::Compute::ByteRegion::Make(0, 64);
    REQUIRE(region.has_value());
    const Sora::Compute::BufferResidencySnapshot uninitialized{
        .desc = {.id = {1, 1},
                 .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                 .layout = Sora::Compute::LayoutKind::Contiguous,
                 .rank = 1,
                 .sizeBytes = 64,
                 .alignment = 64},
    };
    const auto noVersion = Sora::Compute::PlanResidency(
        uninitialized, Sora::Compute::ResidencyRequest{.service = kService, .destination = kGpu, .region = *region},
        kTopology);
    REQUIRE_FALSE(noVersion.has_value());
    REQUIRE(noVersion.error() == Sora::Compute::ResidencyPlanningError::UninitializedBuffer);

    std::atomic<uint32_t> destroyCount = 0;
    Sora::Compute::Buffer buffer = MakeHostBuffer(destroyCount);
    auto snapshot = buffer.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    const std::array noRouteTopology{kTopology[2]};
    const auto noRoute = Sora::Compute::PlanResidency(
        *snapshot, Sora::Compute::ResidencyRequest{.service = kService, .destination = kGpu, .region = *region},
        noRouteTopology);
    REQUIRE_FALSE(noRoute.has_value());
    REQUIRE(noRoute.error() == Sora::Compute::ResidencyPlanningError::NoRoute);

    auto invalidLink = kTopology[0];
    invalidLink.engine = {};
    const std::array invalidTopology{invalidLink};
    const auto invalid = Sora::Compute::PlanResidency(
        *snapshot, Sora::Compute::ResidencyRequest{.service = kService, .destination = kGpu, .region = *region},
        invalidTopology);
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error() == Sora::Compute::ResidencyPlanningError::InvalidTopology);
}

TEST_CASE("Residency paths isolate equal location identities by backend service",
          "[Sora][Compute][Memory][Planner][Service]") {
    constexpr Sora::Compute::BackendServiceId otherService{0x534552564943455FULL, 0x54574FULL};
    constexpr std::array origins{Sora::Compute::ResidencyOrigin{kService, kHost},
                                 Sora::Compute::ResidencyOrigin{otherService, kHost}};
    auto otherLink = kTopology[0];
    otherLink.sourceService = otherService;
    otherLink.service = otherService;
    const std::array isolatedTopology{kTopology[0], otherLink};
    const auto region = Sora::Compute::ByteRegion::Make(0, 64);
    REQUIRE(region.has_value());

    const auto path = Sora::Compute::PlanResidencyPath(
        origins, {.service = otherService, .destination = kGpu, .region = *region}, isolatedTopology);
    REQUIRE(path.has_value());
    REQUIRE(path->origin == 1);
    REQUIRE(path->hops.size() == 1);
    REQUIRE(path->hops.front().service == otherService);
}
