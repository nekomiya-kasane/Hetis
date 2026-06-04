/**
 * @file LinearAllocatorTest.cpp
 * @brief Comprehensive tests for LinearAllocator: allocation, alignment,
 *        savepoint, scope guard, OOM, reset, move, and debug stats.
 */
#include "Mashiro/Core/LinearAllocator.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace Mashiro;

// =============================================================================
// [Construction] — Basic construction and initial state
// =============================================================================

TEST_CASE("Default construction: correct initial state", "[Core.Arena]") {
    LinearAllocator arena(1024);
    REQUIRE(arena.GetCapacity() == 1024);
    REQUIRE(arena.GetUsedBytes() == 0);
    REQUIRE(arena.GetRemainingBytes() == 1024);
    REQUIRE(arena.GetBuffer() != nullptr);
}

TEST_CASE("Small arena (minimum useful size)", "[Core.Arena]") {
    LinearAllocator arena(8);
    REQUIRE(arena.GetCapacity() == 8);
    auto* p = arena.Emplace<int>(42);
    REQUIRE(p != nullptr);
    REQUIRE(*p == 42);
}

// =============================================================================
// [Allocation] — Allocate, AllocateArray, Emplace, CopyToArena
// =============================================================================

TEST_CASE("Allocate raw bytes", "[Core.Arena]") {
    LinearAllocator arena(256);
    void* p = arena.Allocate(64);
    REQUIRE(p != nullptr);
    REQUIRE(arena.GetUsedBytes() == 64);
}

TEST_CASE("Allocate respects alignment", "[Core.Arena]") {
    LinearAllocator arena(256);
    // Allocate 1 byte first to offset
    (void)arena.Allocate(1, 1);
    // Next allocation with 16-byte alignment should be aligned
    void* p = arena.Allocate(16, 16);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % 16 == 0);
}

TEST_CASE("Allocate multiple with different alignments", "[Core.Arena]") {
    LinearAllocator arena(512);
    void* a = arena.Allocate(3, 1);   // 3 bytes, 1-align
    void* b = arena.Allocate(8, 8);   // 8 bytes, 8-align
    void* c = arena.Allocate(4, 32);  // 4 bytes, 32-align
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(b) % 8 == 0);
    REQUIRE(reinterpret_cast<uintptr_t>(c) % 32 == 0);
}

TEST_CASE("AllocateArray returns correct span", "[Core.Arena]") {
    LinearAllocator arena(256);
    auto arr = arena.AllocateArray<float>(10);
    REQUIRE(arr.size() == 10);
    // Write and read back
    for (int i = 0; i < 10; ++i) arr[i] = static_cast<float>(i);
    for (int i = 0; i < 10; ++i) REQUIRE(arr[i] == static_cast<float>(i));
}

TEST_CASE("AllocateArray with count=0 returns empty span", "[Core.Arena]") {
    LinearAllocator arena(256);
    auto arr = arena.AllocateArray<int>(0);
    REQUIRE(arr.empty());
    REQUIRE(arena.GetUsedBytes() == 0);
}

TEST_CASE("Emplace constructs object correctly", "[Core.Arena]") {
    LinearAllocator arena(256);
    struct Vec3 { float x, y, z; };
    auto* v = arena.Emplace<Vec3>(1.0f, 2.0f, 3.0f);
    REQUIRE(v != nullptr);
    REQUIRE(v->x == 1.0f);
    REQUIRE(v->y == 2.0f);
    REQUIRE(v->z == 3.0f);
}

TEST_CASE("Emplace multiple objects", "[Core.Arena]") {
    LinearAllocator arena(1024);
    auto* a = arena.Emplace<int>(10);
    auto* b = arena.Emplace<double>(3.14);
    auto* c = arena.Emplace<char>('X');
    REQUIRE(*a == 10);
    REQUIRE(*b == 3.14);
    REQUIRE(*c == 'X');
    // All are distinct addresses
    REQUIRE(reinterpret_cast<void*>(a) != reinterpret_cast<void*>(b));
    REQUIRE(reinterpret_cast<void*>(b) != reinterpret_cast<void*>(c));
}

TEST_CASE("CopyToArena copies data correctly", "[Core.Arena]") {
    LinearAllocator arena(256);
    int src[] = {10, 20, 30, 40, 50};
    auto copy = arena.CopyToArena(std::span<const int>(src));
    REQUIRE(copy.size() == 5);
    for (int i = 0; i < 5; ++i) REQUIRE(copy[i] == src[i]);
    // Modify copy doesn't affect source
    copy[0] = 999;
    REQUIRE(src[0] == 10);
}

