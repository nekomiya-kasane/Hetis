/**
 * @file PathTest.cpp
 * @brief Verify allocation-free lexical path queries.
 * @ingroup Testing
 */

#include <Sora/Core/Path.h>

#include <catch2/catch_test_macros.hpp>

static_assert(Sora::HasPathSeparator("plugins/render.dll"));
static_assert(Sora::FileName("plugins/render.dll") == "render.dll");

TEST_CASE("Core lexical path splitting accepts both separator families", "[Sora.Core.Path]") {
    const auto [directory, file] = Sora::SplitDirectory("plugins\\renderer/libSora.dll");
    CHECK(directory == "plugins\\renderer/");
    CHECK(file == "libSora.dll");
    CHECK(Sora::FileName("libSora.dll") == "libSora.dll");
}
