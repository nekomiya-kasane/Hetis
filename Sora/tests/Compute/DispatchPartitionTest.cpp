#include <Sora/Compute/Invocation/DispatchGeometry.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

TEST_CASE("ElementwisePartitionPlan constructs an ordered strict cover", "[Sora][Compute][Partition]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 3, .extents = {10, 4, 2}, .workItemCount = 80};
    const auto plan = Sora::Compute::ElementwisePartitionPlan::Balanced(
        domain, Sora::Compute::PartitionContract::Elementwise(3), 0, 3);
    REQUIRE(plan.has_value());
    REQUIRE(plan->Axis() == 0);
    REQUIRE(plan->Shards().size() == 3);
    REQUIRE(plan->Shards()[0] ==
            Sora::Compute::DispatchSlice{.rank = 3, .origin = {0, 0, 0}, .extents = {4, 4, 2}, .workItemCount = 32});
    REQUIRE(plan->Shards()[1] ==
            Sora::Compute::DispatchSlice{.rank = 3, .origin = {4, 0, 0}, .extents = {3, 4, 2}, .workItemCount = 24});
    REQUIRE(plan->Shards()[2] ==
            Sora::Compute::DispatchSlice{.rank = 3, .origin = {7, 0, 0}, .extents = {3, 4, 2}, .workItemCount = 24});
    REQUIRE(Sora::Compute::ElementwisePartitionPlan::ValidateStrictCover(
                domain, Sora::Compute::PartitionContract::Elementwise(3), 0, plan->Shards())
                .has_value());
    uint64_t workItems = 0;
    for (const Sora::Compute::DispatchSlice& shard : plan->Shards()) {
        REQUIRE(shard.ValidWithin(domain));
        REQUIRE(Sora::Compute::PlanDispatchSliceGeometry(shard).has_value());
        workItems += shard.workItemCount;
    }
    REQUIRE(workItems == domain.workItemCount);
}

TEST_CASE("ElementwisePartitionPlan constructs strict covers along every contract-permitted axis",
          "[Sora][Compute][Partition][Axis]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 3, .extents = {2, 5, 7}, .workItemCount = 70};
    constexpr auto contract = Sora::Compute::PartitionContract::Elementwise(domain.rank);

    const auto axis1 = Sora::Compute::ElementwisePartitionPlan::Balanced(domain, contract, 1, 2);
    REQUIRE(axis1.has_value());
    REQUIRE(axis1->Axis() == 1);
    REQUIRE(axis1->Shards().size() == 2);
    REQUIRE(axis1->Shards()[0] ==
            Sora::Compute::DispatchSlice{.rank = 3, .origin = {0, 0, 0}, .extents = {2, 3, 7}, .workItemCount = 42});
    REQUIRE(axis1->Shards()[1] ==
            Sora::Compute::DispatchSlice{.rank = 3, .origin = {0, 3, 0}, .extents = {2, 2, 7}, .workItemCount = 28});

    constexpr std::array axis2Extents{uint64_t{2}, uint64_t{3}, uint64_t{2}};
    const auto axis2 = Sora::Compute::ElementwisePartitionPlan::Create(domain, contract, 2, axis2Extents);
    REQUIRE(axis2.has_value());
    REQUIRE(axis2->Axis() == 2);
    REQUIRE(axis2->Shards()[0].origin == std::array<uint64_t, Sora::Compute::kMaximumIndexRank>{0, 0, 0});
    REQUIRE(axis2->Shards()[1].origin == std::array<uint64_t, Sora::Compute::kMaximumIndexRank>{0, 0, 2});
    REQUIRE(axis2->Shards()[2].origin == std::array<uint64_t, Sora::Compute::kMaximumIndexRank>{0, 0, 5});
    REQUIRE(Sora::Compute::ElementwisePartitionPlan::ValidateStrictCover(domain, contract, 2, axis2->Shards()));
}

