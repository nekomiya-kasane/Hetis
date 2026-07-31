/**
 * @file AutomaticPartitionPlanningBenchmark.cpp
 * @brief Transfer-inclusive automatic partition planning baselines.
 * @ingroup Compute
 */

#include "DispatchPlanningBenchmarkFixture.h"

namespace {

    class MultiAxisPlanningBenchmark {
    public:
        void Invoke() noexcept {
            const auto planned = Sora::Compute::PlanAutomaticDispatch(
                domain_, Sora::Compute::PartitionContract::Elementwise(domain_.rank), workload_, candidates_,
                {.maximumDevices = 2, .minimumImprovementPermille = 0});
            if (!planned || !planned->partitioned || planned->partitioned->axis != 2) {
                failed_ = true;
                return;
            }
            checksum_ += planned->partitioned->totalNanoseconds + planned->partitioned->shards.size();
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }

        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        [[nodiscard]] static Sora::Compute::PlacementCandidate Candidate(uint64_t identity) noexcept {
            const Sora::Compute::DeviceId device{identity, 1};
            return {.service = {identity, 2},
                    .device = {.id = device,
                               .backend = Sora::Compute::BackendKind::Vulkan,
                               .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                               .totalMemoryBytes = uint64_t{16} << 30U,
                               .maximumAllocationBytes = uint64_t{12} << 30U},
                    .target = {.device = device, .backend = Sora::Compute::BackendKind::Vulkan},
                    .queue = {identity, 3},
                    .snapshot = {.id = device,
                                 .state = Sora::Compute::DeviceState::Available,
                                 .generation = 1,
                                 .availableMemoryBytes = uint64_t{12} << 30U},
                    .executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live},
                    .workItemsPerSecond = 1'000'000'000,
                    .launchNanoseconds = 0,
                    .deterministic = true};
        }

        static constexpr Sora::Compute::DispatchDomain domain_{
            .rank = 3, .extents = {1, 3, 1'000}, .workItemCount = 3'000};
        static constexpr Sora::Compute::WorkloadDesc workload_{
            .kernelHash = {91, 92},
            .resourceDemand = {.floatingPointOperations = 2, .bytesRead = 8, .bytesWritten = 4},
            .workItemCount = domain_.workItemCount,
            .requiredDeviceMemoryBytes = 1U << 20U,
            .largestAllocationBytes = 1U << 20U};
        const std::array<Sora::Compute::PlacementCandidate, 2> candidates_{Candidate(1), Candidate(2)};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    DispatchPlanningBenchmarkFixture budget1{1, 2};
    DispatchPlanningBenchmarkFixture budget16{1, 2};
    MultiAxisPlanningBenchmark multiAxis;
    const Sora::Compute::AutomaticPartitionPolicy oneEvaluation{
        .maximumDevices = 2, .maximumPartitionEvaluations = 1, .minimumImprovementPermille = 0};
    const Sora::Compute::AutomaticPartitionPolicy sixteenEvaluations{
        .maximumDevices = 2, .maximumPartitionEvaluations = 16, .minimumImprovementPermille = 0};
    auto caseBudget1 = [&]() noexcept { budget1.InvokeAutomaticPartition(oneEvaluation); };
    auto caseBudget16 = [&]() noexcept { budget16.InvokeAutomaticPartition(sixteenEvaluations); };
    auto caseMultiAxis = [&]() noexcept { multiAxis.Invoke(); };
    const std::array cases{Sora::Benchmark::MakeCase("exact_partition_budget_1", caseBudget1),
                           Sora::Benchmark::MakeCase("exact_partition_budget_16", caseBudget16),
                           Sora::Benchmark::MakeCase("compute_only_three_axis", caseMultiAxis)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    const uint64_t checksum = budget1.Checksum() + budget16.Checksum() + multiAxis.Checksum();
    if (budget1.Failed() || budget16.Failed() || multiAxis.Failed() || checksum == 0 ||
        checksum == std::numeric_limits<uint64_t>::max()) {
        return 2;
    }
    std::println("metric,value,unit");
    std::println("automatic_partition_exact_budget_1,{:.6f},ns/decision", timing.MedianTime(0));
    std::println("automatic_partition_exact_budget_16,{:.6f},ns/decision", timing.MedianTime(1));
    std::println("automatic_partition_budget_16_over_budget_1,{:.6f},ratio", timing.PairedRatio(1, 0));
    std::println("automatic_partition_compute_only_three_axis,{:.6f},ns/decision", timing.MedianTime(2));
    std::println("checksum,{},value", checksum);
}
