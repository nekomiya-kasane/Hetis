/**
 * @file Quanterion.h
 * @brief Complete quaternion support for the Mashiro renderer.
 * @ingroup Core
 *
 * Header-only, fully `constexpr`. A `quat` is a unit (or general) Hamilton
 * quaternion q = w + x*i + y*j + z*k, stored scalar-LAST as {x, y, z, w} so the
 * layout is bit-compatible with vec4 and with the glTF / Vulkan asset
 * convention (glTF node.rotation = [x, y, z, w]).
 *
 * The free functions live in namespace `Mashiro::Quat` (e.g. `Quat::MakeAxisAngle`,
 * `Quat::Slerp`); the value type stays `Mashiro::quat`. The namespace is also
 * reachable as `Mashiro::Math::Quat` (alias) so all renderer math sits under `Math`.
 *
 * Conventions (match MathUtils.h):
 *   - Right-handed coordinate system, active rotations (rotate the vector, not the frame).
 *   - Column-major matrices; m[row, col] is the element, M * v rotates a column vector.
 *   - Hamilton product (a * b applies b first, then a), NOT the JPL convention.
 *   - Quat::ToMat3/4 of Quat::MakeAxisAngle(axis, a) equals Math::MakeRotateAxis(axis, a).
 *
 * Performance: every entry point is `constexpr`. Scalar transcendental math is
 * reused from ScalarMath.h, which dispatches with `if consteval` (P1938) — at run
 * time it lowers to the hardware `std::` intrinsics (sqrtss, vendor trig), while at
 * compile time it evaluates accurate `constexpr` polynomial kernels, so rotations
 * and transforms fold to constants when their inputs are known.
 *
 * Namespace: Mashiro / Mashiro::Quat (alias Mashiro::Math::Quat)
 */
#pragma once

#include "Mashiro/Math/MathUtils.h"

#include <numbers>

namespace Mashiro {

    /**
     * @brief Unit Hamilton quaternion, scalar-last {x, y, z, w}.
     *
     * Default-constructs to the identity rotation (w = 1). alignas(16) and the
     * gapless x/y/z/w layout make it reinterpret-compatible with vec4 and
     * GPU-uploadable. operator[] addresses x/y/z/w as indices 0..3.
     */
    struct alignas(16) quat {
        float x = 0.0f; ///< Imaginary i component.
        float y = 0.0f; ///< Imaginary j component.
        float z = 0.0f; ///< Imaginary k component.
        float w = 1.0f; ///< Real (scalar) component.

        /**
         * @brief Element access by index (0 = x … 3 = w).
         *
         * Uses deducing-this (P0847); run-time takes the single indexed load,
         * constant evaluation selects the member directly.
         */
        [[nodiscard]] constexpr auto&& operator[](this auto&& self, int i) {
            if !consteval { return (&self.x)[i]; }
            switch (i) {
                case 0: return self.x;
                case 1: return self.y;
                case 2: return self.z;
                case 3: return self.w;
            }
            __builtin_unreachable();
        }

