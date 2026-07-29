/**
 * @file RingBufferTopologyTest.cpp
 * @brief Tests compile-time topology reduction plus local and SPMC RingBuffer protocols.
 */
#include "Mashiro/Core/RingBuffer.h"

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

using namespace Mashiro;

TEST_CASE("RingBuffer exact participant counts reduce to canonical topologies", AUTO_TAG) {
    using namespace Concurrency;
    STATIC_REQUIRE(StaticQueueTopology<1, 1, 1>() == QueueTopologies::Local);
    STATIC_REQUIRE(StaticQueueTopology<1, 1, 0>() == QueueTopologies::Spsc);
    STATIC_REQUIRE(StaticQueueTopology<4, 1, 0>() == QueueTopologies::Mpsc);
    STATIC_REQUIRE(StaticQueueTopology<4, 1, 1>() == QueueTopologies::Mpsc);
    STATIC_REQUIRE(StaticQueueTopology<1, 4, 1>() == QueueTopologies::Spmc);
    STATIC_REQUIRE(StaticQueueTopology<4, 4, 4>() == QueueTopologies::Mpmc);

    using Counted = RingBuffer<int, 8, StaticQueueTopology<4, 1, 1>()>;
    using Canonical = RingBuffer<int, 8, QueueTopologies::Mpsc>;
    STATIC_REQUIRE(std::is_same_v<Counted, Canonical>);
}

TEST_CASE("single-thread RingBuffer uses the local FIFO protocol", AUTO_TAG) {
    using LocalRing = RingBuffer<int, 4, Concurrency::QueueTopologies::Local>;
    LocalRing ring;

    STATIC_REQUIRE(LocalRing::topology.sharing == Concurrency::QueueSharing::SingleThread);
    REQUIRE(ring.TryPush(11));
    REQUIRE(ring.TryPush(22));
    REQUIRE(ring.SizeApprox() == 2);

    int value = 0;
    REQUIRE(ring.TryPop(value));
    REQUIRE(value == 11);
    REQUIRE(ring.TryPop(value));
    REQUIRE(value == 22);
    REQUIRE(ring.Empty());
}

TEST_CASE("SPMC RingBuffer transfers each value exactly once", AUTO_TAG) {
    constexpr std::size_t kValueCount = 80'000;
    constexpr std::size_t kConsumerCount = 4;
    using SpmcRing = RingBuffer<std::size_t, 1024, Concurrency::QueueTopologies::Spmc>;

    SpmcRing ring;
    auto seen = std::make_unique<std::atomic_uint8_t[]>(kValueCount);
    std::atomic_size_t consumed{0};
    std::atomic_bool producerDone{false};
    std::vector<std::jthread> consumers;
    consumers.reserve(kConsumerCount);

    for (std::size_t consumer = 0; consumer != kConsumerCount; ++consumer) {
        consumers.emplace_back([&] {
            while (!producerDone.load(std::memory_order_acquire) ||
                   consumed.load(std::memory_order_relaxed) != kValueCount) {
                auto value = ring.TryPop();
                if (!value.has_value()) {
                    std::this_thread::yield();
                    continue;
                }
                REQUIRE(*value < kValueCount);
                REQUIRE(seen[*value].fetch_add(1, std::memory_order_relaxed) == 0);
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t value = 0; value != kValueCount; ++value) {
        while (!ring.TryPush(value)) {
            std::this_thread::yield();
        }
    }
    producerDone.store(true, std::memory_order_release);
    consumers.clear();

    REQUIRE(consumed.load(std::memory_order_relaxed) == kValueCount);
    REQUIRE(ring.Empty());
    for (std::size_t value = 0; value != kValueCount; ++value) {
        REQUIRE(seen[value].load(std::memory_order_relaxed) == 1);
    }
}
