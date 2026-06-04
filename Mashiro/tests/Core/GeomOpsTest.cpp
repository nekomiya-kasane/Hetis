/**
 * @file GeomOpsTest.cpp
 * @brief Tests for GeomOps.h: reflection/annotation-driven affine transforms of primitives.
 */
#include "Mashiro/Geom/GeomOps.h"

#include <catch2/catch_approx.hpp>
#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Mashiro;
using namespace Mashiro::Geom;
using Catch::Approx;

namespace {
    // 90-degree rotation about +z:  R*(x,y,z) = (-y, x, z).
    constexpr Mat<float, 3> RotZ90() {
        Mat<float, 3> m{};
        m[0, 0] = 0;  m[1, 0] = 1; m[2, 0] = 0;
        m[0, 1] = -1; m[1, 1] = 0; m[2, 1] = 0;
        m[0, 2] = 0;  m[1, 2] = 0; m[2, 2] = 1;
        return m;
    }
    // Shear x += 0.5*y (non-orthogonal, det = 1).
    constexpr Mat<float, 3> ShearXY() {
        Mat<float, 3> m = Math::Identity<Mat<float, 3>>();
        m[0, 1] = 0.5f;
        return m;
    }
    constexpr Mat<float, 3> Scale(float sx, float sy, float sz) {
        Mat<float, 3> m{};
        m[0, 0] = sx; m[1, 1] = sy; m[2, 2] = sz;
        return m;
    }
}

// ===========================================================================
// Generic reflection-driven path (Ray) — dispatch by Quantity annotation
// ===========================================================================

TEST_CASE("Translation moves a Ray's origin but leaves its direction invariant", AUTO_TAG) {
    constexpr Ray r = Translation(vec3{10, 20, 30}) * Ray{.origin = {1, 1, 1}, .direction = {0, 0, 1}};
    STATIC_REQUIRE(r.origin.x == 11.0f);
    STATIC_REQUIRE(r.origin.y == 21.0f);
    STATIC_REQUIRE(r.origin.z == 31.0f);
    STATIC_REQUIRE(r.direction.x == 0.0f); // Direction quantity ignores translation
    STATIC_REQUIRE(r.direction.z == 1.0f);
}

TEST_CASE("Rotation acts on both Point and Direction members of a Ray", AUTO_TAG) {
    constexpr Ray r = LinearMap(RotZ90()) * Ray{.origin = {1, 0, 0}, .direction = {1, 0, 0}};
    STATIC_REQUIRE(r.origin.x == 0.0f);
    STATIC_REQUIRE(r.origin.y == 1.0f);
    STATIC_REQUIRE(r.direction.x == 0.0f);
    STATIC_REQUIRE(r.direction.y == 1.0f);
}

// ===========================================================================
// Box: corner re-fit (correct for any affine)
// ===========================================================================

TEST_CASE("Box re-fits its axis-aligned bounds under rotation", AUTO_TAG) {
    constexpr AABB b = Affine<float, 3>{.linear = RotZ90()} * AABB{.min = {0, 0, 0}, .max = {1, 1, 1}};
    STATIC_REQUIRE(b.min.x == -1.0f);
    STATIC_REQUIRE(b.min.y == 0.0f);
    STATIC_REQUIRE(b.max.x == 0.0f);
    STATIC_REQUIRE(b.max.y == 1.0f);
    STATIC_REQUIRE(b.max.z == 1.0f);
}

// ===========================================================================
// Ball: centre affine, radius scaled
// ===========================================================================

TEST_CASE("Ball under rigid motion preserves its radius", AUTO_TAG) {
    constexpr Sphere s =
        Rigid<float, 3>{.rotation = RotZ90(), .translation = {10, 0, 0}} * Sphere{.center = {1, 2, 3}, .radius = 2.5f};
    STATIC_REQUIRE(s.center.x == 8.0f); // R*(1,2,3) = (-2,1,3); +t = (8,1,3)
    STATIC_REQUIRE(s.center.y == 1.0f);
    STATIC_REQUIRE(s.center.z == 3.0f);
    STATIC_REQUIRE(s.radius == 2.5f);
}

