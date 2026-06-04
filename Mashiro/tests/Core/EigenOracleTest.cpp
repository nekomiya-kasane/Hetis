/**
 * @file EigenOracleTest.cpp
 * @brief Comprehensive numeric oracle tests for Mashiro Vec, Mat, Quat, and MathUtils.
 *
 * Uses Eigen as the reference (ground-truth) implementation. Every Mashiro
 * operation is compared element-wise against the equivalent Eigen computation.
 * Conversions and comparison helpers come from the generic EigenBridge.h.
 */
#include "Support/EigenBridge.h"
#include "Support/Meta.h"

#include <Eigen/Dense>

#include <cmath>
#include <numbers>

using namespace Mashiro;
using namespace Mashiro::Testing;
using Catch::Approx;

namespace {
    constexpr float kEps = 1e-5f;
    constexpr float kPi = std::numbers::pi_v<float>;
}

// ===========================================================================
// [Vec.Arith] — Vector arithmetic ops vs Eigen
// ===========================================================================

TEST_CASE("Vec3 arithmetic matches Eigen", AUTO_TAG) {
    vec3 a{1.5f, -2.3f, 4.1f};
    vec3 b{3.7f, 0.8f, -1.2f};
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    RequireCloseEigen(a + b, ea + eb);
    RequireCloseEigen(a - b, ea - eb);
    RequireCloseEigen(a * 2.5f, ea * 2.5f);
    RequireCloseEigen(2.5f * a, 2.5f * ea);
    RequireCloseEigen(-a, -ea);
}

TEST_CASE("Vec4 arithmetic matches Eigen", AUTO_TAG) {
    vec4 a{1.0f, -2.0f, 3.0f, -4.0f};
    vec4 b{5.0f, 6.0f, -7.0f, 8.0f};
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    RequireCloseEigen(a + b, ea + eb);
    RequireCloseEigen(a - b, ea - eb);
    RequireCloseEigen(a * 3.0f, ea * 3.0f);
    RequireCloseEigen(-b, -eb);
}

TEST_CASE("Vec2 arithmetic matches Eigen", AUTO_TAG) {
    vec2 a{3.0f, -1.5f};
    vec2 b{-2.0f, 4.0f};
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    Eigen::Vector2f esum = ea + eb;
    RequireClose((a + b).x, esum.x());
    RequireClose((a + b).y, esum.y());

    Eigen::Vector2f ediff = ea - eb;
    RequireClose((a - b).x, ediff.x());
    RequireClose((a - b).y, ediff.y());
}

TEST_CASE("Hadamard (component-wise) product matches Eigen cwiseProduct", AUTO_TAG) {
    vec3 a{2.0f, 3.0f, 4.0f};
    vec3 b{5.0f, 6.0f, 7.0f};
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    Eigen::Vector3f expected = ea.cwiseProduct(eb);
    RequireCloseEigen(a * b, expected);
}

TEST_CASE("Component-wise division matches Eigen cwiseQuotient", AUTO_TAG) {
    vec3 a{6.0f, 8.0f, 10.0f};
    vec3 b{2.0f, 4.0f, 5.0f};
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    Eigen::Vector3f expected = ea.cwiseQuotient(eb);
    RequireCloseEigen(a / b, expected);
}

// ===========================================================================
// [Vec.Func] — Vector functions vs Eigen
// ===========================================================================

TEST_CASE("Dot product matches Eigen", AUTO_TAG) {
    vec3 a{1.0f, 2.0f, 3.0f};
    vec3 b{4.0f, -5.0f, 6.0f};
    RequireClose(Math::Dot(a, b), Testing::ToEigen(a).dot(Testing::ToEigen(b)));

    vec4 c{1.0f, 2.0f, 3.0f, 4.0f};
    vec4 d{5.0f, 6.0f, 7.0f, 8.0f};
    RequireClose(Math::Dot(c, d), Testing::ToEigen(c).dot(Testing::ToEigen(d)));

    vec2 e{3.0f, 4.0f};
    vec2 f{-1.0f, 2.0f};
    RequireClose(Math::Dot(e, f), Testing::ToEigen(e).dot(Testing::ToEigen(f)));
}

