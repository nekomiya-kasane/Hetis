#include <Sora/Core/Functional.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    struct MutableOnly {
        constexpr int operator()(int value) & noexcept { return value + 1; }
        int operator()(int) const& = delete;
        int operator()(int) && = delete;
    };

    struct RvalueOnly {
        constexpr int operator()(int value) && noexcept { return value + 1; }
    };

    struct ValueTag {};

    struct ReferenceAccumulator {
        constexpr int&& operator()(int&& accumulator, int value) const noexcept {
            accumulator += value;
            return std::move(accumulator);
        }
    };

    struct Row {
        int id;
        std::string name;
    };

    class PrivateRecord {
      public:
        constexpr explicit PrivateRecord(int value) : value_(value) {}

      private:
        int value_;
    };

    template<typename Range>
    auto Collect(Range&& range) {
        using Value = std::ranges::range_value_t<Range>;
        std::vector<Value> result;
        for (auto&& value : range) {
            result.emplace_back(value);
        }
        return result;
    }

    template<typename F, typename First, typename... Rest>
    concept CanMapThread = requires(F&& function, First&& first, Rest&&... rest) {
        Sora::MapThread(std::forward<F>(function), std::forward<First>(first), std::forward<Rest>(rest)...);
    };

    using Sora::operator""_k;

    constexpr auto kDuplicateA = "duplicate"_k = 1;
    constexpr auto kDuplicateB = "duplicate"_k = 2;

    template<typename A, typename B>
    concept CanAssociate = requires(A&& a, B&& b) {
        Sora::Assoc{std::forward<A>(a), std::forward<B>(b)};
    };

    template<typename T>
    concept CanMap = requires(T&& value) {
        std::forward<T>(value) | Sora::Map(std::identity{});
    };

    template<typename Left, typename Right>
    concept CanSoraPipe = requires(Left&& left, Right&& right) {
        Sora::operator|(std::forward<Left>(left), std::forward<Right>(right));
    };

    template<typename Left, typename Right>
    concept CanPipeSyntax = requires(Left&& left, Right&& right) {
        std::forward<Left>(left) | std::forward<Right>(right);
    };

    using ChunkedVector = decltype(std::declval<std::vector<int>&>() | Sora::View::Chunk(2));
    using SlidingVector = decltype(std::declval<std::vector<int>&>() | Sora::View::Slide(2));
    using UnaryClosure = decltype(Sora::Fn([](int value) { return value + 1; }));
    using BinaryClosure = decltype(Sora::Fn([](int value) { return value * 2; }));
    using RvalueClosure = decltype(Sora::Fn(RvalueOnly{}));
    using StringClosure = decltype(Sora::Fn([](std::string value) { return value.size(); }));
    using MoveOnlyClosure = decltype(Sora::Fn(
        [state = std::make_unique<int>(2)](int value) { return value * *state; }));
    using InvalidComposition = decltype(std::declval<UnaryClosure>() | std::declval<StringClosure>());

    static_assert(Sora::Concept::AdaptorClosure<decltype(Sora::Fn([] {}))>);
    static_assert(std::invocable<decltype(Sora::Fn(MutableOnly{}))&, int>);
    static_assert(!std::invocable<const decltype(Sora::Fn(MutableOnly{}))&, int>);
    static_assert(!std::invocable<decltype(Sora::Fn(MutableOnly{}))&&, int>);
    static_assert(std::invocable<decltype(Sora::Fn(RvalueOnly{}))&&, int>);
    static_assert(!std::invocable<decltype(Sora::Fn(RvalueOnly{}))&, int>);
    static_assert(std::same_as<decltype(Sora::Identity(std::declval<int&>())), int&>);
    static_assert(std::same_as<decltype(Sora::Identity(std::declval<const int&>())), const int&>);
    static_assert(std::same_as<decltype(Sora::Identity(std::declval<int&&>())), int&&>);
    static_assert(noexcept(Sora::Identity(1)));
    static_assert(noexcept(1 | Sora::Fn([](int value) noexcept { return value; })));
    static_assert(!std::invocable<decltype(Sora::Constant(std::make_unique<int>(1)))&>);
    static_assert(std::invocable<decltype(Sora::Constant(std::make_unique<int>(1)))&&>);
    static_assert(Sora::Concept::PublicRecord<Row>);
    static_assert(!Sora::Concept::PublicRecord<PrivateRecord>);
    static_assert(!CanMapThread<decltype([](auto, auto) {}), std::tuple<int>, std::tuple<int, int>>);
    static_assert(!CanAssociate<decltype(kDuplicateA), decltype(kDuplicateB)>);
    static_assert(!CanMap<int>);
    static_assert(CanSoraPipe<int, UnaryClosure>);
    static_assert(CanPipeSyntax<int, UnaryClosure>);
    static_assert(CanSoraPipe<int&, UnaryClosure&>);
    static_assert(CanSoraPipe<const int&, const UnaryClosure&>);
    static_assert(CanSoraPipe<UnaryClosure, BinaryClosure>);
    static_assert(CanPipeSyntax<UnaryClosure, BinaryClosure>);
    static_assert(CanSoraPipe<UnaryClosure&, BinaryClosure&>);
    static_assert(CanSoraPipe<int, RvalueClosure>);
    static_assert(!CanSoraPipe<int, RvalueClosure&>);
    static_assert(!CanSoraPipe<ValueTag, UnaryClosure>);
    static_assert(!CanPipeSyntax<ValueTag, UnaryClosure>);
    static_assert(!CanSoraPipe<ValueTag, ValueTag>);
    static_assert(!CanSoraPipe<UnaryClosure, ValueTag>);
    static_assert(!std::invocable<InvalidComposition, int>);
    static_assert(CanSoraPipe<MoveOnlyClosure, UnaryClosure>);
    static_assert(!CanSoraPipe<MoveOnlyClosure&, UnaryClosure>);
    static_assert(std::ranges::forward_range<ChunkedVector>);
    static_assert(std::ranges::forward_range<SlidingVector>);

} // namespace

