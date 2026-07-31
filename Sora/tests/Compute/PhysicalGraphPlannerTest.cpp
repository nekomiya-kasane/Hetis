#include "BackendTestSupport.h"

#include <Sora/Compute/Runtime/PhysicalGraphPlanner.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace {

    constexpr Sora::Compute::BufferId kBuffer{0xB0, 0x32};
    constexpr Sora::Compute::GraphNodeId kConsumer{0xC026};
    constexpr Sora::Compute::BackendServiceId kService{0x534F52415F544553ULL, 0x545F4241434B454EULL};
    constexpr Sora::Compute::BackendServiceId kPeerService{0x504545525F534552ULL, 0x564943455F303031ULL};
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {1, 1}, .device = {10, 10}, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kSharedCpu{
        .domain = {2, 2}, .device = {10, 10}, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kSharedGpu{
        .domain = {2, 2}, .device = {20, 20}, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::MemoryLocation kDevice{
        .domain = {3, 3}, .device = {20, 20}, .backend = Sora::Compute::BackendKind::Vulkan};

    struct NativeAllocation {
        std::atomic<uint32_t>* destroyCount = nullptr;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        allocation->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    Sora::Compute::Allocation MakeAllocation(Sora::Compute::MemoryLocation location,
                                             std::atomic<uint32_t>& destroyCount) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Sora::Compute::Allocation::Adopt(Sora::Test::TestBackendService(),
                                                           {.location = location,
                                                            .memory = {0xA0, 0x01},
                                                            .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                            .sizeBytes = 4096,
                                                            .alignment = 64,
                                                            .state = native.release(),
                                                            .destroy = DestroyAllocation});
        REQUIRE(allocation.has_value());
        return std::move(*allocation);
    }

    Sora::Compute::Buffer MakeBuffer() {
        auto buffer = Sora::Compute::Buffer::Create({.id = kBuffer,
                                                     .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                                                     .layout = Sora::Compute::LayoutKind::Contiguous,
                                                     .rank = 1,
                                                     .sizeBytes = 4096,
                                                     .alignment = 64});
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Sora::Compute::ResidencyRoute MakeRoute(std::atomic<uint32_t>& destroyCount, Sora::Compute::MemoryLocation source,
                                            Sora::Compute::MemoryLocation destination,
                                            std::span<const Sora::Compute::TransferLink> hops) {
        const auto whole = Sora::Compute::ByteRegion::Make(0, 4096);
        const auto region = Sora::Compute::ByteRegion::Make(128, 64);
        REQUIRE(whole.has_value());
        REQUIRE(region.has_value());
        return {.buffer = kBuffer,
                .source = {.allocation = MakeAllocation(source, destroyCount),
                           .bufferRegion = *whole,
                           .allocationOffset = 0,
                           .version = 7},
                .destinationService = hops.empty() ? kService : hops.back().service,
                .destination = destination,
                .region = *region,
                .version = 7,
                .estimatedNanoseconds = 1234,
                .hops = std::vector<Sora::Compute::TransferLink>{hops.begin(), hops.end()}};
    }

    constexpr Sora::Compute::TransferLink kHostToShared{.sourceService = kService,
                                                        .service = kService,
                                                        .engine = {1, 1},
                                                        .source = kHost,
                                                        .destination = kSharedCpu,
                                                        .kind = Sora::Compute::TransferKind::HostBridge,
                                                        .bytesPerSecond = 1'000'000'000,
                                                        .latencyNanoseconds = 100};
    constexpr Sora::Compute::TransferLink kSharedAlias{.sourceService = kService,
                                                       .service = kService,
                                                       .engine = {2, 2},
                                                       .source = kSharedCpu,
                                                       .destination = kSharedGpu,
                                                       .kind = Sora::Compute::TransferKind::CoherentAlias,
                                                       .latencyNanoseconds = 10};
    constexpr Sora::Compute::TransferLink kSharedToDevice{.sourceService = kService,
                                                          .service = kService,
                                                          .engine = {3, 3},
                                                          .source = kSharedGpu,
                                                          .destination = kDevice,
                                                          .kind = Sora::Compute::TransferKind::Copy,
                                                          .bytesPerSecond = 2'000'000'000,
                                                          .latencyNanoseconds = 50};
    constexpr Sora::Compute::TransferLink kPeerTransfer{.sourceService = kService,
                                                        .service = kPeerService,
                                                        .engine = {4, 4},
                                                        .source = kHost,
                                                        .destination = kDevice,
                                                        .kind = Sora::Compute::TransferKind::Remote,
                                                        .bytesPerSecond = 8'000'000'000,
                                                        .latencyNanoseconds = 80};

} // namespace

TEST_CASE("Physical graph planner preserves ready storage and expands one asynchronous hop",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner]") {
    std::atomic<uint32_t> destroyCount = 0;

    SECTION("already resident") {
        auto fragment =
            Sora::Compute::PlanPhysicalResidency(MakeBuffer(), kConsumer, MakeRoute(destroyCount, kHost, kHost, {}));
        REQUIRE(fragment.has_value());
        REQUIRE(fragment->storage.allocations.empty());
        REQUIRE(fragment->storage.segments.empty());
        REQUIRE(fragment->storage.edges.empty());
        REQUIRE(fragment->transfers.empty());
        REQUIRE(fragment->estimatedNanoseconds == 1234);
        const auto* ready = std::get_if<Sora::Compute::ReadyStorageRef>(&fragment->destination);
        REQUIRE(ready != nullptr);
        REQUIRE(ready->buffer == kBuffer);
        REQUIRE(ready->replica.allocation.Location() == kHost);
        REQUIRE(destroyCount.load(std::memory_order_relaxed) == 0);
    }

    SECTION("single asynchronous hop") {
        constexpr std::array hops{kHostToShared};
        auto fragment = Sora::Compute::PlanPhysicalResidency(MakeBuffer(), kConsumer,
                                                             MakeRoute(destroyCount, kHost, kSharedCpu, hops),
                                                             {.alignment = 64, .publishDestination = true});
        REQUIRE(fragment.has_value());
        REQUIRE(fragment->storage.allocations.size() == 1);
        REQUIRE(fragment->storage.segments.size() == 1);
        REQUIRE(fragment->storage.edges.empty());
        REQUIRE(fragment->transfers == std::vector<Sora::Compute::PhysicalRouteTransfer>{{.hop = 0, .segment = {0}}});

        const auto& allocation = fragment->storage.allocations[0];
        REQUIRE(allocation.id == Sora::Compute::PlannedAllocationId{0});
        REQUIRE(allocation.BufferIdentity() == kBuffer);
        REQUIRE(allocation.location == kSharedCpu);
        REQUIRE(allocation.publication.has_value());
        REQUIRE(allocation.publication->kind == Sora::Compute::PlannedPublicationKind::Replica);
        REQUIRE(allocation.publication->expectedVersion == 7);
        REQUIRE(allocation.publication->producer == Sora::Compute::PhysicalPlanSegmentId{0});
        const auto* materialization = std::get_if<Sora::Compute::PlannedAllocate>(&allocation.materialization);
        REQUIRE(materialization != nullptr);
        REQUIRE(materialization->alignment == 64);

        const auto* transfer = std::get_if<Sora::Compute::PlannedTransfer>(&fragment->storage.segments[0]);
        REQUIRE(transfer != nullptr);
        REQUIRE(transfer->engine == kHostToShared.engine);
        REQUIRE(transfer->destination == allocation.id);
        REQUIRE(std::holds_alternative<Sora::Compute::ReadyStorageRef>(transfer->source));
        const auto destination = std::get<Sora::Compute::PlannedAllocationRef>(fragment->destination);
        REQUIRE(destination.allocation == allocation.id);
        REQUIRE(destination.readyAfter == Sora::Compute::PhysicalPlanSegmentId{0});
        REQUIRE(destroyCount.load(std::memory_order_relaxed) == 0);
    }

    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Physical graph planner inherits readiness through coherent aliases",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner][Alias]") {
    std::atomic<uint32_t> destroyCount = 0;

    SECTION("alias-only route") {
        constexpr std::array hops{kSharedAlias};
        auto fragment = Sora::Compute::PlanPhysicalResidency(MakeBuffer(), kConsumer,
                                                             MakeRoute(destroyCount, kSharedCpu, kSharedGpu, hops),
                                                             {.publishDestination = true});
        REQUIRE(fragment.has_value());
        REQUIRE(fragment->storage.allocations.size() == 1);
        REQUIRE(fragment->storage.segments.empty());
        REQUIRE(fragment->storage.edges.empty());
        REQUIRE(fragment->transfers.empty());

        const auto& allocation = fragment->storage.allocations[0];
        REQUIRE(allocation.BufferIdentity() == kBuffer);
        REQUIRE(allocation.publication.has_value());
        REQUIRE(allocation.publication->kind == Sora::Compute::PlannedPublicationKind::Replica);
        REQUIRE(allocation.publication->expectedVersion == 7);
        REQUIRE_FALSE(allocation.publication->producer.Valid());
        const auto* import = std::get_if<Sora::Compute::PlannedCoherentAlias>(&allocation.materialization);
        REQUIRE(import != nullptr);
        REQUIRE(import->engine == kSharedAlias.engine);
        REQUIRE(std::holds_alternative<Sora::Compute::ReadyStorageRef>(import->source));
        const auto destination = std::get<Sora::Compute::PlannedAllocationRef>(fragment->destination);
        REQUIRE(destination.allocation == allocation.id);
        REQUIRE_FALSE(destination.readyAfter.Valid());
        REQUIRE(destroyCount.load(std::memory_order_relaxed) == 0);
    }

    SECTION("asynchronous alias chain") {
        constexpr std::array hops{kHostToShared, kSharedAlias, kSharedToDevice};
        auto fragment = Sora::Compute::PlanPhysicalResidency(
            MakeBuffer(), kConsumer, MakeRoute(destroyCount, kHost, kDevice, hops), {.publishDestination = true});
        REQUIRE(fragment.has_value());
        REQUIRE(fragment->storage.allocations.size() == 3);
        REQUIRE(fragment->storage.segments.size() == 2);
        REQUIRE(fragment->transfers == std::vector<Sora::Compute::PhysicalRouteTransfer>{{.hop = 0, .segment = {0}},
                                                                                         {.hop = 2, .segment = {1}}});
        REQUIRE(fragment->storage.edges ==
                std::vector<Sora::Compute::PhysicalPlanEdge>{{.predecessor = {0}, .successor = {1}}});

        const auto& first = fragment->storage.allocations[0];
        const auto& alias = fragment->storage.allocations[1];
        const auto& last = fragment->storage.allocations[2];
        REQUIRE_FALSE(first.publication.has_value());
        REQUIRE_FALSE(alias.publication.has_value());
        REQUIRE(last.publication.has_value());
        REQUIRE(last.publication->kind == Sora::Compute::PlannedPublicationKind::Replica);
        REQUIRE(last.publication->expectedVersion == 7);
        REQUIRE(last.publication->producer == Sora::Compute::PhysicalPlanSegmentId{1});

        const auto* import = std::get_if<Sora::Compute::PlannedCoherentAlias>(&alias.materialization);
        REQUIRE(import != nullptr);
        REQUIRE(import->engine == kSharedAlias.engine);
        const auto importState = std::get<Sora::Compute::PlannedAllocationRef>(import->source);
        REQUIRE(importState.allocation == first.id);
        REQUIRE(importState.readyAfter == Sora::Compute::PhysicalPlanSegmentId{0});

        const auto* transfer = std::get_if<Sora::Compute::PlannedTransfer>(&fragment->storage.segments[1]);
        REQUIRE(transfer != nullptr);
        const auto transferState = std::get<Sora::Compute::PlannedAllocationRef>(transfer->source);
        REQUIRE(transferState.allocation == alias.id);
        REQUIRE(transferState.readyAfter == Sora::Compute::PhysicalPlanSegmentId{0});
        REQUIRE(transfer->destination == last.id);
        const auto destination = std::get<Sora::Compute::PlannedAllocationRef>(fragment->destination);
        REQUIRE(destination.allocation == last.id);
        REQUIRE(destination.readyAfter == Sora::Compute::PhysicalPlanSegmentId{1});
        REQUIRE(destroyCount.load(std::memory_order_relaxed) == 0);
    }

    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Physical graph planner preserves a service-changing transfer hop",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner][Service]") {
    std::atomic<uint32_t> destroyCount = 0;
    constexpr std::array hops{kPeerTransfer};
    auto fragment =
        Sora::Compute::PlanPhysicalResidency(MakeBuffer(), kConsumer, MakeRoute(destroyCount, kHost, kDevice, hops));
    REQUIRE(fragment.has_value());
    REQUIRE(fragment->storage.allocations.size() == 1);
    REQUIRE(fragment->storage.allocations[0].service == kPeerService);
    REQUIRE(fragment->storage.allocations[0].location == kDevice);
    const auto* transfer = std::get_if<Sora::Compute::PlannedTransfer>(&fragment->storage.segments[0]);
    REQUIRE(transfer != nullptr);
    REQUIRE(transfer->service == kPeerService);
    REQUIRE(transfer->engine == kPeerTransfer.engine);
}

TEST_CASE("Physical graph planner resolves symbolic produced storage against a global plan prefix",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner][Produced]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer();
    const auto whole = Sora::Compute::ByteRegion::Make(0, 4096);
    const auto region = Sora::Compute::ByteRegion::Make(128, 64);
    REQUIRE(whole.has_value());
    REQUIRE(region.has_value());

    Sora::Compute::PhysicalGraphPlanStorage prefix;
    prefix.allocations.push_back({.id = {0},
                                  .buffer = buffer,
                                  .service = kService,
                                  .location = kHost,
                                  .bufferRegion = *whole,
                                  .version = 7,
                                  .materialization = Sora::Compute::PlannedAllocate{.alignment = 64}});
    prefix.segments.emplace_back(Sora::Compute::PlannedTransfer{});
    const Sora::Compute::GraphLocalResidencySource symbolic{
        .buffer = kBuffer,
        .coverage = *whole,
        .version = 7,
        .producer = Sora::Compute::GraphNodeId{1},
        .replica = {.service = kService, .location = kHost, .materializer = {1}}};
    Sora::Compute::GraphLocalStorageMap produced;
    REQUIRE(produced.Bind({.symbolic = symbolic, .physical = {.allocation = {0}, .readyAfter = {0}}}).has_value());
    REQUIRE(produced.Size() == 1);
    REQUIRE(*produced.Find(symbolic) == Sora::Compute::PlannedAllocationRef{.allocation = {0}, .readyAfter = {0}});
    REQUIRE(produced.Bind({.symbolic = symbolic, .physical = {.allocation = {0}, .readyAfter = {0}}}).error() ==
            Sora::Compute::PhysicalGraphPlanningError::DuplicateGraphLocalBinding);
    REQUIRE(produced.Size() == 1);
    REQUIRE(*produced.Find(symbolic) == Sora::Compute::PlannedAllocationRef{.allocation = {0}, .readyAfter = {0}});
    REQUIRE(produced.Bind({.physical = {.allocation = {1}, .readyAfter = {1}}}).error() ==
            Sora::Compute::PhysicalGraphPlanningError::InvalidGraphLocalBinding);
    REQUIRE(produced.Bind({.symbolic = symbolic}).error() ==
            Sora::Compute::PhysicalGraphPlanningError::InvalidGraphLocalBinding);
    REQUIRE(produced.Size() == 1);

    constexpr std::array hops{kPeerTransfer};
    Sora::Compute::GraphResidencyRoute route{.buffer = kBuffer,
                                             .source = symbolic,
                                             .destinationService = kPeerService,
                                             .destination = kDevice,
                                             .region = *region,
                                             .version = 7,
                                             .estimatedNanoseconds = 1234,
                                             .hops =
                                                 std::vector<Sora::Compute::TransferLink>{hops.begin(), hops.end()}};
    auto fragment = Sora::Compute::PlanPhysicalResidency(buffer, kConsumer, route, prefix, produced, {.alignment = 64});
    REQUIRE(fragment.has_value());
    REQUIRE(fragment->storage.allocations.size() == 1);
    REQUIRE(fragment->storage.allocations[0].id == Sora::Compute::PlannedAllocationId{1});
    REQUIRE(fragment->storage.segments.size() == 1);
    const auto* transfer = std::get_if<Sora::Compute::PlannedTransfer>(&fragment->storage.segments[0]);
    REQUIRE(transfer != nullptr);
    REQUIRE(std::get<Sora::Compute::PlannedAllocationRef>(transfer->source) ==
            Sora::Compute::PlannedAllocationRef{.allocation = {0}, .readyAfter = {0}});
    REQUIRE(transfer->destination == Sora::Compute::PlannedAllocationId{1});
    REQUIRE(fragment->storage.edges ==
            std::vector<Sora::Compute::PhysicalPlanEdge>{{.predecessor = {0}, .successor = {1}}});
    REQUIRE(std::get<Sora::Compute::PlannedAllocationRef>(fragment->destination) ==
            Sora::Compute::PlannedAllocationRef{.allocation = {1}, .readyAfter = {1}});

    auto wrongServicePrefix = prefix;
    wrongServicePrefix.allocations[0].service = kPeerService;
    REQUIRE(Sora::Compute::PlanPhysicalResidency(buffer, kConsumer, route, wrongServicePrefix, produced).error() ==
            Sora::Compute::PhysicalGraphPlanningError::InvalidGraphLocalBinding);

    Sora::Compute::GraphLocalStorageMap invalidProducer;
    REQUIRE(
        invalidProducer.Bind({.symbolic = symbolic, .physical = {.allocation = {0}, .readyAfter = {1}}}).has_value());
    REQUIRE(Sora::Compute::PlanPhysicalResidency(buffer, kConsumer, route, prefix, invalidProducer).error() ==
            Sora::Compute::PhysicalGraphPlanningError::InvalidGraphLocalBinding);

    Sora::Compute::GraphLocalStorageMap missing;
    REQUIRE(Sora::Compute::PlanPhysicalResidency(buffer, kConsumer, std::move(route), prefix, missing).error() ==
            Sora::Compute::PhysicalGraphPlanningError::MissingGraphLocalBinding);
}

