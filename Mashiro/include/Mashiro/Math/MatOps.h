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
            T a2323 = m[2, 2] * m[3, 3] - m[2, 3] * m[3, 2];
            T a1323 = m[2, 1] * m[3, 3] - m[2, 3] * m[3, 1];
            T a1223 = m[2, 1] * m[3, 2] - m[2, 2] * m[3, 1];
            T a0323 = m[2, 0] * m[3, 3] - m[2, 3] * m[3, 0];
            T a0223 = m[2, 0] * m[3, 2] - m[2, 2] * m[3, 0];
            T a0123 = m[2, 0] * m[3, 1] - m[2, 1] * m[3, 0];
            return m[0, 0] * (m[1, 1] * a2323 - m[1, 2] * a1323 + m[1, 3] * a1223) -
                   m[0, 1] * (m[1, 0] * a2323 - m[1, 2] * a0323 + m[1, 3] * a0223) +
                   m[0, 2] * (m[1, 0] * a1323 - m[1, 1] * a0323 + m[1, 3] * a0123) -
                   m[0, 3] * (m[1, 0] * a1223 - m[1, 1] * a0223 + m[1, 2] * a0123);
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
            T a2323 = m[2, 2] * m[3, 3] - m[2, 3] * m[3, 2];
            T a1323 = m[2, 1] * m[3, 3] - m[2, 3] * m[3, 1];
            T a1223 = m[2, 1] * m[3, 2] - m[2, 2] * m[3, 1];
            T a0323 = m[2, 0] * m[3, 3] - m[2, 3] * m[3, 0];
            T a0223 = m[2, 0] * m[3, 2] - m[2, 2] * m[3, 0];
            T a0123 = m[2, 0] * m[3, 1] - m[2, 1] * m[3, 0];
            T a2313 = m[1, 2] * m[3, 3] - m[1, 3] * m[3, 2];
            T a1313 = m[1, 1] * m[3, 3] - m[1, 3] * m[3, 1];
            T a1213 = m[1, 1] * m[3, 2] - m[1, 2] * m[3, 1];
            T a2312 = m[1, 2] * m[2, 3] - m[1, 3] * m[2, 2];
            T a1312 = m[1, 1] * m[2, 3] - m[1, 3] * m[2, 1];
            T a1212 = m[1, 1] * m[2, 2] - m[1, 2] * m[2, 1];
            T a0313 = m[1, 0] * m[3, 3] - m[1, 3] * m[3, 0];
            T a0213 = m[1, 0] * m[3, 2] - m[1, 2] * m[3, 0];
            T a0312 = m[1, 0] * m[2, 3] - m[1, 3] * m[2, 0];
            T a0212 = m[1, 0] * m[2, 2] - m[1, 2] * m[2, 0];
            T a0113 = m[1, 0] * m[3, 1] - m[1, 1] * m[3, 0];
            T a0112 = m[1, 0] * m[2, 1] - m[1, 1] * m[2, 0];

            T invDet = T(1) / (m[0, 0] * (m[1, 1] * a2323 - m[1, 2] * a1323 + m[1, 3] * a1223) -
                               m[0, 1] * (m[1, 0] * a2323 - m[1, 2] * a0323 + m[1, 3] * a0223) +
                               m[0, 2] * (m[1, 0] * a1323 - m[1, 1] * a0323 + m[1, 3] * a0123) -
                               m[0, 3] * (m[1, 0] * a1223 - m[1, 1] * a0223 + m[1, 2] * a0123));

            Mat<T, 4> r{};
            r[0, 0] = invDet * (m[1, 1] * a2323 - m[1, 2] * a1323 + m[1, 3] * a1223);
            r[0, 1] = invDet * -(m[0, 1] * a2323 - m[0, 2] * a1323 + m[0, 3] * a1223);
            r[0, 2] = invDet * (m[0, 1] * a2313 - m[0, 2] * a1313 + m[0, 3] * a1213);
            r[0, 3] = invDet * -(m[0, 1] * a2312 - m[0, 2] * a1312 + m[0, 3] * a1212);
            r[1, 0] = invDet * -(m[1, 0] * a2323 - m[1, 2] * a0323 + m[1, 3] * a0223);
            r[1, 1] = invDet * (m[0, 0] * a2323 - m[0, 2] * a0323 + m[0, 3] * a0223);
            r[1, 2] = invDet * -(m[0, 0] * a2313 - m[0, 2] * a0313 + m[0, 3] * a0213);
            r[1, 3] = invDet * (m[0, 0] * a2312 - m[0, 2] * a0312 + m[0, 3] * a0212);
            r[2, 0] = invDet * (m[1, 0] * a1323 - m[1, 1] * a0323 + m[1, 3] * a0123);
            r[2, 1] = invDet * -(m[0, 0] * a1323 - m[0, 1] * a0323 + m[0, 3] * a0123);
            r[2, 2] = invDet * (m[0, 0] * a1313 - m[0, 1] * a0313 + m[0, 3] * a0113);
            r[2, 3] = invDet * -(m[0, 0] * a1312 - m[0, 1] * a0312 + m[0, 2] * a0112);
            r[3, 0] = invDet * -(m[1, 0] * a1223 - m[1, 1] * a0223 + m[1, 2] * a0123);
            r[3, 1] = invDet * (m[0, 0] * a1223 - m[0, 1] * a0223 + m[0, 2] * a0123);
            r[3, 2] = invDet * -(m[0, 0] * a1213 - m[0, 1] * a0213 + m[0, 2] * a0113);
            r[3, 3] = invDet * (m[0, 0] * a1212 - m[0, 1] * a0212 + m[0, 2] * a0112);
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
