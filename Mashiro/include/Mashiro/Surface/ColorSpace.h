/**
 * @file ColorSpace.h
 * @brief Comprehensive color science framework for the Mashiro renderer.
 *
 * A physically-grounded, compile-time-first color system, fully parameterized over
 * `std::floating_point T` (default `float`). Supports `float`, `double`, and
 * quantized integer representations (`uint8_t` / `uint16_t`).
 *
 * **Scalar precision:** Every color operation is templated over `T ∈ {float, double}`.
 * `float` for GPU/realtime paths, `double` for spectral / calibration / offline.
 *
 * **Alpha channel:** `ColorA<CS, T>` extends `Color<CS, T>` with a fourth alpha
 * component. Alpha is NOT part of color space conversions (it is coverage/opacity,
 * orthogonal to chromaticity). Premultiply / unpremultiply helpers are provided.
 *
 * **Integer quantization:** `PackedColor<CS, U>` with `U ∈ {uint8_t, uint16_t}`
 * stores encoded (transfer-function-applied) RGB in integer range. Conversion to/from
 * `Color<CS, T>` handles rounding, clamping, and sRGB-aware encode/decode.
 *
 * **Color space descriptors** (`ColorSpaceDesc`) and `Chromaticity` remain `float` —
 * CIE xy values have ≤5 significant digits; metadata precision needs no `double`.
 *
 * Namespace: `Mashiro::Color`
 *
 * @ingroup Surface
 */
#pragma once

