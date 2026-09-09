/**
 * @file ColorSpaceTest.cpp
 * @brief Behavioral tests for Sora surface color-space primitives.
 * @ingroup Color
 */
#include <Sora/Core/ColorSpaceUtils.h>
#include <Sora/Math/Simd.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace Sora::Color {

    TEST_CASE("sRGB transfer functions round-trip boundaries and signed extension", "[Sora][Color]") {
        for (const float value : {0.0f, 0.0031308f, 0.18f, 1.0f}) {
            REQUIRE(Decode(TransferFunction::SRGB, Encode(TransferFunction::SRGB, value)) ==
                    Catch::Approx(value).margin(1.0e-5f));
        }
        REQUIRE(Encode(TransferFunction::SRGB, -0.25f) < 0.0f);
        REQUIRE(Decode(TransferFunction::SRGB, -0.25f) < 0.0f);
    }

    TEST_CASE("transfer functions keep SIMD boundary lanes finite", "[Sora][Color]") {
        using Pack = Sora::Simd::F32<4>;
        const auto srgb = Encode(TransferFunction::SRGB, Pack(-0.25F));
        const auto bt709 = Encode(TransferFunction::BT709, Pack(-0.25F));
        const auto hlg = Encode(TransferFunction::HLG, Pack(-0.25F));
        REQUIRE_FALSE(std::isnan(srgb[0]));
        REQUIRE_FALSE(std::isnan(bt709[0]));
        REQUIRE_FALSE(std::isnan(hlg[0]));
    }

    TEST_CASE("tagged RGB conversion round-trips through Display P3", "[Sora][Color]") {
        const Color<CS::SRGB> source{0.25f, 0.5f, 0.75f};
        const auto p3 = Convert<CS::DisplayP3>(source);
        const auto restored = Convert<CS::SRGB>(p3);
        REQUIRE(restored.value[0] == Catch::Approx(source.value[0]).margin(2.0e-4f));
        REQUIRE(restored.value[1] == Catch::Approx(source.value[1]).margin(2.0e-4f));
        REQUIRE(restored.value[2] == Catch::Approx(source.value[2]).margin(2.0e-4f));
    }

    TEST_CASE("CIE XYZ is a tristimulus tag rather than a degenerate RGB space", "[Sora][Color]") {
        const Color<CS::LinearSRGB> source{0.25f, 0.5f, 0.75f};
        const auto xyz = Convert<CS::CIEXYZ>(source);
        const auto restored = Convert<CS::LinearSRGB>(xyz);
        REQUIRE(restored.value[0] == Catch::Approx(source.value[0]).margin(2.0e-4f));
        REQUIRE(restored.value[1] == Catch::Approx(source.value[1]).margin(2.0e-4f));
        REQUIRE(restored.value[2] == Catch::Approx(source.value[2]).margin(2.0e-4f));
    }

    TEST_CASE("runtime descriptions expose the same luminance path as tagged colors", "[Sora][Color]") {
        const Color<CS::SRGB> tagged{0.25f, 0.5f, 0.75f};
        REQUIRE(Luminance(SpaceDescription<CS::SRGB>(), tagged.value) ==
                Catch::Approx(Luminance(tagged)).margin(1.0e-6f));
        REQUIRE(Cbrt(-8.0f) == Catch::Approx(-2.0f));
    }

    TEST_CASE("tristimulus matrix paths are identity and transfer functions round-trip", "[Sora][Color]") {
        const auto xyzMatrix = RGBtoXYZ<float>(SpaceDescription<CS::CIEXYZ>());
        const auto inverseXyzMatrix = XYZtoRGB<float>(SpaceDescription<CS::CIEXYZ>());
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                const float expected = row == column ? 1.0f : 0.0f;
                REQUIRE(xyzMatrix(row, column) == Catch::Approx(expected).margin(1.0e-6f));
                REQUIRE(inverseXyzMatrix(row, column) == Catch::Approx(expected).margin(1.0e-6f));
            }
        }

        const auto rgbToXyz = RGBtoRGB<float>(SpaceDescription<CS::LinearSRGB>(), SpaceDescription<CS::CIEXYZ>());
        const auto expectedRgbToXyz =
            BradfordAdaptation<float>(SpaceDescription<CS::LinearSRGB>().white, SpaceDescription<CS::CIEXYZ>().white) *
            RGBtoXYZ<float>(SpaceDescription<CS::LinearSRGB>());
        REQUIRE(rgbToXyz(0, 0) == Catch::Approx(expectedRgbToXyz(0, 0)).margin(1.0e-6f));
        REQUIRE(rgbToXyz(1, 1) == Catch::Approx(expectedRgbToXyz(1, 1)).margin(1.0e-6f));

        const auto xyzToRgb = RGBtoRGB<float>(SpaceDescription<CS::CIEXYZ>(), SpaceDescription<CS::LinearSRGB>());
        const auto expectedXyzToRgb =
            XYZtoRGB<float>(SpaceDescription<CS::LinearSRGB>()) *
            BradfordAdaptation<float>(SpaceDescription<CS::CIEXYZ>().white, SpaceDescription<CS::LinearSRGB>().white);
        REQUIRE(xyzToRgb(0, 0) == Catch::Approx(expectedXyzToRgb(0, 0)).margin(1.0e-6f));
        REQUIRE(xyzToRgb(1, 1) == Catch::Approx(expectedXyzToRgb(1, 1)).margin(1.0e-6f));

        constexpr std::array functions{TransferFunction::Linear,  TransferFunction::SRGB,    TransferFunction::BT709,
                                       TransferFunction::Gamma22, TransferFunction::Gamma26, TransferFunction::PQ,
                                       TransferFunction::HLG,     TransferFunction::ACEScct};
        for (const auto function : functions) {
            const float value = 0.18f;
            REQUIRE(Decode(function, Encode(function, value)) == Catch::Approx(value).margin(2.0e-4f));
        }
    }

    TEST_CASE("ACEScct preserves extended linear values", "[Sora][Color]") {
        constexpr float cutLinear = 0.0078125f;
        constexpr float cutEncoded = 0.155251141552511f;
        REQUIRE(Encode(TransferFunction::ACEScct, cutLinear) == Catch::Approx(cutEncoded).margin(1.0e-6f));
        REQUIRE(Decode(TransferFunction::ACEScct, cutEncoded) == Catch::Approx(cutLinear).margin(1.0e-6f));
        REQUIRE(Decode(TransferFunction::ACEScct, Encode(TransferFunction::ACEScct, -0.001f)) ==
                Catch::Approx(-0.001f).margin(1.0e-6f));
        REQUIRE(Encode(TransferFunction::ACEScct, -0.1f) < 0.0f);
        REQUIRE(Encode(TransferFunction::ACEScct, 100.0f) > Encode(TransferFunction::ACEScct, 1.0f));
    }

    TEST_CASE("Bradford adaptation maps the source white to the destination white", "[Sora][Color]") {
        const Math::Vector<float, 3> sourceWhite{Illuminant::D65[0] / Illuminant::D65[1], 1.0f,
                                                 (1.0f - Illuminant::D65[0] - Illuminant::D65[1]) / Illuminant::D65[1]};
        const Math::Vector<float, 3> destinationWhite{Illuminant::D50[0] / Illuminant::D50[1], 1.0f,
                                                      (1.0f - Illuminant::D50[0] - Illuminant::D50[1]) /
                                                          Illuminant::D50[1]};
        const auto adapted = BradfordAdaptation<float>(Illuminant::D65, Illuminant::D50) * sourceWhite;
        REQUIRE(adapted[0] == Catch::Approx(destinationWhite[0]).margin(1.0e-5f));
        REQUIRE(adapted[1] == Catch::Approx(destinationWhite[1]).margin(1.0e-5f));
        REQUIRE(adapted[2] == Catch::Approx(destinationWhite[2]).margin(1.0e-5f));
    }

    TEST_CASE("packed colors preserve encoded signal and expose explicit linear conversion", "[Sora][Color]") {
        const Color<CS::SRGB> encoded{0.5f, 0.25f, 1.0f};
        const auto packed = sRGB8::FromFloat(encoded);
        const auto restored = packed.ToFloat();
        REQUIRE(restored.value[0] == Catch::Approx(encoded.value[0]).margin(5.0e-3f));
        REQUIRE(packed.ToLinear().value[0] == Catch::Approx(0.214041f).margin(5.0e-3f));
        REQUIRE(sRGB8::FromLinear(packed.ToLinear()) == packed);
    }

    TEST_CASE("packed alpha colors preserve encoded signal and round-trip linear conversion", "[Sora][Color]") {
        const ColorA<CS::SRGB> encoded{0.5f, 0.25f, 1.0f, 0.4f};
        const auto packed = sRGBA8::FromFloat(encoded);
        const auto restored = packed.ToFloat();
        REQUIRE(restored.value[0] == Catch::Approx(encoded.value[0]).margin(5.0e-3f));
        REQUIRE(restored.alpha == Catch::Approx(encoded.alpha).margin(5.0e-3f));
        REQUIRE(packed.ToLinear().value[0] == Catch::Approx(0.214041f).margin(5.0e-3f));
        REQUIRE(packed.ToLinear().alpha == Catch::Approx(encoded.alpha).margin(5.0e-3f));
        REQUIRE(sRGBA8::FromLinear(packed.ToLinear()) == packed);
    }

    TEST_CASE("source-over composites in scene-linear coordinates", "[Sora][Color]") {
        const ColorA<CS::SRGB> background{0.0f, 0.0f, 1.0f, 1.0f};
        const ColorA<CS::SRGB> foreground{1.0f, 0.0f, 0.0f, 0.5f};
        const auto result = CompositeOver(background, foreground);
        REQUIRE(result.alpha == Catch::Approx(1.0f));
        REQUIRE(result.value[0] == Catch::Approx(0.735356f).margin(2.0e-4f));
        REQUIRE(result.value[2] == Catch::Approx(0.735356f).margin(2.0e-4f));
    }

    TEST_CASE("linear-light color utility primitives preserve their stated invariants", "[Sora][Color]") {
        const Color<CS::SRGB> red{1.0f, 0.0f, 0.0f};
        const auto tinted = Tint(red, 0.5f);
        const auto gray = Grayscale(red);
        const auto exposed = AdjustExposure(Color<CS::SRGB>{0.18f, 0.18f, 0.18f}, 1.0f);
        const Color<CS::SRGB> black{0.0f, 0.0f, 0.0f};
        const Color<CS::SRGB> white{1.0f, 1.0f, 1.0f};
        const ColorA<CS::SRGB> translucent{1.0f, 0.0f, 0.0f, 1.0f};

        REQUIRE(tinted.value[0] == Catch::Approx(1.0f).margin(1.0e-5f));
        REQUIRE(tinted.value[1] > 0.7f);
        REQUIRE(gray.value[0] == Catch::Approx(gray.value[1]).margin(1.0e-5f));
        REQUIRE(gray.value[1] == Catch::Approx(gray.value[2]).margin(1.0e-5f));
        REQUIRE(exposed.value[0] > 0.18f);
        REQUIRE(ContrastRatio(black, white) == Catch::Approx(21.0f).margin(1.0e-5f));
        REQUIRE(Fade(translucent, 0.25f).alpha == Catch::Approx(0.75f));
    }

    TEST_CASE("perceptual spaces round-trip and CIEDE2000 matches reference", "[Sora][Color]") {
        const Math::Vector<float, 3> xyz{0.25f, 0.4f, 0.1f};
        const auto lab = Lab::FromXYZ(xyz);
        const auto luv = Luv::FromXYZ(xyz);
        const auto oklab = Oklab::FromXYZ(xyz);
        REQUIRE(Lab::ToXYZ(lab)[1] == Catch::Approx(xyz[1]).margin(2.0e-4f));
        REQUIRE(Luv::ToXYZ(luv)[1] == Catch::Approx(xyz[1]).margin(2.0e-4f));
        REQUIRE(Oklab::ToXYZ(oklab)[1] == Catch::Approx(xyz[1]).margin(2.0e-4f));

        const Math::Vector<float, 3> first{50.0f, 2.6772f, -79.7751f};
        const Math::Vector<float, 3> second{50.0f, 0.0f, -82.7485f};
        REQUIRE(CIEDE2000(first, second) == Catch::Approx(2.0425f).margin(2.0e-3f));
        REQUIRE(Luv::ToXYZ(Math::Vector<float, 3>{0.0f, 0.0f, 0.0f}) == Math::Vector<float, 3>{});
    }

    TEST_CASE("HSV and HSL round-trip a representative color", "[Sora][Color]") {
        const Math::Vector<float, 3> rgb{0.25f, 0.5f, 0.75f};
        const auto hsv = HSV::FromRGB(rgb);
        const auto hsl = HSL::FromRGB(rgb);
        REQUIRE(HSV::ToRGB(hsv)[1] == Catch::Approx(rgb[1]).margin(1.0e-5f));
        REQUIRE(HSL::ToRGB(hsl)[1] == Catch::Approx(rgb[1]).margin(1.0e-5f));
    }

    TEST_CASE("Oklab gamut mapping produces a destination-gamut color", "[Sora][Color]") {
        const auto mapped = GamutMapToSRGB(Math::Vector<float, 3>{0.65f, 0.4f, 0.3f});
        REQUIRE(InGamut(mapped, 1.0e-3f));

        const auto d50 = SpaceDescription<CS::ProPhotoRGB>().WithTransfer(TransferFunction::Linear);
        const auto d50Mapped =
            GamutMapOklab(Math::Vector<float, 3>{0.8f, 0.2f, 0.1f}, SpaceDescription<CS::LinearSRGB>(), d50);
        REQUIRE(InGamut(d50Mapped, 1.0e-3f));
    }

} // namespace Sora::Color



