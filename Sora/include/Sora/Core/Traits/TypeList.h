/**
 * @file TypeList.h
 * @brief Type-pack boundary for Sora's reflection-native meta algebra.
 * @ingroup Core
 *
 * @details `TypeList<Ts...>` is intentionally a passive type-system projection. Reflection algorithms operate on
 * `Meta::InfoList`; this header owns only conversion between that value-level sequence and a C++ type parameter pack.
 * It deliberately contains no map, filter, fold, or set algorithms; those live in `MetaOperators.h` and share one
 * `InfoList` implementation.
 *
 * @par Example
 * @code{.cpp}
 * using Input = Sora::Traits::TypeList<int, double>;
 * constexpr auto infos = Sora::Meta::ToInfoList(^^Input);
 * constexpr auto reflected = Sora::Meta::ToTypeList(infos);
 * using RoundTrip = Sora::Meta::InfoType<reflected>;
 *
 * static_assert(std::same_as<RoundTrip, Input>);
 * static_assert(Sora::Traits::Length<Input> == 2);
 * static_assert(std::same_as<Sora::Traits::At<Input, 1>, double>);
 * @endcode
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <meta>
#include <ranges>
#include <type_traits>
#include <vector>

#include <Sora/Core/Traits/MetaInfo.h>
#include <Sora/Core/Traits/MetaInfoList.h>

namespace Sora {

    namespace Traits {

        /**
         * @brief Passive compile-time sequence of C++ types.
         * @tparam Types Ordered type pack represented by this specialization.
         *
         * @details Distinct packs have distinct C++ type identity and can be matched by partial specialization. The
         * type contains no state and introduces no runtime object or code.
         */
        template<typename... Types>
        struct TypeList {
            static constexpr std::size_t npos = static_cast<std::size_t>(-1); /**< Sentinel for a missing index. */
            static constexpr std::size_t size = sizeof...(Types);             /**< Number of represented types. */
            static constexpr bool empty = size == 0;                          /**< Whether the pack is empty. */

            /** @brief Return the type at zero-based @p Index. */
            template<std::size_t Index>
                requires(Index < size)
            using At = Types...[Index];
        };

    } // namespace Traits

    namespace Meta {

        /**
         * @brief Return whether @p info reflects a `Traits::TypeList` specialization.
         * @param[in] info Reflection to inspect; aliases are dealiased before matching.
         */
        [[nodiscard]] consteval bool IsTypeList(std::meta::info info) noexcept {
            info = std::meta::dealias(info);
            return std::meta::is_class_type(info) && std::meta::has_template_arguments(info) &&
                   std::meta::template_of(info) == ^^Traits::TypeList;
        }

        /**
         * @brief Return a stable InfoList view of a reflected TypeList's dealiased type arguments.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Static sequence preserving template-argument order.
         * @pre @p list reflects a `Traits::TypeList`; violation makes constant evaluation fail with a diagnostic.
         */
        [[nodiscard]] consteval InfoList ToInfoList(std::meta::info list) {
            list = std::meta::dealias(list);
            if (!IsTypeList(list)) {
                throw std::define_static_string("Meta::ToInfoList requires Traits::TypeList.");
            }
            auto types = std::meta::template_arguments_of(list) | std::views::transform(std::meta::dealias) |
                         std::ranges::to<std::vector>();
            return Promote(types);
        }

        /**
         * @brief Copy a reflected TypeList's dealiased arguments into a mutable vector.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Mutable compile-time vector preserving template-argument order.
         * @pre @p list reflects a `Traits::TypeList`; violation makes constant evaluation fail with a diagnostic.
         */
        [[nodiscard]] consteval std::vector<std::meta::info> CopyTypeInfos(std::meta::info list) {
            const auto infos = ToInfoList(list);
            return {infos.begin(), infos.end()};
        }

        /**
         * @brief Project a sequence of reflected types into a reflected `Traits::TypeList`.
         * @param[in] types Reflected types in template-argument order.
         * @return Reflection of `Traits::TypeList<Ts...>`.
         * @pre Every element reflects a type; violation makes constant evaluation fail with a diagnostic.
         */
        [[nodiscard]] consteval std::meta::info ToTypeList(const InfoList& types) {
            for (const auto type : types) {
                if (!std::meta::is_type(type)) {
                    throw std::define_static_string("Meta::ToTypeList requires only reflected types.");
                }
            }
            return std::meta::substitute(^^Traits::TypeList, types);
        }

        /**
         * @brief Project a compile-time info range into a reflected `Traits::TypeList`.
         * @param[in] types Sized compile-time range of type reflections.
         * @return Reflection of `Traits::TypeList<Ts...>` preserving range order.
         * @pre Every element reflects a type; violation makes constant evaluation fail with a diagnostic.
         */
        [[nodiscard]] consteval std::meta::info ToTypeList(auto&& types)
            requires std::ranges::sized_range<decltype(types)> &&
                     (!std::same_as<std::remove_cvref_t<decltype(types)>, InfoList>) &&
                     std::same_as<std::remove_cv_t<std::ranges::range_value_t<decltype(types)>>, std::meta::info>
        {
            return ToTypeList(Promote(types));
        }

    } // namespace Meta

    namespace Concept {

        /** @brief True when @p T is a `Traits::TypeList` specialization. */
        template<typename T>
        concept TypeList = Meta::IsTypeList(std::meta::dealias(^^std::remove_cvref_t<T>));

    } // namespace Concept

    namespace Traits {

        /** @brief Whether @p List is a `TypeList`. */
        template<typename List>
        inline constexpr bool IsTypeList = Concept::TypeList<List>;

        /** @brief Number of types in @p List. */
        template<typename List>
            requires Concept::TypeList<List>
        inline constexpr std::size_t Length = std::remove_cvref_t<List>::size;

        /** @brief Return the @p Index-th type of @p List. */
        template<typename List, std::size_t Index>
            requires(Concept::TypeList<List> && Index < Length<List>)
        using At = typename std::remove_cvref_t<List>::template At<Index>;

        /** @brief First type of a non-empty @p List. */
        template<typename List>
            requires(Concept::TypeList<List> && Length<List> > 0)
        using Head = At<List, 0>;

        /** @brief Final type of a non-empty @p List. */
        template<typename List>
            requires(Concept::TypeList<List> && Length<List> > 0)
        using Last = At<List, Length<List> - 1>;

    } // namespace Traits

} // namespace Sora