// =============================================================================
// [OOM] — Out-of-memory handling
// =============================================================================

TEST_CASE("Allocate returns nullptr when exhausted", "[Core.Arena]") {
    LinearAllocator arena(32);
    void* p1 = arena.Allocate(32);
    REQUIRE(p1 != nullptr);
    void* p2 = arena.Allocate(1);
    REQUIRE(p2 == nullptr);
}

TEST_CASE("Emplace returns nullptr when exhausted", "[Core.Arena]") {
    LinearAllocator arena(4); // only 4 bytes
    auto* p = arena.Emplace<double>(1.0); // needs 8 bytes
    REQUIRE(p == nullptr);
}

TEST_CASE("AllocateArray returns empty span when exhausted", "[Core.Arena]") {
    LinearAllocator arena(16);
    auto arr = arena.AllocateArray<int>(100); // needs 400 bytes
    REQUIRE(arr.empty());
}

TEST_CASE("Alignment padding can cause OOM", "[Core.Arena]") {
    LinearAllocator arena(20);
    (void)arena.Allocate(1, 1); // offset=1
    // Need 16 bytes at 16-align: aligned offset=16, end=32 > capacity=20
    void* p = arena.Allocate(16, 16);
    REQUIRE(p == nullptr);
}

// =============================================================================
// [Savepoint / Restore] — Manual rollback
// =============================================================================

TEST_CASE("Save and Restore rewinds allocations", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(1);
    auto mark = arena.Save();
    size_t savedOffset = arena.GetUsedBytes();

    (void)arena.Emplace<int>(2);
    (void)arena.Emplace<int>(3);
    REQUIRE(arena.GetUsedBytes() > savedOffset);

    arena.Restore(mark);
    REQUIRE(arena.GetUsedBytes() == savedOffset);
}

TEST_CASE("Restore to beginning (offset=0)", "[Core.Arena]") {
    LinearAllocator arena(256);
    auto mark = arena.Save(); // offset=0
    (void)arena.Emplace<int>(42);
    arena.Restore(mark);
    REQUIRE(arena.GetUsedBytes() == 0);
}

TEST_CASE("Multiple save/restore nested", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(1);
    auto mark1 = arena.Save();

    (void)arena.Emplace<int>(2);
    auto mark2 = arena.Save();

    (void)arena.Emplace<int>(3);
    arena.Restore(mark2);
    REQUIRE(arena.GetUsedBytes() == mark2.offset);

    arena.Restore(mark1);
    REQUIRE(arena.GetUsedBytes() == mark1.offset);
}

// =============================================================================
// [ScopeGuard] — RAII restore
// =============================================================================

TEST_CASE("ScopeGuard auto-restores on destruction", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(1);
    size_t before = arena.GetUsedBytes();
    {
        auto scope = arena.MakeScope();
        (void)arena.Emplace<int>(2);
        (void)arena.Emplace<int>(3);
        REQUIRE(arena.GetUsedBytes() > before);
    }
    REQUIRE(arena.GetUsedBytes() == before);
}

TEST_CASE("ScopeGuard Release commits allocations", "[Core.Arena]") {
    LinearAllocator arena(256);
    size_t after;
    {
        auto scope = arena.MakeScope();
        (void)arena.Emplace<int>(42);
        after = arena.GetUsedBytes();
        scope.Release(); // don't restore
    }
    REQUIRE(arena.GetUsedBytes() == after); // not rolled back
}

TEST_CASE("ScopeGuard move construction", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(1);
    size_t before = arena.GetUsedBytes();
    {
        auto scope = arena.MakeScope();
        (void)arena.Emplace<int>(2);
        auto scope2 = std::move(scope); // transfer ownership
        (void)arena.Emplace<int>(3);
    } // scope2 destructs, restores to 'before'
    REQUIRE(arena.GetUsedBytes() == before);
}

TEST_CASE("Nested ScopeGuards", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(1);
    size_t level0 = arena.GetUsedBytes();
    {
        auto outer = arena.MakeScope();
        (void)arena.Emplace<int>(2);
        size_t level1 = arena.GetUsedBytes();
        {
            auto inner = arena.MakeScope();
            (void)arena.Emplace<int>(3);
            REQUIRE(arena.GetUsedBytes() > level1);
        }
        REQUIRE(arena.GetUsedBytes() == level1); // inner restored
    }
    REQUIRE(arena.GetUsedBytes() == level0); // outer restored
}

