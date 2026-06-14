/**
 * @file MpscQueueTest.cpp
 * @brief Tests for Core/MpscQueue.h — bounded, lock-free MPSC FIFO queue.
 *
 * Coverage:
 * - Empty / full / wrap behaviour on a single thread.
 * - FIFO ordering preserved through @ref MpscQueue::Drain.
 * - Move-only payloads (placement-new + destroy semantics on the cell).
 * - Multi-producer correctness: every producer's contributions are observed exactly once,
 *   in producer-local order (the standard MPSC guarantee — global order is interleaved).
 * - Sustained drain under producer pressure: no events are duplicated or lost when the
 *   ring never fills.
 * - Compile-time @ref Concurrency::AnalyzeCacheLayout report passes the false-sharing
 *   audit for representative instantiations.
 */
#include "Mashiro/Core/MpscQueue.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace Mashiro;

// =============================================================================
// Section 1 — Compile-time guarantees
// =============================================================================

TEST_CASE("MpscQueue layout passes the false-sharing audit", AUTO_TAG) {
    using Q = MpscQueue<std::uint64_t, 256>;
    constexpr auto layout = Q::Layout();
    STATIC_REQUIRE(layout.valid);
    STATIC_REQUIRE_FALSE(layout.hasConflict);
    STATIC_REQUIRE(layout.classifiedAll);
}

TEST_CASE("MpscQueue capacity is exposed at compile time", AUTO_TAG) {
    using Q = MpscQueue<int, 64>;
    STATIC_REQUIRE(Q::kCapacity == 64);
}

// =============================================================================
// Section 2 — Single-threaded behaviour
// =============================================================================

TEST_CASE("Empty queue reports empty and yields nullopt on pop", AUTO_TAG) {
    MpscQueue<int, 8> q;
    REQUIRE(q.Empty());
    REQUIRE(q.ApproxSize() == 0);
    REQUIRE_FALSE(q.TryPop().has_value());
}

TEST_CASE("Push/pop preserves FIFO order on a single thread", AUTO_TAG) {
    MpscQueue<int, 8> q;
    REQUIRE(q.TryPush(1));
    REQUIRE(q.TryPush(2));
    REQUIRE(q.TryPush(3));
    REQUIRE(q.ApproxSize() == 3);

    auto a = q.TryPop();
    auto b = q.TryPop();
    auto c = q.TryPop();
    auto d = q.TryPop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE_FALSE(d.has_value());
    REQUIRE(*a == 1);
    REQUIRE(*b == 2);
    REQUIRE(*c == 3);
    REQUIRE(q.Empty());
}

TEST_CASE("Ring rejects a push when full and accepts again after drain", AUTO_TAG) {
    MpscQueue<int, 4> q;
    REQUIRE(q.TryPush(10));
    REQUIRE(q.TryPush(20));
    REQUIRE(q.TryPush(30));
    REQUIRE(q.TryPush(40));
    REQUIRE_FALSE(q.TryPush(50));    // ring is full

    REQUIRE(q.TryPop().value() == 10);
    REQUIRE(q.TryPush(50));          // one slot freed -> push succeeds
    REQUIRE(q.TryPop().value() == 20);
    REQUIRE(q.TryPop().value() == 30);
    REQUIRE(q.TryPop().value() == 40);
    REQUIRE(q.TryPop().value() == 50);
    REQUIRE(q.Empty());
}

TEST_CASE("Wrap-around: many push/pop cycles do not desync the cells", AUTO_TAG) {
    // Capacity 4 — drives several full laps of the ring.
    MpscQueue<int, 4> q;
    for (int round = 0; round < 16; ++round) {
        for (int i = 0; i < 4; ++i) REQUIRE(q.TryPush(round * 10 + i));
        for (int i = 0; i < 4; ++i) REQUIRE(q.TryPop().value() == round * 10 + i);
        REQUIRE(q.Empty());
    }
}

// =============================================================================
// Section 3 — Move-only payloads
// =============================================================================

TEST_CASE("Move-only payloads transit the ring without copying", AUTO_TAG) {
    using UPtr = std::unique_ptr<int>;
    MpscQueue<UPtr, 8> q;

    REQUIRE(q.TryPush(std::make_unique<int>(42)));
    REQUIRE(q.TryPush(std::make_unique<int>(7)));

    auto a = q.TryPop();
    auto b = q.TryPop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(**a == 42);
    REQUIRE(**b == 7);
    REQUIRE(q.Empty());
}

