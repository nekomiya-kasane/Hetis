/**
 * @file ScalarUtils.h
 * @brief Compile-time-friendly classification utilities for arithmetic scalars.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Contracts.h>

#include <cmath>
#include <concepts>
#include <limits>
#include <type_traits>

namespace Sora::Math {

    /** @brief Signed or unsigned widened carrier used for checked integral accumulation. */
    template<std::integral T>
    using WideInteger = std::conditional_t<std::is_signed_v<T>, __int128_t, __uint128_t>;

    /** @brief Cast a widened integral result after enforcing that it is representable in @p T. */
    template<std::integral T>
    [[nodiscard]] constexpr T CheckedCast(WideInteger<T> value) noexcept {
        SORA_PRE(value >= static_cast<WideInteger<T>>((std::numeric_limits<T>::lowest)()));
        SORA_PRE(value <= static_cast<WideInteger<T>>((std::numeric_limits<T>::max)()));
        return static_cast<T>(value);
    }

    /** @brief Return whether @p value is a NaN. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool IsNaN(T value) noexcept {
        return value != value;
    }

    /** @brief Return false for an integral scalar, which has no NaN representation. */
    template<std::integral T>
        requires(!std::same_as<T, bool>)
    [[nodiscard]] constexpr bool IsNaN(T) noexcept {
        return false;
    }

    /** @brief Return whether @p value is not a NaN. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool IsNotNaN(T value) noexcept {
        return value == value;
    }

    /** @brief Return true for an integral scalar, which has no NaN representation. */
    template<std::integral T>
        requires(!std::same_as<T, bool>)
    [[nodiscard]] constexpr bool IsNotNaN(T) noexcept {
        return true;
    }

    /** @brief Return whether @p value is finite, including zero and subnormal values. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool IsFinite(T value) noexcept {
        const T maximum = (std::numeric_limits<T>::max)();
        return value == value && value >= -maximum && value <= maximum;
    }

    /** @brief Return whether @p value is positive or negative infinity. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool IsInfinite(T value) noexcept {
        if constexpr (std::numeric_limits<T>::has_infinity) {
            const T infinity = std::numeric_limits<T>::infinity();
            return value == infinity || value == -infinity;
        } else {
            return false;
        }
    }

    /** @brief Return whether @p value is a non-zero normal floating-point value. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool IsNormal(T value) noexcept {
        if (!IsFinite(value) || value == T{0}) {
            return false;
        }
        const T magnitude = value < T{0} ? -value : value;
        return magnitude >= (std::numeric_limits<T>::min)();
    }

    /** @brief Return whether @p value is a non-zero subnormal floating-point value. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool IsSubnormal(T value) noexcept {
        if (!IsFinite(value) || value == T{0}) {
            return false;
        }
        const T magnitude = value < T{0} ? -value : value;
        return magnitude < (std::numeric_limits<T>::min)();
    }

    /** @brief Return whether @p value is either positive or negative zero. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool IsZero(T value) noexcept {
        return value == T{0};
    }

} // namespace Sora::Math
