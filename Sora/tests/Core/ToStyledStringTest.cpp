#include <Sora/Core/ToStyledString.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace ToStyledStringTest {

    struct MemberStyled {
        void ToStyledString(Sora::Styled::StyledStringBuilder& builder) const {
            builder.Text(Sora::Styled::StyledRole::Plain, "member");
        }
    };

    struct AdlStyled {};

    void ToStyledString(Sora::Styled::StyledStringBuilder& builder, AdlStyled&&) {
        builder.Text(Sora::Styled::StyledRole::Plain, "adl");
    }

    void FunctionTarget() {}

    [[nodiscard]] Sora::Styled::StyledStringOptions PlainOptions() {
        return {.color = false, .escapePolicy = Sora::Styled::StyledEscapePolicy::None};
    }

} // namespace ToStyledStringTest

using namespace ToStyledStringTest;

static_assert(Sora::Concept::CustomStyledStringFormattable<MemberStyled>);
static_assert(Sora::Concept::CustomStyledStringFormattable<AdlStyled>);

TEST_CASE("Styled string CPO dispatches member and ADL customizations", "[Sora.Core.ToStyledString]") {
    REQUIRE(Sora::ToStyledString(MemberStyled{}, PlainOptions()) == "member");
    REQUIRE(Sora::ToStyledString(AdlStyled{}, PlainOptions()) == "adl");
}

TEST_CASE("Styled string conversion reuses canonical Unicode and path conversion", "[Sora.Core.ToStyledString]") {
    constexpr std::string_view utf8 = "A\xF0\x9F\x98\x80";
    REQUIRE(Sora::ToStyledString(std::u16string_view{u"A\U0001F600"}, PlainOptions()) ==
            std::string{"\""} + std::string{utf8} + "\"");

    const std::filesystem::path path = std::filesystem::path{L"Sora"} / L"Styled";
    REQUIRE(Sora::ToStyledString(path, PlainOptions()) == "\"" + Sora::ToString(path) + "\"");
}

TEST_CASE("Styled string conversion handles function pointers without object-pointer casts",
          "[Sora.Core.ToStyledString]") {
    REQUIRE(Sora::ToStyledString(&FunctionTarget, PlainOptions()).find("function") != std::string::npos);
}

TEST_CASE("Meta ToStyledString renders reflected named variables with structural separation",
          "[Sora.Core.ToStyledString]") {
    std::string status = "ready\nnext";
    int completed = 7;
    int pending = 2;

    REQUIRE(Sora::Meta::ToStyledString<^^status>(status, {.color = false}) ==
            std::format("variable[status : {} = \"ready\\nnext\"]",
                        Sora::Meta::DisplayStringOf(Sora::Meta::TypeOf(^^status))));
    REQUIRE(Sora::Meta::ToStyledString<^^completed>(completed, PlainOptions()) ==
            "variable[completed : int = 7]");
    REQUIRE(Sora::Meta::ToStyledString<^^pending>(pending, PlainOptions()) == "variable[pending : int = 2]");
}

TEST_CASE("Styled formatter composes compact text-attribute flags", "[Sora.Core.ToStyledString]") {
    constexpr auto attributes =
        tapioca::attr::bold | tapioca::attr::italic | tapioca::attr::underline | tapioca::attr::reverse;
    const auto expected = Sora::ToStyledString(MemberStyled{}, {.attributes = attributes});

    REQUIRE(std::format("{:shiur}", MemberStyled{}) == expected);
    REQUIRE(std::format("{:hiur}", MemberStyled{}) == expected);
}
