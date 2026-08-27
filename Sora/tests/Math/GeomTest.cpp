/**
 * @file GeomTest.cpp
 * @brief Contract tests for Sora mathematical geometry.
 */
#include <Sora/Core/Contracts.h>
#include <Sora/Math/Geom.h>
#include <Sora/Math/Types.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace {

    using Sora::Math::Affine;
    using Sora::Math::Point;
    using Sora::Math::Vector;
    using Point3 = Point<float, 3>;
    using Vector3 = Vector<float, 3>;

    constexpr Sora::Math::Box3f kUnitBox{Point3{-1.0F, -1.0F, -1.0F}, Point3{1.0F, 1.0F, 1.0F}};
    bool gContractHandlerCalled = false;
    void RecordContractViolation(const Sora::Contracts::Violation&) noexcept {
        gContractHandlerCalled = true;
    }

} // namespace

TEST_CASE("Geometry constructors preserve their contracts", "[Sora][Math][Geom]") {
    constexpr Sora::Math::Box1f interval{-1.0F, 1.0F};
    constexpr Sora::Math::Box3f box{Point3{-1.0F, -1.0F, -1.0F}, Point3{1.0F, 1.0F, 1.0F}};
    constexpr Sora::Math::Sphere3f sphere{Point3{}, 1.0F};
    constexpr Sora::Math::Ray3f ray{Point3{}, Vector3{1.0F, 0.0F, 0.0F}};
    constexpr Sora::Math::Plane3f plane{Vector3{1.0F, 0.0F, 0.0F}, 0.0F};
    STATIC_REQUIRE(interval.Lower() == -1.0F && interval.Upper() == 1.0F);
    STATIC_REQUIRE(box.Min()[0] == -1.0F && sphere.Radius() == 1.0F);
    STATIC_REQUIRE(ray.Direction()[0] == 1.0F && plane.Normal()[0] == 1.0F);
}

TEST_CASE("Infinite coordinates are representable", "[Sora][Math][Geom]") {
    const float infinity = std::numeric_limits<float>::infinity();
    const Sora::Math::Box1f interval{-infinity, infinity};
    const Sora::Math::Sphere3f sphere{Point3{infinity, 0.0F, 0.0F}, infinity};
    CHECK(std::isinf(interval.Lower()));
    CHECK(std::isinf(sphere.Center()[0]));
    CHECK(std::isinf(sphere.Radius()));
}

TEST_CASE("One-dimensional boxes provide interval operations", "[Sora][Math][Geom]") {
    constexpr Sora::Math::Box1f interval{-2.0F, 4.0F};
    STATIC_REQUIRE(interval.Lower() == -2.0F && interval.Upper() == 4.0F);
    STATIC_REQUIRE(Sora::Math::Length(interval) == 6.0F);
    STATIC_REQUIRE(Sora::Math::Midpoint(interval) == 1.0F);
    STATIC_REQUIRE(Sora::Math::Contains(interval, 0.0F));
    STATIC_REQUIRE(Sora::Math::ClampTo(8.0F, interval) == 4.0F);
    constexpr Sora::Math::Box1f expanded = Sora::Math::Expand(interval, 1.0F);
    STATIC_REQUIRE(expanded.Lower() == -3.0F && expanded.Upper() == 5.0F);
}

TEST_CASE("Integral boxes and base-extent rectangles preserve ordered geometry", "[Sora][Math][Geom]") {
    constexpr Sora::Math::Box1u interval{2U, 9U};
    constexpr Sora::Math::Box1i signedInterval{-10, 12};
    constexpr Sora::Math::Rect2u rectangle{Sora::Math::Point2u{2U, 3U}, Sora::Math::Vec2u{7U, 8U}};
    STATIC_REQUIRE(Sora::Math::Length(interval) == 7U);
    STATIC_REQUIRE(Sora::Math::Length(signedInterval) == 22U);
    STATIC_REQUIRE(Sora::Math::Midpoint(interval) == 5U);
    STATIC_REQUIRE(Sora::Math::Contains(interval, 7U));
    STATIC_REQUIRE(Sora::Math::Expand(interval, 2U).Lower() == 0U);
    STATIC_REQUIRE(rectangle.Base() == Sora::Math::Point2u{2U, 3U});
    STATIC_REQUIRE(
        Sora::Math::Rect1u{Sora::Math::Point<unsigned, 1>{2U}, Sora::Math::Vector<unsigned, 1>{7U}}.Width() == 7U);
    STATIC_REQUIRE(rectangle.Width() == 7U && rectangle.Height() == 8U);
    STATIC_REQUIRE(rectangle.Max() == Sora::Math::Point2u{9U, 11U});
    STATIC_REQUIRE(rectangle.ToBox().Max() == rectangle.Max());
}

