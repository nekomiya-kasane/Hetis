/**
 * @file ChunkedSlotMapTest.cpp
 * @brief Comprehensive tests for ChunkedSlotMap: insertion, removal, lookup,
 *        dense iteration, generation safety, edge cases, and stress.
 */
#include "Mashiro/Core/ChunkedSlotMap.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

using namespace Mashiro;

// =============================================================================
// Helper types
// =============================================================================

namespace {

    struct DtorTracker {
        int* counter;
        int value;
        explicit DtorTracker(int* c, int v = 0) : counter(c), value(v) {}
        ~DtorTracker() { if (counter) ++(*counter); }
        DtorTracker(DtorTracker&& o) noexcept : counter(o.counter), value(o.value) { o.counter = nullptr; }
        DtorTracker& operator=(DtorTracker&& o) noexcept {
            if (this != &o) { counter = o.counter; value = o.value; o.counter = nullptr; }
            return *this;
        }
    };

    struct Heavy {
        std::string name;
        int id = 0;
        Heavy() = default;
        Heavy(std::string n, int i) : name(std::move(n)), id(i) {}
    };

} // anonymous namespace

// =============================================================================
// [SlotHandle] — Value semantics
// =============================================================================

TEST_CASE("SlotHandle: default is null", AUTO_TAG) {
    constexpr SlotHandle h;
    STATIC_REQUIRE(!h.IsValid());
    STATIC_REQUIRE(h == SlotHandle::Null());
}

TEST_CASE("SlotHandle: valid handle", AUTO_TAG) {
    constexpr SlotHandle h{1, 1};
    STATIC_REQUIRE(h.IsValid());
    STATIC_REQUIRE(h != SlotHandle::Null());
}

TEST_CASE("SlotHandle: equality", AUTO_TAG) {
    constexpr SlotHandle a{5, 3};
    constexpr SlotHandle b{5, 3};
    constexpr SlotHandle c{5, 4};
    STATIC_REQUIRE(a == b);
    STATIC_REQUIRE(a != c);
}

// =============================================================================
// [Construction] — Default state
// =============================================================================

TEST_CASE("Default construction: empty map", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    REQUIRE(map.Size() == 0);
    REQUIRE(map.Empty());
    REQUIRE(map.Capacity() > 0); // at least one chunk allocated
}

// =============================================================================
// [Insertion] — Emplace and handle validity
// =============================================================================

TEST_CASE("Emplace single element", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(42);
    REQUIRE(h.IsValid());
    REQUIRE(map.Size() == 1);
    REQUIRE(*map.Get(h) == 42);
}

TEST_CASE("Emplace multiple elements", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h1 = map.Emplace(10);
    auto h2 = map.Emplace(20);
    auto h3 = map.Emplace(30);
    REQUIRE(map.Size() == 3);
    REQUIRE(*map.Get(h1) == 10);
    REQUIRE(*map.Get(h2) == 20);
    REQUIRE(*map.Get(h3) == 30);
}

TEST_CASE("Emplace with complex type", AUTO_TAG) {
    ChunkedSlotMap<Heavy> map;
    auto h = map.Emplace("hello", 99);
    REQUIRE(map.Get(h)->name == "hello");
    REQUIRE(map.Get(h)->id == 99);
}

TEST_CASE("Handles have unique indices", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h1 = map.Emplace(1);
    auto h2 = map.Emplace(2);
    auto h3 = map.Emplace(3);
    REQUIRE(h1.index != h2.index);
    REQUIRE(h2.index != h3.index);
    REQUIRE(h1.index != h3.index);
}

// =============================================================================
// [Removal] — Free and swap-and-pop correctness
// =============================================================================

TEST_CASE("Free removes element", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(42);
    REQUIRE(map.Free(h));
    REQUIRE(map.Size() == 0);
    REQUIRE(map.Get(h) == nullptr);
}

TEST_CASE("Free invalidates handle (generation mismatch)", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(42);
    map.Free(h);
    REQUIRE(!map.IsAlive(h));
    REQUIRE(map.Get(h) == nullptr);
}

TEST_CASE("Double-free returns false", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(42);
    REQUIRE(map.Free(h));
    REQUIRE(!map.Free(h)); // already freed
}

TEST_CASE("Free null handle returns false", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    REQUIRE(!map.Free(SlotHandle::Null()));
}

TEST_CASE("Free middle element preserves others (swap-and-pop)", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h1 = map.Emplace(10);
    auto h2 = map.Emplace(20);
    auto h3 = map.Emplace(30);

    map.Free(h2); // middle removed, h3's dense entry swaps into h2's position
    REQUIRE(map.Size() == 2);
    REQUIRE(*map.Get(h1) == 10);
    REQUIRE(map.Get(h2) == nullptr);
    REQUIRE(*map.Get(h3) == 30);
}

