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

// ===========================================================================
// Interval operations — constexpr
// ===========================================================================

TEST_CASE("Interval: Length and Midpoint", "[Core.Geometry.Interval]") {
    constexpr Interval iv{.lo = 2.0f, .hi = 8.0f};
    STATIC_REQUIRE(Length(iv) == 6.0f);
    STATIC_REQUIRE(Midpoint(iv) == 5.0f);
}

TEST_CASE("Interval: Contains", "[Core.Geometry.Interval]") {
    constexpr Interval iv{.lo = 0.0f, .hi = 10.0f};
    STATIC_REQUIRE(Contains(iv, 5.0f));
    STATIC_REQUIRE(Contains(iv, 0.0f));
    STATIC_REQUIRE(Contains(iv, 10.0f));
    STATIC_REQUIRE(!Contains(iv, -1.0f));
    STATIC_REQUIRE(!Contains(iv, 11.0f));
}

TEST_CASE("Interval: Intersects", "[Core.Geometry.Interval]") {
    constexpr Interval a{.lo = 0, .hi = 5};
    constexpr Interval b{.lo = 3, .hi = 8};
    constexpr Interval c{.lo = 6, .hi = 9};
    STATIC_REQUIRE(Intersects(a, b));
    STATIC_REQUIRE(!Intersects(a, c));
}

TEST_CASE("Interval: Union and Intersection", "[Core.Geometry.Interval]") {
    constexpr Interval a{.lo = 1, .hi = 5};
    constexpr Interval b{.lo = 3, .hi = 8};
    constexpr auto u = Union(a, b);
    STATIC_REQUIRE(u.lo == 1.0f);
    STATIC_REQUIRE(u.hi == 8.0f);
    constexpr auto i = Intersection(a, b);
    STATIC_REQUIRE(i.lo == 3.0f);
    STATIC_REQUIRE(i.hi == 5.0f);
}

TEST_CASE("Interval: Union with point", "[Core.Geometry.Interval]") {
    constexpr Interval iv{.lo = 2, .hi = 4};
    constexpr auto grown = Union(iv, 7.0f);
    STATIC_REQUIRE(grown.lo == 2.0f);
    STATIC_REQUIRE(grown.hi == 7.0f);
}

TEST_CASE("Interval: Expand and Clamp", "[Core.Geometry.Interval]") {
    constexpr Interval iv{.lo = 2, .hi = 4};
    constexpr auto exp = Expand(iv, 1.0f);
    STATIC_REQUIRE(exp.lo == 1.0f);
    STATIC_REQUIRE(exp.hi == 5.0f);
    STATIC_REQUIRE(Clamp(0.0f, iv) == 2.0f);
    STATIC_REQUIRE(Clamp(3.0f, iv) == 3.0f);
    STATIC_REQUIRE(Clamp(9.0f, iv) == 4.0f);
}

// ===========================================================================
// Box2D operations — constexpr
// ===========================================================================

TEST_CASE("Box2D: Union of two boxes", "[Core.Geometry.Box2D]") {
    constexpr Box2D a{.min={.x=0,.y=0}, .max={.x=2,.y=2}};
    constexpr Box2D b{.min={.x=1,.y=1}, .max={.x=4,.y=3}};
    constexpr auto u = Union(a, b);
    STATIC_REQUIRE(u.min.x == 0.0f);
    STATIC_REQUIRE(u.max.x == 4.0f);
    STATIC_REQUIRE(u.max.y == 3.0f);
}

TEST_CASE("Box2D: Union with point", "[Core.Geometry.Box2D]") {
    constexpr Box2D box{.min={.x=0,.y=0}, .max={.x=1,.y=1}};
    constexpr auto grown = Union(box, vec2{5.0f, -1.0f});
    STATIC_REQUIRE(grown.min.y == -1.0f);
    STATIC_REQUIRE(grown.max.x == 5.0f);
}

TEST_CASE("Box2D: Area and Perimeter", "[Core.Geometry.Box2D]") {
    constexpr Box2D box{.min={.x=0,.y=0}, .max={.x=3,.y=4}};
    STATIC_REQUIRE(Area(box) == 12.0f);
    STATIC_REQUIRE(Perimeter(box) == 14.0f);
}

TEST_CASE("Box2D: Contains and Intersects", "[Core.Geometry.Box2D]") {
    constexpr Box2D a{.min={.x=0,.y=0}, .max={.x=4,.y=4}};
    constexpr Box2D b{.min={.x=3,.y=3}, .max={.x=6,.y=6}};
    constexpr Box2D c{.min={.x=5,.y=5}, .max={.x=7,.y=7}};
    STATIC_REQUIRE(Contains(a, vec2{2.0f, 2.0f}));
    STATIC_REQUIRE(!Contains(a, vec2{5.0f, 5.0f}));
    STATIC_REQUIRE(Intersects(a, b));
    STATIC_REQUIRE(!Intersects(a, c));
}

TEST_CASE("Box2D: Intersection region", "[Core.Geometry.Box2D]") {
    constexpr Box2D a{.min={.x=0,.y=0}, .max={.x=4,.y=4}};
    constexpr Box2D b{.min={.x=2,.y=2}, .max={.x=6,.y=6}};
    constexpr auto i = Intersection(a, b);
    STATIC_REQUIRE(i.min.x == 2.0f);
    STATIC_REQUIRE(i.min.y == 2.0f);
    STATIC_REQUIRE(i.max.x == 4.0f);
    STATIC_REQUIRE(i.max.y == 4.0f);
}

