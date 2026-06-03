/**
 * @file EigenOracleTest.cpp
 * @brief Comprehensive numeric oracle tests for Mashiro Vec, Mat, and MathUtils.
 *
 * Uses Eigen as the reference (ground-truth) implementation. Every Mashiro
 * operation is compared element-wise against the equivalent Eigen computation.
 */
#include "Mashiro/Math/MathUtils.h"
#include "Mashiro/Core/Quanterion.h"
#include "Mashiro/Core/Types.h"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/Dense>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace Mashiro;
using Catch::Approx;

// ===========================================================================
// Conversion bridge: Mashiro ↔ Eigen
// ===========================================================================

namespace Bridge {

    // --- Vec → Eigen ---
    inline Eigen::Vector2f ToEigen(vec2 v) { return {v.x, v.y}; }
    inline Eigen::Vector3f ToEigen(vec3 v) { return {v.x, v.y, v.z}; }
    inline Eigen::Vector4f ToEigen(vec4 v) { return {v.x, v.y, v.z, v.w}; }
    inline Eigen::Vector2d ToEigen(Vec<double, 2> v) { return {v.x, v.y}; }
    inline Eigen::Vector3d ToEigen(Vec<double, 3> v) { return {v.x, v.y, v.z}; }

    // --- Eigen → Vec ---
    inline vec2 FromEigen(const Eigen::Vector2f& v) { return {v.x(), v.y()}; }
    inline vec3 FromEigen(const Eigen::Vector3f& v) { return {v.x(), v.y(), v.z()}; }
    inline vec4 FromEigen(const Eigen::Vector4f& v) { return {v.x(), v.y(), v.z(), v.w()}; }

