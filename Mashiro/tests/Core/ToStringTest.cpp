#include "Mashiro/Core/ToString.h"
#include "Mashiro/Math/Types.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <concepts>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace Mashiro;

// Free-function customizations live in their own namespace (found via ADL by the ToString CPO) so they don't clash with the Mashiro::ToString *object* under
// `using namespace Mashiro`. Only the type names are pulled in, for terse tests.
namespace Detail {

struct FreeToStringType {
    int value{};
};

std::string ToString(const FreeToStringType& v) {
    return "free-ToString:" + std::to_string(v.value);
}

struct FreeAndMemberType {
    std::string ToString() const { return "member"; }
};

std::string ToString(const FreeAndMemberType&) {
    return "free";
}

} // namespace Detail

using ::Detail::FreeToStringType;
using ::Detail::FreeAndMemberType;

struct MemberToStringType {
    int value{};

    std::string ToString() const { return "member-ToString:" + std::to_string(value); }
};

struct ConvertibleAndMemberType {
    operator std::string() const { return "converted"; }

    std::string ToString() const { return "member"; }
};

enum class Color { red, green, blue };

enum class SparseEnum : int { minus = -1, big = 1024 };

enum class AliasEnum { first = 1, second = 1 };

enum UnscopedColor { unscopedRed, unscopedGreen, unscopedBlue };

enum UnscopedFlags : unsigned { flagNone = 0, flagA = 1, flagB = 1024 };

enum class Permission : unsigned {
    none  = 0,
    read  = 1u << 0,
    write = 1u << 1,
    exec  = 1u << 2,
};

struct OstreamOnlyType {
    int value{};
};

std::ostream& operator<<(std::ostream& os, const OstreamOnlyType& v) {
    return os << "ostream:" << v.value;
}

struct PlainObject {
    int id{};
    std::string name{};
    Color color{};
};

struct NestedObject {
    PlainObject plain{};
    std::vector<Color> colors{};
};

struct EmptyObject {};

struct BitFieldObject {
    unsigned flags : 3;
    int      level : 4;
    int            : 2; // unnamed padding bit-field, must be skipped by reflection
    Color    color : 3;
    int      plain;
};

namespace {

    template<class T>
    std::string S(T&& value) {
        return std::string(Mashiro::ToString(std::forward<T>(value)));
    }

    template<class T>
    concept ToStringCallable = requires(T&& v) { Mashiro::ToString(std::forward<T>(v)); };

    template<class T>
    concept ToStringReturnsStringLike = requires(T&& v) {
        { Mashiro::ToString(std::forward<T>(v)) } -> std::convertible_to<std::string>;
    };

} // namespace

TEST_CASE("ToString accepts all common value categories", AUTO_TAG) {
    int x = 42;
    const int cx = 43;

    STATIC_REQUIRE(ToStringCallable<int&>);
    STATIC_REQUIRE(ToStringCallable<const int&>);
    STATIC_REQUIRE(ToStringCallable<int&&>);
    STATIC_REQUIRE(ToStringCallable<const int&&>);

    REQUIRE(S(x) == "42");
    REQUIRE(S(cx) == "43");
    REQUIRE(S(44) == "44");
}

TEST_CASE("ToString returns string-compatible results", AUTO_TAG) {
    STATIC_REQUIRE(ToStringReturnsStringLike<int>);
    STATIC_REQUIRE(ToStringReturnsStringLike<bool>);
    STATIC_REQUIRE(ToStringReturnsStringLike<char>);
    STATIC_REQUIRE(ToStringReturnsStringLike<std::string>);
    STATIC_REQUIRE(ToStringReturnsStringLike<Color>);
    STATIC_REQUIRE(ToStringReturnsStringLike<std::vector<int>>);
    STATIC_REQUIRE(ToStringReturnsStringLike<std::tuple<int, std::string>>);
    STATIC_REQUIRE(ToStringReturnsStringLike<PlainObject>);
    STATIC_REQUIRE(ToStringReturnsStringLike<OstreamOnlyType>);
}

TEST_CASE("ToString prefers custom free ToString", AUTO_TAG) {
    REQUIRE(S(FreeToStringType{7}) == "free-ToString:7");
    REQUIRE(S(FreeAndMemberType{}) == "free");
}

TEST_CASE("ToString prefers member ToString over string conversion", AUTO_TAG) {
    REQUIRE(S(MemberToStringType{9}) == "member-ToString:9");
    REQUIRE(S(ConvertibleAndMemberType{}) == "member");
}

