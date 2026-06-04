/**
 * @file FunctionalTest.cpp
 * @brief Tests for Functional.h: value combinators (Meta) and type-level (Traits).
 */
#include "Mashiro/Core/Functional.h"

#include "Support/Meta.h"

#include <catch2/catch_approx.hpp>

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Traits;
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
    STATIC_REQUIRE(L::size == 3);
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

// =========================================================================
// §9  Extended: chained pipes and multi-step pipelines
// =========================================================================

TEST_CASE("Chained pipes compose left-to-right", AUTO_TAG) {
    auto inc = Fn([](int x) { return x + 1; });
    auto dbl = Fn([](int x) { return x * 2; });
    auto neg = Fn([](int x) { return -x; });

    STATIC_REQUIRE((3 | inc | dbl | neg) == -8); // neg(dbl(inc(3))) = neg(dbl(4)) = neg(8) = -8
}

TEST_CASE("Pipe with Map and Fold chained", AUTO_TAG) {
    std::vector<int> v{1, 2, 3, 4, 5};
    auto result = v | Map([](int x) { return x * x; })
                    | Fold([](int a, int b) { return a + b; }, 0);
    REQUIRE(result == 55); // 1+4+9+16+25
}

// =========================================================================
// §10  Extended: Compose / Then with 3+ functions
// =========================================================================

TEST_CASE("Compose with 3 functions", AUTO_TAG) {
    auto f = [](int x) { return x + 1; };
    auto g = [](int x) { return x * 2; };
    auto h = [](int x) { return x - 3; };

    // Compose(f, g, h)(x) = f(g(h(x))) = (x-3)*2+1
    STATIC_REQUIRE(Compose(f, g, h)(10) == 15);
}

TEST_CASE("Then with 3 functions", AUTO_TAG) {
    auto f = [](int x) { return x + 1; };
    auto g = [](int x) { return x * 2; };
    auto h = [](int x) { return x - 3; };

    // Then(f, g, h)(x) = h(g(f(x))) = (x+1)*2-3
    STATIC_REQUIRE(Then(f, g, h)(10) == 19);
}

// =========================================================================
// §11  Extended: Map with type-changing lambda
// =========================================================================

TEST_CASE("Map over tuple with heterogeneous type change", AUTO_TAG) {
    constexpr auto r = std::tuple{1, 2.5, 'a'} | Map([](auto x) { return static_cast<double>(x); });
    STATIC_REQUIRE(std::get<0>(r) == 1.0);
    STATIC_REQUIRE(std::get<1>(r) == 2.5);
    STATIC_REQUIRE(std::get<2>(r) == 97.0); // 'a' = 97
}

TEST_CASE("Map over single-element tuple", AUTO_TAG) {
    constexpr auto r = std::tuple{42} | Map([](int x) { return x * 2; });
    STATIC_REQUIRE(std::get<0>(r) == 84);
}

// =========================================================================
// §12  Extended: Fold edge cases
// =========================================================================

TEST_CASE("Fold over single-element tuple", AUTO_TAG) {
    constexpr auto r = std::tuple{42} | Fold([](int a, int b) { return a + b; }, 0);
    STATIC_REQUIRE(r == 42);
}

TEST_CASE("Fold over empty range returns seed", AUTO_TAG) {
    std::vector<int> empty;
    auto r = empty | Fold([](int a, int b) { return a + b; }, 99);
    REQUIRE(r == 99);
}

TEST_CASE("Fold builds a string from range", AUTO_TAG) {
    std::vector<int> v{1, 2, 3};
    auto r = v | Fold([](std::string acc, int x) {
        return acc.empty() ? std::to_string(x) : acc + "," + std::to_string(x);
    }, std::string{});
    REQUIRE(r == "1,2,3");
}

// =========================================================================
// §13  Extended: Filter edge cases
// =========================================================================

TEST_CASE("Filter with no matches yields empty range", AUTO_TAG) {
    std::vector<int> v{1, 3, 5, 7};
    auto view = v | Filter([](int x) { return x % 2 == 0; });
    std::vector<int> out(view.begin(), view.end());
    REQUIRE(out.empty());
}