TEST_CASE("ElementwisePartitionPlan rejects unsupported or incomplete decompositions",
          "[Sora][Compute][Partition][Validation]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 2, .extents = {8, 4}, .workItemCount = 32};
    constexpr std::array complete{uint64_t{3}, uint64_t{5}};
    constexpr std::array incomplete{uint64_t{3}, uint64_t{4}};

    const auto indivisible = Sora::Compute::ElementwisePartitionPlan::Create(
        domain, Sora::Compute::PartitionContract::Indivisible(), 0, complete);
    REQUIRE_FALSE(indivisible.has_value());
    REQUIRE(indivisible.error() == Sora::Compute::DispatchPartitionError::InvalidContract);

    constexpr std::array axis1Cover{uint64_t{1}, uint64_t{3}};
    constexpr Sora::Compute::PartitionContract axis0Only{.kind = Sora::Compute::PartitionKind::Elementwise,
                                                         .splittableAxisMask = 1};
    const auto disallowedAxis = Sora::Compute::ElementwisePartitionPlan::Create(domain, axis0Only, 1, axis1Cover);
    REQUIRE_FALSE(disallowedAxis.has_value());
    REQUIRE(disallowedAxis.error() == Sora::Compute::DispatchPartitionError::InvalidContract);

    const auto incompleteCover = Sora::Compute::ElementwisePartitionPlan::Create(
        domain, Sora::Compute::PartitionContract::Elementwise(2), 0, incomplete);
    REQUIRE_FALSE(incompleteCover.has_value());
    REQUIRE(incompleteCover.error() == Sora::Compute::DispatchPartitionError::IncompleteCover);

    constexpr std::array overlappingShards{
        Sora::Compute::DispatchSlice{.rank = 2, .origin = {0, 0}, .extents = {5, 4}, .workItemCount = 20},
        Sora::Compute::DispatchSlice{.rank = 2, .origin = {4, 0}, .extents = {4, 4}, .workItemCount = 16},
    };
    const auto overlappingCover = Sora::Compute::ElementwisePartitionPlan::ValidateStrictCover(
        domain, Sora::Compute::PartitionContract::Elementwise(2), 0, overlappingShards);
    REQUIRE_FALSE(overlappingCover.has_value());
    REQUIRE(overlappingCover.error() == Sora::Compute::DispatchPartitionError::IncompleteCover);

    const auto tooMany = Sora::Compute::ElementwisePartitionPlan::Balanced(
        domain, Sora::Compute::PartitionContract::Elementwise(2), 0, 9);
    REQUIRE_FALSE(tooMany.has_value());
    REQUIRE(tooMany.error() == Sora::Compute::DispatchPartitionError::TooManyShards);

    constexpr Sora::Compute::DispatchDomain inconsistentDomain{.rank = 2, .extents = {8, 4}, .workItemCount = 31};
    const auto inconsistent = Sora::Compute::ElementwisePartitionPlan::Balanced(
        inconsistentDomain, Sora::Compute::PartitionContract::Elementwise(2), 0, 2);
    REQUIRE_FALSE(inconsistent.has_value());
    REQUIRE(inconsistent.error() == Sora::Compute::DispatchPartitionError::InvalidDomain);

    constexpr Sora::Compute::DispatchDomain excessiveRank{
        .rank = Sora::Compute::kMaximumIndexRank + 1, .extents = {8, 4, 1}, .workItemCount = 32};
    const auto excessive = Sora::Compute::ElementwisePartitionPlan::Balanced(
        excessiveRank, Sora::Compute::PartitionContract::Elementwise(2), Sora::Compute::kMaximumIndexRank, 2);
    REQUIRE_FALSE(excessive.has_value());
    REQUIRE(excessive.error() == Sora::Compute::DispatchPartitionError::InvalidDomain);

    constexpr Sora::Compute::DispatchSlice inactiveAxisData{
        .rank = 1, .origin = {0, 1, 0}, .extents = {8, 0, 0}, .workItemCount = 8};
    REQUIRE_FALSE(Sora::Compute::PlanDispatchSliceGeometry(inactiveAxisData).has_value());
}

