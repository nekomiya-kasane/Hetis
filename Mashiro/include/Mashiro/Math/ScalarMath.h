/**
 * @file ScalarMath.h
 * @brief Constexpr scalar math: hardware at run-time, polynomial kernels at compile-time.
 *
 * Every transcendental function (`Sqrt`, `Sin`, `Cos`, `Tan`, `Atan2`, `Asin`, `Acos`)
 * is a single function template over `std::floating_point T`. Each dispatches with
 * `if consteval` (P1938): at run time it calls the hardware `std::` intrinsic; during
 * constant evaluation it runs a polynomial/Newton kernel of full precision for `T`.
 *
 * This dual path is forced — on the current toolchain (Clang-p2996 / libc++)
 * `__cpp_lib_constexpr_cmath` is undefined and sqrt/trig builtins are not
 * constant-foldable. The exceptions are `Abs` and `CopySign`, whose builtins *do*
 * fold, so they are `constexpr` directly (no dual path).
 *
 * **Precision.**
 * - `float`: 5-term Taylor tails (sin/cos), 6-term atan, 5 Newton–Raphson iterations
 *   for sqrt. Error \f$< 3 \times 10^{-7}\f$ (\f$\approx 1\f$ ULP).
 * - `double`: 10-term Taylor tails, 12-term atan, 8 Newton–Raphson iterations.
 *   Error \f$< 2 \times 10^{-15}\f$ (\f$\approx 1\f$ ULP).
 *
 * A compile-time–folded value may differ from its runtime counterpart by a few ULP;
 * irrelevant for renderer constants.
 *
 * **Architecture.** Public constants live in `Mashiro::Math::Const` (see Constants.h).
 * This file's `Detail` namespace contains *only* the compile-time polynomial/Newton
 * kernels and their private configuration (term counts, Horner evaluator, coefficient
 * tables). The public API lives directly in `Mashiro::Math`.
 *
 * @see Constants.h
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Math/Constants.h"

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>

namespace Mashiro::Math {

    // =================================================================
    // Polynomial coefficient tables & Horner evaluation
    // =================================================================

    /**
     * @brief Compile-time factorial computed in `long double` for maximum precision.
     * @param n  Non-negative integer.
     * @return \f$ n! \f$.
     */
    [[nodiscard]] constexpr long double Factorial(int n) {
        long double f = 1.0L;
        for (int i = 2; i <= n; ++i) {
            f *= static_cast<long double>(i);
        }
        return f;
    }

    /**
     * @brief Horner-scheme polynomial evaluation.
     *
     * Computes \f$ c_0 + t(c_1 + t(c_2 + \cdots)) \f$.
     *
     * @tparam T  Floating-point scalar type.
     * @tparam N  Number of coefficients.
     * @param t   Evaluation point.
     * @param c   Coefficient array (ascending powers).
     */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr T Horner(T t, const std::array<T, N>& c) {
        T acc = c[N - 1];
        for (size_t i = N - 1; i > 0; --i) {
            acc = acc * t + c[i - 1];
        }
        return acc;
    }

    /**
     * @brief Build a polynomial coefficient table at compile time.
     * @tparam T      Scalar type of the coefficients.
     * @tparam Terms  Number of terms to generate.
     * @param gen     Generator callable: `gen(int k) -> T`.
     */
    template<std::floating_point T, size_t Terms, typename Gen>
    [[nodiscard]] consteval std::array<T, Terms> MakePoly(Gen gen) {
        std::array<T, Terms> c{};
        for (size_t k = 0; k < Terms; ++k) {
            c[k] = gen(static_cast<int>(k));
        }
        return c;
    }

    /** @cond INTERNAL */
    namespace Detail {

        // -----------------------------------------------------------------
        // Precision-dependent configuration
        // -----------------------------------------------------------------

        /// @brief Taylor-tail term count for \f$\sin\f$.
        template<std::floating_point T>
        inline constexpr size_t kSinTerms = 5;
        template<>
        inline constexpr size_t kSinTerms<double> = 10;

        /// @brief Taylor-tail term count for \f$\cos\f$.
        template<std::floating_point T>
        inline constexpr size_t kCosTerms = 5;
        template<>
        inline constexpr size_t kCosTerms<double> = 10;

        /// @brief Taylor-tail term count for \f$\operatorname{atan}\f$.
        template<std::floating_point T>
        inline constexpr size_t kAtanTerms = 6;
        template<>
        inline constexpr size_t kAtanTerms<double> = 12;

        /// @brief Newton–Raphson step count for \f$\sqrt{x}\f$: more iterations for wider mantissa.
        template<std::floating_point T>
        inline constexpr int kSqrtNewtonSteps = 5;
        template<>
        inline constexpr int kSqrtNewtonSteps<double> = 8;

        // -----------------------------------------------------------------
        // Precomputed polynomial coefficient tables
        // -----------------------------------------------------------------

        /// \f$ \sin(r) = r \cdot P(r^2),\; P(t) = \sum_k \frac{(-1)^k\, t^k}{(2k+1)!} \f$
        template<std::floating_point T>
        inline constexpr auto kSinPoly = MakePoly<T, kSinTerms<T>>([](int k) -> T {
            long double v = 1.0L / Factorial(2 * k + 1);
            return static_cast<T>((k & 1) ? -v : v);
        });

        /// \f$ \cos(r) = Q(r^2),\; Q(t) = \sum_k \frac{(-1)^k\, t^k}{(2k)!} \f$
        template<std::floating_point T>
        inline constexpr auto kCosPoly = MakePoly<T, kCosTerms<T>>([](int k) -> T {
            long double v = 1.0L / Factorial(2 * k);
            return static_cast<T>((k & 1) ? -v : v);
        });

        /// \f$ \operatorname{atan}(a) = a \cdot A(a^2),\; A(t) = \sum_k \frac{(-1)^k\, t^k}{2k+1}
        /// \f$
        template<std::floating_point T>
        inline constexpr auto kAtanPoly = MakePoly<T, kAtanTerms<T>>([](int k) -> T {
            long double v = 1.0L / static_cast<long double>(2 * k + 1);
            return static_cast<T>((k & 1) ? -v : v);
        });

        // =================================================================
        // Constexpr kernels
        // =================================================================

        /**
         * @brief Constexpr square root via bit-hack seed + Newton–Raphson.
         *
         * Uses the "fast inverse square root" bit trick to generate an initial estimate,
         * then refines with Newton iterations to full `T` precision.
         *
         * @tparam T  `float` or `double`.
         * @param x   Non-negative input.
         * @return \f$ \sqrt{x} \f$.
         */
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtSqrt(T x) {
            if (x <= T(0)) {
                return T(0);
            }
            T y;
            if constexpr (std::same_as<T, float>) {
                y = std::bit_cast<float>((std::bit_cast<std::uint32_t>(x) >> 1) + 0x1fbd1df5u);
            } else {
                y = std::bit_cast<double>((std::bit_cast<std::uint64_t>(x) >> 1) +
                                          0x1ff7a3bea91d9b1bULL);
            }
            for (int i = 0; i < kSqrtNewtonSteps<T>; ++i) {
                y = T(0.5) * (y + x / y);
            }
            return y;
        }

        /// @brief Internal sin/cos result pair from the kernel.
        template<std::floating_point T>
        struct SinCosResult {
            T s; ///< \f$\sin\f$ component.
            T c; ///< \f$\cos\f$ component.
        };

        /**
         * @brief Cody–Waite argument reduction to \f$[-\pi/4, \pi/4]\f$, then Taylor tails.
         * @tparam T  `float` or `double`.
         * @param x   Input angle in radians.
         * @return Simultaneously computed \f$(\sin x, \cos x)\f$.
         */
        template<std::floating_point T>
        [[nodiscard]] constexpr SinCosResult<T> CtSinCos(T x) {
            T kf = x * Const::kTwoOverPi<T>;
            long long k = static_cast<long long>(kf + (kf < T(0) ? T(-0.5) : T(0.5)));
            T kfl = static_cast<T>(k);
            T r = (x - kfl * Const::kHalfPiHi<T>)-kfl * Const::kHalfPiLo<T>;
            T r2 = r * r;

            T s = r * Horner(r2, kSinPoly<T>);
            T c = Horner(r2, kCosPoly<T>);

            switch (static_cast<unsigned>(k) & 3u) {
            case 0:
                return {s, c};
            case 1:
                return {c, -s};
            case 2:
                return {-s, -c};
            default:
                return {-c, s};
            }
        }

        /**
         * @brief Constexpr arctangent via range reduction to \f$[-\tan 15°, \tan 15°]\f$.
         * @tparam T  `float` or `double`.
         * @param x   Input value.
         * @return \f$\arctan(x)\f$ in \f$[-\pi/2, \pi/2]\f$.
         */
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAtan(T x) {
            bool neg = x < T(0);
            T a = neg ? -x : x;
            bool inv = a > T(1);
            if (inv) {
                a = T(1) / a;
            }
            bool red = a > Const::kTanPiOverTwelve<T>;
            if (red) {
                a = (a - Const::kTanPiOverSix<T>) / (T(1) + Const::kTanPiOverSix<T> * a);
            }
            T a2 = a * a;
            T p = a * Horner(a2, kAtanPoly<T>);
            if (red) {
                p += Const::kPiOverSix<T>;
            }
            if (inv) {
                p = Const::kHalfPi<T> - p;
            }
            return neg ? -p : p;
        }

        /**
         * @brief Constexpr four-quadrant arctangent.
         * @tparam T  `float` or `double`.
         * @return \f$\operatorname{atan2}(y, x)\f$ in \f$[-\pi, \pi]\f$.
         */
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAtan2(T y, T x) {
            if (x > T(0)) {
                return CtAtan(y / x);
            }
            if (x < T(0)) {
                return CtAtan(y / x) + (y < T(0) ? -Const::kPi<T> : Const::kPi<T>);
            }
            if (y > T(0)) {
                return Const::kHalfPi<T>;
            }
            if (y < T(0)) {
                return -Const::kHalfPi<T>;
            }
            return T(0);
        }

        /**
         * @brief Constexpr arcsine via the identity \f$\arcsin(x) = \operatorname{atan2}(x,
         * \sqrt{1-x^2})\f$.
         * @tparam T  `float` or `double`.
         */
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAsin(T x) {
            if (x >= T(1)) {
                return Const::kHalfPi<T>;
            }
            if (x <= T(-1)) {
                return -Const::kHalfPi<T>;
            }
            return CtAtan2(x, CtSqrt(T(1) - x * x));
        }

        /**
         * @brief Constexpr arccosine via the identity \f$\arccos(x) =
         * \operatorname{atan2}(\sqrt{1-x^2}, x)\f$.
         * @tparam T  `float` or `double`.
         */
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAcos(T x) {
            if (x >= T(1)) {
                return T(0);
            }
            if (x <= T(-1)) {
                return Const::kPi<T>;
            }
            return CtAtan2(CtSqrt(T(1) - x * x), x);
        }

    } // namespace Detail
    /** @endcond */

    // =========================================================================
    // Public API
    // =========================================================================

    /**
     * @brief sin/cos pair returned together (one argument reduction computes both).
     * @tparam T  Floating-point scalar (default `float`).
     */
    template<std::floating_point T = float>
    struct SinCosPair {
        T sin; ///< \f$\sin\f$ component.
        T cos; ///< \f$\cos\f$ component.
    };

    /// @name Absolute value / sign copy
    /// @{

    /**
     * @brief Absolute value (floating-point).
     *
     * Uses compiler builtins that fold at compile time — no `if consteval` needed.
     */
    template<std::floating_point T>
    [[nodiscard]] constexpr T Abs(T x) {
        if constexpr (std::same_as<T, float>) {
            return __builtin_fabsf(x);
        } else {
            return __builtin_fabs(x);
        }
    }

    /// @brief Absolute value (signed integer).
    template<std::signed_integral T>
    [[nodiscard]] constexpr T Abs(T x) {
        return x < T(0) ? static_cast<T>(-x) : x;
    }

    /**
     * @brief Copy the sign of @p sgn onto the magnitude of @p mag.
     *
     * Uses compiler builtins that fold at compile time.
     */
    template<std::floating_point T>
    [[nodiscard]] constexpr T CopySign(T mag, T sgn) {
        if constexpr (std::same_as<T, float>) {
            return __builtin_copysignf(mag, sgn);
        } else {
            return __builtin_copysign(mag, sgn);
        }
    }

    /// @}

    /// @name Transcendental functions
    /// @{

    /// @brief Square root: \f$\sqrt{x}\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Sqrt(T x) {
        if consteval {
            return Detail::CtSqrt(x);
        } else {
            return std::sqrt(x);
        }
    }

    /// @brief Sine: \f$\sin(x)\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Sin(T x) {
        if consteval {
            return Detail::CtSinCos(x).s;
        } else {
            return std::sin(x);
        }
    }

    /// @brief Cosine: \f$\cos(x)\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Cos(T x) {
        if consteval {
            return Detail::CtSinCos(x).c;
        } else {
            return std::cos(x);
        }
    }

    /**
     * @brief Both \f$\sin\f$ and \f$\cos\f$ from a single argument reduction.
     * @return SinCosPair with `.sin` and `.cos` fields.
     */
    template<std::floating_point T>
    [[nodiscard]] constexpr SinCosPair<T> SinCos(T x) {
        if consteval {
            auto r = Detail::CtSinCos(x);
            return {r.s, r.c};
        } else {
            return {std::sin(x), std::cos(x)};
        }
    }

    /// @brief Tangent: \f$\tan(x)\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Tan(T x) {
        if consteval {
            auto r = Detail::CtSinCos(x);
            return r.s / r.c;
        } else {
            return std::tan(x);
        }
    }

    /// @brief Arctangent: \f$\arctan(x)\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Atan(T x) {
        if consteval {
            return Detail::CtAtan(x);
        } else {
            return std::atan(x);
        }
    }

    /// @brief Four-quadrant arctangent: \f$\operatorname{atan2}(y, x)\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Atan2(T y, T x) {
        if consteval {
            return Detail::CtAtan2(y, x);
        } else {
            return std::atan2(y, x);
        }
    }

    /// @brief Arcsine: \f$\arcsin(x)\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Asin(T x) {
        if consteval {
            return Detail::CtAsin(x);
        } else {
            return std::asin(x);
        }
    }

    /// @brief Arccosine: \f$\arccos(x)\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Acos(T x) {
        if consteval {
            return Detail::CtAcos(x);
        } else {
            return std::acos(x);
        }
    }

    /// @}

    /// @name Generic scalar helpers
    /// @{

    /// @brief Variadic minimum (fold over 1+ args of the same arithmetic type).
    template<typename T, std::same_as<T>... Ts>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Min(T a, Ts... rest) {
        ((a = rest < a ? rest : a), ...);
        return a;
    }

    /// @brief Variadic maximum.
    template<typename T, std::same_as<T>... Ts>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Max(T a, Ts... rest) {
        ((a = rest > a ? rest : a), ...);
        return a;
    }

    /// @brief Clamp @p v to \f$[\text{lo}, \text{hi}]\f$.
    template<typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Clamp(T v, T lo, T hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /// @brief Clamp to \f$[0, 1]\f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Saturate(T v) {
        return Clamp(v, T(0), T(1));
    }

    /// @brief Scalar linear interpolation: \f$ a + (b - a) \cdot t \f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Lerp(T a, T b, T t) {
        return a + (b - a) * t;
    }

    /// @brief Sign function: returns \f$-1\f$, \f$0\f$, or \f$+1\f$.
    template<typename T>
        requires(std::is_arithmetic_v<T> && std::is_signed_v<T>)
    [[nodiscard]] constexpr T Sign(T x) {
        return x > T(0) ? T(1) : (x < T(0) ? T(-1) : T(0));
    }

    /// @brief Degrees to radians: \f$ \deg \cdot \frac{\pi}{180} \f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Radians(T deg) {
        return deg * Const::kDegToRad<T>;
    }

    /// @brief Radians to degrees: \f$ \text{rad} \cdot \frac{180}{\pi} \f$.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Degrees(T rad) {
        return rad * Const::kRadToDeg<T>;
    }

    /// @}

    /// @brief Degrees literal (float): `90.0_deg` → radians.
    [[nodiscard]] consteval float operator""_deg(long double deg) {
        return static_cast<float>(deg * Const::kDegToRad<long double>);
    }

    /// @brief Degrees literal (integer, float): `90_deg` → radians.
    [[nodiscard]] consteval float operator""_deg(unsigned long long deg) {
        return static_cast<float>(static_cast<long double>(deg) * Const::kDegToRad<long double>);
    }

    /// @brief Radians literal (float): `1.57_rad` — identity, serves as documentation.
    [[nodiscard]] consteval float operator""_rad(long double rad) {
        return static_cast<float>(rad);
    }

    /// @brief Radians literal (integer, float): `1_rad` — identity.
    [[nodiscard]] consteval float operator""_rad(unsigned long long rad) {
        return static_cast<float>(rad);
    }

    /// @brief Degrees literal (long double): `90.0_deg_d` → radians.
    [[nodiscard]] consteval long double operator""_deg_d(long double deg) {
        return deg * Const::kDegToRad<long double>;
    }

    /// @brief Degrees literal (integer, long double): `90_deg_d` → radians.
    [[nodiscard]] consteval long double operator""_deg_d(unsigned long long deg) {
        return static_cast<long double>(deg) * Const::kDegToRad<long double>;
    }

    /// @brief Radians literal (long double): `1.57_rad_d` — identity.
    [[nodiscard]] consteval long double operator""_rad_d(long double rad) {
        return rad;
    }

    /// @brief Radians literal (integer, long double): `1_rad_d` — identity.
    [[nodiscard]] consteval long double operator""_rad_d(unsigned long long rad) {
        return static_cast<long double>(rad);
    }

} // namespace Mashiro::Math
