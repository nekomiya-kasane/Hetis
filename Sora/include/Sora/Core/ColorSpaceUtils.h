/**
 * @file ColorSpaceUtils.h
 * @brief Color mixing, compositing, perceptual distance, gamut, and color-temperature primitives.
 * @ingroup Color
 *
 * @details Signal-space mixing evaluates $C=(1-t)C_0+tC_1$, while @ref MixLinear applies the transfer inverse before
 * interpolation and the transfer function afterward. Porter-Duff source-over uses
 * $A_o=A_s+A_b(1-A_s)$ and $C_o=(C_sA_s+C_bA_b(1-A_s))/A_o$ in scene-linear coordinates. CIE76 is Euclidean
 * distance in CIELAB; CIE94 adds chroma and hue weighting; CIEDE2000 adds hue wrapping, neutral-color handling and
 * the rotation term. Gamut mapping performs a bounded binary search in Oklch while preserving lightness and hue.
 */
#pragma once

#include <Sora/Core/Contracts.h>
#include <Sora/Math/Simd.h>
#include <Sora/Core/ColorSpace.h>

#include <algorithm>
#include <cmath>

namespace Sora::Color {
    namespace Detail {

        /** @name Formula Constants @{ ———————————————————————————————————————————————— */

        /** @brief Return pi as the principal angle from the negative x-axis. */
        template<typename T>
        [[nodiscard]] constexpr T Pi() noexcept {
            using namespace Sora::Math;
            return Atan2(Rational<T, 0>(), Rational<T, -1>());
        }

        /** @brief Return the full turn in radians, tau = 2 pi. */
        template<typename T>
        [[nodiscard]] constexpr T Tau() noexcept {
            return Rational<T, 2>() * Pi<T>();
        }

        /** @brief Return the CIEDE2000 chroma pivot, 25^7. */
        template<typename T>
        [[nodiscard]] constexpr T CiedeChromaPivot() noexcept {
            return Math::Pow(Rational<T, 25>(), Rational<T, 7>());
        }

        /** @brief Convert radians to degrees using 180 degrees per half turn. */
        template<typename T>
        [[nodiscard]] constexpr T RadiansToDegrees() noexcept {
            return Rational<T, 180>() / Pi<T>();
        }

        namespace CIEDE2000 {

            /** @brief Sharma et al. hue weighting coefficient. */
            template<typename T>
            [[nodiscard]] constexpr T HueWeightA() noexcept { return Rational<T, 17, 100>(); }

            /** @brief Sharma et al. double-hue weighting coefficient. */
            template<typename T>
            [[nodiscard]] constexpr T HueWeightB() noexcept { return Rational<T, 24, 100>(); }

            /** @brief Sharma et al. triple-hue weighting coefficient. */
            template<typename T>
            [[nodiscard]] constexpr T HueWeightC() noexcept { return Rational<T, 32, 100>(); }

            /** @brief Sharma et al. quadruple-hue weighting coefficient. */
            template<typename T>
            [[nodiscard]] constexpr T HueWeightD() noexcept { return Rational<T, 20, 100>(); }

            /** @brief Lightness weighting coefficient. */
            template<typename T>
            [[nodiscard]] constexpr T LightnessWeight() noexcept { return Rational<T, 15, 1000>(); }

            /** @brief Chroma weighting coefficient. */
            template<typename T>
            [[nodiscard]] constexpr T ChromaWeight() noexcept { return Rational<T, 45, 1000>(); }

            /** @brief Rotation-term hue center in degrees. */
            template<typename T>
            [[nodiscard]] constexpr T RotationHueCenter() noexcept { return Rational<T, 275>(); }

            /** @brief Rotation-term Gaussian denominator in degrees squared. */
            template<typename T>
            [[nodiscard]] constexpr T RotationGaussianDenominator() noexcept { return Rational<T, 625>(); }

            /** @brief Reference lightness used by the CIEDE2000 S_L weighting function. */
            template<typename T>
            [[nodiscard]] constexpr T ReferenceLightness() noexcept { return Rational<T, 50>(); }

            /** @brief S_L denominator constant. */
            template<typename T>
            [[nodiscard]] constexpr T LightnessDenominator() noexcept { return Rational<T, 20>(); }

        } // namespace CIEDE2000

        /** @} —————————————————————————————————————————————————————————————————— */

        namespace CCT {

