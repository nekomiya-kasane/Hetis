/**
 * @file VecOps.h
 * @brief Concrete vector operations layered on the Algebra.h abstraction.
 *
 * This header is the bridge between the concrete `Vec<T,N>` type (Vec.h) and the
 * abstract algebraic concept hierarchy (Algebra.h). It provides:
 *
 *   - The ADL `InnerProduct(HomogeneousVec)` hook, which makes every homogeneous
 *     vector model `InnerProductSpace` (and therefore `NormedSpace` / `MetricSpace`),
 *     so all generic algorithms in Algebra.h (`Norm`, `Normalize`, `Distance`, …)
 *     light up automatically for `vec2/3/4`, `ivec*`, `dvec*`, etc.
 *   - The p-norm family `Norm1` / `Norm2` / `NormInf` and the `Norm<NormType>` dispatcher.
 *   - The cross product (`Cross`) for 2D (perp-dot scalar) and 3D vectors.
 *   - Component-wise reductions and clamps (`Min` / `Max` / `Clamp` / `Abs`).
 *
 * Every operation is `constexpr` and expanded with P1306 `template for` over a static
 * index sequence, so the compiler fully unrolls the fixed (2/3/4) dimension and the
 * result is identical to hand-written scalar code — zero runtime overhead.
 *
 * Namespace: `Mashiro::Math` (algorithms); `Mashiro` (the `InnerProduct` ADL hook).
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Core/Meta.h"
#include "Mashiro/Math/Algebra.h"
#include "Mashiro/Math/Vec.h"

namespace Mashiro {

    /**
     * @brief ADL `InnerProduct` for every HomogeneousVec — the Euclidean dot product.
     *
     * This is the single hook that injects the entire `Vec` family into the algebraic
     * concept hierarchy of Algebra.h. It MUST live at `Mashiro` scope (not in a nested
     * namespace): argument-dependent lookup for `Vec<T,N>` only searches `Vec`'s own
     * namespace `Mashiro`, so a hook inside `Mashiro::Math` would be invisible to ADL and
     * `Vec` would fail to model `InnerProductSpace`.
     */
    template<HomogeneousVec V>
        requires std::is_arithmetic_v<ScalarOf<V>>
    [[nodiscard]] constexpr ScalarOf<V> InnerProduct(V a, V b) {
        ScalarOf<V> sum{};
        template for (constexpr int i : Iota<VecDim<V>>) {
            sum += a[i] * b[i];
        }
        return sum;
    }

    namespace Math {

        /// @name Cross product
        /// @{

        /** @brief 2D cross (perp-dot): z-component of (a, 0) × (b, 0). */
        template<HomogeneousVec V>
            requires(VecDim<V> == 2)
        [[nodiscard]] constexpr ScalarOf<V> Cross(V a, V b) {
            return a[0] * b[1] - a[1] * b[0];
        }

        /** @brief 3D cross product: a × b. */
        template<HomogeneousVec V>
            requires(VecDim<V> == 3)
        [[nodiscard]] constexpr V Cross(V a, V b) {
            V r;
            r[0] = a[1] * b[2] - a[2] * b[1];
            r[1] = a[2] * b[0] - a[0] * b[2];
            r[2] = a[0] * b[1] - a[1] * b[0];
            return r;
        }

        /// @}

        /// @name p-norm family
        /// @{

        /** @brief Squared L2 norm (avoids the square root). Alias for NormSq. */
        template<InnerProductSpace V>
        [[nodiscard]] constexpr FieldOf<V> Norm2Sq(V v) {
            return NormSq(v);
        }

        /** @brief L2 (Euclidean) norm. Equivalent to Norm(v) for InnerProductSpace types. */
        template<InnerProductSpace V>
            requires std::floating_point<FieldOf<V>>
        [[nodiscard]] constexpr FieldOf<V> Norm2(V v) {
            return Norm(v);
        }

        /** @brief L1 (Manhattan / taxicab) norm: Σ|xᵢ|. */
        template<HomogeneousVec V>
            requires std::floating_point<ScalarOf<V>>
        [[nodiscard]] constexpr ScalarOf<V> Norm1(V v) {
            ScalarOf<V> sum{};
            template for (constexpr int i : Iota<VecDim<V>>) {
                sum += Abs(v[i]);
            }
            return sum;
        }

        /** @brief L-infinity (Chebyshev / max-abs) norm: maxᵢ|xᵢ|. */
        template<HomogeneousVec V>
            requires std::floating_point<ScalarOf<V>>
        [[nodiscard]] constexpr ScalarOf<V> NormInf(V v) {
            ScalarOf<V> m{};
            template for (constexpr int i : Iota<VecDim<V>>) {
                m = Max(m, Abs(v[i]));
            }
            return m;
        }

        /** @brief Which vector norm to compute: L1 (Σ|x|), L2 (Euclidean), Linf (max|x|). */
        enum class NormType { L1, L2, Linf };

        /** @brief Dispatch to a specific p-norm: L1, L2, or Linf. Must specify P explicitly. */
        template<NormType P, HomogeneousVec V>
            requires std::floating_point<ScalarOf<V>>
        [[nodiscard]] constexpr ScalarOf<V> Norm(V v) {
            if constexpr (P == NormType::L1) {
                return Norm1(v);
            } else if constexpr (P == NormType::Linf) {
                return NormInf(v);
            } else {
                return Norm2(v);
            }
        }

        /// @}

        /// @name Component-wise reductions and clamps
        /// @{

        /** @brief Component-wise minimum over one or more vectors of the same type. */
        template<HomogeneousVec V, std::same_as<V>... Vs>
        [[nodiscard]] constexpr V Min(V a, Vs... rest) {
            V r;
            template for (constexpr int i : Iota<VecDim<V>>) {
                r[i] = Min(a[i], rest[i]...);
            }
            return r;
        }

        /** @brief Component-wise maximum over one or more vectors of the same type. */
        template<HomogeneousVec V, std::same_as<V>... Vs>
        [[nodiscard]] constexpr V Max(V a, Vs... rest) {
            V r;
            template for (constexpr int i : Iota<VecDim<V>>) {
                r[i] = Max(a[i], rest[i]...);
            }
            return r;
        }

        /** @brief Component-wise clamp: each element clamped to [lo, hi]. */
        template<HomogeneousVec V>
        [[nodiscard]] constexpr V Clamp(V v, V lo, V hi) {
            V r;
            template for (constexpr int i : Iota<VecDim<V>>) {
                r[i] = Clamp(v[i], lo[i], hi[i]);
            }
            return r;
        }

        /** @brief Component-wise absolute value. */
        template<HomogeneousVec V>
            requires std::is_signed_v<ScalarOf<V>>
        [[nodiscard]] constexpr V Abs(V v) {
            V r;
            template for (constexpr int i : Iota<VecDim<V>>) {
                r[i] = Abs(v[i]);
            }
            return r;
        }

        /// @}

    } // namespace Math

} // namespace Mashiro
