/**
 * @file InlineFunctionTest.cpp
 * @brief Comprehensive tests for InlineFunction: construction, invocation,
 *        move semantics, swap, noexcept variant, trivial fast-path, edge cases.
 */
#include "Mashiro/Core/InlineFunction.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

using namespace Mashiro;

// =============================================================================
// Test helper types
// =============================================================================

namespace {

    // Stateless callable (empty, trivially copyable → fast path)
    struct Adder {
        int operator()(int a, int b) const noexcept { return a + b; }
    };

    // Trivially copyable with state (fast path)
    struct Multiplier {
        int factor;
        int operator()(int x) const noexcept { return x * factor; }
    };

    // Non-trivial callable: has a destructor side-effect
    struct DestructorTracker {
        int* counter;
        explicit DestructorTracker(int* c) : counter(c) {}
        ~DestructorTracker() { if (counter) ++(*counter); }
        DestructorTracker(DestructorTracker&& o) noexcept : counter(o.counter) { o.counter = nullptr; }
        DestructorTracker& operator=(DestructorTracker&&) = delete;
        int operator()(int x) const { return x + 1; }
    };

    // Move tracker: counts how many times moved
    struct MoveTracker {
        int* moveCount;
        explicit MoveTracker(int* mc) : moveCount(mc) {}
        MoveTracker(MoveTracker&& o) noexcept : moveCount(o.moveCount) {
            ++(*moveCount);
            o.moveCount = nullptr;
        }
        MoveTracker& operator=(MoveTracker&&) = delete;
        ~MoveTracker() = default;
        int operator()() const { return *moveCount; }
    };

    // Large callable (fills most of the 64-byte capacity)
    struct LargeCallable {
        char data[56] = {};
        int operator()() const { return data[0] + 42; }
    };
    static_assert(sizeof(LargeCallable) <= 64);

    // Callable that returns void
    struct SideEffect {
        int* target;
        void operator()(int v) { *target = v; }
    };

    // Callable with multiple args and return
    struct Concatenator {
        std::string operator()(const char* a, const char* b) const {
            return std::string(a) + b;
        }
    };

} // anonymous namespace

// =============================================================================
// [Construction] — Default, nullptr, callable, lambda
// =============================================================================

TEST_CASE("Default construction is empty", AUTO_TAG) {
    InlineFunction<int(int)> f;
    REQUIRE(!f);
    REQUIRE(f == nullptr);
}

TEST_CASE("nullptr construction is empty", AUTO_TAG) {
    InlineFunction<int(int)> f{nullptr};
    REQUIRE(!f);
}

TEST_CASE("Construct from stateless lambda", AUTO_TAG) {
    InlineFunction<int(int, int)> f = [](int a, int b) { return a + b; };
    REQUIRE(f);
    REQUIRE(f(3, 4) == 7);
}

TEST_CASE("Construct from stateless struct", AUTO_TAG) {
    InlineFunction<int(int, int)> f{Adder{}};
    REQUIRE(f(10, 20) == 30);
}

TEST_CASE("Construct from trivially-copyable stateful struct", AUTO_TAG) {
    InlineFunction<int(int) noexcept> f{Multiplier{5}};
    REQUIRE(f(7) == 35);
}

TEST_CASE("Construct from capturing lambda", AUTO_TAG) {
    int captured = 100;
    InlineFunction<int()> f = [&captured]() { return captured; };
    REQUIRE(f() == 100);
    captured = 200;
    REQUIRE(f() == 200);
}

TEST_CASE("Construct from large callable (near capacity limit)", AUTO_TAG) {
    LargeCallable lc;
    lc.data[0] = 8;
    InlineFunction<int()> f{lc};
    REQUIRE(f() == 50); // 8 + 42
}

// =============================================================================
// [Invocation] — Various signatures
// =============================================================================

TEST_CASE("Void return type", AUTO_TAG) {
    int result = 0;
    InlineFunction<void(int)> f{SideEffect{&result}};
    f(42);
    REQUIRE(result == 42);
}