TEST_CASE("ToString handles string-like values", AUTO_TAG) {
    std::string s = "hello";
    std::string_view sv = "view";
    const char* cstr = "cstr";

    REQUIRE(S(s) == "hello");
    REQUIRE(S(std::string{"world"}) == "world");
    REQUIRE(S(sv) == "view");
    REQUIRE(S(cstr) == "cstr");
}

TEST_CASE("ToString handles scalar values", AUTO_TAG) {
    REQUIRE(S(true) == "true");
    REQUIRE(S(false) == "false");
    REQUIRE(S('x') == "x");
    REQUIRE(S(123) == "123");
    REQUIRE(S(-456) == "-456");
    REQUIRE(S(3.5) == "3.500000");
}

TEST_CASE("ToString handles enum values", AUTO_TAG) {
    REQUIRE(S(Color::red) == "red");
    REQUIRE(S(Color::green) == "green");
    REQUIRE(S(Color::blue) == "blue");

    REQUIRE(S(SparseEnum::minus) == "minus");
    REQUIRE(S(SparseEnum::big) == "big");
}

TEST_CASE("ToString handles unknown enum values", AUTO_TAG) {
    auto s = S(static_cast<Color>(123));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("unknown"));
}

TEST_CASE("ToString handles enum aliases deterministically", AUTO_TAG) {
    REQUIRE(S(AliasEnum::first) == "first");
    REQUIRE(S(AliasEnum::second) == "first");
}

TEST_CASE("ToString handles unscoped enum values", AUTO_TAG) {
    REQUIRE(S(unscopedRed) == "unscopedRed");
    REQUIRE(S(unscopedGreen) == "unscopedGreen");
    REQUIRE(S(unscopedBlue) == "unscopedBlue");

    REQUIRE(S(flagNone) == "flagNone");
    REQUIRE(S(flagB) == "flagB");

    // UnscopedFlags has a fixed underlying type, so an out-of-range cast is well-defined.
    auto unknown = S(static_cast<UnscopedFlags>(99));
    REQUIRE_THAT(unknown, Catch::Matchers::ContainsSubstring("unknown"));
}

TEST_CASE("ToString decomposes flag enums", AUTO_TAG) {
    // Exact matches take precedence (including the zero value).
    REQUIRE(S(Permission::none) == "none");
    REQUIRE(S(Permission::read) == "read");

    // Bitmask decomposition for values without a single named enumerator.
    REQUIRE(S(static_cast<Permission>(0b011)) == "read | write");
    REQUIRE(S(static_cast<Permission>(0b111)) == "read | write | exec");

    // A value with bits outside any flag falls back to unknown.
    auto leftover = S(static_cast<Permission>(0b1001));
    REQUIRE_THAT(leftover, Catch::Matchers::ContainsSubstring("unknown"));
}

TEST_CASE("ToString handles tuple-like values", AUTO_TAG) {
    REQUIRE(S(std::tuple<>{}) == "()");
    REQUIRE(S(std::tuple{1}) == "(1)");
    REQUIRE(S(std::tuple{1, std::string{"abc"}, Color::blue}) == "(1, abc, blue)");
    REQUIRE(S(std::pair{2, std::string{"two"}}) == "(2, two)");
}

TEST_CASE("ToString handles range-like values", AUTO_TAG) {
    REQUIRE(S(std::vector<int>{}) == "[]");
    REQUIRE(S(std::vector<int>{1}) == "[1]");
    REQUIRE(S(std::vector<int>{1, 2, 3}) == "[1, 2, 3]");
    REQUIRE(S(std::array<std::string, 2>{"a", "b"}) == "[a, b]");
    REQUIRE(S(std::vector<Color>{Color::red, Color::green}) == "[red, green]");
}

TEST_CASE("ToString treats string as a string instead of a range", AUTO_TAG) {
    REQUIRE(S(std::string{"abc"}) == "abc");
}

TEST_CASE("ToString handles map-like ranges through pair formatting", AUTO_TAG) {
    std::map<std::string, int> m{
        {"a", 1},
        {"b", 2},
    };

    REQUIRE(S(m) == "[(a, 1), (b, 2)]");
}

TEST_CASE("ToString handles pointer values", AUTO_TAG) {
    int value = 42;
    int* p = &value;
    int* null_p = nullptr;

    auto non_null = S(p);
    REQUIRE_THAT(non_null, Catch::Matchers::ContainsSubstring("int"));
    REQUIRE_THAT(non_null, Catch::Matchers::ContainsSubstring("*("));
    REQUIRE_THAT(non_null, Catch::Matchers::ContainsSubstring("0x"));

    auto null_text = S(null_p);
    REQUIRE_THAT(null_text, Catch::Matchers::ContainsSubstring("int"));
    REQUIRE_THAT(null_text, Catch::Matchers::ContainsSubstring("nullptr"));
}

