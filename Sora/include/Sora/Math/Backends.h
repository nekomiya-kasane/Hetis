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
#include <meta>
#include <string>
#include <type_traits>

#include <Sora/Core/Traits/TypeTraits.h>
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
                static constexpr bool kIsScalar = true;  /**< This backend operates on scalar values. */
                static constexpr bool kIsSimd = false;   /**< This backend does not operate on SIMD carriers. */

                template<Concept::NumericScalar T, std::same_as<T>... Ts>
                [[nodiscard]] static constexpr T Add(T x, Ts... args) noexcept {
                    return (x + ... + args);
                }

                template<Concept::NumericScalar T, std::same_as<T>... Ts>
                [[nodiscard]] static constexpr T Mul(T x, Ts... args) noexcept {
                    return (x * ... * args);
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Sub(T x, T y) noexcept {
                    return x - y;
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Div(T x, T y) noexcept {
                    return x / y;
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Neg(T x) noexcept {
                    return -x;
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Inv(T x) noexcept {
                    return T{1} / x;
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Square(T x) noexcept {
                    return x * x;
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Abs(T x) noexcept {
                    if constexpr (std::unsigned_integral<T>) {
                        return x;
                    } else {
                        return std::abs(x);
                    }
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Sin(T x) noexcept {
                    return std::sin(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Cos(T x) noexcept {
                    return std::cos(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Exp(T x) noexcept {
                    return std::exp(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Log(T x) noexcept {
                    return std::log(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Pow(T base, T exponent) noexcept {
                    return std::pow(base, exponent);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Sqrt(T x) noexcept {
                    return std::sqrt(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Atan(T x) noexcept {
                    return std::atan(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Atan2(T y, T x) noexcept {
                    return std::atan2(y, x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Asin(T x) noexcept {
                    return std::asin(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Acos(T x) noexcept {
                    return std::acos(x);
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Tan(T x) noexcept {
                    return std::tan(x);
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Clamp(T value, T lower, T upper) noexcept {
                    return std::clamp(value, lower, upper);
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Saturate(T value) noexcept {
                    return Clamp(value, T{0}, T{1});
                }

                template<std::floating_point T>
                [[nodiscard]] static constexpr T Lerp(T a, T b, T t) noexcept {
                    return std::lerp(a, b, t);
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Fma(T a, T b, T c) noexcept {
                    if constexpr (std::floating_point<T>) {
                        return std::fma(a, b, c);
                    } else {
                        return a * b + c;
                    }
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Mfs(T a, T b, T c) noexcept {
                    if constexpr (std::floating_point<T>) {
                        return std::fma(a, b, -c);
                    } else {
                        return a * b - c;
                    }
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Nms(T a, T b, T c) noexcept {
                    if constexpr (std::floating_point<T>) {
                        return std::fma(-a, b, -c);
                    } else {
                        return -a * b - c;
                    }
                }

                template<Concept::NumericScalar T>
                [[nodiscard]] static constexpr T Nma(T a, T b, T c) noexcept {
                    if constexpr (std::floating_point<T>) {
                        return std::fma(-a, b, c);
                    } else {
                        return -a * b + c;
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
                                          Sora::Concept::NumericScalar<typename std::remove_cvref_t<T>::ValueType> &&
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

                static constexpr bool kIsScalar = false;  /**< This backend does not operate on scalar values. */
                static constexpr bool kIsSimd = true;     /**< This backend operates on SIMD carriers. */
                static constexpr std::size_t kSize = N;   /**< Number of lanes accepted by this specialization. */

            private:
                template<typename V, typename F>
                [[nodiscard]] static constexpr V Transform(const V& x, F fn) {
                    return V([&](auto index) { return fn(x[index]); });
                }

                template<typename V, typename F>
                [[nodiscard]] static constexpr V Transform(const V& x, const V& y, F fn) {
                    return V([&](auto index) { return fn(x[index], y[index]); });
                }

                template<typename V, typename F>
                [[nodiscard]] static constexpr V Transform(const V& x, const V& y, const V& z, F fn) {
                    return V([&](auto index) { return fn(x[index], y[index], z[index]); });
                }

            public:
                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Splat(typename V::ValueType value) {
                    return V(value);
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Load(const typename V::ValueType* values) {
                    return V([&](auto index) { return values[static_cast<std::size_t>(index)]; });
                }

                template<Concept::FixedSimdValue<N> V, typename M>
                [[nodiscard]] static constexpr V MaskedLoad(const typename V::ValueType* values, const M& active,
                                                            V fallback) {
                    return Simd::Select(active, Load<V>(values), fallback);
                }

                template<Concept::FixedSimdValue<N> V>
                static constexpr void Store(typename V::ValueType* values, const V& value) {
                    for (std::size_t index = 0; index < N; ++index) {
                        values[index] = value[index];
                    }
                }

                template<Concept::FixedSimdValue<N> V, typename M>
                static constexpr void MaskedStore(typename V::ValueType* values, const V& value, const M& active) {
                    Store(values, Simd::Select(active, value, Load<V>(values)));
                }

                template<Concept::FixedSimdValue<N> V, typename M>
                [[nodiscard]] static constexpr V Select(const M& condition, const V& yes, const V& no) {
                    return Simd::Select(condition, yes, no);
                }

                template<typename V, typename... Vs>
                    requires Concept::FixedSimdValue<V, N> && (std::same_as<V, Vs> && ...)
                [[nodiscard]] static constexpr V Add(V x, Vs... args) {
                    return (x + ... + args);
                }

                template<typename V, typename... Vs>
                    requires Concept::FixedSimdValue<V, N> && (std::same_as<V, Vs> && ...)
                [[nodiscard]] static constexpr V Mul(V x, Vs... args) {
                    return (x * ... * args);
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Sub(V x, V y) {
                    return x - y;
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Div(V x, V y) {
                    return x / y;
                }

                [[nodiscard]] static constexpr auto Neg(Concept::FixedSimdValue<N> auto x) { return -x; }

                [[nodiscard]] static constexpr auto Inv(Concept::FixedSimdValue<N> auto x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return V(T(1)) / x;
                }

                [[nodiscard]] static constexpr auto Square(Concept::FixedSimdValue<N> auto x) { return x * x; }

                [[nodiscard]] static constexpr auto Abs(Concept::FixedSimdValue<N> auto x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    if constexpr (std::unsigned_integral<T>) {
                        return x;
                    } else if constexpr (std::signed_integral<T>) {
                        return Simd::Abs(x);
                    } else {
                        return Transform(x, [](T value) { return std::abs(value); });
                    }
                }

                [[nodiscard]] static constexpr auto Sin(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::sin(value); });
                }

                [[nodiscard]] static constexpr auto Cos(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::cos(value); });
                }

                [[nodiscard]] static constexpr auto Exp(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::exp(value); });
                }

                [[nodiscard]] static constexpr auto Log(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::log(value); });
                }

                template<Concept::FixedSimdFloatingValue<N> V>
                [[nodiscard]] static constexpr V Pow(const V& base, const V& exponent) {
                    using T = typename V::ValueType;
                    return Transform(base, exponent, [](T x, T y) { return std::pow(x, y); });
                }

                [[nodiscard]] static constexpr auto Sqrt(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::sqrt(value); });
                }

                [[nodiscard]] static constexpr auto Atan(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::atan(value); });
                }

                template<Concept::FixedSimdFloatingValue<N> V>
                [[nodiscard]] static constexpr V Atan2(const V& y, const V& x) {
                    using T = typename V::ValueType;
                    return Transform(y, x, [](T yValue, T xValue) { return std::atan2(yValue, xValue); });
                }

                [[nodiscard]] static constexpr auto Asin(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::asin(value); });
                }

                [[nodiscard]] static constexpr auto Acos(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::acos(value); });
                }

                [[nodiscard]] static constexpr auto Tan(const Concept::FixedSimdFloatingValue<N> auto& x) {
                    using V = std::remove_cvref_t<decltype(x)>;
                    using T = typename V::ValueType;
                    return Transform(x, [](T value) { return std::tan(value); });
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Clamp(V value, V lower, V upper) {
                    return Simd::Clamp(value, lower, upper);
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Saturate(V value) {
                    using T = typename V::ValueType;
                    return Clamp(value, V(T(0)), V(T(1)));
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Lerp(const V& a, const V& b, const V& t) {
                    using T = typename V::ValueType;
                    if constexpr (std::floating_point<T>) {
                        return Transform(a, b, t, [](T x, T y, T weight) { return std::lerp(x, y, weight); });
                    } else {
                        return a + (b - a) * t;
                    }
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Fma(const V& a, const V& b, const V& c) {
                    using T = typename V::ValueType;
                    if constexpr (std::floating_point<T>) {
                        return Transform(a, b, c, [](T x, T y, T addend) { return std::fma(x, y, addend); });
                    } else {
                        return a * b + c;
                    }
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Mfs(const V& a, const V& b, const V& c) {
                    using T = typename V::ValueType;
                    if constexpr (std::floating_point<T>) {
                        return Transform(a, b, c, [](T x, T y, T subtrahend) { return std::fma(x, y, -subtrahend); });
                    } else {
                        return a * b - c;
                    }
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Nms(const V& a, const V& b, const V& c) {
                    using T = typename V::ValueType;
                    if constexpr (std::floating_point<T>) {
                        return Transform(a, b, c, [](T x, T y, T subtrahend) { return std::fma(-x, y, -subtrahend); });
                    } else {
                        return -a * b - c;
                    }
                }

                template<Concept::FixedSimdValue<N> V>
                [[nodiscard]] static constexpr V Nma(const V& a, const V& b, const V& c) {
                    using T = typename V::ValueType;
                    if constexpr (std::floating_point<T>) {
                        return Transform(a, b, c, [](T x, T y, T addend) { return std::fma(-x, y, addend); });
                    } else {
                        return -a * b + c;
                    }
                }

                [[nodiscard]] static constexpr auto Sign(const Concept::FixedSimdSignedValue<N> auto& x) {
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
                    using Selected = typename Hook::Backend<Carrier>::Type;
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