#include "Mashiro/Core/FixedString.h"
#include "Mashiro/Math/MatOps.h"

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Mashiro::Coloring {

    namespace Detail {

        template<std::floating_point T>
        [[nodiscard]] constexpr T CtCbrt(T x) {
            using Mashiro::Math::Pow;

            if (x == T(0)) {
                return T(0);
            }
            bool neg = x < T(0);
            T a = neg ? -x : x;
            T y = Pow(a, T(1) / T(3));
            // Newton refinement: y = (2y + a/y^2) / 3
            constexpr int steps = std::same_as<T, float> ? 4 : 8;
            for (int i = 0; i < steps; ++i) {
                y = (T(2) * y + a / (y * y)) / T(3);
            }
            return neg ? -y : y;
        }

    } // namespace Detail

    template<std::floating_point T>
    [[nodiscard]] constexpr T Cbrt(T x) {
        if consteval {
            return Detail::CtCbrt(x);
        } else {
            return std::cbrt(x);
        }
    }

    // =====================================================================
    //  CIE xy Chromaticity & Standard Illuminants
    // =====================================================================

    using Chromaticity = vec2;

    namespace Illuminant {

        inline constexpr Chromaticity A{.x = 0.44757f, .y = 0.40745f};
        inline constexpr Chromaticity C{.x = 0.31006f, .y = 0.31616f};
        inline constexpr Chromaticity D50{.x = 0.34567f, .y = 0.35850f};
        inline constexpr Chromaticity D55{.x = 0.33242f, .y = 0.34743f};
        inline constexpr Chromaticity D65{.x = 0.31271f, .y = 0.32902f};
        inline constexpr Chromaticity D75{.x = 0.29902f, .y = 0.31485f};
        inline constexpr Chromaticity E{.x = 1.0f / 3.0f, .y = 1.0f / 3.0f};
        inline constexpr Chromaticity F2{.x = 0.37208f, .y = 0.37529f};
        inline constexpr Chromaticity F7{.x = 0.31292f, .y = 0.32933f};
        inline constexpr Chromaticity F11{.x = 0.38052f, .y = 0.37713f};
        inline constexpr Chromaticity DCI{.x = 0.314f, .y = 0.351f};

    } // namespace Illuminant

    // =====================================================================
    //  Transfer Function Tags
    // =====================================================================

    enum class TransferFn : uint8_t {
        Linear,
        sRGB,  // IEC 61966-2-1:1999
        BT709, // ITU-R BT.709 (same numeric curve as BT.601/BT.2020 10-bit)
        Gamma22,
        Gamma26,
        PQ,      // SMPTE ST 2084 (Perceptual Quantizer)
        HLG,     // ARIB STD-B67 (Hybrid Log-Gamma)
        ACEScct, // ACES cct
    };

    // =====================================================================
    //  Color Space Descriptor
    // =====================================================================

    struct ColorSpaceDesc {
        Chromaticity r;
        Chromaticity g;
        Chromaticity b;
        Chromaticity white;

        TransferFn transfer;

        FixedString<32> name;

        [[nodiscard]] constexpr ColorSpaceDesc WithTransfer(TransferFn tf) const {
            return {r, g, b, white, tf, name};
        }

        [[nodiscard]] constexpr ColorSpaceDesc WithWhite(Chromaticity w) const {
            return {r, g, b, w, transfer, name};
        }

        [[nodiscard]] friend constexpr bool operator==(const ColorSpaceDesc& a,
                                                       const ColorSpaceDesc& b) {
            return a.r == b.r && a.g == b.g && a.b == b.b && a.white == b.white &&
                   a.transfer == b.transfer;
        }
    };

    // =====================================================================
    //  §4  Standard Color Spaces
    // =====================================================================

    namespace CS {

        inline constexpr ColorSpaceDesc sRGB{.r = {0.6400f, 0.3300f},
                                             .g = {0.3000f, 0.6000f},
                                             .b = {0.1500f, 0.0600f},
                                             .white = Illuminant::D65,
                                             .transfer = TransferFn::sRGB,
                                             .name = FixedString<32>(std::string_view("sRGB"))};

        inline constexpr ColorSpaceDesc LinearSRGB{
            .r = {0.6400f, 0.3300f},
            .g = {0.3000f, 0.6000f},
            .b = {0.1500f, 0.0600f},
            .white = Illuminant::D65,
            .transfer = TransferFn::Linear,
            .name = FixedString<32>(std::string_view("Linear sRGB"))};

        inline constexpr ColorSpaceDesc DisplayP3{
            .r = {0.6800f, 0.3200f},
            .g = {0.2650f, 0.6900f},
            .b = {0.1500f, 0.0600f},
            .white = Illuminant::D65,
            .transfer = TransferFn::sRGB,
            .name = FixedString<32>(std::string_view("Display P3"))};

        inline constexpr ColorSpaceDesc LinearDisplayP3{
            .r = {0.6800f, 0.3200f},
            .g = {0.2650f, 0.6900f},
            .b = {0.1500f, 0.0600f},
            .white = Illuminant::D65,
            .transfer = TransferFn::Linear,
            .name = FixedString<32>(std::string_view("Linear Display P3"))};

        inline constexpr ColorSpaceDesc DCIP3{.r = {0.6800f, 0.3200f},
                                              .g = {0.2650f, 0.6900f},
                                              .b = {0.1500f, 0.0600f},
                                              .white = Illuminant::DCI,
                                              .transfer = TransferFn::Gamma26,
                                              .name = FixedString<32>(std::string_view("DCI-P3"))};

        inline constexpr ColorSpaceDesc BT709{.r = {0.6400f, 0.3300f},
                                              .g = {0.3000f, 0.6000f},
                                              .b = {0.1500f, 0.0600f},
                                              .white = Illuminant::D65,
                                              .transfer = TransferFn::BT709,
                                              .name = FixedString<32>(std::string_view("BT.709"))};

        inline constexpr ColorSpaceDesc BT2020{.r = {0.7080f, 0.2920f},
                                               .g = {0.1700f, 0.7970f},
                                               .b = {0.1310f, 0.0460f},
                                               .white = Illuminant::D65,
                                               .transfer = TransferFn::BT709,
                                               .name =
                                                   FixedString<32>(std::string_view("BT.2020"))};

        inline constexpr ColorSpaceDesc BT2100PQ{
            .r = {0.7080f, 0.2920f},
            .g = {0.1700f, 0.7970f},
            .b = {0.1310f, 0.0460f},
            .white = Illuminant::D65,
            .transfer = TransferFn::PQ,
            .name = FixedString<32>(std::string_view("BT.2100 PQ"))};

        inline constexpr ColorSpaceDesc BT2100HLG{
            .r = {0.7080f, 0.2920f},
            .g = {0.1700f, 0.7970f},
            .b = {0.1310f, 0.0460f},
            .white = Illuminant::D65,
            .transfer = TransferFn::HLG,
            .name = FixedString<32>(std::string_view("BT.2100 HLG"))};

        inline constexpr ColorSpaceDesc AdobeRGB{
            .r = {0.6400f, 0.3300f},
            .g = {0.2100f, 0.7100f},
            .b = {0.1500f, 0.0600f},
            .white = Illuminant::D65,
            .transfer = TransferFn::Gamma22,
            .name = FixedString<32>(std::string_view("Adobe RGB"))};

        inline constexpr ColorSpaceDesc ProPhotoRGB{
            .r = {0.7347f, 0.2653f},
            .g = {0.1596f, 0.8404f},
            .b = {0.0366f, 0.0001f},
            .white = Illuminant::D50,
            .transfer = TransferFn::Gamma22,
            .name = FixedString<32>(std::string_view("ProPhoto RGB"))};

        inline constexpr ColorSpaceDesc ACES_AP0{.r = {0.7347f, 0.2653f},
                                                 .g = {0.0000f, 1.0000f},
                                                 .b = {0.0001f, -0.0770f},
                                                 .white = {0.32168f, 0.33767f},
                                                 .transfer = TransferFn::Linear,
                                                 .name =
                                                     FixedString<32>(std::string_view("ACES AP0"))};

        inline constexpr ColorSpaceDesc ACES_AP1{.r = {0.7130f, 0.2930f},
                                                 .g = {0.1650f, 0.8300f},
                                                 .b = {0.1280f, 0.0440f},
                                                 .white = {0.32168f, 0.33767f},
                                                 .transfer = TransferFn::Linear,
                                                 .name =
                                                     FixedString<32>(std::string_view("ACES AP1"))};

        inline constexpr ColorSpaceDesc ACEScct{.r = {0.7130f, 0.2930f},
                                                .g = {0.1650f, 0.8300f},
                                                .b = {0.1280f, 0.0440f},
                                                .white = {0.32168f, 0.33767f},
                                                .transfer = TransferFn::ACEScct,
                                                .name =
                                                    FixedString<32>(std::string_view("ACEScct"))};

        inline constexpr ColorSpaceDesc CIE_XYZ{.r = {1.0f, 0.0f},
                                                .g = {0.0f, 1.0f},
                                                .b = {0.0f, 0.0f},
                                                .white = Illuminant::E,
                                                .transfer = TransferFn::Linear,
                                                .name =
                                                    FixedString<32>(std::string_view("CIE XYZ"))};

        inline constexpr ColorSpaceDesc CIE_RGB{.r = {0.7350f, 0.2650f},
                                                .g = {0.2740f, 0.7170f},
                                                .b = {0.1670f, 0.0090f},
                                                .white = Illuminant::E,
                                                .transfer = TransferFn::Linear,
                                                .name =
                                                    FixedString<32>(std::string_view("CIE RGB"))};

    } // namespace CS

    // =====================================================================
    //  Chromatic Adaptation & NPM (reusing Mat<T,3> / Vec<T,3>)
    // =====================================================================

    namespace Detail {

        [[nodiscard]] constexpr vec3 WhiteXYZ(Chromaticity w) {
            return {w.x / w.y, 1.0f, (1.0f - w.x - w.y) / w.y};
        }

        // Bradford cone-response matrix (row-major semantic, stored as Mat column-major)
        [[nodiscard]] consteval mat3 MakeBradford() {
            mat3 m{};
            m[0, 0] = 0.8951f;
            m[0, 1] = 0.2664f;
            m[0, 2] = -0.1614f;
            m[1, 0] = -0.7502f;
            m[1, 1] = 1.7135f;
            m[1, 2] = 0.0367f;
            m[2, 0] = 0.0389f;
            m[2, 1] = -0.0685f;
            m[2, 2] = 1.0296f;
            return m;
        }

        [[nodiscard]] consteval mat3 MakeBradfordInv() {
            mat3 m{};
            m[0, 0] = 0.9869929f;
            m[0, 1] = -0.1470543f;
            m[0, 2] = 0.1599627f;
            m[1, 0] = 0.4323053f;
            m[1, 1] = 0.5183603f;
            m[1, 2] = 0.0492912f;
            m[2, 0] = -0.0085287f;
            m[2, 1] = 0.0400428f;
            m[2, 2] = 0.9684867f;
            return m;
        }

        inline constexpr mat3 kBradford = MakeBradford();
        inline constexpr mat3 kBradfordInv = MakeBradfordInv();

        [[nodiscard]] constexpr mat3 BradfordAdaptation(Chromaticity src, Chromaticity dst) {
            if (src == dst) {
                return Math::Identity<mat3>();
            }
            vec3 srcCone = kBradford * WhiteXYZ(src);
            vec3 dstCone = kBradford * WhiteXYZ(dst);

            mat3 scale = Math::Identity<mat3>();
            scale[0, 0] = dstCone.x / srcCone.x;
            scale[1, 1] = dstCone.y / srcCone.y;
            scale[2, 2] = dstCone.z / srcCone.z;

            return kBradfordInv * scale * kBradford;
        }

        [[nodiscard]] constexpr mat3 PrimariesMatrix(const ColorSpaceDesc& cs) {
            mat3 p{};
            Chromaticity prim[3] = {cs.r, cs.g, cs.b};
            for (int i = 0; i < 3; ++i) {
                p[0, i] = prim[i].x / prim[i].y;
                p[1, i] = 1.0f;
                p[2, i] = (1.0f - prim[i].x - prim[i].y) / prim[i].y;
            }
            return p;
        }

        [[nodiscard]] constexpr mat3 ScaleColumns(const mat3& m, vec3 s) {
            mat3 r = m;
            for (int col = 0; col < 3; ++col) {
                r.columns[col] = r.columns[col] * s[col];
            }
            return r;
        }

        [[nodiscard]] constexpr mat3 NPMCS(const ColorSpaceDesc& cs) {
            mat3 p = PrimariesMatrix(cs);
            mat3 pInv = Math::Inverse(p);
            vec3 w = WhiteXYZ(cs.white);
            vec3 s = pInv * w;
            return ScaleColumns(p, s);
        }

    } // namespace Detail

    [[nodiscard]] constexpr mat3 BradfordAdaptation(Chromaticity src, Chromaticity dst) {
        return Detail::BradfordAdaptation(src, dst);
    }

    [[nodiscard]] constexpr mat3 NPM(const ColorSpaceDesc& cs) {
        return Detail::NPMCS(cs);
    }

    [[nodiscard]] constexpr mat3 InverseNPM(const ColorSpaceDesc& cs) {
        return Math::Inverse(Detail::NPMCS(cs));
    }

    // =====================================================================
    //  RGB ↔ XYZ Conversion Matrices
    // =====================================================================

    [[nodiscard]] constexpr mat3 RGBtoXYZ(const ColorSpaceDesc& cs) {
        return NPM(cs);
    }
    [[nodiscard]] constexpr mat3 XYZtoRGB(const ColorSpaceDesc& cs) {
        return InverseNPM(cs);
    }

    [[nodiscard]] constexpr mat3 RGBtoRGB(const ColorSpaceDesc& src, const ColorSpaceDesc& dst) {
        return InverseNPM(dst) * Detail::BradfordAdaptation(src.white, dst.white) * NPM(src);
    }

    // =====================================================================
    //  §9  Transfer Functions (OETF / EOTF)
    // =====================================================================

    namespace TF {

        // ----- sRGB (IEC 61966-2-1) -----

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T sRGB_EOTF(T v) {
            return (v <= T(0.04045)) ? v / T(12.92)
                                     : Math::Pow(((v + T(0.055)) / T(1.055)), T(2.4));
        }

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T sRGB_OETF(T v) {
            return (v <= T(0.0031308)) ? v * T(12.92)
                                       : T(1.055) * Math::Pow(v, T(1) / T(2.4)) - T(0.055);
        }

        // ----- BT.709 / BT.2020 -----

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T BT709_EOTF(T v) {
            return (v < T(0.081)) ? v / T(4.5)
                                  : Math::Pow((v + T(0.099)) / T(1.099), T(1) / T(0.45));
        }

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T BT709_OETF(T v) {
            return (v < T(0.018)) ? v * T(4.5) : T(1.099) * Math::Pow(v, T(0.45)) - T(0.099);
        }

        // ----- Pure gamma -----

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T GammaEncode(T v, T gamma) {
            return (v <= T(0)) ? T(0) : Math::Pow(v, T(1) / gamma);
        }

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T GammaDecode(T v, T gamma) {
            return (v <= T(0)) ? T(0) : Math::Pow(v, gamma);
        }

        // ----- PQ (SMPTE ST 2084) -----

        namespace PQConst {

            template<std::floating_point T>
            inline constexpr T m1 = T(0.1593017578125);
            template<std::floating_point T>
            inline constexpr T m2 = T(78.84375);
            template<std::floating_point T>
            inline constexpr T c1 = T(0.8359375);
            template<std::floating_point T>
            inline constexpr T c2 = T(18.8515625);
            template<std::floating_point T>
            inline constexpr T c3 = T(18.6875);

        } // namespace PQConst

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T PQ_EOTF(T v) {
            T Vm1 = Math::Pow(v, T(1) / PQConst::m2<T>);
            T num = Vm1 - PQConst::c1<T>;
            if (num < T(0)) {
                num = T(0);
            }
            T den = PQConst::c2<T> - PQConst::c3<T> * Vm1;
            return Math::Pow(num / den, T(1) / PQConst::m1<T>) * T(10000);
        }

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T PQ_OETF(T v) {
            T Y = v / T(10000);
            T Ym1 = Math::Pow(Y, PQConst::m1<T>);
            T num = PQConst::c1<T> + PQConst::c2<T> * Ym1;
            T den = T(1) + PQConst::c3<T> * Ym1;
            return Math::Pow(num / den, PQConst::m2<T>);
        }

        // ----- HLG (ARIB STD-B67) -----

        namespace HLGConst {

            template<std::floating_point T>
            inline constexpr T a = T(0.17883277);
            template<std::floating_point T>
            inline constexpr T b = T(0.28466892);
            template<std::floating_point T>
            inline constexpr T c = T(0.55991073);

        } // namespace HLGConst

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T HLG_OETF(T v) {
            return (v <= T(1) / T(12))
                       ? Math::Sqrt(T(3) * v)
                       : HLGConst::a<T> * Math::Log(T(12) * v - HLGConst::b<T>) + HLGConst::c<T>;
        }

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T HLG_EOTF(T v) {
            return (v <= T(0.5))
                       ? v * v / T(3)
                       : (Math::Exp((v - HLGConst::c<T>) / HLGConst::a<T>) + HLGConst::b<T>) /
                             T(12);
        }

        // ----- ACEScct -----

        namespace ACEScctConst {

            template<std::floating_point T>
            inline constexpr T cutLin = T(0.0078125);
            template<std::floating_point T>
            inline constexpr T cutLog = T(0.155251141552511);
            template<std::floating_point T>
            inline constexpr T A = T(10.5402377416545);
            template<std::floating_point T>
            inline constexpr T B = T(0.0729055341958355);

        } // namespace ACEScctConst

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T ACEScct_Encode(T v) {
            return (v <= ACEScctConst::cutLin<T>)
                       ? (v + ACEScctConst::B<T>)*ACEScctConst::A<T>
                       : (Math::Log(v) / Math::Log(T(2)) + T(9.72)) / T(17.52);
        }

        template<std::floating_point T = float>
        [[nodiscard]] constexpr T ACEScct_Decode(T v) {
            return (v <= ACEScctConst::cutLog<T>) ? v / ACEScctConst::A<T> - ACEScctConst::B<T>
                                                  : Math::Pow(T(2), v * T(17.52) - T(9.72));
        }

    } // namespace TF

    template<std::floating_point T = float>
    [[nodiscard]] constexpr T Encode(TransferFn tf, T linear) {
        switch (tf) {
        case TransferFn::Linear:
            return linear;
        case TransferFn::sRGB:
            return TF::sRGB_OETF<T>(linear);
        case TransferFn::BT709:
            return TF::BT709_OETF<T>(linear);
        case TransferFn::Gamma22:
            return TF::GammaEncode<T>(linear, T(2.2));
        case TransferFn::Gamma26:
            return TF::GammaEncode<T>(linear, T(2.6));
        case TransferFn::PQ:
            return TF::PQ_OETF<T>(linear);
        case TransferFn::HLG:
            return TF::HLG_OETF<T>(linear);
        case TransferFn::ACEScct:
            return TF::ACEScct_Encode<T>(linear);
        }
        return linear;
    }

    template<std::floating_point T = float>
    [[nodiscard]] constexpr T Decode(TransferFn tf, T encoded) {
        switch (tf) {
        case TransferFn::Linear:
            return encoded;
        case TransferFn::sRGB:
            return TF::sRGB_EOTF<T>(encoded);
        case TransferFn::BT709:
            return TF::BT709_EOTF<T>(encoded);
        case TransferFn::Gamma22:
            return TF::GammaDecode<T>(encoded, T(2.2));
        case TransferFn::Gamma26:
            return TF::GammaDecode<T>(encoded, T(2.6));
        case TransferFn::PQ:
            return TF::PQ_EOTF<T>(encoded);
        case TransferFn::HLG:
            return TF::HLG_EOTF<T>(encoded);
        case TransferFn::ACEScct:
            return TF::ACEScct_Decode<T>(encoded);
        }
        return encoded;
    }

    [[nodiscard]] constexpr vec3 Encode(TransferFn tf, vec3 linear) {
        return {Encode<float>(tf, linear.x), Encode<float>(tf, linear.y),
                Encode<float>(tf, linear.z)};
    }

    [[nodiscard]] constexpr vec3 Decode(TransferFn tf, vec3 encoded) {
        return {Decode<float>(tf, encoded.x), Decode<float>(tf, encoded.y),
                Decode<float>(tf, encoded.z)};
    }

    // =====================================================================
    //  §10  Strongly-Typed Color<CS>
    // =====================================================================

    template<ColorSpaceDesc CS>
    struct Color {
        vec3 v;

        [[nodiscard]] constexpr float& r() { return v.x; }
        [[nodiscard]] constexpr float& g() { return v.y; }
        [[nodiscard]] constexpr float& b() { return v.z; }
        [[nodiscard]] constexpr float r() const { return v.x; }
        [[nodiscard]] constexpr float g() const { return v.y; }
        [[nodiscard]] constexpr float b() const { return v.z; }

        [[nodiscard]] constexpr float& operator[](int i) { return v[i]; }
        [[nodiscard]] constexpr float operator[](int i) const { return v[i]; }

        [[nodiscard]] static consteval const ColorSpaceDesc& space() { return CS; }
        [[nodiscard]] static consteval TransferFn transfer() { return CS.transfer; }

        [[nodiscard]] friend constexpr bool operator==(const Color&, const Color&) = default;

        [[nodiscard]] constexpr Color operator+(const Color& o) const { return {v + o.v}; }
        [[nodiscard]] constexpr Color operator-(const Color& o) const { return {v - o.v}; }
        [[nodiscard]] constexpr Color operator*(float s) const { return {v * s}; }
        [[nodiscard]] constexpr Color operator/(float s) const { return {v / s}; }
        [[nodiscard]] friend constexpr Color operator*(float s, const Color& c) {
            return {c.v * s};
        }

        constexpr Color& operator+=(const Color& o) {
            v += o.v;
            return *this;
        }
        constexpr Color& operator-=(const Color& o) {
            v -= o.v;
            return *this;
        }
        constexpr Color& operator*=(float s) {
            v *= s;
            return *this;
        }
        constexpr Color& operator/=(float s) {
            v /= s;
            return *this;
        }
    };

    // =====================================================================
    //  §11  Conversion Between Color Spaces
    // =====================================================================

    template<ColorSpaceDesc Dst, ColorSpaceDesc Src>
    [[nodiscard]] constexpr Color<Dst> Convert(Color<Src> src) {
        if constexpr (Dst == Src) {
            return src;
        } else {
            vec3 linear = Decode(Src.transfer, src.v);
            constexpr mat3 srcNPM = NPM(Src);
            vec3 xyz = srcNPM * linear;

            if constexpr (!(Src.white == Dst.white)) {
                constexpr mat3 adapt = Detail::BradfordAdaptation(Src.white, Dst.white);
                xyz = adapt * xyz;
            }

            constexpr mat3 dstNPMInv = Math::Inverse(NPM(Dst));
            vec3 dstLinear = dstNPMInv * xyz;
            return Color<Dst>{Encode(Dst.transfer, dstLinear)};
        }
    }

    template<ColorSpaceDesc Dst>
    [[nodiscard]] constexpr vec3 ConvertVec3(const ColorSpaceDesc& src, vec3 v) {
        vec3 linear = Decode(src.transfer, v);
        mat3 combined =
            InverseNPM(Dst) * Detail::BradfordAdaptation(src.white, Dst.white) * NPM(src);
        return Encode(Dst.transfer, combined * linear);
    }

    // =====================================================================
    //  CIE XYZ Utilities
    // =====================================================================

    [[nodiscard]] constexpr vec3 ToXYZ(const ColorSpaceDesc& cs, vec3 rgb) {
        return NPM(cs) * Decode(cs.transfer, rgb);
    }

    [[nodiscard]] constexpr vec3 FromXYZ(const ColorSpaceDesc& cs, vec3 xyz) {
        return Encode(cs.transfer, InverseNPM(cs) * xyz);
    }

    [[nodiscard]] constexpr float Luminance(const ColorSpaceDesc& cs, vec3 rgb) {
        vec3 xyz = NPM(cs) * Decode(cs.transfer, rgb);
        return xyz.y;
    }

    template<ColorSpaceDesc CS>
    [[nodiscard]] constexpr float Luminance(Color<CS> c) {
        return Luminance(CS, c.v);
    }

    // =====================================================================
    //  §13  CIE L*a*b* (CIELAB)
    // =====================================================================

    namespace Lab {

        inline constexpr float kDelta = 6.0f / 29.0f;
        inline constexpr float kDelta2 = kDelta * kDelta;
        inline constexpr float kDelta3 = kDelta * kDelta * kDelta;

        [[nodiscard]] constexpr float F(float t) {
            return (t > kDelta3) ? Cbrt(t) : t / (3.0f * kDelta2) + 4.0f / 29.0f;
        }

        [[nodiscard]] constexpr float FInv(float t) {
            return (t > kDelta) ? t * t * t : 3.0f * kDelta2 * (t - 4.0f / 29.0f);
        }

        [[nodiscard]] constexpr vec3 FromXYZ(vec3 xyz, Chromaticity white = Illuminant::D65) {
            float Xn = white.x / white.y;
            float Yn = 1.0f;
            float Zn = (1.0f - white.x - white.y) / white.y;
            float fx = F(xyz.x / Xn);
            float fy = F(xyz.y / Yn);
            float fz = F(xyz.z / Zn);
            return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
        }

        [[nodiscard]] constexpr vec3 ToXYZ(vec3 lab, Chromaticity white = Illuminant::D65) {
            float Xn = white.x / white.y;
            float Yn = 1.0f;
            float Zn = (1.0f - white.x - white.y) / white.y;
            float fy = (lab.x + 16.0f) / 116.0f;
            float fx = lab.y / 500.0f + fy;
            float fz = fy - lab.z / 200.0f;
            return {Xn * FInv(fx), Yn * FInv(fy), Zn * FInv(fz)};
        }

    } // namespace Lab

    // =====================================================================
    //  §14  CIE L*u*v* (CIELUV)
    // =====================================================================

    namespace Luv {

        [[nodiscard]] constexpr float Up(vec3 xyz) {
            float d = xyz.x + 15.0f * xyz.y + 3.0f * xyz.z;
            return (d > 0.0f) ? 4.0f * xyz.x / d : 0.0f;
        }

        [[nodiscard]] constexpr float Vp(vec3 xyz) {
            float d = xyz.x + 15.0f * xyz.y + 3.0f * xyz.z;
            return (d > 0.0f) ? 9.0f * xyz.y / d : 0.0f;
        }

        [[nodiscard]] constexpr vec3 FromXYZ(vec3 xyz, Chromaticity white = Illuminant::D65) {
            float Yn = 1.0f;
            float yr = xyz.y / Yn;

            float L = (yr > Lab::kDelta3) ? 116.0f * Cbrt(yr) - 16.0f : 903.3f * yr;

            vec3 wXYZ = {white.x / white.y, 1.0f, (1.0f - white.x - white.y) / white.y};
            float upn = Up(wXYZ);
            float vpn = Vp(wXYZ);
            float up = Up(xyz);
            float vp = Vp(xyz);

            return {L, 13.0f * L * (up - upn), 13.0f * L * (vp - vpn)};
        }

        [[nodiscard]] constexpr vec3 ToXYZ(vec3 luv, Chromaticity white = Illuminant::D65) {
            float L = luv.x, u = luv.y, v = luv.z;
            if (L <= 0.0f) {
                return {0.0f, 0.0f, 0.0f};
            }

            vec3 wXYZ = {white.x / white.y, 1.0f, (1.0f - white.x - white.y) / white.y};
            float upn = Up(wXYZ);
            float vpn = Vp(wXYZ);

            float up = u / (13.0f * L) + upn;
            float vp = v / (13.0f * L) + vpn;

            float Y = (L > 8.0f) ? Math::Pow((L + 16.0f) / 116.0f, 3.0f) : L / 903.3f;
            float X = Y * 9.0f * up / (4.0f * vp);
            float Z = Y * (12.0f - 3.0f * up - 20.0f * vp) / (4.0f * vp);
            return {X, Y, Z};
        }

    } // namespace Luv

    // =====================================================================
    //  §15  Oklab (Björn Ottosson 2020)
    // =====================================================================

    namespace Oklab {

        namespace Detail {

            [[nodiscard]] consteval mat3 MakeM1() {
                mat3 m{};
                m[0, 0] = 0.8189330101f;
                m[0, 1] = 0.3618667424f;
                m[0, 2] = -0.1288597137f;
                m[1, 0] = 0.0329845436f;
                m[1, 1] = 0.9293118715f;
                m[1, 2] = 0.0361456387f;
                m[2, 0] = 0.0482003018f;
                m[2, 1] = 0.2643662691f;
                m[2, 2] = 0.6338517070f;
                return m;
            }
            [[nodiscard]] consteval mat3 MakeM2() {
                mat3 m{};
                m[0, 0] = 0.2104542553f;
                m[0, 1] = 0.7936177850f;
                m[0, 2] = -0.0040720468f;
                m[1, 0] = 1.9779984951f;
                m[1, 1] = -2.4285922050f;
                m[1, 2] = 0.4505937099f;
                m[2, 0] = 0.0259040371f;
                m[2, 1] = 0.7827717662f;
                m[2, 2] = -0.8086757660f;
                return m;
            }
            [[nodiscard]] consteval mat3 MakeM1Inv() {
                mat3 m{};
                m[0, 0] = 1.2270138511f;
                m[0, 1] = -0.5577999807f;
                m[0, 2] = 0.2812561490f;
                m[1, 0] = -0.0405801784f;
                m[1, 1] = 1.1122568696f;
                m[1, 2] = -0.0716766787f;
                m[2, 0] = -0.0763812845f;
                m[2, 1] = -0.4214819784f;
                m[2, 2] = 1.5861632204f;
                return m;
            }
            [[nodiscard]] consteval mat3 MakeM2Inv() {
                mat3 m{};
                m[0, 0] = 1.0f;
                m[0, 1] = 0.3963377774f;
                m[0, 2] = 0.2158037573f;
                m[1, 0] = 1.0f;
                m[1, 1] = -0.1055613458f;
                m[1, 2] = -0.0638541728f;
                m[2, 0] = 1.0f;
                m[2, 1] = -0.0894841775f;
                m[2, 2] = -1.2914855480f;
                return m;
            }

        } // namespace Detail

        inline constexpr mat3 kM1 = Detail::MakeM1();
        inline constexpr mat3 kM2 = Detail::MakeM2();
        inline constexpr mat3 kM1Inv = Detail::MakeM1Inv();
        inline constexpr mat3 kM2Inv = Detail::MakeM2Inv();

        [[nodiscard]] constexpr vec3 FromXYZ(vec3 xyz) {
            vec3 lms = kM1 * xyz;
            lms = {Cbrt(lms.x), Cbrt(lms.y), Cbrt(lms.z)};
            return kM2 * lms;
        }

        [[nodiscard]] constexpr vec3 ToXYZ(vec3 lab) {
            vec3 lms = kM2Inv * lab;
            lms = {lms.x * lms.x * lms.x, lms.y * lms.y * lms.y, lms.z * lms.z * lms.z};
            return kM1Inv * lms;
        }

        [[nodiscard]] constexpr vec3 FromLinearSRGB(vec3 rgb) {
            // Optimized direct path: linear sRGB → Oklab without going through XYZ first
            float l = 0.4122214708f * rgb.x + 0.5363325363f * rgb.y + 0.0514459929f * rgb.z;
            float m = 0.2119034982f * rgb.x + 0.6806995451f * rgb.y + 0.1073969566f * rgb.z;
            float s = 0.0883024619f * rgb.x + 0.2817188376f * rgb.y + 0.6299787005f * rgb.z;

            l = Cbrt(l);
            m = Cbrt(m);
            s = Cbrt(s);

            return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
                    1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
                    0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
        }

        [[nodiscard]] constexpr vec3 ToLinearSRGB(vec3 lab) {
            float l = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
            float m = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
            float s = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;

            l = l * l * l;
            m = m * m * m;
            s = s * s * s;

            return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                    -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                    -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
        }

    } // namespace Oklab

    // =====================================================================
    //  §16  Oklch (Oklab in cylindrical form)
    // =====================================================================

    namespace Oklch {

        [[nodiscard]] constexpr vec3 FromOklab(vec3 lab) {
            float C = Math::Sqrt(lab.y * lab.y + lab.z * lab.z);
            float h = Math::Atan2(lab.z, lab.y);
            return {lab.x, C, h};
        }

        [[nodiscard]] constexpr vec3 ToOklab(vec3 lch) {
            float a = lch.y * Math::Cos(lch.z);
            float b = lch.y * Math::Sin(lch.z);
            return {lch.x, a, b};
        }

        [[nodiscard]] constexpr vec3 FromLinearSRGB(vec3 rgb) {
            return FromOklab(Oklab::FromLinearSRGB(rgb));
        }

        [[nodiscard]] constexpr vec3 ToLinearSRGB(vec3 lch) {
            return Oklab::ToLinearSRGB(ToOklab(lch));
        }

    } // namespace Oklch

    // =====================================================================
    //  §17  Delta E Color Differences
    // =====================================================================

    namespace DeltaE {

        [[nodiscard]] constexpr float CIE76(vec3 lab1, vec3 lab2) {
            float dL = lab2.x - lab1.x;
            float da = lab2.y - lab1.y;
            float db = lab2.z - lab1.z;
            return Math::Sqrt(dL * dL + da * da + db * db);
        }

        [[nodiscard]] constexpr float CIE94(vec3 lab1, vec3 lab2, float kL = 1.0f,
                                            float K1 = 0.045f, float K2 = 0.015f) {
            float dL = lab1.x - lab2.x;
            float C1 = Math::Sqrt(lab1.y * lab1.y + lab1.z * lab1.z);
            float C2 = Math::Sqrt(lab2.y * lab2.y + lab2.z * lab2.z);
            float dC = C1 - C2;
            float da = lab1.y - lab2.y;
            float db = lab1.z - lab2.z;
            float dH2 = da * da + db * db - dC * dC;
            if (dH2 < 0.0f) {
                dH2 = 0.0f;
            }

            float SL = 1.0f;
            float SC = 1.0f + K1 * C1;
            float SH = 1.0f + K2 * C1;

            float t1 = dL / (kL * SL);
            float t2 = dC / SC;
            float t3sq = dH2 / (SH * SH);

            return Math::Sqrt(t1 * t1 + t2 * t2 + t3sq);
        }

        [[nodiscard]] constexpr float CIEDE2000(vec3 lab1, vec3 lab2) {
            float L1 = lab1.x, a1 = lab1.y, b1 = lab1.z;
            float L2 = lab2.x, a2 = lab2.y, b2 = lab2.z;

            float Cab1 = Math::Sqrt(a1 * a1 + b1 * b1);
            float Cab2 = Math::Sqrt(a2 * a2 + b2 * b2);
            float CabAvg = (Cab1 + Cab2) * 0.5f;

            float CabAvg7 = CabAvg * CabAvg * CabAvg * CabAvg * CabAvg * CabAvg * CabAvg;
            float G = 0.5f * (1.0f - Math::Sqrt(CabAvg7 / (CabAvg7 + 6103515625.0f))); // 25^7

            float ap1 = a1 * (1.0f + G);
            float ap2 = a2 * (1.0f + G);

            float Cp1 = Math::Sqrt(ap1 * ap1 + b1 * b1);
            float Cp2 = Math::Sqrt(ap2 * ap2 + b2 * b2);

            float hp1 = Math::Atan2(b1, ap1);
            float hp2 = Math::Atan2(b2, ap2);
            if (hp1 < 0.0f) {
                hp1 += Math::Const::kTau<float>;
            }
            if (hp2 < 0.0f) {
                hp2 += Math::Const::kTau<float>;
            }

            float dLp = L2 - L1;
            float dCp = Cp2 - Cp1;

            float dhp = hp2 - hp1;
            if (Math::Abs(dhp) > Math::Const::kPi<float>) {
                dhp += (dhp < 0.0f) ? Math::Const::kTau<float> : -Math::Const::kTau<float>;
            }
            if (Cp1 * Cp2 == 0.0f) {
                dhp = 0.0f;
            }
            float dHp = 2.0f * Math::Sqrt(Cp1 * Cp2) * Math::Sin(dhp * 0.5f);

            float Lp = (L1 + L2) * 0.5f;
            float Cp = (Cp1 + Cp2) * 0.5f;

            float hpAvg;
            if (Cp1 * Cp2 == 0.0f) {
                hpAvg = hp1 + hp2;
            } else if (Math::Abs(hp1 - hp2) <= Math::Const::kPi<float>) {
                hpAvg = (hp1 + hp2) * 0.5f;
            } else if (hp1 + hp2 < Math::Const::kTau<float>) {
                hpAvg = (hp1 + hp2 + Math::Const::kTau<float>)*0.5f;
            } else {
                hpAvg = (hp1 + hp2 - Math::Const::kTau<float>)*0.5f;
            }

            float T = 1.0f - 0.17f * Math::Cos(hpAvg - Math::Radians(30.0f)) +
                      0.24f * Math::Cos(2.0f * hpAvg) +
                      0.32f * Math::Cos(3.0f * hpAvg + Math::Radians(6.0f)) -
                      0.20f * Math::Cos(4.0f * hpAvg - Math::Radians(63.0f));

            float Lp50sq = (Lp - 50.0f) * (Lp - 50.0f);
            float SL = 1.0f + 0.015f * Lp50sq / Math::Sqrt(20.0f + Lp50sq);
            float SC = 1.0f + 0.045f * Cp;
            float SH = 1.0f + 0.015f * Cp * T;

            float Cp7 = Cp * Cp * Cp * Cp * Cp * Cp * Cp;
            float RC = 2.0f * Math::Sqrt(Cp7 / (Cp7 + 6103515625.0f));

            float hpDeg = Math::Degrees(hpAvg);
            if (hpDeg < 0.0f) {
                hpDeg += 360.0f;
            }
            float dTheta = Math::Radians(30.0f) *
                           Math::Exp(-((hpDeg - 275.0f) / 25.0f) * ((hpDeg - 275.0f) / 25.0f));
            float RT = -Math::Sin(2.0f * dTheta) * RC;

            float t1 = dLp / SL;
            float t2 = dCp / SC;
            float t3 = dHp / SH;

            return Math::Sqrt(t1 * t1 + t2 * t2 + t3 * t3 + RT * t2 * t3);
        }

        [[nodiscard]] constexpr float OklabDE(vec3 oklab1, vec3 oklab2) {
            float dL = oklab2.x - oklab1.x;
            float da = oklab2.y - oklab1.y;
            float db = oklab2.z - oklab1.z;
            return Math::Sqrt(dL * dL + da * da + db * db);
        }

    } // namespace DeltaE

    // =====================================================================
    //  §18  Gamut Operations
    // =====================================================================

    [[nodiscard]] constexpr bool InGamut(vec3 rgb, float tolerance = 0.0f) {
        return rgb.x >= -tolerance && rgb.x <= 1.0f + tolerance && rgb.y >= -tolerance &&
               rgb.y <= 1.0f + tolerance && rgb.z >= -tolerance && rgb.z <= 1.0f + tolerance;
    }

    [[nodiscard]] constexpr vec3 ClampGamut(vec3 rgb) {
        return {Math::Clamp(rgb.x, 0.0f, 1.0f), Math::Clamp(rgb.y, 0.0f, 1.0f),
                Math::Clamp(rgb.z, 0.0f, 1.0f)};
    }

    [[nodiscard]] constexpr vec3 GamutMapOklab(vec3 linearSrcRGB, const ColorSpaceDesc& src,
                                               const ColorSpaceDesc& dst, float targetDE = 0.02f) {
        // Convert source linear RGB to Oklab
        vec3 srcXYZ = ToXYZ(src.WithTransfer(TransferFn::Linear), linearSrcRGB);
        vec3 oklabSrc = Oklab::FromXYZ(srcXYZ);
        float L = oklabSrc.x;
        float C = Math::Sqrt(oklabSrc.y * oklabSrc.y + oklabSrc.z * oklabSrc.z);
        float h = Math::Atan2(oklabSrc.z, oklabSrc.y);

        if (C < 1e-6f) {
            // Achromatic — just clamp lightness
            vec3 grey = {Math::Clamp(L, 0.0f, 1.0f), 0.0f, 0.0f};
            return Oklab::ToLinearSRGB(grey);
        }

        // Binary search on chroma
        float lo = 0.0f, hi = C;
        for (int i = 0; i < 32; ++i) {
            float mid = (lo + hi) * 0.5f;
            vec3 testLab = {L, mid * Math::Cos(h), mid * Math::Sin(h)};
            vec3 testXYZ = Oklab::ToXYZ(testLab);
            vec3 testRGB = FromXYZ(dst.WithTransfer(TransferFn::Linear), testXYZ);
            if (InGamut(testRGB, 0.001f)) {
                lo = mid;
            } else {
                hi = mid;
            }
        }

        vec3 mappedLab = {L, lo * Math::Cos(h), lo * Math::Sin(h)};
        vec3 mappedXYZ = Oklab::ToXYZ(mappedLab);
        return FromXYZ(dst.WithTransfer(TransferFn::Linear), mappedXYZ);
    }

    // Simplified overload that works in Oklab space for sRGB directly
    [[nodiscard]] constexpr vec3 GamutMapToSRGB(vec3 oklabColor) {
        float L = oklabColor.x;
        float C = Math::Sqrt(oklabColor.y * oklabColor.y + oklabColor.z * oklabColor.z);
        float h = Math::Atan2(oklabColor.z, oklabColor.y);

        if (C < 1e-6f) {
            return Oklab::ToLinearSRGB({Math::Clamp(L, 0.0f, 1.0f), 0.0f, 0.0f});
        }

        float lo = 0.0f, hi = C;
        for (int i = 0; i < 32; ++i) {
            float mid = (lo + hi) * 0.5f;
            vec3 test = Oklab::ToLinearSRGB({L, mid * Math::Cos(h), mid * Math::Sin(h)});
            if (InGamut(test, 0.001f)) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return Oklab::ToLinearSRGB({L, lo * Math::Cos(h), lo * Math::Sin(h)});
    }

    // =====================================================================
    //  §19  Correlated Color Temperature (CCT)
    // =====================================================================

    [[nodiscard]] constexpr float CCTFromXY(Chromaticity c) {
        // McCamy's approximation (valid ~2000K-12500K)
        float n = (c.x - 0.3320f) / (0.1858f - c.y);
        return 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;
    }

    [[nodiscard]] constexpr Chromaticity CCTtoXY(float T) {
        // CIE daylight locus approximation (4000K-25000K)
        float x;
        if (T <= 7000.0f) {
            x = -4.6070e9f / (T * T * T) + 2.9678e6f / (T * T) + 0.09911e3f / T + 0.244063f;
        } else {
            x = -2.0064e9f / (T * T * T) + 1.9018e6f / (T * T) + 0.24748e3f / T + 0.237040f;
        }
        float y = -3.0f * x * x + 2.87f * x - 0.275f;
        return {x, y};
    }

    // =====================================================================
    //  §20  Blackbody Spectrum (Planckian Locus)
    // =====================================================================

    [[nodiscard]] constexpr vec3 BlackbodyXYZ(float T) {
        // Approximate: use CIE daylight locus for chromaticity, scale Y=1
        Chromaticity c = CCTtoXY(Math::Clamp(T, 1667.0f, 25000.0f));
        return {c.x / c.y, 1.0f, (1.0f - c.x - c.y) / c.y};
    }

    template<ColorSpaceDesc CS>
    [[nodiscard]] constexpr Color<CS> BlackbodyColor(float T) {
        vec3 xyz = BlackbodyXYZ(T);
        vec3 rgb = FromXYZ(CS, xyz);
        // Normalize so max channel = 1
        float mx = Math::Max(rgb.x, rgb.y, rgb.z);
        if (mx > 0.0f) {
            rgb = rgb / mx;
        }
        return Color<CS>{rgb};
    }

    // =====================================================================
    //  §21  HSV / HSL (sRGB-native convenience)
    // =====================================================================

    namespace HSV {

        [[nodiscard]] constexpr vec3 FromRGB(vec3 rgb) {
            float R = rgb.x, G = rgb.y, B = rgb.z;
            float mx = Math::Max(R, G, B);
            float mn = Math::Min(R, G, B);
            float d = mx - mn;
            float V = mx;
            float S = (mx > 0.0f) ? d / mx : 0.0f;
            float H = 0.0f;
            if (d > 0.0f) {
                if (mx == R) {
                    H = (G - B) / d + (G < B ? 6.0f : 0.0f);
                } else if (mx == G) {
                    H = (B - R) / d + 2.0f;
                } else {
                    H = (R - G) / d + 4.0f;
                }
                H /= 6.0f;
            }
            return {H, S, V};
        }

        [[nodiscard]] constexpr vec3 ToRGB(vec3 hsv) {
            float H = hsv.x * 6.0f, S = hsv.y, V = hsv.z;
            int hi = static_cast<int>(H);
            float f = H - static_cast<float>(hi);
            float p = V * (1.0f - S);
            float q = V * (1.0f - S * f);
            float t = V * (1.0f - S * (1.0f - f));
            switch (hi % 6) {
            case 0:
                return {V, t, p};
            case 1:
                return {q, V, p};
            case 2:
                return {p, V, t};
            case 3:
                return {p, q, V};
            case 4:
                return {t, p, V};
            default:
                return {V, p, q};
            }
        }

    } // namespace HSV

    namespace HSL {

        [[nodiscard]] constexpr vec3 FromRGB(vec3 rgb) {
            float R = rgb.x, G = rgb.y, B = rgb.z;
            float mx = Math::Max(R, G, B);
            float mn = Math::Min(R, G, B);
            float L = (mx + mn) * 0.5f;
            float d = mx - mn;
            float S = 0.0f;
            if (d > 0.0f) {
                S = (L > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);
            }
            float H = 0.0f;
            if (d > 0.0f) {
                if (mx == R) {
                    H = (G - B) / d + (G < B ? 6.0f : 0.0f);
                } else if (mx == G) {
                    H = (B - R) / d + 2.0f;
                } else {
                    H = (R - G) / d + 4.0f;
                }
                H /= 6.0f;
            }
            return {H, S, L};
        }

        [[nodiscard]] constexpr vec3 ToRGB(vec3 hsl) {
            float H = hsl.x, S = hsl.y, L = hsl.z;
            if (S == 0.0f) {
                return {L, L, L};
            }

            float q = (L < 0.5f) ? L * (1.0f + S) : L + S - L * S;
            float p = 2.0f * L - q;

            auto hue2rgb = [](float p, float q, float t) -> float {
                if (t < 0.0f) {
                    t += 1.0f;
                }
                if (t > 1.0f) {
                    t -= 1.0f;
                }
                if (t < 1.0f / 6.0f) {
                    return p + (q - p) * 6.0f * t;
                }
                if (t < 1.0f / 2.0f) {
                    return q;
                }
                if (t < 2.0f / 3.0f) {
                    return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
                }
                return p;
            };

            return {hue2rgb(p, q, H + 1.0f / 3.0f), hue2rgb(p, q, H),
                    hue2rgb(p, q, H - 1.0f / 3.0f)};
        }

    } // namespace HSL

    // =====================================================================
    //  §22  Color Constants
    // =====================================================================

    namespace Named {

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> Black = Color<CS>{{0.0f, 0.0f, 0.0f}};

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> White = Color<CS>{{1.0f, 1.0f, 1.0f}};

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> Red = Color<CS>{{1.0f, 0.0f, 0.0f}};

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> Green = Color<CS>{{0.0f, 1.0f, 0.0f}};

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> Blue = Color<CS>{{0.0f, 0.0f, 1.0f}};

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> Yellow = Color<CS>{{1.0f, 1.0f, 0.0f}};

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> Cyan = Color<CS>{{0.0f, 1.0f, 1.0f}};

        template<ColorSpaceDesc CS = CS::sRGB>
        inline constexpr Color<CS> Magenta = Color<CS>{{1.0f, 0.0f, 1.0f}};

    } // namespace Named

    // =====================================================================
    //  §23  Compile-time Verification & Static Assertions
    // =====================================================================

    namespace Detail {

        consteval bool VerifyNPMWhitePoint(const ColorSpaceDesc& cs) {
            mat3 npm = NPM(cs);
            vec3 white = npm * vec3{1.0f, 1.0f, 1.0f};
            vec3 expected = WhiteXYZ(cs.white);
            vec3 d = white - expected;
            float err = d.x * d.x + d.y * d.y + d.z * d.z;
            return err < 0.001f;
        }

        static_assert(VerifyNPMWhitePoint(CS::sRGB), "sRGB NPM white-point verification");
        static_assert(VerifyNPMWhitePoint(CS::DisplayP3),
                      "Display P3 NPM white-point verification");
        static_assert(VerifyNPMWhitePoint(CS::BT2020), "BT.2020 NPM white-point verification");
        static_assert(VerifyNPMWhitePoint(CS::AdobeRGB), "Adobe RGB NPM white-point verification");
        static_assert(VerifyNPMWhitePoint(CS::ProPhotoRGB),
                      "ProPhoto RGB NPM white-point verification");
        static_assert(VerifyNPMWhitePoint(CS::ACES_AP0), "ACES AP0 NPM white-point verification");
        static_assert(VerifyNPMWhitePoint(CS::ACES_AP1), "ACES AP1 NPM white-point verification");

        consteval bool VerifyRoundTrip() {
            mat3 npm = NPM(CS::sRGB);
            mat3 I = Math::Inverse(npm) * npm;
            float err = 0.0f;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    float delta = I[i, j] - (i == j ? 1.0f : 0.0f);
                    err += delta * delta;
                }
            }
            return err < 0.0001f;
        }

        static_assert(VerifyRoundTrip(), "NPM * NPM^-1 ≈ I round-trip verification");

    } // namespace Detail

    // =====================================================================
    //  RGBA Color
    // =====================================================================

    /**
     * @brief RGBA color — extends Color<CS> with an alpha channel.
     *
     * Alpha represents coverage/opacity and is orthogonal to chromaticity;
     * it does NOT participate in color-space conversions.
     */

    template<ColorSpaceDesc CS>
    struct ColorA : Color<CS> {
        float a{1.0f};

        constexpr ColorA() = default;
        constexpr ColorA(float r, float g, float b, float alpha = 1.0f)
            : Color<CS>{{r, g, b}}, a{alpha} {}
        constexpr ColorA(Color<CS> rgb, float alpha = 1.0f) : Color<CS>{rgb}, a{alpha} {}

        [[nodiscard]] friend constexpr bool operator==(const ColorA&, const ColorA&) = default;

        [[nodiscard]] constexpr ColorA operator*(float s) const { return {this->v * s, a * s}; }
        [[nodiscard]] constexpr ColorA operator/(float s) const {
            float inv = 1.0f / s;
            return {this->v * inv, a * inv};
        }
    };

    template<ColorSpaceDesc CS>
    [[nodiscard]] constexpr ColorA<CS> Premultiply(ColorA<CS> c) {
        return {c.v * c.a, c.a};
    }

    template<ColorSpaceDesc CS>
    [[nodiscard]] constexpr ColorA<CS> Unpremultiply(ColorA<CS> c) {
        if (c.a <= 0.0f) {
            return {vec3{0.0f, 0.0f, 0.0f}, 0.0f};
        }
        float inv = 1.0f / c.a;
        return {c.v * inv, c.a};
    }

    template<ColorSpaceDesc Dst, ColorSpaceDesc Src>
    [[nodiscard]] constexpr ColorA<Dst> Convert(ColorA<Src> src) {
        Color<Dst> rgb = Convert<Dst>(static_cast<const Color<Src>&>(src));
        return {rgb, src.a};
    }

    // =====================================================================
    //  Quantized Integer Colors (uint8_t / uint16_t)
    // =====================================================================

    /** @brief Unsigned integer type suitable for pixel storage (8-bit or 16-bit). */
    template<typename U>
    concept QuantizedUint = std::same_as<U, uint8_t> || std::same_as<U, uint16_t>;

    /**
     * @brief Quantized RGB color in integer signal space (transfer-function encoded).
     *
     * Integer values [0, kMax] map to the [0, 1] encoded (post-OETF) range.
     * `toFloat()` decodes back to linear via the color space's transfer function.
     * `fromFloat()` encodes + quantizes with correct rounding.
     */
    template<ColorSpaceDesc CS, QuantizedUint U = uint8_t>
    struct PackedColor {
        U r{}, g{}, b{};

        static constexpr U kMax = std::numeric_limits<U>::max();

        [[nodiscard]] constexpr Color<CS> toFloat() const {
            float inv = 1.0f / float(kMax);
            float er = float(r) * inv;
            float eg = float(g) * inv;
            float eb = float(b) * inv;
            return Color<CS>{
                {Decode(CS.transfer, er), Decode(CS.transfer, eg), Decode(CS.transfer, eb)}};
        }

        [[nodiscard]] static constexpr PackedColor fromFloat(Color<CS> c) {
            float er = Clamp(Encode(CS.transfer, c.v.x), 0.0f, 1.0f);
            float eg = Clamp(Encode(CS.transfer, c.v.y), 0.0f, 1.0f);
            float eb = Clamp(Encode(CS.transfer, c.v.z), 0.0f, 1.0f);
            return {static_cast<U>(er * float(kMax) + 0.5f),
                    static_cast<U>(eg * float(kMax) + 0.5f),
                    static_cast<U>(eb * float(kMax) + 0.5f)};
        }

        [[nodiscard]] static constexpr PackedColor fromRaw(U rv, U gv, U bv) {
            return {rv, gv, bv};
        }

        [[nodiscard]] friend constexpr bool operator==(const PackedColor&,
                                                       const PackedColor&) = default;
    };

    /** @brief Quantized RGBA color — extends PackedColor with an alpha byte/short. */
    template<ColorSpaceDesc CS, QuantizedUint U = uint8_t>
    struct PackedColorA {
        U r{}, g{}, b{}, a{std::numeric_limits<U>::max()};

        static constexpr U kMax = std::numeric_limits<U>::max();

        [[nodiscard]] constexpr ColorA<CS> toFloat() const {
            float inv = 1.0f / float(kMax);
            float er = float(r) * inv;
            float eg = float(g) * inv;
            float eb = float(b) * inv;
            return {Decode(CS.transfer, er), Decode(CS.transfer, eg), Decode(CS.transfer, eb),
                    float(a) * inv};
        }

        [[nodiscard]] static constexpr PackedColorA fromFloat(ColorA<CS> c) {
            float er = Clamp(Encode(CS.transfer, c.r), 0.0f, 1.0f);
            float eg = Clamp(Encode(CS.transfer, c.g), 0.0f, 1.0f);
            float eb = Clamp(Encode(CS.transfer, c.b), 0.0f, 1.0f);
            float ea = Clamp(c.a, 0.0f, 1.0f);
            return {
                static_cast<U>(er * float(kMax) + 0.5f), static_cast<U>(eg * float(kMax) + 0.5f),
                static_cast<U>(eb * float(kMax) + 0.5f), static_cast<U>(ea * float(kMax) + 0.5f)};
        }

        [[nodiscard]] static constexpr PackedColorA fromRaw(U rv, U gv, U bv, U av) {
            return {rv, gv, bv, av};
        }

        [[nodiscard]] friend constexpr bool operator==(const PackedColorA&,
                                                       const PackedColorA&) = default;
    };

    /// @name Common packed-color type aliases
    /// @{
    using sRGB8 = PackedColor<CS::sRGB, uint8_t>;
    using sRGB16 = PackedColor<CS::sRGB, uint16_t>;
    using sRGBA8 = PackedColorA<CS::sRGB, uint8_t>;
    using sRGBA16 = PackedColorA<CS::sRGB, uint16_t>;
    /// @}

} // namespace Mashiro::Coloring
