/**
 * @file NumberTheory.h
 * @brief Compile-time-friendly integer number-theory algorithms.
 * @ingroup Math
 *
 * @details GCD is the shared scalar/SIMD frontend from @ref PrimaryFunctions.h. The scalar-only algorithms below
 * return unsigned magnitudes so that the absolute value of the most-negative signed input remains representable.
 * LCM reports arithmetic overflow with @c std::overflow_error; domain failures use @c std::domain_error.
 */
#pragma once

#include <Sora/Core/Int128.h>
#include <Sora/Math/Integer.h>
#include <Sora/Math/PrimaryFunctions.h>

#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Sora::Math {

    /**
     * @brief Result of the extended Euclidean algorithm.
     * @tparam T Integral operand type used for the input and GCD magnitude.
     */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool> && sizeof(std::remove_cv_t<T>) <= sizeof(std::uint64_t))
    struct ExtendedGCDResult {
        using Magnitude = Integer::Magnitude<T>;

        Magnitude gcd;
        Sora::int128_t coefficientLeft;
        Sora::int128_t coefficientRight;
    };

    /**
     * @brief Compute a GCD and Bézout coefficients for two integral values.
     * @details The returned values satisfy @c left*coefficientLeft + right*coefficientRight == gcd. Coefficients
     * use Sora's signed 128-bit type and inputs are limited to at most 64 bits.
     * @tparam T Integral operand type no wider than 64 bits, excluding @c bool.
     * @param[in] left First operand.
     * @param[in] right Second operand.
     * @return GCD magnitude and coefficients, with a non-negative GCD.
     */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool> && sizeof(std::remove_cv_t<T>) <= sizeof(std::uint64_t))
    [[nodiscard]] constexpr ExtendedGCDResult<T> ExtendedGCD(T left, T right) noexcept {
        using U = Integer::Magnitude<T>;
        Sora::int128_t oldR = static_cast<Sora::int128_t>(Integer::MagnitudeOf(left));
        Sora::int128_t remainder = static_cast<Sora::int128_t>(Integer::MagnitudeOf(right));
        Sora::int128_t oldS = 1;
        Sora::int128_t coefficientS = 0;
        Sora::int128_t oldT = 0;
        Sora::int128_t coefficientT = 1;

        while (remainder != 0) {
            const Sora::int128_t quotient = oldR / remainder;
            const Sora::int128_t nextR = oldR - quotient * remainder;
            oldR = remainder;
            remainder = nextR;

            const Sora::int128_t nextS = oldS - quotient * coefficientS;
            oldS = coefficientS;
            coefficientS = nextS;

            const Sora::int128_t nextT = oldT - quotient * coefficientT;
            oldT = coefficientT;
            coefficientT = nextT;
        }

        if (Integer::IsNegative(left)) {
            oldS = -oldS;
        }
        if (Integer::IsNegative(right)) {
            oldT = -oldT;
        }
        return {static_cast<U>(oldR), oldS, oldT};
    }

    /**
     * @brief Return the floor of the square root of a non-negative integral value.
     * @tparam T Integral operand type, excluding @c bool.
     * @param[in] value Non-negative input.
     * @return The greatest integral @c result such that @c result*result <= @p value.
     * @throws std::domain_error if a signed input is negative.
     */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] constexpr Integer::Magnitude<T> IntegerSqrt(T value) {
        using U = Integer::Magnitude<T>;
        if constexpr (std::signed_integral<T>) {
            if (value < 0) {
                throw std::domain_error("IntegerSqrt requires a non-negative integer");
            }
        }

        const U input = Integer::MagnitudeOf(value);
        U lower = 0;
        U upper = input;
        U result = 0;
        while (lower <= upper) {
            const U middle = static_cast<U>(lower + (upper - lower) / U{2});
            if (middle == 0 || middle <= input / middle) {
                result = middle;
                lower = static_cast<U>(middle + U{1});
            } else {
                upper = static_cast<U>(middle - U{1});
            }
        }
        return result;
    }

    /**
     * @brief Return whether an integral value is prime.
     * @details This deterministic scalar implementation uses trial division and is intended for configuration,
     * compile-time, and moderate-size values rather than bulk factorisation of large 64-bit inputs.
     * @tparam T Integral operand type, excluding @c bool.
     * @param[in] value Value to classify.
     * @return @c true exactly for integers greater than one with no non-trivial divisor.
     */
    template<std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] constexpr bool IsPrime(T value) noexcept {
        using U = Integer::Magnitude<T>;
        if constexpr (std::signed_integral<T>) {
            if (value < 0) {
                return false;
            }
        }

        const U input = Integer::MagnitudeOf(value);
        if (input < U{2}) {
            return false;
        }
        if (input % U{2} == 0) {
            return input == U{2};
        }
        for (U divisor = U{3}; divisor <= input / divisor; divisor += U{2}) {
            if (input % divisor == 0) {
                return false;
            }
        }
        return true;
    }

} // namespace Sora::Math
