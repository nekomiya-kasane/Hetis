#include "Mashiro/Math/MathUtils.h"
#include "Mashiro/Core/Types.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace Mashiro;
using Catch::Approx;

namespace {

    constexpr float kPi = 3.14159265358979323846f;

    void RequireVecApprox(vec3 a, vec3 b, float margin = 1e-5f) {
        REQUIRE(a.x == Approx(b.x).margin(margin));
        REQUIRE(a.y == Approx(b.y).margin(margin));
        REQUIRE(a.z == Approx(b.z).margin(margin));
    }

    void RequireMatApprox(const mat4& a, const mat4& b, float margin = 1e-4f) {
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                REQUIRE((a[row, col]) == Approx((b[row, col])).margin(margin));
            }
        }
    }

    // Reference column-major multiply, independent of operator* under test.
    [[nodiscard]] mat4 RefMul(const mat4& a, const mat4& b) {
        mat4 r{};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a[row, k] * b[k, col];
                }
                r[row, col] = sum;
            }
        }
        return r;
    }

    constexpr bool Close(float a, float b, float eps = 1e-4f) {
        float d = a - b;
        return (d < 0.0f ? -d : d) < eps;
    }

} // namespace

// ---------------------------------------------------------------------------
// Vector functions
// ---------------------------------------------------------------------------

TEST_CASE("Dot / Norm2 / Normalize agree with hand math", "[Core.MathUtils]") {
    REQUIRE(Math::Dot(vec3{1, 2, 3}, vec3{4, 5, 6}) == Approx(32.0f));
    REQUIRE(Math::Dot(vec2{1, 0}, vec2{0, 1}) == Approx(0.0f));
    REQUIRE(Math::Dot(vec4{1, 2, 3, 4}, vec4{1, 1, 1, 1}) == Approx(10.0f));

    REQUIRE(Math::Norm2Sq(vec3{2, 3, 6}) == Approx(49.0f));
    REQUIRE(Math::Norm2(vec3{2, 3, 6}) == Approx(7.0f));

    vec3 n = Math::Normalize(vec3{0, 3, 4});
    RequireVecApprox(n, vec3{0.0f, 0.6f, 0.8f});
    REQUIRE(Math::Norm2(n) == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Cross follows the right-hand rule", "[Core.MathUtils]") {
    RequireVecApprox(Math::Cross(vec3{1, 0, 0}, vec3{0, 1, 0}), vec3{0, 0, 1});
    REQUIRE(Math::Cross(vec2{1, 0}, vec2{0, 1}) == Approx(1.0f));
}

TEST_CASE("Min / Max / Clamp / Lerp / Reflect are component-wise", "[Core.MathUtils]") {
    RequireVecApprox(Math::Min(vec3{1, 5, 3}, vec3{4, 2, 6}), vec3{1, 2, 3});
    RequireVecApprox(Math::Max(vec3{1, 5, 3}, vec3{4, 2, 6}), vec3{4, 5, 6});
    RequireVecApprox(Math::Clamp(vec3{-1, 0.5f, 9}, vec3{0, 0, 0}, vec3{1, 1, 1}), vec3{0, 0.5f, 1});
    RequireVecApprox(Math::Lerp(vec3{0, 0, 0}, vec3{2, 4, 6}, 0.5f), vec3{1, 2, 3});
    RequireVecApprox(Math::Reflect(vec3{1, -1, 0}, vec3{0, 1, 0}), vec3{1, 1, 0});
}

TEST_CASE("Min / Max accept one or more vectors", "[Core.MathUtils]") {
    RequireVecApprox(Math::Min(vec3{1, 5, 3}), vec3{1, 5, 3}); // single arg is identity
    RequireVecApprox(Math::Max(vec3{1, 5, 3}), vec3{1, 5, 3});
    RequireVecApprox(Math::Min(vec3{1, 5, 3}, vec3{4, 2, 6}, vec3{0, 9, 2}), vec3{0, 2, 2});
    RequireVecApprox(Math::Max(vec3{1, 5, 3}, vec3{4, 2, 6}, vec3{0, 9, 2}), vec3{4, 9, 6});
}

TEST_CASE("Norm1 / Norm2 / NormInf; Distance uses L2", "[Core.MathUtils]") {
    vec3 v{3, -4, 12}; // |v|_1 = 19, |v|_2 = 13, |v|_inf = 12
    REQUIRE(Math::Norm1(v) == Approx(19.0f));
    REQUIRE(Math::Norm2(v) == Approx(13.0f));
    REQUIRE(Math::NormInf(v) == Approx(12.0f));

    vec3 a{1, 2, 3};
    vec3 b{4, 6, 3}; // b - a = (3, 4, 0)
    REQUIRE(Math::Distance(a, b) == Approx(5.0f));

    STATIC_REQUIRE(Close(Math::Norm1(vec3{3, -4, 12}), 19.0f));
    STATIC_REQUIRE(Close(Math::NormInf(vec3{3, -4, 12}), 12.0f));
}

// ---------------------------------------------------------------------------
// Operators (hidden friends from Types.h)
// ---------------------------------------------------------------------------

TEST_CASE("Vector operators are component-wise with scalar broadcast", "[Core.MathUtils]") {
    vec3 a{1, 2, 3};
    vec3 b{4, 5, 6};
    RequireVecApprox(a + b, vec3{5, 7, 9});
    RequireVecApprox(b - a, vec3{3, 3, 3});
    RequireVecApprox(a * b, vec3{4, 10, 18}); // Hadamard
    RequireVecApprox(a * 2.0f, vec3{2, 4, 6});
    RequireVecApprox(2.0f * a, vec3{2, 4, 6});
    RequireVecApprox(-a, vec3{-1, -2, -3});

    a += b;
    RequireVecApprox(a, vec3{5, 7, 9});
    a *= 2.0f;
    RequireVecApprox(a, vec3{10, 14, 18});

    REQUIRE(vec3{1, 2, 3} == vec3{1, 2, 3});
    REQUIRE(vec3{1, 2, 3} != vec3{1, 2, 4});
}

TEST_CASE("Matrix * vector transforms; matrix * matrix matches reference", "[Core.MathUtils]") {
    mat4 t = Math::MakeTranslation(vec3{10, 20, 30});
    vec4 p = t * vec4{1, 2, 3, 1};
    REQUIRE(p.x == Approx(11.0f));
    REQUIRE(p.y == Approx(22.0f));
    REQUIRE(p.z == Approx(33.0f));
    REQUIRE(p.w == Approx(1.0f));

    mat4 a = Math::MakeRotateZ(0.5f);
    mat4 b = Math::MakeTranslation(vec3{1, 2, 3});
    RequireMatApprox(a * b, RefMul(a, b));
    RequireMatApprox(a * Math::Identity(), a);
}

// ---------------------------------------------------------------------------
// Matrix builders / transpose / inverse
// ---------------------------------------------------------------------------

TEST_CASE("MakeRotateZ turns +X toward +Y", "[Core.MathUtils]") {
    mat4 m = Math::MakeRotateZ(kPi * 0.5f);
    vec4 r = m * vec4{1, 0, 0, 0};
    REQUIRE(r.x == Approx(0.0f).margin(1e-6f));
    REQUIRE(r.y == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Transpose swaps rows and columns", "[Core.MathUtils]") {
    mat4 m = Math::MakeTranslation(vec3{1, 2, 3});
    mat4 mt = Math::Transpose(m);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            REQUIRE((mt[row, col]) == Approx((m[col, row])));
        }
    }
}