            /** @brief Lower bound of the McCamy daylight-locus approximation in kelvin. */
            [[nodiscard]] constexpr float MinimumKelvin() noexcept { return Rational<float, 1667>(); }

            /** @brief Upper bound of the McCamy daylight-locus approximation in kelvin. */
            [[nodiscard]] constexpr float MaximumKelvin() noexcept { return Rational<float, 25000>(); }

            /** @brief Branch point of the two daylight-locus x polynomials in kelvin. */
            [[nodiscard]] constexpr float PolynomialSplitKelvin() noexcept { return Rational<float, 7000>(); }

            /** @brief Evaluate the low-temperature daylight-locus polynomial for x. */
            [[nodiscard]] constexpr float LowTemperatureX(float kelvin) noexcept {
                return Rational<float, -4607000000>() / (kelvin * kelvin * kelvin) +
                       Rational<float, 2967800>() / (kelvin * kelvin) + Rational<float, 9911, 100>() / kelvin +
                       Rational<float, 244063, 1000000>();
            }

            /** @brief Evaluate the high-temperature daylight-locus polynomial for x. */
            [[nodiscard]] constexpr float HighTemperatureX(float kelvin) noexcept {
                return Rational<float, -2006400000>() / (kelvin * kelvin * kelvin) +
                       Rational<float, 1901800>() / (kelvin * kelvin) + Rational<float, 24748, 100>() / kelvin +
                       Rational<float, 237040, 1000000>();
            }

            /** @brief Evaluate McCamy's cubic y(x) daylight-locus polynomial. */
            [[nodiscard]] constexpr float YFromX(float x) noexcept {
                return Rational<float, -3>() * x * x + Rational<float, 287, 100>() * x +
                       Rational<float, -11, 40>();
            }

            /** @brief Return McCamy's reference x coordinate for CCT estimation. */
            template<std::floating_point T>
            [[nodiscard]] constexpr T ReferenceX() noexcept { return Rational<T, 332, 1000>(); }

            /** @brief Return McCamy's reference y coordinate for CCT estimation. */
            template<std::floating_point T>
            [[nodiscard]] constexpr T ReferenceY() noexcept { return Rational<T, 1858, 10000>(); }

            /** @brief Return McCamy's cubic coefficient. */
            template<std::floating_point T>
            [[nodiscard]] constexpr T CubicCoefficient() noexcept { return Rational<T, 449>(); }

            /** @brief Return McCamy's quadratic coefficient. */
            template<std::floating_point T>
            [[nodiscard]] constexpr T QuadraticCoefficient() noexcept { return Rational<T, 3525>(); }

            /** @brief Return McCamy's linear coefficient. */
            template<std::floating_point T>
            [[nodiscard]] constexpr T LinearCoefficient() noexcept { return Rational<T, 68233, 10>(); }

            /** @brief Return McCamy's temperature offset. */
            template<std::floating_point T>
            [[nodiscard]] constexpr T Offset() noexcept { return Rational<T, 552033, 100>(); }

        } // namespace CCT

    } // namespace Detail

    /**
     * @brief Interpolate or extrapolate two colors in their stored signal coordinates by weight @p weight.
     * @details The result is (1-weight) first + weight second; weights outside [0,1] are intentionally permitted.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> Mix(const Color<Space, T>& first, const Color<Space, T>& second,
                                                T weight) noexcept {
        using namespace Sora::Math;
        return Color<Space, T>{first.value * (Detail::One<T>() - weight) + second.value * weight};
    }

    /**
     * @brief Fade a color toward zero in its stored coordinates, with zero at @p amount equal to one.
     * @details This is a signal-space operation. Use @ref MixLinear when zero-light interpolation is required.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> Fade(const Color<Space, T>& color, T amount) noexcept {
        using namespace Sora::Math;
        return Color<Space, T>{color.value * (Detail::One<T>() - amount)};
    }

    /**
     * @brief Reduce opacity without modifying unassociated color channels.
     * @details Alpha becomes (1-amount) alpha and RGB is unchanged.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr ColorA<Space, T> Fade(const ColorA<Space, T>& color, T amount) noexcept {
        using namespace Sora::Math;
        return {color.RGB(), color.alpha * (Detail::One<T>() - amount)};
    }

    /** @brief Scale a color in its stored coordinates by @p amount. */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> Shade(const Color<Space, T>& color, T amount) noexcept {
        return Color<Space, T>{color.value * amount};
    }

