/**
 * @file TypeTraits.h
 * @brief Reflection-based type traits, structural concepts, and utility types.
 *
 * Provides compile-time introspection helpers built on C++26 static reflection
 * (`<meta>`, P2996) and standard concepts. Every facility is `consteval` /
 * `inline constexpr`, so it folds to immediate values or pure type computation
 * with **zero** runtime cost. Key facilities, grouped by concern:
 *
 * @par Class member reflection
 * - `Members<T>` / `PublicMembers<T>` and their `*Count` — reflected NSDMs.
 * - `MemberType<T,I>` / `MemberName<T,I>` / `MemberNames<T>` — typed/named access.
 * - `MemberOffset<T,I>` / `MemberIndex<T>(name)` / `HasMemberNamed<T>(name)`.
 * - `MemberBytesTotal<T>` / `PaddingBytes<T>` / `Compact<T>` — layout queries.
 * - `Homogeneous<T>` — all non-static data members share the same type.
 *
 * @par Base-class reflection
 * - `Bases<T>` / `BasesCount<T>` / `BaseType<T,I>` — direct base introspection.
 * - `RootClass<T>` / `SingleInheritedClass<T>` / `UniqueIdentifier<T>`.
 *
 * @par Enum reflection
 * - `Enumerators<T>` / `EnumeratorsCount<T>` — reflected enumerators.
 * - `EnumUnderlying<E>` / `EnumValues<E>` / `EnumNames<E>` — value/name tables.
 * - `EnumName(value)` / `EnumCast<E>(name)` — runtime value<->name conversion.
 * - `SequentialEnum<T>` / `BitfieldEnum<E>` / `kBitfieldMask<E>` — categorisation.
 *
 * @par Structural & categorisation concepts
 * - `TupleLike<T>` / `VariantLike<T>` — structural duck-type detection.
 * - `Aggregate` / `StandardLayoutType` / `TriviallyCopyableType` / `EmptyType` /
 *   `PolymorphicType` / `UniquelyRepresented` / `Reflectable` / `ScopedEnum` …
 * - `SpecializationOf<T,Tmpl>` — generic class-template specialisation probe
 *   (reflection-based; @p Tmpl must be an all-type-parameter template).
 * - Standard-library categorisation: `StdOptional`, `StdVariant`,
 *   `ChronoDuration`, `ChronoTimePoint`, `FilesystemPath`, `ByteRange`,
 *   `StringViewConvertible`, `StringKeyedAssociative`.
 *
 * @par Type-level algebra
 * - `TypeList` plus `At` / `Head` / `Tail` / `Last` / `Concat` / `MapT` /
 *   `FilterT` / `FoldT` / `Reverse` / `Unique` / `PushFront` / `PushBack` /
 *   `PopBack` / `IndexOf` / `Contains` / `AllOf` / `AnyOf` / `NoneOf` / `CountIf`.
 *
 * @par Annotations (C++26 `[[=...]]`)
 * - `Anno::*` — annotation probing + member filtering/ordering driven by
 *   user-supplied `Ignore` / `Key` / `Order` annotation tag types.
 *
 * - `Overload` — lambda-overload-set builder for `std::visit`.
 *
 * @ingroup Core
 */