TEST_CASE("Produced storage identity distinguishes sparse coverages with the same bounding interval",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner][Produced][Affine]") {
    using namespace Sora::Compute;

    constexpr std::array denseExtents{uint64_t{4}};
    constexpr std::array denseStrides{uint64_t{8}};
    constexpr std::array sparseExtents{uint64_t{2}};
    constexpr std::array sparseStrides{uint64_t{24}};
    constexpr auto dense = AffineByteFootprint::Create(denseExtents, denseStrides, 4, 0, 4, 1);
    constexpr auto sparse = AffineByteFootprint::Create(sparseExtents, sparseStrides, 4, 0, 4, 1);
    static_assert(dense && sparse);
    static_assert(dense->BoundingRegion() == sparse->BoundingRegion());

    const auto source = [](const AffineByteFootprint& footprint) {
        return GraphLocalResidencySource{.buffer = kBuffer,
                                         .coverage = footprint.BoundingRegion(),
                                         .byteFootprint = footprint,
                                         .version = 7,
                                         .producer = GraphNodeId{1},
                                         .replica = {.service = kService, .location = kHost, .materializer = {1}}};
    };
    const GraphLocalResidencySource denseSource = source(*dense);
    const GraphLocalResidencySource sparseSource = source(*sparse);
    GraphLocalStorageMap produced;
    REQUIRE(produced.Bind({.symbolic = denseSource, .physical = {.allocation = {0}, .readyAfter = {0}}}).has_value());
    REQUIRE(produced.Bind({.symbolic = sparseSource, .physical = {.allocation = {1}, .readyAfter = {1}}}).has_value());
    REQUIRE(produced.Size() == 2);
    REQUIRE(*produced.Find(denseSource) == PlannedAllocationRef{.allocation = {0}, .readyAfter = {0}});
    REQUIRE(*produced.Find(sparseSource) == PlannedAllocationRef{.allocation = {1}, .readyAfter = {1}});
}

