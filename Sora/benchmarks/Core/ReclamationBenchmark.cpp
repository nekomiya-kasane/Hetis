/**
 * @file ReclamationBenchmark.cpp
 * @brief Measure stamped publication and safe-reclamation operation boundaries.
 */
#include <Sora/Core/Concurrency/StampedValue.h>
#include <Sora/Core/Concurrency/Reclamation.h>

#include <BenchmarkHarness.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
#include <span>
#include <thread>
#include <vector>

namespace {

    struct alignas(16) Node {
        std::uint64_t value = 1;
    };

    constexpr std::size_t kSamples = 9;
    constexpr std::size_t kOperations = 1'000'000;
    constexpr std::size_t kRetireBatch = 16'384;

    struct KeepAlive {
        void operator()(Node*) const noexcept {}
    };

    void Require(bool condition) noexcept {
        if (!condition) {
            std::abort();
        }
    }

    template<std::size_t CaseCount>
    void PrintGroup(std::string_view name, const std::array<Sora::Benchmark::Case, CaseCount>& cases,
                    const Sora::Benchmark::IndependentResult<CaseCount, kSamples>& result) {
        std::println("{}", name);
        for (std::size_t index = 0; index < cases.size(); ++index) {
            std::println("  {:24} median {:8.2f} ns/op  p95 {:8.2f}", cases[index].name, result.MedianTime(index),
                         result.P95Time(index));
        }
    }