TEST_CASE("Box queries preserve affine point/vector semantics", "[Sora][Math][Geom]") {
    STATIC_REQUIRE(Sora::Math::Contains(kUnitBox, Point3{}));
    STATIC_REQUIRE(Sora::Math::SquaredDistance(Point3{3.0F, 0.0F, 0.0F}, kUnitBox) == 4.0F);
    STATIC_REQUIRE(Sora::Math::Centroid(kUnitBox) == Point3{});
    STATIC_REQUIRE(Sora::Math::Extents(kUnitBox) == Vector3{1.0F, 1.0F, 1.0F});
    constexpr Sora::Math::Box3f dominantBox{Point3{}, Point3{1.0F, 3.0F, 2.0F}};
    STATIC_REQUIRE(Sora::Math::LargestAxis(dominantBox) == 1);
}

TEST_CASE("Discrete boxes support ordered queries and checked affine transforms", "[Sora][Math][Geom]") {
    constexpr Sora::Math::Box2i box{Sora::Math::Point2i{-2, -3}, Sora::Math::Point2i{4, 8}};
    constexpr auto translation = Sora::Math::Translate(Sora::Math::Vec2i{10, 20});
    constexpr auto transformed = Sora::Math::Transform(translation, box);
    constexpr Sora::Math::Rect2i rectangle{Sora::Math::Point2i{-2, -3}, Sora::Math::Vec2i{6, 11}};
    constexpr auto transformedRectangle = Sora::Math::Transform(translation, rectangle);
    STATIC_REQUIRE(Sora::Math::Size(box) == Sora::Math::Vec2i{6, 11});
    STATIC_REQUIRE(Sora::Math::Centroid(box) == Sora::Math::Point2i{1, 2});
    STATIC_REQUIRE(Sora::Math::Extents(box)[1] == 5.5);
    STATIC_REQUIRE(Sora::Math::ClosestPoint(box, Sora::Math::Point2i{8, 0}) == Sora::Math::Point2i{4, 0});
    STATIC_REQUIRE(Sora::Math::LargestAxis(box) == 1);
    STATIC_REQUIRE(transformed.Min() == Sora::Math::Point2i{8, 17});
    STATIC_REQUIRE(transformedRectangle.Max() == Sora::Math::Point2i{14, 28});
}

TEST_CASE("Affine boxes use exact quadratic interval propagation", "[Sora][Math][Geom]") {
    constexpr Sora::Math::Box2i box{Sora::Math::Point2i{-2, -3}, Sora::Math::Point2i{4, 8}};
    constexpr Sora::Math::Rect2i rect{Sora::Math::Point2i{-2, -3}, Sora::Math::Vec2i{6, 11}};
    constexpr Affine<int, 2> affine{Sora::Math::Matrix<int, 2, 2>{0, -1, 2, 0}, Sora::Math::Vec2i{10, -3}};
    constexpr auto transformedBox = Sora::Math::Transform(affine, box);
    constexpr auto transformedRect = Sora::Math::Transform(affine, rect);
    STATIC_REQUIRE(transformedBox.Min() == Sora::Math::Point2i{2, -7});
    STATIC_REQUIRE(transformedBox.Max() == Sora::Math::Point2i{13, 5});
    STATIC_REQUIRE(transformedRect.Min() == transformedBox.Min());
    STATIC_REQUIRE(transformedRect.Max() == transformedBox.Max());
}

TEST_CASE("Affine box propagation skips zero times unbounded coordinates", "[Sora][Math][Geom]") {
    const float infinity = std::numeric_limits<float>::infinity();
    const Sora::Math::Box3f unbounded{Point3{-infinity, -2.0F, -3.0F}, Point3{infinity, 4.0F, 5.0F}};
    const auto transformed = Sora::Math::Transform(Affine<float, 3>::Identity(), unbounded);

    CHECK(transformed.Min()[0] == -infinity);
    CHECK(transformed.Max()[0] == infinity);
    CHECK(transformed.Min()[1] == -2.0F);
    CHECK(transformed.Max()[1] == 4.0F);
    CHECK(transformed.Min()[2] == -3.0F);
    CHECK(transformed.Max()[2] == 5.0F);
    CHECK(Sora::Math::IsNotNaN(transformed.Min()));
    CHECK(Sora::Math::IsNotNaN(transformed.Max()));
}