TEST_CASE("ToString uses ostream before class reflection", AUTO_TAG) {
    REQUIRE(S(OstreamOnlyType{11}) == "ostream:11");
}

TEST_CASE("ToString reflects plain objects", AUTO_TAG) {
    PlainObject obj{
        .id = 1,
        .name = "neo",
        .color = Color::green,
    };

    auto s = S(obj);

    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("PlainObject"));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("id=1"));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("name=neo"));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("color=green"));
    REQUIRE_THAT(s, !Catch::Matchers::ContainsSubstring("{, "));
}

TEST_CASE("ToString reflects nested objects recursively", AUTO_TAG) {
    NestedObject obj{
        .plain =
            PlainObject{
                .id = 2,
                .name = "trinity",
                .color = Color::red,
            },
        .colors = {Color::red, Color::blue},
    };

    auto s = S(obj);

    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("NestedObject"));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("plain="));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("id=2"));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("name=trinity"));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("colors=[red, blue]"));
}

TEST_CASE("ToString reflects empty objects", AUTO_TAG) {
    auto s = S(EmptyObject{});

    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("EmptyObject"));
    REQUIRE_THAT(s, Catch::Matchers::ContainsSubstring("{}"));
}

TEST_CASE("ToString reflects bit-field members", AUTO_TAG) {
    STATIC_REQUIRE(ToStringReturnsStringLike<BitFieldObject>);

    BitFieldObject obj{
        .flags = 5,
        .level = -3,
        .color = Color::blue,
        .plain = 42,
    };

    REQUIRE(S(obj) == "BitFieldObject {flags=5, level=-3, color=blue, plain=42}");
}

TEST_CASE("Enum to string conversion", AUTO_TAG) {
    enum class TestEnum {
        ValueA = 0,
        ValueB = 1,
        ValueC = 2,
    };

    CHECK(ToString(TestEnum::ValueA) == "ValueA");
    CHECK(ToString(TestEnum::ValueB) == "ValueB");
    CHECK(ToString(TestEnum::ValueC) == "ValueC");
}

namespace cust {

struct FreeToStringViewType {
    int value{};
};

std::string_view ToStringView(const FreeToStringViewType&) {
    return "free-view";
}

} // namespace cust

using cust::FreeToStringViewType;

struct MemberToStringViewType {
    std::string_view ToStringView() const { return "member-view"; }
};

TEST_CASE("ToStringView prefers custom free and member hooks", AUTO_TAG) {
    REQUIRE(ToStringView(FreeToStringViewType{}) == "free-view");
    REQUIRE(ToStringView(MemberToStringViewType{}) == "member-view");
}

TEST_CASE("ToStringView handles built-in types", AUTO_TAG) {
    REQUIRE(ToStringView(true) == "true");
    REQUIRE(ToStringView(false) == "false");
    REQUIRE(ToStringView(nullptr) == "nullptr");

    REQUIRE(ToStringView(Color::red) == "red");
    REQUIRE(ToStringView(Color::blue) == "blue");
    REQUIRE(ToStringView(unscopedGreen) == "unscopedGreen");
}

TEST_CASE("ToStringView falls back to the type name for unknown enums", AUTO_TAG) {
    auto sv = ToStringView(static_cast<UnscopedFlags>(99));
    REQUIRE_THAT(std::string(sv), Catch::Matchers::ContainsSubstring("UnscopedFlags"));
}

TEST_CASE("FromString reconstructs built-in values", AUTO_TAG) {
    REQUIRE(FromString<bool>("true") == true);
    REQUIRE(FromString<bool>("false") == false);

    REQUIRE(FromString<Color>("red") == Color::red);
    REQUIRE(FromString<Color>("blue") == Color::blue);
    REQUIRE(FromString<UnscopedColor>("unscopedGreen") == unscopedGreen);
}

TEST_CASE("FromString round-trips enum values through ToString", AUTO_TAG) {
    for (auto c : {Color::red, Color::green, Color::blue}) {
        REQUIRE(FromString<Color>(ToString(c)) == c);
    }
}

TEST_CASE("FromString handles null pointer text", AUTO_TAG) {
    REQUIRE(FromString<int*>("nullptr") == nullptr);
}

TEST_CASE("Str yields a string_view", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<decltype(Str(Color::red)), std::string_view>);
}

