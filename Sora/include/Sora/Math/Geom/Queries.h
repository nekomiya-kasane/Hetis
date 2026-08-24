/**
 * @file Queries.h
 * @brief Branch-transparent geometric queries over Sora mathematical primitives.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Geom/Primitives.h>
#include <Sora/Math/GeomUtils.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <type_traits>

namespace Sora::Math {

    namespace Detail {

        template<Sora::Concept::ArithmeticScalar T, size_t N>
        [[nodiscard]] constexpr T ComponentMin(const Point<T, N>& left, const Point<T, N>& right,
                                               size_t index) noexcept {
            return left[index] < right[index] ? left[index] : right[index];
        }

        template<Sora::Concept::ArithmeticScalar T, size_t N>
        [[nodiscard]] constexpr T ComponentMax(const Point<T, N>& left, const Point<T, N>& right,
                                               size_t index) noexcept {
            return left[index] > right[index] ? left[index] : right[index];
        }

        template<Sora::Concept::ArithmeticScalar T>
        [[nodiscard]] constexpr T Difference(T minimum, T maximum) noexcept {
            SORA_PRE(minimum <= maximum);
            if constexpr (std::integral<T> && std::is_signed_v<T>) {
                using Unsigned = std::make_unsigned_t<T>;
                const Unsigned difference = static_cast<Unsigned>(maximum) - static_cast<Unsigned>(minimum);
                SORA_PRE(difference <= static_cast<Unsigned>((std::numeric_limits<T>::max)()));
                return static_cast<T>(difference);
            } else {
                return maximum - minimum;
            }
        }

        template<Sora::Concept::ArithmeticScalar T>
        using HalfExtentScalar =
            std::conditional_t<std::integral<T>,
                               std::conditional_t<(sizeof(T) <= sizeof(std::uint32_t)), double, long double>, T>;

    } // namespace Detail

    /** @brief Return the length of a one-dimensional box. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr auto Length(const Box<T, 1>& interval) noexcept {
        if constexpr (std::integral<T>) {
            using Unsigned = std::make_unsigned_t<T>;
            return static_cast<Unsigned>(interval.Upper()) - static_cast<Unsigned>(interval.Lower());
        } else {
            return interval.Upper() - interval.Lower();
        }
    }

    /** @brief Return the midpoint of a one-dimensional box without constructing a second box. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr T Midpoint(const Box<T, 1>& interval) noexcept {
        return std::midpoint(interval.Lower(), interval.Upper());
    }

    /** @brief Test inclusive containment in a one-dimensional box. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr bool Contains(const Box<T, 1>& interval, T value) noexcept {
        return value >= interval.Lower() && value <= interval.Upper();
    }

    /** @brief Test inclusive intersection of two one-dimensional boxes. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr bool Intersects(const Box<T, 1>& left, const Box<T, 1>& right) noexcept {
        return left.Lower() <= right.Upper() && right.Lower() <= left.Upper();
    }

    /** @brief Return the smallest one-dimensional box containing both arguments. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr Box<T, 1> Union(const Box<T, 1>& left, const Box<T, 1>& right) noexcept {
        return Box<T, 1>{left.Lower() < right.Lower() ? left.Lower() : right.Lower(),
                         left.Upper() > right.Upper() ? left.Upper() : right.Upper()};
    }

    /** @brief Return the smallest one-dimensional box containing a box and a scalar. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr Box<T, 1> Union(const Box<T, 1>& interval, T value) noexcept {
        return Box<T, 1>{interval.Lower() < value ? interval.Lower() : value,
                         interval.Upper() > value ? interval.Upper() : value};
    }

    /** @brief Return the common one-dimensional box, or empty when the inputs are disjoint. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr std::optional<Box<T, 1>> Intersection(const Box<T, 1>& left,
                                                                  const Box<T, 1>& right) noexcept {
        const T lower = left.Lower() > right.Lower() ? left.Lower() : right.Lower();
        const T upper = left.Upper() < right.Upper() ? left.Upper() : right.Upper();
        if (lower > upper) {
            return std::nullopt;
        }
        return Box<T, 1>{lower, upper};
    }

    /** @brief Expand a one-dimensional box by a non-negative margin on both sides. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr Box<T, 1> Expand(const Box<T, 1>& interval, T margin) noexcept {
        SORA_PRE(IsNotNaN(margin));
        SORA_PRE(margin >= T{0});
        if constexpr (std::integral<T>) {
            if constexpr (std::is_signed_v<T>) {
                SORA_PRE((std::numeric_limits<T>::lowest)() + margin <= interval.Lower());
            } else {
                SORA_PRE(margin <= interval.Lower());
            }
            SORA_PRE(interval.Upper() <= (std::numeric_limits<T>::max)() - margin);
        }
        return Box<T, 1>{interval.Lower() - margin, interval.Upper() + margin};
    }

    /** @brief Clamp a scalar to a closed interval. */
    template<Sora::Concept::ArithmeticScalar T>
    [[nodiscard]] constexpr T ClampTo(T value, const Box<T, 1>& interval) noexcept {
        return value < interval.Lower() ? interval.Lower() : (value > interval.Upper() ? interval.Upper() : value);
    }

    /** @brief Return the smallest box containing both boxes. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Box<T, N> Union(const Box<T, N>& left, const Box<T, N>& right) noexcept {
        Point<T, N> minimum;
        Point<T, N> maximum;
        for (size_t i = 0; i < N; ++i) {
            minimum[i] = Detail::ComponentMin(left.Min(), right.Min(), i);
            maximum[i] = Detail::ComponentMax(left.Max(), right.Max(), i);
        }
        return Box<T, N>{std::move(minimum), std::move(maximum)};
    }

    /** @brief Grow a box to include a point. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Box<T, N> Union(const Box<T, N>& box, const Point<T, N>& point) noexcept {
        Point<T, N> minimum = box.Min();
        Point<T, N> maximum = box.Max();
        for (size_t i = 0; i < N; ++i) {
            minimum[i] = minimum[i] < point[i] ? minimum[i] : point[i];
            maximum[i] = maximum[i] > point[i] ? maximum[i] : point[i];
        }
        return Box<T, N>{std::move(minimum), std::move(maximum)};
    }

    /** @brief Return the common box, or empty when any axis is disjoint. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr std::optional<Box<T, N>> Intersection(const Box<T, N>& left,
                                                                  const Box<T, N>& right) noexcept {
        Point<T, N> minimum;
        Point<T, N> maximum;
        for (size_t i = 0; i < N; ++i) {
            minimum[i] = left.Min()[i] > right.Min()[i] ? left.Min()[i] : right.Min()[i];
            maximum[i] = left.Max()[i] < right.Max()[i] ? left.Max()[i] : right.Max()[i];
            if (minimum[i] > maximum[i]) {
                return std::nullopt;
            }
        }
        return Box<T, N>{std::move(minimum), std::move(maximum)};
    }

    /** @brief Test inclusive box intersection. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr bool Intersects(const Box<T, N>& left, const Box<T, N>& right) noexcept {
        for (size_t i = 0; i < N; ++i) {
            if (left.Min()[i] > right.Max()[i] || right.Min()[i] > left.Max()[i]) {
                return false;
            }
        }
        return true;
    }

    /** @brief Test inclusive containment of a point in a box. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr bool Contains(const Box<T, N>& box, const Point<T, N>& point) noexcept {
        for (size_t i = 0; i < N; ++i) {
            if (point[i] < box.Min()[i] || point[i] > box.Max()[i]) {
                return false;
            }
        }
        return true;
    }

    /** @brief Squared distance from a point to a box, zero for an interior point. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr T SquaredDistance(const Point<T, N>& point, const Box<T, N>& box) noexcept {
        Vector<T, N> delta;
        for (size_t i = 0; i < N; ++i) {
            const T below = box.Min()[i] - point[i];
            const T above = point[i] - box.Max()[i];
            delta[i] = below > T{0} ? below : (above > T{0} ? above : T{0});
        }
        return Sora::Math::SquaredNorm(delta);
    }

    /** @brief Return the closest point on or inside a box. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Point<T, N> ClosestPoint(const Box<T, N>& box, const Point<T, N>& point) noexcept {
        Point<T, N> result;
        for (size_t i = 0; i < N; ++i) {
            result[i] = point[i] < box.Min()[i] ? box.Min()[i] : (point[i] > box.Max()[i] ? box.Max()[i] : point[i]);
        }
        return result;
    }

    /** @brief Return the box centroid. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Point<T, N> Centroid(const Box<T, N>& box) noexcept {
        Point<T, N> result;
        for (size_t i = 0; i < N; ++i) {
            result[i] = std::midpoint(box.Min()[i], box.Max()[i]);
        }
        return result;
    }

    /** @brief Return the full non-negative size along every box axis. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Vector<T, N> Size(const Box<T, N>& box) noexcept {
        Vector<T, N> result;
        for (size_t i = 0; i < N; ++i) {
            result[i] = Detail::Difference(box.Min()[i], box.Max()[i]);
        }
        return result;
    }

    /** @brief Return half-extents as a free vector; integral boxes use a promoted floating carrier. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Vector<Detail::HalfExtentScalar<T>, N> Extents(const Box<T, N>& box) noexcept {
        using Result = Detail::HalfExtentScalar<T>;
        Vector<Result, N> result;
        for (size_t i = 0; i < N; ++i) {
            if constexpr (std::integral<T>) {
                result[i] =
                    static_cast<Result>(box.Max()[i]) * Result{0.5} - static_cast<Result>(box.Min()[i]) * Result{0.5};
            } else {
                result[i] = (box.Max()[i] - box.Min()[i]) * Result{0.5};
            }
        }
        return result;
    }

    /** @brief Expand a box by a non-negative margin on every axis. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr Box<T, N> Expand(const Box<T, N>& box, T margin) noexcept {
        SORA_PRE(IsNotNaN(margin));
        SORA_PRE(margin >= T{0});
        Point<T, N> minimum;
        Point<T, N> maximum;
        for (size_t i = 0; i < N; ++i) {
            if constexpr (std::integral<T>) {
                if constexpr (std::is_signed_v<T>) {
                    SORA_PRE((std::numeric_limits<T>::lowest)() + margin <= box.Min()[i]);
                } else {
                    SORA_PRE(margin <= box.Min()[i]);
                }
                SORA_PRE(box.Max()[i] <= (std::numeric_limits<T>::max)() - margin);
            }
            minimum[i] = box.Min()[i] - margin;
            maximum[i] = box.Max()[i] + margin;
        }
        return Box<T, N>{minimum, maximum};
    }

    /** @brief Return the dominant box extent axis. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
    [[nodiscard]] constexpr size_t LargestAxis(const Box<T, N>& box) noexcept {
        size_t result = 0;
        const Vector<T, N> extent = Size(box);
        for (size_t i = 1; i < N; ++i) {
            if (extent[i] > extent[result]) {
                result = i;
            }
        }
        return result;
    }

    /** @brief Test inclusive point containment in a ball. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr bool Contains(const Ball<T, N>& ball, const Point<T, N>& point) noexcept {
        return SquaredDistance(ball.Center(), point) <= ball.Radius() * ball.Radius();
    }

    /** @brief Test inclusive intersection of two balls. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr bool Intersects(const Ball<T, N>& left, const Ball<T, N>& right) noexcept {
        const T radius = left.Radius() + right.Radius();
        return SquaredDistance(left.Center(), right.Center()) <= radius * radius;
    }

    /** @brief Test inclusive ball-box intersection. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr bool Intersects(const Ball<T, N>& ball, const Box<T, N>& box) noexcept {
        return SquaredDistance(ball.Center(), box) <= ball.Radius() * ball.Radius();
    }

    /** @brief Return the axis-aligned bounds of a ball. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr Box<T, N> Bounds(const Ball<T, N>& ball) noexcept {
        Point<T, N> minimum;
        Point<T, N> maximum;
        for (size_t i = 0; i < N; ++i) {
            minimum[i] = ball.Center()[i] - ball.Radius();
            maximum[i] = ball.Center()[i] + ball.Radius();
        }
        return Box<T, N>{std::move(minimum), std::move(maximum)};
    }

    /** @brief Return the signed distance to a normalized hyperplane. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr T SignedDistance(const Hyperplane<T, N>& plane, const Point<T, N>& point) noexcept {
        return Dot(plane.Normal().Value(), ToVector(point)) + plane.Offset();
    }

    /** @brief Return the side of a normalized hyperplane containing a point. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr int Side(const Hyperplane<T, N>& plane, const Point<T, N>& point) noexcept {
        const T distance = SignedDistance(plane, point);
        return distance > T{0} ? 1 : (distance < T{0} ? -1 : 0);
    }

    /** @brief Test a box against all six frustum half-spaces. */
    template<std::floating_point T>
    [[nodiscard]] constexpr bool Intersects(const Box<T, 3>& box, const Frustum<T>& frustum) noexcept {
        for (const ClipPlane<T>& plane : frustum.planes) {
            const Point<T, 3>& positive = box.Max();
            const Point<T, 3>& negative = box.Min();
            const T x = plane.normal[0] >= T{0} ? positive[0] : negative[0];
            const T y = plane.normal[1] >= T{0} ? positive[1] : negative[1];
            const T z = plane.normal[2] >= T{0} ? positive[2] : negative[2];
            if (plane.normal[0] * x + plane.normal[1] * y + plane.normal[2] * z + plane.offset < T{0}) {
                return false;
            }
        }
        return true;
    }

    /** @brief Prepared ray data for branch-stable slab intersection. */
    template<std::floating_point T, size_t N>
    struct PreparedRay {
        Point<T, N> origin{};
        Vector<T, N> inverseDirection{};
        std::array<bool, N> parallel{};
    };

    /** @brief Precompute reciprocal direction values without epsilon-based distortion. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr PreparedRay<T, N> Prepare(const Ray<T, N>& ray) noexcept {
        PreparedRay<T, N> result{.origin = ray.Origin()};
        for (size_t i = 0; i < N; ++i) {
            result.parallel[i] = ray.Direction()[i] == T{0};
            result.inverseDirection[i] = result.parallel[i] ? T{0} : T{1} / ray.Direction()[i];
        }
        return result;
    }

    /** @brief Return the inclusive `[near, far]` ray parameter interval, or empty on a miss. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr std::optional<RayHit<T>> Intersect(const PreparedRay<T, N>& ray,
                                                               const Box<T, N>& box) noexcept {
        T near = T{0};
        T far = std::numeric_limits<T>::infinity();
        for (size_t i = 0; i < N; ++i) {
            if (ray.parallel[i]) {
                if (ray.origin[i] < box.Min()[i] || ray.origin[i] > box.Max()[i]) {
                    return std::nullopt;
                }
                continue;
            }
            const T first = (box.Min()[i] - ray.origin[i]) * ray.inverseDirection[i];
            const T second = (box.Max()[i] - ray.origin[i]) * ray.inverseDirection[i];
            const T axisNear = first < second ? first : second;
            const T axisFar = first > second ? first : second;
            near = near > axisNear ? near : axisNear;
            far = far < axisFar ? far : axisFar;
            if (near > far) {
                return std::nullopt;
            }
        }
        return RayHit<T>{.near = near, .far = far};
    }

    /** @brief Convenience overload that prepares a ray before intersection. */
    template<std::floating_point T, size_t N>
    [[nodiscard]] constexpr std::optional<RayHit<T>> Intersect(const Ray<T, N>& ray, const Box<T, N>& box) noexcept {
        return Intersect(Prepare(ray), box);
    }

} // namespace Sora::Math
