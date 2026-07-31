/**
 * @file DispatchGraphLoweringBenchmark.cpp
 * @brief Isolated mixed whole/partitioned dispatch-DAG lowering baseline.
 * @ingroup Compute
 */

#include "DispatchPlanningBenchmarkFixture.h"

int main() {
    DispatchPlanningBenchmarkFixture fixture{2, 2};
    if (!fixture.PrepareMixedDagLowering()) {
        return 2;
    }
    auto lower = [&]() noexcept { fixture.InvokeMixedDagLowering(); };
    const std::array cases{Sora::Benchmark::MakeCase("partitioned_producer_whole_consumer", lower)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Failed() || fixture.Checksum() == 0 || fixture.Checksum() == std::numeric_limits<uint64_t>::max()) {
        return 3;
    }
    std::println("metric,value,unit");
    std::println("partitioned_producer_whole_consumer,{:.6f},ns/lowering", timing.MedianTime(0));
    std::println("partitioned_producer_whole_consumer_repetitions,{},count", timing.repetitions[0]);
    std::println("checksum,{},value", fixture.Checksum());
}