TEST_CASE("Callable closures preserve state category and composition order", "[Sora][Core][Functional][Callable]") {
    auto mutableClosure = Sora::Fn([state = 0](int value) mutable { return state += value; });
    REQUIRE((2 | mutableClosure) == 2);
    REQUIRE((3 | mutableClosure) == 5);

    auto composed = Sora::Fn([](int value) { return value + 1; }) |
                    Sora::Fn([](int value) { return value * 3; });
    REQUIRE((4 | composed) == 15);
    REQUIRE(Sora::Compose([](int value) { return value * 3; }, [](int value) { return value + 1; })(4) == 15);
    REQUIRE(Sora::Then([](int value) { return value + 1; }, [](int value) { return value * 3; })(4) == 15);

    auto moveOnly = Sora::Fn([state = std::make_unique<int>(2)](int value) { return value * *state; });
    auto movePipeline = std::move(moveOnly) | Sora::Fn([](int value) { return value + 1; });
    REQUIRE((3 | movePipeline) == 7);

    auto constant = Sora::Constant(std::make_unique<int>(7));
    auto value = std::move(constant)();
    REQUIRE(*value == 7);
}

TEST_CASE("Fanout shares one input observation across typed result branches", "[Sora][Core][Functional][Fanout]") {
    auto measures = Sora::Fanout(
        [](const std::unique_ptr<int>& value) noexcept { return *value + 1; },
        [](const std::unique_ptr<int>& value) noexcept { return *value * 2; });

    auto output = measures(std::make_unique<int>(4));
    REQUIRE(output == std::tuple{5, 8});
    STATIC_REQUIRE(noexcept(measures(std::declval<std::unique_ptr<int>>())));

    auto piped = 3 | Sora::Fanout([](int value) { return value - 1; }, [](int value) { return value + 1; });
    REQUIRE(piped == std::tuple{2, 4});
}

TEST_CASE("Transform and filter are lazy and own temporary ranges", "[Sora][Core][Functional][View]") {
    int calls = 0;
    auto transformed = std::vector{1, 2, 3, 4} | Sora::View::Transform([&](int value) {
                           ++calls;
                           return value * 2;
                       }) |
                       Sora::View::Filter([](int value) { return value > 4; });

    REQUIRE(calls == 0);
    REQUIRE(Collect(transformed) == std::vector{6, 8});
    REQUIRE(calls == 6);

    calls = 0;
    auto cached = std::vector{1, 2, 3, 4} | Sora::View::Transform([&](int value) {
                      ++calls;
                      return value * 2;
                  }) |
                  Sora::View::CacheLatest() | Sora::View::Filter([](int value) { return value > 4; });
    REQUIRE(Collect(cached) == std::vector{6, 8});
    REQUIRE(calls == 4);
}

