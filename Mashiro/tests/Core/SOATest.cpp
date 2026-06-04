/**
 * @file SOATest.cpp
 * @brief Comprehensive tests for SOA.h: ToSoA type generation and Array runtime container.
 */
#include "Mashiro/Core/SOA.h"

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::SoA;

// =========================================================================
// Test fixture types
// =========================================================================

struct Particle {
    float x;
    float y;
    float z;
    float life;
};

struct WithSkip {
    int    kept1;
    double kept2;
    [[=Skip{}]] int skipped;
    float  kept3;
};

struct Trivial {
    int a;
    int b;
};

struct SingleField {
    double value;
};

struct WithString {
    int id;
    std::string name;
};

// =========================================================================
// §1  SoA::Define + SoAType — compile-time type generation
// =========================================================================

consteval { SoA::Define<Particle, 4>(); }
consteval { SoA::Define<Particle, 16>(); }
consteval { SoA::Define<WithSkip, 8>(); }
consteval { SoA::Define<SingleField, 2>(); }
consteval { SoA::Define<Trivial, 1>(); }
consteval { SoA::Define<Trivial, 256>(); }

TEST_CASE("SoAType generates correct aggregate layout", AUTO_TAG) {
    using P = SoAType<Particle, 4>;

    STATIC_REQUIRE(std::is_aggregate_v<P>);

    P data{};
    STATIC_REQUIRE(std::is_same_v<decltype(data.x), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(data.y), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(data.z), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(data.life), std::array<float, 4>>);
}

TEST_CASE("SoAType respects Skip annotation", AUTO_TAG) {
    using S = SoAType<WithSkip, 8>;

    S data{};
    STATIC_REQUIRE(std::is_same_v<decltype(data.kept1), std::array<int, 8>>);
    STATIC_REQUIRE(std::is_same_v<decltype(data.kept2), std::array<double, 8>>);
    STATIC_REQUIRE(std::is_same_v<decltype(data.kept3), std::array<float, 8>>);

    // 'skipped' should NOT exist — 3 fields, not 4
    STATIC_REQUIRE(Traits::MembersCount<S> == 3);
}

TEST_CASE("SoAType field access by name", AUTO_TAG) {
    using P = SoAType<Particle, 16>;
    P data{};

    data.x[0] = 1.0f;
    data.y[0] = 2.0f;
    data.z[0] = 3.0f;
    data.life[0] = 10.0f;

    REQUIRE(data.x[0] == 1.0f);
    REQUIRE(data.y[0] == 2.0f);
    REQUIRE(data.z[0] == 3.0f);
    REQUIRE(data.life[0] == 10.0f);
}

TEST_CASE("SoAType single-field struct", AUTO_TAG) {
    using S = SoAType<SingleField, 2>;
    S data{};
    data.value[0] = 3.14;
    data.value[1] = 2.71;
    REQUIRE(data.value[0] == 3.14);
    REQUIRE(data.value[1] == 2.71);
}

TEST_CASE("SoAType with different N values", AUTO_TAG) {
    using P1 = SoAType<Trivial, 1>;
    using P256 = SoAType<Trivial, 256>;

    STATIC_REQUIRE(sizeof(P1) == 2 * sizeof(std::array<int, 1>));
    STATIC_REQUIRE(sizeof(P256) == 2 * sizeof(std::array<int, 256>));
}

// =========================================================================
// §2  SoA::Array — construction and capacity
// =========================================================================

TEST_CASE("Array default-constructs empty", AUTO_TAG) {
    Array<Particle> a;
    REQUIRE(a.Size() == 0);
    REQUIRE(a.Capacity() == 0);
    REQUIRE(a.Empty());
}

TEST_CASE("Array::Reserve allocates without changing size", AUTO_TAG) {
    Array<Particle> a;
    a.Reserve(100);
    REQUIRE(a.Size() == 0);
    REQUIRE(a.Capacity() >= 100);
    REQUIRE(a.Empty());
}

TEST_CASE("Array::Resize grows and default-constructs", AUTO_TAG) {
    Array<Particle> a;
    a.Resize(10);
    REQUIRE(a.Size() == 10);
    REQUIRE(a.Capacity() >= 10);
    REQUIRE_FALSE(a.Empty());

    // default-constructed floats should be 0
    REQUIRE(a.Get<"x">(0) == 0.0f);
    REQUIRE(a.Get<"life">(9) == 0.0f);
}

TEST_CASE("Array::Resize shrinks and destroys", AUTO_TAG) {
    Array<Particle> a;
    a.Resize(100);
    a.Resize(10);
    REQUIRE(a.Size() == 10);
}

