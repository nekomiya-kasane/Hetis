/**
 * @file AlgebraTest.cpp
 * @brief Tests for the algebraic structure concept hierarchy (Algebra.h).
 *
 * Verifies that:
 *   - Vec2/3/4 and quat satisfy the concept hierarchy
 *   - Generic algorithms (Math::Dot/Norm/Normalize/Distance/Lerp/Reflect/Project/Reject)
 *     produce correct results when called through the concept-constrained interface
 *   - Compile-time evaluation works for all algorithms
 */
#include "Mashiro/Math/Algebra.h"
#include "Mashiro/Core/Quanterion.h"
#include "Mashiro/Core/Types.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace Mashiro;
using namespace Mashiro::Math;
using Catch::Approx;

namespace {
    constexpr float kEps = 1e-5f;
}

// ===========================================================================
// Concept satisfaction (compile-time)
// ===========================================================================

TEST_CASE("Vec types satisfy the full concept hierarchy", "[Algebra.Concepts]") {
    STATIC_REQUIRE(AdditiveGroup<vec2>);
    STATIC_REQUIRE(AdditiveGroup<vec3>);
    STATIC_REQUIRE(AdditiveGroup<vec4>);

    STATIC_REQUIRE(VectorSpace<vec2>);
    STATIC_REQUIRE(VectorSpace<vec3>);
    STATIC_REQUIRE(VectorSpace<vec4>);

    STATIC_REQUIRE(InnerProductSpace<vec2>);
    STATIC_REQUIRE(InnerProductSpace<vec3>);
    STATIC_REQUIRE(InnerProductSpace<vec4>);

    STATIC_REQUIRE(NormedSpace<vec2>);
    STATIC_REQUIRE(NormedSpace<vec3>);
    STATIC_REQUIRE(NormedSpace<vec4>);

    STATIC_REQUIRE(MetricSpace<vec2>);
    STATIC_REQUIRE(MetricSpace<vec3>);
    STATIC_REQUIRE(MetricSpace<vec4>);
}

TEST_CASE("quat satisfies InnerProductSpace / NormedSpace / MetricSpace", "[Algebra.Concepts]") {
    STATIC_REQUIRE(AdditiveGroup<quat>);
    STATIC_REQUIRE(VectorSpace<quat>);
    STATIC_REQUIRE(InnerProductSpace<quat>);
    STATIC_REQUIRE(NormedSpace<quat>);
    STATIC_REQUIRE(MetricSpace<quat>);
}

TEST_CASE("Integer vectors satisfy InnerProductSpace (Dot works on int)", "[Algebra.Concepts]") {
    STATIC_REQUIRE(AdditiveGroup<ivec2>);
    STATIC_REQUIRE(VectorSpace<ivec2>);
    STATIC_REQUIRE(InnerProductSpace<ivec2>);
    // NormedSpace is satisfied (InnerProductSpace implies it) but Norm/Normalize
    // functions themselves require floating_point and won't instantiate for int.
    STATIC_REQUIRE(NormedSpace<ivec2>);
}

// ===========================================================================
// FieldOf type extraction
// ===========================================================================

TEST_CASE("FieldOf extracts correct scalar types", "[Algebra.FieldOf]") {
    STATIC_REQUIRE(std::same_as<FieldOf<vec2>, float>);
    STATIC_REQUIRE(std::same_as<FieldOf<vec3>, float>);
    STATIC_REQUIRE(std::same_as<FieldOf<vec4>, float>);
    STATIC_REQUIRE(std::same_as<FieldOf<quat>, float>);
    STATIC_REQUIRE(std::same_as<FieldOf<ivec2>, int32_t>);
    STATIC_REQUIRE(std::same_as<FieldOf<uvec3>, uint32_t>);
}

// ===========================================================================
// Math::Dot — inner product
// ===========================================================================