TEST_CASE("Where supports projection and custom comparison", "[Sora][Core][Functional][View]") {
    std::vector<Row> rows{{1, "one"}, {2, "two"}, {3, "three"}};
    auto selected = rows | Sora::View::Where(2, &Row::id);
    REQUIRE(std::ranges::distance(selected) == 1);
    REQUIRE(selected.front().name == "two");

    auto atLeastTwo = rows | Sora::View::Where(2, &Row::id, std::ranges::greater_equal{});
    REQUIRE(std::ranges::distance(atLeastTwo) == 2);
}

TEST_CASE("CacheLatest evaluates an upstream proxy once per position", "[Sora][Core][Functional][CacheLatest]") {
    int calls = 0;
    auto cached = std::vector{2, 3} | Sora::View::Transform([&](int value) {
                      ++calls;
                      return value * value;
                  }) |
                  Sora::View::CacheLatest();

    auto iterator = cached.begin();
    REQUIRE(*iterator == 4);
    REQUIRE(*iterator == 4);
    REQUIRE(calls == 1);
    ++iterator;
    REQUIRE(*iterator == 9);
    REQUIRE(*iterator == 9);
    REQUIRE(calls == 2);

    std::vector<bool> bits{true, false};
    auto materialized = bits | Sora::View::CacheLatest();
    auto bit = materialized.begin();
    REQUIRE(*bit);
    bits[0] = false;
    REQUIRE(*bit);
    ++bit;
    REQUIRE_FALSE(*bit);

    std::vector values{1};
    auto throwing = values | std::views::transform([](int& value) -> int& {
                        throw std::runtime_error{"dereference"};
                        return value;
                    }) |
                    Sora::View::CacheLatest();
    REQUIRE_THROWS_AS(*throwing.begin(), std::runtime_error);
}

TEST_CASE("Scan and partial sum are distinct lazy prefix views", "[Sora][Core][Functional][Prefix]") {
    std::array values{1, 2, 3};
    auto scan = values | Sora::View::Scan(std::minus{}, 10);
    REQUIRE(Collect(scan) == std::vector{9, 7, 4});
    REQUIRE(Collect(scan) == std::vector{9, 7, 4});

    std::array partialValues{10, 1, 2};
    auto partial = partialValues | Sora::View::PartialSum(std::minus{});
    REQUIRE(Collect(partial) == std::vector{10, 9, 7});

    auto referenceResult = values | Sora::View::Scan(ReferenceAccumulator{}, 0);
    REQUIRE(Collect(referenceResult) == std::vector{1, 3, 6});

    std::array<int, 0> empty{};
    REQUIRE(Collect(empty | Sora::View::Scan(std::plus{}, 0)).empty());
    REQUIRE(Collect(empty | Sora::View::PartialSum()).empty());
}

TEST_CASE("ZipWith owns bound temporaries and stops at the shortest range", "[Sora][Core][Functional][Zip]") {
    auto zipped = std::vector{1, 2, 3} | Sora::View::ZipWith(std::vector{4, 5});
    std::vector<int> sums;
    for (auto [left, right] : zipped) {
        sums.push_back(left + right);
    }
    REQUIRE(sums == std::vector{5, 7});

    auto enumerated = std::vector{8, 9} | Sora::View::Enumerate(4);
    auto iterator = enumerated.begin();
    REQUIRE(std::get<0>(*iterator) == 4);
    REQUIRE(std::get<1>(*iterator) == 8);
}

TEST_CASE("FlatMap owns temporary inner ranges", "[Sora][Core][Functional][FlatMap]") {
    auto flattened = std::vector{1, 3} | Sora::View::FlatMap([](int value) {
                         return std::array{value, value + 1};
                     });
    REQUIRE(Collect(flattened) == std::vector{1, 2, 3, 4});
}

