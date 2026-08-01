/**
 * @file GraphPlanner.h
 * @brief Compile-time protocol for backend-independent logical-to-physical
 * graph planning.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/ArtifactCache.h>
#include <Sora/Compute/Graph/ExecutionGraph.h>
#include <Sora/Compute/Runtime/AutomaticPartitionPlanningCalibration.h>
#include <Sora/Compute/Runtime/ExecutableGraph.h>
#include <Sora/Compute/Runtime/PerformanceModel.h>
#include <Sora/Compute/Runtime/PerformancePrior.h>
#include <Sora/Compute/Runtime/PhysicalGraphPlan.h>
#include <Sora/Compute/Runtime/PhysicalResourceInventory.h>
#include <Sora/Compute/Runtime/ProgramCatalog.h>
#include <Sora/Compute/Runtime/RuntimeEpoch.h>

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Restricted immutable runtime facts visible during one pure planning
     * invocation. */
    struct GraphPlanningContext {
        uint64_t epochSequence = 0;
        std::span<const RuntimeDeviceBinding> devices{};
        std::span<const DeviceSnapshot> snapshots{};
        const GraphSubmissionError* previousSubmission = nullptr;
        uint32_t attempt = 0;
    };

    /** @brief Context-owned services available to one pure physical-planning
     * invocation. */
    struct GraphPlanningServices {
        const ProgramCatalogSnapshot& packages;
        const PhysicalResourceInventory& resources;
        const ArtifactAvailabilitySnapshot& artifacts;
        const PerformanceSnapshot& performance;
        const PerformancePriorModel* priors = nullptr;
        AutomaticPartitionPlanningCalibrationBatch* automaticPartitionCalibration = nullptr;
    };

    /** @brief One pure physical plan paired with the planner objective that selected it. */
    struct GraphPlanSelection {
        PhysicalGraphPlanStorage storage{};
        uint64_t predictedMakespanNanoseconds = 0;
    };

    /** @brief Expected-like result whose success value is one physical graph-plan selection. */
    template<typename Result>
    concept GraphPlanSelectionResult = requires(Result& result) {
        typename Result::value_type;
        typename Result::error_type;
        requires std::same_as<typename Result::value_type, GraphPlanSelection>;
        { result.has_value() } noexcept -> std::same_as<bool>;
        { *result } -> std::same_as<GraphPlanSelection&>;
        { result.error() } -> std::same_as<typename Result::error_type&>;
    };

    /**
     * @brief Thread-safe noexcept logical-to-physical planning protocol used once
     * per fresh graph attempt.
     * @details Implementations may update bounded telemetry, but must not allocate
     * backend resources, compile, or submit.
     */
    template<typename Planner>
    concept GraphPlanningEngine = requires(Planner& planner, const ExecutionGraph& graph,
                                           const GraphPlanningContext& context, GraphPlanningServices services) {
        requires GraphPlanSelectionResult<
            std::invoke_result_t<Planner&, const ExecutionGraph&, const GraphPlanningContext&, GraphPlanningServices>>;
        requires std::is_nothrow_invocable_v<Planner&, const ExecutionGraph&, const GraphPlanningContext&,
                                             GraphPlanningServices>;
        requires std::is_nothrow_move_constructible_v<typename std::invoke_result_t<
            Planner&, const ExecutionGraph&, const GraphPlanningContext&, GraphPlanningServices>::error_type>;
        { std::invoke(planner, graph, context, services) } noexcept;
    };

    static_assert(std::is_nothrow_move_constructible_v<GraphPlanSelection>);

} // namespace Sora::Compute
