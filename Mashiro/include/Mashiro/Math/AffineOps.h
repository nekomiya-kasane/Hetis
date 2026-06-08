/**
 * @file AffineOps.h
 * @brief Affine transform operations on plain Mat<T,R,C> matrices.
 *
 * Affine transforms are represented as plain matrices with no wrapper type:
 * - Compact form: `Mat<T, N, N+1>` (implicit bottom row `[0…0 1]`)
 * - Full form: `Mat<T, N+1>` (materialised bottom row)
 *
 * Type aliases `affine<T,N,F>` select the form; short aliases `affine2<F>`,
 * `affine3<F>` default to compact float. All operations are concept-constrained
 * free functions dispatched at compile time by the matrix shape — zero overhead.
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Core/Meta.h"
#include "Mashiro/Math/MatOps.h"
#include "Mashiro/Math/VecOps.h"

#include <concepts>
#include <type_traits>

namespace Mashiro {

    /** @brief Storage form for affine transform alias selection. */
    enum class AffineForm { Compact, Full };

    /** @brief Affine matrix alias: compact `Mat<T,N,N+1>` or full `Mat<T,N+1>`. */
    template<std::floating_point T, int N, AffineForm F = AffineForm::Compact>
        requires(N == 2 || N == 3)
    using affine = std::conditional_t<
        F == AffineForm::Compact,
        Mat<T, N, N + 1>,
        Mat<T, N + 1>
    >;

    /// @name Affine alias shortcuts
    /// @{
    template<AffineForm F = AffineForm::Compact> using affine2  = affine<float, 2, F>;
    template<AffineForm F = AffineForm::Compact> using affine3  = affine<float, 3, F>;
    template<AffineForm F = AffineForm::Compact> using daffine2 = affine<double, 2, F>;
    template<AffineForm F = AffineForm::Compact> using daffine3 = affine<double, 3, F>;
    /// @}

    /// @name Affine shape concepts
    /// @{

    /** @brief Matrix with N rows × (N+1) columns: compact affine storage. */
    template<typename M>
    concept AffineCompact = ColumnMajorMat<M> && (MatCols<M> == MatRows<M> + 1);

    /** @brief Square matrix usable as full affine storage. */
    template<typename M>
    concept AffineFull = ColumnMajorMat<M> && (MatRows<M> == MatCols<M>);

    /** @brief Any matrix that represents an affine transform (compact or full). */
    template<typename M>
    concept AffineMatrix = AffineCompact<M> || AffineFull<M>;

    /** @brief Spatial dimension N of an affine matrix. */
    template<AffineMatrix M>
    inline constexpr int AffineDim = [] {
        if constexpr (AffineCompact<M>) return MatRows<M>;
        else                            return MatRows<M> - 1;
    }();

    /// @}

    namespace Math {

    /// @name Affine accessors
    /// @{

    /** @brief Extract the N×N linear (rotation/scale/shear) submatrix. */
    template<AffineMatrix M>
    [[nodiscard]] constexpr auto Linear(const M& m) {
        using T = ScalarOf<MatColType<M>>;
        constexpr int N = AffineDim<M>;
        Mat<T, N> r{};
        template for (constexpr int col : Iota<N>) {
            template for (constexpr int row : Iota<N>) {
                r[row, col] = m[row, col];
            }
        }
        return r;
    }

    /** @brief Extract the translation vector (last column, top N rows). */
    template<AffineMatrix M>
    [[nodiscard]] constexpr auto Translation(const M& m) {
        using T = ScalarOf<MatColType<M>>;
        constexpr int N = AffineDim<M>;
        Vec<T, N> t{};
        template for (constexpr int i : Iota<N>) {
            t[i] = m[i, N];
        }
        return t;
    }

    /// @}

    /// @name Affine construction
    /// @{

    /** @brief Identity affine transform. */
    template<std::floating_point T, int N, AffineForm F = AffineForm::Compact>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr affine<T, N, F> IdentityAffine() {
        affine<T, N, F> m{};
        template for (constexpr int i : Iota<N>) {
            m[i, i] = T(1);
        }
        if constexpr (F == AffineForm::Full) {
            m[N, N] = T(1);
        }
        return m;
    }

    /** @brief Compose an affine from a linear matrix and translation. */
    template<std::floating_point T, int N, AffineForm F = AffineForm::Compact>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr affine<T, N, F> MakeAffine(Mat<T, N> linear, Vec<T, N> translation) {
        affine<T, N, F> m{};
        template for (constexpr int col : Iota<N>) {
            template for (constexpr int row : Iota<N>) {
                m[row, col] = linear[row, col];
            }
        }
        template for (constexpr int i : Iota<N>) {
            m[i, N] = translation[i];
        }
        if constexpr (F == AffineForm::Full) {
            m[N, N] = T(1);
        }
        return m;
    }

    /// @}

    /// @name Affine transform application
    /// @{

    /** @brief Map a point: p ↦ A·p + t. */
    template<AffineMatrix M, typename T, int N, AlignTag A>
        requires(AffineDim<M> == N && std::same_as<T, ScalarOf<MatColType<M>>>)
    [[nodiscard]] constexpr Vec<T, N> TransformPoint(const M& m, Vec<T, N, A> p) {
        Vec<T, N> r{};
        template for (constexpr int row : Iota<N>) {
            T sum = m[row, N];
            template for (constexpr int k : Iota<N>) {
                sum += m[row, k] * p[k];
            }
            r[row] = sum;
        }
        return r;
    }

    /** @brief Map a direction: v ↦ A·v (translation ignored). */
    template<AffineMatrix M, typename T, int N, AlignTag A>
        requires(AffineDim<M> == N && std::same_as<T, ScalarOf<MatColType<M>>>)
    [[nodiscard]] constexpr Vec<T, N> TransformVector(const M& m, Vec<T, N, A> v) {
        Vec<T, N> r{};
        template for (constexpr int row : Iota<N>) {
            T sum{};
            template for (constexpr int k : Iota<N>) {
                sum += m[row, k] * v[k];
            }
            r[row] = sum;
        }
        return r;
    }

    /** @brief Map a normal (covector): n ↦ normalize((A⁻¹)ᵀ · n). */
    template<AffineMatrix M, typename T, int N, AlignTag A>
        requires(AffineDim<M> == N && std::same_as<T, ScalarOf<MatColType<M>>>)
    [[nodiscard]] constexpr Vec<T, N> TransformNormal(const M& m, Vec<T, N, A> n) {
        Mat<T, N> co = Transpose(Inverse(Linear(m)));
        return Normalize(co * n);
    }

    /** @brief Map a normal assuming orthonormal linear part (rigid): n ↦ A·n. */
    template<AffineMatrix M, typename T, int N, AlignTag A>
        requires(AffineDim<M> == N && std::same_as<T, ScalarOf<MatColType<M>>>)
    [[nodiscard]] constexpr Vec<T, N> TransformNormalRigid(const M& m, Vec<T, N, A> n) {
        return TransformVector(m, n);
    }

    /// @}

    /// @name Affine composition and inverse
    /// @{

    /** @brief Compose two compact affine transforms: (a ∘ b)(x) = a(b(x)). */
    template<typename T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Mat<T, N, N + 1> ComposeAffine(const Mat<T, N, N + 1>& a,
                                                            const Mat<T, N, N + 1>& b) {
        Mat<T, N, N + 1> r{};
        template for (constexpr int col : Iota<N>) {
            template for (constexpr int row : Iota<N>) {
                T sum{};
                template for (constexpr int k : Iota<N>) {
                    sum += a[row, k] * b[k, col];
                }
                r[row, col] = sum;
            }
        }
        template for (constexpr int row : Iota<N>) {
            T sum = a[row, N];
            template for (constexpr int k : Iota<N>) {
                sum += a[row, k] * b[k, N];
            }
            r[row, N] = sum;
        }
        return r;
    }

    /** @brief Compose two full affine transforms (delegates to mat multiply). */
    template<typename T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Mat<T, N + 1> ComposeAffine(const Mat<T, N + 1>& a,
                                                         const Mat<T, N + 1>& b) {
        return a * b;
    }

    /** @brief Inverse of a compact affine: [A⁻¹ | −A⁻¹·t]. */
    template<typename T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Mat<T, N, N + 1> InverseAffine(const Mat<T, N, N + 1>& m) {
        Mat<T, N> li = Inverse(Linear(m));
        Vec<T, N> t  = Translation(m);
        Mat<T, N, N + 1> r{};
        template for (constexpr int col : Iota<N>) {
            template for (constexpr int row : Iota<N>) {
                r[row, col] = li[row, col];
            }
        }
        template for (constexpr int row : Iota<N>) {
            T sum{};
            template for (constexpr int k : Iota<N>) {
                sum += li[row, k] * t[k];
            }
            r[row, N] = -sum;
        }
        return r;
    }

    /** @brief Inverse of a full affine: materialises [A⁻¹ | −A⁻¹·t ; 0…0 1]. */
    template<typename T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Mat<T, N + 1> InverseAffine(const Mat<T, N + 1>& m) {
        auto compact = InverseAffine(ToCompact(m));
        return ToFull(compact);
    }

    /// @}

    /// @name Affine conversion
    /// @{

    /** @brief Compact → Full: materialise the bottom row [0…0 1]. */
    template<typename T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Mat<T, N + 1> ToFull(const Mat<T, N, N + 1>& m) {
        Mat<T, N + 1> r{};
        template for (constexpr int col : Iota<N + 1>) {
            template for (constexpr int row : Iota<N>) {
                r[row, col] = m[row, col];
            }
        }
        r[N, N] = T(1);
        return r;
    }

    /** @brief Full → Compact: drop the bottom row. */
    template<typename T, int N>
        requires(N >= 2 && N <= 4)
    [[nodiscard]] constexpr Mat<T, N - 1, N> ToCompact(const Mat<T, N>& m) {
        Mat<T, N - 1, N> r{};
        template for (constexpr int col : Iota<N>) {
            template for (constexpr int row : Iota<N - 1>) {
                r[row, col] = m[row, col];
            }
        }
        return r;
    }

    /** @brief Determinant of the linear part of an affine matrix. */
    template<AffineMatrix M>
    [[nodiscard]] constexpr auto DetAffine(const M& m) {
        return Det(Linear(m));
    }

    /// @}

    /// @name Affine transform builders
    /// @{

    /** @brief Translation: [I | t]. Dimension deduced from @p t. */
    template<AffineForm F = AffineForm::Compact, std::floating_point T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr affine<T, N, F> MakeTranslation(Vec<T, N> t) {
        auto m = IdentityAffine<T, N, F>();
        template for (constexpr int i : Iota<N>) {
            m[i, N] = t[i];
        }
        return m;
    }

    /** @brief Non-uniform scale: [diag(s) | 0]. Dimension deduced from @p s. */
    template<AffineForm F = AffineForm::Compact, std::floating_point T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr affine<T, N, F> MakeScale(Vec<T, N> s) {
        affine<T, N, F> m{};
        template for (constexpr int i : Iota<N>) {
            m[i, i] = s[i];
        }
        if constexpr (F == AffineForm::Full) {
            m[N, N] = T(1);
        }
        return m;
    }

    /** @brief 2D rotation by @p rad radians (counter-clockwise). */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 2, F> MakeRotate2D(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine<T, 2, F>();
        m[0, 0] = c;  m[0, 1] = -s;
        m[1, 0] = s;  m[1, 1] = c;
        return m;
    }

    /** @brief 3D rotation about the X axis. */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeRotateX(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine<T, 3, F>();
        m[1, 1] = c;  m[1, 2] = -s;
        m[2, 1] = s;  m[2, 2] = c;
        return m;
    }

    /** @brief 3D rotation about the Y axis. */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeRotateY(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine<T, 3, F>();
        m[0, 0] = c;  m[0, 2] = s;
        m[2, 0] = -s; m[2, 2] = c;
        return m;
    }

    /** @brief 3D rotation about the Z axis. */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeRotateZ(T rad) {
        auto [s, c] = SinCos(rad);
        auto m = IdentityAffine<T, 3, F>();
        m[0, 0] = c;  m[0, 1] = -s;
        m[1, 0] = s;  m[1, 1] = c;
        return m;
    }

    /** @brief 3D rotation about an arbitrary @p axis by @p rad radians (Rodrigues). */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeRotateAxis(Vec<T, 3> axis, T rad) {
        Vec<T, 3> a = Normalize(axis);
        auto [s, c] = SinCos(rad);
        T k = T(1) - c;
        auto m = IdentityAffine<T, 3, F>();
        m[0, 0] = k * a.x * a.x + c;
        m[0, 1] = k * a.x * a.y - s * a.z;
        m[0, 2] = k * a.x * a.z + s * a.y;
        m[1, 0] = k * a.x * a.y + s * a.z;
        m[1, 1] = k * a.y * a.y + c;
        m[1, 2] = k * a.y * a.z - s * a.x;
        m[2, 0] = k * a.x * a.z - s * a.y;
        m[2, 1] = k * a.y * a.z + s * a.x;
        m[2, 2] = k * a.z * a.z + c;
        return m;
    }

    /** @brief Right-handed look-at view transform: maps world → view space. */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeLookAt(Vec<T, 3> eye, Vec<T, 3> target,
                                                        Vec<T, 3> up) {
        Vec<T, 3> f = Normalize(target - eye);
        Vec<T, 3> s = Normalize(Cross(f, up));
        Vec<T, 3> u = Cross(s, f);
        auto m = IdentityAffine<T, 3, F>();
        m[0, 0] = s.x;  m[0, 1] = s.y;  m[0, 2] = s.z;
        m[1, 0] = u.x;  m[1, 1] = u.y;  m[1, 2] = u.z;
        m[2, 0] = -f.x; m[2, 1] = -f.y; m[2, 2] = -f.z;
        m[0, 3] = -Dot(s, eye);
        m[1, 3] = -Dot(u, eye);
        m[2, 3] = Dot(f, eye);
        return m;
    }

    /** @brief Mirror (reflection) across a plane through the origin with unit @p normal. */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeMirror(Vec<T, 3> normal) {
        Vec<T, 3> n = Normalize(normal);
        auto m = IdentityAffine<T, 3, F>();
        m[0, 0] = T(1) - T(2) * n.x * n.x;
        m[0, 1] =       -T(2) * n.x * n.y;
        m[0, 2] =       -T(2) * n.x * n.z;
        m[1, 0] =       -T(2) * n.y * n.x;
        m[1, 1] = T(1) - T(2) * n.y * n.y;
        m[1, 2] =       -T(2) * n.y * n.z;
        m[2, 0] =       -T(2) * n.z * n.x;
        m[2, 1] =       -T(2) * n.z * n.y;
        m[2, 2] = T(1) - T(2) * n.z * n.z;
        return m;
    }

    /** @brief Mirror (reflection) across a line through the origin with unit @p normal (2D). */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 2, F> MakeMirror2D(Vec<T, 2> normal) {
        Vec<T, 2> n = Normalize(normal);
        auto m = IdentityAffine<T, 2, F>();
        m[0, 0] = T(1) - T(2) * n.x * n.x;
        m[0, 1] =       -T(2) * n.x * n.y;
        m[1, 0] =       -T(2) * n.y * n.x;
        m[1, 1] = T(1) - T(2) * n.y * n.y;
        return m;
    }

    /** @brief Mirror across the YZ plane (flip X axis). */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeMirrorX() {
        auto m = IdentityAffine<T, 3, F>();
        m[0, 0] = T(-1);
        return m;
    }

    /** @brief Mirror across the XZ plane (flip Y axis). */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeMirrorY() {
        auto m = IdentityAffine<T, 3, F>();
        m[1, 1] = T(-1);
        return m;
    }

    /** @brief Mirror across the XY plane (flip Z axis). */
    template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
    [[nodiscard]] constexpr affine<T, 3, F> MakeMirrorZ() {
        auto m = IdentityAffine<T, 3, F>();
        m[2, 2] = T(-1);
        return m;
    }

    /// @}

    } // namespace Math

    /// @name Affine operators (Mashiro scope for ADL on Mat<T,R,C>)
    /// @{

    /** @brief Compact affine composition: (a ∘ b)(x) = a(b(x)). */
    template<typename T, int N>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Mat<T, N, N + 1> operator*(const Mat<T, N, N + 1>& a,
                                                        const Mat<T, N, N + 1>& b) {
        return Math::ComposeAffine(a, b);
    }

    /** @brief Compact affine * point: TransformPoint. */
    template<typename T, int N, AlignTag A>
        requires(N == 2 || N == 3)
    [[nodiscard]] constexpr Vec<T, N> operator*(const Mat<T, N, N + 1>& m, Vec<T, N, A> p) {
        return Math::TransformPoint(m, p);
    }

    /// @}

} // namespace Mashiro





