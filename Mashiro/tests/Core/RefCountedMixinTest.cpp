/**
 * @file RefCountedMixinTest.cpp
 * @brief Tests for Core/RefCountedMixin.h — CRTP intrusive reference counting.
 *
 * Coverage:
 * - Compile-time invariants: BestSizeType selection, storage size, polymorphism, derived layout.
 * - RefCounted (single-threaded): initial count = 1, AddRef/Release pairing, transition to zero
 *   triggers exactly one delete (verified through a destructor-side counter), const-method access.
 * - RefCountedAtomic (thread-safe): same single-threaded properties, plus a high-contention
 *   correctness check across many threads (every AddRef paired with a Release destroys the
 *   object exactly once).
 * - Bits parameterisation: an 8-bit counter fits in 1 byte; a 32-bit counter fits in 4.
 * - Inheritance compatibility: a polymorphic descendant works (CRTP target carries the virtual
 *   destructor) — confirms the mixin's `delete static_cast<T*>(this)` reaches the most-derived
 *   destructor when @c T does the right thing.
 *
 * The lifecycle tests use a global atomic counter incremented in each fixture's destructor so a
 * leaked object surfaces as a non-zero count at the end of the test, and a double-delete would
 * be caught by ASan in the project's @c x64-asan preset.
 */
#include "Mashiro/Core/RefCountedMixin.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

using namespace Mashiro;

// =============================================================================
// Fixtures
// =============================================================================

namespace {

    /// @brief Process-wide counter of live @ref Tracked objects. Each test resets it; non-zero at
    ///        scope exit means an object leaked.
    inline std::atomic<int> g_liveTracked{0};

    /// @brief Simple non-atomic-RC fixture. Increments the global on construct, decrements on
    ///        destruct, so tests can REQUIRE(g_liveTracked == 0) after the expected delete.
    struct Tracked : RefCounted<Tracked> {
        int payload = 0;

        Tracked()  noexcept { g_liveTracked.fetch_add(1, std::memory_order_relaxed); }
        ~Tracked() noexcept { g_liveTracked.fetch_sub(1, std::memory_order_relaxed); }
    };

    /// @brief Atomic-RC fixture, same lifecycle bookkeeping.
    struct AtomicTracked : RefCountedAtomic<AtomicTracked> {
        int payload = 0;

        AtomicTracked()  noexcept { g_liveTracked.fetch_add(1, std::memory_order_relaxed); }
        ~AtomicTracked() noexcept { g_liveTracked.fetch_sub(1, std::memory_order_relaxed); }
    };

    /// @brief Reset helper used at the start of each lifecycle test.
    void ResetLiveCounter() noexcept { g_liveTracked.store(0, std::memory_order_relaxed); }

    /// @brief Polymorphic descendant — exercise the "CRTP base + virtual dtor on T" pattern.
    struct Base : RefCountedAtomic<Base> {
        virtual ~Base()       = default;
        virtual int Identify() const { return 0; }
    };
    struct Derived : Base {
        int Identify() const override { return 7; }
    };

} // namespace

// =============================================================================
// Section 1 — Compile-time invariants
// =============================================================================

TEST_CASE("BestSizeType picks the smallest integer for a given bit-width", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<Traits::BestSizeType<8>,  std::uint8_t>);
    STATIC_REQUIRE(std::same_as<Traits::BestSizeType<16>, std::uint16_t>);
    STATIC_REQUIRE(std::same_as<Traits::BestSizeType<32>, std::uint32_t>);
    STATIC_REQUIRE(std::same_as<Traits::BestSizeType<64>, std::uint64_t>);
    // Values between supported widths still resolve, picking the next-larger fit.
    STATIC_REQUIRE(std::same_as<Traits::BestSizeType<1>,  std::uint8_t>);
    STATIC_REQUIRE(std::same_as<Traits::BestSizeType<24>, std::uint32_t>);
}

TEST_CASE("RefCounted has the expected per-instance size and trivial destruction", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(RefCounted<Tracked, 8>)  == sizeof(std::uint8_t));
    STATIC_REQUIRE(sizeof(RefCounted<Tracked, 16>) == sizeof(std::uint16_t));
    STATIC_REQUIRE(sizeof(RefCounted<Tracked, 32>) == sizeof(std::uint32_t));
    STATIC_REQUIRE(std::is_trivially_destructible_v<RefCounted<Tracked>>);
    STATIC_REQUIRE_FALSE(std::is_polymorphic_v<RefCounted<Tracked>>);
}

TEST_CASE("RefCountedAtomic has the expected size and trivial destruction", AUTO_TAG) {
    // Atomic counters have implementation-defined alignment but the same logical size as the
    // underlying integer; test against std::atomic<SizeType> rather than the bare integer.
    STATIC_REQUIRE(sizeof(RefCountedAtomic<AtomicTracked, 16>) == sizeof(std::atomic<std::uint16_t>));
    STATIC_REQUIRE(std::is_trivially_destructible_v<RefCountedAtomic<AtomicTracked>>);
    STATIC_REQUIRE_FALSE(std::is_polymorphic_v<RefCountedAtomic<AtomicTracked>>);
}

