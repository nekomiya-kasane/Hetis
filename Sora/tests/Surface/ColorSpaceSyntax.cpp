#include <Sora/Core/ColorSpace.h>
#include <Sora/Core/ColorSpaceUtils.h>

#include <concepts>
#include <Sora/Math/Simd.h>

#include <array>
#include <cmath>

constexpr Sora::Math::Vector<float, 3> kCiedeFirst{50.0f, 2.6772f, -79.7751f};
constexpr Sora::Math::Vector<float, 3> kCiedeSecond{50.0f, 0.0f, -82.7485f};
static_assert(Sora::Color::CIEDE2000(kCiedeFirst, kCiedeSecond) > 2.04f);
static_assert(Sora::Color::CIEDE2000(kCiedeFirst, kCiedeSecond) < 2.05f);
static_assert(Sora::Color::DeltaE::OklabDE(Sora::Math::Vector<float, 3>{}, Sora::Math::Vector<float, 3>{}) == 0.0f);
static_assert(Sora::Color::Traits::Domain<Sora::Color::CS::SRGB> == Sora::Color::ColorDomain::EncodedRGB);
static_assert(Sora::Color::Traits::Domain<Sora::Color::CS::LinearSRGB> == Sora::Color::ColorDomain::SceneLinear);
static_assert(Sora::Color::Traits::Domain<Sora::Color::CS::CIEXYZ> == Sora::Color::ColorDomain::Tristimulus);
static_assert(Sora::Color::Meta::IsEncoded<Sora::Color::CS::SRGB>());
static_assert(Sora::Color::Meta::IsSceneLinear<Sora::Color::CS::LinearSRGB>());
static_assert(Sora::Color::Meta::IsTristimulus<Sora::Color::CS::CIEXYZ>());
static_assert(Sora::Color::Meta::Domain<Sora::Color::CS::SRGB>() == Sora::Color::ColorDomain::EncodedRGB);
static_assert(Sora::Color::Meta::Transfer<Sora::Color::CS::SRGB>() == Sora::Color::TransferFunction::SRGB);
static_assert(std::same_as<Sora::Color::Chromaticity, Sora::Math::Point2f>);
static_assert(Sora::Color::Illuminant::D65[0] == 0.31271F);
static_assert(Sora::Color::Illuminant::D65[1] == 0.32902F);
constexpr auto kDaylight6500 = Sora::Color::CCTToXY(6500.0F);
static_assert(kDaylight6500[0] > 0.0F && kDaylight6500[1] > 0.0F);
static_assert(Sora::Color::sRGB8::FromFloat(Sora::Color::Color<Sora::Color::CS::SRGB>{0.5f, 0.25f, 1.0f})
                  .ToFloat()
                  .value[0] > 0.49f);

