// clang-format off
/**
 * @file ColorSpace.h
 * @brief Tag-dispatched color spaces, transfer functions, and color conversions.
 * @ingroup Color
 *
 * @details A chromaticity pair $(x,y)$ is interpreted with unit luminance as
 * $XYZ=(x/y,1,(1-x-y)/y)$. For RGB primaries, the normalized primary matrix has these XYZ columns and is
 * scaled so that $M(1,1,1)^T=W$. A tagged conversion is
 * $RGB_d=E_d(M_d^{-1}A_{s\to d}M_sE_s^{-1}(RGB_s))$, where $E$ is the transfer function and $A$ is Bradford
 * chromatic adaptation. Bradford uses $A=B^{-1}\operatorname{diag}(B W_d / B W_s)B$.
 *
 * The transfer functions implement the published signal laws: sRGB and BT.709 piecewise power curves, pure gamma,
 * SMPTE ST 2084 PQ in absolute cd/m^2 (0--10000), ARIB STD-B67 HLG, and ACEScct. CIELAB, CIELUV, Oklab and Oklch
 * use their standard reference-white and matrix definitions. All numerical carriers are delegated to Math backends,
 * so a fixed-width SIMD carrier follows the same formulas lane-wise without a scalar fallback.
 *
 * RGB colors are stored in the signal coordinates named by their color-space tag. For encoded spaces, conversion first
 * applies the source EOTF, converts through CIE XYZ (D65/D50 adaptation uses Bradford), and applies the destination
 * OETF. Linear tags skip the transfer step. The scalar component is a Math backend carrier, so the same code is
 * usable with a scalar or a fixed-width SIMD value.
 */
// clang-format on
#pragma once

