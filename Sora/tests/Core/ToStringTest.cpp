#include <Sora/Core/ToString.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

namespace ToStringTest {

    enum class Mode : std::uint8_t {
        Fast,
        Safe,
    };

    struct Port {
        int value = 0;
    };

    [[nodiscard]] std::string ToString(const Port& port) {
        return "port=" + std::to_string(port.value);
    }

    struct RenamedField {
        [[= Sora::$::Serialization::Rename<Sora::FixedString<6>{"answer"}>{}]] int value = 42;
    };

    struct MemberParsed {
        int value = 0;

        [[nodiscard]] static constexpr Sora::Result<MemberParsed> FromString(std::string_view text) {
            auto decoded = Sora::FromString(std::in_place_type<int>, text);
            return decoded ? Sora::Result<MemberParsed>{MemberParsed{.value = *decoded}}
                           : std::unexpected(decoded.error());
        }
    };

    struct CanonicalValue {
        int value = 0;

        [[nodiscard]] std::string ToString() const { return "value=" + std::to_string(value); }

        [[nodiscard]] static constexpr Sora::Result<CanonicalValue> FromString(std::string_view text) {
            if (!text.starts_with("value=")) {
                return std::unexpected(Sora::ErrorCode::InvalidSyntax);
            }
            auto decoded = Sora::FromString(std::in_place_type<int>, text.substr(6));
            return decoded ? Sora::Result<CanonicalValue>{CanonicalValue{.value = *decoded}}
                           : std::unexpected(decoded.error());
        }
    };

    [[nodiscard]] constexpr Sora::VoidResult FromString(Port& port, std::string_view text) {
        auto decoded = Sora::FromString(std::in_place_type<int>, text);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        port.value = *decoded;
        return {};
    }

} // namespace ToStringTest

using namespace ToStringTest;

static_assert(std::same_as<decltype(Sora::Str(std::declval<std::string&>())), std::string_view>);
static_assert(std::same_as<decltype(Sora::Str(std::declval<std::string>())), std::string>);
static_assert(std::same_as<decltype(Sora::Str(Mode::Fast)), std::string_view>);
static_assert(Sora::Traits::BuiltinStringDeserializable<int>);
static_assert(!Sora::Traits::BuiltinStringDeserializable<MemberParsed>);
static_assert(Sora::Concept::CustomStringFormattable<Port>);
static_assert(Sora::Concept::StringFormattable<CanonicalValue>);
static_assert(Sora::Concept::StringDeserializable<CanonicalValue>);
static_assert(Sora::Concept::StringViewable<Mode>);

TEST_CASE("FromString strictly deserializes canonical scalar values", "[Sora.Core.ToString]") {
    STATIC_REQUIRE(Sora::FromString(std::in_place_type<bool>, "yes") == true);
    STATIC_REQUIRE(Sora::FromString(std::in_place_type<int>, "-42") == -42);
    STATIC_REQUIRE(Sora::FromString(std::in_place_type<Mode>, "Fast") == Mode::Fast);
    STATIC_REQUIRE_FALSE(Sora::FromString(std::in_place_type<bool>, "sometimes").has_value());
    STATIC_REQUIRE(Sora::FromString(std::in_place_type<unsigned>, "-1").error() == Sora::ErrorCode::InvalidSyntax);
    STATIC_REQUIRE(Sora::FromString(std::in_place_type<std::string>, "\xF0\x28\x8C\x28").error() ==
                   Sora::ErrorCode::InvalidUtf8Continuation);
    STATIC_REQUIRE(Sora::FromString(std::in_place_type<MemberParsed>, "17")->value == 17);
    STATIC_REQUIRE_FALSE(Sora::FromString(std::in_place_type<MemberParsed>, "not-an-integer").has_value());
}

TEST_CASE("ToString is the single canonical formatting protocol", "[Sora.Core.ToString]") {
    REQUIRE(Sora::ToString(false) == "false");
    REQUIRE(Sora::ToString(42) == "42");
    REQUIRE(Sora::ToString(Mode::Safe) == "Safe");
    REQUIRE(Sora::ToString(std::string_view{"text"}) == "text");
    REQUIRE(Sora::ToString(Port{.value = 443}) == "port=443");

    const CanonicalValue value{.value = 23};
    REQUIRE(Sora::ToString(value) == "value=23");
    REQUIRE(Sora::FromString(std::in_place_type<CanonicalValue>, "value=23")->value == 23);

    const auto port = Sora::FromString(std::in_place_type<Port>, "443");
    REQUIRE(port.has_value());
    REQUIRE(port->value == 443);

    const std::string reflected = Sora::ToString(RenamedField{});
    REQUIRE(reflected.find("answer=42") != std::string::npos);
    REQUIRE(reflected.find("value=42") == std::string::npos);
}

TEST_CASE("ToString transcodes Unicode strings and native paths as UTF-8", "[Sora.Core.ToString]") {
    constexpr std::string_view utf8 = "A\xF0\x9F\x98\x80";
    REQUIRE(Sora::ToString(std::u16string_view{u"A\U0001F600"}) == utf8);
    REQUIRE(Sora::ToString(std::u32string_view{U"A\U0001F600"}) == utf8);
    const auto utf16 = Sora::FromString(std::in_place_type<std::u16string>, utf8);
    const auto utf32 = Sora::FromString(std::in_place_type<std::u32string>, utf8);
    REQUIRE(utf16.has_value());
    REQUIRE(utf32.has_value());
    REQUIRE(*utf16 == std::u16string{u"A\U0001F600"});
    REQUIRE(*utf32 == std::u32string{U"A\U0001F600"});

    const std::filesystem::path path = std::filesystem::path{L"Sora"} / L"Unicode";
    const auto parsedPath = Sora::FromString(std::in_place_type<std::filesystem::path>, Sora::ToString(path));
    REQUIRE(parsedPath.has_value());
    REQUIRE(*parsedPath == path);
}
