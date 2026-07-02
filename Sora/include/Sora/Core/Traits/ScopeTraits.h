#pragma once

#include <meta>

namespace Sora {

    namespace Meta {

        enum class ScopeType : uint8_t {
            Global,
            Namespace,
            Class,
            Struct,
            Union,
            Enum,
            Function,
            Variable,
            TypeAlias,
            TemplateParameter,
        };

        consteval std::vector<std::meta::info> ScopeChainOf(std::meta::info info) {
            std::vector<std::meta::info> chain;
            chain.push_back(info);
            auto parent = std::meta::parent_of(info);
            while (parent != ^^::) {
                chain.push_back(parent);
                parent = std::meta::parent_of(parent);
            }
            return chain;
        }

        consteval size_t ScopeDepthOf(std::meta::info info) {
            return ScopeChainOf(info).size();
        }

        consteval std::string_view ScopeChainIdentifierOf(std::meta::info info) {
            std::vector<std::meta::info> chain = ScopeChainOf(info);
            std::string desc;
            for (size_t i = chain.size(); i-- > 0;) {
                if (!desc.empty()) {
                    desc += ".";
                }
                if (std::meta::has_identifier(chain[i])) {
                    desc += std::meta::identifier_of(chain[i]);
                } else {
                    desc += std::meta::display_string_of(chain[i]);
                }
            }
            auto ret = std::define_static_string(desc);
            return std::string_view{ret, desc.size()};
        }

        consteval std::string_view ScopeChainDisplayStringOf(std::meta::info info) {
            std::vector<std::meta::info> chain = ScopeChainOf(info);
            std::string desc;
            for (size_t i = chain.size(); i-- > 0;) {
                if (!desc.empty()) {
                    desc += ".";
                }
                desc += std::meta::display_string_of(chain[i]);
            }
            auto ret = std::define_static_string(desc);
            return std::string_view{ret, desc.size()};
        }

        consteval bool IsInScope(std::meta::info info, std::meta::info scope) {
            std::vector<std::meta::info> chain = ScopeChainOf(info);
            for (auto& scope : chain) {
                if (scope == scope) {
                    return true;
                }
            }
            return false;
        }

    } // namespace Meta

    namespace Traits {

        template<typename T>
        inline constexpr size_t ScopeDepth = Meta::ScopeDepthOf(^^T);

        template<typename T>
        inline constexpr std::string_view ScopeChainIdentifier = Meta::ScopeChainIdentifierOf(^^T);

        template<typename T>
        inline constexpr std::string_view ScopeChainDisplayString = Meta::ScopeChainDisplayStringOf(^^T);

    } // namespace Traits

} // namespace Sora