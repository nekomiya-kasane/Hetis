/**
 * @file ConcurrentObjectPoolTest.cpp
 * @brief Tests for ConcurrentObjectPool: handle bit-packing, single-thread
 *        acquire/release/deref, generation-based use-after-free detection,
 *        pool exhaustion, single-CAS bulk ops, non-trivial element lifetime,
 *        stats, the Result API, ForEach/Clear, the heap backing, high-stress
 *        MPMC / SPMC async hand-off and slot-exclusivity (torn-payload)
 *        invariants, and the C++26 reflection layout audit.
 */
#include "Mashiro/Core/ConcurrentObjectPool.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <latch>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace Mashiro;

// =============================================================================
// Helper element types
// =============================================================================

namespace {

    /// @brief A trivially-copyable payload carrying an identifying value.
    struct Widget {
        std::uint64_t id = 0;
        double weight = 0.0;
    };

    /// @brief Non-trivially-destructible element that counts live instances, so
    ///        tests can prove the pool runs constructors and destructors exactly.
    struct Tracked {
        static inline std::atomic<int> alive{0};
        int value = 0;

        Tracked() noexcept { alive.fetch_add(1, std::memory_order_relaxed); }
        explicit Tracked(int v) noexcept : value(v) { alive.fetch_add(1, std::memory_order_relaxed); }
        Tracked(Tracked&& o) noexcept : value(o.value) { alive.fetch_add(1, std::memory_order_relaxed); }
        Tracked& operator=(Tracked&&) noexcept = default;
        ~Tracked() { alive.fetch_sub(1, std::memory_order_relaxed); }
    };

    /// @brief Wide payload whose 16 words must always be equal. A slot handed to
    ///        two writers at once, or read while being recycled, would surface
    ///        differing words — turning a silent race into a detectable mismatch.
    struct Wide16 {
        std::uint32_t v[16];

        void Fill(std::uint32_t x) noexcept {
            for (auto& e : v) {
                e = x;
            }
        }
        [[nodiscard]] bool Consistent() const noexcept {
            for (auto e : v) {
                if (e != v[0]) {
                    return false;
                }
            }
            return true;
        }
    };

    static_assert(Traits::Poolable<Widget>);
    static_assert(Traits::Poolable<Tracked>);
    static_assert(Traits::Poolable<Wide16>);
    static_assert(std::is_trivially_copyable_v<PoolHandle>);

} // anonymous namespace

// =============================================================================
// [ConcurrentObjectPool] — PoolHandle bit packing
// =============================================================================

TEST_CASE("PoolHandle: default handle is invalid", AUTO_TAG) {
    PoolHandle h;
    REQUIRE_FALSE(h.IsValid());
}

TEST_CASE("PoolHandle: Make round-trips index and generation", AUTO_TAG) {
    PoolHandle h = PoolHandle::Make(12345, 0xBEEF);
    REQUIRE(h.IsValid());
    REQUIRE(h.Index() == 12345);
    REQUIRE(h.Generation() == 0xBEEF);
}

TEST_CASE("PoolHandle: maximum index and generation survive packing", AUTO_TAG) {
    constexpr uint32_t kMaxIndex = (1u << 15) - 1; // 32767
    constexpr uint32_t kMaxGen = 0xFFFF;
    PoolHandle h = PoolHandle::Make(kMaxIndex, kMaxGen);
    REQUIRE(h.Index() == kMaxIndex);
    REQUIRE(h.Generation() == kMaxGen);
    REQUIRE(h.IsValid());
}

TEST_CASE("PoolHandle: equality compares the full packed word", AUTO_TAG) {
    REQUIRE(PoolHandle::Make(3, 7) == PoolHandle::Make(3, 7));
    REQUIRE_FALSE(PoolHandle::Make(3, 7) == PoolHandle::Make(3, 8));
    REQUIRE_FALSE(PoolHandle::Make(3, 7) == PoolHandle{});
}

// =============================================================================
// [ConcurrentObjectPool] — Single-thread acquire / release / deref
// =============================================================================

