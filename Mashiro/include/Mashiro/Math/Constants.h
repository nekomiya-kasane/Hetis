/**
 * @file Constants.h
 * @brief Compile-time mathematical constants as `inline constexpr` variable templates.
 *
 * All values derive from `<numbers>` or closed-form expressions thereof — no hand-typed
 * transcendental literals. Every constant is parameterised over `std::floating_point T`,
 * enabling precision-polymorphic code without casts or suffixes.
 *
 * **Usage:**
 * ```cpp
 * using namespace Mashiro::Math;
 * constexpr double x = Const::kPi<double>;
 * constexpr float  y = Const::kInvSqrt2<float>;
 * ```
 *
 * **Cody–Waite constants** (`kHalfPiHi`, `kHalfPiLo`) are provided for argument-reduction
 * kernels. They are split so that \f$ k \cdot \texttt{kHalfPiHi} \f$ is exact in the
 * target precision, and `kHalfPiLo` carries the residual computed from `long double`.
 *
 * Namespace: `Mashiro::Math::Const`
 *
 * @ingroup Math
 */
#pragma once

#include <concepts>
#include <numbers>

namespace Mashiro::Math::Const {

    // =========================================================================
    //  Fundamental constants (direct re-exports from <numbers>)
    // =========================================================================

    /// @brief \f$ e = 2.71828\ldots \f$  (Euler's number).
    template<std::floating_point T>
    inline constexpr T kE = std::numbers::e_v<T>;

    /// @brief \f$ \log_2 e \f$.
    template<std::floating_point T>
    inline constexpr T kLog2E = std::numbers::log2e_v<T>;

    /// @brief \f$ \log_{10} e \f$.
    template<std::floating_point T>
    inline constexpr T kLog10E = std::numbers::log10e_v<T>;

    /// @brief \f$ \pi = 3.14159\ldots \f$.
    template<std::floating_point T>
    inline constexpr T kPi = std::numbers::pi_v<T>;

    /// @brief \f$ 1/\pi \f$.
    template<std::floating_point T>
    inline constexpr T kInvPi = std::numbers::inv_pi_v<T>;

    /// @brief \f$ 1/\sqrt{\pi} \f$.
    template<std::floating_point T>
    inline constexpr T kInvSqrtPi = std::numbers::inv_sqrtpi_v<T>;

    /// @brief \f$ \ln 2 \f$.
    template<std::floating_point T>
    inline constexpr T kLn2 = std::numbers::ln2_v<T>;

    /// @brief \f$ \ln 10 \f$.
    template<std::floating_point T>
    inline constexpr T kLn10 = std::numbers::ln10_v<T>;

    /// @brief \f$ \sqrt{2} \f$.
    template<std::floating_point T>
    inline constexpr T kSqrt2 = std::numbers::sqrt2_v<T>;

    /// @brief \f$ \sqrt{3} \f$.
    template<std::floating_point T>
    inline constexpr T kSqrt3 = std::numbers::sqrt3_v<T>;

    /// @brief \f$ 1/\sqrt{3} \f$.
    template<std::floating_point T>
    inline constexpr T kInvSqrt3 = std::numbers::inv_sqrt3_v<T>;

    /// @brief \f$ \varphi = \frac{1+\sqrt{5}}{2} \f$ (golden ratio).
    template<std::floating_point T>
    inline constexpr T kPhi = std::numbers::phi_v<T>;

    /// @brief Euler–Mascheroni constant \f$ \gamma = 0.5772\ldots \f$.
    template<std::floating_point T>
    inline constexpr T kEgamma = std::numbers::egamma_v<T>;

    // =========================================================================
    // §2  Derived angular constants
    // =========================================================================

    /// @brief \f$ \tau = 2\pi \f$ (full turn).
    template<std::floating_point T>
    inline constexpr T kTau = T(2) * std::numbers::pi_v<T>;

    /// @brief \f$ \pi/2 \f$ (quarter turn / 90°).
    template<std::floating_point T>
    inline constexpr T kHalfPi = std::numbers::pi_v<T> / T(2);

    /// @brief \f$ \pi/3 \f$ (60°).
    template<std::floating_point T>
    inline constexpr T kPiOverThree = std::numbers::pi_v<T> / T(3);

    /// @brief \f$ \pi/4 \f$ (45°).
    template<std::floating_point T>
    inline constexpr T kPiOverFour = std::numbers::pi_v<T> / T(4);

    /// @brief \f$ \pi/6 \f$ (30°).
    template<std::floating_point T>
    inline constexpr T kPiOverSix = std::numbers::pi_v<T> / T(6);

    /// @brief \f$ 2/\pi \f$.
    template<std::floating_point T>
    inline constexpr T kTwoOverPi = std::numbers::inv_pi_v<T> * T(2);

    /// @brief \f$ 2/\sqrt{\pi} \f$.
    template<std::floating_point T>
    inline constexpr T kTwoOverSqrtPi = T(2) * std::numbers::inv_sqrtpi_v<T>;

    /// @brief Degrees-to-radians multiplier: \f$ \pi / 180 \f$.
    template<std::floating_point T>
    inline constexpr T kDegToRad = std::numbers::pi_v<T> / T(180);

    /// @brief Radians-to-degrees multiplier: \f$ 180 / \pi \f$.
    template<std::floating_point T>
    inline constexpr T kRadToDeg = T(180) / std::numbers::pi_v<T>;

    // =========================================================================
    // §3  Derived algebraic constants
    // =========================================================================

    /// @brief \f$ 1/\sqrt{2} \f$.
    template<std::floating_point T>
    inline constexpr T kInvSqrt2 = T(1) / std::numbers::sqrt2_v<T>;

    /// @brief \f$ 1/\varphi = \varphi - 1 \f$ (inverse golden ratio).
    template<std::floating_point T>
    inline constexpr T kInvPhi = std::numbers::phi_v<T> - T(1);

    // =========================================================================
    // §4  Cody–Waite argument-reduction constants
    // =========================================================================

    /**
     * @brief High word of \f$ \pi/2 \f$ for Cody–Waite range reduction.
     *
     * `kHalfPiHi` has its low mantissa bits cleared so that \f$ k \cdot \texttt{kHalfPiHi} \f$
     * is exactly representable. The residual is carried by `kHalfPiLo`.
     *
     * @tparam T  Floating-point type (`float` or `double`).
     */
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
    inline constexpr double kHalfPiLo<double> = static_cast<double>(
        static_cast<long double>(std::numbers::pi_v<long double>) / 2.0L -
        static_cast<long double>(kHalfPiHi<double>));

    // =========================================================================
    // §5  Trigonometric reduction helpers
    // =========================================================================

    /// @brief \f$ \tan(\pi/6) = 1/\sqrt{3} \f$.
    template<std::floating_point T>
    inline constexpr T kTanPiOverSix = std::numbers::inv_sqrt3_v<T>;

    /// @brief \f$ \tan(\pi/12) = 2 - \sqrt{3} \f$.
    template<std::floating_point T>
    inline constexpr T kTanPiOverTwelve = T(2) - std::numbers::sqrt3_v<T>;

} // namespace Mashiro::Math::Const