TEST_CASE("Cross product (3D) matches Eigen", AUTO_TAG) {
    vec3 a{1.0f, 0.0f, 0.0f};
    vec3 b{0.0f, 1.0f, 0.0f};
    RequireCloseEigen(Math::Cross(a, b), Testing::ToEigen(a).cross(Testing::ToEigen(b)));

    vec3 c{2.5f, -1.3f, 0.7f};
    vec3 d{-0.4f, 3.1f, -2.2f};
    RequireCloseEigen(Math::Cross(c, d), Testing::ToEigen(c).cross(Testing::ToEigen(d)));
}

TEST_CASE("Cross product (2D) matches Eigen-style computation", AUTO_TAG) {
    vec2 a{3.0f, 4.0f};
    vec2 b{-1.0f, 2.0f};
    // 2D cross = a.x*b.y - a.y*b.x
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);
    float eigenCross = ea.x() * eb.y() - ea.y() * eb.x();
    RequireClose(Math::Cross(a, b), eigenCross);
}

TEST_CASE("Norm1 / Norm2 / NormInf match Eigen norms", AUTO_TAG) {
    vec3 v{3.0f, -4.0f, 12.0f};
    auto ev = Testing::ToEigen(v);

    RequireClose(Math::Norm1(v), ev.lpNorm<1>());
    RequireClose(Math::Norm2(v), ev.norm());
    RequireClose(Math::NormInf(v), ev.lpNorm<Eigen::Infinity>());

    vec4 w{1.0f, -2.0f, 3.0f, -4.0f};
    auto ew = Testing::ToEigen(w);
    RequireClose(Math::Norm1(w), ew.lpNorm<1>());
    RequireClose(Math::Norm2(w), ew.norm());
    RequireClose(Math::NormInf(w), ew.lpNorm<Eigen::Infinity>());
}

TEST_CASE("Normalize matches Eigen .normalized()", AUTO_TAG) {
    vec3 v{3.0f, -4.0f, 0.0f};
    auto ev = Testing::ToEigen(v);
    RequireCloseEigen(Math::Normalize(v), ev.normalized());

    vec4 w{1.0f, 2.0f, 3.0f, 4.0f};
    auto ew = Testing::ToEigen(w);
    RequireCloseEigen(Math::Normalize(w), ew.normalized());
}

TEST_CASE("Distance matches Eigen (b-a).norm()", AUTO_TAG) {
    vec3 a{1.0f, 2.0f, 3.0f};
    vec3 b{4.0f, 6.0f, 3.0f};
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);
    RequireClose(Math::Distance(a, b), (eb - ea).norm());
}

// ===========================================================================
// [Vec.CW] — Component-wise functions vs Eigen
// ===========================================================================

TEST_CASE("Min / Max match Eigen cwiseMin / cwiseMax", AUTO_TAG) {
    vec3 a{1.0f, 5.0f, 3.0f};
    vec3 b{4.0f, 2.0f, 6.0f};
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    RequireCloseEigen(Math::Min(a, b), ea.cwiseMin(eb));
    RequireCloseEigen(Math::Max(a, b), ea.cwiseMax(eb));
}

TEST_CASE("Abs matches Eigen .cwiseAbs()", AUTO_TAG) {
    vec3 v{-1.0f, 2.0f, -3.0f};
    auto ev = Testing::ToEigen(v);
    RequireCloseEigen(Math::Abs(v), ev.cwiseAbs());
}

TEST_CASE("Lerp matches Eigen manual lerp", AUTO_TAG) {
    vec3 a{0.0f, 0.0f, 0.0f};
    vec3 b{10.0f, 20.0f, 30.0f};
    float t = 0.3f;
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);
    Eigen::Vector3f expected = ea + (eb - ea) * t;
    RequireCloseEigen(Math::Lerp(a, b, t), expected);
}

