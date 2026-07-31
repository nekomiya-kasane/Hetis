/**
 * @file HeftPlanningBenchmark.cpp
 * @brief Independent baselines for calibrated HEFT ranking, placement, and physical lowering.
 * @ingroup Compute
 */

#include <Compute/DispatchPlanningBenchmarkFixture.h>
#include <Sora/Compute/Runtime/HeftPlanner.h>

#include <memory>

int main() {
    Sora::Compute::HeftPlanningPolicy wholeOnlyPolicy;
    wholeOnlyPolicy.automaticPartition.enabled = false;
    Sora::Compute::HeftPlanningPolicy automaticPolicy;
    automaticPolicy.automaticPartition.maximumPartitionEvaluations = 8;
    automaticPolicy.automaticPartition.minimumAmortizedBenefitNanoseconds = 0;
    automaticPolicy.automaticPartitionTuning.enabled = false;
    Sora::Compute::HeftPlanningPolicy rejectedPartitionPolicy;
    rejectedPartitionPolicy.automaticPartition.maximumPartitionEvaluations = 8;
    rejectedPartitionPolicy.automaticPartition.minimumAmortizedBenefitNanoseconds = 0;
    rejectedPartitionPolicy.automaticPartitionTuning.enabled = false;
    rejectedPartitionPolicy.automaticPartition.partitionOverheadNanoseconds = 100'000;
    Sora::Compute::HeftPlanningPolicy oneNeighborPolicy;
    oneNeighborPolicy.automaticPartition.enabled = false;
    oneNeighborPolicy.localSearch = {.maximumPasses = 1, .maximumNeighborEvaluations = 1};
    Sora::Compute::HeftPlanningPolicy eightNeighborPolicy;
    eightNeighborPolicy.automaticPartition.enabled = false;
    eightNeighborPolicy.localSearch = {.maximumPasses = 1, .maximumNeighborEvaluations = 8};
    auto adaptiveCalibration = std::make_shared<Sora::Compute::AutomaticPartitionPlanningCalibration>();
    constexpr Sora::Compute::AutomaticPartitionPlanningShape adaptiveShape{
        .axisCount = 1, .placementCount = 2, .inputRouteCount = 2};
    for (uint32_t sample = 0; sample < 4; ++sample) {
        if (!adaptiveCalibration->Observe(
                {.shape = adaptiveShape, .evaluationCount = 8, .elapsedNanoseconds = 64'000})) {
            return 2;
        }
    }
    auto deferredCalibration = adaptiveCalibration->BeginBatch();
    Sora::Compute::HeftPlanningPolicy adaptivePolicy;
    adaptivePolicy.dispatch.expectedReuseCount = 1'000;
    return RunHeftPlanningBenchmark(
        Sora::Compute::HeftPlanner{wholeOnlyPolicy}, Sora::Compute::HeftPlanner{automaticPolicy},
        Sora::Compute::HeftPlanner{}, Sora::Compute::HeftPlanner{rejectedPartitionPolicy},
        Sora::Compute::HeftPlanner{oneNeighborPolicy}, Sora::Compute::HeftPlanner{eightNeighborPolicy},
        Sora::Compute::HeftPlanner{adaptivePolicy, std::move(adaptiveCalibration)}, deferredCalibration);
}
