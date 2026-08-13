/**
 * @file ReclamationBenchmark.cpp
 * @brief Measure stamped publication and safe-reclamation operation boundaries.
 */
#include <Sora/Core/Concurrency/PointerStamp.h>
#include <Sora/Core/Concurrency/Reclamation.h>

#include <BenchmarkHarness.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

namespace {

    struct alignas(16) Node {
        std::uint64_t value = 1;
    };

    constexpr std::size_t kSamples = 9;
    constexpr std::size_t kOperations = 1'000'000;

    template<std::size_t CaseCount>
    void PrintGroup(std::string_view name, const std::array<Sora::Benchmark::Case, CaseCount>& cases,
                    const Sora::Benchmark::IndependentResult<CaseCount, kSamples>& result) {
        std::println("{}", name);
        for (std::size_t index = 0; index < cases.size(); ++index) {
            std::println("  {:24} median {:8.2f} ns/op  p95 {:8.2f}", cases[index].name,
                         result.MedianTime(index), result.P95Time(index));
        }
    }

} // namespace

int main() {
    Node node;
    std::atomic<Node*> source{&node};
    Sora::Concurrency::AtomicVersionedPointer<Node, 4> versionedSource{
        Sora::Concurrency::VersionedPointer<Node, 4>{&node, 1}};
    std::atomic<std::uintptr_t> packedSource{reinterpret_cast<std::uintptr_t>(&node) | 1};
    Sora::Concurrency::HazardDomain hazards;
    auto hazard = hazards.MakeHazardPointer();
    Sora::Concurrency::EpochDomain epochs;
    auto epochParticipant = epochs.Register();
    Sora::Concurrency::RcuDomain rcu;
    auto rcuParticipant = rcu.Register();
    std::uint64_t sink = 0;

    auto rawRead = [&]() noexcept { sink += source.load(std::memory_order_acquire)->value; };
    auto hazardRead = [&]() noexcept { sink += hazard.Protect(source)->value; };
    auto epochRead = [&]() noexcept { sink += epochParticipant.Protect(source)->value; };
    auto rcuRead = [&]() noexcept { sink += rcuParticipant.Protect(source)->value; };
    const std::array readCases{
        Sora::Benchmark::MakeCase("raw acquire load", rawRead),
        Sora::Benchmark::MakeCase("hazard protect reused", hazardRead),
        Sora::Benchmark::MakeCase("epoch protect", epochRead),
        Sora::Benchmark::MakeCase("RCU protect", rcuRead),
    };
    const auto readResult = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(
        readCases, 1, std::chrono::milliseconds{30});
    PrintGroup("single-thread read-side fixed cost", readCases, readResult);

    auto plainCas = [&]() noexcept {
        Node* expected = source.load(std::memory_order_relaxed);
        (void)source.compare_exchange_weak(expected, &node, std::memory_order_relaxed);
    };
    auto versionedCas = [&]() noexcept {
        auto expected = versionedSource.Load(std::memory_order_relaxed);
        (void)versionedSource.CompareExchangeWeak(expected, expected.NextVersion(), std::memory_order_relaxed,
                                                  std::memory_order_relaxed);
    };
    auto packedCas = [&]() noexcept {
        std::uintptr_t expected = packedSource.load(std::memory_order_relaxed);
        const std::uintptr_t desired = (expected & ~std::uintptr_t{15}) | ((expected + 1) & 15);
        (void)packedSource.compare_exchange_weak(expected, desired, std::memory_order_relaxed);
    };
    const std::array casCases{
        Sora::Benchmark::MakeCase("pointer CAS", plainCas),
        Sora::Benchmark::MakeCase("manual packed CAS", packedCas),
        Sora::Benchmark::MakeCase("versioned pointer CAS", versionedCas),
    };
    const auto casResult = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(
        casCases, 1, std::chrono::milliseconds{30});
    PrintGroup("single-thread publication cost", casCases, casResult);

    for (std::size_t readerCount : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        std::barrier start{static_cast<std::ptrdiff_t>(readerCount + 1)};
        std::atomic<std::uint64_t> total = 0;
        std::vector<std::jthread> readers;
        readers.reserve(readerCount);
        for (std::size_t reader = 0; reader < readerCount; ++reader) {
            readers.emplace_back([&] {
                auto localHazard = hazards.MakeHazardPointer();
                start.arrive_and_wait();
                std::uint64_t local = 0;
                for (std::size_t operation = 0; operation < kOperations; ++operation) {
                    local += localHazard.Protect(source)->value;
                }
                total.fetch_add(local, std::memory_order_relaxed);
            });
        }
        const auto begin = std::chrono::steady_clock::now();
        start.arrive_and_wait();
        readers.clear();
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        const double nsPerOperation = std::chrono::duration<double, std::nano>(elapsed).count() /
                                      static_cast<double>(readerCount * kOperations);
        std::println("hazard readers {:2}: {:8.2f} ns/op aggregate, checksum {}", readerCount, nsPerOperation,
                     total.load(std::memory_order_relaxed));
    }

    constexpr std::size_t kRetireBatch = 16'384;
    const auto retireBegin = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kRetireBatch; ++index) {
        hazards.Retire(new Node{index});
    }
    const auto collectBegin = std::chrono::steady_clock::now();
    const std::size_t reclaimed = hazards.Collect();
    const auto collectEnd = std::chrono::steady_clock::now();
    std::println("hazard retire batch: {:.2f} ns/object; collect: {:.2f} ns/object; reclaimed {}",
                 std::chrono::duration<double, std::nano>(collectBegin - retireBegin).count() / kRetireBatch,
                 std::chrono::duration<double, std::nano>(collectEnd - collectBegin).count() / kRetireBatch,
                 reclaimed);
    return sink == 0;
}