TEST_CASE("Filter with all matching yields all", AUTO_TAG) {
    std::vector<int> v{2, 4, 6};
    auto view = v | Filter([](int x) { return x % 2 == 0; });
    std::vector<int> out(view.begin(), view.end());
    REQUIRE(out == std::vector<int>{2, 4, 6});
}

// =========================================================================
// §14  Extended: Apply edge cases
// =========================================================================

TEST_CASE("Apply with single-element tuple", AUTO_TAG) {
    auto negate = [](int x) { return -x; };
    STATIC_REQUIRE((std::tuple{42} | Apply(negate)) == -42);
}

// =========================================================================
// §15  Extended: MapThread with 3-way zip
// =========================================================================

TEST_CASE("MapThread with 3 tuples", AUTO_TAG) {
    auto sum3 = [](int a, int b, int c) { return a + b + c; };
    constexpr auto r = MapThread(sum3,
        std::tuple{1, 2}, std::tuple{10, 20}, std::tuple{100, 200});
    STATIC_REQUIRE(std::get<0>(r) == 111);
    STATIC_REQUIRE(std::get<1>(r) == 222);
}

// =========================================================================
// §16  Extended: Assoc edge cases
// =========================================================================

TEST_CASE("Assoc with single key", AUTO_TAG) {
    constexpr auto a = Assoc("only"_k = 42);
    STATIC_REQUIRE(a["only"_k] == 42);
    STATIC_REQUIRE(decltype(a)::Contains<"only">());
    STATIC_REQUIRE_FALSE(decltype(a)::Contains<"nope">());
}

TEST_CASE("Assoc values are mutable", AUTO_TAG) {
    auto a = Assoc("x"_k = 1, "y"_k = 2);
    a["x"_k] = 100;
    REQUIRE(a["x"_k] == 100);
    REQUIRE(a["y"_k] == 2);
}

TEST_CASE("Assoc with heterogeneous value types", AUTO_TAG) {
    constexpr auto a = Assoc(
        "name"_k = FixedString("hello"),
        "id"_k = 42,
        "ratio"_k = 3.14
    );
    STATIC_REQUIRE(a["id"_k] == 42);
    STATIC_REQUIRE(a["ratio"_k] == 3.14);
}

// =========================================================================
// §17  Extended: ForEachField mutation
// =========================================================================

TEST_CASE("ForEachField can mutate members", AUTO_TAG) {
    Point p{10, 20};
    ForEachField(p, [](std::string_view, int& value) {
        value *= 3;
    });
    REQUIRE(p.x == 30);
    REQUIRE(p.y == 60);
}

namespace {
    struct RGB { float r; float g; float b; };
}

TEST_CASE("ForEachField on a 3-member aggregate", AUTO_TAG) {
    RGB c{0.1f, 0.2f, 0.3f};
    int count = 0;
    float total = 0.0f;
    ForEachField(c, [&](std::string_view, float value) {
        ++count;
        total += value;
    });
    REQUIRE(count == 3);
    REQUIRE(total == Approx(0.6f));
}

// =========================================================================
// §18  Extended: ToTuple roundtrip
// =========================================================================

TEST_CASE("ToTuple preserves all fields", AUTO_TAG) {
    RGB c{1.0f, 2.0f, 3.0f};
    auto t = ToTuple(c);
    STATIC_REQUIRE(std::tuple_size_v<decltype(t)> == 3);
    REQUIRE(std::get<0>(t) == 1.0f);
    REQUIRE(std::get<1>(t) == 2.0f);
    REQUIRE(std::get<2>(t) == 3.0f);
}

// =========================================================================
// §19  Extended: TypeList edge cases
// =========================================================================

namespace {
    using Empty = T::TypeList<>;
    using One   = T::TypeList<int>;
}

TEST_CASE("TypeList empty list operations", AUTO_TAG) {
    STATIC_REQUIRE(Empty::size == 0);
    STATIC_REQUIRE(std::is_same_v<T::Concat<Empty, Empty>, Empty>);
    STATIC_REQUIRE(std::is_same_v<T::Concat<Empty, One>, One>);
    STATIC_REQUIRE(T::IndexOf<Empty, int> == size_t(-1));
    STATIC_REQUIRE_FALSE(T::Contains<Empty, int>);
}

