/**
 * @file GeometryUtils.h
 * @brief Spatial query primitives: AABB, Ray, Frustum operations.
 *
 * All functions are `constexpr` and pass small POD types by value for
 * register-friendly calling convention. Lives in `Mashiro::Geom`.
 * Type names are omitted from function names because overload resolution
 * on the parameter types is unambiguous.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/Types.h"
#include "Mashiro/Math/MathUtils.h"

#include <limits>
#include <utility>

namespace Mashiro {

    /** @brief Axis index enumeration for readability. */
    enum class Axis : int { X = 0, Y = 1, Z = 2 };

    /// @name Geometry types
    /// @{

    /** @brief Axis-aligned bounding box (32 B, align 16). */
    struct alignas(16) AABB {
        vec3 min = {}; ///< Corner with smallest coordinates.
        vec3 max = {}; ///< Corner with largest coordinates.
    };
    static_assert(sizeof(AABB) == 32);
    static_assert(alignof(AABB) == 16);

    /** @brief Alias for AABB, commonly used in spatial queries. */
    using BoundingBox = AABB;

    /** @brief Sphere: packed center (vec3b) + radius, 16 bytes, align 16. */
    struct alignas(16) Sphere {
        vec3b center = {}; ///< Sphere centre (packed, no padding).
        float radius = {}; ///< Sphere radius.
    };
    static_assert(sizeof(Sphere) == 16);
    static_assert(alignof(Sphere) == 16);

    /** @brief Alias for Sphere, commonly used in spatial queries. */
    using BoundingSphere = Sphere;

    /** @brief Six frustum clipping planes: left, right, bottom, top, near, far. */
    struct alignas(16) FrustumPlanes {
        vec4 planes[6] = {}; ///< `dot(normal, point) + w >= 0` → inside.
    };
    static_assert(sizeof(FrustumPlanes) == 96);
    static_assert(alignof(FrustumPlanes) == 16);

    /** @brief Ray defined by an origin point and a direction vector. */
    struct alignas(16) Ray {
        vec3 origin = {};    ///< Ray start point.
        vec3 direction = {}; ///< Ray direction (not necessarily unit-length).
    };
    static_assert(sizeof(Ray) == 32);
    static_assert(alignof(Ray) == 16);

    /** @brief Plane defined by normal + distance from origin.
     *
     * A 3-float normal (vec3b, no padding) plus the distance fill exactly 16 bytes
     * while maintaining alignas(16).
     */
    struct alignas(16) Plane {
        vec3b normal = {}; ///< Unit outward normal.
        float dist = {};   ///< Signed distance from origin along @c normal.
    };

    /// @}
    static_assert(sizeof(Plane) == 16);
    static_assert(alignof(Plane) == 16);

    // =========================================================================
    // 1D / 2D primitives
    // =========================================================================

    /** @brief 1D interval [lo, hi]. Fundamental building block for slab tests. */
    struct Interval {
        float lo = 0.0f; ///< Lower bound (inclusive).
        float hi = 0.0f; ///< Upper bound (inclusive).

        friend constexpr bool operator==(const Interval&, const Interval&) = default;
    };

    /** @brief 2D axis-aligned bounding box. */
    struct Box2D {
        vec2 min = {}; ///< Corner with smallest coordinates.
        vec2 max = {}; ///< Corner with largest coordinates.

        friend constexpr bool operator==(const Box2D&, const Box2D&) = default;
    };

    /** @brief 2D circle (center + radius). */
    struct Circle {
        vec2 center = {}; ///< Circle center.
        float radius = 0.0f; ///< Circle radius.

        friend constexpr bool operator==(const Circle&, const Circle&) = default;
    };

    namespace Geom {

        // =====================================================================
        // Interval operations
        // =====================================================================

        /// @brief Length of interval (hi - lo).
        [[nodiscard]] constexpr float Length(Interval i) { return i.hi - i.lo; }

        /// @brief Midpoint of interval.
        [[nodiscard]] constexpr float Midpoint(Interval i) { return (i.lo + i.hi) * 0.5f; }

        /// @brief Test if a value lies within [lo, hi].
        [[nodiscard]] constexpr bool Contains(Interval i, float v) { return v >= i.lo && v <= i.hi; }

        /// @brief Test if two intervals overlap.
        [[nodiscard]] constexpr bool Intersects(Interval a, Interval b) {
            return a.lo <= b.hi && b.lo <= a.hi;
        }

        /// @brief Union of two intervals (smallest interval enclosing both).
        [[nodiscard]] constexpr Interval Union(Interval a, Interval b) {
            return {.lo = Math::Min(a.lo, b.lo), .hi = Math::Max(a.hi, b.hi)};
        }

        /// @brief Grow interval to include a value.
        [[nodiscard]] constexpr Interval Union(Interval i, float v) {
            return {.lo = Math::Min(i.lo, v), .hi = Math::Max(i.hi, v)};
        }

        /// @brief Intersection of two intervals (overlap region). Empty if no overlap.
        [[nodiscard]] constexpr Interval Intersection(Interval a, Interval b) {
            float lo = Math::Max(a.lo, b.lo);
            float hi = Math::Min(a.hi, b.hi);
            return (lo <= hi) ? Interval{lo, hi} : Interval{0, 0};
        }

        /// @brief Expand interval by margin on both sides.
        [[nodiscard]] constexpr Interval Expand(Interval i, float margin) {
            return {.lo = i.lo - margin, .hi = i.hi + margin};
        }

        /// @brief Clamp a value to interval bounds.
        [[nodiscard]] constexpr float Clamp(float v, Interval i) {
            return Math::Clamp(v, i.lo, i.hi);
        }

        // =====================================================================
        // Box2D operations
        // =====================================================================

        /// @brief Smallest Box2D enclosing both inputs.
        [[nodiscard]] constexpr Box2D Union(Box2D a, Box2D b) {
            return {.min = Math::Min(a.min, b.min), .max = Math::Max(a.max, b.max)};
        }

        /// @brief Grow Box2D to include a point.
        [[nodiscard]] constexpr Box2D Union(Box2D box, vec2 p) {
            return {.min = Math::Min(box.min, p), .max = Math::Max(box.max, p)};
        }

        /// @brief Area of a 2D box.
        [[nodiscard]] constexpr float Area(Box2D box) {
            vec2 d = box.max - box.min;
            return d.x * d.y;
        }

        /// @brief Perimeter of a 2D box.
        [[nodiscard]] constexpr float Perimeter(Box2D box) {
            vec2 d = box.max - box.min;
            return 2.0f * (d.x + d.y);
        }

        /// @brief Centroid of a 2D box.
        [[nodiscard]] constexpr vec2 Centroid(Box2D box) {
            return (box.min + box.max) * 0.5f;
        }

        /// @brief Half-extents of a 2D box.
        [[nodiscard]] constexpr vec2 Extents(Box2D box) {
            return (box.max - box.min) * 0.5f;
        }

        /// @brief Test if two Box2D overlap.
        [[nodiscard]] constexpr bool Intersects(Box2D a, Box2D b) {
            return a.min.x <= b.max.x && a.max.x >= b.min.x &&
                   a.min.y <= b.max.y && a.max.y >= b.min.y;
        }

        /// @brief Test if a point is inside a Box2D.
        [[nodiscard]] constexpr bool Contains(Box2D box, vec2 p) {
            return p.x >= box.min.x && p.x <= box.max.x &&
                   p.y >= box.min.y && p.y <= box.max.y;
        }

        /// @brief Intersection region of two Box2D. Returns zero-area box if no overlap.
        [[nodiscard]] constexpr Box2D Intersection(Box2D a, Box2D b) {
            vec2 lo = Math::Max(a.min, b.min);
            vec2 hi = Math::Min(a.max, b.max);
            if (lo.x > hi.x || lo.y > hi.y) return {};
            return {.min = lo, .max = hi};
        }

        /// @brief Expand Box2D by margin on all sides.
        [[nodiscard]] constexpr Box2D Expand(Box2D box, float margin) {
            return {.min = box.min - vec2{margin, margin}, .max = box.max + vec2{margin, margin}};
        }

        /// @brief Squared distance from point to nearest point on Box2D. Zero if inside.
        [[nodiscard]] constexpr float DistanceSq(vec2 p, Box2D box) {
            vec2 below = box.min - p;
            vec2 above = p - box.max;
            vec2 d = Math::Max(Math::Max(below, above), vec2{});
            return Math::Dot(d, d);
        }

        /// @brief Closest point on (or inside) Box2D to a given point.
        [[nodiscard]] constexpr vec2 ClosestPoint(Box2D box, vec2 p) {
            return Math::Clamp(p, box.min, box.max);
        }

        // =====================================================================
        // Circle operations
        // =====================================================================

        /// @brief Test if a point is inside a circle.
        [[nodiscard]] constexpr bool Contains(Circle c, vec2 p) {
            return Math::DistanceSq(c.center, p) <= c.radius * c.radius;
        }

        /// @brief Test if two circles overlap.
        [[nodiscard]] constexpr bool Intersects(Circle a, Circle b) {
            float rSum = a.radius + b.radius;
            return Math::DistanceSq(a.center, b.center) <= rSum * rSum;
        }

        /// @brief Smallest circle enclosing both (conservative: Ritter-style).
        [[nodiscard]] constexpr Circle Union(Circle a, Circle b) {
            vec2 diff = b.center - a.center;
            float dist = Math::Norm(diff);
            if (dist + b.radius <= a.radius) return a; // b inside a
            if (dist + a.radius <= b.radius) return b; // a inside b
            float newRadius = (dist + a.radius + b.radius) * 0.5f;
            vec2 newCenter = a.center + diff * ((newRadius - a.radius) / dist);
            return {.center = newCenter, .radius = newRadius};
        }

        /// @brief Compute the axis-aligned bounding box of a circle.
        [[nodiscard]] constexpr Box2D Bounds(Circle c) {
            vec2 r{c.radius, c.radius};
            return {.min = c.center - r, .max = c.center + r};
        }

        // =====================================================================
        // AABB additional operations
        // =====================================================================

        /// @brief Smallest AABB enclosing both inputs.
        [[nodiscard]] constexpr AABB Union(AABB a, AABB b) {
            return {.min = Math::Min(a.min, b.min), .max = Math::Max(a.max, b.max)};
        }

        /// @brief Grow AABB to include a point.
        [[nodiscard]] constexpr AABB Union(AABB box, vec3 p) {
            return {.min = Math::Min(box.min, p), .max = Math::Max(box.max, p)};
        }

        /// @brief Intersection region of two AABBs. Returns zero-volume box if no overlap.
        [[nodiscard]] constexpr AABB Intersection(AABB a, AABB b) {
            vec3 lo = Math::Max(a.min, b.min);
            vec3 hi = Math::Min(a.max, b.max);
            if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z) return {};
            return {.min = lo, .max = hi};
        }

        /// @brief Expand AABB by margin on all sides.
        [[nodiscard]] constexpr AABB Expand(AABB box, float margin) {
            vec3 m{margin, margin, margin};
            return {.min = box.min - m, .max = box.max + m};
        }

        /// @brief Volume of an AABB.
        [[nodiscard]] constexpr float Volume(AABB box) {
            vec3 d = box.max - box.min;
            return d.x * d.y * d.z;
        }

        /// @brief Closest point on (or inside) AABB to a given point.
        [[nodiscard]] constexpr vec3 ClosestPoint(AABB box, vec3 p) {
            return Math::Clamp(p, box.min, box.max);
        }

        // =====================================================================
        // Sphere operations
        // =====================================================================

        /// @brief Test if a point is inside a sphere.
        [[nodiscard]] constexpr bool Contains(Sphere s, vec3 p) {
            vec3 d = p - vec3{s.center.x, s.center.y, s.center.z};
            return Math::Dot(d, d) <= s.radius * s.radius;
        }

        /// @brief Test if two spheres overlap.
        [[nodiscard]] constexpr bool Intersects(Sphere a, Sphere b) {
            vec3 ca{a.center.x, a.center.y, a.center.z};
            vec3 cb{b.center.x, b.center.y, b.center.z};
            float rSum = a.radius + b.radius;
            return Math::DistanceSq(ca, cb) <= rSum * rSum;
        }

        // Intersects(Sphere, AABB) is defined after DistanceSq(vec3, AABB) below.

        // =====================================================================
        // Plane operations
        // =====================================================================

        /// @brief Signed distance from a point to a plane.
        [[nodiscard]] constexpr float SignedDistance(Plane pl, vec3 p) {
            return Math::Dot(vec3{pl.normal.x, pl.normal.y, pl.normal.z}, p) + pl.dist;
        }

        /// @brief Which side of the plane a point is on (+1 front, -1 back, 0 on plane).
        [[nodiscard]] constexpr int Side(Plane pl, vec3 p) {
            float d = SignedDistance(pl, p);
            return (d > 0.0f) ? 1 : (d < 0.0f ? -1 : 0);
        }

        // =====================================================================
        // AABB queries (original)
        // =====================================================================

        /** @brief Surface area (used by SAH cost evaluation). */
        [[nodiscard]] constexpr float SurfaceArea(AABB box) {
            vec3 d = box.max - box.min;
            return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
        }

        /** @brief Centroid (midpoint). */
        [[nodiscard]] constexpr vec3 Centroid(AABB box) {
            return (box.min + box.max) * 0.5f;
        }

        /** @brief Half-extents per axis. */
        [[nodiscard]] constexpr vec3 Extents(AABB box) {
            return (box.max - box.min) * 0.5f;
        }

        /** @brief Index of the largest extent axis (0 = x, 1 = y, 2 = z). */
        [[nodiscard]] constexpr int LargestAxis(AABB box) {
            vec3 d = box.max - box.min;
            return (d.x >= d.y && d.x >= d.z) ? 0 : (d.y >= d.z ? 1 : 2);
        }

        // =====================================================================
        // Overlap / containment / distance
        // =====================================================================

        /** @brief Test if two AABBs overlap (inclusive boundary). */
        [[nodiscard]] constexpr bool Intersects(AABB a, AABB b) {
            return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y &&
                   a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
        }

        /** @brief Test if a point lies inside an AABB (inclusive). */
        [[nodiscard]] constexpr bool Contains(AABB box, vec3 p) {
            return p.x >= box.min.x && p.x <= box.max.x && p.y >= box.min.y && p.y <= box.max.y &&
                   p.z >= box.min.z && p.z <= box.max.z;
        }

        /** @brief Squared distance from a point to the nearest surface of an AABB. Zero if inside.
         */
        [[nodiscard]] constexpr float DistanceSq(vec3 p, AABB box) {
            vec3 below = box.min - p; // positive when p < min
            vec3 above = p - box.max; // positive when p > max
            vec3 d = Math::Max(Math::Max(below, above), vec3{});
            return Math::Dot(d, d);
        }

        /// @brief Test if a sphere and AABB overlap.
        [[nodiscard]] constexpr bool Intersects(Sphere s, AABB box) {
            vec3 c{s.center.x, s.center.y, s.center.z};
            return DistanceSq(c, box) <= s.radius * s.radius;
        }

        // =====================================================================
        // Frustum culling
        // =====================================================================

        /**
         * @brief Conservative AABB-vs-frustum test (6 planes).
         *
         * May return `true` for near-misses (false positive), never `false`
         * for a box actually inside. Plane convention: `dot(n, pt) + w >= 0` → inside.
         */
        [[nodiscard]] constexpr bool Intersects(AABB box, FrustumPlanes const& frustum) {
            for (int i = 0; i < 6; ++i) {
                vec4 pl = frustum.planes[i];
                float px = (pl.x >= 0.0f) ? box.max.x : box.min.x;
                float py = (pl.y >= 0.0f) ? box.max.y : box.min.y;
                float pz = (pl.z >= 0.0f) ? box.max.z : box.min.z;
                if (px * pl.x + py * pl.y + pz * pl.z + pl.w < 0.0f) return false;
            }
            return true;
        }

        // =====================================================================
        // Ray precomputation + intersection
        // =====================================================================

        /** @brief Precomputed reciprocal-direction ray for fast BVH slab tests. */
        struct RayPrecomp {
            vec3 origin;         ///< Ray origin.
            vec3 invDir;         ///< Component-wise `1 / direction` (safe for dir ≈ 0).
            uint32_t dirSign[3]; ///< 1 if direction component < 0, 0 otherwise.
        };

        /** @brief Precompute reciprocal direction + sign flags. Call once per ray. */
        [[nodiscard]] constexpr RayPrecomp Prepare(Ray ray) {
            RayPrecomp r;
            r.origin = ray.origin;
            for (int i = 0; i < 3; ++i) {
                float d = ray.direction[i];
                r.invDir[i] = (Math::Abs(d) < 1e-20f) ? Math::Copysign(1e20f, d) : 1.0f / d;
                r.dirSign[i] = (d < 0.0f) ? 1u : 0u;
            }
            return r;
        }

        /**
         * @brief Tavian Barnes 2022 NaN-safe, boundary-inclusive ray–AABB slab test.
         *
         * @return `(tMin, tMax)`. Hit iff `tMin ≤ tMax`. `tMin < 0` ⇒ origin inside box.
         * @see https://tavianator.com/2022/ray_box_boundary.html
         */
        [[nodiscard]] constexpr std::pair<float, float> Intersect(RayPrecomp const& ray, AABB box) {
            float tmin = 0.0f;
            float tmax = std::numeric_limits<float>::infinity();
            for (int i = 0; i < 3; ++i) {
                float t1 = (box.min[i] - ray.origin[i]) * ray.invDir[i];
                float t2 = (box.max[i] - ray.origin[i]) * ray.invDir[i];
                tmin = Math::Min(Math::Max(t1, tmin), Math::Max(t2, tmin));
                tmax = Math::Max(Math::Min(t1, tmax), Math::Min(t2, tmax));
            }
            return {tmin, tmax};
        }

        /** @brief Convenience: intersect a raw Ray with an AABB (prepares internally). */
        [[nodiscard]] constexpr std::pair<float, float> Intersect(Ray ray, AABB box) {
            return Intersect(Prepare(ray), box);
        }

    } // namespace Geom

} // namespace Mashiro