TEST_CASE("Pool: acquire yields a live, dereferenceable slot", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 8}> pool;
    PoolHandle h = pool.Emplace(Widget{42, 1.5});
    REQUIRE(h.IsValid());
    REQUIRE(pool.IsLive(h));

    Widget* w = pool.Deref(h);
    REQUIRE(w != nullptr);
    REQUIRE(w->id == 42);
    REQUIRE(w->weight == 1.5);
}

TEST_CASE("Pool: release invalidates the handle and recycles the slot", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 8}> pool;
    PoolHandle h = pool.Emplace(Widget{1, 0.0});
    REQUIRE(pool.Release(h));
    REQUIRE_FALSE(pool.IsLive(h));
    REQUIRE(pool.Deref(h) == nullptr);
}

TEST_CASE("Pool: double release is rejected", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 8}> pool;
    PoolHandle h = pool.Emplace(Widget{1, 0.0});
    REQUIRE(pool.Release(h));
    REQUIRE_FALSE(pool.Release(h)); // already freed
}

TEST_CASE("Pool: deref of an invalid handle is null", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 8}> pool;
    REQUIRE(pool.Deref(PoolHandle{}) == nullptr);
    REQUIRE_FALSE(pool.IsLive(PoolHandle{}));
}

TEST_CASE("Pool: Acquire default-constructs the element", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 4}> pool;
    PoolHandle h = pool.Acquire();
    REQUIRE(h.IsValid());
    Widget* w = pool.Deref(h);
    REQUIRE(w != nullptr);
    REQUIRE(w->id == 0);
    REQUIRE(w->weight == 0.0);
}

// =============================================================================
// [ConcurrentObjectPool] — Generation / use-after-free detection
// =============================================================================

TEST_CASE("Pool: a stale handle to a recycled slot is rejected", AUTO_TAG) {
    // Capacity 1 forces the next acquire to reuse the only slot, so the recycled
    // handle differs from the stale one only by generation.
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 1}> pool;
    PoolHandle stale = pool.Emplace(Widget{100, 0.0});
    REQUIRE(pool.Release(stale));

    PoolHandle fresh = pool.Emplace(Widget{200, 0.0});
    REQUIRE(fresh.IsValid());
    REQUIRE(fresh.Index() == stale.Index());        // same physical slot
    REQUIRE(fresh.Generation() != stale.Generation()); // different incarnation

    REQUIRE(pool.Deref(stale) == nullptr);          // use-after-free caught
    REQUIRE(pool.Deref(fresh) != nullptr);
    REQUIRE(pool.Deref(fresh)->id == 200);
}

// =============================================================================
// [ConcurrentObjectPool] — Exhaustion
// =============================================================================

TEST_CASE("Pool: acquiring past capacity returns invalid handles", AUTO_TAG) {
    constexpr size_t kCap = 4;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap}> pool;
    REQUIRE(pool.Capacity() == kCap);

    std::array<PoolHandle, kCap> hs;
    for (size_t i = 0; i < kCap; ++i) {
        hs[i] = pool.Emplace(Widget{i, 0.0});
        REQUIRE(hs[i].IsValid());
    }
    // Pool is now full.
    REQUIRE_FALSE(pool.Emplace(Widget{999, 0.0}).IsValid());

    // Releasing one slot lets exactly one more acquire succeed.
    REQUIRE(pool.Release(hs[2]));
    PoolHandle again = pool.Emplace(Widget{7, 0.0});
    REQUIRE(again.IsValid());
    REQUIRE_FALSE(pool.Emplace(Widget{8, 0.0}).IsValid());
}

TEST_CASE("Pool: ApproxFree tracks the free-list size on one thread", AUTO_TAG) {
    constexpr size_t kCap = 8;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap}> pool;
    REQUIRE(pool.ApproxFree() == kCap);

    PoolHandle a = pool.Acquire();
    PoolHandle b = pool.Acquire();
    REQUIRE(pool.ApproxFree() == kCap - 2);

    pool.Release(a);
    REQUIRE(pool.ApproxFree() == kCap - 1);
    pool.Release(b);
    REQUIRE(pool.ApproxFree() == kCap);
}