TEST_CASE("Drain consumes every element and returns the count", AUTO_TAG) {
    MpscQueue<int, 16> q;
    for (int i = 0; i < 10; ++i) REQUIRE(q.TryPush(i));

    std::vector<int> drained;
    const size_t n = q.Drain([&](int v) { drained.push_back(v); });
    REQUIRE(n == 10);
    REQUIRE(drained.size() == 10);
    for (int i = 0; i < 10; ++i) REQUIRE(drained[i] == i);
    REQUIRE(q.Empty());
}

// =============================================================================
// Section 4 — Multi-producer correctness
// =============================================================================

TEST_CASE("Many producers, one consumer: every element observed exactly once", AUTO_TAG) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 1024;
    MpscQueue<int, 4096> q;

    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kPerProducer; ++i) {
                const int payload = p * kPerProducer + i;
                while (!q.TryPush(payload)) {
                    std::this_thread::yield();          // ring full -> back off and retry
                }
            }
        });
    }

    std::vector<int> drained;
    drained.reserve(kProducers * kPerProducer);
    std::thread consumer([&]() {
        while (start.load(std::memory_order_acquire) == false) {}
        while (drained.size() < static_cast<size_t>(kProducers * kPerProducer)) {
            q.Drain([&](int v) { drained.push_back(v); });
            std::this_thread::yield();
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();

    REQUIRE(drained.size() == static_cast<size_t>(kProducers * kPerProducer));
    // Every payload occurs exactly once across all producers.
    std::set<int> unique(drained.begin(), drained.end());
    REQUIRE(unique.size() == drained.size());
    REQUIRE(*unique.begin() == 0);
    REQUIRE(*unique.rbegin() == kProducers * kPerProducer - 1);
}

TEST_CASE("Per-producer FIFO is preserved under contention", AUTO_TAG) {
    // The MPSC contract guarantees per-producer ordering: payloads pushed by producer P
    // arrive at the consumer in the order P pushed them. Inter-producer order is
    // arbitrary. We encode the producer id and the per-producer sequence into one int and
    // reconstruct each producer's stream from the drained log.
    constexpr int kProducers = 3;
    constexpr int kPerProducer = 2000;
    MpscQueue<int, 1024> q;

    auto encode = [](int producer, int seq) { return producer * 100000 + seq; };

    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kPerProducer; ++i) {
                while (!q.TryPush(encode(p, i))) std::this_thread::yield();
            }
        });
    }

    std::vector<int> drained;
    drained.reserve(kProducers * kPerProducer);
    std::thread consumer([&]() {
        while (drained.size() < static_cast<size_t>(kProducers * kPerProducer)) {
            q.Drain([&](int v) { drained.push_back(v); });
            std::this_thread::yield();
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();

    // Verify per-producer order: extract the sub-sequence produced by each producer p,
    // it must be 0,1,2,... kPerProducer-1.
    for (int p = 0; p < kProducers; ++p) {
        int expected = 0;
        for (int payload : drained) {
            if (payload / 100000 == p) {
                REQUIRE(payload % 100000 == expected);
                ++expected;
            }
        }
        REQUIRE(expected == kPerProducer);
    }
}

// =============================================================================
// Section 5 — Sustained pressure with a bounded ring
// =============================================================================

TEST_CASE("Producers spin on TryPush when the ring is full; nothing lost", AUTO_TAG) {
    // Capacity 16 vs. 4 producers each pushing 1024 -> the ring is in steady-state
    // back-pressure for most of the run; the consumer must never miss an element.
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 1024;
    MpscQueue<int, 16> q;

    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kPerProducer; ++i) {
                while (!q.TryPush(p * kPerProducer + i)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::atomic<int> drainedCount{0};
    std::vector<int> drained;
    drained.reserve(kProducers * kPerProducer);
    std::thread consumer([&]() {
        while (drainedCount.load(std::memory_order_relaxed) < kProducers * kPerProducer) {
            q.Drain([&](int v) {
                drained.push_back(v);
                drainedCount.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();

    REQUIRE(drained.size() == static_cast<size_t>(kProducers * kPerProducer));
    std::set<int> unique(drained.begin(), drained.end());
    REQUIRE(unique.size() == drained.size());
}
