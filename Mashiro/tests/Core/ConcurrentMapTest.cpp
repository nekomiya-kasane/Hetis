/**
 * @file ConcurrentMapTest.cpp
 * @brief Topology, collision, publication, and concurrent mutation tests for ConcurrentMap and ConcurrentSet.
 * @ingroup Testing
 */
#include <Mashiro/Core/ConcurrentMap.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Concurrency;
using namespace std::chrono_literals;

namespace {

    struct ConstantHash {
        using ResultType = std::uint64_t;

        [[nodiscard]] constexpr ResultType operator()(std::span<const std::byte>) const noexcept { return 0; }
    };

    template<MapTopology Topology>
    void RequireSequentialMapSemantics() {
        ConcurrentMap<std::uint64_t, std::uint64_t, 8, Topology> map;
        REQUIRE(map.Empty());
        for (std::uint64_t key = 0; key != 8; ++key) {
            REQUIRE(map.TryInsert(key, key * 10) == MapInsertResult::Inserted);
        }
        REQUIRE(map.SizeApprox() == 8);
        REQUIRE(map.TryInsert(0, 999) == MapInsertResult::Exists);
        REQUIRE(map.TryInsert(100, 1) == MapInsertResult::Full);

        for (std::uint64_t key = 0; key != 8; ++key) {
            REQUIRE(map.Find(key) == key * 10);
        }
        REQUIRE(map.InsertOrAssign(3, 777) == MapAssignResult::Assigned);
        REQUIRE(map.Find(3) == 777);
        REQUIRE(map.Erase(3));
        REQUIRE_FALSE(map.Contains(3));
        REQUIRE(map.TryInsert(11, 110) == MapInsertResult::Inserted);
        REQUIRE(map.Find(11) == 110);
        REQUIRE(map.SizeApprox() == 8);
    }

    template<MapTopology Topology>
    void RequireSequentialSetSemantics() {
        ConcurrentSet<std::uint64_t, 8, Topology> set;
        for (std::uint64_t key = 0; key != 8; ++key) {
            REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
        }
        REQUIRE(set.TryInsert(2) == MapInsertResult::Exists);
        REQUIRE(set.TryInsert(100) == MapInsertResult::Full);
        REQUIRE(set.Erase(2));
        REQUIRE_FALSE(set.Contains(2));
        REQUIRE(set.TryInsert(100) == MapInsertResult::Inserted);
        REQUIRE(set.Contains(100));
    }

