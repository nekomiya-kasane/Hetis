/**
 * @file ExecutionGraph.h
 * @brief Immutable runtime-owned logical execution graph.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/GraphOperation.h>

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace Sora::Compute {

    class GraphBuilder;

    /** @brief Runtime-owned logical graph with one owning operation payload per node. */
    class ExecutionGraph {
    public:
        /** @brief Return the immutable graph without copying. */
        [[nodiscard]] GraphView View() const noexcept {
            return {.nodes = nodes_, .accesses = accesses_, .edges = edges_, .footprints = footprints_};
        }

        /** @brief Return the active node count. */
        [[nodiscard]] size_t NodeCount() const noexcept { return nodes_.size(); }

        /** @brief Return the active access count. */
        [[nodiscard]] size_t AccessCount() const noexcept { return accesses_.size(); }

        /** @brief Return the active dependency count. */
        [[nodiscard]] size_t EdgeCount() const noexcept { return edges_.size(); }

        /** @brief Return the interned non-contiguous footprint count. */
        [[nodiscard]] size_t FootprintCount() const noexcept { return footprints_.size(); }

        /** @brief Return the owning payload count, which is always equal to NodeCount(). */
        [[nodiscard]] size_t OperationCount() const noexcept { return operations_.size(); }

        /** @brief Return the owning operation of @p node. @pre @p node identifies this graph. */
        [[nodiscard]] const GraphOperation& Operation(GraphNodeId node) const noexcept {
            assert(node.Valid() && node.index < operations_.size());
            return operations_[node.index];
        }

    private:
        friend class GraphBuilder;

        ExecutionGraph(std::vector<GraphNode> nodes, std::vector<ResourceAccess> accesses,
                       std::vector<AffineByteFootprint> footprints, std::vector<GraphEdge> edges,
                       std::vector<GraphOperation> operations) noexcept
            : nodes_(std::move(nodes)),
              accesses_(std::move(accesses)),
              footprints_(std::move(footprints)),
              edges_(std::move(edges)),
              operations_(std::move(operations)) {
            assert(nodes_.size() == operations_.size());
        }

        std::vector<GraphNode> nodes_{};
        std::vector<ResourceAccess> accesses_{};
        std::vector<AffineByteFootprint> footprints_{};
        std::vector<GraphEdge> edges_{};
        std::vector<GraphOperation> operations_{};
    };

} // namespace Sora::Compute
