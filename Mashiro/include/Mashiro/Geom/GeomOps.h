/**
 * @file GeomOps.h
 * @brief Reflection + annotation driven affine transformation of geometric primitives.
 *
 * A spatial transform (`Affine<T,N>` / `Rigid<T,N>`) acts on a primitive by mapping
 * each data member according to its `Geom::Quantity` annotation (the field's tensor
 * transformation law). For "affinely decomposable" primitives (e.g. `Ray`) the action
 * is derived structurally via P2996 reflection + P1306 `template for`, dispatched by
 * `if constexpr` — zero runtime overhead. Primitives whose affine image is *not* the
 * direct sum of their fields (`Box` refit, `Ball` radius scaling, `Hyperplane`/`Frustum`
 * homogeneous-covector coupling) provide explicit, mathematically correct overloads.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Geom/Geom.h"
#include "Mashiro/Math/MathUtils.h"

#include <concepts>
#include <meta>

namespace Mashiro::Geom {

    // =====================================================================
    // Spatial transforms (group elements acting on primitives)
    // =====================================================================

    /** @brief General affine map `x ↦ A·x + b` with A ∈ GL(n). */
    template <std::floating_point T, int N>
    struct Affine {
        using scalar_type                  = T;
        static constexpr int dimension     = N;

        Mat<T, N> linear      = Math::Identity<Mat<T, N>>(); ///< Linear part A.
        Vec<T, N> translation = {};                    ///< Translation b.

        [[nodiscard]] constexpr Vec<T, N> TransformPoint(Vec<T, N> p) const { return linear * p + translation; }
        [[nodiscard]] constexpr Vec<T, N> TransformDirection(Vec<T, N> v) const { return linear * v; }
        /// @brief Normal cotransform A⁻ᵀ (preserves orthogonality).
        [[nodiscard]] constexpr Mat<T, N> Cotransform() const { return Math::Transpose(Math::Inverse(linear)); }
        [[nodiscard]] constexpr Vec<T, N> TransformNormal(Vec<T, N> n) const {
            return Math::Normalize(Cotransform() * n);
        }
        /// @brief Conservative enclosing-ball scale: σ_max(A) ≤ √(‖A‖₁·‖A‖∞). Exact for similarities.
        [[nodiscard]] constexpr T RadiusScale() const {
            T c1 = T(0), cInf = T(0);
            for (int col = 0; col < N; ++col) {
                T s = T(0);
                for (int row = 0; row < N; ++row) s += Math::Abs(linear[row, col]);
                c1 = Math::Max(c1, s);
            }
            for (int row = 0; row < N; ++row) {
                T s = T(0);
                for (int col = 0; col < N; ++col) s += Math::Abs(linear[row, col]);
                cInf = Math::Max(cInf, s);
            }
            return Math::Sqrt(c1 * cInf);
        }
    };

    /** @brief Rigid motion `x ↦ R·x + t` with R orthonormal (SE(n)): exact, no inverse needed. */
    template <std::floating_point T, int N>
    struct Rigid {
        using scalar_type              = T;
        static constexpr int dimension = N;

        Mat<T, N> rotation    = Math::Identity<Mat<T, N>>(); ///< Orthonormal rotation R.
        Vec<T, N> translation = {};                    ///< Translation t.

        [[nodiscard]] constexpr Vec<T, N> TransformPoint(Vec<T, N> p) const { return rotation * p + translation; }
        [[nodiscard]] constexpr Vec<T, N> TransformDirection(Vec<T, N> v) const { return rotation * v; }
        [[nodiscard]] constexpr Mat<T, N> Cotransform() const { return rotation; } // R⁻ᵀ = R
        [[nodiscard]] constexpr Vec<T, N> TransformNormal(Vec<T, N> n) const {
            return Math::Normalize(rotation * n);
        }
        [[nodiscard]] constexpr T RadiusScale() const { return T(1); } // isometry preserves radius
    };

    /** @brief A group element that can act on geometric primitives. */
    template <typename X>
    concept SpatialTransform = requires {
        typename X::scalar_type;
        { X::dimension } -> std::convertible_to<int>;
    } && requires(const X& x, Vec<typename X::scalar_type, X::dimension> v) {
        { x.TransformPoint(v) } -> std::same_as<Vec<typename X::scalar_type, X::dimension>>;
        { x.TransformDirection(v) } -> std::same_as<Vec<typename X::scalar_type, X::dimension>>;
        { x.TransformNormal(v) } -> std::same_as<Vec<typename X::scalar_type, X::dimension>>;
        { x.RadiusScale() } -> std::same_as<typename X::scalar_type>;
        { x.Cotransform() };
        { x.translation };
    };

    // =====================================================================
    // Decomposability customization point
    // =====================================================================

    /// @brief Opt-in: a primitive whose affine image equals the direct sum of its
    ///        fields' transformed values (no joint invariant). `Ray` qualifies.
    template <typename P>
    inline constexpr bool affinely_decomposable_v = false;
    template <std::floating_point T, int N>
    inline constexpr bool affinely_decomposable_v<RayT<T, N>> = true;

    /** @brief A primitive eligible for the reflection-driven generic transform. */
    template <typename P>
    concept Decomposable = Primitive<P> && affinely_decomposable_v<P>;

    /** @cond INTERNAL */
    namespace Detail {
        /// @brief Quantity annotation of a data member (defaults to `Scalar`).
        template <std::meta::info M>
        consteval Quantity QuantityOf() {
            template for (constexpr auto a : std::define_static_array(std::meta::annotations_of(M))) {
                if constexpr (std::meta::type_of(a) == ^^Quantity)
                    return std::meta::extract<Quantity>(a);
            }
            return Quantity::Scalar;
        }

        /// @brief Apply the transformation law selected by @p Q to a single field.
        template <Quantity Q, SpatialTransform X, typename F>
        [[nodiscard]] constexpr F ApplyField(const X& x, F f) {
            if constexpr (Q == Quantity::Point) return x.TransformPoint(f);
            else if constexpr (Q == Quantity::Direction) return x.TransformDirection(f);
            else if constexpr (Q == Quantity::Normal) return x.TransformNormal(f);
            else return f; // Scalar / unknown: invariant under the generic path
        }

        /// @brief i-th corner of a box (bit i of @p mask selects max over min on axis i).
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr Vec<T, N> BoxCorner(const BoxT<T, N>& b, int mask) {
            Vec<T, N> c{};
            for (int i = 0; i < N; ++i) c[i] = ((mask >> i) & 1) ? b.max[i] : b.min[i];
            return c;
        }
    } // namespace Detail
    /** @endcond */

    // =====================================================================
    // Generic reflection-driven transform (decomposable primitives)
    // =====================================================================

    /**
     * @brief Affine action derived structurally from the primitive's annotated fields.
     *
     * Each member is dispatched to its `Geom::Quantity` transformation law at compile
     * time; the loop is fully unrolled and folds to the same code as a hand-written
     * transform. Only enabled for `Decomposable` primitives (e.g. `Ray`).
     */
    template <SpatialTransform X, Decomposable P>
    [[nodiscard]] constexpr P Transform(const X& x, P prim) {
        template for (constexpr auto m : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^P, std::meta::access_context::unchecked()))) {
            constexpr Quantity q = Detail::QuantityOf<m>();
            prim.[:m:]           = Detail::ApplyField<q>(x, prim.[:m:]);
        }
        return prim;
    }

    // =====================================================================
    // Explicit laws for non-decomposable primitives
    // =====================================================================

    /// @brief Box: transform all 2ᴺ corners and re-fit the axis-aligned bounds (correct for any affine).
    template <SpatialTransform X, std::floating_point T, int N>
        requires(X::dimension == N)
    [[nodiscard]] constexpr BoxT<T, N> Transform(const X& x, BoxT<T, N> box) {
        Vec<T, N> c0 = x.TransformPoint(Detail::BoxCorner(box, 0));
        BoxT<T, N> r{.min = c0, .max = c0};
        for (int mask = 1; mask < (1 << N); ++mask) {
            Vec<T, N> p = x.TransformPoint(Detail::BoxCorner(box, mask));
            r.min       = Math::Min(r.min, p);
            r.max       = Math::Max(r.max, p);
        }
        return r;
    }

    /// @brief Ball: centre transforms affinely; radius scales by the transform's (conservative) factor.
    template <SpatialTransform X, std::floating_point T, int N>
        requires(X::dimension == N)
    [[nodiscard]] constexpr BallT<T, N> Transform(const X& x, BallT<T, N> b) {
        Vec<T, N>  c = x.TransformPoint(Geom::Detail::Widen(b.center));
        BallT<T, N> r;
        for (int i = 0; i < N; ++i) r.center[i] = c[i];
        r.radius = b.radius * x.RadiusScale();
        return r;
    }

    /// @brief Hyperplane: homogeneous covector law `n' = A⁻ᵀn`, `d' = d - n'·b`, then renormalised.
    template <SpatialTransform X, std::floating_point T, int N>
        requires(X::dimension == N)
    [[nodiscard]] constexpr HyperplaneT<T, N> Transform(const X& x, HyperplaneT<T, N> pl) {
        Vec<T, N> nRaw = x.Cotransform() * Geom::Detail::Widen(pl.normal);
        T         dRaw = pl.dist - Math::Dot(nRaw, x.translation);
        T         len  = Math::Norm(nRaw);
        T         inv  = (len > T(0)) ? T(1) / len : T(1);
        HyperplaneT<T, N> r;
        for (int i = 0; i < N; ++i) r.normal[i] = nRaw[i] * inv;
        r.dist = dRaw * inv;
        return r;
    }

    /// @brief Frustum: each plane transforms as a homogeneous covector (3-D transforms only).
    template <SpatialTransform X, std::floating_point T>
        requires(X::dimension == 3)
    [[nodiscard]] constexpr FrustumT<T> Transform(const X& x, FrustumT<T> fr) {
        Mat<T, 3>  co = x.Cotransform();
        FrustumT<T> r;
        for (int i = 0; i < 6; ++i) {
            Vec<T, 4> p    = fr.planes[i];
            Vec<T, 3> nRaw = co * Vec<T, 3>{p.x, p.y, p.z};
            T         wRaw = p.w - Math::Dot(nRaw, x.translation);
            T         len  = Math::Norm(nRaw);
            T         inv  = (len > T(0)) ? T(1) / len : T(1);
            r.planes[i]    = Vec<T, 4>{nRaw.x * inv, nRaw.y * inv, nRaw.z * inv, wRaw * inv};
        }
        return r;
    }

    // =====================================================================
    // Sugar
    // =====================================================================

    /// @brief Apply a transform to a primitive: `x * primitive`.
    template <SpatialTransform X, Primitive P>
    [[nodiscard]] constexpr P operator*(const X& x, P prim) { return Transform(x, prim); }

    /// @brief Compose two affine maps: `(a ∘ b)(x) = a(b(x))`.
    template <std::floating_point T, int N>
    [[nodiscard]] constexpr Affine<T, N> operator*(const Affine<T, N>& a, const Affine<T, N>& b) {
        return {.linear = a.linear * b.linear, .translation = a.linear * b.translation + a.translation};
    }

    /// @brief Build a pure-translation affine map.
    template <std::floating_point T, int N>
    [[nodiscard]] constexpr Affine<T, N> Translation(Vec<T, N> t) {
        return {.linear = Math::Identity<Mat<T, N>>(), .translation = t};
    }

    /// @brief Build a pure linear affine map (no translation).
    template <std::floating_point T, int N>
    [[nodiscard]] constexpr Affine<T, N> LinearMap(Mat<T, N> a) {
        return {.linear = a, .translation = {}};
    }

} // namespace Mashiro::Geom
