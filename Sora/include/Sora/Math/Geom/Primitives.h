/**
 * @file Primitives.h
 * @brief Invariant-aware geometric value types built on Sora mathematical values.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Contracts.h>
#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Math/GeomUtils.h>
#include <Sora/Math/Point.h>
#include <Sora/Math/UnitVector.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief Non-empty axis-aligned box; Box<T, 1> is the canonical closed scalar interval. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
        requires(N > 0)
    class Box {
    public:
        static constexpr size_t kDimension = N;

        /** @brief Construct the degenerate box at the affine origin. */
        constexpr Box() noexcept = default;

        /** @brief Construct an ordered axis-aligned box; infinities are valid, NaN is not. */
        constexpr Box(const Point<T, N>& minimum, const Point<T, N>& maximum) noexcept
            : minimum_(minimum), maximum_(maximum) {
            ValidateBounds(minimum, maximum);
        }

        /** @brief Construct a one-dimensional box from its scalar lower and upper bounds. */
        constexpr Box(T minimum, T maximum) noexcept
            requires(N == 1)
            : minimum_(minimum), maximum_(maximum) {
            ValidateBounds(minimum_, maximum_);
        }

        /** @brief Return the minimum corner of the box. */
        [[nodiscard]] constexpr const Point<T, N>& Min() const noexcept { return minimum_; }

        /** @brief Return the maximum corner of the box. */
        [[nodiscard]] constexpr const Point<T, N>& Max() const noexcept { return maximum_; }

        /** @brief Return the extent along the first coordinate axis. */
        [[nodiscard]] constexpr T Width() const noexcept
            requires(N >= 1)
        {
            return maximum_[0] - minimum_[0];
        }

        /** @brief Return the extent along the second coordinate axis when present. */
        [[nodiscard]] constexpr T Height() const noexcept
            requires(N >= 2)
        {
            return maximum_[1] - minimum_[1];
        }

        /** @brief Return the extent along the third coordinate axis when present. */
        [[nodiscard]] constexpr T Depth() const noexcept
            requires(N >= 3)
        {
            return maximum_[2] - minimum_[2];
        }

        /** @brief Return the lower bound of a one-dimensional box. */
        [[nodiscard]] constexpr T Lower() const noexcept
            requires(N == 1)
        {
            return minimum_[0];
        }

        /** @brief Return the upper bound of a one-dimensional box. */
        [[nodiscard]] constexpr T Upper() const noexcept
            requires(N == 1)
        {
            return maximum_[0];
        }

    private:
        static constexpr void ValidateBounds(const Point<T, N>& minimum, const Point<T, N>& maximum) noexcept {
            SORA_PRE(IsNotNaN(minimum));
            SORA_PRE(IsNotNaN(maximum));
            for (size_t i = 0; i < N; ++i) {
                SORA_PRE(minimum[i] <= maximum[i]);
            }
        }

        Point<T, N> minimum_{};
        Point<T, N> maximum_{};
    };

    /** @brief Closed axis-aligned box represented by its minimum point and non-negative extent. */
    template<Sora::Concept::ArithmeticScalar T, size_t N>
        requires(N > 0)
    class Rect {
    public:
        static constexpr size_t kDimension = N;

        /** @brief Construct the degenerate rectangle at the affine origin. */
        constexpr Rect() noexcept = default;

        /** @brief Construct a closed box from its minimum point and coordinate-wise extent. */
        constexpr Rect(const Point<T, N>& base, const Vector<T, N>& extent) noexcept : base_(base), extent_(extent) {
            Validate(base_, extent_);
        }

        /** @brief Construct the base/extent representation from an axis-aligned box. */
        constexpr explicit Rect(const Box<T, N>& box) noexcept
            : base_(box.Min()), extent_(Difference(box.Min(), box.Max())) {
            Validate(base_, extent_);
        }

        [[nodiscard]] constexpr const Point<T, N>& Base() const noexcept { return base_; }
        [[nodiscard]] constexpr const Vector<T, N>& Extent() const noexcept { return extent_; }
        [[nodiscard]] constexpr const Point<T, N>& Min() const noexcept { return base_; }

        /** @brief Return the extent along the first coordinate axis. */
        [[nodiscard]] constexpr T Width() const noexcept
            requires(N >= 1)
        {
            return extent_[0];
        }

        /** @brief Return the extent along the second coordinate axis when present. */
        [[nodiscard]] constexpr T Height() const noexcept
            requires(N >= 2)
        {
            return extent_[1];
        }

        /** @brief Return the extent along the third coordinate axis when present. */
        [[nodiscard]] constexpr T Depth() const noexcept
            requires(N >= 3)
        {
            return extent_[2];
        }

        /** @brief Return the maximum point; construction guarantees that no scalar overflow occurs. */
        [[nodiscard]] constexpr Point<T, N> Max() const noexcept { return base_ + extent_; }

        /** @brief Convert this base/extent representation to the canonical minimum/maximum box. */
        [[nodiscard]] constexpr Box<T, N> ToBox() const noexcept { return Box<T, N>{base_, Max()}; }

    private:
        static constexpr Vector<T, N> Difference(const Point<T, N>& minimum, const Point<T, N>& maximum) noexcept {
            SORA_PRE(IsNotNaN(minimum));
            SORA_PRE(IsNotNaN(maximum));
            Vector<T, N> result;
            for (size_t axis = 0; axis < N; ++axis) {
                SORA_PRE(minimum[axis] <= maximum[axis]);
                if constexpr (std::integral<T> && std::is_signed_v<T>) {
                    using Unsigned = std::make_unsigned_t<T>;
                    const Unsigned difference =
                        static_cast<Unsigned>(maximum[axis]) - static_cast<Unsigned>(minimum[axis]);
                    SORA_PRE(difference <= static_cast<Unsigned>((std::numeric_limits<T>::max)()));
                    result[axis] = static_cast<T>(difference);
                } else {
                    result[axis] = maximum[axis] - minimum[axis];
                }
            }
            return result;
        }

        static constexpr void Validate(const Point<T, N>& base, const Vector<T, N>& extent) noexcept {
            SORA_PRE(IsNotNaN(base));
            SORA_PRE(IsNotNaN(extent));
            for (size_t axis = 0; axis < N; ++axis) {
                SORA_PRE(extent[axis] >= T{0});
                if constexpr (std::floating_point<T>) {
                    SORA_PRE(IsNotNaN(base[axis] + extent[axis]));
                } else {
                    if (extent[axis] >= T{0}) {
                        SORA_PRE(base[axis] <= (std::numeric_limits<T>::max)() - extent[axis]);
                    }
                }
            }
        }

        Point<T, N> base_{};
        Vector<T, N> extent_{};
    };

    /** @brief Box with an explicit orthonormal direction frame and non-negative half-extents. */
    template<std::floating_point T, size_t N>
        requires(N > 0)
    class DirectedBox {
    public:
        using Axis = UnitVector<T, N>;
        using Axes = std::array<Axis, N>;
        static constexpr size_t kDimension = N;

        /** @brief Construct a directed box from its center, orthonormal axes, and half-extents. */
        constexpr DirectedBox(const Point<T, N>& center, Axes axes, const Vector<T, N>& halfExtents) noexcept
            : center_(center), axes_(std::move(axes)), halfExtents_(halfExtents) {
            Validate(center_, axes_, halfExtents_);
        }

        [[nodiscard]] constexpr const Point<T, N>& Center() const noexcept { return center_; }
        [[nodiscard]] constexpr const Axes& AxesFrame() const noexcept { return axes_; }
        [[nodiscard]] constexpr const Axis& AxisAt(size_t index) const noexcept { return axes_[index]; }
        [[nodiscard]] constexpr const Vector<T, N>& HalfExtents() const noexcept { return halfExtents_; }

    private:
        static constexpr T OrthogonalityTolerance() noexcept { return std::numeric_limits<T>::epsilon() * T{64}; }

        static constexpr void Validate(const Point<T, N>& center, const Axes& axes,
                                       const Vector<T, N>& halfExtents) noexcept {
            SORA_PRE(IsNotNaN(center));
            SORA_PRE(IsNotNaN(halfExtents));
            for (size_t axis = 0; axis < N; ++axis) {
                SORA_PRE(halfExtents[axis] >= T{0});
                for (size_t other = axis + 1; other < N; ++other) {
                    const T dot = Dot(axes[axis].Value(), axes[other].Value());
                    SORA_PRE(-OrthogonalityTolerance() <= dot && dot <= OrthogonalityTolerance());
                }
            }
        }

        Point<T, N> center_{};
        Axes axes_;
        Vector<T, N> halfExtents_{};
    };

    /** @brief Closed ball with a non-negative radius; an infinite radius is permitted. */
    template<std::floating_point T, size_t N>
        requires(N > 0)
    class Ball {
    public:
        static constexpr size_t kDimension = N;

        constexpr Ball() noexcept = default;

        /** @brief Construct a closed ball; infinite radius is valid, NaN and negative radius are not. */
        constexpr Ball(const Point<T, N>& center, T radius) noexcept : center_(center), radius_(radius) {
            SORA_PRE(IsNotNaN(center));
            SORA_PRE(IsNotNaN(radius));
            SORA_PRE(radius >= T{0});
        }

        [[nodiscard]] constexpr const Point<T, N>& Center() const noexcept { return center_; }
        [[nodiscard]] constexpr T Radius() const noexcept { return radius_; }

    private:
        Point<T, N> center_{};
        T radius_ = T{0};
    };

    /** @brief Two-dimensional circle with a non-negative radius. */
    template<std::floating_point T, size_t N>
        requires(N == 2 || N == 3)
    class Circle;

    template<std::floating_point T>
    class Circle<T, 2> {
    public:
        static constexpr size_t kDimension = 2;

        constexpr Circle() noexcept = default;

        /** @brief Construct a two-dimensional circle; infinite radius is valid. */
        constexpr Circle(const Point<T, 2>& center, T radius) noexcept : center_(center), radius_(radius) {
            SORA_PRE(IsNotNaN(center));
            SORA_PRE(IsNotNaN(radius));
            SORA_PRE(radius >= T{0});
        }

        [[nodiscard]] constexpr const Point<T, 2>& Center() const noexcept { return center_; }
        [[nodiscard]] constexpr T Radius() const noexcept { return radius_; }

    private:
        Point<T, 2> center_{};
        T radius_ = T{0};
    };

    /** @brief Three-dimensional circle embedded in a plane with an explicit unit normal. */
    template<std::floating_point T>
    class Circle<T, 3> {
    public:
        static constexpr size_t kDimension = 3;

        /** @brief Construct a three-dimensional circle from its center, plane normal, and radius. */
        constexpr Circle(const Point<T, 3>& center, const Vector<T, 3>& normal, T radius) noexcept
            : center_(center), normal_(Normalize(normal)), radius_(radius) {
            SORA_PRE(IsNotNaN(center));
            SORA_PRE(IsNotNaN(normal));
            SORA_PRE(IsNotNaN(radius));
            SORA_PRE(radius >= T{0});
        }

        [[nodiscard]] constexpr const Point<T, 3>& Center() const noexcept { return center_; }
        [[nodiscard]] constexpr const UnitVector<T, 3>& Normal() const noexcept { return normal_; }
        [[nodiscard]] constexpr T Radius() const noexcept { return radius_; }

    private:
        [[nodiscard]] static constexpr UnitVector<T, 3> Normalize(const Vector<T, 3>& normal) noexcept {
            const auto unit = TryNormalize(normal);
            SORA_PRE(unit.has_value());
            return *unit;
        }

        Point<T, 3> center_{};
        UnitVector<T, 3> normal_;
        T radius_ = T{0};
    };

    /** @brief Ray with a non-zero direction; infinite components are permitted. */
    template<std::floating_point T, size_t N>
        requires(N > 0)
    class Ray {
    public:
        static constexpr size_t kDimension = N;

        /** @brief Construct a ray; infinite components are valid, NaN and zero direction are not. */
        constexpr Ray(const Point<T, N>& origin, const Vector<T, N>& direction) noexcept
            : origin_(origin), direction_(direction) {
            SORA_PRE(IsNotNaN(origin));
            SORA_PRE(IsNotNaN(direction));
            SORA_PRE(SquaredNorm(direction) > T{0});
        }

        [[nodiscard]] constexpr const Point<T, N>& Origin() const noexcept { return origin_; }
        [[nodiscard]] constexpr const Vector<T, N>& Direction() const noexcept { return direction_; }

    private:
        Point<T, N> origin_{};
        Vector<T, N> direction_{};
    };

    /** @brief Normalized affine hyperplane represented by `normal dot x + offset == 0`. */
    template<std::floating_point T, size_t N>
        requires(N > 0)
    class Hyperplane {
    public:
        static constexpr size_t kDimension = N;

        /** @brief Construct and normalize a plane equation from a finite non-zero normal. */
        constexpr Hyperplane(const Vector<T, N>& normal, T offset) noexcept
            : normal_(Normalize(normal)), offset_(offset / Norm(normal)) {
            SORA_PRE(IsNotNaN(normal));
            SORA_PRE(IsNotNaN(offset));
            SORA_PRE(SquaredNorm(normal) > T{0});
        }

        [[nodiscard]] static constexpr UnitVector<T, N> Normalize(const Vector<T, N>& normal) noexcept {
            auto unit = TryNormalize(normal);
            SORA_PRE(unit.has_value());
            return *unit;
        }

        [[nodiscard]] constexpr const UnitVector<T, N>& Normal() const noexcept { return normal_; }
        [[nodiscard]] constexpr T Offset() const noexcept { return offset_; }

    private:
        UnitVector<T, N> normal_{};
        T offset_ = T{0};
    };

    /** @brief Raw clipping half-space plane used by frusta and GPU clip equations. */
    template<std::floating_point T>
    struct ClipPlane {
        Vector<T, 3> normal{};
        T offset = T{0};
    };

    /** @brief Six ordered clipping planes forming a three-dimensional view frustum. */
    template<std::floating_point T>
    struct Frustum {
        static constexpr size_t kDimension = 3;
        std::array<ClipPlane<T>, 6> planes{};
    };

    /** @brief Inclusive ray parameter interval returned by a ray-box query. */
    template<std::floating_point T>
    struct RayHit {
        T near = T{0};
        T far = T{0};
    };

    // clang-format off
    using Box1f          = Box<Sora::Traits::Float<32>, 1>;
    using Box2f          = Box<Sora::Traits::Float<32>, 2>;
    using Box3f          = Box<Sora::Traits::Float<32>, 3>;
    using Box1d          = Box<Sora::Traits::Float<64>, 1>;
    using Box2d          = Box<Sora::Traits::Float<64>, 2>;
    using Box3d          = Box<Sora::Traits::Float<64>, 3>;
    using Box1i          = Box<Sora::Traits::Signed<32>, 1>;
    using Box2i          = Box<Sora::Traits::Signed<32>, 2>;
    using Box3i          = Box<Sora::Traits::Signed<32>, 3>;
    using Box1u          = Box<Sora::Traits::Unsigned<32>, 1>;
    using Box2u          = Box<Sora::Traits::Unsigned<32>, 2>;
    using Box3u          = Box<Sora::Traits::Unsigned<32>, 3>;
    using Rect1f         = Rect<Sora::Traits::Float<32>, 1>;
    using Rect2f         = Rect<Sora::Traits::Float<32>, 2>;
    using Rect3f         = Rect<Sora::Traits::Float<32>, 3>;
    using Rect1d         = Rect<Sora::Traits::Float<64>, 1>;
    using Rect2d         = Rect<Sora::Traits::Float<64>, 2>;
    using Rect3d         = Rect<Sora::Traits::Float<64>, 3>;
    using Rect1i         = Rect<Sora::Traits::Signed<32>, 1>;
    using Rect2i         = Rect<Sora::Traits::Signed<32>, 2>;
    using Rect3i         = Rect<Sora::Traits::Signed<32>, 3>;
    using Rect1u         = Rect<Sora::Traits::Unsigned<32>, 1>;
    using Rect2u         = Rect<Sora::Traits::Unsigned<32>, 2>;
    using Rect3u         = Rect<Sora::Traits::Unsigned<32>, 3>;
    using Circle2f       = Circle<Sora::Traits::Float<32>, 2>;
    using Circle3f       = Circle<Sora::Traits::Float<32>, 3>;
    using Sphere3f       = Ball<Sora::Traits::Float<32>, 3>;
    using DirectedBox2f = DirectedBox<Sora::Traits::Float<32>, 2>;
    using DirectedBox3f = DirectedBox<Sora::Traits::Float<32>, 3>;
    using Ray2f          = Ray<Sora::Traits::Float<32>, 2>;
    using Ray3f          = Ray<Sora::Traits::Float<32>, 3>;
    using Line2f         = Hyperplane<Sora::Traits::Float<32>, 2>;
    using Line3f         = Hyperplane<Sora::Traits::Float<32>, 3>;
    using Plane3f        = Hyperplane<Sora::Traits::Float<32>, 3>;
    using Frustum2f      = Frustum<Sora::Traits::Float<32>>;
    using Frustum3f      = Frustum<Sora::Traits::Float<32>>;
    // clang-format on

} // namespace Sora::Math