TEST_CASE("Math::Dot matches Math::Dot for vec3", "[Algebra.Dot]") {
    vec3 a{1, 2, 3};
    vec3 b{4, 5, 6};
    REQUIRE(Dot(a, b) == Approx(Math::Dot(a, b)));
}

TEST_CASE("Math::Dot works on vec4", "[Algebra.Dot]") {
    vec4 a{1, 2, 3, 4};
    vec4 b{5, 6, 7, 8};
    REQUIRE(Dot(a, b) == Approx(Math::Dot(a, b)));
}

TEST_CASE("Math::Dot works on quat", "[Algebra.Dot]") {
    quat a{.x = 1, .y = 2, .z = 3, .w = 4};
    quat b{.x = 5, .y = 6, .z = 7, .w = 8};
    REQUIRE(Dot(a, b) == Approx(Quat::Dot(a, b)));
    REQUIRE(Dot(a, b) == Approx(1*5 + 2*6 + 3*7 + 4*8));
}

// ===========================================================================
// Math::NormSq / Norm
// ===========================================================================

TEST_CASE("Math::NormSq matches Math::Norm2Sq for vec3", "[Algebra.Norm]") {
    vec3 v{3, 4, 0};
    REQUIRE(NormSq(v) == Approx(Math::Norm2Sq(v)));
    REQUIRE(NormSq(v) == Approx(25.0f));
}

TEST_CASE("Math::Norm matches Math::Norm2 for vec3", "[Algebra.Norm]") {
    vec3 v{3, 4, 0};
    REQUIRE(Norm(v) == Approx(Math::Norm2(v)));
    REQUIRE(Norm(v) == Approx(5.0f));
}

TEST_CASE("Math::Norm works on quat", "[Algebra.Norm]") {
    quat q{.x = 1, .y = 0, .z = 0, .w = 0};
    REQUIRE(Norm(q) == Approx(1.0f));

    quat q2{.x = 1, .y = 2, .z = 3, .w = 4};
    REQUIRE(Norm(q2) == Approx(Quat::Norm2(q2)));
}

// ===========================================================================
// Math::Normalize
// ===========================================================================

TEST_CASE("Math::Normalize matches Math::Normalize for vec3", "[Algebra.Normalize]") {
    vec3 v{3, 4, 0};
    vec3 n = Normalize(v);
    REQUIRE(n.x == Approx(0.6f));
    REQUIRE(n.y == Approx(0.8f));
    REQUIRE(n.z == Approx(0.0f));
    REQUIRE(Norm(n) == Approx(1.0f).margin(kEps));
}

TEST_CASE("Math::Normalize works on quat", "[Algebra.Normalize]") {
    quat q{.x = 1, .y = 2, .z = 3, .w = 4};
    quat n = Normalize(q);
    REQUIRE(Norm(n) == Approx(1.0f).margin(kEps));

    // Verify against Quat::Normalize
    quat expected = Quat::Normalize(q);
    REQUIRE(n.x == Approx(expected.x).margin(kEps));
    REQUIRE(n.y == Approx(expected.y).margin(kEps));
    REQUIRE(n.z == Approx(expected.z).margin(kEps));
    REQUIRE(n.w == Approx(expected.w).margin(kEps));
}

// ===========================================================================
// Math::Distance / DistanceSq
// ===========================================================================

TEST_CASE("Math::Distance matches Math::Distance for vec3", "[Algebra.Distance]") {
    vec3 a{1, 2, 3};
    vec3 b{4, 6, 3};
    REQUIRE(Distance(a, b) == Approx(Math::Distance(a, b)));
    REQUIRE(Distance(a, b) == Approx(5.0f));
    REQUIRE(DistanceSq(a, b) == Approx(25.0f));
}

TEST_CASE("Math::Distance works on quat", "[Algebra.Distance]") {
    quat a{.x = 0, .y = 0, .z = 0, .w = 1};
    quat b{.x = 1, .y = 0, .z = 0, .w = 0};
    // |a - b| = |(−1, 0, 0, 1)| = √2
    REQUIRE(Distance(a, b) == Approx(Math::Sqrt(2.0f)).margin(kEps));
}

