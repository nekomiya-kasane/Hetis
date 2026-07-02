/**
 * @file TypeTraits.h
 * @brief Reflection-based type traits, structural concepts, and utility types.
 *
 * Provides compile-time introspection helpers built on C++26 static reflection (`<meta>`, P2996) and standard
 * concepts. Every facility is `consteval` / `inline constexpr`, so it folds to immediate values or pure type
 * computation with **zero** runtime cost. Key facilities, grouped by concern:
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
 * - `RootClass<T>` / `SingleInheritedClass<T>`.
 * - `ChainedIdentifier<T>` / `ChainedDisplayString<T>` — dotted root-to-derived inheritance path;
 *   `ScopedIdentifier<T>` / `ScopedDisplayString<T>` — dotted enclosing-scope path. `*Identifier` use `identifier_of`;
 *   `*DisplayString` use `display_string_of` (keeping template arguments).
 * @ingroup Core
 */
#pragma once

#include <meta>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "Sora/Platform.h"

namespace Sora {

    namespace Meta {

        /// @brief display string of
        consteval auto DisplayStringOf(std::meta::info iMeta) {
            return std::meta::display_string_of(iMeta);
        }

        /// @brief identifier of
        consteval auto IdentifierOf(std::meta::info iMeta) {
            return std::meta::identifier_of(iMeta);
        }

    } // namespace Meta

    namespace Traits {

        template<typename T>
        inline constexpr auto TypeName = std::meta::display_string_of(^^T);

        template<typename T>
        inline constexpr auto DealiasedTypeName =
            std::meta::display_string_of(std::meta::dealias(std::meta::remove_cvref(^^T)));

        /// @brief Static reflection array of @p T's non-static data members.
        template<typename T>
        inline constexpr auto Members =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));

        /// @brief Number of non-static data members in @p T.
        template<typename T>
        inline constexpr size_t MembersCount = Members<T>.size();

        /**
         * @brief The source identifier of @p T's @p I-th non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index; must be `< MembersCount<T>`.
         */
        template<typename T, size_t I> 
            requires std::is_class_v<T>
        inline constexpr std::string_view MemberIdentifier = std::meta::identifier_of(Members<T>[I]);

        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto MemberIdentifiersArr = [] consteval {
            return []<size_t...I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(Members<T>[I])...};
            }(std::make_index_sequence<MembersCount<T>>{});
        }();

        /// @brief Static reflection array of @p T's public non-static data members.
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto PublicMembers = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));

        /// @brief Number of public non-static data members in @p T.
        template<typename T>
        inline constexpr size_t PublicMembersCount = PublicMembers<T>.size();

        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto PublicMemberIdentifiersArr = [] consteval {
            return []<size_t...I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(PublicMembers<T>[I])...};
            }(std::make_index_sequence<PublicMembersCount<T>>{});
        }();

        /**
         * @brief The source identifier of @p T's @p I-th public non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index; must be `< PublicMembersCount<T>`.
         */
        template<typename T, size_t I> 
            requires std::is_class_v<T>
        inline constexpr std::string_view PublicMemberIdentifier = std::meta::identifier_of(PublicMembers<T>[I]);

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
            requires std::is_class_v<T>
        using MemberType = typename [:std::meta::type_of(Members<T>[I]):];

        template<typename T>
            requires std::is_class_v<T>
        inline constexpr size_t PaddingBytes = sizeof(T) - [] consteval {
            size_t total = 0;
            for (auto m : Members<T>) {
                total += std::meta::size_of(std::meta::type_of(m));
            }
            return total;
        }();

    } // namespace Traits

    namespace Concept {

        template<typename T>
        concept CompactClass = std::is_class_v<T> && Traits::MembersCount<T> > 0 && Traits::PaddingBytes<T> == 0;

        template<typename T>
        concept PaddedClass = std::is_class_v<T> && Traits::PaddingBytes<T> > 0;

        template<typename T>
        concept HomogeneousClass = std::is_class_v<T> && Traits::MembersCount<T> > 0 && [] consteval {
            auto first_type = std::meta::dealias(std::meta::type_of(Traits::Members<T>[0]));
            for (auto m : Traits::Members<T>) {
                if (std::meta::dealias(std::meta::type_of(m)) != first_type) {
                    return false;
                }
            }
            return true;
        }();

        template<typename T>
        concept TupleLikeClass = requires {
            typename std::tuple_size<std::remove_cvref_t<T>>::value_type;
        } && []<size_t... N>(std::index_sequence<N...>) {
            return requires(T&& t) {
                (std::get<N>(t), ...);
            };
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

        template<typename T>
        concept VariantLikeClass = requires {
            typename std::variant_size<std::remove_cvref_t<T>>::value_type;
        } && []<size_t... N>(std::index_sequence<N...>) {
            return requires(T&& t) {
                (std::get<N>(t), ...);
            };
        }(std::make_index_sequence<std::variant_size_v<std::remove_cvref_t<T>>>{});

    } // namespace Concept

} // namespace Sora