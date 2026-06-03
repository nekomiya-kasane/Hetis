/**
 * @file AlgebraApproxTest.cpp
 * @brief Tests for Algebra.h ApproxEq/Ne/Lt/Le/Gt/Ge operators.
 */
#include "Mashiro/Math/Algebra.h"
#include "Mashiro/Math/Quanterion.h"
#include "Mashiro/Core/Types.h"

#include <catch2/catch_test_macros.hpp>

using namespace Mashiro;
using namespace Mashiro::Math;

// ===========================================================================
// ApproxEq — floating-point scalars
// ===========================================================================

TEST_CASE("ApproxEq: float identical values", "[Core.Algebra]") {
    STATIC_REQUIRE(ApproxEq(1.0f, 1.0f));
    STATIC_REQUIRE(ApproxEq(0.0f, 0.0f));
    STATIC_REQUIRE(ApproxEq(-5.0f, -5.0f));
}

TEST_CASE("ApproxEq: float within epsilon", "[Core.Algebra]") {
    STATIC_REQUIRE(ApproxEq(1.0f, 1.0f + 1e-8f));
    STATIC_REQUIRE(!ApproxEq(1.0f, 2.0f));
}

TEST_CASE("ApproxEq: float custom epsilon", "[Core.Algebra]") {
    STATIC_REQUIRE(ApproxEq(1.0f, 1.05f, 0.1f));
    STATIC_REQUIRE(!ApproxEq(1.0f, 1.2f, 0.1f));
}

TEST_CASE("ApproxEq: double precision", "[Core.Algebra]") {
    STATIC_REQUIRE(ApproxEq(1.0, 1.0 + 1e-16));
    STATIC_REQUIRE(!ApproxEq(1.0, 1.1));
}

// ===========================================================================
// ApproxEq — MetricSpace (vec3, quat)
// ===========================================================================

TEST_CASE("ApproxEq: vec3 identical", "[Core.Algebra]") {
    constexpr vec3 a{1,2,3};
    STATIC_REQUIRE(ApproxEq(a, a));
}

TEST_CASE("ApproxEq: vec3 within epsilon", "[Core.Algebra]") {
    constexpr vec3 a{1,0,0}, b{1,1e-8f,0};
    STATIC_REQUIRE(ApproxEq(a, b));
}

TEST_CASE("ApproxEq: vec3 far apart", "[Core.Algebra]") {
    constexpr vec3 a{1,0,0}, b{0,1,0};
    STATIC_REQUIRE(!ApproxEq(a, b));
}

TEST_CASE("ApproxEq: quat within epsilon", "[Core.Algebra]") {
    constexpr quat a{}, b{.x=1e-8f, .y=0, .z=0, .w=1};
    STATIC_REQUIRE(ApproxEq(a, b, 1e-4f));
}

// ===========================================================================
// ApproxNe
// ===========================================================================

TEST_CASE("ApproxNe is negation of ApproxEq", "[Core.Algebra]") {
    STATIC_REQUIRE(!ApproxNe(1.0f, 1.0f));
    STATIC_REQUIRE(ApproxNe(1.0f, 2.0f));
    constexpr vec3 a{1,0,0}, b{0,1,0};
    STATIC_REQUIRE(ApproxNe(a, b));
}

// ===========================================================================
// Ordered comparisons — floating-point
// ===========================================================================

TEST_CASE("ApproxLt: strictly less with tolerance", "[Core.Algebra]") {
    STATIC_REQUIRE(!ApproxLt(1.0f, 1.0f));      // equal -> not less
    STATIC_REQUIRE(!ApproxLt(1.0f, 1.00001f));   // within eps -> not less
    STATIC_REQUIRE(ApproxLt(1.0f, 1.1f));         // clearly less
    STATIC_REQUIRE(!ApproxLt(1.1f, 1.0f));        // greater
}

TEST_CASE("ApproxLe: less-or-equal with tolerance", "[Core.Algebra]") {
    STATIC_REQUIRE(ApproxLe(1.0f, 1.0f));         // equal -> le
    STATIC_REQUIRE(ApproxLe(1.0f, 1.00001f));     // within eps -> le
    STATIC_REQUIRE(ApproxLe(0.5f, 1.0f));          // clearly less
    STATIC_REQUIRE(!ApproxLe(2.0f, 1.0f));         // greater
}

TEST_CASE("ApproxGt: strictly greater with tolerance", "[Core.Algebra]") {
    STATIC_REQUIRE(!ApproxGt(1.0f, 1.0f));
    STATIC_REQUIRE(!ApproxGt(1.00001f, 1.0f));
    STATIC_REQUIRE(ApproxGt(1.1f, 1.0f));
    STATIC_REQUIRE(!ApproxGt(0.5f, 1.0f));
}

TEST_CASE("ApproxGe: greater-or-equal with tolerance", "[Core.Algebra]") {
    STATIC_REQUIRE(ApproxGe(1.0f, 1.0f));
    STATIC_REQUIRE(ApproxGe(1.00001f, 1.0f));
    STATIC_REQUIRE(ApproxGe(2.0f, 1.0f));
    STATIC_REQUIRE(!ApproxGe(0.5f, 1.0f));
}

// ===========================================================================
// Constexpr fold verification
// ===========================================================================

TEST_CASE("All Approx operators fold at compile time", "[Core.Algebra]") {
    constexpr vec3 a{1,2,3}, b{1,2,3};
    STATIC_REQUIRE(ApproxEq(a, b));
    STATIC_REQUIRE(!ApproxNe(a, b));
    STATIC_REQUIRE(ApproxEq(3.14f, 3.14f));
    STATIC_REQUIRE(ApproxLe(1.0f, 1.0f));
    STATIC_REQUIRE(ApproxGe(1.0f, 1.0f));
}
