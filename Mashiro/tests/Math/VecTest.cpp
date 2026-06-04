/**
 * @file VecTest.cpp
 * @brief Vec arithmetic and functions validated against Eigen reference.
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
// Arithmetic
// ===========================================================================

TEST_CASE("Vec3 arithmetic matches Eigen", AUTO_TAG) {
    vec3 a{1.5f, -2.3f, 4.1f};
    vec3 b{3.7f, 0.8f, -1.2f};
    auto ea = ToEigen(a), eb = ToEigen(b);

    RequireCloseEigen(a + b, ea + eb);
    RequireCloseEigen(a - b, ea - eb);
    RequireCloseEigen(a * 2.5f, ea * 2.5f);
    RequireCloseEigen(2.5f * a, 2.5f * ea);
    RequireCloseEigen(-a, -ea);
}

TEST_CASE("Vec4 arithmetic matches Eigen", AUTO_TAG) {
    vec4 a{1.0f, -2.0f, 3.0f, -4.0f};
    vec4 b{5.0f, 6.0f, -7.0f, 8.0f};
    auto ea = ToEigen(a), eb = ToEigen(b);

    RequireCloseEigen(a + b, ea + eb);
    RequireCloseEigen(a - b, ea - eb);
    RequireCloseEigen(a * 3.0f, ea * 3.0f);
    RequireCloseEigen(-b, -eb);
}

TEST_CASE("Vec2 arithmetic matches Eigen", AUTO_TAG) {
    vec2 a{3.0f, -1.5f}, b{-2.0f, 4.0f};
    auto ea = ToEigen(a), eb = ToEigen(b);
    RequireCloseEigen(a + b, Eigen::Vector2f(ea + eb));
    RequireCloseEigen(a - b, Eigen::Vector2f(ea - eb));
}

TEST_CASE("Hadamard product matches Eigen cwiseProduct", AUTO_TAG) {
    vec3 a{2.0f, 3.0f, 4.0f}, b{5.0f, 6.0f, 7.0f};
    RequireCloseEigen(a * b, ToEigen(a).cwiseProduct(ToEigen(b)));
}

TEST_CASE("Component-wise division matches Eigen cwiseQuotient", AUTO_TAG) {
    vec3 a{6.0f, 8.0f, 10.0f}, b{2.0f, 4.0f, 5.0f};
    RequireCloseEigen(a / b, ToEigen(a).cwiseQuotient(ToEigen(b)));
}

// ===========================================================================
// Functions
// ===========================================================================

TEST_CASE("Dot product matches Eigen", AUTO_TAG) {
    vec3 a{1,2,3}, b{4,-5,6};
    RequireClose(Math::Dot(a, b), ToEigen(a).dot(ToEigen(b)));
    vec4 c{1,2,3,4}, d{5,6,7,8};
    RequireClose(Math::Dot(c, d), ToEigen(c).dot(ToEigen(d)));
    vec2 e{3,4}, f{-1,2};
    RequireClose(Math::Dot(e, f), ToEigen(e).dot(ToEigen(f)));
}

TEST_CASE("Cross product (3D) matches Eigen", AUTO_TAG) {
    vec3 a{1,0,0}, b{0,1,0};
    RequireCloseEigen(Math::Cross(a, b), ToEigen(a).cross(ToEigen(b)));
    vec3 c{2.5f,-1.3f,0.7f}, d{-0.4f,3.1f,-2.2f};
    RequireCloseEigen(Math::Cross(c, d), ToEigen(c).cross(ToEigen(d)));
}

TEST_CASE("Cross product (2D) matches manual formula", AUTO_TAG) {
    vec2 a{3,4}, b{-1,2};
    auto ea = ToEigen(a), eb = ToEigen(b);
    RequireClose(Math::Cross(a, b), ea.x() * eb.y() - ea.y() * eb.x());
}

TEST_CASE("Norm1/Norm2/NormInf match Eigen norms", AUTO_TAG) {
    vec3 v{3,-4,12};
    auto ev = ToEigen(v);
    RequireClose(Math::Norm1(v), ev.lpNorm<1>());
    RequireClose(Math::Norm2(v), ev.norm());
    RequireClose(Math::NormInf(v), ev.lpNorm<Eigen::Infinity>());

    vec4 w{1,-2,3,-4};
    auto ew = ToEigen(w);
    RequireClose(Math::Norm1(w), ew.lpNorm<1>());
    RequireClose(Math::Norm2(w), ew.norm());
    RequireClose(Math::NormInf(w), ew.lpNorm<Eigen::Infinity>());
}

TEST_CASE("Normalize matches Eigen .normalized()", AUTO_TAG) {
    vec3 v{3,-4,0};
    RequireCloseEigen(Math::Normalize(v), ToEigen(v).normalized());
    vec4 w{1,2,3,4};
    RequireCloseEigen(Math::Normalize(w), ToEigen(w).normalized());
}

TEST_CASE("Distance matches Eigen (b-a).norm()", AUTO_TAG) {
    vec3 a{1,2,3}, b{4,6,3};
    RequireClose(Math::Distance(a, b), (ToEigen(b) - ToEigen(a)).norm());
}

// ===========================================================================
// Component-wise functions
// ===========================================================================

TEST_CASE("Min/Max match Eigen cwiseMin/cwiseMax", AUTO_TAG) {
    vec3 a{1,5,3}, b{4,2,6};
    auto ea = ToEigen(a), eb = ToEigen(b);
    RequireCloseEigen(Math::Min(a, b), ea.cwiseMin(eb));
    RequireCloseEigen(Math::Max(a, b), ea.cwiseMax(eb));
}

TEST_CASE("Abs matches Eigen .cwiseAbs()", AUTO_TAG) {
    vec3 v{-1,2,-3};
    RequireCloseEigen(Math::Abs(v), ToEigen(v).cwiseAbs());
}

TEST_CASE("Lerp matches Eigen manual lerp", AUTO_TAG) {
    vec3 a{0,0,0}, b{10,20,30};
    float t = 0.3f;
    auto ea = ToEigen(a), eb = ToEigen(b);
    RequireCloseEigen(Math::Lerp(a, b, t), Eigen::Vector3f(ea + (eb - ea) * t));
}

TEST_CASE("Reflect matches Eigen formula", AUTO_TAG) {
    vec3 v{1,-1,0}, n{0,1,0};
    auto ev = ToEigen(v), en = ToEigen(n);
    RequireCloseEigen(Math::Reflect(v, n), Eigen::Vector3f(ev - 2.0f * ev.dot(en) * en));
}

TEST_CASE("Clamp matches Eigen cwiseMin/cwiseMax composition", AUTO_TAG) {
    vec3 v{-1,0.5f,9}, lo{0,0,0}, hi{1,1,1};
    auto ev = ToEigen(v), elo = ToEigen(lo), ehi = ToEigen(hi);
    RequireCloseEigen(Math::Clamp(v, lo, hi), Eigen::Vector3f(ev.cwiseMax(elo).cwiseMin(ehi)));
}
