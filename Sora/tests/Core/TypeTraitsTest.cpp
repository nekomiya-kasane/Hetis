#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Core/Traits/MetaError.h>
#include <Sora/Core/Traits/MetaInvocable.h>
#include <Sora/Core/Traits/MetaOperators.h>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <type_traits>

namespace {

    enum class SignedEnum : int { Value = -1 };
    enum class UnsignedEnum : unsigned { Value = 1 };

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

    consteval bool Always() noexcept {
        return true;
    }

    consteval bool Same(std::meta::info left, std::meta::info right) noexcept {
        return left == right;
    }

    consteval bool AllDistinct(std::meta::info first, std::meta::info second, std::meta::info third) noexcept {
        return first != second && first != third && second != third;
    }

    consteval bool WrongArgument(int) noexcept {
        return true;
    }

    consteval int NotBoolean2(std::meta::info, std::meta::info) noexcept {
        return 1;
    }

    template<std::meta::info Pred>
    concept HasPredArity = requires { Sora::Traits::PredArity<Pred>; };

    template<typename Left, typename Right>
    using Pair = std::pair<Left, Right>;

    template<typename Left, typename Right>
    inline constexpr bool SameType = std::same_as<Left, Right>;

    template<typename Type>
        requires std::integral<Type>
    using IntegralIdentity = Type;

    template<auto Value>
    using ConstantType = std::integral_constant<decltype(Value), Value>;

    template<typename Type>
    inline constexpr std::size_t TypeSize = sizeof(Type);

    template<typename Left, typename Right>
    inline constexpr int PairSize = sizeof(Left) + sizeof(Right);

    template<typename Type>
    using PairWithConst = Sora::Traits::TypeList<Type, const Type>;

    template<typename>
    using EmptyExpansion = Sora::Traits::TypeList<>;

    consteval Sora::Meta::InfoList PairInfos(std::meta::info type) {
        const auto constType = std::meta::dealias(std::meta::substitute(^^std::add_const_t, {type}));
        return {type, constType};
    }

    consteval Sora::Meta::InfoList EmptyInfos(std::meta::info) {
        return {};
    }

    static_assert(std::meta::return_type_of(^^PairInfos) == ^^Sora::Meta::InfoList);
    constexpr auto directPairInfos = std::meta::reflect_invoke(^^PairInfos, {std::meta::reflect_constant(^^int)});
    static_assert(std::meta::type_of(directPairInfos) == ^^Sora::Meta::InfoList);

    template<typename Type>
    using NonListExpansion = Type;

} // namespace

TEST_CASE("TypeList core queries", "[Sora][Core][TypeTraits]") {
    STATIC_REQUIRE(Sora::Traits::Length<List> == 3);
    STATIC_REQUIRE(Sora::Traits::Length<Empty> == 0);
    STATIC_REQUIRE(std::same_as<Sora::Traits::At<List, 1>, double>);
    using OutOfRange = Sora::Traits::At<List, 99>;
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<OutOfRange>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^OutOfRange) == ^^Sora::Meta::Error::OutOfRange);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Front<List>, int>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Back<List>, char>);
    STATIC_REQUIRE(Sora::Traits::IndexOf<List, int> == 0);
    STATIC_REQUIRE(Sora::Traits::IndexOf<List, char> == 2);
    STATIC_REQUIRE(Sora::Traits::Contains<List, double>);
    STATIC_REQUIRE_FALSE(Sora::Traits::Contains<List, bool>);
}

TEST_CASE("TypeList structural transforms", "[Sora][Core][TypeTraits]") {
    STATIC_REQUIRE(std::same_as<Sora::Traits::Concat<List, Sora::Traits::TypeList<bool, short>>,
                                Sora::Traits::TypeList<int, double, char, bool, short>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::PushFront<List, bool>, Sora::Traits::TypeList<bool, int, double, char>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::PushBack<List, bool>, Sora::Traits::TypeList<int, double, char, bool>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Reverse<List>, Sora::Traits::TypeList<char, double, int>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::PopFront<List>, Sora::Traits::TypeList<double, char>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::PopBack<List>, Sora::Traits::TypeList<int, double>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Take<List, 2>, Sora::Traits::TypeList<int, double>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Take<List, 8>, List>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Drop<List, 2>, Sora::Traits::TypeList<char>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Drop<List, 8>, Empty>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Slice<List, 1, 2>, Sora::Traits::TypeList<double, char>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Clear<List>, Empty>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Fill<3, int>, Sora::Traits::TypeList<int, int, int>>);
    using Repeated = Sora::Traits::TypeList<bool, short, int, bool, double, char, short>;
    STATIC_REQUIRE(
        std::same_as<Sora::Traits::Unique<Repeated>, Sora::Traits::TypeList<bool, short, int, double, char>>);
    STATIC_REQUIRE(Sora::Traits::IsUnique<List>);
    STATIC_REQUIRE_FALSE(Sora::Traits::IsUnique<Repeated>);
}

