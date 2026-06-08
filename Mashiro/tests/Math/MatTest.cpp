/**
 * @file MatTest.cpp
 * @brief Mat arithmetic and functions validated against Eigen reference.
 */
#include "Support/EigenBridge.h"
#include "Support/Meta.h"

#include <Eigen/Dense>

using namespace Mashiro;
using namespace Mashiro::Testing;
using Catch::Approx;

// ===========================================================================
// Arithmetic
// ===========================================================================

TEST_CASE("Mat4 add/sub/scalar matches Eigen", AUTO_TAG) {
    mat4 a = Math::ToFull(Math::MakeTranslation(vec3{1,2,3}));
    mat4 b = Math::ToFull(Math::MakeScale(vec3{2,3,4}));
    auto ea = ToEigen(a), eb = ToEigen(b);

    RequireCloseEigen(a + b, ea + eb);
    RequireCloseEigen(a - b, ea - eb);
    RequireCloseEigen(a * 2.5f, ea * 2.5f);
    RequireCloseEigen(-a, -ea);
}

TEST_CASE("Mat4 * Mat4 matches Eigen", AUTO_TAG) {
    mat4 a = Math::ToFull(Math::MakeRotateZ(0.7f));
    mat4 b = Math::ToFull(Math::MakeTranslation(vec3{5,-3,2}));
    auto ea = ToEigen(a), eb = ToEigen(b);
    RequireCloseEigen(a * b, ea * eb);
    RequireCloseEigen(b * a, eb * ea);
}

TEST_CASE("Mat4 * Vec4 matches Eigen", AUTO_TAG) {
    mat4 m = Math::ToFull(Math::MakeRotateAxis(vec3{0.3f,0.8f,-0.5f}, 1.1f)
             * Math::MakeTranslation(vec3{10,20,30}));
    vec4 v{1,2,3,1};
    RequireCloseEigen(m * v, ToEigen(m) * ToEigen(v));
}

TEST_CASE("Mat3 * Mat3 matches Eigen", AUTO_TAG) {
    mat3 a{};
    a[0,0]=1; a[0,1]=2; a[0,2]=3;
    a[1,0]=4; a[1,1]=5; a[1,2]=6;
    a[2,0]=7; a[2,1]=8; a[2,2]=9;
    mat3 b{};
    b[0,0]=9; b[0,1]=8; b[0,2]=7;
    b[1,0]=6; b[1,1]=5; b[1,2]=4;
    b[2,0]=3; b[2,1]=2; b[2,2]=1;
    RequireCloseEigen(a * b, ToEigen(a) * ToEigen(b));
}

TEST_CASE("Mat2 * Vec2 matches Eigen", AUTO_TAG) {
    mat2 m{};
    m[0,0]=1; m[0,1]=2;
    m[1,0]=3; m[1,1]=4;
    vec2 v{5,6};
    RequireCloseEigen(m * v, ToEigen(m) * ToEigen(v));
}

// ===========================================================================
// Functions: Transpose / Det / Inverse / Identity
// ===========================================================================

TEST_CASE("Transpose matches Eigen .transpose()", AUTO_TAG) {
    mat4 m = Math::ToFull(Math::MakeRotateAxis(vec3{1,1,0}, 0.5f));
    RequireCloseEigen(Math::Transpose(m), ToEigen(m).transpose());

    mat3 m3{};
    m3[0,0]=1; m3[0,1]=2; m3[0,2]=3;
    m3[1,0]=4; m3[1,1]=5; m3[1,2]=6;
    m3[2,0]=7; m3[2,1]=8; m3[2,2]=9;
    RequireCloseEigen(Math::Transpose(m3), ToEigen(m3).transpose());
}

TEST_CASE("Det matches Eigen .determinant()", AUTO_TAG) {
    mat2 m2{};
    m2[0,0]=3; m2[0,1]=8;
    m2[1,0]=4; m2[1,1]=6;
    RequireClose(Math::Det(m2), ToEigen(m2).determinant());

    mat3 m3{};
    m3[0,0]=6; m3[0,1]=1; m3[0,2]=1;
    m3[1,0]=4; m3[1,1]=-2; m3[1,2]=5;
    m3[2,0]=2; m3[2,1]=8; m3[2,2]=7;
    RequireClose(Math::Det(m3), ToEigen(m3).determinant());

    mat4 m4 = Math::ToFull(Math::MakeRotateAxis(vec3{0.3f,0.8f,-0.5f}, 1.1f)
              * Math::MakeScale(vec3{2,0.5f,1.5f}));
    RequireClose(Math::Det(m4), ToEigen(m4).determinant(), 1e-3f);
}

TEST_CASE("Inverse(mat2) matches Eigen", AUTO_TAG) {
    mat2 m{};
    m[0,0]=3; m[0,1]=8;
    m[1,0]=4; m[1,1]=6;
    RequireCloseEigen(Math::Inverse(m), ToEigen(m).inverse());
}

TEST_CASE("Inverse(mat3) matches Eigen", AUTO_TAG) {
    mat3 m{};
    m[0,0]=6; m[0,1]=1; m[0,2]=1;
    m[1,0]=4; m[1,1]=-2; m[1,2]=5;
    m[2,0]=2; m[2,1]=8; m[2,2]=7;
    RequireCloseEigen(Math::Inverse(m), ToEigen(m).inverse(), 1e-4f);
}

TEST_CASE("Inverse(mat4) matches Eigen", AUTO_TAG) {
    mat4 m = Math::ToFull(Math::MakeTranslation(vec3{5,-3,2})
             * Math::MakeRotateAxis(vec3{0.3f,0.8f,-0.5f}, 1.1f)
             * Math::MakeScale(vec3{2,0.5f,1.5f}));
    RequireCloseEigen(Math::Inverse(m), ToEigen(m).inverse(), 1e-4f);
}

TEST_CASE("Identity matches Eigen Identity", AUTO_TAG) {
    RequireCloseEigen(Math::Identity(), Eigen::Matrix4f::Identity().eval());
    RequireCloseEigen(Math::Identity<mat3>(), Eigen::Matrix3f::Identity().eval());
    RequireCloseEigen(Math::Identity<mat2>(), Eigen::Matrix2f::Identity().eval());
}