    /**
     * @brief Clamp all color channels to the inclusive interval [@p lower,@p upper].
     * @details Alpha is not affected because this overload operates on RGB only.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> Clamp(const Color<Space, T>& color, T lower = Detail::Zero<T>(),
                                                  T upper = Detail::One<T>()) noexcept {
        return Color<Space, T>{Math::Vector<T, 3>{Math::Clamp(color.value[0], lower, upper),
                                                  Math::Clamp(color.value[1], lower, upper),
                                                  Math::Clamp(color.value[2], lower, upper)}};
    }

    /**
     * @brief Interpolate two colors after decoding them to scene-linear coordinates.
     * @details The linear-light result is re-encoded with the same tagged transfer function. Extrapolation is allowed.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> MixLinear(const Color<Space, T>& first, const Color<Space, T>& second,
                                                      T weight) noexcept {
        const auto firstLinear = Decode(SpaceDescription<Space>().transfer, first.value);
        const auto secondLinear = Decode(SpaceDescription<Space>().transfer, second.value);
        return Color<Space, T>{Encode(SpaceDescription<Space>().transfer,
                                      firstLinear * (Detail::One<T>() - weight) + secondLinear * weight)};
    }

    /**
     * @brief Tint a color toward reference white in scene-linear coordinates.
     * @details The result is $E((1-t)E^{-1}(C)+t)$, where $E$ is the color-space transfer function.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> Tint(const Color<Space, T>& color, T amount) noexcept {
        const auto transfer = SpaceDescription<Space>().transfer;
        const auto linear = Decode(transfer, color.value);
        const T one = Detail::One<T>();
        return Color<Space, T>{Encode(transfer, linear * (one - amount) + Math::Vector<T, 3>{one, one, one} * amount)};
    }

    /**
     * @brief Convert a color to a neutral of equal CIE Y luminance.
     * @details For an RGB space normalized to its reference white, $(Y,Y,Y)$ is neutral and preserves relative
     * luminance because the Y row of its NPM sums to one. The result remains in the same tagged signal domain.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> Grayscale(const Color<Space, T>& color) noexcept {
        const T luminance = Luminance(color);
        const auto transfer = SpaceDescription<Space>().transfer;
        return Color<Space, T>{Encode(transfer, Math::Vector<T, 3>{luminance, luminance, luminance})};
    }

    /**
     * @brief Apply photographic exposure in scene-linear coordinates.
     * @details An exposure offset of @p stops multiplies linear light by $2^{stops}$ before re-encoding. Negative
     * stops reduce light and positive stops increase it.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> AdjustExposure(const Color<Space, T>& color, T stops) noexcept {
        const auto transfer = SpaceDescription<Space>().transfer;
        const T scale = Math::Pow(Detail::Rational<T, 2>(), stops);
        return Color<Space, T>{Encode(transfer, Decode(transfer, color.value) * scale)};
    }

    /**
     * @brief Return the WCAG relative-luminance contrast ratio of two opaque colors.
     * @details The ratio is $(L_1+0.05)/(L_2+0.05)$ for lighter luminance $L_1$ and darker luminance $L_2$.
     * Inputs are expected to represent normalized display colors; the result lies in [1,21] for in-gamut SDR colors.
     */
    template<Concept::ColorSpaceTag FirstSpace, Concept::ColorSpaceTag SecondSpace, Concept::ColorCarrier T>
    [[nodiscard]] constexpr T ContrastRatio(const Color<FirstSpace, T>& first,
                                            const Color<SecondSpace, T>& second) noexcept {
        const T firstLuminance = Luminance(first);
        const T secondLuminance = Luminance(second);
        const T lighter = Math::Select(firstLuminance > secondLuminance, firstLuminance, secondLuminance);
        const T darker = Math::Select(firstLuminance > secondLuminance, secondLuminance, firstLuminance);
        const T offset = Detail::Rational<T, 5, 100>();
        return (lighter + offset) / (darker + offset);
    }