TEST_CASE("Reflect matches Eigen formula: v - 2(v.n)n", AUTO_TAG) {
    vec3 v{1.0f, -1.0f, 0.0f};
    vec3 n{0.0f, 1.0f, 0.0f};
    auto ev = Testing::ToEigen(v);
    auto en = Testing::ToEigen(n);
    Eigen::Vector3f expected = ev - 2.0f * ev.dot(en) * en;
    RequireCloseEigen(Math::Reflect(v, n), expected);
}

TEST_CASE("Clamp matches Eigen cwiseMin/cwiseMax composition", AUTO_TAG) {
    vec3 v{-1.0f, 0.5f, 9.0f};
    vec3 lo{0.0f, 0.0f, 0.0f};
    vec3 hi{1.0f, 1.0f, 1.0f};
    auto ev = Testing::ToEigen(v);
    auto elo = Testing::ToEigen(lo);
    auto ehi = Testing::ToEigen(hi);
    Eigen::Vector3f expected = ev.cwiseMax(elo).cwiseMin(ehi);
    RequireCloseEigen(Math::Clamp(v, lo, hi), expected);
}

// ===========================================================================
// [Mat.Arith] — Matrix arithmetic vs Eigen
// ===========================================================================

TEST_CASE("Mat4 add/sub/scalar matches Eigen", AUTO_TAG) {
    mat4 a = Math::MakeTranslation(vec3{1, 2, 3});
    mat4 b = Math::MakeScale(vec3{2, 3, 4});
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    RequireCloseEigen(a + b, ea + eb);
    RequireCloseEigen(a - b, ea - eb);
    RequireCloseEigen(a * 2.5f, ea * 2.5f);
    RequireCloseEigen(-a, -ea);
}

TEST_CASE("Mat4 * Mat4 matches Eigen", AUTO_TAG) {
    mat4 a = Math::MakeRotateZ(0.7f);
    mat4 b = Math::MakeTranslation(vec3{5, -3, 2});
    auto ea = Testing::ToEigen(a);
    auto eb = Testing::ToEigen(b);

    RequireCloseEigen(a * b, ea * eb);
    RequireCloseEigen(b * a, eb * ea);
}

TEST_CASE("Mat4 * Vec4 matches Eigen", AUTO_TAG) {
    mat4 m = Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
             * Math::MakeTranslation(vec3{10, 20, 30});
    vec4 v{1, 2, 3, 1};
    auto em = Testing::ToEigen(m);
    auto ev = Testing::ToEigen(v);
    RequireCloseEigen(m * v, em * ev);
}

TEST_CASE("Mat3 * Mat3 matches Eigen", AUTO_TAG) {
    mat3 a{};
    a[0, 0] = 1; a[0, 1] = 2; a[0, 2] = 3;
    a[1, 0] = 4; a[1, 1] = 5; a[1, 2] = 6;
    a[2, 0] = 7; a[2, 1] = 8; a[2, 2] = 9;

    mat3 b{};
    b[0, 0] = 9; b[0, 1] = 8; b[0, 2] = 7;
    b[1, 0] = 6; b[1, 1] = 5; b[1, 2] = 4;
    b[2, 0] = 3; b[2, 1] = 2; b[2, 2] = 1;

    RequireCloseEigen(a * b, Testing::ToEigen(a) * Testing::ToEigen(b));
}

TEST_CASE("Mat2 * Vec2 matches Eigen", AUTO_TAG) {
    mat2 m{};
    m[0, 0] = 1; m[0, 1] = 2;
    m[1, 0] = 3; m[1, 1] = 4;
    vec2 v{5, 6};

    auto em = Testing::ToEigen(m);
    auto ev = Testing::ToEigen(v);
    Eigen::Vector2f expected = em * ev;
    vec2 result = m * v;
    RequireClose(result.x, expected.x());
    RequireClose(result.y, expected.y());
}

