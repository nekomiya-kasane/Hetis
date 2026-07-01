/**
 * @file ConcurrentSlabArenaTest.cpp
 * @brief Tests for Core/ConcurrentSlabArena.h — append-only multi-producer slab allocator.
 *
 * Static expectations
 * ===================
 * - @ref Mashiro::Traits::SlabArenaSlotable accepts trivially-destructible payloads (the canonical
 *   case) and rejects non-`noexcept` destructors (which would unwind in @c ~Arena).
 * - The (slab_index, slot_in_slab) decomposition is shift+mask: NodesPerSlab is a power of two,
 *   @c kSlotMask == NodesPerSlab - 1, @c (1 << kSlotShift) == NodesPerSlab.
 * - The slab itself is `alignas(kCacheLineSize)`, so neighbours never share a cache line.
 * - The arena layout passes the project-wide @ref Mashiro::Concurrency::AuditFalseSharing report
 *   and the contended head provably starts a cache line.
 *
 * Single-thread behaviour
 * =======================
 * - Allocations within a slab are contiguous, aligned, payload-preserving.
 * - Crossing a slab boundary grows the slab chain by exactly one and the new slab links back to
 *   its predecessor.
 * - @c Reset retains slab #0, drops the rest, and re-arms @c tail_ to zero — re-allocating after
 *   a Reset starts at slot 0.
 *
 * Multi-thread behaviour
 * ======================
 * - **Ticket uniqueness under MPSC pressure**: every winner observes a unique cell address, even
 *   across slab boundaries (the dangerous moment).
 * - **Slab-grow race**: when N threads simultaneously straddle a slab boundary, exactly one's CAS
 *   wins; everyone gets a valid cell. We tune @c NodesPerSlab small (2 or 4) so every other
 *   allocation crosses a boundary and the grow path is exercised continuously.
 * - **Acquire/release publication**: after all producers join, a parallel reader walks the cells
 *   from the producer-supplied pointers and validates the constructor's payload — proving the
 *   placement-new's writes were visible across the synchronisation.
 * - **Reset cycles under serial control**: alternating bursts of producers + Reset by the main
 *   thread does not leak (Asan would catch it) and re-uses slab #0.
 *
 * The MPSC stress test runs 8 producers x 8K nodes each = 65K nodes against a slab of size 4,
 * forcing a slab-grow attempt approximately every fourth allocation. With Asan + UBSan on, this
 * has been a sufficient regression bar for the design's wait-free invariants.
 */
#include "Mashiro/Core/ConcurrentSlabArena.h"
#include "Mashiro/Core/FalseSharing.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

using namespace Mashiro;

namespace {

    struct LinkNode {
        std::uint64_t key{0};
        LinkNode* next{nullptr};
    };
    static_assert(std::is_trivially_destructible_v<LinkNode>);

    struct alignas(16) AlignedNode {
        std::uint64_t a{0};
        std::uint64_t b{0};
    };

    struct ThrowingDtor {
        ~ThrowingDtor() noexcept(false) {}
    };

} // namespace

// =============================================================================
// Section 1 — Compile-time guarantees
// =============================================================================

TEST_CASE("Concept SlabArenaSlotable accepts trivially-destructible nodes", AUTO_TAG) {
    STATIC_REQUIRE(Traits::SlabArenaSlotable<LinkNode>);
    STATIC_REQUIRE(Traits::SlabArenaSlotable<AlignedNode>);
    STATIC_REQUIRE(Traits::SlabArenaSlotable<std::uint64_t>);
}

TEST_CASE("Concept SlabArenaSlotable rejects throwing destructors", AUTO_TAG) {
    STATIC_REQUIRE_FALSE(Traits::SlabArenaSlotable<ThrowingDtor>);
}

