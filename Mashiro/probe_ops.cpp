#include "Mashiro/Geom/GeomOps.h"

using namespace Mashiro;
using namespace Mashiro::Geom;

// 90-degree rotation about +z:  R*(x,y,z) = (-y, x, z)
constexpr Mat<float, 3> RotZ90() {
    Mat<float, 3> m{};
    m[0, 0] = 0; m[1, 0] = 1; m[2, 0] = 0;   // col0 = R*ex = (0,1,0)
    m[0, 1] = -1; m[1, 1] = 0; m[2, 1] = 0;  // col1 = R*ey = (-1,0,0)
    m[0, 2] = 0; m[1, 2] = 0; m[2, 2] = 1;   // col2 = R*ez = (0,0,1)
    return m;
}

// (1) Generic reflection path: pure translation of a Ray.
constexpr Ray TranslatedRay() {
    return Translation(vec3{10, 20, 30}) * Ray{.origin = {1, 1, 1}, .direction = {0, 0, 1}};
}
static_assert(TranslatedRay().origin.x == 11.0f && TranslatedRay().origin.z == 31.0f);
static_assert(TranslatedRay().direction.x == 0.0f && TranslatedRay().direction.z == 1.0f); // direction invariant

// (2) Generic path: rotate a Ray (origin point-law, direction direction-law).
constexpr Ray RotatedRay() {
    return LinearMap(RotZ90()) * Ray{.origin = {1, 0, 0}, .direction = {1, 0, 0}};
}
static_assert(RotatedRay().origin.x == 0.0f && RotatedRay().origin.y == 1.0f);
static_assert(RotatedRay().direction.x == 0.0f && RotatedRay().direction.y == 1.0f);

// (3) Box refit under rotation: [0,1]^3 -> min(-1,0,0) max(0,1,1).
constexpr AABB RotatedBox() {
    return Affine<float, 3>{.linear = RotZ90(), .translation = {}} * AABB{.min = {0, 0, 0}, .max = {1, 1, 1}};
}
static_assert(RotatedBox().min.x == -1.0f && RotatedBox().min.y == 0.0f && RotatedBox().min.z == 0.0f);
static_assert(RotatedBox().max.x == 0.0f && RotatedBox().max.y == 1.0f && RotatedBox().max.z == 1.0f);

// (4) Ball under rigid motion: radius preserved, centre rigidly moved.
constexpr Sphere MovedSphere() {
    return Rigid<float, 3>{.rotation = RotZ90(), .translation = {10, 0, 0}}
         * Sphere{.center = {1, 2, 3}, .radius = 2.5f};
}
static_assert(MovedSphere().center.x == 8.0f && MovedSphere().center.y == 1.0f && MovedSphere().center.z == 3.0f);
static_assert(MovedSphere().radius == 2.5f);

// (5) Plane transform: rotate plane y>=0 (normal (0,1,0), dist 0) by RotZ90 -> normal (-1,0,0).
constexpr Plane RotatedPlane() {
    return Rigid<float, 3>{.rotation = RotZ90(), .translation = {}} * Plane{.normal = {0, 1, 0}, .dist = 0};
}
static_assert(RotatedPlane().normal.x == -1.0f && RotatedPlane().normal.y == 0.0f);

// (6) Composition: (T2 ∘ T1) applied equals T2(T1(.)).
constexpr Ray ComposedRay() {
    auto t1 = Translation(vec3{1, 0, 0});
    auto t2 = Translation(vec3{0, 5, 0});
    return (t2 * t1) * Ray{.origin = {0, 0, 0}, .direction = {1, 0, 0}};
}
static_assert(ComposedRay().origin.x == 1.0f && ComposedRay().origin.y == 5.0f);

// (7) Concept sanity.
static_assert(SpatialTransform<Affine<float, 3>>);
static_assert(SpatialTransform<Rigid<double, 2>>);
static_assert(Decomposable<Ray> && !Decomposable<AABB> && !Decomposable<Sphere>);

int main() { return 0; }
