/**
 * @file GetSet.h
 * @brief Reflection-based direct access helpers for public or explicitly exposed data members.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Core/Traits/ScopeTraits.h"

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
                if (!std::meta::is_nonstatic_data_member(M)) {
                    throw std::define_static_string("Meta::Get/Set: '" + std::string{std::meta::display_string_of(M)} +
                                                    "' is not a non-static data member reflection.");
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
        decltype(auto) Get(T&& object) {
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
        void Set(T&& object, V&& value) {
            Detail::ValidateObjectAndAccess<M, T>("Set");
            std::forward<T>(object).[:M:] = std::forward<V>(value);
        }

    } // namespace Meta

    using Meta::Get;
    using Meta::Set;

} // namespace Sora