    // --- Mat → Eigen (column-major both sides) ---
    inline Eigen::Matrix2f ToEigen(const mat2& m) {
        Eigen::Matrix2f e;
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c)
                e(r, c) = m[r, c];
        return e;
    }
    inline Eigen::Matrix3f ToEigen(const mat3& m) {
        Eigen::Matrix3f e;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                e(r, c) = m[r, c];
        return e;
    }
    inline Eigen::Matrix4f ToEigen(const mat4& m) {
        Eigen::Matrix4f e;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                e(r, c) = m[r, c];
        return e;
    }

    // Non-square: Mat<float, 2, 3> → Eigen::Matrix<float, 2, 3>
    inline Eigen::Matrix<float, 2, 3> ToEigen(const Mat<float, 2, 3>& m) {
        Eigen::Matrix<float, 2, 3> e;
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                e(r, c) = m[r, c];
        return e;
    }
    // Non-square: Mat<float, 3, 4> → Eigen::Matrix<float, 3, 4>
    inline Eigen::Matrix<float, 3, 4> ToEigen(const Mat<float, 3, 4>& m) {
        Eigen::Matrix<float, 3, 4> e;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                e(r, c) = m[r, c];
        return e;
    }

    // --- Eigen → Mat ---
    inline mat2 FromEigenMat2(const Eigen::Matrix2f& e) {
        mat2 m{};
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c)
                m[r, c] = e(r, c);
        return m;
    }
    inline mat3 FromEigenMat3(const Eigen::Matrix3f& e) {
        mat3 m{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m[r, c] = e(r, c);
        return m;
    }
    inline mat4 FromEigenMat4(const Eigen::Matrix4f& e) {
        mat4 m{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m[r, c] = e(r, c);
        return m;
    }

} // namespace Bridge

// ===========================================================================
// Comparison helpers
// ===========================================================================

namespace {

    constexpr float kEps = 1e-5f;
    constexpr float kPi = std::numbers::pi_v<float>;

    void RequireClose(float a, float b, float margin = kEps) {
        REQUIRE(a == Approx(b).margin(margin));
    }

    void RequireVec2Close(vec2 a, vec2 b, float margin = kEps) {
        RequireClose(a.x, b.x, margin);
        RequireClose(a.y, b.y, margin);
    }

    void RequireVec3Close(vec3 a, vec3 b, float margin = kEps) {
        RequireClose(a.x, b.x, margin);
        RequireClose(a.y, b.y, margin);
        RequireClose(a.z, b.z, margin);
    }

    void RequireVec4Close(vec4 a, vec4 b, float margin = kEps) {
        RequireClose(a.x, b.x, margin);
        RequireClose(a.y, b.y, margin);
        RequireClose(a.z, b.z, margin);
        RequireClose(a.w, b.w, margin);
    }

    void RequireVec3CloseEigen(vec3 v, const Eigen::Vector3f& e, float margin = kEps) {
        RequireClose(v.x, e.x(), margin);
        RequireClose(v.y, e.y(), margin);
        RequireClose(v.z, e.z(), margin);
    }

    void RequireVec4CloseEigen(vec4 v, const Eigen::Vector4f& e, float margin = kEps) {
        RequireClose(v.x, e.x(), margin);
        RequireClose(v.y, e.y(), margin);
        RequireClose(v.z, e.z(), margin);
        RequireClose(v.w, e.w(), margin);
    }

    template <int R, int C>
    void RequireMatClose(const Mat<float, R, C>& a, const Mat<float, R, C>& b, float margin = kEps) {
        for (int r = 0; r < R; ++r)
            for (int c = 0; c < C; ++c)
                REQUIRE((a[r, c]) == Approx((b[r, c])).margin(margin));
    }

    template <int R, int C, typename EigenExpr>
    void RequireMatCloseEigen(const Mat<float, R, C>& m,
                              const EigenExpr& expr,
                              float margin = kEps) {
        Eigen::Matrix<float, R, C> e = expr;
        for (int r = 0; r < R; ++r)
            for (int c = 0; c < C; ++c)
                REQUIRE((m[r, c]) == Approx(e(r, c)).margin(margin));
    }

} // namespace

// ===========================================================================
// [Vec.Arith] — Vector arithmetic ops vs Eigen
// ===========================================================================

TEST_CASE("Vec3 arithmetic matches Eigen", "[Vec.Arith][Eigen]") {
    vec3 a{1.5f, -2.3f, 4.1f};
    vec3 b{3.7f, 0.8f, -1.2f};
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    RequireVec3CloseEigen(a + b, ea + eb);
    RequireVec3CloseEigen(a - b, ea - eb);
    RequireVec3CloseEigen(a * 2.5f, ea * 2.5f);
    RequireVec3CloseEigen(2.5f * a, 2.5f * ea);
    RequireVec3CloseEigen(-a, -ea);
}

TEST_CASE("Vec4 arithmetic matches Eigen", "[Vec.Arith][Eigen]") {
    vec4 a{1.0f, -2.0f, 3.0f, -4.0f};
    vec4 b{5.0f, 6.0f, -7.0f, 8.0f};
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    RequireVec4CloseEigen(a + b, ea + eb);
    RequireVec4CloseEigen(a - b, ea - eb);
    RequireVec4CloseEigen(a * 3.0f, ea * 3.0f);
    RequireVec4CloseEigen(-b, -eb);
}

TEST_CASE("Vec2 arithmetic matches Eigen", "[Vec.Arith][Eigen]") {
    vec2 a{3.0f, -1.5f};
    vec2 b{-2.0f, 4.0f};
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    Eigen::Vector2f esum = ea + eb;
    RequireClose((a + b).x, esum.x());
    RequireClose((a + b).y, esum.y());

    Eigen::Vector2f ediff = ea - eb;
    RequireClose((a - b).x, ediff.x());
    RequireClose((a - b).y, ediff.y());
}

TEST_CASE("Hadamard (component-wise) product matches Eigen cwiseProduct", "[Vec.Arith][Eigen]") {
    vec3 a{2.0f, 3.0f, 4.0f};
    vec3 b{5.0f, 6.0f, 7.0f};
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    Eigen::Vector3f expected = ea.cwiseProduct(eb);
    RequireVec3CloseEigen(a * b, expected);
}

TEST_CASE("Component-wise division matches Eigen cwiseQuotient", "[Vec.Arith][Eigen]") {
    vec3 a{6.0f, 8.0f, 10.0f};
    vec3 b{2.0f, 4.0f, 5.0f};
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    Eigen::Vector3f expected = ea.cwiseQuotient(eb);
    RequireVec3CloseEigen(a / b, expected);
}

// ===========================================================================
// [Vec.Func] — Vector functions vs Eigen
// ===========================================================================

TEST_CASE("Dot product matches Eigen", "[Vec.Func][Eigen]") {
    vec3 a{1.0f, 2.0f, 3.0f};
    vec3 b{4.0f, -5.0f, 6.0f};
    RequireClose(Math::Dot(a, b), Bridge::ToEigen(a).dot(Bridge::ToEigen(b)));

    vec4 c{1.0f, 2.0f, 3.0f, 4.0f};
    vec4 d{5.0f, 6.0f, 7.0f, 8.0f};
    RequireClose(Math::Dot(c, d), Bridge::ToEigen(c).dot(Bridge::ToEigen(d)));

    vec2 e{3.0f, 4.0f};
    vec2 f{-1.0f, 2.0f};
    RequireClose(Math::Dot(e, f), Bridge::ToEigen(e).dot(Bridge::ToEigen(f)));
}

TEST_CASE("Cross product (3D) matches Eigen", "[Vec.Func][Eigen]") {
    vec3 a{1.0f, 0.0f, 0.0f};
    vec3 b{0.0f, 1.0f, 0.0f};
    RequireVec3CloseEigen(Math::Cross(a, b), Bridge::ToEigen(a).cross(Bridge::ToEigen(b)));

    vec3 c{2.5f, -1.3f, 0.7f};
    vec3 d{-0.4f, 3.1f, -2.2f};
    RequireVec3CloseEigen(Math::Cross(c, d), Bridge::ToEigen(c).cross(Bridge::ToEigen(d)));
}

TEST_CASE("Cross product (2D) matches Eigen-style computation", "[Vec.Func][Eigen]") {
    vec2 a{3.0f, 4.0f};
    vec2 b{-1.0f, 2.0f};
    // 2D cross = a.x*b.y - a.y*b.x
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);
    float eigenCross = ea.x() * eb.y() - ea.y() * eb.x();
    RequireClose(Math::Cross(a, b), eigenCross);
}