TEST_CASE("InfoList is the reflection-sequence algebra", "[Sora][Core][MetaInfoList]") {
    constexpr Sora::Meta::InfoList input{^^int, ^^double, ^^int};
    constexpr Sora::Meta::InfoList suffix{^^char, ^^bool};
    constexpr auto joined = Sora::Meta::Concat(input, suffix);
    constexpr auto unique = Sora::Meta::Unique(joined);
    constexpr auto reversed = Sora::Meta::Reverse(unique);

    STATIC_REQUIRE(input.size() == 3);
    STATIC_REQUIRE(input.front() == ^^int);
    STATIC_REQUIRE(input.back() == ^^int);
    STATIC_REQUIRE(Sora::Meta::Contains(input, ^^double));
    STATIC_REQUIRE(Sora::Meta::IndexOf(input, ^^double) == 1);
    STATIC_REQUIRE(Sora::Meta::Count(input, ^^int) == 2);
    STATIC_REQUIRE(unique.size() == 4);
    STATIC_REQUIRE(reversed.front() == ^^bool);
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Slice(joined, 1, 2), Sora::Meta::InfoList{^^double, ^^int}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Take(joined, 2), Sora::Meta::InfoList{^^int, ^^double}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Drop(joined, 3), suffix));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::PopFront(input), Sora::Meta::InfoList{^^double, ^^int}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::PopBack(input), Sora::Meta::InfoList{^^int, ^^double}));
    STATIC_REQUIRE(
        Sora::Meta::Equal(Sora::Meta::Replace(input, ^^int, ^^char), Sora::Meta::InfoList{^^char, ^^double, ^^char}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Remove(input, ^^int), Sora::Meta::InfoList{^^double}));
    STATIC_REQUIRE(Sora::Meta::IndexOf(input, ^^bool) == Sora::Meta::InfoList::npos);
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Take(input, 0), {}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Drop(input, input.size()), {}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Take(input, 100), input));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Drop(input, 100), {}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Slice(input, input.size(), 0), {}));
    STATIC_REQUIRE_FALSE(Sora::Meta::Equal(Sora::Meta::InfoList{^^int, ^^double},
                                           Sora::Meta::Reverse(Sora::Meta::InfoList{^^int, ^^double})));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Replace({}, ^^int, ^^char), {}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::Remove({}, ^^int), {}));
    STATIC_REQUIRE(Sora::Meta::Equal({}, {}));
    STATIC_REQUIRE_FALSE(Sora::Meta::IsUnique(input));
    STATIC_REQUIRE(Sora::Meta::Concat(Sora::Meta::InfoList{}, Sora::Meta::InfoList{}).empty());
    STATIC_REQUIRE(Sora::Meta::Unique({}).empty());
    STATIC_REQUIRE(Sora::Meta::Reverse({}).empty());
    STATIC_REQUIRE(Sora::Meta::PopFront({}).empty());
    STATIC_REQUIRE(Sora::Meta::PopBack({}).empty());
}

TEST_CASE("InfoList set and higher-order operations", "[Sora][Core][MetaInfoList][MetaOperators]") {
    constexpr Sora::Meta::InfoList left{^^int, ^^double, ^^int, ^^char};
    constexpr Sora::Meta::InfoList right{^^char, ^^bool, ^^double, ^^char};

    STATIC_REQUIRE(
        Sora::Meta::Equal(Sora::Meta::SetUnion(left, right), Sora::Meta::InfoList{^^int, ^^double, ^^char, ^^bool}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::SetIntersection(left, right), Sora::Meta::InfoList{^^double, ^^char}));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::SetDifference(left, right), Sora::Meta::InfoList{^^int}));
    STATIC_REQUIRE(Sora::Meta::SetUnion({}, {}).empty());
    STATIC_REQUIRE(Sora::Meta::SetIntersection(left, {}).empty());
    STATIC_REQUIRE(Sora::Meta::SetDifference({}, right).empty());

    constexpr auto flat = Sora::Meta::FlatMap(^^PairInfos, Sora::Meta::InfoList{^^int, ^^double});
    STATIC_REQUIRE(Sora::Meta::IsInfoList(flat));
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::ExtractInfoList(flat),
                                     Sora::Meta::InfoList{^^int, ^^const int, ^^double, ^^const double}));
    constexpr auto zeroFlat = Sora::Meta::FlatMap(^^EmptyInfos, Sora::Meta::InfoList{^^int, ^^double});
    STATIC_REQUIRE(Sora::Meta::IsInfoList(zeroFlat));
    STATIC_REQUIRE(Sora::Meta::ExtractInfoList(zeroFlat).empty());
    constexpr auto invalidFlat = Sora::Meta::FlatMap(^^NonListExpansion, Sora::Meta::InfoList{^^int});
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFlat) == ^^Sora::Meta::Error::NonInvocable);
    constexpr auto wrongCodomain = Sora::Meta::FlatMap(^^PairWithConst, Sora::Meta::InfoList{^^int});
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(wrongCodomain) == ^^Sora::Meta::Error::NonInvocable);

    constexpr auto replaced =
        Sora::Meta::ReplaceIf(^^IsIntegral, ^^bool, Sora::Meta::InfoList{^^int, ^^double, ^^char});
    STATIC_REQUIRE(
        Sora::Meta::Equal(Sora::Meta::ExtractInfoList(replaced), Sora::Meta::InfoList{^^bool, ^^double, ^^bool}));
    constexpr auto removed = Sora::Meta::RemoveIf(^^IsIntegral, Sora::Meta::InfoList{^^int, ^^double, ^^char});
    STATIC_REQUIRE(Sora::Meta::Equal(Sora::Meta::ExtractInfoList(removed), Sora::Meta::InfoList{^^double}));
}

TEST_CASE("Reflection-native meta invocation", "[Sora][Core][MetaInvocable]") {
    using Pair = Sora::Meta::InvokeResult<^^std::pair, int, double>;
    STATIC_REQUIRE(std::same_as<Pair, std::pair<int, double>>);

    constexpr auto invalid = Sora::Meta::Invoke<^^std::pair, ^^int>;
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalid));
    STATIC_REQUIRE(Sora::Meta::ErrorContextCount(invalid) == 2);

    constexpr auto invalidList = Sora::Meta::Types::Apply(^^std::pair, ^^Sora::Traits::TypeList<int>);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalidList));

    using Input = Sora::Traits::TypeList<int, double>;
    using Tuple = Sora::Traits::Apply<^^std::tuple, Input>;
    STATIC_REQUIRE(std::same_as<Tuple, std::tuple<int, double>>);
}

