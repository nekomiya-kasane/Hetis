/**
 * @file ScalarMathTest.cpp
 * @brief Tests for ScalarMath.h: constexpr polynomial kernels vs runtime std::.
 */
#include "Mashiro/Math/ScalarMath.h"

#include <catch2/catch_approx.hpp>
#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace Mashiro;
using namespace Mashiro::Math;
using Catch::Approx;

namespace {
    constexpr float kPi = std::numbers::pi_v<float>;
    constexpr float kTrigEps = 2e-6f; // polynomial kernel tolerance

    constexpr bool CtClose(float a, float b, float eps = kTrigEps) {
        return (a - b < eps) && (b - a < eps);
    }

    constexpr double kPiD = std::numbers::pi_v<double>;
    constexpr double kTrigEpsD = 2e-14; // double polynomial kernel tolerance

    constexpr bool CtCloseD(double a, double b, double eps = kTrigEpsD) {
        return (a - b < eps) && (b - a < eps);
    }
}

// ===========================================================================
// Abs / CopySign (constexpr builtins)
// ===========================================================================

TEST_CASE("Abs works for float, double, int — constexpr", AUTO_TAG) {
    STATIC_REQUIRE(Abs(-3.0f) == 3.0f);
    STATIC_REQUIRE(Abs(3.0f) == 3.0f);
    STATIC_REQUIRE(Abs(0.0f) == 0.0f);
    STATIC_REQUIRE(Abs(-5.0) == 5.0);
    STATIC_REQUIRE(Abs(-7) == 7);
    STATIC_REQUIRE(Abs(0) == 0);
}

TEST_CASE("CopySign transfers sign — constexpr", AUTO_TAG) {
    STATIC_REQUIRE(CopySign(3.0f, -1.0f) == -3.0f);
    STATIC_REQUIRE(CopySign(3.0f, 1.0f) == 3.0f);
    STATIC_REQUIRE(CopySign(-3.0, 1.0) == 3.0);
}

// ===========================================================================
// Sqrt: constexpr kernel vs runtime std::sqrt
// ===========================================================================

TEST_CASE("Sqrt constexpr matches runtime", AUTO_TAG) {
    STATIC_REQUIRE(Sqrt(0.0f) == 0.0f);
    STATIC_REQUIRE(Sqrt(1.0f) == 1.0f);
    STATIC_REQUIRE(CtClose(Sqrt(4.0f), 2.0f));
    STATIC_REQUIRE(CtClose(Sqrt(2.0f), 1.41421356f, 1e-5f));

    REQUIRE(Sqrt(9.0f) == Approx(std::sqrt(9.0f)).margin(1e-6f));
    REQUIRE(Sqrt(0.01f) == Approx(std::sqrt(0.01f)).margin(1e-6f));
}

// ===========================================================================
// Trig: constexpr kernel vs runtime std::
// ===========================================================================

TEST_CASE("Sin constexpr matches runtime", AUTO_TAG) {
    constexpr float angles[] = {0.0f, kPi/6, kPi/4, kPi/3, kPi/2, kPi, -kPi/4, 3*kPi, 10.0f};
    for (float a : angles) {
        REQUIRE(Sin(a) == Approx(std::sin(a)).margin(kTrigEps));
    }
    STATIC_REQUIRE(CtClose(Sin(0.0f), 0.0f));
    STATIC_REQUIRE(CtClose(Sin(kPi/2), 1.0f));
}

TEST_CASE("Cos constexpr matches runtime", AUTO_TAG) {
    constexpr float angles[] = {0.0f, kPi/6, kPi/4, kPi/3, kPi/2, kPi, -kPi/3, 5*kPi};
    for (float a : angles) {
        REQUIRE(Cos(a) == Approx(std::cos(a)).margin(kTrigEps));
    }
    STATIC_REQUIRE(CtClose(Cos(0.0f), 1.0f));
}

