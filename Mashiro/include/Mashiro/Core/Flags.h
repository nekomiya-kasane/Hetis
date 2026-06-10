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

#include "Mashiro/Core/TypeTraits.h"

namespace Mashiro {

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

    namespace Detail {

        /// @brief Lightweight input range over the set bits of a bitfield enum.
        template <Traits::BitfieldEnum E>
        class FlagRange {
            using U = Traits::Detail::UnsignedUnderlying<E>;
            U bits_;

        public:
            constexpr explicit FlagRange(E flags) noexcept
                : bits_{static_cast<U>(flags)} {}

            struct Sentinel {};

            struct Iterator {
                using difference_type = std::ptrdiff_t;
                using value_type      = E;

                U bits;

                constexpr E operator*() const noexcept {
                    return static_cast<E>(bits & static_cast<U>(-bits));
                }
                constexpr Iterator& operator++() noexcept {
                    bits &= bits - 1; // clear lowest set bit
                    return *this;
                }
                constexpr void operator++(int) noexcept { ++*this; }
                friend constexpr bool operator==(Iterator it, Sentinel) noexcept {
                    return it.bits == 0;
                }
            };

            [[nodiscard]] constexpr Iterator begin() const noexcept { return {bits_}; }
            [[nodiscard]] constexpr Sentinel end()   const noexcept { return {}; }
        };

    } // namespace Detail

    /**
     * @brief Lazily yields each set flag bit as its enumerator, lowest first.
     *
     * @code
     * for (auto f : EachFlag(Access::Read | Access::Exec)) {  Read, Exec  }
     * @endcode
     */
    template <Traits::BitfieldEnum E>
    [[nodiscard]] constexpr Detail::FlagRange<E> EachFlag(E flags) noexcept {
        return Detail::FlagRange<E>{flags};
    }

}  // namespace Mashiro
