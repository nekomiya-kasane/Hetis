/**
 * @file MatOps.h
 * @brief Generic matrix algebra and projective (perspective / orthographic) builders.
 *
 * The concrete matrix counterpart to VecOps.h. Header-only and fully `constexpr`.
 * Matrix functions are concept-constrained templates that work with any `Mat<T,R,C>`
 * satisfying `ColumnMajorMat`; transcendental math (`Sqrt`, `Sin`, `Cos`, …) is routed
 * through ScalarMath.h (hardware `std::` at run time, polynomial kernels during constant
 * evaluation), so projections fold to constants when inputs are known.
 *
 * Provides:
 *   - Generic matrix algebra: `Transpose`, `Identity`, `Det`, `Inverse`.
 *   - Projective camera matrices: `MakePerspective`, `MakeOrtho` (and Y-flipped variants).
 *
 * *Affine* transforms (translation, scale, rotation, look-at) and their composition live
 * in Affine.h, built on the first-class `Affine<T,N,Storage>` type — they are distinct
 * from the projective transforms here, which genuinely require the full homogeneous matrix.
 *
 * Fixed-dimension loops are expanded with P1306 `template for` over a static index
 * sequence, fully unrolled by the compiler — zero runtime overhead.
 *
 * Convention: column-major. Vulkan clip space: Y-down, Z [0,1].
 *
 * Namespace: `Mashiro::Math`.
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Core/Meta.h"
#include "Mashiro/Math/Types.h"
#include "Mashiro/Math/VecOps.h"

namespace Mashiro::Math {

    /// @name Generic matrix functions (any ColumnMajorMat)
    /// @{

    /** @brief Transpose: Mat<T,R,C> → Mat<T,C,R>. Works for any shape. */
    template<typename T, int R, int C>
    [[nodiscard]] constexpr Mat<T, C, R> Transpose(const Mat<T, R, C>& m) {
        Mat<T, C, R> r{};
        template for (constexpr int row : Iota<R>) {
            template for (constexpr int col : Iota<C>) {
                r[col, row] = m[row, col];
            }
        }
        return r;
    }

    /** @brief Identity matrix for any square ColumnMajorMat (defaults to mat4). */
    template<ColumnMajorMat M = mat4>
    [[nodiscard]] constexpr M Identity() {
        using S = ScalarOf<MatColType<M>>;
        M m{};
        template for (constexpr int i : Iota<MatDim<M>>) {
            m[i, i] = S(1);
        }
        return m;
    }

    /// @}

    /// @name Determinant (square matrices 2×2, 3×3, 4×4)
    /// @{

    /** @brief Determinant of a square matrix (2×2, 3×3, or 4×4). Cofactor expansion. */
    template<typename T, int N>
        requires(N >= 2 && N <= 4)
    [[nodiscard]] constexpr T Det(const Mat<T, N>& m) {
        if constexpr (N == 2) {
            return m[0, 0] * m[1, 1] - m[0, 1] * m[1, 0];
        } else if constexpr (N == 3) {
            return m[0, 0] * (m[1, 1] * m[2, 2] - m[1, 2] * m[2, 1]) -
                   m[0, 1] * (m[1, 0] * m[2, 2] - m[1, 2] * m[2, 0]) +
                   m[0, 2] * (m[1, 0] * m[2, 1] - m[1, 1] * m[2, 0]);
        } else {
            T s0 = m[0,0]*m[1,1] - m[0,1]*m[1,0];
            T s1 = m[0,0]*m[1,2] - m[0,2]*m[1,0];
            T s2 = m[0,0]*m[1,3] - m[0,3]*m[1,0];
            T s3 = m[0,1]*m[1,2] - m[0,2]*m[1,1];
            T s4 = m[0,1]*m[1,3] - m[0,3]*m[1,1];
            T s5 = m[0,2]*m[1,3] - m[0,3]*m[1,2];
            T c5 = m[2,2]*m[3,3] - m[2,3]*m[3,2];
            T c4 = m[2,1]*m[3,3] - m[2,3]*m[3,1];
            T c3 = m[2,1]*m[3,2] - m[2,2]*m[3,1];
            T c2 = m[2,0]*m[3,3] - m[2,3]*m[3,0];
            T c1 = m[2,0]*m[3,2] - m[2,2]*m[3,0];
            T c0 = m[2,0]*m[3,1] - m[2,1]*m[3,0];
            return s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
        }
    }

    /// @}

    /// @name Inverse (square matrices 2×2, 3×3, 4×4)
    /// @{

    /** @brief Inverse of a square matrix (2×2, 3×3, or 4×4). Assumes invertible. */
    template<typename T, int N>
        requires(N >= 2 && N <= 4)
    [[nodiscard]] constexpr Mat<T, N> Inverse(const Mat<T, N>& m) {
        if constexpr (N == 2) {
            T invDet = T(1) / Det(m);
            Mat<T, 2> r{};
            r[0, 0] = m[1, 1] * invDet;
            r[0, 1] = -m[0, 1] * invDet;
            r[1, 0] = -m[1, 0] * invDet;
            r[1, 1] = m[0, 0] * invDet;
            return r;
        } else if constexpr (N == 3) {
            T a11 = m[1, 1] * m[2, 2] - m[1, 2] * m[2, 1];
            T a10 = m[1, 0] * m[2, 2] - m[1, 2] * m[2, 0];
            T a09 = m[1, 0] * m[2, 1] - m[1, 1] * m[2, 0];
            T invDet = T(1) / (m[0, 0] * a11 - m[0, 1] * a10 + m[0, 2] * a09);
            Mat<T, 3> r{};
            r[0, 0] = a11 * invDet;
            r[0, 1] = (m[0, 2] * m[2, 1] - m[0, 1] * m[2, 2]) * invDet;
            r[0, 2] = (m[0, 1] * m[1, 2] - m[0, 2] * m[1, 1]) * invDet;
            r[1, 0] = -a10 * invDet;
            r[1, 1] = (m[0, 0] * m[2, 2] - m[0, 2] * m[2, 0]) * invDet;
            r[1, 2] = (m[0, 2] * m[1, 0] - m[0, 0] * m[1, 2]) * invDet;
            r[2, 0] = a09 * invDet;
            r[2, 1] = (m[0, 1] * m[2, 0] - m[0, 0] * m[2, 1]) * invDet;
            r[2, 2] = (m[0, 0] * m[1, 1] - m[0, 1] * m[1, 0]) * invDet;
            return r;
        } else {
            // Cofactor expansion along first row for determinant, then adjugate.
            T s0 = m[0,0]*m[1,1] - m[0,1]*m[1,0];
            T s1 = m[0,0]*m[1,2] - m[0,2]*m[1,0];
            T s2 = m[0,0]*m[1,3] - m[0,3]*m[1,0];
            T s3 = m[0,1]*m[1,2] - m[0,2]*m[1,1];
            T s4 = m[0,1]*m[1,3] - m[0,3]*m[1,1];
            T s5 = m[0,2]*m[1,3] - m[0,3]*m[1,2];

            T c5 = m[2,2]*m[3,3] - m[2,3]*m[3,2];
            T c4 = m[2,1]*m[3,3] - m[2,3]*m[3,1];
            T c3 = m[2,1]*m[3,2] - m[2,2]*m[3,1];
            T c2 = m[2,0]*m[3,3] - m[2,3]*m[3,0];
            T c1 = m[2,0]*m[3,2] - m[2,2]*m[3,0];
            T c0 = m[2,0]*m[3,1] - m[2,1]*m[3,0];

            T invDet = T(1) / (s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0);

            Mat<T, 4> r{};
            r[0,0] = ( m[1,1]*c5 - m[1,2]*c4 + m[1,3]*c3) * invDet;
            r[0,1] = (-m[0,1]*c5 + m[0,2]*c4 - m[0,3]*c3) * invDet;
            r[0,2] = ( m[3,1]*s5 - m[3,2]*s4 + m[3,3]*s3) * invDet;
            r[0,3] = (-m[2,1]*s5 + m[2,2]*s4 - m[2,3]*s3) * invDet;

            r[1,0] = (-m[1,0]*c5 + m[1,2]*c2 - m[1,3]*c1) * invDet;
            r[1,1] = ( m[0,0]*c5 - m[0,2]*c2 + m[0,3]*c1) * invDet;
            r[1,2] = (-m[3,0]*s5 + m[3,2]*s2 - m[3,3]*s1) * invDet;
            r[1,3] = ( m[2,0]*s5 - m[2,2]*s2 + m[2,3]*s1) * invDet;

            r[2,0] = ( m[1,0]*c4 - m[1,1]*c2 + m[1,3]*c0) * invDet;
            r[2,1] = (-m[0,0]*c4 + m[0,1]*c2 - m[0,3]*c0) * invDet;
            r[2,2] = ( m[3,0]*s4 - m[3,1]*s2 + m[3,3]*s0) * invDet;
            r[2,3] = (-m[2,0]*s4 + m[2,1]*s2 - m[2,3]*s0) * invDet;

            r[3,0] = (-m[1,0]*c3 + m[1,1]*c1 - m[1,2]*c0) * invDet;
            r[3,1] = ( m[0,0]*c3 - m[0,1]*c1 + m[0,2]*c0) * invDet;
            r[3,2] = (-m[3,0]*s3 + m[3,1]*s1 - m[3,2]*s0) * invDet;
            r[3,3] = ( m[2,0]*s3 - m[2,1]*s1 + m[2,2]*s0) * invDet;
            return r;
        }
    }

    /// @}

    /// @name Projection (generic, default `float`)
    /// @{

    /** @brief Perspective projection (Vulkan clip space: Y-down, Z [0,1]). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakePerspective(T fovY, T aspect, T nearZ, T farZ) {
        const T h = Tan(fovY * T(0.5));
        Mat<T, 4> m{};
        m[0, 0] = T(1) / (aspect * h);
        m[1, 1] = T(-1) / h; // Vulkan Y-flip
        m[2, 2] = farZ / (nearZ - farZ);
        m[3, 2] = T(-1);
        m[2, 3] = nearZ * farZ / (nearZ - farZ);
        return m;
    }

    /** @brief Perspective projection (YFlipped clip space: Y-up, Z [0,1]). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakePerspectiveYFlipped(T fovY, T aspect, T nearZ, T farZ) {
        const T h = Tan(fovY * T(0.5));
        Mat<T, 4> m{};
        m[0, 0] = T(1) / (aspect * h);
        m[1, 1] = T(1) / h;
        m[2, 2] = farZ / (nearZ - farZ);
        m[3, 2] = T(-1);
        m[2, 3] = nearZ * farZ / (nearZ - farZ);
        return m;
    }

    /** @brief Orthographic projection (Vulkan clip space: Y-down, Z [0,1]). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeOrtho(T l, T r, T b, T t, T n, T f) {
        Mat<T, 4> m{};
        m[0, 0] = T(2) / (r - l);
        m[1, 1] = T(-2) / (t - b); // Vulkan Y-flip
        m[2, 2] = T(1) / (n - f);
        m[0, 3] = -(r + l) / (r - l);
        m[1, 3] = (t + b) / (t - b);
        m[2, 3] = n / (n - f);
        m[3, 3] = T(1);
        return m;
    }

    /** @brief Orthographic projection (YFlipped clip space: Y-up, Z [0,1]). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeOrthoYFlipped(T l, T r, T b, T t, T n, T f) {
        Mat<T, 4> m{};
        m[0, 0] = T(2) / (r - l);
        m[1, 1] = T(2) / (t - b);
        m[2, 2] = T(1) / (n - f);
        m[0, 3] = -(r + l) / (r - l);
        m[1, 3] = -(t + b) / (t - b);
        m[2, 3] = n / (n - f);
        m[3, 3] = T(1);
        return m;
    }

    /// @}

} // namespace Mashiro::Math
