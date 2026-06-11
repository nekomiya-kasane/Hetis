// SPDX-License-Identifier: MIT
//
// Tests for Mashiro::ToJson / Mashiro::FromJson — reflection + annotation
// driven JSON serialisation built on nlohmann/json 3.12.

#include "Mashiro/Core/Flags.h"
#include "Mashiro/Core/ToJson.h"
#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace Mashiro;
namespace An = Mashiro::Json::Anno;

// -------------------------------------------------------------------------
// Fixture types — exercise scalar / enum / nested / annotated paths.
// -------------------------------------------------------------------------

namespace TestTypes {

    enum class Color { Red, Green, Blue };

    enum class Permissions : uint8_t {
        None = 0,
        Read = 1u << 0,
        Write = 1u << 1,
        Exec = 1u << 2,
    };

    enum class IntCoded : int { A = 1, B = 2, C = 3 };

    struct Vec3 {
        float x = 0, y = 0, z = 0;
    };

    struct Person {
        std::string name;
        int age = 0;
        Color favoriteColor = Color::Red;
        std::vector<std::string> hobbies;
    };

    struct Annotated {
        int kept = 1;
        [[= An::Ignore{}]] int skipped = 99;
        [[= An::Rename<"display">{}]] std::string label;
        [[= An::Order{0}]] int first = 0;
        [[= An::Order{1}]] int second = 0;
        [[= An::Optional{}]] int opt = 7;
        [[= An::Required{}]] int req = 0;
    };

    struct Inner {
        int a = 0;
        int b = 0;
    };

    struct Outer {
        [[= An::Flatten{}]] Inner inner;
        std::string name;
    };

    struct WithKey {
        [[= An::Key{}]] int identity = 0;
        [[= An::Key{}]] std::string label;
        int hidden = 999;
    };

    struct OptHolder {
        std::optional<int> opt;
        std::optional<std::string> name;
    };

    struct Variants {
        std::variant<int, std::string, double> v = 0;
    };

    struct WithDefaults {
        [[= An::EmitDefault{false}]] int skipWhenZero = 0;
        [[= An::EmitDefault{false}]] std::string skipWhenEmpty;
        int always = 0;
    };

    struct CustomMember {
        int payload = 0;
        json ToJson() const { return json{{"x", payload * 2}}; }
        static CustomMember FromJson(const json& j) {
            return CustomMember{j.at("x").get<int>() / 2};
        }
    };

    struct EnumIntCoded {
        [[= An::AsInt{}]] Color tag = Color::Red;
    };

} // namespace TestTypes

using namespace TestTypes;

// =========================================================================
// Section 1 — Scalars / strings / bool / null
// =========================================================================

TEST_CASE("ToJson handles scalars", AUTO_TAG) {
    REQUIRE(ToJson(42) == json(42));
    REQUIRE(ToJson(3.5) == json(3.5));
    REQUIRE(ToJson(true) == json(true));
    REQUIRE(ToJson(nullptr) == json(nullptr));
    REQUIRE(ToJson(std::string("hi")) == json("hi"));
    REQUIRE(ToJson(std::string_view("sv")) == json("sv"));
}

TEST_CASE("FromJson handles scalars", AUTO_TAG) {
    REQUIRE(FromJson<int>(json(42)) == 42);
    REQUIRE(FromJson<double>(json(3.5)) == 3.5);
    REQUIRE(FromJson<bool>(json(true)) == true);
    REQUIRE(FromJson<std::string>(json("hi")) == "hi");
}

// =========================================================================
// Section 2 — Enums (name, bitmask, AsInt)
// =========================================================================

TEST_CASE("Enum serialises by name by default", AUTO_TAG) {
    REQUIRE(ToJson(Color::Green) == json("Green"));
    REQUIRE(FromJson<Color>(json("Blue")) == Color::Blue);
}

TEST_CASE("Bitfield enum decomposes into '|' separated names", AUTO_TAG) {
    auto j = ToJson(Permissions::Read | Permissions::Write);
    REQUIRE(j.is_string());
    auto sv = j.get<std::string>();
    REQUIRE(sv == "Read|Write");

    auto round = FromJson<Permissions>(j);
    REQUIRE(round == (Permissions::Read | Permissions::Write));
}

TEST_CASE("Bitfield enum: zero serialises as '0' and parses back", "[ToJson][enum][bitfield]") {
    auto j = ToJson(Permissions::None);
    REQUIRE(j == json("0"));
    REQUIRE(FromJson<Permissions>(j) == Permissions::None);
}

