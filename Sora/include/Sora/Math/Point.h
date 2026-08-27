/**
 * @file Point.h
 * @brief Trivial fixed-size affine points with vector-distinct arithmetic.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Vector.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <meta>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief Affine point backed by rank-one dense storage but excluded from general tensor algebra. */
    template<ValueCarrier T, size_t N>
        requires(N > 0)
    using Point = BasicTensor<StaticDenseStorage<T, StaticShape<N>, RightMapping>, Algebra::AffinePoint>;

    /** @brief Subtract two points to obtain their displacement vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } && ValueCarrier<Traits::SubResult<T, U>>
    [[nodiscard]] constexpr auto operator-(const Point<T, N>& left, const Point<U, N>& right) noexcept(
        noexcept(Sora::Math::Sub(left[0], right[0])) &&
        std::is_nothrow_constructible_v<Traits::SubResult<T, U>, decltype(Sora::Math::Sub(left[0], right[0]))>)
        -> Vector<Traits::SubResult<T, U>, N> {
        return [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return Vector<Traits::SubResult<T, U>, N>{Sora::Math::Sub(left[Indices], right[Indices])...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Translate a point by a vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T point, U vector) { Sora::Math::Add(point, vector); } &&
                 ValueCarrier<Traits::AddResult<T, U>>
    [[nodiscard]] constexpr auto operator+(const Point<T, N>& point, const Vector<U, N>& vector) noexcept(
        noexcept(Sora::Math::Add(point[0], vector[0])) &&
        std::is_nothrow_constructible_v<Traits::AddResult<T, U>, decltype(Sora::Math::Add(point[0], vector[0]))>)
        -> Point<Traits::AddResult<T, U>, N> {
        return [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return Point<Traits::AddResult<T, U>, N>{Sora::Math::Add(point[Indices], vector[Indices])...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Translate a point by a vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T vector, U point) { Sora::Math::Add(vector, point); } &&
                 ValueCarrier<Traits::AddResult<T, U>>
    [[nodiscard]] constexpr auto operator+(const Vector<T, N>& vector, const Point<U, N>& point) noexcept(
        noexcept(Sora::Math::Add(vector[0], point[0])) &&
        std::is_nothrow_constructible_v<Traits::AddResult<T, U>, decltype(Sora::Math::Add(vector[0], point[0]))>)
        -> Point<Traits::AddResult<T, U>, N> {
        return [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return Point<Traits::AddResult<T, U>, N>{Sora::Math::Add(vector[Indices], point[Indices])...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Translate a point by the negation of a vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T point, U vector) { Sora::Math::Sub(point, vector); } &&
                 ValueCarrier<Traits::SubResult<T, U>>
    [[nodiscard]] constexpr auto operator-(const Point<T, N>& point, const Vector<U, N>& vector) noexcept(
        noexcept(Sora::Math::Sub(point[0], vector[0])) &&
        std::is_nothrow_constructible_v<Traits::SubResult<T, U>, decltype(Sora::Math::Sub(point[0], vector[0]))>)
        -> Point<Traits::SubResult<T, U>, N> {
        return [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return Point<Traits::SubResult<T, U>, N>{Sora::Math::Sub(point[Indices], vector[Indices])...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Return the lane-wise conjunction of coordinate equalities. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) {
            left == right;
            (left == right) & (left == right);
        }
    [[nodiscard]] constexpr auto operator==(const Point<T, N>& left, const Point<U, N>& right) noexcept(
        noexcept(left[0] == right[0]) && noexcept((left[0] == right[0]) & (left[0] == right[0]))) {
        auto equal = left[0] == right[0];
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{1}, N))) {
            equal = equal & (left[i] == right[i]);
        }
        return equal;
    }

    /** @brief Return the lane-wise negation of point equality. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(const Point<T, N>& left, const Point<U, N>& right) { !(left == right); }
    [[nodiscard]] constexpr auto operator!=(const Point<T, N>& left,
                                            const Point<U, N>& right) noexcept(noexcept(!(left == right))) {
        return !(left == right);
    }

    /** @brief Compute squared Euclidean distance between two points. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(const Point<T, N>& left, const Point<U, N>& right) { Sora::Math::SquaredNorm(left - right); }
    [[nodiscard]] constexpr auto
    SquaredDistance(const Point<T, N>& left,
                    const Point<U, N>& right) noexcept(noexcept(Sora::Math::SquaredNorm(left - right))) {
        return Sora::Math::SquaredNorm(left - right);
    }

    /** @brief Compute Euclidean distance between two points. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(const Point<T, N>& left, const Point<U, N>& right) { Sora::Math::Norm(left - right); }
    [[nodiscard]] constexpr auto Distance(const Point<T, N>& left,
                                          const Point<U, N>& right) noexcept(noexcept(Sora::Math::Norm(left - right))) {
        return Sora::Math::Norm(left - right);
    }

} // namespace Sora::Math
