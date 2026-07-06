/**
 * @file InheritanceTraits.h
 * @brief Reflection-based inheritance traits and inheritance-chain metadata.
 * @ingroup Core
 *
 * @details Provides C++26 static-reflection helpers for direct base discovery, recursive base discovery, and simple
 * inheritance-chain descriptions. The facilities are intended for compile-time queries and expose their results through
 * @c consteval functions, concepts, and @c inline @c constexpr variable templates.
 */
#pragma once

#include <cstddef>
#include <meta>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Sora {

    namespace Meta {

        /**
         * @brief Return direct base types of a reflected class type.
         * @param[in] type Reflection of a class type.
         * @param[in] context Reflection access context used for base-specifier discovery.
         * @return Vector of type reflections for every direct base in declaration order.
         */
        consteval std::vector<std::meta::info>
        DirectBaseTypesOf(std::meta::info type,
                          std::meta::access_context context = std::meta::access_context::unchecked()) {
            std::vector<std::meta::info> bases;
            for (auto base : std::meta::bases_of(type, context)) {
                bases.push_back(std::meta::type_of(base));
            }
            return bases;
        }

        /**
         * @brief Return all recursively reachable base types of a reflected class type.
         * @param[in] type Reflection of a class type.
         * @param[in] context Reflection access context used for base-specifier discovery.
         * @return Vector of type reflections in depth-first direct-base order.
         */
        consteval std::vector<std::meta::info>
        RecursiveBaseTypesOf(std::meta::info type,
                             std::meta::access_context context = std::meta::access_context::unchecked()) {
            std::vector<std::meta::info> bases;
            auto directBaseTypes = DirectBaseTypesOf(type, context);
            for (auto base : directBaseTypes) {
                bases.push_back(base);
                auto recursiveBases = RecursiveBaseTypesOf(base, context);
                bases.insert_range(bases.end(), recursiveBases);
            }
            return bases;
        }

        /**
         * @brief Walk the class @p root's reflection hierarchy in derived-first order, invoking @p visit for each
         * (depth, member) pair across @p root and every base class. Visits each unique base class exactly once (handles
         * diamond inheritance and virtual bases) — duplicates are filtered by reflection-info equality of the base
         * class type.
         *
         * @tparam Visit Callable as @c void(size_t depth, std::meta::info member). Depth is 0 for members declared in
         * @p root, 1 for direct bases, etc.
         */
        template<typename Visit>
        consteval void WalkHierarchyMembers(std::meta::info root, Visit&& visit) {
            std::vector<std::meta::info> stack{root};
            std::vector<std::meta::info> seen{root};
            for (size_t i = 0; i < stack.size(); ++i) {
                const size_t depth = i;
                for (auto m : std::meta::members_of(stack[i], std::meta::access_context::unchecked())) {
                    visit(depth, m);
                }
                for (auto b : std::meta::bases_of(stack[i], std::meta::access_context::unchecked())) {
                    const auto baseType = std::meta::type_of(b);
                    bool already = false;
                    for (auto s : seen) {
                        if (s == baseType) {
                            already = true;
                            break;
                        }
                    }
                    if (!already) {
                        stack.push_back(baseType);
                        seen.push_back(baseType);
                    }
                }
            }
        }

    } // namespace Meta

    namespace Traits {

        /** @brief Number of direct bases of @p T. */
        template<typename T>
        inline constexpr std::size_t DirectBasesCount = Meta::DirectBaseTypesOf(^^T).size();

        /** @brief Number of recursively reachable bases of @p T. */
        template<typename T>
        inline constexpr std::size_t RecursiveBasesCount = Meta::RecursiveBaseTypesOf(^^T).size();

        /**
         * @brief Type of @p T's @p I-th direct base.
         * @tparam T Reflectable class type.
         * @tparam I Zero-based direct-base index. Must be less than @ref DirectBasesCount for @p T.
         */
        template<typename T, std::size_t I>
            requires std::is_class_v<T>
        using DirectBaseType = typename [:[] consteval {
            auto bases = Meta::DirectBaseTypesOf(^^T);
            return bases.size() ? bases[I] : ^^void;
        }():];

    } // namespace Traits

    namespace Concept {

        /** @brief Class type with exactly one direct base. */
        template<typename T>
        concept SingleInheritedClass = std::is_class_v<T> && Traits::DirectBasesCount<T> == 1;

        /** @brief Class type with more than one direct base. */
        template<typename T>
        concept MultiInheritedClass = std::is_class_v<T> && Traits::DirectBasesCount<T> > 1;

        /** @brief Class type with no direct base. */
        template<typename T>
        concept RootClass = std::is_class_v<T> && Traits::DirectBasesCount<T> == 0;

    } // namespace Concept

    namespace Meta {

        /**
         * @brief Return the single-inheritance chain containing @p type.
         * @param[in] type Reflection of a class type.
         * @return Vector containing @p type followed by each direct base in chain order.
         * @throws std::logic_error during constant evaluation when a type in the chain has multiple direct bases.
         */
        consteval std::vector<std::meta::info> InheritanceChainOf(std::meta::info type) {
            std::vector<std::meta::info> chain;
            while (true) {
                chain.push_back(type);
                auto bases = DirectBaseTypesOf(type, std::meta::access_context::unchecked());
                if (bases.empty()) {
                    break;
                }
                if (bases.size() > 1) {
                    throw std::logic_error("Type does not have a single inheritance chain");
                }
                type = bases[0];
            }
            return chain;
        }

    } // namespace Meta

    namespace Concept {

        /** @brief Class type whose bases can be represented as a single inheritance chain. */
        template<typename T>
        concept SingleInheritanceChainClass = std::is_class_v<T> && Meta::InheritanceChainOf(^^T).size() > 0;

    } // namespace Concept

    namespace Traits {

        /** @brief Number of types in @p T's single-inheritance chain, including @p T itself. */
        template<Concept::SingleInheritanceChainClass T>
        inline constexpr std::size_t InheritanceDepth = Meta::InheritanceChainOf(^^T).size();

        /** @brief Dot-separated reflection identifiers for @p T's single-inheritance chain. */
        template<Concept::SingleInheritanceChainClass T>
        inline constexpr std::string_view InheritanceChainIdentifier = [] consteval {
            std::vector<std::meta::info> chain = Meta::InheritanceChainOf(^^T);
            std::string desc;
            for (auto& type : chain) {
                if (!desc.empty()) {
                    desc += ".";
                }
                desc += std::meta::identifier_of(type);
            }
            auto ret = std::define_static_array(desc);
            return std::string_view{ret.data(), ret.size()};
        }();

        /** @brief Dot-separated reflection display strings for @p T's single-inheritance chain. */
        template<Concept::SingleInheritanceChainClass T>
        inline constexpr std::string_view InheritanceChainDisplayString = [] consteval {
            std::vector<std::meta::info> chain = Meta::InheritanceChainOf(^^T);
            std::string desc;
            for (auto& type : chain) {
                if (!desc.empty()) {
                    desc += ".";
                }
                desc += std::meta::display_string_of(type);
            }
            auto ret = std::define_static_array(desc);
            return std::string_view{ret.data(), ret.size()};
        }();

    } // namespace Traits

} // namespace Sora
