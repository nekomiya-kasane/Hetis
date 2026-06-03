/**
 * @file GeometryUtils.h
 * @brief Geometry utility functions for AABB, Ray, and Frustum primitives.
 *
 * Header-only, all `inline`. Operates on Mashiro geometry types defined in
 * Types.h. Consumed by BVH, octree, GPU culling, picking, and spatial queries.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/Types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Mashiro {

    /// @name AABB operations
    /// @{

    /** @brief Smallest AABB enclosing both @p a and @p b. */
    [[nodiscard]] inline auto UnionAABB(AABB const& a, AABB const& b) -> AABB {
        return AABB{
            .min = {.x = std::min(a.min.x, b.min.x), .y = std::min(a.min.y, b.min.y), .z = std::min(a.min.z, b.min.z)},
            .max = {.x = std::max(a.max.x, b.max.x), .y = std::max(a.max.y, b.max.y), .z = std::max(a.max.z, b.max.z)}
        };
    }

    /** @brief Surface area of an AABB (used by SAH cost evaluation). */
    [[nodiscard]] inline auto SurfaceArea(AABB const& box) -> float {
        float dx = box.max.x - box.min.x;
        float dy = box.max.y - box.min.y;
        float dz = box.max.z - box.min.z;
        return 2.0f * (dx * dy + dy * dz + dz * dx);
    }

    /** @brief Centroid of an AABB. */
    [[nodiscard]] inline auto Centroid(AABB const& box) -> vec3 {
        return vec3{
            .x = 0.5f * (box.min.x + box.max.x),
            .y = 0.5f * (box.min.y + box.max.y),
            .z = 0.5f * (box.min.z + box.max.z)
        };
    }

    /** @brief Extents (half-size per axis) of an AABB. */
    [[nodiscard]] inline auto Extents(AABB const& box) -> vec3 {
        return vec3{
            .x = 0.5f * (box.max.x - box.min.x),
            .y = 0.5f * (box.max.y - box.min.y),
            .z = 0.5f * (box.max.z - box.min.z)
        };
    }

    /** @brief Test if two AABBs overlap. */
    [[nodiscard]] inline auto AABBIntersectsAABB(AABB const& a, AABB const& b) -> bool {
        return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y
               && a.min.z <= b.max.z && a.max.z >= b.min.z;
    }

    /** @brief Test if a point is inside an AABB (inclusive). */
    [[nodiscard]] inline auto AABBContainsPoint(AABB const& box, vec3 const& p) -> bool {
        return p.x >= box.min.x && p.x <= box.max.x && p.y >= box.min.y && p.y <= box.max.y && p.z >= box.min.z
               && p.z <= box.max.z;
    }

    /// @}

    /// @name Frustum tests
    /// @{

    /**
     * @brief Conservative AABB-vs-frustum test (6 planes).
     *
     * May return `true` for near-misses (false positive), but never `false`
     * for a box that is actually inside the frustum.
     *
     * Plane equation convention: `dot(normal, point) + w >= 0` means *inside*.
     *
     * @param box     Axis-aligned bounding box to test.
     * @param frustum Six clipping planes of the view frustum.
     * @return `true` if the box is (conservatively) inside the frustum.
     */
    [[nodiscard]] inline auto AABBIntersectsFrustum(AABB const& box, FrustumPlanes const& frustum) -> bool {
        for (int i = 0; i < 6; ++i) {
            auto const& p = frustum.planes[i];
            float px = (p.x >= 0.0f) ? box.max.x : box.min.x;
            float py = (p.y >= 0.0f) ? box.max.y : box.min.y;
            float pz = (p.z >= 0.0f) ? box.max.z : box.min.z;
            float dot = px * p.x + py * p.y + pz * p.z + p.w;
            if (dot < 0.0f) {
                return false;
            }
        }
        return true;
    }

    /// @}

    /// @name Point–AABB distance
    /// @{

    /**
     * @brief Squared distance from point @p p to the nearest point on @p box.
     * @return 0 if @p p is inside or on the surface; used by nearest-neighbour queries.
     */
    [[nodiscard]] inline auto PointAABBDistSq(vec3 const& p, AABB const& box) -> float {
        float dx = std::max(std::max(box.min.x - p.x, p.x - box.max.x), 0.0f);
        float dy = std::max(std::max(box.min.y - p.y, p.y - box.max.y), 0.0f);
        float dz = std::max(std::max(box.min.z - p.z, p.z - box.max.z), 0.0f);
        return dx * dx + dy * dy + dz * dz;
    }

    /// @brief Index of the largest extent axis (0 = x, 1 = y, 2 = z).
    [[nodiscard]] inline auto LargestAxis(AABB const& box) -> uint32_t {
        float dx = box.max.x - box.min.x;
        float dy = box.max.y - box.min.y;
        float dz = box.max.z - box.min.z;
        if (dx >= dy && dx >= dz) {
            return 0;
        }
        if (dy >= dz) {
            return 1;
        }
        return 2;
    }

    /// @}

    /// @name Ray precomputation
    /// @{

    /**
     * @brief Precomputed ray data for fast BVH traversal.
     *
     * Compute once per ray at traversal entry with PrepareRay(), then reuse for
     * every node test. `invDir` handles near-zero components via
     * `copysign(1e20f)` — IEEE 754 guarantees correct slab arithmetic.
     */
    struct RayPrecomp {
        vec3     origin;      ///< Ray origin.
        vec3     invDir;      ///< Component-wise `1 / direction` (safe for dir ≈ 0).
        uint32_t dirSign[3];  ///< 1 if direction component < 0, 0 otherwise.
    };

    /** @brief Prepare a ray for fast traversal. O(1), call once per ray. */
    [[nodiscard]] inline auto PrepareRay(Ray const& ray) -> RayPrecomp {
        RayPrecomp r;
        r.origin = ray.origin;
        for (int i = 0; i < 3; ++i) {
            float d = (&ray.direction.x)[i];
            (&r.invDir.x)[i] = (std::abs(d) < 1e-20f) ? std::copysign(1e20f, d) : 1.0f / d;
            r.dirSign[i] = (d < 0.0f) ? 1u : 0u;
        }
        return r;
    }

    /// @}

    /// @name Ray–AABB intersection tests
    /// @{

    /**
     * @brief Tavian Barnes 2022 NaN-safe, boundary-inclusive ray–AABB slab test.
     *
     * Uses precomputed `invDir`. Correctly handles:
     * - Ray direction ≈ 0 (`invDir = ±1e20`, IEEE 754 correct).
     * - Ray exactly on AABB face / edge / corner (boundary-inclusive, `tmin ≤ tmax`).
     * - NaN from `0 × ∞` (pushed into inner min/max; at least one arg is non-NaN).
     *
     * @param ray Precomputed ray (see PrepareRay()).
     * @param box Axis-aligned bounding box.
     * @return `(tMin, tMax)`. Hit iff `tMin ≤ tMax`. `tMin < 0` ⇒ origin inside box.
     *
     * @see https://tavianator.com/2022/ray_box_boundary.html
     */
    [[nodiscard]] inline auto RayAABBFast(RayPrecomp const& ray, AABB const& box) -> std::pair<float, float> {
        float tmin = 0.0f;
        float tmax = std::numeric_limits<float>::infinity();

        for (int i = 0; i < 3; ++i) {
            float orig = (&ray.origin.x)[i];
            float invD = (&ray.invDir.x)[i];
            float bmin = (&box.min.x)[i];
            float bmax = (&box.max.x)[i];

            float t1 = (bmin - orig) * invD;
            float t2 = (bmax - orig) * invD;

            // Tavian Barnes boundary-inclusive NaN-safe formula:
            // tmin = min(max(t1, tmin), max(t2, tmin))
            // tmax = max(min(t1, tmax), min(t2, tmax))
            tmin = std::min(std::max(t1, tmin), std::max(t2, tmin));
            tmax = std::max(std::min(t1, tmax), std::min(t2, tmax));
        }
        return {tmin, tmax};
    }

    /**
     * @brief Convenience ray–AABB test without precomputation.
     * @return `(tMin, tMax)`. If `tMin > tMax`, no intersection.
     *         `tMin < 0` means the ray origin is inside the box.
     */
    [[nodiscard]] inline auto RayAABBIntersect(Ray const& ray, AABB const& box) -> std::pair<float, float> {
        auto precomp = PrepareRay(ray);
        return RayAABBFast(precomp, box);
    }

    /// @}

}  // namespace Mashiro
