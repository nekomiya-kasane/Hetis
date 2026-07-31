#include <Sora/Compute/Runtime/PerformanceModel.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr size_t BucketFor(uint64_t units) noexcept {
            return units == 0 ? 0 : std::bit_width(units) - 1;
        }

        [[nodiscard]] constexpr uint64_t Rate(uint64_t units, uint64_t nanoseconds) noexcept {
            if (units == 0 || nanoseconds == 0) {
                return 0;
            }
            constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000;
            const uint64_t quotient = units / nanoseconds;
            if (quotient > std::numeric_limits<uint64_t>::max() / kNanosecondsPerSecond) {
                return std::numeric_limits<uint64_t>::max();
            }
            const uint64_t whole = quotient * kNanosecondsPerSecond;
            const uint64_t remainder = units % nanoseconds;
            const long double fractional = static_cast<long double>(remainder) * kNanosecondsPerSecond / nanoseconds;
            const uint64_t tail = static_cast<uint64_t>(std::ceil(fractional));
            return whole > std::numeric_limits<uint64_t>::max() - tail ? std::numeric_limits<uint64_t>::max()
                                                                       : whole + tail;
        }

        [[nodiscard]] uint64_t Blend(uint64_t previous, uint64_t observed, uint32_t previousWeight) noexcept {
            if (previousWeight == 0) {
                return observed;
            }
            const long double value =
                (static_cast<long double>(previous) * previousWeight + observed) / (previousWeight + 1U);
            const long double maximum = static_cast<long double>(std::numeric_limits<uint64_t>::max());
            return value >= maximum ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(value + 0.5L);
        }

        [[nodiscard]] constexpr uint32_t IncrementBounded(uint32_t value, uint32_t maximum) noexcept {
            return value < maximum ? value + 1U : value;
        }

        static_assert(IncrementBounded(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()) ==
                      std::numeric_limits<uint32_t>::max());

        struct KernelKeyLess {
            [[nodiscard]] constexpr bool operator()(const KernelCalibrationKey& left,
                                                    const KernelCalibrationKey& right) const noexcept {
                const ArtifactKey leftArtifact = MakeArtifactKey(ExecutableKey{
                    .lowered = left.kernel, .target = left.target, .specialization = left.specialization});
                const ArtifactKey rightArtifact = MakeArtifactKey(ExecutableKey{
                    .lowered = right.kernel, .target = right.target, .specialization = right.specialization});
                if (std::ranges::lexicographical_compare(leftArtifact.Words(), rightArtifact.Words())) {
                    return true;
                }
                if (std::ranges::lexicographical_compare(rightArtifact.Words(), leftArtifact.Words())) {
                    return false;
                }
                return left.queue < right.queue;
            }
        };

        struct TransferKeyLess {
            [[nodiscard]] constexpr bool operator()(const TransferCalibrationKey& left,
                                                    const TransferCalibrationKey& right) const noexcept {
                const auto words = [](const TransferCalibrationKey& key) {
                    return std::array{key.service.low,
                                      key.service.high,
                                      key.engine.low,
                                      key.engine.high,
                                      key.source.device.low,
                                      key.source.device.high,
                                      key.source.domain.low,
                                      key.source.domain.high,
                                      key.destination.device.low,
                                      key.destination.device.high,
                                      key.destination.domain.low,
                                      key.destination.domain.high,
                                      uint64_t{std::to_underlying(key.source.backend)},
                                      uint64_t{std::to_underlying(key.destination.backend)}};
                };
                return words(left) < words(right);
            }
        };

        [[nodiscard]] constexpr size_t PathIndex(ArtifactResolutionPath path) noexcept {
            return static_cast<size_t>(std::to_underlying(path));
        }

        template<typename Estimate>
        [[nodiscard]] std::optional<Estimate>
        NearestEstimate(const std::array<Estimate, kPerformanceBucketCount>& buckets, uint64_t units) noexcept {
            const size_t selected = BucketFor(units);
            if (buckets[selected].Valid()) {
                return buckets[selected];
            }
            for (size_t distance = 1; distance < kPerformanceBucketCount; ++distance) {
                if (selected >= distance && buckets[selected - distance].Valid()) {
                    return buckets[selected - distance];
                }
                if (selected + distance < kPerformanceBucketCount && buckets[selected + distance].Valid()) {
                    return buckets[selected + distance];
                }
            }
            return std::nullopt;
        }

        template<typename Entry, typename Key, typename LessFunction>
        [[nodiscard]] auto FindEntry(std::vector<Entry>& entries, const Key& key, LessFunction less) noexcept {
            return std::ranges::lower_bound(entries, key, less, &Entry::key);
        }

        template<typename Entry>
        void EvictOldest(std::vector<Entry>& entries) noexcept {
            const auto oldest = std::ranges::min_element(entries, {}, &Entry::updateSequence);
            entries.erase(oldest);
        }

    } // namespace

    const PerformanceSnapshot& PerformanceSnapshot::Empty() noexcept {
        static const PerformanceSnapshot empty;
        return empty;
    }

    std::optional<KernelPerformanceEstimate> PerformanceSnapshot::Estimate(const KernelCalibrationKey& key,
                                                                           uint64_t workItemCount) const noexcept {
        if (!key.Valid() || workItemCount == 0) {
            return std::nullopt;
        }
        const auto found = std::ranges::lower_bound(kernels_, key, KernelKeyLess{}, &KernelCalibrationEntry::key);
        return found != kernels_.end() && found->key == key ? NearestEstimate(found->buckets, workItemCount)
                                                            : std::nullopt;
    }

    std::optional<TransferPerformanceEstimate> PerformanceSnapshot::Estimate(const TransferCalibrationKey& key,
                                                                             uint64_t bytes) const noexcept {
        if (!key.Valid() || bytes == 0) {
            return std::nullopt;
        }
        const auto found = std::ranges::lower_bound(transfers_, key, TransferKeyLess{}, &TransferCalibrationEntry::key);
        return found != transfers_.end() && found->key == key ? NearestEstimate(found->buckets, bytes) : std::nullopt;
    }

    std::optional<ArtifactResolutionEstimate>
    PerformanceSnapshot::Estimate(const ArtifactKey& key, ArtifactResolutionPath path) const noexcept {
        if (!key.Valid() || !ValidArtifactResolutionPath(path) || path == ArtifactResolutionPath::Live) {
            return std::nullopt;
        }
        const auto found = std::ranges::lower_bound(artifactResolutions_, key, ArtifactKeyLess{},
                                                    &ArtifactResolutionCalibrationEntry::key);
        const ArtifactResolutionEstimate& estimate = found != artifactResolutions_.end() && found->key == key
                                                         ? found->paths[PathIndex(path)]
                                                         : ArtifactResolutionEstimate{};
        return estimate.Valid() ? std::optional{estimate} : std::nullopt;
    }

    std::optional<PersistentArtifactResolutionEstimate>
    PerformanceSnapshot::EstimatePersistentResolution(const ArtifactKey& key) const noexcept {
        if (!key.Valid()) {
            return std::nullopt;
        }
        const auto found = std::ranges::lower_bound(artifactResolutions_, key, ArtifactKeyLess{},
                                                    &ArtifactResolutionCalibrationEntry::key);
        if (found == artifactResolutions_.end() || found->key != key ||
            (found->persistentImportWeight == 0 && found->persistentProductionWeight == 0)) {
            return std::nullopt;
        }
        const ArtifactResolutionEstimate& imported = found->paths[PathIndex(ArtifactResolutionPath::PersistentImport)];
        const ArtifactResolutionEstimate& produced =
            found->paths[PathIndex(ArtifactResolutionPath::PersistentProduction)];
        const uint64_t weight =
            static_cast<uint64_t>(found->persistentImportWeight) + found->persistentProductionWeight;
        const long double maximum = static_cast<long double>(std::numeric_limits<uint64_t>::max());
        const auto contribution = [weight, maximum](const ArtifactResolutionEstimate& estimate,
                                                    uint32_t outcomeWeight) {
            if (!estimate.Valid() || outcomeWeight == 0) {
                return uint64_t{0};
            }
            const long double weighted = static_cast<long double>(estimate.nanoseconds) * outcomeWeight / weight;
            return weighted >= maximum ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(weighted + 0.5L);
        };
        return PersistentArtifactResolutionEstimate{
            .importNanoseconds = contribution(imported, found->persistentImportWeight),
            .productionNanoseconds = contribution(produced, found->persistentProductionWeight),
            .importWeight = found->persistentImportWeight,
            .productionWeight = found->persistentProductionWeight,
        };
    }

    std::shared_ptr<const PerformanceSnapshot> PerformanceModel::Snapshot() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
        return snapshot_.load(std::memory_order_acquire);
