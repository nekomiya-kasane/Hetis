/**
 * @file Flags.h
 * @brief Zero-boilerplate bitfield support for scoped enumerations via C++26 reflection.
 *
 * Any `enum class` whose enumerators are all either 0 or exact powers of two automatically 
 * satisfies the `BitfieldEnum` concept. For such enums this header provides:
 *
 * - **Bitwise operators** (`|`, `&`, `^`, `~`, `|=`, `&=`, `^=`) that return the
 *   enum type itself — no wrapper class, no macros, no opt-in boilerplate.
 * - **Query functions**: `HasFlag`, `HasAny`, `HasAll`, `IsEmpty`, `PopCount`.
 * - **Mutation functions**: `SetFlag`, `ClearFlag`, `ToggleFlag`.
 * - **Iteration**: `EachFlag(flags)` returns a `std::generator<E>` that yields
 *   each set bit as its enumerator, lowest first.
 * - **Compile-time safety**: `BitfieldEnum` is validated at compile time via
 *   P2996 reflection — a non-power-of-2 enumerator is a hard error.
 * - **Masked complement**: `operator~` only flips bits within the valid flag
 *   space (OR of all enumerators), never sets high garbage bits.
 *
 * @ingroup Core
 */
#pragma once

#include <bit>
#include <concepts>
#include <generator>
#include <meta>
#include <type_traits>

namespace Mashiro {

    namespace Traits {

        /** @cond INTERNAL */
        namespace Detail {

            /// @brief Unsigned version of an enum's underlying type.
            template <typename E> requires std::is_enum_v<E>
            using UnsignedUnderlying = std::make_unsigned_t<std::underlying_type_t<E>>;

            /// @brief Compile-time check: every enumerator is 0 or a power of 2.
            template <typename E>
            consteval bool AllPowerOfTwo() {
                for (auto e : std::meta::enumerators_of(^^E)) {
                    auto v = static_cast<UnsignedUnderlying<E>>([:e:]);
                    if (v != 0 && !std::has_single_bit(v))
                        return false;
                }
                return true;
            }

            /// @brief Compile-time OR of all enumerators — the valid-bit mask.
            template <typename E>
            consteval UnsignedUnderlying<E> AllBitsMask() {
                UnsignedUnderlying<E> mask{};
                for (auto e : std::meta::enumerators_of(^^E)) {
                    mask |= static_cast<UnsignedUnderlying<E>>([:e:]);
                }
                return mask;
            }

        } // namespace Detail
        /** @endcond */

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
        inline constexpr auto BitfieldMask = static_cast<E>(Detail::AllBitsMask<E>());

    }

    // =========================================================================
    // Bitwise operators — return E, not a wrapper
    // =========================================================================

    /// @name Bitwise operators for BitfieldEnum
    /// @{

    /// @brief Bitwise OR.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr E operator|(E a, E b) noexcept {
        using U = Traits::Detail::UnsignedUnderlying<E>;
        return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
    }

    /// @brief Bitwise AND.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr E operator&(E a, E b) noexcept {
        using U = Traits::Detail::UnsignedUnderlying<E>;
        return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
    }

    /// @brief Bitwise XOR.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr E operator^(E a, E b) noexcept {
        using U = Traits::Detail::UnsignedUnderlying<E>;
        return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));
    }

    /// @brief Masked complement — only flips bits within the valid flag space.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr E operator~(E a) noexcept {
        using U = Traits::Detail::UnsignedUnderlying<E>;
        return static_cast<E>(~static_cast<U>(a) & Traits::Detail::AllBitsMask<E>());
    }

    /// @brief Compound OR.
    template <Traits::BitfieldEnum E>
    constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; }

    /// @brief Compound AND.
    template <Traits::BitfieldEnum E>
    constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }

    /// @brief Compound XOR.
    template <Traits::BitfieldEnum E>
    constexpr E& operator^=(E& a, E b) noexcept { return a = a ^ b; }

    /// @}

    // =========================================================================
    // Query functions
    // =========================================================================

    /// @name Bitfield queries
    /// @{

    /// @brief Test whether all bits of @p flag are set in @p set.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr bool HasFlag(E set, E flag) noexcept {
        using U = Traits::Detail::UnsignedUnderlying<E>;
        auto f = static_cast<U>(flag);
        return f == 0 ? static_cast<U>(set) == 0 : (static_cast<U>(set) & f) == f;
    }

    /// @brief Test whether *any* bit of @p test is set in @p set.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr bool HasAny(E set, E test) noexcept {
        using U = Traits::Detail::UnsignedUnderlying<E>;
        return (static_cast<U>(set) & static_cast<U>(test)) != 0;
    }

    /// @brief Test whether *all* bits of @p test are set in @p set.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr bool HasAll(E set, E test) noexcept {
        using U = Traits::Detail::UnsignedUnderlying<E>;
        return (static_cast<U>(set) & static_cast<U>(test)) == static_cast<U>(test);
    }

    /// @brief True if no bits are set.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr bool IsEmpty(E e) noexcept {
        return static_cast<Traits::Detail::UnsignedUnderlying<E>>(e) == 0;
    }

    /// @brief Number of set bits.
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr int PopCount(E e) noexcept {
        return std::popcount(static_cast<Traits::Detail::UnsignedUnderlying<E>>(e));
    }

    /// @}

    // =========================================================================
    // Mutation functions
    // =========================================================================

    /// @name Bitfield mutation
    /// @{

    /// @brief Set flag bit(s).
    template <Traits::BitfieldEnum E>
    constexpr E& SetFlag(E& set, E flag) noexcept { return set |= flag; }

    /// @brief Clear flag bit(s).
    template <Traits::BitfieldEnum E>
    constexpr E& ClearFlag(E& set, E flag) noexcept { return set = set & ~flag; }

    /// @brief Toggle flag bit(s).
    template <Traits::BitfieldEnum E>
    constexpr E& ToggleFlag(E& set, E flag) noexcept { return set ^= flag; }

    /// @}

    // =========================================================================
    // Iteration — yields each set bit as its enumerator (lowest first)
    // =========================================================================

    /**
     * @brief Lazily yields each set flag bit as its enumerator, lowest first.
     *
     * @code
     * for (auto f : EachFlag(Access::Read | Access::Exec)) {  Read, Exec  }
     * @endcode
     */
    template <Traits::BitfieldEnum E>
    std::generator<E> EachFlag(E flags) {
        auto bits = static_cast<Traits::Detail::UnsignedUnderlying<E>>(flags);
        while (bits != 0) {
            auto lowest = bits & static_cast<decltype(bits)>(-bits);
            co_yield static_cast<E>(lowest);
            bits &= ~lowest;
        }
    }

}  // namespace Mashiro
