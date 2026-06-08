# Unified Affine Transform Design

**Date:** 2026-06-05
**Status:** Draft
**Scope:** Mashiro Math + Geom modules

## Goal

Eliminate the `Affine<T,N,S>` wrapper type and the duplicate `Geom::Affine`/`Geom::Rigid`
structs. Affine transforms become plain `Mat<T,R,C>` values with type aliases and
concept-constrained free functions for affine-specific operations. The same matrix acts
uniformly on `Vec`, `Mat`, and geometric primitives.

## 1. Type System

### 1.1 Form Tag

```cpp
enum class AffineForm { Compact, Full };
```

### 1.2 Alias Template

```cpp
template<std::floating_point T, int N, AffineForm F = AffineForm::Compact>
using affine = std::conditional_t<
    F == AffineForm::Compact,
    Mat<T, N, N + 1>,    // N×(N+1): implicit bottom row [0…0 1]
    Mat<T, N + 1>        // (N+1)×(N+1): bottom row materialised
>;
```

### 1.3 Short Aliases (Types.h)

```cpp
template<AffineForm F = AffineForm::Compact> using affine2  = affine<float, 2, F>;
template<AffineForm F = AffineForm::Compact> using affine3  = affine<float, 3, F>;
template<AffineForm F = AffineForm::Compact> using daffine2 = affine<double, 2, F>;
template<AffineForm F = AffineForm::Compact> using daffine3 = affine<double, 3, F>;
```

Usage: `affine3<>` = `mat3x4`, `affine3<Full>` = `mat4`.

## 2. Concept Layer (MatOps.h)

### 2.1 Shape Detection

```cpp
/// Matrix with N rows × (N+1) columns: compact affine storage.
template<typename M>
concept AffineCompact = ColumnMajorMat<M> && (MatCols<M> == MatRows<M> + 1);

/// Square matrix: full affine storage (bottom row = [0…0 1]).
template<typename M>
concept AffineFull = ColumnMajorMat<M> && (MatRows<M> == MatCols<M>);

/// Any matrix that represents an affine transform (compact or full).
template<typename M>
concept AffineMatrix = AffineCompact<M> || AffineFull<M>;
```

### 2.2 Spatial Dimension Extraction

```cpp
/// Spatial dimension N of an affine matrix.
template<AffineMatrix M>
inline constexpr int AffineDim = [] {
    if constexpr (AffineCompact<M>) return MatRows<M>;       // N×(N+1) → N
    else                            return MatRows<M> - 1;   // (N+1)×(N+1) → N
}();
```

## 3. Affine Operations (new file: `Math/AffineOps.h`)

All functions live in `Mashiro::Math`. Every operation is `constexpr` and uses
`template for` for unrolled compile-time loops.

### 3.1 Accessors

```cpp
/// Extract the N×N linear (rotation/scale/shear) submatrix.
template<AffineMatrix M>
[[nodiscard]] constexpr auto Linear(const M& m)
    -> Mat<ScalarOf<MatColType<M>>, AffineDim<M>>;

/// Extract the translation vector (column N).
template<AffineMatrix M>
[[nodiscard]] constexpr auto Translation(const M& m)
    -> Vec<ScalarOf<MatColType<M>>, AffineDim<M>>;
```

### 3.2 Construction

```cpp
/// Identity affine transform.
template<std::floating_point T, int N, AffineForm F = AffineForm::Compact>
[[nodiscard]] constexpr affine<T, N, F> IdentityAffine();

/// Compose affine from linear matrix + translation vector.
template<std::floating_point T, int N, AffineForm F = AffineForm::Compact>
[[nodiscard]] constexpr affine<T, N, F> MakeAffine(Mat<T, N> linear, Vec<T, N> translation);
```

### 3.3 Transform Application