#include <Sora/Core/FixedString.h>
#include <Sora/Core/Contracts.h>
#include <Sora/Math/ConstexprMath.h>
#include <Sora/Math/Matrix.h>
#include <Sora/Math/PrimaryFunctions.h>
#include <Sora/Math/ScalarUtils.h>
#include <Sora/Math/Types.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace Sora::Color {

    namespace $ {

        /**
         * @brief Annotation vocabulary for color-domain metadata.
         * @details `$` is intentionally not a general implementation namespace: entities here are compile-time
         * annotation values consumed by reflection or tooling. Eligibility belongs in `Concept`, and projections
         * belong in `Traits`.
         */

        /** @brief Annotation marking a color value whose components have a transfer-signal interpretation. */
        struct SignalStorage {};

        /** @brief Annotation marking alpha as coverage or opacity rather than a chromatic component. */
        struct AlphaChannel {};

    } // namespace $

    /**
     * @brief CIE 1931 xy chromaticity coordinate.
     * @details This is an affine point alias rather than a second coordinate type. `p[0]` is x and `p[1]` is y;
     * both coordinates are dimensionless. A physical chromaticity satisfies x >= 0, y > 0 and x + y <= 1.
     * With unit luminance Y = 1, the corresponding CIE XYZ tristimulus is
     * $(X,Y,Z)=(x/y,1,(1-x-y)/y)$.
     */
    using Chromaticity = Math::Point2f;

    static_assert(std::same_as<Chromaticity, Math::Point2f>);

    /**
     * @brief Transfer function applied to a color-space signal.
     * @details Encoding maps scene-linear light to a stored signal (OETF); decoding maps the stored signal back to
     * scene-linear light (EOTF). PQ values use absolute luminance in cd/m^2, while the other functions use normalized
     * signal values.
     */
    enum class TransferFunction : uint8_t { Linear, SRGB, sRGB = SRGB, BT709, Gamma22, Gamma26, PQ, HLG, ACEScct };
    using TransferFn = TransferFunction;

    /** @brief Semantic storage domain associated with a color-space tag or runtime description. */
    enum class ColorDomain : uint8_t { EncodedRGB, SceneLinear, Tristimulus };

    namespace Detail {

        /**
         * @brief Evaluate a compile-time rational in a scalar or SIMD carrier.
         * @details The exact source value is \f$n/d\f$; using this function records the defining ratio while keeping
         * conversion to the selected carrier in constant evaluation.
         */
        template<typename T, long long Numerator, long long Denominator = 1>
        [[nodiscard]] constexpr T Rational() noexcept {
            static_assert(Denominator != 0, "a color constant cannot have a zero denominator");
            return Math::Broadcast<T>(Numerator) / Math::Broadcast<T>(Denominator);
        }

        /** @brief Construct a CIE xy coordinate from exact rational coordinates. */
        template<long long XNumerator, long long XDenominator, long long YNumerator, long long YDenominator>
        [[nodiscard]] constexpr Chromaticity ChromaticityOf() noexcept {
            return {static_cast<float>(XNumerator) / static_cast<float>(XDenominator),
                    static_cast<float>(YNumerator) / static_cast<float>(YDenominator)};
        }

        /** @brief Return the additive identity of a scalar or fixed-width SIMD carrier. */
        template<typename T>
        [[nodiscard]] constexpr T Zero() noexcept {
            return Rational<T, 0>();
        }

        /** @brief Return the multiplicative identity of a scalar or fixed-width SIMD carrier. */
        template<typename T>
        [[nodiscard]] constexpr T One() noexcept {
            return Rational<T, 1>();
        }

        /** @brief Return the normalized half used by midpoint formulas. */
        template<typename T>
        [[nodiscard]] constexpr T Half() noexcept {
            return Rational<T, 1, 2>();
        }

    } // namespace Detail

    /**
     * @brief Runtime color-space metadata used by matrix and transfer-function primitives.
     * @details RGB descriptions contain CIE xy primaries and a reference white. Tristimulus descriptions use the
     * identity RGB/XYZ matrix and do not apply a transfer function during conversion.
     */
    struct ColorSpaceDescription {
        Chromaticity red{};
        Chromaticity green{};
        Chromaticity blue{};
        Chromaticity white{};
        TransferFunction transfer = TransferFunction::Linear;
        ColorDomain domain = ColorDomain::SceneLinear;
        FixedString<32> name{};

        /** @brief Return a copy with a different transfer function and derived RGB domain. */
        [[nodiscard]] constexpr ColorSpaceDescription WithTransfer(TransferFunction value) const noexcept {
            auto result = *this;
            result.transfer = value;
            if (result.domain != ColorDomain::Tristimulus) {
                result.domain = value == TransferFunction::Linear ? ColorDomain::SceneLinear : ColorDomain::EncodedRGB;
            }
            return result;
        }

        /** @brief Return a copy with a different reference white. */
        [[nodiscard]] constexpr ColorSpaceDescription WithWhite(Chromaticity value) const noexcept {
            auto result = *this;
            result.white = value;
            return result;
        }

        /** @brief Return a copy with an explicitly selected semantic storage domain. */
        [[nodiscard]] constexpr ColorSpaceDescription WithDomain(ColorDomain value) const noexcept {
            auto result = *this;
            result.domain = value;
            return result;
        }

        [[nodiscard]] friend constexpr bool operator==(const ColorSpaceDescription&,
                                                       const ColorSpaceDescription&) = default;
    };
    using ColorSpaceDesc = ColorSpaceDescription;

    namespace Illuminant {

        /** @name CIE Standard Illuminants @{ —————————————————————————————————————————————— */

        inline constexpr Chromaticity A = Detail::ChromaticityOf<44757, 100000, 40745, 100000>();
        inline constexpr Chromaticity C = Detail::ChromaticityOf<31006, 100000, 31616, 100000>();
        inline constexpr Chromaticity D50 = Detail::ChromaticityOf<34567, 100000, 35850, 100000>();
        inline constexpr Chromaticity D55 = Detail::ChromaticityOf<33242, 100000, 34743, 100000>();
        inline constexpr Chromaticity D65 = Detail::ChromaticityOf<31271, 100000, 32902, 100000>();
        inline constexpr Chromaticity D75 = Detail::ChromaticityOf<29902, 100000, 31485, 100000>();
        inline constexpr Chromaticity E = Detail::ChromaticityOf<1, 3, 1, 3>();
        inline constexpr Chromaticity F2 = Detail::ChromaticityOf<37208, 100000, 37529, 100000>();
        inline constexpr Chromaticity F7 = Detail::ChromaticityOf<31292, 100000, 32933, 100000>();
        inline constexpr Chromaticity F11 = Detail::ChromaticityOf<38052, 100000, 37713, 100000>();
        inline constexpr Chromaticity DCI = Detail::ChromaticityOf<314, 1000, 351, 1000>();
        inline constexpr Chromaticity ACES = Detail::ChromaticityOf<32168, 100000, 33767, 100000>();

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace Illuminant

    namespace Primary {

        /** @name CIE xy Primary Chromaticities @{ ———————————————————————————————————————— */

        inline constexpr Chromaticity SRGBRed = Detail::ChromaticityOf<640, 1000, 330, 1000>();
        inline constexpr Chromaticity SRGBGreen = Detail::ChromaticityOf<300, 1000, 600, 1000>();
        inline constexpr Chromaticity SRGBBlue = Detail::ChromaticityOf<150, 1000, 60, 1000>();
        inline constexpr Chromaticity P3Red = Detail::ChromaticityOf<680, 1000, 320, 1000>();
        inline constexpr Chromaticity P3Green = Detail::ChromaticityOf<265, 1000, 690, 1000>();
        inline constexpr Chromaticity BT2020Red = Detail::ChromaticityOf<708, 1000, 292, 1000>();
        inline constexpr Chromaticity BT2020Green = Detail::ChromaticityOf<170, 1000, 797, 1000>();
        inline constexpr Chromaticity BT2020Blue = Detail::ChromaticityOf<131, 1000, 46, 1000>();
        inline constexpr Chromaticity AdobeRGBGreen = Detail::ChromaticityOf<210, 1000, 710, 1000>();
        inline constexpr Chromaticity ProPhotoRed = Detail::ChromaticityOf<7347, 10000, 2653, 10000>();
        inline constexpr Chromaticity ProPhotoGreen = Detail::ChromaticityOf<1596, 10000, 8404, 10000>();
        inline constexpr Chromaticity ProPhotoBlue = Detail::ChromaticityOf<366, 10000, 1, 10000>();
        inline constexpr Chromaticity ACESAP0Red = Detail::ChromaticityOf<7347, 10000, 2653, 10000>();
        inline constexpr Chromaticity ACESAP0Green = Detail::ChromaticityOf<0, 1, 1, 1>();
        inline constexpr Chromaticity ACESAP0Blue = Detail::ChromaticityOf<1, 10000, -770, 10000>();
        inline constexpr Chromaticity ACESAP1Red = Detail::ChromaticityOf<713, 1000, 293, 1000>();
        inline constexpr Chromaticity ACESAP1Green = Detail::ChromaticityOf<165, 1000, 830, 1000>();
        inline constexpr Chromaticity ACESAP1Blue = Detail::ChromaticityOf<128, 1000, 44, 1000>();
        inline constexpr Chromaticity CIEXYZRed = Detail::ChromaticityOf<1, 1, 0, 1>();
        inline constexpr Chromaticity CIEXYZGreen = Detail::ChromaticityOf<0, 1, 1, 1>();
        inline constexpr Chromaticity CIEXYZBlue = Detail::ChromaticityOf<0, 1, 0, 1>();
        inline constexpr Chromaticity CIERGBRed = Detail::ChromaticityOf<735, 1000, 265, 1000>();
        inline constexpr Chromaticity CIERGBGreen = Detail::ChromaticityOf<274, 1000, 717, 1000>();
        inline constexpr Chromaticity CIERGBBlue = Detail::ChromaticityOf<167, 1000, 9, 1000>();

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace Primary

    namespace CS {

        /** @name Standard Color-Space Tags @{ ——————————————————————————————————————————— */

        struct SRGB;
        struct LinearSRGB;
        struct DisplayP3;
        struct LinearDisplayP3;
        struct DCIP3;
        struct BT709;
        struct BT2020;
        struct BT2100PQ;
        struct BT2100HLG;
        struct AdobeRGB;
        struct ProPhotoRGB;
        struct ACESAP0;
        struct ACESAP1;
        struct ACEScct;
        struct CIEXYZ;
        struct CIERGB;

        using sRGB = SRGB;
        using ACES_AP0 = ACESAP0;
        using ACES_AP1 = ACESAP1;
        using CIE_XYZ = CIEXYZ;
        using CIE_RGB = CIERGB;

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace CS

    namespace Traits {

        /** @brief Primary customization point mapping a color-space tag to its immutable descriptor. */
        template<typename Tag>
        struct ColorSpace;

    } // namespace Traits

    namespace Concept {

        /**
         * @brief Eligibility predicate for a registered color-space tag.
         * @details A tag is valid exactly when its Traits specialization exposes a static `Description` member.
         * This namespace contains predicates only; it does not own derived metadata.
         */
        template<typename Tag>
        concept ColorSpaceTag = requires { Traits::ColorSpace<Tag>::Description; };

        /**
         * @brief Eligibility predicate for scalar or fixed-width SIMD color carriers.
         * @details A carrier must support the differentiable Math operations used by transfer and color metrics.
         */
        template<typename T>
        concept ColorCarrier = Math::DifferentiableValue<T>;

    } // namespace Concept

    namespace Traits {

        /** @brief Stable compile-time descriptor projection for a registered color-space tag. */
        template<Concept::ColorSpaceTag Tag>
        inline constexpr ColorSpaceDescription Description = ColorSpace<Tag>::Description;

        /** @brief Stable storage-domain projection for a registered color-space tag. */
        template<Concept::ColorSpaceTag Tag>
        inline constexpr ColorDomain Domain = ColorSpace<Tag>::Domain;

        /** @brief Stable transfer-function projection for a registered color-space tag. */
        template<Concept::ColorSpaceTag Tag>
        inline constexpr TransferFunction Transfer = ColorSpace<Tag>::Description.transfer;

    } // namespace Traits

    namespace Detail {

        /** @brief Default number of bisections for bounded Oklab gamut mapping. */
        [[nodiscard]] constexpr int DefaultGamutIterations() noexcept {
            return 32;
        }

        /** @brief Tolerance used by compile-time normalized-primary-matrix verification. */
        [[nodiscard]] constexpr float MatrixVerificationTolerance() noexcept {
            return Rational<float, 1, 100000>();
        }

        /** @name Published Transfer-Function Constants @{ ————————————————————————————————— */

        template<typename T>
        [[nodiscard]] constexpr T SRGBLinearSlope() noexcept {
            return Rational<T, 323, 25>();
        }
        template<typename T>
        [[nodiscard]] constexpr T SRGBSignalOffset() noexcept {
            return Rational<T, 11, 200>();
        }
        template<typename T>
        [[nodiscard]] constexpr T SRGBSignalScale() noexcept {
            return Rational<T, 211, 200>();
        }
        template<typename T>
        [[nodiscard]] constexpr T SRGBEotfCut() noexcept {
            return Rational<T, 809, 20000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T SRGBOetfCut() noexcept {
            return Rational<T, 783, 250000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T SRGBGamma() noexcept {
            return Rational<T, 12, 5>();
        }

        template<typename T>
        [[nodiscard]] constexpr T BT709LinearSlope() noexcept {
            return Rational<T, 9, 2>();
        }
        template<typename T>
        [[nodiscard]] constexpr T BT709SignalOffset() noexcept {
            return Rational<T, 99, 1000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T BT709SignalScale() noexcept {
            return Rational<T, 1099, 1000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T BT709EotfCut() noexcept {
            return Rational<T, 81, 1000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T BT709OetfCut() noexcept {
            return Rational<T, 9, 500>();
        }
        template<typename T>
        [[nodiscard]] constexpr T BT709Gamma() noexcept {
            return Rational<T, 9, 20>();
        }

        template<typename T>
        [[nodiscard]] constexpr T PQM1() noexcept {
            return Rational<T, 2610, 16384>();
        }
        template<typename T>
        [[nodiscard]] constexpr T PQM2() noexcept {
            return Rational<T, 2523, 32>();
        }
        template<typename T>
        [[nodiscard]] constexpr T PQC1() noexcept {
            return Rational<T, 3424, 4096>();
        }
        template<typename T>
        [[nodiscard]] constexpr T PQC2() noexcept {
            return Rational<T, 2413, 128>();
        }
        template<typename T>
        [[nodiscard]] constexpr T PQC3() noexcept {
            return Rational<T, 2392, 128>();
        }
        template<typename T>
        [[nodiscard]] constexpr T PQPeakLuminance() noexcept {
            return Rational<T, 10000>();
        }

        template<typename T>
        [[nodiscard]] constexpr T HLG_a() noexcept {
            return Rational<T, 17883277, 100000000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T HLG_b() noexcept {
            return Rational<T, 28466892, 100000000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T HLG_c() noexcept {
            return Rational<T, 55991073, 100000000>();
        }

        template<typename T>
        [[nodiscard]] constexpr T ACEScctLinearSlope() noexcept {
            return Rational<T, 105402377416545, 10000000000000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T ACEScctLinearOffset() noexcept {
            return Rational<T, 729055341958355, 10000000000000000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T ACEScctEncodeCut() noexcept {
            return Rational<T, 1, 128>();
        }
        template<typename T>
        [[nodiscard]] constexpr T ACEScctDecodeCut() noexcept {
            return Rational<T, 155251141552511, 1000000000000000>();
        }
        template<typename T>
        [[nodiscard]] constexpr T ACEScctLogOffset() noexcept {
            return Rational<T, 243, 25>();
        }
        template<typename T>
        [[nodiscard]] constexpr T ACEScctLogDenominator() noexcept {
            return Rational<T, 438, 25>();
        }

        /** @} —————————————————————————————————————————————————————————————————— */

        template<typename Tag, TransferFunction TF>
        struct SpaceDefinition {
            static constexpr ColorDomain Domain =
                std::same_as<Tag, CS::CIEXYZ>
                    ? ColorDomain::Tristimulus
                    : (TF == TransferFunction::Linear ? ColorDomain::SceneLinear : ColorDomain::EncodedRGB);
        };

    } // namespace Detail

#define SORA_COLOR_SPACE(TAG, R, G, B, W, TRANSFER, NAME)                                                              \
    template<>                                                                                                         \
    struct Traits::ColorSpace<CS::TAG> : Detail::SpaceDefinition<CS::TAG, TransferFunction::TRANSFER> {                \
        static constexpr ColorSpaceDescription Description{                                                            \
            R, G, B, W, TransferFunction::TRANSFER, Domain, FixedString<32>(NAME)};                                    \
    }

    SORA_COLOR_SPACE(SRGB, Primary::SRGBRed, Primary::SRGBGreen, Primary::SRGBBlue, Illuminant::D65, SRGB, "sRGB");
    SORA_COLOR_SPACE(LinearSRGB, Primary::SRGBRed, Primary::SRGBGreen, Primary::SRGBBlue, Illuminant::D65, Linear,
                     "Linear sRGB");
    SORA_COLOR_SPACE(DisplayP3, Primary::P3Red, Primary::P3Green, Primary::SRGBBlue, Illuminant::D65, SRGB,
                     "Display P3");
    SORA_COLOR_SPACE(LinearDisplayP3, Primary::P3Red, Primary::P3Green, Primary::SRGBBlue, Illuminant::D65, Linear,
                     "Linear Display P3");
    SORA_COLOR_SPACE(DCIP3, Primary::P3Red, Primary::P3Green, Primary::SRGBBlue, Illuminant::DCI, Gamma26, "DCI-P3");
    SORA_COLOR_SPACE(BT709, Primary::SRGBRed, Primary::SRGBGreen, Primary::SRGBBlue, Illuminant::D65, BT709, "BT.709");
    SORA_COLOR_SPACE(BT2020, Primary::BT2020Red, Primary::BT2020Green, Primary::BT2020Blue, Illuminant::D65, BT709,
                     "BT.2020");
    SORA_COLOR_SPACE(BT2100PQ, Primary::BT2020Red, Primary::BT2020Green, Primary::BT2020Blue, Illuminant::D65, PQ,
                     "BT.2100 PQ");
    SORA_COLOR_SPACE(BT2100HLG, Primary::BT2020Red, Primary::BT2020Green, Primary::BT2020Blue, Illuminant::D65, HLG,
                     "BT.2100 HLG");
    SORA_COLOR_SPACE(AdobeRGB, Primary::SRGBRed, Primary::AdobeRGBGreen, Primary::SRGBBlue, Illuminant::D65, Gamma22,
                     "Adobe RGB");
    SORA_COLOR_SPACE(ProPhotoRGB, Primary::ProPhotoRed, Primary::ProPhotoGreen, Primary::ProPhotoBlue, Illuminant::D50,
                     Gamma22, "ProPhoto RGB");
    SORA_COLOR_SPACE(ACESAP0, Primary::ACESAP0Red, Primary::ACESAP0Green, Primary::ACESAP0Blue, Illuminant::ACES,
                     Linear, "ACES AP0");
    SORA_COLOR_SPACE(ACESAP1, Primary::ACESAP1Red, Primary::ACESAP1Green, Primary::ACESAP1Blue, Illuminant::ACES,
                     Linear, "ACES AP1");
    SORA_COLOR_SPACE(ACEScct, Primary::ACESAP1Red, Primary::ACESAP1Green, Primary::ACESAP1Blue, Illuminant::ACES,
                     ACEScct, "ACEScct");
    SORA_COLOR_SPACE(CIEXYZ, Primary::CIEXYZRed, Primary::CIEXYZGreen, Primary::CIEXYZBlue, Illuminant::E, Linear,
                     "CIE XYZ");
    SORA_COLOR_SPACE(CIERGB, Primary::CIERGBRed, Primary::CIERGBGreen, Primary::CIERGBBlue, Illuminant::E, Linear,
                     "CIE RGB");

#undef SORA_COLOR_SPACE

    /**
     * @brief Return the immutable compile-time description associated with @p Tag.
     * @tparam Tag A registered color-space tag.
     */
    template<Concept::ColorSpaceTag Tag>
    [[nodiscard]] consteval const ColorSpaceDescription& SpaceDescription() noexcept {
        return Traits::ColorSpace<Tag>::Description;
    }

    namespace Meta {

        /** @name Compile-Time Color-Space Reflection @{ ————————————————————————————————— */

        /** @brief Return the source-level identifier of a color-space tag through reflection. */
        template<Concept::ColorSpaceTag Tag>
        [[nodiscard]] consteval auto TagName() {
            return FixedString<64>{std::meta::identifier_of(^^Tag)};
        }

        /** @brief Return the user-facing display name stored in the tag descriptor. */
        template<Concept::ColorSpaceTag Tag>
        [[nodiscard]] consteval auto DisplayName() {
            return Traits::Description<Tag>.name;
        }

        /** @brief Test whether a tag stores transfer-encoded RGB signals. */
        template<Concept::ColorSpaceTag Tag>
        [[nodiscard]] consteval bool IsEncoded() noexcept {
            return Traits::Domain<Tag> == ColorDomain::EncodedRGB;
        }

        /** @brief Test whether a tag stores scene-linear RGB values. */
        template<Concept::ColorSpaceTag Tag>
        [[nodiscard]] consteval bool IsSceneLinear() noexcept {
            return Traits::Domain<Tag> == ColorDomain::SceneLinear;
        }

        /** @brief Test whether a tag denotes CIE XYZ-like tristimulus values. */
        template<Concept::ColorSpaceTag Tag>
        [[nodiscard]] consteval bool IsTristimulus() noexcept {
            return Traits::Domain<Tag> == ColorDomain::Tristimulus;
        }

        /** @brief Return the storage domain of a color-space tag. */
        template<Concept::ColorSpaceTag Tag>
        [[nodiscard]] consteval ColorDomain Domain() noexcept {
            return Traits::Domain<Tag>;
        }

        /** @brief Return the transfer function associated with a color-space tag. */
        template<Concept::ColorSpaceTag Tag>
        [[nodiscard]] consteval TransferFunction Transfer() noexcept {
            return Traits::Transfer<Tag>;
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace Meta

    namespace TF {

        /** @name Transfer-Function Primitives @{ ————————————————————————————————————————— */

        /**
         * @brief Return the real signed cube root of a scalar or SIMD carrier.
         * @details Unlike a fractional power, negative inputs remain finite and retain their sign. This is required
         * by Oklab and CIELUV and is evaluated lane-wise for fixed-width SIMD carriers.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T Cbrt(T value) noexcept {
            const T magnitude = Math::AbsValue(value);
            const T root = Math::Pow(magnitude, Detail::One<T>() / Detail::Rational<T, 3>());
            return Math::Select(value < Detail::Zero<T>(), Math::Neg(root), root);
        }

        /**
         * @brief Decode an sRGB signal using IEC 61966-2-1.
         * @details The linear branch is v/12.92 for v <= 0.04045; the power branch is
         * ((v+0.055)/1.055)^2.4. Negative extended values use the linear branch.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T SRGB_EOTF(T value) noexcept {
            const T zero = Detail::Zero<T>();
            const T positive = Math::Select(value > zero, value, zero);
            const T low = Math::Div(value, Detail::SRGBLinearSlope<T>());
            const T high =
                Math::Pow(Math::Div(Math::Add(positive, Detail::SRGBSignalOffset<T>()), Detail::SRGBSignalScale<T>()),
                          Detail::SRGBGamma<T>());
            return Math::Select(value <= Detail::SRGBEotfCut<T>(), low, high);
        }

        /**
         * @brief Encode scene-linear light using the IEC 61966-2-1 sRGB OETF.
         * @details The linear branch is 12.92v for v <= 0.0031308; the power branch is
         * 1.055v^(1/2.4)-0.055. Negative extended values use the linear branch.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T SRGB_OETF(T value) noexcept {
            const T zero = Detail::Zero<T>();
            const T positive = Math::Select(value > zero, value, zero);
            const T low = Math::Mul(value, Detail::SRGBLinearSlope<T>());
            const T high = Math::Sub(
                Math::Mul(Detail::SRGBSignalScale<T>(), Math::Pow(positive, Detail::One<T>() / Detail::SRGBGamma<T>())),
                Detail::SRGBSignalOffset<T>());
            return Math::Select(value <= Detail::SRGBOetfCut<T>(), low, high);
        }

        /** @brief Compatibility spelling for the IEC 61966-2-1 sRGB EOTF. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T sRGB_EOTF(T value) noexcept {
            return SRGB_EOTF(value);
        }

        /** @brief Compatibility spelling for the IEC 61966-2-1 sRGB OETF. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T sRGB_OETF(T value) noexcept {
            return SRGB_OETF(value);
        }

        /**
         * @brief Decode a BT.709/BT.2020 normalized signal.
         * @details The linear branch is v/4.5 for v < 0.081; the power branch is
         * ((v+0.099)/1.099)^(1/0.45).
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T BT709_EOTF(T value) noexcept {
            const T zero = Detail::Zero<T>();
            const T positive = Math::Select(value > zero, value, zero);
            const T low = Math::Div(value, Detail::BT709LinearSlope<T>());
            const T high =
                Math::Pow(Math::Div(Math::Add(positive, Detail::BT709SignalOffset<T>()), Detail::BT709SignalScale<T>()),
                          Detail::One<T>() / Detail::BT709Gamma<T>());
            return Math::Select(value < Detail::BT709EotfCut<T>(), low, high);
        }

        /**
         * @brief Encode normalized scene-linear light with the BT.709 OETF.
         * @details The linear branch is 4.5v for v < 0.018; the power branch is 1.099v^0.45-0.099.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T BT709_OETF(T value) noexcept {
            const T zero = Detail::Zero<T>();
            const T positive = Math::Select(value > zero, value, zero);
            const T low = Math::Mul(value, Detail::BT709LinearSlope<T>());
            const T high =
                Math::Sub(Math::Mul(Detail::BT709SignalScale<T>(), Math::Pow(positive, Detail::BT709Gamma<T>())),
                          Detail::BT709SignalOffset<T>());
            return Math::Select(value < Detail::BT709OetfCut<T>(), low, high);
        }

        /** @brief Encode positive scene-linear light as v^(1/gamma); non-positive values map to zero. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T GammaEncode(T value, T gamma) noexcept {
            const T zero = Detail::Zero<T>();
            const T positive = Math::Select(value > zero, value, zero);
            return Math::Select(value <= zero, zero, Math::Pow(positive, Math::Div(Detail::One<T>(), gamma)));
        }

        /** @brief Decode a positive pure-gamma signal as v^gamma; non-positive values map to zero. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T GammaDecode(T value, T gamma) noexcept {
            return Math::Select(value <= Detail::Zero<T>(), Detail::Zero<T>(), Math::Pow(value, gamma));
        }

        /**
         * @brief Decode SMPTE ST 2084 Perceptual Quantizer values.
         * @details Input is normalized to [0,1] and output is absolute luminance in [0,10000] cd/m^2. Inputs outside
         * the nominal range are saturated before evaluation.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T PQ_EOTF(T value) noexcept {
            const T m1 = Detail::PQM1<T>();
            const T m2 = Detail::PQM2<T>();
            const T c1 = Detail::PQC1<T>();
            const T c2 = Detail::PQC2<T>();
            const T c3 = Detail::PQC3<T>();
            const T powered = Math::Pow(Math::Saturate(value), Math::Div(Detail::One<T>(), m2));
            const T numerator = Math::Clamp(Math::Sub(powered, c1), Detail::Zero<T>(), powered);
            return Math::Mul(
                Math::Pow(Math::Div(numerator, Math::Sub(c2, Math::Mul(c3, powered))), Math::Div(Detail::One<T>(), m1)),
                Detail::PQPeakLuminance<T>());
        }

        /**
         * @brief Encode absolute luminance for SMPTE ST 2084.
         * @details Input is interpreted in cd/m^2 and saturated to [0,10000]; output is a normalized signal.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T PQ_OETF(T value) noexcept {
            const T m1 = Detail::PQM1<T>();
            const T m2 = Detail::PQM2<T>();
            const T c1 = Detail::PQC1<T>();
            const T c2 = Detail::PQC2<T>();
            const T c3 = Detail::PQC3<T>();
            const T powered = Math::Pow(Math::Saturate(Math::Div(value, Detail::PQPeakLuminance<T>())), m1);
            return Math::Pow(
                Math::Div(Math::Add(c1, Math::Mul(c2, powered)), Math::Add(Detail::One<T>(), Math::Mul(c3, powered))),
                m2);
        }

        /**
         * @brief Encode normalized scene-linear light with ARIB STD-B67 HLG.
         * @details The low branch is sqrt(3v) for v <= 1/12; the high branch is a log curve above that threshold.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T HLG_OETF(T value) noexcept {
            const T a = Detail::HLG_a<T>();
            const T b = Detail::HLG_b<T>();
            const T c = Detail::HLG_c<T>();
            const T zero = Detail::Zero<T>();
            const T threshold = Detail::Rational<T, 1, 12>();
            const T positive = Math::Select(value > zero, value, zero);
            const T low = Math::Sqrt(Detail::Rational<T, 3>() * positive);
            const T logarithmInput =
                Math::Select(value > threshold, Math::Sub(Detail::Rational<T, 12>() * value, b), Detail::One<T>());
            const T high = Math::Add(Math::Mul(a, Math::Log(logarithmInput)), c);
            return Math::Select(value <= threshold, low, high);
        }

        /**
         * @brief Decode an ARIB STD-B67 HLG signal.
         * @details The quadratic branch is v^2/3 for v <= 1/2; the high branch is
         * (exp((v-c)/a)+b)/12.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T HLG_EOTF(T value) noexcept {
            const T a = Detail::HLG_a<T>();
            const T b = Detail::HLG_b<T>();
            const T c = Detail::HLG_c<T>();
            return Math::Select(value <= Detail::Rational<T, 1, 2>(),
                                Math::Div(value * value, Detail::Rational<T, 3>()),
                                Math::Div(Math::Add(Math::Exp(Math::Div(value - c, a)), b), Detail::Rational<T, 12>()));
        }

        /**
         * @brief Encode ACES scene-linear values with the ACEScct logarithmic curve.
         * @details Values at or below 0.0078125 use
         * $y=10.5402377416545x+0.0729055341958355$; larger values use
         * $y=(\log_2(x)+9.72)/17.52$.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T ACEScct_Encode(T value) noexcept {
            const T cut = Detail::ACEScctEncodeCut<T>();
            const T low = Detail::ACEScctLinearSlope<T>() * value + Detail::ACEScctLinearOffset<T>();
            const T logarithmInput = Math::Select(value > Detail::Zero<T>(), value, cut);
            const T high =
                (Math::Log(logarithmInput) / Math::Log(Detail::Rational<T, 2>()) + Detail::ACEScctLogOffset<T>()) /
                Detail::ACEScctLogDenominator<T>();
            return Math::Select(value <= cut, low, high);
        }

        /**
         * @brief Decode ACEScct values into scene-linear values.
         * @details Values at or below 0.155251141552511 use
         * $x=(y-0.0729055341958355)/10.5402377416545$; larger values use
         * $x=2^{17.52y-9.72}$.
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T ACEScct_Decode(T value) noexcept {
            const T cut = Detail::ACEScctDecodeCut<T>();
            const T low = (value - Detail::ACEScctLinearOffset<T>()) / Detail::ACEScctLinearSlope<T>();
            const T high = Math::Pow(Detail::Rational<T, 2>(),
                                     value * Detail::ACEScctLogDenominator<T>() - Detail::ACEScctLogOffset<T>());
            return Math::Select(value <= cut, low, high);
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace TF

    /** @brief Return the signed cube root used by perceptual color-space transforms. */
    template<Concept::ColorCarrier T>
    [[nodiscard]] constexpr T Cbrt(T value) noexcept {
        return TF::Cbrt(value);
    }

    /** @brief Return whether @p function names one of the supported transfer functions. */
    [[nodiscard]] constexpr bool IsValidTransferFunction(TransferFunction function) noexcept {
        switch (function) {
            case TransferFunction::Linear:
            case TransferFunction::SRGB:
            case TransferFunction::BT709:
            case TransferFunction::Gamma22:
            case TransferFunction::Gamma26:
            case TransferFunction::PQ:
            case TransferFunction::HLG:
            case TransferFunction::ACEScct:
                return true;
        }
        return false;
    }

    /**
     * @brief Apply a transfer function in the encoding direction.
     * @param[in] function Transfer function selected by the color-space description.
     * @param[in] value Scene-linear carrier; PQ uses cd/m^2.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr T Encode(TransferFunction function, T value) noexcept {
        SORA_PRE(IsValidTransferFunction(function));
        switch (function) {
            case TransferFunction::Linear:
                return value;
            case TransferFunction::SRGB:
                return TF::SRGB_OETF(value);
            case TransferFunction::BT709:
                return TF::BT709_OETF(value);
            case TransferFunction::Gamma22:
                return TF::GammaEncode(value, Detail::Rational<T, 11, 5>());
            case TransferFunction::Gamma26:
                return TF::GammaEncode(value, Detail::Rational<T, 13, 5>());
            case TransferFunction::PQ:
                return TF::PQ_OETF(value);
            case TransferFunction::HLG:
                return TF::HLG_OETF(value);
            case TransferFunction::ACEScct:
                return TF::ACEScct_Encode(value);
        }
        return value;
    }

    /**
     * @brief Apply a transfer function in the decoding direction.
     * @param[in] function Transfer function selected by the color-space description.
     * @param[in] value Stored signal carrier; PQ returns cd/m^2.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr T Decode(TransferFunction function, T value) noexcept {
        SORA_PRE(IsValidTransferFunction(function));
        switch (function) {
            case TransferFunction::Linear:
                return value;
            case TransferFunction::SRGB:
                return TF::SRGB_EOTF(value);
            case TransferFunction::BT709:
                return TF::BT709_EOTF(value);
            case TransferFunction::Gamma22:
                return TF::GammaDecode(value, Detail::Rational<T, 11, 5>());
            case TransferFunction::Gamma26:
                return TF::GammaDecode(value, Detail::Rational<T, 13, 5>());
            case TransferFunction::PQ:
                return TF::PQ_EOTF(value);
            case TransferFunction::HLG:
                return TF::HLG_EOTF(value);
            case TransferFunction::ACEScct:
                return TF::ACEScct_Decode(value);
        }
        return value;
    }

    /** @brief Apply @p function independently to the three components of an RGB carrier. */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Vector<T, 3> Encode(TransferFunction function, Math::Vector<T, 3> value) noexcept {
        return {Encode(function, value[0]), Encode(function, value[1]), Encode(function, value[2])};
    }

    /** @brief Decode @p function independently for the three components of an RGB carrier. */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Vector<T, 3> Decode(TransferFunction function, Math::Vector<T, 3> value) noexcept {
        return {Decode(function, value[0]), Decode(function, value[1]), Decode(function, value[2])};
    }

    namespace Detail {

        /** @brief Convert a chromaticity and unit luminance to the corresponding XYZ white point. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> WhiteXYZ(Chromaticity white) noexcept {
            const float unitLuminance = Detail::One<float>();
            return {Math::Broadcast<T>(white[0] / white[1]), Detail::One<T>(),
                    Math::Broadcast<T>((unitLuminance - white[0] - white[1]) / white[1])};
        }

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Matrix<T, 3, 3> Bradford() noexcept {
            return Math::Matrix<T, 3, 3>{Detail::Rational<T, 8951, 10000>(), Detail::Rational<T, 333, 1250>(),
                                         Detail::Rational<T, -807, 5000>(),  Detail::Rational<T, -3751, 5000>(),
                                         Detail::Rational<T, 3427, 2000>(),  Detail::Rational<T, 367, 10000>(),
                                         Detail::Rational<T, 389, 10000>(),  Detail::Rational<T, -137, 2000>(),
                                         Detail::Rational<T, 1287, 1250>()};
        }

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Matrix<T, 3, 3> BradfordInverse() noexcept {
            return Math::Matrix<T, 3, 3>{
                Detail::Rational<T, 9869929, 10000000>(), Detail::Rational<T, -1470543, 10000000>(),
                Detail::Rational<T, 1599627, 10000000>(), Detail::Rational<T, 4323053, 10000000>(),
                Detail::Rational<T, 5183603, 10000000>(), Detail::Rational<T, 492912, 10000000>(),
                Detail::Rational<T, -85287, 10000000>(),  Detail::Rational<T, 400428, 10000000>(),
                Detail::Rational<T, 9684867, 10000000>()};
        }

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Matrix<T, 3, 3> Inverse3(const Math::Matrix<T, 3, 3>& m) noexcept {
            const T a = m(0, 0), b = m(0, 1), c = m(0, 2), d = m(1, 0), e = m(1, 1), f = m(1, 2), g = m(2, 0),
                    h = m(2, 1), i = m(2, 2);
            const T determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            return Math::Matrix<T, 3, 3>{
                (e * i - f * h) / determinant, (c * h - b * i) / determinant, (b * f - c * e) / determinant,
                (f * g - d * i) / determinant, (a * i - c * g) / determinant, (c * d - a * f) / determinant,
                (d * h - e * g) / determinant, (b * g - a * h) / determinant, (a * e - b * d) / determinant};
        }

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Matrix<T, 3, 3> Primaries(const ColorSpaceDescription& space) noexcept {
            return Math::Matrix<T, 3, 3>{Math::Broadcast<T>(space.red[0] / space.red[1]),
                                         Math::Broadcast<T>(space.green[0] / space.green[1]),
                                         Math::Broadcast<T>(space.blue[0] / space.blue[1]),
                                         Detail::Rational<T, 1>(),
                                         Detail::Rational<T, 1>(),
                                         Detail::Rational<T, 1>(),
                                         Math::Broadcast<T>((1 - space.red[0] - space.red[1]) / space.red[1]),
                                         Math::Broadcast<T>((1 - space.green[0] - space.green[1]) / space.green[1]),
                                         Math::Broadcast<T>((1 - space.blue[0] - space.blue[1]) / space.blue[1])};
        }

    } // namespace Detail

    /**
     * @brief Compute Bradford chromatic adaptation from one reference white to another.
     * @details The returned matrix is B^-1 diag((B W_d)/(B W_s)) B, applied to XYZ column vectors. Equal whites
     * return the identity matrix. The carrier may be scalar or fixed-width SIMD.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Matrix<T, 3, 3> BradfordAdaptation(Chromaticity source, Chromaticity destination) {
        if (source == destination) {
            return Math::Matrix<T, 3, 3>::Identity();
        }
        const auto bradford = Detail::Bradford<T>();
        const auto sourceCone = bradford * Detail::WhiteXYZ<T>(source);
        const auto destinationCone = bradford * Detail::WhiteXYZ<T>(destination);
        Math::Matrix<T, 3, 3> scale = Math::Matrix<T, 3, 3>::Identity();
        scale(0, 0) = destinationCone[0] / sourceCone[0];
        scale(1, 1) = destinationCone[1] / sourceCone[1];
        scale(2, 2) = destinationCone[2] / sourceCone[2];
        return Detail::BradfordInverse<T>() * scale * bradford;
    }

    /**
     * @brief Compute the normalized primary matrix for a runtime color-space description.
     * @details Its columns are the XYZ coordinates of the red, green, and blue unit primaries, scaled so their sum
     * equals the description's reference white. Tristimulus descriptions return identity.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Matrix<T, 3, 3> NPM(const ColorSpaceDescription& space) {
        if (space.domain == ColorDomain::Tristimulus) {
            return Math::Matrix<T, 3, 3>::Identity();
        }
        const auto primary = Detail::Primaries<T>(space);
        const auto scale = Detail::Inverse3(primary) * Detail::WhiteXYZ<T>(space.white);
        Math::Matrix<T, 3, 3> result = primary;
        result(0, 0) *= scale[0];
        result(1, 0) *= scale[0];
        result(2, 0) *= scale[0];
        result(0, 1) *= scale[1];
        result(1, 1) *= scale[1];
        result(2, 1) *= scale[1];
        result(0, 2) *= scale[2];
        result(1, 2) *= scale[2];
        result(2, 2) *= scale[2];
        return result;
    }

    /** @brief Return the inverse normalized primary matrix, mapping XYZ to the description's RGB basis. */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Matrix<T, 3, 3> InverseNPM(const ColorSpaceDescription& space) {
        return Detail::Inverse3(NPM<T>(space));
    }

    /** @brief Return the RGB-to-XYZ matrix for a runtime description. */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Matrix<T, 3, 3> RGBtoXYZ(const ColorSpaceDescription& space) {
        return NPM<T>(space);
    }

    /** @brief Return the XYZ-to-RGB matrix for a runtime description. */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Matrix<T, 3, 3> XYZtoRGB(const ColorSpaceDescription& space) {
        return InverseNPM<T>(space);
    }

    /**
     * @brief Return the complete linear conversion matrix between two runtime descriptions.
     * @details The result includes the source primary matrix, Bradford white-point adaptation, and destination
     * inverse primary matrix. Tristimulus descriptions are treated as XYZ rather than degenerate RGB.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Matrix<T, 3, 3> RGBtoRGB(const ColorSpaceDescription& source,
                                                           const ColorSpaceDescription& destination) {
        const auto adaptation = BradfordAdaptation<T>(source.white, destination.white);
        if (source.domain == ColorDomain::Tristimulus && destination.domain == ColorDomain::Tristimulus) {
            return adaptation;
        }
        if (source.domain == ColorDomain::Tristimulus) {
            return InverseNPM<T>(destination) * adaptation;
        }
        if (destination.domain == ColorDomain::Tristimulus) {
            return adaptation * NPM<T>(source);
        }
        return InverseNPM<T>(destination) * adaptation * NPM<T>(source);
    }

    namespace Detail {

        template<Concept::ColorSpaceTag Space>
        consteval bool VerifyNPMWhitePoint() {
            if constexpr (Traits::Domain<Space> == ColorDomain::Tristimulus) {
                return true;
            } else {
                const auto actual = NPM<float>(SpaceDescription<Space>()) *
                                    Math::Vector<float, 3>{One<float>(), One<float>(), One<float>()};
                const auto expected = WhiteXYZ<float>(SpaceDescription<Space>().white);
                const auto delta = actual - expected;
                return Math::Dot(delta, delta) < MatrixVerificationTolerance();
            }
        }

        template<Concept::ColorSpaceTag Space>
        consteval bool VerifyNPMRoundTrip() {
            if constexpr (Traits::Domain<Space> == ColorDomain::Tristimulus) {
                return true;
            } else {
                const auto product =
                    InverseNPM<float>(SpaceDescription<Space>()) * NPM<float>(SpaceDescription<Space>());
                const auto identity = Math::Matrix<float, 3, 3>::Identity();
                float error = Zero<float>();
                for (std::size_t row = 0; row < 3; ++row) {
                    for (std::size_t column = 0; column < 3; ++column) {
                        const float difference = product(row, column) - identity(row, column);
                        error += difference * difference;
                    }
                }
                return error < MatrixVerificationTolerance();
            }
        }

        static_assert(VerifyNPMWhitePoint<CS::SRGB>());
        static_assert(VerifyNPMWhitePoint<CS::LinearSRGB>());
        static_assert(VerifyNPMWhitePoint<CS::DisplayP3>());
        static_assert(VerifyNPMWhitePoint<CS::LinearDisplayP3>());
        static_assert(VerifyNPMWhitePoint<CS::DCIP3>());
        static_assert(VerifyNPMWhitePoint<CS::BT709>());
        static_assert(VerifyNPMWhitePoint<CS::BT2020>());
        static_assert(VerifyNPMWhitePoint<CS::BT2100PQ>());
        static_assert(VerifyNPMWhitePoint<CS::BT2100HLG>());
        static_assert(VerifyNPMWhitePoint<CS::AdobeRGB>());
        static_assert(VerifyNPMWhitePoint<CS::ProPhotoRGB>());
        static_assert(VerifyNPMWhitePoint<CS::ACESAP0>());
        static_assert(VerifyNPMWhitePoint<CS::ACESAP1>());
        static_assert(VerifyNPMWhitePoint<CS::ACEScct>());
        static_assert(VerifyNPMWhitePoint<CS::CIERGB>());
        static_assert(VerifyNPMRoundTrip<CS::SRGB>());

    } // namespace Detail

    /**
     * @brief Strongly typed three-channel color value.
     * @details @p Space is part of the type, preventing accidental arithmetic between different primaries or transfer
     * functions. @p T may be a scalar or a fixed-width Math SIMD carrier; the stored components retain the signal
     * domain named by @p Space.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T = float>
    struct Color {
        using SpaceTag = Space;
        using CarrierType = T;
        using VectorType = Math::Vector<T, 3>;

        [[= Sora::Color::$::SignalStorage{}]] VectorType value{};

        constexpr Color() noexcept = default;
        constexpr explicit Color(VectorType components) noexcept : value(std::move(components)) {}
        constexpr Color(T red, T green, T blue) noexcept : value(red, green, blue) {}

        [[nodiscard]] constexpr T& r() noexcept { return value[0]; }
        [[nodiscard]] constexpr T& g() noexcept { return value[1]; }
        [[nodiscard]] constexpr T& b() noexcept { return value[2]; }
        [[nodiscard]] constexpr const T& r() const noexcept { return value[0]; }
        [[nodiscard]] constexpr const T& g() const noexcept { return value[1]; }
        [[nodiscard]] constexpr const T& b() const noexcept { return value[2]; }
        [[nodiscard]] constexpr T& operator[](std::size_t index) noexcept { return value[index]; }
        [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept { return value[index]; }
        [[nodiscard]] static consteval const ColorSpaceDescription& Description() noexcept {
            return SpaceDescription<Space>();
        }
        [[nodiscard]] static consteval TransferFunction Transfer() noexcept {
            return SpaceDescription<Space>().transfer;
        }

        [[nodiscard]] friend constexpr bool operator==(const Color&, const Color&) = default;
        [[nodiscard]] constexpr Color operator+(const Color& other) const noexcept {
            return Color{value + other.value};
        }
        [[nodiscard]] constexpr Color operator-(const Color& other) const noexcept {
            return Color{value - other.value};
        }
        [[nodiscard]] constexpr Color operator*(T scale) const noexcept { return Color{value * scale}; }
        [[nodiscard]] constexpr Color operator/(T scale) const noexcept { return Color{value / scale}; }
        constexpr Color& operator+=(const Color& other) noexcept {
            value += other.value;
            return *this;
        }
        constexpr Color& operator-=(const Color& other) noexcept {
            value -= other.value;
            return *this;
        }
        constexpr Color& operator*=(T scale) noexcept {
            value *= scale;
            return *this;
        }
        constexpr Color& operator/=(T scale) noexcept {
            value /= scale;
            return *this;
        }
    };

    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Space, T> operator*(T scale, const Color<Space, T>& color) noexcept {
        return color * scale;
    }

    /**
     * @brief Convert a tagged color through scene-linear CIE XYZ into another tagged color space.
     * @details Source decoding, Bradford adaptation, destination primary inversion, and destination encoding are
     * all selected at compile time for tags. Alpha is intentionally handled by the separate ColorA overload.
     */
    template<Concept::ColorSpaceTag Destination, Concept::ColorSpaceTag Source, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Color<Destination, T> Convert(const Color<Source, T>& source) noexcept {
        if constexpr (std::same_as<Destination, Source>) {
            return source;
        } else {
            Math::Vector<T, 3> xyz;
            if constexpr (Traits::Domain<Source> == ColorDomain::Tristimulus) {
                xyz = source.value;
            } else {
                xyz = NPM<T>(SpaceDescription<Source>()) * Decode(SpaceDescription<Source>().transfer, source.value);
            }
            if constexpr (SpaceDescription<Source>().white != SpaceDescription<Destination>().white) {
                xyz = BradfordAdaptation<T>(SpaceDescription<Source>().white, SpaceDescription<Destination>().white) *
                      xyz;
            }
            if constexpr (Traits::Domain<Destination> == ColorDomain::Tristimulus) {
                return Color<Destination, T>{xyz};
            } else {
                return Color<Destination, T>{Encode(SpaceDescription<Destination>().transfer,
                                                    InverseNPM<T>(SpaceDescription<Destination>()) * xyz)};
            }
        }
    }

    /**
     * @brief Convert a runtime-described RGB or XYZ vector to a compile-time destination tag.
     * @details This is the dynamic-description counterpart of the tagged @ref Convert overload and retains SIMD
     * lane-wise execution.
     */
    template<Concept::ColorSpaceTag Destination, Concept::ColorCarrier T>
    [[nodiscard]] constexpr Math::Vector<T, 3> ConvertVec3(const ColorSpaceDescription& source,
                                                           Math::Vector<T, 3> value) noexcept {
        const auto xyz =
            source.domain == ColorDomain::Tristimulus ? value : NPM<T>(source) * Decode(source.transfer, value);
        const auto adapted = BradfordAdaptation<T>(source.white, SpaceDescription<Destination>().white) * xyz;
        if constexpr (Traits::Domain<Destination> == ColorDomain::Tristimulus) {
            return adapted;
        } else {
            return Encode(SpaceDescription<Destination>().transfer,
                          InverseNPM<T>(SpaceDescription<Destination>()) * adapted);
        }
    }

    /**
     * @brief Decode a runtime-described color vector into CIE XYZ tristimulus coordinates.
     * @details For RGB descriptions the transfer function is decoded before applying the normalized primary matrix;
     * tristimulus descriptions pass through unchanged.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Vector<T, 3> ToXYZ(const ColorSpaceDescription& space,
                                                     Math::Vector<T, 3> value) noexcept {
        if (space.domain == ColorDomain::Tristimulus) {
            return value;
        }
        return NPM<T>(space) * Decode(space.transfer, value);
    }

    /**
     * @brief Encode CIE XYZ tristimulus coordinates into a runtime-described color vector.
     * @details Tristimulus descriptions pass through unchanged; RGB descriptions apply the inverse primary matrix
     * followed by the selected transfer function.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr Math::Vector<T, 3> FromXYZ(const ColorSpaceDescription& space,
                                                       Math::Vector<T, 3> value) noexcept {
        if (space.domain == ColorDomain::Tristimulus) {
            return value;
        }
        return Encode(space.transfer, InverseNPM<T>(space) * value);
    }

    /**
     * @brief Return relative luminance, the Y component of the decoded CIE XYZ vector.
     * @details Luminance is normalized to the description's reference white and is independent of alpha.
     */
    template<Concept::ColorCarrier T = float>
    [[nodiscard]] constexpr T Luminance(const ColorSpaceDescription& space, Math::Vector<T, 3> value) noexcept {
        return ToXYZ(space, value)[1];
    }

    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr T Luminance(const Color<Space, T>& color) noexcept {
        return ToXYZ(SpaceDescription<Space>(), color.value)[1];
    }

    namespace Lab {

        /** @name CIELAB Transfer Functions @{ ——————————————————————————————————————————— */
        /** @brief Apply the CIELAB piecewise f(t) function with the 6/29 breakpoint. */

        template<Concept::ColorCarrier T = float>
        [[nodiscard]] constexpr T F(T value) noexcept {
            constexpr T delta = Detail::Rational<T, 6>() / Detail::Rational<T, 29>();
            return Math::Select(value > delta * delta * delta, TF::Cbrt(value),
                                value / (Detail::Rational<T, 3>() * delta * delta) +
                                    Detail::Rational<T, 4>() / Detail::Rational<T, 29>());
        }

        /** @brief Apply the inverse CIELAB f^-1(t) function to recover a normalized XYZ component. */
        template<Concept::ColorCarrier T = float>
        [[nodiscard]] constexpr T FInv(T value) noexcept {
            constexpr T delta = Detail::Rational<T, 6>() / Detail::Rational<T, 29>();
            return Math::Select(value > delta, value * value * value,
                                Detail::Rational<T, 3>() * delta * delta *
                                    (value - Detail::Rational<T, 4>() / Detail::Rational<T, 29>()));
        }

        /**
         * @brief Convert CIE XYZ to CIELAB relative to @p white.
         * @details The result is (L*,a*,b*) with L*=116 f(Y/Y_n)-16, a*=500(f(X/X_n)-f(Y/Y_n)), and
         * b*=200(f(Y/Y_n)-f(Z/Z_n)).
         */
        template<Concept::ColorCarrier T = float>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromXYZ(Math::Vector<T, 3> xyz,
                                                           Chromaticity white = Illuminant::D65) noexcept {
            const auto reference = Detail::WhiteXYZ<T>(white);
            const T fx = F(xyz[0] / reference[0]);
            const T fy = F(xyz[1] / reference[1]);
            const T fz = F(xyz[2] / reference[2]);
            return {Detail::Rational<T, 116>() * fy - Detail::Rational<T, 16>(), Detail::Rational<T, 500>() * (fx - fy),
                    Detail::Rational<T, 200>() * (fy - fz)};
        }

        /** @brief Convert CIELAB coordinates back to CIE XYZ relative to @p white. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToXYZ(Math::Vector<T, 3> lab,
                                                         Chromaticity white = Illuminant::D65) noexcept {
            const auto reference = Detail::WhiteXYZ<T>(white);
            const T fy = (lab[0] + Detail::Rational<T, 16>()) / Detail::Rational<T, 116>();
            const T fx = lab[1] / Detail::Rational<T, 500>() + fy;
            const T fz = fy - lab[2] / Detail::Rational<T, 200>();
            return {reference[0] * FInv(fx), reference[1] * FInv(fy), reference[2] * FInv(fz)};
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace Lab

    namespace Luv {

        /** @name CIELUV Coordinates @{ ———————————————————————————————————————————————— */
        /** @brief Return the CIE 1976 u' coordinate, safely returning zero for a zero denominator. */

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T Up(Math::Vector<T, 3> xyz) noexcept {
            const T denominator = xyz[0] + Detail::Rational<T, 15>() * xyz[1] + Detail::Rational<T, 3>() * xyz[2];
            const T safeDenominator =
                Math::Select(denominator > Detail::Rational<T, 0>(), denominator, Detail::Rational<T, 1>());
            return Math::Select(denominator > Detail::Rational<T, 0>(),
                                Detail::Rational<T, 4>() * xyz[0] / safeDenominator, Detail::Rational<T, 0>());
        }

        /** @brief Return the CIE 1976 v' coordinate, safely returning zero for a zero denominator. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr T Vp(Math::Vector<T, 3> xyz) noexcept {
            const T denominator = xyz[0] + Detail::Rational<T, 15>() * xyz[1] + Detail::Rational<T, 3>() * xyz[2];
            const T safeDenominator =
                Math::Select(denominator > Detail::Rational<T, 0>(), denominator, Detail::Rational<T, 1>());
            return Math::Select(denominator > Detail::Rational<T, 0>(),
                                Detail::Rational<T, 9>() * xyz[1] / safeDenominator, Detail::Rational<T, 0>());
        }

        /**
         * @brief Convert CIE XYZ to CIELUV relative to @p white.
         * @details The result is (L*,u*,v*) with u*=13L*(u'-u'_n) and v*=13L*(v'-v'_n).
         */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromXYZ(Math::Vector<T, 3> xyz,
                                                           Chromaticity white = Illuminant::D65) noexcept {
            const T y = xyz[1];
            const T lightness = Math::Select(y > Detail::Rational<T, 8856451679, 1000000000000>(),
                                             Detail::Rational<T, 116>() * TF::Cbrt(y) - Detail::Rational<T, 16>(),
                                             Detail::Rational<T, 9033, 10>() * y);
            const auto reference = Detail::WhiteXYZ<T>(white);
            return {lightness, Detail::Rational<T, 13>() * lightness * (Up(xyz) - Up(reference)),
                    Detail::Rational<T, 13>() * lightness * (Vp(xyz) - Vp(reference))};
        }

        /** @brief Convert CIELUV coordinates back to CIE XYZ relative to @p white. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToXYZ(Math::Vector<T, 3> luv,
                                                         Chromaticity white = Illuminant::D65) noexcept {
            const auto reference = Detail::WhiteXYZ<T>(white);
            const T lightness = luv[0];
            const T zero = Detail::Rational<T, 0>();
            const T safeLightness = Math::Select(lightness > zero, lightness, Detail::Rational<T, 1>());
            const T up = luv[1] / (Detail::Rational<T, 13>() * safeLightness) + Up(reference);
            const T vp = luv[2] / (Detail::Rational<T, 13>() * safeLightness) + Vp(reference);
            const T y = Math::Select(lightness > Detail::Rational<T, 8>(),
                                     Math::Pow((safeLightness + Detail::Rational<T, 16>()) / Detail::Rational<T, 116>(),
                                               Detail::Rational<T, 3>()),
                                     lightness / Detail::Rational<T, 9033, 10>());
            const T safeVp = Math::Select(vp != zero, vp, Detail::Rational<T, 1>());
            const T x = y * Detail::Rational<T, 9>() * up / (Detail::Rational<T, 4>() * safeVp);
            const T z = y *
                        (Detail::Rational<T, 12>() - Detail::Rational<T, 3>() * up - Detail::Rational<T, 20>() * vp) /
                        (Detail::Rational<T, 4>() * safeVp);
            return {Math::Select(lightness > zero, x, zero), Math::Select(lightness > zero, y, zero),
                    Math::Select(lightness > zero, z, zero)};
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace Luv

    namespace Oklab {

        /** @name Oklab Coordinates @{ ———————————————————————————————————————————————— */
        /** @brief Convert CIE XYZ tristimulus values to Oklab using the Ottosson LMS cube-root transform. */

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromXYZ(Math::Vector<T, 3> xyz) noexcept {
            const T l = TF::Cbrt(Detail::Rational<T, 8189330101, 10000000000>() * xyz[0] +
                                 Detail::Rational<T, 3618667424, 10000000000>() * xyz[1] -
                                 Detail::Rational<T, 1288597137, 10000000000>() * xyz[2]);
            const T m = TF::Cbrt(Detail::Rational<T, 329845436, 10000000000>() * xyz[0] +
                                 Detail::Rational<T, 9293118715, 10000000000>() * xyz[1] +
                                 Detail::Rational<T, 361456387, 10000000000>() * xyz[2]);
            const T s = TF::Cbrt(Detail::Rational<T, 482003018, 10000000000>() * xyz[0] +
                                 Detail::Rational<T, 2643662691, 10000000000>() * xyz[1] +
                                 Detail::Rational<T, 633851707, 10000000000>() * xyz[2]);
            return {
                Detail::Rational<T, 2104542553, 10000000000>() * l + Detail::Rational<T, 793617785, 10000000000>() * m -
                    Detail::Rational<T, 40720468, 10000000000>() * s,
                Detail::Rational<T, 19779984951, 10000000000>() * l -
                    Detail::Rational<T, 24285922050, 10000000000>() * m +
                    Detail::Rational<T, 4505937099, 10000000000>() * s,
                Detail::Rational<T, 259040371, 10000000000>() * l + Detail::Rational<T, 7827717662, 10000000000>() * m -
                    Detail::Rational<T, 8086757660, 10000000000>() * s};
        }

        /** @brief Convert Oklab coordinates back to CIE XYZ using the inverse LMS cube transform. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToXYZ(Math::Vector<T, 3> lab) noexcept {
            const T l = lab[0] + Detail::Rational<T, 3963377774, 10000000000>() * lab[1] +
                        Detail::Rational<T, 2158037573, 10000000000>() * lab[2];
            const T m = lab[0] - Detail::Rational<T, 1055613458, 10000000000>() * lab[1] -
                        Detail::Rational<T, 638541728, 10000000000>() * lab[2];
            const T s = lab[0] - Detail::Rational<T, 894841775, 10000000000>() * lab[1] -
                        Detail::Rational<T, 12914855480, 10000000000>() * lab[2];
            const T l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
            return {Detail::Rational<T, 12268798734, 10000000000>() * l3 -
                        Detail::Rational<T, 5578149966, 10000000000>() * m3 +
                        Detail::Rational<T, 2813910502, 10000000000>() * s3,
                    -Detail::Rational<T, 405757626, 10000000000>() * l3 +
                        Detail::Rational<T, 11122868294, 10000000000>() * m3 -
                        Detail::Rational<T, 717110667, 10000000000>() * s3,
                    -Detail::Rational<T, 763729497, 10000000000>() * l3 -
                        Detail::Rational<T, 4214933239, 10000000000>() * m3 +
                        Detail::Rational<T, 15869240244, 10000000000>() * s3};
        }

        /** @brief Convert linear sRGB directly to Oklab using the optimized fixed matrix path. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromLinearSRGB(Math::Vector<T, 3> rgb) noexcept {
            return FromXYZ(Math::Vector<T, 3>{Detail::Rational<T, 4122214708, 10000000000>() * rgb[0] +
                                                  Detail::Rational<T, 5363325363, 10000000000>() * rgb[1] +
                                                  Detail::Rational<T, 514459929, 10000000000>() * rgb[2],
                                              Detail::Rational<T, 2119034982, 10000000000>() * rgb[0] +
                                                  Detail::Rational<T, 6806995451, 10000000000>() * rgb[1] +
                                                  Detail::Rational<T, 1073969566, 10000000000>() * rgb[2],
                                              Detail::Rational<T, 883024619, 10000000000>() * rgb[0] +
                                                  Detail::Rational<T, 2817188376, 10000000000>() * rgb[1] +
                                                  Detail::Rational<T, 6299787005, 10000000000>() * rgb[2]});
        }

        /** @brief Convert Oklab directly to linear sRGB using the optimized fixed matrix path. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToLinearSRGB(Math::Vector<T, 3> lab) noexcept {
            const T l = lab[0] + Detail::Rational<T, 3963377774, 10000000000>() * lab[1] +
                        Detail::Rational<T, 2158037573, 10000000000>() * lab[2];
            const T m = lab[0] - Detail::Rational<T, 1055613458, 10000000000>() * lab[1] -
                        Detail::Rational<T, 638541728, 10000000000>() * lab[2];
            const T s = lab[0] - Detail::Rational<T, 894841775, 10000000000>() * lab[1] -
                        Detail::Rational<T, 12914855480, 10000000000>() * lab[2];
            const T l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
            return {Detail::Rational<T, 40767416621, 10000000000>() * l3 -
                        Detail::Rational<T, 33077115913, 10000000000>() * m3 +
                        Detail::Rational<T, 2309699292, 10000000000>() * s3,
                    -Detail::Rational<T, 12684380046, 10000000000>() * l3 +
                        Detail::Rational<T, 26097574011, 10000000000>() * m3 -
                        Detail::Rational<T, 3413193965, 10000000000>() * s3,
                    -Detail::Rational<T, 41960863, 10000000000>() * l3 -
                        Detail::Rational<T, 7034186147, 10000000000>() * m3 +
                        Detail::Rational<T, 17076147010, 10000000000>() * s3};
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace Oklab

    namespace Oklch {

        /** @name Oklch Coordinates @{ ———————————————————————————————————————————————— */
        /** @brief Convert Cartesian Oklab (L,a,b) to cylindrical (L,C,h), with h in radians. */

        /** @brief Convert Cartesian Oklab coordinates to cylindrical Oklch (L,C,h), with h in radians. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromOklab(Math::Vector<T, 3> lab) noexcept {
            return {lab[0], Math::Sqrt(lab[1] * lab[1] + lab[2] * lab[2]), Math::Atan2(lab[2], lab[1])};
        }

        /** @brief Convert cylindrical Oklch coordinates back to Cartesian Oklab. */
        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToOklab(Math::Vector<T, 3> lch) noexcept {
            return {lch[0], lch[1] * Math::Cos(lch[2]), lch[1] * Math::Sin(lch[2])};
        }

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr auto FromLinearSRGB(Math::Vector<T, 3> rgb) noexcept {
            return FromOklab(Oklab::FromLinearSRGB(rgb));
        }

        template<Concept::ColorCarrier T>
        [[nodiscard]] constexpr auto ToLinearSRGB(Math::Vector<T, 3> lch) noexcept {
            return Oklab::ToLinearSRGB(ToOklab(lch));
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace Oklch

    namespace HSV {

        /** @name HSV Coordinates @{ ———————————————————————————————————————————————— */
        /** @brief HSV cylindrical coordinates: hue and saturation are normalized to [0,1], value is signal maximum. */

        template<std::floating_point T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromRGB(Math::Vector<T, 3> rgb) noexcept {
            const T maximum = Math::Select(rgb[0] > rgb[1], Math::Select(rgb[0] > rgb[2], rgb[0], rgb[2]),
                                           Math::Select(rgb[1] > rgb[2], rgb[1], rgb[2]));
            const T minimum = Math::Select(rgb[0] < rgb[1], Math::Select(rgb[0] < rgb[2], rgb[0], rgb[2]),
                                           Math::Select(rgb[1] < rgb[2], rgb[1], rgb[2]));
            const T delta = maximum - minimum;
            const T zero = Detail::Rational<T, 0>();
            const T safeMaximum = Math::Select(maximum > zero, maximum, Detail::Rational<T, 1>());
            const T safeDelta = Math::Select(delta > zero, delta, Detail::Rational<T, 1>());
            const T saturation = Math::Select(maximum > zero, delta / safeMaximum, zero);
            const T redHue =
                (rgb[1] - rgb[2]) / safeDelta + Math::Select(rgb[1] < rgb[2], Detail::Rational<T, 6>(), zero);
            const T greenHue = (rgb[2] - rgb[0]) / safeDelta + Detail::Rational<T, 2>();
            const T blueHue = (rgb[0] - rgb[1]) / safeDelta + Detail::Rational<T, 4>();
            const T hue = Math::Select(
                delta <= zero, zero,
                Math::Select(maximum == rgb[0], redHue, Math::Select(maximum == rgb[1], greenHue, blueHue)) /
                    Detail::Rational<T, 6>());
            return {hue, saturation, maximum};
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToRGB(Math::Vector<T, 3> hsv) noexcept {
            const T h = hsv[0] * Detail::Rational<T, 6>();
            if constexpr (std::floating_point<T>) {
                const int sector = static_cast<int>(h);
                const T f = h - Math::Broadcast<T>(sector);
                const T p = hsv[2] * (Detail::Rational<T, 1>() - hsv[1]);
                const T q = hsv[2] * (Detail::Rational<T, 1>() - hsv[1] * f);
                const T t = hsv[2] * (Detail::Rational<T, 1>() - hsv[1] * (Detail::Rational<T, 1>() - f));
                switch ((sector % 6 + 6) % 6) {
                    case 0:
                        return {hsv[2], t, p};
                    case 1:
                        return {q, hsv[2], p};
                    case 2:
                        return {p, hsv[2], t};
                    case 3:
                        return {p, q, hsv[2]};
                    case 4:
                        return {t, p, hsv[2]};
                    default:
                        return {hsv[2], p, q};
                }
            } else {
                return {hsv[2], hsv[2], hsv[2]};
            }
        }

        /** @brief Convert RGB to HSV for a fixed-width floating SIMD carrier. */
        template<Math::FloatingSimdValue T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromRGB(Math::Vector<T, 3> rgb) noexcept {
            const T maximum = Math::Select(rgb[0] > rgb[1], Math::Select(rgb[0] > rgb[2], rgb[0], rgb[2]),
                                           Math::Select(rgb[1] > rgb[2], rgb[1], rgb[2]));
            const T minimum = Math::Select(rgb[0] < rgb[1], Math::Select(rgb[0] < rgb[2], rgb[0], rgb[2]),
                                           Math::Select(rgb[1] < rgb[2], rgb[1], rgb[2]));
            const T zero = Detail::Rational<T, 0>();
            const T delta = maximum - minimum;
            const T safeDelta = Math::Select(delta > zero, delta, Detail::Rational<T, 1>());
            const T safeMaximum = Math::Select(maximum > zero, maximum, Detail::Rational<T, 1>());
            const T redHue =
                (rgb[1] - rgb[2]) / safeDelta + Math::Select(rgb[1] < rgb[2], Detail::Rational<T, 6>(), zero);
            const T greenHue = (rgb[2] - rgb[0]) / safeDelta + Detail::Rational<T, 2>();
            const T blueHue = (rgb[0] - rgb[1]) / safeDelta + Detail::Rational<T, 4>();
            const T hue = Math::Select(
                delta > zero,
                Math::Select(maximum == rgb[0], redHue, Math::Select(maximum == rgb[1], greenHue, blueHue)) /
                    Detail::Rational<T, 6>(),
                zero);
            return {hue, Math::Select(maximum > zero, delta / safeMaximum, zero), maximum};
        }

        /** @brief Convert HSV to RGB for a fixed-width floating SIMD carrier. */
        template<Math::FloatingSimdValue T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToRGB(Math::Vector<T, 3> hsv) noexcept {
            const T zero = Detail::Rational<T, 0>();
            const T one = Detail::Rational<T, 1>();
            const T six = Detail::Rational<T, 6>();
            T sector = Math::Saturate(hsv[0]) * six;
            sector = Math::Select(sector >= six, zero, sector);
            const T p = hsv[2] * (one - hsv[1]);
            const T f0 = sector;
            const T f1 = sector - one;
            const T f2 = sector - Detail::Rational<T, 2>();
            const T f3 = sector - Detail::Rational<T, 3>();
            const T f4 = sector - Detail::Rational<T, 4>();
            const T f5 = sector - Detail::Rational<T, 5>();
            const auto q = [&](T fraction) constexpr { return hsv[2] * (one - hsv[1] * fraction); };
            const auto t = [&](T fraction) constexpr { return hsv[2] * (one - hsv[1] * (one - fraction)); };
            const Math::Vector<T, 3> c0{hsv[2], t(f0), p};
            const Math::Vector<T, 3> c1{q(f1), hsv[2], p};
            const Math::Vector<T, 3> c2{p, hsv[2], t(f2)};
            const Math::Vector<T, 3> c3{p, q(f3), hsv[2]};
            const Math::Vector<T, 3> c4{t(f4), p, hsv[2]};
            const Math::Vector<T, 3> c5{hsv[2], p, q(f5)};
            const auto selectVector = [](auto condition, const Math::Vector<T, 3>& yes,
                                         const Math::Vector<T, 3>& no) constexpr {
                return Math::Vector<T, 3>{Math::Select(condition, yes[0], no[0]),
                                          Math::Select(condition, yes[1], no[1]),
                                          Math::Select(condition, yes[2], no[2])};
            };
            return selectVector(
                sector < one, c0,
                selectVector(sector < Detail::Rational<T, 2>(), c1,
                             selectVector(sector < Detail::Rational<T, 3>(), c2,
                                          selectVector(sector < Detail::Rational<T, 4>(), c3,
                                                       selectVector(sector < Detail::Rational<T, 5>(), c4, c5)))));
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace HSV

    namespace HSL {

        /** @name HSL Coordinates @{ ———————————————————————————————————————————————— */
        /** @brief HSL cylindrical coordinates with normalized hue, saturation, and lightness components. */

        template<std::floating_point T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromRGB(Math::Vector<T, 3> rgb) noexcept {
            const T maximum = std::max({rgb[0], rgb[1], rgb[2]});
            const T minimum = std::min({rgb[0], rgb[1], rgb[2]});
            const T delta = maximum - minimum;
            const T lightness = (maximum + minimum) * Detail::Half<T>();
            const T denominator =
                lightness > Detail::Half<T>() ? Detail::Rational<T, 2>() - maximum - minimum : maximum + minimum;
            const T saturation = delta == Detail::Rational<T, 0>() ? Detail::Rational<T, 0>() : delta / denominator;
            T hue = Detail::Rational<T, 0>();
            if (delta != Detail::Rational<T, 0>()) {
                if (maximum == rgb[0]) {
                    hue = (rgb[1] - rgb[2]) / delta +
                          (rgb[1] < rgb[2] ? Detail::Rational<T, 6>() : Detail::Rational<T, 0>());
                } else if (maximum == rgb[1]) {
                    hue = (rgb[2] - rgb[0]) / delta + Detail::Rational<T, 2>();
                } else {
                    hue = (rgb[0] - rgb[1]) / delta + Detail::Rational<T, 4>();
                }
                hue /= Detail::Rational<T, 6>();
            }
            return {hue, saturation, lightness};
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToRGB(Math::Vector<T, 3> hsl) noexcept {
            if constexpr (std::floating_point<T>) {
                if (hsl[1] == Detail::Rational<T, 0>()) {
                    return {hsl[2], hsl[2], hsl[2]};
                }
                const T q = hsl[2] < Detail::Half<T>() ? hsl[2] * (Detail::One<T>() + hsl[1])
                                                       : hsl[2] + hsl[1] - hsl[2] * hsl[1];
                const T p = Detail::Rational<T, 2>() * hsl[2] - q;
                const auto hue = [p, q](T value) constexpr {
                    value += value < Detail::Rational<T, 0>() ? Detail::Rational<T, 1>() : Detail::Rational<T, 0>();
                    value -= value > Detail::Rational<T, 1>() ? Detail::Rational<T, 1>() : Detail::Rational<T, 0>();
                    return value < Detail::Rational<T, 1>() / Detail::Rational<T, 6>()
                               ? p + (q - p) * Detail::Rational<T, 6>() * value
                           : value < Detail::Rational<T, 1>() / Detail::Rational<T, 2>() ? q
                           : value < Detail::Rational<T, 2>() / Detail::Rational<T, 3>()
                               ? p + (q - p) * (Detail::Rational<T, 2>() / Detail::Rational<T, 3>() - value) *
                                         Detail::Rational<T, 6>()
                               : p;
                };
                return {hue(hsl[0] + Detail::Rational<T, 1>() / Detail::Rational<T, 3>()), hue(hsl[0]),
                        hue(hsl[0] - Detail::Rational<T, 1>() / Detail::Rational<T, 3>())};
            } else {
                return {hsl[2], hsl[2], hsl[2]};
            }
        }

        /** @brief Convert RGB to HSL for a fixed-width floating SIMD carrier. */
        template<Math::FloatingSimdValue T>
        [[nodiscard]] constexpr Math::Vector<T, 3> FromRGB(Math::Vector<T, 3> rgb) noexcept {
            const T maximum = Math::Select(rgb[0] > rgb[1], Math::Select(rgb[0] > rgb[2], rgb[0], rgb[2]),
                                           Math::Select(rgb[1] > rgb[2], rgb[1], rgb[2]));
            const T minimum = Math::Select(rgb[0] < rgb[1], Math::Select(rgb[0] < rgb[2], rgb[0], rgb[2]),
                                           Math::Select(rgb[1] < rgb[2], rgb[1], rgb[2]));
            const T zero = Detail::Rational<T, 0>();
            const T delta = maximum - minimum;
            const T lightness = (maximum + minimum) * Detail::Half<T>();
            const T denominator = Math::Select(lightness > Detail::Half<T>(),
                                               Detail::Rational<T, 2>() - maximum - minimum, maximum + minimum);
            const T safeDenominator = Math::Select(denominator != zero, denominator, Detail::One<T>());
            const T safeDelta = Math::Select(delta > zero, delta, Detail::Rational<T, 1>());
            const T redHue =
                (rgb[1] - rgb[2]) / safeDelta + Math::Select(rgb[1] < rgb[2], Detail::Rational<T, 6>(), zero);
            const T greenHue = (rgb[2] - rgb[0]) / safeDelta + Detail::Rational<T, 2>();
            const T blueHue = (rgb[0] - rgb[1]) / safeDelta + Detail::Rational<T, 4>();
            const T hue = Math::Select(
                delta > zero,
                Math::Select(maximum == rgb[0], redHue, Math::Select(maximum == rgb[1], greenHue, blueHue)) /
                    Detail::Rational<T, 6>(),
                zero);
            return {hue, Math::Select(delta > zero, delta / safeDenominator, zero), lightness};
        }

        /** @brief Convert HSL to RGB for a fixed-width floating SIMD carrier. */
        template<Math::FloatingSimdValue T>
        [[nodiscard]] constexpr Math::Vector<T, 3> ToRGB(Math::Vector<T, 3> hsl) noexcept {
            const T one = Detail::Rational<T, 1>();
            const T six = Detail::Rational<T, 6>();
            const T q =
                Math::Select(hsl[2] < Detail::Half<T>(), hsl[2] * (one + hsl[1]), hsl[2] + hsl[1] - hsl[2] * hsl[1]);
            const T p = Detail::Rational<T, 2>() * hsl[2] - q;
            const T twoThirds = Detail::Rational<T, 2>() / Detail::Rational<T, 3>();
            const auto hue = [p, q, one, six, twoThirds](T value) constexpr {
                value = Math::Select(value < Detail::Rational<T, 0>(), value + one, value);
                value = Math::Select(value > one, value - one, value);
                return Math::Select(
                    value < one / Detail::Rational<T, 6>(), p + (q - p) * six * value,
                    Math::Select(value < Detail::Half<T>(), q,
                                 Math::Select(value < twoThirds, p + (q - p) * (twoThirds - value) * six, p)));
            };
            return {hue(hsl[0] + one / Detail::Rational<T, 3>()), hue(hsl[0]),
                    hue(hsl[0] - one / Detail::Rational<T, 3>())};
        }

        /** @} —————————————————————————————————————————————————————————————————— */

    } // namespace HSL

    /**
     * @brief Three-channel color plus an independent alpha/coverage component.
     * @details Alpha is not decoded, adapted, or otherwise transformed by color-space conversion. The RGB channels
     * use the same tagged signal domain and carrier as @ref Color.
     */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T = float>
    struct ColorA {
        [[= Sora::Color::$::SignalStorage{}]] typename Color<Space, T>::VectorType value{};
        [[= Sora::Color::$::AlphaChannel{}]] T alpha = Detail::Rational<T, 1>();

        /** @brief Construct black with opaque alpha in the tagged color space. */
        constexpr ColorA() noexcept = default;
        constexpr ColorA(T red, T green, T blue, T opacity = Detail::Rational<T, 1>()) noexcept
            : value(red, green, blue), alpha(opacity) {}
        constexpr ColorA(Color<Space, T> rgb, T opacity = Detail::Rational<T, 1>()) noexcept
            : value(rgb.value), alpha(opacity) {}
        [[nodiscard]] constexpr T& r() noexcept { return value[0]; }
        [[nodiscard]] constexpr T& g() noexcept { return value[1]; }
        [[nodiscard]] constexpr T& b() noexcept { return value[2]; }
        [[nodiscard]] constexpr const T& r() const noexcept { return value[0]; }
        [[nodiscard]] constexpr const T& g() const noexcept { return value[1]; }
        [[nodiscard]] constexpr const T& b() const noexcept { return value[2]; }
        [[nodiscard]] constexpr T& operator[](std::size_t index) noexcept { return index == 3 ? alpha : value[index]; }
        [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept {
            return index == 3 ? alpha : value[index];
        }
        [[nodiscard]] static consteval const ColorSpaceDescription& Description() noexcept {
            return SpaceDescription<Space>();
        }
        [[nodiscard]] static consteval TransferFunction Transfer() noexcept {
            return SpaceDescription<Space>().transfer;
        }
        /** @brief Return the RGB channels without changing their signal domain or alpha association. */
        [[nodiscard]] constexpr Color<Space, T> RGB() const noexcept { return Color<Space, T>{value}; }
        [[nodiscard]] friend constexpr bool operator==(const ColorA&, const ColorA&) = default;
        [[nodiscard]] constexpr ColorA operator+(const ColorA& other) const noexcept {
            return {Color<Space, T>{value + other.value}, alpha + other.alpha};
        }
        [[nodiscard]] constexpr ColorA operator-(const ColorA& other) const noexcept {
            return {Color<Space, T>{value - other.value}, alpha - other.alpha};
        }
        [[nodiscard]] constexpr ColorA operator*(T scale) const noexcept {
            return {Color<Space, T>{value * scale}, alpha * scale};
        }
        [[nodiscard]] constexpr ColorA operator/(T scale) const noexcept {
            return {Color<Space, T>{value / scale}, alpha / scale};
        }
        constexpr ColorA& operator+=(const ColorA& other) noexcept {
            value = value + other.value;
            alpha += other.alpha;
            return *this;
        }
        constexpr ColorA& operator-=(const ColorA& other) noexcept {
            value = value - other.value;
            alpha -= other.alpha;
            return *this;
        }
        constexpr ColorA& operator*=(T scale) noexcept {
            value = value * scale;
            alpha *= scale;
            return *this;
        }
        constexpr ColorA& operator/=(T scale) noexcept {
            value = value / scale;
            alpha /= scale;
            return *this;
        }
    };

    /** @brief Scale both RGB channels and alpha by a scalar carrier. */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr ColorA<Space, T> operator*(T scale, const ColorA<Space, T>& color) noexcept {
        return color * scale;
    }

    /** @brief Premultiply RGB channels by alpha without changing alpha itself. */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr ColorA<Space, T> Premultiply(ColorA<Space, T> color) noexcept {
        return {Color<Space, T>{color.value * color.alpha}, color.alpha};
    }

    /** @brief Unpremultiply RGB channels when alpha is positive; zero-alpha colors become black. */
    template<Concept::ColorSpaceTag Space, Concept::ColorCarrier T>
    [[nodiscard]] constexpr ColorA<Space, T> Unpremultiply(ColorA<Space, T> color) noexcept {
        const T inverse = Math::Select(color.alpha > Detail::Rational<T, 0>(), Detail::Rational<T, 1>() / color.alpha,
                                       Detail::Rational<T, 0>());
        return {Color<Space, T>{color.value * inverse},
                Math::Select(color.alpha > Detail::Rational<T, 0>(), color.alpha, Detail::Rational<T, 0>())};
    }

    /** @brief Convert RGB channels between tags while preserving alpha exactly. */
    template<Concept::ColorSpaceTag Destination, Concept::ColorSpaceTag Source, Concept::ColorCarrier T>
    [[nodiscard]] constexpr ColorA<Destination, T> Convert(const ColorA<Source, T>& source) noexcept {
        return {Convert<Destination>(source.RGB()), source.alpha};
    }

    /** @brief Eligibility predicate for the supported unsigned 8-bit and 16-bit quantized channels. */
    template<typename T>
    concept QuantizedUint = std::same_as<T, uint8_t> || std::same_as<T, uint16_t>;

    /**
     * @brief Packed quantized RGB signal with an explicit color-space tag.
     * @details Components are encoded channels in [0,max(U)]. @ref ToFloat preserves encoded values, while
     * @ref ToLinear applies the tag's decoding transfer function. Quantization clamps to [0,1] and rounds half up.
     */
    template<Concept::ColorSpaceTag Space, QuantizedUint U = uint8_t>
    struct PackedColor {
        U red{}, green{}, blue{};
        static constexpr U kMax = (std::numeric_limits<U>::max)();

        [[nodiscard]] constexpr U& r() noexcept { return red; }
        [[nodiscard]] constexpr U& g() noexcept { return green; }
        [[nodiscard]] constexpr U& b() noexcept { return blue; }
        [[nodiscard]] constexpr const U& r() const noexcept { return red; }
        [[nodiscard]] constexpr const U& g() const noexcept { return green; }
        [[nodiscard]] constexpr const U& b() const noexcept { return blue; }

        [[nodiscard]] constexpr Color<Space, float> ToFloat() const noexcept {
            const float scale = Detail::One<float>() / static_cast<float>(kMax);
            return {static_cast<float>(red) * scale, static_cast<float>(green) * scale,
                    static_cast<float>(blue) * scale};
        }

        [[nodiscard]] constexpr Color<Space, float> ToLinear() const noexcept {
            return Color<Space, float>{Decode(SpaceDescription<Space>().transfer, ToFloat().value)};
        }

        [[nodiscard]] static constexpr PackedColor FromFloat(const Color<Space, float>& color) noexcept {
            const auto quantize = [](float value) constexpr {
                SORA_PRE(value == value);
                return static_cast<U>(Math::Clamp(value, Detail::Zero<float>(), Detail::One<float>()) *
                                          static_cast<float>(kMax) +
                                      Detail::Half<float>());
            };
            return {quantize(color.r()), quantize(color.g()), quantize(color.b())};
        }

        [[nodiscard]] static constexpr PackedColor FromLinear(const Color<Space, float>& color) noexcept {
            return FromFloat(Color<Space, float>{Encode(SpaceDescription<Space>().transfer, color.value)});
        }

        [[nodiscard]] static constexpr PackedColor FromRaw(U redValue, U greenValue, U blueValue) noexcept {
            return {redValue, greenValue, blueValue};
        }
        [[nodiscard]] friend constexpr bool operator==(const PackedColor&, const PackedColor&) = default;
    };

    /** @brief Packed quantized RGB plus unassociated alpha; alpha is linearly quantized and never transfer-coded. */
    template<Concept::ColorSpaceTag Space, QuantizedUint U = uint8_t>
    struct PackedColorA {
        U red{}, green{}, blue{}, alpha = (std::numeric_limits<U>::max)();
        static constexpr U kMax = (std::numeric_limits<U>::max)();

        [[nodiscard]] constexpr U& r() noexcept { return red; }
        [[nodiscard]] constexpr U& g() noexcept { return green; }
        [[nodiscard]] constexpr U& b() noexcept { return blue; }
        [[nodiscard]] constexpr U& a() noexcept { return alpha; }
        [[nodiscard]] constexpr const U& r() const noexcept { return red; }
        [[nodiscard]] constexpr const U& g() const noexcept { return green; }
        [[nodiscard]] constexpr const U& b() const noexcept { return blue; }
        [[nodiscard]] constexpr const U& a() const noexcept { return alpha; }

        [[nodiscard]] constexpr ColorA<Space, float> ToFloat() const noexcept {
            const float scale = Detail::One<float>() / static_cast<float>(kMax);
            return {static_cast<float>(red) * scale, static_cast<float>(green) * scale,
                    static_cast<float>(blue) * scale, static_cast<float>(alpha) * scale};
        }

        [[nodiscard]] constexpr ColorA<Space, float> ToLinear() const noexcept {
            const auto encoded = ToFloat();
            return {Color<Space, float>{Decode(SpaceDescription<Space>().transfer, encoded.value)}, encoded.alpha};
        }

        [[nodiscard]] static constexpr PackedColorA FromFloat(const ColorA<Space, float>& color) noexcept {
            const auto quantize = [](float value) constexpr {
                SORA_PRE(value == value);
                return static_cast<U>(Math::Clamp(value, Detail::Zero<float>(), Detail::One<float>()) *
                                          static_cast<float>(kMax) +
                                      Detail::Half<float>());
            };
            return {quantize(color.value[0]), quantize(color.value[1]), quantize(color.value[2]),
                    quantize(color.alpha)};
        }

        [[nodiscard]] static constexpr PackedColorA FromLinear(const ColorA<Space, float>& color) noexcept {
            return FromFloat(ColorA<Space, float>{
                Color<Space, float>{Encode(SpaceDescription<Space>().transfer, color.value)}, color.alpha});
        }

        [[nodiscard]] static constexpr PackedColorA FromRaw(U redValue, U greenValue, U blueValue,
                                                            U alphaValue) noexcept {
            return {redValue, greenValue, blueValue, alphaValue};
        }
        [[nodiscard]] friend constexpr bool operator==(const PackedColorA&, const PackedColorA&) = default;
    };

    using sRGB8 = PackedColor<CS::SRGB, uint8_t>;
    using sRGB16 = PackedColor<CS::SRGB, uint16_t>;
    using sRGBA8 = PackedColorA<CS::SRGB, uint8_t>;
    using sRGBA16 = PackedColorA<CS::SRGB, uint16_t>;

    using RGB8 = PackedColor<CS::LinearSRGB, uint8_t>;
    using RGB16 = PackedColor<CS::LinearSRGB, uint16_t>;
    using RGBA8 = PackedColorA<CS::LinearSRGB, uint8_t>;
    using RGBA16 = PackedColorA<CS::LinearSRGB, uint16_t>;

} // namespace Sora::Color