TEST_CASE("AsInt annotation forces integer encoding", AUTO_TAG) {
    EnumIntCoded e{Color::Blue};
    auto j = ToJson(e);
    REQUIRE(j.at("tag").is_number_integer());
    REQUIRE(j.at("tag").get<int>() == static_cast<int>(Color::Blue));

    auto round = FromJson<EnumIntCoded>(j);
    REQUIRE(round.tag == Color::Blue);
}

TEST_CASE("FromJson: unknown enumerator throws", AUTO_TAG) {
    REQUIRE_THROWS(FromJson<Color>(json("Yellow")));
}

// =========================================================================
// Section 3 — Aggregates via reflection
// =========================================================================

TEST_CASE("Reflectable struct round-trips", AUTO_TAG) {
    Person p{"Alice", 30, Color::Green, {"reading", "hiking"}};
    auto j = ToJson(p);

    REQUIRE(j.at("name") == "Alice");
    REQUIRE(j.at("age") == 30);
    REQUIRE(j.at("favoriteColor") == "Green");
    REQUIRE(j.at("hobbies").size() == 2);

    auto back = FromJson<Person>(j);
    REQUIRE(back.name == p.name);
    REQUIRE(back.age == p.age);
    REQUIRE(back.favoriteColor == p.favoriteColor);
    REQUIRE(back.hobbies == p.hobbies);
}

TEST_CASE("Nested vec3 reflectable", AUTO_TAG) {
    Vec3 v{1.0f, 2.0f, 3.0f};
    auto j = ToJson(v);
    REQUIRE(j.at("x") == 1.0);
    REQUIRE(j.at("y") == 2.0);
    REQUIRE(j.at("z") == 3.0);
    auto back = FromJson<Vec3>(j);
    REQUIRE(back.x == v.x);
    REQUIRE(back.y == v.y);
    REQUIRE(back.z == v.z);
}

// =========================================================================
// Section 4 — Annotations: Ignore / Rename / Order / Optional / Required
// =========================================================================

TEST_CASE("Ignore + Rename annotations apply", AUTO_TAG) {
    Annotated a{};
    a.kept = 5;
    a.skipped = 99;
    a.label = "hello";
    a.req = 10;
    auto j = ToJson(a);

    REQUIRE_FALSE(j.contains("skipped"));
    REQUIRE(j.contains("display"));
    REQUIRE(j.at("display") == "hello");
    REQUIRE(j.at("kept") == 5);
}

TEST_CASE("Required missing key throws", AUTO_TAG) {
    json j = json::object();
    j["kept"] = 5;
    j["display"] = "x";
    j["first"] = 1;
    j["second"] = 2;
    REQUIRE_THROWS(FromJson<Annotated>(j));
}

TEST_CASE("Optional missing key tolerated", AUTO_TAG) {
    json j = json::object();
    j["kept"] = 5;
    j["display"] = "x";
    j["first"] = 1;
    j["second"] = 2;
    j["req"] = 10;
    auto back = FromJson<Annotated>(j);
    REQUIRE(back.opt == 7);
    REQUIRE(back.req == 10);
}

// =========================================================================
// Section 5 — Flatten
// =========================================================================

TEST_CASE("Flatten splices nested object", AUTO_TAG) {
    Outer o{};
    o.inner.a = 1;
    o.inner.b = 2;
    o.name = "n";
    auto j = ToJson(o);
    REQUIRE(j.contains("a"));
    REQUIRE(j.contains("b"));
    REQUIRE(j.contains("name"));
    REQUIRE_FALSE(j.contains("inner"));

    auto back = FromJson<Outer>(j);
    REQUIRE(back.inner.a == 1);
    REQUIRE(back.inner.b == 2);
    REQUIRE(back.name == "n");
}

// =========================================================================
// Section 6 — Key whitelist
// =========================================================================

TEST_CASE("Key annotation engages whitelist mode", AUTO_TAG) {
    WithKey w{};
    w.identity = 5;
    w.label = "name";
    w.hidden = 999;
    auto j = ToJson(w);
    REQUIRE(j.contains("identity"));
    REQUIRE(j.contains("label"));
    REQUIRE_FALSE(j.contains("hidden"));
}

// =========================================================================
// Section 7 — Optional / variant / containers
// =========================================================================

TEST_CASE("optional<T> serialises as null when empty", AUTO_TAG) {
    OptHolder h{};
    auto j = ToJson(h);
    REQUIRE(j.at("opt").is_null());
    REQUIRE(j.at("name").is_null());

    h.opt = 42;
    h.name = "set";
    j = ToJson(h);
    REQUIRE(j.at("opt") == 42);
    REQUIRE(j.at("name") == "set");

    auto back = FromJson<OptHolder>(j);
    REQUIRE(back.opt == 42);
    REQUIRE(back.name == "set");
}

