/**
 * @file GraphBuilder.cpp
 * @brief Owning runtime graph construction and sparse dependency finalization.
 * @ingroup Compute
 */

#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/Graph/HazardAnalysis.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Sora::Compute {

    std::expected<GraphNodeId, GraphError> GraphBuilder::AddDispatch(KernelInvocation invocation) noexcept {
        return AddOperation(GraphOperation{std::move(invocation)});
    }

    std::expected<GraphNodeId, GraphError> GraphBuilder::AddCopy(CopyOperation operation) noexcept {
        return AddOperation(GraphOperation{std::move(operation)});
    }

    std::expected<GraphNodeId, GraphError> GraphBuilder::AddFill(FillOperation operation) noexcept {
        return AddOperation(GraphOperation{std::move(operation)});
    }

    std::expected<GraphNodeId, GraphError> GraphBuilder::AddExternal(ExternalOperation operation) noexcept {
        return AddOperation(GraphOperation{std::move(operation)});
    }

    std::expected<GraphNodeId, GraphError> GraphBuilder::AddOperation(GraphOperation operation) noexcept {
        const GraphNodeSpec spec = DescribeGraphOperation(operation);
        const std::span<const ResourceAccess> operationAccesses = GraphOperationAccesses(operation);
        const std::span<const AffineByteFootprint> operationFootprints = GraphOperationFootprints(operation);
        if (nodes_.size() == std::numeric_limits<uint32_t>::max() ||
            operationAccesses.size() > std::numeric_limits<uint32_t>::max() - accesses_.size()) {
            return std::unexpected(GraphError{GraphErrorCode::CapacityExceeded, static_cast<uint32_t>(nodes_.size())});
        }

        GraphNode candidate{.kind = spec.kind,
                            .operation = spec.operation,
                            .workItemCount = spec.workItemCount,
                            .firstAccess = static_cast<uint32_t>(accesses_.size()),
                            .accessCount = static_cast<uint32_t>(operationAccesses.size())};
        GraphView candidateView{
            .nodes = {&candidate, 1}, .accesses = operationAccesses, .edges = {}, .footprints = operationFootprints};
        candidate.firstAccess = 0;
        if (auto validation = ValidateGraphStructure(candidateView); !validation) {
            return std::unexpected(GraphError{validation.error().code, static_cast<uint32_t>(nodes_.size())});
        }
        candidate.firstAccess = static_cast<uint32_t>(accesses_.size());

        try {
            std::vector<ResourceAccess> stagedAccesses;
            std::vector<AffineByteFootprint> stagedFootprints;
            stagedAccesses.reserve(operationAccesses.size());
            stagedFootprints.reserve(operationFootprints.size());
            for (ResourceAccess access : operationAccesses) {
                if (access.footprint.Valid()) {
                    const AffineByteFootprint& footprint = operationFootprints[access.footprint.index];
                    const auto interned = std::ranges::find(footprints_, footprint);
                    if (interned != footprints_.end()) {
                        access.footprint.index = static_cast<uint32_t>(interned - footprints_.begin());
                    } else {
                        const auto staged = std::ranges::find(stagedFootprints, footprint);
                        const size_t localIndex = staged == stagedFootprints.end()
                                                      ? stagedFootprints.size()
                                                      : static_cast<size_t>(staged - stagedFootprints.begin());
                        if (staged == stagedFootprints.end()) {
                            stagedFootprints.push_back(footprint);
                        }
                        if (footprints_.size() >= kInvalidByteFootprintIndex ||
                            localIndex >= kInvalidByteFootprintIndex - footprints_.size()) {
                            return std::unexpected(
                                GraphError{GraphErrorCode::CapacityExceeded, static_cast<uint32_t>(nodes_.size())});
                        }
                        access.footprint.index = static_cast<uint32_t>(footprints_.size() + localIndex);
                    }
                }
                stagedAccesses.push_back(access);
            }

            nodes_.reserve(nodes_.size() + 1);
            operations_.reserve(operations_.size() + 1);
            accesses_.reserve(accesses_.size() + stagedAccesses.size());
            footprints_.reserve(footprints_.size() + stagedFootprints.size());
            footprints_.insert(footprints_.end(), stagedFootprints.begin(), stagedFootprints.end());
            accesses_.insert(accesses_.end(), stagedAccesses.begin(), stagedAccesses.end());
            operations_.push_back(std::move(operation));
            nodes_.push_back(candidate);
            return GraphNodeId{static_cast<uint32_t>(nodes_.size() - 1)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError{GraphErrorCode::OutOfMemory, static_cast<uint32_t>(nodes_.size())});
        }
    }

    std::expected<void, GraphError> GraphBuilder::AddDependency(GraphNodeId predecessor,
                                                                GraphNodeId successor) noexcept {
        if (!predecessor.Valid() || !successor.Valid() || predecessor.index >= nodes_.size() ||
            successor.index >= nodes_.size() || predecessor.index >= successor.index) {
            return std::unexpected(GraphError{GraphErrorCode::InvalidDependency, successor.index, predecessor.index});
        }
        for (const GraphEdge& edge : explicitEdges_) {
            if (edge.predecessor == predecessor && edge.successor == successor) {
                return {};
            }
        }
        if (explicitEdges_.size() == explicitEdges_.max_size()) {
            return std::unexpected(GraphError{GraphErrorCode::CapacityExceeded, successor.index, predecessor.index});
        }
        try {
            explicitEdges_.push_back({.predecessor = predecessor, .successor = successor});
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError{GraphErrorCode::OutOfMemory, successor.index, predecessor.index});
        }
    }

    std::expected<ExecutionGraph, GraphError> GraphBuilder::Finish() && noexcept {
        GraphView source{.nodes = nodes_, .accesses = accesses_, .edges = explicitEdges_, .footprints = footprints_};
        auto hazards = DeriveHazardDependencies(source);
        if (!hazards) {
            return std::unexpected(hazards.error());
        }

        try {
            if (hazards->size() > explicitEdges_.max_size() - explicitEdges_.size()) {
                return std::unexpected(GraphError{GraphErrorCode::CapacityExceeded});
            }
            explicitEdges_.reserve(explicitEdges_.size() + hazards->size());
            explicitEdges_.insert(explicitEdges_.end(), hazards->begin(), hazards->end());
            std::ranges::sort(explicitEdges_, GraphEdgeLess);
            const auto uniqueEnd = std::ranges::unique(explicitEdges_).begin();
            explicitEdges_.erase(uniqueEnd, explicitEdges_.end());
            source.edges = explicitEdges_;
            return ExecutionGraph{std::move(nodes_), std::move(accesses_), std::move(footprints_),
                                  std::move(explicitEdges_), std::move(operations_)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError{GraphErrorCode::OutOfMemory});
        }
    }

} // namespace Sora::Compute