TEST_CASE("Reflected TypeList adapters cover structural operations", "[Sora][Core][MetaOperators]") {
    constexpr auto list = ^^Sora::Traits::TypeList<int, double, char, int>;

    STATIC_REQUIRE(std::meta::extract<std::size_t>(Sora::Meta::Types::Length(list)) == 4);
    STATIC_REQUIRE(Sora::Meta::Types::At(list, 1) == ^^double);
    STATIC_REQUIRE(Sora::Meta::Types::Front(list) == ^^int);
    STATIC_REQUIRE(Sora::Meta::Types::Back(list) == ^^int);
    STATIC_REQUIRE(std::meta::extract<bool>(
        Sora::Meta::Types::Equal(Sora::Meta::Types::Reverse(list), ^^Sora::Traits::TypeList<int, char, double, int>)));
    STATIC_REQUIRE(std::meta::extract<bool>(
        Sora::Meta::Types::Equal(Sora::Meta::Types::Unique(list), ^^Sora::Traits::TypeList<int, double, char>)));
    STATIC_REQUIRE(std::meta::extract<bool>(
        Sora::Meta::Types::Equal(Sora::Meta::Types::Take(list, 2), ^^Sora::Traits::TypeList<int, double>)));
    STATIC_REQUIRE(std::meta::extract<bool>(
        Sora::Meta::Types::Equal(Sora::Meta::Types::Drop(list, 2), ^^Sora::Traits::TypeList<char, int>)));
    STATIC_REQUIRE(std::meta::extract<bool>(
        Sora::Meta::Types::Equal(Sora::Meta::Types::Slice(list, 1, 2), ^^Sora::Traits::TypeList<double, char>)));

    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::Types::Equal(
        Sora::Meta::Types::PushFront(list, ^^bool), ^^Sora::Traits::TypeList<bool, int, double, char, int>)));
    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::Types::Equal(
        Sora::Meta::Types::PushBack(list, ^^bool), ^^Sora::Traits::TypeList<int, double, char, int, bool>)));
    STATIC_REQUIRE(std::meta::extract<bool>(
        Sora::Meta::Types::Equal(Sora::Meta::Types::PopFront(list), ^^Sora::Traits::TypeList<double, char, int>)));
    STATIC_REQUIRE(std::meta::extract<bool>(
        Sora::Meta::Types::Equal(Sora::Meta::Types::PopBack(list), ^^Sora::Traits::TypeList<int, double, char>)));
    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::Types::Contains(list, ^^char)));
    STATIC_REQUIRE_FALSE(std::meta::extract<bool>(Sora::Meta::Types::Contains(list, ^^bool)));
    STATIC_REQUIRE(std::meta::extract<std::size_t>(Sora::Meta::Types::IndexOf(list, ^^char)) == 2);
    STATIC_REQUIRE(std::meta::extract<std::size_t>(Sora::Meta::Types::IndexOf(list, ^^bool)) ==
                   Sora::Traits::TypeList<int, double, char, int>::npos);
    STATIC_REQUIRE(std::meta::extract<std::size_t>(Sora::Meta::Types::Count(list, ^^int)) == 2);
    STATIC_REQUIRE_FALSE(std::meta::extract<bool>(Sora::Meta::Types::IsUnique(list)));
    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::Types::IsUnique(^^Sora::Traits::TypeList<int, double>)));

    constexpr auto invalid = Sora::Meta::Types::At(list, 4);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalid) == ^^Sora::Meta::Error::OutOfRange);
    constexpr auto malformed = Sora::Meta::Types::Slice(list, 3, 2);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(malformed) == ^^Sora::Meta::Error::OutOfRange);

    struct Category {};
    constexpr auto error = Sora::Meta::MakeMetaError(^^Category, {^^int});
    using ErrorType = Sora::Meta::InfoType<error>;
    constexpr auto nested = ^^Sora::Traits::TypeList<int, ErrorType, double>;
    STATIC_REQUIRE(Sora::Meta::Types::Length(nested) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Reverse(nested) == error);
    STATIC_REQUIRE(Sora::Meta::Types::PushBack(nested, ^^bool) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Contains(nested, ^^int) == error);
    STATIC_REQUIRE(Sora::Meta::Types::IsUnique(nested) == error);
}

TEST_CASE("Reflection-native meta errors preserve provenance", "[Sora][Core][MetaError]") {
    struct Category {};
    constexpr auto error = Sora::Meta::MakeMetaError(^^Category, {^^int, ^^double});
    STATIC_REQUIRE(Sora::Meta::IsMetaError(error));
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(error) == ^^Category);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(error) == ^^Category);
    STATIC_REQUIRE(Sora::Meta::HasErrorCategory<^^Category>(error));
    STATIC_REQUIRE(Sora::Meta::ErrorContextCount(error) == 2);
    STATIC_REQUIRE(Sora::Meta::ErrorContexts(error)[0] == ^^int);
    STATIC_REQUIRE(Sora::Meta::ErrorContexts(error)[1] == ^^double);
    constexpr auto extended = Sora::Meta::AppendContext(error, {^^char});
    STATIC_REQUIRE(Sora::Meta::ErrorContextCount(extended) == 3);
    STATIC_REQUIRE(Sora::Meta::ErrorContexts(extended)[2] == ^^char);
    constexpr auto description = Sora::Meta::ErrorDescription(error);
    STATIC_REQUIRE(!description.empty());
    using ErrorType = Sora::Meta::InfoType<error>;
    constexpr auto typeDescription = Sora::Meta::ErrorDescriptionOf<ErrorType>();
    STATIC_REQUIRE(typeDescription == description);
}

TEST_CASE("Scalar concepts canonicalize cvref and enum representations", "[Sora][Core][TypeTraits]") {
    STATIC_REQUIRE(Sora::Concept::ArithmeticScalar<const int&>);
    STATIC_REQUIRE(Sora::Concept::Vectorizable<const double&>);
    STATIC_REQUIRE(Sora::Concept::FloatingLike<const float&>);
    STATIC_REQUIRE(Sora::Concept::SignedArithmetic<const int&>);
    STATIC_REQUIRE(Sora::Concept::UnsignedArithmetic<const unsigned&>);
    STATIC_REQUIRE(Sora::Concept::SignedIntegerLike<SignedEnum>);
    STATIC_REQUIRE(Sora::Concept::UnsignedIntegerLike<UnsignedEnum>);
}

TEST_CASE("Reflection-native predicates compose with TypeList", "[Sora][Core][MetaPred]") {
    using Input = Sora::Traits::TypeList<int, double, long>;
    using Integral = Sora::Traits::Filter<^^IsIntegral, Input>;

    STATIC_REQUIRE(std::same_as<Integral, Sora::Traits::TypeList<int, long>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::FindFirst<^^IsClass, Input>, void>);
    STATIC_REQUIRE_FALSE(Sora::Traits::AllOf<^^IsIntegral, Input>);
    STATIC_REQUIRE_FALSE(Sora::Traits::AnyOf<^^IsClass, Input>);
    STATIC_REQUIRE(Sora::Traits::NoneOf<^^IsClass, Input>);
    STATIC_REQUIRE(Sora::Traits::CountIf<^^IsIntegral, Input> == 2);

    constexpr auto invalid = Sora::Meta::Types::Filter(^^NotBoolean, ^^Input);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalid));
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalid) == ^^Sora::Meta::Error::NonBooleanPredResult);
}

