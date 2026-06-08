/**
 * @file ColorSpaceTest.cpp
 * @brief Comprehensive tests for Mashiro::Color color science utilities.
 */
#include "Mashiro/Surface/ColorSpace.h"
#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string_view>

using namespace Mashiro;
using namespace Mashiro::Coloring;

using namespace std::string_view_literals;

using Catch::Approx;

namespace {
    constexpr float kEps = 1e-4f;

    void ExpectVecNear(vec3 actual, vec3 expected, float eps = kEps) {
        REQUIRE(actual.x == Approx(expected.x).margin(eps));
        REQUIRE(actual.y == Approx(expected.y).margin(eps));
        REQUIRE(actual.z == Approx(expected.z).margin(eps));
    }

    template<ColorSpaceDesc CS>
    void ExpectColorNear(Color<CS> actual, Color<CS> expected, float eps = kEps) {
        ExpectVecNear(actual.v, expected.v, eps);
    }

    template<ColorSpaceDesc CS>
    void ExpectColorANear(ColorA<CS> actual, ColorA<CS> expected, float eps = kEps) {
        ExpectVecNear(actual.v, expected.v, eps);
        REQUIRE(actual.a == Approx(expected.a).margin(eps));
    }
}

// =============================================================================
// Constants & descriptors
// =============================================================================

TEST_CASE("ColorSpace constants and descriptors expose the expected metadata", AUTO_TAG) {
    STATIC_REQUIRE(Illuminant::D65.x == 0.31271f);
    STATIC_REQUIRE(Illuminant::D65.y == 0.32902f);
    STATIC_REQUIRE(Illuminant::D50.x == 0.34567f);
    STATIC_REQUIRE(Illuminant::E.x == 1.0f / 3.0f);

    STATIC_REQUIRE(CS::sRGB.white == Illuminant::D65);
    STATIC_REQUIRE(CS::sRGB.transfer == TransferFn::sRGB);
    STATIC_REQUIRE(CS::LinearSRGB.transfer == TransferFn::Linear);
    STATIC_REQUIRE(CS::DisplayP3.transfer == TransferFn::sRGB);
    STATIC_REQUIRE(CS::BT2020.transfer == TransferFn::BT709);
    STATIC_REQUIRE(CS::ACEScct.transfer == TransferFn::ACEScct);
    STATIC_REQUIRE(CS::CIE_XYZ.transfer == TransferFn::Linear);
    STATIC_REQUIRE(CS::CIE_XYZ.white == Illuminant::E);

    STATIC_REQUIRE(CS::sRGB.name.view() == "sRGB"sv);
    STATIC_REQUIRE(CS::LinearSRGB.name.view() == "Linear sRGB"sv);
    STATIC_REQUIRE(CS::DisplayP3.name.view() == "Display P3"sv);
    STATIC_REQUIRE(CS::CIE_XYZ.name.view() == "CIE XYZ"sv);

    constexpr auto p3Linear = CS::DisplayP3.WithTransfer(TransferFn::Linear);
    STATIC_REQUIRE(p3Linear.transfer == TransferFn::Linear);
    STATIC_REQUIRE(p3Linear.white == CS::DisplayP3.white);
    STATIC_REQUIRE(p3Linear.name.view() == CS::DisplayP3.name.view());

    constexpr auto p3D50 = CS::DisplayP3.WithWhite(Illuminant::D50);
    STATIC_REQUIRE(p3D50.white == Illuminant::D50);
    STATIC_REQUIRE(p3D50.transfer == CS::DisplayP3.transfer);
    STATIC_REQUIRE(p3D50.name.view() == CS::DisplayP3.name.view());

    STATIC_REQUIRE(Color<CS::sRGB>::transfer() == TransferFn::sRGB);
    STATIC_REQUIRE(Color<CS::LinearSRGB>::transfer() == TransferFn::Linear);
    STATIC_REQUIRE(Color<CS::sRGB>::space() == CS::sRGB);

    STATIC_REQUIRE(Named::Black<CS::sRGB> == Color<CS::sRGB>{{0.0f, 0.0f, 0.0f}});
    STATIC_REQUIRE(Named::White<CS::sRGB> == Color<CS::sRGB>{{1.0f, 1.0f, 1.0f}});
    STATIC_REQUIRE(Named::Red<CS::sRGB> == Color<CS::sRGB>{{1.0f, 0.0f, 0.0f}});
    STATIC_REQUIRE(Named::Green<CS::sRGB> == Color<CS::sRGB>{{0.0f, 1.0f, 0.0f}});
    STATIC_REQUIRE(Named::Blue<CS::sRGB> == Color<CS::sRGB>{{0.0f, 0.0f, 1.0f}});
}

