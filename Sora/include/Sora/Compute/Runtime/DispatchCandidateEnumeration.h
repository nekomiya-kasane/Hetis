/**
 * @file DispatchCandidateEnumeration.h
 * @brief Exact residency-aware candidate enumeration for one logical dispatch
 * node.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/ExecutionGraph.h>
#include <Sora/Compute/Invocation/DispatchPartition.h>
#include <Sora/Compute/Runtime/CandidateSynthesis.h>
#include <Sora/Compute/Runtime/GraphResidency.h>
#include <Sora/Compute/Runtime/GraphPlanner.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace Sora::Compute {

    /** @brief Whether a selected destination remains graph-local or must be
     * published after completion. */
    enum class DestinationPublication : uint8_t {
        GraphLocal,
        Publish,
    };

    /**
     * @brief Hard admissibility constraints and deterministic preferences for
     * dispatch-visible memory.
     * @details Invalid service, device, and location identities mean unconstrained.
     * Preferred locations form an ordered list; memory-property coverage and stable
     * location identity break ties after that order.
     */
    struct DestinationMemoryPolicy {
        BackendServiceId service{};
        DeviceId device{};
        std::optional<BackendKind> backend{};
        MemoryLocation requiredLocation{};
        std::span<const MemoryLocation> preferredLocations{};
        MemoryPropertyMask requiredProperties{};
        MemoryPropertyMask preferredProperties{MemoryProperty::DeviceLocal};
        bool allowCoherentAlias = true;
        DestinationPublication publication = DestinationPublication::GraphLocal;
    };

    /** @brief Candidate-independent dispatch controls participating in
     * specialization and cost estimation. */
    struct DispatchEnumerationPolicy {
        DestinationMemoryPolicy memory{};
        SpecializationKey specialization{};
        uint64_t expectedReuseCount = 1;
        bool deterministic = true;
    };

    /** @brief One signature-ordered logical buffer binding projected onto a
     * destination. */
    struct DispatchMemoryBinding {
        KernelArgumentId argument{};
        AccessMode access = AccessMode::Read;
        Buffer buffer{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t alignment = 1;
        DestinationPublication publication = DestinationPublication::GraphLocal;
    };

    /**
     * @brief Queue-independent memory projection shared by every placement on one
     * service and device.
     * @details Scalar-only dispatches have no destination memory descriptor and an
     * exact zero transfer cost.
     */
    struct DispatchResidencyProjection {
        BackendServiceId service{};
        DeviceId device{};
        std::optional<PhysicalMemoryDescriptor> destination{};
        std::vector<DispatchMemoryBinding> bindings{};
        std::vector<GraphResidencyRoute> inputs{};
        CandidateTransferCost transfer{};
        bool allowCoherentAlias = true;
    };

    /** @brief One calibrated queue placement referring to shared residency state by
     * stable local index. */
    struct DispatchPlacementCandidate {
        PlacementCandidate placement{};
        uint32_t residencyProjection = UINT32_MAX;
    };

    /** @brief One calibrated placement and its selected destination before input residency is projected. */
    struct DispatchResourceCandidate {
        PlacementCandidate placement{};
        std::optional<PhysicalMemoryDescriptor> destination{};
    };

    /** @brief Residency-independent resource candidates used by DAG ranking and exact route projection. */
    struct DispatchResourceCandidateSet {
        GraphNodeId node{};
        WorkloadDesc workload{};
        DispatchGeometry geometry{};
        std::vector<DispatchMemoryBinding> bindings{};
        std::vector<DispatchResourceCandidate> candidates{};
    };

    /** @brief Complete exact-residency-cost candidate set for one dispatch node under one immutable planning snapshot.
     */
    struct DispatchCandidateSet {
        GraphNodeId node{};
        WorkloadDesc workload{};
        DispatchGeometry geometry{};
        std::vector<DispatchResidencyProjection> residencies{};
        std::vector<DispatchPlacementCandidate> placements{};
    };

    /** @brief Stable stage at which exact dispatch-candidate enumeration failed. */
    enum class DispatchCandidateEnumerationErrorCode : uint8_t {
        InvalidRequest,
        InvalidNode,
        NotDispatch,
        ProgramUnavailable,
        ResidencySnapshotFailed,
        ResidencyProjectionFailed,
        InputAssemblyUnavailable,
        CandidateSynthesisFailed,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Dispatch enumeration failure with precise logical node, argument, and
     * nested cause. */
    struct DispatchCandidateEnumerationError {
        DispatchCandidateEnumerationErrorCode code = DispatchCandidateEnumerationErrorCode::InvalidRequest;
        GraphNodeId node{};
        KernelArgumentId argument{};
        uint64_t cause = 0;

        friend constexpr bool operator==(DispatchCandidateEnumerationError,
                                         DispatchCandidateEnumerationError) noexcept = default;
    };

    /**
     * @brief Enumerate calibrated device, queue, memory, executable, and pipeline variants without input routes.
     * @details The result contains no synthetic transfer estimate. It is suitable for HEFT average-computation rank;
     * an executable placement still requires EnumerateDispatchCandidates under the current symbolic residency.
     */
    [[nodiscard]] std::expected<DispatchResourceCandidateSet, DispatchCandidateEnumerationError>
    EnumerateDispatchResourceCandidates(const ExecutionGraph& graph, GraphNodeId node,
                                        const GraphPlanningContext& context, const GraphPlanningServices& services,
                                        const DispatchEnumerationPolicy& policy = {}) noexcept;

    /**
     * @brief Enumerate exact residency-aware placements for one dispatch in @p
     * graph.
     * @details The operation consumes one attempt-frozen snapshot for each distinct logical buffer, projects calibrated
     * routes without backend callbacks, shares route state across queues on the same device, and returns stable queue
     * candidates. Produced intervals are projected from @p residency; uncovered intervals retain their frozen Buffer
     * snapshot source. Device-pipeline queues are expanded over catalogued pipeline configurations and priced with
     * exact service-scoped executable and pipeline availability plus calibrated cold materialization cost.
     */
    [[nodiscard]] std::expected<DispatchCandidateSet, DispatchCandidateEnumerationError>
    EnumerateDispatchCandidates(const ExecutionGraph& graph, GraphNodeId node, const GraphPlanningContext& context,
                                const GraphPlanningServices& services, const GraphResidencyState& residency,
                                const DispatchEnumerationPolicy& policy = {}) noexcept;

    /**
     * @brief Project one exact dispatch slice onto an already selected destination identity.
     * @details Every buffer binding is replaced by its exact slice subview and every readable coverage is rerouted
     * from the immutable residency snapshot. The result owns complete route provenance and performs no backend work.
     */
    [[nodiscard]] std::expected<DispatchResidencyProjection, DispatchCandidateEnumerationError>
    ProjectDispatchSliceResidency(const KernelInvocation& invocation, GraphNodeId node, const DispatchSlice& slice,
                                  const DispatchResidencyProjection& destination, const GraphPlanningServices& services,
                                  const GraphResidencyState& residency) noexcept;

    /** @brief Atomically advance symbolic residency after selecting @p projection for @p node. */
    [[nodiscard]] std::expected<void, GraphResidencyError>
    CommitDispatchResidencySelection(GraphNodeId node, const DispatchResidencyProjection& projection,
                                     GraphResidencyState& residency) noexcept;

    /** @brief Commit one selection into an isolated consumed state without a rollback copy. */
    [[nodiscard]] std::expected<GraphResidencyState, GraphResidencyError>
    CommitDispatchResidencySelection(GraphNodeId node, const DispatchResidencyProjection& projection,
                                     GraphResidencyState&& residency) noexcept;

    /**
     * @brief Atomically advance symbolic residency after selecting an exact strict cover of dispatch shards.
     * @details Every shard must preserve @p logicalBindings as its binding schema. For each writable argument, shard
     * coverages must be pairwise disjoint and form the unpartitioned binding's exact coverage. All transferred replicas
     * and writes are committed by one GraphResidencyState::Apply call, so rejection leaves @p residency unchanged.
     */
    [[nodiscard]] std::expected<void, GraphResidencyError> CommitPartitionedDispatchResidencySelection(
        GraphNodeId node, std::span<const DispatchMemoryBinding> logicalBindings,
        std::span<const DispatchResidencyProjection> projections, GraphResidencyState& residency) noexcept;

} // namespace Sora::Compute