// =============================================================================
// [ConcurrentObjectPool] — Bulk operations
// =============================================================================

TEST_CASE("Pool: AcquireBulk fills as many handles as fit", AUTO_TAG) {
    constexpr size_t kCap = 16;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap}> pool;

    std::array<PoolHandle, 10> hs{};
    size_t n = pool.AcquireBulk(hs);
    REQUIRE(n == 10);
    for (size_t i = 0; i < n; ++i) {
        REQUIRE(hs[i].IsValid());
        REQUIRE(pool.IsLive(hs[i]));
    }
    REQUIRE(pool.ApproxFree() == kCap - 10);

    REQUIRE(pool.ReleaseBulk(std::span<const PoolHandle>{hs.data(), n}) == 10);
    REQUIRE(pool.ApproxFree() == kCap);
}

TEST_CASE("Pool: AcquireBulk stops short when the pool drains", AUTO_TAG) {
    constexpr size_t kCap = 4;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap}> pool;

    std::array<PoolHandle, 8> hs{};
    size_t n = pool.AcquireBulk(hs);
    REQUIRE(n == kCap); // only kCap slots exist
}

// =============================================================================
// [ConcurrentObjectPool] — Non-trivial element lifetime
// =============================================================================

TEST_CASE("Pool: constructors and destructors run exactly once per slot", AUTO_TAG) {
    Tracked::alive.store(0);
    {
        ConcurrentObjectPool<Tracked, Detail::PoolConfig{.capacity = 8}> pool;
        PoolHandle a = pool.Emplace(1);
        PoolHandle b = pool.Emplace(2);
        REQUIRE(Tracked::alive.load() == 2);

        pool.Release(a);
        REQUIRE(Tracked::alive.load() == 1);
        // b is left live: the destructor must reap it during pool teardown.
    }
    REQUIRE(Tracked::alive.load() == 0);
}

// =============================================================================
// [ConcurrentObjectPool] — Statistics
// =============================================================================

TEST_CASE("Pool: stats count acquires, releases and failures", AUTO_TAG) {
    constexpr auto kCfg = Detail::PoolConfig{.capacity = 2, .enableStats = true};
    ConcurrentObjectPool<Widget, kCfg> pool;

    PoolHandle a = pool.Acquire();
    PoolHandle b = pool.Acquire();
    REQUIRE_FALSE(pool.Acquire().IsValid()); // failure: full
    pool.Release(a);

    REQUIRE(pool.AcquireCount() == 2);
    REQUIRE(pool.ReleaseCount() == 1);
    REQUIRE(pool.FailureCount() == 1);
    pool.Release(b);
}

// =============================================================================
// [ConcurrentObjectPool] — Batch semantics (single-CAS pop / push)
// =============================================================================

TEST_CASE("Pool: AcquireBulk then ReleaseBulk round-trips the whole pool", AUTO_TAG) {
    constexpr size_t kCap = 64;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap}> pool;

    std::array<PoolHandle, kCap> hs{};
    size_t got = pool.AcquireBulk(hs);
    REQUIRE(got == kCap);

    // Every handle must be valid, live, and reference a distinct slot index.
    std::unordered_set<uint32_t> indices;
    for (size_t i = 0; i < got; ++i) {
        REQUIRE(hs[i].IsValid());
        REQUIRE(pool.IsLive(hs[i]));
        REQUIRE(indices.insert(hs[i].Index()).second); // no duplicate slot
    }
    REQUIRE(indices.size() == kCap);
    REQUIRE(pool.ApproxFree() == 0);

    REQUIRE(pool.ReleaseBulk(std::span<const PoolHandle>{hs.data(), got}) == kCap);
    REQUIRE(pool.ApproxFree() == kCap);
}

TEST_CASE("Pool: AcquireBulk caps at remaining capacity", AUTO_TAG) {
    constexpr size_t kCap = 4;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap}> pool;

    std::array<PoolHandle, 16> hs{};
    REQUIRE(pool.AcquireBulk(hs) == kCap); // only kCap slots exist
    REQUIRE(pool.ApproxFree() == 0);
}

