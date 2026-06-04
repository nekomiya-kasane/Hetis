#include "Mashiro/Math/Quanterion.h"
#include "Mashiro/Math/Types.h"

#include <catch2/catch_approx.hpp>
#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <concepts>
#include <type_traits>

using namespace Mashiro;
using Catch::Approx;

namespace {

    constexpr float kPi = 3.14159265358979323846f;

    // Reference matrix-vector multiply (column-major, M * v) for cross-checking Quat::Rotate.
    [[nodiscard]] vec3 Mat3MulVec(const mat3& m, vec3 v) {
        return vec3{
            .x = m[0, 0] * v.x + m[0, 1] * v.y + m[0, 2] * v.z,
            .y = m[1, 0] * v.x + m[1, 1] * v.y + m[1, 2] * v.z,
            .z = m[2, 0] * v.x + m[2, 1] * v.y + m[2, 2] * v.z,
        };
    }

    [[nodiscard]] mat3 Mat3Mul(const mat3& a, const mat3& b) {
        mat3 r{};
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) {
                r[row, col] = a[row, 0] * b[0, col] + a[row, 1] * b[1, col] + a[row, 2] * b[2, col];
            }
        }
        return r;
    }

    void RequireVecApprox(vec3 a, vec3 b, float margin = 1e-5f) {
        REQUIRE(a.x == Approx(b.x).margin(margin));
        REQUIRE(a.y == Approx(b.y).margin(margin));
        REQUIRE(a.z == Approx(b.z).margin(margin));
    }

    // Compare orientations: q and -q represent the same rotation, so compare |dot| ~ 1.
    void RequireSameRotation(quat a, quat b, float margin = 1e-4f) {
        REQUIRE(std::abs(Quat::Dot(Quat::Normalize(a), Quat::Normalize(b))) ==
                Approx(1.0f).margin(margin));
    }

} // namespace

// ---------------------------------------------------------------------------
// Layout / ABI
// ---------------------------------------------------------------------------

TEST_CASE("quat has GPU-compatible layout", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(quat) == 16);
    STATIC_REQUIRE(alignof(quat) == 16);
    STATIC_REQUIRE(std::is_trivially_copyable_v<quat>);
    STATIC_REQUIRE(std::is_standard_layout_v<quat>);
}

TEST_CASE("quat default-constructs to identity", AUTO_TAG) {
    quat q{};
    REQUIRE(q.x == 0.0f);
    REQUIRE(q.y == 0.0f);
    REQUIRE(q.z == 0.0f);
    REQUIRE(q.w == 1.0f);
    REQUIRE(q == Quat::Identity());
}

TEST_CASE("quat subscript matches members", AUTO_TAG) {
    quat q{.x = 1.0f, .y = 2.0f, .z = 3.0f, .w = 4.0f};
    REQUIRE(q[0] == 1.0f);
    REQUIRE(q[1] == 2.0f);
    REQUIRE(q[2] == 3.0f);
    REQUIRE(q[3] == 4.0f);
}

// ---------------------------------------------------------------------------
// Construction / extraction
// ---------------------------------------------------------------------------

TEST_CASE("Quat::MakeAxisAngle round-trips through axis/angle", AUTO_TAG) {
    vec3 axis = vec3{0.0f, 0.0f, 1.0f};
    quat q = Quat::MakeAxisAngle(axis, kPi * 0.5f);

    REQUIRE(Quat::GetAngle(q) == Approx(kPi * 0.5f).margin(1e-5f));
    RequireVecApprox(Quat::GetAxis(q), axis);
    REQUIRE(Quat::Norm2(q) == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Quat::MakeRotate{X,Y,Z} agree with axis-angle", AUTO_TAG) {
    RequireSameRotation(Quat::MakeRotateX(0.7f), Quat::MakeAxisAngle({1, 0, 0}, 0.7f));
    RequireSameRotation(Quat::MakeRotateY(0.7f), Quat::MakeAxisAngle({0, 1, 0}, 0.7f));
    RequireSameRotation(Quat::MakeRotateZ(0.7f), Quat::MakeAxisAngle({0, 0, 1}, 0.7f));
}

// ---------------------------------------------------------------------------
// Algebra
// ---------------------------------------------------------------------------

TEST_CASE("Conjugate and inverse undo a unit rotation", AUTO_TAG) {
    quat q = Quat::Normalize(Quat::MakeAxisAngle({1, 2, 3}, 1.1f));

    RequireSameRotation(Quat::Mul(q, Quat::Conjugate(q)), Quat::Identity());
    RequireSameRotation(Quat::Mul(q, Quat::Inverse(q)), Quat::Identity());
}

TEST_CASE("Quat::Mul composes rotations like matrix multiply", AUTO_TAG) {
    quat a = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);
    quat b = Quat::MakeAxisAngle({1, 0, 0}, kPi * 0.5f);

    mat3 lhs = Quat::ToMat3(Quat::Mul(a, b));
    mat3 rhs = Mat3Mul(Quat::ToMat3(a), Quat::ToMat3(b));

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            REQUIRE((lhs[row, col]) == Approx((rhs[row, col])).margin(1e-5f));
        }
    }
}

