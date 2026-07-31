/**
 * @file StaticGraph.h
 * @brief Fixed-capacity constexpr execution graph and effect-aware builder.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/GraphModel.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <utility>

namespace Sora::Compute {

    template<size_t NodeCapacity, size_t AccessCapacity, size_t EdgeCapacity, size_t FootprintCapacity = AccessCapacity>
    class StaticGraphBuilder;

    /** @brief Immutable-by-interface fixed-capacity execution graph. */
    template<size_t NodeCapacity, size_t AccessCapacity, size_t EdgeCapacity, size_t FootprintCapacity = AccessCapacity>
    class StaticExecutionGraph {
        static_assert(NodeCapacity <= std::numeric_limits<uint32_t>::max());
        static_assert(AccessCapacity <= std::numeric_limits<uint32_t>::max());
        static_assert(EdgeCapacity <= std::numeric_limits<uint32_t>::max());
        static_assert(FootprintCapacity <= std::numeric_limits<uint32_t>::max());

    public:
        /** @brief Return the active logical graph without copying. */
        [[nodiscard]] constexpr GraphView View() const noexcept {
            return {.nodes = {nodes_.data(), nodeCount_},
                    .accesses = {accesses_.data(), accessCount_},
                    .edges = {edges_.data(), edgeCount_},
                    .footprints = {footprints_.data(), footprintCount_}};
        }

        /** @brief Return the active node count. */
        [[nodiscard]] constexpr size_t NodeCount() const noexcept { return nodeCount_; }

        /** @brief Return the active access count. */
        [[nodiscard]] constexpr size_t AccessCount() const noexcept { return accessCount_; }

        /** @brief Return the active dependency count. */
        [[nodiscard]] constexpr size_t EdgeCount() const noexcept { return edgeCount_; }

        /** @brief Return the active interned footprint count. */
        [[nodiscard]] constexpr size_t FootprintCount() const noexcept { return footprintCount_; }

    private:
        friend class StaticGraphBuilder<NodeCapacity, AccessCapacity, EdgeCapacity, FootprintCapacity>;

        std::array<GraphNode, NodeCapacity> nodes_{};
        std::array<ResourceAccess, AccessCapacity> accesses_{};
        std::array<GraphEdge, EdgeCapacity> edges_{};
        std::array<AffineByteFootprint, FootprintCapacity> footprints_{};
        uint32_t nodeCount_ = 0;
        uint32_t accessCount_ = 0;
        uint32_t edgeCount_ = 0;
        uint32_t footprintCount_ = 0;
    };

    /** @brief Transactional fixed-capacity builder that derives dependencies from buffer effects. */
    template<size_t NodeCapacity, size_t AccessCapacity, size_t EdgeCapacity, size_t FootprintCapacity>
    class StaticGraphBuilder {
        using Graph = StaticExecutionGraph<NodeCapacity, AccessCapacity, EdgeCapacity, FootprintCapacity>;

    public:
        /** @brief Add one node and every direct effect dependency from earlier nodes. */
        [[nodiscard]] constexpr std::expected<GraphNodeId, GraphError>
        AddNode(GraphNodeSpec spec, std::span<const ResourceAccess> accesses,
                std::span<const AffineByteFootprint> footprints = {}) noexcept {
            const uint32_t newNodeIndex = graph_.nodeCount_;
            if (graph_.nodeCount_ == NodeCapacity || accesses.size() > AccessCapacity - graph_.accessCount_ ||
                footprints.size() > FootprintCapacity - graph_.footprintCount_) {
                return std::unexpected(GraphError{GraphErrorCode::CapacityExceeded, newNodeIndex});
            }
            if (!Detail::ValidNodeKind(spec.kind)) {
                return std::unexpected(GraphError{GraphErrorCode::InvalidNode, newNodeIndex});
            }
            for (const AffineByteFootprint& footprint : footprints) {
                if (!footprint.Valid()) {
                    return std::unexpected(GraphError{GraphErrorCode::InvalidAccess, newNodeIndex});
                }
            }

            GraphNode candidate{.kind = spec.kind,
                                .operation = spec.operation,
                                .workItemCount = spec.workItemCount,
                                .firstAccess = 0,
                                .accessCount = static_cast<uint32_t>(accesses.size())};
            GraphView candidateGraph{
                .nodes = {&candidate, 1}, .accesses = accesses, .edges = {}, .footprints = footprints};
            if (auto validation = ValidateGraphStructure(candidateGraph); !validation) {
                return std::unexpected(GraphError{validation.error().code, newNodeIndex});
            }
            candidate.firstAccess = graph_.accessCount_;

            for (size_t index = 0; index < footprints.size(); ++index) {
                graph_.footprints_[graph_.footprintCount_ + index] = footprints[index];
            }
            std::array<ResourceAccess, AccessCapacity> remapped{};
            for (size_t index = 0; index < accesses.size(); ++index) {
                remapped[index] = accesses[index];
                if (remapped[index].footprint.Valid()) {
                    remapped[index].footprint.index += graph_.footprintCount_;
                }
            }
            const GraphView remappedCandidate{
                .nodes = {&candidate, 1},
                .accesses = {remapped.data(), accesses.size()},
                .edges = {},
                .footprints = {graph_.footprints_.data(), graph_.footprintCount_ + footprints.size()}};

            size_t requiredEdges = 0;
            const GraphView current = graph_.View();
            for (uint32_t predecessor = 0; predecessor < graph_.nodeCount_; ++predecessor) {
                bool conflicts = false;
                for (const ResourceAccess& previous : Detail::NodeAccesses(current, current.nodes[predecessor])) {
                    for (const ResourceAccess& next : remappedCandidate.accesses) {
                        conflicts = conflicts || Detail::EffectsConflict(current, previous, remappedCandidate, next);
                    }
                }
                requiredEdges += static_cast<size_t>(conflicts);
            }
            if (requiredEdges > EdgeCapacity - graph_.edgeCount_) {
                return std::unexpected(GraphError{GraphErrorCode::CapacityExceeded, newNodeIndex});
            }

            for (uint32_t predecessor = 0; predecessor < graph_.nodeCount_; ++predecessor) {
                bool conflicts = false;
                for (const ResourceAccess& previous : Detail::NodeAccesses(current, current.nodes[predecessor])) {
                    for (const ResourceAccess& next : remappedCandidate.accesses) {
                        conflicts = conflicts || Detail::EffectsConflict(current, previous, remappedCandidate, next);
                    }
                }
                if (conflicts) {
                    graph_.edges_[graph_.edgeCount_++] = {
                        .predecessor = {predecessor},
                        .successor = {newNodeIndex},
                    };
                }
            }
            graph_.footprintCount_ += static_cast<uint32_t>(footprints.size());
            for (const ResourceAccess& access : remappedCandidate.accesses) {
                graph_.accesses_[graph_.accessCount_++] = access;
            }
            graph_.nodes_[graph_.nodeCount_++] = candidate;
            return GraphNodeId{newNodeIndex};
        }

        /** @brief Add an explicit dependency between two existing topologically ordered nodes. */
        [[nodiscard]] constexpr std::expected<void, GraphError> AddDependency(GraphNodeId predecessor,
                                                                              GraphNodeId successor) noexcept {
            if (!predecessor.Valid() || !successor.Valid() || predecessor.index >= graph_.nodeCount_ ||
                successor.index >= graph_.nodeCount_ || predecessor.index >= successor.index) {
                return std::unexpected(
                    GraphError{GraphErrorCode::InvalidDependency, successor.index, predecessor.index});
            }
            const GraphView current = graph_.View();
            if (Detail::HasDirectEdge(current.edges, predecessor, successor)) {
                return {};
            }
            if (graph_.edgeCount_ == EdgeCapacity) {
                return std::unexpected(
                    GraphError{GraphErrorCode::CapacityExceeded, successor.index, predecessor.index});
            }
            graph_.edges_[graph_.edgeCount_++] = {.predecessor = predecessor, .successor = successor};
            return {};
        }

        /** @brief Return the current active graph for diagnostics without transferring ownership. */
        [[nodiscard]] constexpr GraphView View() const noexcept { return graph_.View(); }

        /** @brief Validate and freeze this builder's graph. */
        [[nodiscard]] constexpr std::expected<Graph, GraphError> Finish() && noexcept {
            if (auto validation = ValidateGraph(graph_.View()); !validation) {
                return std::unexpected(validation.error());
            }
            return std::move(graph_);
        }

    private:
        Graph graph_{};
    };

} // namespace Sora::Compute
