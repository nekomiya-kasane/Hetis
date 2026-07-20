/**
 * @file UnitVector.h
 * @brief Invariant-preserving unit vectors with checked scalar and SIMD normalization.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Point.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <meta>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief Failure reported when a free vector cannot define a unit direction. */
    enum class NormalizationError : uint8_t {
        ZeroLength, /**< At least one scalar or SIMD lane has exactly zero length. */
        NonFinite,  /**< At least one component is NaN or infinite. */
    };

    template<DifferentiableValue T, size_t N>
        requires(N > 0 && ValueCarrier<T>)
    class UnitVector;

    /** @brief Normalize @p value while preserving the unit-vector invariant in every scalar or SIMD lane. */
    template<DifferentiableValue T, size_t N>
        requires(N > 0 && ValueCarrier<T>)
    [[nodiscard]] constexpr auto TryNormalize(const Vector<T, N>& value) noexcept
        -> std::expected<UnitVector<T, N>, NormalizationError>;

    /**
     * @brief Fixed-size vector whose Euclidean norm is one within carrier rounding semantics.
     * @details Construction is available only through @ref TryNormalize. The wrapped free vector is exposed read-only,
     * so ordinary mutation cannot invalidate the normalized state.
     */
    template<DifferentiableValue T, size_t N>
        requires(N > 0 && ValueCarrier<T>)
    class UnitVector {
    public:
        using CarrierType = T;                  /**< Mathematical component carrier. */
        static constexpr size_t kDimension = N; /**< Number of vector components. */

        /** @brief Access a normalized component. @pre @p index is less than @c N. */
        [[nodiscard]] constexpr const T& operator[](size_t index) const& noexcept { return value_[index]; }

        [[nodiscard]] const T&
        operator[](size_t) const&& = delete ("A temporary UnitVector cannot expose a component reference");

        /** @brief Return the normalized free-vector representation without permitting mutation. */
        [[nodiscard]] constexpr const Vector<T, N>& Value() const& noexcept { return value_; }

        [[nodiscard]] const Vector<T, N>&
        Value() const&& = delete ("A temporary UnitVector cannot expose its stored vector by reference");

        /** @brief Reverse the direction while preserving unit length. */
        [[nodiscard]] constexpr UnitVector operator-() const noexcept(noexcept(-value_)) {
            return UnitVector{-value_, NormalizedTag{}};
        }

    private:
        struct NormalizedTag {};

        constexpr explicit UnitVector(Vector<T, N> value, NormalizedTag) noexcept : value_(std::move(value)) {}

        friend constexpr auto TryNormalize<T, N>(const Vector<T, N>& value) noexcept
            -> std::expected<UnitVector<T, N>, NormalizationError>;

        Vector<T, N> value_;
    };

    namespace Detail {

        template<DifferentiableValue T>
        [[nodiscard]] constexpr auto CarrierFinite(T value) noexcept {
            if constexpr (std::floating_point<T>) {
                if consteval {
                    constexpr T largest = std::numeric_limits<T>::max();
                    return value == value && -largest <= value && value <= largest;
                } else {
                    return std::isfinite(value);
                }
            } else {
                using Scalar = typename T::ValueType;
                const T largest(std::numeric_limits<Scalar>::max());
                return (value == value) & (Sora::Math::Abs(value) <= largest);
            }
        }

        template<DifferentiableValue T>
        [[nodiscard]] constexpr T CarrierAbs(T value) noexcept {
            if constexpr (std::floating_point<T>) {
                if consteval {
                    return value < T{0} ? -value : value;
                } else {
                    return Sora::Math::Abs(value);
                }
            } else {
                return Sora::Math::Abs(value);
            }
        }

        template<DifferentiableValue T>
        [[nodiscard]] constexpr bool AllLanes(auto mask) noexcept {
            if constexpr (std::floating_point<T>) {
                return mask;
            } else {
                return Simd::AllOf(mask);
            }
        }

        template<DifferentiableValue T>
        [[nodiscard]] constexpr T CarrierMax(T left, T right) noexcept {
            if constexpr (std::floating_point<T>) {
                return left < right ? right : left;
            } else {
                return Simd::Max(left, right);
            }
        }

    } // namespace Detail

    template<DifferentiableValue T, size_t N>
        requires(N > 0 && ValueCarrier<T>)
    [[nodiscard]] constexpr auto TryNormalize(const Vector<T, N>& value) noexcept
        -> std::expected<UnitVector<T, N>, NormalizationError> {
        auto finite = Detail::CarrierFinite(value[0]);
        T scale = Detail::CarrierAbs(value[0]);
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{1}, N))) {
            finite = finite & Detail::CarrierFinite(value[i]);
            scale = Detail::CarrierMax(scale, Detail::CarrierAbs(value[i]));
        }

        if (!Detail::AllLanes<T>(finite)) {
            return std::unexpected(NormalizationError::NonFinite);
        }
        if (!Detail::AllLanes<T>(scale > Detail::ZeroCarrier<T>())) {
            return std::unexpected(NormalizationError::ZeroLength);
        }

        Vector<T, N> scaled;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            scaled[i] = Sora::Math::Div(value[i], scale);
        }

        T squaredLength = Sora::Math::Square(scaled[0]);
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{1}, N))) {
            if consteval {
                squaredLength = Sora::Math::Add(Sora::Math::Square(scaled[i]), squaredLength);
            } else {
                squaredLength = Sora::Math::Fma(scaled[i], scaled[i], squaredLength);
            }
        }
        const T inverseLength = Sora::Math::Inv(Sora::Math::Sqrt(squaredLength));

        Vector<T, N> normalized;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            normalized[i] = Sora::Math::Mul(scaled[i], inverseLength);
        }
        return UnitVector<T, N>{std::move(normalized), typename UnitVector<T, N>::NormalizedTag{}};
    }

} // namespace Sora::Math