TEST_CASE("TypeList single-element", AUTO_TAG) {
    STATIC_REQUIRE(One::size == 1);
    STATIC_REQUIRE(std::is_same_v<T::Head<One>, int>);
    STATIC_REQUIRE(std::is_same_v<T::Tail<One>, Empty>);
    STATIC_REQUIRE(T::IndexOf<One, int> == 0);
}

TEST_CASE("TypeList Concat multiple lists", AUTO_TAG) {
    using A = T::TypeList<int>;
    using B = T::TypeList<double, float>;
    using C = T::TypeList<char>;
    STATIC_REQUIRE(std::is_same_v<T::Concat<A, B, C>,
                                  T::TypeList<int, double, float, char>>);
}

TEST_CASE("TypeList FilterT with all matching", AUTO_TAG) {
    using AllInt = T::TypeList<int, short, long>;
    STATIC_REQUIRE(std::is_same_v<T::FilterT<IsIntegral, AllInt>, AllInt>);
}

TEST_CASE("TypeList FilterT with none matching", AUTO_TAG) {
    using AllFloat = T::TypeList<float, double>;
    STATIC_REQUIRE(std::is_same_v<T::FilterT<IsIntegral, AllFloat>, Empty>);
}

TEST_CASE("TypeList IndexOf not found returns -1", AUTO_TAG) {
    STATIC_REQUIRE(T::IndexOf<L, float> == size_t(-1));
    STATIC_REQUIRE(T::IndexOf<L, bool> == size_t(-1));
}

// =========================================================================
// §20  Constexpr correctness of all value combinators
// =========================================================================

TEST_CASE("All value combinators are constexpr-friendly", AUTO_TAG) {
    constexpr auto pipe_result = 10 | Fn([](int x) { return x + 5; });
    STATIC_REQUIRE(pipe_result == 15);

    constexpr auto map_result = std::tuple{1, 2} | Map([](int x) { return x * 3; });
    STATIC_REQUIRE(std::get<0>(map_result) == 3);
    STATIC_REQUIRE(std::get<1>(map_result) == 6);

    constexpr auto fold_result = std::tuple{1, 2, 3} | Fold([](int a, int b) { return a + b; }, 0);
    STATIC_REQUIRE(fold_result == 6);

    constexpr auto apply_result = std::tuple{2, 3} | Apply([](int a, int b) { return a * b; });
    STATIC_REQUIRE(apply_result == 6);

    constexpr auto compose_result = Compose([](int x) { return x + 1; }, [](int x) { return x * 2; })(5);
    STATIC_REQUIRE(compose_result == 11);

    constexpr auto then_result = Then([](int x) { return x + 1; }, [](int x) { return x * 2; })(5);
    STATIC_REQUIRE(then_result == 12);

    constexpr auto identity_result = 42 | Identity;
    STATIC_REQUIRE(identity_result == 42);

    constexpr auto const_result = 0 | Constant(777);
    STATIC_REQUIRE(const_result == 777);
}

// =========================================================================
// §21  Fallible pipe adaptors — std::expected
// =========================================================================

namespace {
    using Res = std::expected<int, int>;

    constexpr Res SafeDouble(int x) {
        return (x > 100) ? Res{std::unexpect, -1} : Res{x * 2};
    }
}

TEST_CASE("AndThen on expected success", AUTO_TAG) {
    constexpr Res ok{5};
    constexpr auto r = ok | AndThen(SafeDouble);
    STATIC_REQUIRE(r.has_value());
    STATIC_REQUIRE(r.value() == 10);
}

TEST_CASE("AndThen on expected error propagates", AUTO_TAG) {
    constexpr Res err{std::unexpect, -99};
    constexpr auto r = err | AndThen(SafeDouble);
    STATIC_REQUIRE(!r.has_value());
    STATIC_REQUIRE(r.error() == -99);
}

TEST_CASE("AndThen chain short-circuits on first error", AUTO_TAG) {
    constexpr auto r = Res{60}
        | AndThen(SafeDouble)    // 120
        | AndThen(SafeDouble);   // 120 > 100 → error
    STATIC_REQUIRE(!r.has_value());
    STATIC_REQUIRE(r.error() == -1);
}

