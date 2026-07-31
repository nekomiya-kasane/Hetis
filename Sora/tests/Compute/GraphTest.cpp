#include <Sora/Compute/Graph/StaticGraph.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace {

    constexpr Sora::Compute::BufferId kFirstBuffer{1, 1};
    constexpr Sora::Compute::BufferId kSecondBuffer{2, 2};
    constexpr Sora::Compute::SemanticHash kTransform{3, 4};

    constexpr Sora::Compute::ByteRegion Region(uint64_t offset, uint64_t size) {
        return *Sora::Compute::ByteRegion::Make(offset, size);
    }

    consteval auto BuildStaticGraph() {
        Sora::Compute::StaticGraphBuilder<4, 6, 4> builder;
        constexpr std::array firstWrite{
            Sora::Compute::ResourceAccess{kFirstBuffer, Region(0, 64), Sora::Compute::AccessMode::Write},
        };
        constexpr std::array disjointRead{
            Sora::Compute::ResourceAccess{kFirstBuffer, Region(64, 64), Sora::Compute::AccessMode::Read},
        };
        constexpr std::array overlappingRead{
            Sora::Compute::ResourceAccess{kFirstBuffer, Region(32, 16), Sora::Compute::AccessMode::Read},
        };
        constexpr std::array secondWrite{
            Sora::Compute::ResourceAccess{kSecondBuffer, Region(0, 128), Sora::Compute::AccessMode::Write},
        };

        auto first = builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 64}, firstWrite);
        auto second = builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 64}, disjointRead);
        auto third = builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 16}, overlappingRead);
        auto fourth = builder.AddNode({Sora::Compute::GraphNodeKind::Fill, {}, 128}, secondWrite);
        if (!first || !second || !third || !fourth) {
            throw "static graph construction failed";
        }
        return std::move(builder).Finish();
    }

    constexpr auto kStaticGraph = BuildStaticGraph();
    static_assert(kStaticGraph.has_value());
    static_assert(kStaticGraph->NodeCount() == 4);
    static_assert(kStaticGraph->AccessCount() == 4);
    static_assert(kStaticGraph->EdgeCount() == 1);
    static_assert(kStaticGraph->View().edges[0] ==
                  Sora::Compute::GraphEdge{Sora::Compute::GraphNodeId{0}, Sora::Compute::GraphNodeId{2}});

    consteval auto BuildAffineStaticGraph() {
        constexpr std::array extents{uint64_t{4}};
        constexpr std::array strides{uint64_t{8}};
        constexpr auto even = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
        constexpr auto odd = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 4, 4, 1);
        if (!even || !odd) {
            throw "affine footprint construction failed";
        }

        Sora::Compute::StaticGraphBuilder<2, 2, 1, 2> builder;
        constexpr std::array write{
            Sora::Compute::ResourceAccess{kFirstBuffer, even->BoundingRegion(), Sora::Compute::AccessMode::Write, {0}},
        };
        constexpr std::array read{
            Sora::Compute::ResourceAccess{kFirstBuffer, odd->BoundingRegion(), Sora::Compute::AccessMode::Read, {0}},
        };
        constexpr std::array evenFootprint{*even};
        constexpr std::array oddFootprint{*odd};
        if (!builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 4}, write, evenFootprint) ||
            !builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 4}, read, oddFootprint)) {
            throw "affine static graph construction failed";
        }
        return std::move(builder).Finish();
    }

    constexpr auto kAffineStaticGraph = BuildAffineStaticGraph();
    static_assert(kAffineStaticGraph.has_value());
    static_assert(kAffineStaticGraph->FootprintCount() == 2);
    static_assert(kAffineStaticGraph->EdgeCount() == 0);

} // namespace

TEST_CASE("StaticGraph derives only overlapping write hazards", "[Sora][Compute][Graph]") {
    const Sora::Compute::GraphView graph = kStaticGraph->View();
    REQUIRE(graph.nodes.size() == 4);
    REQUIRE(graph.accesses.size() == 4);
    REQUIRE(graph.edges.size() == 1);
    REQUIRE(graph.edges[0].predecessor.index == 0);
    REQUIRE(graph.edges[0].successor.index == 2);
    REQUIRE(Sora::Compute::ValidateGraph(graph).has_value());
}

