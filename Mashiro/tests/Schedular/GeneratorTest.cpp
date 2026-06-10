#include <Mashiro/Schedular/Generator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

using namespace Mashiro;

// =====================================================================
// Basic generators
// =====================================================================

Generator<int> Iota(int start, int end) {
    for (int i = start; i < end; ++i)
        co_yield i;
}

Generator<int> Fibonacci(int count) {
    int a = 0, b = 1;
    for (int i = 0; i < count; ++i) {
        co_yield a;
        auto next = a + b;
        a = b;
        b = next;
    }
}

Generator<int> Empty() {
    co_return;
}

Generator<int> SingleValue(int v) {
    co_yield v;
}

// =====================================================================
// Reference semantics generators
// =====================================================================

Generator<const std::string&> StringSeq() {
    static const std::string a = "hello";
    static const std::string b = "world";
    co_yield a;
    co_yield b;
}

// =====================================================================
// Recursive delegation generators
// =====================================================================

Generator<int> Flatten(const std::vector<std::vector<int>>& vv) {
    for (auto& v : vv)
        co_yield ElementsOf(Iota(0, static_cast<int>(v.size())));
}

Generator<int> ChainTwo(int n1, int n2) {
    co_yield ElementsOf(Iota(0, n1));
    co_yield ElementsOf(Iota(100, 100 + n2));
}

// Deep recursion: binary countdown
Generator<int> BinaryCountdown(int n) {
    if (n <= 0) {
        co_yield 0;
        co_return;
    }
    co_yield n;
    co_yield ElementsOf(BinaryCountdown(n - 1));
}

// =====================================================================
// Tests
// =====================================================================

TEST_CASE("Generator basic iota", "[Schedular.Generator]") {
    std::vector<int> result;
    for (int v : Iota(0, 5))
        result.push_back(v);
    REQUIRE(result == std::vector{0, 1, 2, 3, 4});
}

TEST_CASE("Generator fibonacci", "[Schedular.Generator]") {
    std::vector<int> result;
    for (int v : Fibonacci(8))
        result.push_back(v);
    REQUIRE(result == std::vector{0, 1, 1, 2, 3, 5, 8, 13});
}

TEST_CASE("Generator empty", "[Schedular.Generator]") {
    std::vector<int> result;
    for (int v : Empty())
        result.push_back(v);
    REQUIRE(result.empty());
}

TEST_CASE("Generator single value", "[Schedular.Generator]") {
    std::vector<int> result;
    for (int v : SingleValue(42))
        result.push_back(v);
    REQUIRE(result == std::vector{42});
}

TEST_CASE("Generator reference semantics", "[Schedular.Generator]") {
    std::vector<std::string> result;
    for (const auto& s : StringSeq())
        result.emplace_back(s);
    REQUIRE(result == std::vector<std::string>{"hello", "world"});
}

TEST_CASE("Generator move-only", "[Schedular.Generator]") {
    auto gen = Iota(0, 3);
    auto gen2 = std::move(gen);
    REQUIRE_FALSE(static_cast<bool>(gen));
    REQUIRE(static_cast<bool>(gen2));

    std::vector<int> result;
    for (int v : gen2)
        result.push_back(v);
    REQUIRE(result == std::vector{0, 1, 2});
}

TEST_CASE("Generator recursive delegation - chain", "[Schedular.Generator]") {
    std::vector<int> result;
    for (int v : ChainTwo(3, 2))
        result.push_back(v);
    REQUIRE(result == std::vector{0, 1, 2, 100, 101});
}

TEST_CASE("Generator recursive delegation - deep", "[Schedular.Generator]") {
    std::vector<int> result;
    for (int v : BinaryCountdown(5))
        result.push_back(v);
    REQUIRE(result == std::vector{5, 4, 3, 2, 1, 0});
}

Generator<int> ThrowingGen() {
    co_yield 1;
    throw std::runtime_error("test error");
}

TEST_CASE("Generator exception propagation", "[Schedular.Generator]") {
    auto gen = ThrowingGen();
    auto it = gen.begin();
    REQUIRE(*it == 1);
    REQUIRE_THROWS_AS(++it, std::runtime_error);
}

TEST_CASE("Generator early destruction", "[Schedular.Generator]") {
    // Ensure destroying a partially-consumed generator doesn't leak.
    {
        auto gen = Iota(0, 1000);
        auto it = gen.begin();
        REQUIRE(*it == 0);
        ++it;
        REQUIRE(*it == 1);
        // gen destroyed here with 998 unconsumed values
    }
    SUCCEED("No leak/crash on early destruction");
}

TEST_CASE("Generator range-for with structured binding proxy", "[Schedular.Generator]") {
    auto pairs = []() -> Generator<int> {
        for (int i = 0; i < 5; ++i)
            co_yield i * i;
    };

    int sum = 0;
    for (int v : pairs())
        sum += v;
    REQUIRE(sum == 0 + 1 + 4 + 9 + 16);
}