// ===========================================================================
// [Mat.Func] — Transpose / Identity / Det / Inverse vs Eigen
// ===========================================================================

TEST_CASE("Transpose matches Eigen .transpose()", AUTO_TAG) {
    mat4 m = Math::MakeRotateAxis(vec3{1, 1, 0}, 0.5f);
    auto em = Testing::ToEigen(m);
    RequireCloseEigen(Math::Transpose(m), em.transpose());

    mat3 m3{};
    m3[0, 0] = 1; m3[0, 1] = 2; m3[0, 2] = 3;
    m3[1, 0] = 4; m3[1, 1] = 5; m3[1, 2] = 6;
    m3[2, 0] = 7; m3[2, 1] = 8; m3[2, 2] = 9;
    RequireCloseEigen(Math::Transpose(m3), Testing::ToEigen(m3).transpose());
}

TEST_CASE("Det matches Eigen .determinant()", AUTO_TAG) {
    // mat2
    mat2 m2{};
    m2[0, 0] = 3; m2[0, 1] = 8;
    m2[1, 0] = 4; m2[1, 1] = 6;
    RequireClose(Math::Det(m2), Testing::ToEigen(m2).determinant());

    // mat3
    mat3 m3{};
    m3[0, 0] = 6; m3[0, 1] = 1; m3[0, 2] = 1;
    m3[1, 0] = 4; m3[1, 1] = -2; m3[1, 2] = 5;
    m3[2, 0] = 2; m3[2, 1] = 8; m3[2, 2] = 7;
    RequireClose(Math::Det(m3), Testing::ToEigen(m3).determinant());

    // mat4
    mat4 m4 = Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
              * Math::MakeScale(vec3{2, 0.5f, 1.5f});
    RequireClose(Math::Det(m4), Testing::ToEigen(m4).determinant(), 1e-3f);
}

TEST_CASE("Inverse(mat2) matches Eigen .inverse()", AUTO_TAG) {
    mat2 m{};
    m[0, 0] = 3; m[0, 1] = 8;
    m[1, 0] = 4; m[1, 1] = 6;
    RequireCloseEigen(Math::Inverse(m), Testing::ToEigen(m).inverse());
}

TEST_CASE("Inverse(mat3) matches Eigen .inverse()", AUTO_TAG) {
    mat3 m{};
    m[0, 0] = 6; m[0, 1] = 1; m[0, 2] = 1;
    m[1, 0] = 4; m[1, 1] = -2; m[1, 2] = 5;
    m[2, 0] = 2; m[2, 1] = 8; m[2, 2] = 7;
    RequireCloseEigen(Math::Inverse(m), Testing::ToEigen(m).inverse(), 1e-4f);
}

TEST_CASE("Inverse(mat4) matches Eigen .inverse()", AUTO_TAG) {
    mat4 m = Math::MakeTranslation(vec3{5, -3, 2})
             * Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
             * Math::MakeScale(vec3{2, 0.5f, 1.5f});
    RequireCloseEigen(Math::Inverse(m), Testing::ToEigen(m).inverse(), 1e-4f);
}

TEST_CASE("Identity matches Eigen Identity", AUTO_TAG) {
    RequireCloseEigen(Math::Identity(), Eigen::Matrix4f::Identity().eval());
    RequireCloseEigen(Math::Identity<mat3>(), Eigen::Matrix3f::Identity().eval());
    RequireCloseEigen(Math::Identity<mat2>(), Eigen::Matrix2f::Identity().eval());
}

// ===========================================================================
// [Xform3D] — 3D Transform builders vs Eigen
// ===========================================================================