TEST_CASE("StaticGraph preserves exact affine coverage in constexpr construction", "[Sora][Compute][Graph][Affine]") {
    REQUIRE(kAffineStaticGraph->View().footprints.size() == 2);
    REQUIRE(kAffineStaticGraph->View().edges.empty());
    REQUIRE(Sora::Compute::ValidateGraph(kAffineStaticGraph->View()).has_value());
}

TEST_CASE("StaticGraphBuilder rejects edge overflow transactionally", "[Sora][Compute][Graph]") {
    Sora::Compute::StaticGraphBuilder<3, 3, 0> builder;
    constexpr std::array write{
        Sora::Compute::ResourceAccess{kFirstBuffer, Region(0, 64), Sora::Compute::AccessMode::Write},
    };
    constexpr std::array read{
        Sora::Compute::ResourceAccess{kFirstBuffer, Region(0, 64), Sora::Compute::AccessMode::Read},
    };
    constexpr std::array independent{
        Sora::Compute::ResourceAccess{kSecondBuffer, Region(0, 64), Sora::Compute::AccessMode::Read},
    };

    REQUIRE(builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 64}, write).has_value());
    const auto rejected = builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 64}, read);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == Sora::Compute::GraphErrorCode::CapacityExceeded);
    REQUIRE(builder.View().nodes.size() == 1);
    REQUIRE(builder.View().accesses.size() == 1);
    REQUIRE(builder.View().edges.empty());
    REQUIRE(builder.AddNode({Sora::Compute::GraphNodeKind::Dispatch, kTransform, 64}, independent).has_value());
    REQUIRE(builder.View().nodes.size() == 2);
}

TEST_CASE("Graph validation rejects missing effect dependencies", "[Sora][Compute][Graph][Validation]") {
    static constexpr std::array nodes{
        Sora::Compute::GraphNode{Sora::Compute::GraphNodeKind::Dispatch, kTransform, 64, 0, 1},
        Sora::Compute::GraphNode{Sora::Compute::GraphNodeKind::Dispatch, kTransform, 64, 1, 1},
    };
    static constexpr std::array accesses{
        Sora::Compute::ResourceAccess{kFirstBuffer, Region(0, 64), Sora::Compute::AccessMode::Write},
        Sora::Compute::ResourceAccess{kFirstBuffer, Region(0, 64), Sora::Compute::AccessMode::Read},
    };
    constexpr Sora::Compute::GraphView graph{nodes, accesses, {}};
    constexpr auto validation = Sora::Compute::ValidateGraph(graph);
    static_assert(!validation.has_value());
    static_assert(validation.error().code == Sora::Compute::GraphErrorCode::MissingEffectDependency);

    REQUIRE_FALSE(validation.has_value());
    REQUIRE(validation.error().node == 1);
    REQUIRE(validation.error().related == 0);
}

TEST_CASE("Graph validation rejects invalid copy semantics and backward edges", "[Sora][Compute][Graph][Validation]") {
    static constexpr std::array invalidCopyNodes{
        Sora::Compute::GraphNode{Sora::Compute::GraphNodeKind::Copy, {}, 64, 0, 1},
    };
    static constexpr std::array invalidCopyAccesses{
        Sora::Compute::ResourceAccess{kFirstBuffer, Region(0, 64), Sora::Compute::AccessMode::Read},
    };
    constexpr auto invalidCopy =
        Sora::Compute::ValidateGraph({.nodes = invalidCopyNodes, .accesses = invalidCopyAccesses, .edges = {}});
    static_assert(!invalidCopy.has_value());
    static_assert(invalidCopy.error().code == Sora::Compute::GraphErrorCode::InvalidNode);

    static constexpr std::array validNodes{
        Sora::Compute::GraphNode{Sora::Compute::GraphNodeKind::External, kTransform, 0, 0, 0},
        Sora::Compute::GraphNode{Sora::Compute::GraphNodeKind::External, kTransform, 0, 0, 0},
    };
    static constexpr std::array backwardEdge{
        Sora::Compute::GraphEdge{Sora::Compute::GraphNodeId{1}, Sora::Compute::GraphNodeId{0}},
    };
    constexpr auto invalidEdge =
        Sora::Compute::ValidateGraph({.nodes = validNodes, .accesses = {}, .edges = backwardEdge});
    static_assert(!invalidEdge.has_value());
    static_assert(invalidEdge.error().code == Sora::Compute::GraphErrorCode::InvalidDependency);

    REQUIRE_FALSE(invalidCopy.has_value());
    REQUIRE_FALSE(invalidEdge.has_value());
}
