/**
 * @file GraphBuilder.h
 * @brief Transactional runtime construction of owning sparse execution graphs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/ExecutionGraph.h>

#include <cstddef>
#include <expected>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Runtime builder that accepts only typed owning operations and derives all logical effects. */
    class GraphBuilder final {
    public:
        GraphBuilder() = default;
        GraphBuilder(const GraphBuilder&) = delete;
        GraphBuilder& operator=(const GraphBuilder&) = delete;
        GraphBuilder(GraphBuilder&&) noexcept = default;
        GraphBuilder& operator=(GraphBuilder&&) noexcept = default;

        /** @brief Add one validated portable kernel invocation. */
        [[nodiscard]] std::expected<GraphNodeId, GraphError> AddDispatch(KernelInvocation invocation) noexcept;

        /** @brief Add one validated logical byte copy. */
        [[nodiscard]] std::expected<GraphNodeId, GraphError> AddCopy(CopyOperation operation) noexcept;

        /** @brief Add one validated logical byte fill. */
        [[nodiscard]] std::expected<GraphNodeId, GraphError> AddFill(FillOperation operation) noexcept;

        /** @brief Add one explicitly registered external operation. */
        [[nodiscard]] std::expected<GraphNodeId, GraphError> AddExternal(ExternalOperation operation) noexcept;

        /** @brief Add an explicit dependency between existing topologically ordered nodes. */
        [[nodiscard]] std::expected<void, GraphError> AddDependency(GraphNodeId predecessor,
                                                                    GraphNodeId successor) noexcept;

        /** @brief Return the current node count without exposing an incomplete edge view. */
        [[nodiscard]] size_t NodeCount() const noexcept { return nodes_.size(); }

        /** @brief Derive sparse hazards, canonicalize dependencies, and transfer all leases into an immutable graph. */
        [[nodiscard]] std::expected<ExecutionGraph, GraphError> Finish() && noexcept;

    private:
        [[nodiscard]] std::expected<GraphNodeId, GraphError> AddOperation(GraphOperation operation) noexcept;

        std::vector<GraphNode> nodes_{};
        std::vector<ResourceAccess> accesses_{};
        std::vector<AffineByteFootprint> footprints_{};
        std::vector<GraphEdge> explicitEdges_{};
        std::vector<GraphOperation> operations_{};
    };

} // namespace Sora::Compute
