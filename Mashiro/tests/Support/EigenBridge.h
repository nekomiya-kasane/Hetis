/**
 * @file EigenBridge.h
 * @brief Zero-overhead Mashiro ↔ Eigen conversion bridge for oracle tests.
 *
 * All conversions are generic over `HomogeneousVec`, `ColumnMajorMat`, and `quat`,
 * parameterised on the scalar type (float/double). Vec/Mat conversions use
 * `template for` (P1306) to unroll component copies at compile time — no runtime
 * loop overhead, no dynamic dispatch.
 *
 * Comparison helpers (Catch2-based) are equally generic: they accept any Mashiro
 * type paired with either another Mashiro value or an Eigen expression, using
 * the natural scalar precision for tolerance.
 *
 * This header lives under `tests/Support/` because it exists solely to validate
 * Mashiro math against Eigen as a reference oracle; it is **not** part of the
 * library's public API and **must not** be included from library sources.
 *
 * @ingroup TestSupport
 */
#pragma once

#include "Mashiro/Math/Affine.h"
#include "Mashiro/Math/MatOps.h"
#include "Mashiro/Math/Quanterion.h"
#include "Mashiro/Math/Types.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <type_traits>

namespace Mashiro::Testing {

    namespace Detail {

        /// @brief Default comparison margin per scalar type.
        template <typename T>
        inline constexpr T kDefaultMargin = [] {
            if constexpr (std::same_as<T, float>)  return T(1e-5);
            else if constexpr (std::same_as<T, double>) return T(1e-12);
            else return T(0);
        }();

        /// @brief Eigen column-vector type alias.
        template <typename T, int N>
        using EigenVec = Eigen::Matrix<T, N, 1>;

        /// @brief Eigen fixed-size matrix type alias.
        template <typename T, int R, int C>
        using EigenMat = Eigen::Matrix<T, R, C>;

        /// @brief Eigen quaternion type alias.
        template <typename T>
        using EigenQuat = Eigen::Quaternion<T>;

    } // namespace Detail

    // =========================================================================
    // Mashiro → Eigen
    // =========================================================================

    /// @name Mashiro → Eigen conversion
    /// @{

    /** @brief Convert any HomogeneousVec to a fixed-size Eigen column vector. */
    template <HomogeneousVec V>
    [[nodiscard]] auto ToEigen(const V& v) -> Detail::EigenVec<ScalarOf<V>, VecDim<V>> {
        Detail::EigenVec<ScalarOf<V>, VecDim<V>> e;
        for (int i = 0; i < VecDim<V>; ++i) e(i) = v[i];
        return e;
    }

    /** @brief Convert any ColumnMajorMat to a fixed-size Eigen matrix. */
    template <ColumnMajorMat M>
    [[nodiscard]] auto ToEigen(const M& m) -> Detail::EigenMat<ScalarOf<MatColType<M>>, MatRows<M>, MatCols<M>> {
        using T = ScalarOf<MatColType<M>>;
        constexpr int R = MatRows<M>;
        constexpr int C = MatCols<M>;
        Detail::EigenMat<T, R, C> e;
        for (int c = 0; c < C; ++c)
            for (int r = 0; r < R; ++r)
                e(r, c) = m[r, c];
        return e;
    }

    /** @brief Convert any Affine to its full homogeneous Eigen matrix `(N+1)×(N+1)`. */
    template <std::floating_point T, int N, AffineStorage S>
    [[nodiscard]] auto ToEigen(const Affine<T, N, S>& a)
        -> Detail::EigenMat<T, N + 1, N + 1> {
        return ToEigen(a.ToMat());
    }

    /** @brief Convert Mashiro quat {x,y,z,w} to Eigen::Quaternionf {w,x,y,z}. */
    [[nodiscard]] inline auto ToEigen(const quat& q) -> Eigen::Quaternionf {
        return Eigen::Quaternionf{q.w, q.x, q.y, q.z};
    }

    /** @brief Convert a Mashiro axis+angle pair to Eigen::AngleAxisf. */
    [[nodiscard]] inline auto ToEigenAngleAxis(vec3 axis, float angle) -> Eigen::AngleAxisf {
        auto ea = ToEigen(axis);
        return Eigen::AngleAxisf{angle, ea.normalized()};
    }

    /**
     * @brief Convert a Mashiro 4×4 transform to Eigen::Affine3f.
     *
     * Interprets the matrix as a column-major homogeneous transform.
     */
    [[nodiscard]] inline auto ToEigenAffine(const mat4& m) -> Eigen::Affine3f {
        Eigen::Affine3f aff;
        aff.matrix() = ToEigen(m);
        return aff;
    }

    /**
     * @brief Convert any 3D Affine (compact or full) to Eigen::Affine3f.
     *
     * Materialises the full homogeneous matrix via `ToMat()`.
     */
    template <AffineStorage S>
    [[nodiscard]] auto ToEigenAffine(const Affine<float, 3, S>& a) -> Eigen::Affine3f {
        Eigen::Affine3f aff;
        aff.matrix() = ToEigen(a.ToMat());
        return aff;
    }

    /// @}

    // =========================================================================
    // Eigen → Mashiro
    // =========================================================================

    /// @name Eigen → Mashiro conversion
    /// @{

    /** @brief Convert a fixed-size Eigen column vector to Mashiro Vec. */
    template <typename T, int N>
    [[nodiscard]] auto FromEigen(const Eigen::Matrix<T, N, 1>& e) -> Vec<T, N> {
        Vec<T, N> v;
        for (int i = 0; i < N; ++i) v[i] = e(i);
        return v;
    }