// =============================================================================
// Transfer functions
// =============================================================================

TEST_CASE("Transfer functions round-trip representative scalar values", AUTO_TAG) {
    REQUIRE(Decode(TransferFn::Linear, 0.42f) == Approx(0.42f));
    REQUIRE(Encode(TransferFn::Linear, 0.42f) == Approx(0.42f));

    REQUIRE(Decode(TransferFn::sRGB, Encode(TransferFn::sRGB, 0.18f)) == Approx(0.18f).margin(1e-5f));
    REQUIRE(Decode(TransferFn::BT709, Encode(TransferFn::BT709, 0.18f)) == Approx(0.18f).margin(1e-5f));
    REQUIRE(Decode(TransferFn::Gamma22, Encode(TransferFn::Gamma22, 0.18f)) == Approx(0.18f).margin(1e-5f));
    REQUIRE(Decode(TransferFn::Gamma26, Encode(TransferFn::Gamma26, 0.18f)) == Approx(0.18f).margin(1e-5f));
    REQUIRE(Decode(TransferFn::ACEScct, Encode(TransferFn::ACEScct, 0.18f)) == Approx(0.18f).margin(1e-4f));

    REQUIRE(Decode(TransferFn::PQ, Encode(TransferFn::PQ, 100.0f)) == Approx(100.0f).margin(0.5f));
    REQUIRE(Decode(TransferFn::HLG, Encode(TransferFn::HLG, 0.18f)) == Approx(0.18f).margin(1e-4f));
}

TEST_CASE("Transfer functions operate component-wise on vec3 values", AUTO_TAG) {
    vec3 linear{0.05f, 0.25f, 0.75f};

    ExpectVecNear(Encode(TransferFn::Linear, linear), linear);
    ExpectVecNear(Decode(TransferFn::Linear, linear), linear);

    vec3 encoded = Encode(TransferFn::sRGB, linear);
    ExpectVecNear(Decode(TransferFn::sRGB, encoded), linear, 1e-5f);

    vec3 bt709 = Encode(TransferFn::BT709, linear);
    ExpectVecNear(Decode(TransferFn::BT709, bt709), linear, 1e-5f);
}

// =============================================================================
// RGB/XYZ and RGB/RGB conversion helpers
// =============================================================================

TEST_CASE("XYZ helpers preserve CIE XYZ and luminance semantics", AUTO_TAG) {
    vec3 xyz{0.25f, 0.40f, 0.10f};

    ExpectVecNear(ToXYZ(CS::CIE_XYZ, xyz), xyz, 1e-6f);
    ExpectVecNear(FromXYZ(CS::CIE_XYZ, xyz), xyz, 1e-6f);
    REQUIRE(Luminance(CS::CIE_XYZ, xyz) == Approx(0.40f).margin(1e-6f));
    REQUIRE(Luminance(Color<CS::CIE_XYZ>{xyz}) == Approx(0.40f).margin(1e-6f));
}

TEST_CASE("Color conversion round-trips across transfer and primaries", AUTO_TAG) {
    Color<CS::sRGB> source{{0.25f, 0.50f, 0.75f}};

    auto linear = Convert<CS::LinearSRGB>(source);
    ExpectVecNear(linear.v, Decode(TransferFn::sRGB, source.v), 1e-5f);

    auto p3 = Convert<CS::LinearDisplayP3>(linear);
    auto back = Convert<CS::LinearSRGB>(p3);
    ExpectColorNear(back, linear, 2e-4f);

    auto viaVec = ConvertVec3<CS::LinearDisplayP3>(CS::LinearSRGB, linear.v);
    ExpectVecNear(viaVec, p3.v, 2e-4f);
}