    /**
     * @brief Composite @p foreground over @p background using Porter-Duff source-over.
     * @details RGB is decoded, composited premultiplied in scene-linear coordinates, and encoded again. Alpha is
     * independent of chromaticity: $A_o=A_s+A_b(1-A_s)$ and $C_o=(C_sA_s+C_bA_b(1-A_s))/A_o$.
     */
    template<Concept::ColorSpaceTag Destination, Concept::ColorSpaceTag Source, Concept::ColorCarrier T>
    [[nodiscard]] constexpr ColorA<Destination, T> CompositeOver(const ColorA<Destination, T>& background,
                                                                 const ColorA<Source, T>& foreground) noexcept {
        const auto source = Convert<Destination>(foreground);
        const auto transfer = SpaceDescription<Destination>().transfer;
        const auto sourceLinear = Decode(transfer, source.value);
        const auto backgroundLinear = Decode(transfer, background.value);
        const T inverseAlpha = Detail::One<T>() - source.alpha;
        const T outputAlpha = source.alpha + background.alpha * inverseAlpha;
        const T inverseOutput = Math::Select(outputAlpha > Detail::Zero<T>(), Detail::One<T>() / outputAlpha,
                                             Detail::Zero<T>());
        const auto premultiplied = sourceLinear * source.alpha + backgroundLinear * background.alpha * inverseAlpha;
        return {Color<Destination, T>{Encode(transfer, premultiplied * inverseOutput)}, outputAlpha};
    }

    /** @brief Return the relative CIE Y luminance component of a color. */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr T Luminance(const ColorA<Space, T>& color) noexcept {
        return Luminance(color.RGB());
    }

    /**
     * @brief Return Euclidean CIE76 distance in L*a*b* coordinates.
     * @details $\Delta E_{76}=\sqrt{(\Delta L^*)^2+(\Delta a^*)^2+(\Delta b^*)^2}$.
     */
    template<Math::DifferentiableValue T>
    [[nodiscard]] constexpr T CIE76(const Math::Vector<T, 3>& first, const Math::Vector<T, 3>& second) noexcept {
        const auto delta = first - second;
        return Math::Sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    }

    /**
     * @brief Return Euclidean Oklab distance; this is the Oklab Delta E approximation.
     * @details The metric is the Euclidean norm of the three Oklab coordinate differences.
     */
    template<Math::DifferentiableValue T>
    [[nodiscard]] constexpr T OklabDelta(const Math::Vector<T, 3>& first, const Math::Vector<T, 3>& second) noexcept {
        return CIE76(first, second);
    }

    /** @brief Compatibility name for the Euclidean Oklab color difference. */
    template<Math::DifferentiableValue T>
    [[nodiscard]] constexpr T OklabDE(const Math::Vector<T, 3>& first, const Math::Vector<T, 3>& second) noexcept {
        return OklabDelta(first, second);
    }

    /**
     * @brief Return the CIE94 color difference with graphic-arts weighting defaults.
     * @details The default weighting is kL=1, K1=0.045, K2=0.015. These parameters can be changed for other
     * application-specific tolerances. With $C_i=\sqrt{a_i^2+b_i^2}$,
     * $S_L=1$, $S_C=1+K_1C_1$, and $S_H=1+K_2C_1$, the result is
     * $\Delta E_{94}=\sqrt{(\Delta L/(k_LS_L))^2+(\Delta C/S_C)^2+(\Delta H/S_H)^2}$.
     */
    template<Math::DifferentiableValue T>
    [[nodiscard]] constexpr T CIE94(const Math::Vector<T, 3>& first, const Math::Vector<T, 3>& second,
                                    T lightnessWeight = Detail::One<T>(),
                                    T chromaWeight = Detail::CIEDE2000::ChromaWeight<T>(),
                                    T hueWeight = Detail::CIEDE2000::LightnessWeight<T>()) noexcept {
        const T zero = Detail::Zero<T>();
        const T one = Detail::One<T>();
        if constexpr (std::floating_point<T>) {
            SORA_PRE(lightnessWeight > zero);
            SORA_PRE(chromaWeight >= zero);
            SORA_PRE(hueWeight >= zero);
        } else {
            SORA_PRE(Math::Simd::AllOf(lightnessWeight > zero));
            SORA_PRE(Math::Simd::AllOf(chromaWeight >= zero));
            SORA_PRE(Math::Simd::AllOf(hueWeight >= zero));
        }
        const T deltaLightness = first[0] - second[0];
        const T firstChroma = Math::Sqrt(first[1] * first[1] + first[2] * first[2]);
        const T secondChroma = Math::Sqrt(second[1] * second[1] + second[2] * second[2]);
        const T deltaChroma = firstChroma - secondChroma;
        const T deltaA = first[1] - second[1];
        const T deltaB = first[2] - second[2];
        const T deltaHueSquaredRaw = deltaA * deltaA + deltaB * deltaB - deltaChroma * deltaChroma;
        const T deltaHueSquared = Math::Select(deltaHueSquaredRaw < zero, zero, deltaHueSquaredRaw);
        const T chromaScale = one + chromaWeight * firstChroma;
        const T hueScale = one + hueWeight * firstChroma;
        const T lightnessTerm = deltaLightness / lightnessWeight;
        const T chromaTerm = deltaChroma / chromaScale;
        const T hueTerm = Math::Sqrt(deltaHueSquared) / hueScale;
        return Math::Sqrt(lightnessTerm * lightnessTerm + chromaTerm * chromaTerm + hueTerm * hueTerm);
    }

