/**
 * @file Mat.h
 * @brief Generic column-major matrix template with reflection-generated operators.
 *
 * `Mat<T, R, C>` is an R-row, C-column matrix stored as C columns of `Vec<T, R>`.
 * The square case `Mat<T, N>` falls out via the defaulted column count `C = R`.
 * Alignment matches the column vector (std430-compatible by default).
 *
 * Dual subscript access (C++23 P2128R6):
 * | Expression   | Type            |
 * |--------------|-----------------|
 * | `m[col]`     | `Vec<T,R>&`     |
 * | `m[row,col]` | `T&`            |
 *
 * All matrix operators (add, sub, scalar mul/div, mat×mat, mat×vec, vec×mat,
 * compound assignments) are defined at namespace scope and found via ADL.
 * They are written against the concrete `Mat<T,R,C>` template so the result
 * shape is exact for non-square operands (e.g. `Mat<R,K> * Mat<K,C> → Mat<R,C>`).
 * Only `operator==` is declared in-class (`= default`).
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Math/Vec.h"
#include "Mashiro/Core/TypeTraits.h"

namespace Mashiro {

    /**
     * @brief Column-major matrix: R rows × C columns, stored as C columns of `Vec<T,R>`.
     *
     * @tparam T Arithmetic scalar type.
     * @tparam R Row count (2–4).
     * @tparam C Column count (2–4, defaults to R for square matrices).
     */
    template<typename T, int R, int C = R>
        requires(std::is_arithmetic_v<T> && (R >= 2) && (R <= 4) && (C >= 2) && (C <= 4))
    struct alignas(alignof(Vec<T, R>)) Mat {
        Vec<T, R> columns[C]; ///< Column-major storage.

        [[nodiscard]] constexpr auto&& operator[](this auto&& self, int col) {
            return self.columns[col];
        }
        [[nodiscard]] constexpr auto&& operator[](this auto&& self, int row, int col) {
            return self.columns[col][row];
        }

        [[nodiscard]] friend constexpr bool operator==(const Mat&, const Mat&) = default;
    };

    /// @name Matrix concept and compile-time queries
    /// @{

    /**
     * @brief A column-major matrix: a struct with a single `columns` array member
     *        whose element type satisfies HomogeneousVec, and that provides the
     *        dual subscript access m[col] / m[row, col].
     */
    template<typename T>
    concept ColumnMajorMat =
        Traits::Homogeneous<T> && requires(T m) {
            { m.columns };
        } && std::is_array_v<std::remove_cvref_t<decltype(std::declval<T&>().columns)>> &&
        requires(T m, int i, int j) {
            { m[i] };
            { m[i, j] };
        } && HomogeneousVec<std::remove_cvref_t<decltype(std::declval<T>()[0])>>;

    /** @brief Column vector type (Vec<T, R>) of a column-major matrix. */
    template<ColumnMajorMat M>
    using MatColType = std::remove_cvref_t<decltype(std::declval<M>()[0])>;

    /** @brief Row count R (length of each column vector). */
    template<ColumnMajorMat M>
    inline constexpr int MatRows = VecDim<MatColType<M>>;

    /** @brief Column count C (number of stored columns). */
    template<ColumnMajorMat M>
    inline constexpr int MatCols = static_cast<int>(
        std::extent_v<std::remove_cvref_t<decltype(std::declval<M&>().columns)>>);

    /** @brief Square dimension N; only defined when the matrix is square. */
    template<ColumnMajorMat M>
        requires(MatRows<M> == MatCols<M>)
    inline constexpr int MatDim = MatRows<M>;

    /// @}

    /// @name Matrix operators (namespace-scope, found via ADL)
    /// @{

    /** @brief Unary negation (signed scalars only). */
    template<typename T, int R, int C>
        requires std::is_signed_v<T>
    [[nodiscard]] constexpr Mat<T, R, C> operator-(const Mat<T, R, C>& a) {
        Mat<T, R, C> r{};
        template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
            r.columns[c] = -a.columns[c];
        }
        return r;
    }

    /** @brief Element-wise matrix addition. */
    template<typename T, int R, int C>
    [[nodiscard]] constexpr Mat<T, R, C> operator+(const Mat<T, R, C>& a, const Mat<T, R, C>& b) {
        Mat<T, R, C> r{};
        template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
            r.columns[c] = a.columns[c] + b.columns[c];
        }
        return r;
    }

    /** @brief Element-wise matrix subtraction. */
    template<typename T, int R, int C>
    [[nodiscard]] constexpr Mat<T, R, C> operator-(const Mat<T, R, C>& a, const Mat<T, R, C>& b) {
        Mat<T, R, C> r{};
        template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
            r.columns[c] = a.columns[c] - b.columns[c];
        }
        return r;
    }

    /** @brief Matrix scaled by scalar (right). */
    template<typename T, int R, int C>
    [[nodiscard]] constexpr Mat<T, R, C> operator*(const Mat<T, R, C>& a, T s) {
        Mat<T, R, C> r{};
        template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
            r.columns[c] = a.columns[c] * s;
        }
        return r;
    }

    /** @brief Matrix scaled by scalar (left). */
    template<typename T, int R, int C>
    [[nodiscard]] constexpr Mat<T, R, C> operator*(T s, const Mat<T, R, C>& a) {
        return a * s;
    }

    /** @brief Matrix divided by scalar. Uses reciprocal multiplication for floating-point. */
    template<typename T, int R, int C>
    [[nodiscard]] constexpr Mat<T, R, C> operator/(const Mat<T, R, C>& a, T s) {
        if constexpr (std::floating_point<T>) {
            return a * (T(1) / s);
        } else {
            Mat<T, R, C> r{};
            template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
                r.columns[c] = a.columns[c] / s;
            }
            return r;
        }
    }

    /** @brief Matrix × column vector: Mat<R,C> × Vec<C> → Vec<R>. Column linear combination. */
    template<typename T, int R, int C, AlignTag A>
    [[nodiscard]] constexpr Vec<T, R> operator*(const Mat<T, R, C>& a, const Vec<T, C, A>& v) {
        // r = v[0]*col[0] + v[1]*col[1] + ... — cache-friendly column access.
        Vec<T, R> r = a.columns[0] * v[0];
        template for (constexpr auto k : std::define_static_array(std::views::iota(1, C))) {
            r += a.columns[k] * v[k];
        }
        return r;
    }

    /** @brief Row vector × matrix: Vec<R> × Mat<R,C> → Vec<C> (vᵀ · M). */
    template<typename T, int R, int C, AlignTag A>
    [[nodiscard]] constexpr Vec<T, C> operator*(const Vec<T, R, A>& v, const Mat<T, R, C>& a) {
        // r[col] = dot(v, a.col[col]). Seed with first term.
        Vec<T, C> r;
        template for (constexpr auto col : std::define_static_array(std::views::iota(0, C))) {
            T sum = v[0] * a[0, col];
            template for (constexpr auto row : std::define_static_array(std::views::iota(1, R))) {
                sum += v[row] * a[row, col];
            }
            r[col] = sum;
        }
        return r;
    }

    /** @brief Matrix × matrix: Mat<R,K> × Mat<K,C> → Mat<R,C>. Column linear combination. */
    template<typename T, int R, int K, int C>
    [[nodiscard]] constexpr Mat<T, R, C> operator*(const Mat<T, R, K>& a, const Mat<T, K, C>& b) {
        // r.col[c] = a * b.col[c]  — each column of result is mat*vec.
        Mat<T, R, C> r;
        template for (constexpr auto col : std::define_static_array(std::views::iota(0, C))) {
            r.columns[col] = a * b.columns[col];
        }
        return r;
    }

    /** @brief Compound addition. */
    template<typename T, int R, int C>
    constexpr Mat<T, R, C>& operator+=(Mat<T, R, C>& a, const Mat<T, R, C>& b) {
        template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
            a.columns[c] += b.columns[c];
        }
        return a;
    }
    /** @brief Compound subtraction. */
    template<typename T, int R, int C>
    constexpr Mat<T, R, C>& operator-=(Mat<T, R, C>& a, const Mat<T, R, C>& b) {
        template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
            a.columns[c] -= b.columns[c];
        }
        return a;
    }
    /** @brief Compound scalar multiplication. */
    template<typename T, int R, int C>
    constexpr Mat<T, R, C>& operator*=(Mat<T, R, C>& a, T s) {
        template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
            a.columns[c] *= s;
        }
        return a;
    }
    /** @brief Compound scalar division. Uses reciprocal for floating-point. */
    template<typename T, int R, int C>
    constexpr Mat<T, R, C>& operator/=(Mat<T, R, C>& a, T s) {
        if constexpr (std::floating_point<T>) {
            a *= (T(1) / s);
        } else {
            template for (constexpr auto c : std::define_static_array(std::views::iota(0, C))) {
                a.columns[c] /= s;
            }
        }
        return a;
    }
    /** @brief Compound matrix multiplication (square matrices only). */
    template<typename T, int N>
    constexpr Mat<T, N, N>& operator*=(Mat<T, N, N>& a, const Mat<T, N, N>& b) {
        a = a * b;
        return a;
    }

    /// @}

} // namespace Mashiro
