/**
 * @file AutomaticDispatchPlanning.h
 * @brief Pure single-device versus resident multi-device dispatch planning.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Invocation/DispatchPartition.h>
#include <Sora/Compute/Planner.h>
#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>
#include <Sora/Compute/Runtime/RouteTransferScheduling.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace Sora::Compute {

    inline constexpr uint32_t kMaximumAutomaticPartitionDevices = 64;
    inline constexpr uint8_t kAllAutomaticPartitionAxes = static_cast<uint8_t>((uint8_t{1} << kMaximumIndexRank) - 1U);

    /**
     * @brief Bounded controls and calibrated fixed costs for automatic dispatch partitioning.
     * @details @p allowedAxisMask optionally narrows the semantic axes admitted by the kernel contract.
     * @p maximumPartitionEvaluations is one hard total budget shared by every axis, initial exact covers, device-set
     * proposals, and current or speculative boundary refinements. No hidden per-axis or per-neighborhood budget
     * applies. Transfer-inclusive planning requires at least one evaluation per admitted non-degenerate axis; narrow
     * @p allowedAxisMask when a smaller budget is intentional.
     */
    struct AutomaticPartitionPolicy {
        PlacementPolicy placement{};
        bool enabled = true;
        uint8_t allowedAxisMask = kAllAutomaticPartitionAxes;
        uint32_t maximumDevices = 8;
        uint32_t maximumPartitionEvaluations = 64;
        uint32_t minimumImprovementPermille = 50;
        uint64_t partitionOverheadNanoseconds = 0;
        uint64_t publicationNanoseconds = 0;
        uint64_t minimumAmortizedBenefitNanoseconds = 0;

        /** @brief Return whether the search and acceptance bounds are meaningful. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return !enabled || ((allowedAxisMask & static_cast<uint8_t>(~kAllAutomaticPartitionAxes)) == 0 &&
                                maximumDevices >= 2 && maximumDevices <= kMaximumAutomaticPartitionDevices &&
                                maximumPartitionEvaluations != 0 && minimumImprovementPermille < 1000);
        }
    };

    /** @brief One non-empty logical slice assigned to one exact runtime placement. */
    struct PartitionShardExecution {
        DispatchSlice slice{};
        ExecutionPlan execution{};
        DispatchResidencyProjection residency{};
        std::vector<uint64_t> routeReadyNanoseconds{};
        uint64_t inputReadyNanoseconds = 0;
        uint64_t startNanoseconds = 0;
        uint64_t finishNanoseconds = 0;
    };

    /** @brief Parallel shard schedule on one caller-chosen timeline. */
    struct PartitionedDispatchExecution {
        uint8_t axis = 0;
        std::vector<PartitionShardExecution> shards{};
        std::vector<ScheduledRouteTransfer> transfers{};
        uint32_t partitionEvaluations = 0;
        uint64_t transferReadyNanoseconds = 0;
        uint64_t parallelNanoseconds = 0;
        uint64_t partitionOverheadNanoseconds = 0;
        uint64_t publicationNanoseconds = 0;
        uint64_t totalNanoseconds = 0;
    };

    /** @brief Automatic decision retaining the best single-device counterfactual for diagnostics. */
    struct AutomaticDispatchExecution {
        ExecutionPlan singleDeviceBaseline{};
        std::optional<PartitionedDispatchExecution> partitioned{};

        /** @brief Return whether execution should expand into multiple physical dispatches. */
        [[nodiscard]] bool UsesPartitioning() const noexcept { return partitioned.has_value(); }

        /** @brief Return the selected predicted end-to-end duration. */
        [[nodiscard]] uint64_t SelectedNanoseconds() const noexcept {
            return partitioned ? partitioned->totalNanoseconds : singleDeviceBaseline.cost.totalNanoseconds;
        }
    };

    /** @brief Stable failure before any backend callback or physical resource acquisition. */
    enum class AutomaticDispatchPlanningError : uint8_t {
        InvalidRequest,
        InsufficientSearchBudget,
        NoSingleDeviceCandidate,
        OutOfMemory,
    };

    /**
     * @brief Select a calibrated single-device dispatch or a faster resident multi-device strict cover.
     * @details This low-level overload admits only transfer-free partition candidates. It remains useful for pure
     * compute studies; runtime orchestration should use the exact-residency overload below.
     */
    [[nodiscard]] std::expected<AutomaticDispatchExecution, AutomaticDispatchPlanningError>
    PlanAutomaticDispatch(const DispatchDomain& domain, PartitionContract contract, const WorkloadDesc& workload,
                          std::span<const PlacementCandidate> candidates,
                          const AutomaticPartitionPolicy& policy = {}) noexcept;

    /**
     * @brief Select a single-device baseline or an exact transfer-inclusive multi-device strict cover.
     * @details Each selected slice is projected onto its exact affine buffer subviews, rerouted from the frozen
     * residency state, and scheduled with service-scoped transfer-engine contention. Fixed hop latency, coherent
     * aliases, route topology, and dispatch tails remain explicit; no whole-transfer proportional estimate is used.
     */
    [[nodiscard]] std::expected<AutomaticDispatchExecution, AutomaticDispatchPlanningError>
    PlanAutomaticDispatch(const KernelInvocation& invocation, const DispatchCandidateSet& candidates,
                          const GraphPlanningServices& services, const GraphResidencyState& residency,
                          const AutomaticPartitionPolicy& policy = {}) noexcept;

    static_assert(std::is_trivially_copyable_v<AutomaticPartitionPolicy>);

} // namespace Sora::Compute