TEST_CASE("Produced storage resolves exact affine parts retained by one prior input assembly",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner][Produced][Assembly]") {
    using namespace Sora::Compute;

    constexpr std::array extents{uint64_t{8}};
    constexpr std::array halfExtents{uint64_t{4}};
    constexpr std::array strides{uint64_t{8}};
    constexpr auto whole = AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    constexpr auto first = AffineByteFootprint::Create(halfExtents, strides, 4, 0, 4, 1);
    constexpr auto second = AffineByteFootprint::Create(halfExtents, strides, 4, 32, 4, 1);
    static_assert(whole && first && second);

    auto buffer = MakeBuffer();
    PhysicalGraphPlanStorage prefix;
    prefix.allocations.push_back({.id = {0},
                                  .purpose = PlannedAllocationPurpose::DispatchInputAssembly,
                                  .buffer = buffer,
                                  .service = kPeerService,
                                  .location = kDevice,
                                  .bufferRegion = whole->BoundingRegion(),
                                  .byteFootprint = *whole,
                                  .version = 0,
                                  .materialization = PlannedAllocate{.alignment = 64}});
    const auto transfer = [&](const AffineByteFootprint& footprint) {
        return PlannedTransfer{.logicalNode = kConsumer,
                               .service = kPeerService,
                               .engine = kPeerTransfer.engine,
                               .buffer = kBuffer,
                               .region = footprint.BoundingRegion(),
                               .byteFootprint = footprint,
                               .version = 7,
                               .destination = {0},
                               .target = PlannedTransferTarget::AssemblyPart};
    };
    prefix.segments.emplace_back(transfer(*first));
    prefix.segments.emplace_back(transfer(*second));
    prefix.segments.emplace_back(PlannedAssembly{
        .logicalNode = kConsumer,
        .destination = {0},
        .parts = {{.transferProducer = {0}, .region = first->BoundingRegion(), .byteFootprint = *first},
                  {.transferProducer = {1}, .region = second->BoundingRegion(), .byteFootprint = *second}}});
    prefix.edges = {{.predecessor = {0}, .successor = {2}}, {.predecessor = {1}, .successor = {2}}};

    const auto symbolic = [&](const AffineByteFootprint& footprint) {
        return GraphLocalResidencySource{
            .buffer = kBuffer,
            .coverage = footprint.BoundingRegion(),
            .byteFootprint = footprint,
            .version = 7,
            .producer = GraphNodeId{1},
            .replica = {.service = kPeerService, .location = kDevice, .materializer = kConsumer}};
    };
    const GraphLocalResidencySource firstSource = symbolic(*first);
    const GraphLocalResidencySource secondSource = symbolic(*second);
    GraphLocalStorageMap produced;
    const PlannedAllocationRef assemblyRef{.allocation = {0}, .readyAfter = {2}};
    REQUIRE(produced.Bind({.symbolic = firstSource, .physical = assemblyRef}).has_value());
    REQUIRE(produced.Bind({.symbolic = secondSource, .physical = assemblyRef}).has_value());

    const auto route = [](GraphLocalResidencySource source) {
        return GraphResidencyRoute{.buffer = source.buffer,
                                   .source = source,
                                   .destinationService = source.replica.service,
                                   .destination = source.replica.location,
                                   .region = source.coverage,
                                   .byteFootprint = source.byteFootprint,
                                   .version = source.version};
    };
    const auto firstResolved = PlanPhysicalResidency(buffer, {10}, route(firstSource), prefix, produced);
    const auto secondResolved = PlanPhysicalResidency(buffer, {10}, route(secondSource), prefix, produced);
    REQUIRE(firstResolved.has_value());
    REQUIRE(secondResolved.has_value());
    REQUIRE(firstResolved->storage.allocations.empty());
    REQUIRE(secondResolved->storage.allocations.empty());
    REQUIRE(std::get<PlannedAllocationRef>(firstResolved->destination) == assemblyRef);
    REQUIRE(std::get<PlannedAllocationRef>(secondResolved->destination) == assemblyRef);

    auto missingEdge = prefix;
    missingEdge.edges.erase(missingEdge.edges.begin());
    REQUIRE(PlanPhysicalResidency(buffer, {10}, route(firstSource), missingEdge, produced).error() ==
            PhysicalGraphPlanningError::InvalidGraphLocalBinding);

    auto wrongVersion = prefix;
    std::get<PlannedTransfer>(wrongVersion.segments[0]).version = 8;
    REQUIRE(PlanPhysicalResidency(buffer, {10}, route(firstSource), wrongVersion, produced).error() ==
            PhysicalGraphPlanningError::InvalidGraphLocalBinding);

    auto wrongProducer = prefix;
    std::get<PlannedAssembly>(wrongProducer.segments[2]).logicalNode = {11};
    REQUIRE(PlanPhysicalResidency(buffer, {10}, route(firstSource), wrongProducer, produced).error() ==
            PhysicalGraphPlanningError::InvalidGraphLocalBinding);
}