TEST_CASE("Transform on expected success", AUTO_TAG) {
    constexpr Res ok{7};
    constexpr auto r = ok | Transform([](int x) { return x + 3; });
    STATIC_REQUIRE(r.has_value());
    STATIC_REQUIRE(r.value() == 10);
}

TEST_CASE("Transform on expected error propagates", AUTO_TAG) {
    constexpr Res err{std::unexpect, -5};
    constexpr auto r = err | Transform([](int x) { return x + 3; });
    STATIC_REQUIRE(!r.has_value());
    STATIC_REQUIRE(r.error() == -5);
}

TEST_CASE("OrElse recovers from expected error", AUTO_TAG) {
    constexpr Res err{std::unexpect, -1};
    constexpr auto r = err | OrElse([](int) -> Res { return 42; });
    STATIC_REQUIRE(r.has_value());
    STATIC_REQUIRE(r.value() == 42);
}

TEST_CASE("OrElse is no-op on expected success", AUTO_TAG) {
    constexpr Res ok{10};
    constexpr auto r = ok | OrElse([](int) -> Res { return 999; });
    STATIC_REQUIRE(r.has_value());
    STATIC_REQUIRE(r.value() == 10);
}

TEST_CASE("ValueOr extracts or provides default for expected", AUTO_TAG) {
    constexpr Res ok{7};
    constexpr Res err{std::unexpect, -1};
    STATIC_REQUIRE((ok  | ValueOr(0)) == 7);
    STATIC_REQUIRE((err | ValueOr(0)) == 0);
}

// =========================================================================
// §22  Fallible pipe adaptors — std::optional
// =========================================================================

TEST_CASE("AndThen on optional engaged", AUTO_TAG) {
    constexpr std::optional<int> val{5};
    constexpr auto r = val | AndThen([](int x) -> std::optional<int> {
        return x * 3;
    });
    STATIC_REQUIRE(r.has_value());
    STATIC_REQUIRE(r.value() == 15);
}

TEST_CASE("AndThen on optional nullopt propagates", AUTO_TAG) {
    constexpr std::optional<int> empty;
    constexpr auto r = empty | AndThen([](int x) -> std::optional<int> { return x; });
    STATIC_REQUIRE(!r.has_value());
}

TEST_CASE("Transform on optional", AUTO_TAG) {
    constexpr std::optional<int> val{4};
    constexpr auto r = val | Transform([](int x) { return x * x; });
    STATIC_REQUIRE(r.value() == 16);
}

TEST_CASE("ValueOr on optional", AUTO_TAG) {
    constexpr std::optional<int> val{42};
    constexpr std::optional<int> empty;
    STATIC_REQUIRE((val   | ValueOr(-1)) == 42);
    STATIC_REQUIRE((empty | ValueOr(-1)) == -1);
}

// =========================================================================
// §23  Mixed expected + Transform + AndThen pipeline
// =========================================================================

TEST_CASE("Full expected pipeline: transform then andthen", AUTO_TAG) {
    constexpr auto r = Res{3}
        | Transform([](int x) { return x + 7; })   // 10
        | AndThen(SafeDouble)                        // 20
        | Transform([](int x) { return x - 1; });   // 19
    STATIC_REQUIRE(r.has_value());
    STATIC_REQUIRE(r.value() == 19);
}

// =========================================================================
// §24  Scan (prefix accumulation)
// =========================================================================

TEST_CASE("Scan produces running sums", AUTO_TAG) {
    std::vector<int> v{1, 2, 3, 4};
    auto sums = v | Scan([](int a, int b) { return a + b; }, 0);
    std::vector<int> out(sums.begin(), sums.end());
    REQUIRE(out == std::vector<int>{1, 3, 6, 10});
}

TEST_CASE("Scan produces running products", AUTO_TAG) {
    std::vector<int> v{1, 2, 3, 4};
    auto prods = v | Scan([](int a, int b) { return a * b; }, 1);
    std::vector<int> out(prods.begin(), prods.end());
    REQUIRE(out == std::vector<int>{1, 2, 6, 24});
}

