/**
 * @file MathUtilsXform3DTest.cpp
 * @brief MathUtils.h 3D transforms, affine, projection validated against Eigen.
 */
#include "Support/EigenBridge.h"

#include <Eigen/Dense>
#include <numbers>

using namespace Mashiro;
using namespace Mashiro::Testing;
using Catch::Approx;

namespace {
    constexpr float kEps = 1e-5f;
    constexpr float kPi  = std::numbers::pi_v<float>;
}

// ===========================================================================
// 3D transform builders
// ===========================================================================

TEST_CASE("MakeTranslation matches Eigen", "[Math.MathUtils]") {
    vec3 t{10,-20,30};
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.translate(ToEigen(t));
    RequireCloseEigen(Math::MakeTranslation(t), ea.matrix());
}

TEST_CASE("MakeScale matches Eigen", "[Math.MathUtils]") {
    vec3 s{2,3,4};
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.scale(ToEigen(s));
    RequireCloseEigen(Math::MakeScale(s), ea.matrix());
}

TEST_CASE("MakeRotateX matches Eigen", "[Math.MathUtils]") {
    float angle = 1.2f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitX()));
    RequireCloseEigen(Math::MakeRotateX(angle), ea.matrix());
}

TEST_CASE("MakeRotateY matches Eigen", "[Math.MathUtils]") {
    float angle = -0.7f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitY()));
    RequireCloseEigen(Math::MakeRotateY(angle), ea.matrix());
}

TEST_CASE("MakeRotateZ matches Eigen", "[Math.MathUtils]") {
    float angle = 2.1f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitZ()));
    RequireCloseEigen(Math::MakeRotateZ(angle), ea.matrix());
}

TEST_CASE("MakeRotateAxis matches Eigen", "[Math.MathUtils]") {
    vec3 axis{0.3f,0.8f,-0.5f};
    float angle = 1.1f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, ToEigen(axis).normalized()));
    RequireCloseEigen(Math::MakeRotateAxis(axis, angle), ea.matrix(), 1e-4f);
}

TEST_CASE("Rotation matrices have det = 1", "[Math.MathUtils]") {
    RequireClose(Math::Det(Math::MakeRotateX(0.5f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateY(1.2f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateZ(-0.3f)), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateAxis(vec3{1,1,1}, 2.0f)), 1.0f);
}

TEST_CASE("MakeLookAt matches manual Eigen construction", "[Math.MathUtils]") {
    vec3 eye{0,0,5}, target{0,0,0}, up{0,1,0};
    mat4 view = Math::MakeLookAt(eye, target, up);

    auto eEye = ToEigen(eye), eTarget = ToEigen(target), eUp = ToEigen(up);
    Eigen::Vector3f f = (eTarget - eEye).normalized();
    Eigen::Vector3f s = f.cross(eUp).normalized();
    Eigen::Vector3f u = s.cross(f);

    Eigen::Matrix4f expected = Eigen::Matrix4f::Identity();
    expected(0,0)=s.x(); expected(0,1)=s.y(); expected(0,2)=s.z();
    expected(1,0)=u.x(); expected(1,1)=u.y(); expected(1,2)=u.z();
    expected(2,0)=-f.x(); expected(2,1)=-f.y(); expected(2,2)=-f.z();
    expected(0,3)=-s.dot(eEye);
    expected(1,3)=-u.dot(eEye);
    expected(2,3)=f.dot(eEye);
    RequireCloseEigen(view, expected);
}

// ===========================================================================
// 3D Affine (mat3x4 compact)
// ===========================================================================

TEST_CASE("IdentityAffine is upper 3 rows of Identity()", "[Math.MathUtils]") {
    affine3 a = Math::IdentityAffine();
    mat4 full = Math::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a[r,c], full[r,c]);
}