TEST_CASE("Reflected TypeList adapters cover folds, search, and predicates", "[Sora][Core][MetaOperators]") {
    constexpr auto list = ^^Sora::Traits::TypeList<int, double, char>;
    constexpr auto empty = ^^Sora::Traits::TypeList<>;
    using LeftFold = Pair<Pair<Pair<bool, int>, double>, char>;
    using RightFold = Pair<int, Pair<double, Pair<char, bool>>>;

    STATIC_REQUIRE(Sora::Meta::Types::Clear(list) == empty);
    using FoldLeftResult = Sora::Meta::InfoType<Sora::Meta::Types::FoldLeft(^^Pair, ^^bool, list)>;
    using FoldRightResult = Sora::Meta::InfoType<Sora::Meta::Types::FoldRight(^^Pair, ^^bool, list)>;
    STATIC_REQUIRE(std::same_as<FoldLeftResult, LeftFold>);
    STATIC_REQUIRE(std::same_as<FoldRightResult, RightFold>);
    STATIC_REQUIRE(Sora::Meta::Types::FindFirst(^^IsIntegral, list) == ^^int);
    STATIC_REQUIRE(Sora::Meta::Types::FindLast(^^IsIntegral, list) == ^^char);
    STATIC_REQUIRE(Sora::Meta::Types::FindFirst(^^IsClass, list, ^^bool) == ^^bool);
    STATIC_REQUIRE(Sora::Meta::Types::FindLast(^^IsClass, list, ^^bool) == ^^bool);
    STATIC_REQUIRE_FALSE(std::meta::extract<bool>(Sora::Meta::Types::AllOf(^^IsIntegral, list)));
    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::Types::AnyOf(^^IsIntegral, list)));
    STATIC_REQUIRE_FALSE(std::meta::extract<bool>(Sora::Meta::Types::NoneOf(^^IsIntegral, list)));
    STATIC_REQUIRE(std::meta::extract<std::size_t>(Sora::Meta::Types::CountIf(^^IsIntegral, list)) == 2);

    STATIC_REQUIRE(Sora::Meta::Types::FoldLeft(^^Pair, ^^bool, empty) == ^^bool);
    STATIC_REQUIRE(Sora::Meta::Types::FoldRight(^^Pair, ^^bool, empty) == ^^bool);
    STATIC_REQUIRE(Sora::Meta::Types::FindFirst(^^IsIntegral, empty) == ^^void);
    STATIC_REQUIRE(Sora::Meta::Types::FindLast(^^IsIntegral, empty) == ^^void);
    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::Types::AllOf(^^IsIntegral, empty)));
    STATIC_REQUIRE_FALSE(std::meta::extract<bool>(Sora::Meta::Types::AnyOf(^^IsIntegral, empty)));
    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::Types::NoneOf(^^IsIntegral, empty)));
    STATIC_REQUIRE(std::meta::extract<std::size_t>(Sora::Meta::Types::CountIf(^^IsIntegral, empty)) == 0);

    struct Category {};
    constexpr auto error = Sora::Meta::MakeMetaError(^^Category, {^^int});
    using ErrorType = Sora::Meta::InfoType<error>;
    constexpr auto nested = ^^Sora::Traits::TypeList<int, ErrorType>;
    STATIC_REQUIRE(Sora::Meta::Types::Clear(nested) == error);
    STATIC_REQUIRE(Sora::Meta::Types::FoldLeft(^^Pair, ^^bool, nested) == error);
    STATIC_REQUIRE(Sora::Meta::Types::FindFirst(^^IsIntegral, nested) == error);
    STATIC_REQUIRE(Sora::Meta::Types::AllOf(^^IsIntegral, nested) == error);
    STATIC_REQUIRE(Sora::Meta::Types::CountIf(^^IsIntegral, nested) == error);

    constexpr auto value = std::meta::reflect_constant(42);
    constexpr auto invalidFold = Sora::Meta::Types::FoldLeft(^^PairSize, ^^int, ^^Sora::Traits::TypeList<double>);
    constexpr auto invalidFallback = Sora::Meta::Types::FindFirst(^^IsIntegral, empty, value);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFold) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFallback) == ^^Sora::Meta::Error::NonTypeResult);
}

TEST_CASE("MetaOperators share one InfoList kernel", "[Sora][Core][MetaOperators]") {
    using Input = Sora::Traits::TypeList<const int&, double, int>;
    using Decayed = Sora::Traits::Map<^^std::decay_t, Input>;
    using Integral = Sora::Traits::Filter<^^IsIntegral, Decayed>;
    using Unique = Sora::Traits::Unique<Integral>;
    using Variant = Sora::Traits::Apply<^^std::variant, Unique>;
    using Zipped =
        Sora::Traits::Zip<^^std::pair, Sora::Traits::TypeList<int, float>, Sora::Traits::TypeList<double, long>>;

    STATIC_REQUIRE(std::same_as<Decayed, Sora::Traits::TypeList<int, double, int>>);
    STATIC_REQUIRE(std::same_as<Unique, Sora::Traits::TypeList<int>>);
    STATIC_REQUIRE(std::same_as<Variant, std::variant<int>>);
    STATIC_REQUIRE(std::same_as<Zipped, Sora::Traits::TypeList<std::pair<int, double>, std::pair<float, long>>>);

    constexpr auto mismatch =
        Sora::Meta::Types::Zip(^^std::pair, ^^Sora::Traits::TypeList<int>, ^^Sora::Traits::TypeList<double, long>);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(mismatch));
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(mismatch) == ^^Sora::Meta::Error::LengthMismatch);

    constexpr auto mapped = Sora::Meta::Map(^^std::add_pointer_t, Sora::Meta::InfoList{^^int, ^^double});
    constexpr auto bracedMapped = Sora::Meta::Map(^^std::add_pointer_t, {^^int});
    STATIC_REQUIRE(Sora::Meta::IsInfoList(mapped));
    STATIC_REQUIRE(Sora::Meta::ExtractInfoList(mapped)[1] == ^^double*);
    STATIC_REQUIRE(Sora::Meta::ExtractInfoList(bracedMapped).front() == ^^int*);

    constexpr auto invalidMap = Sora::Meta::Map(^^std::pair, Sora::Meta::InfoList{^^int, ^^double});
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalidMap));
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidMap) == ^^Sora::Meta::Error::NonInvocable);
}

