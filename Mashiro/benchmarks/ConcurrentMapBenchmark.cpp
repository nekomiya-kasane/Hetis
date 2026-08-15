/**
 * @file ConcurrentMapBenchmark.cpp
 * @brief Small reproducible throughput harness for the topology-specialized flat map and set.
 * @ingroup Testing
 */
#include <Mashiro/Core/ConcurrentMap.h>

#include <oneapi/tbb/concurrent_unordered_map.h>

#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Concurrency;

namespace {

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] double MillionOpsPerSecond(std::size_t operations, std::chrono::nanoseconds elapsed) {
        return static_cast<double>(operations) / std::chrono::duration<double>(elapsed).count() / 1.0e6;
    }

    template<class Fn>
    void Measure(const char* name, std::size_t operations, Fn&& fn) {
        const auto begin = Clock::now();
        fn();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        std::cout << name << ',' << MillionOpsPerSecond(operations, elapsed) << ',' << elapsed.count() << "\n";
    }

    void RunLocalLookup() {
        constexpr std::size_t kCapacity = 1U << 16;
        constexpr std::size_t kKeys = kCapacity / 2;
        constexpr std::size_t kIterations = 10'000'000;
        auto map = std::make_unique<ConcurrentMap<std::uint64_t, std::uint64_t, kCapacity, MapTopologies::Local>>();
        std::unordered_map<std::uint64_t, std::uint64_t> reference;
        reference.reserve(kKeys);
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            static_cast<void>(map->TryInsert(key, key + 1));
            reference.emplace(key, key + 1);
        }

        volatile std::uint64_t sink = 0;
        Measure("mashiro_local_find", kIterations, [&] {
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                sink ^= *map->FindRef(iteration & (kKeys - 1));
            }
        });
        Measure("std_unordered_map_local_find", kIterations, [&] {
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                sink ^= reference.find(iteration & (kKeys - 1))->second;
            }
        });
        std::cerr << "sink=" << sink << '\n';
    }

    template<MapTopology Topology>
    void RunConcurrentLookup(const char* name) {
        constexpr std::size_t kCapacity = 1U << 16;
        constexpr std::size_t kKeys = kCapacity / 2;
        constexpr std::size_t kThreads = 4;
        constexpr std::size_t kIterationsPerThread = 2'000'000;
        auto map = std::make_unique<ConcurrentMap<std::uint64_t, std::uint64_t, kCapacity, Topology>>();
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            static_cast<void>(map->TryInsert(key, key + 1));
        }

        std::vector<std::jthread> workers;
        workers.reserve(kThreads);
        const auto begin = Clock::now();
        for (std::size_t thread = 0; thread != kThreads; ++thread) {
            workers.emplace_back([&, thread] {
                std::uint64_t sink = 0;
                std::uint64_t value = 0;
                for (std::size_t iteration = 0; iteration != kIterationsPerThread; ++iteration) {
                    const bool found = map->TryGet((iteration + thread) & (kKeys - 1), value);
                    (void)found;
                    sink ^= value;
                }
                std::atomic_signal_fence(std::memory_order_seq_cst);
                if (sink == 0) {
                    std::cerr << "unexpected zero sink\n";
                }
            });
        }
        workers.clear();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        std::cout << name << ',' << MillionOpsPerSecond(kThreads * kIterationsPerThread, elapsed) << ','
                  << elapsed.count() << "\n";
    }

    void RunTbbConcurrentLookup() {
        constexpr std::size_t kKeys = 1U << 15;
        constexpr std::size_t kThreads = 4;
        constexpr std::size_t kIterationsPerThread = 2'000'000;
        oneapi::tbb::concurrent_unordered_map<std::uint64_t, std::uint64_t> map;
        map.reserve(kKeys);
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            map.emplace(key, key + 1);
        }

        std::vector<std::jthread> workers;
        workers.reserve(kThreads);
        const auto begin = Clock::now();
        for (std::size_t thread = 0; thread != kThreads; ++thread) {
            workers.emplace_back([&, thread] {
                std::uint64_t sink = 0;
                for (std::size_t iteration = 0; iteration != kIterationsPerThread; ++iteration) {
                    const auto iterator = map.find((iteration + thread) & (kKeys - 1));
                    sink ^= iterator->second;
                }
                std::atomic_signal_fence(std::memory_order_seq_cst);
                if (sink == 0) {
                    std::cerr << "unexpected zero sink\n";
                }
            });
        }
        workers.clear();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        std::cout << "tbb_concurrent_unordered_find," << MillionOpsPerSecond(kThreads * kIterationsPerThread, elapsed)
                  << ',' << elapsed.count() << "\n";
    }

    void RunDynamicConcurrentLookup() {
        constexpr std::size_t kKeys = 1U << 18;
        constexpr std::size_t kThreads = 4;
        constexpr std::size_t kIterationsPerThread = 2'000'000;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Mpmc> map{64};
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            if (map.TryInsert(key, key + 1) != MapInsertResult::Inserted) {
                std::terminate();
            }
        }

        std::barrier start{static_cast<std::ptrdiff_t>(kThreads + 1)};
        std::vector<std::jthread> workers;
        workers.reserve(kThreads);
        for (std::size_t thread = 0; thread != kThreads; ++thread) {
            workers.emplace_back([&, thread] {
                start.arrive_and_wait();
                std::uint64_t sink = 0;
                std::uint64_t value = 0;
                for (std::size_t iteration = 0; iteration != kIterationsPerThread; ++iteration) {
                    const bool found = map.TryGet((iteration + thread) & (kKeys - 1), value);
                    (void)found;
                    sink ^= value;
                }
                std::atomic_signal_fence(std::memory_order_seq_cst);
                if (sink == 0) {
                    std::cerr << "unexpected zero sink\n";
                }
            });
        }
        const auto begin = Clock::now();
        start.arrive_and_wait();
        workers.clear();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        std::cout << "mashiro_mpmc_dynamic_find," << MillionOpsPerSecond(kThreads * kIterationsPerThread, elapsed)
                  << ',' << elapsed.count() << "\n";
    }

    void RunTbbDynamicConcurrentLookup() {
        constexpr std::size_t kKeys = 1U << 18;
        constexpr std::size_t kThreads = 4;
        constexpr std::size_t kIterationsPerThread = 2'000'000;
        oneapi::tbb::concurrent_unordered_map<std::uint64_t, std::uint64_t> map;
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            map.emplace(key, key + 1);
        }

        std::barrier start{static_cast<std::ptrdiff_t>(kThreads + 1)};
        std::vector<std::jthread> workers;
        workers.reserve(kThreads);
        for (std::size_t thread = 0; thread != kThreads; ++thread) {
            workers.emplace_back([&, thread] {
                start.arrive_and_wait();
                std::uint64_t sink = 0;
                for (std::size_t iteration = 0; iteration != kIterationsPerThread; ++iteration) {
                    const auto iterator = map.find((iteration + thread) & (kKeys - 1));
                    sink ^= iterator->second;
                }
                std::atomic_signal_fence(std::memory_order_seq_cst);
                if (sink == 0) {
                    std::cerr << "unexpected zero sink\n";
                }
            });
        }
        const auto begin = Clock::now();
        start.arrive_and_wait();
        workers.clear();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        std::cout << "tbb_dynamic_find," << MillionOpsPerSecond(kThreads * kIterationsPerThread, elapsed) << ','
                  << elapsed.count() << "\n";
    }

    void RunMpmcInsert() {
        constexpr std::size_t kWriters = 4;
        constexpr std::size_t kKeysPerWriter = 100'000;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Mpmc> map{64};
        std::barrier start{static_cast<std::ptrdiff_t>(kWriters + 1)};
        std::vector<std::jthread> workers;
        workers.reserve(kWriters);
        for (std::size_t writer = 0; writer != kWriters; ++writer) {
            workers.emplace_back([&, writer] {
                start.arrive_and_wait();
                for (std::size_t offset = 0; offset != kKeysPerWriter; ++offset) {
                    const std::uint64_t key = writer * kKeysPerWriter + offset;
                    if (map.TryInsert(key, key + 1) != MapInsertResult::Inserted) {
                        std::terminate();
                    }
                }
            });
        }
        const auto begin = Clock::now();
        start.arrive_and_wait();
        workers.clear();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        std::cout << "mashiro_mpmc_dynamic_insert," << MillionOpsPerSecond(kWriters * kKeysPerWriter, elapsed) << ','
                  << elapsed.count() << "\n";
    }

    void RunTbbMpmcInsert() {
        constexpr std::size_t kWriters = 4;
        constexpr std::size_t kKeysPerWriter = 100'000;
        oneapi::tbb::concurrent_unordered_map<std::uint64_t, std::uint64_t> map;
        std::barrier start{static_cast<std::ptrdiff_t>(kWriters + 1)};
        std::vector<std::jthread> workers;
        workers.reserve(kWriters);
        for (std::size_t writer = 0; writer != kWriters; ++writer) {
            workers.emplace_back([&, writer] {
                start.arrive_and_wait();
                for (std::size_t offset = 0; offset != kKeysPerWriter; ++offset) {
                    const std::uint64_t key = writer * kKeysPerWriter + offset;
                    if (!map.emplace(key, key + 1).second) {
                        std::terminate();
                    }
                }
            });
        }
        const auto begin = Clock::now();
        start.arrive_and_wait();
        workers.clear();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        std::cout << "tbb_mpmc_dynamic_insert," << MillionOpsPerSecond(kWriters * kKeysPerWriter, elapsed) << ','
                  << elapsed.count() << "\n";
    }

} // namespace

int main() {
    std::cout << "case,mops_per_second,nanoseconds\n";
    RunLocalLookup();
    RunConcurrentLookup<MapTopologies::Spsc>("mashiro_spsc_find");
    RunConcurrentLookup<MapTopologies::Spmc>("mashiro_spmc_find");
    RunConcurrentLookup<MapTopologies::Mpmc>("mashiro_mpmc_find");
    RunTbbConcurrentLookup();
    RunDynamicConcurrentLookup();
    RunTbbDynamicConcurrentLookup();
    RunMpmcInsert();
    RunTbbMpmcInsert();
}
