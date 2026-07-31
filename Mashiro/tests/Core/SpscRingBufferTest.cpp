/**
 * @file SpscRingBufferTest.cpp
 * @brief Correctness, lifetime, wraparound, and cross-thread tests for the SPSC object ring.
 *        correctness, cross-thread, capacity limits, destruction, edge cases.
 */
#include "Mashiro/Core/RingBuffer.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace Mashiro;

template<class T, std::size_t Capacity>
using SpscRing = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Spsc>;

// =============================================================================
// Helper types
// =============================================================================

namespace {

    struct NonTrivial {
        std::string data;
        int id = 0;
        NonTrivial() = default;
        NonTrivial(std::string d, int i) : data(std::move(d)), id(i) {}
    };

    struct MoveOnly {
        int value = 0;
        MoveOnly() = default;
        explicit MoveOnly(int v) : value(v) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&& o) noexcept : value(o.value) { o.value = -1; }
        MoveOnly& operator=(MoveOnly&& o) noexcept {
            value = o.value;
            o.value = -1;
            return *this;
        }
    };

    struct DtorCounter {
        int* counter;
        explicit DtorCounter(int* c) : counter(c) {}
        ~DtorCounter() {
            if (counter) {
                ++(*counter);
            }
        }
        DtorCounter(DtorCounter&& o) noexcept : counter(o.counter) { o.counter = nullptr; }
        DtorCounter& operator=(DtorCounter&& o) noexcept {
            counter = o.counter;
            o.counter = nullptr;
            return *this;
        }
    };

} // anonymous namespace

// =============================================================================
// [SpscRingBuffer] â€” Basic operations
// =============================================================================

TEST_CASE("SpscRingBuffer: push and pop single element", AUTO_TAG) {
    SpscRing<int, 4> q;
    REQUIRE(q.Empty());
    REQUIRE(q.TryPush(42));
    REQUIRE(!q.Empty());
    REQUIRE(q.SizeApprox() == 1);

    int v = 0;
    REQUIRE(q.TryPop(v));
    REQUIRE(v == 42);
    REQUIRE(q.Empty());
}

TEST_CASE("SpscRingBuffer: fill to capacity", AUTO_TAG) {
    SpscRing<int, 4> q; // capacity = 4
    REQUIRE(q.TryPush(1));
    REQUIRE(q.TryPush(2));
    REQUIRE(q.TryPush(3));
    REQUIRE(q.TryPush(4));
    REQUIRE(!q.TryPush(5)); // full
    REQUIRE(q.SizeApprox() == 4);
}

TEST_CASE("SpscRingBuffer: pop empties then refill", AUTO_TAG) {
    SpscRing<int, 4> q;
    for (int i = 0; i < 4; ++i) {
        (void)q.TryPush(i);
    }

    int v;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(q.TryPop(v));
        REQUIRE(v == i);
    }
    REQUIRE(q.Empty());

    // Refill (tests wrap-around)
    for (int i = 10; i < 14; ++i) {
        REQUIRE(q.TryPush(i));
    }
    for (int i = 10; i < 14; ++i) {
        REQUIRE(q.TryPop(v));
        REQUIRE(v == i);
    }
}

TEST_CASE("SpscRingBuffer: TryPop on empty returns false", AUTO_TAG) {
    SpscRing<int, 4> q;
    int v = 99;
    REQUIRE(!q.TryPop(v));
    REQUIRE(v == 99); // unchanged
}

TEST_CASE("SpscRingBuffer: TryPop optional on empty returns nullopt", AUTO_TAG) {
    SpscRing<int, 4> q;
    auto opt = q.TryPop();
    REQUIRE(!opt.has_value());
}

TEST_CASE("SpscRingBuffer: TryPop optional on non-empty", AUTO_TAG) {
    SpscRing<int, 4> q;
    (void)q.TryPush(77);
    auto opt = q.TryPop();
    REQUIRE(opt.has_value());
    REQUIRE(*opt == 77);
}