TEST_CASE("Disjoint intersections are represented as empty optionals", "[Sora][Math][Geom]") {
    const Sora::Math::Box3f left{Point3{0.0F, 0.0F, 0.0F}, Point3{1.0F, 1.0F, 1.0F}};
    const Sora::Math::Box3f right{Point3{2.0F, 0.0F, 0.0F}, Point3{3.0F, 1.0F, 1.0F}};
    REQUIRE_FALSE(Sora::Math::Intersection(left, right).has_value());
}

TEST_CASE("Ray-box intersection exposes a non-negative parameter interval", "[Sora][Math][Geom]") {
    const Sora::Math::Ray3f ray{Point3{0.0F, 0.0F, -5.0F}, Vector3{0.0F, 0.0F, 1.0F}};
    const auto hit = Sora::Math::Intersect(ray, kUnitBox);
    REQUIRE(hit.has_value());
    CHECK(hit->near == Catch::Approx(4.0F));
    CHECK(hit->far == Catch::Approx(6.0F));

    const Sora::Math::Ray3f parallel{Point3{2.0F, 0.0F, -5.0F}, Vector3{0.0F, 0.0F, 1.0F}};
    REQUIRE_FALSE(Sora::Math::Intersect(parallel, kUnitBox).has_value());
}

TEST_CASE("Affine maps preserve point/vector and plane transformation laws", "[Sora][Math][Geom]") {
    constexpr Sora::Math::Plane3f plane{Vector3{1.0F, 0.0F, 0.0F}, 0.0F};
    constexpr auto translation = Sora::Math::Translate(Vector3{2.0F, 0.0F, 0.0F});
    const auto transformed = Sora::Math::TryTransform(translation, plane);
    REQUIRE(transformed.has_value());
    CHECK(Sora::Math::SignedDistance(*transformed, Point3{2.0F, 0.0F, 0.0F}) == Catch::Approx(0.0F));
    CHECK(Sora::Math::TransformPoint(translation, Point3{}) == Point3{2.0F, 0.0F, 0.0F});
    CHECK(Sora::Math::TransformVector(translation, Vector3{1.0F, 0.0F, 0.0F}) == Vector3{1.0F, 0.0F, 0.0F});
}

TEST_CASE("Fallible affine geometry transforms preserve primitive invariants", "[Sora][Math][Geom]") {
    const Affine<float, 3> affine{Sora::Math::Matrix<float, 3, 3>{2.0F, 0.0F, 0.0F, 0.0F, 3.0F, 0.0F, 0.0F, 0.0F, 0.5F},
                                  Vector3{5.0F, -2.0F, 1.0F}};
    const Sora::Math::Ray3f ray{Point3{1.0F, 2.0F, 3.0F}, Vector3{1.0F, 1.0F, 0.0F}};
    const auto transformedRay = Sora::Math::TryTransform(affine, ray);
    REQUIRE(transformedRay.has_value());
    CHECK(transformedRay->Origin() == Point3{7.0F, 4.0F, 2.5F});
    CHECK(transformedRay->Direction() == Vector3{2.0F, 3.0F, 0.0F});

    const Sora::Math::Sphere3f sphere{Point3{}, 2.0F};
    const auto transformedSphere = Sora::Math::Transform(affine, sphere);
    CHECK(transformedSphere.Center() == Point3{5.0F, -2.0F, 1.0F});
    CHECK(transformedSphere.Radius() == Catch::Approx(6.0F));

    const auto zeroScale = Sora::Math::UniformScale<3>(0.0F);
    const Sora::Math::Sphere3f unboundedSphere{Point3{}, std::numeric_limits<float>::infinity()};
    CHECK(Sora::Math::Transform(zeroScale, unboundedSphere).Radius() == 0.0F);

    const Affine<float, 3> singular{
        Sora::Math::Matrix<float, 3, 3>{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}, Vector3{}};
    const Sora::Math::Ray3f collapsed{Point3{}, Vector3{0.0F, 1.0F, 0.0F}};
    const auto collapsedResult = Sora::Math::TryTransform(singular, collapsed);
    REQUIRE_FALSE(collapsedResult.has_value());
    CHECK(collapsedResult.error() == Sora::Math::RayTransformError::CollapsedDirection);

    const float infinity = std::numeric_limits<float>::infinity();
    const Affine<float, 3> indeterminate{
        Sora::Math::Matrix<float, 3, 3>{infinity, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F}, Vector3{}};
    const Sora::Math::Ray3f indeterminateOrigin{Point3{}, Vector3{1.0F, 0.0F, 0.0F}};
    const auto originResult = Sora::Math::TryTransform(indeterminate, indeterminateOrigin);
    REQUIRE_FALSE(originResult.has_value());
    CHECK(originResult.error() == Sora::Math::RayTransformError::IndeterminateOrigin);

    const Sora::Math::Ray3f indeterminateDirection{Point3{1.0F, 0.0F, 0.0F}, Vector3{0.0F, 1.0F, 0.0F}};
    const auto directionResult = Sora::Math::TryTransform(indeterminate, indeterminateDirection);
    REQUIRE_FALSE(directionResult.has_value());
    CHECK(directionResult.error() == Sora::Math::RayTransformError::IndeterminateDirection);

    const auto singularPlane = Sora::Math::TryTransform(singular, Sora::Math::Plane3f{Vector3{1, 0, 0}, 0});
    REQUIRE_FALSE(singularPlane.has_value());
    CHECK(singularPlane.error() == Sora::ErrorCode::SingularMatrix);

    const auto singularFrustum = Sora::Math::TryTransform(singular, Sora::Math::Frustum<float>{});
    REQUIRE_FALSE(singularFrustum.has_value());
    CHECK(singularFrustum.error() == Sora::ErrorCode::SingularMatrix);
}

