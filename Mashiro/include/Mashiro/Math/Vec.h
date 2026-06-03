/**
 * @file Vec.h
 * @brief Generic N-component vector template with reflection-generated operators.
 *
 * `Vec<T, N, A>` is the single source of truth for all N-component vector types.
 *
 * @tparam T Arithmetic scalar (`float`, `double`, `int32_t`, `uint32_t`, …).
 * @tparam N Dimension (2, 3, or 4).
 * @tparam A Alignment policy: `AlignTag::Gpu` (std430) or `AlignTag::Packed`.
 *
 * Named members (`x`, `y`, `z`, `w`) are preserved for designated-initialiser support,
 * GPU debugger readability, and ergonomic member access. Each specialisation provides a
 * deducing-this `operator[]` with an `if consteval` dual path for zero-cost subscript
 * access at both compile time and run time.
 *
 * Operators are defined at namespace scope (found via ADL) and generated generically
 * over the `HomogeneousVec` concept using P2996 reflection + `template for`.
 * Only `operator==` is declared in-class (`= default`) because the language requires it.
 *
 * @ingroup Math
 */
#pragma once

#include <type_traits>
#include <ranges>

#include "Mashiro/Core/TypeTraits.h"

namespace Mashiro {

    /// @name Alignment policy
    /// @{

    /**
     * @brief Tag type for vector alignment policy.
     *
     * - `Packed` — natural alignment of the scalar type.
     * - `Gpu` — std430-compatible alignment for GPU buffer compatibility.
     */
    enum class AlignTag {
        Packed, ///< Natural alignment (`alignof(T)`).
        Gpu,    ///< std430 alignment (`2*sizeof(T)` for vec2, `4*sizeof(T)` for vec3/4).
    };

    /// @}

    namespace Detail {

        /**
         * @brief Computes the alignment for a vector type based on std430 rules.
         *
         * @tparam T Scalar element type.
         * @tparam N Number of components (2, 3, or 4).
         * @tparam A Alignment policy tag.
         * @return std430 alignment: vec2 → 2*sizeof(T), vec3/vec4 → 4*sizeof(T).
         *         If Packed, returns alignof(T).
         */
        template<typename T, int N, AlignTag A>
        consteval size_t VecAlignment() {
            if constexpr (A == AlignTag::Packed) {
                return alignof(T);
            } else if constexpr (N == 2) {
                return 2 * sizeof(T);
            } else {
                return 4 * sizeof(T);
            }
        }

    } // namespace Detail

    /// @name Vec specialisations
    /// @{

    /**
     * @brief Primary vector template.
     *
     * Intentionally incomplete; only specializations for N=2, 3, 4 are defined.
     *
     * @tparam T Arithmetic scalar type.
     * @tparam N Number of components (must be 2, 3, or 4).
     * @tparam A Alignment policy (default: Gpu for std430 compatibility).
     */
    template<typename T, int N, AlignTag A = AlignTag::Gpu>
        requires(std::is_arithmetic_v<T> && (N >= 2) && (N <= 4))
    struct Vec;

    /** @brief 2-component vector specialisation. */
    template<typename T, AlignTag A>
    struct alignas(Detail::VecAlignment<T, 2, A>()) Vec<T, 2, A> {
        T x; /**< First component. */
        T y; /**< Second component. */

        [[nodiscard]] constexpr auto&& operator[](this auto&& self, int i) {
            if !consteval {
                return (&self.x)[i];
            }
            switch (i) {
            case 0:
                return self.x;
            case 1:
                return self.y;
            default:
                __builtin_unreachable();
            }
        }

        [[nodiscard]] friend constexpr bool operator==(const Vec&, const Vec&) = default;
    };

    /** @brief 3-component vector specialisation. */
    template<typename T, AlignTag A>
    struct alignas(Detail::VecAlignment<T, 3, A>()) Vec<T, 3, A> {
        T x; /**< First component. */
        T y; /**< Second component. */
        T z; /**< Third component. */

        [[nodiscard]] constexpr auto&& operator[](this auto&& self, int i) {
            if !consteval {
                return (&self.x)[i];
            }
            switch (i) {
            case 0:
                return self.x;
            case 1:
                return self.y;
            case 2:
                return self.z;
            default:
                __builtin_unreachable();
            }
        }

        [[nodiscard]] friend constexpr bool operator==(const Vec&, const Vec&) = default;
    };

