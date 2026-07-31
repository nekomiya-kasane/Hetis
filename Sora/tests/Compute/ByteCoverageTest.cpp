#include <Sora/Compute/Memory/ByteCoverage.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace {

    consteval bool AffineByteMembershipIsExact() {
        constexpr std::array extents{uint64_t{3}, uint64_t{2}};
        constexpr std::array strides{uint64_t{12}, uint64_t{8}};
        constexpr auto footprint = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 16, 4, 1);
        if (!footprint) {
            return false;
        }
        constexpr auto regions = footprint->ProjectCanonicalRegions<4>();
        if (!regions) {
            return false;
        }
        for (uint64_t offset = 0; offset < footprint->BoundingRegion().End() + 1; ++offset) {
            bool expected = false;
            for (const Sora::Compute::ByteRegion region : regions->Regions()) {
                expected = expected || region.Contains(*Sora::Compute::ByteRegion::Make(offset, 1));
            }
            if (footprint->OccupiesByte(offset) != expected) {
                return false;
            }
        }
        return true;
    }

    consteval bool CoverageContainmentIsSound() {
        constexpr std::array extents{uint64_t{3}, uint64_t{2}};
        constexpr std::array strides{uint64_t{12}, uint64_t{8}};
        constexpr auto footprint = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 16, 4, 1);
        if (!footprint) {
            return false;
        }
        constexpr auto regions = footprint->ProjectCanonicalRegions<4>();
        if (!regions) {
            return false;
        }
        const auto coverage = Sora::Compute::ByteCoverageView::Affine(*footprint);
        for (uint64_t offset = 0; offset < footprint->BoundingRegion().End() + 1; ++offset) {
            for (uint64_t size = 1; size <= 5; ++size) {
                const auto requested = Sora::Compute::ByteRegion::Make(offset, size);
                if (!requested) {
                    return false;
                }
                bool expected = false;
                for (const Sora::Compute::ByteRegion region : regions->Regions()) {
                    expected = expected || region.Contains(*requested);
                }
                const auto actual = coverage.Contains(*requested);
                if ((actual == Sora::Compute::CoverageContainment::Yes && !expected) ||
                    (actual == Sora::Compute::CoverageContainment::No && expected)) {
                    return false;
                }
            }
        }
        return true;
    }

    static_assert(AffineByteMembershipIsExact());
    static_assert(CoverageContainmentIsSound());

} // namespace

TEST_CASE("Byte coverage preserves exact interval and affine semantics", "[Sora][Compute][Memory][ByteCoverage]") {
    using namespace Sora::Compute;

    constexpr auto interval = ByteCoverageView::Interval(*ByteRegion::Make(16, 32));
    static_assert(interval.Valid());
    static_assert(interval.IsContiguous());
    static_assert(interval.OccupiedByteCount() == 32);
    static_assert(interval.Contains(*ByteRegion::Make(24, 8)) == CoverageContainment::Yes);
    static_assert(interval.Contains(*ByteRegion::Make(8, 16)) == CoverageContainment::No);

    constexpr std::array extents{uint64_t{4}};
    constexpr std::array strides{uint64_t{8}};
    static constexpr auto footprint = AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    static_assert(footprint.has_value());
    constexpr auto affine = ByteCoverageView::Affine(*footprint);
    static_assert(affine.Contains(*ByteRegion::Make(8, 4)) == CoverageContainment::Yes);
    static_assert(affine.Contains(*ByteRegion::Make(4, 4)) == CoverageContainment::No);
    static_assert(affine.Relation(ByteCoverageView::Interval(*ByteRegion::Make(32, 4))) == CoverageOverlap::Disjoint);
    static_assert(affine.Relation(ByteCoverageView::Interval(*ByteRegion::Make(5, 2))) == CoverageOverlap::Unknown);
    REQUIRE(sizeof(ByteCoverageView) <= 24);
}