TEST_CASE("Free last element (no swap needed)", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h1 = map.Emplace(10);
    auto h2 = map.Emplace(20);

    map.Free(h2); // last element, just pop
    REQUIRE(map.Size() == 1);
    REQUIRE(*map.Get(h1) == 10);
}

// =============================================================================
// [Destructor] — Non-trivial types properly destroyed
// =============================================================================

TEST_CASE("Destructor called on Free", AUTO_TAG) {
    int dtorCount = 0;
    {
        ChunkedSlotMap<DtorTracker> map;
        auto h = map.Emplace(&dtorCount, 1);
        map.Free(h);
        REQUIRE(dtorCount == 1);
    }
    REQUIRE(dtorCount == 1); // no double destroy
}

TEST_CASE("Destructor called on map destruction", AUTO_TAG) {
    int dtorCount = 0;
    {
        ChunkedSlotMap<DtorTracker> map;
        (void)map.Emplace(&dtorCount, 1);
        (void)map.Emplace(&dtorCount, 2);
        (void)map.Emplace(&dtorCount, 3);
    }
    REQUIRE(dtorCount == 3);
}

TEST_CASE("Destructor called on Clear", AUTO_TAG) {
    int dtorCount = 0;
    ChunkedSlotMap<DtorTracker> map;
    (void)map.Emplace(&dtorCount, 1);
    (void)map.Emplace(&dtorCount, 2);
    map.Clear();
    REQUIRE(dtorCount == 2);
    REQUIRE(map.Size() == 0);
}

// =============================================================================
// [Lookup] — Get, operator[], IsAlive
// =============================================================================

TEST_CASE("Get returns nullptr for stale handle", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(42);
    map.Free(h);
    REQUIRE(map.Get(h) == nullptr);
}

TEST_CASE("operator[] unchecked access", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(77);
    REQUIRE(map[h] == 77);
    map[h] = 88;
    REQUIRE(*map.Get(h) == 88);
}

TEST_CASE("IsAlive true for live, false for dead", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(1);
    REQUIRE(map.IsAlive(h));
    map.Free(h);
    REQUIRE(!map.IsAlive(h));
}

// =============================================================================
// [Generation] — Reuse of freed slots with incremented generation
// =============================================================================

TEST_CASE("Freed slot is reused with new generation", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h1 = map.Emplace(100);
    uint32_t oldIndex = h1.index;
    map.Free(h1);

    auto h2 = map.Emplace(200);
    // Reused same sparse index (from free-list)
    REQUIRE(h2.index == oldIndex);
    // But different generation
    REQUIRE(h2.generation != h1.generation);
    // Old handle is dead, new one is alive
    REQUIRE(!map.IsAlive(h1));
    REQUIRE(map.IsAlive(h2));
    REQUIRE(*map.Get(h2) == 200);
}

TEST_CASE("Multiple reuse cycles maintain correctness", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    std::vector<SlotHandle> staleHandles;

    for (int i = 0; i < 10; ++i) {
        auto h = map.Emplace(i);
        staleHandles.push_back(h);
        map.Free(h);
    }
    // All stale handles should be dead
    for (auto& h : staleHandles) {
        REQUIRE(!map.IsAlive(h));
    }
    REQUIRE(map.Size() == 0);
}

// =============================================================================
// [Dense iteration] — ForEach, Data()
// =============================================================================

TEST_CASE("ForEach iterates only live elements", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    (void)map.Emplace(1);
    auto h2 = map.Emplace(2);
    (void)map.Emplace(3);
    map.Free(h2);

    std::vector<int> values;
    map.ForEach([&](int& v) { values.push_back(v); });
    REQUIRE(values.size() == 2);
    // Values should be 1 and 3 (order may differ due to swap-and-pop)
    std::sort(values.begin(), values.end());
    REQUIRE(values == std::vector<int>{1, 3});
}

TEST_CASE("ForEach with handle+value signature", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h1 = map.Emplace(10);
    auto h2 = map.Emplace(20);

    int sum = 0;
    map.ForEach([&](SlotHandle h, int& v) {
        REQUIRE(h.IsValid());
        sum += v;
    });
    REQUIRE(sum == 30);
}

TEST_CASE("ForEach on empty map does nothing", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    int count = 0;
    map.ForEach([&](int&) { ++count; });
    REQUIRE(count == 0);
}

TEST_CASE("Data() returns pointer to first dense value", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    REQUIRE(map.Data() == nullptr); // empty

    auto h1 = map.Emplace(42);
    REQUIRE(map.Data() != nullptr);
    REQUIRE(*map.Data() == 42);
}

