/**
 * @file ConcurrentMapTest.cpp
 * @brief Topology, collision, publication, and concurrent mutation tests for ConcurrentMap and ConcurrentSet.
 * @ingroup Testing
 */
#include <Sora/Core/Concurrency/ConcurrentMap.h>

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

using namespace Sora;
using namespace Sora::Concurrency;
using namespace std::chrono_literals;

namespace {

    struct ConstantHash {
        using ResultType = std::uint64_t;

        [[nodiscard]] constexpr ResultType operator()(std::span<const std::byte>) const noexcept { return 0; }
    };

    struct CountingHash {
        inline static std::atomic<std::size_t> calls{};

        [[nodiscard]] std::uint64_t operator()(std::span<const std::byte> bytes) const noexcept {
            calls.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t hash = 0xcbf29ce484222325ULL;
            for (std::byte byte : bytes) {
                hash = (hash ^ std::to_integer<std::uint8_t>(byte)) * 0x100000001b3ULL;
            }
            return hash;
        }

        static void Reset() noexcept { calls.store(0, std::memory_order_relaxed); }
        [[nodiscard]] static std::size_t Calls() noexcept { return calls.load(std::memory_order_relaxed); }
    };

    struct BlockingKeyEqual {
        inline static thread_local bool blockReader = false;
        inline static std::atomic<bool> entered{};
        inline static std::atomic<bool> release{};

        [[nodiscard]] bool operator()(std::uint64_t left, std::uint64_t right) const noexcept {
            if (blockReader && right == 0) {
                entered.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            return left == right;
        }

        static void Reset() noexcept {
            entered.store(false, std::memory_order_relaxed);
            release.store(false, std::memory_order_relaxed);
        }
    };

    struct LifetimeTrackedValue {
        inline static std::atomic<std::size_t> liveInstances{};

        explicit LifetimeTrackedValue(std::uint64_t payload = 0) noexcept : payload{payload} {
            liveInstances.fetch_add(1, std::memory_order_relaxed);
        }

        LifetimeTrackedValue(const LifetimeTrackedValue& other) noexcept : payload{other.payload} {
            liveInstances.fetch_add(1, std::memory_order_relaxed);
        }

        LifetimeTrackedValue(LifetimeTrackedValue&& other) noexcept : payload{other.payload} {
            liveInstances.fetch_add(1, std::memory_order_relaxed);
        }

        LifetimeTrackedValue& operator=(const LifetimeTrackedValue&) noexcept = default;
        LifetimeTrackedValue& operator=(LifetimeTrackedValue&&) noexcept = default;

        ~LifetimeTrackedValue() noexcept { liveInstances.fetch_sub(1, std::memory_order_relaxed); }

        std::uint64_t payload;
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

    template<MapTopology Topology>
    void RequireDynamicMapMutationSemantics() {
        constexpr std::uint64_t kKeyCount = 2'048;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, Topology> map{8};
        for (std::uint64_t key = 0; key != kKeyCount; ++key) {
            REQUIRE(map.TryInsert(key, key + 1) == MapInsertResult::Inserted);
        }
        for (std::uint64_t key = 0; key != kKeyCount; key += 2) {
            REQUIRE(map.InsertOrAssign(key, ~key) == MapAssignResult::Assigned);
        }
        for (std::uint64_t key = 0; key < kKeyCount; key += 3) {
            CAPTURE(key);
            REQUIRE(map.Erase(key));
            REQUIRE(map.TryInsert(key, key ^ 0xd6e8feb86659fd93ULL) == MapInsertResult::Inserted);
        }
        REQUIRE(map.SizeApprox() == kKeyCount);
        for (std::uint64_t key = 0; key != kKeyCount; ++key) {
            const std::uint64_t expected = key % 3 == 0 ? key ^ 0xd6e8feb86659fd93ULL : (key % 2 == 0 ? ~key : key + 1);
            REQUIRE(map.Find(key) == expected);
        }
        map.Clear();
        REQUIRE(map.Empty());
        REQUIRE(map.TryInsert(7, 11) == MapInsertResult::Inserted);
        REQUIRE(map.Find(7) == 11);
    }

    template<MapTopology Topology>
    void RequireDynamicSetMutationSemantics() {
        constexpr std::uint64_t kKeyCount = 2'048;
        ConcurrentSet<std::uint64_t, std::dynamic_extent, Topology> set{8};
        for (std::uint64_t key = 0; key != kKeyCount; ++key) {
            REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
        }
        for (std::uint64_t key = 0; key < kKeyCount; key += 3) {
            CAPTURE(key);
            REQUIRE(set.Erase(key));
            REQUIRE(set.TryInsert(key + kKeyCount) == MapInsertResult::Inserted);
        }
        REQUIRE(set.SizeApprox() == kKeyCount);
        for (std::uint64_t key = 0; key != kKeyCount; ++key) {
            REQUIRE(set.Contains(key) == (key % 3 != 0));
            if (key % 3 == 0) {
                REQUIRE(set.Contains(key + kKeyCount));
            }
        }
        set.Clear();
        REQUIRE(set.Empty());
        REQUIRE(set.TryInsert(7) == MapInsertResult::Inserted);
        REQUIRE(set.Contains(7));
    }