    template<MapTopology Topology, std::size_t WriterCount, std::size_t ReaderCount>
    void RequireConcurrentUniqueInsertion() {
        constexpr std::size_t kKeysPerWriter = 2'000;
        constexpr std::size_t kKeyCount = WriterCount * kKeysPerWriter;
        ConcurrentMap<std::uint64_t, std::uint64_t, 16'384, Topology> map;
        std::barrier start{static_cast<std::ptrdiff_t>(WriterCount + ReaderCount)};
        std::atomic<std::size_t> writersDone{};
        std::atomic<int> failure{};
        std::vector<std::jthread> workers;
        workers.reserve(WriterCount + ReaderCount);

        for (std::size_t writer = 0; writer != WriterCount; ++writer) {
            workers.emplace_back([&, writer] {
                start.arrive_and_wait();
                for (std::size_t offset = 0; offset != kKeysPerWriter; ++offset) {
                    const std::uint64_t key = writer * kKeysPerWriter + offset;
                    if (map.TryInsert(key, key ^ 0x9e3779b97f4a7c15ULL) != MapInsertResult::Inserted) {
                        failure.store(1, std::memory_order_relaxed);
                        break;
                    }
                }
                writersDone.fetch_add(1, std::memory_order_release);
            });
        }

        for (std::size_t reader = 0; reader != ReaderCount; ++reader) {
            workers.emplace_back([&, reader] {
                start.arrive_and_wait();
                std::size_t cursor = reader;
                while (writersDone.load(std::memory_order_acquire) != WriterCount) {
                    const std::uint64_t key = cursor++ % kKeyCount;
                    if (auto value = map.Find(key); value && *value != (key ^ 0x9e3779b97f4a7c15ULL)) {
                        failure.store(2, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        workers.clear();
        INFO("failure code: " << failure.load(std::memory_order_relaxed));
        REQUIRE(failure.load(std::memory_order_relaxed) == 0);
        REQUIRE(map.SizeApprox() == kKeyCount);
        for (std::uint64_t key = 0; key != kKeyCount; ++key) {
            REQUIRE(map.Find(key) == (key ^ 0x9e3779b97f4a7c15ULL));
        }
    }

    template<MapTopology Topology, std::size_t WriterCount, std::size_t ReaderCount>
    void RequireDynamicConcurrentUniqueInsertion() {
        constexpr std::size_t kKeysPerWriter = 1'500;
        constexpr std::size_t kKeyCount = WriterCount * kKeysPerWriter;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, Topology> map{8};
        std::barrier start{static_cast<std::ptrdiff_t>(WriterCount + ReaderCount)};
        std::atomic<std::size_t> writersDone{};
        std::atomic<int> failure{};
        std::vector<std::jthread> workers;
        workers.reserve(WriterCount + ReaderCount);

        for (std::size_t writer = 0; writer != WriterCount; ++writer) {
            workers.emplace_back([&, writer] {
                start.arrive_and_wait();
                for (std::size_t offset = 0; offset != kKeysPerWriter; ++offset) {
                    const std::uint64_t key = writer * kKeysPerWriter + offset;
                    if (map.TryInsert(key, key ^ 0xd6e8feb86659fd93ULL) != MapInsertResult::Inserted) {
                        failure.store(1, std::memory_order_relaxed);
                        break;
                    }
                }
                writersDone.fetch_add(1, std::memory_order_release);
            });
        }

        for (std::size_t reader = 0; reader != ReaderCount; ++reader) {
            workers.emplace_back([&, reader] {
                start.arrive_and_wait();
                std::size_t cursor = reader;
                while (writersDone.load(std::memory_order_acquire) != WriterCount) {
                    const std::uint64_t key = cursor++ % kKeyCount;
                    if (auto value = map.Find(key); value && *value != (key ^ 0xd6e8feb86659fd93ULL)) {
                        failure.store(2, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        workers.clear();
        REQUIRE(failure.load(std::memory_order_relaxed) == 0);
        REQUIRE(map.SizeApprox() == kKeyCount);
        for (std::uint64_t key = 0; key != kKeyCount; ++key) {
            REQUIRE(map.Find(key) == (key ^ 0xd6e8feb86659fd93ULL));
        }
        REQUIRE(map.TryInsert(0, 7) == MapInsertResult::Exists);
        REQUIRE(map.InsertOrAssign(0, 7) == MapAssignResult::Assigned);
        REQUIRE(map.Find(0) == 7);
    }

} // namespace

TEST_CASE("Map topology vocabulary reduces exact participant counts at compile time", AUTO_TAG) {
    STATIC_REQUIRE(StaticMapTopology<1, 1, 1>() == MapTopologies::Local);
    STATIC_REQUIRE(StaticMapTopology<1, 1>() == MapTopologies::Spsc);
    STATIC_REQUIRE(StaticMapTopology<4, 1>() == MapTopologies::Mpsc);
    STATIC_REQUIRE(StaticMapTopology<1, 4>() == MapTopologies::Spmc);
    STATIC_REQUIRE(StaticMapTopology<4, 4, 2>() == MapTopologies::Mpmc);
    using HashMapAlias = ConcurrentHashMap<std::uint64_t, std::uint64_t, 8, MapTopologies::Local>;
    using HashSetAlias = ConcurrentHashSet<std::uint64_t, 8, MapTopologies::Local>;
    STATIC_REQUIRE(HashMapAlias::order == MapOrder::Unordered);
    STATIC_REQUIRE(HashSetAlias::order == MapOrder::Unordered);
}

TEST_CASE("ConcurrentMap every topology retains sequential map semantics", AUTO_TAG) {
    RequireSequentialMapSemantics<MapTopologies::Local>();
    RequireSequentialMapSemantics<MapTopologies::Spsc>();
    RequireSequentialMapSemantics<MapTopologies::Mpsc>();
    RequireSequentialMapSemantics<MapTopologies::Spmc>();
    RequireSequentialMapSemantics<MapTopologies::Mpmc>();
}

TEST_CASE("ConcurrentSet every topology retains sequential set semantics", AUTO_TAG) {
    RequireSequentialSetSemantics<MapTopologies::Local>();
    RequireSequentialSetSemantics<MapTopologies::Spsc>();
    RequireSequentialSetSemantics<MapTopologies::Mpsc>();
    RequireSequentialSetSemantics<MapTopologies::Spmc>();
    RequireSequentialSetSemantics<MapTopologies::Mpmc>();
}

TEST_CASE("ConcurrentMap collision chains survive tombstone reuse", AUTO_TAG) {
    ConcurrentMap<std::uint64_t, std::uint64_t, 16, MapTopologies::Mpmc, MapOrder::Unordered, ConstantHash> map;
    for (std::uint64_t key = 0; key != 12; ++key) {
        REQUIRE(map.TryInsert(key, key + 1) == MapInsertResult::Inserted);
    }
    for (std::uint64_t key = 0; key != 12; key += 2) {
        REQUIRE(map.Erase(key));
    }
    for (std::uint64_t key = 100; key != 106; ++key) {
        REQUIRE(map.TryInsert(key, key + 1) == MapInsertResult::Inserted);
    }
    for (std::uint64_t key = 1; key < 12; key += 2) {
        REQUIRE(map.Find(key) == key + 1);
    }
}

TEST_CASE("ConcurrentMap SPSC publishes complete key-value snapshots", AUTO_TAG) {
    constexpr std::uint64_t kIterations = 100'000;
    ConcurrentMap<std::uint64_t, std::uint64_t, 2, MapTopologies::Spsc> map;
    std::atomic<bool> failed{};
    std::jthread reader{[&] {
        for (std::uint64_t expected = 1; expected <= kIterations; ++expected) {
            for (;;) {
                if (auto value = map.Find(0); value && *value >= expected) {
                    if (*value > kIterations) {
                        failed.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
            }
        }
    }};
    REQUIRE(map.TryInsert(0, 0) == MapInsertResult::Inserted);
    for (std::uint64_t value = 1; value <= kIterations; ++value) {
        REQUIRE(map.InsertOrAssign(0, value) == MapAssignResult::Assigned);
    }
    reader.join();
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(map.Find(0) == kIterations);
}

TEST_CASE("ConcurrentMap MPSC serializes duplicate insertion by key", AUTO_TAG) {
    constexpr std::size_t kWriterCount = 8;
    constexpr std::size_t kKeyCount = 1'000;
    ConcurrentMap<std::uint64_t, std::uint64_t, 2'048, MapTopologies::Mpsc> map;
    std::barrier start{static_cast<std::ptrdiff_t>(kWriterCount)};
    std::array<std::jthread, kWriterCount> writers;
    for (std::size_t writer = 0; writer != kWriterCount; ++writer) {
        writers[writer] = std::jthread{[&] {
            start.arrive_and_wait();
            for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                const auto result = map.TryInsert(key, key);
                REQUIRE((result == MapInsertResult::Inserted || result == MapInsertResult::Exists));
            }
        }};
    }
    for (auto& writer : writers) {
        writer.join();
    }
    REQUIRE(map.SizeApprox() == kKeyCount);
}

TEST_CASE("ConcurrentMap topology-specialized paths preserve unique concurrent insertion", AUTO_TAG) {
    INFO("MPSC");
    RequireConcurrentUniqueInsertion<MapTopologies::Mpsc, 4, 1>();
    INFO("SPMC");
    RequireConcurrentUniqueInsertion<MapTopologies::Spmc, 1, 4>();
    INFO("MPMC");
    RequireConcurrentUniqueInsertion<MapTopologies::Mpmc, 4, 4>();
}

TEST_CASE("Dynamic ConcurrentMap and ConcurrentSet grow without rehashing published segments", AUTO_TAG) {
    ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Local> map{8};
    ConcurrentSet<std::uint64_t, std::dynamic_extent, MapTopologies::Local> set{8};
    for (std::uint64_t key = 0; key != 10'000; ++key) {
        REQUIRE(map.TryInsert(key, key * 3) == MapInsertResult::Inserted);
        REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
    }
    REQUIRE(map.SizeApprox() == 10'000);
    REQUIRE(set.SizeApprox() == 10'000);
    for (std::uint64_t key = 0; key != 10'000; ++key) {
        REQUIRE(map.Find(key) == key * 3);
        REQUIRE(set.Contains(key));
    }
    REQUIRE(map.TryInsert(0, 999) == MapInsertResult::Exists);
    REQUIRE(set.TryInsert(0) == MapInsertResult::Exists);
    for (std::uint64_t key = 0; key != 10'000; key += 2) {
        REQUIRE(map.Erase(key));
        REQUIRE(set.Erase(key));
    }
    map.Clear();
    set.Clear();
    REQUIRE(map.Empty());
    REQUIRE(set.Empty());
    REQUIRE(map.TryInsert(42, 84) == MapInsertResult::Inserted);
    REQUIRE(set.TryInsert(42) == MapInsertResult::Inserted);
}

TEST_CASE("Dynamic ConcurrentMap MPMC grows while readers observe complete values", AUTO_TAG) {
    constexpr std::size_t kWriterCount = 4;
    constexpr std::size_t kReaderCount = 4;
    constexpr std::size_t kKeysPerWriter = 4'000;
    constexpr std::size_t kKeyCount = kWriterCount * kKeysPerWriter;
    ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Mpmc> map{8};
    std::barrier start{static_cast<std::ptrdiff_t>(kWriterCount + kReaderCount)};
    std::atomic<std::size_t> writersDone{};
    std::atomic<bool> failed{};
    std::vector<std::jthread> workers;

    for (std::size_t writer = 0; writer != kWriterCount; ++writer) {
        workers.emplace_back([&, writer] {
            start.arrive_and_wait();
            for (std::size_t offset = 0; offset != kKeysPerWriter; ++offset) {
                const std::uint64_t key = writer * kKeysPerWriter + offset;
                if (map.TryInsert(key, ~key) != MapInsertResult::Inserted) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            writersDone.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::size_t reader = 0; reader != kReaderCount; ++reader) {
        workers.emplace_back([&, reader] {
            start.arrive_and_wait();
            std::size_t cursor = reader;
            while (writersDone.load(std::memory_order_acquire) != kWriterCount) {
                const std::uint64_t key = cursor++ % kKeyCount;
                if (auto value = map.Find(key); value && *value != ~key) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }
    workers.clear();
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(map.SizeApprox() == kKeyCount);
    for (std::uint64_t key = 0; key != kKeyCount; ++key) {
        REQUIRE(map.Find(key) == ~key);
    }
}

TEST_CASE("Dynamic ConcurrentMap SPSC MPSC and SPMC publish unique values", AUTO_TAG_WITH("Stress")) {
    INFO("SPSC");
    RequireDynamicConcurrentUniqueInsertion<MapTopologies::Spsc, 1, 1>();
    INFO("MPSC");
    RequireDynamicConcurrentUniqueInsertion<MapTopologies::Mpsc, 4, 1>();
    INFO("SPMC");
    RequireDynamicConcurrentUniqueInsertion<MapTopologies::Spmc, 1, 4>();
}

TEST_CASE("Ordered ConcurrentMap and ConcurrentSet expose key-sorted snapshots", AUTO_TAG) {
    ConcurrentMap<std::uint64_t, std::uint64_t, 32, MapTopologies::Mpmc, MapOrder::Ordered> map;
    ConcurrentSet<std::uint64_t, 32, MapTopologies::Mpmc, MapOrder::Ordered> set;
    for (const std::uint64_t key : {7, 1, 15, 3, 9, 2}) {
        REQUIRE(map.TryInsert(key, key * 10) == MapInsertResult::Inserted);
        REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
    }
    const auto mapEntries = map.Snapshot();
    const auto setKeys = set.Snapshot();
    REQUIRE(mapEntries.size() == 6);
    REQUIRE(setKeys.size() == 6);
    for (std::size_t index = 1; index != mapEntries.size(); ++index) {
        REQUIRE(mapEntries[index - 1].first < mapEntries[index].first);
        REQUIRE(mapEntries[index].second == mapEntries[index].first * 10);
        REQUIRE(setKeys[index - 1] < setKeys[index]);
    }
}

TEST_CASE("ConcurrentMap MPMC update erase and tombstone churn preserve complete snapshots", AUTO_TAG_WITH("Stress")) {
    constexpr std::size_t kWriterCount = 4;
    constexpr std::size_t kReaderCount = 4;
    constexpr std::size_t kKeyCount = 64;
    constexpr std::size_t kIterations = 2'000;
    ConcurrentMap<std::uint64_t, std::uint64_t, 1'024, MapTopologies::Mpmc> map;
    for (std::uint64_t key = 0; key != kKeyCount; ++key) {
        REQUIRE(map.TryInsert(key, key << 32) == MapInsertResult::Inserted);
    }

    std::barrier start{static_cast<std::ptrdiff_t>(kWriterCount + kReaderCount)};
    std::atomic<std::size_t> writersDone{};
    std::atomic<bool> failed{};
    std::vector<std::jthread> workers;
    workers.reserve(kWriterCount + kReaderCount);
    for (std::size_t writer = 0; writer != kWriterCount; ++writer) {
        workers.emplace_back([&, writer] {
            start.arrive_and_wait();
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                const std::uint64_t key = (iteration * 17 + writer * 29) & (kKeyCount - 1);
                const std::uint64_t value = (key << 32) | iteration;
                if (map.InsertOrAssign(key, value) == MapAssignResult::Full) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
                if ((iteration & 7U) == 0) {
                    static_cast<void>(map.Erase(key));
                    const auto result = map.TryInsert(key, value);
                    if (result == MapInsertResult::Full) {
                        failed.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            }
            writersDone.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::size_t reader = 0; reader != kReaderCount; ++reader) {
        workers.emplace_back([&, reader] {
            start.arrive_and_wait();
            std::uint64_t cursor = reader;
            while (writersDone.load(std::memory_order_acquire) != kWriterCount) {
                const std::uint64_t key = cursor++ & (kKeyCount - 1);
                std::uint64_t value = 0;
                if (map.TryGet(key, value) && (value >> 32) != key) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    workers.clear();
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    for (std::uint64_t key = 0; key != kKeyCount; ++key) {
        const auto result = map.InsertOrAssign(key, key << 32);
        REQUIRE((result == MapAssignResult::Assigned || result == MapAssignResult::Inserted));
        std::uint64_t finalValue = 0;
        REQUIRE(map.TryGet(key, finalValue));
        REQUIRE((finalValue >> 32) == key);
    }
    REQUIRE(map.SizeApprox() == kKeyCount);
}

TEST_CASE("ConcurrentSet MPMC erase and reuse conserves membership", AUTO_TAG_WITH("Stress")) {
    constexpr std::size_t kWriterCount = 4;
    constexpr std::size_t kReaderCount = 4;
    constexpr std::size_t kKeyCount = 64;
    constexpr std::size_t kIterations = 2'000;
    ConcurrentSet<std::uint64_t, 512, MapTopologies::Mpmc> set;
    for (std::uint64_t key = 0; key != kKeyCount; ++key) {
        REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
    }
    std::barrier start{static_cast<std::ptrdiff_t>(kWriterCount + kReaderCount)};
    std::atomic<std::size_t> writersDone{};
    std::atomic<bool> failed{};
    std::vector<std::jthread> workers;
    workers.reserve(kWriterCount + kReaderCount);
    for (std::size_t writer = 0; writer != kWriterCount; ++writer) {
        workers.emplace_back([&, writer] {
            start.arrive_and_wait();
            for (std::size_t iteration = 0; iteration != kIterations; ++iteration) {
                const std::uint64_t key = (iteration * 13 + writer * 31) & (kKeyCount - 1);
                static_cast<void>(set.Erase(key));
                if (set.TryInsert(key) == MapInsertResult::Full) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            writersDone.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::size_t reader = 0; reader != kReaderCount; ++reader) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            while (writersDone.load(std::memory_order_acquire) != kWriterCount) {
                for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                    static_cast<void>(set.Contains(key));
                }
            }
        });
    }
    workers.clear();
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(set.SizeApprox() == kKeyCount);
    for (std::uint64_t key = 0; key != kKeyCount; ++key) {
        REQUIRE(set.Contains(key));
    }
}