TEST_CASE("MetaOperators compose reflected functions and folds", "[Sora][Core][MetaOperators]") {
    constexpr auto bindFront = Sora::Meta::BindFront<^^std::pair, ^^int>;
    constexpr auto bindBack = Sora::Meta::BindBack<^^std::pair, ^^double>;
    constexpr auto compose = Sora::Meta::Compose<^^std::add_pointer_t, ^^std::add_const_t>;
    constexpr auto transform = Sora::Meta::Transform<^^std::decay_t>;
    constexpr auto valueTransform = Sora::Meta::Transform<^^TypeSize>;
    constexpr auto uncurry = Sora::Meta::Uncurry<^^std::pair>;
    constexpr auto constant = Sora::Meta::Constant<^^void>;
    constexpr auto compose3 = Sora::Meta::Compose<^^std::add_pointer_t, ^^std::add_const_t, ^^std::decay_t>;
    constexpr auto valueCompose = Sora::Meta::Compose<^^std::add_pointer_t, ^^TypeSize>;

    STATIC_REQUIRE(std::same_as<Sora::Meta::InvokeResult<bindFront, double>, std::pair<int, double>>);
    STATIC_REQUIRE(std::same_as<Sora::Meta::InvokeResult<bindBack, int>, std::pair<int, double>>);
    STATIC_REQUIRE(std::same_as<Sora::Meta::InvokeResult<compose, int>, const int*>);
    STATIC_REQUIRE(
        std::same_as<Sora::Meta::InvokeResult<transform, const int&, double[]>, Sora::Traits::TypeList<int, double*>>);
    STATIC_REQUIRE(
        std::same_as<Sora::Meta::InvokeResult<uncurry, Sora::Traits::TypeList<int, double>>, std::pair<int, double>>);
    STATIC_REQUIRE(std::same_as<Sora::Meta::InvokeResult<constant, int, double>, void>);
    STATIC_REQUIRE(std::same_as<Sora::Meta::InvokeResult<compose3, const int&>, const int*>);
    using ValueTransformResult = Sora::Meta::InvokeResult<valueTransform, int>;
    using ValueComposeResult = Sora::Meta::InvokeResult<valueCompose, int>;
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<ValueTransformResult>);
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<ValueComposeResult>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^ValueTransformResult) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^ValueComposeResult) == ^^Sora::Meta::Error::NonTypeResult);

    using FoldInput = Sora::Traits::TypeList<double, char>;
    STATIC_REQUIRE(std::same_as<Sora::Traits::FoldLeft<^^Pair, int, FoldInput>, Pair<Pair<int, double>, char>>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::FoldRight<^^Pair, int, FoldInput>, Pair<double, Pair<char, int>>>);

    STATIC_REQUIRE(std::meta::extract<bool>(Sora::Meta::AllOf(^^IsIntegral, {})));
    STATIC_REQUIRE_FALSE(std::meta::extract<bool>(Sora::Meta::AnyOf(^^IsIntegral, {})));
    STATIC_REQUIRE(std::meta::extract<std::size_t>(Sora::Meta::CountIf(^^IsIntegral, {})) == 0);
}

