/**
 * @file LinearChainPlanner.h
 * @brief Exact dynamic-programming placement for the closed linear dispatch
 * graph class.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>
#include <Sora/Compute/Runtime/GraphPlanner.h>

#include <cstdint>
#include <expected>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Policies applied uniformly to every transition in one linear-chain planning attempt. */
    struct LinearChainPlanningPolicy {
        DispatchEnumerationPolicy dispatch = [] {
            DispatchEnumerationPolicy value;
            value.memory.publication = DestinationPublication::Publish;
            return value;
        }();
        PlacementPolicy placement{};
        uint32_t maximumFrontierStates = 4096;

        /** @brief Return whether the exact frontier has a nonzero hard capacity. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return maximumFrontierStates != 0; }
    };

    /** @brief Stable stage at which exact linear-chain planning failed. */
    enum class LinearChainPlanningErrorCode : uint8_t {
        InvalidPolicy,
        InvalidGraph,
        NotLinearChain,
        UnsupportedOperation,
        UnsupportedBinding,
        ResidencySnapshotFailed,
        CandidateEnumerationFailed,
        NoCandidate,
        ResidencyTransitionFailed,
        ResidencyLoweringFailed,
        ProgramUnavailable,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Linear-chain failure retaining the first logical node, argument, and
     * nested stable cause. */
    struct LinearChainPlanningError {
        LinearChainPlanningErrorCode code = LinearChainPlanningErrorCode::InvalidGraph;
        GraphNodeId node{};
        KernelArgumentId argument{};
        uint64_t cause = 0;

        friend constexpr bool operator==(LinearChainPlanningError, LinearChainPlanningError) noexcept = default;
    };

    /**
     * @brief Pure exact planner for dispatch-only graphs whose dependency set is
     * precisely the node-order chain.
     * @details Every buffer binding must be unique, denote exact affine subcoverage, and use either Read or Write
     * access. Partial final writes must remain graph-local because whole-value publication requires exact logical
     * coverage. Each dispatch must write at least one buffer. Dynamic-programming states merge only when their complete
     * canonical symbolic residency is equal; exceeding @c maximumFrontierStates returns @c CapacityExceeded rather
     * than approximating the state space. The planner performs no backend callback, compilation, allocation, or
     * submission.
     */
    class LinearChainPlanner final {
    public:
        constexpr explicit LinearChainPlanner(LinearChainPlanningPolicy policy = {}) noexcept : policy_(policy) {}

        /** @brief Select the globally minimum additive calibrated path and lower it
         * to pure physical-plan storage. */
        [[nodiscard]] std::expected<PhysicalGraphPlanStorage, LinearChainPlanningError>
        operator()(const ExecutionGraph& graph, const GraphPlanningContext& context,
                   GraphPlanningServices services) const noexcept;

    private:
        LinearChainPlanningPolicy policy_{};
    };

    static_assert(GraphPlanningEngine<LinearChainPlanner>);
    static_assert(std::is_nothrow_move_constructible_v<LinearChainPlanningError>);

} // namespace Sora::Compute
