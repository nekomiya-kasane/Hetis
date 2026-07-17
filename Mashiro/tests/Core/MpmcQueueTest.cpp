/**
 * @file MpmcQueueTest.cpp
 * @brief Correctness tests for bounded multi-producer/multi-consumer queue storage.
 */
#include "Mashiro/Core/MpmcQueue.h"

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace Mashiro;

TEST_CASE("MpmcQueue output-parameter pop preserves FIFO order", AUTO_TAG) {
    MpmcQueue<int, 8> queue;
    REQUIRE(queue.TryPush(11));
    REQUIRE(queue.TryPush(22));

    int value = 0;
    REQUIRE(queue.TryPop(value));
    REQUIRE(value == 11);
    REQUIRE(queue.TryPop(value));
    REQUIRE(value == 22);
    REQUIRE_FALSE(queue.TryPop(value));
}

TEST_CASE("MpmcQueue reuses every cell after full-ring wrap-around", AUTO_TAG) {
    MpmcQueue<std::uint64_t, 4> queue;
    std::uint64_t value = 0;
    for (std::uint64_t round = 0; round != 64; ++round) {
        for (std::uint64_t index = 0; index != 4; ++index) {
            REQUIRE(queue.TryPush(round * 4 + index));
        }
        REQUIRE_FALSE(queue.TryPush(999));
        for (std::uint64_t index = 0; index != 4; ++index) {
            REQUIRE(queue.TryPop(value));
            REQUIRE(value == round * 4 + index);
        }
    }
}

TEST_CASE("MpmcQueue optional pop supports move-only payloads", AUTO_TAG) {
    MpmcQueue<std::unique_ptr<int>, 4> queue;
    REQUIRE(queue.TryPush(std::make_unique<int>(42)));
    auto value = queue.TryPop();
    REQUIRE(value.has_value());
    REQUIRE(**value == 42);
}

TEST_CASE("MpmcQueue transfers each value exactly once under symmetric contention", AUTO_TAG) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::size_t kItemsPerProducer = 10'000;
    constexpr std::size_t kItemCount = kProducers * kItemsPerProducer;
    MpmcQueue<std::size_t, 1024> queue;
    auto seen = std::make_unique<std::atomic<std::uint8_t>[]>(kItemCount);
    std::atomic<bool> start{false};
    std::atomic<std::size_t> activeProducers{kProducers};
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> invalidValue{false};
    std::vector<std::thread> workers;

    for (std::size_t producer = 0; producer != kProducers; ++producer) {
        workers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t index = 0; index != kItemsPerProducer; ++index) {
                const std::size_t value = producer * kItemsPerProducer + index;
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
            std::size_t value = 0;
            for (;;) {
                if (queue.TryPop(value)) {
                    if (value >= kItemCount) {
                        invalidValue.store(true, std::memory_order_relaxed);
                    } else {
                        seen[value].fetch_add(1, std::memory_order_relaxed);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (activeProducers.load(std::memory_order_acquire) == 0 && queue.Empty()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }

    REQUIRE_FALSE(invalidValue.load(std::memory_order_relaxed));
    REQUIRE(consumed.load(std::memory_order_relaxed) == kItemCount);
    for (std::size_t index = 0; index != kItemCount; ++index) {
        REQUIRE(seen[index].load(std::memory_order_relaxed) == 1);
    }
}
