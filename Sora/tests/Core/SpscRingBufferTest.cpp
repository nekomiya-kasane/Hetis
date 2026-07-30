#include <Sora/Core/Concurrency/RingBuffer.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {

    template<class T, std::size_t Capacity>
    using SpscRing = Sora::Concurrency::RingBuffer<T, Capacity, Sora::Concurrency::QueueTopologies::Spsc>;

    struct TrackedValue {
        inline static int liveCount = 0;
        inline static int moveCount = 0;

        int value = 0;

        explicit TrackedValue(int initial) noexcept : value(initial) { ++liveCount; }
        TrackedValue(TrackedValue&& other) noexcept : value(other.value) {
            ++liveCount;
            ++moveCount;
        }
        TrackedValue& operator=(TrackedValue&& other) noexcept {
            value = other.value;
            ++moveCount;
            return *this;
        }
        ~TrackedValue() noexcept { --liveCount; }

        TrackedValue(const TrackedValue&) = delete;
        TrackedValue& operator=(const TrackedValue&) = delete;
    };

    struct ThrowingMove {
        inline static bool throwOnMove = false;

        int value = 0;

        explicit ThrowingMove(int initial) noexcept : value(initial) {}
        ThrowingMove(ThrowingMove&& other) : value(other.value) {
            if (throwOnMove) {
                throw std::runtime_error("move failed");
            }
        }
        ThrowingMove& operator=(ThrowingMove&& other) {
            if (throwOnMove) {
                throw std::runtime_error("move assignment failed");
            }
            value = other.value;
            return *this;
        }

        ThrowingMove(const ThrowingMove&) = delete;
        ThrowingMove& operator=(const ThrowingMove&) = delete;
    };

    template<Sora::Concurrency::QueueTopology Topology, std::size_t ProducerCount, std::size_t ConsumerCount>
    [[nodiscard]] bool TransfersExactlyOnce() {
        constexpr std::size_t kValuesPerProducer = 4'000;
        constexpr std::size_t kTotal = ProducerCount * kValuesPerProducer;
        using Ring = Sora::Concurrency::RingBuffer<std::uint32_t, 256, Topology>;

        Ring ring;
        std::array<std::atomic<std::uint8_t>, kTotal> occurrences{};
        std::array<std::size_t, ProducerCount> nextSequence{};
        std::atomic<std::size_t> consumed = 0;
        std::atomic<bool> start = false;
        std::atomic<bool> valid = true;

        std::array<std::jthread, ConsumerCount> consumers;
        for (std::size_t consumer = 0; consumer != ConsumerCount; ++consumer) {
            consumers[consumer] = std::jthread([&] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                while (consumed.load(std::memory_order_acquire) != kTotal) {
                    auto value = ring.TryPop();
                    if (!value) {
                        std::this_thread::yield();
                        continue;
                    }
                    if (*value >= kTotal) {
                        valid.store(false, std::memory_order_relaxed);
                    } else {
                        if (occurrences[*value].fetch_add(1, std::memory_order_relaxed) != 0) {
                            valid.store(false, std::memory_order_relaxed);
                        }
                        if constexpr (ConsumerCount == 1) {
                            const std::size_t producer = *value / kValuesPerProducer;
                            const std::size_t sequence = *value % kValuesPerProducer;
                            if (sequence != nextSequence[producer]++) {
                                valid.store(false, std::memory_order_relaxed);
                            }
                        }
                    }
                    consumed.fetch_add(1, std::memory_order_release);
                }
            });
        }

        std::array<std::jthread, ProducerCount> producers;
        for (std::size_t producer = 0; producer != ProducerCount; ++producer) {
            producers[producer] = std::jthread([&, producer] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const std::uint32_t begin = static_cast<std::uint32_t>(producer * kValuesPerProducer);
                const std::uint32_t end = static_cast<std::uint32_t>(begin + kValuesPerProducer);
                for (std::uint32_t value = begin; value != end; ++value) {
                    while (!ring.TryPush(value)) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& producer : producers) {
            producer.join();
        }
        for (auto& consumer : consumers) {
            consumer.join();
        }

        if (consumed.load(std::memory_order_relaxed) != kTotal || !ring.IsEmpty()) {
            return false;
        }
        for (const auto& count : occurrences) {
            if (count.load(std::memory_order_relaxed) != 1) {
                return false;
            }
        }
        return valid.load(std::memory_order_relaxed);
    }

} // namespace

