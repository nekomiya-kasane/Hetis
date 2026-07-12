#include <Sora/Core/Functional.h>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <vector>

namespace {

    struct Row {
        int id = 0;
        std::string name;
    };

} // namespace

TEST_CASE("View Where filters by projected equality", "[Sora][Core][Functional]") {
    std::vector<Row> rows{
        {.id = 1, .name = "one"},
        {.id = 2, .name = "two"},
        {.id = 2, .name = "deux"},
        {.id = 3, .name = "three"},
    };

    std::vector<std::string> names;
    for (const Row& row : rows | Sora::View::Where(2, &Row::id)) {
        names.push_back(row.name);
    }

    REQUIRE(names == std::vector<std::string>{"two", "deux"});
}

TEST_CASE("View Where accepts custom comparators", "[Sora][Core][Functional]") {
    std::vector<Row> rows{
        {.id = 4, .name = "cold"},
        {.id = 8, .name = "warm"},
        {.id = 16, .name = "hot"},
    };

    std::vector<int> ids;
    for (const Row& row : rows | Sora::View::Where(8, &Row::id, std::ranges::greater_equal{})) {
        ids.push_back(row.id);
    }

    REQUIRE(ids == std::vector<int>{8, 16});
}
