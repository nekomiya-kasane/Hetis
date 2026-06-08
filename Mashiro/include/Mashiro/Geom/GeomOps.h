/**
 * @file GeomOps.h
 * @brief Annotation-driven affine transformation of geometric primitives.
 *
 * A matrix satisfying `AffineMatrix` (compact `Mat<T,N,N+1>` or full `Mat<T,N+1>`)
 * acts on a primitive by mapping each data member according to its `Geom::Quantity`
 * annotation (the field's tensor transformation law). For "affinely decomposable"
 * primitives (e.g. `Ray`) the action is derived structurally via P2996 reflection +
 * P1306 `template for`, dispatched by `if constexpr` — zero runtime overhead.
 * Primitives whose affine image is *not* the direct sum of their fields (`Box` refit,
 * `Ball` radius scaling, `Hyperplane`/`Frustum` homogeneous-covector coupling) provide
 * explicit, mathematically correct overloads.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Geom/Geom.h"
#include "Mashiro/Math/AffineOps.h"

#include <concepts>
#include <meta>

namespace Mashiro::Geom {

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

        /// @brief Apply the transformation law selected by @p Q to a field.
        template <Quantity Q, AffineMatrix M, typename F>
        [[nodiscard]] constexpr F ApplyField(const M& m, F f) {
            if constexpr (Q == Quantity::Point)          return Math::TransformPoint(m, f);
            else if constexpr (Q == Quantity::Direction) return Math::TransformVector(m, f);
            else if constexpr (Q == Quantity::Normal)    return Math::TransformNormal(m, f);
            else return f;
        }

        /// @brief i-th corner of a box (bit i of @p mask selects max over min).
        template <std::floating_point T, int N>
        [[nodiscard]] constexpr Vec<T, N> BoxCorner(const BoxT<T, N>& b, int mask) {
            Vec<T, N> c{};
            for (int i = 0; i < N; ++i) c[i] = ((mask >> i) & 1) ? b.max[i] : b.min[i];
            return c;
        }

        /// @brief Cotransform: Transpose(Inverse(Linear(m))).
        template <AffineMatrix M>
        [[nodiscard]] constexpr auto Cotransform(const M& m) {
            return Math::Transpose(Math::Inverse(Math::Linear(m)));
        }

        /// @brief Conservative σ_max(A) ≤ √(‖A‖₁·‖A‖∞).
        template <AffineMatrix M>
        [[nodiscard]] constexpr auto RadiusScale(const M& m) {
            using T = ScalarOf<MatColType<M>>;
            constexpr int N = AffineDim<M>;
            auto linear = Math::Linear(m);
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

    } // namespace Detail
    /** @endcond */

    // =====================================================================
    // Generic reflection-driven transform (decomposable primitives)
    // =====================================================================

    /** @brief Affine action derived structurally from annotated fields. */
    template <AffineMatrix M, Decomposable P>
        requires(AffineDim<M> == Dim<P>)
    [[nodiscard]] constexpr P Transform(const M& m, P prim) {
        template for (constexpr auto mem : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^P, std::meta::access_context::unchecked()))) {
            constexpr Quantity q = Detail::QuantityOf<mem>();
            prim.[:mem:]         = Detail::ApplyField<q>(m, prim.[:mem:]);
        }
        return prim;
    }

    // =====================================================================
    // Explicit laws for non-decomposable primitives
    // =====================================================================

    /** @brief Box: transform all 2^N corners and refit axis-aligned bounds. */
    template <AffineMatrix M, std::floating_point T, int N>
        requires(AffineDim<M> == N)
    [[nodiscard]] constexpr BoxT<T, N> Transform(const M& m, BoxT<T, N> box) {
        Vec<T, N> c0 = Math::TransformPoint(m, Detail::BoxCorner(box, 0));
        BoxT<T, N> r{.min = c0, .max = c0};
        for (int mask = 1; mask < (1 << N); ++mask) {
            Vec<T, N> p = Math::TransformPoint(m, Detail::BoxCorner(box, mask));
            r.min       = Math::Min(r.min, p);
            r.max       = Math::Max(r.max, p);
        }
        return r;
    }

    /** @brief Ball: centre as point; radius scales by conservative spectral bound. */
    template <AffineMatrix M, std::floating_point T, int N>
        requires(AffineDim<M> == N)
    [[nodiscard]] constexpr BallT<T, N> Transform(const M& m, BallT<T, N> b) {
        Vec<T, N> wideCenter{};
        for (int i = 0; i < N; ++i) wideCenter[i] = b.center[i];
        Vec<T, N> c = Math::TransformPoint(m, wideCenter);
        BallT<T, N> r;
        for (int i = 0; i < N; ++i) r.center[i] = c[i];
        r.radius = b.radius * Detail::RadiusScale(m);
        return r;
    }

    /** @brief Hyperplane: covector law n' = (A⁻¹)ᵀ·n, d' adjusted, renormalised. */
    template <AffineMatrix M, std::floating_point T, int N>
        requires(AffineDim<M> == N)
    [[nodiscard]] constexpr HyperplaneT<T, N> Transform(const M& m, HyperplaneT<T, N> pl) {
        auto co = Detail::Cotransform(m);
        Vec<T, N> nWide{};
        for (int i = 0; i < N; ++i) nWide[i] = pl.normal[i];
        Vec<T, N> nRaw = co * nWide;
        T dRaw = pl.dist - Math::Dot(nRaw, Math::Translation(m));
        T len  = Math::Norm(nRaw);
        T inv  = (len > T(0)) ? T(1) / len : T(1);
        HyperplaneT<T, N> r;
        for (int i = 0; i < N; ++i) r.normal[i] = nRaw[i] * inv;
        r.dist = dRaw * inv;
        return r;
    }

    /** @brief Frustum: each plane transforms as covector (3D only). */
    template <AffineMatrix M, std::floating_point T>
        requires(AffineDim<M> == 3)
    [[nodiscard]] constexpr FrustumT<T> Transform(const M& m, FrustumT<T> fr) {
        auto co = Detail::Cotransform(m);
        auto t  = Math::Translation(m);
        FrustumT<T> r;
        for (int i = 0; i < 6; ++i) {
            Vec<T, 4> p    = fr.planes[i];
            Vec<T, 3> nRaw = co * Vec<T, 3>{p.x, p.y, p.z};
            T wRaw = p.w - Math::Dot(nRaw, t);
            T len  = Math::Norm(nRaw);
            T inv  = (len > T(0)) ? T(1) / len : T(1);
            r.planes[i] = Vec<T, 4>{nRaw.x * inv, nRaw.y * inv, nRaw.z * inv, wRaw * inv};
        }
        return r;
    }

    // =====================================================================
    // Sugar & construction helpers
    // =====================================================================

    /** @brief Apply transform to primitive: `m * primitive`. */
    template <AffineMatrix M, Primitive P>
        requires(AffineDim<M> == Dim<P>)
    [[nodiscard]] constexpr P operator*(const M& m, P prim) { return Transform(m, prim); }

    /** @brief Build a pure-translation compact affine. */
    template <std::floating_point T, int N>
    [[nodiscard]] constexpr Mat<T, N, N + 1> Translation(Vec<T, N> t) {
        return Math::MakeTranslation(t);
    }

    /** @brief Build a pure linear (no-translation) compact affine from a square matrix. */
    template <std::floating_point T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Mat<T, N, N + 1> LinearMap(Mat<T, N> a) {
        return Math::MakeAffine(a, Vec<T, N>{});
    }

} // namespace Mashiro::Geom


