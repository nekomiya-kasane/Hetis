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
        concept OneInitializable = std::constructible_from<T, int> || requires { T(typename T::ValueType{1}); };

        template<typename T>
            requires OneInitializable<T>
        inline constexpr bool kNothrowOneCarrier = [] {
            if constexpr (std::constructible_from<T, int>) {
                return std::is_nothrow_constructible_v<T, int>;
            } else {
                return noexcept(T(typename T::ValueType{1}));
            }
        }();

        template<typename T>
            requires OneInitializable<T>
        [[nodiscard]] constexpr T OneCarrier() noexcept(kNothrowOneCarrier<T>) {
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
        constexpr Matrix() noexcept(Detail::kNothrowZeroCarrier<T>)
            requires Detail::ZeroInitializable<T>
            : Matrix(ZeroTag{}, std::make_index_sequence<kElementCount>{}) {}

        /** @brief Construct from components already arranged in physical storage order. */
        constexpr explicit Matrix(std::array<T, kElementCount> storage) noexcept(
            std::is_nothrow_move_constructible_v<T>)
            : values_(std::move(storage)) {}

        /** @brief Construct from exactly @c Rows*Columns values in logical row-major order. */
        template<typename... U>
            requires(sizeof...(U) == kElementCount && (std::constructible_from<T, U &&> && ...))
        constexpr explicit(!(std::convertible_to<U&&, T> && ...))
            Matrix(U&&... values) noexcept((std::is_nothrow_constructible_v<T, U&&> && ...))
            : Matrix(std::tuple<U&&...>(std::forward<U>(values)...), std::make_index_sequence<kElementCount>{}) {}

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
        [[nodiscard]] static constexpr Matrix Identity() noexcept(Detail::kNothrowZeroCarrier<T> &&
                                                                  Detail::kNothrowOneCarrier<T>)
            requires(Rows == Columns && Detail::ZeroInitializable<T> && Detail::OneInitializable<T>)
        {
            return Matrix(IdentityTag{}, std::make_index_sequence<kElementCount>{});
        }

    private:
        struct ZeroTag {};
        struct IdentityTag {};

        template<size_t PhysicalIndex>
        [[nodiscard]] static consteval size_t LogicalIndex() {
            if constexpr (Layout == MatrixLayout::ColumnMajor) {
                constexpr size_t row = PhysicalIndex % Rows;
                constexpr size_t column = PhysicalIndex / Rows;
                return row * Columns + column;
            } else {
                return PhysicalIndex;
            }
        }

        template<size_t PhysicalIndex>
        [[nodiscard]] static constexpr T IdentityCarrier() noexcept(Detail::kNothrowZeroCarrier<T> &&
                                                                    Detail::kNothrowOneCarrier<T>)
            requires Detail::ZeroInitializable<T> && Detail::OneInitializable<T>
        {
            constexpr size_t logicalIndex = LogicalIndex<PhysicalIndex>();
            if constexpr (logicalIndex / Columns == logicalIndex % Columns) {
                return Detail::OneCarrier<T>();
            } else {
                return Detail::ZeroCarrier<T>();
            }
        }

        template<size_t... PhysicalIndices>
        constexpr explicit Matrix(ZeroTag,
                                  std::index_sequence<PhysicalIndices...>) noexcept(Detail::kNothrowZeroCarrier<T>)
            requires Detail::ZeroInitializable<T>
            : values_{((void)PhysicalIndices, Detail::ZeroCarrier<T>())...} {}

        template<size_t... PhysicalIndices>
        constexpr explicit Matrix(IdentityTag,
                                  std::index_sequence<PhysicalIndices...>) noexcept(Detail::kNothrowZeroCarrier<T> &&
                                                                                    Detail::kNothrowOneCarrier<T>)
            requires Detail::ZeroInitializable<T> && Detail::OneInitializable<T>
            : values_{IdentityCarrier<PhysicalIndices>()...} {}

        template<typename Tuple, size_t... PhysicalIndices>
        constexpr explicit Matrix(Tuple&& arguments, std::index_sequence<PhysicalIndices...>) noexcept(
            (std::is_nothrow_constructible_v<
                 T, decltype(std::get<LogicalIndex<PhysicalIndices>()>(std::forward<Tuple>(arguments)))> &&
             ...))
            : values_{T(std::get<LogicalIndex<PhysicalIndices>()>(std::forward<Tuple>(arguments)))...} {}

        [[nodiscard]] static constexpr size_t StorageIndex(size_t row, size_t column) noexcept {
            if constexpr (Layout == MatrixLayout::ColumnMajor) {
                return column * Rows + row;
            } else {
                return row * Columns + column;
            }
        }

        std::array<T, kElementCount> values_;
    };

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
        concept LinearCombinable = requires(T left, U right, MulResult<T, U> sum) {
            sum = Sora::Math::Add(Sora::Math::Mul(left, right), sum);
            sum = Sora::Math::Fma(left, right, sum);
        } && ValueCarrier<MulResult<T, U>>;

        template<typename T, typename U>
            requires LinearCombinable<T, U>
        inline constexpr bool kNothrowLinearCombination = [] {
            using R = MulResult<T, U>;
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
        requires(Rows > 0 && Columns > 0)
    using Mat = Matrix<T, Rows, Columns, Layout>;

    /** @brief Add two matrices componentwise regardless of physical layout. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) { Sora::Math::Add(left, right); } && ValueCarrier<Detail::AddResult<T, U>>
    [[nodiscard]] constexpr auto
    operator+(const Matrix<T, Rows, Columns, LeftLayout>& left,
              const Matrix<U, Rows, Columns, RightLayout>&
                  right) noexcept(noexcept(Sora::Math::Add(left(0, 0), right(0, 0))) &&
                                  std::is_nothrow_constructible_v<Detail::AddResult<T, U>,
                                                                  decltype(Sora::Math::Add(left(0, 0), right(0, 0)))>)
        -> Matrix<Detail::AddResult<T, U>, Rows, Columns, LeftLayout> {
        return Detail::GenerateMatrix<Detail::AddResult<T, U>, Rows, Columns, LeftLayout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Add(left(0, 0), right(0, 0)))) {
                const size_t row = index / Columns;
                const size_t column = index % Columns;
                return Sora::Math::Add(left(row, column), right(row, column));
            });
    }

    /** @brief Subtract two matrices componentwise regardless of physical layout. */
    template<ValueCarrier T, ValueCarrier U, size_t Rows, size_t Columns, MatrixLayout LeftLayout,
             MatrixLayout RightLayout>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } && ValueCarrier<Detail::SubResult<T, U>>
    [[nodiscard]] constexpr auto
    operator-(const Matrix<T, Rows, Columns, LeftLayout>& left,
              const Matrix<U, Rows, Columns, RightLayout>&
                  right) noexcept(noexcept(Sora::Math::Sub(left(0, 0), right(0, 0))) &&
                                  std::is_nothrow_constructible_v<Detail::SubResult<T, U>,
                                                                  decltype(Sora::Math::Sub(left(0, 0), right(0, 0)))>)
        -> Matrix<Detail::SubResult<T, U>, Rows, Columns, LeftLayout> {
        return Detail::GenerateMatrix<Detail::SubResult<T, U>, Rows, Columns, LeftLayout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Sub(left(0, 0), right(0, 0)))) {
                const size_t row = index / Columns;
                const size_t column = index % Columns;
                return Sora::Math::Sub(left(row, column), right(row, column));
            });
    }

    /** @brief Multiply every matrix component by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(T value, S scale) { Sora::Math::Mul(value, scale); } && ValueCarrier<Detail::MulResult<T, S>>
    [[nodiscard]] constexpr auto operator*(const Matrix<T, Rows, Columns, Layout>& value, const S& scale) noexcept(
        noexcept(Sora::Math::Mul(value(0, 0), scale)) &&
        std::is_nothrow_constructible_v<Detail::MulResult<T, S>, decltype(Sora::Math::Mul(value(0, 0), scale))>)
        -> Matrix<Detail::MulResult<T, S>, Rows, Columns, Layout> {
        return Detail::GenerateMatrix<Detail::MulResult<T, S>, Rows, Columns, Layout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Mul(value(0, 0), scale))) {
                return Sora::Math::Mul(value(index / Columns, index % Columns), scale);
            });
    }

    /** @brief Multiply every matrix component by a scalar or broadcast-compatible carrier. */
    template<typename S, ValueCarrier T, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(S scale, T value) { Sora::Math::Mul(scale, value); } && ValueCarrier<Detail::MulResult<S, T>>
    [[nodiscard]] constexpr auto operator*(const S& scale, const Matrix<T, Rows, Columns, Layout>& value) noexcept(
        noexcept(Sora::Math::Mul(scale, value(0, 0))) &&
        std::is_nothrow_constructible_v<Detail::MulResult<S, T>, decltype(Sora::Math::Mul(scale, value(0, 0)))>)
        -> Matrix<Detail::MulResult<S, T>, Rows, Columns, Layout> {
        return Detail::GenerateMatrix<Detail::MulResult<S, T>, Rows, Columns, Layout>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Mul(scale, value(0, 0)))) {
                return Sora::Math::Mul(scale, value(index / Columns, index % Columns));
            });
    }

    /** @brief Divide every matrix component by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires requires(T value, S scale) { Sora::Math::Div(value, scale); } && ValueCarrier<Detail::DivResult<T, S>>
    [[nodiscard]] constexpr auto operator/(const Matrix<T, Rows, Columns, Layout>& value, const S& scale) noexcept(
        noexcept(Sora::Math::Div(value(0, 0), scale)) &&
        std::is_nothrow_constructible_v<Detail::DivResult<T, S>, decltype(Sora::Math::Div(value(0, 0), scale))>)
        -> Matrix<Detail::DivResult<T, S>, Rows, Columns, Layout> {
        return Detail::GenerateMatrix<Detail::DivResult<T, S>, Rows, Columns, Layout>(
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
        -> Vector<Detail::MulResult<T, U>, Rows> {
        using R = Detail::MulResult<T, U>;
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
        -> Matrix<Detail::MulResult<T, U>, Rows, Columns, LeftLayout> {
        using R = Detail::MulResult<T, U>;
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
