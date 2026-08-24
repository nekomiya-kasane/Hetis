/**
 * @file Transform.h
 * @brief Affine transformation laws for mathematical geometry.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Affine.h>
#include <Sora/Math/Geom/Queries.h>

#include <concepts>
#include <utility>

namespace Sora::Math {

    namespace Detail {

        template<Sora::Concept::ArithmeticScalar T, size_t N>
        [[nodiscard]] constexpr Point<T, N> Corner(const Box<T, N>& box, size_t mask) noexcept {
            Point<T, N> result;
            for (size_t axis = 0; axis < N; ++axis) {
                result[axis] = (mask & (size_t{1} << axis)) ? box.Max()[axis] : box.Min()[axis];
            }
            return result;
        }

        template<std::floating_point T, size_t N>
        [[nodiscard]] constexpr T RadiusScale(const Affine<T, N>& affine) noexcept {
            T oneNorm = T{0};
            T infinityNorm = T{0};
            for (size_t column = 0; column < N; ++column) {
                T sum = T{0};
                for (size_t row = 0; row < N; ++row) {
                    sum += Abs(affine.LinearPart()(row, column));
                }
                oneNorm = oneNorm > sum ? oneNorm : sum;
            }
            for (size_t row = 0; row < N; ++row) {
                T sum = T{0};
                for (size_t column = 0; column < N; ++column) {
                    sum += Abs(affine.LinearPart()(row, column));
                }
                infinityNorm = infinityNorm > sum ? infinityNorm : sum;
            }
            return Math::Sqrt(oneNorm * infinityNorm);
        }

    } // namespace Detail

    /** @brief Transform a ray by applying the affine map to its origin and linear part to its direction. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr Ray<T, N> Transform(const Affine<T, N>& affine, const Ray<T, N>& ray) noexcept {
        return Ray<T, N>{TransformPoint(affine, ray.Origin()), TransformVector(affine, ray.Direction())};
    }

    /** @brief Transform a box by mapping every corner and refitting an axis-aligned box.
     * @pre Integral affine corner results are representable in @p T.
     */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Box<T, N> Transform(const Affine<T, N>& affine, const Box<T, N>& box) noexcept {
        Point<T, N> minimum = TransformPoint(affine, Detail::Corner(box, 0));
        Point<T, N> maximum = minimum;
        for (size_t mask = 1; mask < (size_t{1} << N); ++mask) {
            const Point<T, N> corner = TransformPoint(affine, Detail::Corner(box, mask));
            for (size_t axis = 0; axis < N; ++axis) {
                minimum[axis] = minimum[axis] < corner[axis] ? minimum[axis] : corner[axis];
                maximum[axis] = maximum[axis] > corner[axis] ? maximum[axis] : corner[axis];
            }
        }
        return Box<T, N>{std::move(minimum), std::move(maximum)};
    }

    /** @brief Transform a base/extent rectangle and return its fitted axis-aligned box.
     * @pre Integral affine corner results are representable in @p T.
     */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Box<T, N> Transform(const Affine<T, N>& affine, const Rect<T, N>& rect) noexcept {
        return Transform(affine, rect.ToBox());
    }

    /** @brief Transform a ball conservatively under arbitrary linear distortion. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr Ball<T, N> Transform(const Affine<T, N>& affine, const Ball<T, N>& ball) noexcept {
        return Ball<T, N>{TransformPoint(affine, ball.Center()), ball.Radius() * Detail::RadiusScale(affine)};
    }

    /** @brief Transform a normalized hyperplane; a singular affine map violates the precondition. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr Hyperplane<T, N> Transform(const Affine<T, N>& affine,
                                                       const Hyperplane<T, N>& plane) noexcept {
        const auto inverse = TryInverse(affine);
        SORA_PRE(inverse.has_value());
        const auto covector = Transpose(inverse->LinearPart()) * plane.Normal().Value();
        const T offset = plane.Offset() - Dot(covector, affine.TranslationPart());
        return Hyperplane<T, N>{covector, offset};
    }

    /** @brief Transform all raw clip planes of a frustum as affine covectors. */
    template<std::floating_point T>
    [[nodiscard]] constexpr Frustum<T> Transform(const Affine<T, 3>& affine, const Frustum<T>& frustum) noexcept {
        const auto inverse = TryInverse(affine);
        SORA_PRE(inverse.has_value());
        Frustum<T> result;
        for (size_t i = 0; i < result.planes.size(); ++i) {
            result.planes[i].normal = Transpose(inverse->LinearPart()) * frustum.planes[i].normal;
            result.planes[i].offset = frustum.planes[i].offset - Dot(result.planes[i].normal, affine.TranslationPart());
        }
        return result;
    }

} // namespace Sora::Math
