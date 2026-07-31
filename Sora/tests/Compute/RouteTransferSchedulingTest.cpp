/**
 * @file RouteTransferSchedulingTest.cpp
 * @brief Exact route transfer timeline tests.
 * @ingroup Compute
 */

#include "PlanningTestSupport.h"

#include <Sora/Compute/Runtime/RouteTransferScheduling.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>

namespace {

    using namespace Sora::Compute;
    using namespace Sora::Test::ComputePlanning;

    [[nodiscard]] GraphResidencyRoute Route(Buffer buffer, const ResidentReplica& source, TransferEngineId engine,
                                            uint64_t bytes, uint64_t latency, uint64_t bandwidth) {
        const auto region = ByteRegion::Make(0, bytes);
        REQUIRE(region.has_value());
        const TransferLink hop{.sourceService = source.allocation.View().service,
                               .service = kServiceB,
                               .engine = engine,
                               .source = source.allocation.Location(),
                               .destination = kDeviceB,
                               .kind = TransferKind::Copy,
                               .bytesPerSecond = bandwidth,
                               .latencyNanoseconds = latency,
                               .maximumTransferBytes = bytes};
        return {.buffer = buffer.Describe().id,
                .source = source,
                .destinationService = kServiceB,
                .destination = kDeviceB,
                .region = *region,
                .version = source.version,
                .estimatedNanoseconds = Detail::TransferCost(hop, bytes),
                .hops = {hop}};
    }

} // namespace

TEST_CASE("Route transfer scheduling preserves fixed latency for every exact shard route",
          "[Sora][Compute][Planner][Transfer][Partition]") {
    std::atomic<uint32_t> destroyCount{0};
    Buffer buffer = MakeBuffer({0xD, 1});
    REQUIRE(buffer.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    const auto snapshot = buffer.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->replicas.size() == 1);
    GraphResidencyRoute first = Route(buffer, snapshot->replicas.front(), {1, 1}, 512, 1'000, 1'000'000'000);
    GraphResidencyRoute second = first;
    const std::array firstRoutes{first};
    const std::array secondRoutes{second};
    const std::array batches{RouteTransferBatch{.operation = 0, .dispatchNanoseconds = 100, .routes = firstRoutes},
                             RouteTransferBatch{.operation = 1, .dispatchNanoseconds = 100, .routes = secondRoutes}};

    const auto schedule = ScheduleRouteTransfers(batches);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->transfers.size() == 2);
    REQUIRE(schedule->readyNanoseconds[0] == 1'512);
    REQUIRE(schedule->readyNanoseconds[1] == 3'024);
    REQUIRE(schedule->routeReadyNanoseconds[0] == std::vector<uint64_t>{1'512});
    REQUIRE(schedule->routeReadyNanoseconds[1] == std::vector<uint64_t>{3'024});
    REQUIRE(schedule->makespanNanoseconds == 3'024);
}

TEST_CASE("Route transfer scheduling composes source readiness with committed engine reservations",
          "[Sora][Compute][Planner][Transfer][Committed]") {
    std::atomic<uint32_t> destroyCount{0};
    Buffer buffer = MakeBuffer({0xD, 4});
    REQUIRE(buffer.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    const auto snapshot = buffer.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    GraphResidencyRoute route = Route(buffer, snapshot->replicas.front(), {1, 1}, 512, 1'000, 1'000'000'000);
    const std::array routes{route};
    constexpr std::array sourceReady{2'000ULL};
    const std::array batches{RouteTransferBatch{
        .operation = 0, .dispatchNanoseconds = 100, .routes = routes, .sourceReadyNanoseconds = sourceReady}};
    constexpr std::array committed{RouteTransferReservation{
        .service = kServiceB, .engine = {1, 1}, .beginNanoseconds = 2'500, .endNanoseconds = 4'000}};

    const auto schedule = ScheduleRouteTransfers(batches, committed);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->transfers.size() == 1);
    REQUIRE(schedule->transfers.front().beginNanoseconds == 4'000);
    REQUIRE(schedule->transfers.front().endNanoseconds == 5'512);
    REQUIRE(schedule->routeReadyNanoseconds[0] == std::vector<uint64_t>{5'512});
    REQUIRE(schedule->readyNanoseconds[0] == 5'512);
}

TEST_CASE("Route transfer scheduling permits independent service-scoped engines to overlap",
          "[Sora][Compute][Planner][Transfer][Contention]") {
    std::atomic<uint32_t> destroyCount{0};
    Buffer buffer = MakeBuffer({0xD, 2});
    REQUIRE(buffer.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    const auto snapshot = buffer.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    GraphResidencyRoute first = Route(buffer, snapshot->replicas.front(), {1, 1}, 512, 1'000, 1'000'000'000);
    GraphResidencyRoute second = Route(buffer, snapshot->replicas.front(), {2, 1}, 512, 1'000, 1'000'000'000);
    const std::array firstRoutes{first};
    const std::array secondRoutes{second};
    const std::array batches{RouteTransferBatch{.operation = 0, .dispatchNanoseconds = 100, .routes = firstRoutes},
                             RouteTransferBatch{.operation = 1, .dispatchNanoseconds = 100, .routes = secondRoutes}};

    const auto schedule = ScheduleRouteTransfers(batches);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->readyNanoseconds[0] == 1'512);
    REQUIRE(schedule->readyNanoseconds[1] == 1'512);
    REQUIRE(schedule->makespanNanoseconds == 1'512);
}

TEST_CASE("Route transfer scheduling is deterministic across equal-tail batch input order",
          "[Sora][Compute][Planner][Transfer][Determinism]") {
    std::atomic<uint32_t> destroyCount{0};
    Buffer buffer = MakeBuffer({0xD, 3});
    REQUIRE(buffer.CommitWholeWrite(MakeAllocation(kDevice, destroyCount)).has_value());
    const auto snapshot = buffer.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    GraphResidencyRoute first = Route(buffer, snapshot->replicas.front(), {1, 1}, 512, 1'000, 1'000'000'000);
    GraphResidencyRoute second = first;
    const std::array firstRoutes{first};
    const std::array secondRoutes{second};
    const std::array batches{RouteTransferBatch{.operation = 1, .dispatchNanoseconds = 100, .routes = secondRoutes},
                             RouteTransferBatch{.operation = 0, .dispatchNanoseconds = 100, .routes = firstRoutes}};

    const auto schedule = ScheduleRouteTransfers(batches);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->transfers.size() == 2);
    REQUIRE(schedule->transfers[0].operation == 0);
    REQUIRE(schedule->transfers[1].operation == 1);
    REQUIRE(schedule->readyNanoseconds[0] == 1'512);
    REQUIRE(schedule->readyNanoseconds[1] == 3'024);
}
