/**
 * @file NamingTraits.h
 * @brief Stable reflection-derived unique names for types, members, and callable declarations.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/ABI.h>
#include <Sora/Core/Traits/InheritanceTraits.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>

#include <meta>
#include <string>
#include <string_view>

namespace Sora {

    namespace Meta {

        /**
         * @brief Return a stable, overload-aware unique name for @p info.
         * @param[in] info Reflected type, member, function, or method declaration.
         * @return Static-storage unique name view.
         *
         * @details Type and member spellings preserve the historical Sora naming scheme. Callable declarations use
         * the compile-time MSVC mangler because it already encodes the owning scope, name, return type, parameter
         * types, static/free/member form, and member cv/ref qualifiers. This keeps overload discrimination in one
         * implementation instead of duplicating a fragile signature renderer here.
         */
        consteval auto UniqueNameOf(std::meta::info info) {
            std::string desc;
            if (std::meta::is_function(info)) {
                desc += "function:";
                if (std::meta::has_identifier(info)) {
                    desc += Sora::Meta::ABI::Mangle<Sora::Meta::ABI::Kind::Itanium>(info);
                } else {
                    desc += std::meta::display_string_of(info);
                }
            } else if (std::meta::is_type(info) && std::meta::is_class_type(info)) {
                std::vector<std::meta::info> chain = InheritanceChainOf(info);
                for (auto& type : chain) {
                    if (!desc.empty()) {
                        desc += ".";
                    }
                    desc += "(";
                    desc += Sora::Meta::ScopeChainIdentifierOf(type, ":");
                    desc += ")";
                }
            } else if (std::meta::is_nonstatic_data_member(info)) {
                desc += "::";
                desc += Sora::Meta::IdentifierOrDisplayStringOf(info);
            } else if (std::meta::is_static_member(info)) {
                desc += ".";
                desc += Sora::Meta::IdentifierOrDisplayStringOf(info);
            }
            auto ret = std::define_static_array(desc);
            return std::string_view{ret.data(), ret.size()};
        }

    } // namespace Meta

    namespace Traits {

        template<typename T>
        inline constexpr std::string_view UniqueName = Meta::UniqueNameOf(^^T);

    } // namespace Traits

} // namespace Sora
