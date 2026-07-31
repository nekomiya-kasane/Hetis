/**
 * @file AutomaticDispatchExpansion.h
 * @brief Transfer-inclusive automatic-dispatch expansion into physical graph segments.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/AutomaticDispatchPlanning.h>
#include <Sora/Compute/Runtime/DispatchGraphPlanning.h>

#include <cstdint>
#include <expected>

namespace Sora::Compute {

    /** @brief Why physical lowering retained the automatic decision's single-device counterfactual. */
    enum class AutomaticDispatchFallback : uint8_t {
        None,
        SingleDeviceSelected,
        PartitionFootprintUnavailable,
        PartitionInputResidencyUnavailable,
    };

    /** @brief Pure physical storage plus an explicit record of whether shard expansion occurred. */
    struct AutomaticDispatchGraphLowering {
        PhysicalGraphPlanStorage storage{};
        AutomaticDispatchFallback fallback = AutomaticDispatchFallback::None;

        /** @brief Return whether one logical dispatch became multiple physical dispatches. */
        [[nodiscard]] bool UsesPartitioning() const noexcept { return fallback == AutomaticDispatchFallback::None; }
    };

    /** @brief Stable failure before physical allocation, compilation, import, or backend submission. */
    enum class AutomaticDispatchExpansionErrorCode : uint8_t {
        InvalidRequest,
        UnsupportedGraph,
        CandidateMismatch,
        ProgramUnavailable,
        CapacityExceeded,
        BaselineLoweringFailed,
        OutOfMemory,
    };

    /** @brief Automatic expansion failure with the first logical node, argument, and nested cause. */
    struct AutomaticDispatchExpansionError {
        AutomaticDispatchExpansionErrorCode code = AutomaticDispatchExpansionErrorCode::InvalidRequest;
        GraphNodeId node{};
        KernelArgumentId argument{};
        uint64_t cause = 0;

        friend constexpr bool operator==(AutomaticDispatchExpansionError,
                                         AutomaticDispatchExpansionError) noexcept = default;
    };

    /**
     * @brief Lower one automatic resident dispatch decision into its selected physical execution.
     * @details This first graph-rewrite slice accepts one logical dispatch node. A selected partition becomes one
     * physical dispatch per strict shard with joint PartitionedWrite publication. If a buffer view's shard footprints
     * cannot be proven to form the registered exact logical coverage within bounded work, lowering transparently
     * retains the validated single-device baseline. Input residency that cannot map to one ready allocation per shard
     * is reported separately from an unprovable write footprint.
     */
    [[nodiscard]] std::expected<AutomaticDispatchGraphLowering, AutomaticDispatchExpansionError>
    LowerAutomaticDispatchGraph(const ExecutionGraph& graph, GraphPlanningServices services,
                                const DestinationMemoryPolicy& memoryPolicy, const GraphResidencyState& baseline,
                                const DispatchCandidateSet& candidates,
                                const AutomaticDispatchExecution& execution) noexcept;

} // namespace Sora::Compute