TEST_CASE("Physical graph planner redirects only the final asynchronous hop into an assembly part",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner][Prefill]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto buffer = MakeBuffer();
    const auto region = Sora::Compute::ByteRegion::Make(128, 64);
    REQUIRE(region.has_value());
    constexpr std::array hops{kPeerTransfer};
    auto selected = MakeRoute(destroyCount, kHost, kDevice, hops);
    Sora::Compute::GraphResidencyRoute route{.buffer = selected.buffer,
                                             .source = std::move(selected.source),
                                             .destinationService = selected.destinationService,
                                             .destination = selected.destination,
                                             .region = selected.region,
                                             .version = selected.version,
                                             .estimatedNanoseconds = selected.estimatedNanoseconds,
                                             .hops = std::move(selected.hops)};
    Sora::Compute::PhysicalGraphPlanStorage prefix;
    prefix.allocations.push_back({.id = {0},
                                  .purpose = Sora::Compute::PlannedAllocationPurpose::DispatchInputAssembly,
                                  .buffer = buffer,
                                  .service = kPeerService,
                                  .location = kDevice,
                                  .bufferRegion = *region,
                                  .version = 0,
                                  .materialization = Sora::Compute::PlannedAllocate{.alignment = 64}});

    auto fragment = Sora::Compute::PlanPhysicalResidencyPrefill(buffer, kConsumer, std::move(route), prefix, {}, {0},
                                                                {.alignment = 64});
    REQUIRE(fragment.has_value());
    REQUIRE(fragment->storage.allocations.empty());
    REQUIRE(fragment->storage.segments.size() == 1);
    REQUIRE(fragment->producer == Sora::Compute::PhysicalPlanSegmentId{0});
    REQUIRE(fragment->transfers == std::vector<Sora::Compute::PhysicalRouteTransfer>{{.hop = 0, .segment = {0}}});
    const auto* transfer = std::get_if<Sora::Compute::PlannedTransfer>(&fragment->storage.segments.front());
    REQUIRE(transfer != nullptr);
    REQUIRE(transfer->destination == Sora::Compute::PlannedAllocationId{0});
    REQUIRE(transfer->target == Sora::Compute::PlannedTransferTarget::AssemblyPart);
    REQUIRE(transfer->Coverage() == Sora::Compute::ByteCoverageView::Interval(*region));
}