        /// @name Operators (hidden friends — ADL, no namespace pollution)
        /// @{
        // operator* on two quats is the Hamilton product (a then-b composition: a * b
        // applies b first); quat * scalar scales all four components.
        [[nodiscard]] friend constexpr bool operator==(quat, quat) = default;
        [[nodiscard]] friend constexpr quat operator-(quat q) {
            return quat{.x = -q.x, .y = -q.y, .z = -q.z, .w = -q.w};
        }
        [[nodiscard]] friend constexpr quat operator+(quat a, quat b) {
            return quat{.x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z, .w = a.w + b.w};
        }
        [[nodiscard]] friend constexpr quat operator-(quat a, quat b) {
            return quat{.x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z, .w = a.w - b.w};
        }
        [[nodiscard]] friend constexpr quat operator*(quat a, quat b) {
            return quat{
                .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            };
        }
        [[nodiscard]] friend constexpr quat operator*(quat q, float s) {
            return quat{.x = q.x * s, .y = q.y * s, .z = q.z * s, .w = q.w * s};
        }
        [[nodiscard]] friend constexpr quat operator*(float s, quat q) { return q * s; }
        [[nodiscard]] friend constexpr quat operator/(quat q, float s) {
            return quat{.x = q.x / s, .y = q.y / s, .z = q.z / s, .w = q.w / s};
        }
        friend constexpr quat& operator+=(quat& a, quat b) { a = a + b; return a; }
        friend constexpr quat& operator-=(quat& a, quat b) { a = a - b; return a; }
        friend constexpr quat& operator*=(quat& a, quat b) { a = a * b; return a; }
        friend constexpr quat& operator*=(quat& a, float s) { a = a * s; return a; }
        friend constexpr quat& operator/=(quat& a, float s) { a = a / s; return a; }
        /// @}
    };
    static_assert(alignof(quat) == 16);
    static_assert(sizeof(quat) == 4 * sizeof(float),
                  "operator[] addresses x/y/z/w via (&x)[i]; requires gapless, contiguous storage");