TEST_CASE("Neutral colors remain neutral across same-white color spaces", AUTO_TAG) {
    Color<CS::LinearSRGB> gray{{0.3f, 0.3f, 0.3f}};

    auto p3 = Convert<CS::LinearDisplayP3>(gray);
    REQUIRE(p3.v.x == Approx(p3.v.y).margin(1e-5f));
    REQUIRE(p3.v.y == Approx(p3.v.z).margin(1e-5f));

    auto roundTrip = Convert<CS::LinearSRGB>(p3);
    ExpectColorNear(roundTrip, gray, 2e-4f);
}

// =============================================================================
// Strongly-typed color and alpha helpers
// =============================================================================

TEST_CASE("Color arithmetic and component accessors work", AUTO_TAG) {
    Color<CS::sRGB> a{{0.2f, 0.4f, 0.6f}};
    Color<CS::sRGB> b{{0.1f, 0.3f, 0.5f}};

    REQUIRE(a.r() == Approx(0.2f));
    REQUIRE(a.g() == Approx(0.4f));
    REQUIRE(a.b() == Approx(0.6f));

    ExpectColorNear(a + b, Color<CS::sRGB>{{0.3f, 0.7f, 1.1f}}, 1e-6f);
    ExpectColorNear(a - b, Color<CS::sRGB>{{0.1f, 0.1f, 0.1f}}, 1e-6f);
    ExpectColorNear(a * 2.0f, Color<CS::sRGB>{{0.4f, 0.8f, 1.2f}}, 1e-6f);
    ExpectColorNear(2.0f * a, Color<CS::sRGB>{{0.4f, 0.8f, 1.2f}}, 1e-6f);
    ExpectColorNear(a / 2.0f, Color<CS::sRGB>{{0.1f, 0.2f, 0.3f}}, 1e-6f);
}

TEST_CASE("ColorA premultiply, unpremultiply, and conversion preserve alpha", AUTO_TAG) {
    ColorA<CS::sRGB> rgba{0.2f, 0.4f, 0.6f, 0.25f};

    auto premultiplied = Premultiply(rgba);
    ExpectColorANear(premultiplied, ColorA<CS::sRGB>{{0.05f, 0.10f, 0.15f}, 0.25f}, 1e-6f);

    auto restored = Unpremultiply(premultiplied);
    ExpectColorANear(restored, rgba, 1e-5f);

    auto converted = Convert<CS::LinearSRGB>(rgba);
    REQUIRE(converted.a == Approx(rgba.a).margin(1e-6f));
    ExpectVecNear(converted.v, Decode(TransferFn::sRGB, rgba.v), 1e-5f);
}

// =============================================================================
// Packed integer colors
// =============================================================================

TEST_CASE("PackedColor quantizes encoded values and decodes back to float", AUTO_TAG) {
    auto red = sRGB8::fromFloat(Color<CS::sRGB>{{1.0f, 0.0f, 0.0f}});
    REQUIRE(red.r == 255);
    REQUIRE(red.g == 0);
    REQUIRE(red.b == 0);
    ExpectColorNear(red.toFloat(), Color<CS::sRGB>{{1.0f, 0.0f, 0.0f}}, 1e-6f);

    auto gray = sRGB8::fromFloat(Color<CS::sRGB>{{0.5f, 0.5f, 0.5f}});
    ExpectColorNear(gray.toFloat(), Color<CS::sRGB>{{0.5f, 0.5f, 0.5f}}, 5e-3f);

    auto clamped = sRGB8::fromFloat(Color<CS::sRGB>{{-1.0f, 2.0f, 0.25f}});
    REQUIRE(clamped.r == 0);
    REQUIRE(clamped.g == 255);
    REQUIRE(clamped.b > 0);
    REQUIRE(clamped.b < 255);

    auto raw = sRGB8::fromRaw(1, 2, 3);
    REQUIRE(raw == sRGB8::fromRaw(1, 2, 3));
    REQUIRE(raw != sRGB8::fromRaw(1, 2, 4));
}

