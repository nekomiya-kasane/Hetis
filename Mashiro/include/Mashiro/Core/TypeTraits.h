/**
 * @file TypeTraits.h
 * @brief Reflection-based type traits, structural concepts, and utility types.
 *
 * Provides compile-time introspection helpers built on C++26 static reflection
 * (`<meta>`) and standard concepts. Key facilities:
 * - `Homogeneous<T>` — all non-static data members share the same type.
 * - `TupleLike<T>` / `VariantLike<T>` — structural duck-type detection.
 * - `Overload` — lambda-overload-set builder for `std::visit`.
 *
 * @ingroup Core
 */
#pragma once

#include <utility>
#include <variant>
#include <meta>

namespace Mashiro::Traits {

    // clang-format off

    namespace Detail {

        /// @brief Compile-time check: every NSDM of @p T has the same type.
        template<typename T>
        consteval bool IsAllMemberHomogeneous() {
            auto members =
                std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked());
            if (members.size() == 0) {
                return false;
            }
            auto first_type = type_of(members[0]);
            for (auto m : members) {
                if (type_of(m) != first_type) {
                    return false;
                }
            }
            return true;
        }

    } // namespace Detail

    /**
     * @brief Concept: all non-static data members of @p T share the same type.
     *
     * Requires @p T to be a class with at least one NSDM, and uses P2996
     * reflection to verify type homogeneity.
     */
    template<typename T>
    concept Homogeneous = std::is_class_v<T> && Detail::IsAllMemberHomogeneous<T>();

    /// @brief Number of non-static data members in a Homogeneous type.
    template<Homogeneous T>
    consteval size_t GetHomogeneousMemberCount() {
        return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())
            .size();
    }

    // clang-format on

    /// @name Tuple / variant structural concepts
    /// @{

    /**
     * @brief True if `std::get<N>(t)` is valid and returns the expected element type.
     * @tparam T Type to probe.
     * @tparam N Zero-based element index.
     */
    template<typename T, size_t N>
    concept HasTupleElement = requires(T&& t) {
        typename std::tuple_element_t<N, std::remove_cvref_t<T>>;
        { std::get<N>(t) } -> std::convertible_to<const std::tuple_element_t<N, T>&>;
    };

    /// @brief A type that provides `std::tuple_size` and per-index `std::get`.
    template<typename T>
    concept TupleLike = requires {
        typename std::tuple_size<std::remove_cvref_t<T>>::value_type;
    } && []<size_t... N>(std::index_sequence<N...>) {
        return (HasTupleElement<T, N> && ...);
    }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

    /**
     * @brief Detected `std::variant` specialisation.
     *
     * Uses `std::variant_size`, which is only specialised for `std::variant`.
     */
    template<typename T>
    concept VariantLike = requires { std::variant_size<std::remove_cvref_t<T>>::value; };

    /// @}

    /**
     * @brief Overload-set builder for `std::visit` and general dispatch.
     *
     * Inherits `operator()` from each callable in @p Ts, producing a single
     * callable object. The CTAD guide allows direct brace-list construction.
     *
     * @tparam Ts Callable types (typically lambdas).
     *
     * @code
     * std::variant<int, std::string, float> v = std::string{"hi"};
     * auto label = std::visit(Overload{
     *     [](int i)                { return "int:"   + std::to_string(i); },
     *     [](const std::string& s) { return "str:"   + s; },
     *     [](auto&&)               { return std::string{"other"}; },
     * }, v);
     * // label == "str:hi"
     * @endcode
     */
    template<typename... Ts>
    struct Overload : Ts... {
        using Ts::operator()...;
    };
    /// @brief CTAD guide — deduce @c Ts from constructor arguments.
    template<typename... Ts>
    Overload(Ts...) -> Overload<Ts...>;

    namespace Detail {

        /// @brief Compile-time check: the enum values increases from 0 within the enum type
        template<typename T>
            requires std::is_enum_v<T>
        consteval bool IsEnumSequential() {
            auto enumerators = std::meta::enumerators_of(^^T);
            if (enumerators.size() == 0) {
                return false;
            }
            for (size_t i = 0; i < enumerators.size(); ++i) {
                if (std::meta::value_of(enumerators[i]) !=
                    static_cast<std::underlying_type_t<T>>(i)) {
                    return false;
                }
            }
            return true;
        }

        /// @brief Compile-time check: the enum type is a flags enum
        template<typename T>
            requires std::is_enum_v<T>
        consteval bool IsEnumFlags() {
            auto enumerators = std::meta::enumerators_of(^^T);
            if (enumerators.size() == 0) {
                return false;
            }
            for (size_t i = 0; i < enumerators.size(); ++i) {
                if (std::meta::value_of(enumerators[i]) !=
                    static_cast<std::underlying_type_t<T>>(1 << i)) {
                    return false;
                }
            }
            return true;
        }

        /// @brief Compile-time getter for the number of enumerators in an enum type
        template<typename T>
            requires std::is_enum_v<T>
        consteval size_t GetEnumEnumeratorCount() {
            return std::meta::enumerators_of(^^T).size();
        }

    } // namespace Detail

    template<typename T> requires std::is_enum_v<T>
    concept SequentialEnum = Detail::IsEnumSequential<T>();
    
    template<typename T> requires std::is_enum_v<T>
    concept FlagLikeEnum = Detail::IsEnumFlags<T>();

    template<typename T>
    inline constexpr size_t EnumEnumeratorsCount = Detail::GetEnumEnumeratorCount<T>();

} // namespace Mashiro::Traits
