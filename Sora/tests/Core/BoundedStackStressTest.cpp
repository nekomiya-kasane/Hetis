/**
 * @file BoundedStackStressTest.cpp
 * @brief High-contention randomized invariants for every bounded LIFO topology.
 * @ingroup Testing
 */
#include <Sora/Core/Concurrency/BoundedStack.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace Sora;
using namespace Sora::Concurrency;

namespace {

    struct Payload {
        std::uint64_t ticket{};
        std::uint64_t guard{};

        Payload() noexcept = default;
        Payload(std::uint64_t value) noexcept : ticket(value), guard(~value) {}
        Payload(const Payload&) = delete;
        Payload& operator=(const Payload&) = delete;
        Payload(Payload&&) noexcept = default;
        Payload& operator=(Payload&&) noexcept = default;
    };

    struct PayloadBatch {
        std::array<Payload, 4> values{};
        std::size_t count{};

        [[nodiscard]] std::size_t size() const noexcept { return count; }
        [[nodiscard]] Payload& operator[](std::size_t index) noexcept { return values[index]; }
    };

    [[nodiscard]] std::uint64_t NextRandom(std::uint64_t& state) noexcept {
        state ^= state << 7;
        state ^= state >> 9;
        state ^= state << 8;
        return state;
    }

    template<QueueTopology Topology, std::size_t Capacity, std::size_t ProducerCount, std::size_t ConsumerCount>
    void RunStress(std::uint64_t seed) {
        constexpr std::size_t kValuesPerProducer = 8'000;
        constexpr std::size_t kTotal = ProducerCount * kValuesPerProducer;
        using Stack = BoundedStack<Payload, Capacity, Topology>;

        Stack stack;
        auto seen = std::make_unique<std::atomic_uint8_t[]>(kTotal);
        for (std::size_t index = 0; index != kTotal; ++index) {
            seen[index].store(0, std::memory_order_relaxed);
        }
        std::atomic<std::size_t> activeProducers{ProducerCount};
        std::atomic<std::size_t> consumed{};
        std::atomic<bool> failed{};
        std::barrier start{static_cast<std::ptrdiff_t>(ProducerCount + ConsumerCount)};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        std::vector<std::jthread> workers;
        workers.reserve(ProducerCount + ConsumerCount);

        for (std::size_t producer = 0; producer != ProducerCount; ++producer) {
            workers.emplace_back([&, producer] {
                std::uint64_t random = seed ^ (producer + 1) * 0x9E3779B97F4A7C15ull;
                start.arrive_and_wait();
                for (std::size_t index = 0; index != kValuesPerProducer;) {
                    if (failed.load(std::memory_order_relaxed)) {
                        break;
                    }
                    constexpr std::size_t kMaxBatch = Capacity < 4 ? Capacity : 4;
                    const std::size_t remaining = kValuesPerProducer - index;
                    const std::size_t count = std::min<std::size_t>(1 + NextRandom(random) % kMaxBatch, remaining);
                    PayloadBatch batch{.count = count};
                    for (std::size_t offset = 0; offset != count; ++offset) {
                        batch.values[offset] = Payload{producer * kValuesPerProducer + index + offset};
                    }
                    const auto tryTransfer = [&]() noexcept {
                        return count == 1 ? stack.TryPush(std::move(batch.values[0])) : stack.TryPushBatch(batch);
                    };
                    while (!tryTransfer()) {
                        if (std::chrono::steady_clock::now() >= deadline) {
                            failed.store(true, std::memory_order_release);
                            break;
                        }
                        if ((NextRandom(random) & 7u) == 0) {
                            std::this_thread::yield();
                        }
                    }
                    index += count;
                }
                activeProducers.fetch_sub(1, std::memory_order_release);
            });
        }

        for (std::size_t consumer = 0; consumer != ConsumerCount; ++consumer) {
            workers.emplace_back([&, consumer] {
                std::uint64_t random = seed ^ (consumer + ProducerCount + 1) * 0xD1B54A32D192ED03ull;
                start.arrive_and_wait();
                for (;;) {
                    if (auto value = stack.TryPop()) {
                        if (value->ticket >= kTotal || value->guard != ~value->ticket ||
                            seen[value->ticket].fetch_add(1, std::memory_order_relaxed) != 0) {
                            failed.store(true, std::memory_order_release);
                        }
                        consumed.fetch_add(1, std::memory_order_release);
                    } else if (failed.load(std::memory_order_acquire) ||
                               (activeProducers.load(std::memory_order_acquire) == 0 && stack.Empty())) {
                        break;
                    } else if (std::chrono::steady_clock::now() >= deadline) {
                        failed.store(true, std::memory_order_release);
                        break;
                    } else if ((NextRandom(random) & 7u) == 0) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        workers.clear();

        REQUIRE_FALSE(failed.load(std::memory_order_acquire));
        REQUIRE(consumed.load(std::memory_order_acquire) == kTotal);
        REQUIRE(stack.Empty());
        for (std::size_t index = 0; index != kTotal; ++index) {
            REQUIRE(seen[index].load(std::memory_order_relaxed) == 1);
        }
    }

} // namespace

TEST_CASE("BoundedStack stress preserves payloads under SPSC churn", AUTO_TAG_WITH("Stress")) {
    for (std::uint64_t round = 0; round != 3; ++round) {
        RunStress<QueueTopologies::Spsc, 1, 1, 1>(0x1000 + round);
        RunStress<QueueTopologies::Spsc, 7, 1, 1>(0x1100 + round);
    }
}

TEST_CASE("BoundedStack stress preserves payloads under MPSC churn", AUTO_TAG_WITH("Stress")) {
    for (std::uint64_t round = 0; round != 3; ++round) {
        RunStress<QueueTopologies::Mpsc, 1, 4, 1>(0x2000 + round);
        RunStress<QueueTopologies::Mpsc, 7, 4, 1>(0x2100 + round);
    }
}

TEST_CASE("BoundedStack stress preserves payloads under SPMC churn", AUTO_TAG_WITH("Stress")) {
    for (std::uint64_t round = 0; round != 3; ++round) {
        RunStress<QueueTopologies::Spmc, 1, 1, 4>(0x3000 + round);
        RunStress<QueueTopologies::Spmc, 7, 1, 4>(0x3100 + round);
    }
}

TEST_CASE("BoundedStack stress preserves payloads under MPMC churn", AUTO_TAG_WITH("Stress")) {
    for (std::uint64_t round = 0; round != 3; ++round) {
        RunStress<QueueTopologies::Mpmc, 1, 4, 4>(0x4000 + round);
        RunStress<QueueTopologies::Mpmc, 3, 4, 4>(0x5000 + round);
        RunStress<QueueTopologies::Mpmc, 17, 4, 4>(0x6000 + round);
    }
}