TEST_CASE("Str returns views for view-supported types", AUTO_TAG) {
    REQUIRE(Str(true) == "true");
    REQUIRE(Str(false) == "false");
    REQUIRE(Str(nullptr) == "nullptr");

    REQUIRE(Str(Color::red) == "red");
    REQUIRE(Str(Color::blue) == "blue");
    REQUIRE(Str(unscopedGreen) == "unscopedGreen");
}

TEST_CASE("Str dispatches to custom ToStringView hooks", AUTO_TAG) {
    REQUIRE(Str(FreeToStringViewType{}) == "free-view");
    REQUIRE(Str(MemberToStringViewType{}) == "member-view");
}

TEST_CASE("Enum parses scoped and unscoped enums", AUTO_TAG) {
    REQUIRE(Enum<Color>("red") == Color::red);
    REQUIRE(Enum<Color>("green") == Color::green);
    REQUIRE(Enum<Color>("blue") == Color::blue);

    REQUIRE(Enum<UnscopedColor>("unscopedGreen") == unscopedGreen);
    REQUIRE(Enum<Permission>("read") == Permission::read);
}

TEST_CASE("Enum round-trips through ToString", AUTO_TAG) {
    for (auto c : {Color::red, Color::green, Color::blue}) {
        REQUIRE(Enum<Color>(ToString(c)) == c);
    }
}

TEST_CASE("Enum is callable for enum types", AUTO_TAG) {
    STATIC_REQUIRE(requires(std::string_view sv) { Enum<Color>(sv); });
    STATIC_REQUIRE(requires(std::string_view sv) { Enum<UnscopedColor>(sv); });
}

TEST_CASE("Str is callable for view-stable types", AUTO_TAG) {
    // Accepted: custom hooks, the null-pointer literal, bool, and reflected enums.
    STATIC_REQUIRE(requires(bool b) { Str(b); });
    STATIC_REQUIRE(requires { Str(nullptr); });
    STATIC_REQUIRE(requires(Color c) { Str(c); });
    STATIC_REQUIRE(requires(UnscopedColor c) { Str(c); });
    STATIC_REQUIRE(requires(FreeToStringViewType v) { Str(v); });
    STATIC_REQUIRE(requires(MemberToStringViewType v) { Str(v); });
}

TEST_CASE("ViewStringable gates Str to types with a stable view", AUTO_TAG) {
    using Mashiro::Detail::ViewStringable;

    // Accepted: views backed by storage that outlives the call.
    STATIC_REQUIRE(ViewStringable<bool>);
    STATIC_REQUIRE(ViewStringable<std::nullptr_t>);
    STATIC_REQUIRE(ViewStringable<Color>);
    STATIC_REQUIRE(ViewStringable<UnscopedColor>);
    STATIC_REQUIRE(ViewStringable<FreeToStringViewType>);
    STATIC_REQUIRE(ViewStringable<MemberToStringViewType>);

    // Rejected: types whose view would dangle (no stable storage) — use ToString instead.
    STATIC_REQUIRE_FALSE(ViewStringable<int>);
    STATIC_REQUIRE_FALSE(ViewStringable<double>);
    STATIC_REQUIRE_FALSE(ViewStringable<std::string>);
    STATIC_REQUIRE_FALSE(ViewStringable<const char*>);
    STATIC_REQUIRE_FALSE(ViewStringable<std::vector<int>>);
    STATIC_REQUIRE_FALSE(ViewStringable<PlainObject>);
}

TEST_CASE("Str agrees with ToStringView", AUTO_TAG) {
    REQUIRE(Str(Color::green) == ToStringView(Color::green));
    REQUIRE(Str(true) == ToStringView(true));
    REQUIRE(Str(FreeToStringViewType{}) == ToStringView(FreeToStringViewType{}));
}

TEST_CASE("Str picks string_view for view-stable types and std::string otherwise", AUTO_TAG) {
    // View branch: non-owning std::string_view.
    STATIC_REQUIRE(std::same_as<decltype(Str(true)), std::string_view>);
    STATIC_REQUIRE(std::same_as<decltype(Str(nullptr)), std::string_view>);
    STATIC_REQUIRE(std::same_as<decltype(Str(Color::red)), std::string_view>);
    STATIC_REQUIRE(std::same_as<decltype(Str(unscopedGreen)), std::string_view>);
    STATIC_REQUIRE(std::same_as<decltype(Str(FreeToStringViewType{})), std::string_view>);
    STATIC_REQUIRE(std::same_as<decltype(Str(MemberToStringViewType{})), std::string_view>);

    // Owning branch: std::string by value (no dangling).
    STATIC_REQUIRE(std::same_as<decltype(Str(42)), std::string>);
    STATIC_REQUIRE(std::same_as<decltype(Str(3.5)), std::string>);
    STATIC_REQUIRE(std::same_as<decltype(Str(std::string{})), std::string>);
    STATIC_REQUIRE(std::same_as<decltype(Str(std::vector<int>{})), std::string>);
    STATIC_REQUIRE(std::same_as<decltype(Str(PlainObject{})), std::string>);
    STATIC_REQUIRE(std::same_as<decltype(Str(FreeToStringType{})), std::string>);
    STATIC_REQUIRE(std::same_as<decltype(Str(MemberToStringType{})), std::string>);
}