TEST_CASE("SpscRingBuffer: TryEmplace", AUTO_TAG) {
    SpscRing<NonTrivial, 4> q;
    REQUIRE(q.TryEmplace("hello", 5));
    NonTrivial out;
    REQUIRE(q.TryPop(out));
    REQUIRE(out.data == "hello");
    REQUIRE(out.id == 5);
}

// =============================================================================
// [SpscRingBuffer] â€” Non-trivial types
// =============================================================================

TEST_CASE("SpscRingBuffer: move-only type", AUTO_TAG) {
    SpscRing<MoveOnly, 4> q;
    REQUIRE(q.TryPush(MoveOnly{42}));
    auto opt = q.TryPop();
    REQUIRE(opt.has_value());
    REQUIRE(opt->value == 42);
}

TEST_CASE("SpscRingBuffer: destructor called on remaining elements", AUTO_TAG) {
    int dtorCount = 0;
    {
        SpscRing<DtorCounter, 8> q;
        (void)q.TryPush(DtorCounter{&dtorCount});
        (void)q.TryPush(DtorCounter{&dtorCount});
        (void)q.TryPush(DtorCounter{&dtorCount});
        // 3 elements remain when queue destructs
    }
    REQUIRE(dtorCount == 3);
}

TEST_CASE("SpscRingBuffer: destructor not called after pop", AUTO_TAG) {
    int dtorCount = 0;
    {
        SpscRing<DtorCounter, 8> q;
        (void)q.TryPush(DtorCounter{&dtorCount});
        auto opt = q.TryPop();
        // opt destructs the DtorCounter, but queue should not double-destroy
    }
    REQUIRE(dtorCount == 1); // only from the optional going out of scope
}

// =============================================================================
TEST_CASE("SpscRingBuffer: Drain consumes one FIFO snapshot and releases slots", AUTO_TAG) {
    SpscRing<int, 4> q;
    REQUIRE(q.TryPush(10));
    REQUIRE(q.TryPush(20));
    REQUIRE(q.TryPush(30));

    std::vector<int> drained;
    const uint32_t count = q.Drain([&drained](int&& value) noexcept { drained.push_back(value); });

    REQUIRE(count == 3);
    REQUIRE(drained == std::vector<int>{10, 20, 30});
    REQUIRE(q.Empty());
    REQUIRE(q.TryPush(40));
    REQUIRE(q.TryPop().value() == 40);
}

// [SpscRingBuffer] â€” Wrap-around stress
// =============================================================================

TEST_CASE("SpscRingBuffer: wrap-around correctness (1000 cycles)", AUTO_TAG) {
    SpscRing<int, 8> q;
    for (int cycle = 0; cycle < 1000; ++cycle) {
        REQUIRE(q.TryPush(cycle));
        int v;
        REQUIRE(q.TryPop(v));
        REQUIRE(v == cycle);
    }
}

TEST_CASE("SpscRingBuffer: interleaved push/pop maintains FIFO", AUTO_TAG) {
    SpscRing<int, 16> q;
    std::vector<int> results;

    for (int i = 0; i < 100; ++i) {
        (void)q.TryPush(i);
        if (i % 3 == 0) {
            int v;
            if (q.TryPop(v)) {
                results.push_back(v);
            }
        }
    }
    // Drain remaining
    int v;
    while (q.TryPop(v)) {
        results.push_back(v);
    }

    // All should be in order
    for (size_t i = 1; i < results.size(); ++i) {
        REQUIRE(results[i] > results[i - 1]);
    }
}

// =============================================================================
// [SpscRingBuffer] â€” Cross-thread
// =============================================================================

