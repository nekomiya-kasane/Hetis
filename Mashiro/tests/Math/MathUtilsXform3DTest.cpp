/**
 * @file MathUtilsXform3DTest.cpp
 * @brief MathUtils.h 3D transforms, affine, projection validated against Eigen.
 */
#include "Support/EigenBridge.h"
#include "Support/Meta.h"

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

TEST_CASE("MakeTranslation matches Eigen", AUTO_TAG) {
    vec3 t{10,-20,30};
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.translate(ToEigen(t));
    RequireCloseEigen(Math::MakeTranslation(t).ToMat(), ea.matrix());
}

TEST_CASE("MakeScale matches Eigen", AUTO_TAG) {
    vec3 s{2,3,4};
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.scale(ToEigen(s));
    RequireCloseEigen(Math::MakeScale(s).ToMat(), ea.matrix());
}

TEST_CASE("MakeRotateX matches Eigen", AUTO_TAG) {
    float angle = 1.2f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitX()));
    RequireCloseEigen(Math::MakeRotateX(angle).ToMat(), ea.matrix());
}

TEST_CASE("MakeRotateY matches Eigen", AUTO_TAG) {
    float angle = -0.7f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitY()));
    RequireCloseEigen(Math::MakeRotateY(angle).ToMat(), ea.matrix());
}

TEST_CASE("MakeRotateZ matches Eigen", AUTO_TAG) {
    float angle = 2.1f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitZ()));
    RequireCloseEigen(Math::MakeRotateZ(angle).ToMat(), ea.matrix());
}

TEST_CASE("MakeRotateAxis matches Eigen", AUTO_TAG) {
    vec3 axis{0.3f,0.8f,-0.5f};
    float angle = 1.1f;
    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, ToEigen(axis).normalized()));
    RequireCloseEigen(Math::MakeRotateAxis(axis, angle).ToMat(), ea.matrix(), 1e-4f);
}

TEST_CASE("Rotation matrices have det = 1", AUTO_TAG) {
    RequireClose(Math::Det(Math::MakeRotateX(0.5f).Linear()), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateY(1.2f).Linear()), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateZ(-0.3f).Linear()), 1.0f);
    RequireClose(Math::Det(Math::MakeRotateAxis(vec3{1,1,1}, 2.0f).Linear()), 1.0f);
}

TEST_CASE("MakeLookAt matches manual Eigen construction", AUTO_TAG) {
    vec3 eye{0,0,5}, target{0,0,0}, up{0,1,0};
    mat4 view = Math::MakeLookAt(eye, target, up).ToMat();

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

TEST_CASE("IdentityAffine is upper 3 rows of Identity()", AUTO_TAG) {
    affine3 a = affine3::Identity();
    mat4 full = Math::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(a.m[r,c], full[r,c]);
}

TEST_CASE("Affine3D builders match upper rows of 4x4", AUTO_TAG) {
    auto check = [](auto affFn, auto fullFn) {
        auto a = affFn();
        auto f = fullFn();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                RequireClose(a.m[r,c], f[r,c]);
    };

    check([]{return Math::MakeTranslation(vec3{5,-3,7});},
          []{return Math::MakeTranslation(vec3{5,-3,7}).ToMat();});
    check([]{return Math::MakeScale(vec3{2,0.5f,3});},
          []{return Math::MakeScale(vec3{2,0.5f,3}).ToMat();});
    check([]{return Math::MakeRotateX(0.9f);},
          []{return Math::MakeRotateX(0.9f).ToMat();});
    check([]{return Math::MakeRotateY(-1.3f);},
          []{return Math::MakeRotateY(-1.3f).ToMat();});
    check([]{return Math::MakeRotateZ(2.0f);},
          []{return Math::MakeRotateZ(2.0f).ToMat();});
    check([]{return Math::MakeRotateAxis(vec3{0.3f,0.8f,-0.5f}, 1.1f);},
          []{return Math::MakeRotateAxis(vec3{0.3f,0.8f,-0.5f}, 1.1f).ToMat();});
    check([]{return Math::MakeLookAt(vec3{3,4,5},vec3{0,0,0},vec3{0,1,0});},
          []{return Math::MakeLookAt(vec3{3,4,5},vec3{0,0,0},vec3{0,1,0}).ToMat();});
}

TEST_CASE("InverseAffine matches Eigen Affine3f .inverse()", AUTO_TAG) {
    vec3 axis{0.3f,0.8f,-0.5f}, t{5,-3,2};
    float angle = 1.1f;
    affine3 a = Math::MakeRotateAxis(axis, angle);
    a.m[0,3]=t.x; a.m[1,3]=t.y; a.m[2,3]=t.z;
    affine3 ai = Math::Inverse(a);

    Eigen::Affine3f ea = Eigen::Affine3f::Identity();
    ea.rotate(Eigen::AngleAxisf(angle, ToEigen(axis).normalized()));
    ea.translation() = ToEigen(t);
    Eigen::Affine3f eai = ea.inverse();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            RequireClose(ai.m[r,c], eai.matrix()(r,c), 1e-4f);
}

// ===========================================================================
// Projection
// ===========================================================================

TEST_CASE("MakePerspective: near->Z=0, far->Z=1 (Vulkan)", AUTO_TAG) {
    float nearZ=0.1f, farZ=100.0f;
    mat4 p = Math::MakePerspective(kPi*0.25f, 16.0f/9.0f, nearZ, farZ);
    vec4 nearPt = p * vec4{0,0,-nearZ,1};
    RequireClose(nearPt.z / nearPt.w, 0.0f, 1e-4f);
    vec4 farPt = p * vec4{0,0,-farZ,1};
    RequireClose(farPt.z / farPt.w, 1.0f, 1e-4f);
}

TEST_CASE("MakePerspectiveYFlipped has positive Y", AUTO_TAG) {
    mat4 p = Math::MakePerspectiveYFlipped(kPi*0.25f, 1.0f, 0.1f, 100.0f);
    vec4 above = p * vec4{0,1,-1,1};
    REQUIRE(above.y / above.w > 0.0f);
}

TEST_CASE("MakeOrtho: near->Z=0, far->Z=1 (Vulkan)", AUTO_TAG) {
    float l=-1,r=1,b=-1,t=1,n=0.1f,f=100.0f;
    mat4 o = Math::MakeOrtho(l,r,b,t,n,f);
    vec4 nearPt = o * vec4{0,0,-n,1};
    RequireClose(nearPt.z / nearPt.w, 0.0f, 1e-4f);
    vec4 farPt = o * vec4{0,0,-f,1};
    RequireClose(farPt.z / farPt.w, 1.0f, 1e-4f);
}

TEST_CASE("MakeOrthoYFlipped has positive Y", AUTO_TAG) {
    mat4 o = Math::MakeOrthoYFlipped(-1.0f,1.0f,-1.0f,1.0f,0.1f,100.0f);
    vec4 above = o * vec4{0,0.5f,-1,1};
    REQUIRE(above.y / above.w > 0.0f);
}