TEST_CASE("SpscRingBuffer preserves bounded FIFO semantics", "[Sora.Core.Concurrency.SpscRingBuffer]") {
    SpscRing<int, 4> ring;

    REQUIRE(ring.IsEmpty());
    REQUIRE_FALSE(ring.IsFull());
    REQUIRE_FALSE(ring.TryPop().has_value());

    REQUIRE(ring.TryPush(1));
    REQUIRE(ring.TryPush(2));
    REQUIRE(ring.TryPush(3));
    REQUIRE(ring.TryPush(4));
    REQUIRE_FALSE(ring.TryPush(5));
    REQUIRE(ring.IsFull());
    REQUIRE(ring.SizeApprox() == 4);

    for (int expected = 1; expected <= 4; ++expected) {
        const auto value = ring.TryPop();
        REQUIRE(value.has_value());
        REQUIRE(*value == expected);
    }
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("SpscRingBuffer wraps physical slots without disturbing FIFO order",
          "[Sora.Core.Concurrency.SpscRingBuffer]") {
    SpscRing<std::uint32_t, 8> ring;

    for (std::uint32_t cycle = 0; cycle < 1'000; ++cycle) {
        for (std::uint32_t offset = 0; offset < ring.kCapacity; ++offset) {
            REQUIRE(ring.TryPush(cycle * ring.kCapacity + offset));
        }
        for (std::uint32_t offset = 0; offset < ring.kCapacity; ++offset) {
            REQUIRE(ring.TryPop() == cycle * ring.kCapacity + offset);
        }
    }
}

TEST_CASE("SpscRingBuffer destroys exactly the elements that remain queued", "[Sora.Core.Concurrency.SpscRingBuffer]") {
    REQUIRE(TrackedValue::liveCount == 0);
    TrackedValue::moveCount = 0;
    {
        SpscRing<TrackedValue, 8> empty;
    }
    REQUIRE(TrackedValue::liveCount == 0);

    {
        SpscRing<TrackedValue, 8> ring;
        REQUIRE(ring.TryEmplace(1));
        REQUIRE(ring.TryEmplace(2));
        REQUIRE(ring.TryEmplace(3));
        REQUIRE(TrackedValue::liveCount == 3);

        auto consumed = ring.TryPop();
        REQUIRE(consumed.has_value());
        REQUIRE(TrackedValue::liveCount == 3);
    }
    REQUIRE(TrackedValue::liveCount == 0);
}

TEST_CASE("Local RingBuffer preserves state when moving or draining throws", "[Sora.Core.Concurrency.RingBuffer]") {
    using Ring = Sora::Concurrency::RingBuffer<ThrowingMove, 4, Sora::Concurrency::QueueTopologies::Local>;
    Ring ring;

    REQUIRE(ring.TryEmplace(7));
    REQUIRE(ring.TryEmplace(8));

    ThrowingMove::throwOnMove = true;
    REQUIRE_THROWS_AS(ring.TryPop(), std::runtime_error);
    REQUIRE(ring.SizeApprox() == 2);

    ThrowingMove::throwOnMove = false;
    auto first = ring.TryPop();
    REQUIRE(first.has_value());
    REQUIRE(first->value == 7);

    REQUIRE_THROWS_AS(ring.Drain([](ThrowingMove&&) { throw std::runtime_error("sink failed"); }), std::runtime_error);
    REQUIRE(ring.SizeApprox() == 1);
    REQUIRE(ring.Front() != nullptr);
    REQUIRE(ring.Front()->value == 8);
    ring.PopFront();
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("SPSC RingBuffer publishes successful Drain elements before a sink exception",
          "[Sora.Core.Concurrency.RingBuffer]") {
    SpscRing<int, 8> ring;
    REQUIRE(ring.TryPush(1));
    REQUIRE(ring.TryPush(2));
    REQUIRE(ring.TryPush(3));

    int calls = 0;
    REQUIRE_THROWS_AS(ring.Drain([&calls](int&&) {
        if (++calls == 2) {
            throw std::runtime_error("sink failed");
        }
    }),
                      std::runtime_error);

    REQUIRE(ring.SizeApprox() == 2);
    REQUIRE(ring.TryPop() == 2);
    REQUIRE(ring.TryPop() == 3);
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("SPSC RingBuffer commits Pop only after a throwing move succeeds", "[Sora.Core.Concurrency.RingBuffer]") {
    SpscRing<ThrowingMove, 4> ring;
    ring.Emplace(9);

    ThrowingMove::throwOnMove = true;
    REQUIRE_THROWS_AS(ring.Pop(), std::runtime_error);
    REQUIRE(ring.SizeApprox() == 1);

    ThrowingMove::throwOnMove = false;
    REQUIRE(ring.Pop().value == 9);
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("Sequence-cell RingBuffer destruction does not move queued payloads", "[Sora.Core.Concurrency.RingBuffer]") {
    using Ring = Sora::Concurrency::RingBuffer<TrackedValue, 8, Sora::Concurrency::QueueTopologies::Mpsc>;

    REQUIRE(TrackedValue::liveCount == 0);
    TrackedValue::moveCount = 0;
    {
        Ring ring;
        REQUIRE(ring.TryEmplace(1));
        REQUIRE(ring.TryEmplace(2));
        REQUIRE(TrackedValue::liveCount == 2);
    }
    REQUIRE(TrackedValue::liveCount == 0);
    REQUIRE(TrackedValue::moveCount == 0);
}

TEST_CASE("MPSC RingBuffer drains committed cells and supports move-assignment consumption",
          "[Sora.Core.Concurrency.RingBuffer]") {
    using Ring = Sora::Concurrency::RingBuffer<int, 8, Sora::Concurrency::QueueTopologies::Mpsc>;
    Ring ring;
    REQUIRE(ring.TryPush(1));
    REQUIRE(ring.TryPush(2));
    REQUIRE(ring.TryPush(3));

    int calls = 0;
    REQUIRE_THROWS_AS(ring.Drain([&calls](int&&) {
        if (++calls == 2) {
            throw std::runtime_error("sink failed");
        }
    }),
                      std::runtime_error);
    REQUIRE(ring.SizeApprox() == 1);

    int output = 0;
    REQUIRE(ring.TryPop(output));
    REQUIRE(output == 3);
    REQUIRE_FALSE(ring.TryPop(output));

    REQUIRE(ring.TryPush(4));
    REQUIRE(ring.TryPush(5));
    int sum = 0;
    REQUIRE(ring.Drain([&sum](int&& value) noexcept { sum += value; }) == 2);
    REQUIRE(sum == 9);
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("SpscRingBuffer Drain consumes one publication snapshot", "[Sora.Core.Concurrency.SpscRingBuffer]") {
    SpscRing<int, 8> ring;
    std::array<int, 2> drained{};
    std::size_t index = 0;
    bool reentrantPushSucceeded = false;

    REQUIRE(ring.TryPush(1));
    REQUIRE(ring.TryPush(2));
    const std::uint32_t count = ring.Drain([&](int&& value) noexcept {
        drained[index++] = value;
        if (value == 1) {
            reentrantPushSucceeded = ring.TryPush(3);
        }
    });

    REQUIRE(count == 2);
    REQUIRE(index == 2);
    REQUIRE(drained == std::array{1, 2});
    REQUIRE(reentrantPushSucceeded);
    REQUIRE(ring.TryPop() == 3);
}

TEST_CASE("SpscRingBuffer transfers a sustained sequence between threads", "[Sora.Core.Concurrency.SpscRingBuffer]") {
    constexpr std::uint32_t kCount = 200'000;
    SpscRing<std::uint32_t, 1024> ring;
    std::atomic<bool> valid = true;

    std::jthread producer([&] {
        for (std::uint32_t value = 0; value < kCount; ++value) {
            ring.Push(value);
        }
    });
    std::jthread consumer([&] {
        for (std::uint32_t expected = 0; expected < kCount; ++expected) {
            if (ring.Pop() != expected) {
                valid.store(false, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();
    REQUIRE(valid.load(std::memory_order_relaxed));
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("SpscRingBuffer layout separates producer and consumer contention domains",
          "[Sora.Core.Concurrency.SpscRingBuffer]") {
    using Ring = SpscRing<std::uint64_t, 64>;

    STATIC_REQUIRE(Ring::kCapacity == 64);
    STATIC_REQUIRE(Sora::Concurrency::Meta::AuditFalseSharing<Ring>());
    STATIC_REQUIRE(Sora::Concurrency::Traits::IsOccupyingWholeLines<Ring>);
}

TEST_CASE("MPSC RingBuffer preserves every producer's FIFO order", "[Sora.Core.Concurrency.RingBuffer]") {
    REQUIRE(TransfersExactlyOnce<Sora::Concurrency::QueueTopologies::Mpsc, 4, 1>());
}

TEST_CASE("SPMC RingBuffer transfers each element to exactly one consumer", "[Sora.Core.Concurrency.RingBuffer]") {
    REQUIRE(TransfersExactlyOnce<Sora::Concurrency::QueueTopologies::Spmc, 1, 4>());
}

TEST_CASE("MPMC RingBuffer transfers each element exactly once", "[Sora.Core.Concurrency.RingBuffer]") {
    REQUIRE(TransfersExactlyOnce<Sora::Concurrency::QueueTopologies::Mpmc, 4, 4>());
}

TEST_CASE("Sequence-cell RingBuffer layouts isolate all contended domains", "[Sora.Core.Concurrency.RingBuffer]") {
    using MpscRing = Sora::Concurrency::RingBuffer<std::uint64_t, 64, Sora::Concurrency::QueueTopologies::Mpsc>;
    using SpmcRing = Sora::Concurrency::RingBuffer<std::uint64_t, 64, Sora::Concurrency::QueueTopologies::Spmc>;
    using MpmcRing = Sora::Concurrency::RingBuffer<std::uint64_t, 64, Sora::Concurrency::QueueTopologies::Mpmc>;

    STATIC_REQUIRE(MpscRing::Layout().valid);
    STATIC_REQUIRE(SpmcRing::Layout().valid);
    STATIC_REQUIRE(MpmcRing::Layout().valid);
    STATIC_REQUIRE(Sora::Concurrency::Traits::IsOccupyingWholeLines<MpscRing>);
    STATIC_REQUIRE(Sora::Concurrency::Traits::IsOccupyingWholeLines<SpmcRing>);
    STATIC_REQUIRE(Sora::Concurrency::Traits::IsOccupyingWholeLines<MpmcRing>);
}
