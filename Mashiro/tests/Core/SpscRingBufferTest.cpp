/**
 * @file SpscRingBufferTest.cpp
 * @brief Comprehensive tests for SpscQueue and SpscByteRing: single-thread
 *        correctness, cross-thread, capacity limits, destruction, edge cases.
 */
#include "Mashiro/Core/SpscRingBuffer.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace Mashiro;

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
// [SpscQueue] — Basic operations
// =============================================================================

TEST_CASE("SpscQueue: push and pop single element", AUTO_TAG) {
    SpscQueue<int, 4> q;
    REQUIRE(q.Empty());
    REQUIRE(q.TryPush(42));
    REQUIRE(!q.Empty());
    REQUIRE(q.SizeApprox() == 1);

    int v = 0;
    REQUIRE(q.TryPop(v));
    REQUIRE(v == 42);
    REQUIRE(q.Empty());
}

TEST_CASE("SpscQueue: fill to capacity", AUTO_TAG) {
    SpscQueue<int, 4> q; // capacity = 4
    REQUIRE(q.TryPush(1));
    REQUIRE(q.TryPush(2));
    REQUIRE(q.TryPush(3));
    REQUIRE(q.TryPush(4));
    REQUIRE(!q.TryPush(5)); // full
    REQUIRE(q.SizeApprox() == 4);
}

