#include "Mashiro/Math/Types.h"
#include "Mashiro/Geom/Geom.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>

using namespace Mashiro;

namespace {

    // Compile-time exercises for the constexpr accessors, asserted via STATIC_REQUIRE.
    // The vector operator[] now dispatches with `if consteval`, so it is a constant
    // expression for every index (compile time uses member access; runtime keeps the
    // fast (&x)[i] load). As a result matrix m[row, col] also folds for any row.
    constexpr float VecSubscript() {
        vec4 v{};
        v[0] = 1.0f;
        v[1] = 2.0f;
        v[2] = 3.0f;
        v[3] = 4.0f; // every index is constexpr-valid now
        return v[0] + v[1] + v[2] + v[3];
    }

    constexpr uint32_t UintSubscript() {
        uvec3 u{};
        u[2] = 7u; // index != 0 was previously not a constant expression
        return u[2];
    }

    constexpr float MatrixElement() {
        mat4 m{};
        m[1, 2] = 7.0f; // dual subscript, row != 0 -> now a constant expression
        m[3, 0] = 9.0f;
        return m[1, 2] + m[3, 0];
    }

    constexpr float MatrixColumn() {
        mat4 m{};
        m[2].x = 5.0f; // single subscript -> columns[2], then member
        return m[2].x;
    }

    constexpr float SphereRadius() {
        Sphere s{};
        s.radius = 4.5f;
        return s.radius;
    }

} // namespace

// ---------------------------------------------------------------------------
// Layout / ABI invariants (must stay GPU-uploadable)
// ---------------------------------------------------------------------------

TEST_CASE("Vector types have GPU-compatible size and alignment", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(vec2) == 8);
    STATIC_REQUIRE(alignof(vec2) == 8);
    STATIC_REQUIRE(sizeof(vec3b) == 12);
    STATIC_REQUIRE(alignof(vec3b) == 4);
    STATIC_REQUIRE(sizeof(vec3) == 16);
    STATIC_REQUIRE(alignof(vec3) == 16);
    STATIC_REQUIRE(sizeof(vec4) == 16);
    STATIC_REQUIRE(alignof(vec4) == 16);

    STATIC_REQUIRE(sizeof(ivec2) == 8);
    STATIC_REQUIRE(alignof(ivec2) == 8);
    STATIC_REQUIRE(sizeof(uvec2) == 8);
    STATIC_REQUIRE(alignof(uvec2) == 8);
    STATIC_REQUIRE(sizeof(uvec3) == 16);
    STATIC_REQUIRE(alignof(uvec3) == 16);
    STATIC_REQUIRE(sizeof(uvec4) == 16);
    STATIC_REQUIRE(alignof(uvec4) == 16);
}

TEST_CASE("Matrix types have GPU-compatible size and alignment", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(mat2) == 16);
    STATIC_REQUIRE(alignof(mat2) == 8);
    STATIC_REQUIRE(sizeof(mat3) == 48);
    STATIC_REQUIRE(alignof(mat3) == 16);
    STATIC_REQUIRE(sizeof(mat4) == 64);
    STATIC_REQUIRE(alignof(mat4) == 16);
}

TEST_CASE("Geometry types have GPU-compatible size and alignment", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(AABB) == 32);
    STATIC_REQUIRE(alignof(AABB) == 16);
    STATIC_REQUIRE(sizeof(Sphere) == 16);
    STATIC_REQUIRE(alignof(Sphere) == 16);
    STATIC_REQUIRE(sizeof(FrustumPlanes) == 96);
    STATIC_REQUIRE(alignof(FrustumPlanes) == 16);
    STATIC_REQUIRE(sizeof(Ray) == 32);
    STATIC_REQUIRE(alignof(Ray) == 16);
    STATIC_REQUIRE(sizeof(Plane) == 16);
    STATIC_REQUIRE(alignof(Plane) == 16);
}