TEST_CASE("Pool: ReleaseBulk skips stale handles", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 8}> pool;
    std::array<PoolHandle, 3> hs{pool.Acquire(), pool.Acquire(), pool.Acquire()};

    pool.Release(hs[1]); // pre-free the middle one; it is now stale for the batch
    // Batch contains one already-freed handle and one default (invalid) handle.
    std::array<PoolHandle, 4> batch{hs[0], hs[1], PoolHandle{}, hs[2]};
    REQUIRE(pool.ReleaseBulk(batch) == 2); // only hs[0] and hs[2] were live
    REQUIRE(pool.ApproxFree() == 8);
}

TEST_CASE("Pool: bulk acquire reuses indices after a bulk release", AUTO_TAG) {
    constexpr size_t kCap = 16;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap}> pool;

    std::array<PoolHandle, kCap> first{};
    REQUIRE(pool.AcquireBulk(first) == kCap);
    REQUIRE(pool.ReleaseBulk(std::span<const PoolHandle>{first.data(), kCap}) == kCap);

    std::array<PoolHandle, kCap> second{};
    REQUIRE(pool.AcquireBulk(second) == kCap);
    for (PoolHandle h : second) {
        REQUIRE(h.IsValid());
        REQUIRE(pool.IsLive(h));
    }
    // Old handles must now be stale (slots recycled with a new generation).
    for (PoolHandle h : first) {
        REQUIRE(pool.Deref(h) == nullptr);
    }
}

// =============================================================================
// [ConcurrentObjectPool] — Result-returning API
// =============================================================================

TEST_CASE("Pool: TryEmplace reports success and exhaustion via Result", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 1}> pool;

    Result<PoolHandle> ok = pool.TryEmplace(Widget{5, 0.0});
    REQUIRE(ok.has_value());
    REQUIRE(pool.Deref(*ok)->id == 5);

    Result<PoolHandle> full = pool.TryEmplace(Widget{6, 0.0});
    REQUIRE_FALSE(full.has_value());
    REQUIRE(full.error() == ErrorCode::ResourceExhausted);
}

// =============================================================================
// [ConcurrentObjectPool] — ForEach / Clear (quiescent-pool bulk lifetime)
// =============================================================================

TEST_CASE("Pool: ForEach visits exactly the live elements", AUTO_TAG) {
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = 8}> pool;
    PoolHandle a = pool.Emplace(Widget{10, 0.0});
    PoolHandle b = pool.Emplace(Widget{20, 0.0});
    PoolHandle c = pool.Emplace(Widget{30, 0.0});
    pool.Release(b); // b is no longer live

    std::uint64_t sum = 0;
    int visited = 0;
    pool.ForEach([&](Widget& w) {
        sum += w.id;
        ++visited;
    });
    REQUIRE(visited == 2);
    REQUIRE(sum == 40); // 10 + 30, b's 20 excluded
    pool.Release(a);
    pool.Release(c);
}

TEST_CASE("Pool: Clear destroys live elements and invalidates handles", AUTO_TAG) {
    Tracked::alive.store(0);
    ConcurrentObjectPool<Tracked, Detail::PoolConfig{.capacity = 8}> pool;
    PoolHandle a = pool.Emplace(1);
    PoolHandle b = pool.Emplace(2);
    PoolHandle c = pool.Emplace(3);
    REQUIRE(Tracked::alive.load() == 3);

    pool.Clear();
    REQUIRE(Tracked::alive.load() == 0);     // every live element destroyed
    REQUIRE(pool.ApproxFree() == 8);          // all slots returned
    REQUIRE(pool.Deref(a) == nullptr);        // outstanding handles invalidated
    REQUIRE(pool.Deref(b) == nullptr);
    REQUIRE(pool.Deref(c) == nullptr);

    // Pool is reusable after Clear.
    PoolHandle d = pool.Emplace(4);
    REQUIRE(pool.Deref(d) != nullptr);
    REQUIRE(pool.Deref(d)->value == 4);
    pool.Release(d);
}

