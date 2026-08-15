/**
 * @file ConcurrentMapBenchmark.cpp
 * @brief Topology-valid throughput matrix for ConcurrentMap and ConcurrentSet.
 * @ingroup Testing
 */
#include <Mashiro/Core/ConcurrentMap.h>

#include <oneapi/tbb/concurrent_unordered_map.h>
#include <oneapi/tbb/concurrent_unordered_set.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Concurrency;

namespace {

    using Clock = std::chrono::steady_clock;

    std::atomic<std::uint64_t> benchmarkSink{};

    template<MapTopology Topology>
    inline constexpr std::size_t kReaderThreads =
        Topology.readers == MapCardinality::Multiple ? std::size_t{4} : std::size_t{1};

    template<MapTopology Topology>
    inline constexpr std::size_t kWriterThreads =
        Topology.writers == MapCardinality::Multiple ? std::size_t{4} : std::size_t{1};

    [[nodiscard]] double MillionOpsPerSecond(std::size_t operations, std::chrono::nanoseconds elapsed) noexcept {
        return static_cast<double>(operations) / std::chrono::duration<double>(elapsed).count() / 1.0e6;
    }

    void Report(std::string_view name, std::size_t operations, std::size_t threads,
                std::chrono::nanoseconds elapsed) {
        std::cout << name << ',' << threads << ',' << operations << ',' << MillionOpsPerSecond(operations, elapsed)
                  << ',' << elapsed.count() << '\n';
    }