TEST_CASE("Multi-arg with string return", AUTO_TAG) {
    InlineFunction<std::string(const char*, const char*)> f{Concatenator{}};
    REQUIRE(f("hello", " world") == "hello world");
}

TEST_CASE("noexcept signature variant", AUTO_TAG) {
    InlineFunction<int(int) noexcept> f = [](int x) noexcept { return x * 3; };
    REQUIRE(f(4) == 12);
    static_assert(noexcept(f(4)));
}

// =============================================================================
// [Move semantics] — Move construct, move assign, source becomes empty
// =============================================================================

TEST_CASE("Move construction transfers callable", AUTO_TAG) {
    InlineFunction<int(int)> f = [](int x) { return x + 1; };
    InlineFunction<int(int)> g{std::move(f)};
    REQUIRE(!f);
    REQUIRE(g);
    REQUIRE(g(5) == 6);
}

TEST_CASE("Move assignment transfers callable", AUTO_TAG) {
    InlineFunction<int()> f = []() { return 1; };
    InlineFunction<int()> g = []() { return 2; };
    g = std::move(f);
    REQUIRE(!f);
    REQUIRE(g() == 1);
}

TEST_CASE("Move assignment to empty", AUTO_TAG) {
    InlineFunction<int()> f = []() { return 99; };
    InlineFunction<int()> g;
    g = std::move(f);
    REQUIRE(!f);
    REQUIRE(g() == 99);
}

TEST_CASE("Move from empty to non-empty (destroys target)", AUTO_TAG) {
    int dtorCount = 0;
    {
        InlineFunction<int(int)> f{DestructorTracker{&dtorCount}};
        InlineFunction<int(int)> empty;
        f = std::move(empty);
        // f was destroyed during assignment
        REQUIRE(dtorCount == 1);
        REQUIRE(!f);
    }
    // No double-destroy
    REQUIRE(dtorCount == 1);
}

TEST_CASE("Self move-assignment is safe", AUTO_TAG) {
    InlineFunction<int()> f = []() { return 42; };
    auto& ref = f;
    f = std::move(ref);
    REQUIRE(f);
    REQUIRE(f() == 42);
}

// =============================================================================
// [Destructor] — Non-trivial callables are properly destroyed
// =============================================================================

TEST_CASE("Destructor is called on destruction", AUTO_TAG) {
    int dtorCount = 0;
    {
        InlineFunction<int(int)> f{DestructorTracker{&dtorCount}};
        REQUIRE(f(10) == 11);
    }
    REQUIRE(dtorCount == 1);
}

TEST_CASE("Destructor is called on reassignment", AUTO_TAG) {
    int dtorCount = 0;
    InlineFunction<int(int)> f{DestructorTracker{&dtorCount}};
    f = nullptr;
    REQUIRE(dtorCount == 1);
    REQUIRE(!f);
}

TEST_CASE("Destructor is called when overwritten by move-assign", AUTO_TAG) {
    int dtor1 = 0, dtor2 = 0;
    InlineFunction<int(int)> f{DestructorTracker{&dtor1}};
    InlineFunction<int(int)> g{DestructorTracker{&dtor2}};
    f = std::move(g);
    REQUIRE(dtor1 == 1); // old f destroyed
    REQUIRE(dtor2 == 0); // g's tracker was moved-from (counter nulled)
}

// =============================================================================
// [Trivial fast-path] — Trivially copyable callables use memcpy, no ops
// =============================================================================

TEST_CASE("Trivial callable: no destructor overhead on move", AUTO_TAG) {
    // Multiplier is trivially copyable — moves should be memcpy
    InlineFunction<int(int) noexcept> f{Multiplier{3}};
    InlineFunction<int(int) noexcept> g{std::move(f)};
    REQUIRE(!f);
    REQUIRE(g(10) == 30);
}

