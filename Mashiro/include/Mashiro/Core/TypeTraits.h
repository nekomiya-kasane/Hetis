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

#include <bit>
#include <concepts>
#include <cstddef>
#include <meta>
#include <new>
#include <type_traits>
#include <utility>
#include <variant>

namespace Mashiro {

    namespace Traits {

        /// @brief Static reflection array of @p T's non-static data members.
        template<typename T>
        inline constexpr auto Members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));

        /// @brief Number of non-static data members in @p T.
        template<typename T>
        inline constexpr size_t MembersCount = Members<T>.size();

        /// @brief Static reflection array of @p T's public non-static data members.
        template<typename T>
        inline constexpr auto PublicMembers = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));

        /// @brief Number of public non-static data members in @p T.
        template<typename T>
        inline constexpr size_t PublicMembersCount = PublicMembers<T>.size();

        namespace Detail {

            /// @brief Total byte size of all data members (may differ from sizeof due to padding).
            template<typename T>
            consteval size_t MemberBytesTotal() {
                size_t total = 0;
                for (auto m : Mashiro::Traits::Members<T>) {
                    total += std::meta::size_of(std::meta::type_of(m));
                }
                return total;
            }

        }

        /// @brief Total byte size of all data members (may differ from sizeof due to padding).
        template<typename T> requires std::is_class_v<T>
        inline constexpr size_t MemberBytesTotal = Detail::MemberBytesTotal<T>();
    
        /// @brief Padding bytes = sizeof(T) - sum of member sizes.
        template<typename T> requires std::is_class_v<T>
        inline constexpr size_t PaddingBytes = sizeof(T) - MemberBytesTotal<T>;

        /// @brief Compact class: all members are homogeneous and there's no padding.
        template<typename T>
        concept Compact = std::is_class_v<T> && MembersCount<T> > 0 && PaddingBytes<T> == 0;

        // clang-format off

        namespace Detail {

            /// @brief Compile-time check: every NSDM of @p T has the same type.
            template<typename T>
            consteval bool IsAllMemberHomogeneous() {
                if (MembersCount<T> == 0) {
                    return false;
                }
                auto first_type = type_of(Members<T>[0]);
                for (auto m : Members<T>) {
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

        // ==========================================================================
        // Enum Reflection
        // ==========================================================================

        /// @brief Static reflection array of @p T's enumerators.
        template<typename T>
        inline constexpr auto Enumerators = std::define_static_array(
            std::meta::enumerators_of(^^T));

        /// @brief Number of enumerators in @p T.
        template<typename T>
        inline constexpr size_t EnumeratorsCount = Enumerators<T>.size();

        namespace Detail {

            /// @brief Unsigned version of an enum's underlying type.
            template<typename E>
                requires std::is_enum_v<E>
            using UnsignedUnderlying = std::make_unsigned_t<std::underlying_type_t<E>>;

            /// @brief Compile-time check: every enumerator is 0 or a power of 2.
            template <typename E>
            consteval bool AllPowerOfTwo() {
                for (auto e : Enumerators<E>) {
                    auto v = static_cast<UnsignedUnderlying<E>>([:e:]);
                    if (v != 0 && !std::has_single_bit(v))
                        return false;
                }
                return true;
            }

            /// @brief Compile-time check: the enum values increases from 0 within the enum type
            template<typename T>
                requires std::is_enum_v<T>
            consteval bool IsEnumSequential() {
                if (EnumeratorsCount<T> == 0) {
                    return false;
                }
                for (size_t i = 0; i < EnumeratorsCount<T>; ++i) {
                    if (std::meta::constant_of(Enumerators<T>[i]) !=
                        static_cast<std::underlying_type_t<T>>(i)) {
                        return false;
                    }
                }
                return true;
            }

            /// @brief Compile-time OR of all enumerators — the valid-bit mask.
            template <typename E>
            consteval UnsignedUnderlying<E> AllBitsMask() {
                UnsignedUnderlying<E> mask{};
                for (auto e : Enumerators<E>) {
                    mask |= static_cast<UnsignedUnderlying<E>>([:e:]);
                }
                return mask;
            }

        } // namespace Detail

        /**
         * @brief A scoped enum whose enumerators form a contiguous sequence starting from 0.
         *
         * Validated at compile time via P2996 static reflection. Useful for
         * enum-to-index mapping and array indexing by enum value.
         */
        template<typename T>
        concept SequentialEnum = std::is_enum_v<T> && Detail::IsEnumSequential<T>();

        /**
         * @brief A scoped enum whose enumerators are all 0 or exact powers of two.
         *
         * Validated at compile time via P2996 static reflection. Any `enum class`
         * satisfying this concept automatically gets bitwise operators and query
         * functions — zero opt-in required.
         */
        template <typename E>
        concept BitfieldEnum = std::is_enum_v<E> && Detail::AllPowerOfTwo<E>();
        
        /// @brief All-valid-bits mask for a BitfieldEnum (OR of all enumerators).
        template <BitfieldEnum E>
        inline constexpr auto kBitfieldMask = static_cast<E>(Detail::AllBitsMask<E>());

    } // namespace Traits

    namespace Platform {

        /// @brief Cache line size for false-sharing avoidance (`alignas(kCacheLineSize)`).
#ifdef __cpp_lib_hardware_interference_size
        inline constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
        inline constexpr size_t kCacheLineSize = 64;
#endif

    } // namespace Platform

} // namespace Mashiro