    template<class Function>
    void Measure(std::string_view name, std::size_t operations, Function&& function) {
        const auto begin = Clock::now();
        benchmarkSink.fetch_xor(function(), std::memory_order_relaxed);
        Report(name, operations, 1, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin));
    }

    template<std::size_t ThreadCount, class Function>
    void MeasureParallel(std::string_view name, std::size_t operations, Function&& function) {
        std::barrier start{static_cast<std::ptrdiff_t>(ThreadCount + 1)};
        std::vector<std::jthread> workers;
        workers.reserve(ThreadCount);
        for (std::size_t thread = 0; thread != ThreadCount; ++thread) {
            workers.emplace_back([&, thread] {
                start.arrive_and_wait();
                benchmarkSink.fetch_xor(function(thread), std::memory_order_relaxed);
            });
        }
        const auto begin = Clock::now();
        start.arrive_and_wait();
        workers.clear();
        Report(name, operations, ThreadCount,
               std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin));
    }

    void RunFixedLocalLookup() {
        constexpr std::size_t kCapacity = 1U << 18;
        constexpr std::size_t kKeys = kCapacity / 2;
        constexpr std::size_t kIterations = 4'000'000;
        auto map = std::make_unique<ConcurrentMap<std::uint64_t, std::uint64_t, kCapacity, MapTopologies::Local>>();
        std::unordered_map<std::uint64_t, std::uint64_t> reference;
        reference.reserve(kKeys);
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            static_cast<void>(map->TryInsert(key, key + 1));
            reference.emplace(key, key + 1);
        }

        Measure("mashiro_fixed_local_find_hit", kIterations, [&] {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                sink ^= *map->FindRef(iteration & (kKeys - 1));
            }
            return sink;
        });
        Measure("std_unordered_map_find_hit", kIterations, [&] {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                sink ^= reference.find(iteration & (kKeys - 1))->second;
            }
            return sink;
        });
    }

    void RunDynamicLocalLookup() {
        constexpr std::size_t kKeys = 1U << 17;
        constexpr std::size_t kIterations = 4'000'000;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Local> map{64};
        std::unordered_map<std::uint64_t, std::uint64_t> reference;
        reference.reserve(kKeys);
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            static_cast<void>(map.TryInsert(key, key + 1));
            reference.emplace(key, key + 1);
        }

        Measure("mashiro_dynamic_local_find_hit", kIterations, [&] {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                sink ^= *map.FindRef(iteration & (kKeys - 1));
            }
            return sink;
        });
        Measure("std_unordered_map_dynamic_find_hit", kIterations, [&] {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                sink ^= reference.find(iteration & (kKeys - 1))->second;
            }
            return sink;
        });
    }

    void RunLocalInsert() {
        constexpr std::size_t kCapacity = 1U << 18;
        constexpr std::size_t kKeys = kCapacity / 2;

        auto fixedMap = std::make_unique<
            ConcurrentMap<std::uint64_t, std::uint64_t, kCapacity, MapTopologies::Local>>();
        Measure("mashiro_fixed_local_insert", kKeys, [&] {
            std::uint64_t failures = 0;
            for (std::uint64_t key = 0; key != kKeys; ++key) {
                failures += fixedMap->TryInsert(key, key + 1) != MapInsertResult::Inserted;
            }
            return failures;
        });
        if (fixedMap->SizeApprox() != kKeys) {
            std::terminate();
        }

        std::unordered_map<std::uint64_t, std::uint64_t> fixedReference;
        fixedReference.reserve(kKeys);
        Measure("std_unordered_map_reserved_insert", kKeys, [&] {
            std::uint64_t failures = 0;
            for (std::uint64_t key = 0; key != kKeys; ++key) {
                failures += !fixedReference.emplace(key, key + 1).second;
            }
            return failures;
        });
        if (fixedReference.size() != kKeys) {
            std::terminate();
        }

        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Local> dynamicMap{64};
        Measure("mashiro_dynamic_local_insert", kKeys, [&] {
            std::uint64_t failures = 0;
            for (std::uint64_t key = 0; key != kKeys; ++key) {
                failures += dynamicMap.TryInsert(key, key + 1) != MapInsertResult::Inserted;
            }
            return failures;
        });
        if (dynamicMap.SizeApprox() != kKeys) {
            std::terminate();
        }

        std::unordered_map<std::uint64_t, std::uint64_t> dynamicReference;
        Measure("std_unordered_map_dynamic_insert", kKeys, [&] {
            std::uint64_t failures = 0;
            for (std::uint64_t key = 0; key != kKeys; ++key) {
                failures += !dynamicReference.emplace(key, key + 1).second;
            }
            return failures;
        });
        if (dynamicReference.size() != kKeys) {
            std::terminate();
        }
    }

    template<MapTopology Topology>
    void RunFixedLookup(std::string_view hitName, std::string_view missName) {
        constexpr std::size_t kCapacity = 1U << 18;
        constexpr std::size_t kKeys = kCapacity / 2;
        constexpr std::size_t kIterationsPerReader = 1'000'000;
        constexpr std::size_t kReaders = kReaderThreads<Topology>;
        auto map = std::make_unique<ConcurrentMap<std::uint64_t, std::uint64_t, kCapacity, Topology>>();
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            static_cast<void>(map->TryInsert(key, key + 1));
        }

        MeasureParallel<kReaders>(hitName, kReaders * kIterationsPerReader, [map = map.get()](std::size_t reader) {
            std::uint64_t sink = 0;
            std::uint64_t value = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                static_cast<void>(map->TryGet((iteration + reader * 977) & (kKeys - 1), value));
                sink ^= value;
            }
            return sink;
        });
        MeasureParallel<kReaders>(missName, kReaders * kIterationsPerReader, [map = map.get()](std::size_t reader) {
            std::uint64_t sink = 0;
            std::uint64_t value = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink += map->TryGet(kKeys + ((iteration + reader * 977) & (kKeys - 1)), value);
            }
            return sink;
        });
    }

    template<std::size_t ReaderCount>
    void RunTbbFixedLookup(std::string_view hitName, std::string_view missName) {
        constexpr std::size_t kKeys = 1U << 17;
        constexpr std::size_t kIterationsPerReader = 1'000'000;
        oneapi::tbb::concurrent_unordered_map<std::uint64_t, std::uint64_t> map;
        map.reserve(kKeys);
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            map.emplace(key, key + 1);
        }

        MeasureParallel<ReaderCount>(hitName, ReaderCount * kIterationsPerReader, [&map](std::size_t reader) {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink ^= map.find((iteration + reader * 977) & (kKeys - 1))->second;
            }
            return sink;
        });
        MeasureParallel<ReaderCount>(missName, ReaderCount * kIterationsPerReader, [&map](std::size_t reader) {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink += map.find(kKeys + ((iteration + reader * 977) & (kKeys - 1))) != map.end();
            }
            return sink;
        });
    }

    template<MapTopology Topology>
    void RunDynamicLookup(std::string_view hitName, std::string_view missName) {
        constexpr std::size_t kKeys = 1U << 17;
        constexpr std::size_t kIterationsPerReader = 1'000'000;
        constexpr std::size_t kReaders = kReaderThreads<Topology>;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, Topology> map{64};
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            if (map.TryInsert(key, key + 1) != MapInsertResult::Inserted) {
                std::terminate();
            }
        }

        MeasureParallel<kReaders>(hitName, kReaders * kIterationsPerReader, [&map](std::size_t reader) {
            std::uint64_t sink = 0;
            std::uint64_t value = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                static_cast<void>(map.TryGet((iteration + reader * 977) & (kKeys - 1), value));
                sink ^= value;
            }
            return sink;
        });
        MeasureParallel<kReaders>(missName, kReaders * kIterationsPerReader, [&map](std::size_t reader) {
            std::uint64_t sink = 0;
            std::uint64_t value = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink += map.TryGet(kKeys + ((iteration + reader * 977) & (kKeys - 1)), value);
            }
            return sink;
        });
    }

    template<std::size_t ReaderCount>
    void RunTbbDynamicLookup(std::string_view hitName, std::string_view missName) {
        constexpr std::size_t kKeys = 1U << 17;
        constexpr std::size_t kIterationsPerReader = 1'000'000;
        oneapi::tbb::concurrent_unordered_map<std::uint64_t, std::uint64_t> map;
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            map.emplace(key, key + 1);
        }

        MeasureParallel<ReaderCount>(hitName, ReaderCount * kIterationsPerReader, [&map](std::size_t reader) {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink ^= map.find((iteration + reader * 977) & (kKeys - 1))->second;
            }
            return sink;
        });
        MeasureParallel<ReaderCount>(missName, ReaderCount * kIterationsPerReader, [&map](std::size_t reader) {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink += map.find(kKeys + ((iteration + reader * 977) & (kKeys - 1))) != map.end();
            }
            return sink;
        });
    }

    template<MapTopology Topology>
    void RunDynamicInsert(std::string_view name) {
        constexpr std::size_t kWriters = kWriterThreads<Topology>;
        constexpr std::size_t kKeysPerWriter = 75'000;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, Topology> map{64};
        std::atomic<std::size_t> failures{};
        MeasureParallel<kWriters>(name, kWriters * kKeysPerWriter, [&](std::size_t writer) {
            std::size_t localFailures = 0;
            for (std::size_t offset = 0; offset != kKeysPerWriter; ++offset) {
                const std::uint64_t key = writer * kKeysPerWriter + offset;
                localFailures += map.TryInsert(key, key + 1) != MapInsertResult::Inserted;
            }
            failures.fetch_add(localFailures, std::memory_order_relaxed);
            return std::uint64_t{0};
        });
        if (failures.load(std::memory_order_relaxed) != 0 || map.SizeApprox() != kWriters * kKeysPerWriter) {
            std::terminate();
        }
    }

    template<std::size_t WriterCount>
    void RunTbbDynamicInsert(std::string_view name) {
        constexpr std::size_t kKeysPerWriter = 75'000;
        oneapi::tbb::concurrent_unordered_map<std::uint64_t, std::uint64_t> map;
        std::atomic<std::size_t> failures{};
        MeasureParallel<WriterCount>(name, WriterCount * kKeysPerWriter, [&](std::size_t writer) {
            std::size_t localFailures = 0;
            for (std::size_t offset = 0; offset != kKeysPerWriter; ++offset) {
                const std::uint64_t key = writer * kKeysPerWriter + offset;
                localFailures += !map.emplace(key, key + 1).second;
            }
            failures.fetch_add(localFailures, std::memory_order_relaxed);
            return std::uint64_t{0};
        });
        if (failures.load(std::memory_order_relaxed) != 0 || map.size() != WriterCount * kKeysPerWriter) {
            std::terminate();
        }
    }

    template<MapTopology Topology>
    void RunDynamicSetLookup(std::string_view name) {
        constexpr std::size_t kKeys = 1U << 17;
        constexpr std::size_t kIterationsPerReader = 1'000'000;
        constexpr std::size_t kReaders = kReaderThreads<Topology>;
        ConcurrentSet<std::uint64_t, std::dynamic_extent, Topology> set{64};
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            if (set.TryInsert(key) != MapInsertResult::Inserted) {
                std::terminate();
            }
        }
        MeasureParallel<kReaders>(name, kReaders * kIterationsPerReader, [&set](std::size_t reader) {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink += set.Contains((iteration + reader * 977) & (kKeys - 1));
            }
            return sink;
        });
    }

    template<std::size_t ReaderCount>
    void RunTbbDynamicSetLookup(std::string_view name) {
        constexpr std::size_t kKeys = 1U << 17;
        constexpr std::size_t kIterationsPerReader = 1'000'000;
        oneapi::tbb::concurrent_unordered_set<std::uint64_t> set;
        for (std::uint64_t key = 0; key != kKeys; ++key) {
            set.emplace(key);
        }
        MeasureParallel<ReaderCount>(name, ReaderCount * kIterationsPerReader, [&set](std::size_t reader) {
            std::uint64_t sink = 0;
            for (std::size_t iteration = 0; iteration != kIterationsPerReader; ++iteration) {
                sink += set.find((iteration + reader * 977) & (kKeys - 1)) != set.end();
            }
            return sink;
        });
    }

    template<MapTopology Topology>
    void RunDynamicMixed(std::string_view name) {
        constexpr std::size_t kReaders = kReaderThreads<Topology>;
        constexpr std::size_t kWriters = kWriterThreads<Topology>;
        constexpr std::size_t kInitialKeys = 1U << 15;
        constexpr std::size_t kReadsPerReader = 500'000;
        constexpr std::size_t kWritesPerWriter = 50'000;
        constexpr std::size_t kFinalKeys = kInitialKeys + kWriters * kWritesPerWriter;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, Topology> map{64};
        for (std::uint64_t key = 0; key != kInitialKeys; ++key) {
            static_cast<void>(map.TryInsert(key, key + 1));
        }

        std::barrier start{static_cast<std::ptrdiff_t>(kReaders + kWriters + 1)};
        std::atomic<std::size_t> failures{};
        std::vector<std::jthread> workers;
        workers.reserve(kReaders + kWriters);
        for (std::size_t writer = 0; writer != kWriters; ++writer) {
            workers.emplace_back([&, writer] {
                start.arrive_and_wait();
                std::size_t localFailures = 0;
                for (std::size_t offset = 0; offset != kWritesPerWriter; ++offset) {
                    const std::uint64_t key = kInitialKeys + writer * kWritesPerWriter + offset;
                    localFailures += map.TryInsert(key, key + 1) != MapInsertResult::Inserted;
                }
                failures.fetch_add(localFailures, std::memory_order_relaxed);
            });
        }
        for (std::size_t reader = 0; reader != kReaders; ++reader) {
            workers.emplace_back([&, reader] {
                start.arrive_and_wait();
                std::uint64_t sink = 0;
                std::uint64_t value = 0;
                for (std::size_t iteration = 0; iteration != kReadsPerReader; ++iteration) {
                    const std::uint64_t key = (iteration * 1'001 + reader * 977) % kFinalKeys;
                    sink += map.TryGet(key, value);
                }
                benchmarkSink.fetch_xor(sink, std::memory_order_relaxed);
            });
        }
        const auto begin = Clock::now();
        start.arrive_and_wait();
        workers.clear();
        constexpr std::size_t kOperations = kReaders * kReadsPerReader + kWriters * kWritesPerWriter;
        Report(name, kOperations, kReaders + kWriters,
               std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin));
        if (failures.load(std::memory_order_relaxed) != 0 || map.SizeApprox() != kFinalKeys) {
            std::terminate();
        }
    }

    template<std::size_t ReaderCount, std::size_t WriterCount>
    void RunTbbDynamicMixed(std::string_view name) {
        constexpr std::size_t kInitialKeys = 1U << 15;
        constexpr std::size_t kReadsPerReader = 500'000;
        constexpr std::size_t kWritesPerWriter = 50'000;
        constexpr std::size_t kFinalKeys = kInitialKeys + WriterCount * kWritesPerWriter;
        oneapi::tbb::concurrent_unordered_map<std::uint64_t, std::uint64_t> map;
        for (std::uint64_t key = 0; key != kInitialKeys; ++key) {
            map.emplace(key, key + 1);
        }

        std::barrier start{static_cast<std::ptrdiff_t>(ReaderCount + WriterCount + 1)};
        std::atomic<std::size_t> failures{};
        std::vector<std::jthread> workers;
        workers.reserve(ReaderCount + WriterCount);
        for (std::size_t writer = 0; writer != WriterCount; ++writer) {
            workers.emplace_back([&, writer] {
                start.arrive_and_wait();
                std::size_t localFailures = 0;
                for (std::size_t offset = 0; offset != kWritesPerWriter; ++offset) {
                    const std::uint64_t key = kInitialKeys + writer * kWritesPerWriter + offset;
                    localFailures += !map.emplace(key, key + 1).second;
                }
                failures.fetch_add(localFailures, std::memory_order_relaxed);
            });
        }
        for (std::size_t reader = 0; reader != ReaderCount; ++reader) {
            workers.emplace_back([&, reader] {
                start.arrive_and_wait();
                std::uint64_t sink = 0;
                for (std::size_t iteration = 0; iteration != kReadsPerReader; ++iteration) {
                    const std::uint64_t key = (iteration * 1'001 + reader * 977) % kFinalKeys;
                    sink += map.find(key) != map.end();
                }
                benchmarkSink.fetch_xor(sink, std::memory_order_relaxed);
            });
        }
        const auto begin = Clock::now();
        start.arrive_and_wait();
        workers.clear();
        constexpr std::size_t kOperations = ReaderCount * kReadsPerReader + WriterCount * kWritesPerWriter;
        Report(name, kOperations, ReaderCount + WriterCount,
               std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin));
        if (failures.load(std::memory_order_relaxed) != 0 || map.size() != kFinalKeys) {
            std::terminate();
        }
    }

} // namespace