    /**
     * @brief Compute the CIEDE2000 perceptual difference of two CIE L*a*b* values.
     * @details This follows Sharma et al.'s hue-compensated formula, including the G factor, hue-wrap branch, weighting
     * functions, and rotation term. With $\bar C=(C_1+C_2)/2$,
     * $G=\frac12\left(1-\sqrt{\bar C^7/(\bar C^7+25^7)}\right)$ modifies $a$ before computing hue. The final
     * metric is $\Delta E_{00}=\sqrt{(\Delta L'/S_L)^2+(\Delta C'/S_C)^2+(\Delta H'/S_H)^2$
     * $+R_T(\Delta C'/S_C)(\Delta H'/S_H)}$.
     * The result is dimensionless in the customary Delta E units.
     */
    template<Math::DifferentiableValue T>
    [[nodiscard]] constexpr T CIEDE2000(const Math::Vector<T, 3>& first, const Math::Vector<T, 3>& second) noexcept {
        using namespace Sora::Math;
        const T zero = Detail::Zero<T>();
        const T one = Detail::One<T>();
        const T two = Detail::Rational<T, 2>();
        const T three = Detail::Rational<T, 3>();
        const T four = Detail::Rational<T, 4>();
        const T half = Detail::Half<T>();
        const T twentyFiveToSeventh = Detail::CiedeChromaPivot<T>();
        const T tau = Detail::Tau<T>();
        const T pi = Detail::Pi<T>();
        const T l1 = first[0], a1 = first[1], b1 = first[2];
        const T l2 = second[0], a2 = second[1], b2 = second[2];
        const T c1 = Sqrt(a1 * a1 + b1 * b1);
        const T c2 = Sqrt(a2 * a2 + b2 * b2);
        const T cBar = (c1 + c2) * half;
        const T cBar7 = cBar * cBar * cBar * cBar * cBar * cBar * cBar;
        const T g = half * (one - Sqrt(cBar7 / (cBar7 + twentyFiveToSeventh)));
        const T aPrime1 = (one + g) * a1;
        const T aPrime2 = (one + g) * a2;
        const T cPrime1 = Sqrt(aPrime1 * aPrime1 + b1 * b1);
        const T cPrime2 = Sqrt(aPrime2 * aPrime2 + b2 * b2);
        const T rawH1 = Atan2(b1, aPrime1);
        const T rawH2 = Atan2(b2, aPrime2);
        const T h1 = Select(rawH1 < zero, rawH1 + tau, rawH1);
        const T h2 = Select(rawH2 < zero, rawH2 + tau, rawH2);
        const T deltaL = l2 - l1;
        const T deltaC = cPrime2 - cPrime1;
        const T chromaProduct = cPrime1 * cPrime2;
        const T deltaH = Select(
            chromaProduct == zero, zero,
            Select(Abs(h2 - h1) <= pi, h2 - h1, Select(h2 <= h1, h2 - h1 + tau, h2 - h1 - tau)));
        const T deltaBigH = two * Sqrt(chromaProduct) * Sin(deltaH * half);
        const T lBar = (l1 + l2) * half;
        const T cPrimeBar = (cPrime1 + cPrime2) * half;
        const T hBar =
            Select(chromaProduct == zero, h1 + h2,
                         Select(Abs(h1 - h2) <= pi, (h1 + h2) * half,
                                      Select(h1 + h2 < tau, (h1 + h2 + tau) * half, (h1 + h2 - tau) * half)));
        const T degrees30 = Detail::Pi<T>() / Detail::Rational<T, 6>();
        const T degrees6 = Detail::Pi<T>() / Detail::Rational<T, 30>();
        const T degrees63 = Detail::Pi<T>() * Detail::Rational<T, 7>() / Detail::Rational<T, 20>();
        const T t = one - Detail::CIEDE2000::HueWeightA<T>() * Cos(hBar - degrees30) +
                    Detail::CIEDE2000::HueWeightB<T>() * Cos(two * hBar) +
                    Detail::CIEDE2000::HueWeightC<T>() * Cos(three * hBar + degrees6) -
                    Detail::CIEDE2000::HueWeightD<T>() * Cos(four * hBar - degrees63);
        const T lDelta = lBar - Detail::CIEDE2000::ReferenceLightness<T>();
        const T sL = one + Detail::CIEDE2000::LightnessWeight<T>() * lDelta * lDelta /
                               Sqrt(Detail::CIEDE2000::LightnessDenominator<T>() + lDelta * lDelta);
        const T sC = one + Detail::CIEDE2000::ChromaWeight<T>() * cPrimeBar;
        const T sH = one + Detail::CIEDE2000::LightnessWeight<T>() * cPrimeBar * t;
        const T cPrimeBar7 = cPrimeBar * cPrimeBar * cPrimeBar * cPrimeBar * cPrimeBar * cPrimeBar * cPrimeBar;
        const T rC = two * Sqrt(cPrimeBar7 / (cPrimeBar7 + twentyFiveToSeventh));
        const T hBarDegrees = hBar * Detail::RadiansToDegrees<T>();
        const T hOffset = hBarDegrees - Detail::CIEDE2000::RotationHueCenter<T>();
        const T deltaTheta = degrees30 * Exp(
            -(hOffset * hOffset) / Detail::CIEDE2000::RotationGaussianDenominator<T>());
        const T rT = -Sin(two * deltaTheta) * rC;
        const T lTerm = deltaL / sL;
        const T cTerm = deltaC / sC;
        const T hTerm = deltaBigH / sH;
        return Sqrt(lTerm * lTerm + cTerm * cTerm + hTerm * hTerm + rT * cTerm * hTerm);
    }