TEST_CASE("All GPU types are trivially copyable and standard layout", AUTO_TAG) {
    STATIC_REQUIRE(std::is_trivially_copyable_v<vec2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<vec3b>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<vec3>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<vec4>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<mat2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<mat3>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<mat4>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<ivec2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<uvec2>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<uvec3>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<uvec4>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<AABB>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Sphere>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<FrustumPlanes>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Ray>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Plane>);

    STATIC_REQUIRE(std::is_standard_layout_v<vec2>);
    STATIC_REQUIRE(std::is_standard_layout_v<vec3>);
    STATIC_REQUIRE(std::is_standard_layout_v<vec4>);
    STATIC_REQUIRE(std::is_standard_layout_v<mat4>);
    STATIC_REQUIRE(std::is_standard_layout_v<uvec4>);
    STATIC_REQUIRE(std::is_standard_layout_v<AABB>);
    STATIC_REQUIRE(std::is_standard_layout_v<Ray>);
    STATIC_REQUIRE(std::is_standard_layout_v<Plane>);
    STATIC_REQUIRE(std::is_standard_layout_v<Sphere>);
}

// ---------------------------------------------------------------------------
// Default initialization
// ---------------------------------------------------------------------------

TEST_CASE("Vector types value-initialize to zero", AUTO_TAG) {
    vec2 f2{};
    REQUIRE(f2.x == 0.0f);
    REQUIRE(f2.y == 0.0f);

    vec3 f3{};
    REQUIRE(f3.x == 0.0f);
    REQUIRE(f3.y == 0.0f);
    REQUIRE(f3.z == 0.0f);

    vec4 f4{};
    REQUIRE(f4.x == 0.0f);
    REQUIRE(f4.w == 0.0f);

    ivec2 i2{};
    REQUIRE(i2.x == 0);
    REQUIRE(i2.y == 0);

    uvec4 u4{};
    REQUIRE(u4.x == 0u);
    REQUIRE(u4.z == 0u);
    REQUIRE(u4.w == 0u);
}

// ---------------------------------------------------------------------------
// Vector subscript
// ---------------------------------------------------------------------------

TEST_CASE("Vector operator[] reads and writes components", AUTO_TAG) {
    vec4 v{};
    v[0] = 1.0f;
    v[1] = 2.0f;
    v[2] = 3.0f;
    v[3] = 4.0f;
    REQUIRE(v.x == 1.0f);
    REQUIRE(v.y == 2.0f);
    REQUIRE(v.z == 3.0f);
    REQUIRE(v.w == 4.0f);

    const vec4& cv = v;
    REQUIRE(cv[0] == 1.0f);
    REQUIRE(cv[3] == 4.0f);

    uvec3 u{};
    u[0] = 10u;
    u[1] = 20u;
    u[2] = 30u;
    REQUIRE(u.x == 10u);
    REQUIRE(u.y == 20u);
    REQUIRE(u.z == 30u);

    ivec2 i{};
    i[0] = -5;
    i[1] = 7;
    REQUIRE(i.x == -5);
    REQUIRE(i.y == 7);
}

TEST_CASE("vec3 operator[] reads and writes x, y, z", AUTO_TAG) {
    vec3 v{};
    v[0] = 1.0f;
    v[1] = 2.0f;
    v[2] = 3.0f;
    REQUIRE(v.x == 1.0f);
    REQUIRE(v.y == 2.0f);
    REQUIRE(v.z == 3.0f);
}


// ---------------------------------------------------------------------------
// Matrix subscript (column-major)
// ---------------------------------------------------------------------------

TEST_CASE("Matrix single subscript returns the column vector", AUTO_TAG) {
    mat4 m{};
    m[2].x = 5.0f;
    REQUIRE(m.columns[2].x == 5.0f);

    vec4& col = m[2];
    col.y = 6.0f;
    REQUIRE(m.columns[2].y == 6.0f);

    const mat4& cm = m;
    REQUIRE(cm[2].x == 5.0f);
}