// ---------------------------------------------------------------------------
// Apply to vectors / matrix conversion
// ---------------------------------------------------------------------------

TEST_CASE("Quat::Rotate turns +X into +Y under a 90deg Z rotation", AUTO_TAG) {
    quat q = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);
    RequireVecApprox(Quat::Rotate(q, vec3{1, 0, 0}), vec3{0, 1, 0});
}

TEST_CASE("Quat::Rotate matches Quat::ToMat3 application", AUTO_TAG) {
    quat q = Quat::Normalize(Quat::MakeAxisAngle({0.3f, -0.6f, 0.8f}, 2.0f));
    mat3 m = Quat::ToMat3(q);

    for (vec3 v : {vec3{1, 0, 0}, vec3{0, 1, 0}, vec3{0, 0, 1}, vec3{1, 2, 3}}) {
        RequireVecApprox(Quat::Rotate(q, v), Mat3MulVec(m, v));
    }
}

TEST_CASE("Quat::ToMat4 embeds the 3x3 rotation with affine identity", AUTO_TAG) {
    quat q = Quat::MakeAxisAngle({0, 1, 0}, kPi * 0.25f);
    mat4 m = Quat::ToMat4(q);

    REQUIRE((m[3, 3]) == Approx(1.0f));
    REQUIRE((m[0, 3]) == Approx(0.0f));
    REQUIRE((m[3, 0]) == Approx(0.0f));
}

TEST_CASE("Quat::MakeTransform builds a TRS matrix", AUTO_TAG) {
    quat q = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);
    mat4 m = Quat::MakeTransform(vec3{5, 6, 7}, q, vec3{2, 2, 2});

    // Column 3 is the translation.
    REQUIRE((m[0, 3]) == Approx(5.0f));
    REQUIRE((m[1, 3]) == Approx(6.0f));
    REQUIRE((m[2, 3]) == Approx(7.0f));

    // Scaled rotation: +X (length 2) maps to +Y * 2.
    REQUIRE((m[0, 0]) == Approx(0.0f).margin(1e-5f));
    REQUIRE((m[1, 0]) == Approx(2.0f).margin(1e-5f));
}

TEST_CASE("Quat::RotateMat3 equals matrix product of the rotation", AUTO_TAG) {
    quat q = Quat::Normalize(Quat::MakeAxisAngle({0.1f, 0.9f, -0.2f}, 1.3f));
    mat3 base = Quat::ToMat3(Quat::MakeAxisAngle({0, 0, 1}, 0.6f));

    mat3 got = Quat::RotateMat3(q, base);
    mat3 want = Mat3Mul(Quat::ToMat3(q), base);

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            REQUIRE((got[row, col]) == Approx((want[row, col])).margin(1e-5f));
        }
    }
}

TEST_CASE("Quat::RotateMat4 rotates basis and translation, keeps homogeneous row", AUTO_TAG) {
    quat q = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);
    mat4 m = Quat::MakeTransform(vec3{1, 0, 0}, Quat::Identity(), vec3{1, 1, 1});

    mat4 out = Quat::RotateMat4(q, m);

    // Translation (1,0,0) rotates 90deg about Z -> (0,1,0).
    REQUIRE((out[0, 3]) == Approx(0.0f).margin(1e-5f));
    REQUIRE((out[1, 3]) == Approx(1.0f).margin(1e-5f));
    REQUIRE((out[3, 3]) == Approx(1.0f));
    REQUIRE((out[3, 0]) == Approx(0.0f));
}

