/**
 * @file ScopeTraits.h
 * @brief Reflection-based lexical scope traits and scope-chain metadata.
 * @ingroup Core
 *
 * @details Provides C++26 static-reflection helpers for walking an entity's parent scopes and materializing stable
 * identifier or display-string descriptions of that scope chain. The public traits expose the same metadata for C++
 * types through @c inline @c constexpr variable templates.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <meta>
#include <string>
#include <string_view>
#include <vector>

namespace Sora {

    namespace Meta {

        /** @brief Coarse lexical-scope category used by scope-oriented reflection utilities. */
        enum class ScopeType : std::uint8_t {
            Global,            /**< Global namespace scope. */
            Namespace,         /**< Named or inline namespace scope. */
            Class,             /**< Class scope. */
            Struct,            /**< Struct scope. */
            Union,             /**< Union scope. */
            Enum,              /**< Enumeration scope. */
            Function,          /**< Function or function-template scope. */
            Variable,          /**< Variable declaration scope. */
            TypeAlias,         /**< Type alias declaration scope. */
            TemplateParameter, /**< Template-parameter scope. */
        };

        /**
         * @brief Return the parent-scope chain for @p info.
         * @param[in] info Reflected declaration or type whose scope chain is requested.
         * @return Vector containing @p info followed by each parent scope up to, but not including, global scope.
         */
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

        /**
         * @brief Return the number of entries in @p info's scope chain.
         * @param[in] info Reflected declaration or type whose scope depth is requested.
         * @return Scope-chain entry count, including @p info itself.
         */
        consteval size_t ScopeDepthOf(std::meta::info info) {
            return ScopeChainOf(info).size();
        }

        /**
         * @brief Return a dot-separated identifier path for @p info's scope chain.
         * @param[in] info Reflected declaration or type whose scope path is requested.
         * @return Static-storage string view containing identifiers where available and display strings otherwise.
         */
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

        /**
         * @brief Return a dot-separated display-string path for @p info's scope chain.
         * @param[in] info Reflected declaration or type whose scope path is requested.
         * @return Static-storage string view containing reflection display strings.
         */
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

        /**
         * @brief Return whether @p info appears under @p scope in the reflected parent chain.
         * @param[in] info Reflected declaration or type to inspect.
         * @param[in] scope Candidate ancestor scope.
         * @return True when @p scope is found in @p info's scope chain; otherwise false.
         */
        consteval bool IsInScope(std::meta::info info, std::meta::info scope) {
            std::vector<std::meta::info> chain = ScopeChainOf(info);
            for (auto& s : chain) {
                if (s == scope) {
                    return true;
                }
            }
            return false;
        }

    } // namespace Meta

    namespace Traits {

        /** @brief Scope-chain depth for reflected type @p T, including @p T itself. */
        template<typename T>
        inline constexpr size_t ScopeDepth = Meta::ScopeDepthOf(^^T);

        /** @brief Dot-separated identifier path for reflected type @p T's scope chain. */
        template<typename T>
        inline constexpr std::string_view ScopeChainIdentifier = Meta::ScopeChainIdentifierOf(^^T);

        /** @brief Dot-separated display-string path for reflected type @p T's scope chain. */
        template<typename T>
        inline constexpr std::string_view ScopeChainDisplayString = Meta::ScopeChainDisplayStringOf(^^T);

    } // namespace Traits

} // namespace Sora
