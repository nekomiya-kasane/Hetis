#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Core/Traits/MetaError.h>
#include <Sora/Core/Traits/MetaInvocable.h>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <type_traits>

namespace {

    using List = Sora::Traits::TypeList<int, double, char>;
    using Empty = Sora::Traits::TypeList<>;

    consteval bool IsIntegral(std::meta::info type) noexcept {
        return std::meta::is_integral_type(type);
    }

    consteval bool IsClass(std::meta::info type) noexcept {
        return std::meta::is_class_type(type);
    }

    consteval int NotBoolean(std::meta::info) noexcept {
        return 1;
    }

} // namespace

TEST_CASE("TypeList core queries", "[Sora][Core][TypeTraits]") {
    STATIC_REQUIRE(Sora::Traits::Length<List> == 3);
    STATIC_REQUIRE(Sora::Traits::Length<Empty> == 0);
    STATIC_REQUIRE(std::same_as<Sora::Traits::At<List, 1>, double>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Head<List>, int>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Last<List>, char>);
    STATIC_REQUIRE(List{}.IndexOf<int>() == 0);
    STATIC_REQUIRE(List{}.IndexOf<char>() == 2);
    STATIC_REQUIRE(List{}.Contains<double>());
    STATIC_REQUIRE_FALSE(List{}.Contains<bool>());
}

TEST_CASE("TypeList structural transforms", "[Sora][Core][TypeTraits]") {
    STATIC_REQUIRE(std::same_as<decltype(List{}.Concat<bool, short>()),
                                Sora::Traits::TypeList<int, double, char, bool, short>>);
    STATIC_REQUIRE(std::same_as<decltype(List{}.Prepend<bool, short>()),
                                Sora::Traits::TypeList<bool, short, int, double, char>>);
    STATIC_REQUIRE(std::same_as<decltype(List{}.PrependUnique<int, bool>()),
                                Sora::Traits::TypeList<int, bool, double, char>>);
    STATIC_REQUIRE(std::same_as<decltype(List{}.Append<int, bool>()),
                                Sora::Traits::TypeList<int, double, char, int, bool>>);
    STATIC_REQUIRE(std::same_as<decltype(List{}.AppendUnique<int, char, short>()),
                                Sora::Traits::TypeList<int, double, char, short>>);
}

TEST_CASE("Reflection-native meta invocation", "[Sora][Core][MetaInvocable]") {
    using Pair = Sora::Meta::InvokeResult<^^std::pair, int, double>;
    STATIC_REQUIRE(std::same_as<Pair, std::pair<int, double>>);

    constexpr auto invalid = Sora::Meta::Invoke<^^std::pair, ^^int>;
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalid));
    STATIC_REQUIRE(Sora::Meta::ErrorContextCount(invalid) == 2);

    constexpr auto invalidList =
        Sora::Meta::ApplyToTypeList(^^std::pair, ^^Sora::Traits::TypeList<int>);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalidList));

    using Input = Sora::Traits::TypeList<int, double>;
    using Tuple = Sora::Traits::Apply<^^std::tuple, Input>;
    STATIC_REQUIRE(std::same_as<Tuple, std::tuple<int, double>>);
}

TEST_CASE("Reflection-native meta errors preserve provenance", "[Sora][Core][MetaError]") {
    struct Category {};
    constexpr auto error = Sora::Meta::MakeMetaError(^^Category, {^^int, ^^double});
    STATIC_REQUIRE(Sora::Meta::IsMetaError(error));
    STATIC_REQUIRE(Sora::Meta::ErrorTagInfo(error) == ^^Category);
    STATIC_REQUIRE(Sora::Meta::ErrorContextCount(error) == 2);
    STATIC_REQUIRE(Sora::Meta::ErrorContextInfos(error)[0] == ^^int);
    STATIC_REQUIRE(Sora::Meta::ErrorContextInfos(error)[1] == ^^double);
}

TEST_CASE("Reflection-native predicates compose with TypeList", "[Sora][Core][MetaPredicate]") {
    using Input = Sora::Traits::TypeList<int, double, long>;
    using Integral = Sora::Traits::FilterT<^^IsIntegral, Input>;

    STATIC_REQUIRE(std::same_as<Integral, Sora::Traits::TypeList<int, long>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::FindFirstIf<^^IsClass, Input>, void>);
    STATIC_REQUIRE_FALSE(Sora::Traits::AllOf<^^IsIntegral, Input>);
    STATIC_REQUIRE_FALSE(Sora::Traits::AnyOf<^^IsClass, Input>);
    STATIC_REQUIRE(Sora::Traits::NoneOf<^^IsClass, Input>);
    STATIC_REQUIRE(Sora::Traits::CountIf<^^IsIntegral, Input> == 2);

    constexpr auto invalid = Sora::Meta::FilterInfo(^^NotBoolean, ^^Input);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalid));
    STATIC_REQUIRE(Sora::Meta::ErrorTagInfo(invalid) ==
                   ^^Sora::Meta::Error::NonBooleanPredicateResultTag);
}
