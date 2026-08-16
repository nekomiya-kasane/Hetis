/**
 * @file MichaelScottQueueTest.cpp
 * @brief Correctness, fixed-arena reuse, and reclamation stress for Michael-Scott MPMC storage.
 * @ingroup Testing
 */
#include "Sora/Core/Concurrency/MichaelScottQueue.h"
#include "Sora/Core/Concurrency/QueueAdapter.h"

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace Sora;

namespace {

    using SmallQueue = MichaelScottQueue<int, MichaelScottQueueConfig{.capacity = 4,
                                                                      .maxConcurrentOperations = 16,
                                                                      .reclamationThreshold = 4,
                                                                      .backing = MichaelScottBacking::Inline}>;

    using StressQueue = MichaelScottQueue<std::size_t, MichaelScottQueueConfig{.capacity = 64,
                                                                               .maxConcurrentOperations = 16,
                                                                               .reclamationThreshold = 16,
                                                                               .backing = MichaelScottBacking::Inline,
                                                                               .enableStats = true}>;

    static_assert(SmallQueue::kPhysicalNodeCount ==
                  SmallQueue::kCapacity + 1 + 3 * SmallQueue::kMaxConcurrentOperations);
    static_assert(Concurrency::QueueAdapter<SmallQueue>::pushFailurePreservesInput);
    static_assert(Concurrency::QueueAdapter<SmallQueue>::pushProgress == Concurrency::QueueProgress::LockFree);

} // namespace

TEST_CASE("Michael-Scott queue preserves FIFO order and bounded push input", AUTO_TAG) {
    SmallQueue queue;
    REQUIRE(queue.TryPush(10));
    REQUIRE(queue.TryPush(20));
    REQUIRE(queue.TryPush(30));
    REQUIRE(queue.TryPush(40));

    int rejected = 50;
    REQUIRE_FALSE(queue.TryPush(rejected));
    REQUIRE(rejected == 50);
    REQUIRE(queue.SizeApprox() == 4);

    int value = 0;
    for (int expected = 10; expected <= 40; expected += 10) {
        REQUIRE(queue.TryPop(value));
        REQUIRE(value == expected);
    }
    REQUIRE_FALSE(queue.TryPop(value));
    REQUIRE(queue.Empty());
}

TEST_CASE("Michael-Scott queue rejects an arena size that overflows the free-list index", AUTO_TAG) {
    // The historical validation divided the remaining index space by two, while
    // the arena actually needs three reclamation nodes per active operation.
    constexpr auto overflowing = MichaelScottQueueConfig{
        .capacity = 4'294'967'291uz,
        .maxConcurrentOperations = 1,
        .reclamationThreshold = 1,
    };
    STATIC_REQUIRE_FALSE(Detail::ValidateMichaelScottConfig(overflowing));
}

TEST_CASE("Michael-Scott queue supports move-only payloads", AUTO_TAG) {
    using Queue =
        MichaelScottQueue<std::unique_ptr<int>, MichaelScottQueueConfig{.capacity = 8,
                                                                        .maxConcurrentOperations = 4,
                                                                        .reclamationThreshold = 4,
                                                                        .backing = MichaelScottBacking::Inline}>;
    Queue queue;
    REQUIRE(queue.TryPush(std::make_unique<int>(42)));
    auto result = queue.TryPop();
    REQUIRE(result.has_value());
    REQUIRE(**result == 42);
}

TEST_CASE("Michael-Scott queue transfers every MPMC value exactly once", AUTO_TAG) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::size_t kItemsPerProducer = 10'000;
    constexpr std::size_t kTotal = kProducers * kItemsPerProducer;

    StressQueue queue;
    auto seen = std::make_unique<std::atomic<std::uint8_t>[]>(kTotal);
    std::atomic<bool> start{false};
    std::atomic<std::size_t> activeProducers{kProducers};
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> invalid{false};
    std::atomic<std::size_t> firstDuplicate{kTotal};
    std::vector<std::jthread> workers;
    workers.reserve(kProducers + kConsumers);

    for (std::size_t producer = 0; producer != kProducers; ++producer) {
        workers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t index = 0; index != kItemsPerProducer; ++index) {
                std::size_t value = producer * kItemsPerProducer + index;
                while (!queue.TryPush(value)) {
                    std::this_thread::yield();
                }
            }
            activeProducers.fetch_sub(1, std::memory_order_release);
        });
    }
    for (std::size_t consumer = 0; consumer != kConsumers; ++consumer) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (;;) {
                auto value = queue.TryPop();
                if (value.has_value()) {
                    if (*value >= kTotal) {
                        invalid.store(true, std::memory_order_relaxed);
                    } else {
                        if (seen[*value].fetch_add(1, std::memory_order_relaxed) != 0) {
                            std::size_t expected = kTotal;
                            firstDuplicate.compare_exchange_strong(expected, *value, std::memory_order_relaxed);
                        }
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (activeProducers.load(std::memory_order_acquire) == 0 && queue.Empty()) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    REQUIRE_FALSE(invalid.load(std::memory_order_relaxed));
    INFO("first duplicate value: " << firstDuplicate.load(std::memory_order_relaxed));
    REQUIRE(consumed.load(std::memory_order_relaxed) == kTotal);
    for (std::size_t index = 0; index != kTotal; ++index) {
        REQUIRE(seen[index].load(std::memory_order_relaxed) == 1);
    }
    REQUIRE(queue.Empty());
}

TEST_CASE("Michael-Scott queue reclamation stress reuses every arena after sustained churn", AUTO_TAG) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::size_t kRounds = 25'000;
    using Queue = MichaelScottQueue<std::uint64_t, MichaelScottQueueConfig{.capacity = 2,
                                                                           .maxConcurrentOperations = 16,
                                                                           .reclamationThreshold = 2,
                                                                           .backing = MichaelScottBacking::Inline,
                                                                           .enableStats = true}>;
    Queue queue;
    std::atomic<bool> start{false};
    std::atomic<std::size_t> active{kProducers};
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> invalid{false};
    std::vector<std::jthread> workers;
    workers.reserve(kProducers + kConsumers);

    for (std::size_t producer = 0; producer != kProducers; ++producer) {
        workers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t index = 0; index != kRounds; ++index) {
                std::uint64_t value = (producer << 32) | index;
                while (!queue.TryPush(value)) {
                    std::this_thread::yield();
                }
            }
            active.fetch_sub(1, std::memory_order_release);
        });
    }
    for (std::size_t consumer = 0; consumer != kConsumers; ++consumer) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (;;) {
                auto value = queue.TryPop();
                if (value.has_value()) {
                    if (static_cast<std::size_t>(*value & 0xFFFF'FFFFu) >= kRounds) {
                        invalid.store(true, std::memory_order_relaxed);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (active.load(std::memory_order_acquire) == 0 && queue.Empty()) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    REQUIRE_FALSE(invalid.load(std::memory_order_relaxed));
    REQUIRE(consumed.load(std::memory_order_relaxed) == kProducers * kRounds);
    REQUIRE(queue.Empty());
    REQUIRE(queue.ReclaimedNodeCount() > kProducers * kRounds / 2);
    REQUIRE(queue.CollectionCount() > 0);
}