TEST_CASE("Norm1 / Norm2 / NormInf match Eigen norms", "[Vec.Func][Eigen]") {
    vec3 v{3.0f, -4.0f, 12.0f};
    auto ev = Bridge::ToEigen(v);

    RequireClose(Math::Norm1(v), ev.lpNorm<1>());
    RequireClose(Math::Norm2(v), ev.norm());
    RequireClose(Math::NormInf(v), ev.lpNorm<Eigen::Infinity>());

    vec4 w{1.0f, -2.0f, 3.0f, -4.0f};
    auto ew = Bridge::ToEigen(w);
    RequireClose(Math::Norm1(w), ew.lpNorm<1>());
    RequireClose(Math::Norm2(w), ew.norm());
    RequireClose(Math::NormInf(w), ew.lpNorm<Eigen::Infinity>());
}

TEST_CASE("Normalize matches Eigen .normalized()", "[Vec.Func][Eigen]") {
    vec3 v{3.0f, -4.0f, 0.0f};
    auto ev = Bridge::ToEigen(v);
    RequireVec3CloseEigen(Math::Normalize(v), ev.normalized());

    vec4 w{1.0f, 2.0f, 3.0f, 4.0f};
    auto ew = Bridge::ToEigen(w);
    RequireVec4CloseEigen(Math::Normalize(w), ew.normalized());
}

TEST_CASE("Distance matches Eigen (b-a).norm()", "[Vec.Func][Eigen]") {
    vec3 a{1.0f, 2.0f, 3.0f};
    vec3 b{4.0f, 6.0f, 3.0f};
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);
    RequireClose(Math::Distance(a, b), (eb - ea).norm());
}

// ===========================================================================
// [Vec.CW] — Component-wise functions vs Eigen
// ===========================================================================

