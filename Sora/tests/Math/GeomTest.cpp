/**
 * @file GeomTest.cpp
 * @brief Contract tests for Sora mathematical geometry.
 */
#include <Sora/Core/Contracts.h>
#include <Sora/Math/Geom.h>
#include <Sora/Math/Types.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    constexpr auto translation = Sora::Math::Affine<int, 2>::Translation(Sora::Math::Vec2i{10, 20});
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
    constexpr auto translation = Affine<float, 3>::Translation(Vector3{2.0F, 0.0F, 0.0F});
    const Sora::Math::Plane3f transformed = Sora::Math::Transform(translation, plane);
    CHECK(Sora::Math::SignedDistance(transformed, Point3{2.0F, 0.0F, 0.0F}) == Catch::Approx(0.0F));
    CHECK(Sora::Math::TransformPoint(translation, Point3{}) == Point3{2.0F, 0.0F, 0.0F});
    CHECK(Sora::Math::TransformVector(translation, Vector3{1.0F, 0.0F, 0.0F}) == Vector3{1.0F, 0.0F, 0.0F});
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