```cpp
/// Map a point: p ↦ A·p + t (affine action).
template<AffineMatrix M>
[[nodiscard]] constexpr auto TransformPoint(const M& m, Vec<T, N> p) -> Vec<T, N>;

/// Map a direction/vector: v ↦ A·v (translation ignored).
template<AffineMatrix M>
[[nodiscard]] constexpr auto TransformVector(const M& m, Vec<T, N> v) -> Vec<T, N>;

/// Map a normal (covector): n ↦ normalize((A⁻¹)ᵀ · n).
template<AffineMatrix M>
[[nodiscard]] constexpr auto TransformNormal(const M& m, Vec<T, N> n) -> Vec<T, N>;

/// Map a normal assuming the linear part is orthonormal (rigid motion): n ↦ A·n.
template<AffineMatrix M>
[[nodiscard]] constexpr auto TransformNormalRigid(const M& m, Vec<T, N> n) -> Vec<T, N>;
```

### 3.4 Composition

```cpp
/// Compose two affine transforms: (a ∘ b)(x) = a(b(x)).
/// Compact: r.L = a.L·b.L, r.t = a.L·b.t + a.t (avoids bottom-row multiply).
/// Full: delegates to regular mat4 * mat4.
template<AffineMatrix M>
[[nodiscard]] constexpr M ComposeAffine(const M& a, const M& b);
```

### 3.5 Inverse

```cpp
/// Inverse of an affine transform: [A⁻¹ | −A⁻¹·t].
/// Only inverts the N×N linear part (cheaper than full (N+1)×(N+1) inverse).
template<AffineMatrix M>
[[nodiscard]] constexpr M InverseAffine(const M& m);
```

### 3.6 Conversion

```cpp
/// Compact → Full: materialise the implicit bottom row [0…0 1].
template<AffineCompact M>
[[nodiscard]] constexpr auto ToFull(const M& m)
    -> Mat<ScalarOf<MatColType<M>>, MatRows<M> + 1>;

/// Full → Compact: drop the bottom row.
template<AffineFull M>
[[nodiscard]] constexpr auto ToCompact(const M& m)
    -> Mat<ScalarOf<MatColType<M>>, MatRows<M> - 1, MatRows<M>>;

/// Determinant of the linear part (for rigid-check, volume-scale, etc.).
template<AffineMatrix M>
[[nodiscard]] constexpr auto DetAffine(const M& m)
    -> ScalarOf<MatColType<M>>;
```

## 4. Transform Builders (move from Affine.h → AffineOps.h)

All builders default to `AffineForm::Compact`. Spatial dimension is deduced from the
vector argument where applicable. Transcendental math routes through `ScalarMath.h`.

```cpp
template<AffineForm F = AffineForm::Compact, std::floating_point T, int N>
    requires(N == 2 || N == 3)
[[nodiscard]] constexpr affine<T, N, F> MakeTranslation(Vec<T, N> t);

template<AffineForm F = AffineForm::Compact, std::floating_point T, int N>
    requires(N == 2 || N == 3)
[[nodiscard]] constexpr affine<T, N, F> MakeScale(Vec<T, N> s);

template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
[[nodiscard]] constexpr affine<T, 2, F> MakeRotate2D(T rad);

template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
[[nodiscard]] constexpr affine<T, 3, F> MakeRotateX(T rad);

template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
[[nodiscard]] constexpr affine<T, 3, F> MakeRotateY(T rad);

template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
[[nodiscard]] constexpr affine<T, 3, F> MakeRotateZ(T rad);

template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
[[nodiscard]] constexpr affine<T, 3, F> MakeRotateAxis(Vec<T, 3> axis, T rad);

template<AffineForm F = AffineForm::Compact, std::floating_point T = float>
[[nodiscard]] constexpr affine<T, 3, F> MakeLookAt(Vec<T,3> eye, Vec<T,3> target, Vec<T,3> up);
```

## 5. Quaternion Integration (Quanterion.h)

`Quat::MakeTransform` returns `affine<float, 3, F>` (i.e. `mat3x4` or `mat4`):

```cpp
template<AffineForm F = AffineForm::Compact>
[[nodiscard]] constexpr affine<float, 3, F> MakeTransform(vec3 t, quat rot, vec3 scale);
```

## 6. Geometry Integration (GeomOps.h)

### 6.1 Elimination of `Geom::Affine`, `Geom::Rigid`, `SpatialTransform`