TEST_CASE("Array::Clear resets size to zero", AUTO_TAG) {
    Array<Particle> a;
    a.Resize(50);
    a.Clear();
    REQUIRE(a.Size() == 0);
    REQUIRE(a.Empty());
    REQUIRE(a.Capacity() >= 50); // memory retained
}

// =========================================================================
// §3  SoA::Array — Push / Get / Field
// =========================================================================

TEST_CASE("Array::Push distributes fields correctly", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1.0f, 2.0f, 3.0f, 100.0f});
    a.Push(Particle{4.0f, 5.0f, 6.0f, 200.0f});

    REQUIRE(a.Size() == 2);
    REQUIRE(a.Get<"x">(0) == 1.0f);
    REQUIRE(a.Get<"y">(0) == 2.0f);
    REQUIRE(a.Get<"z">(0) == 3.0f);
    REQUIRE(a.Get<"life">(0) == 100.0f);
    REQUIRE(a.Get<"x">(1) == 4.0f);
    REQUIRE(a.Get<"life">(1) == 200.0f);
}

TEST_CASE("Array::Push move semantics", AUTO_TAG) {
    Array<WithString> a;
    WithString ws{42, "hello"};
    a.Push(std::move(ws));
    REQUIRE(a.Size() == 1);
    REQUIRE(a.Get<"id">(0) == 42);
    REQUIRE(a.Get<"name">(0) == "hello");
}

TEST_CASE("Array::Get mutable access", AUTO_TAG) {
    Array<Particle> a;
    a.Resize(1);
    a.Get<"x">(0) = 42.0f;
    a.Get<"life">(0) = 99.0f;
    REQUIRE(a.Get<"x">(0) == 42.0f);
    REQUIRE(a.Get<"life">(0) == 99.0f);
}

TEST_CASE("Array::Get const access", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1, 2, 3, 4});
    const auto& ca = a;
    REQUIRE(ca.Get<"x">(0) == 1.0f);
    REQUIRE(ca.Get<"life">(0) == 4.0f);
}

TEST_CASE("Array::Field returns span over entire field array", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1, 0, 0, 10});
    a.Push(Particle{2, 0, 0, 20});
    a.Push(Particle{3, 0, 0, 30});

    auto xs = a.Field<"x">();
    REQUIRE(xs.size() == 3);
    REQUIRE(xs[0] == 1.0f);
    REQUIRE(xs[1] == 2.0f);
    REQUIRE(xs[2] == 3.0f);

    // mutate via span
    for (auto& v : xs) v *= 10.0f;
    REQUIRE(a.Get<"x">(0) == 10.0f);
    REQUIRE(a.Get<"x">(2) == 30.0f);
}

TEST_CASE("Array::Field const span", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1, 2, 3, 4});
    const auto& ca = a;
    auto lifes = ca.Field<"life">();
    REQUIRE(lifes.size() == 1);
    REQUIRE(lifes[0] == 4.0f);
}

// =========================================================================
// §4  SoA::Array — Gather (SoA → AoS reconstruction)
// =========================================================================

TEST_CASE("Array::Gather reconstructs AoS element", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1.0f, 2.0f, 3.0f, 99.0f});

    Particle p = a.Gather(0);
    REQUIRE(p.x == 1.0f);
    REQUIRE(p.y == 2.0f);
    REQUIRE(p.z == 3.0f);
    REQUIRE(p.life == 99.0f);
}

TEST_CASE("Array Push then Gather roundtrips", AUTO_TAG) {
    Array<Particle> a;
    for (int i = 0; i < 50; ++i) {
        float f = static_cast<float>(i);
        a.Push(Particle{f, f * 2, f * 3, f * 4});
    }

    for (int i = 0; i < 50; ++i) {
        float f = static_cast<float>(i);
        Particle p = a.Gather(i);
        REQUIRE(p.x == f);
        REQUIRE(p.y == f * 2);
        REQUIRE(p.z == f * 3);
        REQUIRE(p.life == f * 4);
    }
}

// =========================================================================
// §5  SoA::Array — ForEach
// =========================================================================

TEST_CASE("Array::ForEach visits all fields of element", AUTO_TAG) {
    Array<Trivial> a;
    a.Push(Trivial{10, 20});

    std::vector<std::string> names;
    int total = 0;
    a.ForEach(0, [&](std::string_view name, int value) {
        names.emplace_back(name);
        total += value;
    });
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "a");
    REQUIRE(names[1] == "b");
    REQUIRE(total == 30);
}

