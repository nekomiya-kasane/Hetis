#include <Sora/Core/ToJson.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace ToJsonTest {

    struct Payload {
        [[= Sora::$::Serialization::Rename<Sora::FixedString<6>{"answer"}>{}]] int value = 0;
        std::string name;

        constexpr bool operator==(const Payload&) const = default;
    };

    struct CustomValue {
        int value = 0;

        [[nodiscard]] Sora::Json ToJson() const { return {{"custom", value}}; }

        [[nodiscard]] static CustomValue FromJson(const Sora::Json& input) {
            return {.value = input.at("custom").get<int>()};
        }
    };

    struct NlohmannValue {
        int value = 0;

        constexpr bool operator==(const NlohmannValue&) const = default;
    };

    void to_json(Sora::Json& output, const NlohmannValue& value) {
        output = {{"native", value.value}};
    }

    void from_json(const Sora::Json& input, NlohmannValue& value) {
        value.value = input.at("native").get<int>();
    }

} // namespace ToJsonTest

using namespace ToJsonTest;

static_assert(Sora::Meta::IsChronoDuration(^^std::chrono::seconds));
static_assert(Sora::Concept::ChronoTimePoint<std::chrono::system_clock::time_point>);
static_assert(Sora::Concept::JsonReflectableClass<Payload>);
static_assert(Sora::Concept::JsonCustomSerializable<CustomValue>);
static_assert(Sora::Concept::JsonCustomDeserializable<CustomValue>);
static_assert(Sora::Concept::NlohmannJsonSerializable<NlohmannValue>);
static_assert(Sora::Concept::NlohmannJsonDeserializable<NlohmannValue>);

TEST_CASE("JSON CPOs serialize and deserialize reflected classes", "[Sora.Core.ToJson]") {
    const Payload source{.value = 42, .name = "Sora"};
    const Sora::Json encoded = Sora::ToJson(source);

    REQUIRE(encoded == Sora::Json{{"answer", 42}, {"name", "Sora"}});
    REQUIRE(Sora::FromJson(std::in_place_type<Payload>, encoded) == source);

    Payload output;
    Sora::FromJson(encoded, output);
    REQUIRE(output == source);
}

TEST_CASE("JSON CPOs honor explicit member customizations", "[Sora.Core.ToJson]") {
    const CustomValue source{.value = 17};
    const Sora::Json encoded = Sora::ToJson(source);

    REQUIRE(encoded == Sora::Json{{"custom", 17}});
    REQUIRE(Sora::FromJson(std::in_place_type<CustomValue>, encoded).value == 17);

    const NlohmannValue native{.value = 23};
    REQUIRE(Sora::ToJson(native) == Sora::Json{{"native", 23}});
    REQUIRE(Sora::FromJson(std::in_place_type<NlohmannValue>, Sora::ToJson(native)) == native);
}

TEST_CASE("JSON text conversion preserves Unicode and native paths through canonical UTF-8", "[Sora.Core.ToJson]") {
    constexpr std::string_view utf8 = "A\xF0\x9F\x98\x80";
    const std::u16string utf16{u"A\U0001F600"};
    REQUIRE(Sora::ToJson(utf16).get<std::string>() == std::string{utf8});
    REQUIRE(Sora::FromJson(std::in_place_type<std::u16string>, Sora::Json(std::string{utf8})) == utf16);

    const std::filesystem::path path = std::filesystem::path{L"Sora"} / L"Json";
    REQUIRE(Sora::FromJson(std::in_place_type<std::filesystem::path>, Sora::ToJson(path)) == path);
}

TEST_CASE("JSON variant deserialization tries later alternatives", "[Sora.Core.ToJson]") {
    using Value = std::variant<int, std::string>;
    const Sora::Json input = "text";
    const Value decoded = Sora::FromJson(std::in_place_type<Value>, input);

    REQUIRE(std::holds_alternative<std::string>(decoded));
    REQUIRE(std::get<std::string>(decoded) == "text");
    REQUIRE_THROWS_AS(Sora::FromJson(std::in_place_type<std::vector<int>>, Sora::Json::object()),
                      nlohmann::json::type_error);
}
