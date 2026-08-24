/**
 * @file GeomUtils.h
 * @brief Zero-copy coordinate views and classification utilities for affine geometry values.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Math/Point.h>
#include <Sora/Math/ScalarUtils.h>

#include <concepts>
#include <cstddef>
#include <span>

namespace Sora::Math {

    /** @brief Convert point coordinates to a free vector value. */
    template<ValueCarrier T, std::size_t N>
    [[nodiscard]] constexpr Vector<T, N> ToVector(const Point<T, N>& point) noexcept {
        Vector<T, N> result;
        for (std::size_t index = 0; index < N; ++index) {
            result[index] = point[index];
        }
        return result;
    }

    /** @brief Convert vector coordinates to a point value. */
    template<ValueCarrier T, std::size_t N>
    [[nodiscard]] constexpr Point<T, N> ToPoint(const Vector<T, N>& vector) noexcept {
        Point<T, N> result;
        for (std::size_t index = 0; index < N; ++index) {
            result[index] = vector[index];
        }
        return result;
    }

    /** @brief Return whether any coordinate of @p value is NaN. */
    template<Sora::Concept::ArithmeticScalar T, std::size_t N>
    [[nodiscard]] constexpr bool IsNaN(const Vector<T, N>& value) noexcept {
        for (std::size_t index = 0; index < N; ++index) {
            if (IsNaN(value[index])) {
                return true;
            }
        }
        return false;
    }

    /** @brief Return whether no coordinate of @p value is NaN. */
    template<Sora::Concept::ArithmeticScalar T, std::size_t N>
    [[nodiscard]] constexpr bool IsNotNaN(const Vector<T, N>& value) noexcept {
        return !IsNaN(value);
    }

    /** @brief Return whether any coordinate of @p value is NaN. */
    template<Sora::Concept::ArithmeticScalar T, std::size_t N>
    [[nodiscard]] constexpr bool IsNaN(const Point<T, N>& value) noexcept {
        for (std::size_t index = 0; index < N; ++index) {
            if (IsNaN(value[index])) {
                return true;
            }
        }
        return false;
    }

    /** @brief Return whether no coordinate of @p value is NaN. */
    template<Sora::Concept::ArithmeticScalar T, std::size_t N>
    [[nodiscard]] constexpr bool IsNotNaN(const Point<T, N>& value) noexcept {
        return !IsNaN(value);
    }

    /** @brief Return a mutable vector-coordinate view of a point without allocating or copying. */
    template<std::floating_point T, std::size_t N>
    [[nodiscard]] constexpr std::span<T, N> AsVectorRef(Point<T, N>& value) noexcept {
        return std::span<T, N>{value.Data(), N};
    }

    /** @brief Return a read-only vector-coordinate view of a point without allocating or copying. */
    template<std::floating_point T, std::size_t N>
    [[nodiscard]] constexpr std::span<const T, N> AsVectorRef(const Point<T, N>& value) noexcept {
        return std::span<const T, N>{value.Data(), N};
    }

    /** @brief Return a mutable point-coordinate view of a vector without allocating or copying. */
    template<std::floating_point T, std::size_t N>
    [[nodiscard]] constexpr std::span<T, N> AsPointRef(Vector<T, N>& value) noexcept {
        return std::span<T, N>{value.Data(), N};
    }

    /** @brief Return a read-only point-coordinate view of a vector without allocating or copying. */
    template<std::floating_point T, std::size_t N>
    [[nodiscard]] constexpr std::span<const T, N> AsPointRef(const Vector<T, N>& value) noexcept {
        return std::span<const T, N>{value.Data(), N};
    }

} // namespace Sora::Math
