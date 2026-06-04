/**
 * @file Geom.h
 * @brief Dimension- and scalar-generic spatial primitives and query operations.
 *
 * Every primitive is a class template parameterised on its scalar field `T` and
 * spatial dimension `N`:
 *
 *     BoxT<T, N>          axis-aligned box         (N = 2, 3, 4, …)
 *     BallT<T, N>         solid ball / disk        (N = 2, 3, 4, …)
 *     RayT<T, N>          ray                      (N = 2, 3, …)
 *     HyperplaneT<T, N>   affine hyperplane        (line in 2-D, plane in 3-D)
 *     IntervalT<T>        1-D interval             (Vec needs N ≥ 2, so 1-D is its own type)
 *     FrustumT<T>         view frustum (6 planes)  (intrinsically 3-D)
 *
 * The canonical short names are dimension-explicit `float` aliases (`Box3`, `Box2`,
 * `Sphere`, `Circle`, `Ray3`, `Plane`, …); `double` variants carry a `d` prefix
 * (`dBox3`, `dSphere`, …). Legacy names (`AABB`, `Box2D`, `FrustumPlanes`) remain as
 * aliases. The `float` 3-D specialisations reproduce the exact GPU-friendly layout
 * the engine relies on (verified by `static_assert`).
 *
 * **Compile-time metadata (P2996 reflection + P3385 annotations).**
 *   - Each *type* carries a `Geom::Shape{dim, bounded, convex}` annotation. The trait
 *     layer exposes `Geom::Dim<P>`, `IsBounded<P>`, `IsConvex<P>`, `ScalarType<P>`,
 *     and the `Primitive` / `Primitive2D` / `Primitive3D` / `BoundedPrimitive` concepts.
 *   - Each *data member* carries a `Geom::Quantity` annotation (`Point`, `Direction`,
 *     `Normal`, `Scalar`) declaring its transformation character under the affine group.
 *     This is the foundation for the planned reflection-driven `Transform(M, primitive)`
 *     that dispatches each field to its tensor transformation law (see the design notes
 *     accompanying this header). The tags cost nothing at runtime.
 *
 * Dimension-generic measures unify the classic named quantities:
 *   `Measure` = ∏ extentᵢ  (length / area / volume / …); `Area`/`Volume` are wrappers.
 *   `BoundaryMeasure` = 2 ∑ᵢ ∏_{j≠i} extentⱼ (perimeter / surface area / …).
 *
 * All operations are `constexpr`, pass small POD types by value, and fold to constants
 * when their inputs are known. Primitives live in `Mashiro`; operations and the trait
 * layer live in `Mashiro::Geom`.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/Types.h"
#include "Mashiro/Math/MathUtils.h"

#include <concepts>
#include <cstdint>
#include <limits>
#include <meta>
#include <type_traits>
#include <utility>

namespace Mashiro {

    /** @brief Axis index enumeration for readability. */
    enum class Axis : int { X = 0, Y = 1, Z = 2, W = 3 };

    namespace Geom {
        /**
         * @brief Type-level semantic descriptor attached to every primitive via annotation.
         *
         * Carries facts that cannot be inferred from the data layout alone and that
         * downstream generic code dispatches on at compile time.
         */
        struct Shape {
            int  dim;     ///< Spatial dimension the primitive lives in.
            bool bounded; ///< Encloses a finite region (false for `Ray`, `Hyperplane`).
            bool convex;  ///< Represents a convex point set / boundary.
        };

        /**
         * @brief Member-level transformation character under the affine/linear group.
         *
         * Declares how a data member transforms when the primitive is mapped by a
         * matrix `M` (with translation `t`). This is the tensor "valence" of the field;
         * the planned generic `Transform` reads it via reflection and applies the
         * matching law (see header design notes).
         */
        enum class Quantity {
            Point,     ///< Affine position: p ↦ M·p + t.
            Direction, ///< Free/displacement vector: v ↦ M·v (no translation).
            Normal,    ///< Covector: n ↦ (M⁻¹)ᵀ·n (preserves orthogonality).
            Scalar     ///< Length-like scalar: invariant under rigid motion (radius, offset).
        };

        /** @cond INTERNAL */
        namespace Detail {
            /// @brief std430 alignment of a homogeneous 4-slot (= `4 * sizeof(T)`): 16 for float, 32 for double.
            template <std::floating_point T>
            inline constexpr size_t kHomogeneousSlotAlign = 4 * sizeof(T);
        } // namespace Detail
        /** @endcond */
    } // namespace Geom

    // =========================================================================
    // Primitive templates
    // =========================================================================

    /**
     * @brief Axis-aligned bounding box in `N` dimensions.
     *
     * Two opposite corners. Alignment follows the natural `Vec<T,N>` alignment, so the
     * 3-D float specialisation is 32 B / align 16 (two GPU `vec3` slots).
     */
    template <std::floating_point T, int N>
    struct [[=Geom::Shape{N, true, true}]] BoxT {
        [[=Geom::Quantity::Point]] Vec<T, N> min = {}; ///< Corner with smallest coordinates.
        [[=Geom::Quantity::Point]] Vec<T, N> max = {}; ///< Corner with largest coordinates.

        [[nodiscard]] friend constexpr bool operator==(const BoxT&, const BoxT&) = default;
    };

    /**
     * @brief Solid ball (disk in 2-D, ball in 3-D): centre + radius.
     *
     * The centre is *packed* and the type is aligned to one homogeneous 4-slot so the
     * 3-D float specialisation is exactly 16 B / align 16 (centre + radius fill a `vec4`).
     */
    template <std::floating_point T, int N>
    struct [[=Geom::Shape{N, true, true}]] alignas(Geom::Detail::kHomogeneousSlotAlign<T>) BallT {
        [[=Geom::Quantity::Point]]  Vec<T, N, AlignTag::Packed> center = {};   ///< Ball centre.
        [[=Geom::Quantity::Scalar]] T                           radius = T(0); ///< Ball radius.

        [[nodiscard]] friend constexpr bool operator==(const BallT&, const BallT&) = default;
    };

    /** @brief Ray: an origin point and a direction vector (not necessarily unit-length). */
    template <std::floating_point T, int N>
    struct [[=Geom::Shape{N, false, true}]] alignas(Geom::Detail::kHomogeneousSlotAlign<T>) RayT {
        [[=Geom::Quantity::Point]]     Vec<T, N> origin    = {}; ///< Ray start point.
        [[=Geom::Quantity::Direction]] Vec<T, N> direction = {}; ///< Ray direction.

        [[nodiscard]] friend constexpr bool operator==(const RayT&, const RayT&) = default;
    };

    /**
     * @brief Affine hyperplane: unit normal + signed distance from origin.
     *
     * `dot(normal, x) + dist == 0` defines the hyperplane (a line in 2-D, a plane in 3-D).
     * Packed normal + distance fill one homogeneous 4-slot (3-D float: 16 B / align 16).
     */
    template <std::floating_point T, int N>
    struct [[=Geom::Shape{N, false, true}]] alignas(Geom::Detail::kHomogeneousSlotAlign<T>) HyperplaneT {
        [[=Geom::Quantity::Normal]] Vec<T, N, AlignTag::Packed> normal = {};   ///< Unit normal.
        [[=Geom::Quantity::Scalar]] T                           dist   = T(0); ///< Signed offset along @c normal.

        [[nodiscard]] friend constexpr bool operator==(const HyperplaneT&, const HyperplaneT&) = default;
    };

    /** @brief 1-D interval [lo, hi]. (`Vec` requires N ≥ 2, so 1-D is its own scalar type.) */
    template <std::floating_point T>
    struct [[=Geom::Shape{1, true, true}]] IntervalT {
        [[=Geom::Quantity::Point]] T lo = T(0); ///< Lower bound (inclusive).
        [[=Geom::Quantity::Point]] T hi = T(0); ///< Upper bound (inclusive).

        [[nodiscard]] friend constexpr bool operator==(const IntervalT&, const IntervalT&) = default;
    };

    /** @brief View frustum: six clipping planes (left, right, bottom, top, near, far). Intrinsically 3-D. */
    template <std::floating_point T>
    struct [[=Geom::Shape{3, true, true}]] alignas(Geom::Detail::kHomogeneousSlotAlign<T>) FrustumT {
        Vec<T, 4> planes[6] = {}; ///< Each plane: `dot(normal, point) + w >= 0` → inside.

        [[nodiscard]] friend constexpr bool operator==(const FrustumT&, const FrustumT&) = default;
    };

    // =========================================================================
    // Aliases: dimension-explicit canonical names (float) + double variants + legacy
    // =========================================================================

    using Box2   = BoxT<float, 2>;        ///< 2-D float axis-aligned box.
    using Box3   = BoxT<float, 3>;        ///< 3-D float axis-aligned box.
    using Circle = BallT<float, 2>;       ///< 2-D float ball (disk).
    using Sphere = BallT<float, 3>;       ///< 3-D float ball.
    using Ray2   = RayT<float, 2>;        ///< 2-D float ray.
    using Ray3   = RayT<float, 3>;        ///< 3-D float ray.
    using Line2  = HyperplaneT<float, 2>; ///< 2-D float hyperplane (line).
    using Plane  = HyperplaneT<float, 3>; ///< 3-D float hyperplane (plane).
    using Interval = IntervalT<float>;    ///< Float 1-D interval.
    using Frustum  = FrustumT<float>;     ///< Float view frustum.

    using dBox2   = BoxT<double, 2>;        ///< 2-D double axis-aligned box.
    using dBox3   = BoxT<double, 3>;        ///< 3-D double axis-aligned box.
    using dCircle = BallT<double, 2>;       ///< 2-D double ball (disk).
    using dSphere = BallT<double, 3>;       ///< 3-D double ball.
    using dRay2   = RayT<double, 2>;        ///< 2-D double ray.
    using dRay3   = RayT<double, 3>;        ///< 3-D double ray.
    using dLine2  = HyperplaneT<double, 2>; ///< 2-D double hyperplane (line).
    using dPlane  = HyperplaneT<double, 3>; ///< 3-D double hyperplane (plane).
    using dInterval = IntervalT<double>;    ///< Double 1-D interval.
    using dFrustum  = FrustumT<double>;     ///< Double view frustum.

    // Legacy / convenience names.
    using AABB          = Box3;    ///< Legacy: 3-D axis-aligned bounding box.
    using Box2D         = Box2;    ///< Legacy: 2-D axis-aligned box.
    using Ray           = Ray3;    ///< Legacy: 3-D ray.
    using FrustumPlanes = Frustum; ///< Legacy: view frustum (6 planes).
    using BoundingBox    = Box3;   ///< Bounding box used in spatial queries.
    using BoundingSphere = Sphere; ///< Bounding sphere used in spatial queries.

    // GPU layout contract for the 3-D float specialisations.
    static_assert(sizeof(Box3) == 32 && alignof(Box3) == 16);
    static_assert(sizeof(Sphere) == 16 && alignof(Sphere) == 16);
    static_assert(sizeof(Ray3) == 32 && alignof(Ray3) == 16);
    static_assert(sizeof(Plane) == 16 && alignof(Plane) == 16);
    static_assert(sizeof(Frustum) == 96 && alignof(Frustum) == 16);

    namespace Geom {

        // =====================================================================
        // Compile-time trait layer (reflection over annotations)
        // =====================================================================

        /** @cond INTERNAL */
        namespace Detail {
            /// @brief True if @p P is a class carrying a `Geom::Shape` annotation.
            template <typename P>
            consteval bool HasShape() {
                if constexpr (std::is_class_v<P>) {
                    for (auto a : std::meta::annotations_of(^^P))
                        if (std::meta::type_of(a) == ^^Shape) return true;
                }
                return false;
            }
        } // namespace Detail
        /** @endcond */

        /** @brief A geometric primitive: a class annotated with a `Geom::Shape`. */
        template <typename P>
        concept Primitive = Detail::HasShape<P>();

        /** @brief Extract the `Shape` descriptor of a primitive at compile time. */
        template <Primitive P>
        consteval Shape ShapeOf() {
            template for (constexpr auto a :
                          std::define_static_array(std::meta::annotations_of(^^P))) {
                if constexpr (std::meta::type_of(a) == ^^Shape)
                    return std::meta::extract<Shape>(a);
            }
            return {};
        }

        /** @brief Cached `Shape` descriptor for a primitive. */
        template <Primitive P> inline constexpr Shape kShape = ShapeOf<P>();
        /** @brief Spatial dimension of a primitive. */
        template <Primitive P> inline constexpr int  Dim       = kShape<P>.dim;
        /** @brief Whether a primitive encloses a finite region. */
        template <Primitive P> inline constexpr bool IsBounded = kShape<P>.bounded;
        /** @brief Whether a primitive is a convex point set / boundary. */
        template <Primitive P> inline constexpr bool IsConvex  = kShape<P>.convex;

        /** @cond INTERNAL */
        namespace Detail {
            /// @brief Reflection of a primitive's scalar field, read off its first data member.
            template <Primitive P>
            consteval std::meta::info ScalarReflOf() {
                auto members = std::meta::nonstatic_data_members_of(^^P, std::meta::access_context::unchecked());
                std::meta::info t = std::meta::remove_all_extents(std::meta::type_of(members[0])); // peel arrays (FrustumT planes[6])
                if (std::meta::has_template_arguments(t))
                    return std::meta::template_arguments_of(t)[0]; // Vec<T,N,A> → T
                return t;                                          // scalar member (e.g. Interval)
            }
        } // namespace Detail
        /** @endcond */

        /** @brief Scalar field type of a primitive (`float`, `double`, …). */
        template <Primitive P>
        using ScalarType = typename[:Detail::ScalarReflOf<P>():];

        /** @brief A primitive living in 2-D space. */
        template <typename P> concept Primitive2D = Primitive<P> && (Dim<P> == 2);
        /** @brief A primitive living in 3-D space. */
        template <typename P> concept Primitive3D = Primitive<P> && (Dim<P> == 3);
        /** @brief A primitive that encloses a finite region. */
        template <typename P> concept BoundedPrimitive = Primitive<P> && IsBounded<P>;

        /** @cond INTERNAL */
        namespace Detail {
            /// @brief Broadcast a scalar into every component of a vector.
            template <HomogeneousVec V>
            [[nodiscard]] constexpr V Splat(ScalarOf<V> s) {
                V v{};
                for (int i = 0; i < VecDim<V>; ++i) v[i] = s;
                return v;
            }
            /// @brief Widen a (possibly packed) vector to the GPU-aligned `Vec<T,N>`.
            template <std::floating_point T, int N, AlignTag A>
            [[nodiscard]] constexpr Vec<T, N> Widen(Vec<T, N, A> v) {
                Vec<T, N> r{};
                for (int i = 0; i < N; ++i) r[i] = v[i];
                return r;
            }
        } // namespace Detail
        /** @endcond */

        // =====================================================================
        // Interval operations (1-D)
        // =====================================================================

        /// @brief Length of interval (hi - lo).
        template <std::floating_point T>
        [[nodiscard]] constexpr T Length(IntervalT<T> i) { return i.hi - i.lo; }

        /// @brief Midpoint of interval.
        template <std::floating_point T>
        [[nodiscard]] constexpr T Midpoint(IntervalT<T> i) { return (i.lo + i.hi) * T(0.5); }

        /// @brief Test if a value lies within [lo, hi].
        template <std::floating_point T>
        [[nodiscard]] constexpr bool Contains(IntervalT<T> i, T v) { return v >= i.lo && v <= i.hi; }

        /// @brief Test if two intervals overlap.
        template <std::floating_point T>
        [[nodiscard]] constexpr bool Intersects(IntervalT<T> a, IntervalT<T> b) {
            return a.lo <= b.hi && b.lo <= a.hi;
        }

        /// @brief Union of two intervals (smallest interval enclosing both).
        template <std::floating_point T>
        [[nodiscard]] constexpr IntervalT<T> Union(IntervalT<T> a, IntervalT<T> b) {
            return {.lo = Math::Min(a.lo, b.lo), .hi = Math::Max(a.hi, b.hi)};
        }

        /// @brief Grow interval to include a value.
        template <std::floating_point T>
        [[nodiscard]] constexpr IntervalT<T> Union(IntervalT<T> i, T v) {
            return {.lo = Math::Min(i.lo, v), .hi = Math::Max(i.hi, v)};
        }

        /// @brief Intersection of two intervals (overlap region). Empty if no overlap.
        template <std::floating_point T>
        [[nodiscard]] constexpr IntervalT<T> Intersection(IntervalT<T> a, IntervalT<T> b) {
            T lo = Math::Max(a.lo, b.lo);
            T hi = Math::Min(a.hi, b.hi);
            return (lo <= hi) ? IntervalT<T>{lo, hi} : IntervalT<T>{};
        }

        /// @brief Expand interval by margin on both sides.
        template <std::floating_point T>
        [[nodiscard]] constexpr IntervalT<T> Expand(IntervalT<T> i, T margin) {
            return {.lo = i.lo - margin, .hi = i.hi + margin};
        }

        /// @brief Clamp a value to interval bounds.
        template <std::floating_point T>
        [[nodiscard]] constexpr T Clamp(T v, IntervalT<T> i) { return Math::Clamp(v, i.lo, i.hi); }

        // =====================================================================
        // Axis-aligned box operations (generic over dimension N)
        // =====================================================================

        /// @brief Smallest box enclosing both inputs.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr BoxT<T, N> Union(BoxT<T, N> a, BoxT<T, N> b) {
            return {.min = Math::Min(a.min, b.min), .max = Math::Max(a.max, b.max)};
        }

        /// @brief Grow a box to include a point.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr BoxT<T, N> Union(BoxT<T, N> box, Vec<T, N> p) {
            return {.min = Math::Min(box.min, p), .max = Math::Max(box.max, p)};
        }

        /// @brief Intersection region of two boxes. Returns an empty box if disjoint.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr BoxT<T, N> Intersection(BoxT<T, N> a, BoxT<T, N> b) {
            Vec<T, N> lo = Math::Max(a.min, b.min);
            Vec<T, N> hi = Math::Min(a.max, b.max);
            for (int i = 0; i < N; ++i)
                if (lo[i] > hi[i]) return {};
            return {.min = lo, .max = hi};
        }

        /// @brief Expand a box by margin on all sides.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr BoxT<T, N> Expand(BoxT<T, N> box, T margin) {
            Vec<T, N> m = Detail::Splat<Vec<T, N>>(margin);
            return {.min = box.min - m, .max = box.max + m};
        }

        /// @brief Test if two boxes overlap (inclusive boundary).
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr bool Intersects(BoxT<T, N> a, BoxT<T, N> b) {
            for (int i = 0; i < N; ++i)
                if (!(a.min[i] <= b.max[i] && a.max[i] >= b.min[i])) return false;
            return true;
        }

        /// @brief Test if a point lies inside a box (inclusive).
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr bool Contains(BoxT<T, N> box, Vec<T, N> p) {
            for (int i = 0; i < N; ++i)
                if (!(p[i] >= box.min[i] && p[i] <= box.max[i])) return false;
            return true;
        }

        /// @brief Squared distance from a point to the nearest surface of a box. Zero if inside.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr T DistanceSq(Vec<T, N> p, BoxT<T, N> box) {
            Vec<T, N> below = box.min - p; // positive when p < min
            Vec<T, N> above = p - box.max; // positive when p > max
            Vec<T, N> d     = Math::Max(Math::Max(below, above), Vec<T, N>{});
            return Math::Dot(d, d);
        }

        /// @brief Closest point on (or inside) a box to a given point.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr Vec<T, N> ClosestPoint(BoxT<T, N> box, Vec<T, N> p) {
            return Math::Clamp(p, box.min, box.max);
        }

        /// @brief Centroid (midpoint) of a box.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr Vec<T, N> Centroid(BoxT<T, N> box) {
            return (box.min + box.max) * T(0.5);
        }

        /// @brief Half-extents per axis of a box.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr Vec<T, N> Extents(BoxT<T, N> box) {
            return (box.max - box.min) * T(0.5);
        }

        /// @brief Index of the largest extent axis of a box (0 = x, 1 = y, …).
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr int LargestAxis(BoxT<T, N> box) {
            Vec<T, N> e    = box.max - box.min;
            int       best = 0;
            for (int i = 1; i < N; ++i)
                if (e[i] > e[best]) best = i;
            return best;
        }

        /**
         * @brief `N`-dimensional Lebesgue measure of a box: ∏ extentᵢ.
         *
         * Length (N=1), area (N=2), volume (N=3), hypervolume (N≥4).
         */
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr T Measure(BoxT<T, N> box) {
            Vec<T, N> e = box.max - box.min;
            T         m = T(1);
            for (int i = 0; i < N; ++i) m *= e[i];
            return m;
        }

        /**
         * @brief (N-1)-measure of a box's boundary: 2 ∑ᵢ ∏_{j≠i} extentⱼ.
         *
         * Perimeter (N=2), surface area (N=3).
         */
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr T BoundaryMeasure(BoxT<T, N> box) {
            Vec<T, N> e   = box.max - box.min;
            T         sum = T(0);
            for (int i = 0; i < N; ++i) {
                T prod = T(1);
                for (int j = 0; j < N; ++j)
                    if (j != i) prod *= e[j];
                sum += prod;
            }
            return T(2) * sum;
        }

        /// @brief Area of a 2-D box (alias for `Measure`).
        template <std::floating_point T>
        [[nodiscard]] constexpr T Area(BoxT<T, 2> box) { return Measure(box); }
        /// @brief Perimeter of a 2-D box (alias for `BoundaryMeasure`).
        template <std::floating_point T>
        [[nodiscard]] constexpr T Perimeter(BoxT<T, 2> box) { return BoundaryMeasure(box); }
        /// @brief Volume of a 3-D box (alias for `Measure`).
        template <std::floating_point T>
        [[nodiscard]] constexpr T Volume(BoxT<T, 3> box) { return Measure(box); }
        /// @brief Surface area of a 3-D box (alias for `BoundaryMeasure`; used by SAH cost).
        template <std::floating_point T>
        [[nodiscard]] constexpr T SurfaceArea(BoxT<T, 3> box) { return BoundaryMeasure(box); }

        // =====================================================================
        // Ball operations (generic over dimension N)
        // =====================================================================

        /// @brief Test if a point is inside a ball.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr bool Contains(BallT<T, N> b, Vec<T, N> p) {
            Vec<T, N> d{};
            for (int i = 0; i < N; ++i) d[i] = p[i] - b.center[i];
            return Math::Dot(d, d) <= b.radius * b.radius;
        }

        /// @brief Test if two balls overlap.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr bool Intersects(BallT<T, N> a, BallT<T, N> b) {
            Vec<T, N> d{};
            for (int i = 0; i < N; ++i) d[i] = a.center[i] - b.center[i];
            T rSum = a.radius + b.radius;
            return Math::Dot(d, d) <= rSum * rSum;
        }

        /// @brief Test if a ball and a box overlap.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr bool Intersects(BallT<T, N> b, BoxT<T, N> box) {
            return DistanceSq(Detail::Widen(b.center), box) <= b.radius * b.radius;
        }

        /// @brief Smallest ball enclosing both (conservative: Ritter-style).
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr BallT<T, N> Union(BallT<T, N> a, BallT<T, N> b) {
            Vec<T, N> diff{};
            for (int i = 0; i < N; ++i) diff[i] = b.center[i] - a.center[i];
            T dist = Math::Norm(diff);
            if (dist + b.radius <= a.radius) return a; // b inside a
            if (dist + a.radius <= b.radius) return b; // a inside b
            T          newRadius = (dist + a.radius + b.radius) * T(0.5);
            T          s         = (newRadius - a.radius) / dist;
            BallT<T, N> r;
            r.radius = newRadius;
            for (int i = 0; i < N; ++i) r.center[i] = a.center[i] + diff[i] * s;
            return r;
        }

        /// @brief Axis-aligned bounding box of a ball.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr BoxT<T, N> Bounds(BallT<T, N> b) {
            BoxT<T, N> box;
            for (int i = 0; i < N; ++i) {
                box.min[i] = b.center[i] - b.radius;
                box.max[i] = b.center[i] + b.radius;
            }
            return box;
        }

        // =====================================================================
        // Hyperplane operations (generic over dimension N)
        // =====================================================================

        /// @brief Signed distance from a point to a hyperplane.
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr T SignedDistance(HyperplaneT<T, N> pl, Vec<T, N> p) {
            T d = pl.dist;
            for (int i = 0; i < N; ++i) d += pl.normal[i] * p[i];
            return d;
        }

        /// @brief Which side of the hyperplane a point is on (+1 front, -1 back, 0 on).
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr int Side(HyperplaneT<T, N> pl, Vec<T, N> p) {
            T d = SignedDistance(pl, p);
            return (d > T(0)) ? 1 : (d < T(0) ? -1 : 0);
        }

        // =====================================================================
        // Frustum culling (3-D)
        // =====================================================================

        /**
         * @brief Conservative box-vs-frustum test (6 planes).
         *
         * May return `true` for near-misses (false positive), never `false`
         * for a box actually inside. Plane convention: `dot(n, pt) + w >= 0` → inside.
         */
        template <std::floating_point T>
        [[nodiscard]] constexpr bool Intersects(BoxT<T, 3> box, const FrustumT<T>& frustum) {
            for (int i = 0; i < 6; ++i) {
                Vec<T, 4> pl = frustum.planes[i];
                T         px = (pl.x >= T(0)) ? box.max.x : box.min.x;
                T         py = (pl.y >= T(0)) ? box.max.y : box.min.y;
                T         pz = (pl.z >= T(0)) ? box.max.z : box.min.z;
                if (px * pl.x + py * pl.y + pz * pl.z + pl.w < T(0)) return false;
            }
            return true;
        }

        // =====================================================================
        // Ray precomputation + intersection (generic over dimension N)
        // =====================================================================

        /** @brief Precomputed reciprocal-direction ray for fast slab tests. */
        template <std::floating_point T, int N>
        struct RayPrecompT {
            Vec<T, N> origin;     ///< Ray origin.
            Vec<T, N> invDir;     ///< Component-wise `1 / direction` (safe for dir ≈ 0).
            uint32_t  dirSign[N]; ///< 1 if direction component < 0, 0 otherwise.
        };

        using RayPrecomp = RayPrecompT<float, 3>; ///< 3-D float precomputed ray.

        /** @brief Precompute reciprocal direction + sign flags. Call once per ray. */
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr RayPrecompT<T, N> Prepare(RayT<T, N> ray) {
            RayPrecompT<T, N> r;
            r.origin = ray.origin;
            for (int i = 0; i < N; ++i) {
                T d          = ray.direction[i];
                r.invDir[i]  = (Math::Abs(d) < T(1e-20)) ? Math::CopySign(T(1e20), d) : T(1) / d;
                r.dirSign[i] = (d < T(0)) ? 1u : 0u;
            }
            return r;
        }

        /**
         * @brief Tavian Barnes 2022 NaN-safe, boundary-inclusive ray–box slab test.
         *
         * @return `(tMin, tMax)`. Hit iff `tMin ≤ tMax`. `tMin < 0` ⇒ origin inside box.
         * @see https://tavianator.com/2022/ray_box_boundary.html
         */
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr std::pair<T, T> Intersect(const RayPrecompT<T, N>& ray, BoxT<T, N> box) {
            T tmin = T(0);
            T tmax = std::numeric_limits<T>::infinity();
            for (int i = 0; i < N; ++i) {
                T t1 = (box.min[i] - ray.origin[i]) * ray.invDir[i];
                T t2 = (box.max[i] - ray.origin[i]) * ray.invDir[i];
                tmin = Math::Min(Math::Max(t1, tmin), Math::Max(t2, tmin));
                tmax = Math::Max(Math::Min(t1, tmax), Math::Min(t2, tmax));
            }
            return {tmin, tmax};
        }

        /** @brief Convenience: intersect a raw Ray with a box (prepares internally). */
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr std::pair<T, T> Intersect(RayT<T, N> ray, BoxT<T, N> box) {
            return Intersect(Prepare(ray), box);
        }

    } // namespace Geom

} // namespace Mashiro