TEST_CASE("BufferView subviews preserve closed affine layouts along arbitrary axes",
          "[Sora][Compute][Partition][BufferView]") {
    constexpr Sora::Compute::ValueType scalar{Sora::Compute::ScalarKind::Float32, 1};
    auto contiguousBuffer = Sora::Compute::Buffer::Create({.id = {1, 1},
                                                           .elementType = scalar,
                                                           .layout = Sora::Compute::LayoutKind::Contiguous,
                                                           .rank = 2,
                                                           .sizeBytes = 128,
                                                           .alignment = 64});
    REQUIRE(contiguousBuffer.has_value());
    constexpr std::array global2d{uint64_t{4}, uint64_t{8}};
    auto contiguous = Sora::Compute::BufferView::Contiguous(*contiguousBuffer, global2d);
    REQUIRE(contiguous.has_value());
    constexpr std::array origin2d{uint64_t{1}, uint64_t{0}};
    constexpr std::array extent2d{uint64_t{2}, uint64_t{8}};
    const auto contiguousShard = contiguous->SubView(origin2d, extent2d);
    REQUIRE(contiguousShard.has_value());
    REQUIRE(contiguousShard->Describe().layout == Sora::Compute::LayoutKind::Contiguous);
    REQUIRE(contiguousShard->Describe().offsetBytes == 32);
    REQUIRE(contiguousShard->Region() == *Sora::Compute::ByteRegion::Make(32, 64));

    constexpr std::array columnOrigin{uint64_t{0}, uint64_t{2}};
    constexpr std::array columnExtents{uint64_t{4}, uint64_t{3}};
    const auto contiguousColumns = contiguous->SubView(columnOrigin, columnExtents);
    REQUIRE(contiguousColumns.has_value());
    REQUIRE(contiguousColumns->Describe().layout == Sora::Compute::LayoutKind::Strided);
    REQUIRE(contiguousColumns->Describe().offsetBytes == 8);
    REQUIRE(contiguousColumns->Region() == *Sora::Compute::ByteRegion::Make(8, 108));
    REQUIRE(contiguousColumns->Footprint().OccupiedByteCount() == 48);

    constexpr Sora::Compute::ValueType vec3{Sora::Compute::ScalarKind::Float32, 3};
    constexpr std::array global1d{uint64_t{16}};
    constexpr std::array soaStrides{uint64_t{4}};
    constexpr auto soaCoverage = Sora::Compute::AffineByteFootprint::Create(global1d, soaStrides, 64, 0, 4, 3);
    static_assert(soaCoverage.has_value());
    auto soaBuffer = Sora::Compute::Buffer::Create({.id = {2, 2},
                                                    .elementType = vec3,
                                                    .layout = Sora::Compute::LayoutKind::StructureOfArrays,
                                                    .rank = 1,
                                                    .sizeBytes = 192,
                                                    .alignment = 64},
                                                   *soaCoverage);
    REQUIRE(soaBuffer.has_value());
    auto soa = Sora::Compute::BufferView::StructureOfArrays(*soaBuffer, global1d);
    REQUIRE(soa.has_value());
    constexpr std::array origin1d{uint64_t{4}};
    constexpr std::array extent1d{uint64_t{6}};
    const auto soaShard = soa->SubView(origin1d, extent1d);
    REQUIRE(soaShard.has_value());
    REQUIRE(soaShard->Describe().componentStrideBytes == 64);
    REQUIRE(soaShard->Describe().offsetBytes == 16);
    REQUIRE(soaShard->Region() == *Sora::Compute::ByteRegion::Make(16, 152));

    constexpr std::array soa2dStrides{uint64_t{32}, uint64_t{4}};
    constexpr auto soa2dCoverage =
        Sora::Compute::AffineByteFootprint::Create(global2d, soa2dStrides, 128, 0, sizeof(float), 3);
    static_assert(soa2dCoverage.has_value());
    auto soa2dBuffer = Sora::Compute::Buffer::Create({.id = {3, 3},
                                                      .elementType = vec3,
                                                      .layout = Sora::Compute::LayoutKind::StructureOfArrays,
                                                      .rank = 2,
                                                      .sizeBytes = 384,
                                                      .alignment = 64},
                                                     *soa2dCoverage);
    REQUIRE(soa2dBuffer.has_value());
    auto soa2d = Sora::Compute::BufferView::StructureOfArrays(*soa2dBuffer, global2d);
    REQUIRE(soa2d.has_value());
    const auto soaColumns = soa2d->SubView(columnOrigin, columnExtents);
    REQUIRE(soaColumns.has_value());
    REQUIRE(soaColumns->Describe().layout == Sora::Compute::LayoutKind::Strided);
    REQUIRE(soaColumns->Describe().offsetBytes == 8);
    REQUIRE(soaColumns->Describe().componentStrideBytes == 128);
    REQUIRE(soaColumns->Region() == *Sora::Compute::ByteRegion::Make(8, 364));
    REQUIRE(soaColumns->Footprint().OccupiedByteCount() == 144);
}
