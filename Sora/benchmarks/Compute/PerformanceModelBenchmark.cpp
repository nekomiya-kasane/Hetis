/**
 * @file PerformanceModelBenchmark.cpp
 * @brief Reader-path baseline for immutable heterogeneous performance calibration.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Runtime/AutomaticPartitionPlanningCalibration.h>
#include <Sora/Compute/Runtime/PerformanceModel.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <print>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);

    constexpr Sora::Compute::KernelCalibrationKey CalibrationKey() noexcept {
        return {.kernel = {1, 2},
                .target = {.device = {3, 4},
                           .backend = Sora::Compute::BackendKind::Cpu,
                           .hostArchitecture = Sora::CpuArchitecture::X64,
                           .operatingSystem = Sora::OperatingSystem::Windows,
                           .binaryFormat = Sora::BinaryFormat::PE,
                           .byteOrder = Sora::ByteOrder::Little,
                           .abi = Sora::AbiFamily::MSVC,
                           .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                           .addressBits = 64,
                           .architectureId = 5,
                           .compilerIdentity = {6, 7},
                           .driverIdentity = {8, 9}},
                .queue = {10, 11}};
    }

    constexpr Sora::Compute::ArtifactKey ResolutionKey() noexcept {
        return Sora::Compute::MakeArtifactKey(
            Sora::Compute::ExecutableKey{.lowered = CalibrationKey().kernel, .target = CalibrationKey().target});
    }

    class PerformanceFixture final {
    public:
        PerformanceFixture() {
            const auto updated =
                model_.Update({.key = CalibrationKey(), .workItemCount = 1U << 20U, .computeNanoseconds = 500'000});
            if (!updated) {
                failed_ = true;
                return;
            }
            if (!model_.Update({.key = ResolutionKey(),
                                .path = Sora::Compute::ArtifactResolutionPath::PersistentImport,
                                .nanoseconds = 1'000}) ||
                !model_.Update({.key = ResolutionKey(),
                                .path = Sora::Compute::ArtifactResolutionPath::PersistentProduction,
                                .nanoseconds = 9'000})) {
                failed_ = true;
                return;
            }
            snapshot_ = model_.Snapshot();
            resolutionObserver_ = model_.ResolutionObserver();
            for (uint32_t sample = 0; sample < 4; ++sample) {
                if (!automaticPlanning_.Observe(
                        {.shape = kAutomaticPlanningShape, .evaluationCount = 8, .elapsedNanoseconds = 64'000})) {
                    failed_ = true;
                    return;
                }
            }
            automaticPlanningSnapshot_ = automaticPlanning_.Snapshot();
        }

        void LoadSnapshot() noexcept {
            const auto snapshot = model_.Snapshot();
            checksum_ += snapshot ? snapshot->KernelEntries().size() : 0;
        }

        void LookupCalibration() noexcept {
            const auto estimate = snapshot_->Estimate(CalibrationKey(), 1U << 20U);
            checksum_ += estimate ? estimate->workItemsPerSecond : 0;
        }

        void LookupArtifactResolution() noexcept {
            const auto estimate = snapshot_->EstimatePersistentResolution(ResolutionKey());
            checksum_ += estimate ? estimate->importNanoseconds + estimate->productionNanoseconds : 0;
        }

        void ObserveArtifactResolution() noexcept {
            resolutionObserver_.Observe({.key = ResolutionKey(),
                                         .path = Sora::Compute::ArtifactResolutionPath::PackageImport,
                                         .nanoseconds = 1'500});
            ++checksum_;
        }

        void FreezeAutomaticPlanning() noexcept {
            automaticPlanningSnapshot_ = automaticPlanning_.Snapshot();
            checksum_ += automaticPlanningSnapshot_.Estimate(kAutomaticPlanningShape).has_value();
        }

        void LookupAutomaticPlanning() noexcept {
            const auto estimate = automaticPlanningSnapshot_.Estimate(kAutomaticPlanningShape);
            checksum_ += estimate ? estimate->GuardedNanosecondsPerEvaluation() : 0;
        }

        void ObserveAutomaticPlanning() noexcept {
            checksum_ += automaticPlanning_.Observe(
                {.shape = kAutomaticPlanningShape, .evaluationCount = 8, .elapsedNanoseconds = 64'000});
        }

        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }
        [[nodiscard]] bool Failed() const noexcept { return failed_ || !snapshot_; }

    private:
        static constexpr Sora::Compute::AutomaticPartitionPlanningShape kAutomaticPlanningShape{
            .axisCount = 1, .placementCount = 2, .inputRouteCount = 2};

        Sora::Compute::PerformanceModel model_{};
        std::shared_ptr<const Sora::Compute::PerformanceSnapshot> snapshot_{};
        Sora::Compute::ArtifactResolutionObserver resolutionObserver_{};
        Sora::Compute::AutomaticPartitionPlanningCalibration automaticPlanning_{};
        Sora::Compute::AutomaticPartitionPlanningSnapshot automaticPlanningSnapshot_{};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    PerformanceFixture fixture;
    if (fixture.Failed()) {
        return 2;
    }
    auto load = [&fixture]() noexcept { fixture.LoadSnapshot(); };
    auto lookup = [&fixture]() noexcept { fixture.LookupCalibration(); };
    auto freezePlanning = [&fixture]() noexcept { fixture.FreezeAutomaticPlanning(); };
    auto lookupPlanning = [&fixture]() noexcept { fixture.LookupAutomaticPlanning(); };
    auto observePlanning = [&fixture]() noexcept { fixture.ObserveAutomaticPlanning(); };
    auto lookupResolution = [&fixture]() noexcept { fixture.LookupArtifactResolution(); };
    auto observeResolution = [&fixture]() noexcept { fixture.ObserveArtifactResolution(); };
    const std::array cases{Sora::Benchmark::MakeCase("snapshot_load", load),
                           Sora::Benchmark::MakeCase("calibration_lookup", lookup),
                           Sora::Benchmark::MakeCase("automatic_planning_snapshot", freezePlanning),
                           Sora::Benchmark::MakeCase("automatic_planning_lookup", lookupPlanning),
                           Sora::Benchmark::MakeCase("automatic_planning_observation", observePlanning)};
    const auto timing = Sora::Benchmark::MeasureInterleaved<kSamples>(cases, 1, kMinimumSampleTime);
    const std::array resolutionCases{Sora::Benchmark::MakeCase("artifact_resolution_lookup", lookupResolution),
                                     Sora::Benchmark::MakeCase("artifact_resolution_observation", observeResolution)};
    const auto resolutionTiming =
        Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(resolutionCases, 1, kMinimumSampleTime);
    if (fixture.Checksum() == 0 || fixture.Checksum() == std::numeric_limits<uint64_t>::max()) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("performance_snapshot_load,{:.6f},ns/operation", timing.MedianTime(0));
    std::println("performance_calibration_lookup,{:.6f},ns/operation", timing.MedianTime(1));
    std::println("automatic_planning_snapshot,{:.6f},ns/operation", timing.MedianTime(2));
    std::println("automatic_planning_lookup,{:.6f},ns/operation", timing.MedianTime(3));
    std::println("automatic_planning_observation,{:.6f},ns/operation", timing.MedianTime(4));
    std::println("artifact_resolution_lookup,{:.6f},ns/operation", resolutionTiming.MedianTime(0));
    std::println("artifact_resolution_observation,{:.6f},ns/operation", resolutionTiming.MedianTime(1));
    std::println("repetitions,{},count", timing.repetitions);
    std::println("artifact_resolution_lookup_repetitions,{},count", resolutionTiming.repetitions[0]);
    std::println("artifact_resolution_observation_repetitions,{},count", resolutionTiming.repetitions[1]);
    std::println("checksum,{},value", fixture.Checksum());
}
