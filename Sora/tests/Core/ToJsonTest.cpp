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

    struct NativeJsonValue {
        int value = 0;

        constexpr bool operator==(const NativeJsonValue&) const = default;
    };

    Sora::Json ToJson(const NativeJsonValue& value) {
        return {{"native", value.value}};
    }

    void FromJson(const Sora::Json& input, NativeJsonValue& value) {
        value.value = input.at("native").get<int>();
    }

    struct JsonGrandBase {
        int grandBaseValue = 0;

        constexpr bool operator==(const JsonGrandBase&) const = default;
    };

    struct JsonBase : JsonGrandBase {
        int baseValue = 0;

        constexpr bool operator==(const JsonBase&) const = default;
    };

    struct JsonSideBase {
        std::string sideValue;

        constexpr bool operator==(const JsonSideBase&) const = default;
    };

    struct JsonDerived : JsonBase, JsonSideBase {
        int derivedValue = 0;

        constexpr bool operator==(const JsonDerived&) const = default;
    };

} // namespace ToJsonTest

using namespace ToJsonTest;

static_assert(Sora::Meta::IsChronoDuration(^^std::chrono::seconds));
static_assert(Sora::Concept::ChronoTimePoint<std::chrono::system_clock::time_point>);
static_assert(Sora::Concept::JsonReflectableClass<Payload>);
static_assert(Sora::Concept::JsonCustomSerializable<CustomValue>);
static_assert(Sora::Concept::JsonCustomDeserializable<CustomValue>);

TEST_CASE("JSON CPOs serialize and deserialize reflected classes", "[Sora.Core.ToJson]") {
    const Payload source{.value = 42, .name = "Sora"};
    const Sora::Json encoded = Sora::ToJson(source);

    REQUIRE(encoded == Sora::Json{{"answer", 42}, {"name", "Sora"}});
    REQUIRE(Sora::FromJson(std::in_place_type<Payload>, encoded) == source);

    Payload output;
    Sora::FromJson(encoded, output);
    REQUIRE(output == source);
}

TEST_CASE("JSON reflection preserves base subobjects in an explicit inheritance partition", "[Sora.Core.ToJson]") {
    JsonDerived source;
    source.baseValue = 29;
    source.sideValue = "side";
    source.derivedValue = 31;

    const Sora::Json encoded = Sora::ToJson(source);
    const auto& bases = encoded.at("$bases");
    const auto& encodedBase = bases.at(std::string(Sora::Traits::TypeName<JsonBase>));
    REQUIRE(encoded.at("derivedValue") == 31);
    REQUIRE(encodedBase.at("baseValue") == 29);
    REQUIRE(encodedBase.at("$bases").at(std::string(Sora::Traits::TypeName<JsonGrandBase>)).at("grandBaseValue") == 0);
    REQUIRE(bases.at(std::string(Sora::Traits::TypeName<JsonSideBase>)).at("sideValue") == "side");
    REQUIRE(Sora::FromJson(std::in_place_type<JsonDerived>, encoded) == source);
    REQUIRE_THROWS_AS(Sora::FromJson(std::in_place_type<JsonDerived>, Sora::Json{{"$bases", Sora::Json::array()}}),
                      Sora::JsonException);
}

TEST_CASE("JSON CPOs honor explicit member customizations", "[Sora.Core.ToJson]") {
    const CustomValue source{.value = 17};
    const Sora::Json encoded = Sora::ToJson(source);

    REQUIRE(encoded == Sora::Json{{"custom", 17}});
    REQUIRE(Sora::FromJson(std::in_place_type<CustomValue>, encoded).value == 17);

    const NativeJsonValue native{.value = 23};
    REQUIRE(Sora::ToJson(native) == Sora::Json{{"native", 23}});
    REQUIRE(Sora::FromJson(std::in_place_type<NativeJsonValue>, Sora::ToJson(native)) == native);
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
                      Sora::JsonException);
}

TEST_CASE("JSON CPOs deserialize directly from simdjson DOM views", "[Sora.Core.ToJson]") {
    Sora::JsonParser parser;
    const JsonDerived source = [] {
        JsonDerived value;
        value.baseValue = 29;
        value.sideValue = "side";
        value.derivedValue = 31;
        return value;
    }();
    const std::string text = Sora::ToJson(source).dump();

    const Sora::JsonView view = parser.Parse(text);
    REQUIRE(Sora::FromJson(std::in_place_type<JsonDerived>, view) == source);

    const Sora::JsonView arrayView = parser.Parse("[1,2,3]");
    const auto values = Sora::FromJson(std::in_place_type<std::vector<int>>, arrayView);
    REQUIRE(values == std::vector<int>{1, 2, 3});

    const Sora::JsonView payloadView = parser.Parse(R"({"answer":42,"name":"Sora"})");
    REQUIRE(Sora::FromJson(std::in_place_type<Payload>, payloadView) == Payload{.value = 42, .name = "Sora"});
}