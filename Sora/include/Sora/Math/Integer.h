/**
 * @file Integer.h
 * @brief Shared unsigned-magnitude kernels for integral mathematical carriers.
 * @ingroup Math
 *
 * @details This header owns the policy-free kernels reused by scalar and SIMD math frontends. The public algorithms
 * are exposed through @ref Sora::Math::GCD and the number-theory facade; callers should not depend on this low-level
 * namespace directly.
 */
#pragma once

#include <concepts>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Sora::Math::Integer {

    /** @brief Unsigned type capable of representing every magnitude of @p T. */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    using Magnitude = std::make_unsigned_t<std::remove_cv_t<T>>;

    /** @brief Return the unsigned magnitude of @p value, including the most-negative signed value. */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] constexpr Magnitude<T> MagnitudeOf(T value) noexcept {
        using U = Magnitude<T>;
        const U bits = static_cast<U>(value);
        if constexpr (std::signed_integral<T>) {
            return value < 0 ? U{0} - bits : bits;
        } else {
            return bits;
        }
    }

    /** @brief Return whether @p value is negative without converting its magnitude. */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] constexpr bool IsNegative(T value) noexcept {
        if constexpr (std::signed_integral<T>) {
            return value < 0;
        } else {
            return false;
        }
    }

    /** @brief Return the non-negative GCD of two integral magnitudes. */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] constexpr Magnitude<T> GCD(T left, T right) noexcept {
        using U = Magnitude<T>;
        U remainder = MagnitudeOf(left);
        U divisor = MagnitudeOf(right);
        while (divisor != 0) {
            const U next = static_cast<U>(remainder % divisor);
            remainder = divisor;
            divisor = next;
        }
        return remainder;
    }

    /** @brief Return the checked least common multiple of two integral magnitudes. */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] constexpr Magnitude<T> LCM(T left, T right) {
        using U = Magnitude<T>;
        const U leftMagnitude = MagnitudeOf(left);
        const U rightMagnitude = MagnitudeOf(right);
        if (leftMagnitude == 0 || rightMagnitude == 0) {
            return U{0};
        }

        const U divisor = GCD(leftMagnitude, rightMagnitude);
        const U reducedLeft = static_cast<U>(leftMagnitude / divisor);
        if (reducedLeft > (std::numeric_limits<U>::max)() / rightMagnitude) {
            throw std::overflow_error("LCM result is not representable");
        }
        return static_cast<U>(reducedLeft * rightMagnitude);
    }

} // namespace Sora::Math::Integer
