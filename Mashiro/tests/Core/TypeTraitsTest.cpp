/**
 * @file TypeTraitsTest.cpp
 * @brief Tests for the reflection-based type traits in Core/TypeTraits.h.
 *
 * Coverage spans every facility group exposed by the header: class-member
 * reflection, base-class reflection, enum reflection, the compile-time
 * `TypeList` algebra, structural and standard-library categorisation concepts,
 * annotation probing, and the `Overload` visitor builder. Because the header is
 * a pure compile-time facility, the vast majority of assertions are
 * `STATIC_REQUIRE` so that any regression fails the build itself.
 */
#include "Mashiro/Core/TypeTraits.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

using namespace Mashiro;
namespace T = Mashiro::Traits;

namespace {

    // --- Fixtures: plain aggregates ------------------------------------------

    struct Point {
        int x;
        int y;
    };

    struct Mixed {
        std::int32_t id;
        double weight;
        char tag;
    };

    struct Homo {
        float a;
        float b;
        float c;
    };

    struct Empty {};

    // --- Fixtures: inheritance chains ----------------------------------------

    struct Root {
        int r;
    };
    struct Middle : Root {
        int m;
    };
    struct Leaf : Middle {
        int l;
    };

    struct Lhs {
        int a;
    };
    struct Rhs {
        int b;
    };
    struct Diamond : Lhs, Rhs {
        int c;
    };

    struct Virtual {
        virtual ~Virtual() = default;
    };

    // --- Fixtures: enums -----------------------------------------------------

    enum class Color : std::uint8_t {
        Red = 0,
        Green = 1,
        Blue = 2,
    };

    enum class Sparse : std::uint16_t {
        Lo = 1,
        Mid = 16,
        Hi = 256,
    };

    enum class NonPow2 : std::uint8_t {
        None = 0,
        One = 1,
        Three = 3,
    };

    enum Unscoped { UA, UB, UC };

    // --- Helpers for the TypeList predicate folds ----------------------------

    template <typename U>
    using IsIntegral = std::is_integral<U>;

} // namespace

// =============================================================================
// Class-member reflection
// =============================================================================

TEST_CASE("Member counts reflect the declared NSDMs", AUTO_TAG) {
    STATIC_REQUIRE(T::MembersCount<Point> == 2);
    STATIC_REQUIRE(T::MembersCount<Mixed> == 3);
    STATIC_REQUIRE(T::MembersCount<Empty> == 0);
    STATIC_REQUIRE(T::PublicMembersCount<Point> == 2);
}

TEST_CASE("MemberType splices the declared member type", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<T::MemberType<Point, 0>, int>);
    STATIC_REQUIRE(std::same_as<T::MemberType<Mixed, 0>, std::int32_t>);
    STATIC_REQUIRE(std::same_as<T::MemberType<Mixed, 1>, double>);
    STATIC_REQUIRE(std::same_as<T::MemberType<Mixed, 2>, char>);
}

TEST_CASE("MemberName and MemberNames expose source identifiers", AUTO_TAG) {
    STATIC_REQUIRE(T::MemberName<Point, 0> == "x");
    STATIC_REQUIRE(T::MemberName<Point, 1> == "y");

    constexpr auto names = T::MemberNames<Mixed>;
    STATIC_REQUIRE(names.size() == 3);
    STATIC_REQUIRE(names[0] == "id");
    STATIC_REQUIRE(names[1] == "weight");
    STATIC_REQUIRE(names[2] == "tag");
}

TEST_CASE("MemberIndex and HasMemberNamed locate members by name", AUTO_TAG) {
    STATIC_REQUIRE(T::MemberIndex<Mixed>("id") == 0);
    STATIC_REQUIRE(T::MemberIndex<Mixed>("weight") == 1);
    STATIC_REQUIRE(T::MemberIndex<Mixed>("tag") == 2);
    STATIC_REQUIRE(T::MemberIndex<Mixed>("missing") == T::kNotFound);

    STATIC_REQUIRE(T::HasMemberNamed<Mixed>("weight"));
    STATIC_REQUIRE_FALSE(T::HasMemberNamed<Mixed>("nope"));
}

