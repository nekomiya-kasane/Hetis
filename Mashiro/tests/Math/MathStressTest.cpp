/**
 * @file MathStressTest.cpp
 * @brief Stress tests and constexpr fold verification for math library.
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
}

// ===========================================================================
// Constexpr fold verification
// ===========================================================================

TEST_CASE("Constexpr vector ops fold correctly", AUTO_TAG) {
    constexpr vec3 a{1,2,3}, b{4,5,6};
    constexpr vec3 sum = a + b;
    STATIC_REQUIRE(sum.x == 5.0f);
    STATIC_REQUIRE(sum.y == 7.0f);
    STATIC_REQUIRE(sum.z == 9.0f);

    constexpr float dot = Math::Dot(a, b);
    STATIC_REQUIRE(dot == 32.0f);

    constexpr vec3 cross = Math::Cross(vec3{1,0,0}, vec3{0,1,0});
    STATIC_REQUIRE(cross.z == 1.0f);
}

TEST_CASE("Constexpr matrix ops fold correctly", AUTO_TAG) {
    constexpr mat4 id = Math::Identity();
    STATIC_REQUIRE(id[0,0] == 1.0f);
    STATIC_REQUIRE(id[1,1] == 1.0f);
    STATIC_REQUIRE(id[0,1] == 0.0f);

    constexpr mat4 t = Math::MakeTranslation(vec3{1,2,3});
    STATIC_REQUIRE(t[0,3] == 1.0f);
    STATIC_REQUIRE(t[1,3] == 2.0f);
    STATIC_REQUIRE(t[2,3] == 3.0f);
    STATIC_REQUIRE(t[3,3] == 1.0f);
}

TEST_CASE("Constexpr Det folds at compile time", AUTO_TAG) {
    constexpr mat2 m2 = [] {
        mat2 m{};
        m[0,0]=3; m[0,1]=8;
        m[1,0]=4; m[1,1]=6;
        return m;
    }();
    STATIC_REQUIRE(Math::Det(m2) == -14.0f);
}

TEST_CASE("Constexpr 2D rotation folds at compile time", AUTO_TAG) {
    constexpr mat3 r = Math::MakeRotate2D(0.0f);
    STATIC_REQUIRE(r[0,0] == 1.0f);
    STATIC_REQUIRE(r[1,1] == 1.0f);
    STATIC_REQUIRE(r[0,1] == 0.0f);
    STATIC_REQUIRE(r[1,0] == 0.0f);
}

// ===========================================================================
// Stress tests
// ===========================================================================

TEST_CASE("Inverse is left and right inverse (multiple TRS)", AUTO_TAG) {
    struct TD { vec3 t; vec3 axis; float angle; vec3 s; };
    TD cases[] = {
        {{1,2,3},      {1,0,0}, 0.5f,  {1,1,1}},
        {{-5,7,0.1f},  {0,1,0}, 1.2f,  {2,0.5f,3}},
        {{0,0,0},      {0,0,1}, -0.3f, {0.1f,10,0.5f}},
        {{100,-50,25}, {1,1,1}, 3.0f,  {0.5f,0.5f,0.5f}},
    };
    for (auto& [t,axis,angle,s] : cases) {
        mat4 m = Math::MakeTranslation(t)
                 * Math::MakeRotateAxis(axis, angle)
                 * Math::MakeScale(s);
        mat4 mi = Math::Inverse(m);
        RequireCloseEigen(m * mi, Eigen::Matrix4f::Identity().eval(), 1e-3f);
        RequireCloseEigen(mi * m, Eigen::Matrix4f::Identity().eval(), 1e-3f);
    }
}

TEST_CASE("InverseAffine matches full Inverse upper rows", AUTO_TAG) {
    struct TD { vec3 axis; float angle; vec3 t; };
    TD cases[] = {
        {{1,0,0}, 0.5f,  {1,2,3}},
        {{0,1,0}, -1.2f, {-5,7,0}},
        {{1,1,1}, 2.5f,  {0,0,100}},
    };
    for (auto& [axis,angle,t] : cases) {
        affine3 a = Math::MakeRotateAxisAffine(axis, angle);
        a[0,3]=t.x; a[1,3]=t.y; a[2,3]=t.z;
        affine3 ai = Math::InverseAffine(a);

        mat4 full = Math::Identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                full[r,c] = a[r,c];
        mat4 fullInv = Math::Inverse(full);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                RequireClose(ai[r,c], fullInv[r,c], 1e-4f);
    }
}
