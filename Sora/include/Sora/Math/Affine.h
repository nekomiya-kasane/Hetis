/**
 * @file Affine.h
 * @brief Fixed-dimensional affine maps shared by mathematical geometry and transforms.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Contracts.h>
#include <Sora/ErrorCode.h>
#include <Sora/Math/GeomUtils.h>
#include <Sora/Math/Matrix.h>
#include <Sora/Math/Point.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <utility>

namespace Sora::Math {

    /**
     * @brief Affine map `x -> linear * x + translation` in an @p N-dimensional space.
     * @details The representation is mathematical, not a GPU or interchange ABI. It has no hidden allocation and is
     * composed with the same fixed-size Matrix and Vector carriers used by the rest of Sora Math.
     */
    template<ValueCarrier T, std::size_t N>
        requires(N > 0)
    class Affine {
    public:
        using CarrierType = T;
        static constexpr std::size_t kDimension = N;
        using LinearType = Matrix<T, N, N>;
        using TranslationType = Vector<T, N>;

        /** @brief Construct the identity affine map. */
        constexpr Affine() noexcept : linear_(LinearType::Identity()) {}

        /** @brief Construct an affine map from its linear and translation parts. */
        constexpr explicit Affine(LinearType linear, TranslationType translation) noexcept
            : linear_(std::move(linear)), translation_(std::move(translation)) {}

        /** @brief Return the identity map. */
        [[nodiscard]] static constexpr Affine Identity() noexcept { return {}; }

        /** @brief Return a pure translation. */
        [[nodiscard]] static constexpr Affine Translation(TranslationType translation) noexcept {
            return Affine{LinearType::Identity(), std::move(translation)};
        }

        /** @brief Return a pure linear map. */
        [[nodiscard]] static constexpr Affine Linear(LinearType linear) noexcept {
            return Affine{std::move(linear), TranslationType{}};
        }

        [[nodiscard]] constexpr const LinearType& LinearPart() const noexcept { return linear_; }
        [[nodiscard]] constexpr const TranslationType& TranslationPart() const noexcept { return translation_; }

    private:
        LinearType linear_{};
        TranslationType translation_{};
    };

    /** @brief Apply an affine map to an affine point; integral results must be representable. */
    template<ValueCarrier T, std::size_t N>
    [[nodiscard]] constexpr Point<T, N> TransformPoint(const Affine<T, N>& affine, const Point<T, N>& point) noexcept {
        if constexpr (std::integral<T>) {
            Point<T, N> result;
            for (std::size_t row = 0; row < N; ++row) {
                WideInteger<T> value = static_cast<WideInteger<T>>(affine.TranslationPart()[row]);
                for (std::size_t column = 0; column < N; ++column) {
                    value += static_cast<WideInteger<T>>(affine.LinearPart()(row, column)) *
                             static_cast<WideInteger<T>>(point[column]);
                }
                result[row] = CheckedCast<T>(value);
            }
            return result;
        }
        return ToPoint(affine.LinearPart() * ToVector(point)) + affine.TranslationPart();
    }

    /** @brief Apply only the linear part of an affine map; integral results must be representable. */
    template<ValueCarrier T, std::size_t N>
    [[nodiscard]] constexpr Vector<T, N> TransformVector(const Affine<T, N>& affine,
                                                         const Vector<T, N>& vector) noexcept {
        if constexpr (std::integral<T>) {
            Vector<T, N> result;
            for (std::size_t row = 0; row < N; ++row) {
                WideInteger<T> value = 0;
                for (std::size_t column = 0; column < N; ++column) {
                    value += static_cast<WideInteger<T>>(affine.LinearPart()(row, column)) *
                             static_cast<WideInteger<T>>(vector[column]);
                }
                result[row] = CheckedCast<T>(value);
            }
            return result;
        }
        return affine.LinearPart() * vector;
    }

    /** @brief Compose two affine maps so that `Compose(left, right)(x) = left(right(x))`. */
    template<ValueCarrier T, std::size_t N>
    [[nodiscard]] constexpr Affine<T, N> Compose(const Affine<T, N>& left, const Affine<T, N>& right) noexcept {
        return Affine<T, N>{left.LinearPart() * right.LinearPart(),
                            left.LinearPart() * right.TranslationPart() + left.TranslationPart()};
    }

    /** @brief Return the inverse of a nonsingular affine map. */
    template<std::floating_point T, std::size_t N>
    [[nodiscard]] constexpr std::expected<Affine<T, N>, Sora::ErrorCode>
    TryInverse(const Affine<T, N>& affine) noexcept {
        std::array<std::array<T, 2 * N>, N> augmented{};
        for (std::size_t row = 0; row < N; ++row) {
            for (std::size_t column = 0; column < N; ++column) {
                augmented[row][column] = affine.LinearPart()(row, column);
            }
            augmented[row][N + row] = T{1};
        }

        for (std::size_t column = 0; column < N; ++column) {
            std::size_t pivot = column;
            for (std::size_t row = column + 1; row < N; ++row) {
                if (Abs(augmented[row][column]) > Abs(augmented[pivot][column])) {
                    pivot = row;
                }
            }
            if (augmented[pivot][column] == T{0}) {
                return std::unexpected(Sora::ErrorCode::SingularMatrix);
            }
            if (pivot != column) {
                std::swap(augmented[pivot], augmented[column]);
            }

            const T inversePivot = T{1} / augmented[column][column];
            for (std::size_t value = 0; value < 2 * N; ++value) {
                augmented[column][value] *= inversePivot;
            }
            for (std::size_t row = 0; row < N; ++row) {
                if (row == column) {
                    continue;
                }
                const T factor = augmented[row][column];
                for (std::size_t value = 0; value < 2 * N; ++value) {
                    augmented[row][value] -= factor * augmented[column][value];
                }
            }
        }

        typename Affine<T, N>::LinearType inverseLinear;
        for (std::size_t row = 0; row < N; ++row) {
            for (std::size_t column = 0; column < N; ++column) {
                inverseLinear(row, column) = augmented[row][N + column];
            }
        }
        return Affine<T, N>{inverseLinear, -(inverseLinear * affine.TranslationPart())};
    }

} // namespace Sora::Math
