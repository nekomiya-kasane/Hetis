/**
 * @file Vector.h
 * @brief Trivial fixed-size vectors over scalar, SIMD, and extensible mathematical carriers.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>
#include <Sora/Math/TensorCore.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <meta>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    namespace Traits {

        template<typename T, typename U>
        using AddResult = std::remove_cvref_t<decltype(Sora::Math::Add(std::declval<T>(), std::declval<U>()))>;

        template<typename T, typename U>
        using SubResult = std::remove_cvref_t<decltype(Sora::Math::Sub(std::declval<T>(), std::declval<U>()))>;

        template<typename T, typename U>
        using MulResult = std::remove_cvref_t<decltype(Sora::Math::Mul(std::declval<T>(), std::declval<U>()))>;

        template<typename T, typename U>
        using DivResult = std::remove_cvref_t<decltype(Sora::Math::Div(std::declval<T>(), std::declval<U>()))>;

    } // namespace Traits

    /** @brief Fixed-size free vector backed by the canonical rank-one dense tensor representation. */
    template<ValueCarrier T, size_t N>
        requires(N > 0)
    using Vector = BasicTensor<StaticDenseStorage<T, StaticShape<N>, RightMapping>, Algebra::FreeVector>;

    /** @brief Canonical short name for a fixed-size free vector. */
    template<ValueCarrier T, size_t N>
        requires(N > 0)
    using Vec = Vector<T, N>;

    namespace Detail {

        template<ValueCarrier T, size_t N, typename Generator, size_t... Indices>
            requires requires(Generator& generator) { Vector<T, N>{generator(Indices)...}; }
        [[nodiscard]] constexpr Vector<T, N>
        GenerateVector(Generator& generator,
                       std::index_sequence<Indices...>) noexcept(noexcept(Vector<T, N>{generator(Indices)...})) {
            return Vector<T, N>{generator(Indices)...};
        }

        template<ValueCarrier T, size_t N, typename Generator>
            requires requires(Generator& generator) { GenerateVector<T, N>(generator, std::make_index_sequence<N>{}); }
        [[nodiscard]] constexpr Vector<T, N> GenerateVector(Generator&& generator) noexcept(
            noexcept(GenerateVector<T, N>(generator, std::make_index_sequence<N>{}))) {
            return GenerateVector<T, N>(generator, std::make_index_sequence<N>{});
        }

    } // namespace Detail

    /** @brief Add two vectors componentwise. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) { Sora::Math::Add(left, right); } && ValueCarrier<Traits::AddResult<T, U>>
    [[nodiscard]] constexpr auto operator+(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(Sora::Math::Add(left[0], right[0])) &&
        std::is_nothrow_constructible_v<Traits::AddResult<T, U>, decltype(Sora::Math::Add(left[0], right[0]))>)
        -> Vector<Traits::AddResult<T, U>, N> {
        return Detail::GenerateVector<Traits::AddResult<T, U>, N>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Add(left[0], right[0]))) {
                return Sora::Math::Add(left[index], right[index]);
            });
    }

    /** @brief Subtract two vectors componentwise. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } && ValueCarrier<Traits::SubResult<T, U>>
    [[nodiscard]] constexpr auto operator-(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(Sora::Math::Sub(left[0], right[0])) &&
        std::is_nothrow_constructible_v<Traits::SubResult<T, U>, decltype(Sora::Math::Sub(left[0], right[0]))>)
        -> Vector<Traits::SubResult<T, U>, N> {
        return Detail::GenerateVector<Traits::SubResult<T, U>, N>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Sub(left[0], right[0]))) {
                return Sora::Math::Sub(left[index], right[index]);
            });
    }

    /** @brief Negate every vector component. */
    template<ValueCarrier T, size_t N>
        requires requires(T value) { Sora::Math::Neg(value); } &&
                 ValueCarrier<std::remove_cvref_t<decltype(Sora::Math::Neg(std::declval<T>()))>>
    [[nodiscard]] constexpr auto operator-(const Vector<T, N>& value) noexcept(
        noexcept(Sora::Math::Neg(value[0])) &&
        std::is_nothrow_constructible_v<std::remove_cvref_t<decltype(Sora::Math::Neg(std::declval<T>()))>,
                                        decltype(Sora::Math::Neg(value[0]))>)
        -> Vector<std::remove_cvref_t<decltype(Sora::Math::Neg(std::declval<T>()))>, N> {
        using R = std::remove_cvref_t<decltype(Sora::Math::Neg(std::declval<T>()))>;
        return Detail::GenerateVector<R, N>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Neg(value[0]))) { return Sora::Math::Neg(value[index]); });
    }

    /** @brief Multiply a vector by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t N>
        requires requires(T value, S scale) { Sora::Math::Mul(value, scale); } && ValueCarrier<Traits::MulResult<T, S>>
    [[nodiscard]] constexpr auto operator*(const Vector<T, N>& value, const S& scale) noexcept(
        noexcept(Sora::Math::Mul(value[0], scale)) &&
        std::is_nothrow_constructible_v<Traits::MulResult<T, S>, decltype(Sora::Math::Mul(value[0], scale))>)
        -> Vector<Traits::MulResult<T, S>, N> {
        return Detail::GenerateVector<Traits::MulResult<T, S>, N>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Mul(value[0], scale))) {
                return Sora::Math::Mul(value[index], scale);
            });
    }

    /** @brief Multiply a vector by a scalar or broadcast-compatible carrier. */
    template<typename S, ValueCarrier T, size_t N>
        requires requires(S scale, T value) { Sora::Math::Mul(scale, value); } && ValueCarrier<Traits::MulResult<S, T>>
    [[nodiscard]] constexpr auto operator*(const S& scale, const Vector<T, N>& value) noexcept(
        noexcept(Sora::Math::Mul(scale, value[0])) &&
        std::is_nothrow_constructible_v<Traits::MulResult<S, T>, decltype(Sora::Math::Mul(scale, value[0]))>)
        -> Vector<Traits::MulResult<S, T>, N> {
        return Detail::GenerateVector<Traits::MulResult<S, T>, N>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Mul(scale, value[0]))) {
                return Sora::Math::Mul(scale, value[index]);
            });
    }

    /** @brief Divide a vector by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t N>
        requires requires(T value, S scale) { Sora::Math::Div(value, scale); } && ValueCarrier<Traits::DivResult<T, S>>
    [[nodiscard]] constexpr auto operator/(const Vector<T, N>& value, const S& scale) noexcept(
        noexcept(Sora::Math::Div(value[0], scale)) &&
        std::is_nothrow_constructible_v<Traits::DivResult<T, S>, decltype(Sora::Math::Div(value[0], scale))>)
        -> Vector<Traits::DivResult<T, S>, N> {
        return Detail::GenerateVector<Traits::DivResult<T, S>, N>(
            [&](size_t index) noexcept(noexcept(Sora::Math::Div(value[0], scale))) {
                return Sora::Math::Div(value[index], scale);
            });
    }

    /** @brief Return the lane-wise conjunction of component equalities. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) {
            left == right;
            (left == right) & (left == right);
        }
    [[nodiscard]] constexpr auto operator==(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(left[0] == right[0]) && noexcept((left[0] == right[0]) & (left[0] == right[0]))) {
        auto equal = left[0] == right[0];
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{1}, N))) {
            equal = equal & (left[i] == right[i]);
        }
        return equal;
    }

    /** @brief Return the lane-wise negation of vector equality. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(const Vector<T, N>& left, const Vector<U, N>& right) { !(left == right); }
    [[nodiscard]] constexpr auto operator!=(const Vector<T, N>& left,
                                            const Vector<U, N>& right) noexcept(noexcept(!(left == right))) {
        return !(left == right);
    }

    /** @brief Compute the inner product, using fused multiply-add where the backend provides it. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) {
            Sora::Math::Mul(left, right);
            Sora::Math::Fma(left, right, Sora::Math::Mul(left, right));
            Sora::Math::Add(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
        }
    [[nodiscard]] constexpr auto Dot(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(Sora::Math::Mul(left[0], right[0])) &&
        noexcept(Sora::Math::Fma(left[0], right[0], Sora::Math::Mul(left[0], right[0])))) {
        auto result = Sora::Math::Mul(left[0], right[0]);
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{1}, N))) {
            if consteval {
                result = Sora::Math::Add(Sora::Math::Mul(left[i], right[i]), result);
            } else {
                result = Sora::Math::Fma(left[i], right[i], result);
            }
        }
        return result;
    }

    /** @brief Compute the squared Euclidean norm. */
    template<ValueCarrier T, size_t N>
        requires requires(const Vector<T, N>& value) { Sora::Math::Dot(value, value); }
    [[nodiscard]] constexpr auto
    SquaredNorm(const Vector<T, N>& value) noexcept(noexcept(Sora::Math::Dot(value, value))) {
        return Sora::Math::Dot(value, value);
    }

    /** @brief Compute the Euclidean norm when the selected carrier backend provides square root. */
    template<ValueCarrier T, size_t N>
        requires requires(const Vector<T, N>& value) { Sora::Math::Sqrt(Sora::Math::SquaredNorm(value)); }
    [[nodiscard]] constexpr auto
    Norm(const Vector<T, N>& value) noexcept(noexcept(Sora::Math::Sqrt(Sora::Math::SquaredNorm(value)))) {
        return Sora::Math::Sqrt(Sora::Math::SquaredNorm(value));
    }

    /** @brief Compute the three-dimensional cross product. */
    template<ValueCarrier T, ValueCarrier U>
        requires requires(T left, U right) {
            Sora::Math::Mul(left, right);
            Sora::Math::Mfs(left, right, Sora::Math::Mul(left, right));
            Sora::Math::Sub(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
        }
    [[nodiscard]] constexpr auto Cross(const Vector<T, 3>& left, const Vector<U, 3>& right) noexcept(
        noexcept(Sora::Math::Mfs(left[1], right[2], Sora::Math::Mul(left[2], right[1])))) {
        using R = std::remove_cvref_t<decltype(Sora::Math::Mfs(std::declval<T>(), std::declval<U>(),
                                                               Sora::Math::Mul(std::declval<T>(), std::declval<U>())))>;
        if consteval {
            return Vector<R, 3>{
                Sora::Math::Sub(Sora::Math::Mul(left[1], right[2]), Sora::Math::Mul(left[2], right[1])),
                Sora::Math::Sub(Sora::Math::Mul(left[2], right[0]), Sora::Math::Mul(left[0], right[2])),
                Sora::Math::Sub(Sora::Math::Mul(left[0], right[1]), Sora::Math::Mul(left[1], right[0]))};
        } else {
            return Vector<R, 3>{Sora::Math::Mfs(left[1], right[2], Sora::Math::Mul(left[2], right[1])),
                                Sora::Math::Mfs(left[2], right[0], Sora::Math::Mul(left[0], right[2])),
                                Sora::Math::Mfs(left[0], right[1], Sora::Math::Mul(left[1], right[0]))};
        }
    }

} // namespace Sora::Math