TEST_CASE("MemberOffset matches the C++ object layout", AUTO_TAG) {
    STATIC_REQUIRE(T::MemberOffset<Point, 0> == offsetof(Point, x));
    STATIC_REQUIRE(T::MemberOffset<Point, 1> == offsetof(Point, y));
    STATIC_REQUIRE(T::MemberOffset<Mixed, 1> == offsetof(Mixed, weight));
}

TEST_CASE("Layout queries distinguish compact and homogeneous types", AUTO_TAG) {
    STATIC_REQUIRE(T::MemberBytesTotal<Point> == sizeof(int) * 2);
    STATIC_REQUIRE(T::PaddingBytes<Point> == 0);
    STATIC_REQUIRE(T::Compact<Point>);
    STATIC_REQUIRE(T::Compact<Homo>);

    STATIC_REQUIRE(T::Homogeneous<Homo>);
    STATIC_REQUIRE(T::Homogeneous<Point>);
    STATIC_REQUIRE_FALSE(T::Homogeneous<Mixed>);
    STATIC_REQUIRE_FALSE(T::Homogeneous<Empty>);
}

// =============================================================================
// Base-class reflection
// =============================================================================

TEST_CASE("Base reflection exposes direct bases", AUTO_TAG) {
    STATIC_REQUIRE(T::BasesCount<Root> == 0);
    STATIC_REQUIRE(T::BasesCount<Middle> == 1);
    STATIC_REQUIRE(T::BasesCount<Diamond> == 2);

    STATIC_REQUIRE(std::same_as<T::BaseType<Middle, 0>, Root>);
    STATIC_REQUIRE(std::same_as<T::BaseType<Diamond, 0>, Lhs>);
    STATIC_REQUIRE(std::same_as<T::BaseType<Diamond, 1>, Rhs>);
}

TEST_CASE("RootClass and SingleInheritedClass classify hierarchies", AUTO_TAG) {
    STATIC_REQUIRE(T::RootClass<Root>);
    STATIC_REQUIRE_FALSE(T::RootClass<Middle>);
    STATIC_REQUIRE_FALSE(T::RootClass<int>);

    STATIC_REQUIRE(T::SingleInheritedClass<Root>);
    STATIC_REQUIRE(T::SingleInheritedClass<Middle>);
    STATIC_REQUIRE(T::SingleInheritedClass<Leaf>);
    STATIC_REQUIRE_FALSE(T::SingleInheritedClass<Diamond>);
}

TEST_CASE("UniqueIdentifier builds a dotted root-to-derived path", AUTO_TAG) {
    STATIC_REQUIRE(T::UniqueIdentifier<Root> == "Root");
    STATIC_REQUIRE(T::UniqueIdentifier<Middle> == "Root.Middle");
    STATIC_REQUIRE(T::UniqueIdentifier<Leaf> == "Root.Middle.Leaf");
}

// =============================================================================
// Enum reflection
// =============================================================================

TEST_CASE("Enum reflection enumerates values and names", AUTO_TAG) {
    STATIC_REQUIRE(T::EnumeratorsCount<Color> == 3);
    STATIC_REQUIRE(std::same_as<T::EnumUnderlying<Color>, std::uint8_t>);

    constexpr auto values = T::EnumValues<Color>;
    STATIC_REQUIRE(values.size() == 3);
    STATIC_REQUIRE(values[0] == Color::Red);
    STATIC_REQUIRE(values[2] == Color::Blue);

    constexpr auto names = T::EnumNames<Color>;
    STATIC_REQUIRE(names[0] == "Red");
    STATIC_REQUIRE(names[1] == "Green");
    STATIC_REQUIRE(names[2] == "Blue");
}

