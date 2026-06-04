/**
 * @file Algebra.h
 * @brief Algebraic structure concepts and generic algorithms for the Mashiro math library.
 *
 * This header defines a compile-time concept hierarchy that models fundamental
 * algebraic/topological structures from linear algebra:
 *
 *     AdditiveGroup  →  VectorSpace  →  InnerProductSpace  →  NormedSpace
 *                                                                  ↓
 *                                                            MetricSpace
 *
 * Generic algorithms (Norm, Normalize, Distance, Lerp, Reflect, …) are constrained
 * to the minimal structure they require, so they automatically work on ANY type that
 * satisfies the concept — Vec<T,N>, quat, future dual-quaternions, complex numbers,
 * or user-defined types.
 *
 * Design principles:
 *   - Zero runtime overhead: all concepts are compile-time checks, all algorithms constexpr.
 *   - Opt-in via ADL: a type models InnerProductSpace by providing a free function
 *     `InnerProduct(T, T) → scalar` findable via ADL (or satisfying HomogeneousVec,
 *     which auto-generates the inner product).
 *   - NormedSpace defaults to the induced norm (√InnerProduct) but can be overridden
 *     by providing a custom `Norm(T) → scalar` ADL function.
 *   - Backward-compatible: existing HomogeneousVec types satisfy all concepts automatically.
 *
 * Namespace: Mashiro::Math (algorithms + concepts)
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Math/ScalarMath.h"
#include "Mashiro/Math/Vec.h"

#include <concepts>
#include <limits>
#include <type_traits>

namespace Mashiro {

    /** @cond INTERNAL */
    namespace Math::Detail {

        /// @brief Type exposes a nested `scalar_type` alias.
        template<typename T>
        concept HasScalarType = requires { typename T::scalar_type; };

        /// @brief Type supports `v[0]` subscript access.
        template<typename T>
        concept HasSubscript = requires(T v) {
            { v[0] };
        };

        /// @brief Deduce the scalar type: prefer `T::scalar_type`, fall back to `decltype(v[0])`.
        template<typename T>
        consteval auto ScalarTypeOfImpl() {
            if constexpr (HasScalarType<T>) {
                return std::type_identity<typename T::scalar_type>{};
            } else if constexpr (HasSubscript<T>) {
                return std::type_identity<std::remove_cvref_t<decltype(std::declval<T>()[0])>>{};
            }
        }

        template<typename T>
            requires requires { ScalarTypeOfImpl<T>(); }
        using ScalarTypeOf = decltype(ScalarTypeOfImpl<T>())::type;

    } // namespace Math::Detail
    /** @endcond */

    /**
     * @brief Extracts the scalar (field) type from a vector-space element.
     *
     * Resolution order:
     *   1. T::scalar_type (explicit opt-in)
     *   2. decltype(t[0]) (HomogeneousVec / subscript-based types)
     */
    template<typename T>
        requires requires { typename Math::Detail::ScalarTypeOf<T>; }
    using FieldOf = Math::Detail::ScalarTypeOf<T>;

    /** @brief An additive group: supports `a + b`, `a - b`, `-a` with result type T. */
    template<typename T>
    concept AdditiveGroup = requires(T a, T b) {
        { a + b } -> std::convertible_to<T>;
        { a - b } -> std::convertible_to<T>;
        { -a } -> std::convertible_to<T>;
    };

    /**
     * @brief A vector space over a scalar field F: AdditiveGroup with scalar multiplication.
     *
     * F is extracted via FieldOf<T>. The type must support `v * s` and `s * v`.
     */
    template<typename T>
    concept VectorSpace = AdditiveGroup<T> && requires {
        typename Math::Detail::ScalarTypeOf<T>;
    } && requires(T v, FieldOf<T> s) {
        { v * s } -> std::convertible_to<T>;
        { s * v } -> std::convertible_to<T>;
    };

    /** @cond INTERNAL */
    namespace Math::Detail {

        /// @brief Detects an ADL `InnerProduct(a, b)` returning the scalar type.
        template<typename T>
        concept HasADLInnerProduct = requires(T a, T b) {
            { InnerProduct(a, b) } -> std::convertible_to<FieldOf<T>>;
        };

    } // namespace Math::Detail
    /** @endcond */

    /** @brief An inner product space: VectorSpace with ADL `InnerProduct(a, b) → scalar`. */
    template<typename T>
    concept InnerProductSpace = VectorSpace<T> && Math::Detail::HasADLInnerProduct<T>;

    /** @cond INTERNAL */
    namespace Math::Detail {

        /// @brief Detects an ADL `Norm(v)` returning the scalar type.
        template<typename T>
        concept HasADLNorm = requires(T v) {
            { Norm(v) } -> std::convertible_to<FieldOf<T>>;
        };

    } // namespace Math::Detail
    /** @endcond */

    /** @brief A normed space: explicit ADL `Norm(v)`, or induced from InnerProductSpace. */
    template<typename T>
    concept NormedSpace = VectorSpace<T> && (Math::Detail::HasADLNorm<T> || InnerProductSpace<T>);

    /** @cond INTERNAL */
    namespace Math::Detail {

        /// @brief Detects an ADL `Distance(a, b)` returning the scalar type.
        template<typename T>
        concept HasADLDistance = requires(T a, T b) {
            { Distance(a, b) } -> std::convertible_to<FieldOf<T>>;
        };

    } // namespace Math::Detail
    /** @endcond */

    /** @brief A metric space: explicit ADL `Distance(a, b)`, or induced from NormedSpace. */
    template<typename T>
    concept MetricSpace = (Math::Detail::HasADLDistance<T> || NormedSpace<T>) &&
                          requires { typename Math::Detail::ScalarTypeOf<T>; };

    /**
     * @brief ADL InnerProduct for all HomogeneousVec types.
     *
     * This bridges the existing Dot product into the algebraic concept system.
     * Any type satisfying HomogeneousVec with an arithmetic scalar automatically
     * satisfies InnerProductSpace.
     */
    template<HomogeneousVec V>
        requires std::is_arithmetic_v<ScalarOf<V>>
    [[nodiscard]] constexpr ScalarOf<V> InnerProduct(V a, V b) {
        ScalarOf<V> sum = a[0] * b[0];
        for (int i = 1; i < VecDim<V>; ++i)
            sum += a[i] * b[i];
        return sum;
    }

    namespace Math {

        /// @name Generic algorithms constrained to algebraic structures
        /// @{

        /** @brief Inner product of two elements. Delegates to ADL `InnerProduct(a, b)`. */
        template<InnerProductSpace T>
        [[nodiscard]] constexpr FieldOf<T> Dot(T a, T b) {
            return InnerProduct(a, b);
        }

        /** @brief Squared norm: InnerProduct(v, v). */
        template<InnerProductSpace T>
        [[nodiscard]] constexpr FieldOf<T> NormSq(T v) {
            return InnerProduct(v, v);
        }

        /** @brief Euclidean norm (L2): √NormSq(v). */
        template<InnerProductSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr FieldOf<T> Norm(T v) {
            return Math::Sqrt(NormSq(v));
        }

        /** @brief Return a unit-length element; returns v unchanged if near-zero. */
        template<InnerProductSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr T Normalize(T v) {
            using F = FieldOf<T>;
            F sq = NormSq(v);
            // Type-appropriate epsilon: ~128 * machine epsilon squared (for the sq comparison).
            constexpr F kMinSq = F(128) * std::numeric_limits<F>::epsilon() * F(128) *
                                 std::numeric_limits<F>::epsilon();
            return sq < kMinSq ? v : v * (F(1) / Math::Sqrt(sq));
        }

        /** @brief Squared distance: NormSq(b - a). */
        template<InnerProductSpace T>
        [[nodiscard]] constexpr FieldOf<T> DistanceSq(T a, T b) {
            return NormSq(b - a);
        }

        /** @brief Distance: Norm(b - a). */
        template<InnerProductSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr FieldOf<T> Distance(T a, T b) {
            return Norm(b - a);
        }

        /** @brief Linear interpolation: a + (b - a) * t. */
        template<VectorSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr T Lerp(T a, T b, FieldOf<T> t) {
            return a + (b - a) * t;
        }

        /** @brief Reflect v across the hyperplane with unit normal n: v - 2⟨v,n⟩n. */
        template<InnerProductSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr T Reflect(T v, T n) {
            using F = FieldOf<T>;
            return v - n * (F(2) * InnerProduct(v, n));
        }

        /** @brief Orthogonal projection of v onto n: (⟨v,n⟩/⟨n,n⟩) n. */
        template<InnerProductSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr T Project(T v, T n) {
            return n * (InnerProduct(v, n) / InnerProduct(n, n));
        }

        /** @brief Rejection of v from n: v - Project(v, n). Component orthogonal to n. */
        template<InnerProductSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr T Reject(T v, T n) {
            return v - Project(v, n);
        }

        /// @}

        // -----------------------------------------------------------------
        // Approximate comparison (tolerance-based)
        // -----------------------------------------------------------------

        /// @name Approximate comparison operators
        /// @{

        /** @cond INTERNAL */
        namespace Detail {
            template<std::floating_point F>
            inline constexpr F kDefaultEps = F(128) * std::numeric_limits<F>::epsilon();
        } // namespace Detail
        /** @endcond */

        /** @brief Approximate equality for MetricSpace types: Distance(a, b) < eps. */
        template<MetricSpace T>
            requires std::floating_point<FieldOf<T>>
        [[nodiscard]] constexpr bool ApproxEq(T a, T b,
                                              FieldOf<T> eps = Detail::kDefaultEps<FieldOf<T>>) {
            return Distance(a, b) < eps;
        }

        /** @brief Approximate equality for floating-point scalars: |a - b| < eps. */
        template<std::floating_point F>
        [[nodiscard]] constexpr bool ApproxEq(F a, F b, F eps = Detail::kDefaultEps<F>) {
            return Math::Abs(a - b) < eps;
        }

        /** @brief Approximate inequality: !ApproxEq(a, b, eps). */
        template<typename T>
            requires requires(T a, T b) { ApproxEq(a, b); }
        [[nodiscard]] constexpr bool ApproxNe(T a, T b, auto... eps) {
            return !ApproxEq(a, b, eps...);
        }

        /** @brief a < b with tolerance: a < b - eps. */
        template<std::floating_point F>
        [[nodiscard]] constexpr bool ApproxLt(F a, F b, F eps = Detail::kDefaultEps<F>) {
            return a < b - eps;
        }

        /** @brief a <= b with tolerance: a < b + eps. */
        template<std::floating_point F>
        [[nodiscard]] constexpr bool ApproxLe(F a, F b, F eps = Detail::kDefaultEps<F>) {
            return a < b + eps;
        }

        /** @brief a > b with tolerance: a > b + eps. */
        template<std::floating_point F>
        [[nodiscard]] constexpr bool ApproxGt(F a, F b, F eps = Detail::kDefaultEps<F>) {
            return a > b + eps;
        }

        /** @brief a >= b with tolerance: a > b - eps. */
        template<std::floating_point F>
        [[nodiscard]] constexpr bool ApproxGe(F a, F b, F eps = Detail::kDefaultEps<F>) {
            return a > b - eps;
        }

        /// @}

    } // namespace Math

} // namespace Mashiro
