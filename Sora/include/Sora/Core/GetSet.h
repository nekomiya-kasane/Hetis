/**
 * @file GetSet.h
 * @brief Reflection-based direct access helpers for public or explicitly exposed data members.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/FixedString.h"
#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Core/Traits/ClassTraits.h"
#include "Sora/Core/Traits/ScopeTraits.h"
#include "Sora/Core/Hash.h"

#include <cstddef>
#include <meta>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace $ {

        /**
         * @brief Marks a non-public data member as accessible through @ref Sora::Meta::Get and @ref Sora::Meta::Set.
         */
        struct Exposure {
            bool value{}; /**< Whether the annotated member is exposed. */
        };

        /** @brief Annotation value enabling reflective get/set access to a data member. */
        inline constexpr Exposure Exposed{true};
        /** @brief Annotation value disabling reflective get/set access to a data member. */
        inline constexpr Exposure Unexposed{false};

    } // namespace $

    namespace Meta {

        namespace Detail {

            /** @brief Return a compact ABI discriminator for reflected member @p M. */
            template<std::meta::info M>
            consteval size_t MemberAccessDiscriminator() {
                constexpr auto owner = Sora::Meta::ParentScopeOf(M);
                size_t ordinal = 0;
                for (auto member :
                     std::meta::nonstatic_data_members_of(owner, std::meta::access_context::unchecked())) {
                    if (member == M) {
                        auto offset = std::meta::offset_of(M).total_bits();
                        return static_cast<size_t>(offset) * 131 + ordinal;
                    }
                    ++ordinal;
                }
                throw std::define_static_string("Meta::Get/Set: reflected data member is not in its owner type.");
            }

            /** @brief ABI-visible token that prevents reflection NTTP specialisations from sharing one symbol name. */
            template<std::meta::info M>
            using MemberAccessToken = std::integral_constant<size_t, MemberAccessDiscriminator<M>()>;

            /** @brief ABI-visible token that prevents name NTTP specialisations from sharing one symbol name. */
            template<auto Name>
                requires requires { Name.view(); }
            using NameAccessToken = std::integral_constant<size_t, static_cast<size_t>(Hashing::Hash(Name.view()))>;

            /** @brief Validate that object type @p T can contain member @p M and that @p M is accessible. */
            template<std::meta::info M, typename T>
            consteval void ValidateObjectAndAccess(std::string_view operation) {
                if (!std::meta::is_nonstatic_data_member(M)) {
                    throw std::define_static_string("Meta::Get/Set: '" + std::string{std::meta::display_string_of(M)} +
                                                    "' is not a non-static data member reflection.");
                }
                constexpr auto owner = Sora::Meta::ParentScopeOf(M);
                using Owner = Sora::Meta::InfoType<owner>;
                using Object = std::remove_cvref_t<T>;
                if (!std::is_base_of_v<Owner, Object>) {
                    throw std::define_static_string(
                        "Meta::" + std::string{operation} + ": '" + std::string{std::meta::display_string_of(M)} +
                        "' is not a member of type '" + std::string{std::meta::display_string_of(^^Object)} + "'.");
                }
                const auto exposure = Sora::$::GetSingleOptional<Sora::$::Exposure>(M);
                bool accessible = (std::meta::is_public(M) && (!exposure.has_value() || exposure->value)) ||
                                  (exposure.has_value() && exposure->value);
                if (!accessible) {
                    throw std::define_static_string("Meta::" + std::string{operation} + ": '" +
                                                    std::string{std::meta::display_string_of(M)} +
                                                    "' is not public and is not marked with Sora::$::Exposed.");
                }
            }

        } // namespace Detail

        /**
         * @brief Return data member @p M from @p object when it is public or annotated with @ref Sora::$::Exposed.
         * @tparam M Reflection of a non-static data member.
         * @tparam T Object type containing @p M or deriving from its owner type.
         * @param[in] object Object whose member is read.
         * @return The selected member, preserving reference category and cv-qualification.
         */
        template<std::meta::info M, typename T>
        decltype(auto) Get(T&& object, Detail::MemberAccessToken<M>* = nullptr) {
            Detail::ValidateObjectAndAccess<M, T>("Get");
            return std::forward<T>(object).[:M:];
        }

        /**
         * @brief Assign @p value to data member @p M when the member is public or annotated with @ref Sora::$::Exposed.
         * @tparam M Reflection of a non-static data member.
         * @tparam T Object type containing @p M or deriving from its owner type.
         * @tparam V Assigned value type.
         * @param[in,out] object Object whose member is assigned.
         * @param[in] value Value forwarded into the selected member.
         */
        template<std::meta::info M, typename T, typename V>
        void Set(T&& object, V&& value, Detail::MemberAccessToken<M>* = nullptr) {
            Detail::ValidateObjectAndAccess<M, T>("Set");
            std::forward<T>(object).[:M:] = std::forward<V>(value);
        }

        /**
         * @brief Return the data member named @p Name from @p object using compile-time unchecked lookup.
         * @tparam Name FixedString-like source identifier or @ref Sora::$::Name schema name.
         * @tparam T Object type whose single-inheritance chain is searched.
         * @param[in] object Object whose member is read.
         * @return The selected member, preserving reference category and cv-qualification.
         */
        template<auto Name, typename T>
            requires requires { Name.view(); }
        decltype(auto) Get(T&& object, Detail::NameAccessToken<Name>* = nullptr) {
            using Root = std::remove_cvref_t<T>;
            return Get<Meta::FindDataMemberByName<Root, Name>()>(std::forward<T>(object));
        }

        /**
         * @brief Return member @p Name selected from explicit root type @p Root.
         * @tparam Root Class whose single-inheritance chain is searched.
         * @tparam Name FixedString-like source identifier or @ref Sora::$::Name schema name.
         * @tparam T Object type containing the selected member.
         * @param[in] object Object whose member is read.
         * @return The selected member, preserving reference category and cv-qualification.
         */
        template<typename Root, auto Name, typename T>
            requires requires { Name.view(); }
        decltype(auto) Get(T&& object, Detail::NameAccessToken<Name>* = nullptr) {
            return Get<Meta::FindDataMemberByName<Root, Name>()>(std::forward<T>(object));
        }

        /**
         * @brief Assign @p value to the data member named @p Name in @p object's static type hierarchy.
         * @tparam Name FixedString-like source identifier or @ref Sora::$::Name schema name.
         * @tparam T Object type whose single-inheritance chain is searched.
         * @tparam V Assigned value type.
         * @param[in,out] object Object whose member is assigned.
         * @param[in] value Value forwarded into the selected member.
         */
        template<auto Name, typename T, typename V>
            requires requires { Name.view(); }
        void Set(T&& object, V&& value, Detail::NameAccessToken<Name>* = nullptr) {
            using Root = std::remove_cvref_t<T>;
            Set<Meta::FindDataMemberByName<Root, Name>()>(std::forward<T>(object), std::forward<V>(value));
        }

        /**
         * @brief Assign @p value to member @p Name selected from explicit root type @p Root.
         * @tparam Root Class whose single-inheritance chain is searched.
         * @tparam Name FixedString-like source identifier or @ref Sora::$::Name schema name.
         * @tparam T Object type containing the selected member.
         * @tparam V Assigned value type.
         * @param[in,out] object Object whose member is assigned.
         * @param[in] value Value forwarded into the selected member.
         */
        template<typename Root, auto Name, typename T, typename V>
            requires requires { Name.view(); }
        void Set(T&& object, V&& value, Detail::NameAccessToken<Name>* = nullptr) {
            Set<Meta::FindDataMemberByName<Root, Name>()>(std::forward<T>(object), std::forward<V>(value));
        }

    } // namespace Meta

    using Meta::Get;
    using Meta::Set;

    constexpr auto&& GetMemberRef(auto&& obj, std::string_view name) {
        using U = std::remove_cvref_t<decltype(obj)>;
        template for (constexpr auto m : Meta::MembersOf(^^U)) {
            if (std::meta::identifier_of(m) == name) {
                return std::forward<decltype(obj)>(obj).[:m:];
            }
        }
        static_assert(!sizeof(U), "GetMember: member not found");
    }

/** @brief Friend the reflection get/set member-access fast path for private exposed members. */
#define ALLOW_GET_SET                                                                                                  \
    template<std::meta::info M, typename T>                                                                            \
    friend decltype(auto) Sora::Meta::Get(T&& object, Sora::Meta::Detail::MemberAccessToken<M>*);                      \
    template<std::meta::info M, typename T, typename V>                                                                \
    friend void Sora::Meta::Set(T&& object, V&& value, Sora::Meta::Detail::MemberAccessToken<M>*);

} // namespace Sora