    namespace DeltaE {

        using Sora::Color::CIE76;
        using Sora::Color::CIE94;
        using Sora::Color::CIEDE2000;
        using Sora::Color::OklabDelta;
        using Sora::Color::OklabDE;

    } // namespace DeltaE

    /**
     * @brief Test whether every RGB lane lies in [0,1] after applying @p tolerance.
     * @details A non-negative tolerance expands both bounds to [-tolerance,1+tolerance]. SIMD carriers return a
     * lane mask; scalar carriers return bool.
     */
    template<Concept::ColorCarrier T>
    [[nodiscard]] constexpr auto InGamut(const Math::Vector<T, 3>& rgb, T tolerance = Detail::Zero<T>()) noexcept {
        if constexpr (std::floating_point<T>) {
            SORA_PRE(tolerance >= Detail::Zero<T>());
        } else {
            SORA_PRE(Math::Simd::AllOf(tolerance >= Detail::Zero<T>()));
        }
        const T lower = -tolerance;
        const T upper = Detail::One<T>() + tolerance;
        if constexpr (std::floating_point<T>) {
            return (rgb[0] >= lower) && (rgb[0] <= upper) && (rgb[1] >= lower) && (rgb[1] <= upper) &&
                   (rgb[2] >= lower) && (rgb[2] <= upper);
        } else {
            return (rgb[0] >= lower) & (rgb[0] <= upper) & (rgb[1] >= lower) & (rgb[1] <= upper) & (rgb[2] >= lower) &
                   (rgb[2] <= upper);
        }
    }

    /** @brief Clamp RGB channels independently to the unit gamut [0,1]. */
    template<Concept::ColorCarrier T>
    [[nodiscard]] constexpr Math::Vector<T, 3> ClampGamut(Math::Vector<T, 3> rgb) noexcept {
        return {Math::Clamp(rgb[0], Detail::Zero<T>(), Detail::One<T>()),
                Math::Clamp(rgb[1], Detail::Zero<T>(), Detail::One<T>()),
                Math::Clamp(rgb[2], Detail::Zero<T>(), Detail::One<T>())};
    }

    /**
     * @brief Approximate the daylight-locus xy chromaticity for a correlated color temperature in kelvin.
     * @details McCamy-style daylight polynomials are used with the valid approximation domain [1667,25000] K;
     * outside values are clamped to that domain. The locus is $y=-3x^2+2.87x-0.275$; $x$ uses the published
     * low-temperature and high-temperature reciprocal-kelvin cubic branches split at 7000 K.
     */
    [[nodiscard]] constexpr Chromaticity CCTToXY(float temperature) noexcept {
        const float t = std::clamp(temperature, Detail::CCT::MinimumKelvin(), Detail::CCT::MaximumKelvin());
        const float x = t <= Detail::CCT::PolynomialSplitKelvin() ? Detail::CCT::LowTemperatureX(t)
                                                                    : Detail::CCT::HighTemperatureX(t);
        return {x, Detail::CCT::YFromX(x)};
    }

