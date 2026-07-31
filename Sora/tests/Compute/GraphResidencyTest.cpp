#include "PlanningTestSupport.h"

#include <Sora/Compute/Runtime/GraphResidency.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

namespace {

    constexpr Sora::Compute::BackendServiceId kService{1, 1};
    constexpr Sora::Compute::MemoryLocation kGpuA{
        .domain = {2, 1}, .device = {3, 1}, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::MemoryLocation kGpuB{
        .domain = {2, 2}, .device = {3, 2}, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::BufferDesc kBuffer{.id = {4, 1},
                                                .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                                                .layout = Sora::Compute::LayoutKind::Contiguous,
                                                .rank = 1,
                                                .sizeBytes = 1024,
                                                .alignment = 64};
    constexpr std::array kAffineExtents{uint64_t{4}, uint64_t{6}};
    constexpr std::array kAffineStrides{uint64_t{64}, uint64_t{8}};
    constexpr std::array kAffineHalfExtents{uint64_t{4}, uint64_t{3}};
    constexpr auto kAffineDomain =
        Sora::Compute::AffineByteFootprint::Create(kAffineExtents, kAffineStrides, sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineFirst = Sora::Compute::AffineByteFootprint::Create(kAffineHalfExtents, kAffineStrides,
                                                                             sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineSecond = Sora::Compute::AffineByteFootprint::Create(kAffineHalfExtents, kAffineStrides,
                                                                              sizeof(float), 24, sizeof(float), 1);
    constexpr auto kAffineOverlap = Sora::Compute::AffineByteFootprint::Create(kAffineHalfExtents, kAffineStrides,
                                                                               sizeof(float), 16, sizeof(float), 1);
    static_assert(kAffineDomain && kAffineFirst && kAffineSecond && kAffineOverlap);

    [[nodiscard]] Sora::Compute::ProducedResidencyWrite Write(uint64_t offset, uint64_t size, uint32_t producer) {
        return {.buffer = kBuffer.id,
                .region = *Sora::Compute::ByteRegion::Make(offset, size),
                .version = 8,
                .producer = {producer},
                .replica = {.service = kService, .location = kGpuA, .materializer = {producer}}};
    }

    [[nodiscard]] Sora::Compute::ProducedResidencyWrite AffineWrite(const Sora::Compute::AffineByteFootprint& footprint,
                                                                    uint32_t producer,
                                                                    Sora::Compute::MemoryLocation location = kGpuA) {
        return {.buffer = {4, 2},
                .region = footprint.BoundingRegion(),
                .byteFootprint = footprint,
                .version = 1,
                .producer = {producer},
                .replica = {.service = kService, .location = location, .materializer = {producer}}};
    }

    [[nodiscard]] Sora::Compute::Buffer MakeAffineBuffer(Sora::Compute::BufferId id = {4, 2}) {
        auto buffer = Sora::Compute::Buffer::Create({.id = id,
                                                     .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                                                     .layout = Sora::Compute::LayoutKind::Strided,
                                                     .rank = 2,
                                                     .sizeBytes = kAffineDomain->BoundingRegion().End(),
                                                     .alignment = 64},
                                                    *kAffineDomain);
        REQUIRE(buffer.has_value());
        return *buffer;
    }

} // namespace

TEST_CASE("Graph residency splits overwritten intervals and preserves unaffected latest producers",
          "[Sora][Compute][Runtime][GraphResidency]") {
    Sora::Compute::GraphResidencyState state;
    REQUIRE(state.Track(kBuffer, 7).has_value());
    REQUIRE(state.Apply({.writes = {Write(0, 1024, 0)}}).has_value());
    REQUIRE(state.Apply({.writes = {Write(256, 256, 1)}}).has_value());

    const auto segments = state.Segments(kBuffer.id);
    REQUIRE(segments.size() == 3);
    REQUIRE(segments[0].region == *Sora::Compute::ByteRegion::Make(0, 256));
    REQUIRE(segments[0].producer == Sora::Compute::GraphNodeId{0});
    REQUIRE(segments[1].region == *Sora::Compute::ByteRegion::Make(256, 256));
    REQUIRE(segments[1].producer == Sora::Compute::GraphNodeId{1});
    REQUIRE(segments[2].region == *Sora::Compute::ByteRegion::Make(512, 512));
    REQUIRE(segments[2].producer == Sora::Compute::GraphNodeId{0});
}

TEST_CASE("Graph residency splits a partial replica addition and reuses the symbolic location",
          "[Sora][Compute][Runtime][GraphResidency][Replica]") {
    Sora::Compute::GraphResidencyState state;
    REQUIRE(state.Track(kBuffer, 7).has_value());
    REQUIRE(state.Apply({.writes = {Write(0, 1024, 0)}}).has_value());
    const Sora::Compute::GraphResidencyReplicaAddition addition{
        .buffer = kBuffer.id,
        .region = *Sora::Compute::ByteRegion::Make(128, 256),
        .version = 8,
        .producer = Sora::Compute::GraphNodeId{0},
        .replica = {.service = kService, .location = kGpuB, .materializer = {1}}};
    REQUIRE(state.Apply({.replicas = {addition}}).has_value());

    const auto segments = state.Segments(kBuffer.id);
    REQUIRE(segments.size() == 3);
    REQUIRE(segments[0].replicas.size() == 1);
    REQUIRE(segments[1].region == addition.region);
    REQUIRE(segments[1].replicas.size() == 2);
    REQUIRE(segments[1].replicas[1].location == kGpuB);
    REQUIRE(segments[2].replicas.size() == 1);
}

TEST_CASE("Graph residency tracks baseline materializations until exact write coverage supersedes them",
          "[Sora][Compute][Runtime][GraphResidency][Replica][Baseline]") {
    using namespace Sora::Compute;

    GraphResidencyState state;
    REQUIRE(state.Track(kBuffer, 7).has_value());
    const GraphResidencyReplicaAddition baseline{
        .buffer = kBuffer.id,
        .region = *ByteRegion::Make(0, 1024),
        .version = 7,
        .producer = std::nullopt,
        .replica = {.service = kService, .location = kGpuB, .materializer = {0}}};
    REQUIRE(state.Apply({.replicas = {baseline}}).has_value());
    REQUIRE(state.Segments(kBuffer.id).size() == 1);
    REQUIRE_FALSE(state.Segments(kBuffer.id).front().producer.has_value());

    auto stale = baseline;
    stale.version = 8;
    REQUIRE(state.Apply({.replicas = {stale}}).error() == GraphResidencyError::MissingLogicalValue);
    REQUIRE(state.Segments(kBuffer.id).size() == 1);

    REQUIRE(state.Apply({.writes = {Write(256, 256, 1)}}).has_value());
    const auto segments = state.Segments(kBuffer.id);
    REQUIRE(segments.size() == 3);
    REQUIRE_FALSE(segments[0].producer.has_value());
    REQUIRE(segments[1].producer == GraphNodeId{1});
    REQUIRE_FALSE(segments[2].producer.has_value());

    state.DiscardProducedBefore({2});
    REQUIRE(state.Segments(kBuffer.id).size() == 2);
    REQUIRE(std::ranges::none_of(state.Segments(kBuffer.id),
                                 [](const GraphResidencySegment& segment) { return segment.producer.has_value(); }));
}

TEST_CASE("Graph residency rejects invalid batches transactionally",
          "[Sora][Compute][Runtime][GraphResidency][Transaction]") {
    Sora::Compute::GraphResidencyState state;
    REQUIRE(state.Track(kBuffer, 7).has_value());
    REQUIRE(state.Apply({.writes = {Write(0, 1024, 0)}}).has_value());
    const auto before = state.Segments(kBuffer.id);
    REQUIRE(before.size() == 1);
    const auto original = before.front();

    auto first = Write(0, 512, 1);
    auto second = Write(256, 512, 1);
    const auto rejected = state.Apply({.writes = {first, second}});
    REQUIRE(rejected.error() == Sora::Compute::GraphResidencyError::OverlappingWrites);
    REQUIRE(state.Segments(kBuffer.id).size() == 1);
    REQUIRE(state.Segments(kBuffer.id).front().region == original.region);
    REQUIRE(state.Segments(kBuffer.id).front().producer == original.producer);

    auto missing = Sora::Compute::GraphResidencyReplicaAddition{
        .buffer = kBuffer.id,
        .region = *Sora::Compute::ByteRegion::Make(0, 1024),
        .version = 8,
        .producer = Sora::Compute::GraphNodeId{9},
        .replica = {.service = kService, .location = kGpuB, .materializer = {2}}};
    REQUIRE(state.Apply({.replicas = {missing}}).error() == Sora::Compute::GraphResidencyError::MissingLogicalValue);
    REQUIRE(state.Segments(kBuffer.id).front().producer == original.producer);
}

TEST_CASE("Consuming graph residency transition equals strong-guarantee application",
          "[Sora][Compute][Runtime][GraphResidency][Transaction][Consuming]") {
    using namespace Sora::Compute;

    GraphResidencyState baseline;
    REQUIRE(baseline.Track(kBuffer, 7).has_value());
    GraphResidencyState strong = baseline;
    GraphResidencyState consumed = baseline;
    const GraphResidencyUpdate update{.writes = {Write(0, 1024, 0)}};

    REQUIRE(strong.Apply(update).has_value());
    auto transitioned = std::move(consumed).Transition(update);
    REQUIRE(transitioned.has_value());
    REQUIRE(strong.SamePlanningState(*transitioned));
}

TEST_CASE("Graph residency discards only produced values older than the live "
          "frontier",
          "[Sora][Compute][Runtime][GraphResidency][Liveness]") {
    Sora::Compute::GraphResidencyState state;
    REQUIRE(state.Track(kBuffer, 7).has_value());
    REQUIRE(state.Apply({.writes = {Write(0, 1024, 0)}}).has_value());
    REQUIRE(state.Apply({.writes = {Write(256, 256, 2)}}).has_value());

    state.DiscardProducedBefore({2});
    const auto live = state.Segments(kBuffer.id);
    REQUIRE(live.size() == 1);
    REQUIRE(live.front().region == *Sora::Compute::ByteRegion::Make(256, 256));
    REQUIRE(live.front().producer == Sora::Compute::GraphNodeId{2});

    state.DiscardProducedBefore({3});
    REQUIRE(state.Segments(kBuffer.id).empty());
}

TEST_CASE("Graph residency discards overlays exactly after their final logical reader",
          "[Sora][Compute][Runtime][GraphResidency][Liveness][Read]") {
    using namespace Sora::Compute;

    constexpr BufferDesc secondBuffer{.id = {4, 5},
                                      .elementType = {ScalarKind::Float32, 1},
                                      .layout = LayoutKind::Contiguous,
                                      .rank = 1,
                                      .sizeBytes = 1024,
                                      .alignment = 64};
    GraphResidencyState state;
    REQUIRE(state.Track(kBuffer, 7).has_value());
    REQUIRE(state.Track(secondBuffer, 3).has_value());
    const GraphResidencyReplicaAddition first{.buffer = kBuffer.id,
                                              .region = *ByteRegion::Make(0, 1024),
                                              .version = 7,
                                              .producer = std::nullopt,
                                              .replica = {.service = kService, .location = kGpuB, .materializer = {0}}};
    auto second = first;
    second.buffer = secondBuffer.id;
    second.version = 3;
    REQUIRE(state.Apply({.replicas = {first, second}}).has_value());

    constexpr std::array lifetimes{GraphBufferReadLifetime{.buffer = kBuffer.id, .lastReader = {2}},
                                   GraphBufferReadLifetime{.buffer = secondBuffer.id, .lastReader = {1}}};
    state.DiscardReadDeadBuffers({1}, lifetimes);
    REQUIRE(state.Segments(kBuffer.id).size() == 1);
    REQUIRE(state.Segments(secondBuffer.id).empty());

    state.DiscardReadDeadBuffers({2}, lifetimes);
    REQUIRE(state.Segments(kBuffer.id).empty());
}

TEST_CASE("Copied graph residency shares its frozen baseline and isolates "
          "produced mutations",
          "[Sora][Compute][Runtime][GraphResidency][CopyOnWrite]") {
    Sora::Compute::GraphResidencyState original;
    REQUIRE(original.Track(kBuffer, 7).has_value());
    REQUIRE(original.Apply({.writes = {Write(0, 1024, 0)}}).has_value());

    Sora::Compute::GraphResidencyState copy = original;
    REQUIRE(original.ReadySnapshot(kBuffer.id) == copy.ReadySnapshot(kBuffer.id));
    REQUIRE(copy.Apply({.writes = {Write(256, 256, 1)}}).has_value());
    REQUIRE(original.Segments(kBuffer.id).size() == 1);
    REQUIRE(original.Segments(kBuffer.id).front().producer == Sora::Compute::GraphNodeId{0});
    REQUIRE(copy.Segments(kBuffer.id).size() == 3);
    REQUIRE(copy.Segments(kBuffer.id)[1].producer == Sora::Compute::GraphNodeId{1});

    REQUIRE(original.Apply({.writes = {Write(512, 256, 2)}}).has_value());
    REQUIRE(original.Segments(kBuffer.id)[1].producer == Sora::Compute::GraphNodeId{2});
    REQUIRE(copy.Segments(kBuffer.id)[2].producer == Sora::Compute::GraphNodeId{0});
}

TEST_CASE("Planning-state equivalence requires one frozen baseline and identical canonical overlays",
          "[Sora][Compute][Runtime][GraphResidency][PlanningState]") {
    using namespace Sora::Compute;

    GraphResidencyState unfrozen;
    REQUIRE_FALSE(unfrozen.SamePlanningState(GraphResidencyState{}));

    GraphResidencyState baseline;
    REQUIRE(baseline.Track(kBuffer, 7).has_value());
    GraphResidencyState equal = baseline;
    REQUIRE(baseline.SamePlanningState(equal));
    REQUIRE(equal.SamePlanningState(baseline));

    GraphResidencyState withReplica = baseline;
    const GraphResidencyReplicaAddition replica{
        .buffer = kBuffer.id,
        .region = *ByteRegion::Make(0, 1024),
        .version = 7,
        .producer = std::nullopt,
        .replica = {.service = kService, .location = kGpuB, .materializer = {0}}};
    REQUIRE(withReplica.Apply({.replicas = {replica}}).has_value());
    REQUIRE_FALSE(baseline.SamePlanningState(withReplica));

    GraphResidencyState withWrite = baseline;
    REQUIRE(withWrite.Apply({.writes = {Write(0, 1024, 0)}}).has_value());
    REQUIRE_FALSE(baseline.SamePlanningState(withWrite));

    GraphResidencyState independentlyTracked;
    REQUIRE(independentlyTracked.Track(kBuffer, 7).has_value());
    REQUIRE_FALSE(baseline.SamePlanningState(independentlyTracked));
}

TEST_CASE("Graph residency baseline remains frozen when the live buffer gains "
          "a replica",
          "[Sora][Compute][Runtime][GraphResidency][Snapshot]") {
    using namespace Sora::Compute;
    using namespace Sora::Test::ComputePlanning;

    std::atomic<uint32_t> destroyCount = 0;
    {
        Buffer buffer = MakeBuffer({4, 2});
        REQUIRE(buffer.CommitWholeWrite(MakeAllocation(kHost, destroyCount)).has_value());
        GraphResidencyState state;
        REQUIRE(state.Track(buffer).has_value());
        const BufferResidencySnapshot* frozen = state.ReadySnapshot(buffer.Describe().id);
        REQUIRE(frozen != nullptr);
        REQUIRE(frozen->replicas.size() == 1);

        REQUIRE(buffer.PublishReplica(1, *ByteRegion::Make(0, kBufferBytes), MakeAllocation(kDevice, destroyCount))
                    .has_value());
        const auto live = buffer.SnapshotResidency();
        REQUIRE(live.has_value());
        REQUIRE(live->replicas.size() == 2);
        REQUIRE(state.ReadySnapshot(buffer.Describe().id) == frozen);
        REQUIRE(frozen->replicas.size() == 1);
        REQUIRE(frozen->replicas.front().allocation.Location() == kHost);
    }
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Graph residency preserves exact affine partitions whose bounding intervals overlap",
          "[Sora][Compute][Runtime][GraphResidency][Affine]") {
    using namespace Sora::Compute;

    Buffer buffer = MakeAffineBuffer();

    GraphResidencyState state;
    REQUIRE(state.Track(buffer).has_value());
    const std::array parts{AffineWrite(*kAffineFirst, 0), AffineWrite(*kAffineSecond, 0, kGpuB)};
    REQUIRE(state.Apply({.writes = {parts.begin(), parts.end()}}).has_value());
    const auto segments = state.Segments(buffer.Describe().id);
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].Coverage() == ByteCoverageView::Affine(*kAffineFirst));
    REQUIRE(segments[1].Coverage() == ByteCoverageView::Affine(*kAffineSecond));
    REQUIRE(segments[0].region.Intersects(segments[1].region));

    const GraphResidencyReplicaAddition addition{
        .buffer = buffer.Describe().id,
        .region = kAffineFirst->BoundingRegion(),
        .byteFootprint = *kAffineFirst,
        .version = 1,
        .producer = GraphNodeId{0},
        .replica = {.service = kService, .location = kGpuB, .materializer = {1}}};
    REQUIRE(state.Apply({.replicas = {addition}}).has_value());
    REQUIRE(state.Segments(buffer.Describe().id).front().replicas.size() == 2);

    const auto rejected = state.Apply({.writes = {AffineWrite(*kAffineOverlap, 1)}});
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == GraphResidencyError::IndeterminateCoverage);
    REQUIRE(state.Segments(buffer.Describe().id).size() == 2);

    GraphResidencyState overlap;
    REQUIRE(overlap.Track(buffer).has_value());
    const auto overlappingBatch =
        overlap.Apply({.writes = {AffineWrite(*kAffineFirst, 0), AffineWrite(*kAffineOverlap, 0)}});
    REQUIRE_FALSE(overlappingBatch.has_value());
    REQUIRE(overlappingBatch.error() == GraphResidencyError::OverlappingWrites);
    REQUIRE(overlap.Segments(buffer.Describe().id).empty());
}

TEST_CASE("Graph residency splits a contained affine replica and proves an exact disjoint partition",
          "[Sora][Compute][Runtime][GraphResidency][Affine][Replica]") {
    using namespace Sora::Compute;

    Buffer buffer = MakeAffineBuffer({4, 3});
    GraphResidencyState state;
    REQUIRE(state.Track(buffer).has_value());
    auto whole = AffineWrite(*kAffineDomain, 0);
    whole.buffer = buffer.Describe().id;
    REQUIRE(state.Apply({.writes = {whole}}).has_value());

    const GraphResidencyReplicaAddition addition{
        .buffer = buffer.Describe().id,
        .region = kAffineOverlap->BoundingRegion(),
        .byteFootprint = *kAffineOverlap,
        .version = 1,
        .producer = GraphNodeId{0},
        .replica = {.service = kService, .location = kGpuB, .materializer = {1}}};
    REQUIRE(state.Apply({.replicas = {addition}}).has_value());

    const auto segments = state.Segments(buffer.Describe().id);
    REQUIRE(segments.size() == 3);
    std::array<ByteCoverageView, 3> coverages{};
    bool foundRequested = false;
    for (size_t index = 0; index < segments.size(); ++index) {
        const GraphResidencySegment& segment = segments[index];
        coverages[index] = segment.Coverage();
        REQUIRE(segment.buffer == buffer.Describe().id);
        REQUIRE(segment.version == 1);
        REQUIRE(segment.producer == GraphNodeId{0});
        if (segment.Coverage() == addition.Coverage()) {
            foundRequested = true;
            REQUIRE(segment.replicas.size() == 2);
            REQUIRE(std::ranges::find(segment.replicas, addition.replica) != segment.replicas.end());
        } else {
            REQUIRE(segment.replicas == std::vector{whole.replica});
        }
    }
    REQUIRE(foundRequested);
    REQUIRE(ExactCoverageUnion<256>(ByteCoverageView::Affine(*kAffineDomain), coverages) == CoverageEquivalence::Equal);
    for (size_t index = 0; index < coverages.size(); ++index) {
        for (size_t prior = 0; prior < index; ++prior) {
            REQUIRE(CoverageRelationWithin(ByteCoverageView::Affine(*kAffineDomain), coverages[prior],
                                           coverages[index]) == CoverageOverlap::Disjoint);
        }
    }
}

TEST_CASE("Graph residency rejects affine subtraction from a non-affine produced value transactionally",
          "[Sora][Compute][Runtime][GraphResidency][Affine][Transaction]") {
    using namespace Sora::Compute;

    GraphResidencyState state;
    REQUIRE(state.Track(kBuffer, 7).has_value());
    REQUIRE(state.Apply({.writes = {Write(0, 1024, 0)}}).has_value());
    const std::vector before(state.Segments(kBuffer.id).begin(), state.Segments(kBuffer.id).end());
    constexpr std::array extents{uint64_t{4}};
    constexpr std::array strides{uint64_t{8}};
    constexpr auto sparse = AffineByteFootprint::Create(extents, strides, sizeof(float), 0, sizeof(float), 1);
    static_assert(sparse);
    const GraphResidencyReplicaAddition addition{
        .buffer = kBuffer.id,
        .region = sparse->BoundingRegion(),
        .byteFootprint = *sparse,
        .version = 8,
        .producer = GraphNodeId{0},
        .replica = {.service = kService, .location = kGpuB, .materializer = {1}}};

    const auto rejected = state.Apply({.replicas = {addition}});
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == GraphResidencyError::IndeterminateCoverage);
    REQUIRE(std::ranges::equal(state.Segments(kBuffer.id), before));
}

