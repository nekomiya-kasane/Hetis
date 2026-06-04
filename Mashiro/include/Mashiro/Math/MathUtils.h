/**
 * @file MathUtils.h
 * @brief Shared math utility functions for the Mashiro renderer.
 *
 * Header-only and fully `constexpr`. Vector/matrix functions are concept-constrained
 * templates that work with any `Vec<T,N>` / `Mat<T,N>` satisfying `HomogeneousVec` or
 * `ColumnMajorMat`. Transcendental math (`Sqrt`, `Sin`, `Cos`, …) is routed through
 * ScalarMath.h (hardware `std::` at run time, polynomial kernels during constant
 * evaluation), so projections and transforms fold to constants when inputs are known.
 *
 * Convention: column-major. Vulkan clip space: Y-down, Z [0,1].
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Math/Algebra.h"
#include "Mashiro/Math/Types.h"

namespace Mashiro::Math {

    

    /// @name Generic matrix functions (any ColumnMajorMat)
    /// @{

    /** @brief Transpose: Mat<T,R,C> → Mat<T,C,R>. Works for any shape. */
    template<typename T, int R, int C>
    [[nodiscard]] constexpr Mat<T, C, R> Transpose(const Mat<T, R, C>& m) {
        Mat<T, C, R> r{};
        for (int row = 0; row < R; ++row) {
            for (int col = 0; col < C; ++col) {
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
        for (int i = 0; i < MatDim<M>; ++i) {
            m[i, i] = S(1);
        }
        return m;
    }

    /// @}

    /**
     * @name 2D transform construction (column-major)
     *
     * Each builder has two flavours:
     * | Suffix        | Result              | Description                         |
     * |---------------|---------------------|-------------------------------------|
     * | `MakeXxx2D`   | `Mat<T,3>` (3×3)    | Full homogeneous matrix.            |
     * | `MakeXxx2DAffine` | `Mat<T,2,3>` (2×3) | Compact affine, omits `[0 0 1]` row. |
     * @{
     */

    /** @brief Compact 2D affine identity: [I₂ | 0] as Mat<T,2,3>. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 2, 3> IdentityAffine2D() {
        Mat<T, 2, 3> m{};
        m[0, 0] = T(1);
        m[1, 1] = T(1);
        return m;
    }

    /** @brief 3×3 homogeneous 2D translation. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3> MakeTranslation2D(Vec<T, 2> t) {
        auto m = Identity<Mat<T, 3>>();
        m[0, 2] = t.x;
        m[1, 2] = t.y;
        return m;
    }
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3> MakeTranslation2D(T x, T y) {
        return MakeTranslation2D<T>(Vec<T, 2>{x, y});
    }

    /** @brief Affine (2×3) 2D translation: [I₂ | t]. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 2, 3> MakeTranslation2DAffine(Vec<T, 2> t) {
        auto m = IdentityAffine2D<T>();
        m[0, 2] = t.x;
        m[1, 2] = t.y;
        return m;
    }
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 2, 3> MakeTranslation2DAffine(T x, T y) {
        return MakeTranslation2DAffine<T>(Vec<T, 2>{x, y});
    }

    /** @brief 3×3 homogeneous 2D non-uniform scale. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3> MakeScale2D(Vec<T, 2> s) {
        Mat<T, 3> m{};
        m[0, 0] = s.x;
        m[1, 1] = s.y;
        m[2, 2] = T(1);
        return m;
    }
    /** @brief 3×3 homogeneous 2D uniform scale. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3> MakeScale2D(T s) {
        return MakeScale2D<T>(Vec<T, 2>{s, s});
    }

    /** @brief Affine (2×3) 2D non-uniform scale: [diag(s) | 0]. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 2, 3> MakeScale2DAffine(Vec<T, 2> s) {
        Mat<T, 2, 3> m{};
        m[0, 0] = s.x;
        m[1, 1] = s.y;
        return m;
    }
    /** @brief Affine (2×3) 2D uniform scale. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 2, 3> MakeScale2DAffine(T s) {
        return MakeScale2DAffine<T>(Vec<T, 2>{s, s});
    }

    /** @brief 3×3 homogeneous 2D rotation by @p rad radians (counter-clockwise). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3> MakeRotate2D(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = Identity<Mat<T, 3>>();
        m[0, 0] = c;
        m[0, 1] = -s;
        m[1, 0] = s;
        m[1, 1] = c;
        return m;
    }

    /** @brief Affine (2×3) 2D rotation by @p rad radians (counter-clockwise). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 2, 3> MakeRotate2DAffine(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine2D<T>();
        m[0, 0] = c;
        m[0, 1] = -s;
        m[1, 0] = s;
        m[1, 1] = c;
        return m;
    }

    /**
     * @brief Fast inverse of a 2D affine transform stored as Mat<T,2,3>.
     *
     * Layout: columns 0–1 hold the 2×2 rotation/scale R, column 2 holds translation t.
     * Result: [R⁻¹ | −R⁻¹ · t].
     */
    template<typename T>
    [[nodiscard]] constexpr Mat<T, 2, 3> InverseAffine2D(const Mat<T, 2, 3>& m) {
        // 2×2 inverse of columns 0–1
        T det = m[0, 0] * m[1, 1] - m[0, 1] * m[1, 0];
        T invDet = T(1) / det;
        Mat<T, 2, 3> r{};
        r[0, 0] = m[1, 1] * invDet;
        r[0, 1] = -m[0, 1] * invDet;
        r[1, 0] = -m[1, 0] * invDet;
        r[1, 1] = m[0, 0] * invDet;
        // −R⁻¹ · t
        Vec<T, 2> t{m[0, 2], m[1, 2]};
        r[0, 2] = -(r[0, 0] * t.x + r[0, 1] * t.y);
        r[1, 2] = -(r[1, 0] * t.x + r[1, 1] * t.y);
        return r;
    }

    /// @}

    /**
     * @name 3D transform construction (column-major)
     *
     * Each builder has two flavours:
     * | Suffix          | Result                | Description                           |
     * |-----------------|-----------------------|---------------------------------------|
     * | `MakeXxx`       | `Mat<T,4>` (4×4)      | Full homogeneous matrix.              |
     * | `MakeXxxAffine` | `Mat<T,3,4>` (3×4)    | Compact affine, omits `[0 0 0 1]` row. |
     * @{
     */

    /** @brief Compact 3D affine identity: [I₃ | 0] as Mat<T,3,4>. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> IdentityAffine() {
        Mat<T, 3, 4> m{};
        m[0, 0] = T(1);
        m[1, 1] = T(1);
        m[2, 2] = T(1);
        return m;
    }

    /** @brief 4×4 translation matrix. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeTranslation(Vec<T, 3> t) {
        auto m = Identity<Mat<T, 4>>();
        m[0, 3] = t.x;
        m[1, 3] = t.y;
        m[2, 3] = t.z;
        return m;
    }
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeTranslation(T x, T y, T z) {
        return MakeTranslation<T>(Vec<T, 3>{x, y, z});
    }

    /** @brief Affine (3×4) translation: [I₃ | t]. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeTranslationAffine(Vec<T, 3> t) {
        auto m = IdentityAffine<T>();
        m[0, 3] = t.x;
        m[1, 3] = t.y;
        m[2, 3] = t.z;
        return m;
    }
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeTranslationAffine(T x, T y, T z) {
        return MakeTranslationAffine<T>(Vec<T, 3>{x, y, z});
    }

    /** @brief 4×4 non-uniform scale matrix. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeScale(Vec<T, 3> s) {
        Mat<T, 4> m{};
        m[0, 0] = s.x;
        m[1, 1] = s.y;
        m[2, 2] = s.z;
        m[3, 3] = T(1);
        return m;
    }
    /** @brief 4×4 uniform scale matrix. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeScale(T s) {
        return MakeScale<T>(Vec<T, 3>{s, s, s});
    }

    /** @brief Affine (3×4) non-uniform scale: [diag(s) | 0]. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeScaleAffine(Vec<T, 3> s) {
        Mat<T, 3, 4> m{};
        m[0, 0] = s.x;
        m[1, 1] = s.y;
        m[2, 2] = s.z;
        return m;
    }
    /** @brief Affine (3×4) uniform scale. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeScaleAffine(T s) {
        return MakeScaleAffine<T>(Vec<T, 3>{s, s, s});
    }

    /** @brief 4×4 rotation around the X axis by @p rad radians. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeRotateX(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = Identity<Mat<T, 4>>();
        m[1, 1] = c;
        m[1, 2] = -s;
        m[2, 1] = s;
        m[2, 2] = c;
        return m;
    }
    /** @brief Affine (3×4) rotation around the X axis. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeRotateXAffine(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine<T>();
        m[1, 1] = c;
        m[1, 2] = -s;
        m[2, 1] = s;
        m[2, 2] = c;
        return m;
    }

    /** @brief 4×4 rotation around the Y axis by @p rad radians. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeRotateY(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = Identity<Mat<T, 4>>();
        m[0, 0] = c;
        m[0, 2] = s;
        m[2, 0] = -s;
        m[2, 2] = c;
        return m;
    }
    /** @brief Affine (3×4) rotation around the Y axis. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeRotateYAffine(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine<T>();
        m[0, 0] = c;
        m[0, 2] = s;
        m[2, 0] = -s;
        m[2, 2] = c;
        return m;
    }

    /** @brief 4×4 rotation around the Z axis by @p rad radians. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeRotateZ(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = Identity<Mat<T, 4>>();
        m[0, 0] = c;
        m[0, 1] = -s;
        m[1, 0] = s;
        m[1, 1] = c;
        return m;
    }
    /** @brief Affine (3×4) rotation around the Z axis. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeRotateZAffine(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine<T>();
        m[0, 0] = c;
        m[0, 1] = -s;
        m[1, 0] = s;
        m[1, 1] = c;
        return m;
    }

    /** @brief 4×4 rotation around an arbitrary unit @p axis by @p rad radians (Rodrigues). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeRotateAxis(Vec<T, 3> axis, T rad) {
        Vec<T, 3> a = Normalize(axis);
        auto [s, c] = SinCos(rad);
        T k = T(1) - c;
        auto m = Identity<Mat<T, 4>>();
        m[0, 0] = k * a.x * a.x + c;
        m[0, 1] = k * a.x * a.y - s * a.z;
        m[0, 2] = k * a.x * a.z + s * a.y;
        m[1, 0] = k * a.x * a.y + s * a.z;
        m[1, 1] = k * a.y * a.y + c;
        m[1, 2] = k * a.y * a.z - s * a.x;
        m[2, 0] = k * a.x * a.z - s * a.y;
        m[2, 1] = k * a.y * a.z + s * a.x;
        m[2, 2] = k * a.z * a.z + c;
        return m;
    }
    /** @brief Affine (3×4) rotation around an arbitrary axis (Rodrigues). */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeRotateAxisAffine(Vec<T, 3> axis, T rad) {
        Vec<T, 3> a = Normalize(axis);
        auto [s, c] = SinCos(rad);
        T k = T(1) - c;
        auto m = IdentityAffine<T>();
        m[0, 0] = k * a.x * a.x + c;
        m[0, 1] = k * a.x * a.y - s * a.z;
        m[0, 2] = k * a.x * a.z + s * a.y;
        m[1, 0] = k * a.x * a.y + s * a.z;
        m[1, 1] = k * a.y * a.y + c;
        m[1, 2] = k * a.y * a.z - s * a.x;
        m[2, 0] = k * a.x * a.z - s * a.y;
        m[2, 1] = k * a.y * a.z + s * a.x;
        m[2, 2] = k * a.z * a.z + c;
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
            r[2, 3] = invDet * -(m[0, 0] * a1312 - m[0, 1] * a0312 + m[0, 3] * a0112);
            r[3, 0] = invDet * -(m[1, 0] * a1223 - m[1, 1] * a0223 + m[1, 2] * a0123);
            r[3, 1] = invDet * (m[0, 0] * a1223 - m[0, 1] * a0223 + m[0, 2] * a0123);
            r[3, 2] = invDet * -(m[0, 0] * a1213 - m[0, 1] * a0213 + m[0, 2] * a0113);
            r[3, 3] = invDet * (m[0, 0] * a1212 - m[0, 1] * a0212 + m[0, 2] * a0112);
            return r;
        }
    }

    /**
     * @brief Fast inverse of an affine transform stored as mat3x4 (3 rows × 4 cols).
     *
     * Layout: columns 0–2 hold the 3×3 rotation/scale R, column 3 holds translation t.
     * Result: [R^-1 | -R^-1 * t]. Uses the general 3×3 inverse for R.
     */
    template<typename T>
    [[nodiscard]] constexpr Mat<T, 3, 4> InverseAffine(const Mat<T, 3, 4>& m) {
        // Extract the 3×3 upper-left
        Mat<T, 3> R{};
        for (int c = 0; c < 3; ++c) {
            R[c] = m[c];
        }
        Mat<T, 3> Ri = Inverse(R);

        // Translation column
        Vec<T, 3> t = m[3]; // column 3
        // -Ri * t
        Vec<T, 3> nt{};
        for (int row = 0; row < 3; ++row) {
            T sum{};
            for (int k = 0; k < 3; ++k) {
                sum += Ri[row, k] * t[k];
            }
            nt[row] = -sum;
        }

        Mat<T, 3, 4> r{};
        for (int c = 0; c < 3; ++c) {
            r[c] = Ri[c];
        }
        r[3] = nt;
        return r;
    }

    /// @}

    /// @name Camera / projection (generic, default `float`)
    /// @{

    /** @brief Right-handed look-at view matrix (4×4). Maps world → view space. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 4> MakeLookAt(Vec<T, 3> eye, Vec<T, 3> target, Vec<T, 3> up) {
        Vec<T, 3> f = Normalize(target - eye);
        Vec<T, 3> s = Normalize(Cross(f, up));
        Vec<T, 3> u = Cross(s, f);
        auto m = Identity<Mat<T, 4>>();
        m[0, 0] = s.x;
        m[1, 0] = u.x;
        m[2, 0] = -f.x;
        m[0, 1] = s.y;
        m[1, 1] = u.y;
        m[2, 1] = -f.y;
        m[0, 2] = s.z;
        m[1, 2] = u.z;
        m[2, 2] = -f.z;
        m[0, 3] = -Dot(s, eye);
        m[1, 3] = -Dot(u, eye);
        m[2, 3] = Dot(f, eye);
        return m;
    }

    /** @brief Affine (3×4) look-at view matrix. Same as MakeLookAt but omits the [0 0 0 1] row. */
    template<std::floating_point T = float>
    [[nodiscard]] constexpr Mat<T, 3, 4> MakeLookAtAffine(Vec<T, 3> eye, Vec<T, 3> target,
                                                          Vec<T, 3> up) {
        Vec<T, 3> f = Normalize(target - eye);
        Vec<T, 3> s = Normalize(Cross(f, up));
        Vec<T, 3> u = Cross(s, f);
        auto m = IdentityAffine<T>();
        m[0, 0] = s.x;
        m[1, 0] = u.x;
        m[2, 0] = -f.x;
        m[0, 1] = s.y;
        m[1, 1] = u.y;
        m[2, 1] = -f.y;
        m[0, 2] = s.z;
        m[1, 2] = u.z;
        m[2, 2] = -f.z;
        m[0, 3] = -Dot(s, eye);
        m[1, 3] = -Dot(u, eye);
        m[2, 3] = Dot(f, eye);
        return m;
    }

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