TEST_CASE("SpscQueue: pop empties then refill", AUTO_TAG) {
    SpscQueue<int, 4> q;
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

TEST_CASE("SpscQueue: TryPop on empty returns false", AUTO_TAG) {
    SpscQueue<int, 4> q;
    int v = 99;
    REQUIRE(!q.TryPop(v));
    REQUIRE(v == 99); // unchanged
}

TEST_CASE("SpscQueue: TryPop optional on empty returns nullopt", AUTO_TAG) {
    SpscQueue<int, 4> q;
    auto opt = q.TryPop();
    REQUIRE(!opt.has_value());
}

TEST_CASE("SpscQueue: TryPop optional on non-empty", AUTO_TAG) {
    SpscQueue<int, 4> q;
    (void)q.TryPush(77);
    auto opt = q.TryPop();
    REQUIRE(opt.has_value());
    REQUIRE(*opt == 77);
}

TEST_CASE("SpscQueue: TryEmplace", AUTO_TAG) {
    SpscQueue<NonTrivial, 4> q;
    REQUIRE(q.TryEmplace("hello", 5));
    NonTrivial out;
    REQUIRE(q.TryPop(out));
    REQUIRE(out.data == "hello");
    REQUIRE(out.id == 5);
}

// =============================================================================
// [SpscQueue] — Non-trivial types
// =============================================================================

TEST_CASE("SpscQueue: move-only type", AUTO_TAG) {
    SpscQueue<MoveOnly, 4> q;
    REQUIRE(q.TryPush(MoveOnly{42}));
    auto opt = q.TryPop();
    REQUIRE(opt.has_value());
    REQUIRE(opt->value == 42);
}

TEST_CASE("SpscQueue: destructor called on remaining elements", AUTO_TAG) {
    int dtorCount = 0;
    {
        SpscQueue<DtorCounter, 8> q;
        (void)q.TryPush(DtorCounter{&dtorCount});
        (void)q.TryPush(DtorCounter{&dtorCount});
        (void)q.TryPush(DtorCounter{&dtorCount});
        // 3 elements remain when queue destructs
    }
    REQUIRE(dtorCount == 3);
}

TEST_CASE("SpscQueue: destructor not called after pop", AUTO_TAG) {
    int dtorCount = 0;
    {
        SpscQueue<DtorCounter, 8> q;
        (void)q.TryPush(DtorCounter{&dtorCount});
        auto opt = q.TryPop();
        // opt destructs the DtorCounter, but queue should not double-destroy
    }
    REQUIRE(dtorCount == 1); // only from the optional going out of scope
}

// =============================================================================
// [SpscQueue] — Wrap-around stress
// =============================================================================

TEST_CASE("SpscQueue: wrap-around correctness (1000 cycles)", AUTO_TAG) {
    SpscQueue<int, 8> q;
    for (int cycle = 0; cycle < 1000; ++cycle) {
        REQUIRE(q.TryPush(cycle));
        int v;
        REQUIRE(q.TryPop(v));
        REQUIRE(v == cycle);
    }
}

TEST_CASE("SpscQueue: interleaved push/pop maintains FIFO", AUTO_TAG) {
    SpscQueue<int, 16> q;
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
// [SpscQueue] — Cross-thread
// =============================================================================

TEST_CASE("SpscQueue: producer-consumer across threads", AUTO_TAG) {
    constexpr int kCount = 10000;
    SpscQueue<int, 1024> q;
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
// [SpscByteRing] — Basic operations
// =============================================================================

TEST_CASE("SpscByteRing: write and read single message", AUTO_TAG) {
    SpscByteRing<256> ring;
    int payload = 42;
    REQUIRE(ring.TryWrite(&payload, sizeof(payload)));
    REQUIRE(ring.HasData());

    int readBack = 0;
    uint32_t count = ring.ReadAll([&](std::span<const std::byte> data) {
        REQUIRE(data.size() == sizeof(int));
        std::memcpy(&readBack, data.data(), sizeof(int));
    });
    REQUIRE(count == 1);
    REQUIRE(readBack == 42);
    REQUIRE(!ring.HasData());
}

TEST_CASE("SpscByteRing: write multiple messages", AUTO_TAG) {
    SpscByteRing<1024> ring;
    for (int i = 0; i < 10; ++i) {
        REQUIRE(ring.TryWrite(&i, sizeof(i)));
    }

    std::vector<int> results;
    ring.ReadAll([&](std::span<const std::byte> data) {
        int v;
        std::memcpy(&v, data.data(), sizeof(int));
        results.push_back(v);
    });
    REQUIRE(results.size() == 10);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(results[i] == i);
    }
}

TEST_CASE("SpscByteRing: write from span", AUTO_TAG) {
    SpscByteRing<256> ring;
    std::byte payload[] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    REQUIRE(ring.TryWrite(std::span<const std::byte>{payload, 4}));

    ring.ReadAll([](std::span<const std::byte> data) {
        REQUIRE(data.size() == 4);
        REQUIRE(data[0] == std::byte{0xDE});
        REQUIRE(data[3] == std::byte{0xEF});
    });
}

// =============================================================================
// [SpscByteRing] — Capacity and OOM
// =============================================================================

TEST_CASE("SpscByteRing: full ring rejects write", AUTO_TAG) {
    SpscByteRing<64> ring; // 64 bytes total
    // Each write = 4 (header) + 28 payload bytes. Two writes fill the ring.
    char payload[28] = {};
    REQUIRE(ring.TryWrite(payload, sizeof(payload)));
    REQUIRE(ring.TryWrite(payload, sizeof(payload)));
    REQUIRE(!ring.TryWrite(payload, sizeof(payload)));
}

TEST_CASE("SpscByteRing: zero-length message", AUTO_TAG) {
    SpscByteRing<256> ring;
    REQUIRE(!ring.TryWrite(nullptr, 0));
    REQUIRE(!ring.HasData());
}

TEST_CASE("SpscByteRing: null non-empty write is rejected", AUTO_TAG) {
    SpscByteRing<256> ring;
    REQUIRE(!ring.TryWrite(nullptr, 4));
    REQUIRE(!ring.HasData());
}

TEST_CASE("SpscByteRing: oversized message is rejected before staging overflow", AUTO_TAG) {
    SpscByteRing<65536> ring;
    std::vector<std::byte> maxPayload(8192, std::byte{0x7F});
    std::vector<std::byte> oversized(8193, std::byte{0x7F});

    REQUIRE(ring.TryWrite(maxPayload.data(), static_cast<uint32_t>(maxPayload.size())));
    REQUIRE(!ring.TryWrite(oversized.data(), static_cast<uint32_t>(oversized.size())));
}

// =============================================================================
// [SpscByteRing] — Wrap-around
// =============================================================================

TEST_CASE("SpscByteRing: wrap-around correctness", AUTO_TAG) {
    SpscByteRing<128> ring;

    for (int cycle = 0; cycle < 100; ++cycle) {
        int payload = cycle;
        REQUIRE(ring.TryWrite(&payload, sizeof(payload)));

        int readBack = -1;
        ring.ReadAll([&](std::span<const std::byte> data) {
            std::memcpy(&readBack, data.data(), sizeof(int));
        });
        REQUIRE(readBack == cycle);
    }
}

// =============================================================================
// [SpscByteRing] — Variable-length messages
// =============================================================================

TEST_CASE("SpscByteRing: mixed message sizes", AUTO_TAG) {
    SpscByteRing<4096> ring;

    // Write messages of varying sizes
    for (int i = 1; i <= 20; ++i) {
        std::vector<std::byte> msg(i * 4, std::byte{static_cast<unsigned char>(i)});
        REQUIRE(ring.TryWrite(msg.data(), static_cast<uint32_t>(msg.size())));
    }

    int msgIndex = 1;
    ring.ReadAll([&](std::span<const std::byte> data) {
        REQUIRE(data.size() == static_cast<size_t>(msgIndex * 4));
        for (auto b : data) {
            REQUIRE(b == std::byte{static_cast<unsigned char>(msgIndex)});
        }
        ++msgIndex;
    });
    REQUIRE(msgIndex == 21); // read all 20 messages
}

// =============================================================================
// [SpscByteRing] — Reset
// =============================================================================

TEST_CASE("SpscByteRing: Reset discards pending data", AUTO_TAG) {
    SpscByteRing<256> ring;
    int v = 1;
    (void)ring.TryWrite(&v, sizeof(v));
    (void)ring.TryWrite(&v, sizeof(v));
    REQUIRE(ring.HasData());
    ring.Reset();
    REQUIRE(!ring.HasData());
    REQUIRE(ring.BytesPending() == 0);
}

// =============================================================================
// [SpscByteRing] — Cross-thread
// =============================================================================

TEST_CASE("SpscByteRing: producer-consumer across threads", AUTO_TAG) {
    constexpr int kCount = 5000;
    SpscByteRing<65536> ring;

    std::atomic<bool> done{false};
    std::vector<int> received;
    received.reserve(kCount);

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            while (!ring.TryWrite(&i, sizeof(i))) {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (!done.load(std::memory_order_acquire) || ring.HasData()) {
            ring.ReadAll([&](std::span<const std::byte> data) {
                int v;
                std::memcpy(&v, data.data(), sizeof(int));
                received.push_back(v);
            });
            if (received.size() < kCount) {
                std::this_thread::yield();
            }
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
// [SpscByteRing] — BytesPending
// =============================================================================

TEST_CASE("SpscByteRing: BytesPending tracks usage", AUTO_TAG) {
    SpscByteRing<256> ring;
    REQUIRE(ring.BytesPending() == 0);

    int v = 1;
    (void)ring.TryWrite(&v, sizeof(v)); // 4 header + 4 payload = 8 bytes
    REQUIRE(ring.BytesPending() == 8);

    (void)ring.TryWrite(&v, sizeof(v)); // another 8
    REQUIRE(ring.BytesPending() == 16);

    ring.ReadAll([](std::span<const std::byte>) {});
    REQUIRE(ring.BytesPending() == 0);
}

// =============================================================================
// [SpscQueue] — GetCapacity
// =============================================================================

TEST_CASE("SpscQueue: GetCapacity is compile-time", AUTO_TAG) {
    STATIC_REQUIRE(SpscQueue<int, 256>::GetCapacity() == 256);
    STATIC_REQUIRE(SpscByteRing<4096>::GetCapacity() == 4096);
}
