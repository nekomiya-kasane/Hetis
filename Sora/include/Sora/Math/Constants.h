/**
 * @file Constants.h
 * @brief Precision-polymorphic compile-time mathematical constants derived from the standard library.
 * @details Transcendental values are derived from @c std::numbers rather than duplicated decimal literals. The
 * Cody-Waite constants use precision-specific high and residual words for @c float and @c double; other standard
 * floating-point types retain the nominal half-pi value with a zero residual.
 * @ingroup Math
 */
#pragma once

#include <concepts>
#include <numbers>

namespace Sora::Math::Const {

    /** @name Standard Constants @{ */

    /** @brief Euler's number $e$. */
    template<std::floating_point T>
    inline constexpr T kE = std::numbers::e_v<T>;

    /** @brief Binary logarithm of Euler's number $\log_2 e$. */
    template<std::floating_point T>
    inline constexpr T kLog2E = std::numbers::log2e_v<T>;

    /** @brief Common logarithm of Euler's number $\log_{10} e$. */
    template<std::floating_point T>
    inline constexpr T kLog10E = std::numbers::log10e_v<T>;

    /** @brief Archimedes' constant $\pi$. */
    template<std::floating_point T>
    inline constexpr T kPi = std::numbers::pi_v<T>;

    /** @brief Reciprocal of Archimedes' constant $1 / \pi$. */
    template<std::floating_point T>
    inline constexpr T kInvPi = std::numbers::inv_pi_v<T>;

    /** @brief Reciprocal square root of Archimedes' constant $1 / \sqrt{\pi}$. */
    template<std::floating_point T>
    inline constexpr T kInvSqrtPi = std::numbers::inv_sqrtpi_v<T>;

    /** @brief Natural logarithm $\ln 2$. */
    template<std::floating_point T>
    inline constexpr T kLn2 = std::numbers::ln2_v<T>;

    /** @brief Natural logarithm $\ln 10$. */
    template<std::floating_point T>
    inline constexpr T kLn10 = std::numbers::ln10_v<T>;

    /** @brief Positive square root $\sqrt{2}$. */
    template<std::floating_point T>
    inline constexpr T kSqrt2 = std::numbers::sqrt2_v<T>;

    /** @brief Positive square root $\sqrt{3}$. */
    template<std::floating_point T>
    inline constexpr T kSqrt3 = std::numbers::sqrt3_v<T>;

    /** @brief Reciprocal square root $1 / \sqrt{3}$. */
    template<std::floating_point T>
    inline constexpr T kInvSqrt3 = std::numbers::inv_sqrt3_v<T>;

    /** @brief Golden ratio $\varphi = (1 + \sqrt{5}) / 2$. */
    template<std::floating_point T>
    inline constexpr T kPhi = std::numbers::phi_v<T>;

    /** @brief Euler-Mascheroni constant $\gamma$. */
    template<std::floating_point T>
    inline constexpr T kEgamma = std::numbers::egamma_v<T>;

    /** @} */

    /** @name Angular Constants @{ */

    /** @brief Full-turn angle $\tau = 2\pi$. */
    template<std::floating_point T>
    inline constexpr T kTau = T{2} * std::numbers::pi_v<T>;

    /** @brief Quarter-turn angle $\pi / 2$. */
    template<std::floating_point T>
    inline constexpr T kHalfPi = std::numbers::pi_v<T> / T{2};

    /** @brief Angle $\pi / 3$. */
    template<std::floating_point T>
    inline constexpr T kPiOverThree = std::numbers::pi_v<T> / T{3};

    /** @brief Angle $\pi / 4$. */
    template<std::floating_point T>
    inline constexpr T kPiOverFour = std::numbers::pi_v<T> / T{4};

    /** @brief Angle $\pi / 6$. */
    template<std::floating_point T>
    inline constexpr T kPiOverSix = std::numbers::pi_v<T> / T{6};

    /** @brief Ratio $2 / \pi$. */
    template<std::floating_point T>
    inline constexpr T kTwoOverPi = T{2} * std::numbers::inv_pi_v<T>;

    /** @brief Ratio $2 / \sqrt{\pi}$. */
    template<std::floating_point T>
    inline constexpr T kTwoOverSqrtPi = T{2} * std::numbers::inv_sqrtpi_v<T>;

    /** @brief Multiplicative conversion factor from degrees to radians. */
    template<std::floating_point T>
    inline constexpr T kDegToRad = std::numbers::pi_v<T> / T{180};

    /** @brief Multiplicative conversion factor from radians to degrees. */
    template<std::floating_point T>
    inline constexpr T kRadToDeg = T{180} / std::numbers::pi_v<T>;

    /** @} */

    /** @name Algebraic Constants @{ */

    /** @brief Reciprocal square root $1 / \sqrt{2}$. */
    template<std::floating_point T>
    inline constexpr T kInvSqrt2 = T{1} / std::numbers::sqrt2_v<T>;

    /** @brief Inverse golden ratio $1 / \varphi = \varphi - 1$. */
    template<std::floating_point T>
    inline constexpr T kInvPhi = std::numbers::phi_v<T> - T{1};

    /** @} */

    /** @cond INTERNAL */
    namespace Detail {

        template<std::floating_point T>
        [[nodiscard]] consteval T HalfPiHigh() noexcept {
            if constexpr (std::same_as<T, float>) {
                return 1.5707963109016418F;
            } else if constexpr (std::same_as<T, double>) {
                return 1.5707963267948966;
            } else {
                return std::numbers::pi_v<T> / T{2};
            }
        }

        template<std::floating_point T>
        [[nodiscard]] consteval T HalfPiLow() noexcept {
            if constexpr (std::same_as<T, float>) {
                return static_cast<float>(std::numbers::pi_v<double> / 2.0 - static_cast<double>(HalfPiHigh<float>()));
            } else if constexpr (std::same_as<T, double>) {
                return static_cast<double>(std::numbers::pi_v<long double> / 2.0L -
                                           static_cast<long double>(HalfPiHigh<double>()));
            } else {
                return T{0};
            }
        }

    } // namespace Detail
    /** @endcond */

    /** @name Cody-Waite Range-Reduction Constants @{ */

    /**
     * @brief High word of $\pi / 2$ used by Cody-Waite argument reduction.
     * @details For @c float and @c double, the high word supports compensated quadrant subtraction with
     * @ref kHalfPiLo. Wider standard floating-point types retain their nominal half-pi representation.
     */
    template<std::floating_point T>
    inline constexpr T kHalfPiHi = Detail::HalfPiHigh<T>();

    /** @brief Residual satisfying $\texttt{kHalfPiHi} + \texttt{kHalfPiLo} \approx \pi / 2$. */
    template<std::floating_point T>
    inline constexpr T kHalfPiLo = Detail::HalfPiLow<T>();

    /** @} */

    /** @name Trigonometric Reduction Constants @{ */

    /** @brief Tangent $\tan(\pi / 6) = 1 / \sqrt{3}$. */
    template<std::floating_point T>
    inline constexpr T kTanPiOverSix = std::numbers::inv_sqrt3_v<T>;

    /** @brief Tangent $\tan(\pi / 12) = 2 - \sqrt{3}$. */
    template<std::floating_point T>
    inline constexpr T kTanPiOverTwelve = T{2} - std::numbers::sqrt3_v<T>;

    /** @} */

} // namespace Sora::Math::Const