// ===========================================================================
// Circle operations — constexpr
// ===========================================================================

TEST_CASE("Circle: Contains point", "[Core.Geometry.Circle]") {
    constexpr Circle c{.center={.x=0,.y=0}, .radius=5.0f};
    STATIC_REQUIRE(Contains(c, vec2{3.0f, 4.0f}));  // distance = 5, on boundary
    STATIC_REQUIRE(!Contains(c, vec2{4.0f, 4.0f})); // distance > 5
}

TEST_CASE("Circle: Intersects circles", "[Core.Geometry.Circle]") {
    constexpr Circle a{.center={.x=0,.y=0}, .radius=3.0f};
    constexpr Circle b{.center={.x=4,.y=0}, .radius=2.0f};
    constexpr Circle c{.center={.x=10,.y=0}, .radius=1.0f};
    STATIC_REQUIRE(Intersects(a, b));  // distance=4, sum radii=5
    STATIC_REQUIRE(!Intersects(a, c)); // distance=10, sum radii=4
}

TEST_CASE("Circle: Bounds", "[Core.Geometry.Circle]") {
    constexpr Circle c{.center={.x=1,.y=2}, .radius=3.0f};
    constexpr auto bb = Bounds(c);
    STATIC_REQUIRE(bb.min.x == -2.0f);
    STATIC_REQUIRE(bb.min.y == -1.0f);
    STATIC_REQUIRE(bb.max.x == 4.0f);
    STATIC_REQUIRE(bb.max.y == 5.0f);
}

// ===========================================================================
// AABB additional operations — constexpr
// ===========================================================================

TEST_CASE("AABB: Union with point", "[Core.Geometry]") {
    constexpr auto grown = Union(kUnit, vec3{5.0f, 5.0f, 5.0f});
    STATIC_REQUIRE(grown.max.x == 5.0f);
    STATIC_REQUIRE(grown.min.x == -1.0f);
}

TEST_CASE("AABB: Intersection region", "[Core.Geometry]") {
    constexpr auto i = Intersection(kUnit, kShift);
    STATIC_REQUIRE(i.min.x == 0.0f);
    STATIC_REQUIRE(i.max.x == 1.0f);
}

TEST_CASE("AABB: Expand", "[Core.Geometry]") {
    constexpr auto exp = Expand(kUnit, 0.5f);
    STATIC_REQUIRE(exp.min.x == -1.5f);
    STATIC_REQUIRE(exp.max.x == 1.5f);
}

TEST_CASE("AABB: Volume", "[Core.Geometry]") {
    STATIC_REQUIRE(Volume(kUnit) == 8.0f); // 2*2*2
}

TEST_CASE("AABB: ClosestPoint", "[Core.Geometry]") {
    constexpr auto cp = ClosestPoint(kUnit, vec3{5.0f, 0.0f, 0.0f});
    STATIC_REQUIRE(cp.x == 1.0f);
    STATIC_REQUIRE(cp.y == 0.0f);
}

// ===========================================================================
// Sphere operations
// ===========================================================================

TEST_CASE("Sphere: Contains point", "[Core.Geometry.Sphere]") {
    constexpr Sphere s{.center={0, 0, 0}, .radius=2.0f};
    STATIC_REQUIRE(Contains(s, vec3{1, 1, 1}));   // dist ~1.73
    STATIC_REQUIRE(!Contains(s, vec3{2, 2, 0}));  // dist ~2.83
}

TEST_CASE("Sphere: Intersects sphere", "[Core.Geometry.Sphere]") {
    constexpr Sphere a{.center={0, 0, 0}, .radius=2.0f};
    constexpr Sphere b{.center={3, 0, 0}, .radius=2.0f};
    constexpr Sphere c{.center={10, 0, 0}, .radius=1.0f};
    STATIC_REQUIRE(Intersects(a, b));
    STATIC_REQUIRE(!Intersects(a, c));
}

TEST_CASE("Sphere: Intersects AABB", "[Core.Geometry.Sphere]") {
    constexpr Sphere s{.center={3, 0, 0}, .radius=2.5f};
    STATIC_REQUIRE(Intersects(s, kUnit));  // sphere at x=3, reaches x=0.5 which is inside [-1,1]
    constexpr Sphere far{.center={10, 10, 10}, .radius=1.0f};
    STATIC_REQUIRE(!Intersects(far, kUnit));
}

// ===========================================================================
// Plane operations
// ===========================================================================

TEST_CASE("Plane: SignedDistance and Side", "[Core.Geometry.Plane]") {
    constexpr Plane pl{.normal={0, 1, 0}, .dist=0.0f}; // y=0 plane
    STATIC_REQUIRE(SignedDistance(pl, vec3{0, 5, 0}) == 5.0f);
    STATIC_REQUIRE(SignedDistance(pl, vec3{0, -3, 0}) == -3.0f);
    STATIC_REQUIRE(Side(pl, vec3{0, 1, 0}) == 1);
    STATIC_REQUIRE(Side(pl, vec3{0, -1, 0}) == -1);
    STATIC_REQUIRE(Side(pl, vec3{0, 0, 0}) == 0);
}
