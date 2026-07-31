#include <Sora/Compute/Memory/AffineByteFootprint.h>
#include <Sora/Compute/Memory/BufferView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

TEST_CASE("Affine byte footprints project contiguous and sparse occupancy exactly",
          "[Sora][Compute][Memory][AffineByteFootprint]") {
    constexpr std::array contiguousExtents{uint64_t{8}};
    constexpr std::array contiguousStrides{uint64_t{4}};
    constexpr auto contiguous =
        Sora::Compute::AffineByteFootprint::Create(contiguousExtents, contiguousStrides, 4, 0, 4, 1);
    static_assert(contiguous.has_value());
    static_assert(contiguous->IsContiguous());
    constexpr auto contiguousRuns = contiguous->ProjectCanonicalRegions<1>();
    static_assert(contiguousRuns.has_value());
    static_assert(contiguousRuns->Size() == 1);
    static_assert(contiguousRuns->Regions()[0] == *Sora::Compute::ByteRegion::Make(0, 32));

    constexpr std::array sparseStrides{uint64_t{8}};
    constexpr auto sparse = Sora::Compute::AffineByteFootprint::Create(contiguousExtents, sparseStrides, 4, 0, 4, 1);
    static_assert(sparse.has_value());
    static_assert(!sparse->IsContiguous());
    constexpr auto sparseRuns = sparse->ProjectCanonicalRegions<8>();
    static_assert(sparseRuns.has_value());
    static_assert(sparseRuns->Size() == 8);
    for (size_t index = 0; index < sparseRuns->Size(); ++index) {
        REQUIRE(sparseRuns->Regions()[index] == *Sora::Compute::ByteRegion::Make(index * 8, 4));
    }
    constexpr auto bounded = sparse->ProjectCanonicalRegions<4>();
    static_assert(!bounded.has_value());
    static_assert(bounded.error() == Sora::Compute::AffineByteFootprintError::CapacityExceeded);
}

TEST_CASE("Affine byte footprints preserve padded SoA planes through BufferView",
          "[Sora][Compute][Memory][AffineByteFootprint][SoA]") {
    constexpr std::array extents{uint64_t{8}};
    constexpr std::array strides{uint64_t{4}};
    constexpr Sora::Compute::ValueType vec3{Sora::Compute::ScalarKind::Float32, 3};
    constexpr auto logical = Sora::Compute::AffineByteFootprint::Create(extents, strides, 96, 0, 4, 3);
    static_assert(logical.has_value());
    auto buffer = Sora::Compute::Buffer::Create({.id = {4, 3},
                                                 .elementType = vec3,
                                                 .layout = Sora::Compute::LayoutKind::StructureOfArrays,
                                                 .rank = 1,
                                                 .sizeBytes = 224,
                                                 .alignment = 64},
                                                *logical);
    REQUIRE(buffer.has_value());
    auto view = Sora::Compute::BufferView::StructureOfArrays(*buffer, extents, 0, 96);
    REQUIRE(view.has_value());
    const auto footprint = view->Footprint();
    REQUIRE(footprint.OccupiedByteCount() == 96);
    REQUIRE(footprint.BoundingRegion() == view->Region());
    const auto runs = footprint.ProjectCanonicalRegions<3>();
    REQUIRE(runs.has_value());
    REQUIRE(runs->Size() == 3);
    REQUIRE(runs->Regions()[0] == *Sora::Compute::ByteRegion::Make(0, 32));
    REQUIRE(runs->Regions()[1] == *Sora::Compute::ByteRegion::Make(96, 32));
    REQUIRE(runs->Regions()[2] == *Sora::Compute::ByteRegion::Make(192, 32));
}

TEST_CASE("Affine byte footprints merge runs across multidimensional boundaries",
          "[Sora][Compute][Memory][AffineByteFootprint][Multidimensional]") {
    constexpr std::array extents{uint64_t{3}, uint64_t{2}};
    constexpr std::array strides{uint64_t{12}, uint64_t{8}};
    constexpr auto footprint = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    static_assert(footprint.has_value());
    static_assert(footprint->OccupiedByteCount() == 24);
    static_assert(footprint->BoundingRegion().Size() == 36);
    constexpr auto runs = footprint->ProjectCanonicalRegions<4>();
    static_assert(runs.has_value());
    static_assert(runs->Size() == 4);
    static_assert(runs->Regions()[0] == *Sora::Compute::ByteRegion::Make(0, 4));
    static_assert(runs->Regions()[1] == *Sora::Compute::ByteRegion::Make(8, 8));
    static_assert(runs->Regions()[2] == *Sora::Compute::ByteRegion::Make(20, 8));
    static_assert(runs->Regions()[3] == *Sora::Compute::ByteRegion::Make(32, 4));
    REQUIRE(runs->Size() == 4);
}

TEST_CASE("Affine byte footprint construction rejects overlap and arithmetic overflow",
          "[Sora][Compute][Memory][AffineByteFootprint][Validation]") {
    constexpr std::array extents{uint64_t{4}};
    constexpr std::array overlappingStrides{uint64_t{4}};
    constexpr auto overlapping = Sora::Compute::AffineByteFootprint::Create(extents, overlappingStrides, 4, 0, 4, 2);
    static_assert(!overlapping.has_value());
    static_assert(overlapping.error() == Sora::Compute::AffineByteFootprintError::OverlappingElements);

    constexpr std::array overflowingExtents{UINT64_MAX};
    constexpr std::array overflowingStrides{uint64_t{8}};
    constexpr auto overflowing =
        Sora::Compute::AffineByteFootprint::Create(overflowingExtents, overflowingStrides, 4, 0, 4, 1);
    static_assert(!overflowing.has_value());
    static_assert(overflowing.error() == Sora::Compute::AffineByteFootprintError::ArithmeticOverflow);
}