TEST_CASE("Graph residency reports affine split capacity overflow transactionally",
          "[Sora][Compute][Runtime][GraphResidency][Affine][Capacity]") {
    using namespace Sora::Compute;

    constexpr size_t segmentCount = 256;
    constexpr std::array domainExtents{uint64_t{segmentCount * 2}};
    constexpr std::array strides{uint64_t{8}};
    constexpr auto domain = AffineByteFootprint::Create(domainExtents, strides, sizeof(float), 0, sizeof(float), 1);
    static_assert(domain);
    auto buffer = Buffer::Create({.id = {4, 4},
                                  .elementType = {ScalarKind::Float32, 1},
                                  .layout = LayoutKind::Strided,
                                  .rank = 1,
                                  .sizeBytes = domain->BoundingRegion().End(),
                                  .alignment = 64},
                                 *domain);
    REQUIRE(buffer.has_value());

    GraphResidencyState state;
    REQUIRE(state.Track(*buffer).has_value());
    std::vector<ProducedResidencyWrite> writes;
    writes.reserve(segmentCount);
    constexpr std::array fragmentExtents{uint64_t{2}};
    for (size_t index = 0; index < segmentCount; ++index) {
        const auto footprint =
            AffineByteFootprint::Create(fragmentExtents, strides, sizeof(float), index * 16, sizeof(float), 1);
        REQUIRE(footprint.has_value());
        writes.push_back({.buffer = buffer->Describe().id,
                          .region = footprint->BoundingRegion(),
                          .byteFootprint = *footprint,
                          .version = 1,
                          .producer = {0},
                          .replica = {.service = kService, .location = kGpuA, .materializer = {0}}});
    }
    REQUIRE(state.Apply({.writes = writes}).has_value());
    const std::vector before(state.Segments(buffer->Describe().id).begin(),
                             state.Segments(buffer->Describe().id).end());
    REQUIRE(before.size() == segmentCount);

    constexpr std::array requestedExtents{uint64_t{1}};
    constexpr auto requested =
        AffineByteFootprint::Create(requestedExtents, strides, sizeof(float), 0, sizeof(float), 1);
    static_assert(requested);
    const GraphResidencyReplicaAddition addition{
        .buffer = buffer->Describe().id,
        .region = requested->BoundingRegion(),
        .byteFootprint = *requested,
        .version = 1,
        .producer = GraphNodeId{0},
        .replica = {.service = kService, .location = kGpuB, .materializer = {1}}};
    const auto rejected = state.Apply({.replicas = {addition}});
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == GraphResidencyError::CapacityExceeded);
    REQUIRE(std::ranges::equal(state.Segments(buffer->Describe().id), before));
}