TEST_CASE("Min / Max match Eigen cwiseMin / cwiseMax", "[Vec.CW][Eigen]") {
    vec3 a{1.0f, 5.0f, 3.0f};
    vec3 b{4.0f, 2.0f, 6.0f};
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    RequireVec3CloseEigen(Math::Min(a, b), ea.cwiseMin(eb));
    RequireVec3CloseEigen(Math::Max(a, b), ea.cwiseMax(eb));
}

TEST_CASE("Abs matches Eigen .cwiseAbs()", "[Vec.CW][Eigen]") {
    vec3 v{-1.0f, 2.0f, -3.0f};
    auto ev = Bridge::ToEigen(v);
    RequireVec3CloseEigen(Math::Abs(v), ev.cwiseAbs());
}

TEST_CASE("Lerp matches Eigen manual lerp", "[Vec.CW][Eigen]") {
    vec3 a{0.0f, 0.0f, 0.0f};
    vec3 b{10.0f, 20.0f, 30.0f};
    float t = 0.3f;
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);
    Eigen::Vector3f expected = ea + (eb - ea) * t;
    RequireVec3CloseEigen(Math::Lerp(a, b, t), expected);
}

TEST_CASE("Reflect matches Eigen formula: v - 2(v.n)n", "[Vec.CW][Eigen]") {
    vec3 v{1.0f, -1.0f, 0.0f};
    vec3 n{0.0f, 1.0f, 0.0f};
    auto ev = Bridge::ToEigen(v);
    auto en = Bridge::ToEigen(n);
    Eigen::Vector3f expected = ev - 2.0f * ev.dot(en) * en;
    RequireVec3CloseEigen(Math::Reflect(v, n), expected);
}

TEST_CASE("Clamp matches Eigen cwiseMin/cwiseMax composition", "[Vec.CW][Eigen]") {
    vec3 v{-1.0f, 0.5f, 9.0f};
    vec3 lo{0.0f, 0.0f, 0.0f};
    vec3 hi{1.0f, 1.0f, 1.0f};
    auto ev = Bridge::ToEigen(v);
    auto elo = Bridge::ToEigen(lo);
    auto ehi = Bridge::ToEigen(hi);
    Eigen::Vector3f expected = ev.cwiseMax(elo).cwiseMin(ehi);
    RequireVec3CloseEigen(Math::Clamp(v, lo, hi), expected);
}

// ===========================================================================
// [Mat.Arith] — Matrix arithmetic vs Eigen
// ===========================================================================

TEST_CASE("Mat4 add/sub/scalar matches Eigen", "[Mat.Arith][Eigen]") {
    mat4 a = Math::MakeTranslation(vec3{1, 2, 3});
    mat4 b = Math::MakeScale(vec3{2, 3, 4});
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    RequireMatCloseEigen(a + b, ea + eb);
    RequireMatCloseEigen(a - b, ea - eb);
    RequireMatCloseEigen(a * 2.5f, ea * 2.5f);
    RequireMatCloseEigen(-a, -ea);
}

TEST_CASE("Mat4 * Mat4 matches Eigen", "[Mat.Arith][Eigen]") {
    mat4 a = Math::MakeRotateZ(0.7f);
    mat4 b = Math::MakeTranslation(vec3{5, -3, 2});
    auto ea = Bridge::ToEigen(a);
    auto eb = Bridge::ToEigen(b);

    RequireMatCloseEigen(a * b, ea * eb);
    RequireMatCloseEigen(b * a, eb * ea);
}

TEST_CASE("Mat4 * Vec4 matches Eigen", "[Mat.Arith][Eigen]") {
    mat4 m = Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
             * Math::MakeTranslation(vec3{10, 20, 30});
    vec4 v{1, 2, 3, 1};
    auto em = Bridge::ToEigen(m);
    auto ev = Bridge::ToEigen(v);
    RequireVec4CloseEigen(m * v, em * ev);
}

TEST_CASE("Mat3 * Mat3 matches Eigen", "[Mat.Arith][Eigen]") {
    mat3 a{};
    a[0, 0] = 1; a[0, 1] = 2; a[0, 2] = 3;
    a[1, 0] = 4; a[1, 1] = 5; a[1, 2] = 6;
    a[2, 0] = 7; a[2, 1] = 8; a[2, 2] = 9;

    mat3 b{};
    b[0, 0] = 9; b[0, 1] = 8; b[0, 2] = 7;
    b[1, 0] = 6; b[1, 1] = 5; b[1, 2] = 4;
    b[2, 0] = 3; b[2, 1] = 2; b[2, 2] = 1;

    RequireMatCloseEigen(a * b, Bridge::ToEigen(a) * Bridge::ToEigen(b));
}