TEST_CASE("Translated byte coverage maps padded SoA planes without publishing holes",
          "[Sora][Compute][Memory][ByteCoverage][Mapping]") {
    using namespace Sora::Compute;

    constexpr std::array extents{uint64_t{8}};
    constexpr std::array strides{uint64_t{4}};
    static constexpr auto footprint = AffineByteFootprint::Create(extents, strides, 96, 64, 4, 3);
    static_assert(footprint.has_value());
    constexpr auto mapping = TranslatedByteCoverage::Create(ByteCoverageView::Affine(*footprint), 128, 384);
    static_assert(mapping.has_value());
    constexpr auto physical = mapping->ProjectPhysicalRegions<3>();
    static_assert(physical.has_value());
    static_assert(physical->Size() == 3);
    static_assert(physical->Regions()[0] == *ByteRegion::Make(128, 32));
    static_assert(physical->Regions()[1] == *ByteRegion::Make(224, 32));
    static_assert(physical->Regions()[2] == *ByteRegion::Make(320, 32));
    static_assert(mapping->Resolve(*ByteRegion::Make(160, 4)).value() == *ByteRegion::Make(224, 4));
    static_assert(mapping->Resolve(*ByteRegion::Make(128, 4)).error() == ByteMappingError::OutsideCoverage);
    REQUIRE(physical->Regions()[2].End() == 352);
}

