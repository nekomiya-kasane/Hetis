/**
 * @file HazardAnalysis.h
 * @brief Sparse exact-coverage dependency analysis for runtime execution graphs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/GraphModel.h>

#include <expected>
#include <vector>

namespace Sora::Compute {

    /**
     * @brief Derive the canonical minimal hazard frontier for @p graph.
     * @details Reads depend on the last overlapping writer. Writes depend on overlapping readers since the last
     * writer, or on that writer when no reader intervened. Affine coverage is projected into at most 64 canonical
     * intervals; larger sparse sets conservatively use their bounding interval. Runtime remains bounded by frontier
     * updates and emitted dependencies rather than all node pairs or logical element count.
     */
    [[nodiscard]] std::expected<std::vector<GraphEdge>, GraphError> DeriveHazardDependencies(GraphView graph) noexcept;

    /**
     * @brief Validate a runtime graph against its sparse canonical hazard frontier.
     * @details Every required frontier edge must be present directly. Additional explicit topological dependencies
     * are allowed; duplicate edges are rejected.
     */
    [[nodiscard]] std::expected<void, GraphError> ValidateSparseGraph(GraphView graph) noexcept;

} // namespace Sora::Compute