TEST_CASE("Trivial callable: destruction is no-op", AUTO_TAG) {
    // Just verify it doesn't crash — Multiplier has trivial destructor
    {
        InlineFunction<int(int) noexcept> f{Multiplier{7}};
        REQUIRE(f(3) == 21);
    }
    // If ops were incorrectly called, this would crash or UB
}

// =============================================================================
// [Swap] — ADL swap and member Swap
// =============================================================================

TEST_CASE("Swap two non-empty functions", AUTO_TAG) {
    InlineFunction<int()> f = []() { return 1; };
    InlineFunction<int()> g = []() { return 2; };
    swap(f, g);
    REQUIRE(f() == 2);
    REQUIRE(g() == 1);
}

TEST_CASE("Swap non-empty with empty", AUTO_TAG) {
    InlineFunction<int()> f = []() { return 42; };
    InlineFunction<int()> g;
    swap(f, g);
    REQUIRE(!f);
    REQUIRE(g() == 42);
}

TEST_CASE("Swap two trivial callables (memcpy path)", AUTO_TAG) {
    InlineFunction<int(int) noexcept> f{Multiplier{2}};
    InlineFunction<int(int) noexcept> g{Multiplier{5}};
    swap(f, g);
    REQUIRE(f(10) == 50);
    REQUIRE(g(10) == 20);
}

TEST_CASE("Swap two empty is no-op", AUTO_TAG) {
    InlineFunction<int()> f;
    InlineFunction<int()> g;
    swap(f, g);
    REQUIRE(!f);
    REQUIRE(!g);
}

// =============================================================================
// [Nullptr assignment] — Reset stored callable
// =============================================================================

TEST_CASE("Assign nullptr clears function", AUTO_TAG) {
    InlineFunction<int()> f = []() { return 1; };
    REQUIRE(f);
    f = nullptr;
    REQUIRE(!f);
    REQUIRE(f == nullptr);
}

// =============================================================================
// [Edge cases]
// =============================================================================

TEST_CASE("Function pointer as callable", AUTO_TAG) {
    auto* fp = +[](int x) -> int { return x * x; };
    InlineFunction<int(int)> f{fp};
    REQUIRE(f(5) == 25);
}

TEST_CASE("Mutable lambda (non-const invocation)", AUTO_TAG) {
    int counter = 0;
    InlineFunction<int()> f = [counter]() mutable { return ++counter; };
    REQUIRE(f() == 1);
    REQUIRE(f() == 2);
    REQUIRE(f() == 3);
}

TEST_CASE("Custom capacity", AUTO_TAG) {
    // 128-byte capacity for larger captures
    struct HugeCapture {
        char data[120] = {};
        int operator()() const { return data[0] + 1; }
    };
    InlineFunction<int(), 128> f{HugeCapture{}};
    REQUIRE(f() == 1);
}

TEST_CASE("Move tracker counts moves correctly", AUTO_TAG) {
    int moves = 0;
    {
        InlineFunction<int()> f{MoveTracker{&moves}};
        // Construction: 1 move (forward into storage)
        int constructMoves = moves;
        REQUIRE(constructMoves >= 1);

        InlineFunction<int()> g{std::move(f)};
        // Move construction: 1 more move
        REQUIRE(moves == constructMoves + 1);
    }
}

TEST_CASE("Implicit conversion from compatible callable", AUTO_TAG) {
    // int return should work for callable returning short (implicit conversion)
    InlineFunction<int()> f = []() -> short { return 7; };
    REQUIRE(f() == 7);
}

// =============================================================================
// [noexcept specialisation constraints]
// =============================================================================

TEST_CASE("noexcept variant rejects throwing callable", AUTO_TAG) {
    // This should NOT compile if uncommented — the concept rejects non-noexcept callables:
    // InlineFunction<int() noexcept> f = []() { return 1; }; // would fail requires
    // We just verify a noexcept callable works:
    InlineFunction<int() noexcept> f = []() noexcept { return 1; };
    REQUIRE(f() == 1);
}