#else
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
#endif
    }

    void PerformanceModel::Publish(std::shared_ptr<const PerformanceSnapshot> snapshot) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
        snapshot_.store(std::move(snapshot), std::memory_order_release);
#else
        std::atomic_store_explicit(&snapshot_, std::move(snapshot), std::memory_order_release);
#endif
    }

    std::expected<void, PerformanceModelError>
    PerformanceModel::Update(const KernelPerformanceObservation& observation) noexcept {
        if (!limits_.Valid()) {
            return std::unexpected(PerformanceModelError::InvalidLimits);
        }
        const uint64_t rate = Rate(observation.workItemCount, observation.computeNanoseconds);
        if (!observation.key.Valid() || rate == 0) {
            return std::unexpected(PerformanceModelError::InvalidObservation);
        }
        try {
            std::lock_guard lock(writer_);
            auto next = std::make_shared<PerformanceSnapshot>();
            if (const auto current = Snapshot()) {
                next->kernels_ = current->kernels_;
                next->transfers_ = current->transfers_;
                next->artifactResolutions_ = current->artifactResolutions_;
            }
            auto found = FindEntry(next->kernels_, observation.key, KernelKeyLess{});
            if (found == next->kernels_.end() || found->key != observation.key) {
                if (next->kernels_.size() == limits_.maximumKernelKeys) {
                    EvictOldest(next->kernels_);
                    found = FindEntry(next->kernels_, observation.key, KernelKeyLess{});
                }
                found = next->kernels_.insert(found, KernelCalibrationEntry{.key = observation.key});
            }
            KernelPerformanceEstimate& estimate = found->buckets[BucketFor(observation.workItemCount)];
            const uint32_t weight = std::min(estimate.sampleCount, limits_.maximumSamplesPerBucket - 1U);
            estimate.workItemsPerSecond = Blend(estimate.workItemsPerSecond, rate, weight);
            estimate.launchNanoseconds = Blend(estimate.launchNanoseconds, observation.launchNanoseconds, weight);
            estimate.synchronizationNanoseconds =
                Blend(estimate.synchronizationNanoseconds, observation.synchronizationNanoseconds, weight);
            estimate.sampleCount = IncrementBounded(estimate.sampleCount, limits_.maximumSamplesPerBucket);
            estimate.deterministic = estimate.deterministic && observation.deterministic;
            found->updateSequence = ++updateSequence_;
            Publish(std::move(next));
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PerformanceModelError::OutOfMemory);
        }
    }

    std::expected<void, PerformanceModelError>
    PerformanceModel::Update(const TransferPerformanceObservation& observation) noexcept {
        if (!limits_.Valid()) {
            return std::unexpected(PerformanceModelError::InvalidLimits);
        }
        const uint64_t rate = Rate(observation.bytes, observation.payloadNanoseconds);
        if (!observation.key.Valid() || rate == 0) {
            return std::unexpected(PerformanceModelError::InvalidObservation);
        }
        try {
            std::lock_guard lock(writer_);
            auto next = std::make_shared<PerformanceSnapshot>();
            if (const auto current = Snapshot()) {
                next->kernels_ = current->kernels_;
                next->transfers_ = current->transfers_;
                next->artifactResolutions_ = current->artifactResolutions_;
            }
            auto found = FindEntry(next->transfers_, observation.key, TransferKeyLess{});
            if (found == next->transfers_.end() || found->key != observation.key) {
                if (next->transfers_.size() == limits_.maximumTransferKeys) {
                    EvictOldest(next->transfers_);
                    found = FindEntry(next->transfers_, observation.key, TransferKeyLess{});
                }
                found = next->transfers_.insert(found, TransferCalibrationEntry{.key = observation.key});
            }
            TransferPerformanceEstimate& estimate = found->buckets[BucketFor(observation.bytes)];
            const uint32_t weight = std::min(estimate.sampleCount, limits_.maximumSamplesPerBucket - 1U);
            estimate.bytesPerSecond = Blend(estimate.bytesPerSecond, rate, weight);
            estimate.latencyNanoseconds = Blend(estimate.latencyNanoseconds, observation.latencyNanoseconds, weight);
            estimate.sampleCount = IncrementBounded(estimate.sampleCount, limits_.maximumSamplesPerBucket);
            found->updateSequence = ++updateSequence_;
            Publish(std::move(next));
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PerformanceModelError::OutOfMemory);
        }
    }

    std::expected<void, PerformanceModelError>
    PerformanceModel::Update(const ArtifactResolutionObservation& observation) noexcept {
        if (!limits_.Valid()) {
            return std::unexpected(PerformanceModelError::InvalidLimits);
        }
        if (!observation.key.Valid() || !ValidArtifactResolutionPath(observation.path) ||
            observation.path == ArtifactResolutionPath::Live || observation.nanoseconds == 0) {
            return std::unexpected(PerformanceModelError::InvalidObservation);
        }
        try {
            std::lock_guard lock(writer_);
            auto next = std::make_shared<PerformanceSnapshot>();
            if (const auto current = Snapshot()) {
                next->kernels_ = current->kernels_;
                next->transfers_ = current->transfers_;
                next->artifactResolutions_ = current->artifactResolutions_;
            }
            auto found = FindEntry(next->artifactResolutions_, observation.key, ArtifactKeyLess{});
            if (found == next->artifactResolutions_.end() || found->key != observation.key) {
                if (next->artifactResolutions_.size() == limits_.maximumArtifactKeys) {
                    EvictOldest(next->artifactResolutions_);
                    found = FindEntry(next->artifactResolutions_, observation.key, ArtifactKeyLess{});
                }
                found = next->artifactResolutions_.insert(found,
                                                          ArtifactResolutionCalibrationEntry{.key = observation.key});
            }
            ArtifactResolutionEstimate& estimate = found->paths[PathIndex(observation.path)];
            const uint32_t weight = std::min(estimate.sampleCount, limits_.maximumSamplesPerBucket - 1U);
            estimate.nanoseconds = Blend(estimate.nanoseconds, observation.nanoseconds, weight);
            estimate.sampleCount = IncrementBounded(estimate.sampleCount, limits_.maximumSamplesPerBucket);
            if (observation.path == ArtifactResolutionPath::PersistentImport ||
                observation.path == ArtifactResolutionPath::PersistentProduction) {
                const uint64_t persistentWeight =
                    static_cast<uint64_t>(found->persistentImportWeight) + found->persistentProductionWeight;
                if (persistentWeight >= limits_.maximumSamplesPerBucket) {
                    found->persistentImportWeight /= 2U;
                    found->persistentProductionWeight /= 2U;
                }
                uint32_t& outcomeWeight = observation.path == ArtifactResolutionPath::PersistentImport
                                              ? found->persistentImportWeight
                                              : found->persistentProductionWeight;
                outcomeWeight = IncrementBounded(outcomeWeight, limits_.maximumSamplesPerBucket);
            }
            found->updateSequence = ++updateSequence_;
            Publish(std::move(next));
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PerformanceModelError::OutOfMemory);
        }
    }

    ArtifactResolutionObserver PerformanceModel::ResolutionObserver() noexcept {
        return {
            .context = this,
            .observe =
                [](void* context, const ArtifactResolutionObservation& observation) noexcept {
                    (void)static_cast<PerformanceModel*>(context)->Update(observation);
                },
        };
    }

    KernelPerformanceObserver PerformanceModel::KernelObserver() noexcept {
        return {
            .context = this,
            .observe =
                [](void* context, const KernelPerformanceObservation& observation) noexcept {
                    (void)static_cast<PerformanceModel*>(context)->Update(observation);
                },
        };
    }

} // namespace Sora::Compute