TEST_CASE("Physical graph planner rejects disconnected or impossible selected routes",
          "[Sora][Compute][Runtime][PhysicalGraphPlanner][Validation]") {
    std::atomic<uint32_t> destroyCount = 0;

    SECTION("disconnected hop") {
        auto disconnected = kSharedToDevice;
        disconnected.source = kSharedCpu;
        const std::array hops{disconnected};
        const auto fragment = Sora::Compute::PlanPhysicalResidency(MakeBuffer(), kConsumer,
                                                                   MakeRoute(destroyCount, kHost, kDevice, hops));
        REQUIRE_FALSE(fragment.has_value());
        REQUIRE(fragment.error() == Sora::Compute::PhysicalGraphPlanningError::InvalidRoute);
    }

    SECTION("hop transfer limit") {
        auto limited = kHostToShared;
        limited.maximumTransferBytes = 63;
        const std::array hops{limited};
        const auto fragment = Sora::Compute::PlanPhysicalResidency(MakeBuffer(), kConsumer,
                                                                   MakeRoute(destroyCount, kHost, kSharedCpu, hops));
        REQUIRE_FALSE(fragment.has_value());
        REQUIRE(fragment.error() == Sora::Compute::PhysicalGraphPlanningError::InvalidRoute);
    }

    SECTION("invalid materialization alignment") {
        constexpr std::array hops{kHostToShared};
        const auto fragment = Sora::Compute::PlanPhysicalResidency(
            MakeBuffer(), kConsumer, MakeRoute(destroyCount, kHost, kSharedCpu, hops), {.alignment = 3});
        REQUIRE_FALSE(fragment.has_value());
        REQUIRE(fragment.error() == Sora::Compute::PhysicalGraphPlanningError::InvalidRoute);
    }

    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}
