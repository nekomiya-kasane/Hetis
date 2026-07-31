/**
 * @file LinearChainPlanningBenchmark.cpp
 * @brief Independent baselines for exact linear-chain placement and physical lowering.
 * @ingroup Compute
 */

#include <Compute/DispatchPlanningBenchmarkFixture.h>
#include <Sora/Compute/Runtime/LinearChainPlanner.h>

int main() {
    return RunDispatchPlanningBenchmark(Sora::Compute::LinearChainPlanner{});
}