TEST_CASE("Array::ForEach const", AUTO_TAG) {
    Array<Trivial> a;
    a.Push(Trivial{5, 7});
    const auto& ca = a;

    int total = 0;
    ca.ForEach(0, [&](std::string_view, int value) {
        total += value;
    });
    REQUIRE(total == 12);
}

// =========================================================================
// §6  SoA::Array — SwapRemove
// =========================================================================

TEST_CASE("Array::SwapRemove removes last element", AUTO_TAG) {
    Array<Trivial> a;
    a.Push(Trivial{1, 2});
    a.SwapRemove(0);
    REQUIRE(a.Size() == 0);
    REQUIRE(a.Empty());
}

TEST_CASE("Array::SwapRemove swaps with last", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1, 0, 0, 0});
    a.Push(Particle{2, 0, 0, 0});
    a.Push(Particle{3, 0, 0, 0});

    a.SwapRemove(0); // removes 1, moves 3 into slot 0
    REQUIRE(a.Size() == 2);
    REQUIRE(a.Get<"x">(0) == 3.0f); // 3 moved to front
    REQUIRE(a.Get<"x">(1) == 2.0f); // 2 untouched
}

TEST_CASE("Array::SwapRemove out-of-range is no-op", AUTO_TAG) {
    Array<Trivial> a;
    a.Push(Trivial{1, 2});
    a.SwapRemove(999);
    REQUIRE(a.Size() == 1);
}

// =========================================================================
// §7  SoA::Array — Skip annotation
// =========================================================================

TEST_CASE("Array respects Skip annotation", AUTO_TAG) {
    Array<WithSkip> a;
    STATIC_REQUIRE(decltype(a)::FieldCount == 3); // 4 members - 1 skipped

    a.Push(WithSkip{10, 2.5, 999, 3.14f});
    REQUIRE(a.Get<"kept1">(0) == 10);
    REQUIRE(a.Get<"kept2">(0) == 2.5);
    REQUIRE(a.Get<"kept3">(0) == 3.14f);
}

// =========================================================================
// §8  SoA::Array — move semantics
// =========================================================================

TEST_CASE("Array move constructor", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1, 2, 3, 4});
    a.Push(Particle{5, 6, 7, 8});

    Array<Particle> b = std::move(a);
    REQUIRE(b.Size() == 2);
    REQUIRE(b.Get<"x">(0) == 1.0f);
    REQUIRE(b.Get<"x">(1) == 5.0f);
    REQUIRE(a.Size() == 0); // NOLINT: testing moved-from state
}

TEST_CASE("Array move assignment", AUTO_TAG) {
    Array<Particle> a;
    a.Push(Particle{1, 2, 3, 4});

    Array<Particle> b;
    b.Push(Particle{10, 20, 30, 40});
    b.Push(Particle{50, 60, 70, 80});

    b = std::move(a);
    REQUIRE(b.Size() == 1);
    REQUIRE(b.Get<"x">(0) == 1.0f);
}

// =========================================================================
// §9  SoA::Array — growth and reallocation
// =========================================================================

TEST_CASE("Array grows correctly across multiple reallocs", AUTO_TAG) {
    Array<Particle> a;
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        float f = static_cast<float>(i);
        a.Push(Particle{f, f, f, f});
    }
    REQUIRE(a.Size() == N);
    REQUIRE(a.Capacity() >= static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        REQUIRE(a.Get<"x">(i) == static_cast<float>(i));
    }
}

// =========================================================================
// §10  SoA::Array — non-trivial types (std::string)
// =========================================================================

TEST_CASE("Array handles non-trivial types", AUTO_TAG) {
    Array<WithString> a;
    a.Push(WithString{1, "alpha"});
    a.Push(WithString{2, "beta"});
    a.Push(WithString{3, "gamma"});

    REQUIRE(a.Get<"name">(0) == "alpha");
    REQUIRE(a.Get<"name">(1) == "beta");
    REQUIRE(a.Get<"name">(2) == "gamma");

    a.SwapRemove(0);
    REQUIRE(a.Size() == 2);
    REQUIRE(a.Get<"name">(0) == "gamma");
    REQUIRE(a.Get<"name">(1) == "beta");

    // Gather roundtrip
    WithString ws = a.Gather(0);
    REQUIRE(ws.id == 3);
    REQUIRE(ws.name == "gamma");
}

TEST_CASE("Array Clear destroys non-trivial elements", AUTO_TAG) {
    Array<WithString> a;
    for (int i = 0; i < 100; ++i)
        a.Push(WithString{i, std::string(100, 'x')});
    a.Clear(); // must not leak
    REQUIRE(a.Size() == 0);
}