    /** @brief 4-component vector specialisation. */
    template<typename T, AlignTag A>
    struct alignas(Detail::VecAlignment<T, 4, A>()) Vec<T, 4, A> {
        T x; /**< First component. */
        T y; /**< Second component. */
        T z; /**< Third component. */
        T w; /**< Fourth component. */

        [[nodiscard]] constexpr auto&& operator[](this auto&& self, int i) {
            if !consteval {
                return (&self.x)[i];
            }
            switch (i) {
            case 0:
                return self.x;
            case 1:
                return self.y;
            case 2:
                return self.z;
            case 3:
                return self.w;
            default:
                __builtin_unreachable();
            }
        }

        [[nodiscard]] friend constexpr bool operator==(const Vec&, const Vec&) = default;
    };

    /// @}

    /// @name Vector concept and compile-time queries
    /// @{

    /**
     * @brief A type whose nonstatic data members are all the same arithmetic
     *        scalar (>= 2 of them) and that supports arithmetic subscript access.
     */
    template<typename T>
    concept HomogeneousVec =
        Traits::Homogeneous<T> && Traits::GetHomogeneousMemberCount<T>() >= 2 &&
        requires(T v, int i) {
            { v[i] };
        } && std::is_arithmetic_v<std::remove_cvref_t<decltype(std::declval<T>()[0])>>;

    /** @brief Component count of a homogeneous vector. */
    template<HomogeneousVec V>
    inline constexpr int VecDim = static_cast<int>(Traits::GetHomogeneousMemberCount<V>());

    /** @brief Scalar element type of a homogeneous vector. */
    template<HomogeneousVec V>
    using ScalarOf = std::remove_cvref_t<decltype(std::declval<V>()[0])>;

    /// @}

    /// @name Generic vector operators (reflection-generated via `template for`)
    /// @{

    /// @brief Unary plus (identity).
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator+(V a) {
        return a;
    }

    /// @brief Component-wise addition.
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator+(V a, V b) {
        V r;
        for (int i = 0; i < VecDim<V>; ++i) r[i] = a[i] + b[i];
        return r;
    }

    /// @brief Component-wise subtraction.
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator-(V a, V b) {
        V r;
        for (int i = 0; i < VecDim<V>; ++i) r[i] = a[i] - b[i];
        return r;
    }

    /// @brief Component-wise multiplication (Hadamard product).
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator*(V a, V b) {
        V r;
        for (int i = 0; i < VecDim<V>; ++i) r[i] = a[i] * b[i];
        return r;
    }

    /// @brief Component-wise division.
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator/(V a, V b) {
        V r;
        for (int i = 0; i < VecDim<V>; ++i) r[i] = a[i] / b[i];
        return r;
    }

    /// @brief Scalar multiplication (v * s).
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator*(V a, ScalarOf<V> s) {
        V r;
        for (int i = 0; i < VecDim<V>; ++i) r[i] = a[i] * s;
        return r;
    }

    /// @brief Scalar multiplication (s * v).
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator*(ScalarOf<V> s, V a) {
        return a * s;
    }

    /// @brief Scalar division (v / s).
    template<HomogeneousVec V>
    [[nodiscard]] constexpr V operator/(V a, ScalarOf<V> s) {
        V r;
        for (int i = 0; i < VecDim<V>; ++i) r[i] = a[i] / s;
        return r;
    }

    /// @brief Unary negation (signed types only).
    template<HomogeneousVec V>
        requires std::is_signed_v<ScalarOf<V>>
    [[nodiscard]] constexpr V operator-(V a) {
        V r;
        for (int i = 0; i < VecDim<V>; ++i) r[i] = -a[i];
        return r;
    }

    /// @name Compound-assignment operators
    /// @{
    template<HomogeneousVec V> constexpr V& operator+=(V& a, V b)          { return a = a + b; }
    template<HomogeneousVec V> constexpr V& operator-=(V& a, V b)          { return a = a - b; }
    template<HomogeneousVec V> constexpr V& operator*=(V& a, V b)          { return a = a * b; }
    template<HomogeneousVec V> constexpr V& operator/=(V& a, V b)          { return a = a / b; }
    template<HomogeneousVec V> constexpr V& operator*=(V& a, ScalarOf<V> s){ return a = a * s; }
    template<HomogeneousVec V> constexpr V& operator/=(V& a, ScalarOf<V> s){ return a = a / s; }
    /// @}

    /// @}

} // namespace Mashiro