TEST_CASE("MakeTranslation matches Eigen Translation3f", AUTO_TAG) {
    vec3 t{10, -20, 30};
    mat4 m = Math::MakeTranslation(t);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.translate(Testing::ToEigen(t));
    RequireCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeScale matches Eigen Scaling", AUTO_TAG) {
    vec3 s{2, 3, 4};
    mat4 m = Math::MakeScale(s);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.scale(Testing::ToEigen(s));
    RequireCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateX matches Eigen AngleAxisf(X)", AUTO_TAG) {
    float angle = 1.2f;
    mat4 m = Math::MakeRotateX(angle);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitX()));
    RequireCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateY matches Eigen AngleAxisf(Y)", AUTO_TAG) {
    float angle = -0.7f;
    mat4 m = Math::MakeRotateY(angle);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitY()));
    RequireCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateZ matches Eigen AngleAxisf(Z)", AUTO_TAG) {
    float angle = 2.1f;
    mat4 m = Math::MakeRotateZ(angle);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitZ()));
    RequireCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateAxis matches Eigen AngleAxisf(arbitrary)", AUTO_TAG) {
    vec3 axis{0.3f, 0.8f, -0.5f};
    float angle = 1.1f;
    mat4 m = Math::MakeRotateAxis(axis, angle);

    Eigen::Vector3f eAxis = Testing::ToEigen(axis).normalized();
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, eAxis));
    RequireCloseEigen(m, ea.matrix(), 1e-4f);
}

