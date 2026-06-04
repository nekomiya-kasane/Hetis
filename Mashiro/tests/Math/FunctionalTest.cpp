/**
 * @file FunctionalTest.cpp
 * @brief Tests for Functional.h: value combinators (Meta) and type-level (Traits).
 */
#include "Mashiro/Math/Functional.h"

#include "Support/Meta.h"

#include <catch2/catch_approx.hpp>

#include <array>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

using namespace Mashiro::Meta;
using Catch::Approx;

namespace {

    struct Point {
        int x;
        int y;
    };

    constexpr int Inc(int v) { return v + 1; }

} // namespace

// =========================================================================
// §1  Pipe, Identity, Fn, Constant
// =========================================================================

TEST_CASE("Pipe applies a closure", AUTO_TAG) {
    auto sq = Fn([](int x) { return x * x; });
    STATIC_REQUIRE((5 | sq) == 25);
    STATIC_REQUIRE((42 | Identity) == 42);

    constexpr auto k = Constant(7);
    STATIC_REQUIRE((123 | k) == 7);
}

// =========================================================================
// §2  Map
// =========================================================================

TEST_CASE("Map over a tuple yields a tuple", AUTO_TAG) {
    constexpr auto r = std::tuple{1, 2, 3} | Map([](int x) { return x * 10; });
    STATIC_REQUIRE(std::get<0>(r) == 10);
    STATIC_REQUIRE(std::get<2>(r) == 30);
}

TEST_CASE("Map over an aggregate reconstructs its type", AUTO_TAG) {
    constexpr Point p = Point{1, 2} | Map(Inc);
    STATIC_REQUIRE(p.x == 2);
    STATIC_REQUIRE(p.y == 3);
}

TEST_CASE("Map over a range is lazy", AUTO_TAG) {
    std::vector<int> v{1, 2, 3, 4};
    auto view = v | Map([](int x) { return x * x; });
    std::vector<int> out(view.begin(), view.end());
    REQUIRE(out == std::vector<int>{1, 4, 9, 16});
}

// =========================================================================
// §3  Filter / Fold
// =========================================================================

TEST_CASE("Filter over a range is lazy", AUTO_TAG) {
    std::vector<int> v{1, 2, 3, 4, 5, 6};
    auto view = v | Filter([](int x) { return x % 2 == 0; });
    std::vector<int> out(view.begin(), view.end());
    REQUIRE(out == std::vector<int>{2, 4, 6});
}

TEST_CASE("Fold over a tuple (heterogeneous accumulator)", AUTO_TAG) {
    constexpr auto sum = std::tuple{1, 2, 3, 4} | Fold([](int a, int b) { return a + b; }, 0);
    STATIC_REQUIRE(sum == 10);

    // accumulator type changes: int seed, build a wider result
    constexpr auto sz = std::tuple{1, 2, 3} | Fold([](auto a, auto) { return a + 1; }, size_t{0});
    STATIC_REQUIRE(sz == 3);
}

TEST_CASE("Fold over a range", AUTO_TAG) {
    std::vector<int> v{1, 2, 3, 4};
    auto prod = v | Fold([](int a, int b) { return a * b; }, 1);
    REQUIRE(prod == 24);
}

// =========================================================================
// §4  Apply / MapApply / MapThread
// =========================================================================

TEST_CASE("Apply spreads a tuple", AUTO_TAG) {
    auto add = [](int a, int b, int c) { return a + b + c; };
    STATIC_REQUIRE((std::tuple{1, 2, 3} | Apply(add)) == 6);
    STATIC_REQUIRE(Apply(add, std::tuple{4, 5, 6}) == 15);
}

TEST_CASE("MapApply applies over each sub-tuple", AUTO_TAG) {
    auto mul = [](int a, int b) { return a * b; };
    constexpr auto r = std::tuple{std::tuple{2, 3}, std::tuple{4, 5}} | MapApply(mul);
    STATIC_REQUIRE(std::get<0>(r) == 6);
    STATIC_REQUIRE(std::get<1>(r) == 20);
}

TEST_CASE("MapThread zips across tuples", AUTO_TAG) {
    auto add = [](int a, int b) { return a + b; };
    constexpr auto r = MapThread(add, std::tuple{1, 2, 3}, std::tuple{10, 20, 30});
    STATIC_REQUIRE(std::get<0>(r) == 11);
    STATIC_REQUIRE(std::get<2>(r) == 33);
}

// =========================================================================
// §5  Compose / Then / Bind
// =========================================================================