TEST_CASE("Mat2 * Vec2 matches Eigen", "[Mat.Arith][Eigen]") {
    mat2 m{};
    m[0, 0] = 1; m[0, 1] = 2;
    m[1, 0] = 3; m[1, 1] = 4;
    vec2 v{5, 6};

    auto em = Bridge::ToEigen(m);
    auto ev = Bridge::ToEigen(v);
    Eigen::Vector2f expected = em * ev;
    vec2 result = m * v;
    RequireClose(result.x, expected.x());
    RequireClose(result.y, expected.y());
}

// ===========================================================================
// [Mat.Func] — Transpose / Identity / Det / Inverse vs Eigen
// ===========================================================================

TEST_CASE("Transpose matches Eigen .transpose()", "[Mat.Func][Eigen]") {
    mat4 m = Math::MakeRotateAxis(vec3{1, 1, 0}, 0.5f);
    auto em = Bridge::ToEigen(m);
    RequireMatCloseEigen(Math::Transpose(m), em.transpose());

    mat3 m3{};
    m3[0, 0] = 1; m3[0, 1] = 2; m3[0, 2] = 3;
    m3[1, 0] = 4; m3[1, 1] = 5; m3[1, 2] = 6;
    m3[2, 0] = 7; m3[2, 1] = 8; m3[2, 2] = 9;
    RequireMatCloseEigen(Math::Transpose(m3), Bridge::ToEigen(m3).transpose());
}

TEST_CASE("Det matches Eigen .determinant()", "[Mat.Func][Eigen]") {
    // mat2
    mat2 m2{};
    m2[0, 0] = 3; m2[0, 1] = 8;
    m2[1, 0] = 4; m2[1, 1] = 6;
    RequireClose(Math::Det(m2), Bridge::ToEigen(m2).determinant());

    // mat3
    mat3 m3{};
    m3[0, 0] = 6; m3[0, 1] = 1; m3[0, 2] = 1;
    m3[1, 0] = 4; m3[1, 1] = -2; m3[1, 2] = 5;
    m3[2, 0] = 2; m3[2, 1] = 8; m3[2, 2] = 7;
    RequireClose(Math::Det(m3), Bridge::ToEigen(m3).determinant());

    // mat4
    mat4 m4 = Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
              * Math::MakeScale(vec3{2, 0.5f, 1.5f});
    RequireClose(Math::Det(m4), Bridge::ToEigen(m4).determinant(), 1e-3f);
}

TEST_CASE("Inverse(mat2) matches Eigen .inverse()", "[Mat.Func][Eigen]") {
    mat2 m{};
    m[0, 0] = 3; m[0, 1] = 8;
    m[1, 0] = 4; m[1, 1] = 6;
    RequireMatCloseEigen(Math::Inverse(m), Bridge::ToEigen(m).inverse());
}

TEST_CASE("Inverse(mat3) matches Eigen .inverse()", "[Mat.Func][Eigen]") {
    mat3 m{};
    m[0, 0] = 6; m[0, 1] = 1; m[0, 2] = 1;
    m[1, 0] = 4; m[1, 1] = -2; m[1, 2] = 5;
    m[2, 0] = 2; m[2, 1] = 8; m[2, 2] = 7;
    RequireMatCloseEigen(Math::Inverse(m), Bridge::ToEigen(m).inverse(), 1e-4f);
}

TEST_CASE("Inverse(mat4) matches Eigen .inverse()", "[Mat.Func][Eigen]") {
    mat4 m = Math::MakeTranslation(vec3{5, -3, 2})
             * Math::MakeRotateAxis(vec3{0.3f, 0.8f, -0.5f}, 1.1f)
             * Math::MakeScale(vec3{2, 0.5f, 1.5f});
    RequireMatCloseEigen(Math::Inverse(m), Bridge::ToEigen(m).inverse(), 1e-4f);
}