TEST_CASE("EnumName and EnumCast round-trip values and names", AUTO_TAG) {
    STATIC_REQUIRE(T::EnumName(Color::Green) == "Green");
    STATIC_REQUIRE(T::EnumName(static_cast<Color>(99)).empty());

    STATIC_REQUIRE(T::EnumCast<Color>("Blue") == Color::Blue);
    STATIC_REQUIRE_FALSE(T::EnumCast<Color>("Purple").has_value());

    // Round-trip property for every declared enumerator.
    template for (constexpr auto e : T::Enumerators<Color>) {
        constexpr auto value = std::meta::extract<Color>(e);
        STATIC_REQUIRE(T::EnumCast<Color>(T::EnumName(value)) == value);
    }
}

TEST_CASE("EnumeratorName resolves a single enumerator", AUTO_TAG) {
    STATIC_REQUIRE(T::EnumeratorName<Color, Color::Red>() == "Red");
    STATIC_REQUIRE(T::EnumeratorName<Color, static_cast<Color>(42)>().empty());
}

TEST_CASE("SequentialEnum and BitfieldEnum categorise enums", AUTO_TAG) {
    STATIC_REQUIRE(T::SequentialEnum<Color>);
    STATIC_REQUIRE_FALSE(T::SequentialEnum<Sparse>);
    STATIC_REQUIRE_FALSE(T::SequentialEnum<NonPow2>);

    STATIC_REQUIRE(T::BitfieldEnum<Sparse>);
    STATIC_REQUIRE_FALSE(T::BitfieldEnum<NonPow2>);
    STATIC_REQUIRE(T::kBitfieldMask<Sparse> ==
                   static_cast<Sparse>(1 | 16 | 256));
}

// =============================================================================
// TypeList algebra
// =============================================================================

namespace {

    using L0 = T::TypeList<>;
    using L3 = T::TypeList<int, double, char>;
    using LDup = T::TypeList<int, double, int, char, double>;

} // namespace

TEST_CASE("TypeList element access", AUTO_TAG) {
    STATIC_REQUIRE(T::Length<T::TypeList, int, double, char> == 3);
    STATIC_REQUIRE(L3::size == 3);
    STATIC_REQUIRE(std::same_as<T::At<L3, 1>, double>);
    STATIC_REQUIRE(std::same_as<T::Head<L3>, int>);
    STATIC_REQUIRE(std::same_as<T::Last<L3>, char>);
    STATIC_REQUIRE(std::same_as<T::Tail<L3>, T::TypeList<double, char>>);
}

TEST_CASE("TypeList structural mutation", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<T::PushFront<bool, L3>,
                                T::TypeList<bool, int, double, char>>);
    STATIC_REQUIRE(std::same_as<T::PushBack<L3, bool>,
                                T::TypeList<int, double, char, bool>>);
    STATIC_REQUIRE(std::same_as<T::PopBack<L3>, T::TypeList<int, double>>);
    STATIC_REQUIRE(std::same_as<T::PopBack<L0>, T::TypeList<>>);
    STATIC_REQUIRE(std::same_as<T::Reverse<L3>, T::TypeList<char, double, int>>);
    STATIC_REQUIRE(std::same_as<T::Concat<L3, T::TypeList<bool>>,
                                T::TypeList<int, double, char, bool>>);
    STATIC_REQUIRE(std::same_as<T::Unique<LDup>, T::TypeList<int, double, char>>);
}

TEST_CASE("TypeList search predicates", AUTO_TAG) {
    STATIC_REQUIRE(T::IndexOf<L3, double> == 1);
    STATIC_REQUIRE(T::IndexOf<L3, bool> == static_cast<std::size_t>(-1));
    STATIC_REQUIRE(T::Contains<L3, char>);
    STATIC_REQUIRE_FALSE(T::Contains<L3, bool>);
}

