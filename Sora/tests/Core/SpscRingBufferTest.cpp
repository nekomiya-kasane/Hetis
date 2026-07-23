#include <Sora/Core/Concurrency/SpscRingBuffer.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace {

    struct TrackedValue {
        inline static int liveCount = 0;

        int value = 0;

        explicit TrackedValue(int initial) noexcept : value(initial) { ++liveCount; }
        TrackedValue(TrackedValue&& other) noexcept : value(other.value) { ++liveCount; }
        TrackedValue& operator=(TrackedValue&& other) noexcept {
            value = other.value;
            return *this;
        }
        ~TrackedValue() noexcept { --liveCount; }

        TrackedValue(const TrackedValue&) = delete;
        TrackedValue& operator=(const TrackedValue&) = delete;
    };

} // namespace

TEST_CASE("SpscRingBuffer preserves bounded FIFO semantics", "[Sora.Core.Concurrency.SpscRingBuffer]") {
    Sora::Concurrency::SpscRingBuffer<int, 4> ring;

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
    Sora::Concurrency::SpscRingBuffer<std::uint32_t, 8> ring;

    for (std::uint32_t cycle = 0; cycle < 1'000; ++cycle) {
        for (std::uint32_t offset = 0; offset < ring.kCapacity; ++offset) {
            REQUIRE(ring.TryPush(cycle * ring.kCapacity + offset));
        }
        for (std::uint32_t offset = 0; offset < ring.kCapacity; ++offset) {
            REQUIRE(ring.TryPop() == cycle * ring.kCapacity + offset);
        }
    }
}

TEST_CASE("SpscRingBuffer destroys exactly the elements that remain queued",
          "[Sora.Core.Concurrency.SpscRingBuffer]") {
    REQUIRE(TrackedValue::liveCount == 0);
    {
        Sora::Concurrency::SpscRingBuffer<TrackedValue, 8> empty;
    }
    REQUIRE(TrackedValue::liveCount == 0);

    {
        Sora::Concurrency::SpscRingBuffer<TrackedValue, 8> ring;
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

TEST_CASE("SpscRingBuffer Drain consumes one publication snapshot",
          "[Sora.Core.Concurrency.SpscRingBuffer]") {
    Sora::Concurrency::SpscRingBuffer<int, 8> ring;
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

TEST_CASE("SpscRingBuffer transfers a sustained sequence between threads",
          "[Sora.Core.Concurrency.SpscRingBuffer]") {
    constexpr std::uint32_t kCount = 200'000;
    Sora::Concurrency::SpscRingBuffer<std::uint32_t, 1024> ring;
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
    using Ring = Sora::Concurrency::SpscRingBuffer<std::uint64_t, 64>;

    STATIC_REQUIRE(Ring::kCapacity == 64);
    STATIC_REQUIRE(Sora::Concurrency::Meta::AuditFalseSharing<Ring>());
    STATIC_REQUIRE(Sora::Concurrency::Traits::IsOccupyingWholeLines<Ring>);
}