TEST_CASE("SinCos matches Sin/Cos individually", AUTO_TAG) {
    constexpr float angles[] = {0.0f, 0.5f, 1.0f, -2.3f, kPi};
    for (float a : angles) {
        auto [s, c] = SinCos(a);
        REQUIRE(s == Approx(Sin(a)).margin(1e-7f));
        REQUIRE(c == Approx(Cos(a)).margin(1e-7f));
    }
}

TEST_CASE("Tan constexpr matches runtime", AUTO_TAG) {
    constexpr float angles[] = {0.0f, 0.3f, -0.7f, 1.0f};
    for (float a : angles) {
        REQUIRE(Tan(a) == Approx(std::tan(a)).margin(kTrigEps));
    }
}

// ===========================================================================
// Inverse trig
// ===========================================================================

TEST_CASE("Atan matches runtime", AUTO_TAG) {
    constexpr float vals[] = {0.0f, 0.5f, 1.0f, -1.0f, 10.0f, -0.3f};
    for (float v : vals) {
        REQUIRE(Atan(v) == Approx(std::atan(v)).margin(kTrigEps));
    }
}

TEST_CASE("Atan2 matches runtime", AUTO_TAG) {
    struct P { float y, x; };
    P cases[] = {{1,1},{-1,1},{1,-1},{-1,-1},{0,1},{1,0},{0,-1},{-1,0}};
    for (auto [y,x] : cases) {
        REQUIRE(Atan2(y,x) == Approx(std::atan2(y,x)).margin(kTrigEps));
    }
}

