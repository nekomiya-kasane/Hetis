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

// =============================================================================
// [SpscQueue] — Zero-copy Front / PopFront
// =============================================================================

TEST_CASE("SpscQueue: Front on empty returns nullptr", AUTO_TAG) {
    SpscQueue<int, 4> q;
    REQUIRE(q.Front() == nullptr);
}

TEST_CASE("SpscQueue: Front peeks without consuming, PopFront consumes", AUTO_TAG) {
    SpscQueue<int, 4> q;
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

TEST_CASE("SpscQueue: Front allows in-place mutation before PopFront", AUTO_TAG) {
    SpscQueue<NonTrivial, 4> q;
    REQUIRE(q.TryEmplace("abc", 1));

    NonTrivial* front = q.Front();
    REQUIRE(front != nullptr);
    front->id = 99;

    NonTrivial out;
    REQUIRE(q.TryPop(out));
    REQUIRE(out.id == 99);
    REQUIRE(out.data == "abc");
}

TEST_CASE("SpscQueue: PopFront destroys the element exactly once", AUTO_TAG) {
    int dtors = 0;
    {
        SpscQueue<DtorCounter, 8> q;
        REQUIRE(q.TryEmplace(&dtors));
        q.PopFront();
        REQUIRE(dtors == 1);
    }
    REQUIRE(dtors == 1); // queue destructor must not double-destroy
}

TEST_CASE("SpscQueue: Front/PopFront interleaved with wrap-around", AUTO_TAG) {
    SpscQueue<int, 4> q;
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
// [SpscQueue] — Diagnostics never exceed capacity
// =============================================================================

TEST_CASE("SpscQueue: SizeApprox is bounded by capacity when full", AUTO_TAG) {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(q.TryPush(i));
    }
    REQUIRE(q.SizeApprox() == 4);
    REQUIRE(q.SizeApprox() <= q.GetCapacity());
}

// =============================================================================
// [SpscQueue] — noexcept contracts
// =============================================================================

TEST_CASE("SpscQueue: noexcept propagation matches element type", AUTO_TAG) {
    SpscQueue<int, 4> qi;
    STATIC_REQUIRE(noexcept(qi.TryPush(1)));
    STATIC_REQUIRE(noexcept(qi.TryEmplace(1)));
    int v = 0;
    STATIC_REQUIRE(noexcept(qi.TryPop(v)));
    STATIC_REQUIRE(noexcept(qi.Front()));
    STATIC_REQUIRE(noexcept(qi.PopFront()));
    STATIC_REQUIRE(noexcept(qi.SizeApprox()));
}

// =============================================================================
// [SpscByteRing] — ReadAll snapshot & reentrancy semantics
// =============================================================================

TEST_CASE("SpscByteRing: ReadAll returns the number of messages consumed", AUTO_TAG) {
    SpscByteRing<256> ring;
    int v = 7;
    REQUIRE(ring.TryWrite(&v, sizeof(v)));
    REQUIRE(ring.TryWrite(&v, sizeof(v)));
    REQUIRE(ring.TryWrite(&v, sizeof(v)));

    const uint32_t count = ring.ReadAll([](std::span<const std::byte>) {});
    REQUIRE(count == 3);
    REQUIRE(ring.ReadAll([](std::span<const std::byte>) {}) == 0);
}

TEST_CASE("SpscByteRing: messages written during ReadAll are deferred", AUTO_TAG) {
    SpscByteRing<256> ring;
    int v = 1;
    REQUIRE(ring.TryWrite(&v, sizeof(v)));

    uint32_t innerWrites = 0;
    const uint32_t first = ring.ReadAll([&](std::span<const std::byte>) {
        int next = 2;
        REQUIRE(ring.TryWrite(&next, sizeof(next))); // reentrant producer call
        ++innerWrites;
    });
    REQUIRE(first == 1);
    REQUIRE(innerWrites == 1);

    // The reentrant write must arrive in the *next* batch, exactly once.
    int seen = 0;
    const uint32_t second = ring.ReadAll([&](std::span<const std::byte> data) {
        std::memcpy(&seen, data.data(), sizeof(seen));
    });
    REQUIRE(second == 1);
    REQUIRE(seen == 2);
    REQUIRE(!ring.HasData());
}

TEST_CASE("SpscByteRing: throwing callback consumes the poison message (at-most-once)",
          AUTO_TAG) {
    struct Poison {};
    SpscByteRing<256> ring;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(ring.TryWrite(&i, sizeof(i)));
    }

    int calls = 0;
    REQUIRE_THROWS_AS(ring.ReadAll([&](std::span<const std::byte>) {
                          ++calls;
                          throw Poison{};
                      }),
                      Poison);
    REQUIRE(calls == 1);

    // The poison message (0) is consumed; 1 and 2 must still be delivered once.
    std::vector<int> rest;
    ring.ReadAll([&](std::span<const std::byte> data) {
        int v;
        std::memcpy(&v, data.data(), sizeof(v));
        rest.push_back(v);
    });
    REQUIRE(rest == std::vector<int>{1, 2});
    REQUIRE(!ring.HasData());

    // The ring must remain fully functional after unwinding.
    int v = 42;
    REQUIRE(ring.TryWrite(&v, sizeof(v)));
    int seen = 0;
    REQUIRE(ring.ReadAll([&](std::span<const std::byte> data) {
                std::memcpy(&seen, data.data(), sizeof(seen));
            }) == 1);
    REQUIRE(seen == 42);
}