// ---------------------------------------------------------------------------
// Matrix -> quaternion
// ---------------------------------------------------------------------------

TEST_CASE("Quat::MakeFromMat3 inverts Quat::ToMat3", AUTO_TAG) {
    for (float angle : {0.1f, 1.0f, 2.5f, 3.0f}) {
        quat q = Quat::Normalize(Quat::MakeAxisAngle({0.2f, 0.5f, -0.84f}, angle));
        RequireSameRotation(Quat::MakeFromMat3(Quat::ToMat3(q)), q);
    }
}

TEST_CASE("Quat::MakeFromMat3 handles all Shepperd branches", AUTO_TAG) {
    // Each 180-degree rotation makes a different diagonal element dominant.
    RequireSameRotation(Quat::MakeFromMat3(Quat::ToMat3(Quat::MakeAxisAngle({1, 0, 0}, kPi))),
                        Quat::MakeAxisAngle({1, 0, 0}, kPi));
    RequireSameRotation(Quat::MakeFromMat3(Quat::ToMat3(Quat::MakeAxisAngle({0, 1, 0}, kPi))),
                        Quat::MakeAxisAngle({0, 1, 0}, kPi));
    RequireSameRotation(Quat::MakeFromMat3(Quat::ToMat3(Quat::MakeAxisAngle({0, 0, 1}, kPi))),
                        Quat::MakeAxisAngle({0, 0, 1}, kPi));
}

// ---------------------------------------------------------------------------
// Euler
// ---------------------------------------------------------------------------

TEST_CASE("Euler round-trips away from gimbal lock", AUTO_TAG) {
    quat q = Quat::MakeFromEuler(0.3f, 0.5f, -0.7f);
    vec3 e = Quat::ToEuler(q);
    RequireSameRotation(Quat::MakeFromEuler(e.x, e.y, e.z), q);
}

// ---------------------------------------------------------------------------
// High-level builders
// ---------------------------------------------------------------------------

TEST_CASE("Quat::MakeFromTo rotates one vector onto another", AUTO_TAG) {
    vec3 from = vec3{1, 0, 0};
    vec3 to = vec3{0, 0, 1};
    quat q = Quat::MakeFromTo(from, to);
    RequireVecApprox(Quat::Rotate(q, from), to);
}

TEST_CASE("Quat::MakeFromTo handles parallel and opposite vectors", AUTO_TAG) {
    RequireSameRotation(Quat::MakeFromTo({0, 1, 0}, {0, 1, 0}), Quat::Identity());

    quat opp = Quat::MakeFromTo({0, 1, 0}, {0, -1, 0});
    RequireVecApprox(Quat::Rotate(opp, vec3{0, 1, 0}), vec3{0, -1, 0});
}

TEST_CASE("Quat::MakeLookRotation aligns forward with +Z", AUTO_TAG) {
    vec3 forward = vec3{0, 0, 1};
    quat q = Quat::MakeLookRotation(forward, vec3{0, 1, 0});
    RequireVecApprox(Quat::GetForward(q), forward);
}

// ---------------------------------------------------------------------------
// Interpolation
// ---------------------------------------------------------------------------

TEST_CASE("Slerp and Nlerp hit the endpoints", AUTO_TAG) {
    quat a = Quat::MakeAxisAngle({0, 0, 1}, 0.0f);
    quat b = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);

    RequireSameRotation(Quat::Slerp(a, b, 0.0f), a);
    RequireSameRotation(Quat::Slerp(a, b, 1.0f), b);
    RequireSameRotation(Quat::Nlerp(a, b, 0.0f), a);
    RequireSameRotation(Quat::Nlerp(a, b, 1.0f), b);
}

TEST_CASE("Slerp midpoint is half the angle", AUTO_TAG) {
    quat a = Quat::MakeAxisAngle({0, 0, 1}, 0.0f);
    quat b = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);

    quat mid = Quat::Slerp(a, b, 0.5f);
    RequireSameRotation(mid, Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.25f));
}

TEST_CASE("Slerp produces unit quaternions", AUTO_TAG) {
    quat a = Quat::Normalize(Quat::MakeAxisAngle({1, 1, 0}, 0.4f));
    quat b = Quat::Normalize(Quat::MakeAxisAngle({0, 1, 1}, 2.2f));

    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(Quat::Norm2(Quat::Slerp(a, b, t)) == Approx(1.0f).margin(1e-4f));
    }
}

