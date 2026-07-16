/**
 * @file ScalarMath.h
 * @brief Scalar convenience algorithms layered on the unified mathematical primitive CPOs.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Constants.h>
#include <Sora/Math/PrimaryFunctions.h>

#include <cmath>
#include <concepts>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Sora::Math {

    /** @brief Compute @p n factorial, rejecting negative inputs and unrepresentable results. */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] constexpr T Factorial(T n) {
        if constexpr (std::signed_integral<T>) {
            if (n < 0) {
                throw std::domain_error("Factorial requires a non-negative integer");
            }
        }

        T result = 1;
        for (T factor = 2; factor <= n; ++factor) {
            if (result > std::numeric_limits<T>::max() / factor) {
                throw std::overflow_error("Factorial result is not representable");
            }
            result *= factor;
        }
        return result;
    }

    /** @brief Pair returned by @ref SinCos. */
    template<std::floating_point T>
    struct SinCosPair {
        T sin;
        T cos;
    };

    /** @brief Compute sine and cosine with the unified primitive semantics. */
    template<std::floating_point T>
    [[nodiscard]] constexpr SinCosPair<T> SinCos(T x) noexcept {
        return {Sin(x), Cos(x)};
    }

    /** @brief Copy the sign of @p sign onto the magnitude of @p magnitude. */
    template<std::floating_point T>
    [[nodiscard]] constexpr T CopySign(T magnitude, T sign) noexcept {
        if consteval {
            return Detail::CtCopySign(magnitude, sign);
        } else {
            return std::copysign(magnitude, sign);
        }
    }

    /** @brief Return the least value under the ordinary arithmetic ordering. */
    template<Concept::NumericScalar T, Concept::NumericScalar... Ts>
    [[nodiscard]] constexpr auto Min(T first, Ts... rest) noexcept {
        using R = std::common_type_t<T, Ts...>;
        R result = static_cast<R>(first);
        ((result = static_cast<R>(rest) < result ? static_cast<R>(rest) : result), ...);
        return result;
    }

    /** @brief Return the greatest value under the ordinary arithmetic ordering. */
    template<Concept::NumericScalar T, Concept::NumericScalar... Ts>
    [[nodiscard]] constexpr auto Max(T first, Ts... rest) noexcept {
        using R = std::common_type_t<T, Ts...>;
        R result = static_cast<R>(first);
        ((result = result < static_cast<R>(rest) ? static_cast<R>(rest) : result), ...);
        return result;
    }

    /** @brief Convert degrees to radians. */
    template<std::floating_point T>
    [[nodiscard]] constexpr T Radians(T degrees) noexcept {
        return degrees * Const::kDegToRad<T>;
    }

    /** @brief Convert radians to degrees. */
    template<std::floating_point T>
    [[nodiscard]] constexpr T Degrees(T radians) noexcept {
        return radians * Const::kRadToDeg<T>;
    }

} // namespace Sora::Math