// =============================================================================
// [Chunk growth] — Multi-chunk allocation
// =============================================================================

TEST_CASE("Grows beyond single chunk", AUTO_TAG) {
    // Use small chunk (ChunkBits=2 → 4 slots/chunk, slot 0 reserved → 3 usable)
    ChunkedSlotMap<int, 2> map;
    std::vector<SlotHandle> handles;

    for (int i = 0; i < 20; ++i) {
        handles.push_back(map.Emplace(i * 10));
    }
    REQUIRE(map.Size() == 20);

    // All handles should be valid
    for (int i = 0; i < 20; ++i) {
        REQUIRE(*map.Get(handles[i]) == i * 10);
    }
}

// =============================================================================
// [Clear] — Bulk reset
// =============================================================================

TEST_CASE("Clear empties map but preserves capacity", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    for (int i = 0; i < 100; ++i) (void)map.Emplace(i);
    uint32_t capBefore = map.Capacity();

    map.Clear();
    REQUIRE(map.Size() == 0);
    REQUIRE(map.Empty());
    REQUIRE(map.Capacity() == capBefore); // chunks not freed

    // Can insert again after clear
    auto h = map.Emplace(999);
    REQUIRE(*map.Get(h) == 999);
}

TEST_CASE("Clear invalidates all previous handles", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h1 = map.Emplace(1);
    auto h2 = map.Emplace(2);
    map.Clear();
    REQUIRE(!map.IsAlive(h1));
    REQUIRE(!map.IsAlive(h2));
}

// =============================================================================
// [Move semantics]
// =============================================================================

TEST_CASE("Move construction transfers ownership", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(42);
    auto map2 = std::move(map);
    REQUIRE(map2.Size() == 1);
    REQUIRE(*map2.Get(h) == 42);
}

TEST_CASE("Move assignment transfers ownership", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    auto h = map.Emplace(42);
    ChunkedSlotMap<int> map2;
    map2 = std::move(map);
    REQUIRE(map2.Size() == 1);
    REQUIRE(*map2.Get(h) == 42);
}

// =============================================================================
// [Stress] — Large scale
// =============================================================================

TEST_CASE("Stress: 10000 insert/remove cycles", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    std::vector<SlotHandle> handles;
    handles.reserve(10000);

    // Insert 10000
    for (int i = 0; i < 10000; ++i) {
        handles.push_back(map.Emplace(i));
    }
    REQUIRE(map.Size() == 10000);

    // Remove odd-indexed
    for (int i = 1; i < 10000; i += 2) {
        REQUIRE(map.Free(handles[i]));
    }
    REQUIRE(map.Size() == 5000);

    // Even-indexed still alive
    for (int i = 0; i < 10000; i += 2) {
        REQUIRE(map.IsAlive(handles[i]));
        REQUIRE(*map.Get(handles[i]) == i);
    }

    // Odd-indexed dead
    for (int i = 1; i < 10000; i += 2) {
        REQUIRE(!map.IsAlive(handles[i]));
    }

    // Re-insert into freed slots
    for (int i = 0; i < 5000; ++i) {
        auto h = map.Emplace(i + 50000);
        REQUIRE(map.IsAlive(h));
    }
    REQUIRE(map.Size() == 10000);
}

TEST_CASE("Stress: ForEach sum matches expected", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    int expected = 0;
    for (int i = 0; i < 1000; ++i) {
        (void)map.Emplace(i);
        expected += i;
    }

    int sum = 0;
    map.ForEach([&](int& v) { sum += v; });
    REQUIRE(sum == expected);
}

// =============================================================================
// [Edge cases]
// =============================================================================

TEST_CASE("Reserve does not change size", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    map.Reserve(1000);
    REQUIRE(map.Size() == 0);
}

TEST_CASE("Free all elements then re-insert", AUTO_TAG) {
    ChunkedSlotMap<int> map;
    std::vector<SlotHandle> handles;
    for (int i = 0; i < 50; ++i) handles.push_back(map.Emplace(i));
    for (auto& h : handles) map.Free(h);
    REQUIRE(map.Size() == 0);

    // All freed slots should be reusable
    for (int i = 0; i < 50; ++i) {
        auto h = map.Emplace(i + 100);
        REQUIRE(map.IsAlive(h));
    }
    REQUIRE(map.Size() == 50);
}

TEST_CASE("kDenseStride is correct", AUTO_TAG) {
    // DenseEntry = {T value, uint32_t sparseIndex}
    // For T=int: stride = sizeof(int) + sizeof(uint32_t) + padding
    REQUIRE(ChunkedSlotMap<int>::kDenseStride >= sizeof(int) + sizeof(uint32_t));
}
