#include <Sora/Core/StringUtils.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("ASCII trim helpers remove only their designated boundaries", "[Sora.Core.StringUtils]") {
    constexpr std::string_view text = " \t\r\n value \f\v ";

    STATIC_REQUIRE(Sora::Ascii::TrimStart(text) == "value \f\v ");
    STATIC_REQUIRE(Sora::Ascii::TrimEnd(text) == " \t\r\n value");
    STATIC_REQUIRE(Sora::Ascii::Trim(text) == "value");
    STATIC_REQUIRE(Sora::Ascii::Trim("\t\r\n") == "");
    STATIC_REQUIRE(Sora::Ascii::Trim("unchanged") == "unchanged");
}
