/**
 * @file AutomaticDispatchPlanningInternal.h
 * @brief Private automatic-partition search customization for runtime planners.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/AutomaticDispatchPlanning.h>

#include <concepts>
#include <expected>
#include <memory>
#include <type_traits>

namespace Sora::Compute::Detail {

    /** @brief Best exact partition found before applying a caller-specific acceptance threshold. */
    struct AutomaticPartitionSearchResult {
        ExecutionPlan singleDeviceBaseline{};
        std::optional<PartitionedDispatchExecution> bestPartition{};
        uint32_t partitionEvaluations = 0;
    };

    template<typename Scheduler>
    concept AutomaticPartitionScheduler = requires(Scheduler& scheduler, PartitionedDispatchExecution& execution) {
        { scheduler(execution) } -> std::same_as<std::expected<bool, AutomaticDispatchPlanningError>>;
    };

    /** @brief Synchronous non-owning scheduler reference used only during bounded partition search. */
    class AutomaticPartitionSchedulerRef final {
    public:
        template<AutomaticPartitionScheduler Scheduler>
        constexpr AutomaticPartitionSchedulerRef(Scheduler& scheduler) noexcept
            : state_(std::addressof(scheduler)),
              schedule_([](void* state, PartitionedDispatchExecution& execution) noexcept {
                  return (*static_cast<Scheduler*>(state))(execution);
              }) {}

        /** @brief Schedule one structurally valid partition candidate on the caller's exact resource timeline. */
        [[nodiscard]] std::expected<bool, AutomaticDispatchPlanningError>
        operator()(PartitionedDispatchExecution& execution) const noexcept {
            return schedule_(state_, execution);
        }

    private:
        using Schedule =
            std::expected<bool, AutomaticDispatchPlanningError> (*)(void*, PartitionedDispatchExecution&) noexcept;

        void* state_ = nullptr;
        Schedule schedule_ = nullptr;
    };

    /** @brief Search exact resident partitions using a caller-provided absolute timeline scheduler. */
    [[nodiscard]] std::expected<AutomaticPartitionSearchResult, AutomaticDispatchPlanningError>
    SearchAutomaticDispatchPartitions(const KernelInvocation& invocation, const DispatchCandidateSet& candidates,
                                      const GraphPlanningServices& services, const GraphResidencyState& residency,
                                      const AutomaticPartitionPolicy& policy,
                                      AutomaticPartitionSchedulerRef scheduler) noexcept;

    static_assert(std::is_trivially_copyable_v<AutomaticPartitionSchedulerRef>);

} // namespace Sora::Compute::Detail
