/**
 * @file EigenAdaptor.h
 * @brief Zero-overhead Eigen aliases for fixed-size vectors, matrices, and external-memory maps.
 * @ingroup Math
 */
#pragma once

#include <Eigen/Core>

#include <concepts>
#include <type_traits>

namespace Sora::Math {

    /** @brief Eigen column vector with @p Size compile-time rows. */
    template<typename Scalar, int Size>
        requires(Size == Eigen::Dynamic || Size > 0)
    using Vec = Eigen::Matrix<Scalar, Size, 1>;

    /** @brief Eigen matrix with compile-time dimensions and configurable storage order. */
    template<typename Scalar, int Rows, int Columns, int Options = Eigen::ColMajor>
        requires((Rows == Eigen::Dynamic || Rows > 0) && (Columns == Eigen::Dynamic || Columns > 0))
    using Mat = Eigen::Matrix<Scalar, Rows, Columns, Options>;

    /** @brief Non-owning Eigen map over a column vector. */
    template<typename Scalar, int Size, int MapOptions = Eigen::Unaligned>
        requires(Size == Eigen::Dynamic || Size > 0)
    using VecMap = Eigen::Map<Vec<Scalar, Size>, MapOptions>;

    /** @brief Read-only non-owning Eigen map over a column vector. */
    template<typename Scalar, int Size, int MapOptions = Eigen::Unaligned>
        requires(Size == Eigen::Dynamic || Size > 0)
    using ConstVecMap = Eigen::Map<const Vec<Scalar, Size>, MapOptions>;

    /** @brief Non-owning Eigen map over a matrix. */
    template<typename Scalar, int Rows, int Columns, int Options = Eigen::ColMajor,
             int MapOptions = Eigen::Unaligned>
        requires((Rows == Eigen::Dynamic || Rows > 0) && (Columns == Eigen::Dynamic || Columns > 0))
    using MatMap = Eigen::Map<Mat<Scalar, Rows, Columns, Options>, MapOptions>;

    /** @brief Read-only non-owning Eigen map over a matrix. */
    template<typename Scalar, int Rows, int Columns, int Options = Eigen::ColMajor,
             int MapOptions = Eigen::Unaligned>
        requires((Rows == Eigen::Dynamic || Rows > 0) && (Columns == Eigen::Dynamic || Columns > 0))
    using ConstMatMap = Eigen::Map<const Mat<Scalar, Rows, Columns, Options>, MapOptions>;

    /** @brief Eigen dense object exposing compile-time matrix dimensions and a scalar type. */
    template<typename T>
    concept EigenDense =
        std::derived_from<std::remove_cvref_t<T>, Eigen::DenseBase<std::remove_cvref_t<T>>>;

    /** @brief Eigen dense object whose compile-time shape is a column vector. */
    template<typename T>
    concept EigenColumnVector = EigenDense<T> && std::remove_cvref_t<T>::ColsAtCompileTime == 1;

} // namespace Sora::Math