// ---------------------------------------------------------------------------
// Compile-time evaluation (constexpr kernels selected via `if consteval`)
// ---------------------------------------------------------------------------

namespace {

    constexpr bool Close(float a, float b, float eps = 1e-4f) {
        float d = a - b;
        return (d < 0.0f ? -d : d) < eps;
    }

} // namespace

TEST_CASE("Quaternion math folds at compile time", AUTO_TAG) {
    // Everything below is initialized in a constant-expression context, so it
    // forces the constexpr sin/cos/sqrt/atan2/asin/acos kernels (not the runtime
    // std:: path) and proves the API is usable in constant expressions.

    // Axis-angle: 90deg about Z -> (0, 0, sin45, cos45).
    constexpr quat q = Quat::MakeAxisAngle(vec3{0, 0, 1}, kPi * 0.5f);
    STATIC_REQUIRE(Close(q.z, 0.70710678f));
    STATIC_REQUIRE(Close(q.w, 0.70710678f));

    // Vector application: +X -> +Y.
    constexpr vec3 v = Quat::Rotate(q, vec3{1, 0, 0});
    STATIC_REQUIRE(Close(v.x, 0.0f));
    STATIC_REQUIRE(Close(v.y, 1.0f));

    // Matrix conversion folds; column 0 = (0, 1, 0). Read via single subscript +
    // member, the only constexpr-safe path (m[row, col] with row != 0 is not).
    constexpr mat3 m = Quat::ToMat3(q);
    STATIC_REQUIRE(Close(m[0].x, 0.0f));
    STATIC_REQUIRE(Close(m[0].y, 1.0f));

    // q * conj(q) == identity (algebra is constexpr).
    constexpr quat e = Quat::Mul(q, Quat::Conjugate(q));
    STATIC_REQUIRE(Close(e.w, 1.0f));
    STATIC_REQUIRE(Close(e.x, 0.0f));

    // Normalization / norm exercise the constexpr Sqrt kernel.
    STATIC_REQUIRE(Close(Quat::Norm2(Quat::MakeAxisAngle(vec3{1, 1, 1}, 1.3f)), 1.0f));

    // Euler round-trip exercises constexpr Sin/Cos (build) and Atan2/Asin (extract).
    constexpr quat eq = Quat::MakeFromEuler(0.3f, 0.5f, -0.7f);
    constexpr vec3 ea = Quat::ToEuler(eq);
    constexpr quat eq2 = Quat::MakeFromEuler(ea.x, ea.y, ea.z);
    STATIC_REQUIRE(Close(Quat::Dot(eq, eq2), 1.0f, 1e-3f));

    // Slerp midpoint exercises constexpr Acos/Sin and equals the half-angle rotation.
    constexpr quat sa = Quat::MakeAxisAngle(vec3{0, 0, 1}, 0.0f);
    constexpr quat sb = Quat::MakeAxisAngle(vec3{0, 0, 1}, kPi * 0.5f);
    constexpr quat mid = Quat::Slerp(sa, sb, 0.5f);
    constexpr quat ref = Quat::MakeAxisAngle(vec3{0, 0, 1}, kPi * 0.25f);
    STATIC_REQUIRE(Close(Quat::Dot(mid, ref), 1.0f, 1e-3f));
}

// ---------------------------------------------------------------------------
// Operators (hidden friends)
// ---------------------------------------------------------------------------

TEST_CASE("quat operators match the named algebra functions", AUTO_TAG) {
    quat a = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);
    quat b = Quat::MakeAxisAngle({1, 0, 0}, kPi * 0.5f);

    REQUIRE(a * b == Quat::Mul(a, b)); // Hamilton product
    REQUIRE(a + b == Quat::Add(a, b));
    REQUIRE(a - b == Quat::Sub(a, b));
    REQUIRE(a * 2.0f == Quat::Scale(a, 2.0f));
    REQUIRE(2.0f * a == Quat::Scale(a, 2.0f));
    REQUIRE(-a == Quat::Neg(a));

    quat c = a;
    c *= b;
    REQUIRE(c == Quat::Mul(a, b));
    c = a;
    c += b;
    REQUIRE(c == Quat::Add(a, b));
}