    template<typename Operation, typename Cleanup>
    [[nodiscard]] double MeasurePrepared(Operation&& operation, Cleanup&& cleanup) {
        std::array<double, kSamples> samples{};
        for (std::size_t sample = 0; sample < kSamples; ++sample) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto begin = std::chrono::steady_clock::now();
            operation();
            const auto end = std::chrono::steady_clock::now();
            std::atomic_signal_fence(std::memory_order_seq_cst);
            samples[sample] = std::chrono::duration<double, std::nano>(end - begin).count();
            cleanup();
        }
        return Sora::Benchmark::Median(samples);
    }

    [[nodiscard]] std::vector<Node*> MakePointers(std::vector<Node>& nodes) {
        std::vector<Node*> pointers;
        pointers.reserve(nodes.size());
        for (Node& node : nodes) {
            pointers.push_back(std::addressof(node));
        }
        return pointers;
    }

    void MeasureRetirementBoundaries() {
        std::vector<Node> nodes(kRetireBatch);
        std::vector<Node*> pointers = MakePointers(nodes);
        const std::span<Node* const> batch{pointers};

        std::vector<Node*> allocated(kRetireBatch);
        const double allocate = MeasurePrepared(
            [&] {
                for (Node*& pointer : allocated) {
                    pointer = new Node;
                }
            },
            [&] {
                for (Node* pointer : allocated) {
                    delete pointer;
                }
            });
        for (Node*& pointer : allocated) {
            pointer = new Node;
        }
        const double deallocate = MeasurePrepared(
            [&] {
                for (Node*& pointer : allocated) {
                    delete pointer;
                    pointer = nullptr;
                }
            },
            [&] {
                for (Node*& pointer : allocated) {
                    pointer = new Node;
                }
            });
        for (Node* pointer : allocated) {
            delete pointer;
        }

        std::array<double, kSamples> coldSamples{};
        for (std::size_t sample = 0; sample < kSamples; ++sample) {
            Sora::Concurrency::HazardDomain coldDomain;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto begin = std::chrono::steady_clock::now();
            coldDomain.Retire(batch, KeepAlive{});
            const auto end = std::chrono::steady_clock::now();
            std::atomic_signal_fence(std::memory_order_seq_cst);
            coldSamples[sample] = std::chrono::duration<double, std::nano>(end - begin).count();
            Require(coldDomain.Collect() == batch.size());
        }

        Sora::Concurrency::HazardDomain singleDomain;
        for (Node* pointer : batch) {
            singleDomain.Retire(pointer, KeepAlive{});
        }
        (void)singleDomain.Collect();
        const double singleRetire = MeasurePrepared(
            [&] {
                for (Node* pointer : batch) {
                    singleDomain.Retire(pointer, KeepAlive{});
                }
            },
            [&] { Require(singleDomain.Collect() == batch.size()); });

        Sora::Concurrency::HazardDomain batchDomain;
        batchDomain.Retire(batch, KeepAlive{});
        (void)batchDomain.Collect();
        const double batchRetire = MeasurePrepared([&] { batchDomain.Retire(batch, KeepAlive{}); },
                                                   [&] { Require(batchDomain.Collect() == batch.size()); });

        std::println("allocation and retirement boundaries");
        std::println("  {:24} median {:8.2f} ns/object", "new Node", allocate / batch.size());
        std::println("  {:24} median {:8.2f} ns/object", "delete Node", deallocate / batch.size());
        std::println("  {:24} median {:8.2f} ns/object", "cold batch Retire",
                     Sora::Benchmark::Median(coldSamples) / batch.size());
        std::println("  {:24} median {:8.2f} ns/object", "single Retire", singleRetire / batch.size());
        std::println("  {:24} median {:8.2f} ns/object", "batch Retire(span)", batchRetire / batch.size());
    }

    void MeasureHazardCollection(std::size_t recordCount, std::size_t batchSize) {
        struct CollectState {
            std::unique_ptr<Sora::Concurrency::HazardDomain> domain;
            std::vector<Sora::Concurrency::HazardDomain::HazardPointer> hazards;
        };

        std::vector<Node> nodes(batchSize);
        std::vector<Node*> pointers = MakePointers(nodes);
        const std::span<Node* const> batch{pointers};
        Node protectedNode;
        std::atomic<Node*> protectedSource{std::addressof(protectedNode)};
        const std::size_t rounds = batchSize <= 8 ? 1'024 : 9;
        std::vector<CollectState> states;
        states.reserve(rounds);
        for (std::size_t round = 0; round < rounds; ++round) {
            CollectState& state = states.emplace_back(std::make_unique<Sora::Concurrency::HazardDomain>());
            state.hazards.reserve(recordCount);
            for (std::size_t index = 0; index < recordCount; ++index) {
                state.hazards.push_back(state.domain->MakeHazardPointer());
                (void)state.hazards.back().Protect(protectedSource);
            }
            state.domain->Retire(batch, KeepAlive{});
        }
        const double collect = MeasurePrepared(
            [&] {
                std::size_t reclaimed = 0;
                for (CollectState& state : states) {
                    reclaimed += state.domain->Collect();
                }
                Require(reclaimed == rounds * batch.size());
            },
            [&] {
                for (CollectState& state : states) {
                    state.domain->Retire(batch, KeepAlive{});
                }
            });
        for (CollectState& state : states) {
            (void)state.domain->Collect();
        }
        std::println("  records {:3}, batch {:5}: {:8.2f} ns/object", recordCount, batchSize,
                     collect / static_cast<double>(rounds * batchSize));
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
    const auto readResult =
        Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(readCases, 1, std::chrono::milliseconds{30});
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
    const auto casResult =
        Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(casCases, 1, std::chrono::milliseconds{30});
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
        const double nsPerOperation =
            std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(readerCount * kOperations);
        std::println("hazard readers {:2}: {:8.2f} ns/op aggregate, checksum {}", readerCount, nsPerOperation,
                     total.load(std::memory_order_relaxed));
    }

    MeasureRetirementBoundaries();
    std::println("hazard collection scan");
    for (std::size_t recordCount : {std::size_t{0}, std::size_t{1}, std::size_t{8}, std::size_t{64}}) {
        MeasureHazardCollection(recordCount, 8);
        MeasureHazardCollection(recordCount, kRetireBatch);
    }
    return sink == 0;
}