TEST_CASE("PackedColorA preserves alpha and clamps it into range", AUTO_TAG) {
    auto packed = sRGBA8::fromFloat(ColorA<CS::sRGB>{{0.0f, 0.5f, 1.0f}, 1.2f});
    REQUIRE(packed.a == 255);

    auto unpacked = packed.toFloat();
    REQUIRE(unpacked.a == Approx(1.0f).margin(1e-6f));
    ExpectVecNear(unpacked.v, Decode(TransferFn::sRGB, vec3{0.0f, 0.5f, 1.0f}), 5e-3f);

    auto raw = sRGBA8::fromRaw(10, 20, 30, 40);
    REQUIRE(raw == sRGBA8::fromRaw(10, 20, 30, 40));
}

// =============================================================================
// Gamut and chromaticity utilities
// =============================================================================

TEST_CASE("Gamut helpers detect and clamp out-of-range values", AUTO_TAG) {
    vec3 inside{0.0f, 0.5f, 1.0f};
    vec3 outside{-0.2f, 0.5f, 1.2f};

    REQUIRE(InGamut(inside));
    REQUIRE(!InGamut(outside));
    REQUIRE(InGamut(outside, 0.25f));
    ExpectVecNear(ClampGamut(outside), vec3{0.0f, 0.5f, 1.0f}, 1e-6f);
}

TEST_CASE("CCT and blackbody helpers produce sensible daylight values", AUTO_TAG) {
    REQUIRE(CCTFromXY(Illuminant::D65) == Approx(6500.0f).margin(500.0f));

    auto d65 = CCTtoXY(6500.0f);
    REQUIRE(d65.x == Approx(Illuminant::D65.x).margin(0.02f));
    REQUIRE(d65.y == Approx(Illuminant::D65.y).margin(0.02f));

    auto bb = BlackbodyXYZ(6500.0f);
    REQUIRE(bb.y == Approx(1.0f).margin(1e-6f));
    REQUIRE(bb.x > 0.0f);
    REQUIRE(bb.z > 0.0f);

    auto white = BlackbodyColor<CS::LinearSRGB>(6500.0f);
    REQUIRE(white.v.x >= 0.0f);
    REQUIRE(white.v.y >= 0.0f);
    REQUIRE(white.v.z >= 0.0f);
}

// =============================================================================
// Perceptual spaces and color differences
// =============================================================================

TEST_CASE("Lab, Luv, Oklab, and Oklch round-trip representative colors", AUTO_TAG) {
    vec3 xyz{0.25f, 0.40f, 0.10f};

    auto lab = Lab::FromXYZ(xyz);
    ExpectVecNear(Lab::ToXYZ(lab), xyz, 2e-4f);

    auto luv = Luv::FromXYZ(xyz);
    ExpectVecNear(Luv::ToXYZ(luv), xyz, 2e-4f);

    auto oklab = Oklab::FromXYZ(xyz);
    ExpectVecNear(Oklab::ToXYZ(oklab), xyz, 2e-4f);
    REQUIRE(DeltaE::OklabDE(oklab, oklab) == Approx(0.0f));

    auto oklch = Oklch::FromOklab(oklab);
    ExpectVecNear(Oklch::ToOklab(oklch), oklab, 2e-4f);
    ExpectVecNear(Oklch::ToLinearSRGB(Oklch::FromLinearSRGB(vec3{0.2f, 0.3f, 0.4f})), vec3{0.2f, 0.3f, 0.4f}, 2e-4f);
}

TEST_CASE("HSV and HSL round-trip representative RGB colors", AUTO_TAG) {
    vec3 rgb{0.25f, 0.50f, 0.75f};

    auto hsv = HSV::FromRGB(rgb);
    ExpectVecNear(HSV::ToRGB(hsv), rgb, 1e-5f);

    auto hsl = HSL::FromRGB(rgb);
    ExpectVecNear(HSL::ToRGB(hsl), rgb, 1e-5f);
}