// =============================================================================
// [Reset] — Full rewind
// =============================================================================

TEST_CASE("Reset rewinds to zero", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(1);
    (void)arena.Emplace<int>(2);
    (void)arena.Emplace<int>(3);
    REQUIRE(arena.GetUsedBytes() > 0);
    arena.Reset();
    REQUIRE(arena.GetUsedBytes() == 0);
    REQUIRE(arena.GetRemainingBytes() == 256);
}

TEST_CASE("Can allocate after Reset", "[Core.Arena]") {
    LinearAllocator arena(64);
    (void)arena.Allocate(64);
    REQUIRE(arena.Allocate(1) == nullptr); // full
    arena.Reset();
    void* p = arena.Allocate(64);
    REQUIRE(p != nullptr);
}

// =============================================================================
// [Move semantics]
// =============================================================================

TEST_CASE("Move construction transfers arena", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(42);
    size_t used = arena.GetUsedBytes();
    auto* buf = arena.GetBuffer();

    auto arena2 = std::move(arena);
    REQUIRE(arena2.GetUsedBytes() == used);
    REQUIRE(arena2.GetBuffer() == buf);
    REQUIRE(arena2.GetCapacity() == 256);
}

TEST_CASE("Move assignment transfers arena", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(42);

    LinearAllocator arena2(64);
    arena2 = std::move(arena);
    REQUIRE(arena2.GetCapacity() == 256);
}

// =============================================================================
// [Debug statistics]
// =============================================================================

#ifndef NDEBUG
TEST_CASE("Debug stats: peak usage tracks high-water mark", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Emplace<int>(1); // ~4 bytes
    (void)arena.Emplace<int>(2); // ~8 bytes
    auto peak1 = arena.GetStats().peakUsage;
    REQUIRE(peak1 > 0);

    auto mark = arena.Save();
    (void)arena.Allocate(64);
    auto peak2 = arena.GetStats().peakUsage;
    REQUIRE(peak2 > peak1);

    arena.Restore(mark); // offset goes back, but peak stays
    REQUIRE(arena.GetStats().peakUsage == peak2);
}

TEST_CASE("Debug stats: allocation count", "[Core.Arena]") {
    LinearAllocator arena(256);
    arena.ResetStats();
    (void)arena.Allocate(4);
    (void)arena.Allocate(8);
    (void)arena.Allocate(16);
    REQUIRE(arena.GetStats().allocationCount == 3);
}

TEST_CASE("Debug stats: ResetStats clears counters", "[Core.Arena]") {
    LinearAllocator arena(256);
    (void)arena.Allocate(4);
    arena.ResetStats();
    REQUIRE(arena.GetStats().allocationCount == 0);
    REQUIRE(arena.GetStats().peakUsage == 0);
}
#endif

// =============================================================================
// [Edge cases]
// =============================================================================

TEST_CASE("Zero-byte allocation succeeds (returns valid pointer)", "[Core.Arena]") {
    LinearAllocator arena(64);
    void* p = arena.Allocate(0);
    // Zero-byte allocation is allowed (pointer to current position)
    REQUIRE(p != nullptr);
}

TEST_CASE("Exact capacity fill then OOM", "[Core.Arena]") {
    LinearAllocator arena(64);
    void* p = arena.Allocate(64, 1); // exactly fills
    REQUIRE(p != nullptr);
    REQUIRE(arena.GetRemainingBytes() == 0);
    REQUIRE(arena.Allocate(1) == nullptr);
}

TEST_CASE("Large alignment with small allocation", "[Core.Arena]") {
    LinearAllocator arena(4096);
    void* p = arena.Allocate(1, 256);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % 256 == 0);
}

TEST_CASE("Sequential allocations are contiguous (no gap beyond alignment)", "[Core.Arena]") {
    LinearAllocator arena(256);
    auto* a = static_cast<std::byte*>(arena.Allocate(4, 1));
    auto* b = static_cast<std::byte*>(arena.Allocate(4, 1));
    REQUIRE(b == a + 4); // directly adjacent (1-byte alignment, no padding)
}

TEST_CASE("GetBuffer returns start of arena", "[Core.Arena]") {
    LinearAllocator arena(256);
    auto* first = arena.Allocate(1, 1);
    REQUIRE(first == arena.GetBuffer());
}
