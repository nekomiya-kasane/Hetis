/**
 * @file PhysicalGraphPlan.h
 * @brief Pure immutable plan for target-specific physical graph
 * materialization.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/GraphOperation.h>
#include <Sora/Compute/Invocation/DispatchGeometry.h>
#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>
#include <Sora/Compute/Runtime/PipelineBindingLayout.h>
#include <Sora/Compute/Runtime/ProgramPackage.h>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {

    inline constexpr uint32_t kInvalidPhysicalPlanIndex = std::numeric_limits<uint32_t>::max();

    /** @brief Strong identity of one physical plan segment. */
    struct PhysicalPlanSegmentId {
        uint32_t index = kInvalidPhysicalPlanIndex;

        [[nodiscard]] constexpr bool Valid() const noexcept { return index != kInvalidPhysicalPlanIndex; }
        friend constexpr bool operator==(PhysicalPlanSegmentId, PhysicalPlanSegmentId) noexcept = default;
        friend constexpr auto operator<=>(PhysicalPlanSegmentId, PhysicalPlanSegmentId) noexcept = default;
    };

    /** @brief Strong identity of one not-yet-materialized physical allocation. */
    struct PlannedAllocationId {
        uint32_t index = kInvalidPhysicalPlanIndex;

        [[nodiscard]] constexpr bool Valid() const noexcept { return index != kInvalidPhysicalPlanIndex; }
        friend constexpr bool operator==(PlannedAllocationId, PlannedAllocationId) noexcept = default;
        friend constexpr auto operator<=>(PlannedAllocationId, PlannedAllocationId) noexcept = default;
    };

    /** @brief Ready retained storage whose buffer identity remains explicit outside
     * its residency table. */
    struct ReadyStorageRef {
        BufferId buffer{};
        ResidentReplica replica{};
    };

    /** @brief One planned allocation and the segment after which its data is ready. */
    struct PlannedAllocationRef {
        PlannedAllocationId allocation{};
        PhysicalPlanSegmentId readyAfter{};

        friend constexpr bool operator==(PlannedAllocationRef, PlannedAllocationRef) noexcept = default;
    };

    /** @brief Ready retained storage or a graph-local planned allocation reference. */
    using PlannedStorageRef = std::variant<ReadyStorageRef, PlannedAllocationRef>;

    /** @brief Backend allocation request evaluated during materialization. */
    struct PlannedAllocate {
        MemoryPropertyMask requiredProperties{};
        MemoryPropertyMask preferredProperties{};
        uint64_t alignment = 1;
    };

    /** @brief Synchronous coherent import evaluated during materialization without
     * creating a graph segment. */
    struct PlannedCoherentAlias {
        TransferEngineId engine{};
        PlannedStorageRef source{};
    };

    /** @brief Logical publication transition committed only after every physical
     * segment completes. */
    enum class PlannedPublicationKind : uint8_t {
        Replica,
        WholeWrite,
        PartitionedWrite,
    };

    /** @brief Final graph-local state and logical version precondition to publish
     * transactionally. */
    struct PlannedPublication {
        PlannedPublicationKind kind = PlannedPublicationKind::Replica;
        uint64_t expectedVersion = 0;
        PhysicalPlanSegmentId producer{};
        uint32_t partitionIndex = UINT32_MAX;
        uint32_t partitionCount = 0;
    };

    /** @brief Closed allocation-handle materialization mechanisms. */
    using PlannedAllocationMaterialization = std::variant<PlannedAllocate, PlannedCoherentAlias>;

    /** @brief Semantic role controlling version and publication invariants of one planned allocation. */
    enum class PlannedAllocationPurpose : uint8_t {
        LogicalValue,
        DispatchInputAssembly,
    };

    /** @brief One physical storage handle and logical replica mapping to
     * materialize. */
    struct PlannedAllocation {
        PlannedAllocationId id{};
        PlannedAllocationPurpose purpose = PlannedAllocationPurpose::LogicalValue;
        Buffer buffer{};
        BackendServiceId service{};
        MemoryLocation location{};
        ByteRegion bufferRegion{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        PlannedAllocationMaterialization materialization{};
        std::optional<PlannedPublication> publication{};

        /** @brief Return the retained logical buffer identity, or an invalid identity
         * for an empty handle. */
        [[nodiscard]] BufferId BufferIdentity() const noexcept { return buffer ? buffer.Describe().id : BufferId{}; }

        /** @brief Borrow the exact logical bytes mapped into this allocation. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(bufferRegion);
        }
    };

    /** @brief Exact backend queue selected for one physical command. */
    struct PlannedQueue {
        BackendServiceId service{};
        BackendKind backend = BackendKind::Cpu;
        DeviceId device{};
        QueueId queue{};

        friend constexpr bool operator==(PlannedQueue, PlannedQueue) noexcept = default;
    };

    /** @brief Explicit input state and output handle for one dispatch buffer
     * argument. */
    struct PlannedDispatchBuffer {
        std::optional<PlannedStorageRef> input{};
        PlannedAllocationId output{};
    };

    /** @brief Pipeline factory and semantic state selected for a pipeline-consuming
     * queue. */
    struct PlannedPipeline {
        SemanticHash factory{};
        SemanticHash state{};

        /** @brief Return whether both planner-selected identities are stable. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return ValidSemanticHash(factory) && ValidSemanticHash(state);
        }

        friend constexpr bool operator==(PlannedPipeline, PlannedPipeline) noexcept = default;
    };

    /** @brief One target-specific dispatch retaining its canonical program and
     * logical invocation. */
    struct PlannedDispatch {
        GraphNodeId logicalNode{};
        PlannedQueue queue{};
        ProgramPackage program{};
        KernelInvocation invocation{};
        DispatchSlice slice{};
        TargetFingerprint target{};
        SpecializationKey specialization{};
        DispatchGeometry geometry{};
        std::optional<PlannedPipeline> pipeline{};
        std::vector<PlannedDispatchBuffer> bufferStorage{};
        bool deterministic = true;
    };

    /** @brief One queue-local physical copy retaining its logical operation. */
    struct PlannedCopy {
        GraphNodeId logicalNode{};
        PlannedQueue queue{};
        CopyOperation operation;
        PlannedStorageRef source{};
        PlannedAllocationId destination{};
    };

    /** @brief One queue-local physical fill retaining its logical operation. */
    struct PlannedFill {
        GraphNodeId logicalNode{};
        PlannedQueue queue{};
        FillOperation operation;
        PlannedAllocationId destination{};
    };

    /** @brief One exact source coverage copied into a canonical dispatch-input assembly allocation. */
    struct PlannedAssemblyPart {
        std::optional<PlannedStorageRef> source{};
        PhysicalPlanSegmentId transferProducer{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};

        /** @brief Borrow the exact logical bytes contributed by this source part. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }
    };

    /** @brief Semantic extent produced by one asynchronous transfer segment. */
    enum class PlannedTransferTarget : uint8_t {
        CompleteAllocation,
        AssemblyPart,
    };

    /** @brief Queue-local exact gather or event join producing one canonical affine dispatch input. */
    struct PlannedAssembly {
        GraphNodeId logicalNode{};
        PlannedQueue queue{};
        PlannedAllocationId destination{};
        std::vector<PlannedAssemblyPart> parts{};
    };

    /** @brief One asynchronous directed residency hop producing exactly one
     * allocated target. */
    struct PlannedTransfer {
        GraphNodeId logicalNode{};
        BackendServiceId service{};
        TransferEngineId engine{};
        BufferId buffer{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        PlannedStorageRef source{};
        PlannedAllocationId destination{};
        PlannedTransferTarget target = PlannedTransferTarget::CompleteAllocation;

        /** @brief Borrow the exact logical bytes transferred by this hop. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }
    };

    /** @brief Closed physical operation set understood by the current materializer.
     */
    using PhysicalPlanSegment =
        std::variant<PlannedDispatch, PlannedCopy, PlannedFill, PlannedAssembly, PlannedTransfer>;

    /** @brief Directed dependency between topologically ordered physical plan
     * segments. */
    struct PhysicalPlanEdge {
        PhysicalPlanSegmentId predecessor{};
        PhysicalPlanSegmentId successor{};

        friend constexpr bool operator==(PhysicalPlanEdge, PhysicalPlanEdge) noexcept = default;
    };

    /** @brief Owning input used to construct one immutable physical plan
     * transactionally. */
    struct PhysicalGraphPlanStorage {
        std::vector<PlannedAllocation> allocations{};
        std::vector<PhysicalPlanSegment> segments{};
        std::vector<PhysicalPlanEdge> edges{};
    };

    /** @brief Stable pure-planning failure before allocation, compilation, import,
     * or submission. */
    enum class PhysicalGraphPlanErrorCode : uint8_t {
        EmptyPlan,
        CapacityExceeded,
        OutOfMemory,
        InvalidAllocation,
        InvalidPublication,
        InvalidSegment,
        MissingPhysicalResource,
        InvalidDependency,
        DuplicateDependency,
        MissingDataDependency,
        DeviceUnavailable,
    };

    /** @brief Physical plan failure with primary and related stable indices. */
    struct PhysicalGraphPlanError {
        PhysicalGraphPlanErrorCode code = PhysicalGraphPlanErrorCode::InvalidSegment;
        uint32_t segment = kInvalidPhysicalPlanIndex;
        uint32_t related = kInvalidPhysicalPlanIndex;

        friend constexpr bool operator==(PhysicalGraphPlanError, PhysicalGraphPlanError) noexcept = default;
    };

    /** @brief Immutable target-specific plan with no backend side effects. */
    class PhysicalGraphPlan final {
    public:
        /** @brief Validate identities, resources, producers, and dependencies, then
         * freeze @p storage. */
        [[nodiscard]] static std::expected<PhysicalGraphPlan, PhysicalGraphPlanError>
        Create(PhysicalGraphPlanStorage storage, const PhysicalResourceCatalog& resources) noexcept;

        [[nodiscard]] std::span<const PlannedAllocation> Allocations() const noexcept { return storage_.allocations; }
        [[nodiscard]] std::span<const PhysicalPlanSegment> Segments() const noexcept { return storage_.segments; }
        [[nodiscard]] std::span<const PhysicalPlanEdge> Edges() const noexcept { return storage_.edges; }
        [[nodiscard]] std::span<const DeviceLease> DeviceLeases() const noexcept { return deviceLeases_; }

    private:
        PhysicalGraphPlan(PhysicalGraphPlanStorage storage, std::vector<DeviceLease> deviceLeases) noexcept
            : storage_(std::move(storage)), deviceLeases_(std::move(deviceLeases)) {}

        PhysicalGraphPlanStorage storage_;
        std::vector<DeviceLease> deviceLeases_;
    };

    static_assert(std::is_trivially_copyable_v<PhysicalPlanSegmentId>);
    static_assert(std::is_trivially_copyable_v<PlannedAllocationId>);
    static_assert(std::is_trivially_copyable_v<PlannedQueue>);
    static_assert(std::is_trivially_copyable_v<PlannedPipeline>);
    static_assert(std::is_trivially_copyable_v<PhysicalPlanEdge>);

} // namespace Sora::Compute