int main() {
    std::cout << "case,threads,operations,mops_per_second,nanoseconds\n";
    RunFixedLocalLookup();
    RunDynamicLocalLookup();
    RunLocalInsert();

    RunFixedLookup<MapTopologies::Spsc>("mashiro_fixed_spsc_find_hit", "mashiro_fixed_spsc_find_miss");
    RunFixedLookup<MapTopologies::Mpsc>("mashiro_fixed_mpsc_find_hit", "mashiro_fixed_mpsc_find_miss");
    RunFixedLookup<MapTopologies::Spmc>("mashiro_fixed_spmc_find_hit", "mashiro_fixed_spmc_find_miss");
    RunFixedLookup<MapTopologies::Mpmc>("mashiro_fixed_mpmc_find_hit", "mashiro_fixed_mpmc_find_miss");
    RunTbbFixedLookup<1>("tbb_fixed_1r_find_hit", "tbb_fixed_1r_find_miss");
    RunTbbFixedLookup<4>("tbb_fixed_4r_find_hit", "tbb_fixed_4r_find_miss");

    RunDynamicLookup<MapTopologies::Spsc>("mashiro_dynamic_spsc_find_hit", "mashiro_dynamic_spsc_find_miss");
    RunDynamicLookup<MapTopologies::Mpsc>("mashiro_dynamic_mpsc_find_hit", "mashiro_dynamic_mpsc_find_miss");
    RunDynamicLookup<MapTopologies::Spmc>("mashiro_dynamic_spmc_find_hit", "mashiro_dynamic_spmc_find_miss");
    RunDynamicLookup<MapTopologies::Mpmc>("mashiro_dynamic_mpmc_find_hit", "mashiro_dynamic_mpmc_find_miss");
    RunTbbDynamicLookup<1>("tbb_dynamic_1r_find_hit", "tbb_dynamic_1r_find_miss");
    RunTbbDynamicLookup<4>("tbb_dynamic_4r_find_hit", "tbb_dynamic_4r_find_miss");

    RunDynamicInsert<MapTopologies::Spsc>("mashiro_dynamic_spsc_insert");
    RunDynamicInsert<MapTopologies::Mpsc>("mashiro_dynamic_mpsc_insert");
    RunDynamicInsert<MapTopologies::Spmc>("mashiro_dynamic_spmc_insert");
    RunDynamicInsert<MapTopologies::Mpmc>("mashiro_dynamic_mpmc_insert");
    RunTbbDynamicInsert<1>("tbb_dynamic_1w_insert");
    RunTbbDynamicInsert<4>("tbb_dynamic_4w_insert");

    RunDynamicSetLookup<MapTopologies::Mpmc>("mashiro_dynamic_mpmc_set_contains");
    RunTbbDynamicSetLookup<4>("tbb_dynamic_4r_set_contains");

    RunDynamicMixed<MapTopologies::Spsc>("mashiro_dynamic_spsc_mixed");
    RunDynamicMixed<MapTopologies::Mpsc>("mashiro_dynamic_mpsc_mixed");
    RunDynamicMixed<MapTopologies::Spmc>("mashiro_dynamic_spmc_mixed");
    RunDynamicMixed<MapTopologies::Mpmc>("mashiro_dynamic_mpmc_mixed");
    RunTbbDynamicMixed<1, 1>("tbb_dynamic_1r1w_mixed");
    RunTbbDynamicMixed<1, 4>("tbb_dynamic_1r4w_mixed");
    RunTbbDynamicMixed<4, 1>("tbb_dynamic_4r1w_mixed");
    RunTbbDynamicMixed<4, 4>("tbb_dynamic_4r4w_mixed");

    std::cerr << "sink=" << benchmarkSink.load(std::memory_order_relaxed) << '\n';
}
