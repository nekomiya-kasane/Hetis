/**
 * @file PerformancePrior.h
 * @brief Explicit cold-start priors for unseen kernel and artifact costs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Device.h>
#include <Sora/Compute/KernelResourceDemand.h>
#include <Sora/Compute/Runtime/PerformanceModel.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Provenance and strength of one planner-visible performance forecast. */
    enum class PerformanceEvidenceKind : uint8_t {
        Exact,
        Prior,
        Observed,
    };

    /** @brief Explicit evidence retained with a forecast without entering measured calibration state. */
    struct PerformanceEvidence {
        PerformanceEvidenceKind kind = PerformanceEvidenceKind::Exact;
        uint16_t confidencePermille = 1'000;
        uint32_t sampleCount = 0;

        /** @brief Return whether evidence fields agree with their semantic source. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            if (confidencePermille > 1'000) {
                return false;
            }
            if (kind == PerformanceEvidenceKind::Exact) {
                return confidencePermille == 1'000 && sampleCount == 0;
            }
            if (kind == PerformanceEvidenceKind::Prior) {
                return confidencePermille != 0 && sampleCount == 0;
            }
            return kind == PerformanceEvidenceKind::Observed && confidencePermille != 0 && sampleCount != 0;
        }

        friend constexpr bool operator==(PerformanceEvidence, PerformanceEvidence) noexcept = default;
    };

    /** @brief Kernel estimate paired with its non-ambiguous evidence provenance. */
    struct KernelPerformanceForecast {
        KernelPerformanceEstimate estimate{};
        PerformanceEvidence evidence{};

        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validEstimate =
                estimate.workItemsPerSecond != 0 &&
                ((evidence.kind == PerformanceEvidenceKind::Prior && estimate.sampleCount == 0) ||
                 (evidence.kind == PerformanceEvidenceKind::Observed && estimate.Valid()));
            return validEstimate && evidence.Valid();
        }

        friend constexpr bool operator==(KernelPerformanceForecast, KernelPerformanceForecast) noexcept = default;
    };

    /** @brief Conservative cold-start service rates for one physical device kind. */
    struct DeviceKernelPrior {
        uint64_t integerOperationsPerSecondPerComputeUnit = 1;
        uint64_t floatingPointOperationsPerSecondPerComputeUnit = 1;
        uint64_t float64OperationsPerSecondPerComputeUnit = 1;
        uint64_t specialFunctionOperationsPerSecondPerComputeUnit = 1;
        uint64_t memoryBytesPerSecondPerComputeUnit = 1;
        uint64_t synchronizationOperationsPerSecondPerComputeUnit = 1;
        uint16_t stridedMemoryEfficiencyPermille = 1'000;
        uint64_t launchNanoseconds = 1;
        uint64_t synchronizationNanoseconds = 1;
        uint16_t confidencePermille = 100;

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return integerOperationsPerSecondPerComputeUnit != 0 &&
                   floatingPointOperationsPerSecondPerComputeUnit != 0 &&
                   float64OperationsPerSecondPerComputeUnit != 0 &&
                   specialFunctionOperationsPerSecondPerComputeUnit != 0 && memoryBytesPerSecondPerComputeUnit != 0 &&
                   synchronizationOperationsPerSecondPerComputeUnit != 0 && stridedMemoryEfficiencyPermille != 0 &&
                   stridedMemoryEfficiencyPermille <= 1'000 && launchNanoseconds != 0 &&
                   synchronizationNanoseconds != 0 && confidencePermille != 0 && confidencePermille <= 1'000;
        }

        friend constexpr bool operator==(DeviceKernelPrior, DeviceKernelPrior) noexcept = default;
    };

    /** @brief Conservative cold-start artifact costs for one backend compilation family. */
    struct BackendArtifactPrior {
        uint64_t packageImportNanoseconds = 1;
        uint64_t persistentImportNanoseconds = 1;
        uint64_t executableProductionNanoseconds = 1;
        uint64_t pipelineProductionNanoseconds = 1;
        uint16_t persistentHitPermille = 750;
        uint16_t confidencePermille = 100;

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return packageImportNanoseconds != 0 && persistentImportNanoseconds != 0 &&
                   executableProductionNanoseconds != 0 && pipelineProductionNanoseconds != 0 &&
                   persistentHitPermille <= 1'000 && confidencePermille != 0 && confidencePermille <= 1'000;
        }

        friend constexpr bool operator==(BackendArtifactPrior, BackendArtifactPrior) noexcept = default;
    };

    /** @brief Unmeasured latency prior for one exact artifact-resolution path. */
    struct ArtifactResolutionPrior {
        uint64_t nanoseconds = 0;

        [[nodiscard]] constexpr bool Valid() const noexcept { return nanoseconds != 0; }

        friend constexpr bool operator==(ArtifactResolutionPrior, ArtifactResolutionPrior) noexcept = default;
    };

    /** @brief Unmeasured persistent import and fallback-production expectation. */
    struct PersistentArtifactResolutionPrior {
        uint64_t importNanoseconds = 0;
        uint64_t productionNanoseconds = 0;
        uint16_t importPermille = 0;
        uint16_t productionPermille = 0;

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return importPermille + productionPermille == 1'000 &&
                   (importNanoseconds != 0 || productionNanoseconds != 0);
        }

        friend constexpr bool operator==(PersistentArtifactResolutionPrior,
                                         PersistentArtifactResolutionPrior) noexcept = default;
    };

    inline constexpr size_t kDeviceKindCount = static_cast<size_t>(std::to_underlying(DeviceKind::Remote)) + 1;
    inline constexpr size_t kBackendKindCount = static_cast<size_t>(std::to_underlying(BackendKind::Remote)) + 1;

    /** @brief Complete opt-out-capable cold-start policy owned by one compute context. */
    struct PerformancePriorPolicy {
        bool enabled = true;
        std::array<DeviceKernelPrior, kDeviceKindCount> kernels{
            DeviceKernelPrior{.integerOperationsPerSecondPerComputeUnit = 100'000'000,
                              .floatingPointOperationsPerSecondPerComputeUnit = 100'000'000,
                              .float64OperationsPerSecondPerComputeUnit = 50'000'000,
                              .specialFunctionOperationsPerSecondPerComputeUnit = 12'500'000,
                              .memoryBytesPerSecondPerComputeUnit = 600'000'000,
                              .synchronizationOperationsPerSecondPerComputeUnit = 6'250'000,
                              .stridedMemoryEfficiencyPermille = 500,
                              .launchNanoseconds = 2'000,
                              .synchronizationNanoseconds = 500,
                              .confidencePermille = 150},
            DeviceKernelPrior{.integerOperationsPerSecondPerComputeUnit = 200'000'000,
                              .floatingPointOperationsPerSecondPerComputeUnit = 200'000'000,
                              .float64OperationsPerSecondPerComputeUnit = 50'000'000,
                              .specialFunctionOperationsPerSecondPerComputeUnit = 25'000'000,
                              .memoryBytesPerSecondPerComputeUnit = 1'200'000'000,
                              .synchronizationOperationsPerSecondPerComputeUnit = 12'500'000,
                              .stridedMemoryEfficiencyPermille = 300,
                              .launchNanoseconds = 20'000,
                              .synchronizationNanoseconds = 5'000,
                              .confidencePermille = 100},
            DeviceKernelPrior{.integerOperationsPerSecondPerComputeUnit = 300'000'000,
                              .floatingPointOperationsPerSecondPerComputeUnit = 300'000'000,
                              .float64OperationsPerSecondPerComputeUnit = 30'000'000,
                              .specialFunctionOperationsPerSecondPerComputeUnit = 37'500'000,
                              .memoryBytesPerSecondPerComputeUnit = 1'800'000'000,
                              .synchronizationOperationsPerSecondPerComputeUnit = 18'750'000,
                              .stridedMemoryEfficiencyPermille = 250,
                              .launchNanoseconds = 15'000,
                              .synchronizationNanoseconds = 5'000,
                              .confidencePermille = 100},
            DeviceKernelPrior{.integerOperationsPerSecondPerComputeUnit = 400'000'000,
                              .floatingPointOperationsPerSecondPerComputeUnit = 400'000'000,
                              .float64OperationsPerSecondPerComputeUnit = 100'000'000,
                              .specialFunctionOperationsPerSecondPerComputeUnit = 50'000'000,
                              .memoryBytesPerSecondPerComputeUnit = 2'400'000'000,
                              .synchronizationOperationsPerSecondPerComputeUnit = 25'000'000,
                              .stridedMemoryEfficiencyPermille = 250,
                              .launchNanoseconds = 15'000,
                              .synchronizationNanoseconds = 5'000,
                              .confidencePermille = 100},
            DeviceKernelPrior{.integerOperationsPerSecondPerComputeUnit = 100'000'000,
                              .floatingPointOperationsPerSecondPerComputeUnit = 100'000'000,
                              .float64OperationsPerSecondPerComputeUnit = 25'000'000,
                              .specialFunctionOperationsPerSecondPerComputeUnit = 12'500'000,
                              .memoryBytesPerSecondPerComputeUnit = 600'000'000,
                              .synchronizationOperationsPerSecondPerComputeUnit = 6'250'000,
                              .stridedMemoryEfficiencyPermille = 500,
                              .launchNanoseconds = 1'000'000,
                              .synchronizationNanoseconds = 1'000'000,
                              .confidencePermille = 50},
        };
        std::array<BackendArtifactPrior, kBackendKindCount> artifacts{
            BackendArtifactPrior{50'000, 150'000, 500'000, 500'000, 800, 150},
            BackendArtifactPrior{100'000, 250'000, 20'000'000, 2'000'000, 750, 100},
            BackendArtifactPrior{100'000, 250'000, 8'000'000, 1'000'000, 750, 100},
            BackendArtifactPrior{100'000, 250'000, 8'000'000, 1'000'000, 750, 100},
            BackendArtifactPrior{100'000, 250'000, 10'000'000, 2'000'000, 750, 100},
            BackendArtifactPrior{100'000, 250'000, 12'000'000, 2'000'000, 750, 100},
            BackendArtifactPrior{1'000'000, 2'000'000, 50'000'000, 10'000'000, 500, 50},
        };

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return std::ranges::all_of(kernels, &DeviceKernelPrior::Valid) &&
                   std::ranges::all_of(artifacts, &BackendArtifactPrior::Valid);
        }

        friend constexpr bool operator==(PerformancePriorPolicy, PerformancePriorPolicy) noexcept = default;
    };

    /** @brief Pure immutable provider of conservative priors for unseen runtime identities. */
    class PerformancePriorModel final {
    public:
        constexpr explicit PerformancePriorModel(PerformancePriorPolicy policy = {}) noexcept
            : policy_(policy), valid_(policy.Valid()) {}

        /** @brief Forecast unseen kernel execution from resource demand and conservative device service rates. */
        [[nodiscard]] constexpr std::optional<KernelPerformanceForecast>
        ForecastKernel(const DeviceDescriptor& device, const KernelResourceDemand& demand,
                       bool deterministic) const noexcept {
            if (!policy_.enabled || !valid_ || !ValidDeviceDescriptor(device) || !demand.Valid()) {
                return std::nullopt;
            }
            const DeviceKernelPrior& prior = policy_.kernels[static_cast<size_t>(std::to_underlying(device.kind))];
            const uint64_t units = std::max<uint64_t>(device.computeUnitCount, 1);
            uint64_t workItemsPerSecond = std::numeric_limits<uint64_t>::max();
            LimitThroughput(workItemsPerSecond, ScaleServiceRate(prior.integerOperationsPerSecondPerComputeUnit, units),
                            demand.integerOperations);
            LimitThroughput(workItemsPerSecond,
                            ScaleServiceRate(prior.floatingPointOperationsPerSecondPerComputeUnit, units),
                            demand.floatingPointOperations);
            LimitThroughput(workItemsPerSecond, ScaleServiceRate(prior.float64OperationsPerSecondPerComputeUnit, units),
                            demand.float64Operations);
            LimitThroughput(workItemsPerSecond,
                            ScaleServiceRate(prior.specialFunctionOperationsPerSecondPerComputeUnit, units),
                            demand.specialFunctionOperations);
            LimitThroughput(workItemsPerSecond, ScaleServiceRate(prior.memoryBytesPerSecondPerComputeUnit, units),
                            EquivalentMemoryBytes(demand, prior.stridedMemoryEfficiencyPermille));
            LimitThroughput(workItemsPerSecond,
                            ScaleServiceRate(prior.synchronizationOperationsPerSecondPerComputeUnit, units),
                            demand.synchronizationOperations);
            return KernelPerformanceForecast{
                .estimate = {.workItemsPerSecond = workItemsPerSecond,
                             .launchNanoseconds = prior.launchNanoseconds,
                             .synchronizationNanoseconds = prior.synchronizationNanoseconds,
                             .sampleCount = 0,
                             .deterministic = deterministic},
                .evidence = {.kind = PerformanceEvidenceKind::Prior,
                             .confidencePermille = prior.confidencePermille,
                             .sampleCount = 0}};
        }

        /** @brief Forecast one unseen non-live artifact path for an exact backend stage. */
        [[nodiscard]] constexpr std::optional<ArtifactResolutionPrior>
        ForecastArtifact(BackendKind backend, ArtifactResolutionPath path, bool pipeline) const noexcept {
            if (!policy_.enabled || !valid_ || !ValidBackendKind(backend) || !ValidArtifactResolutionPath(path) ||
                path == ArtifactResolutionPath::Live) {
                return std::nullopt;
            }
            const BackendArtifactPrior& prior = policy_.artifacts[static_cast<size_t>(std::to_underlying(backend))];
            uint64_t nanoseconds = 0;
            if (path == ArtifactResolutionPath::PackageImport) {
                nanoseconds = prior.packageImportNanoseconds;
            } else if (path == ArtifactResolutionPath::PersistentImport) {
                nanoseconds = prior.persistentImportNanoseconds;
            } else {
                nanoseconds = pipeline ? prior.pipelineProductionNanoseconds : prior.executableProductionNanoseconds;
            }
            return ArtifactResolutionPrior{.nanoseconds = nanoseconds};
        }

        /** @brief Forecast persistent lookup as expected import and fallback-production contributions. */
        [[nodiscard]] constexpr std::optional<PersistentArtifactResolutionPrior>
        ForecastPersistent(BackendKind backend, bool pipeline) const noexcept {
            if (!policy_.enabled || !valid_ || !ValidBackendKind(backend)) {
                return std::nullopt;
            }
            const BackendArtifactPrior& prior = policy_.artifacts[static_cast<size_t>(std::to_underlying(backend))];
            const uint64_t production =
                pipeline ? prior.pipelineProductionNanoseconds : prior.executableProductionNanoseconds;
            const uint64_t missPermille = 1'000 - prior.persistentHitPermille;
            return PersistentArtifactResolutionPrior{
                .importNanoseconds = ScaleByPermille(prior.persistentImportNanoseconds, prior.persistentHitPermille),
                .productionNanoseconds = ScaleByPermille(production, static_cast<uint16_t>(missPermille)),
                .importPermille = prior.persistentHitPermille,
                .productionPermille = static_cast<uint16_t>(missPermille)};
        }

        [[nodiscard]] constexpr PerformancePriorPolicy Policy() const noexcept { return policy_; }

    private:
        [[nodiscard]] static constexpr uint64_t ScaleServiceRate(uint64_t perUnit, uint64_t units) noexcept {
            return perUnit > std::numeric_limits<uint64_t>::max() / units ? std::numeric_limits<uint64_t>::max()
                                                                          : perUnit * units;
        }

        static constexpr void LimitThroughput(uint64_t& throughput, uint64_t capacity, uint64_t demand) noexcept {
            if (demand == 0) {
                return;
            }
            throughput = std::min(throughput, std::max<uint64_t>(capacity / demand, 1));
        }

        [[nodiscard]] static constexpr uint64_t CeilDivide(uint64_t value, uint64_t divisor) noexcept {
            return value / divisor + static_cast<uint64_t>(value % divisor != 0);
        }

        [[nodiscard]] static constexpr uint64_t ScaleByPermille(uint64_t value, uint16_t permille) noexcept {
            if (value <= std::numeric_limits<uint64_t>::max() / 1'000) {
                return value * permille / 1'000;
            }
            return value / 1'000 * permille + value % 1'000 * permille / 1'000;
        }

        [[nodiscard]] static constexpr uint64_t ScaleByInversePermille(uint64_t value,
                                                                       uint16_t efficiencyPermille) noexcept {
            if (value <= std::numeric_limits<uint64_t>::max() / 1'000) {
                return CeilDivide(value * 1'000, efficiencyPermille);
            }
            const uint64_t quotient = value / efficiencyPermille;
            const uint64_t remainder = value % efficiencyPermille;
            const uint64_t whole = Detail::SaturatingDemandMultiply(quotient, 1'000);
            const uint64_t fraction =
                CeilDivide(Detail::SaturatingDemandMultiply(remainder, 1'000), efficiencyPermille);
            return Detail::SaturatingDemandAdd(whole, fraction);
        }

        [[nodiscard]] static constexpr uint64_t EquivalentMemoryBytes(const KernelResourceDemand& demand,
                                                                      uint16_t efficiencyPermille) noexcept {
            const uint64_t total = demand.MemoryBytes();
            const uint64_t strided = demand.StridedMemoryBytes();
            return Detail::SaturatingDemandAdd(total - strided, ScaleByInversePermille(strided, efficiencyPermille));
        }

        PerformancePriorPolicy policy_{};
        bool valid_ = true;
    };

    static_assert(std::is_trivially_copyable_v<PerformanceEvidence>);
    static_assert(std::is_trivially_copyable_v<KernelPerformanceForecast>);
    static_assert(std::is_trivially_copyable_v<DeviceKernelPrior>);
    static_assert(std::is_trivially_copyable_v<BackendArtifactPrior>);
    static_assert(std::is_trivially_copyable_v<ArtifactResolutionPrior>);
    static_assert(std::is_trivially_copyable_v<PersistentArtifactResolutionPrior>);
    static_assert(std::is_trivially_copyable_v<PerformancePriorPolicy>);
    static_assert(std::is_trivially_copyable_v<PerformancePriorModel>);

} // namespace Sora::Compute
