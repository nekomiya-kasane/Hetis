#include "Mashiro/Core/Types.h"

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>

using namespace Mashiro;

namespace {

    // Compile-time exercises for the constexpr accessors. Defined as consteval-style
    // helpers so the tests can assert results with STATIC_REQUIRE.
    // NOTE: the vector operator[] (via (&x)[i]) is only a constant expression for
    // index 0, so compile-time coverage uses the matrix single subscript (array
    // indexing) plus direct member access, which are valid in constant expressions.
    constexpr float MatrixColumn() {
        float4x4 m{};
        m[2].x = 5.0f; // single subscript -> columns[2], then member
        return m[2].x;
    }

    constexpr float SphereRadius() {
        BoundingSphere s{};
        s.SetRadius(4.5f);
        return s.GetRadius();
    }

} // namespace

// ---------------------------------------------------------------------------
// Layout / ABI invariants (must stay GPU-uploadable)
// ---------------------------------------------------------------------------

TEST_CASE("Vector types have GPU-compatible size and alignment", "[types]") {
    STATIC_REQUIRE(sizeof(float2) == 8);
    STATIC_REQUIRE(alignof(float2) == 8);
    STATIC_REQUIRE(sizeof(float3b) == 12);
    STATIC_REQUIRE(alignof(float3b) == 4);
    STATIC_REQUIRE(sizeof(float3) == 16);
    STATIC_REQUIRE(alignof(float3) == 16);
    STATIC_REQUIRE(sizeof(float4) == 16);
    STATIC_REQUIRE(alignof(float4) == 16);

    STATIC_REQUIRE(sizeof(int2) == 8);
    STATIC_REQUIRE(alignof(int2) == 8);
    STATIC_REQUIRE(sizeof(uint2) == 8);
    STATIC_REQUIRE(alignof(uint2) == 8);
    STATIC_REQUIRE(sizeof(uint3) == 16);
    STATIC_REQUIRE(alignof(uint3) == 16);
    STATIC_REQUIRE(sizeof(uint4) == 16);
    STATIC_REQUIRE(alignof(uint4) == 16);
}

TEST_CASE("Matrix types have GPU-compatible size and alignment", "[types]") {
    STATIC_REQUIRE(sizeof(float2x2) == 16);
    STATIC_REQUIRE(alignof(float2x2) == 8);
    STATIC_REQUIRE(sizeof(float3x3) == 48);
    STATIC_REQUIRE(alignof(float3x3) == 16);
    STATIC_REQUIRE(sizeof(float4x4) == 64);
    STATIC_REQUIRE(alignof(float4x4) == 16);
}

TEST_CASE("Geometry types have GPU-compatible size and alignment", "[types]") {
    STATIC_REQUIRE(sizeof(AABB) == 32);
    STATIC_REQUIRE(alignof(AABB) == 16);
    STATIC_REQUIRE(sizeof(BoundingSphere) == 16);
    STATIC_REQUIRE(alignof(BoundingSphere) == 16);
    STATIC_REQUIRE(sizeof(FrustumPlanes) == 96);
    STATIC_REQUIRE(alignof(FrustumPlanes) == 16);
    STATIC_REQUIRE(sizeof(Ray) == 32);
    STATIC_REQUIRE(alignof(Ray) == 16);
    STATIC_REQUIRE(sizeof(Plane) == 16);
    STATIC_REQUIRE(alignof(Plane) == 16);
}

TEST_CASE("All GPU types are trivially copyable and standard layout", "[types]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<float2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<float3b>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<float3>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<float4>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<float2x2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<float3x3>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<float4x4>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<int2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<uint2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<uint3>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<uint4>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<AABB>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<BoundingSphere>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<FrustumPlanes>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Ray>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Plane>);

    STATIC_REQUIRE(std::is_standard_layout_v<float2>);
    STATIC_REQUIRE(std::is_standard_layout_v<float3>);
    STATIC_REQUIRE(std::is_standard_layout_v<float4>);
    STATIC_REQUIRE(std::is_standard_layout_v<float4x4>);
    STATIC_REQUIRE(std::is_standard_layout_v<uint4>);
    STATIC_REQUIRE(std::is_standard_layout_v<AABB>);
    STATIC_REQUIRE(std::is_standard_layout_v<Ray>);
    STATIC_REQUIRE(std::is_standard_layout_v<Plane>);
    STATIC_REQUIRE(std::is_standard_layout_v<BoundingSphere>);
}

// ---------------------------------------------------------------------------
// Default initialization
// ---------------------------------------------------------------------------

TEST_CASE("Vector types value-initialize to zero", "[types]") {
    float2 f2{};
    REQUIRE(f2.x == 0.0f);
    REQUIRE(f2.y == 0.0f);

    float3 f3{};
    REQUIRE(f3.x == 0.0f);
    REQUIRE(f3.y == 0.0f);
    REQUIRE(f3.z == 0.0f);
    REQUIRE(f3._pad == 0.0f);

    float4 f4{};
    REQUIRE(f4.x == 0.0f);
    REQUIRE(f4.w == 0.0f);

    int2 i2{};
    REQUIRE(i2.x == 0);
    REQUIRE(i2.y == 0);

    uint4 u4{};
    REQUIRE(u4.x == 0u);
    REQUIRE(u4.z == 0u);
    REQUIRE(u4.w == 0u);
}