TEST_CASE("Identity matches Eigen Identity", "[Mat.Func][Eigen]") {
    RequireMatCloseEigen(Math::Identity(), Eigen::Matrix4f::Identity().eval());
    RequireMatCloseEigen(Math::Identity<mat3>(), Eigen::Matrix3f::Identity().eval());
    RequireMatCloseEigen(Math::Identity<mat2>(), Eigen::Matrix2f::Identity().eval());
}

// ===========================================================================
// [Xform3D] — 3D Transform builders vs Eigen
// ===========================================================================

TEST_CASE("MakeTranslation matches Eigen Translation3f", "[Xform3D][Eigen]") {
    vec3 t{10, -20, 30};
    mat4 m = Math::MakeTranslation(t);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.translate(Bridge::ToEigen(t));
    RequireMatCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeScale matches Eigen Scaling", "[Xform3D][Eigen]") {
    vec3 s{2, 3, 4};
    mat4 m = Math::MakeScale(s);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.scale(Bridge::ToEigen(s));
    RequireMatCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateX matches Eigen AngleAxisf(X)", "[Xform3D][Eigen]") {
    float angle = 1.2f;
    mat4 m = Math::MakeRotateX(angle);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitX()));
    RequireMatCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateY matches Eigen AngleAxisf(Y)", "[Xform3D][Eigen]") {
    float angle = -0.7f;
    mat4 m = Math::MakeRotateY(angle);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitY()));
    RequireMatCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateZ matches Eigen AngleAxisf(Z)", "[Xform3D][Eigen]") {
    float angle = 2.1f;
    mat4 m = Math::MakeRotateZ(angle);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitZ()));
    RequireMatCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotateAxis matches Eigen AngleAxisf(arbitrary)", "[Xform3D][Eigen]") {
    vec3 axis{0.3f, 0.8f, -0.5f};
    float angle = 1.1f;
    mat4 m = Math::MakeRotateAxis(axis, angle);

    Eigen::Vector3f eAxis = Bridge::ToEigen(axis).normalized();
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, eAxis));
    RequireMatCloseEigen(m, ea.matrix(), 1e-4f);
}