// ---------------------------------------------------------------------------
// Exp / Log / Pow
// ---------------------------------------------------------------------------

TEST_CASE("Exp inverts Log on the unit sphere", AUTO_TAG) {
    quat q = Quat::Normalize(Quat::MakeAxisAngle({0.3f, -0.6f, 0.8f}, 1.7f));
    RequireSameRotation(Quat::Exp(Quat::Log(q)), q);
}

TEST_CASE("Pow behaves like repeated rotation", AUTO_TAG) {
    quat q = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f);

    RequireSameRotation(Quat::Pow(q, 1.0f), q);
    RequireSameRotation(Quat::Pow(q, 0.0f), Quat::Identity());

    quat half = Quat::Pow(q, 0.5f);      // half the rotation...
    RequireSameRotation(half * half, q); // ...applied twice is the whole
    RequireSameRotation(half, Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.25f));
}

// ---------------------------------------------------------------------------
// Lerp / RotateTowards / Squad
// ---------------------------------------------------------------------------

TEST_CASE("Lerp hits the endpoints", AUTO_TAG) {
    quat a = Quat::MakeAxisAngle({0, 0, 1}, 0.2f);
    quat b = Quat::MakeAxisAngle({0, 0, 1}, 1.2f);
    REQUIRE(Quat::Lerp(a, b, 0.0f) == a);
    REQUIRE(Quat::Lerp(a, b, 1.0f) == b);
}

TEST_CASE("RotateTowards clamps to the max step then reaches the target", AUTO_TAG) {
    quat a = Quat::Identity();
    quat b = Quat::MakeAxisAngle({0, 0, 1}, kPi * 0.5f); // 90 degrees away

    // Capped step: a quarter of pi/2 of progress is half the way (45 degrees).
    quat partial = Quat::RotateTowards(a, b, kPi * 0.25f);
    REQUIRE(Quat::GetAngle(partial) == Approx(kPi * 0.25f).margin(1e-4f));

    // Budget exceeds the gap: snaps to the target.
    RequireSameRotation(Quat::RotateTowards(a, b, kPi), b);
    // No budget: stays put.
    RequireSameRotation(Quat::RotateTowards(a, b, 0.0f), a);
}

TEST_CASE("Squad passes through its endpoints", AUTO_TAG) {
    quat q0 = Quat::MakeAxisAngle({0, 0, 1}, 0.0f);
    quat q1 = Quat::MakeAxisAngle({0, 0, 1}, 1.0f);
    quat a = Quat::SquadTangent(Quat::MakeAxisAngle({0, 0, 1}, -0.5f), q0, q1);
    quat b = Quat::SquadTangent(q0, q1, Quat::MakeAxisAngle({0, 0, 1}, 1.5f));

    RequireSameRotation(Quat::Squad(q0, a, b, q1, 0.0f), q0);
    RequireSameRotation(Quat::Squad(q0, a, b, q1, 1.0f), q1);
}

// ---------------------------------------------------------------------------
// Math::Quat alias
// ---------------------------------------------------------------------------

TEST_CASE("Mashiro::Math::Quat aliases Mashiro::Quat", AUTO_TAG) {
    quat viaAlias = Math::Quat::MakeAxisAngle({0, 1, 0}, 0.9f);
    quat viaQuat = Quat::MakeAxisAngle({0, 1, 0}, 0.9f);
    REQUIRE(viaAlias == viaQuat);
    RequireSameRotation(Math::Quat::Slerp(viaAlias, viaQuat, 0.5f), viaQuat);
}

TEST_CASE("Exp/Log/Pow fold at compile time", AUTO_TAG) {
    constexpr quat q = Quat::MakeAxisAngle(vec3{0, 0, 1}, kPi * 0.5f);
    constexpr quat rt = Quat::Exp(Quat::Log(q));
    STATIC_REQUIRE(Close(Quat::Dot(rt, q), 1.0f, 1e-3f));

    constexpr quat hp = Quat::Pow(q, 0.5f);
    constexpr quat ref = Quat::MakeAxisAngle(vec3{0, 0, 1}, kPi * 0.25f);
    STATIC_REQUIRE(Close(Quat::Dot(hp, ref), 1.0f, 1e-3f));
}