    /** @brief Compatibility spelling for @ref CCTToXY. */
    [[nodiscard]] constexpr Chromaticity CCTtoXY(float temperature) noexcept {
        return CCTToXY(temperature);
    }

    /**
     * @brief Return normalized XYZ tristimulus coordinates on the approximate daylight locus.
     * @details Y is normalized to one and X/Z are derived from the CIE xy result.
     */
    [[nodiscard]] constexpr Math::Vector<float, 3> BlackbodyXYZ(float temperature) noexcept {
        const auto chromaticity = CCTToXY(temperature);
        const float one = Detail::One<float>();
        return {chromaticity[0] / chromaticity[1], one, (one - chromaticity[0] - chromaticity[1]) / chromaticity[1]};
    }

    /**
     * @brief Convert a correlated color temperature to a normalized color in @p Space.
     * @details The blackbody approximation is device-independent CIE XYZ with $Y=1$. It is mapped directly through
     * the destination primary matrix and normalized by its maximum linear component before transfer encoding; no
     * chromatic adaptation is applied because the CCT chromaticity is already expressed in the CIE XYZ basis.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Color<Space, T> BlackbodyColor(float temperature) noexcept {
        using namespace Sora::Math;
        const auto xyz = BlackbodyXYZ(temperature);
        const Vector<T, 3> tristimulus{Broadcast<T>(xyz[0]), Broadcast<T>(xyz[1]), Broadcast<T>(xyz[2])};
        auto linearRgb = FromXYZ(SpaceDescription<Space>().WithTransfer(TransferFunction::Linear), tristimulus);
        const T maximum = Math::Select(linearRgb[0] > linearRgb[1],
                                       Math::Select(linearRgb[0] > linearRgb[2], linearRgb[0], linearRgb[2]),
                                       Math::Select(linearRgb[1] > linearRgb[2], linearRgb[1], linearRgb[2]));
        const T inverse =
            Math::Select(maximum > Detail::Zero<T>(), Detail::One<T>() / maximum, Detail::Zero<T>());
        linearRgb = linearRgb * inverse;
        return Color<Space, T>{Encode(SpaceDescription<Space>().transfer, linearRgb)};
    }

    /**
     * @brief Reduce a linear RGB color into a destination gamut by preserving Oklab lightness and hue.
     * @details Chroma is reduced by bounded binary search in Oklch until the destination RGB lies within the supplied
     * tolerance. @p source and @p destination describe linear RGB spaces.
     * @pre @p tolerance is non-negative and @p iterations is positive.
     */
    template<std::floating_point T>
    [[nodiscard]] constexpr Math::Vector<T, 3>
    GamutMapOklab(Math::Vector<T, 3> linearSource, const ColorSpaceDescription& source,
                  const ColorSpaceDescription& destination, T tolerance = Detail::Rational<T, 1, 1000>(),
                  int iterations = Detail::DefaultGamutIterations()) noexcept {
        SORA_PRE(tolerance >= Detail::Zero<T>());
        SORA_PRE(iterations > 0);
        const auto sourceXYZ = BradfordAdaptation<T>(source.white, Illuminant::D65) *
                               ToXYZ(source.WithTransfer(TransferFunction::Linear), linearSource);
        const auto sourceLab = Oklab::FromXYZ(sourceXYZ);
        const T lightness = Math::Clamp(sourceLab[0], Detail::Zero<T>(), Detail::One<T>());
        const T chroma = Math::Sqrt(sourceLab[1] * sourceLab[1] + sourceLab[2] * sourceLab[2]);
        if (chroma <= Detail::Rational<T, 1, 1000000>()) {
            const auto xyz = Oklab::ToXYZ(Math::Vector<T, 3>{lightness, Detail::Zero<T>(), Detail::Zero<T>()});
            return FromXYZ(destination.WithTransfer(TransferFunction::Linear),
                           BradfordAdaptation<T>(Illuminant::D65, destination.white) * xyz);
        }
        const T hue = Math::Atan2(sourceLab[2], sourceLab[1]);
        T lower = Detail::Zero<T>();
        T upper = chroma;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            const T middle = (lower + upper) * Detail::Half<T>();
            const auto lab = Math::Vector<T, 3>{lightness, middle * Math::Cos(hue), middle * Math::Sin(hue)};
            const auto candidateXYZ = BradfordAdaptation<T>(Illuminant::D65, destination.white) * Oklab::ToXYZ(lab);
            const auto rgb = FromXYZ(destination.WithTransfer(TransferFunction::Linear), candidateXYZ);
            if (InGamut(rgb, tolerance)) {
                lower = middle;
            } else {
                upper = middle;
            }
        }
        const auto mapped = Math::Vector<T, 3>{lightness, lower * Math::Cos(hue), lower * Math::Sin(hue)};
        return FromXYZ(destination.WithTransfer(TransferFunction::Linear),
                       BradfordAdaptation<T>(Illuminant::D65, destination.white) * Oklab::ToXYZ(mapped));
    }

    /**
     * @brief Reduce an Oklab color into linear sRGB while preserving its lightness and hue.
     * @details This is the common destination-specialized form of @ref GamutMapOklab.
     */
    template<std::floating_point T>
    [[nodiscard]] constexpr Math::Vector<T, 3> GamutMapToSRGB(
        Math::Vector<T, 3> oklab, T tolerance = Detail::Rational<T, 1, 1000>(),
        int iterations = Detail::DefaultGamutIterations()) noexcept {
        SORA_PRE(tolerance >= Detail::Zero<T>());
        SORA_PRE(iterations > 0);
        const T lightness = Math::Clamp(oklab[0], Detail::Zero<T>(), Detail::One<T>());
        const T chroma = Math::Sqrt(oklab[1] * oklab[1] + oklab[2] * oklab[2]);
        if (chroma <= Detail::Rational<T, 1, 1000000>()) {
            return Oklab::ToLinearSRGB(Math::Vector<T, 3>{lightness, Detail::Zero<T>(), Detail::Zero<T>()});
        }
        const T hue = Math::Atan2(oklab[2], oklab[1]);
        T lower = Detail::Zero<T>();
        T upper = chroma;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            const T middle = (lower + upper) * Detail::Half<T>();
            const auto candidate =
                Oklab::ToLinearSRGB(Math::Vector<T, 3>{lightness, middle * Math::Cos(hue), middle * Math::Sin(hue)});
            if (InGamut(candidate, tolerance)) {
                lower = middle;
            } else {
                upper = middle;
            }
        }
        return Oklab::ToLinearSRGB(Math::Vector<T, 3>{lightness, lower * Math::Cos(hue), lower * Math::Sin(hue)});
    }

    /**
     * @brief Estimate correlated color temperature from xy using McCamy's cubic approximation.
     * @details The estimate is intended for chromaticities near the daylight locus and is returned in kelvin.
     */
    template<std::floating_point T>
    [[nodiscard]] constexpr T CCTFromXY(Chromaticity chromaticity) noexcept {
        const T n = (T{chromaticity[0]} - Detail::CCT::ReferenceX<T>()) /
                    (Detail::CCT::ReferenceY<T>() - T{chromaticity[1]});
        return Detail::CCT::CubicCoefficient<T>() * n * n * n + Detail::CCT::QuadraticCoefficient<T>() * n * n +
               Detail::CCT::LinearCoefficient<T>() * n + Detail::CCT::Offset<T>();
    }

    namespace Named {

        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> Black{Detail::Zero<T>(), Detail::Zero<T>(), Detail::Zero<T>()};
        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> White{Detail::One<T>(), Detail::One<T>(), Detail::One<T>()};
        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> Red{Detail::One<T>(), Detail::Zero<T>(), Detail::Zero<T>()};
        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> Green{Detail::Zero<T>(), Detail::One<T>(), Detail::Zero<T>()};
        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> Blue{Detail::Zero<T>(), Detail::Zero<T>(), Detail::One<T>()};
        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> Yellow{Detail::One<T>(), Detail::One<T>(), Detail::Zero<T>()};
        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> Cyan{Detail::Zero<T>(), Detail::One<T>(), Detail::One<T>()};
        template<Concept::ColorSpaceTag Space = CS::SRGB, Concept::ColorCarrier T = float>
        inline constexpr Color<Space, T> Magenta{Detail::One<T>(), Detail::Zero<T>(), Detail::One<T>()};

    } // namespace Named

} // namespace Sora::Color









