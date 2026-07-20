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

    /**
     * @brief Point in an @p N-dimensional affine space over carrier @p T.
     * @details Point addition is intentionally absent. Subtracting points yields a free vector; translating a point
     * by a vector yields a point. The distinct type makes these affine rules compile-time properties.
     */
    template<ValueCarrier T, size_t N>
        requires(N > 0)
    class Point {
    public:
        using CarrierType = T;                  /**< Coordinate carrier. */
        static constexpr size_t kDimension = N; /**< Number of point coordinates. */

        /** @brief Construct the affine origin. */
        constexpr Point() noexcept { values_.fill(Detail::ZeroCarrier<T>()); }

        /** @brief Construct from the exact coordinate array. */
        constexpr explicit Point(std::array<T, N> values) noexcept(std::is_nothrow_move_constructible_v<T>)
            : values_(std::move(values)) {}

        /** @brief Construct from exactly @p N coordinate values. */
        template<typename... U>
            requires(sizeof...(U) == N && (std::constructible_from<T, U &&> && ...))
        constexpr explicit(!(std::convertible_to<U&&, T> && ...))
            Point(U&&... values) noexcept((std::is_nothrow_constructible_v<T, U&&> && ...))
            : values_{T(std::forward<U>(values))...} {}

        /** @brief Access one coordinate while preserving cv-ref qualification. @pre @p index is less than @c N. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) operator[](this Self&& self, size_t index) noexcept {
            return std::forward_like<Self>(self.values_[index]);
        }

        /** @brief Return the contiguous coordinate array while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Coordinates(this Self&& self) noexcept {
            return std::forward_like<Self>(self.values_);
        }

        /** @brief Return the first coordinate address from an lvalue object. */
        template<typename Self>
            requires std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto Data(this Self&& self) noexcept {
            return self.values_.data();
        }

    private:
        std::array<T, N> values_{};
    };

    /** @brief Subtract two points to obtain their displacement vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } && ValueCarrier<Detail::SubResult<T, U>>
    [[nodiscard]] constexpr auto
    operator-(const Point<T, N>& left, const Point<U, N>& right) noexcept(noexcept(Sora::Math::Sub(left[0], right[0])))
        -> Vector<Detail::SubResult<T, U>, N> {
        Vector<Detail::SubResult<T, U>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Sub(left[i], right[i]);
        }
        return result;
    }

    /** @brief Translate a point by a vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T point, U vector) { Sora::Math::Add(point, vector); } &&
                 ValueCarrier<Detail::AddResult<T, U>>
    [[nodiscard]] constexpr auto operator+(const Point<T, N>& point, const Vector<U, N>& vector) noexcept(
        noexcept(Sora::Math::Add(point[0], vector[0]))) -> Point<Detail::AddResult<T, U>, N> {
        Point<Detail::AddResult<T, U>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Add(point[i], vector[i]);
        }
        return result;
    }

    /** @brief Translate a point by a vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T vector, U point) { Sora::Math::Add(vector, point); } &&
                 ValueCarrier<Detail::AddResult<T, U>>
    [[nodiscard]] constexpr auto operator+(const Vector<T, N>& vector, const Point<U, N>& point) noexcept(
        noexcept(Sora::Math::Add(vector[0], point[0]))) -> Point<Detail::AddResult<T, U>, N> {
        Point<Detail::AddResult<T, U>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Add(vector[i], point[i]);
        }
        return result;
    }

    /** @brief Translate a point by the negation of a vector. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T point, U vector) { Sora::Math::Sub(point, vector); } &&
                 ValueCarrier<Detail::SubResult<T, U>>
    [[nodiscard]] constexpr auto operator-(const Point<T, N>& point, const Vector<U, N>& vector) noexcept(
        noexcept(Sora::Math::Sub(point[0], vector[0]))) -> Point<Detail::SubResult<T, U>, N> {
        Point<Detail::SubResult<T, U>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Sub(point[i], vector[i]);
        }
        return result;
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
