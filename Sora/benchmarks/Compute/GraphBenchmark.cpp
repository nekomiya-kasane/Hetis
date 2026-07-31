/**
 * @file GraphBenchmark.cpp
 * @brief Scaling baseline for sparse interval hazard dependency analysis.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Graph/HazardAnalysis.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <print>
#include <string_view>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr Sora::Compute::BufferId kBuffer{0x47, 0x52};
    constexpr Sora::Compute::SemanticHash kOperation{0x48, 0x5A};

    class GraphFixture {
    public:
        explicit GraphFixture(uint32_t writerCount) : expectedEdgeCount_(writerCount + 1) {
            nodes_.reserve(static_cast<size_t>(writerCount) + 2);
            accesses_.reserve(static_cast<size_t>(writerCount) + 2);
            for (uint32_t index = 0; index < writerCount; ++index) {
                AddNode(*Sora::Compute::ByteRegion::Make(static_cast<uint64_t>(index) * 4, 4),
                        Sora::Compute::AccessMode::Write);
            }
            const auto whole = *Sora::Compute::ByteRegion::Make(0, static_cast<uint64_t>(writerCount) * 4);
            AddNode(whole, Sora::Compute::AccessMode::Read);
            AddNode(whole, Sora::Compute::AccessMode::Write);
        }

        void Run() noexcept {
            auto edges = Sora::Compute::DeriveHazardDependencies({.nodes = nodes_, .accesses = accesses_, .edges = {}});
            if (!edges || edges->size() != expectedEdgeCount_) {
                failed_ = true;
                return;
            }
            checksum_ += edges->size() + edges->back().predecessor.index;
        }

        [[nodiscard]] size_t AccessCount() const noexcept { return accesses_.size(); }
        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] size_t Checksum() const noexcept { return checksum_; }

    private:
        void AddNode(Sora::Compute::ByteRegion region, Sora::Compute::AccessMode mode) {
            const uint32_t index = static_cast<uint32_t>(nodes_.size());
            nodes_.push_back({.kind = Sora::Compute::GraphNodeKind::Dispatch,
                              .operation = kOperation,
                              .workItemCount = 1,
                              .firstAccess = index,
                              .accessCount = 1});
            accesses_.push_back({.buffer = kBuffer, .region = region, .mode = mode});
        }

        std::vector<Sora::Compute::GraphNode> nodes_{};
        std::vector<Sora::Compute::ResourceAccess> accesses_{};
        size_t expectedEdgeCount_ = 0;
        size_t checksum_ = 0;
        bool failed_ = false;
    };

    template<size_t SampleCount>
    [[nodiscard]] auto Measure(GraphFixture& fixture, std::string_view name) {
        auto run = [&fixture]() noexcept { fixture.Run(); };
        const std::array cases{Sora::Benchmark::MakeCase(name, run)};
        return Sora::Benchmark::MeasureInterleaved<SampleCount>(cases, fixture.AccessCount(), kMinimumSampleTime);
    }

} // namespace

int main() {
    GraphFixture small{1U << 10U};
    GraphFixture medium{1U << 12U};
    GraphFixture large{1U << 14U};
    const auto smallTiming = Measure<kSamples>(small, "hazard_1k_intervals");
    const auto mediumTiming = Measure<kSamples>(medium, "hazard_4k_intervals");
    const auto largeTiming = Measure<kSamples>(large, "hazard_16k_intervals");
    if (small.Failed() || medium.Failed() || large.Failed()) {
        return 2;
    }

    const double smallTime = smallTiming.MedianTime(0);
    const double mediumTime = mediumTiming.MedianTime(0);
    const double largeTime = largeTiming.MedianTime(0);
    const size_t checksum = small.Checksum() ^ medium.Checksum() ^ large.Checksum();
    if (checksum == std::numeric_limits<size_t>::max()) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("hazard_1k_intervals,{:.6f},ns/access", smallTime);
    std::println("hazard_4k_intervals,{:.6f},ns/access", mediumTime);
    std::println("hazard_16k_intervals,{:.6f},ns/access", largeTime);
    std::println("hazard_4k_over_1k_normalized,{:.6f},ratio", mediumTime / smallTime);
    std::println("hazard_16k_over_4k_normalized,{:.6f},ratio", largeTime / mediumTime);
    std::println("hazard_1k_repetitions,{},count", smallTiming.repetitions);
    std::println("hazard_4k_repetitions,{},count", mediumTiming.repetitions);
    std::println("hazard_16k_repetitions,{},count", largeTiming.repetitions);
    std::println("checksum,{},value", checksum);
}