// ---------------------------------------------------------------------------
// Vector subscript
// ---------------------------------------------------------------------------

TEST_CASE("Vector operator[] reads and writes components", "[types]") {
    float4 v{};
    v[0] = 1.0f;
    v[1] = 2.0f;
    v[2] = 3.0f;
    v[3] = 4.0f;
    REQUIRE(v.x == 1.0f);
    REQUIRE(v.y == 2.0f);
    REQUIRE(v.z == 3.0f);
    REQUIRE(v.w == 4.0f);

    const float4& cv = v;
    REQUIRE(cv[0] == 1.0f);
    REQUIRE(cv[3] == 4.0f);

    uint3 u{};
    u[0] = 10u;
    u[1] = 20u;
    u[2] = 30u;
    REQUIRE(u.x == 10u);
    REQUIRE(u.y == 20u);
    REQUIRE(u.z == 30u);

    int2 i{};
    i[0] = -5;
    i[1] = 7;
    REQUIRE(i.x == -5);
    REQUIRE(i.y == 7);
}

TEST_CASE("float3 operator[] index 3 maps to the padding slot", "[types]") {
    float3 v{};
    v[0] = 1.0f;
    v[1] = 2.0f;
    v[2] = 3.0f;
    v[3] = 9.0f;
    REQUIRE(v.x == 1.0f);
    REQUIRE(v.z == 3.0f);
    REQUIRE(v._pad == 9.0f);
}


// ---------------------------------------------------------------------------
// Matrix subscript (column-major)
// ---------------------------------------------------------------------------

TEST_CASE("Matrix single subscript returns the column vector", "[types]") {
    float4x4 m{};
    m[2].x = 5.0f;
    REQUIRE(m.columns[2].x == 5.0f);

    float4& col = m[2];
    col.y = 6.0f;
    REQUIRE(m.columns[2].y == 6.0f);

    const float4x4& cm = m;
    REQUIRE(cm[2].x == 5.0f);
}

TEST_CASE("Matrix dual subscript is column-major", "[types]") {
    float4x4 m{};
    m[1, 2] = 7.0f; // row 1, column 2 -> columns[2][1]
    REQUIRE(m.columns[2][1] == 7.0f);
    REQUIRE((m[1, 2]) == 7.0f);

    float2x2 s{};
    s[0, 1] = 3.0f; // row 0, column 1 -> columns[1].x
    REQUIRE(s.columns[1].x == 3.0f);
    REQUIRE((s[0, 1]) == 3.0f);

    float3x3 t{};
    t[2, 0] = 11.0f; // row 2, column 0 -> columns[0].z
    REQUIRE(t.columns[0].z == 11.0f);
}

TEST_CASE("Matrix single subscript works in constant expressions", "[types]") {
    STATIC_REQUIRE(MatrixColumn() == 5.0f);
}

// ---------------------------------------------------------------------------
// Geometry types
// ---------------------------------------------------------------------------

TEST_CASE("AABB stores min and max corners", "[types]") {
    AABB box{};
    box.min = float3{-1.0f, -2.0f, -3.0f};
    box.max = float3{4.0f, 5.0f, 6.0f};
    REQUIRE(box.min.x == -1.0f);
    REQUIRE(box.max.z == 6.0f);
}

TEST_CASE("Ray stores origin and direction", "[types]") {
    Ray r{};
    r.origin    = float3{1.0f, 0.0f, 0.0f};
    r.direction = float3{0.0f, 1.0f, 0.0f};
    REQUIRE(r.origin.x == 1.0f);
    REQUIRE(r.direction.y == 1.0f);
}

TEST_CASE("BoundingSphere radius round-trips through the padding slot", "[types]") {
    BoundingSphere s{};
    s.center = float3{1.0f, 2.0f, 3.0f};
    s.SetRadius(2.5f);
    REQUIRE(s.GetRadius() == 2.5f);
    REQUIRE(s.center._pad == 2.5f); // radius is stored in center._pad
    REQUIRE(s.center.x == 1.0f);    // setting radius leaves x/y/z untouched

    STATIC_REQUIRE(SphereRadius() == 4.5f);
}

TEST_CASE("Plane distance round-trips through the padding slot", "[types]") {
    Plane p{};
    p.normal = float3{0.0f, 1.0f, 0.0f};
    p.SetDistance(-3.0f);
    REQUIRE(p.GetDistance() == -3.0f);
    REQUIRE(p.normal._pad == -3.0f); // distance is stored in normal._pad
    REQUIRE(p.normal.y == 1.0f);
}

TEST_CASE("FrustumPlanes holds six zero-initialized planes", "[types]") {
    FrustumPlanes f{};
    for (int i = 0; i < 6; ++i) {
        REQUIRE(f.planes[i].x == 0.0f);
        REQUIRE(f.planes[i].w == 0.0f);
    }

    f.planes[5] = float4{1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(f.planes[5].w == 4.0f);
}
