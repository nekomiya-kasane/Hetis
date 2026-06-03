/**
 * @file ScalarMath.h
 * @brief Constexpr scalar math: hardware at run-time, polynomial kernels at compile-time.
 *
 * A constexpr-friendly façade over the transcendental functions
 * (`Sqrt`, `Sin`, `Cos`, `Tan`, `Atan2`, `Asin`, `Acos`, …).
 * Each one dispatches with `if consteval` (P1938):
 * at run time it lowers to the hardware `std::` intrinsic (sqrtss, vendor trig),
 * while during constant evaluation it runs an accurate polynomial kernel (error
 * < ~1e-6 over the ranges renderer math exercises). This is forced, not stylistic:
 * on the current toolchain (Clang-p2996 / libc++) `__cpp_lib_constexpr_cmath` is
 * undefined and the sqrt/trig builtins are not constant-foldable, so std:: cannot
 * be called during constant evaluation. The exceptions are Abs and Copysign, whose
 * builtins DO fold, so they call the builtin directly with no dual path.
 *
 * Precision note: a value folded at compile time uses the polynomial kernels and so
 * may differ from the same call made at run time (hardware) by a few ULP. This is
 * irrelevant for renderer constants but is a real (documented) difference.
 *
 * Constants are derived, never hand-typed: angle constants come from <numbers>
 * (pi_v, inv_pi_v, sqrt3_v, inv_sqrt3_v), the Cody-Waite low word is computed from
 * the double-precision pi/2, and every polynomial-kernel coefficient is generated
 * at compile time from its closed form (reciprocal factorials for sin/cos, odd
 * reciprocals for atan) so the series are provably the Taylor/Maclaurin tails.
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

        /// @name Derived constants (no hand-typed transcendental literals)
        /// @{

        inline constexpr float kPi      = std::numbers::pi_v<float>;
        inline constexpr float kHalfPi  = std::numbers::pi_v<float> / 2.0f;
        inline constexpr float kPiSixth = std::numbers::pi_v<float> / 6.0f;
        inline constexpr float k2OverPi = std::numbers::inv_pi_v<float> * 2.0f;

        /// Cody–Waite split of π/2: `kHalfPiHi` keeps its low mantissa bits clear so
        /// that `k * kHalfPiHi` is exact; `kHalfPiLo` carries the residual.
        inline constexpr float kHalfPiHi = 1.5707963109016418f;
        inline constexpr float kHalfPiLo =
            static_cast<float>(std::numbers::pi_v<double> / 2.0 - static_cast<double>(kHalfPiHi));

        /// Argument-reduction tangents: `tan(30°) = 1/√3`, `tan(15°) = 2 − √3`.
        inline constexpr float kTanPi6  = std::numbers::inv_sqrt3_v<float>;
        inline constexpr float kTanPi12 = static_cast<float>(2.0 - std::numbers::sqrt3_v<double>);

        /// @}

        /// @name Polynomial coefficient tables & Horner evaluation
        /// @{

        /// @brief Compile-time factorial.
        [[nodiscard]] constexpr double Factorial(int n) {
            double f = 1.0;
            for (int i = 2; i <= n; ++i) {
                f *= static_cast<double>(i);
            }
            return f;
        }

        /// @brief Horner-scheme evaluation: `c[0] + t*(c[1] + t*(c[2] + …))`.
        template <std::size_t N>
        [[nodiscard]] constexpr float Horner(float t, const std::array<float, N>& c) {
            float acc = c[N - 1];
            for (std::size_t i = N - 1; i > 0; --i) {
                acc = acc * t + c[i - 1];
            }
            return acc;
        }

        /// @brief Build a polynomial coefficient table at compile time.
        template <std::size_t Terms, typename Gen>
        [[nodiscard]] consteval std::array<float, Terms> MakePoly(Gen gen) {
            std::array<float, Terms> c{};
            for (std::size_t k = 0; k < Terms; ++k) {
                c[k] = gen(static_cast<int>(k));
            }
            return c;
        }

        /// sin(r) = r · P(r²), P(t) = ∑ₖ (−1)ᵏ tᵏ / (2k+1)!
        inline constexpr auto kSinPoly = MakePoly<5>([](int k) {
            double v = 1.0 / Factorial(2 * k + 1);
            return static_cast<float>((k & 1) ? -v : v);
        });
        /// cos(r) = Q(r²), Q(t) = ∑ₖ (−1)ᵏ tᵏ / (2k)!
        inline constexpr auto kCosPoly = MakePoly<5>([](int k) {
            double v = 1.0 / Factorial(2 * k);
            return static_cast<float>((k & 1) ? -v : v);
        });
        /// atan(a) = a · A(a²), A(t) = ∑ₖ (−1)ᵏ tᵏ / (2k+1)
        inline constexpr auto kAtanPoly = MakePoly<6>([](int k) {
            double v = 1.0 / static_cast<double>(2 * k + 1);
            return static_cast<float>((k & 1) ? -v : v);
        });

        /// @}

        /// @name Constexpr kernels
        /// @{

        inline constexpr std::uint32_t kSqrtSeedBias    = 0x1fbd1df5u; ///< Bit-hack initial estimate bias.
        inline constexpr int           kSqrtNewtonSteps = 5;           ///< Newton–Raphson iterations for full precision.

        /// @brief Constexpr square root (Newton–Raphson from a bit-hack seed).
        [[nodiscard]] constexpr float CtSqrt(float x) {
            if (x <= 0.0f) {
                return 0.0f;
            }
            float y = std::bit_cast<float>((std::bit_cast<std::uint32_t>(x) >> 1) + kSqrtSeedBias);
            for (int i = 0; i < kSqrtNewtonSteps; ++i) {
                y = 0.5f * (y + x / y);
            }
            return y;
        }

        /// @brief Internal sin/cos pair returned from the kernel.
        struct SinCosResult {
            float s; ///< sin component.
            float c; ///< cos component.
        };

        /// @brief Cody–Waite reduction to [−π/4, π/4], then Taylor tails (< 3e–8 error).
        [[nodiscard]] constexpr SinCosResult CtSinCos(float x) {
            float kf = x * k2OverPi;
            long long k = static_cast<long long>(kf + (kf < 0.0f ? -0.5f : 0.5f));
            float kfl = static_cast<float>(k);
            float r = (x - kfl * kHalfPiHi) - kfl * kHalfPiLo;
            float r2 = r * r;

            float s = r * Horner(r2, kSinPoly);
            float c = Horner(r2, kCosPoly);

            switch (static_cast<unsigned>(k) & 3u) {
            case 0:  return {s, c};
            case 1:  return {c, -s};
            case 2:  return {-s, -c};
            default: return {-c, s};
            }
        }

        /// @brief Constexpr atan via range reduction to [−tan 15°, tan 15°].
        [[nodiscard]] constexpr float CtAtan(float x) {
            bool neg = x < 0.0f;
            float a = neg ? -x : x;
            bool inv = a > 1.0f;
            if (inv) {
                a = 1.0f / a;
            }
            bool red = a > kTanPi12;
            if (red) {
                a = (a - kTanPi6) / (1.0f + kTanPi6 * a);
            }
            float a2 = a * a;
            float p = a * Horner(a2, kAtanPoly);
            if (red) {
                p += kPiSixth;
            }
            if (inv) {
                p = kHalfPi - p;
            }
            return neg ? -p : p;
        }

        /// @brief Constexpr atan2 (four-quadrant).
        [[nodiscard]] constexpr float CtAtan2(float y, float x) {
            if (x > 0.0f) {
                return CtAtan(y / x);
            }
            if (x < 0.0f) {
                return CtAtan(y / x) + (y < 0.0f ? -kPi : kPi);
            }
            if (y > 0.0f) {
                return kHalfPi;
            }
            if (y < 0.0f) {
                return -kHalfPi;
            }
            return 0.0f;
        }

        /// @brief Constexpr arcsin via atan2 identity.
        [[nodiscard]] constexpr float CtAsin(float x) {
            if (x >= 1.0f) {
                return kHalfPi;
            }
            if (x <= -1.0f) {
                return -kHalfPi;
            }
            return CtAtan2(x, CtSqrt(1.0f - x * x));
        }

        /// @brief Constexpr arccos via atan2 identity.
        [[nodiscard]] constexpr float CtAcos(float x) {
            if (x >= 1.0f) {
                return 0.0f;
            }
            if (x <= -1.0f) {
                return kPi;
            }
            return CtAtan2(CtSqrt(1.0f - x * x), x);
        }

        /// @}

    } // namespace Detail
    /** @endcond */

    /** @brief sin/cos pair returned together (one argument reduction computes both). */
    struct SinCosPair {
        float sin; ///< sin component.
        float cos; ///< cos component.
    };

    /// @name Absolute value / sign copy (no dual path — builtins fold at compile time)
    /// @{

    /// @brief Absolute value (float).
    [[nodiscard]] constexpr float  Abs(float x)  { return __builtin_fabsf(x); }
    /// @brief Absolute value (double).
    [[nodiscard]] constexpr double Abs(double x) { return __builtin_fabs(x); }
    /// @brief Absolute value (signed integer).
    template <std::signed_integral T>
    [[nodiscard]] constexpr T Abs(T x) { return x < T(0) ? static_cast<T>(-x) : x; }

    /// @brief Copy the sign of @p sgn onto @p mag (float).
    [[nodiscard]] constexpr float  Copysign(float mag, float sgn)   { return __builtin_copysignf(mag, sgn); }
    /// @brief Copy the sign of @p sgn onto @p mag (double).
    [[nodiscard]] constexpr double Copysign(double mag, double sgn) { return __builtin_copysign(mag, sgn); }

    /// @}

    /// @name Transcendental façade (hardware at run-time, polynomial at compile-time)
    /// @{

    /// @brief Square root.
    [[nodiscard]] constexpr float Sqrt(float x) {
        if consteval { return Detail::CtSqrt(x); } else { return std::sqrt(x); }
    }
    /// @brief Sine.
    [[nodiscard]] constexpr float Sin(float x) {
        if consteval { return Detail::CtSinCos(x).s; } else { return std::sin(x); }
    }
    /// @brief Cosine.
    [[nodiscard]] constexpr float Cos(float x) {
        if consteval { return Detail::CtSinCos(x).c; } else { return std::cos(x); }
    }

    /** @brief Both sin and cos from a single argument reduction. */
    [[nodiscard]] constexpr SinCosPair SinCos(float x) {
        if consteval {
            Detail::SinCosResult r = Detail::CtSinCos(x);
            return {r.s, r.c};
        } else {
            return {std::sin(x), std::cos(x)};
        }
    }

    /// @brief Tangent.
    [[nodiscard]] constexpr float Tan(float x) {
        if consteval {
            Detail::SinCosResult r = Detail::CtSinCos(x);
            return r.s / r.c;
        } else {
            return std::tan(x);
        }
    }
    /// @brief Arctangent.
    [[nodiscard]] constexpr float Atan(float x) {
        if consteval { return Detail::CtAtan(x); } else { return std::atan(x); }
    }
    /// @brief Four-quadrant arctangent.
    [[nodiscard]] constexpr float Atan2(float y, float x) {
        if consteval { return Detail::CtAtan2(y, x); } else { return std::atan2(y, x); }
    }
    /// @brief Arcsine.
    [[nodiscard]] constexpr float Asin(float x) {
        if consteval { return Detail::CtAsin(x); } else { return std::asin(x); }
    }
    /// @brief Arccosine.
    [[nodiscard]] constexpr float Acos(float x) {
        if consteval { return Detail::CtAcos(x); } else { return std::acos(x); }
    }

    /// @}

    /// @name Generic scalar helpers
    /// @{

    /// @brief Variadic minimum (fold, 1+ args of the same arithmetic type).
    template <typename T, std::same_as<T>... Ts>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Min(T a, Ts... rest) {
        ((a = rest < a ? rest : a), ...);
        return a;
    }
    /// @brief Variadic maximum.
    template <typename T, std::same_as<T>... Ts>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Max(T a, Ts... rest) {
        ((a = rest > a ? rest : a), ...);
        return a;
    }

    /// @brief Clamp @p v to [lo, hi].
    template <typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Clamp(T v, T lo, T hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    /// @brief Clamp to [0, 1].
    template <std::floating_point T>
    [[nodiscard]] constexpr T Saturate(T v) { return Clamp(v, T(0), T(1)); }
    /// @brief Scalar linear interpolation: `a + (b − a) * t`.
    template <std::floating_point T>
    [[nodiscard]] constexpr T Lerp(T a, T b, T t) { return a + (b - a) * t; }
    /// @brief Sign function: −1, 0, or +1.
    template <typename T>
        requires(std::is_arithmetic_v<T> && std::is_signed_v<T>)
    [[nodiscard]] constexpr T Sign(T x) { return x > T(0) ? T(1) : (x < T(0) ? T(-1) : T(0)); }
    /// @brief Degrees → radians.
    template <std::floating_point T>
    [[nodiscard]] constexpr T Radians(T deg) { return deg * (std::numbers::pi_v<T> / T(180)); }
    /// @brief Radians → degrees.
    template <std::floating_point T>
    [[nodiscard]] constexpr T Degrees(T rad) { return rad * (T(180) / std::numbers::pi_v<T>); }

    /// @}

} // namespace Mashiro::Math