TEST_CASE("Transpose of a non-square matrix swaps its shape", "[Core.MathUtils]") {
    mat2x3 a{};
    a[0, 0] = 1.0f; a[0, 1] = 2.0f; a[0, 2] = 3.0f;
    a[1, 0] = 4.0f; a[1, 1] = 5.0f; a[1, 2] = 6.0f;

    mat3x2 at = Math::Transpose(a); // Mat<2,3> -> Mat<3,2>
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            REQUIRE((at[col, row]) == (a[row, col]));
        }
    }
}

TEST_CASE("Det for mat2 / mat3 / mat4", "[Core.MathUtils]") {
    mat2 m2{};
    m2[0, 0] = 3.0f; m2[0, 1] = 8.0f;
    m2[1, 0] = 4.0f; m2[1, 1] = 6.0f;
    REQUIRE(Math::Det(m2) == Approx(-14.0f));

    mat3 m3{};
    m3[0, 0] = 6.0f; m3[0, 1] = 1.0f; m3[0, 2] = 1.0f;
    m3[1, 0] = 4.0f; m3[1, 1] = -2.0f; m3[1, 2] = 5.0f;
    m3[2, 0] = 2.0f; m3[2, 1] = 8.0f; m3[2, 2] = 7.0f;
    REQUIRE(Math::Det(m3) == Approx(-306.0f));

    mat4 rotScale = Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
                        * Math::MakeScale(vec3{2, 0.5f, 1.5f});
    REQUIRE(Math::Det(rotScale) == Approx(2.0f * 0.5f * 1.5f).margin(1e-4f));
}

