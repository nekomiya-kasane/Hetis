/**
 * @file PhysicalGraphMaterializer.h
 * @brief Exact memory reservation and transactional conversion of physical plans into executable graphs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/CompilerService.h>
#include <Sora/Compute/PipelineService.h>
#include <Sora/Compute/Runtime/ExecutableGraph.h>
#include <Sora/Compute/Runtime/PhysicalGraphPlan.h>

#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable failure while acquiring every physical storage capability required by one graph. */
    enum class PhysicalGraphMemoryReservationErrorCode : uint8_t {
        InvalidPlan,
        DeviceInvalidated,
        MissingPhysicalResource,
        AllocationFailed,
        AliasImportFailed,
        OutOfMemory,
    };

    /** @brief Whole-graph storage acquisition failure with one allocation or device subject. */
    struct PhysicalGraphMemoryReservationError {
        PhysicalGraphMemoryReservationErrorCode code = PhysicalGraphMemoryReservationErrorCode::InvalidPlan;
        uint32_t subject = kInvalidPhysicalPlanIndex;
        uint32_t cause = 0;
        uint64_t detail = 0;

        friend constexpr bool operator==(PhysicalGraphMemoryReservationError,
                                         PhysicalGraphMemoryReservationError) noexcept = default;
    };

    /**
     * @brief Move-only proof that every physical storage capability required by one immutable plan is held.
     * @details The reservation owns the plan and one allocation handle for every planned allocation, including
     * coherent aliases. Destruction releases handles in reverse plan order. Successful acquisition therefore closes
     * allocator-policy, fragmentation, concurrent-submission, and external-capacity races before JIT begins.
     */
    class PhysicalGraphMemoryReservation final {
    public:
        PhysicalGraphMemoryReservation(const PhysicalGraphMemoryReservation&) = delete;
        PhysicalGraphMemoryReservation& operator=(const PhysicalGraphMemoryReservation&) = delete;
        PhysicalGraphMemoryReservation(PhysicalGraphMemoryReservation&&) noexcept = default;
        PhysicalGraphMemoryReservation& operator=(PhysicalGraphMemoryReservation&&) = delete;
        ~PhysicalGraphMemoryReservation() noexcept;

        /** @brief Borrow the immutable plan whose complete storage capability is held. */
        [[nodiscard]] const PhysicalGraphPlan& Plan() const noexcept { return plan_; }

        /** @brief Borrow allocations indexed identically to @ref PhysicalGraphPlan::Allocations. */
        [[nodiscard]] std::span<const Allocation> Allocations() const noexcept { return allocations_; }

    private:
        friend std::expected<PhysicalGraphMemoryReservation, PhysicalGraphMemoryReservationError>
        ReservePhysicalGraphMemory(PhysicalGraphPlan&& plan, const PhysicalResourceCatalog& resources) noexcept;

        PhysicalGraphMemoryReservation(PhysicalGraphPlan plan, std::vector<Allocation> allocations) noexcept
            : plan_(std::move(plan)), allocations_(std::move(allocations)) {}

        PhysicalGraphPlan plan_;
        std::vector<Allocation> allocations_{};
    };

    static_assert(std::is_nothrow_move_constructible_v<PhysicalGraphMemoryReservation>);
    static_assert(!std::is_move_assignable_v<PhysicalGraphMemoryReservation>);
    static_assert(!std::is_copy_constructible_v<PhysicalGraphMemoryReservation>);

    /**
     * @brief Acquire all physical allocations and coherent aliases required by @p plan as one rollback transaction.
     * @details The function consumes @p plan only on success. It performs no compilation, pipeline creation, transfer,
     * or queue submission. A successful result is exact physical feasibility evidence rather than a byte-count
     * prediction.
     */
    [[nodiscard]] std::expected<PhysicalGraphMemoryReservation, PhysicalGraphMemoryReservationError>
    ReservePhysicalGraphMemory(PhysicalGraphPlan&& plan, const PhysicalResourceCatalog& resources) noexcept;

    /** @brief Stable failure stage while materializing one memory-reserved physical plan. */
    enum class PhysicalGraphMaterializationErrorCode : uint8_t {
        InvalidPlan,
        DeviceInvalidated,
        MissingPhysicalResource,
        CompilationFailed,
        PipelineResolutionFailed,
        BindingFailed,
        BatchConstructionFailed,
        TransferConstructionFailed,
        CompletionConstructionFailed,
        GraphConstructionFailed,
        OutOfMemory,
    };

    /** @brief Non-owning artifact resolvers required by physical graph
     * materialization. */
    struct ArtifactResolutionServices {
        CompilerService& compiler;
        PipelineService& pipeline;
        KernelPerformanceObserver kernelPerformance{};
    };

    /** @brief Materialization failure with stable location and backend-specific
     * diagnostic fields. */
    struct PhysicalGraphMaterializationError {
        PhysicalGraphMaterializationErrorCode code = PhysicalGraphMaterializationErrorCode::InvalidPlan;
        uint32_t segment = kInvalidPhysicalPlanIndex;
        uint32_t subject = kInvalidPhysicalPlanIndex;
        uint32_t cause = 0;
        uint64_t detail = 0;

        friend constexpr bool operator==(PhysicalGraphMaterializationError,
                                         PhysicalGraphMaterializationError) noexcept = default;
    };

    /**
     * @brief Materialize every command, dependency, and completion action from one exact memory reservation.
     * @details No queue or transfer work is submitted. Compilation may occur; the consumed reservation releases its
     * allocations in reverse plan order if any later step fails. Compiler and pipeline cache entries retain lifetimes
     * independent of the returned graph.
     */
    [[nodiscard]] std::expected<ExecutableGraph, PhysicalGraphMaterializationError>
    MaterializePhysicalGraph(PhysicalGraphMemoryReservation reservation, const PhysicalResourceCatalog& resources,
                             ArtifactResolutionServices services) noexcept;

} // namespace Sora::Compute
