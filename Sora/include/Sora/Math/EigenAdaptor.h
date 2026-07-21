/**
 * @file EigenAdaptor.h
 * @brief Explicit zero-copy interoperability between canonical Sora values and Eigen dense objects.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Matrix.h>

#include <Eigen/Core>

#include <concepts>
#include <limits>
#include <type_traits>

namespace Sora::Math::Interop::Eigen {

    /** @brief Scalar accepted by the canonical Eigen interoperability boundary. */
    template<typename T>
    concept Scalar = std::is_arithmetic_v<std::remove_cv_t<T>>;

    /** @brief Eigen-owned column vector used only inside the explicit interoperability namespace. */
    template<Scalar T, int Size>
        requires(Size == ::Eigen::Dynamic || Size > 0)
    using Vector = ::Eigen::Matrix<T, Size, 1>;

    /** @brief Eigen-owned matrix used only inside the explicit interoperability namespace. */
    template<Scalar T, int Rows, int Columns, int Options = ::Eigen::ColMajor>
        requires((Rows == ::Eigen::Dynamic || Rows > 0) && (Columns == ::Eigen::Dynamic || Columns > 0))
    using Matrix = ::Eigen::Matrix<T, Rows, Columns, Options>;

    /** @brief Mutable Eigen map over contiguous column-vector storage. */
    template<Scalar T, int Size, int MapOptions = ::Eigen::Unaligned>
        requires(Size == ::Eigen::Dynamic || Size > 0)
    using VectorMap = ::Eigen::Map<Vector<T, Size>, MapOptions>;

    /** @brief Read-only Eigen map over contiguous column-vector storage. */
    template<Scalar T, int Size, int MapOptions = ::Eigen::Unaligned>
        requires(Size == ::Eigen::Dynamic || Size > 0)
    using ConstVectorMap = ::Eigen::Map<const Vector<T, Size>, MapOptions>;

    /** @brief Mutable Eigen map over contiguous matrix storage. */
    template<Scalar T, int Rows, int Columns, int Options = ::Eigen::ColMajor, int MapOptions = ::Eigen::Unaligned>
        requires((Rows == ::Eigen::Dynamic || Rows > 0) && (Columns == ::Eigen::Dynamic || Columns > 0))
    using MatrixMap = ::Eigen::Map<Matrix<T, Rows, Columns, Options>, MapOptions>;

    /** @brief Read-only Eigen map over contiguous matrix storage. */
    template<Scalar T, int Rows, int Columns, int Options = ::Eigen::ColMajor, int MapOptions = ::Eigen::Unaligned>
        requires((Rows == ::Eigen::Dynamic || Rows > 0) && (Columns == ::Eigen::Dynamic || Columns > 0))
    using ConstMatrixMap = ::Eigen::Map<const Matrix<T, Rows, Columns, Options>, MapOptions>;

    /** @brief Eigen dense expression or object. */
    template<typename T>
    concept Dense = std::derived_from<std::remove_cvref_t<T>, ::Eigen::DenseBase<std::remove_cvref_t<T>>>;

    /** @brief Eigen dense expression whose compile-time shape is a column vector. */
    template<typename T>
    concept ColumnVector = Dense<T> && std::remove_cvref_t<T>::ColsAtCompileTime == 1;

    /** @brief Eigen storage option corresponding to a canonical Sora matrix layout. */
    template<MatrixLayout Layout>
    inline constexpr int kStorageOptions = Layout == MatrixLayout::ColumnMajor ? ::Eigen::ColMajor : ::Eigen::RowMajor;

    /** @brief Expose a mutable canonical vector as an allocation-free Eigen map. */
    template<Scalar T, size_t Size>
        requires(Size <= static_cast<size_t>(std::numeric_limits<int>::max()))
    [[nodiscard]] constexpr auto AsEigen(Sora::Math::Vector<T, Size>& value) noexcept {
        return VectorMap<T, static_cast<int>(Size)>{value.Data()};
    }

    /** @brief Expose a read-only canonical vector as an allocation-free Eigen map. */
    template<Scalar T, size_t Size>
        requires(Size <= static_cast<size_t>(std::numeric_limits<int>::max()))
    [[nodiscard]] constexpr auto AsEigen(const Sora::Math::Vector<T, Size>& value) noexcept {
        return ConstVectorMap<T, static_cast<int>(Size)>{value.Data()};
    }

    /** @brief Expose a mutable canonical matrix as an allocation-free Eigen map. */
    template<Scalar T, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires(Rows <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
                 Columns <= static_cast<size_t>(std::numeric_limits<int>::max()))
    [[nodiscard]] constexpr auto AsEigen(Sora::Math::Matrix<T, Rows, Columns, Layout>& value) noexcept {
        return MatrixMap<T, static_cast<int>(Rows), static_cast<int>(Columns), kStorageOptions<Layout>>{value.Data()};
    }

    /** @brief Expose a read-only canonical matrix as an allocation-free Eigen map. */
    template<Scalar T, size_t Rows, size_t Columns, MatrixLayout Layout>
        requires(Rows <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
                 Columns <= static_cast<size_t>(std::numeric_limits<int>::max()))
    [[nodiscard]] constexpr auto AsEigen(const Sora::Math::Matrix<T, Rows, Columns, Layout>& value) noexcept {
        return ConstMatrixMap<T, static_cast<int>(Rows), static_cast<int>(Columns), kStorageOptions<Layout>>{
            value.Data()};
    }

} // namespace Sora::Math::Interop::Eigen
