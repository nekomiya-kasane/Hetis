/**
 * @file GraphModel.h
 * @brief Backend-independent logical execution-graph model and validation.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/ByteCoverage.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>

namespace Sora::Compute {

    inline constexpr uint32_t kInvalidGraphNodeIndex = std::numeric_limits<uint32_t>::max();
    inline constexpr uint32_t kInvalidByteFootprintIndex = std::numeric_limits<uint32_t>::max();

    /** @brief Strong identity of one node in a frozen execution graph. */
    struct GraphNodeId {
        uint32_t index = kInvalidGraphNodeIndex;

        /** @brief Return whether this identity denotes a graph node. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return index != kInvalidGraphNodeIndex; }

        friend constexpr bool operator==(GraphNodeId, GraphNodeId) noexcept = default;
        friend constexpr auto operator<=>(GraphNodeId, GraphNodeId) noexcept = default;
    };

    /** @brief Semantic role of one host-level logical graph node. */
    enum class GraphNodeKind : uint8_t {
        Dispatch,
        Copy,
        Fill,
        External,
    };

    /** @brief Graph-local identity of one interned non-contiguous affine footprint. */
    struct ByteFootprintId {
        uint32_t index = kInvalidByteFootprintIndex;

        /** @brief Return whether this identity denotes an arena entry. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return index != kInvalidByteFootprintIndex; }

        friend constexpr bool operator==(ByteFootprintId, ByteFootprintId) noexcept = default;
        friend constexpr auto operator<=>(ByteFootprintId, ByteFootprintId) noexcept = default;
    };

    /** @brief One logical buffer effect consumed by dependency derivation and placement. */
    struct ResourceAccess {
        BufferId buffer{};
        ByteRegion region{};
        AccessMode mode = AccessMode::Read;
        ByteFootprintId footprint{};

        friend constexpr bool operator==(ResourceAccess, ResourceAccess) noexcept = default;
    };

    /** @brief Backend-independent node descriptor with a contiguous access slice. */
    struct GraphNode {
        GraphNodeKind kind = GraphNodeKind::Dispatch;
        SemanticHash operation{};
        uint64_t workItemCount = 0;
        uint32_t firstAccess = 0;
        uint32_t accessCount = 0;

        friend constexpr bool operator==(GraphNode, GraphNode) noexcept = default;
    };

    /** @brief Node input before its access slice receives canonical storage offsets. */
    struct GraphNodeSpec {
        GraphNodeKind kind = GraphNodeKind::Dispatch;
        SemanticHash operation{};
        uint64_t workItemCount = 0;
    };

    /** @brief Directed happens-before edge in topological node order. */
    struct GraphEdge {
        GraphNodeId predecessor{};
        GraphNodeId successor{};

        friend constexpr bool operator==(GraphEdge, GraphEdge) noexcept = default;
    };

    /** @brief Canonical dependency ordering by successor and then predecessor. */
    [[nodiscard]] constexpr bool GraphEdgeLess(const GraphEdge& left, const GraphEdge& right) noexcept {
        if (left.successor != right.successor) {
            return left.successor < right.successor;
        }
        return left.predecessor < right.predecessor;
    }

    /** @brief Non-owning view shared by static and runtime graph representations. */
    struct GraphView {
        std::span<const GraphNode> nodes{};
        std::span<const ResourceAccess> accesses{};
        std::span<const GraphEdge> edges{};
        std::span<const AffineByteFootprint> footprints{};
    };

    /** @brief Resolve one access to its exact borrowed byte coverage. */
    [[nodiscard]] constexpr ByteCoverageView AccessCoverage(GraphView graph, const ResourceAccess& access) noexcept {
        if (!access.footprint.Valid()) {
            return ByteCoverageView::Interval(access.region);
        }
        if (access.footprint.index >= graph.footprints.size()) {
            return {};
        }
        return ByteCoverageView::Affine(graph.footprints[access.footprint.index]);
    }

    /** @brief Stable graph-validation failure. */
    enum class GraphErrorCode : uint8_t {
        EmptyGraph,
        CapacityExceeded,
        InvalidNode,
        InvalidAccess,
        InvalidDependency,
        DuplicateDependency,
        MissingEffectDependency,
        OutOfMemory,
    };

    /** @brief Graph error with the primary and related node indices. */
    struct GraphError {
        GraphErrorCode code = GraphErrorCode::InvalidNode;
        uint32_t node = kInvalidGraphNodeIndex;
        uint32_t related = kInvalidGraphNodeIndex;

        friend constexpr bool operator==(GraphError, GraphError) noexcept = default;
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidNodeKind(GraphNodeKind kind) noexcept {
            return kind == GraphNodeKind::Dispatch || kind == GraphNodeKind::Copy || kind == GraphNodeKind::Fill ||
                   kind == GraphNodeKind::External;
        }

        [[nodiscard]] constexpr bool IsWrite(AccessMode mode) noexcept {
            return mode != AccessMode::Read;
        }

        /** @brief Return whether two exact coverages may overlap after bounded projection. */
        [[nodiscard]] constexpr bool CoveragesMayOverlap(ByteCoverageView first, ByteCoverageView second) noexcept {
            const CoverageOverlap relation = first.Relation(second);
            if (relation != CoverageOverlap::Unknown) {
                return relation == CoverageOverlap::Overlap;
            }

            constexpr size_t projectionCapacity = 16;
            const auto firstRegions = first.ProjectCanonicalRegions<projectionCapacity>();
            const auto secondRegions = second.ProjectCanonicalRegions<projectionCapacity>();
            if (!firstRegions || !secondRegions) {
                return true;
            }
            for (const ByteRegion firstRegion : firstRegions->Regions()) {
                for (const ByteRegion secondRegion : secondRegions->Regions()) {
                    if (firstRegion.Intersects(secondRegion)) {
                        return true;
                    }
                }
            }
            return false;
        }

        /** @brief Return whether two effects require happens-before ordering. */
        [[nodiscard]] constexpr bool EffectsConflict(GraphView firstGraph, const ResourceAccess& first,
                                                     GraphView secondGraph, const ResourceAccess& second) noexcept {
            return first.buffer == second.buffer && (IsWrite(first.mode) || IsWrite(second.mode)) &&
                   CoveragesMayOverlap(AccessCoverage(firstGraph, first), AccessCoverage(secondGraph, second));
        }

        /** @brief Return whether two effects in one graph require happens-before ordering. */
        [[nodiscard]] constexpr bool EffectsConflict(GraphView graph, const ResourceAccess& first,
                                                     const ResourceAccess& second) noexcept {
            return EffectsConflict(graph, first, graph, second);
        }

        [[nodiscard]] constexpr bool HasDirectEdge(std::span<const GraphEdge> edges, GraphNodeId predecessor,
                                                   GraphNodeId successor) noexcept {
            for (const GraphEdge& edge : edges) {
                if (edge.predecessor == predecessor && edge.successor == successor) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] constexpr std::span<const ResourceAccess> NodeAccesses(GraphView graph,
                                                                             const GraphNode& node) noexcept {
            return graph.accesses.subspan(node.firstAccess, node.accessCount);
        }

        [[nodiscard]] constexpr bool NodesConflict(GraphView graph, const GraphNode& first,
                                                   const GraphNode& second) noexcept {
            for (const ResourceAccess& firstAccess : NodeAccesses(graph, first)) {
                for (const ResourceAccess& secondAccess : NodeAccesses(graph, second)) {
                    if (EffectsConflict(graph, firstAccess, secondAccess)) {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] constexpr bool ValidOperationIdentity(SemanticHash operation) noexcept {
            return operation.low != 0 || operation.high != 0;
        }

        [[nodiscard]] constexpr bool ValidNodeSemantics(GraphView graph, size_t nodeIndex) noexcept {
            const GraphNode& node = graph.nodes[nodeIndex];
            const auto accesses = NodeAccesses(graph, node);
            if (node.kind == GraphNodeKind::Dispatch) {
                return ValidOperationIdentity(node.operation) && node.workItemCount != 0;
            }
            if (node.kind == GraphNodeKind::Copy) {
                return node.workItemCount != 0 && accesses.size() == 2 && accesses[0].mode == AccessMode::Read &&
                       accesses[1].mode == AccessMode::Write;
            }
            if (node.kind == GraphNodeKind::Fill) {
                return node.workItemCount != 0 && accesses.size() == 1 && accesses[0].mode == AccessMode::Write;
            }
            return node.kind == GraphNodeKind::External && ValidOperationIdentity(node.operation);
        }

    } // namespace Detail

    /** @brief Validate canonical slices, node semantics, and topologically ordered dependency identities. */
    [[nodiscard]] constexpr std::expected<void, GraphError> ValidateGraphStructure(GraphView graph) noexcept {
        if (graph.nodes.empty()) {
            return std::unexpected(GraphError{GraphErrorCode::EmptyGraph});
        }
        if (graph.nodes.size() > std::numeric_limits<uint32_t>::max() ||
            graph.accesses.size() > std::numeric_limits<uint32_t>::max() ||
            graph.edges.size() > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected(GraphError{GraphErrorCode::CapacityExceeded});
        }

        size_t expectedFirstAccess = 0;
        for (size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex) {
            const GraphNode& node = graph.nodes[nodeIndex];
            if (!Detail::ValidNodeKind(node.kind) || node.firstAccess != expectedFirstAccess ||
                node.accessCount > graph.accesses.size() - node.firstAccess) {
                return std::unexpected(GraphError{GraphErrorCode::InvalidNode, static_cast<uint32_t>(nodeIndex)});
            }
            for (const ResourceAccess& access : Detail::NodeAccesses(graph, node)) {
                const ByteCoverageView coverage = AccessCoverage(graph, access);
                if (!access.buffer.Valid() || access.region.Size() == 0 ||
                    !Sora::Compute::ValidAccessMode(access.mode) || !coverage.Valid() ||
                    coverage.BoundingRegion() != access.region) {
                    return std::unexpected(GraphError{GraphErrorCode::InvalidAccess, static_cast<uint32_t>(nodeIndex)});
                }
            }
            if (!Detail::ValidNodeSemantics(graph, nodeIndex)) {
                return std::unexpected(GraphError{GraphErrorCode::InvalidNode, static_cast<uint32_t>(nodeIndex)});
            }
            expectedFirstAccess += node.accessCount;
        }
        if (expectedFirstAccess != graph.accesses.size()) {
            return std::unexpected(GraphError{GraphErrorCode::InvalidAccess});
        }

        for (size_t edgeIndex = 0; edgeIndex < graph.edges.size(); ++edgeIndex) {
            const GraphEdge& edge = graph.edges[edgeIndex];
            if (!edge.predecessor.Valid() || !edge.successor.Valid() || edge.predecessor.index >= graph.nodes.size() ||
                edge.successor.index >= graph.nodes.size() || edge.predecessor.index >= edge.successor.index) {
                return std::unexpected(
                    GraphError{GraphErrorCode::InvalidDependency, edge.successor.index, edge.predecessor.index});
            }
        }
        return {};
    }

    /** @brief Exhaustively validate a small graph, including every direct conflicting pair. */
    [[nodiscard]] constexpr std::expected<void, GraphError> ValidateGraph(GraphView graph) noexcept {
        if (auto structure = ValidateGraphStructure(graph); !structure) {
            return structure;
        }

        for (size_t edgeIndex = 0; edgeIndex < graph.edges.size(); ++edgeIndex) {
            const GraphEdge& edge = graph.edges[edgeIndex];
            for (size_t earlier = 0; earlier < edgeIndex; ++earlier) {
                if (graph.edges[earlier] == edge) {
                    return std::unexpected(
                        GraphError{GraphErrorCode::DuplicateDependency, edge.successor.index, edge.predecessor.index});
                }
            }
        }

        for (size_t successor = 1; successor < graph.nodes.size(); ++successor) {
            for (size_t predecessor = 0; predecessor < successor; ++predecessor) {
                if (Detail::NodesConflict(graph, graph.nodes[predecessor], graph.nodes[successor]) &&
                    !Detail::HasDirectEdge(graph.edges, GraphNodeId{static_cast<uint32_t>(predecessor)},
                                           GraphNodeId{static_cast<uint32_t>(successor)})) {
                    return std::unexpected(GraphError{GraphErrorCode::MissingEffectDependency,
                                                      static_cast<uint32_t>(successor),
                                                      static_cast<uint32_t>(predecessor)});
                }
            }
        }
        return {};
    }

    static_assert(std::is_trivially_copyable_v<GraphNodeId>);
    static_assert(std::is_trivially_copyable_v<ByteFootprintId>);
    static_assert(std::is_trivially_copyable_v<ResourceAccess>);
    static_assert(sizeof(ResourceAccess) == 40);
    static_assert(std::is_trivially_copyable_v<GraphNode>);
    static_assert(std::is_trivially_copyable_v<GraphNodeSpec>);
    static_assert(std::is_trivially_copyable_v<GraphEdge>);
    static_assert(std::is_standard_layout_v<GraphNode>);

} // namespace Sora::Compute