int main() {
    const auto nearlyEqual = [](float first, float second, float tolerance) constexpr {
        return std::abs(first - second) <= tolerance;
    };

    Sora::Color::Color<Sora::Color::CS::SRGB> color{1.0f, 0.0f, 0.0f};
    auto linear = Sora::Color::Convert<Sora::Color::CS::LinearSRGB>(color);
    (void)linear;

    using Pack = Sora::Simd::F32<4>;
    Sora::Color::Color<Sora::Color::CS::LinearSRGB, Pack> packed{Pack(1.0f), Pack(0.0f), Pack(0.0f)};
    Sora::Color::ColorA<Sora::Color::CS::LinearSRGB, Pack> packedAlpha{};
    auto convertedAlpha = Sora::Color::Convert<Sora::Color::CS::DisplayP3>(packedAlpha);
    auto converted = Sora::Color::Convert<Sora::Color::CS::DisplayP3>(packed);
    auto tinted = Sora::Color::Tint(packed, Pack(0.25f));
    auto gray = Sora::Color::Grayscale(packed);
    auto exposed = Sora::Color::AdjustExposure(packed, Pack(1.0f));
    auto contrast = Sora::Color::ContrastRatio(packed, tinted);
    const auto rgbToXyz =
        Sora::Color::RGBtoRGB<float>(Sora::Color::SpaceDescription<Sora::Color::CS::LinearSRGB>(),
                                       Sora::Color::SpaceDescription<Sora::Color::CS::CIEXYZ>());
    const auto expectedRgbToXyz =
        Sora::Color::BradfordAdaptation<float>(Sora::Color::Illuminant::D65, Sora::Color::Illuminant::E) *
        Sora::Color::RGBtoXYZ<float>(Sora::Color::SpaceDescription<Sora::Color::CS::LinearSRGB>());
    auto hsv = Sora::Color::HSV::FromRGB(packed.value);
    auto hsvRgb = Sora::Color::HSV::ToRGB(hsv);
    auto hsl = Sora::Color::HSL::FromRGB(packed.value);
    auto hslRgb = Sora::Color::HSL::ToRGB(hsl);
    auto cie94 = Sora::Color::CIE94(packed.value, converted.value);
    auto ciede2000 = Sora::Color::CIEDE2000(packed.value, converted.value);
    auto gamut = Sora::Color::InGamut(converted.value);
    auto blackbody = Sora::Color::BlackbodyColor<Sora::Color::CS::LinearSRGB, Pack>(6500.0f);
    auto bt709 = Sora::Color::Encode(Sora::Color::TransferFunction::BT709, Pack(0.18f));
    auto pq = Sora::Color::Encode(Sora::Color::TransferFunction::PQ, Pack(100.0f));
    auto hlg = Sora::Color::Encode(Sora::Color::TransferFunction::HLG, Pack(0.18f));
    auto acescct = Sora::Color::Encode(Sora::Color::TransferFunction::ACEScct, Pack(0.18f));
    auto srgbAlias = Sora::Color::TF::sRGB_OETF(Pack(0.18f));
    bool valid = nearlyEqual(hsvRgb[0][0], 1.0f, 1.0e-5f) && nearlyEqual(hsvRgb[1][0], 0.0f, 1.0e-5f) &&
                 nearlyEqual(hslRgb[0][0], 1.0f, 1.0e-5f) && nearlyEqual(hslRgb[1][0], 0.0f, 1.0e-5f) &&
                 std::isfinite(bt709[0]) && std::isfinite(hlg[0]) && std::isfinite(pq[0]) &&
                 std::isfinite(acescct[0]) && std::isfinite(srgbAlias[0]) &&
                 nearlyEqual(Sora::Color::Cbrt(-8.0f), -2.0f, 1.0e-5f) &&
                 nearlyEqual(rgbToXyz(0, 0), expectedRgbToXyz(0, 0), 1.0e-5f) &&
                 nearlyEqual(rgbToXyz(1, 1), expectedRgbToXyz(1, 1), 1.0e-5f) &&
                 nearlyEqual(gray.value[0][0], gray.value[1][0], 1.0e-5f) && tinted.value[1][0] > 0.0f &&
                 exposed.value[0][0] > packed.value[0][0] && contrast[0] > 1.0f &&
                 nearlyEqual(packedAlpha.alpha[0], 1.0f, 1.0e-5f);

    constexpr std::array transferFunctions{Sora::Color::TransferFunction::Linear,
                                           Sora::Color::TransferFunction::SRGB,
                                           Sora::Color::TransferFunction::BT709,
                                           Sora::Color::TransferFunction::Gamma22,
                                           Sora::Color::TransferFunction::Gamma26,
                                           Sora::Color::TransferFunction::PQ,
                                           Sora::Color::TransferFunction::HLG,
                                           Sora::Color::TransferFunction::ACEScct};
    for (const auto function : transferFunctions) {
        const float value = function == Sora::Color::TransferFunction::PQ ? 100.0f : 0.18f;
        const float restored = Sora::Color::Decode(function, Sora::Color::Encode(function, value));
        const float tolerance = function == Sora::Color::TransferFunction::PQ ? 1.0e-2f : 2.0e-4f;
        valid = valid && nearlyEqual(restored, value, tolerance);
    }
    valid = valid && nearlyEqual(Sora::Color::TF::PQ_EOTF(Sora::Color::TF::PQ_OETF(0.0f)), 0.0f, 1.0e-4f);
    valid = valid && nearlyEqual(Sora::Color::TF::PQ_EOTF(Sora::Color::TF::PQ_OETF(10000.0f)), 10000.0f, 1.0f);
    valid = valid && nearlyEqual(Sora::Color::TF::HLG_EOTF(Sora::Color::TF::HLG_OETF(0.18f)), 0.18f, 2.0e-4f);
    valid = valid && nearlyEqual(Sora::Color::TF::ACEScct_Decode(Sora::Color::TF::ACEScct_Encode(0.18f)), 0.18f,
                                 2.0e-4f);
    valid = valid && nearlyEqual(Sora::Color::TF::SRGB_EOTF(Sora::Color::TF::SRGB_OETF(0.18f)), 0.18f, 2.0e-5f);
    valid = valid && nearlyEqual(Sora::Color::TF::BT709_EOTF(Sora::Color::TF::BT709_OETF(0.18f)), 0.18f, 2.0e-4f);

    const auto lab = Sora::Color::Lab::FromXYZ(Sora::Math::Vector<float, 3>{0.25f, 0.4f, 0.1f});
    const auto luv = Sora::Color::Luv::FromXYZ(Sora::Math::Vector<float, 3>{0.25f, 0.4f, 0.1f});
    const auto oklab = Sora::Color::Oklab::FromXYZ(Sora::Math::Vector<float, 3>{0.25f, 0.4f, 0.1f});
    valid = valid && nearlyEqual(Sora::Color::Lab::ToXYZ(lab)[1], 0.4f, 2.0e-4f) &&
            nearlyEqual(Sora::Color::Luv::ToXYZ(luv)[1], 0.4f, 2.0e-4f) &&
            nearlyEqual(Sora::Color::Oklab::ToXYZ(oklab)[1], 0.4f, 2.0e-4f);

    const auto packedColor = Sora::Color::sRGB8::FromFloat(
        Sora::Color::Color<Sora::Color::CS::SRGB>{-1.0f, 0.5f, 2.0f});
    valid = valid && packedColor.r() == 0 && packedColor.g() == 128 && packedColor.b() == 255;
    if (!valid) {
        return 1;
    }
    (void)gamut;
    (void)gray;
    (void)exposed;
    (void)contrast;
    (void)hsvRgb;
    (void)hslRgb;
    (void)cie94;
    (void)ciede2000;
    (void)blackbody;
    (void)bt709;
    (void)pq;
    (void)hlg;
    (void)acescct;

    (void)Sora::Color::CIEDE2000(kCiedeFirst, kCiedeSecond);
    (void)Sora::Color::GamutMapToSRGB(Sora::Math::Vector<float, 3>{0.6f, 0.2f, 0.1f});
    const Sora::Color::ColorA<Sora::Color::CS::SRGB> background{0.0f, 0.0f, 1.0f, 1.0f};
    const Sora::Color::ColorA<Sora::Color::CS::SRGB> foreground{1.0f, 0.0f, 0.0f, 0.5f};
    auto composited = Sora::Color::CompositeOver(background, foreground);
    (void)composited;
    (void)packedAlpha;
    (void)convertedAlpha;
}


