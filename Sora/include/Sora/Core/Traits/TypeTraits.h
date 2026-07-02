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

    } // namespace Meta

    namespace Traits {

        /** @brief Reflection display string for @p T, preserving template arguments where available. */
        template<typename T>
        inline constexpr auto TypeName = std::meta::display_string_of(^^T);

        /** @brief Reflection display string for @p T after removing cv/ref qualifiers and aliases. */
        template<typename T>
        inline constexpr auto DealiasedTypeName =
            std::meta::display_string_of(std::meta::dealias(std::meta::remove_cvref(^^T)));

        /** @brief Static reflection array of @p T's non-static data members using unchecked access. */
        template<typename T>
        inline constexpr auto Members =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));

        /** @brief Number of non-static data members in @p T. */
        template<typename T>
        inline constexpr size_t MembersCount = Members<T>.size();

        /**
         * @brief Source identifier of @p T's @p I-th non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index. Must be less than @ref MembersCount for @p T.
         */
        template<typename T, size_t I>
            requires std::is_class_v<T>
        inline constexpr std::string_view MemberIdentifier = std::meta::identifier_of(Members<T>[I]);

        /** @brief Static array containing source identifiers for every non-static data member of @p T. */
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto MemberIdentifiersArr = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(Members<T>[I])...};
            }(std::make_index_sequence<MembersCount<T>>{});
        }();

        /** @brief Static reflection array of @p T's public non-static data members using unprivileged access. */
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto PublicMembers = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));

        /** @brief Number of public non-static data members in @p T. */
        template<typename T>
        inline constexpr size_t PublicMembersCount = PublicMembers<T>.size();

        /** @brief Static array containing source identifiers for every public non-static data member of @p T. */
        template<typename T>
            requires std::is_class_v<T>
        inline constexpr auto PublicMemberIdentifiersArr = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(PublicMembers<T>[I])...};
            }(std::make_index_sequence<PublicMembersCount<T>>{});
        }();

        /**
         * @brief Source identifier of @p T's @p I-th public non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index. Must be less than @ref PublicMembersCount for @p T.
         */
        template<typename T, size_t I>
            requires std::is_class_v<T>
        inline constexpr std::string_view PublicMemberIdentifier = std::meta::identifier_of(PublicMembers<T>[I]);

        /**
         * @brief Declared type of @p T's @p I-th non-static data member.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based member index. Must be less than @ref MembersCount for @p T.
         */
        template<typename T, size_t I>
            requires std::is_class_v<T>
        using MemberType = typename [:std::meta::type_of(Members<T>[I]):];

        /** @brief Total padding bytes in @p T, computed from object size minus member object sizes. */
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

        /** @brief Class type with at least one non-static data member and no detected padding bytes. */
        template<typename T>
        concept CompactClass = std::is_class_v<T> && Traits::MembersCount<T> > 0 && Traits::PaddingBytes<T> == 0;

        /** @brief Class type with padding bytes detected by @ref Traits::PaddingBytes. */
        template<typename T>
        concept PaddedClass = std::is_class_v<T> && Traits::PaddingBytes<T> > 0;

        /** @brief Class type whose non-static data members all have the same dealiased declared type. */
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

        /** @brief Type that exposes the standard tuple protocol through @c std::tuple_size and @c std::get. */
        template<typename T>
        concept TupleLikeClass = requires {
            typename std::tuple_size<std::remove_cvref_t<T>>::value_type;
        } && []<size_t... N>(std::index_sequence<N...>) {
            return requires(T&& t) {
                (std::get<N>(t), ...);
            };
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

        /** @brief Type that exposes the standard variant protocol through @c std::variant_size and @c std::get. */
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
