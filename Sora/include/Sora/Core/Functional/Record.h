/**
 * @file Record.h
 * @brief Public-record reflection bridges for functional algorithms.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Traits.h>

#include <cstddef>
#include <functional>
#include <meta>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Concept {

        /**
         * @brief Public aggregate record eligible for reflection-driven functional operations.
         * @details A public record is an aggregate whose non-static data members are all visible, excluding ranges
         * and tuple-like types. Its field order is the declaration order.
         */
        template<typename T>
        concept PublicRecord = std::is_aggregate_v<std::remove_cvref_t<T>> &&
                               !Concept::TupleLikeClass<std::remove_cvref_t<T>> && !std::ranges::range<T> &&
                               Traits::PublicDataMembersCount<std::remove_cvref_t<T>> ==
                                   Traits::DataMembersCount<std::remove_cvref_t<T>>;

    } // namespace Concept

    namespace Detail {

        template<typename T, size_t... Indices>
        [[nodiscard]] constexpr auto RecordToTuple(T&& record, std::index_sequence<Indices...>) {
            using U = std::remove_cvref_t<T>;
            return std::tuple{std::forward<T>(record).[:Traits::PublicDataMembers<U>[Indices]:]...};
        }

        template<typename F, typename T, size_t... Indices>
        [[nodiscard]] constexpr auto MapRecord(F& function, T&& record, std::index_sequence<Indices...>) {
            using U = std::remove_cvref_t<T>;
            return U{std::invoke(function,
                                 std::forward<T>(record).[:Traits::PublicDataMembers<U>[Indices]:])...};
        }

    } // namespace Detail

    /**
     * @brief Copy or move a public record's fields into a value tuple.
     * @details Fields are reflected in declaration order. An lvalue record produces copied tuple elements; an rvalue
     * record may move them. The result owns its values independently of @p record.
     * @tparam T Public aggregate record type.
     * @param[in] record Record whose fields are extracted.
     * @return A value tuple containing every public field in declaration order.
     * @code{.cpp}
     * struct Point { int x; int y; };
     * auto input = Point{3, 4};
     * auto output = Sora::ToTuple(input); // (3, 4)
     * @endcode
     */
    template<Concept::PublicRecord T>
    [[nodiscard]] constexpr auto ToTuple(T&& record) {
        using U = std::remove_cvref_t<T>;
        return Detail::RecordToTuple(
            std::forward<T>(record), std::make_index_sequence<Traits::PublicDataMembersCount<U>>{});
    }

    /**
     * @brief Invoke @p function as @c function(name, value) for every public data member.
     * @details Members are visited in declaration order. The name is a compile-time-reflected
     * @c std::string_view and the value preserves the record's cv-ref category; no field metadata is allocated.
     * @tparam T Public aggregate record type.
     * @tparam F Callable accepting a member name and member value.
     * @param[in] record Record to inspect.
     * @param[in] function Visitor called once per public field.
     * @code{.cpp}
     * auto input = Point{3, 4};
     * Sora::ForEachField(input, [](std::string_view name, auto value) { std::println("{}={}", name, value); });
     * // output: x=3, y=4
     * @endcode
     */
    template<Concept::PublicRecord T, typename F>
    constexpr void ForEachField(T&& record, F&& function) {
        using U = std::remove_cvref_t<T>;
        template for (constexpr auto member : Traits::PublicDataMembers<U>) {
            std::invoke(function, std::meta::identifier_of(member), std::forward<T>(record).[:member:]);
        }
    }

} // namespace Sora