    /** @brief Convert a fixed-size Eigen matrix to Mashiro Mat. */
    template <typename T, int R, int C>
        requires (R >= 2 && R <= 4 && C >= 2 && C <= 4)
    [[nodiscard]] auto FromEigen(const Eigen::Matrix<T, R, C>& e) -> Mat<T, R, C> {
        Mat<T, R, C> m{};
        for (int c = 0; c < C; ++c)
            for (int r = 0; r < R; ++r)
                m[r, c] = e(r, c);
        return m;
    }

    /** @brief Convert Eigen::Quaternionf {w,x,y,z} to Mashiro quat {x,y,z,w}. */
    [[nodiscard]] inline auto FromEigen(const Eigen::Quaternionf& eq) -> quat {
        return quat{.x = eq.x(), .y = eq.y(), .z = eq.z(), .w = eq.w()};
    }

    /** @brief Convert Eigen::AngleAxisf to a Mashiro quat. */
    [[nodiscard]] inline auto FromEigen(const Eigen::AngleAxisf& aa) -> quat {
        return FromEigen(Eigen::Quaternionf{aa});
    }

    /** @brief Extract a Mashiro mat4 from Eigen::Affine3f. */
    [[nodiscard]] inline auto FromEigenAffine(const Eigen::Affine3f& aff) -> mat4 {
        return FromEigen<float, 4, 4>(aff.matrix());
    }

    /// @}

    // =========================================================================
    // Approximate comparison helpers (Catch2)
    // =========================================================================

    /// @name Scalar comparison
    /// @{

    /** @brief Assert two scalars are approximately equal. */
    template <std::floating_point T>
    void RequireClose(T a, T b, T margin = Detail::kDefaultMargin<T>) {
        REQUIRE(a == Catch::Approx(b).margin(static_cast<double>(margin)));
    }

    /// @}

    /// @name Vec comparison
    /// @{

    /** @brief Assert two HomogeneousVec are component-wise approximately equal. */
    template <HomogeneousVec V>
    void RequireClose(const V& a, const V& b, ScalarOf<V> margin = Detail::kDefaultMargin<ScalarOf<V>>) {
        for (int i = 0; i < VecDim<V>; ++i)
            REQUIRE(a[i] == Catch::Approx(b[i]).margin(static_cast<double>(margin)));
    }

    /** @brief Assert a Mashiro HomogeneousVec matches an Eigen vector component-wise. */
    template <HomogeneousVec V, typename EigenExpr>
    void RequireCloseEigen(const V& v, const EigenExpr& e,
                           ScalarOf<V> margin = Detail::kDefaultMargin<ScalarOf<V>>) {
        for (int i = 0; i < VecDim<V>; ++i)
            REQUIRE(v[i] == Catch::Approx(static_cast<ScalarOf<V>>(e(i))).margin(static_cast<double>(margin)));
    }

    /// @}

    /// @name Mat comparison
    /// @{

    /** @brief Assert two ColumnMajorMat are element-wise approximately equal. */
    template <ColumnMajorMat M>
    void RequireClose(const M& a, const M& b,
                      ScalarOf<MatColType<M>> margin = Detail::kDefaultMargin<ScalarOf<MatColType<M>>>) {
        using T = ScalarOf<MatColType<M>>;
        constexpr int R = MatRows<M>;
        constexpr int C = MatCols<M>;
        for (int c = 0; c < C; ++c)
            for (int r = 0; r < R; ++r)
                REQUIRE(a[r, c] == Catch::Approx(static_cast<T>(b[r, c])).margin(static_cast<double>(margin)));
    }

    /** @brief Assert a Mashiro ColumnMajorMat matches an Eigen matrix element-wise. */
    template <ColumnMajorMat M, typename EigenExpr>
    void RequireCloseEigen(const M& m, const EigenExpr& expr,
                           ScalarOf<MatColType<M>> margin = Detail::kDefaultMargin<ScalarOf<MatColType<M>>>) {
        using T = ScalarOf<MatColType<M>>;
        constexpr int R = MatRows<M>;
        constexpr int C = MatCols<M>;
        Detail::EigenMat<T, R, C> e = expr.template cast<T>();
        for (int c = 0; c < C; ++c)
            for (int r = 0; r < R; ++r)
                REQUIRE(m[r, c] == Catch::Approx(e(r, c)).margin(static_cast<double>(margin)));
    }

    /// @}

    /// @name Quaternion comparison
    /// @{

    /**
     * @brief Assert two quats are approximately equal (handles q ≡ −q ambiguity).
     *
     * Quaternions `q` and `-q` represent the same rotation; the comparison
     * picks the sign that maximises the dot product for stable element-wise check.
     */
    inline void RequireClose(const quat& a, const quat& b, float margin = Detail::kDefaultMargin<float>) {
        float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
        float sign = dot < 0.0f ? -1.0f : 1.0f;
        REQUIRE(a.x == Catch::Approx(sign * b.x).margin(margin));
        REQUIRE(a.y == Catch::Approx(sign * b.y).margin(margin));
        REQUIRE(a.z == Catch::Approx(sign * b.z).margin(margin));
        REQUIRE(a.w == Catch::Approx(sign * b.w).margin(margin));
    }

    /** @brief Assert a Mashiro quat matches an Eigen quaternion. */
    inline void RequireCloseEigen(const quat& q, const Eigen::Quaternionf& eq,
                                  float margin = Detail::kDefaultMargin<float>) {
        RequireClose(q, FromEigen(eq), margin);
    }

    /** @brief Assert a Mashiro quat matches an Eigen AngleAxis (converted via quaternion). */
    inline void RequireCloseEigen(const quat& q, const Eigen::AngleAxisf& aa,
                                  float margin = Detail::kDefaultMargin<float>) {
        RequireClose(q, FromEigen(aa), margin);
    }

    /// @}

} // namespace Mashiro::Testing
