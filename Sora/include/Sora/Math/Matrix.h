/**
 * @file Matrix.h
 * @brief Trivial fixed-size matrices over scalar, SIMD, and extensible mathematical carriers.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Vector.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <mdspan>
#include <meta>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    namespace Detail {

        template<MatrixLayout Layout>
        using MatrixMdspanLayout =
            std::conditional_t<Layout == MatrixLayout::ColumnMajor, std::layout_left, std::layout_right>;

    } // namespace Detail

    /**
     * @brief Non-owning multidimensional view over fixed-size contiguous matrix storage.
     * @tparam T Element type, optionally const-qualified.
     * @tparam Rows Positive compile-time row count.
     * @tparam Columns Positive compile-time column count.
     * @tparam Layout Physical storage order.
     */
    template<typename T, size_t Rows, size_t Columns, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(Rows > 0 && Columns > 0 &&
                 (Layout == MatrixLayout::ColumnMajor || Layout == MatrixLayout::RowMajor))
    using MatrixView = std::mdspan<T, std::extents<size_t, Rows, Columns>, Detail::MatrixMdspanLayout<Layout>>;

    /**
     * @brief Fixed-size linear map from an @p Columns-dimensional module to an @p Rows-dimensional module.
     * @tparam T Scalar, SIMD pack, or user carrier supported by the mathematical CPO backend.
     * @tparam Rows Positive compile-time row count.
     * @tparam Columns Positive compile-time column count.
     * @tparam Layout Physical storage order; it does not change logical indexing or constructor argument order.
     * @details Storage is exactly one @c std::array<T,Rows*Columns>. Constructor values are always supplied in logical
     * row-major order, while @p Layout controls only their physical placement.
     */
    template<ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(Rows > 0 && Columns > 0 &&
                 (Layout == MatrixLayout::ColumnMajor || Layout == MatrixLayout::RowMajor))
    using Matrix = BasicTensor<StaticDenseStorage<T, StaticShape<Rows, Columns>, MatrixMapping<Layout>>,
                               Algebra::LinearMap>;

    namespace Detail {

        template<ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout, typename Generator,
                 size_t... Indices>
            requires requires(Generator& generator) { Matrix<T, Rows, Columns, Layout>{generator(Indices)...}; }
        [[nodiscard]] constexpr Matrix<T, Rows, Columns, Layout>
        GenerateMatrix(Generator& generator, std::index_sequence<Indices...>) noexcept(
            noexcept(Matrix<T, Rows, Columns, Layout>{generator(Indices)...})) {
            return Matrix<T, Rows, Columns, Layout>{generator(Indices)...};
        }

        template<ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout, typename Generator>
            requires requires(Generator& generator) {
                GenerateMatrix<T, Rows, Columns, Layout>(generator, std::make_index_sequence<Rows * Columns>{});
            }
        [[nodiscard]] constexpr Matrix<T, Rows, Columns, Layout> GenerateMatrix(Generator&& generator) noexcept(
            noexcept(GenerateMatrix<T, Rows, Columns, Layout>(generator, std::make_index_sequence<Rows * Columns>{}))) {
            return GenerateMatrix<T, Rows, Columns, Layout>(generator, std::make_index_sequence<Rows * Columns>{});
        }

        template<typename T, typename U>
        concept LinearCombinable = requires(T left, U right, Traits::MulResult<T, U> sum) {
            sum = Sora::Math::Add(Sora::Math::Mul(left, right), sum);
            sum = Sora::Math::Fma(left, right, sum);
        } && ValueCarrier<Traits::MulResult<T, U>>;

        template<typename T, typename U>
            requires LinearCombinable<T, U>
        inline constexpr bool kNothrowLinearCombination = [] {
            using R = Traits::MulResult<T, U>;
            using Product = decltype(Sora::Math::Mul(std::declval<T>(), std::declval<U>()));
            using Sum = decltype(Sora::Math::Add(std::declval<Product>(), std::declval<R&>()));
            using Fused = decltype(Sora::Math::Fma(std::declval<T>(), std::declval<U>(), std::declval<R&>()));
            return noexcept(Sora::Math::Mul(std::declval<T>(), std::declval<U>())) &&
                   std::is_nothrow_constructible_v<R, Product> &&
                   noexcept(Sora::Math::Add(std::declval<Product>(), std::declval<R&>())) &&
                   std::is_nothrow_assignable_v<R&, Sum> &&
                   noexcept(Sora::Math::Fma(std::declval<T>(), std::declval<U>(), std::declval<R&>())) &&
                   std::is_nothrow_assignable_v<R&, Fused> && std::is_nothrow_move_constructible_v<R>;
        }();

    } // namespace Detail

    /** @brief Canonical short name for a fixed-size matrix. */
    template<ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(Rows > 0 && Columns > 0 &&
                 (Layout == MatrixLayout::ColumnMajor || Layout == MatrixLayout::RowMajor))
    using Mat = Matrix<T, Rows, Columns, Layout>;

    /** @brief Add two matrices componentwise regardless of physical layout. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) { Sora::Math::Add(left, right); } && ValueCarrier<Traits::AddResult<T, U>>
    [[nodiscard]] constexpr auto
    operator+(const Matrix<T, Rows, Columns, LeftLayout>& left,
              const Matrix<U, Rows, Columns, RightLayout>&
                  right) noexcept(noexcept(Sora::Math::Add(left(0, 0), right(0, 0))) &&
                                  std::is_nothrow_constructible_v<Traits::AddResult<T, U>,
                                                                  decltype(Sora::Math::Add(left(0, 0), right(0, 0)))>)
        -> Matrix<Traits::AddResult<T, U>, Rows, Columns, LeftLayout> {
        return Detail::GenerateMatrix<Traits::AddResult<T, U>, Rows, Columns, LeftLayout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Add(left(0, 0), right(0, 0)))) {
                const size_t row = index / Columns;
                const size_t column = index % Columns;
                return Sora::Math::Add(left(row, column), right(row, column));
            });
    }

    /** @brief Subtract two matrices componentwise regardless of physical layout. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } && ValueCarrier<Traits::SubResult<T, U>>
    [[nodiscard]] constexpr auto
    operator-(const Matrix<T, Rows, Columns, LeftLayout>& left,
              const Matrix<U, Rows, Columns, RightLayout>&
                  right) noexcept(noexcept(Sora::Math::Sub(left(0, 0), right(0, 0))) &&
                                  std::is_nothrow_constructible_v<Traits::SubResult<T, U>,
                                                                  decltype(Sora::Math::Sub(left(0, 0), right(0, 0)))>)
        -> Matrix<Traits::SubResult<T, U>, Rows, Columns, LeftLayout> {
        return Detail::GenerateMatrix<Traits::SubResult<T, U>, Rows, Columns, LeftLayout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Sub(left(0, 0), right(0, 0)))) {
                const size_t row = index / Columns;
                const size_t column = index % Columns;
                return Sora::Math::Sub(left(row, column), right(row, column));
            });
    }

    /** @brief Multiply every matrix component by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(T value, S scale) { Sora::Math::Mul(value, scale); } && ValueCarrier<Traits::MulResult<T, S>>
    [[nodiscard]] constexpr auto operator*(const Matrix<T, Rows, Columns, Layout>& value, const S& scale) noexcept(
        noexcept(Sora::Math::Mul(value(0, 0), scale)) &&
        std::is_nothrow_constructible_v<Traits::MulResult<T, S>, decltype(Sora::Math::Mul(value(0, 0), scale))>)
        -> Matrix<Traits::MulResult<T, S>, Rows, Columns, Layout> {
        return Detail::GenerateMatrix<Traits::MulResult<T, S>, Rows, Columns, Layout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Mul(value(0, 0), scale))) {
                return Sora::Math::Mul(value(index / Columns, index % Columns), scale);
            });
    }

    /** @brief Multiply every matrix component by a scalar or broadcast-compatible carrier. */
    template<typename S, ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(S scale, T value) { Sora::Math::Mul(scale, value); } && ValueCarrier<Traits::MulResult<S, T>>
    [[nodiscard]] constexpr auto operator*(const S& scale, const Matrix<T, Rows, Columns, Layout>& value) noexcept(
        noexcept(Sora::Math::Mul(scale, value(0, 0))) &&
        std::is_nothrow_constructible_v<Traits::MulResult<S, T>, decltype(Sora::Math::Mul(scale, value(0, 0)))>)
        -> Matrix<Traits::MulResult<S, T>, Rows, Columns, Layout> {
        return Detail::GenerateMatrix<Traits::MulResult<S, T>, Rows, Columns, Layout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Mul(scale, value(0, 0)))) {
                return Sora::Math::Mul(scale, value(index / Columns, index % Columns));
            });
    }

    /** @brief Divide every matrix component by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(T value, S scale) { Sora::Math::Div(value, scale); } && ValueCarrier<Traits::DivResult<T, S>>
    [[nodiscard]] constexpr auto operator/(const Matrix<T, Rows, Columns, Layout>& value, const S& scale) noexcept(
        noexcept(Sora::Math::Div(value(0, 0), scale)) &&
        std::is_nothrow_constructible_v<Traits::DivResult<T, S>, decltype(Sora::Math::Div(value(0, 0), scale))>)
        -> Matrix<Traits::DivResult<T, S>, Rows, Columns, Layout> {
        return Detail::GenerateMatrix<Traits::DivResult<T, S>, Rows, Columns, Layout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Div(value(0, 0), scale))) {
                return Sora::Math::Div(value(index / Columns, index % Columns), scale);
            });
    }

    /** @brief Apply a matrix to a column vector. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires Detail::LinearCombinable<T, U>
    [[nodiscard]] constexpr auto
    operator*(const Matrix<T, Rows, Columns, Layout>& matrix,
              const Vector<U, Columns>& vector) noexcept(Detail::kNothrowLinearCombination<T, U>)
        -> Vector<Traits::MulResult<T, U>, Rows> {
        using R = Traits::MulResult<T, U>;
        return Detail::GenerateVector<R, Rows>([&](size_t row) noexcept(Detail::kNothrowLinearCombination<T, U>) {
            auto sum = Sora::Math::Mul(matrix(row, 0), vector[0]);
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{1}, Columns))) {
                if consteval {
                    sum = Sora::Math::Add(Sora::Math::Mul(matrix(row, column), vector[column]), sum);
                } else {
                    sum = Sora::Math::Fma(matrix(row, column), vector[column], sum);
                }
            }
            return sum;
        });
    }

    /** @brief Compose two fixed-size linear maps. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Inner, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires Detail::LinearCombinable<T, U>
    [[nodiscard]] constexpr auto
    operator*(const Matrix<T, Rows, Inner, LeftLayout>& left,
              const Matrix<U, Inner, Columns, RightLayout>& right) noexcept(Detail::kNothrowLinearCombination<T, U>)
        -> Matrix<Traits::MulResult<T, U>, Rows, Columns, LeftLayout> {
        using R = Traits::MulResult<T, U>;
        return Detail::GenerateMatrix<R, Rows, Columns, LeftLayout>(
            [&](size_t index) noexcept(Detail::kNothrowLinearCombination<T, U>) {
                const size_t row = index / Columns;
                const size_t column = index % Columns;
                auto sum = Sora::Math::Mul(left(row, 0), right(0, column));
                template for (constexpr size_t inner : std::define_static_array(std::views::iota(size_t{1}, Inner))) {
                    if consteval {
                        sum = Sora::Math::Add(Sora::Math::Mul(left(row, inner), right(inner, column)), sum);
                    } else {
                        sum = Sora::Math::Fma(left(row, inner), right(inner, column), sum);
                    }
                }
                return sum;
            });
    }

    /** @brief Return the transpose while preserving the selected physical layout. */
    template<ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout>
    [[nodiscard]] constexpr auto
    Transpose(const Matrix<T, Rows, Columns, Layout>& value) noexcept(std::is_nothrow_copy_constructible_v<T>)
        -> Matrix<T, Columns, Rows, Layout> {
        return Detail::GenerateMatrix<T, Columns, Rows, Layout>(
            [&](size_t index) noexcept -> const T& { return value(index % Rows, index / Rows); });
    }

    /** @brief Return the lane-wise conjunction of logical component equalities across layouts. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) {
            left == right;
            (left == right) & (left == right);
        }
    [[nodiscard]] constexpr auto
    operator==(const Matrix<T, Rows, Columns, LeftLayout>& left,
               const Matrix<U, Rows, Columns, RightLayout>& right) noexcept(noexcept(left(0, 0) == right(0, 0)) &&
                                                                            noexcept((left(0, 0) == right(0, 0)) &
                                                                                     (left(0, 0) == right(0, 0)))) {
        auto equal = left(0, 0) == right(0, 0);
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                if constexpr (row != 0 || column != 0) {
                    equal = equal & (left(row, column) == right(row, column));
                }
            }
        }
        return equal;
    }

} // namespace Sora::Math