TEST_CASE("Stride chunk and slide provide missing standard views", "[Sora][Core][Functional][View]") {
    std::vector values{1, 2, 3, 4, 5};
    REQUIRE(Collect(values | Sora::View::Stride(2)) == std::vector{1, 3, 5});
    REQUIRE(Collect(values | Sora::View::Stride(0)).empty());

    std::vector<std::vector<int>> chunks;
    for (auto chunk : values | Sora::View::Chunk(2)) {
        chunks.push_back(Collect(chunk));
    }
    REQUIRE(chunks == std::vector<std::vector<int>>{{1, 2}, {3, 4}, {5}});

    std::vector<std::vector<int>> windows;
    for (auto window : values | Sora::View::Slide(3)) {
        windows.push_back(Collect(window));
    }
    REQUIRE(windows == std::vector<std::vector<int>>{{1, 2, 3}, {2, 3, 4}, {3, 4, 5}});

    auto hugeStride = values | Sora::View::Stride(std::numeric_limits<size_t>::max());
    REQUIRE(std::ranges::size(hugeStride) == 1);
    REQUIRE(Collect(hugeStride) == std::vector{1});

    auto hugeChunk = values | Sora::View::Chunk(std::numeric_limits<size_t>::max());
    REQUIRE(std::ranges::size(hugeChunk) == 1);
    REQUIRE(Collect(*hugeChunk.begin()) == values);

    auto hugeSlide = values | Sora::View::Slide(std::numeric_limits<size_t>::max());
    REQUIRE(std::ranges::size(hugeSlide) == 0);
    REQUIRE(hugeSlide.empty());
}

TEST_CASE("Root map and fold transfer move-only state", "[Sora][Core][Functional][Ownership]") {
    auto mapped = std::vector{1, 2, 3} |
                  Sora::Map([factor = std::make_unique<int>(3)](int value) { return value * *factor; });
    REQUIRE(Collect(mapped) == std::vector{3, 6, 9});

    auto sum = std::vector{1, 2, 3} |
               Sora::Fold(
                   [](std::unique_ptr<int> accumulator, int value) {
                       *accumulator += value;
                       return accumulator;
                   },
                   std::make_unique<int>(4));
    REQUIRE(*sum == 10);
}

TEST_CASE("Tuple and record bridges preserve positional semantics", "[Sora][Core][Functional][Record]") {
    auto memberIds = std::tuple{Row{1, "one"}, Row{2, "two"}} | Sora::Map(&Row::id);
    REQUIRE(memberIds == std::tuple{1, 2});

    auto threaded = Sora::MapThread(std::plus{}, std::tuple{1, 2}, std::tuple{10, 20});
    REQUIRE(threaded == std::tuple{11, 22});

    Row row{7, "seven"};
    auto tuple = Sora::ToTuple(row);
    REQUIRE(tuple == std::tuple{7, std::string{"seven"}});

    std::vector<std::string_view> names;
    Sora::ForEachField(row, [&](std::string_view name, auto&&) { names.push_back(name); });
    REQUIRE(names == std::vector<std::string_view>{"id", "name"});
}

TEST_CASE("Associations enforce unique keys and preserve move-only values", "[Sora][Core][Functional][Assoc]") {
    Sora::Assoc empty;
    STATIC_REQUIRE_FALSE(decltype(empty)::Contains<("missing"_k).key>());

    auto association = Sora::Assoc{"id"_k = 3, "payload"_k = std::make_unique<int>(11)};
    STATIC_REQUIRE(decltype(association)::Contains<("id"_k).key>());
    REQUIRE(association["id"_k] == 3);
    REQUIRE(*association["payload"_k] == 11);
    auto payload = std::move(association)["payload"_k];
    REQUIRE(*payload == 11);
}

TEST_CASE("Monadic adaptors use structural operation protocols", "[Sora][Core][Functional][Monadic]") {
    auto expected = std::expected<int, std::string>{4} |
                    Sora::AndThen([](int value) { return std::expected<int, std::string>{value * 2}; }) |
                    Sora::Transform([](int value) { return value + 1; });
    REQUIRE(expected.value() == 9);

    auto optional = std::optional<int>{} | Sora::OrElse([] { return std::optional<int>{5}; });
    REQUIRE(optional.value() == 5);
    REQUIRE((std::optional<int>{} | Sora::ValueOr(7)) == 7);
}