TEST_CASE("Rotation matrices have det = 1", AUTO_TAG) {
    RequireClose(Math::Det(Math::MakeRotateX(0.5f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateY(1.2f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateZ(-0.3f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateAxis(vec3{1, 1, 1}, 2.0f)), 1.0f);
}

TEST_CASE("MakeLookAt matches Eigen LookAt construction", AUTO_TAG) {
    vec3 eye{0, 0, 5};
    vec3 target{0, 0, 0};
    vec3 up{0, 1, 0};

    mat4 view = Math::MakeLookAt(eye, target, up);

    // Manually construct the same view matrix using Eigen
    Eigen::Vector3f eEye = Testing::ToEigen(eye);
    Eigen::Vector3f eTarget = Testing::ToEigen(target);
    Eigen::Vector3f eUp = Testing::ToEigen(up);

    Eigen::Vector3f f = (eTarget - eEye).normalized();
    Eigen::Vector3f s = f.cross(eUp).normalized();
    Eigen::Vector3f u = s.cross(f);

    Eigen::Matrix4f expected = Eigen::Matrix4f::Identity();
    expected(0, 0) = s.x();   expected(0, 1) = s.y();   expected(0, 2) = s.z();
    expected(1, 0) = u.x();   expected(1, 1) = u.y();   expected(1, 2) = u.z();
    expected(2, 0) = -f.x();  expected(2, 1) = -f.y();  expected(2, 2) = -f.z();
    expected(0, 3) = -s.dot(eEye);
    expected(1, 3) = -u.dot(eEye);
    expected(2, 3) = f.dot(eEye);

    RequireCloseEigen(view, expected);
}

// ===========================================================================
// [Affine3D] — 3D Affine transforms (Mat<float,3,4>) vs 4×4 upper rows
// ===========================================================================

TEST_CASE("IdentityAffine is upper 3 rows of Identity()", AUTO_TAG) {
    affine3 a = Math::IdentityAffine();
    mat4 full = Math::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeTranslationAffine is upper 3 rows of MakeTranslation", AUTO_TAG) {
    vec3 t{5, -3, 7};
    affine3 a = Math::MakeTranslationAffine(t);
    mat4 full = Math::MakeTranslation(t);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeScaleAffine is upper 3 rows of MakeScale", AUTO_TAG) {
    vec3 s{2, 0.5f, 3};
    affine3 a = Math::MakeScaleAffine(s);
    mat4 full = Math::MakeScale(s);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateXAffine is upper 3 rows of MakeRotateX", AUTO_TAG) {
    float angle = 0.9f;
    affine3 a = Math::MakeRotateXAffine(angle);
    mat4 full = Math::MakeRotateX(angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateYAffine is upper 3 rows of MakeRotateY", AUTO_TAG) {
    float angle = -1.3f;
    affine3 a = Math::MakeRotateYAffine(angle);
    mat4 full = Math::MakeRotateY(angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateZAffine is upper 3 rows of MakeRotateZ", AUTO_TAG) {
    float angle = 2.0f;
    affine3 a = Math::MakeRotateZAffine(angle);
    mat4 full = Math::MakeRotateZ(angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateAxisAffine is upper 3 rows of MakeRotateAxis", AUTO_TAG) {
    vec3 axis{0.3f, 0.8f, -0.5f};
    float angle = 1.1f;
    affine3 a = Math::MakeRotateAxisAffine(axis, angle);
    mat4 full = Math::MakeRotateAxis(axis, angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeLookAtAffine is upper 3 rows of MakeLookAt", AUTO_TAG) {
    vec3 eye{3, 4, 5};
    vec3 target{0, 0, 0};
    vec3 up{0, 1, 0};
    affine3 a = Math::MakeLookAtAffine(eye, target, up);
    mat4 full = Math::MakeLookAt(eye, target, up);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("InverseAffine matches Eigen Affine3f .inverse()", AUTO_TAG) {
    // Build a rotation+translation affine
    float angle = 1.1f;
    vec3 axis{0.3f, 0.8f, -0.5f};
    vec3 t{5, -3, 2};

    affine3 a = Math::MakeRotateAxisAffine(axis, angle);
    a[0, 3] = t.x;
    a[1, 3] = t.y;
    a[2, 3] = t.z;

    affine3 ai = Math::InverseAffine(a);

    // Build the same in Eigen
    Eigen::Vector3f eAxis = Testing::ToEigen(axis).normalized();
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, eAxis));
    ea.translation() = Testing::ToEigen(t);

    Eigen::Affine3f eai = ea.inverse();
    // Compare upper 3×4
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(ai[r, c], eai.matrix()(r, c), 1e-4f);
}

// ===========================================================================
// [Xform2D] — 2D transform builders vs Eigen
// ===========================================================================

TEST_CASE("MakeTranslation2D matches Eigen Translation2f", AUTO_TAG) {
    vec2 t{3, -7};
    mat3 m = Math::MakeTranslation2D(t);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.translate(Testing::ToEigen(t));
    // Eigen Affine2f .matrix() is 3×3
    Eigen::Matrix3f expected = ea.matrix();
    RequireCloseEigen(m, expected);
}

TEST_CASE("MakeScale2D matches Eigen Scaling2D", AUTO_TAG) {
    vec2 s{2, 3};
    mat3 m = Math::MakeScale2D(s);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.scale(Testing::ToEigen(s));
    RequireCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotate2D matches Eigen Rotation2Df", AUTO_TAG) {
    float angle = 1.5f;
    mat3 m = Math::MakeRotate2D(angle);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.rotate(Eigen::Rotation2Df(angle));
    RequireCloseEigen(m, ea.matrix());
}

// ===========================================================================
// [Affine2D] — 2D Affine transforms (Mat<float,2,3>) vs 3×3 upper rows
// ===========================================================================

TEST_CASE("IdentityAffine2D is upper 2 rows of Identity<mat3>()", AUTO_TAG) {
    affine2 a = Math::IdentityAffine2D();
    mat3 full = Math::Identity<mat3>();
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeTranslation2DAffine is upper 2 rows of MakeTranslation2D", AUTO_TAG) {
    vec2 t{3, -7};
    affine2 a = Math::MakeTranslation2DAffine(t);
    mat3 full = Math::MakeTranslation2D(t);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeScale2DAffine is upper 2 rows of MakeScale2D", AUTO_TAG) {
    vec2 s{2, 0.5f};
    affine2 a = Math::MakeScale2DAffine(s);
    mat3 full = Math::MakeScale2D(s);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotate2DAffine is upper 2 rows of MakeRotate2D", AUTO_TAG) {
    float angle = 0.8f;
    affine2 a = Math::MakeRotate2DAffine(angle);
    mat3 full = Math::MakeRotate2D(angle);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("InverseAffine2D matches Eigen Affine2f .inverse()", AUTO_TAG) {
    float angle = 0.8f;
    vec2 t{3, -7};
    affine2 a = Math::MakeRotate2DAffine(angle);
    a[0, 2] = t.x;
    a[1, 2] = t.y;

    affine2 ai = Math::InverseAffine2D(a);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.rotate(Eigen::Rotation2Df(angle));
    ea.translation() = Testing::ToEigen(t);
    Eigen::Affine2f eai = ea.inverse();

    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(ai[r, c], eai.matrix()(r, c), 1e-4f);
}

// ===========================================================================
// [Proj] — Projection matrices: verify near/far plane mapping
// ===========================================================================

TEST_CASE("MakePerspective: near → Z=0, far → Z=1 (Vulkan)", AUTO_TAG) {
    float nearZ = 0.1f;
    float farZ = 100.0f;
    mat4 p = Math::MakePerspective(kPi * 0.25f, 16.0f / 9.0f, nearZ, farZ);

    // Point on near plane (looking down -Z): (0, 0, -near, 1)
    vec4 nearPt = p * vec4{0, 0, -nearZ, 1};
    RequireClose(nearPt.z / nearPt.w, 0.0f, 1e-4f);

    // Point on far plane
    vec4 farPt = p * vec4{0, 0, -farZ, 1};
    RequireClose(farPt.z / farPt.w, 1.0f, 1e-4f);
}

TEST_CASE("MakePerspectiveYFlipped has positive Y (Y-up)", AUTO_TAG) {
    mat4 p = Math::MakePerspectiveYFlipped(kPi * 0.25f, 1.0f, 0.1f, 100.0f);
    // A point above center should map to positive clip Y
    vec4 above = p * vec4{0, 1, -1, 1};
    REQUIRE(above.y / above.w > 0.0f);
}

TEST_CASE("MakeOrtho: near → Z=0, far → Z=1 (Vulkan)", AUTO_TAG) {
    float l = -1, r = 1, b = -1, t = 1, n = 0.1f, f = 100.0f;
    mat4 o = Math::MakeOrtho(l, r, b, t, n, f);

    // Near plane center
    vec4 nearPt = o * vec4{0, 0, -n, 1};
    RequireClose(nearPt.z / nearPt.w, 0.0f, 1e-4f);

    // Far plane center
    vec4 farPt = o * vec4{0, 0, -f, 1};
    RequireClose(farPt.z / farPt.w, 1.0f, 1e-4f);
}

TEST_CASE("MakeOrthoYFlipped has positive Y for point above center", AUTO_TAG) {
    mat4 o = Math::MakeOrthoYFlipped(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f);
    vec4 above = o * vec4{0, 0.5f, -1, 1};
    REQUIRE(above.y / above.w > 0.0f);
}

// ===========================================================================
// [Constexpr] — compile-time evaluation (constexpr smoke tests)
// ===========================================================================

TEST_CASE("Constexpr vector ops fold correctly", AUTO_TAG) {
    constexpr vec3 a{1, 2, 3};
    constexpr vec3 b{4, 5, 6};
    constexpr vec3 sum = a + b;
    STATIC_REQUIRE(sum.x == 5.0f);
    STATIC_REQUIRE(sum.y == 7.0f);
    STATIC_REQUIRE(sum.z == 9.0f);

    constexpr float dot = Math::Dot(a, b);
    STATIC_REQUIRE(dot == 32.0f);

    constexpr vec3 cross = Math::Cross(vec3{1, 0, 0}, vec3{0, 1, 0});
    STATIC_REQUIRE(cross.z == 1.0f);
}

TEST_CASE("Constexpr matrix ops fold correctly", AUTO_TAG) {
    constexpr mat4 id = Math::Identity();
    STATIC_REQUIRE(id[0, 0] == 1.0f);
    STATIC_REQUIRE(id[1, 1] == 1.0f);
    STATIC_REQUIRE(id[0, 1] == 0.0f);

    constexpr mat4 t = Math::MakeTranslation(vec3{1, 2, 3});
    STATIC_REQUIRE(t[0, 3] == 1.0f);
    STATIC_REQUIRE(t[1, 3] == 2.0f);
    STATIC_REQUIRE(t[2, 3] == 3.0f);
    STATIC_REQUIRE(t[3, 3] == 1.0f);
}

TEST_CASE("Constexpr Det folds at compile time", AUTO_TAG) {
    constexpr mat2 m2 = [] {
        mat2 m{};
        m[0, 0] = 3; m[0, 1] = 8;
        m[1, 0] = 4; m[1, 1] = 6;
        return m;
    }();
    STATIC_REQUIRE(Math::Det(m2) == -14.0f);
}

TEST_CASE("Constexpr 2D rotation folds at compile time", AUTO_TAG) {
    constexpr mat3 r = Math::MakeRotate2D(0.0f);
    STATIC_REQUIRE(r[0, 0] == 1.0f);
    STATIC_REQUIRE(r[1, 1] == 1.0f);
    STATIC_REQUIRE(r[0, 1] == 0.0f);
    STATIC_REQUIRE(r[1, 0] == 0.0f);
}

// ===========================================================================
// [Stress] — Randomized stress tests with multiple inputs
// ===========================================================================

TEST_CASE("Inverse is left and right inverse (multiple random TRS)", AUTO_TAG) {
    struct TestData { vec3 t; vec3 axis; float angle; vec3 s; };
    TestData cases[] = {
        {{1, 2, 3},    {1, 0, 0}, 0.5f,  {1, 1, 1}},
        {{-5, 7, 0.1f},{0, 1, 0}, 1.2f,  {2, 0.5f, 3}},
        {{0, 0, 0},    {0, 0, 1}, -0.3f, {0.1f, 10, 0.5f}},
        {{100, -50, 25},{1, 1, 1}, 3.0f,  {0.5f, 0.5f, 0.5f}},
    };

    for (auto& [t, axis, angle, s] : cases) {
        mat4 m = Math::MakeTranslation(t)
                 * Math::MakeRotateAxis(axis, angle)
                 * Math::MakeScale(s);
        mat4 mi = Math::Inverse(m);
        RequireCloseEigen(m * mi, Eigen::Matrix4f::Identity().eval(), 1e-3f);
        RequireCloseEigen(mi * m, Eigen::Matrix4f::Identity().eval(), 1e-3f);
    }
}

TEST_CASE("InverseAffine matches full Inverse upper rows", AUTO_TAG) {
    struct TestData { vec3 axis; float angle; vec3 t; };
    TestData cases[] = {
        {{1, 0, 0}, 0.5f,  {1, 2, 3}},
        {{0, 1, 0}, -1.2f, {-5, 7, 0}},
        {{1, 1, 1}, 2.5f,  {0, 0, 100}},
    };

    for (auto& [axis, angle, t] : cases) {
        affine3 a = Math::MakeRotateAxisAffine(axis, angle);
        a[0, 3] = t.x; a[1, 3] = t.y; a[2, 3] = t.z;
        affine3 ai = Math::InverseAffine(a);

        // Reconstruct full 4×4 and invert
        mat4 full = Math::Identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                full[r, c] = a[r, c];
        mat4 fullInv = Math::Inverse(full);

        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                RequireClose(ai[r, c], fullInv[r, c], 1e-4f);
    }
}