TEST_CASE("variant<...> picks the first matching alternative", AUTO_TAG) {
    Variants v;
    v.v = 7;
    REQUIRE(ToJson(v).at("v") == 7);

    v.v = std::string("hi");
    REQUIRE(ToJson(v).at("v") == "hi");

    v.v = 3.14;
    REQUIRE(ToJson(v).at("v") == 3.14);
}

TEST_CASE("std::vector / std::array / std::map round-trip", AUTO_TAG) {
    std::vector<int> v{1, 2, 3};
    auto j = ToJson(v);
    REQUIRE(j == json::array({1, 2, 3}));
    REQUIRE(FromJson<std::vector<int>>(j) == v);

    std::array<int, 3> a{4, 5, 6};
    j = ToJson(a);
    REQUIRE(j == json::array({4, 5, 6}));
    auto rounded = FromJson<std::array<int, 3>>(j);
    REQUIRE(rounded == a);

    std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    j = ToJson(m);
    REQUIRE(j.at("a") == 1);
    REQUIRE(j.at("b") == 2);
    auto roundedM = FromJson<std::map<std::string, int>>(j);
    REQUIRE(roundedM == m);
}

TEST_CASE("std::pair / std::tuple serialise as JSON arrays", AUTO_TAG) {
    auto p = std::pair<int, std::string>{1, "x"};
    auto j = ToJson(p);
    REQUIRE(j.is_array());
    REQUIRE(j[0] == 1);
    REQUIRE(j[1] == "x");

    auto t = std::tuple<int, double, std::string>{1, 2.5, "z"};
    j = ToJson(t);
    REQUIRE(j == json::array({1, 2.5, "z"}));
}

// =========================================================================
// Section 8 — Custom hooks (member ToJson / FromJson)
// =========================================================================

TEST_CASE("Member ToJson / FromJson are honoured", AUTO_TAG) {
    CustomMember c{};
    c.payload = 21;
    auto j = ToJson(c);
    REQUIRE(j.at("x") == 42);
    auto back = FromJson<CustomMember>(j);
    REQUIRE(back.payload == 21);
}

// =========================================================================
// Section 9 — chrono / filesystem
// =========================================================================

TEST_CASE("chrono::duration serialises in ns count", AUTO_TAG) {
    using namespace std::chrono_literals;
    auto j = ToJson(123ms);
    REQUIRE(j.is_number_integer());
    REQUIRE(j.get<int64_t>() == 123'000'000);
    REQUIRE(FromJson<std::chrono::milliseconds>(j) == 123ms);
}

TEST_CASE("filesystem::path round-trips", AUTO_TAG) {
    std::filesystem::path p = "/tmp/foo.txt";
    auto j = ToJson(p);
    REQUIRE(j.is_string());
    auto back = FromJson<std::filesystem::path>(j);
    REQUIRE(back == p);
}

// =========================================================================
// Section 10 — EmitDefault: skip zero-valued fields
// =========================================================================

TEST_CASE("EmitDefault{false} skips zero / empty values", AUTO_TAG) {
    WithDefaults d{};
    auto j = ToJson(d);
    REQUIRE_FALSE(j.contains("skipWhenZero"));
    REQUIRE_FALSE(j.contains("skipWhenEmpty"));
    REQUIRE(j.contains("always"));

    d.skipWhenZero = 5;
    d.skipWhenEmpty = "x";
    j = ToJson(d);
    REQUIRE(j.at("skipWhenZero") == 5);
    REQUIRE(j.at("skipWhenEmpty") == "x");
}

// =========================================================================
// Section 11 — nlohmann::adl_serializer bridge
// =========================================================================

TEST_CASE("nlohmann adl_serializer specialisation lets json j = v;", AUTO_TAG) {
    Person p{"Bob", 25, Color::Blue, {"sleep"}};
    json j = p;
    REQUIRE(j.at("name") == "Bob");
    Person back = j.template get<Person>();
    REQUIRE(back.name == "Bob");
    REQUIRE(back.favoriteColor == Color::Blue);
}

// =========================================================================
// Section 12 — dump / parse round-trips
// =========================================================================

TEST_CASE("dump → parse round-trip preserves content", AUTO_TAG) {
    Person p{"C", 1, Color::Red, {"a", "b"}};
    auto j = ToJson(p);
    auto text = j.dump();
    auto reparsed = json::parse(text);
    auto back = FromJson<Person>(reparsed);
    REQUIRE(back.name == p.name);
    REQUIRE(back.hobbies == p.hobbies);
}