// =============================================================================
// [SpscByteRing] — Zero-copy vs staging paths
// =============================================================================

TEST_CASE("SpscByteRing: wrapping payloads are reassembled correctly", AUTO_TAG) {
    // Capacity 128 → max message 64. Force payloads to straddle the boundary.
    SpscByteRing<128> ring;
    uint8_t seq = 0;
    for (int round = 0; round < 64; ++round) {
        std::vector<std::byte> payload(48);
        for (auto& b : payload) {
            b = static_cast<std::byte>(seq++);
        }
        REQUIRE(ring.TryWrite(std::span<const std::byte>{payload}));

        uint32_t reads = ring.ReadAll([&](std::span<const std::byte> data) {
            REQUIRE(data.size() == payload.size());
            REQUIRE(std::memcmp(data.data(), payload.data(), payload.size()) == 0);
        });
        REQUIRE(reads == 1);
    }
}

TEST_CASE("SpscByteRing: max-size message round-trips", AUTO_TAG) {
    SpscByteRing<256> ring;
    constexpr uint32_t kMax = SpscByteRing<256>::GetMaxMessageSize();
    std::vector<std::byte> payload(kMax);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i * 7 + 3);
    }
    REQUIRE(ring.TryWrite(std::span<const std::byte>{payload}));

    bool verified = false;
    ring.ReadAll([&](std::span<const std::byte> data) {
        REQUIRE(data.size() == kMax);
        REQUIRE(std::memcmp(data.data(), payload.data(), kMax) == 0);
        verified = true;
    });
    REQUIRE(verified);

    // One byte above the limit must be rejected.
    std::vector<std::byte> tooBig(kMax + 1);
    REQUIRE(!ring.TryWrite(std::span<const std::byte>{tooBig}));
}

// =============================================================================
// [SpscByteRing] — Cross-thread stress with mixed sizes (wrap + staging paths)
// =============================================================================

TEST_CASE("SpscByteRing: cross-thread stress with variable-size messages", AUTO_TAG) {
    constexpr int kCount = 4000;
    SpscByteRing<1024> ring; // small ring → frequent wrap-around

    std::atomic<bool> done{false};
    std::atomic<int> validated{0};

    std::thread producer([&] {
        std::vector<std::byte> payload;
        for (int i = 0; i < kCount; ++i) {
            const uint32_t size = 1 + static_cast<uint32_t>(i * 37 % 200);
            payload.assign(size, static_cast<std::byte>(i & 0xFF));
            while (!ring.TryWrite(std::span<const std::byte>{payload})) {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        int expected = 0;
        while (!done.load(std::memory_order_acquire) || ring.HasData()) {
            ring.ReadAll([&](std::span<const std::byte> data) {
                const uint32_t size = 1 + static_cast<uint32_t>(expected * 37 % 200);
                REQUIRE(data.size() == size);
                const auto fill = static_cast<std::byte>(expected & 0xFF);
                for (std::byte b : data) {
                    REQUIRE(b == fill);
                }
                ++expected;
                validated.fetch_add(1, std::memory_order_relaxed);
            });
            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    REQUIRE(validated.load() == kCount);
}

// =============================================================================
// Compile-time contracts (C++26 reflection audit + consteval limits)
// =============================================================================

TEST_CASE("SpscByteRing: GetMaxMessageSize is computed at compile time", AUTO_TAG) {
    STATIC_REQUIRE(SpscByteRing<128>::GetMaxMessageSize() == 64);     // Capacity/2 wins
    STATIC_REQUIRE(SpscByteRing<256>::GetMaxMessageSize() == 128);    // Capacity/2 wins
    STATIC_REQUIRE(SpscByteRing<16384>::GetMaxMessageSize() == 8192); // 8 KB cap wins
    STATIC_REQUIRE(SpscByteRing<65536>::GetMaxMessageSize() == 8192); // 8 KB cap wins
}

TEST_CASE("Concurrency: reflection audit proves no cross-role false sharing", AUTO_TAG) {
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<SpscQueue<int, 4>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<SpscQueue<MoveOnly, 64>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<SpscByteRing<128>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<SpscByteRing<64 * 1024>>());
}