TEST_CASE("Matrix dual subscript is column-major", AUTO_TAG) {
    mat4 m{};
    m[1, 2] = 7.0f; // row 1, column 2 -> columns[2][1]
    REQUIRE(m.columns[2][1] == 7.0f);
    REQUIRE((m[1, 2]) == 7.0f);

    mat2 s{};
    s[0, 1] = 3.0f; // row 0, column 1 -> columns[1].x
    REQUIRE(s.columns[1].x == 3.0f);
    REQUIRE((s[0, 1]) == 3.0f);

    mat3 t{};
    t[2, 0] = 11.0f; // row 2, column 0 -> columns[0].z
    REQUIRE(t.columns[0].z == 11.0f);
}

TEST_CASE("Matrix single subscript works in constant expressions", AUTO_TAG) {
    STATIC_REQUIRE(MatrixColumn() == 5.0f);
}

TEST_CASE("Vector subscript works in constant expressions for every index", AUTO_TAG) {
    STATIC_REQUIRE(VecSubscript() == 10.0f);
    STATIC_REQUIRE(UintSubscript() == 7u);
}

TEST_CASE("Matrix dual subscript works in constant expressions for any row", AUTO_TAG) {
    STATIC_REQUIRE(MatrixElement() == 16.0f);
}

// ---------------------------------------------------------------------------
// Geometry types
// ---------------------------------------------------------------------------

TEST_CASE("AABB stores min and max corners", AUTO_TAG) {
    AABB box{};
    box.min = vec3{-1.0f, -2.0f, -3.0f};
    box.max = vec3{4.0f, 5.0f, 6.0f};
    REQUIRE(box.min.x == -1.0f);
    REQUIRE(box.max.z == 6.0f);
}

TEST_CASE("Ray stores origin and direction", AUTO_TAG) {
    Ray r{};
    r.origin    = vec3{1.0f, 0.0f, 0.0f};
    r.direction = vec3{0.0f, 1.0f, 0.0f};
    REQUIRE(r.origin.x == 1.0f);
    REQUIRE(r.direction.y == 1.0f);
}

TEST_CASE("Sphere stores center and radius", AUTO_TAG) {
    Sphere s{};
    s.center = vec3b{1.0f, 2.0f, 3.0f};
    s.radius = 2.5f;
    REQUIRE(s.radius == 2.5f);
    REQUIRE(s.center.x == 1.0f);
    REQUIRE(s.center.z == 3.0f);

    STATIC_REQUIRE(SphereRadius() == 4.5f);
}

TEST_CASE("Plane stores normal and distance", AUTO_TAG) {
    Plane p{};
    p.normal = vec3b{0.0f, 1.0f, 0.0f};
    p.dist = -3.0f;
    REQUIRE(p.dist == -3.0f);
    REQUIRE(p.normal.y == 1.0f);
}