    /**
     * @brief ADL InnerProduct for quat (ℝ⁴ dot product of coefficients).
     *
     * This satisfies InnerProductSpace (see Algebra.h) so all generic algorithms
     * (Math::Norm, Math::Normalize, Math::Distance, …) work on quat.
     */
    [[nodiscard]] constexpr float InnerProduct(quat a, quat b) {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    namespace Quat {

        /// @name Constants / construction
        /// @{

        /// @brief The identity quaternion (0, 0, 0, 1).
        [[nodiscard]] constexpr quat Identity() {
            return quat{.x = 0.0f, .y = 0.0f, .z = 0.0f, .w = 1.0f};
        }

        /** @brief Imaginary (vector) part {x, y, z}. */
        [[nodiscard]] constexpr vec3 GetVec(quat q) {
            return {.x = q.x, .y = q.y, .z = q.z};
        }

        /** @brief Quaternion from a normalized axis and angle (radians), active rotation. */
        [[nodiscard]] constexpr quat MakeAxisAngle(vec3 axis, float rad) {
            vec3 a = Math::Normalize(axis);
            float half = rad * 0.5f;
            float s = Math::Sin(half);
            return quat{.x = a.x * s, .y = a.y * s, .z = a.z * s, .w = Math::Cos(half)};
        }

        /// @}

        /// @brief Rotation of @p rad radians about the X axis.
        [[nodiscard]] constexpr quat MakeRotateX(float rad) {
            float h = rad * 0.5f;
            return quat{.x = Math::Sin(h), .y = 0.0f, .z = 0.0f, .w = Math::Cos(h)};
        }
        /// @brief Rotation of @p rad radians about the Y axis.
        [[nodiscard]] constexpr quat MakeRotateY(float rad) {
            float h = rad * 0.5f;
            return quat{.x = 0.0f, .y = Math::Sin(h), .z = 0.0f, .w = Math::Cos(h)};
        }
        /// @brief Rotation of @p rad radians about the Z axis.
        [[nodiscard]] constexpr quat MakeRotateZ(float rad) {
            float h = rad * 0.5f;
            return quat{.x = 0.0f, .y = 0.0f, .z = Math::Sin(h), .w = Math::Cos(h)};
        }

        /// @name Core algebra
        /// @{

        /// @brief Dot product of two quaternions (ℝ⁴ inner product).
        [[nodiscard]] constexpr float Dot(quat a, quat b) { return InnerProduct(a, b); }
        /// @brief Squared norm (sum of squares of all four components).
        [[nodiscard]] constexpr float Norm2Sq(quat q) { return Math::NormSq(q); }
        /// @brief Euclidean norm (L2).
        [[nodiscard]] constexpr float Norm2(quat q) { return Math::Norm(q); }

        /// @brief Component-wise addition.
        [[nodiscard]] constexpr quat Add(quat a, quat b) { return a + b; }
        /// @brief Component-wise subtraction.
        [[nodiscard]] constexpr quat Sub(quat a, quat b) { return a - b; }
        /// @brief Scalar multiplication.
        [[nodiscard]] constexpr quat Scale(quat q, float s) { return q * s; }
        /// @brief Negation.
        [[nodiscard]] constexpr quat Neg(quat q) { return -q; }

        /** @brief Conjugate q* = (-x, -y, -z, w). Inverse of a unit quaternion. */
        [[nodiscard]] constexpr quat Conjugate(quat q) {
            return {.x = -q.x, .y = -q.y, .z = -q.z, .w = q.w};
        }

        /// @brief Normalize to unit length; returns identity if near-zero.
        [[nodiscard]] constexpr quat Normalize(quat q) {
            float len = Norm2(q);
            return (len < 1e-8f) ? Identity() : Scale(q, 1.0f / len);
        }

        /// @}

        /** @brief General inverse q* / |q|^2 (equals the conjugate when unit-length). */
        [[nodiscard]] constexpr quat Inverse(quat q) {
            float n = Norm2Sq(q);
            return (n < 1e-12f) ? Identity() : Scale(Conjugate(q), 1.0f / n);
        }

        /**
         * @brief Hamilton product: composition where a*b applies b first, then a.
         *
         * Stored scalar-last; matches ToMat3(Mul(a, b)) == ToMat3(a) * ToMat3(b).
         */
        [[nodiscard]] constexpr quat Mul(quat a, quat b) { return a * b; }

        /// @name Exponential map / logarithm (unit-quaternion tangent space)
        /// @{

        /**
         * @brief Logarithm of a unit quaternion: the pure quaternion (theta * axis, 0).
         *
         * Maps a rotation to its tangent at the identity; the imaginary part is
         * (half-angle * axis). Inverse of Exp on the unit sphere.
         */
        [[nodiscard]] constexpr quat Log(quat q) {
            vec3 v = GetVec(q);
            float vlen = Math::Norm2(v);
            if (vlen < 1e-8f) {
                return quat{.x = 0.0f, .y = 0.0f, .z = 0.0f, .w = 0.0f};
            }
            float theta = Math::Atan2(vlen, q.w);
            float k = theta / vlen;
            return quat{.x = v.x * k, .y = v.y * k, .z = v.z * k, .w = 0.0f};
        }

        /**
         * @brief Exponential of a pure quaternion (w ignored): a unit quaternion.
         *
         * exp((v, 0)) = (sin|v| * v/|v|, cos|v|). Inverse of Log on the unit sphere.
         */
        [[nodiscard]] constexpr quat Exp(quat q) {
            vec3 v = GetVec(q);
            float theta = Math::Norm2(v);
            if (theta < 1e-8f) {
                return Identity();
            }
            auto [s, c] = Math::SinCos(theta);
            float k = s / theta;
            return quat{.x = v.x * k, .y = v.y * k, .z = v.z * k, .w = c};
        }

        /** @brief Unit quaternion raised to a real power: exp(t * log(q)). */
        [[nodiscard]] constexpr quat Pow(quat q, float t) { return Exp(t * Log(q)); }

        /// @}

        /// @name Apply to vectors
        /// @{

        /**
         * @brief Rotate a vec3 by a unit quaternion.
         *
         * Uses the branch-free identity v' = v + 2w(u x v) + 2(u x (u x v)) with
         * u = (x, y, z): two cross products, far cheaper than building a matrix.
         */
        [[nodiscard]] constexpr vec3 Rotate(quat q, vec3 v) {
            vec3 u = GetVec(q);
            vec3 t = 2.0f * Math::Cross(u, v);
            return v + q.w * t + Math::Cross(u, t);
        }

        /** @brief Rotate the xyz of a vec4, preserving w (point/direction-agnostic). */
        [[nodiscard]] constexpr vec4 Rotate(quat q, vec4 v) {
            vec3 r = Rotate(q, vec3{.x = v.x, .y = v.y, .z = v.z});
            return {.x = r.x, .y = r.y, .z = r.z, .w = v.w};
        }

        /// @brief Local right-axis (+X rotated by @p q).
        [[nodiscard]] constexpr vec3 GetRight(quat q) { return Rotate(q, vec3{1.0f, 0.0f, 0.0f}); }
        /// @brief Local up-axis (+Y rotated by @p q).
        [[nodiscard]] constexpr vec3 GetUp(quat q) { return Rotate(q, vec3{0.0f, 1.0f, 0.0f}); }
        /// @brief Local forward-axis (+Z rotated by @p q).
        [[nodiscard]] constexpr vec3 GetForward(quat q) { return Rotate(q, vec3{0.0f, 0.0f, 1.0f}); }

        /// @}

        /// @name Quaternion ↔ matrix conversion
        /// @{

        /// @brief Convert to a 3×3 rotation matrix (column-major, matches MakeRotateAxis).
        [[nodiscard]] constexpr mat3 ToMat3(quat q) {
            float x = q.x, y = q.y, z = q.z, w = q.w;
            float xx = x * x, yy = y * y, zz = z * z;
            float xy = x * y, xz = x * z, yz = y * z;
            float wx = w * x, wy = w * y, wz = w * z;
            mat3 m{};
            // Column-wise assignment: each column is the image of a basis vector.
            // (m[row, col] is now a constant expression for every index, but column
            // stores remain the natural, SIMD-friendly way to lay down a rotation.)
            m[0] = vec3{.x = 1.0f - 2.0f * (yy + zz), .y = 2.0f * (xy + wz), .z = 2.0f * (xz - wy)};
            m[1] = vec3{.x = 2.0f * (xy - wz), .y = 1.0f - 2.0f * (xx + zz), .z = 2.0f * (yz + wx)};
            m[2] = vec3{.x = 2.0f * (xz + wy), .y = 2.0f * (yz - wx), .z = 1.0f - 2.0f * (xx + yy)};
            return m;
        }

        /// @brief Convert to a 4×4 rotation matrix (identity 4th row/col).
        [[nodiscard]] constexpr mat4 ToMat4(quat q) {
            mat3 r = ToMat3(q);
            mat4 m{};
            m[0] = vec4{.x = r[0].x, .y = r[0].y, .z = r[0].z, .w = 0.0f};
            m[1] = vec4{.x = r[1].x, .y = r[1].y, .z = r[1].z, .w = 0.0f};
            m[2] = vec4{.x = r[2].x, .y = r[2].y, .z = r[2].z, .w = 0.0f};
            m[3] = vec4{.x = 0.0f, .y = 0.0f, .z = 0.0f, .w = 1.0f};
            return m;
        }

        /**
         * @brief Build a column-major TRS transform: translate * rotate(quat) * scale.
         *
         * The most common "quaternion acting on a matrix" in a renderer: the rotation
         * block is the quaternion matrix scaled per-axis, with translation in column 3.
         */
        [[nodiscard]] constexpr mat4 MakeTransform(vec3 translation, quat rotation, vec3 scale) {
            mat3 r = ToMat3(rotation);
            mat4 m{};
            m[0] = vec4{.x = r[0].x * scale.x, .y = r[0].y * scale.x, .z = r[0].z * scale.x, .w = 0.0f};
            m[1] = vec4{.x = r[1].x * scale.y, .y = r[1].y * scale.y, .z = r[1].z * scale.y, .w = 0.0f};
            m[2] = vec4{.x = r[2].x * scale.z, .y = r[2].y * scale.z, .z = r[2].z * scale.z, .w = 0.0f};
            m[3] = vec4{.x = translation.x, .y = translation.y, .z = translation.z, .w = 1.0f};
            return m;
        }

        /** @brief Compact affine TRS: translate * rotate(quat) * scale → mat3x4. */
        [[nodiscard]] constexpr affine3 MakeTransformAffine(vec3 translation, quat rotation, vec3 scale) {
            mat3 r = ToMat3(rotation);
            affine3 m{};
            m[0] = vec3{.x = r[0].x * scale.x, .y = r[0].y * scale.x, .z = r[0].z * scale.x};
            m[1] = vec3{.x = r[1].x * scale.y, .y = r[1].y * scale.y, .z = r[1].z * scale.y};
            m[2] = vec3{.x = r[2].x * scale.z, .y = r[2].y * scale.z, .z = r[2].z * scale.z};
            m[3] = translation;
            return m;
        }

        /** @brief Apply the rotation to a 3x3 matrix in parent space: returns ToMat3(q) * m. */
        [[nodiscard]] constexpr mat3 RotateMat3(quat q, const mat3& m) { return ToMat3(q) * m; }

        /**
         * @brief Apply the rotation to a 4x4 transform: returns ToMat4(q) * m.
         *
         * Left-multiplies, so the whole transform (basis + translation) is rotated
         * about the origin in its parent/world space. The pure rotation never touches
         * m's homogeneous row, so it is copied through verbatim.
         */
        [[nodiscard]] constexpr mat4 RotateMat4(quat q, const mat4& m) { return ToMat4(q) * m; }

        /**
         * @brief Extract rotation quaternion from a 3×3 matrix (Shepperd's method).
         *
         * Branches on the largest diagonal element for numerical stability.
         */
        [[nodiscard]] constexpr quat MakeFromMat3(const mat3& m) {
            // Read columns once via the constexpr-safe single subscript + member access.
            vec3 c0 = m[0], c1 = m[1], c2 = m[2];
            float m00 = c0.x, m10 = c0.y, m20 = c0.z;
            float m01 = c1.x, m11 = c1.y, m21 = c1.z;
            float m02 = c2.x, m12 = c2.y, m22 = c2.z;
            float trace = m00 + m11 + m22;
            quat q{};
            if (trace > 0.0f) {
                float s = Math::Sqrt(trace + 1.0f) * 2.0f; // s = 4w
                q.w = 0.25f * s;
                q.x = (m21 - m12) / s;
                q.y = (m02 - m20) / s;
                q.z = (m10 - m01) / s;
            } else if (m00 > m11 && m00 > m22) {
                float s = Math::Sqrt(1.0f + m00 - m11 - m22) * 2.0f; // s = 4x
                q.w = (m21 - m12) / s;
                q.x = 0.25f * s;
                q.y = (m01 + m10) / s;
                q.z = (m02 + m20) / s;
            } else if (m11 > m22) {
                float s = Math::Sqrt(1.0f + m11 - m00 - m22) * 2.0f; // s = 4y
                q.w = (m02 - m20) / s;
                q.x = (m01 + m10) / s;
                q.y = 0.25f * s;
                q.z = (m12 + m21) / s;
            } else {
                float s = Math::Sqrt(1.0f + m22 - m00 - m11) * 2.0f; // s = 4z
                q.w = (m10 - m01) / s;
                q.x = (m02 + m20) / s;
                q.y = (m12 + m21) / s;
                q.z = 0.25f * s;
            }
            return Normalize(q);
        }

        /** @brief Extract the rotation from the upper-left 3x3 block of a 4x4 transform. */
        [[nodiscard]] constexpr quat MakeFromMat4(const mat4& m) {
            vec4 c0 = m[0], c1 = m[1], c2 = m[2];
            mat3 r{};
            r[0] = vec3{.x = c0.x, .y = c0.y, .z = c0.z};
            r[1] = vec3{.x = c1.x, .y = c1.y, .z = c1.z};
            r[2] = vec3{.x = c2.x, .y = c2.y, .z = c2.z};
            return MakeFromMat3(r);
        }

        /// @}

        /// @name Axis / angle / Euler extraction
        /// @{

        /** @brief Rotation angle in radians, in [0, pi]. */
        [[nodiscard]] constexpr float GetAngle(quat q) {
            return 2.0f * Math::Atan2(Math::Norm2(GetVec(q)), Math::Abs(q.w));
        }

        /** @brief Rotation axis (normalized); returns +X for the identity (angle 0). */
        [[nodiscard]] constexpr vec3 GetAxis(quat q) {
            vec3 u   = GetVec(q);
            float  len = Math::Norm2(u);
            return (len < 1e-8f) ? vec3{.x = 1.0f, .y = 0.0f, .z = 0.0f} : u * (1.0f / len);
        }

        /**
         * @brief Tait-Bryan Euler angles from a quaternion, in radians.
         *
         * Order matches MakeFromEuler: extrinsic X(pitch)->Y(yaw)->Z(roll), i.e.
         * q = Rz(roll) * Ry(yaw) * Rx(pitch). Returns {pitch=x, yaw=y, roll=z};
         * yaw is clamped to [-pi/2, pi/2] and gimbal-locked poles fold roll into yaw.
         */
        [[nodiscard]] constexpr vec3 ToEuler(quat q) {
            vec3 e{};
            float sinp = 2.0f * (q.w * q.y - q.z * q.x);
            if (Math::Abs(sinp) >= 1.0f) {
                e.y = Math::Copysign(std::numbers::pi_v<float> * 0.5f, sinp); // gimbal lock
                e.x = 0.0f;
                e.z = Math::Atan2(2.0f * (q.x * q.y + q.w * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
                return e;
            }
            e.x = Math::Atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            e.y = Math::Asin(sinp);
            e.z = Math::Atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
            return e;
        }

        /** @brief Build from Tait-Bryan angles: q = Rz(roll) * Ry(yaw) * Rx(pitch). */
        [[nodiscard]] constexpr quat MakeFromEuler(float pitch, float yaw, float roll) {
            float hx = pitch * 0.5f, hy = yaw * 0.5f, hz = roll * 0.5f;
            float cx = Math::Cos(hx), sx = Math::Sin(hx);
            float cy = Math::Cos(hy), sy = Math::Sin(hy);
            float cz = Math::Cos(hz), sz = Math::Sin(hz);
            return quat{
                .x = sx * cy * cz - cx * sy * sz,
                .y = cx * sy * cz + sx * cy * sz,
                .z = cx * cy * sz - sx * sy * cz,
                .w = cx * cy * cz + sx * sy * sz,
            };
        }

        /// @}

        /// @name High-level builders
        /// @{

        /** @brief Shortest-arc rotation that turns unit vector `from` into unit vector `to`. */
        [[nodiscard]] constexpr quat MakeFromTo(vec3 from, vec3 to) {
            vec3 f = Math::Normalize(from);
            vec3 t = Math::Normalize(to);
            float d = Math::Dot(f, t);
            if (d >= 1.0f - 1e-6f) {
                return Identity(); // already aligned
            }
            if (d <= -1.0f + 1e-6f) {
                // Opposite: rotate pi about any axis orthogonal to f.
                vec3 axis = Math::Cross(vec3{1.0f, 0.0f, 0.0f}, f);
                if (Math::Dot(axis, axis) < 1e-6f) {
                    axis = Math::Cross(vec3{0.0f, 1.0f, 0.0f}, f);
                }
                return MakeAxisAngle(Math::Normalize(axis), std::numbers::pi_v<float>);
            }
            vec3 c = Math::Cross(f, t);
            quat q{.x = c.x, .y = c.y, .z = c.z, .w = 1.0f + d};
            return Normalize(q);
        }

        /**
         * @brief Orientation whose forward (+Z) aligns with `forward` and up tends to `up`.
         *
         * Builds an orthonormal right-handed basis (right, up, forward) and converts it.
         */
        [[nodiscard]] constexpr quat MakeLookRotation(vec3 forward, vec3 up) {
            vec3 f = Math::Normalize(forward);
            vec3 r = Math::Normalize(Math::Cross(up, f));
            vec3 u = Math::Cross(f, r);
            mat3 m{};
            m[0] = r; // column 0 = right
            m[1] = u; // column 1 = up
            m[2] = f; // column 2 = forward
            return MakeFromMat3(m);
        }

        /// @}

        /// @name Interpolation
        /// @{

        /** @brief Component-wise linear interpolation (does NOT renormalize). */
        [[nodiscard]] constexpr quat Lerp(quat a, quat b, float t) { return a + (b - a) * t; }

        /** @brief Normalized linear interpolation (cheap, constant-speed-approximate). */
        [[nodiscard]] constexpr quat Nlerp(quat a, quat b, float t) {
            float d = Dot(a, b);
            quat bb = (d < 0.0f) ? Neg(b) : b; // shortest path
            return Normalize(Add(Scale(a, 1.0f - t), Scale(bb, t)));
        }

        /** @brief Spherical linear interpolation; falls back to nlerp for tiny angles. */
        [[nodiscard]] constexpr quat Slerp(quat a, quat b, float t) {
            float d = Dot(a, b);
            quat bb = b;
            if (d < 0.0f) { // shortest path
                bb = Neg(b);
                d = -d;
            }
            if (d > 1.0f - 1e-6f) {
                return Nlerp(a, bb, t); // nearly parallel: avoid division by ~0
            }
            float theta = Math::Acos(d);
            float sinTheta = Math::Sin(theta);
            float wa = Math::Sin((1.0f - t) * theta) / sinTheta;
            float wb = Math::Sin(t * theta) / sinTheta;
            return Add(Scale(a, wa), Scale(bb, wb));
        }

        /**
         * @brief Rotate `from` toward `to`, advancing by at most maxRadians.
         *
         * Returns `to` once within reach. maxRadians is measured in rotation-angle
         * space (the angle of the relative rotation), not quaternion arc length.
         */
        [[nodiscard]] constexpr quat RotateTowards(quat from, quat to, float maxRadians) {
            float d  = Dot(Normalize(from), Normalize(to));
            float ad = Math::Abs(d);
            if (ad > 1.0f - 1e-6f) {
                return to; // already aligned
            }
            float angle    = 2.0f * Math::Acos(ad); // full angle between the rotations
            float fraction = Math::Min(1.0f, maxRadians / angle);
            return Slerp(from, to, fraction);
        }

        /**
         * @brief Inner control quaternion for Squad from three consecutive keys.
         *
         * a_i = q_i * exp(-(log(q_i^-1 q_{i+1}) + log(q_i^-1 q_{i-1})) / 4); feed this
         * (and the next key's tangent) into Squad as the control points.
         */
        [[nodiscard]] constexpr quat SquadTangent(quat prev, quat cur, quat next) {
            quat invCur  = Inverse(cur);
            quat tangent = Log(invCur * next) + Log(invCur * prev);
            return cur * Exp(tangent * -0.25f);
        }

        /**
         * @brief Spherical cubic interpolation (Shoemake's squad) over [q0, q1] with
         * control quaternions a and b (typically from SquadTangent).
         */
        [[nodiscard]] constexpr quat Squad(quat q0, quat a, quat b, quat q1, float t) {
            quat l = Slerp(q0, q1, t);
            quat r = Slerp(a, b, t);
            return Slerp(l, r, 2.0f * t * (1.0f - t));
        }

        /// @}

    } // namespace Quat

    /// Expose the quaternion API under `Mashiro::Math::Quat` as well.
    namespace Math {
        namespace Quat = ::Mashiro::Quat;
    }

} // namespace Mashiro
