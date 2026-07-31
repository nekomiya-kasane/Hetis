/**
 * @file FusionOpportunity.h
 * @brief Pure structural discovery of semantics-preserving graph-fusion opportunities.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/ExecutionGraph.h>

#include <cstdint>
#include <expected>
#include <type_traits>
#include <vector>

namespace Sora::Compute {

    /** @brief One dispatch pair connected by a unique exactly matched intermediate buffer view. */
    struct ElementwiseFusionOpportunity {
        GraphNodeId producer{};
        GraphNodeId consumer{};
        KernelArgumentId producerOutput{};
        KernelArgumentId consumerInput{};
        BufferId intermediate{};

        friend constexpr bool operator==(ElementwiseFusionOpportunity, ElementwiseFusionOpportunity) noexcept = default;
    };

    /** @brief Stable failure while allocating structural fusion-analysis storage. */
    enum class FusionOpportunityError : uint8_t {
        OutOfMemory,
    };

    /**
     * @brief Discover canonical elementwise fusion opportunities without consulting compiler or runtime services.
     * @details An intermediate logical buffer must have exactly one producer write and one consumer read in the
     * complete graph, and the producer and consumer must have adjacent canonical node identifiers. This conservative
     * v1 rule excludes fanout, in-place updates, disjoint subregion sharing, and non-contiguous replacement regions.
     */
    [[nodiscard]] std::expected<std::vector<ElementwiseFusionOpportunity>, FusionOpportunityError>
    FindElementwiseFusionOpportunities(const ExecutionGraph& graph) noexcept;

    static_assert(std::is_trivially_copyable_v<ElementwiseFusionOpportunity>);

} // namespace Sora::Compute
