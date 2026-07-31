/**
 * @file AutomaticPartitionPlanningCalibration.h
 * @brief Bounded process-local calibration for automatic partition search cost.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Invocation/DispatchPartition.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace Sora::Compute {

    inline constexpr uint32_t kMaximumAutomaticPlanningPlacementBucket = 6;
    inline constexpr uint32_t kMaximumAutomaticPlanningInputRouteBucket = 6;

    /** @brief Coarse structural coordinates controlling the cost of one exact partition evaluation. */
    struct AutomaticPartitionPlanningShape {
        uint8_t axisCount = 0;
        uint32_t placementCount = 0;
        uint32_t inputRouteCount = 0;

        /** @brief Return whether the shape can describe a legal multi-placement partition search. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return axisCount != 0 && axisCount <= kMaximumIndexRank && placementCount >= 2;
        }

        friend constexpr bool operator==(AutomaticPartitionPlanningShape,
                                         AutomaticPartitionPlanningShape) noexcept = default;
    };

    /** @brief Immutable empirical cost estimate for resolving one adaptive partition-search budget. */
    struct AutomaticPartitionPlanningEstimate {
        uint64_t meanNanosecondsPerEvaluation = 0;
        uint64_t meanAbsoluteDeviationNanoseconds = 0;
        uint32_t sampleCount = 0;

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return meanNanosecondsPerEvaluation != 0 && sampleCount != 0;
        }

        /** @brief Return the empirical planning-cost guard used by adaptive ROI resolution. */
        [[nodiscard]] constexpr uint64_t GuardedNanosecondsPerEvaluation() const noexcept {
            constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
            return meanAbsoluteDeviationNanoseconds > (maximum - meanNanosecondsPerEvaluation) / 2U
                       ? maximum
                       : meanNanosecondsPerEvaluation + 2U * meanAbsoluteDeviationNanoseconds;
        }

        friend constexpr bool operator==(AutomaticPartitionPlanningEstimate,
                                         AutomaticPartitionPlanningEstimate) noexcept = default;
    };

    /** @brief One completed exact-search observation, excluding model-publication overhead. */
    struct AutomaticPartitionPlanningObservation {
        AutomaticPartitionPlanningShape shape{};
        uint32_t evaluationCount = 0;
        uint64_t elapsedNanoseconds = 0;

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return shape.Valid() && evaluationCount != 0 && elapsedNanoseconds != 0;
        }
    };

    namespace Detail {

        inline constexpr size_t kAutomaticPlanningAxisBucketCount = kMaximumIndexRank;
        inline constexpr size_t kAutomaticPlanningPlacementBucketCount = kMaximumAutomaticPlanningPlacementBucket + 1;
        inline constexpr size_t kAutomaticPlanningInputRouteBucketCount = kMaximumAutomaticPlanningInputRouteBucket + 1;
        inline constexpr size_t kAutomaticPlanningCalibrationBucketCount = kAutomaticPlanningAxisBucketCount *
                                                                           kAutomaticPlanningPlacementBucketCount *
                                                                           kAutomaticPlanningInputRouteBucketCount;
        inline constexpr uint32_t kAutomaticPlanningStatisticBits = 24;
        inline constexpr uint32_t kAutomaticPlanningStatisticUnitNanoseconds = 256;
        inline constexpr uint64_t kAutomaticPlanningStatisticMask =
            (uint64_t{1} << kAutomaticPlanningStatisticBits) - 1U;
        inline constexpr uint32_t kAutomaticPlanningMaximumSamples = (uint32_t{1} << 16U) - 1U;
        inline constexpr uint32_t kAutomaticPlanningMaximumBlendWeight = 63;

        [[nodiscard]] constexpr uint32_t AutomaticPlanningPlacementBucket(uint32_t placementCount) noexcept {
            if (placementCount <= 1) {
                return 0;
            }
            return std::min<uint32_t>(std::bit_width(placementCount - 1U), kMaximumAutomaticPlanningPlacementBucket);
        }

        [[nodiscard]] constexpr uint32_t AutomaticPlanningInputRouteBucket(uint32_t inputRouteCount) noexcept {
            return inputRouteCount == 0
                       ? 0
                       : std::min<uint32_t>(std::bit_width(inputRouteCount), kMaximumAutomaticPlanningInputRouteBucket);
        }

        [[nodiscard]] constexpr size_t AutomaticPlanningBucket(AutomaticPartitionPlanningShape shape) noexcept {
            const size_t axis = shape.axisCount - 1U;
            const size_t placement = AutomaticPlanningPlacementBucket(shape.placementCount);
            const size_t inputRoutes = AutomaticPlanningInputRouteBucket(shape.inputRouteCount);
            return (axis * kAutomaticPlanningPlacementBucketCount + placement) *
                       kAutomaticPlanningInputRouteBucketCount +
                   inputRoutes;
        }

        [[nodiscard]] constexpr uint64_t QuantizeAutomaticPlanningStatistic(uint64_t nanoseconds) noexcept {
            const uint64_t units = nanoseconds / kAutomaticPlanningStatisticUnitNanoseconds +
                                   (nanoseconds % kAutomaticPlanningStatisticUnitNanoseconds != 0 ? 1U : 0U);
            return std::min(units, kAutomaticPlanningStatisticMask);
        }

        [[nodiscard]] constexpr uint64_t PackAutomaticPlanningEstimate(uint64_t mean, uint64_t deviation,
                                                                       uint32_t samples) noexcept {
            return (uint64_t{samples} << (2U * kAutomaticPlanningStatisticBits)) |
                   (deviation << kAutomaticPlanningStatisticBits) | mean;
        }

        [[nodiscard]] constexpr AutomaticPartitionPlanningEstimate
        UnpackAutomaticPlanningEstimate(uint64_t packed) noexcept {
            const uint64_t mean = packed & kAutomaticPlanningStatisticMask;
            const uint64_t deviation = (packed >> kAutomaticPlanningStatisticBits) & kAutomaticPlanningStatisticMask;
            return {.meanNanosecondsPerEvaluation = mean * kAutomaticPlanningStatisticUnitNanoseconds,
                    .meanAbsoluteDeviationNanoseconds = deviation * kAutomaticPlanningStatisticUnitNanoseconds,
                    .sampleCount = static_cast<uint32_t>(packed >> (2U * kAutomaticPlanningStatisticBits))};
        }

    } // namespace Detail

    /** @brief Frozen allocation-free calibration view used for one deterministic HEFT planning attempt. */
    class AutomaticPartitionPlanningSnapshot final {
    public:
        /** @brief Return the current estimate for @p shape when calibrated. */
        [[nodiscard]] constexpr std::optional<AutomaticPartitionPlanningEstimate>
        Estimate(AutomaticPartitionPlanningShape shape) const noexcept {
            if (!shape.Valid()) {
                return std::nullopt;
            }
            const auto estimate =
                Detail::UnpackAutomaticPlanningEstimate(buckets_[Detail::AutomaticPlanningBucket(shape)]);
            return estimate.Valid() ? std::optional{estimate} : std::nullopt;
        }

    private:
        friend class AutomaticPartitionPlanningCalibration;

        std::array<uint64_t, Detail::kAutomaticPlanningCalibrationBucketCount> buckets_{};
    };

    /**
     * @brief Fixed-capacity atomic telemetry for process-local partition-search cost.
     * @details Shape coordinates are compile-time bounded and map directly to atomic cells, so observation performs no
     * allocation, lookup, clock read, or backend call. Each cell stores a bounded moving mean and mean absolute
     * deviation whose last 64 samples retain equal weight. Planners freeze all cells once per attempt before making any
     * adaptive decision. Statistics are rounded upward to 256-nanosecond units and saturate at about 4.29 seconds.
     */
    class AutomaticPartitionPlanningCalibration final {
    public:
        /** @brief Freeze every calibration cell into one deterministic planning snapshot. */
        [[nodiscard]] AutomaticPartitionPlanningSnapshot Snapshot() const noexcept {
            AutomaticPartitionPlanningSnapshot result;
            for (size_t index = 0; index < buckets_.size(); ++index) {
                result.buckets_[index] = buckets_[index].load(std::memory_order_relaxed);
            }
            return result;
        }

        /** @brief Publish one completed search observation; invalid observations are rejected without mutation. */
        [[nodiscard]] bool Observe(AutomaticPartitionPlanningObservation observation) noexcept {
            if (!observation.Valid()) {
                return false;
            }
            const uint64_t rounded = observation.elapsedNanoseconds / observation.evaluationCount +
                                     (observation.elapsedNanoseconds % observation.evaluationCount != 0 ? 1U : 0U);
            const uint64_t observed = Detail::QuantizeAutomaticPlanningStatistic(rounded);
            auto& cell = buckets_[Detail::AutomaticPlanningBucket(observation.shape)];
            uint64_t current = cell.load(std::memory_order_relaxed);
            for (;;) {
                const auto estimate = Detail::UnpackAutomaticPlanningEstimate(current);
                const uint32_t weight = std::min(estimate.sampleCount, Detail::kAutomaticPlanningMaximumBlendWeight);
                const uint64_t previousMean =
                    estimate.meanNanosecondsPerEvaluation / Detail::kAutomaticPlanningStatisticUnitNanoseconds;
                const uint64_t previousDeviation =
                    estimate.meanAbsoluteDeviationNanoseconds / Detail::kAutomaticPlanningStatisticUnitNanoseconds;
                const uint64_t mean =
                    weight == 0 ? observed : (previousMean * weight + observed + weight / 2U) / (weight + 1U);
                const uint64_t absoluteDeviation = observed > mean ? observed - mean : mean - observed;
                const uint64_t deviation =
                    weight == 0 ? 0 : (previousDeviation * weight + absoluteDeviation + weight / 2U) / (weight + 1U);
                const uint32_t samples = std::min(estimate.sampleCount + 1U, Detail::kAutomaticPlanningMaximumSamples);
                const uint64_t replacement = Detail::PackAutomaticPlanningEstimate(
                    std::min(mean, Detail::kAutomaticPlanningStatisticMask),
                    std::min(deviation, Detail::kAutomaticPlanningStatisticMask), samples);
                if (cell.compare_exchange_weak(current, replacement, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
                    return true;
                }
            }
        }

    private:
        std::array<std::atomic<uint64_t>, Detail::kAutomaticPlanningCalibrationBucketCount> buckets_{};
    };

    static_assert(std::is_trivially_copyable_v<AutomaticPartitionPlanningShape>);
    static_assert(std::is_trivially_copyable_v<AutomaticPartitionPlanningEstimate>);
    static_assert(std::is_trivially_copyable_v<AutomaticPartitionPlanningObservation>);
    static_assert(std::is_trivially_copyable_v<AutomaticPartitionPlanningSnapshot>);

} // namespace Sora::Compute