TEST_CASE("Hyperplanes and frusta follow the affine covector law", "[Sora][Math][Geom]") {
    const Affine<float, 3> affine{Sora::Math::Matrix<float, 3, 3>{1.0F, 2.0F, 0.0F, 0.0F, 1.0F, 3.0F, 0.0F, 0.0F, 1.0F},
                                  Vector3{5.0F, -2.0F, 1.0F}};
    const Sora::Math::Plane3f plane{Vector3{1.0F, 2.0F, -1.0F}, -3.0F};
    const auto transformedPlane = Sora::Math::TryTransform(affine, plane);
    REQUIRE(transformedPlane.has_value());
    for (const Point3 point : {Point3{3.0F, 0.0F, 0.0F}, Point3{1.0F, 1.0F, 0.0F}, Point3{0.0F, 2.0F, 1.0F}}) {
        const Point3 mapped = Sora::Math::TransformPoint(affine, point);
        CHECK(Sora::Math::SignedDistance(*transformedPlane, mapped) == Catch::Approx(0.0F).margin(1.0e-5F));
    }

    Sora::Math::Frustum<float> frustum{};
    frustum.planes = {{{Vector3{1.0F, 0.0F, 0.0F}, -1.0F},
                       {Vector3{-1.0F, 0.0F, 0.0F}, -2.0F},
                       {Vector3{0.0F, 1.0F, 0.0F}, -3.0F},
                       {Vector3{0.0F, -1.0F, 0.0F}, -4.0F},
                       {Vector3{0.0F, 0.0F, 1.0F}, -5.0F},
                       {Vector3{1.0F, 2.0F, 3.0F}, -6.0F}}};
    const auto transformedFrustum = Sora::Math::TryTransform(affine, frustum);
    REQUIRE(transformedFrustum.has_value());
    const Point3 point{0.25F, -0.5F, 1.5F};
    const Point3 mapped = Sora::Math::TransformPoint(affine, point);
    for (size_t index = 0; index < frustum.planes.size(); ++index) {
        const float before =
            Sora::Math::Dot(frustum.planes[index].normal, Sora::Math::ToVector(point)) + frustum.planes[index].offset;
        const float after = Sora::Math::Dot(transformedFrustum->planes[index].normal, Sora::Math::ToVector(mapped)) +
                            transformedFrustum->planes[index].offset;
        CHECK(after == Catch::Approx(before).margin(1.0e-5F));
    }
}

