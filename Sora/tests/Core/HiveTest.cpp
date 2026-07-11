#include "Sora/Core/ADT/hive"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <ranges>
#include <type_traits>

TEST_CASE("std::hive polyfill exposes the C++26 surface", "[Sora.Core.ADT.Hive]") {
    STATIC_REQUIRE(__cpp_lib_hive >= 202502L);
    STATIC_REQUIRE(std::same_as<std::hive<int>::value_type, int>);
    STATIC_REQUIRE(std::bidirectional_iterator<std::hive<int>::iterator>);

    const std::hive_limits limits{3, 8};
    std::hive<int> values(limits);
    REQUIRE(values.block_capacity_limits().min == 3);
    REQUIRE(values.block_capacity_limits().max == 8);
}

TEST_CASE("std::hive preserves element addresses across unrelated insert and erase", "[Sora.Core.ADT.Hive]") {
    std::hive<int> values;

    auto stable = values.insert(42);
    int* stableAddress = std::addressof(*stable);

    for (int i = 0; i != 128; ++i) {
        values.insert(i);
    }

    REQUIRE(std::addressof(*stable) == stableAddress);
    REQUIRE(*stable == 42);

    for (auto it = values.begin(); it != values.end();) {
        if (std::addressof(*it) == stableAddress) {
            ++it;
        } else {
            it = values.erase(it);
        }
    }

    REQUIRE(values.size() == 1);
    REQUIRE(*values.begin() == 42);
    REQUIRE(std::addressof(*values.begin()) == stableAddress);
}

TEST_CASE("std::hive integrates with standard erase helpers and range construction", "[Sora.Core.ADT.Hive]") {
    constexpr std::array seed{1, 2, 3, 4, 5, 6, 7, 8};
    std::hive<int> values(std::from_range, seed);

    REQUIRE(values.size() == seed.size());
    REQUIRE(std::erase(values, 3) == 1);
    REQUIRE(std::erase_if(values, [](int value) { return value % 2 == 0; }) == 4);
    REQUIRE(std::ranges::equal(values, std::array{1, 5, 7}));
}

TEST_CASE("std::pmr::hive uses polymorphic allocators", "[Sora.Core.ADT.Hive]") {
    std::array<std::byte, 4096> buffer{};
    std::pmr::monotonic_buffer_resource resource(buffer.data(), buffer.size());
    std::pmr::hive<int> values{std::pmr::polymorphic_allocator<int>{&resource}};

    values.insert(11);
    values.insert(29);

    REQUIRE(values.size() == 2);
    REQUIRE(std::ranges::equal(values, std::array{11, 29}));
}
