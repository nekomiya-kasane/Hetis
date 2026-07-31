#include <Sora/Compute/Graph/HazardAnalysis.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

    constexpr Sora::Compute::BufferId kBuffer{0xCA, 0xFE};
    constexpr Sora::Compute::SemanticHash kOperation{0x12, 0x34};

    constexpr Sora::Compute::ByteRegion Region(uint64_t offset, uint64_t size) {
        return *Sora::Compute::ByteRegion::Make(offset, size);
    }

    constexpr Sora::Compute::GraphNode Node(uint32_t accessIndex) {
        return {.kind = Sora::Compute::GraphNodeKind::Dispatch,
                .operation = kOperation,
                .workItemCount = 1,
                .firstAccess = accessIndex,
                .accessCount = 1};
    }

} // namespace

TEST_CASE("Hazard analysis keeps parallel readers and joins them at the next writer",
          "[Sora][Compute][Graph][Hazard]") {
    constexpr std::array nodes{Node(0), Node(1), Node(2), Node(3)};
    constexpr std::array accesses{
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 64), Sora::Compute::AccessMode::Write},
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 64), Sora::Compute::AccessMode::Read},
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 64), Sora::Compute::AccessMode::Read},
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 64), Sora::Compute::AccessMode::Write},
    };
    const Sora::Compute::GraphView source{.nodes = nodes, .accesses = accesses, .edges = {}};
    const auto derived = Sora::Compute::DeriveHazardDependencies(source);
    REQUIRE(derived.has_value());

    const std::vector expected{
        Sora::Compute::GraphEdge{{0}, {1}},
        Sora::Compute::GraphEdge{{0}, {2}},
        Sora::Compute::GraphEdge{{1}, {3}},
        Sora::Compute::GraphEdge{{2}, {3}},
    };
    REQUIRE(*derived == expected);
    REQUIRE(Sora::Compute::ValidateSparseGraph({.nodes = nodes, .accesses = accesses, .edges = *derived}).has_value());
}

TEST_CASE("Hazard analysis tracks partial writes without retaining transitively obsolete writers",
          "[Sora][Compute][Graph][Hazard][Interval]") {
    constexpr std::array nodes{Node(0), Node(1), Node(2), Node(3)};
    constexpr std::array accesses{
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 100), Sora::Compute::AccessMode::Write},
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 50), Sora::Compute::AccessMode::Write},
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 100), Sora::Compute::AccessMode::Read},
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 100), Sora::Compute::AccessMode::Write},
    };
    const auto derived = Sora::Compute::DeriveHazardDependencies({.nodes = nodes, .accesses = accesses, .edges = {}});
    REQUIRE(derived.has_value());

    const std::vector expected{
        Sora::Compute::GraphEdge{{0}, {1}},
        Sora::Compute::GraphEdge{{0}, {2}},
        Sora::Compute::GraphEdge{{1}, {2}},
        Sora::Compute::GraphEdge{{2}, {3}},
    };
    REQUIRE(*derived == expected);
}

TEST_CASE("Sparse graph validation rejects missing and duplicate frontier dependencies",
          "[Sora][Compute][Graph][Hazard][Validation]") {
    constexpr std::array nodes{Node(0), Node(1), Node(2)};
    constexpr std::array accesses{
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 64), Sora::Compute::AccessMode::Write},
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 64), Sora::Compute::AccessMode::Read},
        Sora::Compute::ResourceAccess{{0xBA, 0xBE}, Region(0, 64), Sora::Compute::AccessMode::Read},
    };
    const std::array required{Sora::Compute::GraphEdge{{0}, {1}}};
    REQUIRE(Sora::Compute::ValidateSparseGraph({.nodes = nodes, .accesses = accesses, .edges = required}).has_value());

    const auto missing = Sora::Compute::ValidateSparseGraph({.nodes = nodes, .accesses = accesses, .edges = {}});
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code == Sora::Compute::GraphErrorCode::MissingEffectDependency);
    REQUIRE(missing.error().node == 1);
    REQUIRE(missing.error().related == 0);

    const std::array duplicate{required[0], required[0]};
    const auto duplicated =
        Sora::Compute::ValidateSparseGraph({.nodes = nodes, .accesses = accesses, .edges = duplicate});
    REQUIRE_FALSE(duplicated.has_value());
    REQUIRE(duplicated.error().code == Sora::Compute::GraphErrorCode::DuplicateDependency);

    const std::array explicitAndRequired{required[0], Sora::Compute::GraphEdge{{0}, {2}}};
    REQUIRE(Sora::Compute::ValidateSparseGraph({.nodes = nodes, .accesses = accesses, .edges = explicitAndRequired})
                .has_value());
}