TEST_CASE("SpscRingBuffer: producer-consumer across threads", AUTO_TAG) {
    constexpr int kCount = 10000;
    SpscRing<int, 1024> q;
    std::vector<int> received;
    received.reserve(kCount);

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            q.Push(i);
        }
    });

    std::thread consumer([&] {
        for (int i = 0; i < kCount; ++i) {
            received.push_back(q.Pop());
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(received.size() == kCount);
    for (int i = 0; i < kCount; ++i) {
        REQUIRE(received[i] == i);
    }
}

// =============================================================================
// [SpscRingBuffer] â€” GetCapacity
// =============================================================================

TEST_CASE("SpscRingBuffer: GetCapacity is compile-time", AUTO_TAG) {
    STATIC_REQUIRE(SpscRing<int, 256>::GetCapacity() == 256);
}

// =============================================================================
// [SpscRingBuffer] â€” Zero-copy Front / PopFront
// =============================================================================

TEST_CASE("SpscRingBuffer: Front on empty returns nullptr", AUTO_TAG) {
    SpscRing<int, 4> q;
    REQUIRE(q.Front() == nullptr);
}

TEST_CASE("SpscRingBuffer: Front peeks without consuming, PopFront consumes", AUTO_TAG) {
    SpscRing<int, 4> q;
    REQUIRE(q.TryPush(10));
    REQUIRE(q.TryPush(20));

    int* front = q.Front();
    REQUIRE(front != nullptr);
    REQUIRE(*front == 10);
    REQUIRE(q.SizeApprox() == 2); // peek must not consume

    q.PopFront();
    front = q.Front();
    REQUIRE(front != nullptr);
    REQUIRE(*front == 20);

    q.PopFront();
    REQUIRE(q.Front() == nullptr);
    REQUIRE(q.Empty());
}

TEST_CASE("SpscRingBuffer: Front allows in-place mutation before PopFront", AUTO_TAG) {
    SpscRing<NonTrivial, 4> q;
    REQUIRE(q.TryEmplace("abc", 1));

    NonTrivial* front = q.Front();
    REQUIRE(front != nullptr);
    front->id = 99;

    NonTrivial out;
    REQUIRE(q.TryPop(out));
    REQUIRE(out.id == 99);
    REQUIRE(out.data == "abc");
}

TEST_CASE("SpscRingBuffer: PopFront destroys the element exactly once", AUTO_TAG) {
    int dtors = 0;
    {
        SpscRing<DtorCounter, 8> q;
        REQUIRE(q.TryEmplace(&dtors));
        REQUIRE(q.Front() != nullptr);
        q.PopFront();
        REQUIRE(dtors == 1);
    }
    REQUIRE(dtors == 1); // queue destructor must not double-destroy
}

TEST_CASE("SpscRingBuffer: Front/PopFront interleaved with wrap-around", AUTO_TAG) {
    SpscRing<int, 4> q;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(q.TryPush(i));
        int* front = q.Front();
        REQUIRE(front != nullptr);
        REQUIRE(*front == i);
        q.PopFront();
    }
    REQUIRE(q.Empty());
}

// =============================================================================
// [SpscRingBuffer] â€” Diagnostics never exceed capacity
// =============================================================================

TEST_CASE("SpscRingBuffer: SizeApprox is bounded by capacity when full", AUTO_TAG) {
    SpscRing<int, 4> q;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(q.TryPush(i));
    }
    REQUIRE(q.SizeApprox() == 4);
    REQUIRE(q.SizeApprox() <= q.GetCapacity());
}

// =============================================================================
// [SpscRingBuffer] â€” noexcept contracts
// =============================================================================

TEST_CASE("SpscRingBuffer: noexcept propagation matches element type", AUTO_TAG) {
    SpscRing<int, 4> qi;
    STATIC_REQUIRE(noexcept(qi.TryPush(1)));
    STATIC_REQUIRE(noexcept(qi.TryEmplace(1)));
    int v = 0;
    STATIC_REQUIRE(noexcept(qi.TryPop(v)));
    STATIC_REQUIRE(noexcept(qi.Front()));
    STATIC_REQUIRE(noexcept(qi.PopFront()));
    STATIC_REQUIRE(noexcept(qi.SizeApprox()));
}

// =============================================================================
TEST_CASE("Concurrency: reflection audit proves no cross-role false sharing", AUTO_TAG) {
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<SpscRing<int, 4>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<SpscRing<MoveOnly, 64>>());
}
