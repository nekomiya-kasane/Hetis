/**
 * @file BoundedStackTest.cpp
 * @brief Topology, LIFO, lifetime, batch, and concurrent-reuse tests for BoundedStack.
 * @ingroup Testing
 */
#include <Mashiro/Core/BoundedStack.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Concurrency;
using namespace std::chrono_literals;

namespace {

    template<QueueTopology Topology>
    using Stack = BoundedStack<std::uint64_t, 256, Topology>;

    template<QueueTopology Topology>
    void RequireSequentialLifo() {
        BoundedStack<int, 8, Topology> stack;
        for (int value = 0; value != 8; ++value) {
            REQUIRE(stack.TryPush(value));
        }
        REQUIRE_FALSE(stack.TryPush(8));
        for (int value = 7; value >= 0; --value) {
            REQUIRE(stack.TryPop() == value);
        }
        REQUIRE(stack.Empty());
    }

    template<QueueTopology Topology, std::size_t ProducerCount, std::size_t ConsumerCount>
    void RequireConcurrentExactlyOnce() {
        constexpr std::size_t kValuesPerProducer = 2'000;
        constexpr std::size_t kValueCount = ProducerCount * kValuesPerProducer;
        BoundedStack<std::size_t, 256, Topology> stack;
        std::array<std::atomic_uint8_t, kValueCount> seen{};
        std::atomic<std::size_t> producersDone{};
        std::atomic<std::size_t> consumed{};
        std::atomic<bool> timedOut{};
        std::barrier start{static_cast<std::ptrdiff_t>(ProducerCount + ConsumerCount)};
        std::vector<std::jthread> workers;
        workers.reserve(ProducerCount + ConsumerCount);

        for (std::size_t producer = 0; producer != ProducerCount; ++producer) {
            workers.emplace_back([&, producer] {
                start.arrive_and_wait();
                const auto deadline = std::chrono::steady_clock::now() + 10s;
                for (std::size_t offset = 0; offset != kValuesPerProducer; ++offset) {
                    std::size_t value = producer * kValuesPerProducer + offset;
                    while (!stack.TryPush(value)) {
                        if (std::chrono::steady_clock::now() >= deadline) {
                            timedOut.store(true, std::memory_order_relaxed);
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
                producersDone.fetch_add(1, std::memory_order_release);
            });
        }

        for (std::size_t consumer = 0; consumer != ConsumerCount; ++consumer) {
            workers.emplace_back([&] {
                start.arrive_and_wait();
                const auto deadline = std::chrono::steady_clock::now() + 10s;
                for (;;) {
                    if (auto value = stack.TryPop()) {
                        if (*value >= kValueCount) {
                            timedOut.store(true, std::memory_order_relaxed);
                            return;
                        }
                        seen[*value].fetch_add(1, std::memory_order_relaxed);
                        consumed.fetch_add(1, std::memory_order_release);
                        continue;
                    }
                    if (producersDone.load(std::memory_order_acquire) == ProducerCount && stack.Empty()) {
                        return;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        timedOut.store(true, std::memory_order_relaxed);
                        return;
                    }
                    std::this_thread::yield();
                }
            });
        }

        workers.clear();
        REQUIRE_FALSE(timedOut.load(std::memory_order_relaxed));
        REQUIRE(consumed.load(std::memory_order_acquire) == kValueCount);
        for (const auto& count : seen) {
            REQUIRE(count.load(std::memory_order_relaxed) == 1);
        }
    }

    struct LifetimeValue {
        std::atomic<int>* destructions{};
        int value{};

        LifetimeValue(std::atomic<int>& count, int input) noexcept : destructions(&count), value(input) {}
        LifetimeValue(const LifetimeValue&) = delete;
        LifetimeValue& operator=(const LifetimeValue&) = delete;
        LifetimeValue(LifetimeValue&& other) noexcept
            : destructions(std::exchange(other.destructions, nullptr)), value(other.value) {}
        LifetimeValue& operator=(LifetimeValue&&) = delete;
        ~LifetimeValue() {
            if (destructions != nullptr) {
                destructions->fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    struct ThrowingMoveValue {
        std::atomic<int>* live{};
        int value{};
        bool throwOnMove{};

        ThrowingMoveValue(std::atomic<int>& count, int input, bool shouldThrow = false) noexcept
            : live(&count), value(input), throwOnMove(shouldThrow) {
            live->fetch_add(1, std::memory_order_relaxed);
        }

        ThrowingMoveValue(const ThrowingMoveValue&) = delete;
        ThrowingMoveValue& operator=(const ThrowingMoveValue&) = delete;

        ThrowingMoveValue(ThrowingMoveValue&& other) : live(other.live), value(other.value) {
            if (other.throwOnMove) {
                throw std::runtime_error("requested move failure");
            }
            other.live = nullptr;
        }

        ThrowingMoveValue& operator=(ThrowingMoveValue&& other) {
            if (other.throwOnMove) {
                throw std::runtime_error("requested move-assignment failure");
            }
            value = other.value;
            return *this;
        }

        ~ThrowingMoveValue() {
            if (live != nullptr) {
                live->fetch_sub(1, std::memory_order_relaxed);
            }
        }
    };

    static_assert(Stack<QueueTopologies::Local>::order == QueueOrder::Lifo);
    static_assert(Stack<QueueTopologies::Spsc>::topology == QueueTopologies::Spsc);
    static_assert(Stack<QueueTopologies::Mpsc>::topology == QueueTopologies::Mpsc);
    static_assert(Stack<QueueTopologies::Spmc>::topology == QueueTopologies::Spmc);
    static_assert(Stack<QueueTopologies::Mpmc>::topology == QueueTopologies::Mpmc);
    static_assert(Stack<QueueTopologies::Mpmc>::transactionalPushBatch);
    static_assert(Stack<QueueTopologies::Mpmc>::GetCapacity() == 256);
    static_assert(AuditFalseSharing<Stack<QueueTopologies::Spsc>>());
    static_assert(AuditFalseSharing<Stack<QueueTopologies::Mpsc>>());
    static_assert(AuditFalseSharing<Stack<QueueTopologies::Spmc>>());
    static_assert(AuditFalseSharing<Stack<QueueTopologies::Mpmc>>());
    static_assert(!std::copy_constructible<Stack<QueueTopologies::Spsc>>);
    static_assert(!std::move_constructible<Stack<QueueTopologies::Spsc>>);

} // namespace

TEST_CASE("BoundedStack every topology exposes sequential LIFO semantics", AUTO_TAG) {
    RequireSequentialLifo<QueueTopologies::Local>();
    RequireSequentialLifo<QueueTopologies::Spsc>();
    RequireSequentialLifo<QueueTopologies::Mpsc>();
    RequireSequentialLifo<QueueTopologies::Spmc>();
    RequireSequentialLifo<QueueTopologies::Mpmc>();
}

TEST_CASE("BoundedStack reuses every slot without stale-head ABA", AUTO_TAG) {
    BoundedStack<std::uint64_t, 2, QueueTopologies::Mpmc> stack;
    for (std::uint64_t cycle = 0; cycle != 100'000; ++cycle) {
        REQUIRE(stack.TryPush(cycle));
        REQUIRE(stack.TryPush(cycle + 1));
        REQUIRE(stack.TryPop() == cycle + 1);
        REQUIRE(stack.TryPop() == cycle);
    }
}

TEST_CASE("BoundedStack supports move-only values and exact destruction", AUTO_TAG) {
    std::atomic<int> destructions{};
    {
        BoundedStack<LifetimeValue, 4, QueueTopologies::Spsc> stack;
        REQUIRE(stack.TryEmplace(destructions, 1));
        REQUIRE(stack.TryEmplace(destructions, 2));
        auto value = stack.TryPop();
        REQUIRE(value.has_value());
        REQUIRE(value->value == 2);
    }
    REQUIRE(destructions.load(std::memory_order_relaxed) == 2);

    BoundedStack<std::unique_ptr<int>, 2, QueueTopologies::Mpmc> pointers;
    REQUIRE(pointers.TryPush(std::make_unique<int>(42)));
    auto pointer = pointers.TryPop();
    REQUIRE(pointer.has_value());
    REQUIRE(**pointer == 42);
}

TEST_CASE("BoundedStack local operations retain live objects when a move throws", AUTO_TAG) {
    std::atomic<int> live{};
    {
        BoundedStack<ThrowingMoveValue, 4, QueueTopologies::Local> stack;
        REQUIRE(stack.TryEmplace(live, 1, true));
        REQUIRE_THROWS_AS(stack.TryPop(), std::runtime_error);
        REQUIRE(stack.SizeApprox() == 1);

        ThrowingMoveValue output{live, 0};
        REQUIRE_THROWS_AS(stack.TryPop(output), std::runtime_error);
        REQUIRE(stack.SizeApprox() == 1);
    }
    REQUIRE(live.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("BoundedStack local batch rolls back constructed values when a move throws", AUTO_TAG) {
    std::atomic<int> live{};
    {
        BoundedStack<ThrowingMoveValue, 4, QueueTopologies::Local> stack;
        std::array batch{ThrowingMoveValue{live, 1}, ThrowingMoveValue{live, 2, true}};
        REQUIRE_THROWS_AS(stack.TryPushBatch(batch), std::runtime_error);
        REQUIRE(stack.Empty());
    }
    REQUIRE(live.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("BoundedStack batch publication is transactional and follows sequential push order", AUTO_TAG) {
    BoundedStack<int, 8, QueueTopologies::Mpmc> stack;
    REQUIRE(stack.TryPush(1));
    std::array batch{2, 3, 4};
    REQUIRE(stack.TryPushBatch(batch));
    REQUIRE(stack.SizeApprox() == 4);
    REQUIRE(stack.TryPop() == 4);
    REQUIRE(stack.TryPop() == 3);
    REQUIRE(stack.TryPop() == 2);
    REQUIRE(stack.TryPop() == 1);

    std::array oversized{1, 2, 3, 4, 5, 6, 7, 8, 9};
    REQUIRE_FALSE(stack.TryPushBatch(oversized));
    REQUIRE(stack.Empty());
}

TEST_CASE("BoundedStack SPSC gives newly published values priority over its consumer-local stack", AUTO_TAG) {
    BoundedStack<int, 8, QueueTopologies::Spsc> stack;
    REQUIRE(stack.TryPush(1));
    REQUIRE(stack.TryPush(2));
    REQUIRE(stack.TryPop() == 2);

    REQUIRE(stack.TryPush(3));
    REQUIRE(stack.TryPush(4));
    REQUIRE(stack.SizeApprox() == 3);
    REQUIRE(stack.TryPop() == 4);
    REQUIRE(stack.TryPop() == 3);
    REQUIRE(stack.TryPop() == 1);
    REQUIRE(stack.Empty());
}

TEST_CASE("BoundedStack MPSC gives newly published values priority over its consumer-local stack", AUTO_TAG) {
    BoundedStack<int, 8, QueueTopologies::Mpsc> stack;
    REQUIRE(stack.TryPush(1));
    REQUIRE(stack.TryPush(2));
    REQUIRE(stack.TryPop() == 2);

    REQUIRE(stack.TryPush(3));
    REQUIRE(stack.TryPush(4));
    REQUIRE(stack.SizeApprox() == 3);
    REQUIRE(stack.TryPop() == 4);
    REQUIRE(stack.TryPop() == 3);
    REQUIRE(stack.TryPop() == 1);
    REQUIRE(stack.Empty());
}

TEST_CASE("BoundedStack SPMC refills its producer-local free-node chain", AUTO_TAG) {
    BoundedStack<int, 2, QueueTopologies::Spmc> stack;
    for (int cycle = 0; cycle != 10'000; ++cycle) {
        REQUIRE(stack.TryPush(cycle));
        REQUIRE(stack.TryPush(cycle + 1));
        REQUIRE_FALSE(stack.TryPush(cycle + 2));
        REQUIRE(stack.TryPop() == cycle + 1);
        REQUIRE(stack.TryPop() == cycle);
    }
}

TEST_CASE("BoundedStack SPSC publishes each batch transactionally above older local values", AUTO_TAG) {
    BoundedStack<int, 8, QueueTopologies::Spsc> stack;
    REQUIRE(stack.TryPush(1));
    std::array firstBatch{2, 3, 4};
    REQUIRE(stack.TryPushBatch(firstBatch));
    REQUIRE(stack.TryPop() == 4);

    std::array secondBatch{5, 6};
    REQUIRE(stack.TryPushBatch(secondBatch));
    REQUIRE(stack.TryPop() == 6);
    REQUIRE(stack.TryPop() == 5);
    REQUIRE(stack.TryPop() == 3);
    REQUIRE(stack.TryPop() == 2);
    REQUIRE(stack.TryPop() == 1);
    REQUIRE(stack.Empty());
}

TEST_CASE("BoundedStack MPSC transfers every value exactly once", AUTO_TAG) {
    RequireConcurrentExactlyOnce<QueueTopologies::Mpsc, 4, 1>();
}

TEST_CASE("BoundedStack SPSC transfers every value exactly once", AUTO_TAG) {
    RequireConcurrentExactlyOnce<QueueTopologies::Spsc, 1, 1>();
}

TEST_CASE("BoundedStack SPMC transfers every value exactly once", AUTO_TAG) {
    RequireConcurrentExactlyOnce<QueueTopologies::Spmc, 1, 4>();
}

TEST_CASE("BoundedStack MPMC transfers every value exactly once", AUTO_TAG) {
    RequireConcurrentExactlyOnce<QueueTopologies::Mpmc, 4, 4>();
}