TEST_CASE("Str materializes an owning string for non-view types", AUTO_TAG) {
    REQUIRE(Str(42) == "42");
    REQUIRE(Str(-7) == "-7");
    REQUIRE(Str(3.5) == ToString(3.5));
    REQUIRE(Str(std::string("hello")) == "hello");

    REQUIRE(Str(FreeToStringType{7}) == "free-ToString:7");
    REQUIRE(Str(MemberToStringType{9}) == "member-ToString:9");

    PlainObject obj{1, "a", Color::red};
    REQUIRE(Str(obj) == ToString(obj));
}

TEST_CASE("Str is now total over any ToString-able type", AUTO_TAG) {
    STATIC_REQUIRE(requires(int x) { Str(x); });
    STATIC_REQUIRE(requires(double x) { Str(x); });
    STATIC_REQUIRE(requires(std::string s) { Str(s); });
    STATIC_REQUIRE(requires(std::vector<int> v) { Str(v); });
    STATIC_REQUIRE(requires(PlainObject o) { Str(o); });
}

TEST_CASE("Str owning result matches ToString for view-incapable types", AUTO_TAG) {
    REQUIRE(Str(std::vector<int>{1, 2, 3}) == ToString(std::vector<int>{1, 2, 3}));
    REQUIRE(Str(NestedObject{}) == ToString(NestedObject{}));
}

TEST_CASE("VariantLike detects only std::variant", AUTO_TAG) {
    STATIC_REQUIRE(Traits::VariantLike<std::variant<int>>);
    STATIC_REQUIRE(Traits::VariantLike<std::variant<int, std::string, Color>>);
    STATIC_REQUIRE(Traits::VariantLike<const std::variant<int>&>);

    STATIC_REQUIRE_FALSE(Traits::VariantLike<int>);
    STATIC_REQUIRE_FALSE(Traits::VariantLike<std::tuple<int, int>>);
    STATIC_REQUIRE_FALSE(Traits::VariantLike<std::vector<int>>);
    STATIC_REQUIRE_FALSE(Traits::VariantLike<PlainObject>);
}

TEST_CASE("ToString renders the active variant alternative", AUTO_TAG) {
    std::variant<int, std::string, Color> v;

    v = 42;
    REQUIRE(S(v) == "42");

    v = std::string("hello");
    REQUIRE(S(v) == "hello");

    v = Color::green;
    REQUIRE(S(v) == "green");
}

TEST_CASE("ToString recurses into composite variant alternatives", AUTO_TAG) {
    std::variant<std::vector<int>, PlainObject> v;

    v = std::vector<int>{1, 2, 3};
    REQUIRE(S(v) == ToString(std::vector<int>{1, 2, 3}));

    v = PlainObject{1, "a", Color::red};
    REQUIRE(S(v) == ToString(PlainObject{1, "a", Color::red}));
}

TEST_CASE("ToString handles variant holding std::monostate", AUTO_TAG) {
    std::variant<std::monostate, int> v;
    REQUIRE(S(v) == S(std::monostate{}));

    v = 7;
    REQUIRE(S(v) == "7");
}

TEST_CASE("ToString supports nested variants", AUTO_TAG) {
    using Inner = std::variant<int, Color>;
    std::variant<Inner, std::string> v = Inner{Color::blue};
    REQUIRE(S(v) == "blue");
}

// Regression: ToString must work on types declared in namespace Mashiro. The CPO
// design prevents the ADL probe from rediscovering the dispatcher (which used to
// recurse infinitely). vec3 also has no named _pad member, so reflection must
// surface only x/y/z.
TEST_CASE("ToString reflects Mashiro types without self-recursion or padding", AUTO_TAG) {
    const std::string out = S(vec3{1.0f, 2.0f, 3.0f});

    using Catch::Matchers::ContainsSubstring;
    REQUIRE_THAT(out, ContainsSubstring("x="));
    REQUIRE_THAT(out, ContainsSubstring("y="));
    REQUIRE_THAT(out, ContainsSubstring("z="));
    REQUIRE(out.find("_pad") == std::string::npos);
}

