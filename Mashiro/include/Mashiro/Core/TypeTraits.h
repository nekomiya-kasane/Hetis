#pragma once

#include <utility>
#include <variant>

namespace Mashiro::Traits {

    /**
     * @brief Test if a type has a tuple element at index N, and that the element is accessible via
     * std::get<N>.
     */
    template<typename T, size_t N>
    concept HasTupleElement = requires(T&& t) {
        typename std::tuple_element_t<N, std::remove_cvref_t<T>>;
        { std::get<N>(t) } -> std::convertible_to<const std::tuple_element_t<N, T>&>;
    };

    template<typename T>
    concept TupleLike = requires { typename std::tuple_size<std::remove_cvref_t<T>>::value_type; } &&
                        []<size_t... N>(std::index_sequence<N...>) {
                            return (HasTupleElement<T, N> && ...);
                        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

    /**
     * @brief Test if a type is a std::variant specialization (detected via std::variant_size, which
     * is only specialized for std::variant).
     */
    template<typename T>
    concept VariantLike = requires { std::variant_size<std::remove_cvref_t<T>>::value; };

    template<typename... Ts> struct Overload : Ts... { using Ts::operator()...; };
    template<typename... Ts> Overload(Ts...) -> Overload<Ts...>;

} // namespace Mashiro::Traits
