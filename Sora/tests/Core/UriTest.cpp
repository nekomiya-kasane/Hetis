#include <Sora/Core/Uri.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("URI identity helpers validate canonical paths", "[Sora][Core][Uri]") {
    static_assert(Sora::IsCanonicalUriIdentityPath(""));
    static_assert(Sora::IsCanonicalUriIdentityPath("/shader/fullscreen.wgsl"));
    static_assert(Sora::IsCanonicalRelativeUriIdentityPath("icons/close.ktx2"));
    static_assert(Sora::IsUriPathFilenameSuffix(".ktx2"));

    REQUIRE_FALSE(Sora::IsCanonicalUriIdentityPath("/shader/../fullscreen.wgsl"));
    REQUIRE_FALSE(Sora::IsCanonicalUriIdentityPath("/shader//fullscreen.wgsl"));
    REQUIRE_FALSE(Sora::IsCanonicalUriIdentityPath("/shader/fullscreen.wgsl/"));
    REQUIRE_FALSE(Sora::IsCanonicalRelativeUriIdentityPath("res://shader/fullscreen.wgsl"));
    REQUIRE_FALSE(Sora::IsCanonicalRelativeUriIdentityPath("../fullscreen.wgsl"));
    REQUIRE_FALSE(Sora::IsUriPathFilenameSuffix("ktx2"));
    REQUIRE_FALSE(Sora::IsUriPathFilenameSuffix("./ktx2"));
}

TEST_CASE("URI identity helpers normalize bases and join relative paths", "[Sora][Core][Uri]") {
    auto normalized = Sora::NormalizeUriIdentityBase<64>("res://image/ui/");
    REQUIRE(normalized.has_value());
    REQUIRE(normalized->view() == "res://image/ui");

    auto joined = Sora::JoinUriIdentityPath<96>(*normalized, "icons/close.ktx2");
    REQUIRE(joined.has_value());
    REQUIRE(joined->view() == "res://image/ui/icons/close.ktx2");

    REQUIRE_FALSE(Sora::NormalizeUriIdentityBase<64>("res://image/ui?q=1").has_value());
    REQUIRE_FALSE(Sora::JoinUriIdentityPath<96>("res://image/ui", "../close.ktx2").has_value());
    REQUIRE_FALSE(Sora::JoinUriIdentityPath<96>("res://image/ui", "res://image/close.ktx2").has_value());
}