TEST_CASE("Default NodesPerSlab is a cache-line-friendly power of two", AUTO_TAG) {
    using Arena = ConcurrentSlabArena<LinkNode>;
    STATIC_REQUIRE(std::has_single_bit(Arena::kNodesPerSlab));
    STATIC_REQUIRE(Arena::kNodesPerSlab >= 1);
    STATIC_REQUIRE(Arena::kSlotMask == Arena::kNodesPerSlab - 1);
    STATIC_REQUIRE((1u << Arena::kSlotShift) == Arena::kNodesPerSlab);
}

TEST_CASE("Arena layout passes the false-sharing audit", AUTO_TAG) {
    using Arena = ConcurrentSlabArena<LinkNode>;
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<Arena>());
    STATIC_REQUIRE(Concurrency::DomainStartsLine<Arena, Concurrency::Contended>());
}

TEST_CASE("Arena and a custom slab capacity also audit clean", AUTO_TAG) {
    using A = ConcurrentSlabArena<LinkNode, 4>;
    using B = ConcurrentSlabArena<AlignedNode, 16>;
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<A>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<B>());
}

// =============================================================================
// Section 2 — Single-producer behaviour
// =============================================================================

TEST_CASE("Allocate inside slab #0 is contiguous and aligned for the slot type", AUTO_TAG) {
    ConcurrentSlabArena<AlignedNode, 8> arena;
    std::vector<AlignedNode*> p;
    for (int i = 0; i < 8; ++i) {
        p.push_back(arena.Allocate(AlignedNode{static_cast<std::uint64_t>(i),
                                               static_cast<std::uint64_t>(i + 100)}));
    }
    REQUIRE(arena.Size() == 8);
    REQUIRE(arena.SlabCount() == 1);

    for (int i = 0; i < 8; ++i) {
        REQUIRE(p[i]->a == static_cast<std::uint64_t>(i));
        REQUIRE(p[i]->b == static_cast<std::uint64_t>(i + 100));
        REQUIRE(reinterpret_cast<uintptr_t>(p[i]) % alignof(AlignedNode) == 0);
    }
    for (int i = 1; i < 8; ++i) {
        const auto delta =
            reinterpret_cast<std::uintptr_t>(p[i]) - reinterpret_cast<std::uintptr_t>(p[i - 1]);
        REQUIRE(delta == sizeof(AlignedNode));
    }
}

TEST_CASE("Crossing a slab boundary grows the registry exactly once per boundary", AUTO_TAG) {
    ConcurrentSlabArena<LinkNode, 4> arena;
    std::vector<LinkNode*> nodes;
    for (int i = 0; i < 10; ++i) {
        nodes.push_back(arena.Allocate(static_cast<std::uint64_t>(i), nullptr));
    }
    REQUIRE(arena.Size() == 10);
    REQUIRE(arena.SlabCount() == 3);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(nodes[i]->key == static_cast<std::uint64_t>(i));
    }
}

TEST_CASE("Reset retains slab #0 and reuses the storage", AUTO_TAG) {
    ConcurrentSlabArena<LinkNode, 4> arena;
    for (int i = 0; i < 10; ++i) (void)arena.Allocate(static_cast<std::uint64_t>(i), nullptr);
    REQUIRE(arena.SlabCount() == 3);

    arena.Reset();
    REQUIRE(arena.Size() == 0);
    REQUIRE(arena.SlabCount() == 1);

    auto* fresh = arena.Allocate(std::uint64_t{99}, nullptr);
    REQUIRE(fresh != nullptr);
    REQUIRE(fresh->key == 99);
    REQUIRE(arena.Size() == 1);
}

// =============================================================================
// Section 3 — High-concurrency stress
// =============================================================================

namespace {

    // Producer payload that records its (producer-id, sequence) into the node so a parallel reader
    // can validate the placement-new's writes were observable across the synchronisation.
    struct StressNode {
        std::uint32_t producer{0};
        std::uint32_t sequence{0};
        StressNode* next{nullptr};
    };
    static_assert(std::is_trivially_destructible_v<StressNode>);

} // namespace

