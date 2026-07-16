/**
 * @file ConstexprMath.h
 * @brief Constant-evaluation kernels used when the platform C math library is not C++26 constexpr-enabled.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Constants.h>

#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Sora::Math::Detail {

    template<std::floating_point T>
    [[nodiscard]] constexpr bool CtIsNan(T x) noexcept {
        return x != x;
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr bool CtIsInf(T x) noexcept {
        return x == std::numeric_limits<T>::infinity() || x == -std::numeric_limits<T>::infinity();
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtAbs(T x) noexcept {
        return x == T{} ? T{} : (x < T{} ? -x : x);
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr bool CtSignBit(T x) noexcept {
        if constexpr (std::same_as<T, float>) {
            return (std::bit_cast<std::uint32_t>(x) >> 31U) != 0;
        } else if constexpr (std::same_as<T, double>) {
            return (std::bit_cast<std::uint64_t>(x) >> 63U) != 0;
        } else {
            return x < T{};
        }
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtCopySign(T magnitude, T sign) noexcept {
        if constexpr (std::same_as<T, float>) {
            constexpr std::uint32_t signMask = std::uint32_t{1} << 31U;
            const auto magnitudeBits = std::bit_cast<std::uint32_t>(magnitude) & ~signMask;
            const auto signBits = std::bit_cast<std::uint32_t>(sign) & signMask;
            return std::bit_cast<float>(magnitudeBits | signBits);
        } else if constexpr (std::same_as<T, double>) {
            constexpr std::uint64_t signMask = std::uint64_t{1} << 63U;
            const auto magnitudeBits = std::bit_cast<std::uint64_t>(magnitude) & ~signMask;
            const auto signBits = std::bit_cast<std::uint64_t>(sign) & signMask;
            return std::bit_cast<double>(magnitudeBits | signBits);
        } else {
            return CtSignBit(sign) ? -CtAbs(magnitude) : CtAbs(magnitude);
        }
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtExp(T x) noexcept {
        if (CtIsNan(x)) {
            return x;
        }
        if (x == std::numeric_limits<T>::infinity()) {
            return x;
        }
        if (x == -std::numeric_limits<T>::infinity()) {
            return T{};
        }

        int exponent = 0;
        constexpr T halfLn2 = Const::kLn2<T> / T{2};
        while (x > halfLn2) {
            x -= Const::kLn2<T>;
            if (++exponent > std::numeric_limits<T>::max_exponent) {
                return std::numeric_limits<T>::infinity();
            }
        }
        while (x < -halfLn2) {
            x += Const::kLn2<T>;
            if (--exponent < std::numeric_limits<T>::min_exponent - std::numeric_limits<T>::digits) {
                return T{};
            }
        }

        T sum = T{1};
        T term = T{1};
        constexpr int terms = std::same_as<T, float> ? 12 : 22;
        for (int i = 1; i <= terms; ++i) {
            term *= x / static_cast<T>(i);
            sum += term;
        }
        while (exponent > 0) {
            sum *= T{2};
            --exponent;
        }
        while (exponent < 0) {
            sum *= T{0.5};
            ++exponent;
        }
        return sum;
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtLog(T x) noexcept {
        if (CtIsNan(x)) {
            return x;
        }
        if (x < T{}) {
            return std::numeric_limits<T>::quiet_NaN();
        }
        if (x == T{}) {
            return -std::numeric_limits<T>::infinity();
        }
        if (x == std::numeric_limits<T>::infinity()) {
            return x;
        }

        int exponent = 0;
        while (x >= T{2}) {
            x *= T{0.5};
            ++exponent;
        }
        while (x < T{1}) {
            x *= T{2};
            --exponent;
        }

        const T z = (x - T{1}) / (x + T{1});
        const T z2 = z * z;
        T term = z;
        T sum = z;
        constexpr int terms = std::same_as<T, float> ? 14 : 32;
        for (int i = 1; i < terms; ++i) {
            term *= z2;
            sum += term / static_cast<T>(2 * i + 1);
        }
        return T{2} * sum + static_cast<T>(exponent) * Const::kLn2<T>;
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtSqrt(T x) noexcept {
        if (CtIsNan(x) || x < T{}) {
            return std::numeric_limits<T>::quiet_NaN();
        }
        if (x == T{} || x == std::numeric_limits<T>::infinity()) {
            return x;
        }

        int scale = 0;
        while (x >= T{4}) {
            x *= T{0.25};
            ++scale;
        }
        while (x < T{1}) {
            x *= T{4};
            --scale;
        }
        T result = (x + T{1}) / T{2};
        constexpr int iterations = std::same_as<T, float> ? 6 : 9;
        for (int i = 0; i < iterations; ++i) {
            result = (result + x / result) / T{2};
        }
        while (scale > 0) {
            result *= T{2};
            --scale;
        }
        while (scale < 0) {
            result *= T{0.5};
            ++scale;
        }
        return result;
    }

    template<std::floating_point T>
    struct CtSinCosResult {
        T sin;
        T cos;
    };

    /** @brief Constant-evaluation sine/cosine kernel with Cody-Waite range reduction. */
    template<std::floating_point T>
    [[nodiscard]] constexpr CtSinCosResult<T> CtSinCos(T x) {
        if (CtIsNan(x) || CtIsInf(x)) {
            const T nan = std::numeric_limits<T>::quiet_NaN();
            return {nan, nan};
        }

        // Full-range Payne-Hanek reduction requires a large 2/pi table. Reject unsupported constant expressions
        // instead of silently returning a phase with no significant bits. Runtime calls never enter this kernel.
        constexpr T rangeLimit = T{1'048'576};
        if (CtAbs(x) > rangeLimit) {
            throw "constexpr Sin/Cos/Tan require |x| <= 2^20; use runtime <cmath> outside this range";
        }

        using W = std::conditional_t<std::same_as<T, float>, double, T>;
        constexpr W invPiOverTwo = W{2} / Const::kPi<W>;
        const W wide = static_cast<W>(x);
        const W scaled = wide * invPiOverTwo;
        const auto quadrant = static_cast<long long>(scaled < W{} ? scaled - W{0.5} : scaled + W{0.5});
        const W reduced =
            (wide - static_cast<W>(quadrant) * Const::kHalfPiHi<W>)-static_cast<W>(quadrant) * Const::kHalfPiLo<W>;

        const W x2 = reduced * reduced;
        W sinTerm = reduced;
        W cosTerm = W{1};
        W sinValue = sinTerm;
        W cosValue = cosTerm;
        constexpr int terms = std::same_as<T, float> ? 7 : 12;
        for (int i = 1; i < terms; ++i) {
            sinTerm *= -x2 / static_cast<W>((2 * i) * (2 * i + 1));
            cosTerm *= -x2 / static_cast<W>((2 * i - 1) * (2 * i));
            sinValue += sinTerm;
            cosValue += cosTerm;
        }

        switch (static_cast<unsigned>(quadrant) & 3U) {
            case 0:
                return {static_cast<T>(sinValue), static_cast<T>(cosValue)};
            case 1:
                return {static_cast<T>(cosValue), static_cast<T>(-sinValue)};
            case 2:
                return {static_cast<T>(-sinValue), static_cast<T>(-cosValue)};
            default:
                return {static_cast<T>(-cosValue), static_cast<T>(sinValue)};
        }
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtAtan(T x) noexcept {
        if (CtIsNan(x)) {
            return x;
        }
        const bool negative = CtSignBit(x);
        T value = CtAbs(x);
        const bool invert = value > T{1};
        if (invert) {
            value = T{1} / value;
        }
        const bool reduce = value > Const::kTanPiOverTwelve<T>;
        if (reduce) {
            value = (value - Const::kTanPiOverSix<T>) / (T{1} + Const::kTanPiOverSix<T> * value);
        }

        const T squared = value * value;
        T term = value;
        T result = value;
        constexpr int terms = std::same_as<T, float> ? 9 : 18;
        for (int i = 1; i < terms; ++i) {
            term *= -squared;
            result += term / static_cast<T>(2 * i + 1);
        }
        if (reduce) {
            result += Const::kPiOverSix<T>;
        }
        if (invert) {
            result = Const::kHalfPi<T> - result;
        }
        return negative ? -result : result;
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtAtan2(T y, T x) noexcept {
        if (CtIsNan(x) || CtIsNan(y)) {
            return std::numeric_limits<T>::quiet_NaN();
        }
        if (CtIsInf(y)) {
            if (CtIsInf(x)) {
                const T angle = CtSignBit(x) ? T{3} * Const::kPiOverFour<T> : Const::kPiOverFour<T>;
                return CtSignBit(y) ? -angle : angle;
            }
            return CtSignBit(y) ? -Const::kHalfPi<T> : Const::kHalfPi<T>;
        }
        if (CtIsInf(x)) {
            if (CtSignBit(x)) {
                return CtSignBit(y) ? -Const::kPi<T> : Const::kPi<T>;
            }
            return CtSignBit(y) ? -T{} : T{};
        }
        if (x > T{}) {
            return CtAtan(y / x);
        }
        if (x < T{}) {
            return CtAtan(y / x) + (CtSignBit(y) ? -Const::kPi<T> : Const::kPi<T>);
        }
        if (y > T{}) {
            return Const::kHalfPi<T>;
        }
        if (y < T{}) {
            return -Const::kHalfPi<T>;
        }
        if (CtSignBit(x)) {
            return CtSignBit(y) ? -Const::kPi<T> : Const::kPi<T>;
        }
        return y;
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr bool CtIsInteger(T x) noexcept {
        if (CtIsNan(x) || CtIsInf(x)) {
            return false;
        }
        T value = CtAbs(x);
        if (value >= static_cast<T>(std::numeric_limits<std::uint64_t>::max())) {
            return true;
        }
        return static_cast<T>(static_cast<std::uint64_t>(value)) == value;
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtPowInteger(T base, T exponent) noexcept {
        const bool negativeExponent = exponent < T{};
        T magnitude = CtAbs(exponent);
        if (magnitude >= static_cast<T>(std::numeric_limits<std::uint64_t>::max())) {
            const T absoluteBase = CtAbs(base);
            if (absoluteBase == T{1}) {
                return T{1};
            }
            if (absoluteBase > T{1}) {
                return negativeExponent ? T{} : std::numeric_limits<T>::infinity();
            }
            return negativeExponent ? std::numeric_limits<T>::infinity() : T{};
        }
        T result = T{1};
        while (magnitude >= T{1}) {
            const auto half = static_cast<std::uint64_t>(magnitude / T{2});
            if (magnitude - static_cast<T>(half) * T{2} >= T{1}) {
                result *= base;
            }
            base *= base;
            magnitude = static_cast<T>(half);
        }
        return negativeExponent ? T{1} / result : result;
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr T CtPow(T base, T exponent) noexcept {
        if (exponent == T{}) {
            return T{1};
        }
        if (base == T{1} || (base == T{-1} && CtIsInf(exponent))) {
            return T{1};
        }
        if (CtIsNan(base) || CtIsNan(exponent)) {
            return std::numeric_limits<T>::quiet_NaN();
        }
        if (base == T{}) {
            const bool odd = CtIsInteger(exponent) && CtAbs(exponent) < T{9007199254740992.0} &&
                             (static_cast<std::uint64_t>(CtAbs(exponent)) & 1U) != 0;
            if (exponent < T{}) {
                return CtSignBit(base) && odd ? -std::numeric_limits<T>::infinity()
                                              : std::numeric_limits<T>::infinity();
            }
            return CtSignBit(base) && odd ? -T{} : T{};
        }
        if (base < T{}) {
            return CtIsInteger(exponent) ? CtPowInteger(base, exponent) : std::numeric_limits<T>::quiet_NaN();
        }
        return CtExp(exponent * CtLog(base));
    }

} // namespace Sora::Math::Detail