#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <meta>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Mashiro {

    namespace Traits {

        namespace Detail {

            /// @brief Compile-time human-readable name of @p T.
            template<typename T>
            [[nodiscard]] consteval std::string_view GetTypeName() {
                return std::meta::display_string_of(^^T);
            }

        }

        /// @brief string_view name of a type.
        template<typename T>
        inline constexpr auto TypeName = Detail::GetTypeName<T>();

        /// @brief display string of
        consteval auto DisplayStringOf(std::meta::info iMeta) {
            return std::meta::display_string_of(iMeta);
        }

        /// @brief identifier of
        consteval auto IdentifierOf(std::meta::info iMeta) {
            return std::meta::identifier_of(iMeta);
        }

        /// @brief Static reflection array of @p T's non-static data members.
        template<typename T>
        inline constexpr auto Members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));

        /// @brief Number of non-static data members in @p T.
        template<typename T>
        inline constexpr size_t MembersCount = Members<T>.size();

        /// @brief Static reflection array of @p T's public non-static data members.
        template<typename T>
        inline constexpr auto PublicMembers = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));

        /// @brief Number of public non-static data members in @p T.
        template<typename T>
        inline constexpr size_t PublicMembersCount = PublicMembers<T>.size();

        namespace Detail {

            /// @brief Total byte size of all data members (may differ from sizeof due to padding).
            template<typename T>
            consteval size_t MemberBytesTotal() {
                size_t total = 0;
                for (auto m : Mashiro::Traits::Members<T>) {
                    total += std::meta::size_of(std::meta::type_of(m));
                }
                return total;
            }

        }

        /// @brief Total byte size of all data members (may differ from sizeof due to padding).
        template<typename T> requires std::is_class_v<T>
        inline constexpr size_t MemberBytesTotal = Detail::MemberBytesTotal<T>();
    
        /// @brief Padding bytes = sizeof(T) - sum of member sizes.
        template<typename T> requires std::is_class_v<T>
        inline constexpr size_t PaddingBytes = sizeof(T) - MemberBytesTotal<T>;

        /// @brief Compact class: all members are homogeneous and there's no padding.
        template<typename T>
        concept Compact = std::is_class_v<T> && MembersCount<T> > 0 && PaddingBytes<T> == 0;

        // clang-format off

        namespace Detail {

            /// @brief Compile-time check: every NSDM of @p T has the same type.
            template<typename T>
            consteval bool IsAllMemberHomogeneous() {
                if (MembersCount<T> == 0) {
                    return false;
                }
                auto first_type = type_of(Members<T>[0]);
                for (auto m : Members<T>) {
                    if (type_of(m) != first_type) {
                        return false;
                    }
                }
                return true;
            }

        } // namespace Detail

        /**
        * @brief Concept: all non-static data members of @p T share the same type.
        *
        * Requires @p T to be a class with at least one NSDM, and uses P2996
        * reflection to verify type homogeneity.
        */
        template<typename T>
        concept Homogeneous = std::is_class_v<T> && Detail::IsAllMemberHomogeneous<T>();

        // clang-format on

        // =====================================================================
        // Member reflection — typed/named access to non-static data members
        // =====================================================================

        /// @brief Sentinel returned by compile-time index lookups when nothing matches.
        inline constexpr size_t kNotFound = static_cast<size_t>(-1);

        /**
         * @brief The declared type of @p T's @p I-th non-static data member.
         *
         * Splices the reflection of the member's type, so `MemberType<T, I>` is a
         * first-class type usable anywhere a type-id is expected.
         *
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index; must be `< MembersCount<T>`.
         */
        template<typename T, size_t I>
        using MemberType = typename [:std::meta::type_of(Members<T>[I]):];

        /**
         * @brief The source identifier of @p T's @p I-th non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index; must be `< MembersCount<T>`.
         */
        template<typename T, size_t I> requires std::is_class_v<T>
        inline constexpr std::string_view MemberName =
            std::meta::identifier_of(Members<T>[I]);

        /** @cond INTERNAL */
        namespace Detail {

            /// @brief Build the `std::array` of every member identifier of @p T.
            template<typename T>
            consteval auto MemberNamesImpl() {
                return []<size_t... I>(std::index_sequence<I...>) {
                    return std::array<std::string_view, sizeof...(I)>{
                        std::meta::identifier_of(Mashiro::Traits::Members<T>[I])...};
                }(std::make_index_sequence<MembersCount<T>>{});
            }

        } // namespace Detail
        /** @endcond */

        /// @brief `std::array<std::string_view, MembersCount<T>>` of member identifiers.
        template<typename T> requires std::is_class_v<T>
        inline constexpr auto MemberNames = Detail::MemberNamesImpl<T>();

        /// @brief Byte offset of @p T's @p I-th non-static data member within @p T.
        template<typename T, size_t I> requires std::is_class_v<T>
        inline constexpr size_t MemberOffset =
            static_cast<size_t>(std::meta::offset_of(Members<T>[I]).bytes);

        /**
         * @brief Index of the first non-static data member of @p T named @p name.
         *
         * Anonymous members (no identifier) are skipped.
         *
         * @return Zero-based index, or @ref kNotFound if no member matches.
         */
        template<typename T> requires std::is_class_v<T>
        [[nodiscard]] consteval size_t MemberIndex(std::string_view name) {
            for (size_t i = 0; i < MembersCount<T>; ++i) {
                if (std::meta::has_identifier(Members<T>[i]) &&
                    std::meta::identifier_of(Members<T>[i]) == name) {
                    return i;
                }
            }
            return kNotFound;
        }

        /// @brief `true` if @p T declares a non-static data member named @p name.
        template<typename T> requires std::is_class_v<T>
        [[nodiscard]] consteval bool HasMemberNamed(std::string_view name) {
            return MemberIndex<T>(name) != kNotFound;
        }

        /// @name Tuple / variant structural concepts
        /// @{

        /**
         * @brief True if `std::get<N>(t)` is valid and returns the expected element type.
         * @tparam T Type to probe.
         * @tparam N Zero-based element index.
         */
        template<typename T, size_t N>
        concept HasTupleElement = requires(T&& t) {
            typename std::tuple_element_t<N, std::remove_cvref_t<T>>;
            { std::get<N>(t) } -> std::convertible_to<const std::tuple_element_t<N, T>&>;
        };

        /// @brief A type that provides `std::tuple_size` and per-index `std::get`.
        template<typename T>
        concept TupleLike = requires {
            typename std::tuple_size<std::remove_cvref_t<T>>::value_type;
        } && []<size_t... N>(std::index_sequence<N...>) {
            return (HasTupleElement<T, N> && ...);
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

        /**
         * @brief Detected `std::variant` specialisation.
         *
         * Uses `std::variant_size`, which is only specialised for `std::variant`.
         */
        template<typename T>
        concept VariantLike = requires { std::variant_size<std::remove_cvref_t<T>>::value; };

        /// @}

        /**
         * @brief Overload-set builder for `std::visit` and general dispatch.
         *
         * Inherits `operator()` from each callable in @p Ts, producing a single
         * callable object. The CTAD guide allows direct brace-list construction.
         *
         * @tparam Ts Callable types (typically lambdas).
         *
         * @code
         * std::variant<int, std::string, float> v = std::string{"hi"};
         * auto label = std::visit(Overload{
         *     [](int i)                { return "int:"   + std::to_string(i); },
         *     [](const std::string& s) { return "str:"   + s; },
         *     [](auto&&)               { return std::string{"other"}; },
         * }, v);
         * // label == "str:hi"
         * @endcode
         */
        template<typename... Ts>
        struct Overload : Ts... {
            using Ts::operator()...;
        };
        /// @brief CTAD guide — deduce @c Ts from constructor arguments.
        template<typename... Ts>
        Overload(Ts...) -> Overload<Ts...>;

        // ==========================================================================
        // Enum Reflection
        // ==========================================================================

        /// @brief Static reflection array of @p T's enumerators.
        template<typename T>
        inline constexpr auto Enumerators = std::define_static_array(std::meta::enumerators_of(^^T));

        /// @brief Number of enumerators in @p T.
        template<typename T>
        inline constexpr size_t EnumeratorsCount = Enumerators<T>.size();

        namespace Detail {

            /// @brief Unsigned version of an enum's underlying type.
            template<typename E>
                requires std::is_enum_v<E>
            using UnsignedUnderlying = std::make_unsigned_t<std::underlying_type_t<E>>;

            /// @brief Compile-time check: every enumerator is 0 or a power of 2.
            template <typename E>
            consteval bool AllPowerOfTwo() {
                template for (constexpr auto e : Enumerators<E>) {
                    auto v = static_cast<UnsignedUnderlying<E>>([:e:]);
                    if (v != 0 && !std::has_single_bit(v) && (std::meta::display_string_of(e) != "None"))
                        return false;
                }
                return true;
            }

            /// @brief Compile-time check: the enum values increases from 0 within the enum type
            template<typename T>
                requires std::is_enum_v<T>
            consteval bool IsEnumSequential() {
                if (EnumeratorsCount<T> == 0) {
                    return false;
                }
                using U = std::underlying_type_t<T>;
                for (size_t i = 0; i < EnumeratorsCount<T>; ++i) {
                    if (static_cast<U>(std::meta::extract<T>(Enumerators<T>[i])) !=
                        static_cast<U>(i)) {
                        return false;
                    }
                }
                return true;
            }

            /// @brief Compile-time OR of all enumerators — the valid-bit mask.
            template <typename E>
            consteval UnsignedUnderlying<E> AllBitsMask() {
                UnsignedUnderlying<E> mask{};
                template for (constexpr auto e : Enumerators<E>) {
                    mask |= static_cast<UnsignedUnderlying<E>>([:e:]);
                }
                return mask;
            }

        } // namespace Detail

        /**
         * @brief A scoped enum whose enumerators form a contiguous sequence starting from 0.
         *
         * Validated at compile time via P2996 static reflection. Useful for
         * enum-to-index mapping and array indexing by enum value.
         */
        template<typename T>
        concept SequentialEnum = std::is_enum_v<T> && Detail::IsEnumSequential<T>();

        /**
         * @brief A scoped enum whose enumerators are all 0 or exact powers of two.
         *
         * Validated at compile time via P2996 static reflection. Any `enum class`
         * satisfying this concept automatically gets bitwise operators and query
         * functions — zero opt-in required.
         */
        template <typename E>
        concept BitfieldEnum = std::is_enum_v<E> && Detail::AllPowerOfTwo<E>();
        
        /// @brief All-valid-bits mask for a BitfieldEnum (OR of all enumerators).
        template <BitfieldEnum E>
        inline constexpr auto kBitfieldMask = static_cast<E>(Detail::AllBitsMask<E>());

        /**
         * @brief Source identifier of an enumerator value, or empty if not found.
         *
         * Walks the enum's reflected enumerator list at compile time and
         * returns the matching enumerator's identifier. Used by serialisation
         * helpers (`ToJson`, `ToString`) and event-stream dispatchers
         * (`SystemEvent::EventKindName`) to render values as their source
         * names.
         *
         * @tparam E Scoped enum type.
         * @tparam V Enumerator value to look up.
         * @return The matching enumerator identifier, or `""` if @p V is not a
         *         declared enumerator (e.g. a synthetic bitmask combination).
         */
        template<typename E, E V> requires std::is_enum_v<E>
        consteval std::string_view EnumeratorName() {
            template for (constexpr auto en : Enumerators<E>) {
                if (std::meta::extract<E>(en) == V)
                    return std::meta::identifier_of(en);
            }
            return {};
        }

        /// @brief The fixed underlying integer type of enum @p E.
        template<typename E> requires std::is_enum_v<E>
        using EnumUnderlying = std::underlying_type_t<E>;

        /** @cond INTERNAL */
        namespace Detail {

            /// @brief Materialise every reflected enumerator value into a `std::array`.
            template<typename E>
            consteval auto EnumValuesImpl() {
                return []<size_t... I>(std::index_sequence<I...>) {
                    return std::array<E, sizeof...(I)>{
                        std::meta::extract<E>(Mashiro::Traits::Enumerators<E>[I])...};
                }(std::make_index_sequence<EnumeratorsCount<E>>{});
            }

            /// @brief Materialise every reflected enumerator identifier into a `std::array`.
            template<typename E>
            consteval auto EnumNamesImpl() {
                return []<size_t... I>(std::index_sequence<I...>) {
                    return std::array<std::string_view, sizeof...(I)>{
                        std::meta::identifier_of(Mashiro::Traits::Enumerators<E>[I])...};
                }(std::make_index_sequence<EnumeratorsCount<E>>{});
            }

        } // namespace Detail
        /** @endcond */

        /// @brief `std::array<E, EnumeratorsCount<E>>` of every declared enumerator value.
        template<typename E> requires std::is_enum_v<E>
        inline constexpr auto EnumValues = Detail::EnumValuesImpl<E>();

        /// @brief `std::array<std::string_view, EnumeratorsCount<E>>` of enumerator identifiers.
        template<typename E> requires std::is_enum_v<E>
        inline constexpr auto EnumNames = Detail::EnumNamesImpl<E>();

        /**
         * @brief Runtime value -> name conversion for a reflected enum.
         *
         * The lookup is a fully unrolled (`template for`) ladder of equality
         * comparisons against the reflected enumerators, so it carries no runtime
         * table and folds to a tight branch sequence. The first enumerator equal
         * to @p value wins (relevant for aliased enumerators).
         *
         * @return The matching enumerator identifier, or `""` if @p value is not a
         *         declared enumerator (e.g. a synthetic bitmask combination).
         */
        template<typename E> requires std::is_enum_v<E>
        [[nodiscard]] constexpr std::string_view EnumName(E value) noexcept {
            template for (constexpr auto e : Enumerators<E>) {
                if (std::meta::extract<E>(e) == value)
                    return std::meta::identifier_of(e);
            }
            return {};
        }

        /**
         * @brief Runtime name -> value conversion for a reflected enum.
         *
         * The inverse of @ref EnumName, implemented with the same zero-table
         * unrolled scan.
         *
         * @return The enumerator whose identifier equals @p name, or
         *         `std::nullopt` when no enumerator matches.
         */
        template<typename E> requires std::is_enum_v<E>
        [[nodiscard]] constexpr std::optional<E> EnumCast(std::string_view name) noexcept {
            template for (constexpr auto e : Enumerators<E>) {
                if (std::meta::identifier_of(e) == name)
                    return std::meta::extract<E>(e);
            }
            return std::nullopt;
        }

        // ==========================================================================
        // Base-class reflection
        // ==========================================================================

        /// @brief Static reflection array of @p T's direct base-class specifiers.
        template<typename T> requires std::is_class_v<T>
        inline constexpr auto Bases = std::define_static_array(
            std::meta::bases_of(^^T, std::meta::access_context::unchecked()));

        /// @brief Number of direct base classes of @p T.
        template<typename T> requires std::is_class_v<T>
        inline constexpr size_t BasesCount = Bases<T>.size();

        /**
         * @brief The type of @p T's @p I-th direct base class.
         * @tparam T Class type.
         * @tparam I Zero-based base index; must be `< BasesCount<T>`.
         */
        template<typename T, size_t I>
        using BaseType = typename [:std::meta::type_of(Bases<T>[I]):];

        /**
         * @brief A class type with no direct base classes.
         *
         * The reflection query is guarded behind `std::is_class_v`, so the concept
         * is well-formed (and simply unsatisfied) for non-class types.
         */
        template<typename T>
        concept RootClass = std::is_class_v<T> &&
            std::meta::bases_of(^^T, std::meta::access_context::unchecked()).empty();

        /** @cond INTERNAL */
        namespace Detail {

            /// @brief Walk the inheritance chain of @p type; returns `false` if any
            ///        level has more than one direct base (i.e. not single inheritance).
            consteval bool IsSingleInheritedChain(std::meta::info type) {
                auto bases = std::meta::bases_of(type, std::meta::access_context::unchecked());
                while (!bases.empty()) {
                    if (bases.size() != 1) {
                        return false;
                    }
                    type = std::meta::type_of(bases[0]);
                    bases = std::meta::bases_of(type, std::meta::access_context::unchecked());
                }
                return true;
            }

            /// @brief Build the dotted root-to-derived identifier path of @p type.
            consteval std::string_view BuildUniqueIdentifier(std::meta::info type) {
                std::vector<std::meta::info> chain; // derived-first
                chain.push_back(type);
                auto bases = std::meta::bases_of(type, std::meta::access_context::unchecked());
                while (!bases.empty()) {
                    type = std::meta::type_of(bases[0]);
                    chain.push_back(type);
                    bases = std::meta::bases_of(type, std::meta::access_context::unchecked());
                }
                std::string res;
                for (size_t i = chain.size(); i-- > 0;) {
                    if (!res.empty()) {
                        res += '.';
                    }
                    res += std::meta::identifier_of(chain[i]);
                }
                return std::define_static_string(res);
            }

            /// @brief Build the dotted root-to-scoped identifier path of @p type.
            consteval std::string_view BuildScopedIdentifier(std::meta::info type) {
                std::vector<std::meta::info> chain; // derived-first
                chain.push_back(type);
                auto parent = std::meta::parent_of(type, std::meta::access_context::unchecked());
                while (!parent.empty()) {
                    type = std::meta::type_of(parent[0]);
                    chain.push_back(type);
                    parent = std::meta::parent_of(type, std::meta::access_context::unchecked());
                }
                std::string res;
                for (size_t i = chain.size(); i-- > 0;) {
                    if (!res.empty()) {
                        res += '.';
                    }
                    res += std::meta::identifier_of(chain[i]);
                }
                return std::define_static_string(res);
            }

        } // namespace Detail
        /** @endcond */

        /**
         * @brief A class whose inheritance chain is single (linear) all the way up.
         *
         * Every level of the hierarchy has at most one direct base. Root classes
         * (no bases) trivially satisfy this. Validated entirely at compile time.
         */
        template<typename T>
        concept SingleInheritedClass =
            std::is_class_v<T> && Detail::IsSingleInheritedChain(^^T);

        /**
         * @brief Stable, dotted root-to-derived identifier path for a linear class.
         *
         * For `struct A {}; struct B : A {}; struct C : B {};`, `UniqueIdentifier<C>`
         * is `"A.B.C"`. Backed by `std::define_static_string`, so the resulting
         * `std::string_view` has static storage duration and is usable as a stable
         * compile-time key (e.g. for type registries or serialisation tags).
         *
         * @tparam T A @ref SingleInheritedClass.
         */
        template<SingleInheritedClass T>
        inline constexpr std::string_view UniqueIdentifier =
            Detail::BuildUniqueIdentifier(^^T);

    } // namespace Traits

    namespace Platform {

        /// @brief Cache line size for false-sharing avoidance (`alignas(kCacheLineSize)`).
#ifdef __cpp_lib_hardware_interference_size
        inline constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
        inline constexpr size_t kCacheLineSize = 64;
#endif

    } // namespace Platform

    namespace Traits {

        // =====================================================================
        // Type-level list and combinators
        // =====================================================================

        /// @brief A compile-time list of types — the type-level analogue of a tuple.
        template<typename... Ts>
        struct TypeList {
            static constexpr size_t size = sizeof...(Ts); ///< Number of elements.
        };

        /// @brief Number of elements in @p L.
        template<template<typename...> class L, typename... Ts>
        inline constexpr size_t Length = L<Ts...>::size;

        /** @cond INTERNAL */
        namespace Detail {

            template<typename L, size_t I>
            struct AtT;
            template<typename... Ts, size_t I>
            struct AtT<TypeList<Ts...>, I> {
                using type = Ts...[I];
            };

            template<typename L>
            struct TailT;
            template<typename T, typename... Ts>
            struct TailT<TypeList<T, Ts...>> {
                using type = TypeList<Ts...>;
            };

            template<typename... Ls>
            consteval std::meta::info ConcatImpl() {
                std::vector<std::meta::info> types;
                auto collect = [&](std::meta::info listType) {
                    for (auto arg : std::meta::template_arguments_of(listType))
                        types.push_back(arg);
                };
                (collect(^^Ls), ...);
                return std::meta::substitute(^^TypeList, types);
            }

            template<typename... Ls>
            struct ConcatT {
                using type = typename [:ConcatImpl<Ls...>():]; 
            };
            template<>
            struct ConcatT<> {
                using type = TypeList<>;
            };

            template<template<typename> typename F, typename L>
            struct MapTT;
            template<template<typename> typename F, typename... Ts>
            struct MapTT<F, TypeList<Ts...>> {
                using type = TypeList<F<Ts>...>;
            };

            template<template<typename> typename Pred, typename L>
            struct FilterTT;
            template<template<typename> typename Pred, typename... Ts>
            struct FilterTT<Pred, TypeList<Ts...>> {
                static consteval std::meta::info Compute() {
                    std::vector<std::meta::info> result;
                    const bool keep[] = {Pred<Ts>::value...};
                    auto all = std::meta::template_arguments_of(^^TypeList<Ts...>);
                    for (size_t i = 0; i < all.size(); ++i) {
                        if (keep[i])
                            result.push_back(all[i]);
                    }
                    return std::meta::substitute(^^TypeList, result);
                }
                using type = typename [:Compute():]; 
            };

            template<template<typename...> typename F, typename L>
            struct ApplyTT;
            template<template<typename...> typename F, typename... Ts>
            struct ApplyTT<F, TypeList<Ts...>> {
                using type = F<Ts...>;
            };

            template<template<typename, typename> typename Op, typename Acc, typename L>
            struct FoldTT;
            template<template<typename, typename> typename Op, typename Acc>
            struct FoldTT<Op, Acc, TypeList<>> {
                using type = Acc;
            };
            template<template<typename, typename> typename Op, typename Acc, typename T, typename... Ts>
            struct FoldTT<Op, Acc, TypeList<T, Ts...>> {
                using type = typename FoldTT<Op, typename [:std::meta::substitute(^^Op, {^^Acc, ^^T}):], TypeList<Ts...>>::type;
            };

            template<typename T, typename... Ts>
            consteval size_t IndexOfImpl() {
                if constexpr (sizeof...(Ts) == 0) {
                    return size_t(-1);
                } else {
                    const bool match[] = {std::is_same_v<T, Ts>...};
                    for (size_t i = 0; i < sizeof...(Ts); ++i) {
                        if (match[i]) {
                            return i;
                        }
                    }
                    return size_t(-1);
                }
            }
            template<typename L, typename T>
            struct IndexOfT;
            template<typename T, typename... Ts>
            struct IndexOfT<TypeList<Ts...>, T> {
                static constexpr size_t value = IndexOfImpl<T, Ts...>();
            };

            template<typename T>
            consteval std::meta::info TypeListReflOf() {
                std::vector<std::meta::info> types;
                for (auto m : std::meta::nonstatic_data_members_of(
                         ^^T, std::meta::access_context::unchecked())) {
                    types.push_back(std::meta::type_of(m));
                }
                return std::meta::substitute(^^TypeList, types);
            }

            template<typename L>
            struct LastT;
            template<typename... Ts>
            struct LastT<TypeList<Ts...>> {
                static_assert(sizeof...(Ts) > 0, "Last of an empty TypeList");
                using type = Ts...[sizeof...(Ts) - 1];
            };

            template<typename T, typename L>
            struct PushFrontT;
            template<typename T, typename... Ts>
            struct PushFrontT<T, TypeList<Ts...>> {
                using type = TypeList<T, Ts...>;
            };

            template<typename L, typename T>
            struct PushBackT;
            template<typename T, typename... Ts>
            struct PushBackT<TypeList<Ts...>, T> {
                using type = TypeList<Ts..., T>;
            };

            template<typename L>
            consteval std::meta::info ReverseImpl() {
                auto args = std::meta::template_arguments_of(^^L);
                std::vector<std::meta::info> rev(args.rbegin(), args.rend());
                return std::meta::substitute(^^TypeList, rev);
            }
            template<typename L>
            struct ReverseT {
                using type = typename [:ReverseImpl<L>():];
            };

            template<typename L>
            consteval std::meta::info PopBackImpl() {
                auto args = std::meta::template_arguments_of(^^L);
                if (!args.empty()) {
                    args.pop_back();
                }
                return std::meta::substitute(^^TypeList, args);
            }
            template<typename L>
            struct PopBackT {
                using type = typename [:PopBackImpl<L>():];
            };

            template<typename... Ts>
            consteval std::meta::info UniqueImpl() {
                std::vector<std::meta::info> result;
                auto all = std::meta::template_arguments_of(^^TypeList<Ts...>);
                for (auto arg : all) {
                    bool seen = false;
                    for (auto kept : result) {
                        if (kept == arg) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) {
                        result.push_back(arg);
                    }
                }
                return std::meta::substitute(^^TypeList, result);
            }
            template<typename L>
            struct UniqueT;
            template<typename... Ts>
            struct UniqueT<TypeList<Ts...>> {
                using type = typename [:UniqueImpl<Ts...>():];
            };

            template<template<typename> typename Pred, typename L>
            struct PredFoldT;
            template<template<typename> typename Pred, typename... Ts>
            struct PredFoldT<Pred, TypeList<Ts...>> {
                static constexpr bool all = (Pred<Ts>::value && ...);
                static constexpr bool any = (Pred<Ts>::value || ...);
                static constexpr size_t count =
                    (size_t{0} + ... + (Pred<Ts>::value ? size_t{1} : size_t{0}));
            };

            template<template<typename> typename Pred, typename L>
            inline constexpr bool AllOfV = PredFoldT<Pred, L>::all;
            template<template<typename> typename Pred, typename L>
            inline constexpr bool AnyOfV = PredFoldT<Pred, L>::any;
            template<template<typename> typename Pred, typename L>
            inline constexpr size_t CountIfV = PredFoldT<Pred, L>::count;

        } // namespace Detail
        /** @endcond */

        /// @brief The @p I-th element of @p L (C++26 pack indexing).
        template<typename L, size_t I>
        using At = typename Detail::AtT<L, I>::type;

        /// @brief First element of @p L.
        template<typename L>
        using Head = At<L, 0>;

        /// @brief All but the first element of @p L.
        template<typename L>
        using Tail = typename Detail::TailT<L>::type;

        /// @brief Concatenate any number of `TypeList`s.
        template<typename... Ls>
        using Concat = typename Detail::ConcatT<Ls...>::type;

        /// @brief Apply unary metafunction @p F to each element: `TypeList<F<Ts>...>`.
        template<template<typename> typename F, typename L>
        using MapT = typename Detail::MapTT<F, L>::type;

        /// @brief Keep elements for which `Pred<T>::value` holds.
        template<template<typename> typename Pred, typename L>
        using FilterT = typename Detail::FilterTT<Pred, L>::type;

        /// @brief Instantiate variadic template @p F with the list elements: `F<Ts...>`.
        template<template<typename...> typename F, typename L>
        using ApplyT = typename Detail::ApplyTT<F, L>::type;

        /// @brief Left fold with binary metafunction `Op<Acc, T>` and seed @p Init.
        template<template<typename, typename> typename Op, typename Init, typename L>
        using FoldT = typename Detail::FoldTT<Op, Init, L>::type;

        /// @brief Index of the first occurrence of @p T in @p L, or `size_t(-1)`.
        template<typename L, typename T>
        inline constexpr size_t IndexOf = Detail::IndexOfT<L, T>::value;

        /// @brief Whether @p L contains type @p T.
        template<typename L, typename T>
        inline constexpr bool Contains = (IndexOf<L, T> != size_t(-1));

        /// @brief Last element of @p L (requires a non-empty list).
        template<typename L>
        using Last = typename Detail::LastT<L>::type;

        /// @brief Prepend @p T to @p L: `TypeList<T, Ts...>`.
        template<typename T, typename L>
        using PushFront = typename Detail::PushFrontT<T, L>::type;

        /// @brief Append @p T to @p L: `TypeList<Ts..., T>`.
        template<typename L, typename T>
        using PushBack = typename Detail::PushBackT<L, T>::type;

        /// @brief Remove the last element of @p L (no-op on an empty list).
        template<typename L>
        using PopBack = typename Detail::PopBackT<L>::type;

        /// @brief Reverse the element order of @p L.
        template<typename L>
        using Reverse = typename Detail::ReverseT<L>::type;

        /// @brief Remove duplicate elements from @p L, keeping first occurrences.
        template<typename L>
        using Unique = typename Detail::UniqueT<L>::type;

        /// @brief `true` if `Pred<T>::value` holds for **every** element of @p L
        ///        (vacuously `true` for an empty list).
        template<template<typename> typename Pred, typename L>
        inline constexpr bool AllOf = Detail::AllOfV<Pred, L>;

        /// @brief `true` if `Pred<T>::value` holds for **at least one** element.
        template<template<typename> typename Pred, typename L>
        inline constexpr bool AnyOf = Detail::AnyOfV<Pred, L>;

        /// @brief `true` if `Pred<T>::value` holds for **no** element.
        template<template<typename> typename Pred, typename L>
        inline constexpr bool NoneOf = !AnyOf<Pred, L>;

        /// @brief Number of elements of @p L for which `Pred<T>::value` holds.
        template<template<typename> typename Pred, typename L>
        inline constexpr size_t CountIf = Detail::CountIfV<Pred, L>;

        /// @brief Reflection bridge: a `TypeList` of @p T's data-member types.
        template<typename T>
        using ToTypeList = [:Detail::TypeListReflOf<T>():];

        // =====================================================================
        // Annotation helpers
        // =====================================================================

        /**
         * @brief Generic helpers for probing and extracting C++26 annotations.
         *
         * Subsystems define their own annotation tag types (e.g.
         * `Hashing::Anno::Ignore`, `Json::Anno::Ignore`) and parameterise these
         * helpers with them; the helpers themselves are entirely subsystem-
         * agnostic. All operations are `consteval` so they fold to immediate
         * values at the call site.
         *
         * @code
         * struct MyIgnore {};
         * struct My { [[=MyIgnore{}]] int hidden; int kept; };
         * static_assert( Mashiro::Traits::Anno::Has<MyIgnore>(My's hidden member) );
         * @endcode
         */
        namespace Anno {

            /// @brief `true` if @p ent carries any annotation of type @p A.
            template <typename A>
            consteval bool Has(std::meta::info ent) {
                return std::meta::annotations_of(ent, ^^A).size() > 0;
            }

            /// @brief Extract the first annotation of type @p A on @p ent, or `nullopt`.
            template <typename A>
            consteval std::optional<A> Get(std::meta::info ent) {
                auto annots = std::meta::annotations_of(ent, ^^A);
                if (annots.size() > 0) return std::meta::extract<A>(annots[0]);
                return std::nullopt;
            }

            /// @brief `true` if any non-static data member of @p T carries
            ///        annotation @p A (the "whitelist" / Key-mode trigger).
            template <typename T, typename A>
            consteval bool AnyMemberHas() {
                for (auto m : std::meta::nonstatic_data_members_of(
                                  ^^T, std::meta::access_context::unchecked()))
                    if (Has<A>(m)) return true;
                return false;
            }

            /**
             * @brief Filter and stably re-order @p T's non-static data members
             *        for serialisation-style traversal.
             *
             * @tparam T Reflectable class.
             * @tparam Ignore Tag type whose presence excludes a member.
             * @tparam Key   Tag type whose presence on *any* member engages
             *               whitelist mode (only Key-tagged members survive).
             * @tparam Order Tag type with a `priority` member (`int`) used to
             *               sort the surviving members in ascending order;
             *               members without `Order` sort after those with it
             *               (priority = INT_MAX), preserving declaration
             *               order within each tier.
             *
             * @return A `std::vector<std::meta::info>` of selected members in
             *         emission order. The result is intended to be wrapped in
             *         `std::define_static_array(...)` at the call site for use
             *         with `template for`.
             */
            template <typename T, typename Ignore, typename Key, typename Order>
            consteval std::vector<std::meta::info> SelectMembers() {
                auto all = std::meta::nonstatic_data_members_of(
                    ^^T, std::meta::access_context::unchecked());
                const bool keyMode = AnyMemberHas<T, Key>();
                std::vector<std::meta::info> result;
                for (auto m : all) {
                    if (Has<Ignore>(m)) continue;
                    if (keyMode && !Has<Key>(m)) continue;
                    result.push_back(m);
                }
                constexpr int kSentinel = 0x7FFFFFFF;
                auto priorityOf = [](std::meta::info m) -> int {
                    auto o = Get<Order>(m);
                    return o ? o->priority : kSentinel;
                };
                for (size_t i = 1; i < result.size(); ++i) {
                    auto key = result[i];
                    int  pk  = priorityOf(key);
                    size_t j = i;
                    while (j > 0 && priorityOf(result[j - 1]) > pk) {
                        result[j] = result[j - 1];
                        --j;
                    }
                    result[j] = key;
                }
                return result;
            }

            /**
             * @brief Extract a typed payload from a templated annotation
             *        (e.g. `Rename<FixedString>`).
             *
             * Specialise the primary template on the user's templated annotation
             * to expose `value = true` and a `static constexpr` payload — see
             * the @ref Mashiro::Json::Detail::RenameTrait pattern for a worked
             * example. This trait is provided here as a convenience anchor for
             * subsystems that all share the same NTTP-as-annotation pattern.
             */
            template <typename T>
            struct PayloadTrait {
                static constexpr bool value = false;
            };

        } // namespace Anno

        // =====================================================================
        // Fundamental type categorisation
        // =====================================================================

        /** @cond INTERNAL */
        namespace Detail {

            /// @brief Reflection probe: is @p T a specialisation of template @p Tmpl?
            ///
            /// Compares the underlying class template of @p T (via
            /// `std::meta::template_of`) against the reflection of @p Tmpl,
            /// rather than relying on partial-specialisation pattern matching.
            /// This handles templates with non-type or template parameters
            /// (e.g. `std::array<T, N>`) that a `T<Args...>`-style partial
            /// specialisation cannot express.
            template<typename T, template<typename...> class Tmpl>
            consteval bool IsSpecializationOf() {
                if (!std::meta::is_class_type(^^T) ||
                    !std::meta::has_template_arguments(^^T)) {
                    return false;
                }
                return std::meta::template_of(^^T) == ^^Tmpl;
            }

        } // namespace Detail
        /** @endcond */

        /**
         * @brief `true` if @p T (cvref-stripped) is a specialisation of class
         *        template @p Tmpl.
         *
         * Implemented via C++26 static reflection: the underlying class template
         * of @p T is recovered with `std::meta::template_of` and compared against
         * the reflection of the template-template parameter @p Tmpl. The
         * `template<typename...> class` parameter means @p Tmpl must be a template
         * whose parameters are all type parameters (e.g. `std::vector`,
         * `std::variant`, `std::optional`); templates with non-type parameters
         * (such as `std::array`) cannot bind to it.
         *
         * @code
         * static_assert(  SpecializationOf<std::vector<int>, std::vector> );
         * static_assert( !SpecializationOf<int,             std::vector> );
         * @endcode
         */
        template<typename T, template<typename...> class Tmpl>
        concept SpecializationOf =
            Detail::IsSpecializationOf<std::remove_cvref_t<T>, Tmpl>();

        /// @brief Any enumeration type (scoped or unscoped).
        template<typename T>
        concept Enumeration = std::is_enum_v<T>;

        /// @brief A scoped enumeration (`enum class` / `enum struct`).
        template<typename T>
        concept ScopedEnum = std::is_scoped_enum_v<T>;

        /// @brief An unscoped (C-style) enumeration.
        template<typename T>
        concept UnscopedEnum = std::is_enum_v<T> && !std::is_scoped_enum_v<T>;

        /// @brief An aggregate type (brace-initialisable, no user-declared ctors).
        template<typename T>
        concept Aggregate = std::is_aggregate_v<T>;

        /// @brief A standard-layout type (safe for C interop / `offsetof`).
        template<typename T>
        concept StandardLayoutType = std::is_standard_layout_v<T>;

        /// @brief A trivially copyable type (safe to relocate via `memcpy`).
        template<typename T>
        concept TriviallyCopyableType = std::is_trivially_copyable_v<T>;

        /// @brief An empty class type (no non-static data members, e.g. a tag).
        template<typename T>
        concept EmptyType = std::is_empty_v<T>;

        /// @brief A polymorphic class type (declares or inherits a virtual function).
        template<typename T>
        concept PolymorphicType = std::is_polymorphic_v<T>;

        /**
         * @brief A type whose value representation has no padding bits, so any two
         *        equal values share the same object representation.
         *
         * Such types are safe to hash or compare over their raw bytes (`memcmp`).
         */
        template<typename T>
        concept UniquelyRepresented = std::has_unique_object_representations_v<T>;

        /**
         * @brief A type that supports the static-reflection queries in this header
         *        (a class type or an enumeration type).
         */
        template<typename T>
        concept Reflectable = std::is_class_v<T> || std::is_enum_v<T>;

        // =====================================================================
        // Standard-library categorisation
        // =====================================================================

        /// @brief A `std::optional<U>` specialisation.
        template<typename T>
        concept StdOptional = SpecializationOf<T, std::optional>;

        /// @brief A `std::variant<...>` specialisation.
        template<typename T>
        concept StdVariant = SpecializationOf<T, std::variant>;

        /// @brief A `std::chrono::duration<Rep, Period>`.
        template <typename T>
        concept ChronoDuration = requires {
            typename T::rep; typename T::period;
            requires std::same_as<std::remove_cvref_t<T>,
                std::chrono::duration<typename T::rep, typename T::period>>;
        };

        /// @brief A `std::chrono::time_point<Clock, Duration>`.
        template <typename T>
        concept ChronoTimePoint = requires {
            typename T::clock; typename T::duration;
            requires std::same_as<std::remove_cvref_t<T>,
                std::chrono::time_point<typename T::clock, typename T::duration>>;
        };

        /// @brief A `std::filesystem::path`.
        template <typename T>
        concept FilesystemPath =
            std::same_as<std::remove_cvref_t<T>, std::filesystem::path>;

        /// @brief A range whose element type is `std::byte` (binary blob).
        template <typename T>
        concept ByteRange = std::ranges::range<T> &&
                            std::same_as<std::ranges::range_value_t<T>, std::byte>;

        /// @brief Convertible to `std::string_view` and not a path.
        ///
        /// `filesystem::path` is convertible-to-string-view on libstdc++ but
        /// has its own dedicated path; exclude it here so dispatchers can
        /// reach the path branch first.
        template <typename T>
        concept StringViewConvertible =
            std::convertible_to<std::remove_cvref_t<T>, std::string_view> &&
            !std::same_as<std::remove_cvref_t<T>, std::filesystem::path>;

        /// @brief An associative range whose key type is convertible to
        ///        `std::string_view` (i.e. encodes naturally as a JSON object).
        template <typename T>
        concept StringKeyedAssociative = std::ranges::range<T> &&
            requires { typename std::remove_cvref_t<T>::key_type; } &&
            std::convertible_to<typename std::remove_cvref_t<T>::key_type,
                                std::string_view>;

    }

    namespace Anno {
        
        using namespace Traits::Anno;

    }

} // namespace Mashiro
