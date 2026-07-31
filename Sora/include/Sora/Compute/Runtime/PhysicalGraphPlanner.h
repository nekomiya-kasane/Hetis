/**
 * @file PhysicalGraphPlanner.h
 * @brief Pure expansion of residency routes into physical graph plan fragments.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/ResidencyPlanner.h>
#include <Sora/Compute/Runtime/GraphResidency.h>
#include <Sora/Compute/Runtime/PhysicalGraphPlan.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>

namespace Sora::Compute {

    /** @brief Allocation and publication policy applied to residency route targets. */
    struct ResidencyPlanOptions {
        MemoryPropertyMask requiredProperties{};
        MemoryPropertyMask preferredProperties{};
        uint64_t alignment = 1;
        bool publishDestination = false;
    };

    /** @brief Exact physical segment emitted for one non-alias route hop. */
    struct PhysicalRouteTransfer {
        uint32_t hop = UINT32_MAX;
        PhysicalPlanSegmentId segment{};

        friend constexpr bool operator==(PhysicalRouteTransfer, PhysicalRouteTransfer) noexcept = default;
    };

    /** @brief Append-ready globally indexed fragment produced from one residency route. */
    struct ResidencyPlanFragment {
        PhysicalGraphPlanStorage storage{};
        PlannedStorageRef destination{};
        std::vector<PhysicalRouteTransfer> transfers{};
        uint64_t estimatedNanoseconds = 0;
    };

    /** @brief Append-ready fragment whose final asynchronous hop directly fills one assembly part. */
    struct ResidencyPrefillFragment {
        PhysicalGraphPlanStorage storage{};
        PhysicalPlanSegmentId producer{};
        std::vector<PhysicalRouteTransfer> transfers{};
        uint64_t estimatedNanoseconds = 0;
    };

    /** @brief Stable failure while purely expanding an already selected residency route. */
    enum class PhysicalGraphPlanningError : uint8_t {
        InvalidRoute,
        InvalidGraphLocalBinding,
        MissingGraphLocalBinding,
        DuplicateGraphLocalBinding,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Exact correspondence between one graph-local subcoverage and its planned physical storage. */
    struct GraphLocalStorageBinding {
        GraphLocalResidencySource symbolic{};
        PlannedAllocationRef physical{};
    };

    static_assert(std::is_nothrow_move_constructible_v<GraphLocalStorageBinding> &&
                  std::is_nothrow_move_assignable_v<GraphLocalStorageBinding>);

    /** @brief Sorted allocation-free lookup from graph-local residency to exact physical plan storage. */
    class GraphLocalStorageMap final {
    public:
        /** @brief Insert one unique exact symbolic-to-physical correspondence with the strong guarantee. */
        [[nodiscard]] std::expected<void, PhysicalGraphPlanningError> Bind(GraphLocalStorageBinding binding) noexcept;

        /** @brief Find the exact physical storage bound to @p source in logarithmic time. */
        [[nodiscard]] const PlannedAllocationRef* Find(const GraphLocalResidencySource& source) const noexcept;

        /** @brief Return the number of bound graph-local replicas. */
        [[nodiscard]] size_t Size() const noexcept { return bindings_.size(); }

    private:
        std::vector<GraphLocalStorageBinding> bindings_{};
    };

    /** @brief Expand coherent imports and asynchronous hops without invoking any backend callback. */
    [[nodiscard]] std::expected<ResidencyPlanFragment, PhysicalGraphPlanningError>
    PlanPhysicalResidency(Buffer buffer, GraphNodeId logicalConsumer, ResidencyRoute route,
                          ResidencyPlanOptions options = {}) noexcept;

    /** @brief Resolve a graph-level source and expand its route after an existing physical plan prefix. */
    [[nodiscard]] std::expected<ResidencyPlanFragment, PhysicalGraphPlanningError>
    PlanPhysicalResidency(Buffer buffer, GraphNodeId logicalConsumer, GraphResidencyRoute route,
                          const PhysicalGraphPlanStorage& prefix, const GraphLocalStorageMap& graphLocal,
                          ResidencyPlanOptions options = {}) noexcept;

    /** @brief Redirect the final asynchronous hop into an existing dispatch-input assembly allocation. */
    [[nodiscard]] std::expected<ResidencyPrefillFragment, PhysicalGraphPlanningError>
    PlanPhysicalResidencyPrefill(Buffer buffer, GraphNodeId logicalConsumer, GraphResidencyRoute route,
                                 const PhysicalGraphPlanStorage& prefix, const GraphLocalStorageMap& graphLocal,
                                 PlannedAllocationId destination, ResidencyPlanOptions options = {}) noexcept;

} // namespace Sora::Compute