TEST_CASE("Asin/Acos match runtime", AUTO_TAG) {
    constexpr float vals[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    for (float v : vals) {
        REQUIRE(Asin(v) == Approx(std::asin(v)).margin(kTrigEps));
        REQUIRE(Acos(v) == Approx(std::acos(v)).margin(kTrigEps));
    }
    STATIC_REQUIRE(CtClose(Asin(0.0f), 0.0f));
    STATIC_REQUIRE(CtClose(Acos(1.0f), 0.0f));
}

// ===========================================================================
// Utility: Min/Max/Clamp/Saturate/Lerp/Sign/Radians/Degrees
// ===========================================================================

TEST_CASE("Min/Max variadic fold — constexpr", AUTO_TAG) {
    STATIC_REQUIRE(Min(3.0f, 1.0f, 2.0f) == 1.0f);
    STATIC_REQUIRE(Max(3.0f, 1.0f, 2.0f) == 3.0f);
    STATIC_REQUIRE(Min(5, 3, 7, 1) == 1);
    STATIC_REQUIRE(Max(5, 3, 7, 1) == 7);
}

TEST_CASE("Clamp/Saturate — constexpr", AUTO_TAG) {
    STATIC_REQUIRE(Clamp(5.0f, 0.0f, 3.0f) == 3.0f);
    STATIC_REQUIRE(Clamp(-1.0f, 0.0f, 3.0f) == 0.0f);
    STATIC_REQUIRE(Clamp(1.5f, 0.0f, 3.0f) == 1.5f);
    STATIC_REQUIRE(Saturate(1.5f) == 1.0f);
    STATIC_REQUIRE(Saturate(-0.5f) == 0.0f);
    STATIC_REQUIRE(Saturate(0.5f) == 0.5f);
}

TEST_CASE("Lerp/Sign/Radians/Degrees — constexpr", AUTO_TAG) {
    STATIC_REQUIRE(Lerp(0.0f, 10.0f, 0.5f) == 5.0f);
    STATIC_REQUIRE(Sign(5.0f) == 1.0f);
    STATIC_REQUIRE(Sign(-3.0f) == -1.0f);
    STATIC_REQUIRE(Sign(0.0f) == 0.0f);
    STATIC_REQUIRE(Sign(-7) == -1);
    STATIC_REQUIRE(CtClose(Radians(180.0f), kPi));
    STATIC_REQUIRE(CtClose(Degrees(kPi), 180.0f, 1e-3f));
}

// ===========================================================================
// Double precision: compile-time kernels
// ===========================================================================

TEST_CASE("Sqrt<double> constexpr matches runtime", AUTO_TAG) {
    STATIC_REQUIRE(Sqrt(0.0) == 0.0);
    STATIC_REQUIRE(Sqrt(1.0) == 1.0);
    STATIC_REQUIRE(CtCloseD(Sqrt(4.0), 2.0));
    STATIC_REQUIRE(CtCloseD(Sqrt(2.0), 1.4142135623730951, 1e-14));

    REQUIRE(Sqrt(9.0) == Approx(std::sqrt(9.0)).margin(1e-14));
    REQUIRE(Sqrt(0.01) == Approx(std::sqrt(0.01)).margin(1e-14));
    REQUIRE(Sqrt(1e20) == Approx(std::sqrt(1e20)).margin(1e6));
}

TEST_CASE("Sin<double> constexpr matches runtime", AUTO_TAG) {
    constexpr double angles[] = {0.0, kPiD/6, kPiD/4, kPiD/3, kPiD/2, kPiD, -kPiD/4, 3*kPiD, 10.0};
    for (double a : angles) {
        REQUIRE(Sin(a) == Approx(std::sin(a)).margin(kTrigEpsD));
    }
    STATIC_REQUIRE(CtCloseD(Sin(0.0), 0.0));
    STATIC_REQUIRE(CtCloseD(Sin(kPiD/2), 1.0));
}

TEST_CASE("Cos<double> constexpr matches runtime", AUTO_TAG) {
    constexpr double angles[] = {0.0, kPiD/6, kPiD/4, kPiD/3, kPiD/2, kPiD, -kPiD/3, 5*kPiD};
    for (double a : angles) {
        REQUIRE(Cos(a) == Approx(std::cos(a)).margin(kTrigEpsD));
    }
    STATIC_REQUIRE(CtCloseD(Cos(0.0), 1.0));
}

TEST_CASE("SinCos<double> matches Sin/Cos individually", AUTO_TAG) {
    constexpr double angles[] = {0.0, 0.5, 1.0, -2.3, kPiD};
    for (double a : angles) {
        auto [s, c] = SinCos(a);
        REQUIRE(s == Approx(Sin(a)).margin(1e-15));
        REQUIRE(c == Approx(Cos(a)).margin(1e-15));
    }
}

TEST_CASE("Tan<double> constexpr matches runtime", AUTO_TAG) {
    constexpr double angles[] = {0.0, 0.3, -0.7, 1.0};
    for (double a : angles) {
        REQUIRE(Tan(a) == Approx(std::tan(a)).margin(kTrigEpsD));
    }
}

TEST_CASE("Atan<double> matches runtime", AUTO_TAG) {
    constexpr double vals[] = {0.0, 0.5, 1.0, -1.0, 10.0, -0.3};
    for (double v : vals) {
        REQUIRE(Atan(v) == Approx(std::atan(v)).margin(kTrigEpsD));
    }
}

TEST_CASE("Atan2<double> matches runtime", AUTO_TAG) {
    struct P { double y, x; };
    P cases[] = {{1,1},{-1,1},{1,-1},{-1,-1},{0,1},{1,0},{0,-1},{-1,0}};
    for (auto [y,x] : cases) {
        REQUIRE(Atan2(y,x) == Approx(std::atan2(y,x)).margin(kTrigEpsD));
    }
}

TEST_CASE("Asin/Acos<double> match runtime", AUTO_TAG) {
    constexpr double vals[] = {-1.0, -0.5, 0.0, 0.5, 1.0};
    for (double v : vals) {
        REQUIRE(Asin(v) == Approx(std::asin(v)).margin(kTrigEpsD));
        REQUIRE(Acos(v) == Approx(std::acos(v)).margin(kTrigEpsD));
    }
    STATIC_REQUIRE(CtCloseD(Asin(0.0), 0.0));
    STATIC_REQUIRE(CtCloseD(Acos(1.0), 0.0));
}

// =========================================================================
// User-defined literals
// =========================================================================

TEST_CASE("_deg literal converts degrees to radians (float)", AUTO_TAG) {
    using namespace Mashiro::Math;
    constexpr float halfPi = Const::kHalfPi<float>;
    constexpr float pi = Const::kPi<float>;

    STATIC_REQUIRE(CtClose(0.0_deg, 0.0f));
    STATIC_REQUIRE(CtClose(90.0_deg, halfPi));
    STATIC_REQUIRE(CtClose(180.0_deg, pi));
    STATIC_REQUIRE(CtClose(360.0_deg, 2.0f * pi));
    STATIC_REQUIRE(CtClose(-90.0_deg, -halfPi));

    // Integer overload
    STATIC_REQUIRE(CtClose(0_deg, 0.0f));
    STATIC_REQUIRE(CtClose(90_deg, halfPi));
    STATIC_REQUIRE(CtClose(180_deg, pi));
    STATIC_REQUIRE(CtClose(45_deg, pi / 4.0f));
}

TEST_CASE("_rad literal is identity (float)", AUTO_TAG) {
    using namespace Mashiro::Math;
    STATIC_REQUIRE(1.0_rad == 1.0f);
    STATIC_REQUIRE(0.0_rad == 0.0f);
    STATIC_REQUIRE(3.14159_rad == static_cast<float>(3.14159L));

    // Integer overload
    STATIC_REQUIRE(0_rad == 0.0f);
    STATIC_REQUIRE(1_rad == 1.0f);
    STATIC_REQUIRE(2_rad == 2.0f);
}

TEST_CASE("_deg_d literal converts degrees to radians (long double)", AUTO_TAG) {
    using namespace Mashiro::Math;
    constexpr long double halfPi = Const::kHalfPi<long double>;
    constexpr long double pi = Const::kPi<long double>;
    constexpr long double eps = 1e-15L;

    constexpr auto ldClose = [](long double a, long double b, long double e = eps) {
        return (a - b < e) && (b - a < e);
    };

    STATIC_REQUIRE(ldClose(0.0_deg_d, 0.0L));
    STATIC_REQUIRE(ldClose(90.0_deg_d, halfPi));
    STATIC_REQUIRE(ldClose(180.0_deg_d, pi));
    STATIC_REQUIRE(ldClose(360.0_deg_d, 2.0L * pi));
    STATIC_REQUIRE(ldClose(-90.0_deg_d, -halfPi));

    // Integer overload
    STATIC_REQUIRE(ldClose(90_deg_d, halfPi));
    STATIC_REQUIRE(ldClose(180_deg_d, pi));
}

TEST_CASE("_rad_d literal is identity (long double)", AUTO_TAG) {
    using namespace Mashiro::Math;
    STATIC_REQUIRE(1.0_rad_d == 1.0L);
    STATIC_REQUIRE(0.0_rad_d == 0.0L);
    STATIC_REQUIRE(3.141592653589793_rad_d == 3.141592653589793L);

    // Integer overload
    STATIC_REQUIRE(0_rad_d == 0.0L);
    STATIC_REQUIRE(1_rad_d == 1.0L);
}

TEST_CASE("_deg / _deg_d precision comparison", AUTO_TAG) {
    using namespace Mashiro::Math;
    // float version has limited precision; long double should be tighter
    constexpr float fVal = 30.0_deg;
    constexpr long double ldVal = 30.0_deg_d;
    constexpr long double exact = Const::kPiOverSix<long double>;

    // float: within ~1e-7 relative
    STATIC_REQUIRE(CtClose(fVal, static_cast<float>(exact)));
    // long double: within ~1e-18
    constexpr long double diff = ldVal > exact ? ldVal - exact : exact - ldVal;
    STATIC_REQUIRE(diff < 1e-18L);
}
