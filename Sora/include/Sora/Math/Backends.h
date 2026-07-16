/**
 * @file Backends.h
 * @brief Scalar and fixed-width SIMD CPU math backends.
 * @ingroup Math
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <meta>
#include <string>
#include <type_traits>

#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Math/ConstexprMath.h>
#include <Sora/Math/Simd.h>

namespace Sora {

    namespace Math {

        /**
         * @brief Open specialization points for mapping user-defined numeric carriers to computation backends.
         *
         * @details Specialize @ref Backend for a cv-unqualified carrier type and expose the selected backend as
         * @c Type. The backend may implement any subset of the primary operations; unavailable operations are removed
         * from the corresponding CPO's overload set.
         *
         * @code{.cpp}
         * struct NumberBackend;
         *
         * template<>
         * struct Sora::Math::Hook::Backend<Number> {
         *     using Type = NumberBackend;
         * };
         * @endcode
         */
        namespace Hook {

            /**
             * @brief Map a numeric carrier to a user-defined backend through a nested @c Type alias.
             * @tparam Carrier Cv-unqualified carrier type being extended.
             */
            template<typename Carrier>
            struct Backend;

        } // namespace Hook

        namespace Backend {

            /**
             * @brief Stateless CPU backend for built-in arithmetic scalar types.
             *
             * @details Scalar operands are passed by value: they are at most register-sized, and a reference-preserving
             * interface would incorrectly expose argument value categories as mathematical result types.
             */
            struct ScalarCPU {
                static constexpr bool kIsScalar = true; /**< This backend operates on scalar values. */
                static constexpr bool kIsSimd = false;  /**< This backend does not operate on SIMD carriers. */
                static constexpr int kPriority = 0;     /**< Priority used when joining heterogeneous backends. */

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar... Ts>
                [[nodiscard]] static constexpr auto Add(T x, Ts... args) noexcept {
                    using R = std::common_type_t<T, Ts...>;
                    return (static_cast<R>(x) + ... + static_cast<R>(args));
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar... Ts>
                [[nodiscard]] static constexpr auto Mul(T x, Ts... args) noexcept {
                    using R = std::common_type_t<T, Ts...>;
                    return (static_cast<R>(x) * ... * static_cast<R>(args));
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U>
                [[nodiscard]] static constexpr auto Sub(T x, U y) noexcept {
                    using R = std::common_type_t<T, U>;
                    return static_cast<R>(x) - static_cast<R>(y);
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U>
                [[nodiscard]] static constexpr auto Div(T x, U y) noexcept {
                    using R = std::common_type_t<T, U>;
                    return static_cast<R>(x) / static_cast<R>(y);
                }

                template<Sora::Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Neg(T x) noexcept {
                    return -x;
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Inv(T x) noexcept {
                    return T{1} / x;
                }

                template<Sora::Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Square(T x) noexcept {
                    return x * x;
                }

                template<Sora::Concept::NumericScalar T>
                [[nodiscard]] static constexpr auto Abs(T x) noexcept {
                    if constexpr (std::unsigned_integral<T>) {
                        return x;
                    } else if constexpr (std::signed_integral<T>) {
                        using U = std::make_unsigned_t<T>;
                        const U bits = static_cast<U>(x);
                        return x < 0 ? U{0} - bits : bits;
                    } else {
                        return std::abs(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Sin(T x) noexcept {
                    if consteval {
                        return Detail::CtSinCos(x).sin;
                    } else {
                        return std::sin(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Cos(T x) noexcept {
                    if consteval {
                        return Detail::CtSinCos(x).cos;
                    } else {
                        return std::cos(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Exp(T x) noexcept {
                    if consteval {
                        return Detail::CtExp(x);
                    } else {
                        return std::exp(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Log(T x) noexcept {
                    if consteval {
                        return Detail::CtLog(x);
                    } else {
                        return std::log(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Pow(T base, T exponent) noexcept {
                    if consteval {
                        return Detail::CtPow(base, exponent);
                    } else {
                        return std::pow(base, exponent);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Sqrt(T x) noexcept {
                    if consteval {
                        return Detail::CtSqrt(x);
                    } else {
                        return std::sqrt(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Atan(T x) noexcept {
                    if consteval {
                        return Detail::CtAtan(x);
                    } else {
                        return std::atan(x);
                    }
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U>
                    requires std::floating_point<std::common_type_t<T, U>>
                [[nodiscard]] static constexpr auto Atan2(T y, U x) noexcept {
                    using R = std::common_type_t<T, U>;
                    if consteval {
                        return Detail::CtAtan2(static_cast<R>(y), static_cast<R>(x));
                    } else {
                        return std::atan2(static_cast<R>(y), static_cast<R>(x));
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Asin(T x) noexcept {
                    if consteval {
                        if (x < T{-1} || x > T{1}) {
                            return std::numeric_limits<T>::quiet_NaN();
                        }
                        return Detail::CtAtan2(x, Detail::CtSqrt(T{1} - x * x));
                    } else {
                        return std::asin(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Acos(T x) noexcept {
                    if consteval {
                        if (x < T{-1} || x > T{1}) {
                            return std::numeric_limits<T>::quiet_NaN();
                        }
                        return Detail::CtAtan2(Detail::CtSqrt(T{1} - x * x), x);
                    } else {
                        return std::acos(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Tan(T x) noexcept {
                    if consteval {
                        const auto result = Detail::CtSinCos(x);
                        return result.sin / result.cos;
                    } else {
                        return std::tan(x);
                    }
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U, Sora::Concept::NumericScalar V>
                [[nodiscard]] static constexpr auto Clamp(T value, U lower, V upper) noexcept {
                    using R = std::common_type_t<T, U, V>;
                    return std::clamp(static_cast<R>(value), static_cast<R>(lower), static_cast<R>(upper));
                }

                template<Sora::Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Saturate(T value) noexcept {
                    return Clamp(value, T{0}, T{1});
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U, Sora::Concept::NumericScalar V>
                    requires std::floating_point<std::common_type_t<T, U, V>>
                [[nodiscard]] static constexpr auto Lerp(T a, U b, V t) noexcept {
                    using R = std::common_type_t<T, U, V>;
                    return std::lerp(static_cast<R>(a), static_cast<R>(b), static_cast<R>(t));
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U, Sora::Concept::NumericScalar V>
                [[nodiscard]] static constexpr auto Fma(T a, U b, V c) noexcept {
                    using R = std::common_type_t<T, U, V>;
                    if constexpr (std::floating_point<R>) {
                        return std::fma(static_cast<R>(a), static_cast<R>(b), static_cast<R>(c));
                    } else {
                        return static_cast<R>(a) * static_cast<R>(b) + static_cast<R>(c);
                    }
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U, Sora::Concept::NumericScalar V>
                [[nodiscard]] static constexpr auto Mfs(T a, U b, V c) noexcept {
                    using R = std::common_type_t<T, U, V>;
                    if constexpr (std::floating_point<R>) {
                        return std::fma(static_cast<R>(a), static_cast<R>(b), -static_cast<R>(c));
                    } else {
                        return static_cast<R>(a) * static_cast<R>(b) - static_cast<R>(c);
                    }
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U, Sora::Concept::NumericScalar V>
                [[nodiscard]] static constexpr auto Nms(T a, U b, V c) noexcept {
                    using R = std::common_type_t<T, U, V>;
                    if constexpr (std::floating_point<R>) {
                        return std::fma(-static_cast<R>(a), static_cast<R>(b), -static_cast<R>(c));
                    } else {
                        return -static_cast<R>(a) * static_cast<R>(b) - static_cast<R>(c);
                    }
                }

                template<Sora::Concept::NumericScalar T, Sora::Concept::NumericScalar U, Sora::Concept::NumericScalar V>
                [[nodiscard]] static constexpr auto Nma(T a, U b, V c) noexcept {
                    using R = std::common_type_t<T, U, V>;
                    if constexpr (std::floating_point<R>) {
                        return std::fma(-static_cast<R>(a), static_cast<R>(b), static_cast<R>(c));
                    } else {
                        return -static_cast<R>(a) * static_cast<R>(b) + static_cast<R>(c);
                    }
                }

                template<Concept::SignedArithmetic T>
                [[nodiscard]] static constexpr T Sign(T x) noexcept {
                    return x > T(0) ? T(1) : (x < T(0) ? T(-1) : T(0));
                }
            };

            namespace Concept {

                /** @brief Fixed-width arithmetic SIMD carrier containing exactly @p N lanes. */
                template<typename T, std::size_t N>
                concept FixedSimdValue = Simd::SimdVecType<std::remove_cvref_t<T>> &&
                                         Sora::Sora::Concept::NumericScalar<typename std::remove_cvref_t<T>::ValueType> &&
                                         std::remove_cvref_t<T>::kSize.value == N;

                /** @brief Fixed-width floating-point SIMD carrier containing exactly @p N lanes. */
                template<typename T, std::size_t N>
                concept FixedSimdFloatingValue =
                    FixedSimdValue<T, N> && std::floating_point<typename std::remove_cvref_t<T>::ValueType>;

                /** @brief Fixed-width signed arithmetic SIMD carrier containing exactly @p N lanes. */
                template<typename T, std::size_t N>
                concept FixedSimdSignedValue =
                    FixedSimdValue<T, N> && std::is_signed_v<typename std::remove_cvref_t<T>::ValueType>;

            } // namespace Concept

            /**
             * @brief CPU backend for arithmetic SIMD vectors with exactly @p N lanes.
             * @tparam N Compile-time lane count shared by every operand.
             */
            template<std::size_t N>
            struct FixedSimdCPU {
                static_assert(N > 0, "FixedSimdCPU requires at least one lane");

                static constexpr bool kIsScalar = false; /**< This backend does not operate on scalar values. */
                static constexpr bool kIsSimd = true;    /**< This backend operates on SIMD carriers. */
                static constexpr std::size_t kSize = N;  /**< Number of lanes accepted by this specialization. */
                static constexpr int kPriority = 1;      /**< Priority used when joining heterogeneous backends. */

            private:
                template<typename T>
                static constexpr bool kCompatibleOperand =
                    Concept::FixedSimdValue<T, N> || Sora::Sora::Concept::NumericScalar<std::remove_cvref_t<T>>;

                template<typename T>
                [[nodiscard]] static consteval auto ElementType() {
                    if constexpr (Concept::FixedSimdValue<T, N>) {
                        return std::type_identity<typename std::remove_cvref_t<T>::ValueType>{};
                    } else {
                        return std::type_identity<std::remove_cvref_t<T>>{};
                    }
                }

                template<typename T>
                using ElementTypeOf = typename decltype(ElementType<T>())::type;

                template<typename First, typename... Rest>
                [[nodiscard]] static consteval auto VectorCarrier() {
                    if constexpr (Concept::FixedSimdValue<First, N>) {
                        return std::type_identity<std::remove_cvref_t<First>>{};
                    } else {
                        return VectorCarrier<Rest...>();
                    }
                }

                template<typename... Args>
                using ResultVector = Simd::Rebind<std::common_type_t<ElementTypeOf<Args>...>,
                                                  typename decltype(VectorCarrier<Args...>())::type>;

                template<typename R, typename T>
                [[nodiscard]] static constexpr R Convert(T&& value) noexcept {
                    if constexpr (Concept::FixedSimdValue<T, N>) {
                        return R(std::forward<T>(value));
                    } else {
                        return R(static_cast<typename R::ValueType>(value));
                    }
                }

                template<typename V, typename F>
                [[nodiscard]] static constexpr V Transform(const V& x, F fn) noexcept {
                    return V([&](auto index) { return fn(x[index]); });
                }

                template<typename V, typename F>
                [[nodiscard]] static constexpr V Transform(const V& x, const V& y, F fn) noexcept {
                    return V([&](auto index) { return fn(x[index], y[index]); });
                }

                template<typename V, typename F>
                [[nodiscard]] static constexpr V Transform(const V& x, const V& y, const V& z, F fn) noexcept {
                    return V([&](auto index) { return fn(x[index], y[index], z[index]); });
                }

            public:
                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Splat(typename V::ValueType value) noexcept {
                    return V(value);
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Load(const typename V::ValueType* values) noexcept {
                    return V([&](auto index) { return values[static_cast<std::size_t>(index)]; });
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V MaskedLoad(const typename V::ValueType* values,
                                                            const typename V::MaskType& active, V fallback) noexcept {
                    return Simd::Select(active, V::MaskedLoad(values, active), fallback);
                }

                template<Concept::FixedSimdValue<N> V>
                static constexpr void Store(typename V::ValueType* values, const V& value) noexcept {
                    for (std::size_t index = 0; index < N; ++index) {
                        values[index] = value[index];
                    }
                }

                template<Concept::FixedSimdValue<N> V>
                static constexpr void MaskedStore(typename V::ValueType* values, const V& value,
                                                  const typename V::MaskType& active) noexcept {
                    V::MaskedStore(value, values, active);
                }

                template<Concept::FixedSimdValue<N> V, typename M>
                [[nodiscard]] static constexpr V Select(const M& condition, const V& yes, const V& no) noexcept {
                    return Simd::Select(condition, yes, no);
                }

                template<typename First, typename... Rest>
                    requires(kCompatibleOperand<First> && (kCompatibleOperand<Rest> && ...) &&
                             (Concept::FixedSimdValue<First, N> || ... || Concept::FixedSimdValue<Rest, N>))
                [[nodiscard]] static constexpr auto Add(First&& first, Rest&&... rest) noexcept {
                    using R = ResultVector<First, Rest...>;
                    return (Convert<R>(std::forward<First>(first)) + ... + Convert<R>(std::forward<Rest>(rest)));
                }

                template<typename First, typename... Rest>
                    requires(kCompatibleOperand<First> && (kCompatibleOperand<Rest> && ...) &&
                             (Concept::FixedSimdValue<First, N> || ... || Concept::FixedSimdValue<Rest, N>))
                [[nodiscard]] static constexpr auto Mul(First&& first, Rest&&... rest) noexcept {
                    using R = ResultVector<First, Rest...>;
                    return (Convert<R>(std::forward<First>(first)) * ... * Convert<R>(std::forward<Rest>(rest)));
                }

                template<typename First, typename Second>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N>))
                [[nodiscard]] static constexpr auto Sub(First&& first, Second&& second) noexcept {
                    using R = ResultVector<First, Second>;
                    return Convert<R>(std::forward<First>(first)) - Convert<R>(std::forward<Second>(second));
                }

                template<typename First, typename Second>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N>))
                [[nodiscard]] static constexpr auto Div(First&& first, Second&& second) noexcept {
                    using R = ResultVector<First, Second>;
                    return Convert<R>(std::forward<First>(first)) / Convert<R>(std::forward<Second>(second));
                }

                [[nodiscard]] static constexpr auto Neg(Concept::FixedSimdValue<N> auto x) noexcept { return -x; }

                [[nodiscard]] static constexpr auto Inv(Concept::FixedSimdFloatingValue<N> auto x) noexcept {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return V(T(1)) / x;
                }

                [[nodiscard]] static constexpr auto Square(Concept::FixedSimdValue<N> auto x) noexcept { return x * x; }

                [[nodiscard]] static constexpr auto Abs(Concept::FixedSimdValue<N> auto x) noexcept {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    if constexpr (std::unsigned_integral<T>) {
                        return x;
                    } else if constexpr (std::signed_integral<T>) {
                        using R = Simd::Rebind<std::make_unsigned_t<T>, V>;
                        const R bits(x);
                        return Simd::Select(x < V(T(0)), R(typename R::ValueType{0}) - bits, bits);
                    } else {
                        return x.Fabs();
                    }
                }

                [[nodiscard]] static constexpr auto Sin(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Sin(x);
                }

                [[nodiscard]] static constexpr auto Cos(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Cos(x);
                }

                [[nodiscard]] static constexpr auto Exp(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Exp(x);
                }

                [[nodiscard]] static constexpr auto Log(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Log(x);
                }

                template<typename First, typename Second>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N>) &&
                             std::floating_point<typename ResultVector<First, Second>::ValueType>)
                [[nodiscard]] static constexpr auto Pow(First&& base, Second&& exponent) noexcept {
                    using R = ResultVector<First, Second>;
                    return Simd::Pow(Convert<R>(std::forward<First>(base)), Convert<R>(std::forward<Second>(exponent)));
                }

                [[nodiscard]] static constexpr auto Sqrt(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Sqrt(x);
                }

                [[nodiscard]] static constexpr auto Atan(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Atan(x);
                }

                template<typename First, typename Second>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N>) &&
                             std::floating_point<typename ResultVector<First, Second>::ValueType>)
                [[nodiscard]] static constexpr auto Atan2(First&& y, Second&& x) noexcept {
                    using R = ResultVector<First, Second>;
                    return Simd::Atan2(Convert<R>(std::forward<First>(y)), Convert<R>(std::forward<Second>(x)));
                }

                [[nodiscard]] static constexpr auto Asin(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Asin(x);
                }

                [[nodiscard]] static constexpr auto Acos(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Acos(x);
                }

                [[nodiscard]] static constexpr auto Tan(const Concept::FixedSimdFloatingValue<N> auto& x) noexcept {
                    return Simd::Tan(x);
                }

                template<typename Value, typename Lower, typename Upper>
                    requires(kCompatibleOperand<Value> && kCompatibleOperand<Lower> && kCompatibleOperand<Upper> &&
                             (Concept::FixedSimdValue<Value, N> || Concept::FixedSimdValue<Lower, N> ||
                              Concept::FixedSimdValue<Upper, N>))
                [[nodiscard]] static constexpr auto Clamp(Value&& value, Lower&& lower, Upper&& upper) noexcept {
                    using R = ResultVector<Value, Lower, Upper>;
                    return Simd::Clamp(Convert<R>(std::forward<Value>(value)), Convert<R>(std::forward<Lower>(lower)),
                                       Convert<R>(std::forward<Upper>(upper)));
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Saturate(V value) noexcept {
                    using T = typename V::ValueType;
                    return Clamp(value, V(T(0)), V(T(1)));
                }

                template<typename First, typename Second, typename Third>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> && kCompatibleOperand<Third> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N> ||
                              Concept::FixedSimdValue<Third, N>) &&
                             std::floating_point<typename ResultVector<First, Second, Third>::ValueType>)
                [[nodiscard]] static constexpr auto Lerp(First&& a, Second&& b, Third&& t) noexcept {
                    using V = ResultVector<First, Second, Third>;
                    using T = typename V::ValueType;
                    const V left = Convert<V>(std::forward<First>(a));
                    const V right = Convert<V>(std::forward<Second>(b));
                    const V weight = Convert<V>(std::forward<Third>(t));
                    return Transform(left, right, weight,
                                     [](T x, T y, T interpolation) { return std::lerp(x, y, interpolation); });
                }

                template<typename First, typename Second, typename Third>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> && kCompatibleOperand<Third> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N> ||
                              Concept::FixedSimdValue<Third, N>))
                [[nodiscard]] static constexpr auto Fma(First&& a, Second&& b, Third&& c) noexcept {
                    using V = ResultVector<First, Second, Third>;
                    using T = typename V::ValueType;
                    const V left = Convert<V>(std::forward<First>(a));
                    const V right = Convert<V>(std::forward<Second>(b));
                    const V addend = Convert<V>(std::forward<Third>(c));
                    if constexpr (std::floating_point<T>) {
                        return Simd::Fma(left, right, addend);
                    } else {
                        return left * right + addend;
                    }
                }

                template<typename First, typename Second, typename Third>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> && kCompatibleOperand<Third> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N> ||
                              Concept::FixedSimdValue<Third, N>))
                [[nodiscard]] static constexpr auto Mfs(First&& a, Second&& b, Third&& c) noexcept {
                    using V = ResultVector<First, Second, Third>;
                    using T = typename V::ValueType;
                    const V left = Convert<V>(std::forward<First>(a));
                    const V right = Convert<V>(std::forward<Second>(b));
                    const V subtrahend = Convert<V>(std::forward<Third>(c));
                    if constexpr (std::floating_point<T>) {
                        return Simd::Fma(left, right, -subtrahend);
                    } else {
                        return left * right - subtrahend;
                    }
                }

                template<typename First, typename Second, typename Third>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> && kCompatibleOperand<Third> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N> ||
                              Concept::FixedSimdValue<Third, N>))
                [[nodiscard]] static constexpr auto Nms(First&& a, Second&& b, Third&& c) noexcept {
                    using V = ResultVector<First, Second, Third>;
                    using T = typename V::ValueType;
                    const V left = Convert<V>(std::forward<First>(a));
                    const V right = Convert<V>(std::forward<Second>(b));
                    const V subtrahend = Convert<V>(std::forward<Third>(c));
                    if constexpr (std::floating_point<T>) {
                        return Simd::Fma(-left, right, -subtrahend);
                    } else {
                        return -left * right - subtrahend;
                    }
                }

                template<typename First, typename Second, typename Third>
                    requires(kCompatibleOperand<First> && kCompatibleOperand<Second> && kCompatibleOperand<Third> &&
                             (Concept::FixedSimdValue<First, N> || Concept::FixedSimdValue<Second, N> ||
                              Concept::FixedSimdValue<Third, N>))
                [[nodiscard]] static constexpr auto Nma(First&& a, Second&& b, Third&& c) noexcept {
                    using V = ResultVector<First, Second, Third>;
                    using T = typename V::ValueType;
                    const V left = Convert<V>(std::forward<First>(a));
                    const V right = Convert<V>(std::forward<Second>(b));
                    const V addend = Convert<V>(std::forward<Third>(c));
                    if constexpr (std::floating_point<T>) {
                        return Simd::Fma(-left, right, addend);
                    } else {
                        return -left * right + addend;
                    }
                }

                [[nodiscard]] static constexpr auto Sign(const Concept::FixedSimdSignedValue<N> auto& x) noexcept {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    const V zero(T(0));
                    return Simd::Select(x > zero, V(T(1)), Simd::Select(x < zero, V(T(-1)), zero));
                }
            };

        } // namespace Backend

        namespace Meta {

            /**
             * @brief Select the zero-overhead computation backend for a reflected carrier type.
             * @tparam CarrierInfo Reflection of a scalar or supported SIMD carrier type.
             * @return Reflection of the backend type selected for @p CarrierInfo.
             */
            template<std::meta::info CarrierInfo>
            consteval std::meta::info GetBackend() {
                using namespace std::meta;

                constexpr info carrier = dealias(remove_cvref(CarrierInfo));
                using Carrier = [:carrier:];

                if constexpr (requires { typename Hook::Backend<Carrier>::Type; }) {
                    using Selected [[maybe_unused]] = typename Hook::Backend<Carrier>::Type;
                    return ^^Selected;
                }

                if constexpr (is_arithmetic_type(carrier)) {
                    return ^^Backend::ScalarCPU;
                }

                if constexpr (has_template_arguments(carrier) &&
                              template_of(carrier) == ^^Sora::Math::Simd::BasicVector) {
                    constexpr std::size_t width = static_cast<std::size_t>(Carrier::kSize.value);
                    return substitute(^^Backend::FixedSimdCPU, {reflect_constant(width)});
                }

                throw std::define_static_string(std::format(
                    "Sora::Math::Meta::GetBackend: unsupported carrier type '{}'", display_string_of(carrier)));
            }

            static_assert(GetBackend<^^int>() == ^^Backend::ScalarCPU, "GetBackend must return a std::meta::info");
            static_assert(GetBackend<^^Sora::Simd::F32<2>>() == ^^Backend::FixedSimdCPU<2>,
                          "GetBackend must return a std::meta::info");

        } // namespace Meta

        namespace Traits {

            /** @brief Backend type selected at compile time for carrier type @p P. */
            template<typename P>
            using BackendTypeOf = [:Meta::GetBackend<^^P>():];

            static_assert(std::same_as<BackendTypeOf<int>, Backend::ScalarCPU>,
                          "BackendTypeOf must return a backend type");
            static_assert(std::same_as<BackendTypeOf<Sora::Simd::F32<2>>, Backend::FixedSimdCPU<2>>,
                          "BackendTypeOf must return a backend type");

        } // namespace Traits

    } // namespace Math

    namespace Meta {

        inline namespace Math {

            /** @brief SIMD carrier whose element type belongs to a floating-point domain. */
            template<typename T>
            concept FloatingSimdValue = Sora::Math::Simd::SimdVecType<std::remove_cvref_t<T>> &&
                                        std::floating_point<typename std::remove_cvref_t<T>::ValueType>;

            /** @brief Scalar or SIMD carrier over which real-valued automatic differentiation is defined. */
            template<typename T>
            concept DifferentiableValue = std::floating_point<std::remove_cvref_t<T>> || FloatingSimdValue<T>;

            using Sora::Math::Meta::GetBackend;

        } // namespace Math

    } // namespace Meta

    namespace Traits {

        inline namespace Math {

            using Sora::Math::Traits::BackendTypeOf;

        } // namespace Math

    } // namespace Traits

    namespace Concept {

        inline namespace Math {

            using Sora::Math::Backend::Concept::FixedSimdFloatingValue;
            using Sora::Math::Backend::Concept::FixedSimdSignedValue;
            using Sora::Math::Backend::Concept::FixedSimdValue;

        } // namespace Math

    } // namespace Concept

} // namespace Sora