TEST_CASE("FrustumPlanes holds six zero-initialized planes", AUTO_TAG) {
    FrustumPlanes f{};
    for (int i = 0; i < 6; ++i) {
        REQUIRE(f.planes[i].x == 0.0f);
        REQUIRE(f.planes[i].w == 0.0f);
    }

    f.planes[5] = vec4{1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(f.planes[5].w == 4.0f);
}

// ---------------------------------------------------------------------------
// Operator overloads (hidden friends). Values are exact in float, so == is safe.
// ---------------------------------------------------------------------------

TEST_CASE("float vector operators are component-wise with scalar broadcast", AUTO_TAG) {
    vec3 a{1.0f, 2.0f, 3.0f};
    vec3 b{4.0f, 5.0f, 6.0f};

    REQUIRE(a + b == vec3{5.0f, 7.0f, 9.0f});
    REQUIRE(b - a == vec3{3.0f, 3.0f, 3.0f});
    REQUIRE(a * b == vec3{4.0f, 10.0f, 18.0f}); // Hadamard
    REQUIRE(b / a == vec3{4.0f, 2.5f, 2.0f});
    REQUIRE(a * 2.0f == vec3{2.0f, 4.0f, 6.0f});
    REQUIRE(2.0f * a == vec3{2.0f, 4.0f, 6.0f});
    REQUIRE(a / 2.0f == vec3{0.5f, 1.0f, 1.5f});
    REQUIRE(-a == vec3{-1.0f, -2.0f, -3.0f});
    REQUIRE(+a == a);

    vec4 c{1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(c + c == vec4{2.0f, 4.0f, 6.0f, 8.0f});
    REQUIRE(c * 0.5f == vec4{0.5f, 1.0f, 1.5f, 2.0f});

    vec2 d{3.0f, 4.0f};
    REQUIRE(d + vec2{1.0f, 1.0f} == vec2{4.0f, 5.0f});
    REQUIRE(-d == vec2{-3.0f, -4.0f});
}

TEST_CASE("float vector compound assignment mutates in place", AUTO_TAG) {
    vec3 v{1.0f, 2.0f, 3.0f};
    v += vec3{1.0f, 1.0f, 1.0f};
    REQUIRE(v == vec3{2.0f, 3.0f, 4.0f});
    v -= vec3{2.0f, 2.0f, 2.0f};
    REQUIRE(v == vec3{0.0f, 1.0f, 2.0f});
    v *= 3.0f;
    REQUIRE(v == vec3{0.0f, 3.0f, 6.0f});
    v /= 3.0f;
    REQUIRE(v == vec3{0.0f, 1.0f, 2.0f});
    v *= vec3{2.0f, 2.0f, 2.0f};
    REQUIRE(v == vec3{0.0f, 2.0f, 4.0f});
}

TEST_CASE("integer vector operators are component-wise", AUTO_TAG) {
    ivec2 a{2, -3};
    ivec2 b{5, 7};
    REQUIRE(a + b == ivec2{7, 4});
    REQUIRE(b - a == ivec2{3, 10});
    REQUIRE(a * b == ivec2{10, -21});
    REQUIRE(a * 2 == ivec2{4, -6});
    REQUIRE(-a == ivec2{-2, 3});

    uvec3 u{1u, 2u, 3u};
    REQUIRE(u + u == uvec3{2u, 4u, 6u});
    REQUIRE(u * 3u == uvec3{3u, 6u, 9u});
    REQUIRE(u == uvec3{1u, 2u, 3u});
    REQUIRE(u != uvec3{1u, 2u, 4u});
}

TEST_CASE("matrix * vector applies the linear map", AUTO_TAG) {
    mat4 m{};
    m[0, 0] = 2.0f;
    m[1, 1] = 3.0f;
    m[2, 2] = 4.0f;
    m[3, 3] = 1.0f;
    m[0, 3] = 10.0f; // translation column
    m[1, 3] = 20.0f;
    m[2, 3] = 30.0f;

    vec4 p = m * vec4{1.0f, 1.0f, 1.0f, 1.0f};
    REQUIRE(p == vec4{12.0f, 23.0f, 34.0f, 1.0f});

    mat3 r{};
    r[0, 0] = 1.0f;
    r[1, 0] = 2.0f; // column 0 = (1, 2, 0)
    r[0, 1] = 3.0f; // column 1 = (3, 0, 0)
    vec3 v = r * vec3{1.0f, 1.0f, 0.0f};
    REQUIRE(v == vec3{4.0f, 2.0f, 0.0f});
}

TEST_CASE("matrix * matrix is column-major composition", AUTO_TAG) {
    mat4 id{};
    id[0, 0] = 1.0f;
    id[1, 1] = 1.0f;
    id[2, 2] = 1.0f;
    id[3, 3] = 1.0f;

    mat4 m{};
    m[0, 0] = 2.0f;
    m[1, 1] = 3.0f;
    m[2, 2] = 4.0f;
    m[3, 3] = 1.0f;
    m[0, 3] = 7.0f;

    REQUIRE(m * id == m);
    REQUIRE(id * m == m);

    mat4 twice = m * m;
    REQUIRE((twice[0, 0]) == 4.0f);  // 2 * 2
    REQUIRE((twice[0, 3]) == 21.0f); // 2*7 + 7*1
    REQUIRE((twice[3, 3]) == 1.0f);
}

TEST_CASE("matrix additive and scalar operators are component-wise", AUTO_TAG) {
    mat2 a{};
    a[0, 0] = 1.0f;
    a[1, 1] = 2.0f;
    mat2 b = a + a;
    REQUIRE((b[0, 0]) == 2.0f);
    REQUIRE((b[1, 1]) == 4.0f);
    REQUIRE(a * 3.0f == b + a);
    REQUIRE(-a == (a * -1.0f));
    REQUIRE(a == a);
}

TEST_CASE("non-square matrix operators produce exact shapes", AUTO_TAG) {
    // A : 2 rows x 3 cols = [[1,2,3],[4,5,6]]
    mat2x3 A{};
    A[0, 0] = 1.0f; A[0, 1] = 2.0f; A[0, 2] = 3.0f;
    A[1, 0] = 4.0f; A[1, 1] = 5.0f; A[1, 2] = 6.0f;

    // B : 3 rows x 2 cols = [[7,8],[9,10],[11,12]]
    mat3x2 B{};
    B[0, 0] = 7.0f;  B[0, 1] = 8.0f;
    B[1, 0] = 9.0f;  B[1, 1] = 10.0f;
    B[2, 0] = 11.0f; B[2, 1] = 12.0f;

    // Mat<2,3> * Mat<3,2> -> Mat<2,2>
    mat2 AB = A * B;
    REQUIRE((AB[0, 0]) == 58.0f);
    REQUIRE((AB[0, 1]) == 64.0f);
    REQUIRE((AB[1, 0]) == 139.0f);
    REQUIRE((AB[1, 1]) == 154.0f);

    // Mat<2,3> * Vec<3> -> Vec<2>
    REQUIRE(A * vec3{1.0f, 1.0f, 1.0f} == vec2{6.0f, 15.0f});

    // Vec<2> * Mat<2,3> -> Vec<3>  (row-vector left-multiply, v^T * A)
    REQUIRE(vec2{1.0f, 1.0f} * A == vec3{5.0f, 7.0f, 9.0f});

    // shape-preserving element-wise + and scalar broadcast
    mat2x3 twice = A + A;
    REQUIRE((twice[1, 2]) == 12.0f);
    REQUIRE(A * 2.0f == twice);
    REQUIRE(-A == A * -1.0f);
}

TEST_CASE("non-square matrix multiply folds at compile time", AUTO_TAG) {
    constexpr mat2 AB = [] {
        mat2x3 A{};
        A[0, 0] = 1.0f; A[0, 1] = 2.0f; A[0, 2] = 3.0f;
        A[1, 0] = 4.0f; A[1, 1] = 5.0f; A[1, 2] = 6.0f;
        mat3x2 B{};
        B[0, 0] = 7.0f;  B[0, 1] = 8.0f;
        B[1, 0] = 9.0f;  B[1, 1] = 10.0f;
        B[2, 0] = 11.0f; B[2, 1] = 12.0f;
        return A * B;
    }();
    STATIC_REQUIRE((AB[1, 1]) == 154.0f);
}

TEST_CASE("operators fold at compile time", AUTO_TAG) {
    constexpr vec3 a{1.0f, 2.0f, 3.0f};
    constexpr vec3 sum = a + vec3{4.0f, 4.0f, 4.0f};
    STATIC_REQUIRE(sum == vec3{5.0f, 6.0f, 7.0f});
    STATIC_REQUIRE(a * 2.0f == vec3{2.0f, 4.0f, 6.0f});
    STATIC_REQUIRE(-a == vec3{-1.0f, -2.0f, -3.0f});

    constexpr mat4 m = [] {
        mat4 r{};
        r[0, 0] = 2.0f;
        r[1, 1] = 2.0f;
        r[2, 2] = 2.0f;
        r[3, 3] = 1.0f;
        return r;
    }();
    constexpr vec4 p = m * vec4{1.0f, 2.0f, 3.0f, 1.0f};
    STATIC_REQUIRE(p == vec4{2.0f, 4.0f, 6.0f, 1.0f});
}