// ===========================================================================
// Math::Lerp
// ===========================================================================

TEST_CASE("Math::Lerp matches Math::Lerp for vec3", "[Algebra.Lerp]") {
    vec3 a{0, 0, 0};
    vec3 b{10, 20, 30};
    vec3 mid = Lerp(a, b, 0.5f);
    REQUIRE(mid.x == Approx(5.0f));
    REQUIRE(mid.y == Approx(10.0f));
    REQUIRE(mid.z == Approx(15.0f));
}

TEST_CASE("Math::Lerp works on quat (linear, not slerp)", "[Algebra.Lerp]") {
    quat a{.x = 0, .y = 0, .z = 0, .w = 1};
    quat b{.x = 1, .y = 0, .z = 0, .w = 0};
    quat mid = Lerp(a, b, 0.5f);
    REQUIRE(mid.x == Approx(0.5f));
    REQUIRE(mid.w == Approx(0.5f));
}

// ===========================================================================
// Math::Reflect
// ===========================================================================

TEST_CASE("Math::Reflect matches Math::Reflect for vec3", "[Algebra.Reflect]") {
    vec3 v{1, -1, 0};
    vec3 n{0, 1, 0};
    vec3 r = Reflect(v, n);
    REQUIRE(r.x == Approx(1.0f));
    REQUIRE(r.y == Approx(1.0f));
    REQUIRE(r.z == Approx(0.0f));
}

// ===========================================================================
// Math::Project / Reject
// ===========================================================================

TEST_CASE("Project gives the component along n", "[Algebra.Project]") {
    vec3 v{3, 4, 0};
    vec3 n{1, 0, 0};
    vec3 p = Project(v, n);
    REQUIRE(p.x == Approx(3.0f));
    REQUIRE(p.y == Approx(0.0f));
    REQUIRE(p.z == Approx(0.0f));
}

TEST_CASE("Reject gives the component orthogonal to n", "[Algebra.Project]") {
    vec3 v{3, 4, 0};
    vec3 n{1, 0, 0};
    vec3 r = Reject(v, n);
    REQUIRE(r.x == Approx(0.0f));
    REQUIRE(r.y == Approx(4.0f));
    REQUIRE(r.z == Approx(0.0f));
}

TEST_CASE("Project + Reject reconstructs the original vector", "[Algebra.Project]") {
    vec3 v{2.5f, -1.3f, 0.7f};
    vec3 n{1, 1, 0}; // not unit, should still work
    vec3 sum = Project(v, n) + Reject(v, n);
    REQUIRE(sum.x == Approx(v.x).margin(kEps));
    REQUIRE(sum.y == Approx(v.y).margin(kEps));
    REQUIRE(sum.z == Approx(v.z).margin(kEps));
}

// ===========================================================================
// Constexpr verification
// ===========================================================================

TEST_CASE("Algebra algorithms fold at compile time", "[Algebra.Constexpr]") {
    constexpr vec3 a{3, 4, 0};
    constexpr vec3 b{0, 0, 5};

    constexpr float dot = Dot(a, b);
    STATIC_REQUIRE(dot == 0.0f);

    constexpr float nsq = NormSq(a);
    STATIC_REQUIRE(nsq == 25.0f);

    constexpr vec3 lerped = Lerp(a, b, 0.0f);
    STATIC_REQUIRE(lerped.x == 3.0f);
    STATIC_REQUIRE(lerped.y == 4.0f);
}

TEST_CASE("Math::Dot on quat folds at compile time", "[Algebra.Constexpr]") {
    constexpr quat a{.x = 1, .y = 0, .z = 0, .w = 0};
    constexpr quat b{.x = 0, .y = 1, .z = 0, .w = 0};
    constexpr float d = Dot(a, b);
    STATIC_REQUIRE(d == 0.0f);

    constexpr float self = Dot(a, a);
    STATIC_REQUIRE(self == 1.0f);
}