TEST_CASE("Rotation matrices have det = 1", "[Xform3D][Eigen]") {
    RequireClose(Math::Det(Math::MakeRotateX(0.5f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateY(1.2f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateZ(-0.3f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateAxis(vec3{1, 1, 1}, 2.0f)), 1.0f);
}

TEST_CASE("MakeLookAt matches Eigen LookAt construction", "[Xform3D][Eigen]") {
    vec3 eye{0, 0, 5};
    vec3 target{0, 0, 0};
    vec3 up{0, 1, 0};

    mat4 view = Math::MakeLookAt(eye, target, up);

    // Manually construct the same view matrix using Eigen
    Eigen::Vector3f eEye = Bridge::ToEigen(eye);
    Eigen::Vector3f eTarget = Bridge::ToEigen(target);
    Eigen::Vector3f eUp = Bridge::ToEigen(up);

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

    RequireMatCloseEigen(view, expected);
}

// ===========================================================================
// [Affine3D] — 3D Affine transforms (Mat<float,3,4>) vs 4×4 upper rows
// ===========================================================================

TEST_CASE("IdentityAffine is upper 3 rows of Identity()", "[Affine3D][Eigen]") {
    affine3 a = Math::IdentityAffine();
    mat4 full = Math::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeTranslationAffine is upper 3 rows of MakeTranslation", "[Affine3D][Eigen]") {
    vec3 t{5, -3, 7};
    affine3 a = Math::MakeTranslationAffine(t);
    mat4 full = Math::MakeTranslation(t);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeScaleAffine is upper 3 rows of MakeScale", "[Affine3D][Eigen]") {
    vec3 s{2, 0.5f, 3};
    affine3 a = Math::MakeScaleAffine(s);
    mat4 full = Math::MakeScale(s);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateXAffine is upper 3 rows of MakeRotateX", "[Affine3D][Eigen]") {
    float angle = 0.9f;
    affine3 a = Math::MakeRotateXAffine(angle);
    mat4 full = Math::MakeRotateX(angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateYAffine is upper 3 rows of MakeRotateY", "[Affine3D][Eigen]") {
    float angle = -1.3f;
    affine3 a = Math::MakeRotateYAffine(angle);
    mat4 full = Math::MakeRotateY(angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateZAffine is upper 3 rows of MakeRotateZ", "[Affine3D][Eigen]") {
    float angle = 2.0f;
    affine3 a = Math::MakeRotateZAffine(angle);
    mat4 full = Math::MakeRotateZ(angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotateAxisAffine is upper 3 rows of MakeRotateAxis", "[Affine3D][Eigen]") {
    vec3 axis{0.3f, 0.8f, -0.5f};
    float angle = 1.1f;
    affine3 a = Math::MakeRotateAxisAffine(axis, angle);
    mat4 full = Math::MakeRotateAxis(axis, angle);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeLookAtAffine is upper 3 rows of MakeLookAt", "[Affine3D][Eigen]") {
    vec3 eye{3, 4, 5};
    vec3 target{0, 0, 0};
    vec3 up{0, 1, 0};
    affine3 a = Math::MakeLookAtAffine(eye, target, up);
    mat4 full = Math::MakeLookAt(eye, target, up);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("InverseAffine matches Eigen Affine3f .inverse()", "[Affine3D][Eigen]") {
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
    Eigen::Vector3f eAxis = Bridge::ToEigen(axis).normalized();
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, eAxis));
    ea.translation() = Bridge::ToEigen(t);

    Eigen::Affine3f eai = ea.inverse();
    // Compare upper 3×4
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(ai[r, c], eai.matrix()(r, c), 1e-4f);
}

// ===========================================================================
// [Xform2D] — 2D transform builders vs Eigen
// ===========================================================================

TEST_CASE("MakeTranslation2D matches Eigen Translation2f", "[Xform2D][Eigen]") {
    vec2 t{3, -7};
    mat3 m = Math::MakeTranslation2D(t);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.translate(Bridge::ToEigen(t));
    // Eigen Affine2f .matrix() is 3×3
    Eigen::Matrix3f expected = ea.matrix();
    RequireMatCloseEigen(m, expected);
}

TEST_CASE("MakeScale2D matches Eigen Scaling2D", "[Xform2D][Eigen]") {
    vec2 s{2, 3};
    mat3 m = Math::MakeScale2D(s);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.scale(Bridge::ToEigen(s));
    RequireMatCloseEigen(m, ea.matrix());
}

TEST_CASE("MakeRotate2D matches Eigen Rotation2Df", "[Xform2D][Eigen]") {
    float angle = 1.5f;
    mat3 m = Math::MakeRotate2D(angle);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.rotate(Eigen::Rotation2Df(angle));
    RequireMatCloseEigen(m, ea.matrix());
}

// ===========================================================================
// [Affine2D] — 2D Affine transforms (Mat<float,2,3>) vs 3×3 upper rows
// ===========================================================================

TEST_CASE("IdentityAffine2D is upper 2 rows of Identity<mat3>()", "[Affine2D][Eigen]") {
    affine2 a = Math::IdentityAffine2D();
    mat3 full = Math::Identity<mat3>();
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeTranslation2DAffine is upper 2 rows of MakeTranslation2D", "[Affine2D][Eigen]") {
    vec2 t{3, -7};
    affine2 a = Math::MakeTranslation2DAffine(t);
    mat3 full = Math::MakeTranslation2D(t);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeScale2DAffine is upper 2 rows of MakeScale2D", "[Affine2D][Eigen]") {
    vec2 s{2, 0.5f};
    affine2 a = Math::MakeScale2DAffine(s);
    mat3 full = Math::MakeScale2D(s);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("MakeRotate2DAffine is upper 2 rows of MakeRotate2D", "[Affine2D][Eigen]") {
    float angle = 0.8f;
    affine2 a = Math::MakeRotate2DAffine(angle);
    mat3 full = Math::MakeRotate2D(angle);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r, c], full[r, c]);
}

TEST_CASE("InverseAffine2D matches Eigen Affine2f .inverse()", "[Affine2D][Eigen]") {
    float angle = 0.8f;
    vec2 t{3, -7};
    affine2 a = Math::MakeRotate2DAffine(angle);
    a[0, 2] = t.x;
    a[1, 2] = t.y;

    affine2 ai = Math::InverseAffine2D(a);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.rotate(Eigen::Rotation2Df(angle));
    ea.translation() = Bridge::ToEigen(t);
    Eigen::Affine2f eai = ea.inverse();

    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(ai[r, c], eai.matrix()(r, c), 1e-4f);
}

// ===========================================================================
// [Proj] — Projection matrices: verify near/far plane mapping
// ===========================================================================

TEST_CASE("MakePerspective: near → Z=0, far → Z=1 (Vulkan)", "[Proj][Eigen]") {
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

TEST_CASE("MakePerspectiveYFlipped has positive Y (Y-up)", "[Proj][Eigen]") {
    mat4 p = Math::MakePerspectiveYFlipped(kPi * 0.25f, 1.0f, 0.1f, 100.0f);
    // A point above center should map to positive clip Y
    vec4 above = p * vec4{0, 1, -1, 1};
    REQUIRE(above.y / above.w > 0.0f);
}

TEST_CASE("MakeOrtho: near → Z=0, far → Z=1 (Vulkan)", "[Proj][Eigen]") {
    float l = -1, r = 1, b = -1, t = 1, n = 0.1f, f = 100.0f;
    mat4 o = Math::MakeOrtho(l, r, b, t, n, f);

    // Near plane center
    vec4 nearPt = o * vec4{0, 0, -n, 1};
    RequireClose(nearPt.z / nearPt.w, 0.0f, 1e-4f);

    // Far plane center
    vec4 farPt = o * vec4{0, 0, -f, 1};
    RequireClose(farPt.z / farPt.w, 1.0f, 1e-4f);
}

TEST_CASE("MakeOrthoYFlipped has positive Y for point above center", "[Proj][Eigen]") {
    mat4 o = Math::MakeOrthoYFlipped(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f);
    vec4 above = o * vec4{0, 0.5f, -1, 1};
    REQUIRE(above.y / above.w > 0.0f);
}

// ===========================================================================
// [Constexpr] — compile-time evaluation (constexpr smoke tests)
// ===========================================================================

TEST_CASE("Constexpr vector ops fold correctly", "[Constexpr][Eigen]") {
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

TEST_CASE("Constexpr matrix ops fold correctly", "[Constexpr][Eigen]") {
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

TEST_CASE("Constexpr Det folds at compile time", "[Constexpr][Eigen]") {
    constexpr mat2 m2 = [] {
        mat2 m{};
        m[0, 0] = 3; m[0, 1] = 8;
        m[1, 0] = 4; m[1, 1] = 6;
        return m;
    }();
    STATIC_REQUIRE(Math::Det(m2) == -14.0f);
}

TEST_CASE("Constexpr 2D rotation folds at compile time", "[Constexpr][Eigen]") {
    constexpr mat3 r = Math::MakeRotate2D(0.0f);
    STATIC_REQUIRE(r[0, 0] == 1.0f);
    STATIC_REQUIRE(r[1, 1] == 1.0f);
    STATIC_REQUIRE(r[0, 1] == 0.0f);
    STATIC_REQUIRE(r[1, 0] == 0.0f);
}

// ===========================================================================
// [Stress] — Randomized stress tests with multiple inputs
// ===========================================================================

TEST_CASE("Inverse is left and right inverse (multiple random TRS)", "[Mat.Func][Eigen][Stress]") {
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
        RequireMatCloseEigen(m * mi, Eigen::Matrix4f::Identity().eval(), 1e-3f);
        RequireMatCloseEigen(mi * m, Eigen::Matrix4f::Identity().eval(), 1e-3f);
    }
}

TEST_CASE("InverseAffine matches full Inverse upper rows", "[Affine3D][Eigen][Stress]") {
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
