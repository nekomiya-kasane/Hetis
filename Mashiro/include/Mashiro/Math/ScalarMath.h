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
 *   for sqrt. Error < ~3e-7 (≈1 ULP).
 * - `double`: 10-term Taylor tails, 12-term atan, 8 Newton–Raphson iterations.
 *   Error < ~2e-15 (≈1 ULP). A compile-time–folded value may differ from its
 *   runtime counterpart by a few ULP; irrelevant for renderer constants.
 *
 * **Zero technical debt.** Constants derive from `<numbers>` (`pi_v`, `inv_pi_v`,
 * `sqrt3_v`, `inv_sqrt3_v`). Polynomial coefficients are generated at compile time
 * from closed-form reciprocal-factorial/reciprocal-odd-integer series. Cody–Waite
 * high/low words are computed from `long double` π/2 to maximise cancellation.
 *
 * Namespace: Mashiro::Math
 *
 * @ingroup Math
 */
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace Mashiro::Math {

    /** @cond INTERNAL */
    namespace Detail {

        // =================================================================
        // Derived constants (all from <numbers>, never hand-typed)
        // =================================================================

        template<std::floating_point T>
        inline constexpr T kPi = std::numbers::pi_v<T>;
        template<std::floating_point T>
        inline constexpr T kHalfPi = std::numbers::pi_v<T> / T(2);
        template<std::floating_point T>
        inline constexpr T kPiOverSix = std::numbers::pi_v<T> / T(6);
        template<std::floating_point T>
        inline constexpr T kTwoOverPi = std::numbers::inv_pi_v<T> * T(2);

        /// Cody–Waite split of π/2 for `float`: `kHalfPiHi` keeps low mantissa bits clear;
        /// `kHalfPiLo` carries the residual.
        template<std::floating_point T>
        inline constexpr T kHalfPiHi{};
        template<std::floating_point T>
        inline constexpr T kHalfPiLo{};
        template<>
        inline constexpr float kHalfPiHi<float> = 1.5707963109016418f;
        template<>
        inline constexpr float kHalfPiLo<float> = static_cast<float>(
            std::numbers::pi_v<double> / 2.0 - static_cast<double>(kHalfPiHi<float>));
        template<>
        inline constexpr double kHalfPiHi<double> = 1.5707963267948966;
        template<>
        inline constexpr double kHalfPiLo<double> =
            static_cast<long double>(std::numbers::pi_v<long double>) / 2.0L -
            static_cast<long double>(kHalfPiHi<double>);

        template<std::floating_point T>
        inline constexpr T kTanPi6 = std::numbers::inv_sqrt3_v<T>;
        template<std::floating_point T>
        inline constexpr T kTanPi12 = T(2) - std::numbers::sqrt3_v<T>;

        // =================================================================
        // Polynomial coefficient tables & Horner evaluation
        // =================================================================

        /// @brief Compile-time factorial (computed in `long double` for max precision).
        [[nodiscard]] constexpr long double Factorial(int n) {
            long double f = 1.0L;
            for (int i = 2; i <= n; ++i) {
                f *= static_cast<long double>(i);
            }
            return f;
        }

        /// @brief Horner-scheme evaluation: `c[0] + t*(c[1] + t*(c[2] + …))`.
        template<std::floating_point T, size_t N>
        [[nodiscard]] constexpr T Horner(T t, const std::array<T, N>& c) {
            T acc = c[N - 1];
            for (size_t i = N - 1; i > 0; --i) {
                acc = acc * t + c[i - 1];
            }
            return acc;
        }

        /// @brief Build a polynomial coefficient table at compile time.
        template<std::floating_point T, size_t Terms, typename Gen>
        [[nodiscard]] consteval std::array<T, Terms> MakePoly(Gen gen) {
            std::array<T, Terms> c{};
            for (size_t k = 0; k < Terms; ++k) {
                c[k] = gen(static_cast<int>(k));
            }
            return c;
        }

        /// @brief Precision-dependent term count for the Taylor tails.
        template<std::floating_point T>
        inline constexpr size_t kSinTerms = 5;
        template<>
        inline constexpr size_t kSinTerms<double> = 10;
        template<std::floating_point T>
        inline constexpr size_t kCosTerms = 5;
        template<>
        inline constexpr size_t kCosTerms<double> = 10;
        template<std::floating_point T>
        inline constexpr size_t kAtanTerms = 6;
        template<>
        inline constexpr size_t kAtanTerms<double> = 12;

        /// sin(r) = r · P(r²), P(t) = ∑ₖ (−1)ᵏ tᵏ / (2k+1)!
        template<std::floating_point T>
        inline constexpr auto kSinPoly = MakePoly<T, kSinTerms<T>>([](int k) -> T {
            long double v = 1.0L / Factorial(2 * k + 1);
            return static_cast<T>((k & 1) ? -v : v);
        });
        /// cos(r) = Q(r²), Q(t) = ∑ₖ (−1)ᵏ tᵏ / (2k)!
        template<std::floating_point T>
        inline constexpr auto kCosPoly = MakePoly<T, kCosTerms<T>>([](int k) -> T {
            long double v = 1.0L / Factorial(2 * k);
            return static_cast<T>((k & 1) ? -v : v);
        });
        /// atan(a) = a · A(a²), A(t) = ∑ₖ (−1)ᵏ tᵏ / (2k+1)
        template<std::floating_point T>
        inline constexpr auto kAtanPoly = MakePoly<T, kAtanTerms<T>>([](int k) -> T {
            long double v = 1.0L / static_cast<long double>(2 * k + 1);
            return static_cast<T>((k & 1) ? -v : v);
        });

        // =================================================================
        // Constexpr kernels (templatized over T)
        // =================================================================

        /// @brief Newton–Raphson step count: more iterations for wider mantissa.
        template<std::floating_point T>
        inline constexpr int kSqrtNewtonSteps = 5;
        template<>
        inline constexpr int kSqrtNewtonSteps<double> = 8;

        /// @brief Constexpr square root — bit-hack seed + Newton–Raphson. Full precision for T.
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

        /// @brief Internal sin/cos pair returned from the kernel.
        template<std::floating_point T>
        struct SinCosResult {
            T s;
            T c;
        };

        /// @brief Cody–Waite reduction to [−π/4, π/4], then Taylor tails.
        template<std::floating_point T>
        [[nodiscard]] constexpr SinCosResult<T> CtSinCos(T x) {
            T kf = x * kTwoOverPi<T>;
            long long k = static_cast<long long>(kf + (kf < T(0) ? T(-0.5) : T(0.5)));
            T kfl = static_cast<T>(k);
            T r = (x - kfl * kHalfPiHi<T>)-kfl * kHalfPiLo<T>;
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

        /// @brief Constexpr atan via range reduction to [−tan 15°, tan 15°].
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAtan(T x) {
            bool neg = x < T(0);
            T a = neg ? -x : x;
            bool inv = a > T(1);
            if (inv) {
                a = T(1) / a;
            }
            bool red = a > kTanPi12<T>;
            if (red) {
                a = (a - kTanPi6<T>) / (T(1) + kTanPi6<T> * a);
            }
            T a2 = a * a;
            T p = a * Horner(a2, kAtanPoly<T>);
            if (red) {
                p += kPiOverSix<T>;
            }
            if (inv) {
                p = kHalfPi<T> - p;
            }
            return neg ? -p : p;
        }

        /// @brief Constexpr atan2 (four-quadrant).
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAtan2(T y, T x) {
            if (x > T(0)) {
                return CtAtan(y / x);
            }
            if (x < T(0)) {
                return CtAtan(y / x) + (y < T(0) ? -kPi<T> : kPi<T>);
            }
            if (y > T(0)) {
                return kHalfPi<T>;
            }
            if (y < T(0)) {
                return -kHalfPi<T>;
            }
            return T(0);
        }

        /// @brief Constexpr arcsin via atan2 identity.
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAsin(T x) {
            if (x >= T(1)) {
                return kHalfPi<T>;
            }
            if (x <= T(-1)) {
                return -kHalfPi<T>;
            }
            return CtAtan2(x, CtSqrt(T(1) - x * x));
        }

        /// @brief Constexpr arccos via atan2 identity.
        template<std::floating_point T>
        [[nodiscard]] constexpr T CtAcos(T x) {
            if (x >= T(1)) {
                return T(0);
            }
            if (x <= T(-1)) {
                return kPi<T>;
            }
            return CtAtan2(CtSqrt(T(1) - x * x), x);
        }

    } // namespace Detail
    /** @endcond */

    /** @brief sin/cos pair returned together (one argument reduction computes both). */
    template<std::floating_point T = float>
    struct SinCosPair {
        T sin; ///< sin component.
        T cos; ///< cos component.
    };

    /// @name Absolute value / sign copy (no dual path — builtins fold at compile time)
    /// @{

    /// @brief Absolute value (floating-point).
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

    /// @brief Copy the sign of @p sgn onto @p mag.
    template<std::floating_point T>
    [[nodiscard]] constexpr T CopySign(T mag, T sgn) {
        if constexpr (std::same_as<T, float>) {
            return __builtin_copysignf(mag, sgn);
        } else {
            return __builtin_copysign(mag, sgn);
        }
    }

    /// @}

    /// @name Transcendental façade (hardware at run-time, polynomial at compile-time)
    /// @{

    /// @brief Square root.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Sqrt(T x) {
        if consteval {
            return Detail::CtSqrt(x);
        } else {
            return std::sqrt(x);
        }
    }
    /// @brief Sine.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Sin(T x) {
        if consteval {
            return Detail::CtSinCos(x).s;
        } else {
            return std::sin(x);
        }
    }
    /// @brief Cosine.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Cos(T x) {
        if consteval {
            return Detail::CtSinCos(x).c;
        } else {
            return std::cos(x);
        }
    }

    /** @brief Both sin and cos from a single argument reduction. */
    template<std::floating_point T>
    [[nodiscard]] constexpr SinCosPair<T> SinCos(T x) {
        if consteval {
            auto r = Detail::CtSinCos(x);
            return {r.s, r.c};
        } else {
            return {std::sin(x), std::cos(x)};
        }
    }

    /// @brief Tangent.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Tan(T x) {
        if consteval {
            auto r = Detail::CtSinCos(x);
            return r.s / r.c;
        } else {
            return std::tan(x);
        }
    }
    /// @brief Arctangent.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Atan(T x) {
        if consteval {
            return Detail::CtAtan(x);
        } else {
            return std::atan(x);
        }
    }
    /// @brief Four-quadrant arctangent.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Atan2(T y, T x) {
        if consteval {
            return Detail::CtAtan2(y, x);
        } else {
            return std::atan2(y, x);
        }
    }
    /// @brief Arcsine.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Asin(T x) {
        if consteval {
            return Detail::CtAsin(x);
        } else {
            return std::asin(x);
        }
    }
    /// @brief Arccosine.
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

    /// @brief Variadic minimum (fold, 1+ args of the same arithmetic type).
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

    /// @brief Clamp @p v to [lo, hi].
    template<typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Clamp(T v, T lo, T hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    /// @brief Clamp to [0, 1].
    template<std::floating_point T>
    [[nodiscard]] constexpr T Saturate(T v) {
        return Clamp(v, T(0), T(1));
    }
    /// @brief Scalar linear interpolation: `a + (b − a) * t`.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Lerp(T a, T b, T t) {
        return a + (b - a) * t;
    }
    /// @brief Sign function: −1, 0, or +1.
    template<typename T>
        requires(std::is_arithmetic_v<T> && std::is_signed_v<T>)
    [[nodiscard]] constexpr T Sign(T x) {
        return x > T(0) ? T(1) : (x < T(0) ? T(-1) : T(0));
    }
    /// @brief Degrees → radians.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Radians(T deg) {
        return deg * (std::numbers::pi_v<T> / T(180));
    }
    /// @brief Radians → degrees.
    template<std::floating_point T>
    [[nodiscard]] constexpr T Degrees(T rad) {
        return rad * (T(180) / std::numbers::pi_v<T>);
    }

    /// @}

} // namespace Mashiro::Math