TEST_CASE("TypeList higher-order combinators", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<T::MapT<std::add_pointer_t, L3>,
                                T::TypeList<int*, double*, char*>>);
    STATIC_REQUIRE(std::same_as<T::FilterT<std::is_integral, L3>,
                                T::TypeList<int, char>>);
    STATIC_REQUIRE(std::same_as<T::ApplyT<std::tuple, L3>,
                                std::tuple<int, double, char>>);
}

TEST_CASE("TypeList quantifier folds", AUTO_TAG) {
    STATIC_REQUIRE(T::AllOf<std::is_arithmetic, L3>);
    STATIC_REQUIRE_FALSE(T::AllOf<IsIntegral, L3>);
    STATIC_REQUIRE(T::AnyOf<IsIntegral, L3>);
    STATIC_REQUIRE_FALSE(T::AnyOf<std::is_pointer, L3>);
    STATIC_REQUIRE(T::NoneOf<std::is_pointer, L3>);
    STATIC_REQUIRE(T::CountIf<IsIntegral, L3> == 2);

    // Vacuous truth / falsehood on the empty list.
    STATIC_REQUIRE(T::AllOf<std::is_integral, L0>);
    STATIC_REQUIRE_FALSE(T::AnyOf<std::is_integral, L0>);
    STATIC_REQUIRE(T::CountIf<std::is_integral, L0> == 0);
}

TEST_CASE("ToTypeList bridges reflection to the type-list algebra", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<T::ToTypeList<Mixed>,
                                T::TypeList<std::int32_t, double, char>>);
    STATIC_REQUIRE(std::same_as<T::ToTypeList<Homo>,
                                T::TypeList<float, float, float>>);
}

// =============================================================================
// Categorisation concepts
// =============================================================================

TEST_CASE("SpecializationOf detects class-template instances", AUTO_TAG) {
    STATIC_REQUIRE(T::SpecializationOf<std::vector<int>, std::vector>);
    STATIC_REQUIRE(T::SpecializationOf<std::optional<int>, std::optional>);
    STATIC_REQUIRE(T::SpecializationOf<std::variant<int, char>, std::variant>);
    STATIC_REQUIRE(T::SpecializationOf<const std::vector<int>&, std::vector>);
    STATIC_REQUIRE_FALSE(T::SpecializationOf<int, std::vector>);
    STATIC_REQUIRE_FALSE(T::SpecializationOf<Point, std::vector>);
    STATIC_REQUIRE_FALSE(T::SpecializationOf<std::optional<int>, std::vector>);
}

TEST_CASE("Fundamental type categorisation concepts", AUTO_TAG) {
    STATIC_REQUIRE(T::Aggregate<Point>);
    STATIC_REQUIRE_FALSE(T::Aggregate<int>);

    STATIC_REQUIRE(T::StandardLayoutType<Point>);
    STATIC_REQUIRE(T::TriviallyCopyableType<Point>);

    STATIC_REQUIRE(T::EmptyType<Empty>);
    STATIC_REQUIRE_FALSE(T::EmptyType<Point>);

    STATIC_REQUIRE(T::PolymorphicType<Virtual>);
    STATIC_REQUIRE_FALSE(T::PolymorphicType<Point>);

    STATIC_REQUIRE(T::UniquelyRepresented<int>);

    STATIC_REQUIRE(T::Reflectable<Point>);
    STATIC_REQUIRE(T::Reflectable<Color>);
    STATIC_REQUIRE_FALSE(T::Reflectable<int>);
}

TEST_CASE("Enumeration sub-concepts distinguish scoped and unscoped", AUTO_TAG) {
    STATIC_REQUIRE(T::Enumeration<Color>);
    STATIC_REQUIRE(T::Enumeration<Unscoped>);

    STATIC_REQUIRE(T::ScopedEnum<Color>);
    STATIC_REQUIRE_FALSE(T::ScopedEnum<Unscoped>);

    STATIC_REQUIRE(T::UnscopedEnum<Unscoped>);
    STATIC_REQUIRE_FALSE(T::UnscopedEnum<Color>);
}

// =============================================================================
// Structural & standard-library concepts
// =============================================================================

