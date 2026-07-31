/**
 * @file DispatchGraphPlanning.h
 * @brief Shared validation and pure physical lowering for selected dispatch DAGs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>
#include <Sora/Compute/Runtime/GraphPlanner.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable failure shared by dispatch-only graph planners after placement selection. */
    enum class DispatchGraphPlanningErrorCode : uint8_t {
        InvalidGraph,
        UnsupportedOperation,
        UnsupportedBinding,
        InvalidSelection,
        ResidencyLoweringFailed,
        ProgramUnavailable,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Shared dispatch-graph failure retaining the first logical node, argument, and nested cause. */
    struct DispatchGraphPlanningError {
        DispatchGraphPlanningErrorCode code = DispatchGraphPlanningErrorCode::InvalidGraph;
        GraphNodeId node{};
        KernelArgumentId argument{};
        uint64_t cause = 0;

        friend constexpr bool operator==(DispatchGraphPlanningError, DispatchGraphPlanningError) noexcept = default;
    };

    /** @brief One physical dispatch shard selected for a logical dispatch node. */
    struct DispatchGraphShardSelection {
        DispatchSlice slice{};
        PlacementCandidate placement{};
        DispatchResidencyProjection residency{};
        DispatchGeometry geometry{};
    };

    /**
     * @brief One strict logical cover whose physical shards publish one logical value transition.
     * @details @p shards must contain at least two entries and form the canonical strict cover admitted by the
     * invocation's partition contract. Every shard preserves the schema of @p logicalBindings while replacing its
     * exact coverage, placement, route projection, and geometry.
     */
    struct DispatchGraphPartitionSelection {
        uint8_t axis = 0;
        std::vector<DispatchMemoryBinding> logicalBindings{};
        std::vector<DispatchGraphShardSelection> shards{};
    };

    /** @brief Closed whole-or-partitioned execution selected for one logical dispatch node. */
    using DispatchGraphExecution = std::variant<DispatchGraphShardSelection, DispatchGraphPartitionSelection>;

    /** @brief One logical dispatch selection with exactly one whole execution or one strict physical partition. */
    struct DispatchGraphSelection {
        GraphNodeId node{};
        WorkloadDesc workload{};
        DispatchGraphExecution execution{};
    };

    /** @brief Stable reference to one asynchronous hop in a selected dispatch input route. */
    struct DispatchTransferRef {
        GraphNodeId consumer{};
        BufferId buffer{};
        uint32_t dispatch = 0;
        uint32_t route = std::numeric_limits<uint32_t>::max();
        uint32_t hop = std::numeric_limits<uint32_t>::max();

        /** @brief Return whether the logical consumer, physical dispatch, buffer, route, and hop are complete. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return consumer.Valid() && buffer.Valid() && route != std::numeric_limits<uint32_t>::max() &&
                   hop != std::numeric_limits<uint32_t>::max();
        }

        friend constexpr bool operator==(DispatchTransferRef, DispatchTransferRef) noexcept = default;
        friend constexpr auto operator<=>(DispatchTransferRef, DispatchTransferRef) noexcept = default;
    };

    /** @brief Required physical order between two planner-scheduled asynchronous transfer hops. */
    struct DispatchTransferConstraint {
        DispatchTransferRef predecessor{};
        DispatchTransferRef successor{};

        /** @brief Return whether both distinct transfer references are complete. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return predecessor.Valid() && successor.Valid() && predecessor != successor;
        }

        friend constexpr bool operator==(DispatchTransferConstraint, DispatchTransferConstraint) noexcept = default;
    };

    /** @brief Return a deterministic total-order comparison over placement identity. */
    [[nodiscard]] bool StableDispatchPlacementLess(const PlacementCandidate& left,
                                                   const PlacementCandidate& right) noexcept;

    /**
     * @brief Validate the dispatch-only affine-buffer graph class shared by exact-chain and DAG planners.
     * @details Every buffer binding must be unique, lie within its logical coverage, and use either Read or Write
     * access. Every dispatch must write at least one buffer. Copy, fill, external, duplicate, and ReadWrite operations
     * are rejected; partial final publication is validated during lowering because it depends on publication policy.
     */
    [[nodiscard]] std::expected<void, DispatchGraphPlanningError>
    ValidateDispatchGraph(const ExecutionGraph& graph) noexcept;

    /** @brief Validate dependencies and stably reorder physical storage into canonical topological order. */
    [[nodiscard]] std::expected<void, DispatchGraphPlanningError>
    CanonicalizePhysicalGraphPlanStorage(PhysicalGraphPlanStorage& storage) noexcept;

    /**
     * @brief Replay a complete topologically ordered selection and lower it to pure physical-plan storage.
     * @details Logical dependencies, graph-produced storage, selected residency transfers, exact-queue ordering, and
     * optional asynchronous-hop constraints become explicit physical dependencies. The resulting segments are
     * stably topologically ordered. No backend callback, compilation, allocation, or submission occurs.
     */
    [[nodiscard]] std::expected<PhysicalGraphPlanStorage, DispatchGraphPlanningError>
    LowerDispatchGraph(const ExecutionGraph& graph, GraphPlanningServices services,
                       const DestinationMemoryPolicy& memoryPolicy, const GraphResidencyState& baseline,
                       std::span<const DispatchGraphSelection> selections,
                       std::span<const DispatchTransferConstraint> transferConstraints = {}) noexcept;

    static_assert(std::is_nothrow_move_constructible_v<DispatchGraphPlanningError>);
    static_assert(std::is_trivially_copyable_v<DispatchTransferRef>);
    static_assert(std::is_trivially_copyable_v<DispatchTransferConstraint>);

} // namespace Sora::Compute
