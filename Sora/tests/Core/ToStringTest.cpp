#include <Sora/Core/ToString.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <meta>
#include <string>
#include <type_traits>
#include <utility>

namespace ToStringTest {

    struct IncompletePointee;

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

    struct ReflectedGrandBase {
        int grandBaseValue = 7;
    };

    struct ReflectedBase : ReflectedGrandBase {
        int baseValue = 11;
    };

    struct ReflectedSideBase {
        int sideValue = 13;
    };

    struct ReflectedDerived : ReflectedBase, ReflectedSideBase {
        int derivedValue = 17;
    };

    struct PrivateBase {
        int privateBaseValue = 31;
    };

    class PrivateDerived : private PrivateBase {
    public:
        int derivedValue = 37;
    };

    struct VirtualStringBase {
        virtual ~VirtualStringBase() = default;
        [[nodiscard]] virtual std::string ToString() const { return "virtual-base"; }
    };

    struct VirtualStringDerived : VirtualStringBase {
        [[nodiscard]] std::string ToString() const override { return "virtual-derived"; }
    };

    struct NonVirtualStringBase {
        virtual ~NonVirtualStringBase() = default;
        int baseValue = 19;
    };

    struct NonVirtualStringDerived : NonVirtualStringBase {
        int derivedValue = 23;
    };

    struct NonPolymorphicBase {
        int baseValue = 41;
    };

    struct NonPolymorphicDerived : NonPolymorphicBase {
        int derivedValue = 43;
    };

    struct PolymorphicPointers {
        [[= Sora::$::Serialization::DerefPrint{}]] const VirtualStringBase* complete = nullptr;
        [[= Sora::$::Serialization::DerefPrint{}]] const NonVirtualStringBase* incomplete = nullptr;
        [[= Sora::$::Serialization::DerefPrint{}]] const NonPolymorphicBase* indeterminate = nullptr;
    };

    struct IncompletePointer {
        [[= Sora::$::Serialization::DerefPrint{}]] const IncompletePointee* value = nullptr;
    };

    consteval std::array<size_t, 4> ReflectedMemberDepths() {
        std::array<size_t, 4> depths{};
        Sora::Meta::WalkHierarchyMembers(^^ReflectedDerived, [&](size_t depth, std::meta::info member) {
            if (!std::meta::is_nonstatic_data_member(member) || !std::meta::has_identifier(member)) {
                return;
            }
            const auto identifier = std::meta::identifier_of(member);
            if (identifier == "derivedValue") {
                depths[0] = depth;
            } else if (identifier == "baseValue") {
                depths[1] = depth;
            } else if (identifier == "sideValue") {
                depths[2] = depth;
            } else if (identifier == "grandBaseValue") {
                depths[3] = depth;
            }
        });
        return depths;
    }

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
static_assert(ReflectedMemberDepths() == std::array<size_t, 4>{0, 1, 1, 2});
static_assert(Sora::Meta::HasVirtualMemberFunctionInHierarchy(^^VirtualStringDerived, "ToString", 0));
static_assert(!Sora::Meta::HasVirtualMemberFunctionInHierarchy(^^NonVirtualStringDerived, "ToString", 0));
static_assert(Sora::Meta::HasInaccessibleDirectBases(^^PrivateDerived));

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

TEST_CASE("ToString partitions reflected base subobjects from directly declared members", "[Sora.Core.ToString]") {
    ReflectedDerived value;
    const std::string text = Sora::ToString(value);

    REQUIRE(text.find("derivedValue=17") != std::string::npos);
    REQUIRE(text.find(std::format("base[{}]", Sora::Traits::TypeName<ReflectedBase>)) != std::string::npos);
    REQUIRE(text.find(std::format("base[{}]", Sora::Traits::TypeName<ReflectedSideBase>)) != std::string::npos);
    REQUIRE(text.find(std::format("base[{}]", Sora::Traits::TypeName<ReflectedGrandBase>)) != std::string::npos);
    REQUIRE(text.find("grandBaseValue=7") != std::string::npos);
    REQUIRE(text.find("baseValue=11") != std::string::npos);
    REQUIRE(text.find("sideValue=13") != std::string::npos);

    const std::string inaccessible = Sora::ToString(PrivateDerived{});
    REQUIRE(inaccessible.find("base[<inaccessible>]=<incomplete>") != std::string::npos);
}

TEST_CASE("DerefPrint uses virtual ToString and diagnoses incomplete static pointer views", "[Sora.Core.ToString]") {
    const VirtualStringDerived complete;
    const NonVirtualStringDerived incomplete;
    const NonPolymorphicDerived indeterminate;
    const std::string text = Sora::ToString(
        PolymorphicPointers{.complete = &complete, .incomplete = &incomplete, .indeterminate = &indeterminate});

    REQUIRE(text.find("complete=virtual-derived") != std::string::npos);
    REQUIRE(text.find("incomplete: dynamic object rendered through non-virtual") != std::string::npos);
    REQUIRE(text.find("possibly incomplete: dynamic type is not observable") != std::string::npos);
    REQUIRE(Sora::ToString(IncompletePointer{}) ==
            std::format("{} {{value=nullptr}}", Sora::Traits::TypeName<IncompletePointer>));
}

TEST_CASE("Meta ToString renders reflected named variables as diagnostic bindings", "[Sora.Core.ToString]") {
    int answer = 42;
    int retries = 3;
    const std::string label = "ready";

    REQUIRE(Sora::Meta::ToString<^^answer>(answer) == "variable[answer : int = 42]");
    REQUIRE(Sora::Meta::ToString<^^retries>(retries) == "variable[retries : int = 3]");
    REQUIRE(Sora::Meta::ToString<^^label>(label) ==
            std::format("variable[label : {} = ready]", Sora::Meta::DisplayStringOf(Sora::Meta::TypeOf(^^label))));
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
