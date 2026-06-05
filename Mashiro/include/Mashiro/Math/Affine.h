/**
 * @file Affine.h
 * @brief First-class affine transform type and the transform builders that produce it.
 *
 * An affine map `x ↦ A·x + t` is the workhorse of a renderer's scene graph: rigid
 * motions, scales, and their compositions all live here, distinct from the *projective*
 * transforms (perspective / orthographic) that genuinely need the full homogeneous
 * matrix and stay in MatOps.h.
 *
 * `Affine<T, N, Storage>` is matrix-backed: it wraps a single `Mat`, and the `Storage`
 * tag selects whether the implicit bottom `[0 … 0 1]` homogeneous row is materialised:
 *
 * | Storage   | Backing matrix         | 3D example | Bottom row     |
 * |-----------|------------------------|------------|----------------|
 * | `Compact` | `Mat<T, N, N+1>`       | `mat3x4`   | implicit       |
 * | `Full`    | `Mat<T, N+1, N+1>`     | `mat4`     | stored `[0 0 0 1]` |
 *
 * Columns `0 … N-1` hold the linear (rotation/scale/shear) basis; column `N` holds the
 * translation. `Compact` is the default — it is the GPU-upload-friendly, cache-dense
 * representation; `Full` is available when a genuine `Mat<T, N+1>` layout is required
 * (e.g. to splice into a projective product without re-expanding).
 *
 * The transform builders (`MakeTranslation`, `MakeScale`, `MakeRotate*`, `MakeLookAt`)
 * are a single templated family parameterised on `Storage` (default `Compact`); the
 * spatial dimension is deduced from the vector argument where possible. To obtain a
 * full homogeneous `Mat<T, N+1>` from any affine, call `.ToMat()`.
 *
 * Every operation is `constexpr` and expanded with P1306 `template for` over a static
 * index sequence, fully unrolled by the compiler — zero runtime overhead. Transcendental
 * math routes through ScalarMath.h, so transforms fold to constants when inputs are known.
 *
 * Convention: column-major, right-handed, active transforms (`a * p` maps the point `p`).
 *
 * Namespace: `Mashiro` (the `Affine` type + operators); `Mashiro::Math` (builders, `Inverse`).
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Core/Meta.h"
#include "Mashiro/Math/MatOps.h"
#include "Mashiro/Math/Types.h"

namespace Mashiro {

    /// @brief Storage policy for @ref Affine: store the homogeneous row, or leave it implicit.
    enum class AffineStorage {
        Compact, ///< Backing `Mat<T, N, N+1>`; bottom `[0 … 0 1]` row is implicit.
        Full,    ///< Backing `Mat<T, N+1, N+1>`; bottom row is materialised.
    };

    /**
     * @brief Affine transform `x ↦ A·x + t` in N spatial dimensions, matrix-backed.
     *
     * @tparam T Floating-point scalar.
     * @tparam N Spatial dimension (2 or 3).
     * @tparam S Storage policy (default `Compact`).
     *
     * Columns `0 … N-1` of @ref m are the linear basis; column `N` is the translation.
     * The type is an aggregate: `Affine<T,N,S>{}` is the zero map, `Identity()` the unit.
     */
    template<std::floating_point T, int N, AffineStorage S = AffineStorage::Compact>
        requires(N == 2 || N == 3)
    struct Affine {
        using ScalarType = T;                       ///< Scalar field.
        static constexpr int Dim = N;               ///< Spatial dimension.
        static constexpr AffineStorage Storage = S; ///< Storage policy.
        static constexpr int Rows = (S == AffineStorage::Compact) ? N : N + 1; ///< Backing rows.
        static constexpr int Cols = N + 1; ///< Backing columns (linear + translation).

        using MatrixType = Mat<T, Rows, Cols>; ///< Backing matrix type.
        using LinearType = Mat<T, N>;          ///< N×N linear-part type.
        using FullType = Mat<T, N + 1>;        ///< Full homogeneous matrix type.
        using PointType = Vec<T, N>;           ///< Point / translation vector type.

        MatrixType m{}; ///< Backing storage: columns 0…N-1 = linear basis, column N = translation.

        /// @brief The identity transform (unit linear part, zero translation).
        [[nodiscard]] static constexpr Affine Identity() {
            Affine a;
            template for (constexpr int i : Iota<N>) {
                a.m[i, i] = T(1);
            }
            if constexpr (S == AffineStorage::Full) {
                a.m[N, N] = T(1);
            }
            return a;
        }

        /// @brief The N×N linear part (rotation / scale / shear).
        [[nodiscard]] constexpr LinearType Linear() const {
            LinearType l{};
            template for (constexpr int col : Iota<N>) {
                template for (constexpr int row : Iota<N>) {
                    l[row, col] = m[row, col];
                }
            }
            return l;
        }

        /// @brief The translation vector (column N).
        [[nodiscard]] constexpr PointType Translation() const {
            PointType t{};
            template for (constexpr int row : Iota<N>) {
                t[row] = m[row, N];
            }
            return t;
        }

        /// @brief As matrix
        [[nodiscard]] constexpr const MatrixType& AsMat() const { return m; }

        /// @brief As matrix
        [[nodiscard]] constexpr MatrixType& AsMat() { return m; }

        /// @brief Full homogeneous matrix `Mat<T, N+1>` with the `[0 … 0 1]` row materialised.
        [[nodiscard]] constexpr FullType ToMat() const {
            if constexpr (S == AffineStorage::Full) {
                return m;
            } else {
                FullType r{};
                template for (constexpr int col : Iota<N + 1>) {
                    template for (constexpr int row : Iota<N>) {
                        r[row, col] = m[row, col];
                    }
                }
                r[N, N] = T(1);
                return r;
            }
        }

        /// @brief Reinterpret this transform under a different storage policy.
        template<AffineStorage S2>
        [[nodiscard]] constexpr Affine<T, N, S2> To() const {
            Affine<T, N, S2> r = Affine<T, N, S2>::Identity();
            template for (constexpr int col : Iota<N + 1>) {
                template for (constexpr int row : Iota<N>) {
                    r.m[row, col] = m[row, col];
                }
            }
            return r;
        }

        /// @brief Map a point: `A·p + t`.
        [[nodiscard]] constexpr PointType TransformPoint(PointType p) const {
            PointType r{};
            template for (constexpr int row : Iota<N>) {
                T sum = m[row, N];
                template for (constexpr int k : Iota<N>) {
                    sum += m[row, k] * p[k];
                }
                r[row] = sum;
            }
            return r;
        }

        /// @brief Map a direction: `A·v` (translation ignored).
        [[nodiscard]] constexpr PointType TransformVector(PointType v) const {
            PointType r{};
            template for (constexpr int row : Iota<N>) {
                T sum{};
                template for (constexpr int k : Iota<N>) {
                    sum += m[row, k] * v[k];
                }
                r[row] = sum;
            }
            return r;
        }

        [[nodiscard]] friend constexpr bool operator==(const Affine&, const Affine&) = default;
    };

    /// @brief Any instantiation of @ref Affine.
    template<typename A>
    concept AffineTransform =
        []<std::floating_point T, int N, AffineStorage S>(std::type_identity<Affine<T, N, S>>) {
            return true;
        }(std::type_identity<std::remove_cvref_t<A>>{});

    /// @name Affine operators (namespace-scope, found via ADL)
    /// @{

    /** @brief Compose two transforms: `(a * b)(x) = a(b(x))`. */
    template<std::floating_point T, int N, AffineStorage S>
    [[nodiscard]] constexpr Affine<T, N, S> operator*(const Affine<T, N, S>& a,
                                                      const Affine<T, N, S>& b) {
        Affine<T, N, S> r = Affine<T, N, S>::Identity();
        // Linear block: r.L = a.L · b.L
        template for (constexpr int col : Iota<N>) {
            template for (constexpr int row : Iota<N>) {
                T sum{};
                template for (constexpr int k : Iota<N>) {
                    sum += a.m[row, k] * b.m[k, col];
                }
                r.m[row, col] = sum;
            }
        }
        // Translation: a applied to b's translation, as a point.
        template for (constexpr int row : Iota<N>) {
            T sum = a.m[row, N];
            template for (constexpr int k : Iota<N>) {
                sum += a.m[row, k] * b.m[k, N];
            }
            r.m[row, N] = sum;
        }
        return r;
    }

    /** @brief Apply a transform to a point (implicit homogeneous `w = 1`). */
    template<std::floating_point T, int N, AffineStorage S>
    [[nodiscard]] constexpr Vec<T, N> operator*(const Affine<T, N, S>& a, Vec<T, N> p) {
        return a.TransformPoint(p);
    }

    /// @}

    /// @name Affine type aliases
    /// @{

    using affine2 = Affine<float, 2>; ///< Compact 2D affine (backing `mat2x3`).
    using affine3 = Affine<float, 3>; ///< Compact 3D affine (backing `mat3x4`).

    /// @}

    namespace Math {

        /// @name Affine inverse
        /// @{

        /** @brief Inverse of an affine transform: `[A⁻¹ | −A⁻¹·t]`. Assumes the linear part is
         * invertible. */
        template<std::floating_point T, int N, AffineStorage S>
        [[nodiscard]] constexpr Affine<T, N, S> Inverse(const Affine<T, N, S>& a) {
            Mat<T, N> li = Inverse(a.Linear());
            Affine<T, N, S> r = Affine<T, N, S>::Identity();
            template for (constexpr int col : Iota<N>) {
                template for (constexpr int row : Iota<N>) {
                    r.m[row, col] = li[row, col];
                }
            }
            template for (constexpr int row : Iota<N>) {
                T sum{};
                template for (constexpr int k : Iota<N>) {
                    sum += li[row, k] * a.m[k, N];
                }
                r.m[row, N] = -sum;
            }
            return r;
        }

        /// @}

        /// @name Affine transform builders
        ///
        /// One templated family, parameterised on @ref AffineStorage (default `Compact`).
        /// The spatial dimension is deduced from the vector argument where present;
        /// rotation builders are named by their nature (2D angle vs 3D axis).
        /// @{

        /** @brief Affine translation `[I | t]`; dimension deduced from `t`. */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T, int N>
            requires(N == 2 || N == 3)
        [[nodiscard]] constexpr Affine<T, N, S> MakeTranslation(Vec<T, N> t) {
            Affine<T, N, S> a = Affine<T, N, S>::Identity();
            template for (constexpr int i : Iota<N>) {
                a.m[i, N] = t[i];
            }
            return a;
        }

        /** @brief Affine non-uniform scale `[diag(s) | 0]`; dimension deduced from `s`. */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T, int N>
            requires(N == 2 || N == 3)
        [[nodiscard]] constexpr Affine<T, N, S> MakeScale(Vec<T, N> s) {
            Affine<T, N, S> a = Affine<T, N, S>::Identity();
            template for (constexpr int i : Iota<N>) {
                a.m[i, i] = s[i];
            }
            return a;
        }

        /** @brief 2D affine rotation by @p rad radians (counter-clockwise). */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T = float>
        [[nodiscard]] constexpr Affine<T, 2, S> MakeRotate2D(T rad) {
            auto [s, c] = SinCos(rad);
            Affine<T, 2, S> a = Affine<T, 2, S>::Identity();
            a.m[0, 0] = c;
            a.m[0, 1] = -s;
            a.m[1, 0] = s;
            a.m[1, 1] = c;
            return a;
        }

        /** @brief 3D affine rotation about the X axis by @p rad radians. */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T = float>
        [[nodiscard]] constexpr Affine<T, 3, S> MakeRotateX(T rad) {
            auto [s, c] = SinCos(rad);
            Affine<T, 3, S> a = Affine<T, 3, S>::Identity();
            a.m[1, 1] = c;
            a.m[1, 2] = -s;
            a.m[2, 1] = s;
            a.m[2, 2] = c;
            return a;
        }

        /** @brief 3D affine rotation about the Y axis by @p rad radians. */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T = float>
        [[nodiscard]] constexpr Affine<T, 3, S> MakeRotateY(T rad) {
            auto [s, c] = SinCos(rad);
            Affine<T, 3, S> a = Affine<T, 3, S>::Identity();
            a.m[0, 0] = c;
            a.m[0, 2] = s;
            a.m[2, 0] = -s;
            a.m[2, 2] = c;
            return a;
        }

        /** @brief 3D affine rotation about the Z axis by @p rad radians. */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T = float>
        [[nodiscard]] constexpr Affine<T, 3, S> MakeRotateZ(T rad) {
            auto [s, c] = SinCos(rad);
            Affine<T, 3, S> a = Affine<T, 3, S>::Identity();
            a.m[0, 0] = c;
            a.m[0, 1] = -s;
            a.m[1, 0] = s;
            a.m[1, 1] = c;
            return a;
        }

        /** @brief 3D affine rotation about an arbitrary @p axis by @p rad radians (Rodrigues). */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T = float>
        [[nodiscard]] constexpr Affine<T, 3, S> MakeRotateAxis(Vec<T, 3> axis, T rad) {
            Vec<T, 3> a = Normalize(axis);
            auto [s, c] = SinCos(rad);
            T k = T(1) - c;
            Affine<T, 3, S> r = Affine<T, 3, S>::Identity();
            r.m[0, 0] = k * a.x * a.x + c;
            r.m[0, 1] = k * a.x * a.y - s * a.z;
            r.m[0, 2] = k * a.x * a.z + s * a.y;
            r.m[1, 0] = k * a.x * a.y + s * a.z;
            r.m[1, 1] = k * a.y * a.y + c;
            r.m[1, 2] = k * a.y * a.z - s * a.x;
            r.m[2, 0] = k * a.x * a.z - s * a.y;
            r.m[2, 1] = k * a.y * a.z + s * a.x;
            r.m[2, 2] = k * a.z * a.z + c;
            return r;
        }

        /** @brief Right-handed look-at view transform: maps world → view space. */
        template<AffineStorage S = AffineStorage::Compact, std::floating_point T = float>
        [[nodiscard]] constexpr Affine<T, 3, S> MakeLookAt(Vec<T, 3> eye, Vec<T, 3> target,
                                                           Vec<T, 3> up) {
            Vec<T, 3> f = Normalize(target - eye);
            Vec<T, 3> s = Normalize(Cross(f, up));
            Vec<T, 3> u = Cross(s, f);
            Affine<T, 3, S> a = Affine<T, 3, S>::Identity();
            a.m[0, 0] = s.x;
            a.m[1, 0] = u.x;
            a.m[2, 0] = -f.x;
            a.m[0, 1] = s.y;
            a.m[1, 1] = u.y;
            a.m[2, 1] = -f.y;
            a.m[0, 2] = s.z;
            a.m[1, 2] = u.z;
            a.m[2, 2] = -f.z;
            a.m[0, 3] = -Dot(s, eye);
            a.m[1, 3] = -Dot(u, eye);
            a.m[2, 3] = Dot(f, eye);
            return a;
        }

        /// @}

    } // namespace Math

} // namespace Mashiro