TEST_CASE("MetaOperators preserve errors and reject invalid projections", "[Sora][Core][MetaOperators]") {
    struct Category {};
    constexpr auto error = Sora::Meta::MakeMetaError(^^Category, {^^int});
    constexpr auto reflectedErrors = Sora::Meta::Reflect(Sora::Meta::InfoList{error});
    using ErrorType = Sora::Meta::InfoType<error>;
    using NestedErrorList = Sora::Traits::TypeList<int, ErrorType>;

    STATIC_REQUIRE(Sora::Meta::ToTypeList(error) == error);
    STATIC_REQUIRE(Sora::Meta::ToTypeList(reflectedErrors) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Concat(Sora::Meta::InfoList{error}) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Apply(^^std::tuple, error) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Map(^^std::decay_t, error) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Filter(^^IsIntegral, error) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Zip(^^std::pair, error, ^^List) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Concat(Sora::Meta::InfoList{^^NestedErrorList}) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Replace(^^NestedErrorList, ^^int, ^^long) == error);
    STATIC_REQUIRE(Sora::Meta::Types::Remove(^^NestedErrorList, ^^int) == error);

    STATIC_REQUIRE(Sora::Meta::Types::SetUnion(Sora::Meta::InfoList{^^NestedErrorList}) == error);
    STATIC_REQUIRE(Sora::Meta::Types::FlatMap(^^PairWithConst, ^^NestedErrorList) == error);
    STATIC_REQUIRE(Sora::Meta::Types::ReplaceIf(^^IsIntegral, ^^bool, ^^NestedErrorList) == error);
    STATIC_REQUIRE(Sora::Meta::Types::RemoveIf(^^IsIntegral, ^^NestedErrorList) == error);

    constexpr auto invalidProject = Sora::Meta::ToTypeList(^^int);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidProject) == ^^Sora::Meta::Error::NonInfoListResult);

    constexpr auto invalidLength = Sora::Meta::Types::Length(^^int);
    constexpr auto invalidPush = Sora::Meta::Types::PushBack(^^int, ^^bool);
    constexpr auto invalidMapInput = Sora::Meta::Types::Map(^^std::decay_t, ^^int);
    constexpr auto invalidEqualInput = Sora::Meta::Types::Equal(^^int, ^^List);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidLength) == ^^Sora::Meta::Error::InvalidSequence);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidPush) == ^^Sora::Meta::Error::InvalidSequence);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidMapInput) == ^^Sora::Meta::Error::InvalidSequence);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidEqualInput) == ^^Sora::Meta::Error::InvalidSequence);

    constexpr auto value = std::meta::reflect_constant(42);
    constexpr auto invalidFill = Sora::Meta::Types::Fill(2, value);
    constexpr auto invalidReplace = Sora::Meta::Types::Replace(^^List, ^^int, value);
    constexpr auto invalidReplaceIf = Sora::Meta::Types::ReplaceIf(^^IsIntegral, value, ^^List);
    constexpr auto invalidPushFront = Sora::Meta::Types::PushFront(^^List, value);
    constexpr auto invalidPushBack = Sora::Meta::Types::PushBack(^^List, value);
    constexpr auto invalidContains = Sora::Meta::Types::Contains(^^List, value);
    constexpr auto invalidIndex = Sora::Meta::Types::IndexOf(^^List, value);
    constexpr auto invalidCount = Sora::Meta::Types::Count(^^List, value);
    constexpr auto invalidRemove = Sora::Meta::Types::Remove(^^List, value);
    constexpr auto valueMap = Sora::Meta::Types::Map(^^TypeSize, ^^List);
    constexpr auto valueApply = Sora::Meta::Types::Apply(^^TypeSize, ^^Sora::Traits::TypeList<int>);
    constexpr auto valueZip = Sora::Meta::Types::Zip(^^PairSize, ^^Sora::Traits::TypeList<int>,
                                                     ^^Sora::Traits::TypeList<double>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFill) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidReplace) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidReplaceIf) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidPushFront) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidPushBack) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidContains) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidIndex) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidCount) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidRemove) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(valueMap) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(valueApply) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(valueZip) == ^^Sora::Meta::Error::NonTypeResult);
    using ValueApplyType = Sora::Traits::Apply<^^TypeSize, Sora::Traits::TypeList<int>>;
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<ValueApplyType>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^ValueApplyType) == ^^Sora::Meta::Error::NonTypeResult);

    using ValueFold = Sora::Traits::FoldLeft<^^PairSize, int, Sora::Traits::TypeList<double>>;
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<ValueFold>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^ValueFold) == ^^Sora::Meta::Error::NonTypeResult);

    using Union = Sora::Traits::SetUnion<Sora::Traits::TypeList<int, double>, Sora::Traits::TypeList<double, char>>;
    using Intersection =
        Sora::Traits::SetIntersection<Sora::Traits::TypeList<int, double>, Sora::Traits::TypeList<double, char>>;
    using Difference =
        Sora::Traits::SetDifference<Sora::Traits::TypeList<int, double>, Sora::Traits::TypeList<double, char>>;
    using Expanded = Sora::Traits::FlatMap<^^PairWithConst, Sora::Traits::TypeList<int, double>>;
    using Replaced = Sora::Traits::ReplaceIf<^^IsIntegral, bool, Sora::Traits::TypeList<int, double>>;
    using Removed = Sora::Traits::RemoveIf<^^IsIntegral, Sora::Traits::TypeList<int, double>>;
    STATIC_REQUIRE(std::same_as<Union, Sora::Traits::TypeList<int, double, char>>);
    STATIC_REQUIRE(std::same_as<Intersection, Sora::Traits::TypeList<double>>);
    STATIC_REQUIRE(std::same_as<Difference, Sora::Traits::TypeList<int>>);
    STATIC_REQUIRE(std::same_as<Expanded, Sora::Traits::TypeList<int, const int, double, const double>>);
    STATIC_REQUIRE(std::same_as<Replaced, Sora::Traits::TypeList<bool, double>>);
    STATIC_REQUIRE(std::same_as<Removed, Sora::Traits::TypeList<double>>);
    STATIC_REQUIRE(Sora::Traits::Equal<Sora::Traits::TypeList<int, double>, Sora::Traits::TypeList<int, double>>);
    STATIC_REQUIRE_FALSE(Sora::Traits::Equal<Sora::Traits::TypeList<int, double>, Sora::Traits::TypeList<double, int>>);

    constexpr auto invalidEmptyMap = Sora::Meta::Map(^^int, Sora::Meta::InfoList{});
    constexpr auto emptyMap = Sora::Meta::Map(^^std::pair, Sora::Meta::InfoList{});
    constexpr auto invalidEmptyFilter = Sora::Meta::Filter(^^NotBoolean, Sora::Meta::InfoList{});
    constexpr auto emptyFold = Sora::Meta::FoldLeft(^^std::decay_t, ^^void, {});
    constexpr auto emptyZip = Sora::Meta::Zip(^^std::decay_t, {}, {});
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidEmptyMap) == ^^Sora::Meta::Error::NonInvocable);
    STATIC_REQUIRE(Sora::Meta::ExtractInfoList(emptyMap).empty());
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidEmptyFilter) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(emptyFold == ^^void);
    STATIC_REQUIRE(Sora::Meta::ExtractInfoList(emptyZip).empty());

    constexpr auto empty = ^^Sora::Traits::TypeList<>;
    STATIC_REQUIRE(Sora::Meta::FoldRight(^^std::decay_t, ^^void, {}) == ^^void);
    STATIC_REQUIRE(Sora::Meta::Types::FoldLeft(^^std::decay_t, ^^bool, empty) == ^^bool);
    STATIC_REQUIRE(Sora::Meta::Types::FoldRight(^^std::decay_t, ^^bool, empty) == ^^bool);
    constexpr auto emptyValueFold = Sora::Meta::Types::FoldLeft(^^int, std::meta::reflect_constant(42), empty);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(emptyValueFold) == ^^Sora::Meta::Error::NonTypeResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::FindFirst(^^NotBoolean, {})) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::AllOf(^^NotBoolean, {})) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::AnyOf(^^NotBoolean, {})) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::NoneOf(^^NotBoolean, {})) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::CountIf(^^NotBoolean, {})) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::Types::Filter(^^NotBoolean, empty)) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::Types::FindFirst(^^NotBoolean, empty)) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(Sora::Meta::Types::AllOf(^^NotBoolean, empty)) ==
                   ^^Sora::Meta::Error::NonBooleanPredResult);

    constexpr auto constrainedMap = Sora::Meta::Map(^^IntegralIdentity, Sora::Meta::InfoList{^^int, ^^long});
    constexpr auto reflectedThree = std::meta::reflect_constant(3);
    constexpr auto constantMap = Sora::Meta::Map(^^ConstantType, Sora::Meta::InfoList{reflectedThree});
    STATIC_REQUIRE(Sora::Meta::ExtractInfoList(constrainedMap)[1] == ^^long);
    STATIC_REQUIRE(Sora::Meta::ExtractInfoList(constantMap).front() == ^^std::integral_constant<int, 3>);

    constexpr auto hiddenAllError = Sora::Meta::AllOf(^^IsIntegral, Sora::Meta::InfoList{^^double, error});
    constexpr auto hiddenAnyError = Sora::Meta::AnyOf(^^IsIntegral, Sora::Meta::InfoList{^^int, error});
    constexpr auto invalidWithInputError = Sora::Meta::Map(^^int, Sora::Meta::InfoList{error});
    STATIC_REQUIRE(hiddenAllError == error);
    STATIC_REQUIRE(hiddenAnyError == error);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidWithInputError) == ^^Sora::Meta::Error::NonInvocable);

    constexpr auto boundValueTemplate = Sora::Meta::BindFront<^^SameType, ^^int>;
    using BoundResult = Sora::Meta::InvokeResult<boundValueTemplate, int>;
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<BoundResult>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^BoundResult) == ^^Sora::Meta::Error::NonTypeResult);

    constexpr auto constantValue = Sora::Meta::Constant<value>;
    using ConstantResult = Sora::Meta::InvokeResult<constantValue>;
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<ConstantResult>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^ConstantResult) == ^^Sora::Meta::Error::NonTypeResult);

    constexpr auto invalidTransform = Sora::Meta::Transform<^^int>;
    using TransformResult = Sora::Meta::InvokeResult<invalidTransform>;
    STATIC_REQUIRE(Sora::Concept::MetaErrorType<TransformResult>);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(^^TransformResult) == ^^Sora::Meta::Error::NonInvocable);

    constexpr auto invalidDataMemberTypes = Sora::Meta::DataMemberTypes(^^int);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(invalidDataMemberTypes));
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidDataMemberTypes) == ^^Sora::Meta::Error::NonInvocable);
}

