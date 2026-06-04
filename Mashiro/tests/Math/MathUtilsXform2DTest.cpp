/**
 * @file MathUtilsXform2DTest.cpp
 * @brief MathUtils.h 2D transforms and affine validated against Eigen.
 */
#include "Support/EigenBridge.h"
#include "Support/Meta.h"

#include <Eigen/Dense>

using namespace Mashiro;
using namespace Mashiro::Testing;
using Catch::Approx;

namespace {
    constexpr float kEps = 1e-5f;
}

TEST_CASE("MakeTranslation2D matches Eigen Translation2f", AUTO_TAG) {
    vec2 t{3,-7};
    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.translate(ToEigen(t));
    RequireCloseEigen(Math::MakeTranslation2D(t), ea.matrix());
}

TEST_CASE("MakeScale2D matches Eigen Scaling2D", AUTO_TAG) {
    vec2 s{2,3};
    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.scale(ToEigen(s));
    RequireCloseEigen(Math::MakeScale2D(s), ea.matrix());
}

TEST_CASE("MakeRotate2D matches Eigen Rotation2Df", AUTO_TAG) {
    float angle = 1.5f;
    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.rotate(Eigen::Rotation2Df(angle));
    RequireCloseEigen(Math::MakeRotate2D(angle), ea.matrix());
}

TEST_CASE("IdentityAffine2D is upper 2 rows of Identity<mat3>()", AUTO_TAG) {
    affine2 a = Math::IdentityAffine2D();
    mat3 full = Math::Identity<mat3>();
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(a[r,c], full[r,c]);
}

TEST_CASE("Affine2D builders match upper rows of 3x3", AUTO_TAG) {
    auto check = [](auto affFn, auto fullFn) {
        auto a = affFn();
        auto f = fullFn();
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                RequireClose(a[r,c], f[r,c]);
    };
    check([]{return Math::MakeTranslation2DAffine(vec2{3,-7});},
          []{return Math::MakeTranslation2D(vec2{3,-7});});
    check([]{return Math::MakeScale2DAffine(vec2{2,0.5f});},
          []{return Math::MakeScale2D(vec2{2,0.5f});});
    check([]{return Math::MakeRotate2DAffine(0.8f);},
          []{return Math::MakeRotate2D(0.8f);});
}

TEST_CASE("InverseAffine2D matches Eigen Affine2f .inverse()", AUTO_TAG) {
    float angle = 0.8f;
    vec2 t{3,-7};
    affine2 a = Math::MakeRotate2DAffine(angle);
    a[0,2]=t.x; a[1,2]=t.y;
    affine2 ai = Math::InverseAffine2D(a);

    Eigen::Affine2f ea = Eigen::Affine2f::Identity();
    ea.rotate(Eigen::Rotation2Df(angle));
    ea.translation() = ToEigen(t);
    Eigen::Affine2f eai = ea.inverse();
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            RequireClose(ai[r,c], eai.matrix()(r,c), 1e-4f);
}
