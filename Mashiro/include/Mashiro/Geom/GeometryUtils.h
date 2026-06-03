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

    namespace Geom {

        // =====================================================================
        // AABB queries
        // =====================================================================

        /** @brief Smallest AABB enclosing both inputs. */
        [[nodiscard]] constexpr AABB Union(AABB a, AABB b) {
            return {.min = Math::Min(a.min, b.min), .max = Math::Max(a.max, b.max)};
        }

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