TEST_CASE("MetaOperators preserve first-error ordering", "[Sora][Core][MetaOperators][MetaError]") {
    struct FunctionError {};
    struct InitialError {};
    struct InputError {};
    struct FallbackError {};

    constexpr auto functionError = Sora::Meta::MakeMetaError(^^FunctionError, {^^int});
    constexpr auto initialError = Sora::Meta::MakeMetaError(^^InitialError, {^^double});
    constexpr auto inputError = Sora::Meta::MakeMetaError(^^InputError, {^^char});
    constexpr auto fallbackError = Sora::Meta::MakeMetaError(^^FallbackError, {^^bool});
    constexpr auto nestedError = Sora::Meta::MakeMetaError(^^InputError, {^^long});
    using NestedErrorInput = Sora::Traits::TypeList<int, Sora::Meta::InfoType<nestedError>>;

    STATIC_REQUIRE(Sora::Meta::Map(functionError, Sora::Meta::InfoList{inputError}) == functionError);
    STATIC_REQUIRE(Sora::Meta::FoldLeft(functionError, initialError, Sora::Meta::InfoList{inputError}) ==
                   functionError);
    STATIC_REQUIRE(Sora::Meta::FoldLeft(^^Pair, initialError, Sora::Meta::InfoList{inputError}) == initialError);
    STATIC_REQUIRE(Sora::Meta::FoldLeft(^^Pair, ^^void, Sora::Meta::InfoList{inputError}) == inputError);
    STATIC_REQUIRE(Sora::Meta::FoldRight(^^Pair, ^^void, Sora::Meta::InfoList{inputError}) == inputError);
    STATIC_REQUIRE(Sora::Meta::Zip(functionError, Sora::Meta::InfoList{inputError},
                                   Sora::Meta::InfoList{fallbackError}) == functionError);
    STATIC_REQUIRE(Sora::Meta::Zip(^^Pair, Sora::Meta::InfoList{inputError}, Sora::Meta::InfoList{fallbackError}) ==
                   inputError);
    STATIC_REQUIRE(Sora::Meta::FindFirst(^^IsIntegral, Sora::Meta::InfoList{inputError}, fallbackError) == inputError);
    STATIC_REQUIRE(Sora::Meta::FindFirst(functionError, Sora::Meta::InfoList{inputError}, fallbackError) ==
                   functionError);
    STATIC_REQUIRE(Sora::Meta::CountIf(^^IsIntegral, Sora::Meta::InfoList{inputError}) == inputError);

    // Nested sequence errors are observed in source order before later top-level errors.
    constexpr auto ordered = Sora::Meta::Types::Concat(Sora::Meta::InfoList{^^NestedErrorInput, fallbackError});
    STATIC_REQUIRE(ordered == nestedError);
    STATIC_REQUIRE(Sora::Meta::Types::SetUnion(Sora::Meta::InfoList{^^NestedErrorInput, fallbackError}) ==
                   nestedError);

    constexpr auto invalidPred = Sora::Meta::Types::Filter(^^NotBoolean, ^^NestedErrorInput);
    constexpr auto invalidFindPred = Sora::Meta::Types::FindFirst(^^NotBoolean, ^^NestedErrorInput, fallbackError);
    constexpr auto invalidLastPred = Sora::Meta::Types::FindLast(^^NotBoolean, ^^NestedErrorInput, fallbackError);
    constexpr auto invalidAllPred = Sora::Meta::Types::AllOf(^^NotBoolean, ^^NestedErrorInput);
    constexpr auto invalidAnyPred = Sora::Meta::Types::AnyOf(^^NotBoolean, ^^NestedErrorInput);
    constexpr auto invalidNonePred = Sora::Meta::Types::NoneOf(^^NotBoolean, ^^NestedErrorInput);
    constexpr auto invalidCountPred = Sora::Meta::Types::CountIf(^^NotBoolean, ^^NestedErrorInput);
    constexpr auto invalidFunction = Sora::Meta::Types::Map(^^int, ^^NestedErrorInput);
    constexpr auto invalidFoldFunction = Sora::Meta::Types::FoldLeft(^^int, ^^bool, ^^NestedErrorInput);
    constexpr auto invalidApplyOrder = Sora::Meta::Apply(^^int, Sora::Meta::InfoList{inputError});
    constexpr auto invalidFoldSequenceOrder = Sora::Meta::Types::FoldLeft(^^int, ^^bool, ^^int);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidPred) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFindPred) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidLastPred) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidAllPred) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidAnyPred) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidNonePred) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidCountPred) == ^^Sora::Meta::Error::NonBooleanPredResult);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFunction) == ^^Sora::Meta::Error::NonInvocable);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFoldFunction) == ^^Sora::Meta::Error::NonInvocable);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidApplyOrder) == ^^Sora::Meta::Error::NonInvocable);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidFoldSequenceOrder) == ^^Sora::Meta::Error::NonInvocable);
    STATIC_REQUIRE(Sora::Meta::Types::FoldLeft(functionError, initialError, ^^NestedErrorInput) == functionError);
    STATIC_REQUIRE(Sora::Meta::Types::FoldRight(functionError, initialError, ^^NestedErrorInput) == functionError);
    STATIC_REQUIRE(Sora::Meta::Types::Map(functionError, fallbackError) == functionError);
    STATIC_REQUIRE(Sora::Meta::Types::Filter(functionError, fallbackError) == functionError);

    constexpr auto invalidInvoke = Sora::Meta::Invoke<^^int, inputError>;
    constexpr auto invalidPredicate = Sora::Meta::InvokePred(^^NotBoolean, inputError);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidInvoke) == ^^Sora::Meta::Error::NonInvocable);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidPredicate) == ^^Sora::Meta::Error::NonBooleanPredResult);

    STATIC_REQUIRE(Sora::Meta::Types::PushFront(^^NestedErrorInput, fallbackError) == nestedError);
    STATIC_REQUIRE(Sora::Meta::Types::PushBack(^^NestedErrorInput, fallbackError) == nestedError);
    STATIC_REQUIRE(Sora::Meta::Types::Replace(^^NestedErrorInput, fallbackError, initialError) == nestedError);
    STATIC_REQUIRE(Sora::Meta::Types::Remove(^^NestedErrorInput, fallbackError) == nestedError);

    STATIC_REQUIRE(Sora::Meta::Types::SetIntersection(^^NestedErrorInput, fallbackError) == nestedError);
    STATIC_REQUIRE(Sora::Meta::Types::SetDifference(^^NestedErrorInput, fallbackError) == nestedError);
    STATIC_REQUIRE(Sora::Meta::Types::Equal(^^NestedErrorInput, fallbackError) == nestedError);
    STATIC_REQUIRE(Sora::Meta::Types::Zip(^^Pair, ^^NestedErrorInput, fallbackError) == nestedError);

    constexpr auto invalidLeft = Sora::Meta::Types::SetIntersection(^^int, fallbackError);
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(invalidLeft) == ^^Sora::Meta::Error::InvalidSequence);

    using NestedErrorType = Sora::Meta::InfoType<nestedError>;
    STATIC_REQUIRE(std::same_as<Sora::Traits::At<NestedErrorInput, 0>, NestedErrorType>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Front<NestedErrorInput>, NestedErrorType>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::Back<NestedErrorInput>, NestedErrorType>);

    using FirstFallback = Sora::Traits::FindFirst<^^IsClass, List, bool>;
    using LastFallback = Sora::Traits::FindLast<^^IsClass, List, bool>;
    STATIC_REQUIRE(std::same_as<FirstFallback, bool>);
    STATIC_REQUIRE(std::same_as<LastFallback, bool>);
}