    [[nodiscard]] constexpr std::uint64_t EncodeRevision(std::uint64_t key, std::uint64_t revision) noexcept {
        return (key << 32) | revision;
    }

    template<std::size_t Capacity, MapTopology Topology, std::size_t WriterCount, std::size_t ReaderCount>
    void RequireConcurrentMapMutationHistory() {
        static_assert(WriterCount == (Topology.writers == MapCardinality::Single ? 1 : 4));
        static_assert(ReaderCount == (Topology.readers == MapCardinality::Single ? 1 : 4));
        constexpr std::size_t kKeysPerWriter = 8;
        constexpr std::size_t kKeyCount = WriterCount * kKeysPerWriter;
        constexpr std::uint64_t kIterations = 4'096;
        using Map = ConcurrentMap<std::uint64_t, std::uint64_t, Capacity, Topology>;

        auto exercise = []<class MapType>(MapType& map) {
            for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                REQUIRE(map.TryInsert(key, EncodeRevision(key, 0)) == MapInsertResult::Inserted);
            }

            std::barrier start{static_cast<std::ptrdiff_t>(WriterCount + ReaderCount)};
            std::atomic<std::size_t> writersDone{};
            std::atomic<int> failure{};
            std::vector<std::jthread> workers;
            workers.reserve(WriterCount + ReaderCount);

            for (std::size_t writer = 0; writer != WriterCount; ++writer) {
                workers.emplace_back([&, writer] {
                    start.arrive_and_wait();
                    for (std::uint64_t revision = 1; revision <= kIterations; ++revision) {
                        const std::uint64_t key = writer * kKeysPerWriter + ((revision * 5U) & (kKeysPerWriter - 1));
                        const std::uint64_t value = EncodeRevision(key, revision);
                        if (map.InsertOrAssign(key, value) != MapAssignResult::Assigned) {
                            failure.store(1, std::memory_order_relaxed);
                            break;
                        }
                        if ((revision & 7U) == 0 &&
                            (!map.Erase(key) || map.TryInsert(key, value) != MapInsertResult::Inserted)) {
                            failure.store(2, std::memory_order_relaxed);
                            break;
                        }
                    }
                    for (std::uint64_t offset = 0; offset != kKeysPerWriter; ++offset) {
                        const std::uint64_t key = writer * kKeysPerWriter + offset;
                        if (map.InsertOrAssign(key, EncodeRevision(key, kIterations + 1)) !=
                            MapAssignResult::Assigned) {
                            failure.store(3, std::memory_order_relaxed);
                            break;
                        }
                    }
                    writersDone.fetch_add(1, std::memory_order_release);
                });
            }

            for (std::size_t reader = 0; reader != ReaderCount; ++reader) {
                workers.emplace_back([&, reader] {
                    std::array<std::uint32_t, kKeyCount> lastSeen{};
                    std::uint64_t cursor = reader * 977U;
                    start.arrive_and_wait();
                    while (writersDone.load(std::memory_order_acquire) != WriterCount) {
                        const std::uint64_t key = (cursor++ * 1'001U) % kKeyCount;
                        std::uint64_t value = 0;
                        if (!map.TryGet(key, value)) {
                            continue;
                        }
                        const std::uint32_t revision = static_cast<std::uint32_t>(value);
                        if ((value >> 32) != key || revision < lastSeen[key]) {
                            failure.store(4, std::memory_order_relaxed);
                            return;
                        }
                        lastSeen[key] = revision;
                    }
                });
            }

            workers.clear();
            INFO("failure code: " << failure.load(std::memory_order_relaxed));
            REQUIRE(failure.load(std::memory_order_relaxed) == 0);
            REQUIRE(map.SizeApprox() == kKeyCount);
            for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                REQUIRE(map.Find(key) == EncodeRevision(key, kIterations + 1));
            }
        };

        if constexpr (Capacity == std::dynamic_extent) {
            Map map{8};
            exercise(map);
        } else {
            Map map;
            exercise(map);
        }
    }

