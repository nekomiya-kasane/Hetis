/**
 * @file Planner.h
 * @brief Deterministic cost-based placement for portable compute workloads.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Cost.h>
#include <Sora/Compute/KernelResourceDemand.h>
#include <Sora/Compute/Runtime/PerformancePrior.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Backend-independent facts needed to place one invocation. */
    struct WorkloadDesc {
        SemanticHash kernelHash{};
        DeviceFeatureMask requiredFeatures{};
        KernelResourceDemand resourceDemand{};
        uint64_t workItemCount = 0;
        uint64_t requiredDeviceMemoryBytes = 0;
        uint64_t largestAllocationBytes = 0;
        uint64_t expectedReuseCount = 1;
        bool deterministic = true;

        /** @brief Return whether identity, resource demand, magnitude, and memory bounds form one workload. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return ValidSemanticHash(kernelHash) && resourceDemand.Valid() && workItemCount != 0 &&
                   expectedReuseCount != 0 && largestAllocationBytes <= requiredDeviceMemoryBytes;
        }
    };

    /** @brief User-visible constraints; backend choice remains automatic. */
    struct PlacementPolicy {
        bool requireDeterminism = true;
        bool allowArtifactImport = true;
        bool allowRuntimeCompilation = true;
        bool allowRuntimePipelineCreation = true;
        bool allowTransfer = true;
    };

    /** @brief Planner-visible resolution mode; persistent lookup retains its production fallback. */
    enum class ArtifactResolutionMode : uint8_t {
        Live,
        PackageImport,
        Persistent,
        TransientProduction,
    };

    /** @brief Return whether @p mode denotes one complete planner resolution mode. */
    [[nodiscard]] constexpr bool ValidArtifactResolutionMode(ArtifactResolutionMode mode) noexcept {
        return mode >= ArtifactResolutionMode::Live && mode <= ArtifactResolutionMode::TransientProduction;
    }

    /** @brief Planner forecast for turning one exact artifact into a live backend handle. */
    struct ArtifactResolutionForecast {
        ArtifactResolutionMode mode = ArtifactResolutionMode::TransientProduction;
        uint64_t importNanoseconds = 0;
        uint64_t productionNanoseconds = 0;
        PerformanceEvidence evidence{};

        /** @brief Return whether the path and latency form one complete forecast. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            if (!ValidArtifactResolutionMode(mode)) {
                return false;
            }
            if (!evidence.Valid()) {
                return false;
            }
            if (mode == ArtifactResolutionMode::Live) {
                return importNanoseconds == 0 && productionNanoseconds == 0;
            }
            if (mode == ArtifactResolutionMode::PackageImport) {
                return importNanoseconds != 0 && productionNanoseconds == 0;
            }
            if (mode == ArtifactResolutionMode::TransientProduction) {
                return importNanoseconds == 0 && productionNanoseconds != 0;
            }
            return importNanoseconds != 0 || productionNanoseconds != 0;
        }

        friend constexpr bool operator==(ArtifactResolutionForecast, ArtifactResolutionForecast) noexcept = default;
    };

    /** @brief Exact candidate-specific residency traffic and end-to-end route cost. */
    struct CandidateTransferCost {
        uint64_t bytes = 0;
        uint64_t nanoseconds = 0;

        friend constexpr bool operator==(CandidateTransferCost, CandidateTransferCost) noexcept = default;
    };

    /** @brief Exact optional pipeline stage selected for one dispatch placement. */
    struct PipelinePlacement {
        PipelineKey key{};
        ArtifactResolutionForecast resolution{};

        friend constexpr bool operator==(PipelinePlacement, PipelinePlacement) noexcept = default;
    };

    /**
     * @brief Measured or conservatively estimated performance of one device/backend variant.
     * @details Rates are expressed per second; latencies are nanoseconds. Providers update these values from bounded
     * profiling, while the planner owns the placement decision.
     */
    struct PlacementCandidate {
        BackendServiceId service{};
        DeviceDescriptor device{};
        TargetFingerprint target{};
        SpecializationKey specialization{};
        QueueId queue{};
        DeviceSnapshot snapshot{};
        ArtifactResolutionForecast executable{};
        std::optional<PipelinePlacement> pipeline{};
        uint64_t workItemsPerSecond = 0;
        std::optional<CandidateTransferCost> transfer{};
        uint64_t launchNanoseconds = 0;
        uint64_t synchronizationNanoseconds = 0;
        bool deterministic = true;
        PerformanceEvidence kernelEvidence{};
    };

    /** @brief Additive cost decomposition retained for diagnostics and online calibration. */
    struct CostBreakdown {
        uint64_t queueNanoseconds = 0;
        uint64_t transferNanoseconds = 0;
        uint64_t launchNanoseconds = 0;
        uint64_t computeNanoseconds = 0;
        uint64_t synchronizationNanoseconds = 0;
        uint64_t amortizedExecutableImportNanoseconds = 0;
        uint64_t amortizedExecutableCompilationNanoseconds = 0;
        uint64_t amortizedPipelineImportNanoseconds = 0;
        uint64_t amortizedPipelineCreationNanoseconds = 0;
        uint64_t totalNanoseconds = 0;
    };

    /** @brief Selected device and its estimated end-to-end invocation cost. */
    struct ExecutionPlan {
        size_t candidateIndex = 0;
        BackendServiceId service{};
        DeviceId device{};
        QueueId queue{};
        BackendKind backend = BackendKind::Cpu;
        TargetFingerprint target{};
        SemanticHash kernelHash{};
        uint64_t deviceGeneration = 0;
        CostBreakdown cost{};
    };

    /** @brief Reason no candidate can satisfy a placement request. */
    enum class PlanningError : uint8_t {
        NoCandidate,
        InvalidWorkload,
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidPipeline(const PlacementCandidate& candidate) noexcept {
            if (!candidate.pipeline) {
                return true;
            }
            return candidate.pipeline->resolution.Valid() && MakeArtifactKey(candidate.pipeline->key).Valid() &&
                   candidate.pipeline->key.target == PipelineTarget(candidate.target);
        }

        [[nodiscard]] constexpr bool ResolutionAllowed(const PlacementPolicy& policy, ArtifactResolutionMode mode,
                                                       bool pipeline) noexcept {
            if (mode == ArtifactResolutionMode::Live) {
                return true;
            }
            if (mode == ArtifactResolutionMode::PackageImport) {
                return policy.allowArtifactImport;
            }
            const bool productionAllowed =
                pipeline ? policy.allowRuntimePipelineCreation : policy.allowRuntimeCompilation;
            return mode == ArtifactResolutionMode::Persistent ? policy.allowArtifactImport && productionAllowed
                                                              : productionAllowed;
        }

        [[nodiscard]] constexpr bool Eligible(const WorkloadDesc& workload, const PlacementPolicy& policy,
                                              const PlacementCandidate& candidate) noexcept {
            return candidate.service.Valid() && candidate.snapshot.id == candidate.device.id &&
                   candidate.snapshot.state == DeviceState::Available && candidate.snapshot.generation != 0 &&
                   candidate.snapshot.loadPermille <= 1000 &&
                   candidate.snapshot.availableMemoryBytes >= workload.requiredDeviceMemoryBytes &&
                   candidate.device.maximumAllocationBytes >= workload.largestAllocationBytes &&
                   candidate.device.features.Contains(workload.requiredFeatures) && candidate.executable.Valid() &&
                   ValidPipeline(candidate) &&
                   (!policy.requireDeterminism || (workload.deterministic && candidate.deterministic)) &&
                   (policy.allowTransfer || !candidate.transfer || candidate.transfer->bytes == 0) &&
                   ResolutionAllowed(policy, candidate.executable.mode, false) &&
                   (!candidate.pipeline || ResolutionAllowed(policy, candidate.pipeline->resolution.mode, true)) &&
                   candidate.workItemsPerSecond != 0;
        }

        [[nodiscard]] constexpr CostBreakdown Estimate(const WorkloadDesc& workload,
                                                       const PlacementCandidate& candidate) noexcept {
            CostBreakdown cost{};
            cost.queueNanoseconds = candidate.snapshot.estimatedQueueDelayNanoseconds;
            cost.transferNanoseconds = candidate.transfer ? candidate.transfer->nanoseconds : 0;
            cost.launchNanoseconds = candidate.launchNanoseconds;
            cost.computeNanoseconds = RateCost(workload.workItemCount, candidate.workItemsPerSecond);
            cost.synchronizationNanoseconds = candidate.synchronizationNanoseconds;
            const uint64_t reuse = std::max(uint64_t{1}, workload.expectedReuseCount);
            cost.amortizedExecutableImportNanoseconds = CeilDivide(candidate.executable.importNanoseconds, reuse);
            cost.amortizedExecutableCompilationNanoseconds =
                CeilDivide(candidate.executable.productionNanoseconds, reuse);
            if (candidate.pipeline) {
                cost.amortizedPipelineImportNanoseconds =
                    CeilDivide(candidate.pipeline->resolution.importNanoseconds, reuse);
                cost.amortizedPipelineCreationNanoseconds =
                    CeilDivide(candidate.pipeline->resolution.productionNanoseconds, reuse);
            }
            cost.totalNanoseconds = cost.queueNanoseconds;
            cost.totalNanoseconds = SaturatingAdd(cost.totalNanoseconds, cost.transferNanoseconds);
            cost.totalNanoseconds = SaturatingAdd(cost.totalNanoseconds, cost.launchNanoseconds);
            cost.totalNanoseconds = SaturatingAdd(cost.totalNanoseconds, cost.computeNanoseconds);
            cost.totalNanoseconds = SaturatingAdd(cost.totalNanoseconds, cost.synchronizationNanoseconds);
            cost.totalNanoseconds = SaturatingAdd(cost.totalNanoseconds, cost.amortizedExecutableImportNanoseconds);
            cost.totalNanoseconds =
                SaturatingAdd(cost.totalNanoseconds, cost.amortizedExecutableCompilationNanoseconds);
            cost.totalNanoseconds = SaturatingAdd(cost.totalNanoseconds, cost.amortizedPipelineImportNanoseconds);
            cost.totalNanoseconds = SaturatingAdd(cost.totalNanoseconds, cost.amortizedPipelineCreationNanoseconds);
            return cost;
        }

        [[nodiscard]] constexpr bool CandidateIdentityLess(const WorkloadDesc& workload, const PlacementCandidate& left,
                                                           const PlacementCandidate& right) noexcept {
            const auto coordinate = [](const PlacementCandidate& candidate) {
                return std::tuple{candidate.device.id, candidate.service, std::to_underlying(candidate.device.backend),
                                  candidate.queue};
            };
            if (coordinate(left) != coordinate(right)) {
                return coordinate(left) < coordinate(right);
            }
            const auto executable = [&](const PlacementCandidate& candidate) {
                return MakeArtifactKey(ExecutableKey{.lowered = workload.kernelHash,
                                                     .target = candidate.target,
                                                     .specialization = candidate.specialization});
            };
            const ArtifactKey leftExecutable = executable(left);
            const ArtifactKey rightExecutable = executable(right);
            if (leftExecutable != rightExecutable) {
                return std::ranges::lexicographical_compare(leftExecutable.Words(), rightExecutable.Words());
            }
            if (left.pipeline.has_value() != right.pipeline.has_value()) {
                return !left.pipeline.has_value();
            }
            if (!left.pipeline) {
                return false;
            }
            const ArtifactKey leftPipeline = MakeArtifactKey(left.pipeline->key);
            const ArtifactKey rightPipeline = MakeArtifactKey(right.pipeline->key);
            return std::ranges::lexicographical_compare(leftPipeline.Words(), rightPipeline.Words());
        }

    } // namespace Detail

    /** @brief Select the minimum end-to-end cost candidate after applying all hard placement constraints. */
    [[nodiscard]] constexpr std::expected<ExecutionPlan, PlanningError>
    PlanExecution(const WorkloadDesc& workload, std::span<const PlacementCandidate> candidates,
                  const PlacementPolicy& policy = {}) noexcept {
        if (!workload.Valid()) {
            return std::unexpected(PlanningError::InvalidWorkload);
        }
        std::expected<ExecutionPlan, PlanningError> best = std::unexpected(PlanningError::NoCandidate);
        for (size_t index = 0; index < candidates.size(); ++index) {
            const PlacementCandidate& candidate = candidates[index];
            if (!Detail::Eligible(workload, policy, candidate)) {
                continue;
            }
            const CostBreakdown cost = Detail::Estimate(workload, candidate);
            if (!best || cost.totalNanoseconds < best->cost.totalNanoseconds ||
                (cost.totalNanoseconds == best->cost.totalNanoseconds &&
                 Detail::CandidateIdentityLess(workload, candidate, candidates[best->candidateIndex]))) {
                best = ExecutionPlan{.candidateIndex = index,
                                     .service = candidate.service,
                                     .device = candidate.device.id,
                                     .queue = candidate.queue,
                                     .backend = candidate.device.backend,
                                     .target = candidate.target,
                                     .kernelHash = workload.kernelHash,
                                     .deviceGeneration = candidate.snapshot.generation,
                                     .cost = cost};
            }
        }
        return best;
    }

    /** @brief Reason a previously selected placement no longer denotes the same available device generation. */
    enum class ExecutionPlanValidationError : uint8_t {
        CandidateMismatch,
        DeviceUnavailable,
        DeviceLost,
        GenerationChanged,
    };

    /** @brief Validate a plan against a freshly observed candidate before materialization. */
    [[nodiscard]] constexpr std::expected<void, ExecutionPlanValidationError>
    ValidateExecutionPlan(const ExecutionPlan& plan, const PlacementCandidate& candidate) noexcept {
        if (plan.service != candidate.service || plan.device != candidate.device.id || plan.queue != candidate.queue ||
            plan.backend != candidate.device.backend || plan.target != candidate.target) {
            return std::unexpected(ExecutionPlanValidationError::CandidateMismatch);
        }
        if (candidate.snapshot.id != plan.device) {
            return std::unexpected(ExecutionPlanValidationError::CandidateMismatch);
        }
        if (candidate.snapshot.generation != plan.deviceGeneration) {
            return std::unexpected(ExecutionPlanValidationError::GenerationChanged);
        }
        if (candidate.snapshot.state == DeviceState::Lost) {
            return std::unexpected(ExecutionPlanValidationError::DeviceLost);
        }
        if (candidate.snapshot.state != DeviceState::Available) {
            return std::unexpected(ExecutionPlanValidationError::DeviceUnavailable);
        }
        return {};
    }

    static_assert(std::is_trivially_copyable_v<CandidateTransferCost>);
    static_assert(std::is_trivially_copyable_v<PipelinePlacement>);

} // namespace Sora::Compute