TEST_CASE("Affine3D builders match upper rows of 4x4", "[Math.MathUtils]") {
    auto check = [](auto affFn, auto fullFn) {
        auto a = affFn();
        auto f = fullFn();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                RequireClose(a[r,c], f[r,c]);
    };

    check([]{return Math::MakeTranslationAffine(vec3{5,-3,7});},
          []{return Math::MakeTranslation(vec3{5,-3,7});});
    check([]{return Math::MakeScaleAffine(vec3{2,0.5f,3});},
          []{return Math::MakeScale(vec3{2,0.5f,3});});
    check([]{return Math::MakeRotateXAffine(0.9f);},
          []{return Math::MakeRotateX(0.9f);});
    check([]{return Math::MakeRotateYAffine(-1.3f);},
          []{return Math::MakeRotateY(-1.3f);});
    check([]{return Math::MakeRotateZAffine(2.0f);},
          []{return Math::MakeRotateZ(2.0f);});
    check([]{return Math::MakeRotateAxisAffine(vec3{0.3f,0.8f,-0.5f}, 1.1f);},
          []{return Math::MakeRotateAxis(vec3{0.3f,0.8f,-0.5f}, 1.1f);});
    check([]{return Math::MakeLookAtAffine(vec3{3,4,5},vec3{0,0,0},vec3{0,1,0});},
          []{return Math::MakeLookAt(vec3{3,4,5},vec3{0,0,0},vec3{0,1,0});});
}

TEST_CASE("InverseAffine matches Eigen Affine3f .inverse()", "[Math.MathUtils]") {
    vec3 axis{0.3f,0.8f,-0.5f}, t{5,-3,2};
    float angle = 1.1f;
    affine3 a = Math::MakeRotateAxisAffine(axis, angle);
    a[0,3]=t.x; a[1,3]=t.y; a[2,3]=t.z;
    affine3 ai = Math::InverseAffine(a);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, ToEigen(axis).normalized()));
    ea.translation() = ToEigen(t);
    Eigen::Affine3f eai = ea.inverse();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(ai[r,c], eai.matrix()(r,c), 1e-4f);
}

// ===========================================================================
// Projection
// ===========================================================================

TEST_CASE("MakePerspective: near->Z=0, far->Z=1 (Vulkan)", "[Math.MathUtils]") {
    float nearZ=0.1f, farZ=100.0f;
    mat4 p = Math::MakePerspective(kPi*0.25f, 16.0f/9.0f, nearZ, farZ);
    vec4 nearPt = p * vec4{0,0,-nearZ,1};
    RequireClose(nearPt.z / nearPt.w, 0.0f, 1e-4f);
    vec4 farPt = p * vec4{0,0,-farZ,1};
    RequireClose(farPt.z / farPt.w, 1.0f, 1e-4f);
}

TEST_CASE("MakePerspectiveYFlipped has positive Y", "[Math.MathUtils]") {
    mat4 p = Math::MakePerspectiveYFlipped(kPi*0.25f, 1.0f, 0.1f, 100.0f);
    vec4 above = p * vec4{0,1,-1,1};
    REQUIRE(above.y / above.w > 0.0f);
}

TEST_CASE("MakeOrtho: near->Z=0, far->Z=1 (Vulkan)", "[Math.MathUtils]") {
    float l=-1,r=1,b=-1,t=1,n=0.1f,f=100.0f;
    mat4 o = Math::MakeOrtho(l,r,b,t,n,f);
    vec4 nearPt = o * vec4{0,0,-n,1};
    RequireClose(nearPt.z / nearPt.w, 0.0f, 1e-4f);
    vec4 farPt = o * vec4{0,0,-f,1};
    RequireClose(farPt.z / farPt.w, 1.0f, 1e-4f);
}

TEST_CASE("MakeOrthoYFlipped has positive Y", "[Math.MathUtils]") {
    mat4 o = Math::MakeOrthoYFlipped(-1.0f,1.0f,-1.0f,1.0f,0.1f,100.0f);
    vec4 above = o * vec4{0,0.5f,-1,1};
    REQUIRE(above.y / above.w > 0.0f);
}