TEST_CASE("MetaPred uniformly supports arbitrary arity", "[Sora][Core][MetaPred]") {
    STATIC_REQUIRE(Sora::Concept::MetaPred<^^Always>);
    STATIC_REQUIRE(Sora::Concept::MetaPred<^^IsIntegral>);
    STATIC_REQUIRE(Sora::Concept::MetaPred<^^Same>);
    STATIC_REQUIRE(Sora::Concept::MetaPred<^^AllDistinct>);
    STATIC_REQUIRE_FALSE(Sora::Concept::MetaPred<^^WrongArgument>);
    STATIC_REQUIRE_FALSE(Sora::Concept::MetaPred<^^NotBoolean2>);

    STATIC_REQUIRE(Sora::Traits::PredArity<^^Always> == 0);
    STATIC_REQUIRE(Sora::Traits::PredArity<^^IsIntegral> == 1);
    STATIC_REQUIRE(Sora::Traits::PredArity<^^Same> == 2);
    STATIC_REQUIRE(Sora::Traits::PredArity<^^AllDistinct> == 3);
    STATIC_REQUIRE_FALSE(HasPredArity<^^WrongArgument>);
    STATIC_REQUIRE_FALSE(HasPredArity<^^NotBoolean2>);

    STATIC_REQUIRE(Sora::Concept::PredFor<^^Always>);
    STATIC_REQUIRE(Sora::Concept::PredFor<^^Same, ^^int, ^^int>);
    STATIC_REQUIRE_FALSE(Sora::Concept::PredFor<^^Same, ^^int>);
    STATIC_REQUIRE(Sora::Meta::EvaluatePred(^^Always));
    STATIC_REQUIRE(Sora::Meta::EvaluatePred(^^Same, ^^int, ^^int));
    STATIC_REQUIRE_FALSE(Sora::Meta::EvaluatePred(^^Same, ^^int, ^^double));
    STATIC_REQUIRE(Sora::Meta::EvaluatePred(^^AllDistinct, ^^int, ^^double, ^^char));

    constexpr auto wrongArity = Sora::Meta::InvokePred(^^Same, ^^int);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(wrongArity));
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(wrongArity) == ^^Sora::Meta::Error::NonInvocable);

    constexpr auto nonBoolean = Sora::Meta::InvokePred(^^NotBoolean2, ^^int, ^^double);
    STATIC_REQUIRE(Sora::Meta::IsMetaError(nonBoolean));
    STATIC_REQUIRE(Sora::Meta::ErrorCategory(nonBoolean) == ^^Sora::Meta::Error::NonBooleanPredResult);
}
