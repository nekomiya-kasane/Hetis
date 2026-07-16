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

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <string>
#include <string_view>
#include <vector>

#include "Sora/Core/Traits/TypeTraits.h"

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
         * @brief Return the parent scope of @p info.
         * @param[in] info Reflected declaration or type whose parent scope is requested.
         * @return Parent scope of @p info.
         */
        consteval std::meta::info ParentScopeOf(std::meta::info info) {
            return std::meta::parent_of(info);
        }

        /**
         * @brief Return the parent-scope chain for @p info.
         * @param[in] info Reflected declaration or type whose scope chain is requested.
         * @return Vector containing @p info followed by each parent scope up to, but not including, global scope.
         */
        consteval std::vector<std::meta::info> ScopeChainOf(std::meta::info info) {
            std::vector<std::meta::info> chain;
            chain.push_back(info);
            auto parent = ParentScopeOf(info);
            while (parent != ^^::) {
                chain.push_back(parent);
                parent = ParentScopeOf(parent);
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
        consteval std::string_view ScopeChainIdentifierOf(std::meta::info info, std::string_view delimiter = ".") {
            std::vector<std::meta::info> chain = ScopeChainOf(info);
            std::string desc;
            for (size_t i = chain.size(); i-- > 0;) {
                if (!desc.empty()) {
                    desc += delimiter;
                }
                desc += IdentifierOrDisplayStringOf(chain[i]);
            }
            auto ret = std::define_static_string(desc);
            return std::string_view{ret, desc.size()};
        }

        /**
         * @brief Return a dot-separated display-string path for @p info's scope chain.
         * @param[in] info Reflected declaration or type whose scope path is requested.
         * @return Static-storage string view containing reflection display strings.
         */
        consteval std::string_view ScopeChainDisplayStringOf(std::meta::info info, std::string_view delimiter = ".") {
            std::vector<std::meta::info> chain = ScopeChainOf(info);
            std::string desc;
            for (size_t i = chain.size(); i-- > 0;) {
                if (!desc.empty()) {
                    desc += delimiter;
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

        /**
         * @brief Return a static reflection array of members for a given scope.
         * @param[in] scope Reflected declaration or type whose members are requested.
         * @param[in] context Reflection access context used for member discovery.
         * @return Static reflection array of members for @p scope.
         */
        consteval auto MembersOf(std::meta::info scope,
                                 std::meta::access_context context = std::meta::access_context::unchecked()) {
            return std::define_static_array(std::meta::members_of(scope, context));
        }

        /**
         * @brief Return a filtered view of direct members for a given scope by name.
         * @param[in] scope Reflected declaration or type whose members are requested.
         * @param[in] name Name of the member to find.
         * @return Filtered view of direct members matching the given name.
         */
        consteval auto FindDirectMembersOf(std::meta::info scope, std::string_view name) {
            return Sora::Meta::MembersOf(scope) | std::views::filter([name](std::meta::info member) {
                       return std::meta::has_identifier(member) && std::meta::identifier_of(member) == name;
                   });
        }

        /**
         * @brief Return the first direct member for a given scope by name.
         * @param[in] scope Reflected declaration or type whose members are requested.
         * @param[in] name Name of the member to find.
         * @return First direct member matching the given name, or an empty reflection if not found.
         */
        consteval std::meta::info FindDirectTypeMemberOf(std::meta::info type, std::string_view name) {
            auto members = Sora::Meta::MembersOf(type);
            auto found = std::ranges::find_if(members, [name](std::meta::info member) {
                return std::meta::has_identifier(member) && std::meta::identifier_of(member) == name &&
                       std::meta::is_type(member);
            });
            return found == std::ranges::end(members) ? std::meta::info{} : std::meta::dealias(*found);
        }

        /** @brief Return whether reflection exposes every storage-bearing part of @p type's direct object layout. */
        consteval bool IsClassLayoutObservable(std::meta::info type) {
            type = std::meta::dealias(std::meta::remove_cvref(type));
            if (!std::meta::is_class_type(type) || std::meta::is_union_type(type) ||
                std::meta::is_polymorphic_type(type)) {
                return false;
            }
            return std::ranges::none_of(std::meta::bases_of(type, std::meta::access_context::unchecked()),
                                        std::meta::is_virtual);
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

        /** @brief Static reflection array of @p T's members using unchecked access. */
        template<typename T>
        inline constexpr auto Members = Meta::MembersOf(^^T);

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

        /**
         * @brief Return the number of uncovered bits in @p type's direct complete-object layout.
         * @details Direct base subobjects and non-static data members contribute bit intervals obtained from
         * @c offset_of and @c bit_size_of. Merging intervals correctly handles overlapping EBO and
         * @c [[no_unique_address]] storage, while bit-field widths expose sub-byte padding. Padding internal to a
         * direct subobject remains that subobject's responsibility and can be queried by applying this function
         * recursively.
         * @param[in] type Reflected non-polymorphic class type without direct virtual bases.
         * @return Number of object-representation bits not covered by a reflected direct subobject.
         */
        template<typename T>
            requires(std::is_class_v<T> && Meta::IsClassLayoutObservable(^^T))
        inline constexpr size_t PaddingBits = [] consteval {
            auto type = ^^T;
            type = std::meta::dealias(std::meta::remove_cvref(type));

            struct BitInterval {
                size_t begin{};
                size_t end{};
            };

            const size_t objectBits = std::meta::size_of(type) * CHAR_BIT;
            std::vector<BitInterval> intervals;
            auto append = [&intervals, objectBits](std::meta::info subobject, size_t width) {
                const auto offset = std::meta::offset_of(subobject).total_bits();
                if (offset < 0 || static_cast<size_t>(offset) > objectBits ||
                    width > objectBits - static_cast<size_t>(offset)) {
                    throw std::define_static_string(
                        "Meta::PaddingBitsOf received an invalid reflected subobject range.");
                }
                if (width != 0) {
                    intervals.push_back({static_cast<size_t>(offset), static_cast<size_t>(offset) + width});
                }
            };

            for (auto base : std::meta::bases_of(type, std::meta::access_context::unchecked())) {
                append(base, std::meta::size_of(std::meta::type_of(base)) * CHAR_BIT);
            }
            for (auto member : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked())) {
                append(member, std::meta::bit_size_of(member));
            }

            std::ranges::sort(intervals, {}, &BitInterval::begin);
            size_t paddingBits = 0;
            size_t coveredUntil = 0;
            for (const auto interval : intervals) {
                if (interval.begin > coveredUntil) {
                    paddingBits += interval.begin - coveredUntil;
                }
                coveredUntil = std::max(coveredUntil, interval.end);
            }
            return paddingBits + objectBits - coveredUntil;
        }();

    } // namespace Traits

    namespace Concept {

        /** @brief Class type whose observable direct complete-object layout can be reflected. */
        template<typename T>
        concept ClassWithObservableLayout = std::is_class_v<T> && Meta::IsClassLayoutObservable(^^T);

        /** @brief Class whose observable direct complete-object layout has no uncovered storage bits. */
        template<typename T>
        concept CompactClass = ClassWithObservableLayout<T> && Traits::PaddingBits<T> == 0;

        /** @brief Class whose observable direct complete-object layout contains uncovered storage bits. */
        template<typename T>
        concept PaddedClass = ClassWithObservableLayout<T> && Traits::PaddingBits<T> > 0;

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

        /** @brief Class type whose non-static data members have different dealiased declared types. */
        template<typename T>
        concept HeterogeneousClass = std::is_class_v<T> && Traits::DataMembersCount<T> > 0 && !HomogeneousClass<T>;

        /** @brief Class type whose non-static data members have different dealiased declared types. */
        template<typename T>
        concept StatefulClass = std::is_class_v<T> && Traits::DataMembersCount<T> > 0;

        /** @brief Class type with no non-static data members. */
        template<typename T>
        concept StatelessClass = std::is_class_v<T> && Traits::DataMembersCount<T> == 0;

    } // namespace Concept

} // namespace Sora