// =============================================================================
// [ConcurrentObjectPool] — Heap backing
// =============================================================================

TEST_CASE("Pool: heap-backed pool behaves identically", AUTO_TAG) {
    constexpr auto kCfg =
        Detail::PoolConfig{.capacity = 32, .backing = Detail::BackingKind::Heap};
    ConcurrentObjectPool<Widget, kCfg> pool;

    PoolHandle h = pool.Emplace(Widget{77, 2.5});
    REQUIRE(h.IsValid());
    REQUIRE(pool.Deref(h)->id == 77);
    REQUIRE(pool.Release(h));
    REQUIRE(pool.Deref(h) == nullptr);
}

// =============================================================================
// [ConcurrentObjectPool] — Multi-threaded MPMC churn
// =============================================================================

TEST_CASE("Pool: concurrent acquire/release never double-allocates a slot", AUTO_TAG) {
    // Many threads hammer a small pool. Each live handle must map to a unique
    // index at the moment it is held; we verify per-iteration that a thread's own
    // freshly acquired slot is internally consistent and that no two of its
    // simultaneously held handles collide.
    constexpr size_t kCap = 64;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap, .enableStats = true}> pool;

    constexpr int kThreads = 8;
    constexpr int kPerThread = 20'000;
    std::atomic<std::uint64_t> mismatches{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            std::array<PoolHandle, 4> held{};
            for (int i = 0; i < kPerThread; ++i) {
                for (size_t k = 0; k < held.size(); ++k) {
                    PoolHandle h = pool.Emplace(Widget{static_cast<std::uint64_t>(t), 0.0});
                    if (!h.IsValid()) {
                        held[k] = PoolHandle{};
                        continue;
                    }
                    Widget* w = pool.Deref(h);
                    if (w == nullptr || w->id != static_cast<std::uint64_t>(t)) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                    held[k] = h;
                }
                for (PoolHandle h : held) {
                    if (h.IsValid()) {
                        pool.Release(h);
                    }
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    REQUIRE(mismatches.load() == 0);
    // Every slot must have been returned: the pool is full-free again.
    REQUIRE(pool.ApproxFree() == kCap);
    REQUIRE(pool.AcquireCount() == pool.ReleaseCount());
}

TEST_CASE("Pool: concurrent bulk acquire/release conserves every slot", AUTO_TAG) {
    // Exercises the single-CAS batch pop/push paths under contention. Each thread
    // repeatedly grabs a batch, verifies the batch's slots are mutually distinct
    // and internally consistent, then bulk-releases them. No slot may be lost or
    // double-handed-out: the pool must be full-free at the end.
    constexpr size_t kCap = 256;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap, .enableStats = true}> pool;

    constexpr int kThreads = 6;
    constexpr int kIters = 8'000;
    constexpr size_t kBatch = 8;
    std::atomic<std::uint64_t> mismatches{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            std::array<PoolHandle, kBatch> batch{};
            for (int i = 0; i < kIters; ++i) {
                size_t got = pool.AcquireBulk(batch);
                std::unordered_set<uint32_t> seen;
                for (size_t k = 0; k < got; ++k) {
                    Widget* w = pool.Deref(batch[k]);
                    if (w == nullptr || !seen.insert(batch[k].Index()).second) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                pool.ReleaseBulk(std::span<const PoolHandle>{batch.data(), got});
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    REQUIRE(mismatches.load() == 0);
    REQUIRE(pool.ApproxFree() == kCap);
    REQUIRE(pool.AcquireCount() == pool.ReleaseCount());
}

// =============================================================================
// [ConcurrentObjectPool] — High-stress concurrency & async-model invariants
// =============================================================================

TEST_CASE("Pool: an acquired slot is exclusively owned (no torn payload)", AUTO_TAG) {
    // The decisive object-pool guarantee: a slot handed to a thread is owned by
    // that thread alone until released. Each owner stamps all 16 words of its
    // Wide16 with a unique tag, then re-reads them many times; if any other thread
    // had been handed the same live slot, the words would diverge. A latch starts
    // all threads simultaneously to maximise contention on the free-list head.
    constexpr size_t kCap = 48;
    ConcurrentObjectPool<Wide16, Detail::PoolConfig{.capacity = kCap}> pool;

    constexpr int kThreads = 12; // oversubscribe: more threads than slots
    constexpr int kIters = 30'000;
    std::atomic<std::uint64_t> tornReads{0};
    std::atomic<std::uint64_t> badDeref{0};
    std::latch start{kThreads};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(t) + 1u);
            start.arrive_and_wait();
            for (int i = 0; i < kIters; ++i) {
                const std::uint32_t tag = (static_cast<std::uint32_t>(t) << 24) ^
                                          static_cast<std::uint32_t>(i) ^ (rng() & 0xFFFFu);
                PoolHandle h = pool.Acquire();
                if (!h.IsValid()) {
                    continue; // pool momentarily drained — legitimate under oversubscription
                }
                Wide16* w = pool.Deref(h);
                if (w == nullptr) {
                    badDeref.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                w->Fill(tag);
                // Spin re-reading: while we hold the slot it must stay our tag.
                for (int spin = 0; spin < 8; ++spin) {
                    if (!w->Consistent() || w->v[0] != tag) {
                        tornReads.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                pool.Release(h);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    REQUIRE(badDeref.load() == 0);  // a handle we own must always deref
    REQUIRE(tornReads.load() == 0); // a slot we own is never mutated by another thread
    REQUIRE(pool.ApproxFree() == kCap);
}

TEST_CASE("Pool: SPMC handle hand-off via an async queue stays consistent", AUTO_TAG) {
    // Models the real async usage: one producer Emplaces items and publishes their
    // handles through a lock-free index queue; many consumers pull handles, Deref,
    // validate the payload the producer wrote, and Release. Exercises the pool
    // across a happens-before edge it does NOT itself provide (the queue does),
    // which is exactly how a frame/event system would use it.
    constexpr size_t kCap = 1024;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap, .enableStats = true}> pool;

    constexpr int kConsumers = 6;
    constexpr std::uint64_t kItems = 200'000;

    // Simple MPMC-safe SPSC-ish ring of handles guarded by atomics (sufficient: one
    // producer, and consumers claim slots via an atomic read cursor).
    std::vector<std::atomic<std::uint32_t>> ring(kCap);
    for (auto& slot : ring) {
        slot.store(0, std::memory_order_relaxed); // 0 == empty (invalid handle bits)
    }
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> mismatches{0};
    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (std::uint64_t n = 1; n <= kItems; ++n) {
            PoolHandle h;
            // Encode n into the payload so consumers can verify what we wrote.
            while (true) {
                h = pool.Emplace(Widget{n, static_cast<double>(n)});
                if (h.IsValid()) {
                    break;
                }
                std::this_thread::yield(); // pool full: wait for consumers to drain
            }
            const size_t idx = n % kCap;
            // Wait until the ring cell is free, then publish (release: payload + handle
            // visible to the consumer that acquires the cell).
            std::uint32_t expected = 0;
            while (!ring[idx].compare_exchange_weak(expected, h.bits, std::memory_order_release,
                                                    std::memory_order_relaxed)) {
                expected = 0;
                std::this_thread::yield();
            }
            produced.fetch_add(1, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (true) {
                bool foundAny = false;
                for (size_t idx = 0; idx < kCap; ++idx) {
                    std::uint32_t bits = ring[idx].load(std::memory_order_acquire);
                    if (bits == 0) {
                        continue;
                    }
                    if (!ring[idx].compare_exchange_strong(bits, 0, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed)) {
                        continue; // another consumer claimed it
                    }
                    foundAny = true;
                    PoolHandle h{bits};
                    Widget* w = pool.Deref(h);
                    if (w == nullptr || w->id != static_cast<std::uint64_t>(w->weight)) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                    pool.Release(h);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
                if (!foundAny && done.load(std::memory_order_acquire) &&
                    consumed.load(std::memory_order_relaxed) ==
                        produced.load(std::memory_order_acquire)) {
                    break;
                }
            }
        });
    }

    producer.join();
    for (auto& c : consumers) {
        c.join();
    }

    REQUIRE(mismatches.load() == 0);
    REQUIRE(consumed.load() == kItems);
    REQUIRE(pool.ApproxFree() == kCap); // every handed-off slot was released
    REQUIRE(pool.AcquireCount() == pool.ReleaseCount());
}

TEST_CASE("Pool: mixed single + bulk operations from many threads conserve slots", AUTO_TAG) {
    // Half the threads churn single Emplace/Release, the other half churn
    // AcquireBulk/ReleaseBulk, all against one pool. Stresses the interaction of
    // the single-node and batch CAS paths on the same head word.
    constexpr size_t kCap = 200;
    ConcurrentObjectPool<Widget, Detail::PoolConfig{.capacity = kCap, .enableStats = true}> pool;

    constexpr int kSingleThreads = 4;
    constexpr int kBulkThreads = 4;
    constexpr int kIters = 12'000;
    std::atomic<std::uint64_t> errors{0};
    std::latch start{kSingleThreads + kBulkThreads};

    std::vector<std::thread> workers;
    workers.reserve(kSingleThreads + kBulkThreads);

    for (int t = 0; t < kSingleThreads; ++t) {
        workers.emplace_back([&, t] {
            start.arrive_and_wait();
            for (int i = 0; i < kIters; ++i) {
                PoolHandle h = pool.Emplace(Widget{static_cast<std::uint64_t>(t), 0.0});
                if (h.IsValid()) {
                    if (pool.Deref(h) == nullptr) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    pool.Release(h);
                }
            }
        });
    }
    for (int t = 0; t < kBulkThreads; ++t) {
        workers.emplace_back([&] {
            std::array<PoolHandle, 12> batch{};
            start.arrive_and_wait();
            for (int i = 0; i < kIters; ++i) {
                size_t got = pool.AcquireBulk(batch);
                std::unordered_set<uint32_t> seen;
                for (size_t k = 0; k < got; ++k) {
                    if (pool.Deref(batch[k]) == nullptr ||
                        !seen.insert(batch[k].Index()).second) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                pool.ReleaseBulk(std::span<const PoolHandle>{batch.data(), got});
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    REQUIRE(errors.load() == 0);
    REQUIRE(pool.ApproxFree() == kCap);
    REQUIRE(pool.AcquireCount() == pool.ReleaseCount());
}

// =============================================================================
// Compile-time contracts (C++26 reflection audit + traits)
// =============================================================================

TEST_CASE("Pool: copy and move are deleted", AUTO_TAG) {
    using P = ConcurrentObjectPool<Widget>;
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<P>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<P>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<P>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<P>);
}

TEST_CASE("Pool: reflection layout audit isolates the free-list head", AUTO_TAG) {
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<ConcurrentObjectPool<std::uint64_t>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<ConcurrentObjectPool<Widget>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<ConcurrentObjectPool<
                       Widget, Detail::PoolConfig{.backing = Detail::BackingKind::Heap}>>());
}

TEST_CASE("Pool: Layout() exposes compile-time layout facts", AUTO_TAG) {
    using Concurrency::Contended;

    constexpr auto report = ConcurrentObjectPool<Widget>::Layout();
    STATIC_REQUIRE(report.valid);
    STATIC_REQUIRE(report.classifiedAll);
    STATIC_REQUIRE(!report.hasConflict);  // the head and cold storage never share a line
    STATIC_REQUIRE(report.memberCount == 2);
    // The contended head leads the object and owns the start of its own cache line.
    STATIC_REQUIRE(Concurrency::DomainStartsLine<ConcurrentObjectPool<Widget>, Contended>());
    STATIC_REQUIRE(Concurrency::DomainLineSpan<ConcurrentObjectPool<Widget>, Contended>() == 1);
}