These structs are replaced by direct `AffineMatrix` constraint. The `SpatialTransform`
concept is deleted.

### 6.2 New `Transform` Overloads

```cpp
namespace Mashiro::Geom {

/// Reflection-driven generic transform for decomposable primitives.
/// Reads Quantity annotations via P2996 and dispatches each field to
/// TransformPoint/TransformVector/TransformNormal.
template<AffineMatrix M, Decomposable P>
[[nodiscard]] constexpr P Transform(const M& m, P prim);

/// Box: transform all 2^N corners and refit AABB.
template<AffineMatrix M, std::floating_point T, int N>
    requires(AffineDim<M> == N)
[[nodiscard]] constexpr BoxT<T, N> Transform(const M& m, BoxT<T, N> box);

/// Ball: centre transforms as point; radius scales by max singular value bound.
template<AffineMatrix M, std::floating_point T, int N>
    requires(AffineDim<M> == N)
[[nodiscard]] constexpr BallT<T, N> Transform(const M& m, BallT<T, N> ball);

/// Hyperplane: covector law n' = (A⁻¹)ᵀ·n, d' = d − n'·t, renormalised.
template<AffineMatrix M, std::floating_point T, int N>
    requires(AffineDim<M> == N)
[[nodiscard]] constexpr HyperplaneT<T, N> Transform(const M& m, HyperplaneT<T, N> pl);

/// Frustum: each plane transforms as covector (3D only).
template<AffineMatrix M, std::floating_point T>
    requires(AffineDim<M> == 3)
[[nodiscard]] constexpr FrustumT<T> Transform(const M& m, FrustumT<T> fr);

/// Sugar: m * primitive.
template<AffineMatrix M, Primitive P>
[[nodiscard]] constexpr P operator*(const M& m, P prim) { return Transform(m, prim); }

} // namespace Mashiro::Geom
```

### 6.3 Internal Helpers (Detail)

```cpp
namespace Detail {
    /// RadiusScale: conservative σ_max(A) ≤ √(‖A‖₁·‖A‖∞). Extracted from Linear(m).
    template<AffineMatrix M>
    [[nodiscard]] constexpr auto RadiusScale(const M& m) -> ScalarOf<MatColType<M>>;

    /// Cotransform: Transpose(Inverse(Linear(m))). Used for normals and planes.
    template<AffineMatrix M>
    [[nodiscard]] constexpr auto Cotransform(const M& m)
        -> Mat<ScalarOf<MatColType<M>>, AffineDim<M>>;
}
```

## 7. Interaction with Existing Mat Operations

### 7.1 `operator*` Disambiguation

`mat3x4 * mat3x4` is NOT a valid general matrix multiply (shapes don't chain: 3×4 × 3×4
is undefined). Therefore `ComposeAffine` is unambiguous — it's the ONLY way to multiply
two compact affine matrices. No `operator*` overload is needed or provided for compact
forms.

For full forms (`mat4 * mat4`), the existing `operator*` already does the right thing
(standard matrix multiplication = affine composition when the bottom row is `[0 0 0 1]`).
`ComposeAffine` on full forms simply delegates to `operator*`.

### 7.2 `operator*` for Point Transform

We provide `operator*(AffineMatrix, Vec<T,N>)` as sugar for `TransformPoint`:

```cpp
/// Affine * point: m * p = TransformPoint(m, p).
template<AffineMatrix M, typename T, int N, AlignTag A>
    requires(AffineDim<M> == N && std::same_as<T, ScalarOf<MatColType<M>>>)
[[nodiscard]] constexpr Vec<T, N> operator*(const M& m, Vec<T, N, A> p);
```

This does NOT conflict with the existing `Mat<R,C> * Vec<C>` general multiply because:
- For compact `mat3x4 * vec3`: the general multiply would expect `Vec<4>` (C=4 columns),
  but we're passing `Vec<3>`. Our overload matches `AffineDim<mat3x4> == 3`.
- For full `mat4 * vec4`: the general multiply still works for homogeneous coordinates.
  Our overload matches `mat4 * vec3` (AffineDim = 3, implicit w=1).