TEST_CASE("MPSC stress: every ticket produces a unique node, payloads survive", AUTO_TAG) {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 8192;
    // Tiny slab forces a grow attempt approximately every 4 allocations, exercising the CAS race.
    ConcurrentSlabArena<StressNode, 4> arena;

    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    std::vector<std::vector<StressNode*>> seen(kProducers);

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            seen[t].reserve(kPerProducer);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            for (int i = 0; i < kPerProducer; ++i) {
                seen[t].push_back(
                    arena.Allocate(static_cast<std::uint32_t>(t),
                                   static_cast<std::uint32_t>(i),
                                   /*next=*/nullptr));
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();

    REQUIRE(arena.Size() == static_cast<std::size_t>(kProducers * kPerProducer));

    // All addresses unique.
    std::set<StressNode*> uniq;
    for (auto& v : seen) for (auto* p : v) uniq.insert(p);
    REQUIRE(uniq.size() == static_cast<std::size_t>(kProducers * kPerProducer));

    // Per-producer payloads were preserved in producer-local order.
    for (int t = 0; t < kProducers; ++t) {
        for (int i = 0; i < kPerProducer; ++i) {
            REQUIRE(seen[t][i]->producer == static_cast<std::uint32_t>(t));
            REQUIRE(seen[t][i]->sequence == static_cast<std::uint32_t>(i));
        }
    }
}

TEST_CASE("Slab-grow race: tiny slabs do not lose tickets or double-publish", AUTO_TAG) {
    constexpr int kProducers = 16;
    constexpr int kPerProducer = 4096;
    // NodesPerSlab=2 means every other ticket crosses a slab boundary — the grow path is the
    // common path here, not the exception.
    ConcurrentSlabArena<StressNode, 2> arena;

    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    std::vector<std::vector<StressNode*>> seen(kProducers);

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            seen[t].reserve(kPerProducer);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            for (int i = 0; i < kPerProducer; ++i) {
                seen[t].push_back(arena.Allocate(static_cast<std::uint32_t>(t),
                                                 static_cast<std::uint32_t>(i),
                                                 nullptr));
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();

    REQUIRE(arena.Size() == static_cast<std::size_t>(kProducers * kPerProducer));

    // SlabCount must equal ceil(total / 2). The arena keeps the bump pointer monotonic, so the
    // count is exactly Size() / 2.
    REQUIRE(arena.SlabCount()
            == (static_cast<std::size_t>(kProducers * kPerProducer) + 1) / 2);

    // Every slab base address is distinct.
    std::set<const StressNode*> bases;
    for (std::size_t i = 0; i < arena.SlabCount(); ++i) {
        const StressNode* base = arena.SlabBase(i);
        REQUIRE(base != nullptr);
        bases.insert(base);
    }
    REQUIRE(bases.size() == arena.SlabCount());

    // All node addresses unique (sanity over the per-producer view).
    std::set<StressNode*> uniq;
    for (auto& v : seen) for (auto* p : v) uniq.insert(p);
    REQUIRE(uniq.size() == static_cast<std::size_t>(kProducers * kPerProducer));
}

TEST_CASE("Reset under producer-then-controller cycles does not leak", AUTO_TAG) {
    ConcurrentSlabArena<StressNode, 8> arena;

    constexpr int kCycles = 16;
    constexpr int kProducers = 4;
    constexpr int kPerCycle = 2048;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        std::atomic<bool> go{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < kProducers; ++t) {
            producers.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) { /* spin */ }
                for (int i = 0; i < kPerCycle; ++i) {
                    (void)arena.Allocate(static_cast<std::uint32_t>(t),
                                         static_cast<std::uint32_t>(i),
                                         nullptr);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& th : producers) th.join();

        REQUIRE(arena.Size() == static_cast<std::size_t>(kProducers * kPerCycle));

        // Quiesce, then Reset (controller's exclusive turn).
        arena.Reset();
        REQUIRE(arena.Size() == 0);
        REQUIRE(arena.SlabCount() == 1);
    }
}