TEST_CASE("Ball transforms preserve exact rotations and conservatively contain sheared boundaries",
          "[Sora][Math][Geom]") {
    const Sora::Math::Sphere3f sphere{Point3{1.0F, -2.0F, 0.5F}, 2.0F};
    const Affine<float, 3> rotation{
        Sora::Math::Matrix<float, 3, 3>{0.0F, -1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F},
        Vector3{3.0F, 4.0F, -1.0F}};
    const auto rotated = Sora::Math::Transform(rotation, sphere);
    CHECK(rotated.Radius() == sphere.Radius());

    const Affine<float, 3> shear{Sora::Math::Matrix<float, 3, 3>{1.0F, 2.0F, 0.0F, 0.0F, 1.0F, 3.0F, 0.0F, 0.0F, 1.0F},
                                 Vector3{5.0F, -2.0F, 1.0F}};
    const auto sheared = Sora::Math::Transform(shear, sphere);
    const auto diagonal = Sora::Math::TryNormalize(Vector3{1.0F, 2.0F, 3.0F});
    REQUIRE(diagonal.has_value());
    const std::array directions{Vector3{1.0F, 0.0F, 0.0F},  Vector3{-1.0F, 0.0F, 0.0F}, Vector3{0.0F, 1.0F, 0.0F},
                                Vector3{0.0F, -1.0F, 0.0F}, Vector3{0.0F, 0.0F, 1.0F},  Vector3{0.0F, 0.0F, -1.0F},
                                diagonal->Value(),          -diagonal->Value()};
    const float squaredRadius = sheared.Radius() * sheared.Radius();
    for (const Vector3& direction : directions) {
        const Point3 boundary = sphere.Center() + sphere.Radius() * direction;
        const Point3 mapped = Sora::Math::TransformPoint(shear, boundary);
        CHECK(Sora::Math::SquaredNorm(mapped - sheared.Center()) <= squaredRadius * (1.0F + 1.0e-6F));
    }

    const float largest = (std::numeric_limits<float>::max)();
    const Affine<float, 3> hugeLinear{
        Sora::Math::Matrix<float, 3, 3>{largest, 0.0F, 0.0F, 0.0F, largest, 0.0F, 0.0F, 0.0F, largest}, Vector3{}};
    const Sora::Math::Sphere3f pointSphere{Point3{}, 0.0F};
    const auto transformedPointSphere = Sora::Math::Transform(hugeLinear, pointSphere);
    CHECK(transformedPointSphere.Radius() == 0.0F);
    CHECK_FALSE(std::isnan(transformedPointSphere.Radius()));
}

TEST_CASE("Padded vector storage preserves explicit alignment", "[Sora][Math][Geom]") {
    constexpr Sora::Math::Sphere3f sphere{Point3{1.0F, 2.0F, 3.0F}, 2.0F};
    constexpr Sora::Math::Vec4Padded packed{
        Sora::Math::Vec4f{sphere.Center()[0], sphere.Center()[1], sphere.Center()[2], sphere.Radius()}};
    STATIC_REQUIRE(sizeof(packed) == 16);
    STATIC_REQUIRE(packed[3] == 2.0F);
    CHECK(packed[0] == sphere.Center()[0]);
    CHECK(packed[1] == sphere.Center()[1]);
}

TEST_CASE("Scalar and affine coordinate utilities are reusable", "[Sora][Math][Geom]") {
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    STATIC_REQUIRE(sizeof(Sora::Math::WideInteger<int>) > sizeof(int));
    STATIC_REQUIRE(Sora::Math::CheckedCast<int>(Sora::Math::WideInteger<int>{42}) == 42);
    STATIC_REQUIRE(Sora::Math::AbsValue(-2.0F) == 2.0F);
    CHECK_FALSE(std::signbit(Sora::Math::AbsValue(-0.0F)));
    CHECK(Sora::Math::IsNaN(nan));
    CHECK(Sora::Math::IsNotNaN(1.0F));
    CHECK(Sora::Math::IsFinite(1.0F));
    CHECK(Sora::Math::IsInfinite(infinity));
    STATIC_REQUIRE(Sora::Math::Box3f::kDimension == 3);

    Sora::Math::Point3f point{1.0F, 2.0F, 3.0F};
    Vector3 vector{4.0F, 5.0F, 6.0F};
    const auto vectorView = Sora::Math::AsVectorRef(point);
    const auto pointView = Sora::Math::AsPointRef(vector);
    vectorView[0] = 7.0F;
    pointView[0] = 8.0F;
    CHECK(vectorView[1] == 2.0F);
    CHECK(pointView[2] == 6.0F);
    CHECK(point[0] == 7.0F);
    CHECK(vector[0] == 8.0F);
    CHECK(Sora::Math::ToVector(point)[0] == 7.0F);
    CHECK(Sora::Math::ToPoint(vector)[0] == 8.0F);
    CHECK(Sora::Math::IsNotNaN(point));
    CHECK(Sora::Math::IsNotNaN(vector));
}

TEST_CASE("Observe reports a violated contract without terminating", "[Sora][Core][Contracts]") {
    gContractHandlerCalled = false;
    const auto previousHandler = Sora::Contracts::SetViolationHandler(RecordContractViolation);
    const bool result = SORA_OBSERVE(false);
    static_cast<void>(Sora::Contracts::SetViolationHandler(previousHandler));
    CHECK_FALSE(result);
    CHECK(gContractHandlerCalled);
}

TEST_CASE("ASSERT is a Boolean expression without nodiscard enforcement", "[Sora][Core][Contracts]") {
    STATIC_REQUIRE(std::is_same_v<decltype(ASSERT(true)), bool>);
    const bool result = ASSERT(true);
    CHECK(result);
    ASSERT(true);
}
