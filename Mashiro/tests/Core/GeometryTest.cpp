/**
 * @file GeometryTest.cpp
 * @brief Tests for GeometryUtils.h: AABB, Ray, Frustum operations.
 */
#include "Mashiro/Geom/GeometryUtils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace Mashiro;
using namespace Mashiro::Geom;
using Catch::Approx;

namespace {
    constexpr AABB kUnit{.min={.x=-1,.y=-1,.z=-1}, .max={.x=1,.y=1,.z=1}};
    constexpr AABB kShift{.min={.x=0,.y=0,.z=0}, .max={.x=2,.y=2,.z=2}};
    constexpr AABB kFar{.min={.x=10,.y=10,.z=10}, .max={.x=20,.y=20,.z=20}};
}

// ===========================================================================
// AABB queries — constexpr
// ===========================================================================

TEST_CASE("Union encloses both inputs", "[Core.Geometry]") {
    constexpr auto u = Union(kUnit, kShift);
    STATIC_REQUIRE(u.min.x == -1.0f);
    STATIC_REQUIRE(u.max.x == 2.0f);
    STATIC_REQUIRE(u.min.y == -1.0f);
    STATIC_REQUIRE(u.max.y == 2.0f);
}

TEST_CASE("SurfaceArea of unit cube is 24", "[Core.Geometry]") {
    STATIC_REQUIRE(SurfaceArea(kUnit) == 24.0f);
}

TEST_CASE("Centroid of symmetric AABB is origin", "[Core.Geometry]") {
    constexpr auto c = Centroid(kUnit);
    STATIC_REQUIRE(c.x == 0.0f);
    STATIC_REQUIRE(c.y == 0.0f);
    STATIC_REQUIRE(c.z == 0.0f);
}

TEST_CASE("Extents of unit cube are 1", "[Core.Geometry]") {
    constexpr auto e = Extents(kUnit);
    STATIC_REQUIRE(e.x == 1.0f);
    STATIC_REQUIRE(e.y == 1.0f);
}

TEST_CASE("LargestAxis finds the dominant dimension", "[Core.Geometry]") {
    STATIC_REQUIRE(LargestAxis(AABB{.min={}, .max={.x=1,.y=3,.z=2}}) == 1);
    STATIC_REQUIRE(LargestAxis(AABB{.min={}, .max={.x=5,.y=3,.z=2}}) == 0);
    STATIC_REQUIRE(LargestAxis(AABB{.min={}, .max={.x=1,.y=1,.z=9}}) == 2);
}

// ===========================================================================
// Overlap / containment / distance — constexpr
// ===========================================================================

TEST_CASE("Intersects detects overlapping AABBs", "[Core.Geometry]") {
    STATIC_REQUIRE(Intersects(kUnit, kShift));
    STATIC_REQUIRE(!Intersects(kUnit, kFar));
}

TEST_CASE("Contains detects interior points", "[Core.Geometry]") {
    STATIC_REQUIRE(Contains(kUnit, vec3{}));
    STATIC_REQUIRE(Contains(kUnit, vec3{.x=1,.y=1,.z=1})); // boundary inclusive
    STATIC_REQUIRE(!Contains(kUnit, vec3{.x=2,.y=0,.z=0}));
}

TEST_CASE("DistanceSq is zero inside, correct outside", "[Core.Geometry]") {
    STATIC_REQUIRE(DistanceSq(vec3{}, kUnit) == 0.0f);
    STATIC_REQUIRE(DistanceSq(vec3{.x=3,.y=0,.z=0}, kUnit) == 4.0f); // dist=2
    // corner distance
    constexpr float d = DistanceSq(vec3{.x=2,.y=2,.z=2}, kUnit);
    STATIC_REQUIRE(d == 3.0f); // sqrt(1+1+1)^2 = 3
}

// ===========================================================================
// Frustum culling
// ===========================================================================

TEST_CASE("Frustum: box outside one plane is rejected", "[Core.Geometry]") {
    FrustumPlanes fp{};
    // One plane: x >= 5 (normal = {1,0,0,0}, w = -5)
    fp.planes[0] = vec4{.x=1,.y=0,.z=0,.w=-5};
    // Other planes pass everything
    for (int i = 1; i < 6; ++i) fp.planes[i] = vec4{.x=0,.y=0,.z=0,.w=1};

    REQUIRE(!Intersects(kUnit, fp)); // unit box max.x=1 < 5
    REQUIRE(Intersects(kFar, fp));   // far box min.x=10 > 5
}

// ===========================================================================
// Ray precomputation + intersection — constexpr
// ===========================================================================

TEST_CASE("Prepare computes correct invDir and dirSign", "[Core.Geometry]") {
    constexpr Ray r{.origin={}, .direction={.x=2,.y=-4,.z=0.5f}};
    constexpr auto p = Prepare(r);
    REQUIRE(p.invDir.x == Approx(0.5f));
    REQUIRE(p.invDir.y == Approx(-0.25f));
    REQUIRE(p.invDir.z == Approx(2.0f));
    REQUIRE(p.dirSign[0] == 0u);
    REQUIRE(p.dirSign[1] == 1u);
    REQUIRE(p.dirSign[2] == 0u);
}

TEST_CASE("Prepare handles near-zero direction safely", "[Core.Geometry]") {
    constexpr Ray r{.origin={}, .direction={.x=0,.y=0,.z=1}};
    constexpr auto p = Prepare(r);
    REQUIRE(Math::Abs(p.invDir.x) >= 1e19f); // clamped to ±1e20
    REQUIRE(Math::Abs(p.invDir.y) >= 1e19f);
    REQUIRE(p.invDir.z == Approx(1.0f));
}

TEST_CASE("Intersect(Ray, AABB) hits the unit cube from -Z", "[Core.Geometry]") {
    constexpr Ray r{.origin={.x=0,.y=0,.z=-5}, .direction={.x=0,.y=0,.z=1}};
    constexpr auto result = Intersect(r, kUnit);
    STATIC_REQUIRE(result.first == 4.0f);
    STATIC_REQUIRE(result.second == 6.0f);
}

TEST_CASE("Intersect reports miss for ray parallel to box", "[Core.Geometry]") {
    constexpr auto result = Intersect(
        Ray{.origin={.x=5,.y=0,.z=-5}, .direction={.x=0,.y=0,.z=1}}, kUnit);
    REQUIRE(result.first > result.second); // miss
}

TEST_CASE("Intersect with precomputed ray matches convenience overload", "[Core.Geometry]") {
    Ray r{.origin={.x=0.5f,.y=0.5f,.z=-3}, .direction={.x=0,.y=0,.z=1}};
    auto r1 = Intersect(r, kUnit);
    auto r2 = Intersect(Prepare(r), kUnit);
    REQUIRE(r1.first == Approx(r2.first));
    REQUIRE(r1.second == Approx(r2.second));
}

TEST_CASE("Intersect: origin inside box gives tmin=0", "[Core.Geometry]") {
    constexpr auto result = Intersect(
        Ray{.origin={}, .direction={.x=1,.y=0,.z=0}}, kUnit);
    STATIC_REQUIRE(result.first == 0.0f);
    REQUIRE(result.second == Approx(1.0f));
}
