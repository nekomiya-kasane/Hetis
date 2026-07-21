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

    /** @brief Physical storage order of a fixed-size matrix. */
    enum class MatrixLayout : unsigned char {
        ColumnMajor,
        RowMajor,
    };

    namespace Detail {

        template<MatrixLayout Layout>
        using MatrixMdspanLayout =
            std::conditional_t<Layout == MatrixLayout::ColumnMajor, std::layout_left, std::layout_right>;

        template<typename T>
            requires ZeroInitializable<T>
        [[nodiscard]] constexpr T OneCarrier() noexcept {
            if constexpr (std::constructible_from<T, int>) {
                return T{1};
            } else {
                return T(typename T::ValueType{1});
            }
        }

    } // namespace Detail

    /**
     * @brief Non-owning multidimensional view over fixed-size contiguous matrix storage.
     * @tparam T Element type, optionally const-qualified.
     * @tparam Rows Positive compile-time row count.
     * @tparam Columns Positive compile-time column count.
     * @tparam Layout Physical storage order.
     */
    template<typename T, size_t Rows, size_t Columns, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(Rows > 0 && Columns > 0)
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
        requires(Rows > 0 && Columns > 0)
    class Matrix {
    public:
        using CarrierType = T;                                  /**< Mathematical component carrier. */
        static constexpr size_t kRows = Rows;                   /**< Logical row count. */
        static constexpr size_t kColumns = Columns;             /**< Logical column count. */
        static constexpr MatrixLayout kLayout = Layout;         /**< Physical storage order. */
        static constexpr size_t kElementCount = Rows * Columns; /**< Number of stored components. */

        /** @brief Construct the zero linear map. */
        constexpr Matrix() noexcept { values_.fill(Detail::ZeroCarrier<T>()); }

        /** @brief Construct from components already arranged in physical storage order. */
        constexpr explicit Matrix(std::array<T, kElementCount> storage) noexcept(
            std::is_nothrow_move_constructible_v<T>)
            : values_(std::move(storage)) {}

        /** @brief Construct from exactly @c Rows*Columns values in logical row-major order. */
        template<typename... U>
            requires(sizeof...(U) == kElementCount && (std::constructible_from<T, U &&> && ...))
        constexpr explicit(!(std::convertible_to<U&&, T> && ...))
            Matrix(U&&... values) noexcept((std::is_nothrow_constructible_v<T, U&&> && ...)) {
            auto arguments = std::tuple<U&&...>(std::forward<U>(values)...);
            template for (constexpr size_t index :
                          std::define_static_array(std::views::iota(size_t{0}, kElementCount))) {
                (*this)(index / Columns, index % Columns) = T(std::get<index>(std::move(arguments)));
            }
        }

        /**
         * @brief Access one logical component while preserving cv-ref qualification.
         * @pre @p row is less than @c Rows and @p column is less than @c Columns.
         */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) operator()(this Self&& self, size_t row, size_t column) noexcept {
            return std::forward_like<Self>(self.values_[StorageIndex(row, column)]);
        }

        /** @brief Return the physical component array while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Storage(this Self&& self) noexcept {
            return std::forward_like<Self>(self.values_);
        }

        /** @brief Return the first component address from an lvalue object. */
        template<typename Self>
            requires std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto Data(this Self&& self) noexcept {
            return self.values_.data();
        }

        /** @brief Return a zero-overhead multidimensional view preserving const qualification. */
        template<typename Self>
            requires std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto View(this Self&& self) noexcept {
            using SelfType = std::remove_reference_t<Self>;
            using Element = std::conditional_t<std::is_const_v<SelfType>, const T, T>;
            return MatrixView<Element, Rows, Columns, Layout>{self.Data()};
        }

        /** @brief Construct the identity linear map. */
        [[nodiscard]] static constexpr Matrix Identity() noexcept
            requires(Rows == Columns)
        {
            Matrix result;
            template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
                result(i, i) = Detail::OneCarrier<T>();
            }
            return result;
        }

    private:
        [[nodiscard]] static constexpr size_t StorageIndex(size_t row, size_t column) noexcept {
            if constexpr (Layout == MatrixLayout::ColumnMajor) {
                return column * Rows + row;
            } else {
                return row * Columns + column;
            }
        }

        std::array<T, kElementCount> values_{};
    };

    /** @brief Canonical short name for a fixed-size matrix. */
    template<ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(Rows > 0 && Columns > 0)
    using Mat = Matrix<T, Rows, Columns, Layout>;

    /** @brief Add two matrices componentwise regardless of physical layout. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) { Sora::Math::Add(left, right); } && ValueCarrier<Detail::AddResult<T, U>>
    [[nodiscard]] constexpr auto operator+(
        const Matrix<T, Rows, Columns, LeftLayout>& left,
        const Matrix<U, Rows, Columns, RightLayout>& right) noexcept(noexcept(Sora::Math::Add(left(0, 0), right(0, 0))))
        -> Matrix<Detail::AddResult<T, U>, Rows, Columns, LeftLayout> {
        Matrix<Detail::AddResult<T, U>, Rows, Columns, LeftLayout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                result(row, column) = Sora::Math::Add(left(row, column), right(row, column));
            }
        }
        return result;
    }

    /** @brief Subtract two matrices componentwise regardless of physical layout. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } && ValueCarrier<Detail::SubResult<T, U>>
    [[nodiscard]] constexpr auto operator-(
        const Matrix<T, Rows, Columns, LeftLayout>& left,
        const Matrix<U, Rows, Columns, RightLayout>& right) noexcept(noexcept(Sora::Math::Sub(left(0, 0), right(0, 0))))
        -> Matrix<Detail::SubResult<T, U>, Rows, Columns, LeftLayout> {
        Matrix<Detail::SubResult<T, U>, Rows, Columns, LeftLayout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                result(row, column) = Sora::Math::Sub(left(row, column), right(row, column));
            }
        }
        return result;
    }

    /** @brief Multiply every matrix component by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(T value, S scale) { Sora::Math::Mul(value, scale); } && ValueCarrier<Detail::MulResult<T, S>>
    [[nodiscard]] constexpr auto operator*(const Matrix<T, Rows, Columns, Layout>& value,
                                           const S& scale) noexcept(noexcept(Sora::Math::Mul(value(0, 0), scale)))
        -> Matrix<Detail::MulResult<T, S>, Rows, Columns, Layout> {
        Matrix<Detail::MulResult<T, S>, Rows, Columns, Layout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                result(row, column) = Sora::Math::Mul(value(row, column), scale);
            }
        }
        return result;
    }

    /** @brief Multiply every matrix component by a scalar or broadcast-compatible carrier. */
    template<typename S, ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(S scale, T value) { Sora::Math::Mul(scale, value); } && ValueCarrier<Detail::MulResult<S, T>>
    [[nodiscard]] constexpr auto operator*(const S& scale, const Matrix<T, Rows, Columns, Layout>& value) noexcept(
        noexcept(Sora::Math::Mul(scale, value(0, 0)))) -> Matrix<Detail::MulResult<S, T>, Rows, Columns, Layout> {
        Matrix<Detail::MulResult<S, T>, Rows, Columns, Layout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                result(row, column) = Sora::Math::Mul(scale, value(row, column));
            }
        }
        return result;
    }

    /** @brief Divide every matrix component by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(T value, S scale) { Sora::Math::Div(value, scale); } && ValueCarrier<Detail::DivResult<T, S>>
    [[nodiscard]] constexpr auto operator/(const Matrix<T, Rows, Columns, Layout>& value,
                                           const S& scale) noexcept(noexcept(Sora::Math::Div(value(0, 0), scale)))
        -> Matrix<Detail::DivResult<T, S>, Rows, Columns, Layout> {
        Matrix<Detail::DivResult<T, S>, Rows, Columns, Layout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                result(row, column) = Sora::Math::Div(value(row, column), scale);
            }
        }
        return result;
    }

    /** @brief Apply a matrix to a column vector. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(T left, U right) {
            Sora::Math::Mul(left, right);
            Sora::Math::Fma(left, right, Sora::Math::Mul(left, right));
            Sora::Math::Add(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
        }
    [[nodiscard]] constexpr auto
    operator*(const Matrix<T, Rows, Columns, Layout>& matrix, const Vector<U, Columns>& vector) noexcept(
        noexcept(Sora::Math::Fma(matrix(0, 0), vector[0], Sora::Math::Mul(matrix(0, 0), vector[0]))))
        -> Vector<Detail::MulResult<T, U>, Rows> {
        Vector<Detail::MulResult<T, U>, Rows> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            auto sum = Sora::Math::Mul(matrix(row, 0), vector[0]);
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{1}, Columns))) {
                if consteval {
                    sum = Sora::Math::Add(Sora::Math::Mul(matrix(row, column), vector[column]), sum);
                } else {
                    sum = Sora::Math::Fma(matrix(row, column), vector[column], sum);
                }
            }
            result[row] = sum;
        }
        return result;
    }

    /** @brief Compose two fixed-size linear maps. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Inner, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) {
            Sora::Math::Mul(left, right);
            Sora::Math::Fma(left, right, Sora::Math::Mul(left, right));
            Sora::Math::Add(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
        }
    [[nodiscard]] constexpr auto
    operator*(const Matrix<T, Rows, Inner, LeftLayout>& left,
              const Matrix<U, Inner, Columns, RightLayout>&
                  right) noexcept(noexcept(Sora::Math::Fma(left(0, 0), right(0, 0),
                                                           Sora::Math::Mul(left(0, 0), right(0, 0)))))
        -> Matrix<Detail::MulResult<T, U>, Rows, Columns, LeftLayout> {
        Matrix<Detail::MulResult<T, U>, Rows, Columns, LeftLayout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                auto sum = Sora::Math::Mul(left(row, 0), right(0, column));
                template for (constexpr size_t inner : std::define_static_array(std::views::iota(size_t{1}, Inner))) {
                    if consteval {
                        sum = Sora::Math::Add(Sora::Math::Mul(left(row, inner), right(inner, column)), sum);
                    } else {
                        sum = Sora::Math::Fma(left(row, inner), right(inner, column), sum);
                    }
                }
                result(row, column) = sum;
            }
        }
        return result;
    }

    /** @brief Return the transpose while preserving the selected physical layout. */
    template<ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout>
    [[nodiscard]] constexpr auto Transpose(const Matrix<T, Rows, Columns, Layout>& value) noexcept
        -> Matrix<T, Columns, Rows, Layout> {
        Matrix<T, Columns, Rows, Layout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, Rows))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, Columns))) {
                result(column, row) = value(row, column);
            }
        }
        return result;
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
