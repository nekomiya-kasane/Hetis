/**
 * @file Types.h
 * @brief GPU-compatible vector, matrix, and geometry type aliases.
 *
 * Single public include for all short-name aliases (`vec3`, `mat4`, `AABB`, …).
 * Template machinery lives in Vec.h and Mat.h; this file provides the GLSL/GLM
 * aliases and enforces GPU layout invariants via `static_assert`.
 *
 * **Naming convention** (GLSL/GLM style):
 * | Prefix / Suffix | Meaning                             |
 * |-----------------|-------------------------------------|
 * | *(none)*        | `float` (e.g. `vec3`, `mat4`)       |
 * | `d`             | `double` (e.g. `dvec3`, `dmat4`)    |
 * | `i`             | `int32_t` (e.g. `ivec3`)            |
 * | `u`             | `uint32_t` (e.g. `uvec3`)           |
 * | trailing `b`    | packed / unpadded (e.g. `vec3b`)    |
 *
 * @note Unlike GLSL where `matCxR` = C columns × R rows, our rectangular
 *       `matRxC` = R rows × C columns (matches `Mat<T,R,C>` param order).
 *
 * **Operator cheat-sheet** (generated via reflection in Vec.h / Mat.h):
 * | Expression        | Semantics                                   |
 * |-------------------|---------------------------------------------|
 * | `v ± v`, `v * v`  | Component-wise (`*` = Hadamard product)     |
 * | `v * s`, `s * v`   | Scalar broadcast (only `*` and `/`)         |
 * | `m * m`, `m * v`   | Linear-algebra multiply (column-major)      |
 * | `-v`               | Negation (signed / float only)              |
 * | `v == v`           | Exact component-wise equality               |
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Math/Mat.h"

#include <cstdint>

namespace Mashiro {

    /// @name Float vector aliases
    /// @{

    using vec2  = Vec<float, 2>;                  ///< 2D float vector (8 B, align 8).
    using vec3  = Vec<float, 3>;                  ///< 3D float vector, std430-padded (16 B, align 16).
    using vec3b = Vec<float, 3, AlignTag::Packed>;///< 3D float vector, packed (12 B, align 4).
    using vec4  = Vec<float, 4>;                  ///< 4D float vector (16 B, align 16).

    static_assert(sizeof(vec2) == 8);
    static_assert(alignof(vec2) == 8);
    static_assert(sizeof(vec3b) == 12);
    static_assert(alignof(vec3b) == 4);
    static_assert(sizeof(vec3) == 16);
    static_assert(alignof(vec3) == 16);
    static_assert(sizeof(vec4) == 16);
    static_assert(alignof(vec4) == 16);

    /// @}

    /// @name Double vector aliases
    /// @{

    using dvec2  = Vec<double, 2>;                  ///< 2D double vector (16 B, align 16).
    using dvec3  = Vec<double, 3>;                  ///< 3D double vector, std430-padded (32 B, align 32).
    using dvec3b = Vec<double, 3, AlignTag::Packed>;///< 3D double vector, packed (24 B, align 8).
    using dvec4  = Vec<double, 4>;                  ///< 4D double vector (32 B, align 32).

    static_assert(sizeof(dvec2) == 16);
    static_assert(alignof(dvec2) == 16);
    static_assert(sizeof(dvec3b) == 24);
    static_assert(alignof(dvec3b) == 8);
    static_assert(sizeof(dvec3) == 32);
    static_assert(alignof(dvec3) == 32);
    static_assert(sizeof(dvec4) == 32);
    static_assert(alignof(dvec4) == 32);

    /// @}

    /// @name Signed integer vector aliases
    /// @{

    using ivec2 = Vec<int32_t, 2>; ///< 2D signed-int vector.
    using ivec3 = Vec<int32_t, 3>; ///< 3D signed-int vector (std430-padded).
    using ivec4 = Vec<int32_t, 4>; ///< 4D signed-int vector.

    static_assert(sizeof(ivec2) == 8);
    static_assert(alignof(ivec2) == 8);
    static_assert(sizeof(ivec3) == 16);
    static_assert(alignof(ivec3) == 16);
    static_assert(sizeof(ivec4) == 16);
    static_assert(alignof(ivec4) == 16);

    /// @}

    /// @name Unsigned integer vector aliases
    /// @{

    using uvec2  = Vec<uint32_t, 2>;                  ///< 2D unsigned-int vector.
    using uvec3  = Vec<uint32_t, 3>;                  ///< 3D unsigned-int vector (std430-padded).
    using uvec3b = Vec<uint32_t, 3, AlignTag::Packed>;///< 3D unsigned-int vector, packed.
    using uvec4  = Vec<uint32_t, 4>;                  ///< 4D unsigned-int vector.

    static_assert(sizeof(uvec2) == 8);
    static_assert(alignof(uvec2) == 8);
    static_assert(sizeof(uvec3b) == 12);
    static_assert(alignof(uvec3b) == 4);
    static_assert(sizeof(uvec3) == 16);
    static_assert(alignof(uvec3) == 16);
    static_assert(sizeof(uvec4) == 16);
    static_assert(alignof(uvec4) == 16);

    /// @}

    /// @name Float matrix aliases
    /// @{

    using mat2 = Mat<float, 2>; ///< 2×2 float matrix.
    using mat3 = Mat<float, 3>; ///< 3×3 float matrix.
    using mat4 = Mat<float, 4>; ///< 4×4 float matrix.

    static_assert(sizeof(mat2) == 16);
    static_assert(alignof(mat2) == 8);
    static_assert(sizeof(mat3) == 48);
    static_assert(alignof(mat3) == 16);
    static_assert(sizeof(mat4) == 64);
    static_assert(alignof(mat4) == 16);

    /**
     * @name Rectangular float matrices
     *
     * `matRxC = Mat<float, R, C>` — R rows, C columns, stored column-major.
     * (Opposite of GLSL's `matCxR`; matches `Mat<T,R,C>` template order.)
     * @{
     */
    using mat2x3 = Mat<float, 2, 3>; ///< 2 rows × 3 cols.
    using mat2x4 = Mat<float, 2, 4>; ///< 2 rows × 4 cols.
    using mat3x2 = Mat<float, 3, 2>; ///< 3 rows × 2 cols.
    using mat3x4 = Mat<float, 3, 4>; ///< 3 rows × 4 cols.
    using mat4x2 = Mat<float, 4, 2>; ///< 4 rows × 2 cols.
    using mat4x3 = Mat<float, 4, 3>; ///< 4 rows × 3 cols.

    /** @brief Compact 2D affine transform: 2×2 rotation/scale + translation column (2 rows × 3 cols). */
    using affine2 = mat2x3;
    /** @brief Compact 3D affine transform: 3×3 rotation/scale + translation column (3 rows × 4 cols). */
    using affine3 = mat3x4;

    static_assert(sizeof(mat2x3) == 24);
    static_assert(alignof(mat2x3) == 8);
    static_assert(sizeof(mat2x4) == 32);
    static_assert(alignof(mat2x4) == 8);
    static_assert(sizeof(mat3x2) == 32);
    static_assert(alignof(mat3x2) == 16);
    static_assert(sizeof(mat3x4) == 64);
    static_assert(alignof(mat3x4) == 16);
    static_assert(sizeof(mat4x2) == 32);
    static_assert(alignof(mat4x2) == 16);
    static_assert(sizeof(mat4x3) == 48);
    static_assert(alignof(mat4x3) == 16);

    /// @}

    /// @name Double matrix aliases
    /// @{

    using dmat2 = Mat<double, 2>; ///< 2×2 double matrix.
    using dmat3 = Mat<double, 3>; ///< 3×3 double matrix.
    using dmat4 = Mat<double, 4>; ///< 4×4 double matrix.

    static_assert(sizeof(dmat2) == 32);
    static_assert(alignof(dmat2) == 16);
    static_assert(sizeof(dmat3) == 96);
    static_assert(alignof(dmat3) == 32);
    static_assert(sizeof(dmat4) == 128);
    static_assert(alignof(dmat4) == 32);

    /// @}

    /// @name Geometry types
    /// @{

    /** @brief Axis-aligned bounding box (32 B, align 16). */
    struct alignas(16) AABB {
        vec3 min = {}; ///< Corner with smallest coordinates.
        vec3 max = {}; ///< Corner with largest coordinates.
    };
    static_assert(sizeof(AABB) == 32);
    static_assert(alignof(AABB) == 16);

    /**
     * @brief Bounding sphere packed into 16 bytes.
     *
     * A 3-float center (vec3b, no padding) plus the radius fill exactly 16 bytes
     * while maintaining alignas(16).
     */
    struct alignas(16) BoundingSphere {
        vec3b center = {}; ///< Sphere centre (packed, no padding).
        float radius = {}; ///< Sphere radius.
    };
    static_assert(sizeof(BoundingSphere) == 16);
    static_assert(alignof(BoundingSphere) == 16);

    /** @brief Six frustum clipping planes: left, right, bottom, top, near, far. */
    struct alignas(16) FrustumPlanes {
        vec4 planes[6] = {}; ///< `dot(normal, point) + w >= 0` → inside.
    };
    static_assert(sizeof(FrustumPlanes) == 96);
    static_assert(alignof(FrustumPlanes) == 16);

    /** @brief Ray defined by an origin point and a direction vector. */
    struct alignas(16) Ray {
        vec3 origin    = {}; ///< Ray start point.
        vec3 direction = {}; ///< Ray direction (not necessarily unit-length).
    };
    static_assert(sizeof(Ray) == 32);
    static_assert(alignof(Ray) == 16);

    /** @brief Plane defined by normal + distance from origin.
     *
     * A 3-float normal (vec3b, no padding) plus the distance fill exactly 16 bytes
     * while maintaining alignas(16).
     */
    struct alignas(16) Plane {
        vec3b normal = {}; ///< Unit outward normal.
        float dist   = {}; ///< Signed distance from origin along @c normal.
    };

    /// @}
    static_assert(sizeof(Plane) == 16);
    static_assert(alignof(Plane) == 16);

}  // namespace Mashiro