TEST_CASE("Graph structure validation rejects non-canonical access slices",
          "[Sora][Compute][Graph][Hazard][Validation]") {
    constexpr std::array nodes{
        Sora::Compute::GraphNode{.kind = Sora::Compute::GraphNodeKind::Dispatch,
                                 .operation = kOperation,
                                 .workItemCount = 1,
                                 .firstAccess = 0,
                                 .accessCount = 0},
    };
    constexpr std::array accesses{
        Sora::Compute::ResourceAccess{kBuffer, Region(0, 4), Sora::Compute::AccessMode::Read},
    };
    constexpr auto invalid = Sora::Compute::ValidateGraphStructure({.nodes = nodes, .accesses = accesses, .edges = {}});
    static_assert(!invalid.has_value());
    static_assert(invalid.error().code == Sora::Compute::GraphErrorCode::InvalidAccess);
    REQUIRE_FALSE(invalid.has_value());
}

TEST_CASE("Hazard analysis scales with sparse interval boundaries instead of node pairs",
          "[Sora][Compute][Graph][Hazard][Scale]") {
    constexpr uint32_t writerCount = 4096;
    constexpr uint32_t readerIndex = writerCount;
    constexpr uint32_t finalWriterIndex = writerCount + 1;
    std::vector<Sora::Compute::GraphNode> nodes;
    std::vector<Sora::Compute::ResourceAccess> accesses;
    nodes.reserve(writerCount + 2);
    accesses.reserve(writerCount + 2);
    for (uint32_t index = 0; index < writerCount; ++index) {
        nodes.push_back(Node(index));
        accesses.push_back({kBuffer, Region(static_cast<uint64_t>(index) * 4, 4), Sora::Compute::AccessMode::Write});
    }
    nodes.push_back(Node(readerIndex));
    accesses.push_back({kBuffer, Region(0, static_cast<uint64_t>(writerCount) * 4), Sora::Compute::AccessMode::Read});
    nodes.push_back(Node(finalWriterIndex));
    accesses.push_back({kBuffer, Region(0, static_cast<uint64_t>(writerCount) * 4), Sora::Compute::AccessMode::Write});

    const auto derived = Sora::Compute::DeriveHazardDependencies({.nodes = nodes, .accesses = accesses, .edges = {}});
    REQUIRE(derived.has_value());
    REQUIRE(derived->size() == writerCount + 1);
    for (uint32_t index = 0; index < writerCount; ++index) {
        REQUIRE((*derived)[index] == Sora::Compute::GraphEdge{{index}, {readerIndex}});
    }
    REQUIRE(derived->back() == Sora::Compute::GraphEdge{{readerIndex}, {finalWriterIndex}});
    REQUIRE(Sora::Compute::ValidateSparseGraph({.nodes = nodes, .accesses = accesses, .edges = *derived}).has_value());
}

TEST_CASE("Hazard analysis distinguishes interleaved affine byte sets with overlapping bounds",
          "[Sora][Compute][Graph][Hazard][Affine]") {
    constexpr std::array extents{uint64_t{4}};
    constexpr std::array strides{uint64_t{8}};
    constexpr auto even = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    constexpr auto odd = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 4, 4, 1);
    static_assert(even.has_value() && odd.has_value());
    constexpr std::array footprints{*even, *odd};
    constexpr std::array nodes{Node(0), Node(1), Node(2)};
    constexpr std::array accesses{
        Sora::Compute::ResourceAccess{kBuffer, even->BoundingRegion(), Sora::Compute::AccessMode::Write, {0}},
        Sora::Compute::ResourceAccess{kBuffer, odd->BoundingRegion(), Sora::Compute::AccessMode::Read, {1}},
        Sora::Compute::ResourceAccess{kBuffer, Region(8, 4), Sora::Compute::AccessMode::Read},
    };
    const Sora::Compute::GraphView source{.nodes = nodes, .accesses = accesses, .edges = {}, .footprints = footprints};
    const auto derived = Sora::Compute::DeriveHazardDependencies(source);
    REQUIRE(derived.has_value());
    REQUIRE(*derived == std::vector{Sora::Compute::GraphEdge{{0}, {2}}});
    REQUIRE(Sora::Compute::ValidateSparseGraph(
                {.nodes = nodes, .accesses = accesses, .edges = *derived, .footprints = footprints})
                .has_value());
}

TEST_CASE("Hazard analysis conservatively bounds affine projections beyond fixed capacity",
          "[Sora][Compute][Graph][Hazard][Affine][Capacity]") {
    constexpr std::array extents{uint64_t{65}};
    constexpr std::array strides{uint64_t{8}};
    constexpr auto even = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    constexpr auto odd = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 4, 4, 1);
    static_assert(even.has_value() && odd.has_value());
    constexpr std::array footprints{*even, *odd};
    constexpr std::array nodes{Node(0), Node(1)};
    constexpr std::array accesses{
        Sora::Compute::ResourceAccess{kBuffer, even->BoundingRegion(), Sora::Compute::AccessMode::Write, {0}},
        Sora::Compute::ResourceAccess{kBuffer, odd->BoundingRegion(), Sora::Compute::AccessMode::Read, {1}},
    };
    const auto derived = Sora::Compute::DeriveHazardDependencies(
        {.nodes = nodes, .accesses = accesses, .edges = {}, .footprints = footprints});
    REQUIRE(derived.has_value());
    REQUIRE(*derived == std::vector{Sora::Compute::GraphEdge{{0}, {1}}});
}