TEST_CASE("Exact coverage union distinguishes sparse covers, gaps, overlap, and bounded uncertainty",
          "[Sora][Compute][Memory][ByteCoverage][Union]") {
    using namespace Sora::Compute;

    constexpr std::array extents{uint64_t{8}};
    constexpr std::array strides{uint64_t{8}};
    static constexpr auto target = AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    constexpr std::array shardExtents{uint64_t{4}};
    static constexpr auto first = AffineByteFootprint::Create(shardExtents, strides, 4, 0, 4, 1);
    static constexpr auto second = AffineByteFootprint::Create(shardExtents, strides, 4, 32, 4, 1);
    static_assert(target.has_value() && first.has_value() && second.has_value());
    constexpr std::array exactParts{ByteCoverageView::Affine(*first), ByteCoverageView::Affine(*second)};
    static_assert(ExactCoverageUnion<8>(ByteCoverageView::Affine(*target), exactParts) == CoverageEquivalence::Equal);

    constexpr std::array overlappingParts{ByteCoverageView::Affine(*first), ByteCoverageView::Affine(*first)};
    static_assert(ExactCoverageUnion<8>(ByteCoverageView::Affine(*target), overlappingParts) ==
                  CoverageEquivalence::Different);
    constexpr std::array missingPart{ByteCoverageView::Affine(*first)};
    static_assert(ExactCoverageUnion<8>(ByteCoverageView::Affine(*target), missingPart) ==
                  CoverageEquivalence::Different);
    static_assert(ExactCoverageUnion<4>(ByteCoverageView::Affine(*target), exactParts) == CoverageEquivalence::Equal);

    constexpr auto intervalParts = [] {
        std::array<ByteCoverageView, 8> result{};
        for (uint64_t index = 0; index < result.size(); ++index) {
            result[index] = ByteCoverageView::Interval(*ByteRegion::Make(index * 8, 4));
        }
        return result;
    }();
    static_assert(ExactCoverageUnion<4>(ByteCoverageView::Affine(*target), intervalParts) ==
                  CoverageEquivalence::Unknown);

    constexpr std::array largeExtents{uint64_t{1'000'000}};
    static constexpr auto largeTarget = AffineByteFootprint::Create(largeExtents, strides, 4, 0, 4, 1);
    constexpr std::array largeShardExtents{uint64_t{500'000}};
    static constexpr auto largeFirst = AffineByteFootprint::Create(largeShardExtents, strides, 4, 0, 4, 1);
    static constexpr auto largeSecond = AffineByteFootprint::Create(largeShardExtents, strides, 4, 4'000'000, 4, 1);
    static_assert(largeTarget.has_value() && largeFirst.has_value() && largeSecond.has_value());
    constexpr std::array largeParts{ByteCoverageView::Affine(*largeFirst), ByteCoverageView::Affine(*largeSecond)};
    static_assert(ExactCoverageUnion<1>(ByteCoverageView::Affine(*largeTarget), largeParts) ==
                  CoverageEquivalence::Equal);
    REQUIRE(ExactCoverageUnion<8>(ByteCoverageView::Affine(*target), exactParts) == CoverageEquivalence::Equal);
}

TEST_CASE("Affine coverage complement subtracts logical boxes without expanding sparse elements",
          "[Sora][Compute][Memory][ByteCoverage][Difference]") {
    using namespace Sora::Compute;

    constexpr std::array extents1d{uint64_t{8}};
    constexpr std::array strides1d{uint64_t{8}};
    static constexpr auto domain1d = AffineByteFootprint::Create(extents1d, strides1d, 4, 0, 4, 1);
    constexpr std::array middleExtents{uint64_t{2}};
    static constexpr auto middle = AffineByteFootprint::Create(middleExtents, strides1d, 4, 24, 4, 1);
    static_assert(domain1d && middle);
    static constexpr std::array removed1d{ByteCoverageView::Affine(*middle)};
    static constexpr auto complement1d =
        AffineCoverageComplement<2>::Create(ByteCoverageView::Affine(*domain1d), removed1d);
    static_assert(complement1d.has_value());
    static_assert(complement1d->Size() == 2);
    static_assert(complement1d->Footprints()[0].Extents()[0] == 3);
    static_assert(complement1d->Footprints()[0].OffsetBytes() == 0);
    static_assert(complement1d->Footprints()[1].Extents()[0] == 3);
    static_assert(complement1d->Footprints()[1].OffsetBytes() == 40);
    static constexpr auto exact1d = [] {
        std::array<ByteCoverageView, 3> parts{ByteCoverageView::Affine(*middle)};
        for (size_t index = 0; index < complement1d->Size(); ++index) {
            parts[index + 1] = ByteCoverageView::Affine(complement1d->Footprints()[index]);
        }
        return ExactCoverageUnion<3>(ByteCoverageView::Affine(*domain1d), parts);
    }();
    static_assert(exact1d == CoverageEquivalence::Equal);

    constexpr std::array overlappingExtents{uint64_t{2}};
    static constexpr auto overlapping = AffineByteFootprint::Create(overlappingExtents, strides1d, 4, 32, 4, 1);
    static_assert(overlapping);
    static constexpr std::array overlappingRemovals{ByteCoverageView::Affine(*middle),
                                                    ByteCoverageView::Affine(*overlapping)};
    static constexpr auto overlappingComplement =
        AffineCoverageComplement<2>::Create(ByteCoverageView::Affine(*domain1d), overlappingRemovals);
    static_assert(overlappingComplement.has_value());
    static_assert(overlappingComplement->Size() == 2);
    static_assert(overlappingComplement->Footprints()[0].Extents()[0] == 3);
    static_assert(overlappingComplement->Footprints()[0].OffsetBytes() == 0);
    static_assert(overlappingComplement->Footprints()[1].Extents()[0] == 2);
    static_assert(overlappingComplement->Footprints()[1].OffsetBytes() == 48);

    static constexpr std::array siblingRemoval{ByteCoverageView::Affine(*overlapping)};
    static constexpr auto siblingComplement = AffineCoverageComplement<2>::Create(
        ByteCoverageView::Affine(*domain1d), ByteCoverageView::Affine(*middle), siblingRemoval);
    static_assert(siblingComplement.has_value());
    static_assert(siblingComplement->Size() == 1);
    static_assert(siblingComplement->Footprints()[0].Extents()[0] == 1);
    static_assert(siblingComplement->Footprints()[0].OffsetBytes() == 24);
    static constexpr auto siblingIntersection = IntersectAffineSubviews(
        ByteCoverageView::Affine(*domain1d), ByteCoverageView::Affine(*middle), ByteCoverageView::Affine(*overlapping));
    static_assert(siblingIntersection.has_value() && siblingIntersection->has_value());
    static_assert((**siblingIntersection).Extents()[0] == 1);
    static_assert((**siblingIntersection).OffsetBytes() == 32);

    static constexpr std::array wholeRemoval{ByteCoverageView::Affine(*domain1d)};
    static constexpr auto emptyComplement =
        AffineCoverageComplement<1>::Create(ByteCoverageView::Affine(*domain1d), wholeRemoval);
    static_assert(emptyComplement.has_value());
    static_assert(emptyComplement->Size() == 0);

    constexpr std::array extents2d{uint64_t{4}, uint64_t{6}};
    constexpr std::array strides2d{uint64_t{64}, uint64_t{8}};
    static constexpr auto domain2d = AffineByteFootprint::Create(extents2d, strides2d, 4, 0, 4, 1);
    constexpr std::array cutExtents{uint64_t{2}, uint64_t{2}};
    static constexpr auto cut = AffineByteFootprint::Create(cutExtents, strides2d, 4, 80, 4, 1);
    static_assert(domain2d && cut);
    static constexpr std::array removed2d{ByteCoverageView::Affine(*cut)};
    static constexpr auto complement2d =
        AffineCoverageComplement<4>::Create(ByteCoverageView::Affine(*domain2d), removed2d);
    static_assert(complement2d.has_value());
    static_assert(complement2d->Size() == 4);
    static constexpr auto exact2d = [] {
        std::array<ByteCoverageView, 5> parts{ByteCoverageView::Affine(*cut)};
        for (size_t index = 0; index < complement2d->Size(); ++index) {
            parts[index + 1] = ByteCoverageView::Affine(complement2d->Footprints()[index]);
        }
        return ExactCoverageUnion<5>(ByteCoverageView::Affine(*domain2d), parts);
    }();
    static_assert(exact2d == CoverageEquivalence::Equal);
    static_assert(AffineCoverageComplement<3>::Create(ByteCoverageView::Affine(*domain2d), removed2d).error() ==
                  AffineByteFootprintError::CapacityExceeded);
    constexpr std::array invalidRemoval{ByteCoverageView::Interval(*ByteRegion::Make(80, 4))};
    static_assert(AffineCoverageComplement<4>::Create(ByteCoverageView::Affine(*domain2d), invalidRemoval).error() ==
                  AffineByteFootprintError::InvalidDescriptor);
    REQUIRE(complement2d->Footprints().size() == 4);
}

TEST_CASE("Common affine domains prove multidimensional and SoA subview relations at constant scale",
          "[Sora][Compute][Memory][ByteCoverage][Relation]") {
    using namespace Sora::Compute;

    constexpr std::array extents2d{uint64_t{4}, uint64_t{6}};
    constexpr std::array strides2d{uint64_t{64}, uint64_t{8}};
    static constexpr auto domain2d = AffineByteFootprint::Create(extents2d, strides2d, 4, 0, 4, 1);
    constexpr std::array half2d{uint64_t{4}, uint64_t{3}};
    static constexpr auto left2d = AffineByteFootprint::Create(half2d, strides2d, 4, 0, 4, 1);
    static constexpr auto right2d = AffineByteFootprint::Create(half2d, strides2d, 4, 24, 4, 1);
    static constexpr auto overlap2d = AffineByteFootprint::Create(half2d, strides2d, 4, 16, 4, 1);
    static_assert(domain2d && left2d && right2d && overlap2d);
    static_assert(left2d->BoundingRegion().Intersects(right2d->BoundingRegion()));
    static_assert(CoverageRelationWithin(ByteCoverageView::Affine(*domain2d), ByteCoverageView::Affine(*left2d),
                                         ByteCoverageView::Affine(*right2d)) == CoverageOverlap::Disjoint);
    static_assert(CoverageRelationWithin(ByteCoverageView::Affine(*domain2d), ByteCoverageView::Affine(*left2d),
                                         ByteCoverageView::Affine(*overlap2d)) == CoverageOverlap::Overlap);

    constexpr std::array extents3d{uint64_t{3}, uint64_t{4}, uint64_t{6}};
    constexpr std::array strides3d{uint64_t{512}, uint64_t{64}, uint64_t{8}};
    static constexpr auto soaDomain = AffineByteFootprint::Create(extents3d, strides3d, 2048, 0, 4, 3);
    constexpr std::array half3d{uint64_t{3}, uint64_t{2}, uint64_t{6}};
    static constexpr auto soaFirst = AffineByteFootprint::Create(half3d, strides3d, 2048, 0, 4, 3);
    static constexpr auto soaSecond = AffineByteFootprint::Create(half3d, strides3d, 2048, 128, 4, 3);
    static_assert(soaDomain && soaFirst && soaSecond);
    static_assert(soaFirst->BoundingRegion().Intersects(soaSecond->BoundingRegion()));
    static_assert(CoverageRelationWithin(ByteCoverageView::Affine(*soaDomain), ByteCoverageView::Affine(*soaFirst),
                                         ByteCoverageView::Affine(*soaSecond)) == CoverageOverlap::Disjoint);
    constexpr std::array soaParts{ByteCoverageView::Affine(*soaFirst), ByteCoverageView::Affine(*soaSecond)};
    static_assert(ExactCoverageUnion<1>(ByteCoverageView::Affine(*soaDomain), soaParts) == CoverageEquivalence::Equal);
    REQUIRE(soaDomain->OccupiedByteCount() == soaFirst->OccupiedByteCount() + soaSecond->OccupiedByteCount());
}
