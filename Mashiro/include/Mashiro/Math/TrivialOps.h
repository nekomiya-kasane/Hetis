/**
 * @file TrivialOps.h
 * @brief Tiny `constexpr` integer-and-bit utilities used across Mashiro.
 *
 * These are the small, well-known operations that show up everywhere — power-of-two queries,
 * ceiling division, alignment rounding, byte-count humanisation. Every function is a single
 * expression, `constexpr noexcept`, and works on any unsigned-integral input. Keeping them here
 * gives the codebase one canonical name per operation, instead of a half-dozen ad-hoc copies
 * sprinkled through allocators, packers, and bit-twiddling utilities.
 *
 * @section categories Categories
 * - **Power of two**: @ref CeilPow2 / @ref FloorPow2 / @ref IsPow2 / @ref Log2Floor / @ref Log2Ceil.
 * - **Division and rounding**: @ref CeilDiv / @ref RoundUp / @ref RoundDown — integer math that
 *   the standard library deliberately omits and that is widely re-derived from scratch.
 * - **Alignment**: @ref AlignUp / @ref AlignDown / @ref IsAlignedTo. Distinct from @c RoundUp/Down
 *   in name only — alignment is the half of the API that takes "alignment must be a power of two"
 *   as a precondition, hence it can use a faster bit-mask path.
 * - **Bit width**: @ref BitWidth / @ref ByteWidth — the smallest number of bits/bytes needed to
 *   represent a non-negative @c v (0 maps to 0, 1 to 1, 256 to 9, etc.).
 *
 * @section design Design
 * Every helper takes its argument by value and returns by value. Arguments are constrained on
 * @c std::unsigned_integral so accidental signed inputs do not silently pass under one of the
 * sign-conversion warning levels in the project, and so the bit-twiddling math is well-defined
 * (no UB on shift-by-bit-width, no implementation-defined behaviour on negative shift). Where the
 * standard library already provides the right primitive (@c std::bit_ceil, @c std::popcount), we
 * call it directly; the helpers here only fill the *gaps* in @c <bit> and @c <numeric>.
 *
 * @ingroup Math
 */
#pragma once

#include <bit>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Mashiro::Math {

    // =========================================================================
    // Section 1 — Power of two
    // =========================================================================

    /// @brief @c true when @p v has exactly one bit set (a positive power of two). Zero is not.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr bool IsPow2(T v) noexcept {
        return v != 0 && (v & (v - 1)) == 0;
    }

    /**
     * @brief Round @p v up to the smallest power of two `>= v`. Zero and one map to one.
     *
     * Wraps @c std::bit_ceil but normalises the @c 0 input to @c 1 so callers can use the result
     * as a multiplier or shift base without a special case at the boundary.
     */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T CeilPow2(T v) noexcept {
        return v <= T{1} ? T{1} : std::bit_ceil(v);
    }

    /// @brief Round @p v down to the largest power of two `<= v`. Zero maps to zero.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T FloorPow2(T v) noexcept {
        return v == 0 ? T{0} : std::bit_floor(v);
    }

    /**
     * @brief @c floor(log2(v)) for @c v > 0. The result for @c v == 0 is 0; the function is total
     *        but the v=0 result is a convention, not a true mathematical answer.
     */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr int Log2Floor(T v) noexcept {
        return v == 0 ? 0 : (sizeof(T) * CHAR_BIT - 1 - std::countl_zero(v));
    }

    /// @brief @c ceil(log2(v)) for @c v > 0. The result for @c v == 0 or @c v == 1 is 0.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr int Log2Ceil(T v) noexcept {
        return v <= T{1} ? 0 : Log2Floor(static_cast<T>(v - 1)) + 1;
    }

    // =========================================================================
    // Section 2 — Division and rounding (general modulus)
    // =========================================================================

    /// @brief Ceiling integer division: @c ceil(num / den). Precondition: @c den > 0.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T CeilDiv(T num, T den) noexcept {
        return (num + den - T{1}) / den;
    }

    /// @brief Smallest multiple of @p step that is `>= v`. @p step must be non-zero.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T RoundUp(T v, T step) noexcept {
        return CeilDiv(v, step) * step;
    }

    /// @brief Largest multiple of @p step that is `<= v`. @p step must be non-zero.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T RoundDown(T v, T step) noexcept {
        return (v / step) * step;
    }

    // =========================================================================
    // Section 3 — Alignment (power-of-two modulus)
    // =========================================================================

    /**
     * @brief Round @p v up to the next multiple of @p alignment. @p alignment must be a power of
     *        two; passing a non-power-of-two is a programming error.
     */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T AlignUp(T v, T alignment) noexcept {
        // Precondition: IsPow2(alignment). Encoded as a contract in callers; here we only do math.
        const T mask = alignment - T{1};
        return (v + mask) & ~mask;
    }

    /// @brief Round @p v down to the previous multiple of @p alignment. @p alignment must be a
    ///        power of two.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T AlignDown(T v, T alignment) noexcept {
        const T mask = alignment - T{1};
        return v & ~mask;
    }

    /// @brief @c true when @p v is a multiple of @p alignment. @p alignment must be a power of two.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr bool IsAlignedTo(T v, T alignment) noexcept {
        const T mask = alignment - T{1};
        return (v & mask) == 0;
    }

    // =========================================================================
    // Section 4 — Width queries
    // =========================================================================

    /// @brief Smallest number of bits needed to represent @p v. @c BitWidth(0) == 0,
    ///        @c BitWidth(1) == 1, @c BitWidth(255) == 8, @c BitWidth(256) == 9.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr int BitWidth(T v) noexcept {
        return static_cast<int>(std::bit_width(v));
    }

    /// @brief Smallest number of bytes needed to hold @p v. @c ByteWidth(0) == 0,
    ///        @c ByteWidth(1) == 1, @c ByteWidth(256) == 2.
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr int ByteWidth(T v) noexcept {
        return v == 0 ? 0 : static_cast<int>((BitWidth(v) + 7) / 8);
    }

} // namespace Mashiro::Math