TEST_CASE("Ball radius scale is exact under uniform scale", AUTO_TAG) {
    // For A = diag(2,2,2): ||A||_1 = ||A||_inf = 2, so the spectral bound = sqrt(4) = 2 exactly.
    constexpr Sphere s = Affine<float, 3>{.linear = Scale(2, 2, 2)} * Sphere{.center = {1, 0, 0}, .radius = 3.0f};
    STATIC_REQUIRE(s.center.x == 2.0f);
    STATIC_REQUIRE(s.radius == 6.0f);
}

TEST_CASE("Ball radius bound stays conservative (encloses) under non-uniform scale", AUTO_TAG) {
    // True image of unit sphere under diag(3,1,1) is an ellipsoid with max semi-axis 3.
    const Sphere s = Affine<float, 3>{.linear = Scale(3, 1, 1)} * Sphere{.center = {0, 0, 0}, .radius = 1.0f};
    REQUIRE(s.radius >= 3.0f); // must enclose the longest axis
}

// ===========================================================================
// Hyperplane: homogeneous covector law (inverse-transpose + incidence)
// ===========================================================================

TEST_CASE("Plane normal uses the inverse-transpose law (orthogonality preserved under shear)", AUTO_TAG) {
    // Plane x = 0 (normal +x). Direction (0,1,0) lies in the plane.
    const Plane pl = Affine<float, 3>{.linear = ShearXY()} * Plane{.normal = {1, 0, 0}, .dist = 0};
    const vec3  dT = ShearXY() * vec3{0, 1, 0}; // transformed in-plane direction
    // n' must remain perpendicular to the transformed in-plane direction.
    REQUIRE((pl.normal.x * dT.x + pl.normal.y * dT.y + pl.normal.z * dT.z) == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("Plane incidence is preserved by an affine transform", AUTO_TAG) {
    const Affine<float, 3> x{.linear = ShearXY(), .translation = {3, 4, 5}};
    const Plane            pl = x * Plane{.normal = {1, 0, 0}, .dist = 0}; // plane through origin, x = 0
    // Two points on the original plane (x = 0) must lie on the transformed plane.
    const vec3 p0 = x.TransformPoint(vec3{0, 0, 0});
    const vec3 p1 = x.TransformPoint(vec3{0, 1, 2});
    REQUIRE(SignedDistance(pl, p0) == Approx(0.0f).margin(1e-5f));
    REQUIRE(SignedDistance(pl, p1) == Approx(0.0f).margin(1e-5f));
}

// ===========================================================================
// Frustum: per-plane covector transform
// ===========================================================================

TEST_CASE("Frustum planes shift their offset under translation", AUTO_TAG) {
    // planes[0] = (1,0,0,1): x >= -1 inside. Translate +2x -> x >= 1, i.e. (1,0,0,-1).
    Frustum f{};
    f.planes[0] = vec4{1, 0, 0, 1};
    const Frustum t = Rigid<float, 3>{.translation = {2, 0, 0}} * f;
    REQUIRE(t.planes[0].x == Approx(1.0f));
    REQUIRE(t.planes[0].w == Approx(-1.0f));
}

// ===========================================================================
// Composition and concepts
// ===========================================================================

TEST_CASE("Affine composition equals sequential application", AUTO_TAG) {
    constexpr auto t1 = Translation(vec3{1, 0, 0});
    constexpr auto t2 = Translation(vec3{0, 5, 0});
    constexpr Ray  r  = (t2 * t1) * Ray{.origin = {0, 0, 0}, .direction = {1, 0, 0}};
    STATIC_REQUIRE(r.origin.x == 1.0f);
    STATIC_REQUIRE(r.origin.y == 5.0f);
}

TEST_CASE("Transform concepts classify types correctly", AUTO_TAG) {
    STATIC_REQUIRE(SpatialTransform<Affine<float, 3>>);
    STATIC_REQUIRE(SpatialTransform<Rigid<double, 2>>);
    STATIC_REQUIRE(Decomposable<Ray>);
    STATIC_REQUIRE_FALSE(Decomposable<AABB>);   // joint invariant: axis-alignment
    STATIC_REQUIRE_FALSE(Decomposable<Sphere>); // joint invariant: radius/centre coupling
}