TEST_CASE("CRTP-derived class inherits the public AddRef/Release/RefCount interface", AUTO_TAG) {
    STATIC_REQUIRE(requires(Tracked* p) { p->AddRef(); p->Release(); p->RefCount(); });
    STATIC_REQUIRE(requires(AtomicTracked* p) { p->AddRef(); p->Release(); p->RefCount(); });
    // RefCount is callable on a const handle (the counter is `mutable`).
    STATIC_REQUIRE(requires(const Tracked* p) { p->RefCount(); });
    STATIC_REQUIRE(requires(const AtomicTracked* p) { p->RefCount(); });
}

// =============================================================================
// Section 2 — Single-threaded RefCounted lifecycle
// =============================================================================

TEST_CASE("RefCounted: initial count is 1 and Release at 1 deletes the object", AUTO_TAG) {
    ResetLiveCounter();
    auto* p = new Tracked{};
    REQUIRE(g_liveTracked.load() == 1);
    REQUIRE(p->RefCount() == 1);
    p->Release();
    REQUIRE(g_liveTracked.load() == 0);            // delete ran exactly once.
}

TEST_CASE("RefCounted: AddRef and Release adjust the count by one each", AUTO_TAG) {
    ResetLiveCounter();
    auto* p = new Tracked{};
    p->AddRef();
    REQUIRE(p->RefCount() == 2);
    p->AddRef();
    REQUIRE(p->RefCount() == 3);
    p->Release();
    REQUIRE(p->RefCount() == 2);
    REQUIRE(g_liveTracked.load() == 1);            // not yet deleted.
    p->Release();
    p->Release();
    REQUIRE(g_liveTracked.load() == 0);            // dropped to zero -> deleted.
}

TEST_CASE("RefCounted: many independent objects each manage their own lifetime", AUTO_TAG) {
    ResetLiveCounter();
    std::vector<Tracked*> objs;
    for (int i = 0; i < 32; ++i) objs.push_back(new Tracked{});
    REQUIRE(g_liveTracked.load() == 32);
    for (auto* p : objs) p->Release();
    REQUIRE(g_liveTracked.load() == 0);
}

// =============================================================================
// Section 3 — Single-threaded RefCountedAtomic lifecycle
// =============================================================================

TEST_CASE("RefCountedAtomic: initial count is 1 and final Release deletes", AUTO_TAG) {
    ResetLiveCounter();
    auto* p = new AtomicTracked{};
    REQUIRE(p->RefCount() == 1);
    p->AddRef();
    p->AddRef();
    REQUIRE(p->RefCount() == 3);
    p->Release();
    p->Release();
    REQUIRE(p->RefCount() == 1);
    REQUIRE(g_liveTracked.load() == 1);
    p->Release();
    REQUIRE(g_liveTracked.load() == 0);
}

// =============================================================================
// Section 4 — Multi-threaded RefCountedAtomic stress
// =============================================================================

TEST_CASE("RefCountedAtomic: concurrent AddRef/Release pairs preserve the lifetime invariant", AUTO_TAG) {
    // Each worker grabs a reference, performs some 'work', and releases. The final Release on
    // the last reference (whichever thread observes the count drop to zero) must delete exactly
    // once. ASan / TSan would catch a use-after-free or a double-delete here.
    ResetLiveCounter();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 4096;

    auto* p = new AtomicTracked{};
    REQUIRE(p->RefCount() == 1);

    // Hand each worker its own reference up-front so we know exactly how many Releases run.
    for (int i = 0; i < kThreads; ++i) p->AddRef();
    REQUIRE(p->RefCount() == 1 + kThreads);

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!start.load(std::memory_order_acquire)) {}
            // Each worker does its own AddRef/Release cycles using its initial reference as
            // the back-stop, so the count never drops to zero until the worker's final
            // Release at the end.
            for (int i = 0; i < kPerThread; ++i) {
                p->AddRef();
                // touch the object to give ASan something to catch if lifetime is wrong.
                (void)p->payload;
                p->Release();
            }
            p->Release();          // releases the per-thread reference seeded above.
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();

    REQUIRE(g_liveTracked.load() == 1);            // only the original "constructor" reference remains.
    REQUIRE(p->RefCount() == 1);
    p->Release();
    REQUIRE(g_liveTracked.load() == 0);
}

// =============================================================================
// Section 5 — Polymorphic descendant
// =============================================================================

TEST_CASE("RefCountedAtomic: deletion of a CRTP base with a virtual T destroys most-derived", AUTO_TAG) {
    // The mixin static_casts to T* (here Base*) before delete. T = Base has a virtual destructor,
    // so deleting through Base* destroys Derived correctly. This pins the documented usage:
    // CRTP target T owns the virtual destructor, the mixin does not.
    Base* p = new Derived{};
    REQUIRE(p->Identify() == 7);                   // virtual dispatch reaches the override.
    p->Release();                                  // count -> 0, mixin does delete static_cast<Base*>(this);
    // Successful return implies ~Derived ran via Base's vtable. ASan would flag a slicing or a
    // half-destruction otherwise.
}
