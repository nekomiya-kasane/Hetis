/**
 * @file Vector.h
 * @brief Trivial fixed-size vectors over scalar, SIMD, and extensible mathematical carriers.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <meta>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    namespace Detail {

        template<typename T>
        concept ZeroInitializable = std::constructible_from<T, int> || requires { T(typename T::ValueType{0}); };

        template<typename T>
            requires ZeroInitializable<T>
        [[nodiscard]] constexpr T ZeroCarrier() noexcept {
            if constexpr (std::constructible_from<T, int>) {
                return T{0};
            } else {
                return T(typename T::ValueType{0});
            }
        }

    } // namespace Detail

    /** @brief Trivially copyable carrier suitable for inline fixed-size mathematical values. */
    template<typename T>
    concept ValueCarrier = std::is_object_v<T> && std::same_as<T, std::remove_cv_t<T>> && std::copyable<T> &&
                           std::is_trivially_copyable_v<T> && Detail::ZeroInitializable<T>;

    /**
     * @brief Fixed-size free vector in an @p N-dimensional module over carrier @p T.
     * @tparam T Scalar, SIMD pack, or user carrier supported by the mathematical CPO backend.
     * @tparam N Positive compile-time dimension.
     * @details Storage is exactly one @c std::array<T,N>. A SIMD carrier therefore represents the same operation over
     * several independent vectors in coordinate-major form; no runtime backend or layout tag enters the value type.
     */
    template<ValueCarrier T, size_t N>
        requires(N > 0)
    class Vector {
    public:
        using CarrierType = T;                  /**< Mathematical component carrier. */
        static constexpr size_t kDimension = N; /**< Number of vector components. */

        /** @brief Construct the additive identity. */
        constexpr Vector() noexcept { values_.fill(Detail::ZeroCarrier<T>()); }

        /** @brief Construct from the exact component array. */
        constexpr explicit Vector(std::array<T, N> values) noexcept(std::is_nothrow_move_constructible_v<T>)
            : values_(std::move(values)) {}

        /** @brief Construct from exactly @p N component values. */
        template<typename... U>
            requires(sizeof...(U) == N && (std::constructible_from<T, U &&> && ...))
        constexpr explicit(!(std::convertible_to<U&&, T> && ...))
            Vector(U&&... values) noexcept((std::is_nothrow_constructible_v<T, U&&> && ...))
            : values_{T(std::forward<U>(values))...} {}

        /** @brief Access one component while preserving cv-ref qualification. @pre @p index is less than @c N. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) operator[](this Self&& self, size_t index) noexcept {
            return std::forward_like<Self>(self.values_[index]);
        }

        /** @brief Return the contiguous component array while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Components(this Self&& self) noexcept {
            return std::forward_like<Self>(self.values_);
        }

        /** @brief Return the first component address from an lvalue object. */
        template<typename Self>
            requires std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto Data(this Self&& self) noexcept {
            return self.values_.data();
        }

    private:
        std::array<T, N> values_{};
    };

    namespace Detail {

        template<typename T, typename U>
        using AddResult = std::remove_cvref_t<decltype(Sora::Math::Add(std::declval<T>(), std::declval<U>()))>;

        template<typename T, typename U>
        using SubResult = std::remove_cvref_t<decltype(Sora::Math::Sub(std::declval<T>(), std::declval<U>()))>;

        template<typename T, typename U>
        using MulResult = std::remove_cvref_t<decltype(Sora::Math::Mul(std::declval<T>(), std::declval<U>()))>;

        template<typename T, typename U>
        using DivResult = std::remove_cvref_t<decltype(Sora::Math::Div(std::declval<T>(), std::declval<U>()))>;

    } // namespace Detail

    /** @brief Add two vectors componentwise. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) { Sora::Math::Add(left, right); } && ValueCarrier<Detail::AddResult<T, U>>
    [[nodiscard]] constexpr auto operator+(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(Sora::Math::Add(left[0], right[0]))) -> Vector<Detail::AddResult<T, U>, N> {
        Vector<Detail::AddResult<T, U>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Add(left[i], right[i]);
        }
        return result;
    }

    /** @brief Subtract two vectors componentwise. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } && ValueCarrier<Detail::SubResult<T, U>>
    [[nodiscard]] constexpr auto operator-(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(Sora::Math::Sub(left[0], right[0]))) -> Vector<Detail::SubResult<T, U>, N> {
        Vector<Detail::SubResult<T, U>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Sub(left[i], right[i]);
        }
        return result;
    }

    /** @brief Negate every vector component. */
    template<ValueCarrier T, size_t N>
        requires requires(T value) { Sora::Math::Neg(value); } &&
                 ValueCarrier<std::remove_cvref_t<decltype(Sora::Math::Neg(std::declval<T>()))>>
    [[nodiscard]] constexpr auto operator-(const Vector<T, N>& value) noexcept(noexcept(Sora::Math::Neg(value[0])))
        -> Vector<std::remove_cvref_t<decltype(Sora::Math::Neg(std::declval<T>()))>, N> {
        using R = std::remove_cvref_t<decltype(Sora::Math::Neg(std::declval<T>()))>;
        Vector<R, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Neg(value[i]);
        }
        return result;
    }

    /** @brief Multiply a vector by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t N>
        requires requires(T value, S scale) { Sora::Math::Mul(value, scale); } && ValueCarrier<Detail::MulResult<T, S>>
    [[nodiscard]] constexpr auto operator*(const Vector<T, N>& value,
                                           const S& scale) noexcept(noexcept(Sora::Math::Mul(value[0], scale)))
        -> Vector<Detail::MulResult<T, S>, N> {
        Vector<Detail::MulResult<T, S>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Mul(value[i], scale);
        }
        return result;
    }

    /** @brief Multiply a vector by a scalar or broadcast-compatible carrier. */
    template<typename S, ValueCarrier T, size_t N>
        requires requires(S scale, T value) { Sora::Math::Mul(scale, value); } && ValueCarrier<Detail::MulResult<S, T>>
    [[nodiscard]] constexpr auto
    operator*(const S& scale, const Vector<T, N>& value) noexcept(noexcept(Sora::Math::Mul(scale, value[0])))
        -> Vector<Detail::MulResult<S, T>, N> {
        Vector<Detail::MulResult<S, T>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Mul(scale, value[i]);
        }
        return result;
    }

    /** @brief Divide a vector by a scalar or broadcast-compatible carrier. */
    template<ValueCarrier T, typename S, size_t N>
        requires requires(T value, S scale) { Sora::Math::Div(value, scale); } && ValueCarrier<Detail::DivResult<T, S>>
    [[nodiscard]] constexpr auto operator/(const Vector<T, N>& value,
                                           const S& scale) noexcept(noexcept(Sora::Math::Div(value[0], scale)))
        -> Vector<Detail::DivResult<T, S>, N> {
        Vector<Detail::DivResult<T, S>, N> result;
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{0}, N))) {
            result[i] = Sora::Math::Div(value[i], scale);
        }
        return result;
    }

    /** @brief Return the lane-wise conjunction of component equalities. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) {
            left == right;
            (left == right) & (left == right);
        }
    [[nodiscard]] constexpr auto operator==(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(left[0] == right[0]) && noexcept((left[0] == right[0]) & (left[0] == right[0]))) {
        auto equal = left[0] == right[0];
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{1}, N))) {
            equal = equal & (left[i] == right[i]);
        }
        return equal;
    }

    /** @brief Return the lane-wise negation of vector equality. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(const Vector<T, N>& left, const Vector<U, N>& right) { !(left == right); }
    [[nodiscard]] constexpr auto operator!=(const Vector<T, N>& left,
                                            const Vector<U, N>& right) noexcept(noexcept(!(left == right))) {
        return !(left == right);
    }

    /** @brief Compute the inner product, using fused multiply-add where the backend provides it. */
    template<ValueCarrier T, ValueCarrier U, size_t N>
        requires requires(T left, U right) {
            Sora::Math::Mul(left, right);
            Sora::Math::Fma(left, right, Sora::Math::Mul(left, right));
            Sora::Math::Add(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
        }
    [[nodiscard]] constexpr auto Dot(const Vector<T, N>& left, const Vector<U, N>& right) noexcept(
        noexcept(Sora::Math::Mul(left[0], right[0])) &&
        noexcept(Sora::Math::Fma(left[0], right[0], Sora::Math::Mul(left[0], right[0])))) {
        auto result = Sora::Math::Mul(left[0], right[0]);
        template for (constexpr size_t i : std::define_static_array(std::views::iota(size_t{1}, N))) {
            if consteval {
                result = Sora::Math::Add(Sora::Math::Mul(left[i], right[i]), result);
            } else {
                result = Sora::Math::Fma(left[i], right[i], result);
            }
        }
        return result;
    }

    /** @brief Compute the squared Euclidean norm. */
    template<ValueCarrier T, size_t N>
        requires requires(const Vector<T, N>& value) { Sora::Math::Dot(value, value); }
    [[nodiscard]] constexpr auto
    SquaredNorm(const Vector<T, N>& value) noexcept(noexcept(Sora::Math::Dot(value, value))) {
        return Sora::Math::Dot(value, value);
    }

    /** @brief Compute the Euclidean norm when the selected carrier backend provides square root. */
    template<ValueCarrier T, size_t N>
        requires requires(const Vector<T, N>& value) { Sora::Math::Sqrt(Sora::Math::SquaredNorm(value)); }
    [[nodiscard]] constexpr auto
    Norm(const Vector<T, N>& value) noexcept(noexcept(Sora::Math::Sqrt(Sora::Math::SquaredNorm(value)))) {
        return Sora::Math::Sqrt(Sora::Math::SquaredNorm(value));
    }

    /** @brief Compute the three-dimensional cross product. */
    template<ValueCarrier T, ValueCarrier U>
        requires requires(T left, U right) {
            Sora::Math::Mul(left, right);
            Sora::Math::Mfs(left, right, Sora::Math::Mul(left, right));
            Sora::Math::Sub(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
        }
    [[nodiscard]] constexpr auto Cross(const Vector<T, 3>& left, const Vector<U, 3>& right) noexcept(
        noexcept(Sora::Math::Mfs(left[1], right[2], Sora::Math::Mul(left[2], right[1])))) {
        using R = std::remove_cvref_t<decltype(Sora::Math::Mfs(std::declval<T>(), std::declval<U>(),
                                                               Sora::Math::Mul(std::declval<T>(), std::declval<U>())))>;
        if consteval {
            return Vector<R, 3>{
                Sora::Math::Sub(Sora::Math::Mul(left[1], right[2]), Sora::Math::Mul(left[2], right[1])),
                Sora::Math::Sub(Sora::Math::Mul(left[2], right[0]), Sora::Math::Mul(left[0], right[2])),
                Sora::Math::Sub(Sora::Math::Mul(left[0], right[1]), Sora::Math::Mul(left[1], right[0]))};
        } else {
            return Vector<R, 3>{Sora::Math::Mfs(left[1], right[2], Sora::Math::Mul(left[2], right[1])),
                                Sora::Math::Mfs(left[2], right[0], Sora::Math::Mul(left[0], right[2])),
                                Sora::Math::Mfs(left[0], right[1], Sora::Math::Mul(left[1], right[0]))};
        }
    }

} // namespace Sora::Math
