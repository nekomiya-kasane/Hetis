/**
 * @file TypeTraits.h
 * @brief Reflection-based type traits, structural concepts, and utility type metadata.
 * @ingroup Core
 *
 * @details Provides compile-time introspection helpers built on C++26 static reflection and standard concepts. Every
 * facility is @c consteval or @c inline @c constexpr oriented, so queries fold to immediate values or pure type
 * computation without runtime cost.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "Sora/Platform.h"

namespace Sora {

    namespace Meta {

        /** @brief Splice a reflected type into a regular C++ type. */
        template<std::meta::info Info>
        using InfoType = [:Info:];

        /**
         * @brief Return the implementation-defined display string for a reflected declaration or type.
         * @param[in] iMeta Reflected entity whose display string is requested.
         * @return Display string reported by the active reflection implementation.
         */
        consteval auto DisplayStringOf(std::meta::info iMeta) {
            return std::meta::display_string_of(iMeta);
        }

        /**
         * @brief Return the source identifier for a reflected declaration.
         * @param[in] iMeta Reflected declaration whose identifier is requested.
         * @return Source identifier reported by the active reflection implementation.
         */
        consteval auto IdentifierOf(std::meta::info iMeta) {
            return std::meta::identifier_of(iMeta);
        }

        consteval auto ParamsOf(std::meta::info iMeta) {
            return std::meta::parameters_of(iMeta);
        }

        consteval auto TypeOf(std::meta::info iMeta) {
            return std::meta::type_of(iMeta);
        }

        /**
         * @brief Project a function reflection's parameter list to a list of parameter type infos.
         * @details Throws a compile-time diagnostic naming the offending entity if @p f is not callable.
         * @param[in] f Reflected function whose parameter types are requested.
         * @return A vector of type infos representing the parameter types of @p f.
         */
        consteval std::vector<std::meta::info> ParamTypesOf(std::meta::info f) {
            if (!std::meta::is_function(f)) {
                throw std::define_static_string(
                    "Meta::ParamTypesOf: '" + std::string{std::meta::display_string_of(f)} +
                    "' is not a function reflection — only methods participate in vtable synthesis.");
            }
            std::vector<std::meta::info> out;
            for (auto p : ParamsOf(f)) {
                out.push_back(std::meta::type_of(p));
            }
            return out;
        }

        /**
         * @brief Return the return type of a reflected function.
         * @param[in] f Reflected function whose return type is requested.
         * @return Type info representing the return type of @p f.
         */
        consteval std::meta::info ReturnTypeOf(std::meta::info f) {
            if (!std::meta::is_function(f)) {
                throw std::define_static_string(
                    "Meta::ReturnTypeOf: '" + std::string{std::meta::display_string_of(f)} +
                    "' is not a function reflection — only methods participate in vtable synthesis.");
            }
            return std::meta::return_type_of(f);
        }

        /**
         * @brief @c true if @p m reflects a non-static, non-special member function. Filters out constructors,
         * destructors, static member functions, and data members.
         */
        consteval bool IsRegularMethod(std::meta::info m) {
            return std::meta::is_function(m) && !std::meta::is_static_member(m) && !std::meta::is_constructor(m) &&
                   !std::meta::is_destructor(m);
        }

    } // namespace Meta

    namespace Traits {

        /** @brief Reflection display string for @p T, preserving template arguments where available. */
        template<typename T>
        inline constexpr auto TypeName = std::meta::display_string_of(^^T);

        /** @brief Reflection display string for @p T after removing cv/ref qualifiers and aliases. */
        template<typename T>
        inline constexpr auto DealiasedTypeName =
            std::meta::display_string_of(std::meta::dealias(std::meta::remove_cvref(^^T)));

        /** @brief Static reflection array of @p T's members using unchecked access. */
        template<typename T>
        inline constexpr auto Members =
            std::define_static_array(std::meta::members_of(^^T, std::meta::access_context::unchecked()));

        /** @brief Number of members in @p T. */
        template<typename T>
        inline constexpr size_t MembersCount = Members<T>.size();

        /** @brief Source identifier of @p T's @p I-th member. */
        template<typename T, size_t I>
            requires std::is_class_v<T>
        inline constexpr std::string_view MemberIdentifier = std::meta::identifier_of(Members<T>[I]);

        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto MemberIdentierArr = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(Members<T>[I])...};
            }(std::make_index_sequence<MembersCount<T>>{});
        };

        /** @brief Static reflection array of @p T's non-static data members using unchecked access. */
        template<typename T>
        inline constexpr auto DataMembers =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));

        /** @brief Number of non-static data members in @p T. */
        template<typename T>
        inline constexpr size_t DataMembersCount = DataMembers<T>.size();

        /**
         * @brief Source identifier of @p T's @p I-th non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index. Must be less than @ref DataMembersCount for @p T.
         */
        template<typename T, size_t I>
            requires std::is_class_v<T>
        inline constexpr std::string_view DataMemberIdentifier = std::meta::identifier_of(DataMembers<T>[I]);

        /** @brief Static array containing source identifiers for every non-static data member of @p T. */
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto DataMemberIdentifiersArr = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(DataMembers<T>[I])...};
            }(std::make_index_sequence<DataMembersCount<T>>{});
        }();

        /** @brief Static reflection array of @p T's public non-static data members using unprivileged access. */
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto PublicDataMembers = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));

        /** @brief Number of public non-static data members in @p T. */
        template<typename T>
        inline constexpr size_t PublicDataMembersCount = PublicDataMembers<T>.size();

        /** @brief Static array containing source identifiers for every public non-static data member of @p T. */
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto PublicDataMemberIdentifiersArr = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(PublicDataMembers<T>[I])...};
            }(std::make_index_sequence<PublicDataMembersCount<T>>{});
        }();

        /**
         * @brief Source identifier of @p T's @p I-th public non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index. Must be less than @ref PublicMembersCount for @p T.
         */
        template<typename T, size_t I>
            requires std::is_class_v<T>
        inline constexpr std::string_view PublicDataMemberIdentifier =
            std::meta::identifier_of(PublicDataMembers<T>[I]);

        /**
         * @brief Declared type of @p T's @p I-th non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index. Must be less than @ref DataMembersCount for @p T.
         */
        template<typename T, size_t I>
            requires std::is_class_v<T>
        using DataMemberType = typename [:std::meta::type_of(DataMembers<T>[I]):];

        /** @brief Total padding bytes in @p T, computed from object size minus member object sizes. */
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr size_t PaddingBytes = sizeof(T) - [] consteval {
            size_t total = 0;
            for (auto m : DataMembers<T>) {
                total += std::meta::size_of(std::meta::type_of(m));
            }
            return total;
        }();

    } // namespace Traits

    namespace Concept {

        /** @brief Class type with at least one non-static data member and no detected padding bytes. */
        template<typename T>
        concept CompactClass = std::is_class_v<T> && Traits::DataMembersCount<T> > 0 && Traits::PaddingBytes<T> == 0;

        /** @brief Class type with padding bytes detected by @ref Traits::PaddingBytes. */
        template<typename T>
        concept PaddedClass = std::is_class_v<T> && Traits::PaddingBytes<T> > 0;

        /** @brief Class type whose non-static data members all have the same dealiased declared type. */
        template<typename T>
        concept HomogeneousClass = std::is_class_v<T> && Traits::DataMembersCount<T> > 0 && [] consteval {
            auto first_type = std::meta::dealias(std::meta::type_of(Traits::DataMembers<T>[0]));
            for (auto m : Traits::DataMembers<T>) {
                if (std::meta::dealias(std::meta::type_of(m)) != first_type) {
                    return false;
                }
            }
            return true;
        }();

        /** @brief An aggregate type (brace-initialisable, no user-declared ctors). */
        template<typename T>
        concept AggregateType = std::is_aggregate_v<T>;

        /** @brief A standard-layout type (safe for C interop / `offsetof`). */
        template<typename T>
        concept StandardLayoutType = std::is_standard_layout_v<T>;

        /** @brief A trivially copyable type (safe to relocate via `memcpy`). */
        template<typename T>
        concept TriviallyCopyableType = std::is_trivially_copyable_v<T>;

        /** @brief An empty class type (no non-static data members, e.g. a tag). */
        template<typename T>
        concept EmptyType = std::is_empty_v<T>;

        /** @brief A polymorphic class type (declares or inherits a virtual function). */
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

        /** @brief Range type whose elements are of type @p E. */
        template<typename T, typename E>
        concept RangeWithElementType = std::ranges::range<T> && std::same_as<std::ranges::range_value_t<T>, E>;

        /** @brief Type that exposes the standard tuple protocol through @c std::tuple_size and @c std::get. */
        template<typename T>
        concept TupleLikeClass = requires { typename std::tuple_size<std::remove_cvref_t<T>>::value_type; } &&
                                 []<size_t... N>(std::index_sequence<N...>) {
                                     return requires(T && t) { (std::get<N>(t), ...); };
                                 }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

        /** @brief Type that exposes the standard variant protocol through @c std::variant_size and @c std::get. */
        template<typename T>
        concept VariantLikeClass = requires { typename std::variant_size<std::remove_cvref_t<T>>::value_type; } &&
                                   []<size_t... N>(std::index_sequence<N...>) {
                                       return requires(T && t) { (std::get<N>(t), ...); };
                                   }(std::make_index_sequence<std::variant_size_v<std::remove_cvref_t<T>>>{});

    } // namespace Concept

    namespace Traits {

        /**
         * @brief Pure-type-parameter substitution target for thunk-pointer construction.
         *         Specialisations are exactly the flat free-function-pointer types `R(*)(Args...)`,
         *         used as the reflection-domain handle for `std::meta::substitute`.
         */
        template<typename Ret, typename... Args>
        using FunctionPointer = Ret (*)(Args...);

    } // namespace Traits

} // namespace Sora