TEST_CASE("Compose is right-to-left, Then is left-to-right", AUTO_TAG) {
    auto inc = [](int x) { return x + 1; };
    auto dbl = [](int x) { return x * 2; };

    STATIC_REQUIRE((Compose(inc, dbl))(5) == 11); // inc(dbl(5)) = 11
    STATIC_REQUIRE((Then(inc, dbl))(5) == 12);     // dbl(inc(5)) = 12

    STATIC_REQUIRE((5 | Compose(inc, dbl)) == 11);
}

TEST_CASE("Bind partially applies", AUTO_TAG) {
    auto add = [](int a, int b, int c) { return a + b + c; };
    auto add10 = Bind(add, 10);
    REQUIRE(add10(20, 30) == 60);
}

// =========================================================================
// §6  Rule / Assoc
// =========================================================================

TEST_CASE("Rule pairs key and value", AUTO_TAG) {
    constexpr auto r = Rule(1, 2.5);
    STATIC_REQUIRE(r.key == 1);
    STATIC_REQUIRE(r.value == 2.5);
}

TEST_CASE("Assoc with compile-time string keys", AUTO_TAG) {
    constexpr auto a = Assoc("x"_k = 1, "y"_k = 2.5, "z"_k = 'c');

    STATIC_REQUIRE(a["x"_k] == 1);
    STATIC_REQUIRE(a.Get<"y">() == 2.5);
    STATIC_REQUIRE(a["z"_k] == 'c');

    STATIC_REQUIRE(decltype(a)::Contains<"x">());
    STATIC_REQUIRE_FALSE(decltype(a)::Contains<"w">());
}

// =========================================================================
// §7  Reflection bridges
// =========================================================================

TEST_CASE("ToTuple converts a record to a tuple", AUTO_TAG) {
    constexpr auto t = ToTuple(Point{7, 8});
    STATIC_REQUIRE(std::get<0>(t) == 7);
    STATIC_REQUIRE(std::get<1>(t) == 8);
}

TEST_CASE("ForEachField visits members with names", AUTO_TAG) {
    Point p{3, 4};
    std::string names;
    int total = 0;
    ForEachField(p, [&](std::string_view name, int value) {
        names += name;
        total += value;
    });
    REQUIRE(names == "xy");
    REQUIRE(total == 7);
}

// =========================================================================
// §8  Type-level (Mashiro::Traits)
// =========================================================================

namespace {
    namespace T = Mashiro::Traits;

    using L = T::TypeList<int, double, char>;

    template<typename X>
    using AddPtr = X*;

    template<typename X>
    struct IsIntegral : std::bool_constant<std::is_integral_v<X>> {};

    template<typename... Xs>
    struct Sink {};

    template<typename Acc, typename X>
    using Append = T::Concat<Acc, T::TypeList<X>>;
} // namespace

TEST_CASE("TypeList basic queries", AUTO_TAG) {
    STATIC_REQUIRE(T::Length<L> == 3);
    STATIC_REQUIRE(std::is_same_v<T::At<L, 1>, double>);
    STATIC_REQUIRE(std::is_same_v<T::Head<L>, int>);
    STATIC_REQUIRE(std::is_same_v<T::Tail<L>, T::TypeList<double, char>>);
    STATIC_REQUIRE(T::IndexOf<L, char> == 2);
    STATIC_REQUIRE(T::Contains<L, double>);
    STATIC_REQUIRE_FALSE(T::Contains<L, float>);
}

TEST_CASE("TypeList Concat / Map / Filter / Apply / Fold", AUTO_TAG) {
    STATIC_REQUIRE(std::is_same_v<T::Concat<T::TypeList<int>, T::TypeList<char, bool>>,
                                  T::TypeList<int, char, bool>>);
    STATIC_REQUIRE(std::is_same_v<T::MapT<AddPtr, T::TypeList<int, char>>,
                                  T::TypeList<int*, char*>>);
    STATIC_REQUIRE(std::is_same_v<T::FilterT<IsIntegral, L>, T::TypeList<int, char>>);
    STATIC_REQUIRE(std::is_same_v<T::ApplyT<Sink, L>, Sink<int, double, char>>);
    STATIC_REQUIRE(std::is_same_v<T::FoldT<Append, T::TypeList<>, T::TypeList<int, char>>,
                                  T::TypeList<int, char>>);
}

TEST_CASE("ToTypeList reflects a record's member types", AUTO_TAG) {
    STATIC_REQUIRE(std::is_same_v<T::ToTypeList<Point>, T::TypeList<int, int>>);
}