### 7.3 `Inverse` Overloading

The existing `Math::Inverse(Mat<T,N>)` handles square matrices. We add `InverseAffine`
as the efficient path for affine matrices. Users choose:
- `Inverse(mat4)` — general 4×4 inverse (more ops, no structural assumption)
- `InverseAffine(mat3x4)` — cheap: invert 3×3, recompute translation

### 7.4 `Det` / `Transpose` on Affine

- `DetAffine(m)` = `Det(Linear(m))` — the volume scaling factor.
- `Transpose` is not semantically meaningful for affine transforms and is not overloaded.

## 8. File Layout Changes

| Before | After |
|--------|-------|
| `Math/Affine.h` (Affine struct + builders) | **Deleted** |
| `Math/MatOps.h` (Identity, Det, Inverse, projections) | Unchanged (projections stay) |
| — | `Math/AffineOps.h` (concepts, aliases, accessors, builders, compose, inverse) |
| `Geom/GeomOps.h` (Geom::Affine, Geom::Rigid, SpatialTransform, Transform) | Simplified: remove structs, use `AffineMatrix` directly |
| `Math/Quanterion.h` includes Affine.h | Includes `AffineOps.h` instead |

## 9. Migration Summary

### Deleted Entities
- `Mashiro::Affine<T, N, S>` struct
- `Mashiro::AffineStorage` enum
- `Mashiro::AffineTransform` concept
- `Mashiro::Detail::IsAffineV`
- `Mashiro::Geom::Affine<T, N>` struct
- `Mashiro::Geom::Rigid<T, N>` struct
- `Mashiro::Geom::SpatialTransform` concept
- `Mashiro::Geom::Translation()`, `Geom::LinearMap()`
- `Mashiro::Geom::operator*(Affine, Affine)` composition

### Renamed / Moved
| Old | New |
|-----|-----|
| `Affine<float,3>::Identity()` | `Math::IdentityAffine<float,3>()` |
| `affine.m[r,c]` | `m[r,c]` (it's just a Mat) |
| `affine.Linear()` | `Math::Linear(m)` |
| `affine.Translation()` | `Math::Translation(m)` |
| `affine.ToMat()` | `Math::ToFull(m)` |
| `affine.TransformPoint(p)` | `Math::TransformPoint(m, p)` or `m * p` |
| `Math::Inverse(Affine)` | `Math::InverseAffine(m)` |
| `Affine * Affine` | `Math::ComposeAffine(a, b)` |
| `Geom::Affine{linear, translation}` | just pass a `mat3x4` |
| `Geom::Transform(spatialXform, prim)` | `Geom::Transform(mat, prim)` |

## 10. C++26 Features Used

| Feature | Usage |
|---------|-------|
| P1306 `template for` | Unroll fixed-dimension loops in all operations |
| P2996 Reflection | Read `Quantity` annotations from primitive fields in generic `Transform` |
| P3385 Annotations | `[[=Geom::Quantity::Point]]` etc. on primitive fields |
| P3289 `consteval {}` | Compile-time invariant checks on type layouts |
| Concepts + `requires` | `AffineCompact`, `AffineFull`, `AffineMatrix` shape detection |
| Deducing this | Not needed here (no member functions on Mat to deduplicate) |
| `if constexpr` | Branch between compact/full paths at zero cost |
| `std::conditional_t` | The `affine<T,N,F>` alias template itself |

## 11. Performance Guarantees

- Zero runtime overhead vs hand-written code: all dispatch is compile-time.
- Compact form (`mat3x4`) saves 25% storage vs `mat4` (48B vs 64B for float).
- `ComposeAffine(compact, compact)` does 9 FMAs for linear + 3 FMAs for translation
  (vs 16+4 for full 4×4 multiply) — exactly the arithmetic minimum.
- `InverseAffine` inverts only the 3×3 block (cofactor expansion) — never the full 4×4.
- `TransformPoint`/`TransformVector` are column-linear-combination (cache-friendly).
- All operations are `constexpr`: inputs known at compile time → result folds to constant.



