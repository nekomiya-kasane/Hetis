/**
 * @file ClassTraits.h
 * @brief Reflection helpers for class member schema lookup.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/FixedString.h"
#include "Sora/Core/Traits/InheritanceTraits.h"

#include <meta>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Sora {

    namespace $ {

        /**
         * @brief Stable schema name used by string-based reflective get/set lookup.
         * @tparam S Compile-time member name exposed to callers.
         *
         * @details When present, @ref Sora::Meta::Get and @ref Sora::Meta::Set prefer this schema name over the source
         * identifier. The annotation lets an implementation rename a data member without changing the external
         * reflective property name.
         */
        template<auto S>
            requires requires { S.view(); }
        struct Name {
            /** @brief Compile-time property name. */
            static constexpr std::string_view value = S.view();
        };

    } // namespace $

    namespace Meta {

        /** @brief Return whether member @p M has a schema name or source identifier equal to @p Name. */
        template<auto Name>
            requires requires { Name.view(); }
        consteval bool DataMemberNameMatches(std::meta::info M) {
            for (auto annotation : std::meta::annotations_of(M)) {
                auto type = std::meta::type_of(annotation);
                if (!std::meta::is_type(type) || !std::meta::has_template_arguments(type)) {
                    continue;
                }
                if (std::meta::template_of(type) != ^^Sora::$::Name) {
                    continue;
                }
                auto args = std::meta::template_arguments_of(type);
                if (args.size() == 1 && args[0] == std::meta::reflect_constant(Name)) {
                    return true;
                }
            }
            return std::meta::has_identifier(M) && std::meta::identifier_of(M) == Name.view();
        }

        /** @brief Return the reflected data member named @p Name in @p Root's inheritance hierarchy. */
        template<typename Root, auto Name>
            requires requires { Name.view(); }
        consteval std::meta::info FindDataMemberByName() {
            static_assert(std::is_class_v<Root>, "Sora::Meta::Get/Set named lookup requires a class root type.");

            std::vector<std::meta::info> matches;
            for (auto type : Sora::Meta::InheritanceChainOf(^^Root)) {
                matches.clear();
                for (auto member : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked())) {
                    if (DataMemberNameMatches<Name>(member)) {
                        matches.push_back(member);
                    }
                }
                if (matches.size() == 1) {
                    return matches.front();
                }
                if (matches.size() > 1) {
                    throw std::define_static_string("Meta::Get/Set: member name '" + std::string{Name.view()} +
                                                    "' is ambiguous in '" +
                                                    std::string{std::meta::display_string_of(type)} + "'.");
                }
            }

            throw std::define_static_string("Meta::Get/Set: no data member named '" + std::string{Name.view()} +
                                            "' exists in '" + std::string{std::meta::display_string_of(^^Root)} +
                                            "' or its single-inheritance base chain.");
        }

    } // namespace Meta

} // namespace Sora