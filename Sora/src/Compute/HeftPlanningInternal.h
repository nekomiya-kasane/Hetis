/**
 * @file HeftPlanningInternal.h
 * @brief Shared private arithmetic, error mapping, and rank-building declarations for HEFT planning.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/HeftPlanner.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

namespace Sora::Compute::HeftDetail {

    [[nodiscard]] constexpr HeftPlanningError Error(HeftPlanningErrorCode code, GraphNodeId node = {},
                                                    KernelArgumentId argument = {}, uint64_t cause = 0) noexcept {
        return {.code = code, .node = node, .argument = argument, .cause = cause};
    }

    [[nodiscard]] inline HeftPlanningError EnumerationError(const DispatchCandidateEnumerationError& error) noexcept {
        HeftPlanningErrorCode code = HeftPlanningErrorCode::CandidateEnumerationFailed;
        if (error.code == DispatchCandidateEnumerationErrorCode::CapacityExceeded) {
            code = HeftPlanningErrorCode::CapacityExceeded;
        } else if (error.code == DispatchCandidateEnumerationErrorCode::OutOfMemory) {
            code = HeftPlanningErrorCode::OutOfMemory;
        }
        return Error(code, error.node, error.argument,
                     error.cause != 0 ? error.cause : static_cast<uint64_t>(error.code));
    }

    [[nodiscard]] inline std::expected<uint64_t, HeftPlanningError> Add(uint64_t left, uint64_t right, GraphNodeId node,
                                                                        HeftPlanningErrorCode code) noexcept {
        if (right > std::numeric_limits<uint64_t>::max() - left) {
            return std::unexpected(Error(code, node));
        }
        return left + right;
    }

    [[nodiscard]] inline std::expected<uint64_t, HeftPlanningError> ServiceDuration(const CostBreakdown& cost,
                                                                                    GraphNodeId node) noexcept {
        uint64_t result = 0;
        for (const uint64_t component :
             {cost.launchNanoseconds, cost.computeNanoseconds, cost.synchronizationNanoseconds,
              cost.amortizedExecutableImportNanoseconds, cost.amortizedExecutableCompilationNanoseconds,
              cost.amortizedPipelineImportNanoseconds, cost.amortizedPipelineCreationNanoseconds}) {
            auto added = Add(result, component, node, HeftPlanningErrorCode::CapacityExceeded);
            if (!added) {
                return std::unexpected(added.error());
            }
            result = *added;
        }
        return result;
    }

    [[nodiscard]] std::expected<std::vector<uint64_t>, HeftPlanningError>
    BuildUpwardRanks(const ExecutionGraph& graph, const GraphPlanningContext& context, GraphPlanningServices services,
                     const HeftPlanningPolicy& policy);

} // namespace Sora::Compute::HeftDetail
