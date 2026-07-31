/**
 * @file PerformanceModel.h
 * @brief Bounded copy-on-write calibration state for heterogeneous placement.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Memory/Transfer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace Sora::Compute {

    inline constexpr size_t kPerformanceBucketCount = 64;

    /** @brief Complete identity of one kernel calibration family; workload magnitude selects a bucket within it. */
    struct KernelCalibrationKey {
        SemanticHash kernel{};
        TargetFingerprint target{};
        SpecializationKey specialization{};
        QueueId queue{};

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return ValidSemanticHash(kernel) && target.Valid() && queue.Valid();
        }

        friend constexpr bool operator==(KernelCalibrationKey, KernelCalibrationKey) noexcept = default;
    };

    /** @brief Identity of measured transfer behavior, kept separate from immutable topology descriptors. */
    struct TransferCalibrationKey {
        BackendServiceId service{};
        TransferEngineId engine{};
        MemoryLocation source{};
        MemoryLocation destination{};

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return service.Valid() && engine.Valid() && source.Valid() && destination.Valid();
        }

        friend constexpr bool operator==(TransferCalibrationKey, TransferCalibrationKey) noexcept = default;
    };

    /** @brief Bounded aggregate for one kernel/work-size bucket. */
    struct KernelPerformanceEstimate {
        uint64_t workItemsPerSecond = 0;
        uint64_t launchNanoseconds = 0;
        uint64_t synchronizationNanoseconds = 0;
        uint32_t sampleCount = 0;
        bool deterministic = true;

        [[nodiscard]] constexpr bool Valid() const noexcept { return workItemsPerSecond != 0 && sampleCount != 0; }

        friend constexpr bool operator==(KernelPerformanceEstimate, KernelPerformanceEstimate) noexcept = default;
    };

    /** @brief Bounded aggregate for one transfer/size bucket. */
    struct TransferPerformanceEstimate {
        uint64_t bytesPerSecond = 0;
        uint64_t latencyNanoseconds = 0;
        uint32_t sampleCount = 0;

        [[nodiscard]] constexpr bool Valid() const noexcept { return bytesPerSecond != 0 && sampleCount != 0; }

        friend constexpr bool operator==(TransferPerformanceEstimate, TransferPerformanceEstimate) noexcept = default;
    };

    /** @brief Bounded aggregate for one exact artifact-resolution outcome. */
    struct ArtifactResolutionEstimate {
        uint64_t nanoseconds = 0;
        uint32_t sampleCount = 0;

        [[nodiscard]] constexpr bool Valid() const noexcept { return nanoseconds != 0 && sampleCount != 0; }

        friend constexpr bool operator==(ArtifactResolutionEstimate, ArtifactResolutionEstimate) noexcept = default;
    };

    /** @brief Expected import and production contributions for a persistent lookup with fallback. */
    struct PersistentArtifactResolutionEstimate {
        uint64_t importNanoseconds = 0;
        uint64_t productionNanoseconds = 0;
        uint32_t importWeight = 0;
        uint32_t productionWeight = 0;

        [[nodiscard]] constexpr bool Valid() const noexcept { return importWeight != 0 || productionWeight != 0; }

        friend constexpr bool operator==(PersistentArtifactResolutionEstimate,
                                         PersistentArtifactResolutionEstimate) noexcept = default;
    };

    /** @brief Externally measured kernel observation; the model never reads a clock on dispatch. */
    struct KernelPerformanceObservation {
        KernelCalibrationKey key{};
        uint64_t workItemCount = 0;
        uint64_t computeNanoseconds = 0;
        uint64_t launchNanoseconds = 0;
        uint64_t synchronizationNanoseconds = 0;
        bool deterministic = true;
    };

    /** @brief Immutable dispatch facts completed later by one backend-provided execution duration. */
    struct KernelPerformanceObservationDescriptor {
        KernelCalibrationKey key{};
        uint64_t workItemCount = 0;
        bool deterministic = true;

        [[nodiscard]] constexpr bool Valid() const noexcept { return key.Valid() && workItemCount != 0; }

        friend constexpr bool operator==(KernelPerformanceObservationDescriptor,
                                         KernelPerformanceObservationDescriptor) noexcept = default;
    };

    /** @brief Non-owning noexcept sink used only after a backend reports a completed timed dispatch. */
    struct KernelPerformanceObserver {
        void* context = nullptr;
        void (*observe)(void* context, const KernelPerformanceObservation& observation) noexcept = nullptr;

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return context != nullptr && observe != nullptr;
        }

        void operator()(const KernelPerformanceObservation& observation) const noexcept {
            if (*this) {
                observe(context, observation);
            }
        }
    };

    /**
     * @brief Externally measured transfer observation; topology remains owned by the resource inventory.
     * @details @p payloadNanoseconds excludes the fixed @p latencyNanoseconds component. Route projection reconstructs
     * end-to-end cost as fixed latency plus payload bytes divided by the calibrated rate.
     */
    struct TransferPerformanceObservation {
        TransferCalibrationKey key{};
        uint64_t bytes = 0;
        uint64_t payloadNanoseconds = 0;
        uint64_t latencyNanoseconds = 0;
    };

    /** @brief Fixed memory and statistical bounds for one process-local performance model. */
    struct PerformanceModelLimits {
        size_t maximumKernelKeys = 256;
        size_t maximumTransferKeys = 256;
        size_t maximumArtifactKeys = 256;
        uint32_t maximumSamplesPerBucket = 64;

        [[nodiscard]] constexpr bool Valid() const noexcept {
            return maximumKernelKeys != 0 && maximumTransferKeys != 0 && maximumArtifactKeys != 0 &&
                   maximumSamplesPerBucket != 0;
        }
    };

    /** @brief One immutable kernel calibration family with logarithmic workload buckets. */
    struct KernelCalibrationEntry {
        KernelCalibrationKey key{};
        std::array<KernelPerformanceEstimate, kPerformanceBucketCount> buckets{};
        uint64_t updateSequence = 0;
    };

    /** @brief One immutable transfer calibration family with logarithmic byte-count buckets. */
    struct TransferCalibrationEntry {
        TransferCalibrationKey key{};
        std::array<TransferPerformanceEstimate, kPerformanceBucketCount> buckets{};
        uint64_t updateSequence = 0;
    };

    /** @brief One immutable exact-artifact calibration family partitioned by successful resolution path. */
    struct ArtifactResolutionCalibrationEntry {
        ArtifactKey key{};
        std::array<ArtifactResolutionEstimate, kArtifactResolutionPathCount> paths{};
        uint32_t persistentImportWeight = 0;
        uint32_t persistentProductionWeight = 0;
        uint64_t updateSequence = 0;
    };

    /** @brief Immutable allocation-free lookup view published atomically to concurrent planners. */
    class PerformanceSnapshot final {
    public:
        /** @brief Return the process-wide immutable empty calibration baseline. */
        [[nodiscard]] static const PerformanceSnapshot& Empty() noexcept;

        [[nodiscard]] std::optional<KernelPerformanceEstimate> Estimate(const KernelCalibrationKey& key,
                                                                        uint64_t workItemCount) const noexcept;

        [[nodiscard]] std::optional<TransferPerformanceEstimate> Estimate(const TransferCalibrationKey& key,
                                                                          uint64_t bytes) const noexcept;

        [[nodiscard]] std::optional<ArtifactResolutionEstimate> Estimate(const ArtifactKey& key,
                                                                         ArtifactResolutionPath path) const noexcept;

        /** @brief Estimate a persistent-tier lookup with production fallback from exact historical outcomes. */
        [[nodiscard]] std::optional<PersistentArtifactResolutionEstimate>
        EstimatePersistentResolution(const ArtifactKey& key) const noexcept;

        [[nodiscard]] std::span<const KernelCalibrationEntry> KernelEntries() const noexcept { return kernels_; }
        [[nodiscard]] std::span<const TransferCalibrationEntry> TransferEntries() const noexcept { return transfers_; }
        [[nodiscard]] std::span<const ArtifactResolutionCalibrationEntry> ArtifactResolutionEntries() const noexcept {
            return artifactResolutions_;
        }

    private:
        friend class PerformanceModel;

        std::vector<KernelCalibrationEntry> kernels_{};
        std::vector<TransferCalibrationEntry> transfers_{};
        std::vector<ArtifactResolutionCalibrationEntry> artifactResolutions_{};
    };

    /** @brief Stable failure while validating or publishing one bounded calibration update. */
    enum class PerformanceModelError : uint8_t {
        InvalidLimits,
        InvalidObservation,
        OutOfMemory,
    };

    /**
     * @brief Serialized-writer, immutable-reader performance calibration service.
     * @details Callers measure only explicitly sampled work and submit observations here. Readers acquire one immutable
     * snapshot and perform binary searches without taking the writer mutex, allocating, reading clocks, or invoking
     * backend callbacks. Atomic shared-pointer publication itself follows the standard library's progress guarantee.
     */
    class PerformanceModel final {
    public:
        explicit PerformanceModel(PerformanceModelLimits limits = {}) noexcept : limits_(limits) {}

        PerformanceModel(const PerformanceModel&) = delete;
        PerformanceModel& operator=(const PerformanceModel&) = delete;

        [[nodiscard]] std::shared_ptr<const PerformanceSnapshot> Snapshot() const noexcept;
        [[nodiscard]] std::expected<void, PerformanceModelError>
        Update(const KernelPerformanceObservation& observation) noexcept;
        [[nodiscard]] std::expected<void, PerformanceModelError>
        Update(const TransferPerformanceObservation& observation) noexcept;
        [[nodiscard]] std::expected<void, PerformanceModelError>
        Update(const ArtifactResolutionObservation& observation) noexcept;

        /** @brief Return a non-owning sink suitable for successful resolver cold paths. */
        [[nodiscard]] ArtifactResolutionObserver ResolutionObserver() noexcept;

        /** @brief Return a non-owning sink suitable for completed backend-timed dispatches. */
        [[nodiscard]] KernelPerformanceObserver KernelObserver() noexcept;

        [[nodiscard]] PerformanceModelLimits Limits() const noexcept { return limits_; }

    private:
        void Publish(std::shared_ptr<const PerformanceSnapshot> snapshot) noexcept;

        PerformanceModelLimits limits_{};
        mutable std::mutex writer_{};
#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<const PerformanceSnapshot>> snapshot_{};
#else
        std::shared_ptr<const PerformanceSnapshot> snapshot_{};
#endif
        uint64_t updateSequence_ = 0;
    };

    static_assert(std::is_trivially_copyable_v<KernelCalibrationKey>);
    static_assert(std::is_trivially_copyable_v<TransferCalibrationKey>);
    static_assert(std::is_trivially_copyable_v<KernelPerformanceEstimate>);
    static_assert(std::is_trivially_copyable_v<TransferPerformanceEstimate>);
    static_assert(std::is_trivially_copyable_v<ArtifactResolutionEstimate>);
    static_assert(std::is_trivially_copyable_v<PersistentArtifactResolutionEstimate>);
    static_assert(std::is_trivially_copyable_v<KernelPerformanceObservation>);
    static_assert(std::is_trivially_copyable_v<KernelPerformanceObservationDescriptor>);
    static_assert(std::is_trivially_copyable_v<KernelPerformanceObserver>);
    static_assert(std::is_trivially_copyable_v<TransferPerformanceObservation>);
    static_assert(std::is_trivially_copyable_v<ArtifactResolutionObservation>);

} // namespace Sora::Compute