TEST_CASE("Tuple and variant structural concepts", AUTO_TAG) {
    STATIC_REQUIRE(T::TupleLike<std::tuple<int, double>>);
    STATIC_REQUIRE(T::TupleLike<std::pair<int, char>>);
    STATIC_REQUIRE_FALSE(T::TupleLike<int>);

    STATIC_REQUIRE(T::VariantLike<std::variant<int, double>>);
    STATIC_REQUIRE_FALSE(T::VariantLike<std::tuple<int>>);
}

TEST_CASE("Standard-library categorisation concepts", AUTO_TAG) {
    STATIC_REQUIRE(T::StdOptional<std::optional<int>>);
    STATIC_REQUIRE_FALSE(T::StdOptional<int>);

    STATIC_REQUIRE(T::StdVariant<std::variant<int, char>>);
    STATIC_REQUIRE_FALSE(T::StdVariant<std::tuple<int>>);

    STATIC_REQUIRE(T::ChronoDuration<std::chrono::milliseconds>);
    STATIC_REQUIRE(T::ChronoTimePoint<
                   std::chrono::time_point<std::chrono::steady_clock>>);
    STATIC_REQUIRE(T::FilesystemPath<std::filesystem::path>);

    STATIC_REQUIRE(T::ByteRange<std::vector<std::byte>>);
    STATIC_REQUIRE_FALSE(T::ByteRange<std::vector<int>>);

    STATIC_REQUIRE(T::StringViewConvertible<std::string>);
    STATIC_REQUIRE(T::StringViewConvertible<const char*>);
    STATIC_REQUIRE_FALSE(T::StringViewConvertible<std::filesystem::path>);

    STATIC_REQUIRE(T::StringKeyedAssociative<std::map<std::string, int>>);
    STATIC_REQUIRE_FALSE(T::StringKeyedAssociative<std::map<int, int>>);
}

// =============================================================================
// Annotations (C++26 [[=...]])
// =============================================================================

namespace {

    struct Ignore {};
    struct Key {};
    struct Order {
        int priority;
    };

    struct Annotated {
        [[=Ignore{}]] int hidden;
        int kept;
        [[=Order{5}]] int ordered;
    };

    struct Whitelisted {
        [[=Key{}]] int included;
        int excluded;
    };

} // namespace

TEST_CASE("Annotation probing detects tag presence and payload", AUTO_TAG) {
    STATIC_REQUIRE(T::Anno::Has<Ignore>(T::Members<Annotated>[0]));
    STATIC_REQUIRE_FALSE(T::Anno::Has<Ignore>(T::Members<Annotated>[1]));

    STATIC_REQUIRE(T::Anno::Get<Order>(T::Members<Annotated>[2]).has_value());
    STATIC_REQUIRE(T::Anno::Get<Order>(T::Members<Annotated>[2])->priority == 5);
    STATIC_REQUIRE_FALSE(T::Anno::Get<Order>(T::Members<Annotated>[1]).has_value());
}

TEST_CASE("AnyMemberHas engages whitelist detection", AUTO_TAG) {
    STATIC_REQUIRE(T::Anno::AnyMemberHas<Whitelisted, Key>());
    STATIC_REQUIRE_FALSE(T::Anno::AnyMemberHas<Annotated, Key>());
}

// =============================================================================
// Overload visitor builder
// =============================================================================

TEST_CASE("Overload composes callables for std::visit", AUTO_TAG) {
    std::variant<int, std::string, double> v = std::string{"hi"};

    auto label = std::visit(T::Overload{
                                [](int) { return std::string{"int"}; },
                                [](const std::string&) { return std::string{"str"}; },
                                [](double) { return std::string{"double"}; },
                            },
                            v);
    REQUIRE(label == "str");

    v = 42;
    label = std::visit(T::Overload{
                           [](int) { return std::string{"int"}; },
                           [](auto&&) { return std::string{"other"}; },
                       },
                       v);
    REQUIRE(label == "int");
}