TEST_CASE("Scan over empty range", AUTO_TAG) {
    std::vector<int> empty;
    auto sums = empty | Scan([](int a, int b) { return a + b; }, 0);
    std::vector<int> out(sums.begin(), sums.end());
    REQUIRE(out.empty());
}

TEST_CASE("Scan over single element", AUTO_TAG) {
    std::vector<int> v{42};
    auto sums = v | Scan([](int a, int b) { return a + b; }, 0);
    std::vector<int> out(sums.begin(), sums.end());
    REQUIRE(out == std::vector<int>{42});
}

// =========================================================================
// §25  Zip / ZipWith
// =========================================================================

TEST_CASE("Zip two ranges into pairs", AUTO_TAG) {
    std::vector<int> a{1, 2, 3};
    std::vector<double> b{10.0, 20.0, 30.0};
    auto z = Zip(a, b);
    std::vector<std::tuple<int, double>> out;
    for (auto [x, y] : z) out.emplace_back(x, y);
    REQUIRE(out.size() == 3);
    REQUIRE(std::get<0>(out[0]) == 1);
    REQUIRE(std::get<1>(out[2]) == 30.0);
}

TEST_CASE("Zip truncates to shortest", AUTO_TAG) {
    std::vector<int> a{1, 2, 3, 4, 5};
    std::vector<int> b{10, 20};
    auto z = Zip(a, b);
    int count = 0;
    for (auto [x, y] : z) ++count;
    REQUIRE(count == 2);
}

TEST_CASE("ZipWith via pipe", AUTO_TAG) {
    std::vector<int> a{1, 2, 3};
    std::vector<int> b{10, 20, 30};
    auto z = a | ZipWith(b);
    std::vector<int> sums;
    for (auto [x, y] : z) sums.push_back(x + y);
    REQUIRE(sums == std::vector<int>{11, 22, 33});
}

TEST_CASE("ZipWith then Map", AUTO_TAG) {
    std::vector<int> a{1, 2, 3};
    std::vector<int> b{10, 20, 30};
    auto products = a | ZipWith(b) | Map([](auto p) {
        auto [x, y] = p;
        return x * y;
    });
    std::vector<int> out(products.begin(), products.end());
    REQUIRE(out == std::vector<int>{10, 40, 90});
}

TEST_CASE("Zip three ranges", AUTO_TAG) {
    std::vector<int> a{1, 2};
    std::vector<int> b{10, 20};
    std::vector<int> c{100, 200};
    auto z = Zip(a, b, c);
    std::vector<int> sums;
    for (auto [x, y, w] : z) sums.push_back(x + y + w);
    REQUIRE(sums == std::vector<int>{111, 222});
}

// =========================================================================
// §26  Enumerate
// =========================================================================

TEST_CASE("Enumerate produces (index, value) pairs", AUTO_TAG) {
    std::vector<std::string> v{"a", "b", "c"};
    std::vector<size_t> indices;
    std::string concat;
    for (auto [i, s] : v | Enumerate()) {
        indices.push_back(i);
        concat += s;
    }
    REQUIRE(indices == std::vector<size_t>{0, 1, 2});
    REQUIRE(concat == "abc");
}

TEST_CASE("Enumerate with custom start", AUTO_TAG) {
    std::vector<int> v{10, 20, 30};
    std::vector<int> indices;
    for (auto [i, val] : v | Enumerate(100)) {
        indices.push_back(i);
    }
    REQUIRE(indices == std::vector<int>{100, 101, 102});
}

TEST_CASE("Enumerate over empty range", AUTO_TAG) {
    std::vector<int> empty;
    int count = 0;
    for (auto [i, v] : empty | Enumerate()) ++count;
    REQUIRE(count == 0);
}

TEST_CASE("Enumerate then Map", AUTO_TAG) {
    std::vector<int> v{10, 20, 30};
    auto weighted = v | Enumerate() | Map([](auto pair) {
        auto [i, val] = pair;
        return static_cast<int>(i) * val;
    });
    std::vector<int> out;
    for (auto x : weighted) out.push_back(x);
    REQUIRE(out == std::vector<int>{0, 20, 60});
}