TEST_CASE("Inverse for mat2 and mat3", "[Core.MathUtils]") {
    mat2 m2{};
    m2[0, 0] = 3.0f; m2[0, 1] = 8.0f;
    m2[1, 0] = 4.0f; m2[1, 1] = 6.0f;
    mat2 id2 = m2 * Math::Inverse(m2);
    REQUIRE(id2[0, 0] == Approx(1.0f).margin(1e-5f));
    REQUIRE(id2[0, 1] == Approx(0.0f).margin(1e-5f));
    REQUIRE(id2[1, 0] == Approx(0.0f).margin(1e-5f));
    REQUIRE(id2[1, 1] == Approx(1.0f).margin(1e-5f));

    mat3 m3{};
    m3[0, 0] = 6.0f; m3[0, 1] = 1.0f; m3[0, 2] = 1.0f;
    m3[1, 0] = 4.0f; m3[1, 1] = -2.0f; m3[1, 2] = 5.0f;
    m3[2, 0] = 2.0f; m3[2, 1] = 8.0f; m3[2, 2] = 7.0f;
    mat3 id3 = m3 * Math::Inverse(m3);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            REQUIRE((id3[i, j]) == Approx(i == j ? 1.0f : 0.0f).margin(1e-4f));
}

TEST_CASE("Inverse undoes a TRS transform (mat4)", "[Core.MathUtils]") {
    mat4 trs = Math::MakeTranslation(vec3{5, -3, 2})
                   * Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
                   * Math::MakeScale(vec3{2, 0.5f, 1.5f});
    RequireMatApprox(trs * Math::Inverse(trs), Math::Identity());
    RequireMatApprox(Math::Inverse(trs) * trs, Math::Identity());
}

TEST_CASE("InverseAffine undoes a compact affine transform", "[Core.MathUtils]") {
    // Build an affine3 from a rotation + translation
    mat3 rot{};
    {
        mat4 r4 = Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                rot[i, j] = r4[i, j];
    }
    vec3 trans{5.0f, -3.0f, 2.0f};

    affine3 a{};
    for (int c = 0; c < 3; ++c)
        a[c] = rot[c];
    a[3] = trans;

    affine3 ai = Math::InverseAffine(a);

    // a * ai should give identity rotation + zero translation
    // Multiply manually: R_result = R * Ri, t_result = R * ti + t
    mat3 Ri{};
    for (int c = 0; c < 3; ++c) Ri[c] = ai[c];
    mat3 product = rot * Ri;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            REQUIRE((product[i, j]) == Approx(i == j ? 1.0f : 0.0f).margin(1e-4f));

    // t_result = R * ai.translation + a.translation should be ~0
    vec3 tResult{};
    for (int i = 0; i < 3; ++i) {
        float s = 0.0f;
        for (int k = 0; k < 3; ++k)
            s += rot[i, k] * ai[3][k];
        tResult[i] = s + trans[i];
    }
    REQUIRE(tResult[0] == Approx(0.0f).margin(1e-4f));
    REQUIRE(tResult[1] == Approx(0.0f).margin(1e-4f));
    REQUIRE(tResult[2] == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("MakeLookAt places the eye at the origin of view space", "[Core.MathUtils]") {
    vec3 eye{0, 0, 5};
    mat4 view = Math::MakeLookAt(eye, vec3{0, 0, 0}, vec3{0, 1, 0});
    vec4 origin = view * vec4{eye.x, eye.y, eye.z, 1.0f};
    REQUIRE(origin.x == Approx(0.0f).margin(1e-5f));
    REQUIRE(origin.y == Approx(0.0f).margin(1e-5f));
    REQUIRE(origin.z == Approx(0.0f).margin(1e-5f));
}

TEST_CASE("MakePerspective maps the near plane to Vulkan Z = 0", "[Core.MathUtils]") {
    float nearZ = 0.1f;
    float farZ  = 100.0f;
    mat4 p  = Math::MakePerspective(kPi * 0.25f, 16.0f / 9.0f, nearZ, farZ);
    vec4 clip = p * vec4{0, 0, -nearZ, 1.0f};
    REQUIRE((clip.z / clip.w) == Approx(0.0f).margin(1e-4f));

    vec4 farClip = p * vec4{0, 0, -farZ, 1.0f};
    REQUIRE((farClip.z / farClip.w) == Approx(1.0f).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// Compile-time evaluation (constexpr kernels via `if consteval`)
// ---------------------------------------------------------------------------

TEST_CASE("MathUtils folds at compile time", "[Core.MathUtils]") {
    constexpr vec3 n = Math::Normalize(vec3{0, 3, 4});
    STATIC_REQUIRE(Close(n.y, 0.6f));
    STATIC_REQUIRE(Close(n.z, 0.8f));

    constexpr float d = Math::Dot(vec3{1, 2, 3}, vec3{4, 5, 6});
    STATIC_REQUIRE(Close(d, 32.0f));

    // operator* and MakeRotateZ fold; column 0 of Rz(90) is (0, 1, 0).
    constexpr mat4 m = Math::MakeRotateZ(kPi * 0.5f);
    STATIC_REQUIRE(Close(m[0].x, 0.0f));
    STATIC_REQUIRE(Close(m[0].y, 1.0f));

    // Perspective is a constant expression (exercises constexpr Tan).
    constexpr mat4 proj = Math::MakePerspective(kPi * 0.25f, 1.0f, 0.1f, 100.0f);
    STATIC_REQUIRE(Close(proj[3, 2], -1.0f));
}
