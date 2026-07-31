/**
 * @file CandidateSynthesis.h
 * @brief Pure capability filtering and calibrated dispatch-candidate construction.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Planner.h>
#include <Sora/Compute/Memory/ResidencyPlanner.h>
#include <Sora/Compute/Runtime/PerformanceModel.h>
#include <Sora/Compute/Runtime/PhysicalResourceInventory.h>
#include <Sora/Compute/Runtime/GraphResidency.h>
#include <Sora/Compute/Runtime/RuntimeEpoch.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace Sora::Compute {

    /** @brief Exact pipeline artifact state associated with one executable dispatch variant. */
    struct PipelineArtifactState {
        PipelineKey key{};
        ArtifactResolutionMode resolution = ArtifactResolutionMode::TransientProduction;

        friend constexpr bool operator==(PipelineArtifactState, PipelineArtifactState) noexcept = default;
    };

    /** @brief Request-local artifact state already projected from runtime caches without granting cache authority. */
    struct CandidateArtifactState {
        BackendServiceId service{};
        KernelCalibrationKey candidate{};
        ArtifactResolutionMode executable = ArtifactResolutionMode::TransientProduction;
        std::optional<PipelineArtifactState> pipeline{};

        friend constexpr bool operator==(CandidateArtifactState, CandidateArtifactState) noexcept = default;
    };

    /** @brief End-to-end measured residency cost projected for one exact dispatch candidate. */
    struct CandidateTransferState {
        BackendServiceId service{};
        KernelCalibrationKey candidate{};
        uint64_t bytes = 0;
        uint64_t nanoseconds = 0;

        friend constexpr bool operator==(CandidateTransferState, CandidateTransferState) noexcept = default;
    };

    /** @brief Pure inputs needed to synthesize resident dispatch candidates for one invocation. */
    struct DispatchCandidateRequest {
        WorkloadDesc workload{};
        SpecializationKey specialization{};
        std::span<const RuntimeDeviceBinding> devices{};
        std::span<const DeviceSnapshot> snapshots{};
        std::span<const CandidateArtifactState> artifacts{};
        std::span<const CandidateTransferState> transfers{};
        bool requireTransferProjection = false;
    };

    /** @brief Stable failure before or during pure candidate synthesis. */
    enum class CandidateSynthesisError : uint8_t {
        InvalidRequest,
        RuntimeContextMismatch,
        TransferModelRequired,
        OutOfMemory,
    };

    /**
     * @brief Join same-index runtime facts, queue capabilities, artifact state, and immutable calibration.
     * @details The result contains no backend handles. A non-resident workload requires an exact transfer-cost
     * projection for each retained candidate; no synthetic aggregate bandwidth is invented. Pipeline queues require
     * one exact configuration-derived pipeline key and a path-specific cold resolution estimate when that key is not
     * live.
     */
    [[nodiscard]] std::expected<std::vector<PlacementCandidate>, CandidateSynthesisError>
    SynthesizeDispatchCandidates(const DispatchCandidateRequest& request, const PhysicalResourceInventory& resources,
                                 const PerformanceSnapshot& performance,
                                 const PerformancePriorModel* priors = nullptr) noexcept;

    /** @brief Aggregate validated residency routes into one exact candidate transfer-cost projection. */
    [[nodiscard]] std::expected<CandidateTransferState, CandidateSynthesisError>
    ProjectCandidateTransferCost(BackendServiceId service, const KernelCalibrationKey& candidate,
                                 std::span<const ResidencyRoute> routes) noexcept;

    /** @brief Aggregate validated graph-level residency routes into one exact candidate transfer-cost projection. */
    [[nodiscard]] std::expected<CandidateTransferState, CandidateSynthesisError>
    ProjectCandidateTransferCost(BackendServiceId service, const KernelCalibrationKey& candidate,
                                 std::span<const GraphResidencyRoute> routes) noexcept;

    static_assert(std::is_trivially_copyable_v<PipelineArtifactState>);
    static_assert(std::is_trivially_copyable_v<CandidateArtifactState>);
    static_assert(std::is_trivially_copyable_v<CandidateTransferState>);

} // namespace Sora::Compute