    template<std::size_t Capacity, MapTopology Topology, std::size_t WriterCount, std::size_t ReaderCount>
    void RequireConcurrentSetMutationHistory() {
        static_assert(WriterCount == (Topology.writers == MapCardinality::Single ? 1 : 4));
        static_assert(ReaderCount == (Topology.readers == MapCardinality::Single ? 1 : 4));
        constexpr std::size_t kKeysPerWriter = 16;
        constexpr std::size_t kKeyCount = WriterCount * kKeysPerWriter;
        constexpr std::uint64_t kIterations = 4'096;
        using Set = ConcurrentSet<std::uint64_t, Capacity, Topology>;

        auto exercise = []<class SetType>(SetType& set) {
            for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
            }

            std::barrier start{static_cast<std::ptrdiff_t>(WriterCount + ReaderCount)};
            std::atomic<std::size_t> writersDone{};
            std::atomic<int> failure{};
            std::vector<std::jthread> workers;
            workers.reserve(WriterCount + ReaderCount);

            for (std::size_t writer = 0; writer != WriterCount; ++writer) {
                workers.emplace_back([&, writer] {
                    start.arrive_and_wait();
                    for (std::uint64_t iteration = 0; iteration != kIterations; ++iteration) {
                        const std::uint64_t key = writer * kKeysPerWriter + ((iteration * 13U) & (kKeysPerWriter - 1));
                        if (!set.Erase(key) || set.TryInsert(key) != MapInsertResult::Inserted) {
                            failure.store(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                    writersDone.fetch_add(1, std::memory_order_release);
                });
            }

            for (std::size_t reader = 0; reader != ReaderCount; ++reader) {
                workers.emplace_back([&, reader] {
                    std::uint64_t cursor = reader * 977U;
                    start.arrive_and_wait();
                    while (writersDone.load(std::memory_order_acquire) != WriterCount) {
                        const std::uint64_t key = (cursor++ * 1'001U) % kKeyCount;
                        static_cast<void>(set.Contains(key));
                        if (set.Contains(kKeyCount + key)) {
                            failure.store(2, std::memory_order_relaxed);
                            return;
                        }
                    }
                });
            }

            workers.clear();
            INFO("failure code: " << failure.load(std::memory_order_relaxed));
            REQUIRE(failure.load(std::memory_order_relaxed) == 0);
            REQUIRE(set.SizeApprox() == kKeyCount);
            for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                REQUIRE(set.Contains(key));
            }
        };

        if constexpr (Capacity == std::dynamic_extent) {
            Set set{8};
            exercise(set);
        } else {
            Set set;
            exercise(set);
        }
    }

    template<std::size_t Capacity, MapTopology Topology, MapOrder Order, std::size_t WriterCount,
             std::size_t ReaderCount>
    void RequireConcurrentSnapshotProperties() {
        static_assert(WriterCount == (Topology.writers == MapCardinality::Single ? 1 : 4));
        static_assert(ReaderCount == (Topology.readers == MapCardinality::Single ? 1 : 4));
        constexpr std::size_t kStableKeysPerWriter = 4;
        constexpr std::size_t kGrowthKeysPerWriter = 64;
        constexpr std::size_t kStableKeyCount = WriterCount * kStableKeysPerWriter;
        constexpr std::size_t kKeyCount = WriterCount * (kStableKeysPerWriter + kGrowthKeysPerWriter);
        constexpr std::uint64_t kIterations = 4'096;
        using Map = ConcurrentMap<std::uint64_t, std::uint64_t, Capacity, Topology, Order>;
        using Set = ConcurrentSet<std::uint64_t, Capacity, Topology, Order>;

        auto exercise = []<class MapType, class SetType>(MapType& map, SetType& set) {
            for (std::uint64_t key = 0; key != kStableKeyCount; ++key) {
                REQUIRE(map.TryInsert(key, EncodeRevision(key, 0)) == MapInsertResult::Inserted);
                REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
            }

            std::barrier start{static_cast<std::ptrdiff_t>(WriterCount + ReaderCount)};
            std::atomic<std::size_t> readersReady{};
            std::atomic<std::size_t> writersStarted{};
            std::atomic<std::size_t> writersDone{};
            std::atomic<std::size_t> readersWithSnapshot{};
            std::atomic<std::size_t> overlappingSnapshots{};
            std::atomic<int> failure{};
            std::vector<std::jthread> workers;
            workers.reserve(WriterCount + ReaderCount);

            for (std::size_t writer = 0; writer != WriterCount; ++writer) {
                workers.emplace_back([&, writer] {
                    start.arrive_and_wait();
                    while (readersReady.load(std::memory_order_acquire) != ReaderCount) {
                        std::this_thread::yield();
                    }
                    writersStarted.fetch_add(1, std::memory_order_release);
                    for (std::uint64_t iteration = 0; iteration != kIterations; ++iteration) {
                        const std::uint64_t stableKey =
                            writer * kStableKeysPerWriter + (iteration & (kStableKeysPerWriter - 1));
                        const std::uint64_t value = EncodeRevision(stableKey, iteration + 1);
                        if (map.InsertOrAssign(stableKey, value) != MapAssignResult::Assigned) {
                            failure.store(1, std::memory_order_relaxed);
                            break;
                        }
                        if ((iteration & 15U) == 0) {
                            if (!map.Erase(stableKey) || map.TryInsert(stableKey, value) != MapInsertResult::Inserted ||
                                !set.Erase(stableKey) || set.TryInsert(stableKey) != MapInsertResult::Inserted) {
                                failure.store(2, std::memory_order_relaxed);
                                break;
                            }
                        }
                        if (iteration < kGrowthKeysPerWriter) {
                            const std::uint64_t key = kStableKeyCount + writer * kGrowthKeysPerWriter + iteration;
                            if (map.TryInsert(key, EncodeRevision(key, iteration + 1)) != MapInsertResult::Inserted ||
                                set.TryInsert(key) != MapInsertResult::Inserted) {
                                failure.store(3, std::memory_order_relaxed);
                                break;
                            }
                        }
                        if (writer == 0 && iteration == kGrowthKeysPerWriter / 2) {
                            while (readersWithSnapshot.load(std::memory_order_acquire) != ReaderCount &&
                                   failure.load(std::memory_order_relaxed) == 0) {
                                std::this_thread::yield();
                            }
                        }
                        if ((iteration & 63U) == 0) {
                            std::this_thread::yield();
                        }
                    }
                    writersDone.fetch_add(1, std::memory_order_release);
                });
            }

            for (std::size_t reader = 0; reader != ReaderCount; ++reader) {
                workers.emplace_back([&] {
                    bool completedSnapshot = false;
                    start.arrive_and_wait();
                    readersReady.fetch_add(1, std::memory_order_release);
                    while (writersStarted.load(std::memory_order_acquire) != WriterCount) {
                        std::this_thread::yield();
                    }
                    while (writersDone.load(std::memory_order_acquire) != WriterCount) {
                        const auto mapEntries = map.Snapshot();
                        const auto setKeys = set.Snapshot();
                        std::array<bool, kKeyCount> mapSeen{};
                        std::array<bool, kKeyCount> setSeen{};
                        std::uint64_t previousMapKey = 0;
                        std::uint64_t previousSetKey = 0;
                        bool firstMap = true;
                        bool firstSet = true;
                        for (const auto& [key, value] : mapEntries) {
                            if (key >= kKeyCount || (value >> 32) != key || mapSeen[key] ||
                                (Order == MapOrder::Ordered && !firstMap && previousMapKey > key)) {
                                failure.store(4, std::memory_order_relaxed);
                                return;
                            }
                            mapSeen[key] = true;
                            previousMapKey = key;
                            firstMap = false;
                        }
                        for (const std::uint64_t key : setKeys) {
                            if (key >= kKeyCount || setSeen[key] ||
                                (Order == MapOrder::Ordered && !firstSet && previousSetKey > key)) {
                                failure.store(5, std::memory_order_relaxed);
                                return;
                            }
                            setSeen[key] = true;
                            previousSetKey = key;
                            firstSet = false;
                        }
                        overlappingSnapshots.fetch_add(1, std::memory_order_relaxed);
                        if (!completedSnapshot) {
                            readersWithSnapshot.fetch_add(1, std::memory_order_release);
                            completedSnapshot = true;
                        }
                    }
                });
            }

            workers.clear();
            INFO("failure code: " << failure.load(std::memory_order_relaxed));
            REQUIRE(failure.load(std::memory_order_relaxed) == 0);
            REQUIRE(readersWithSnapshot.load(std::memory_order_relaxed) == ReaderCount);
            REQUIRE(overlappingSnapshots.load(std::memory_order_relaxed) != 0);
            REQUIRE(map.SizeApprox() == kKeyCount);
            REQUIRE(set.SizeApprox() == kKeyCount);
            for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                std::uint64_t value = 0;
                REQUIRE(map.TryGet(key, value));
                REQUIRE((value >> 32) == key);
                REQUIRE(set.Contains(key));
            }
        };

        if constexpr (Capacity == std::dynamic_extent) {
            Map map{4};
            Set set{4};
            exercise(map, set);
        } else {
            Map map;
            Set set;
            exercise(map, set);
        }
    }

    template<MapTopology Topology, std::size_t Capacity>
    void RequireOrderedSnapshotSemantics() {
        using Map = ConcurrentOrderedMap<std::uint64_t, std::uint64_t, Capacity, Topology>;
        using Set = ConcurrentOrderedSet<std::uint64_t, Capacity, Topology>;
        auto verify = []<class MapType, class SetType>(MapType& map, SetType& set) {
            constexpr std::uint64_t kKeyCount = 256;
            for (std::uint64_t index = 0; index != kKeyCount; ++index) {
                const std::uint64_t key = index * 73U & (kKeyCount - 1);
                REQUIRE(map.TryInsert(key, key * 10) == MapInsertResult::Inserted);
                REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
            }
            for (std::uint64_t key = 0; key < kKeyCount; key += 5) {
                REQUIRE(map.Erase(key));
                REQUIRE(set.Erase(key));
                REQUIRE(map.TryInsert(key, key * 10) == MapInsertResult::Inserted);
                REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
            }
            const auto mapEntries = map.Snapshot();
            const auto setKeys = set.Snapshot();
            REQUIRE(mapEntries.size() == kKeyCount);
            REQUIRE(setKeys.size() == kKeyCount);
            for (std::size_t index = 0; index != kKeyCount; ++index) {
                REQUIRE(mapEntries[index].first == index);
                REQUIRE(mapEntries[index].second == index * 10);
                REQUIRE(setKeys[index] == index);
            }
        };

        if constexpr (Capacity == std::dynamic_extent) {
            Map map{8};
            Set set{8};
            verify(map, set);
        } else {
            Map map;
            Set set;
            verify(map, set);
        }
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
    using OrderedMapAlias = ConcurrentOrderedMap<std::uint64_t, std::uint64_t, 8, MapTopologies::Local>;
    using OrderedSetAlias = ConcurrentOrderedSet<std::uint64_t, 8, MapTopologies::Local>;
    STATIC_REQUIRE(HashMapAlias::order == MapOrder::Unordered);
    STATIC_REQUIRE(HashSetAlias::order == MapOrder::Unordered);
    STATIC_REQUIRE(OrderedMapAlias::order == MapOrder::Ordered);
    STATIC_REQUIRE(OrderedSetAlias::order == MapOrder::Ordered);
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

TEST_CASE("Dynamic map and set hash each mutation once without growth in every topology", AUTO_TAG) {
    auto requireOnce = []<MapTopology Topology> {
        using Map = ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, Topology, MapOrder::Unordered,
                                  CountingHash>;
        using Set = ConcurrentSet<std::uint64_t, std::dynamic_extent, Topology, MapOrder::Unordered, CountingHash>;
        Map map{2'048};
        Set set{2'048};

        CountingHash::Reset();
        for (std::uint64_t key = 0; key != 64; ++key) {
            REQUIRE(map.TryInsert(key, key + 1) == MapInsertResult::Inserted);
        }
        REQUIRE(CountingHash::Calls() == 64);

        CountingHash::Reset();
        for (std::uint64_t key = 0; key != 64; ++key) {
            REQUIRE(map.InsertOrAssign(key, key + 2) == MapAssignResult::Assigned);
        }
        REQUIRE(CountingHash::Calls() == 64);

        CountingHash::Reset();
        for (std::uint64_t key = 0; key != 64; ++key) {
            REQUIRE(map.Erase(key));
        }
        REQUIRE(CountingHash::Calls() == 64);

        CountingHash::Reset();
        for (std::uint64_t key = 0; key != 64; ++key) {
            REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
        }
        REQUIRE(CountingHash::Calls() == 64);
    };

    requireOnce.template operator()<MapTopologies::Local>();
    requireOnce.template operator()<MapTopologies::Spsc>();
    requireOnce.template operator()<MapTopologies::Mpsc>();
    requireOnce.template operator()<MapTopologies::Spmc>();
    requireOnce.template operator()<MapTopologies::Mpmc>();
}

TEST_CASE("ConcurrentMap every topology preserves collision chains across tombstone reuse", AUTO_TAG) {
    auto requireTopology = []<MapTopology Topology> {
        ConcurrentMap<std::uint64_t, std::uint64_t, 16, Topology, MapOrder::Unordered, ConstantHash> map;
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
    };

    requireTopology.template operator()<MapTopologies::Local>();
    requireTopology.template operator()<MapTopologies::Spsc>();
    requireTopology.template operator()<MapTopologies::Mpsc>();
    requireTopology.template operator()<MapTopologies::Spmc>();
    requireTopology.template operator()<MapTopologies::Mpmc>();
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
    std::atomic<bool> failed{};
    std::array<std::jthread, kWriterCount> writers;
    for (std::size_t writer = 0; writer != kWriterCount; ++writer) {
        writers[writer] = std::jthread{[&] {
            start.arrive_and_wait();
            for (std::uint64_t key = 0; key != kKeyCount; ++key) {
                const auto result = map.TryInsert(key, key);
                if (result != MapInsertResult::Inserted && result != MapInsertResult::Exists) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        }};
    }
    for (auto& writer : writers) {
        writer.join();
    }
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
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

TEST_CASE("Dynamic SPSC ConcurrentMap and ConcurrentSet cross every growth tier", AUTO_TAG_WITH("Stress")) {
    using Map = ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Spsc>;
    using Set = ConcurrentSet<std::uint64_t, std::dynamic_extent, MapTopologies::Spsc>;
    constexpr std::uint64_t kKeyCount = 75'000;
    Map map{8};
    Set set{8};
    std::size_t failures = 0;
    for (std::uint64_t key = 0; key != kKeyCount; ++key) {
        failures += map.TryInsert(key, key ^ 0xd6e8feb86659fd93ULL) != MapInsertResult::Inserted;
        failures += set.TryInsert(key) != MapInsertResult::Inserted;
    }
    REQUIRE(failures == 0);
    REQUIRE(map.SizeApprox() == kKeyCount);
    REQUIRE(set.SizeApprox() == kKeyCount);
    for (std::uint64_t key = 0; key < kKeyCount; key += 257) {
        CAPTURE(key);
        REQUIRE(map.Find(key) == (key ^ 0xd6e8feb86659fd93ULL));
        REQUIRE(set.Contains(key));
    }
    map.Clear();
    set.Clear();
    REQUIRE(map.Empty());
    REQUIRE(set.Empty());
}

TEST_CASE("Dynamic Local ConcurrentMap preserves value addresses while rebuilding its locator index", AUTO_TAG) {
    ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Local> map{8};
    REQUIRE(map.TryInsert(0, 17) == MapInsertResult::Inserted);
    std::uint64_t* const stableValue = map.FindRef(0);
    REQUIRE(stableValue != nullptr);

    for (std::uint64_t key = 1; key != 10'000; ++key) {
        REQUIRE(map.TryInsert(key, key * 3) == MapInsertResult::Inserted);
    }
    REQUIRE(map.FindRef(0) == stableValue);
    REQUIRE(map.InsertOrAssign(0, 23) == MapAssignResult::Assigned);
    REQUIRE(map.FindRef(0) == stableValue);
    REQUIRE(*stableValue == 23);
}

TEST_CASE("Dynamic Local ConcurrentMap reuses erased stable slots with exact nontrivial lifetimes",
          AUTO_TAG_WITH("Stress")) {
    REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 0);
    {
        ConcurrentMap<std::uint64_t, LifetimeTrackedValue, std::dynamic_extent, MapTopologies::Local> map{8};
        const LifetimeTrackedValue seed{17};
        REQUIRE(map.TryInsert(0, seed) == MapInsertResult::Inserted);
        LifetimeTrackedValue* const erasedValue = map.FindRef(0);
        REQUIRE(erasedValue != nullptr);
        const std::uintptr_t erasedAddress = reinterpret_cast<std::uintptr_t>(erasedValue);
        REQUIRE(map.Erase(0));

        REQUIRE(map.TryInsert(1, seed) == MapInsertResult::Inserted);
        LifetimeTrackedValue* const stableValue = map.FindRef(1);
        REQUIRE(stableValue != nullptr);
        REQUIRE(reinterpret_cast<std::uintptr_t>(stableValue) == erasedAddress);

        for (std::uint64_t key = 2; key != 4'096; ++key) {
            REQUIRE(map.TryInsert(key, LifetimeTrackedValue{key * 3}) == MapInsertResult::Inserted);
        }
        REQUIRE(map.FindRef(1) == stableValue);
        REQUIRE(stableValue->payload == 17);

        for (std::uint64_t key = 2; key != 4'096; key += 2) {
            REQUIRE(map.Erase(key));
            REQUIRE(map.TryInsert(key + 4'096, LifetimeTrackedValue{key * 5}) == MapInsertResult::Inserted);
        }
        REQUIRE(map.FindRef(1) == stableValue);
        const auto snapshot = map.Snapshot();
        REQUIRE(snapshot.size() == map.SizeApprox());

        map.Clear();
        REQUIRE(map.Empty());
        REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 1 + snapshot.size());
    }
    REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 0);

    {
        ConcurrentMap<std::uint64_t, LifetimeTrackedValue, std::dynamic_extent, MapTopologies::Local> map{8};
        for (std::uint64_t key = 0; key != 256; ++key) {
            REQUIRE(map.TryInsert(key, LifetimeTrackedValue{key}) == MapInsertResult::Inserted);
        }
    }
    REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Fixed Local ConcurrentMap destroys nontrivial full and tombstone values exactly once", AUTO_TAG) {
    REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 0);
    {
        ConcurrentMap<std::uint64_t, LifetimeTrackedValue, 32, MapTopologies::Local> map;
        const LifetimeTrackedValue seed{17};
        for (std::uint64_t key = 0; key != 16; ++key) {
            REQUIRE(map.TryInsert(key, seed) == MapInsertResult::Inserted);
        }
        for (std::uint64_t key = 0; key != 16; key += 2) {
            REQUIRE(map.Erase(key));
        }
        REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 17);
        map.Clear();
        REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 1);
    }
    REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 0);

    {
        ConcurrentMap<std::uint64_t, LifetimeTrackedValue, 32, MapTopologies::Local> map;
        for (std::uint64_t key = 0; key != 16; ++key) {
            REQUIRE(map.TryInsert(key, LifetimeTrackedValue{key}) == MapInsertResult::Inserted);
        }
        for (std::uint64_t key = 0; key != 16; key += 2) {
            REQUIRE(map.Erase(key));
        }
    }
    REQUIRE(LifetimeTrackedValue::liveInstances.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Dynamic Local map and set survive disjoint-key tombstone churn", AUTO_TAG_WITH("Stress")) {
    constexpr std::uint64_t kKeyCount = 1'024;
    constexpr std::uint64_t kRounds = 16;
    ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Local> map{8};
    ConcurrentSet<std::uint64_t, std::dynamic_extent, MapTopologies::Local> set{8};

    for (std::uint64_t key = 0; key != kKeyCount; ++key) {
        REQUIRE(map.TryInsert(key, key) == MapInsertResult::Inserted);
        REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
    }
    for (std::uint64_t round = 1; round != kRounds; ++round) {
        const std::uint64_t oldBase = (round - 1) * kKeyCount;
        const std::uint64_t newBase = round * kKeyCount;
        for (std::uint64_t offset = 0; offset != kKeyCount; ++offset) {
            REQUIRE(map.Erase(oldBase + offset));
            REQUIRE(set.Erase(oldBase + offset));
        }
        for (std::uint64_t offset = 0; offset != kKeyCount; ++offset) {
            const std::uint64_t key = newBase + offset;
            REQUIRE(map.TryInsert(key, ~key) == MapInsertResult::Inserted);
            REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
        }
        REQUIRE(map.SizeApprox() == kKeyCount);
        REQUIRE(set.SizeApprox() == kKeyCount);
    }

    const std::uint64_t finalBase = (kRounds - 1) * kKeyCount;
    for (std::uint64_t offset = 0; offset != kKeyCount; ++offset) {
        const std::uint64_t key = finalBase + offset;
        REQUIRE(map.Find(key) == ~key);
        REQUIRE(set.Contains(key));
    }
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

TEST_CASE("Dynamic ConcurrentMap every topology preserves membership under saturated collision chains",
          AUTO_TAG_WITH("Stress")) {
    auto requireTopology = []<MapTopology Topology> {
        constexpr std::uint64_t kKeyCount = 512;
        ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, Topology, MapOrder::Unordered, ConstantHash>
            map{8};
        for (std::uint64_t key = 0; key != kKeyCount; ++key) {
            REQUIRE(map.TryInsert(key, key + 1) == MapInsertResult::Inserted);
        }
        for (std::uint64_t key = 0; key != kKeyCount; key += 2) {
            REQUIRE(map.Erase(key));
            REQUIRE(map.TryInsert(key + kKeyCount, key + 2) == MapInsertResult::Inserted);
        }
        REQUIRE(map.SizeApprox() == kKeyCount);
        for (std::uint64_t key = 1; key < kKeyCount; key += 2) {
            REQUIRE(map.Find(key) == key + 1);
            REQUIRE(map.TryInsert(key, 0) == MapInsertResult::Exists);
        }
        for (std::uint64_t key = 0; key != kKeyCount; key += 2) {
            REQUIRE_FALSE(map.Contains(key));
            REQUIRE(map.Find(key + kKeyCount) == key + 2);
        }
    };

    requireTopology.template operator()<MapTopologies::Local>();
    requireTopology.template operator()<MapTopologies::Spsc>();
    requireTopology.template operator()<MapTopologies::Mpsc>();
    requireTopology.template operator()<MapTopologies::Spmc>();
    requireTopology.template operator()<MapTopologies::Mpmc>();
}

TEST_CASE("Dynamic ConcurrentMap MPMC admits exactly one duplicate-key insertion across growth",
          AUTO_TAG_WITH("Stress")) {
    constexpr std::size_t kWriterCount = 8;
    constexpr std::size_t kKeyCount = 2'000;
    ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Mpmc> map{8};
    std::array<std::atomic<std::uint8_t>, kKeyCount> insertionCounts{};
    std::barrier start{static_cast<std::ptrdiff_t>(kWriterCount)};
    std::atomic<bool> failed{};
    std::array<std::jthread, kWriterCount> writers;
    for (std::size_t writer = 0; writer != kWriterCount; ++writer) {
        writers[writer] = std::jthread{[&, writer] {
            start.arrive_and_wait();
            for (std::size_t offset = 0; offset != kKeyCount; ++offset) {
                const std::size_t key = (offset * 1'001 + writer * 977) % kKeyCount;
                const MapInsertResult result = map.TryInsert(key, key + 1);
                if (result == MapInsertResult::Inserted) {
                    insertionCounts[key].fetch_add(1, std::memory_order_relaxed);
                } else if (result != MapInsertResult::Exists) {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        }};
    }
    for (auto& writer : writers) {
        writer.join();
    }
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(map.SizeApprox() == kKeyCount);
    for (std::size_t key = 0; key != kKeyCount; ++key) {
        REQUIRE(insertionCounts[key].load(std::memory_order_relaxed) == 1);
        REQUIRE(map.Find(key) == key + 1);
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

TEST_CASE("Dynamic ConcurrentMap and ConcurrentSet preserve mutation semantics in every concurrent topology",
          AUTO_TAG_WITH("Stress")) {
    INFO("SPSC map and set");
    RequireDynamicMapMutationSemantics<MapTopologies::Spsc>();
    RequireDynamicSetMutationSemantics<MapTopologies::Spsc>();
    INFO("MPSC map and set");
    RequireDynamicMapMutationSemantics<MapTopologies::Mpsc>();
    RequireDynamicSetMutationSemantics<MapTopologies::Mpsc>();
    INFO("SPMC map and set");
    RequireDynamicMapMutationSemantics<MapTopologies::Spmc>();
    RequireDynamicSetMutationSemantics<MapTopologies::Spmc>();
    INFO("MPMC map and set");
    RequireDynamicMapMutationSemantics<MapTopologies::Mpmc>();
    RequireDynamicSetMutationSemantics<MapTopologies::Mpmc>();
}

TEST_CASE("ConcurrentMap every concurrent topology preserves per-key mutation histories", AUTO_TAG_WITH("Stress")) {
    RequireConcurrentMapMutationHistory<256, MapTopologies::Spsc, 1, 1>();
    RequireConcurrentMapMutationHistory<256, MapTopologies::Mpsc, 4, 1>();
    RequireConcurrentMapMutationHistory<256, MapTopologies::Spmc, 1, 4>();
    RequireConcurrentMapMutationHistory<256, MapTopologies::Mpmc, 4, 4>();
    RequireConcurrentMapMutationHistory<std::dynamic_extent, MapTopologies::Spsc, 1, 1>();
    RequireConcurrentMapMutationHistory<std::dynamic_extent, MapTopologies::Mpsc, 4, 1>();
    RequireConcurrentMapMutationHistory<std::dynamic_extent, MapTopologies::Spmc, 1, 4>();
    RequireConcurrentMapMutationHistory<std::dynamic_extent, MapTopologies::Mpmc, 4, 4>();
}

TEST_CASE("ConcurrentSet every concurrent topology survives erase and reuse", AUTO_TAG_WITH("Stress")) {
    RequireConcurrentSetMutationHistory<256, MapTopologies::Spsc, 1, 1>();
    RequireConcurrentSetMutationHistory<256, MapTopologies::Mpsc, 4, 1>();
    RequireConcurrentSetMutationHistory<256, MapTopologies::Spmc, 1, 4>();
    RequireConcurrentSetMutationHistory<256, MapTopologies::Mpmc, 4, 4>();
    RequireConcurrentSetMutationHistory<std::dynamic_extent, MapTopologies::Spsc, 1, 1>();
    RequireConcurrentSetMutationHistory<std::dynamic_extent, MapTopologies::Mpsc, 4, 1>();
    RequireConcurrentSetMutationHistory<std::dynamic_extent, MapTopologies::Spmc, 1, 4>();
    RequireConcurrentSetMutationHistory<std::dynamic_extent, MapTopologies::Mpmc, 4, 4>();
}

TEST_CASE("Ordered and unordered snapshots remain valid during concurrent mutation", AUTO_TAG_WITH("Stress")) {
    RequireConcurrentSnapshotProperties<512, MapTopologies::Spsc, MapOrder::Unordered, 1, 1>();
    RequireConcurrentSnapshotProperties<512, MapTopologies::Mpsc, MapOrder::Unordered, 4, 1>();
    RequireConcurrentSnapshotProperties<512, MapTopologies::Spmc, MapOrder::Unordered, 1, 4>();
    RequireConcurrentSnapshotProperties<512, MapTopologies::Mpmc, MapOrder::Unordered, 4, 4>();
    RequireConcurrentSnapshotProperties<std::dynamic_extent, MapTopologies::Spsc, MapOrder::Ordered, 1, 1>();
    RequireConcurrentSnapshotProperties<std::dynamic_extent, MapTopologies::Mpsc, MapOrder::Ordered, 4, 1>();
    RequireConcurrentSnapshotProperties<std::dynamic_extent, MapTopologies::Spmc, MapOrder::Ordered, 1, 4>();
    RequireConcurrentSnapshotProperties<std::dynamic_extent, MapTopologies::Mpmc, MapOrder::Ordered, 4, 4>();
}

TEST_CASE("Dynamic ConcurrentMap keeps a reader-held retired version alive across repeated growth",
          AUTO_TAG_WITH("Stress")) {
    using Map = ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent, MapTopologies::Mpmc,
                              MapOrder::Unordered, Hashing::DefaultAlgo, BlockingKeyEqual>;
    constexpr std::uint64_t kGrowthKeyCount = 16'384;
    BlockingKeyEqual::Reset();
    Map map{8};
    REQUIRE(map.TryInsert(0, 1) == MapInsertResult::Inserted);

    std::optional<std::uint64_t> retiredObservation;
    std::jthread reader{[&] {
        BlockingKeyEqual::blockReader = true;
        retiredObservation = map.Find(0);
        BlockingKeyEqual::blockReader = false;
    }};
    while (!BlockingKeyEqual::entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    bool growthFailed = false;
    for (std::uint64_t key = 1; key != kGrowthKeyCount; ++key) {
        if (map.TryInsert(key, key + 1) != MapInsertResult::Inserted) {
            growthFailed = true;
            break;
        }
    }
    const MapAssignResult assignResult = map.InsertOrAssign(0, 2);
    BlockingKeyEqual::release.store(true, std::memory_order_release);
    reader.join();

    REQUIRE_FALSE(growthFailed);
    REQUIRE(assignResult == MapAssignResult::Assigned);
    REQUIRE(retiredObservation == 1);
    REQUIRE(map.Find(0) == 2);
    REQUIRE(map.SizeApprox() == kGrowthKeyCount);
    map.Clear();
    REQUIRE(map.Empty());
}

TEST_CASE("Ordered ConcurrentMap and ConcurrentSet cover every topology and capacity policy", AUTO_TAG) {
    RequireOrderedSnapshotSemantics<MapTopologies::Local, 512>();
    RequireOrderedSnapshotSemantics<MapTopologies::Spsc, 512>();
    RequireOrderedSnapshotSemantics<MapTopologies::Mpsc, 512>();
    RequireOrderedSnapshotSemantics<MapTopologies::Spmc, 512>();
    RequireOrderedSnapshotSemantics<MapTopologies::Mpmc, 512>();
    RequireOrderedSnapshotSemantics<MapTopologies::Local, std::dynamic_extent>();
    RequireOrderedSnapshotSemantics<MapTopologies::Spsc, std::dynamic_extent>();
    RequireOrderedSnapshotSemantics<MapTopologies::Mpsc, std::dynamic_extent>();
    RequireOrderedSnapshotSemantics<MapTopologies::Spmc, std::dynamic_extent>();
    RequireOrderedSnapshotSemantics<MapTopologies::Mpmc, std::dynamic_extent>();
}

TEST_CASE("Ordered ConcurrentMap and ConcurrentSet honor their compile-time comparator", AUTO_TAG) {
    using Map = ConcurrentOrderedMap<std::uint64_t, std::uint64_t, 32, MapTopologies::Local, Hashing::DefaultAlgo,
                                     std::equal_to<std::uint64_t>, std::greater<std::uint64_t>>;
    using Set = ConcurrentOrderedSet<std::uint64_t, 32, MapTopologies::Local, Hashing::DefaultAlgo,
                                     std::equal_to<std::uint64_t>, std::greater<std::uint64_t>>;
    Map map;
    Set set;
    for (const std::uint64_t key : {7, 1, 15, 3, 9, 2}) {
        REQUIRE(map.TryInsert(key, key * 10) == MapInsertResult::Inserted);
        REQUIRE(set.TryInsert(key) == MapInsertResult::Inserted);
    }
    const auto mapEntries = map.Snapshot();
    const auto setKeys = set.Snapshot();
    for (std::size_t index = 1; index != mapEntries.size(); ++index) {
        REQUIRE(mapEntries[index - 1].first > mapEntries[index].first);
        REQUIRE(setKeys[index - 1] > setKeys[index]);
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
