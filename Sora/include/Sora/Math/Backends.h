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
#include <stdexcept>
#include <type_traits>

#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Math/Simd.h>

namespace Sora {

    namespace Math {

        namespace Backend {

            struct ScalarCPU {
                static constexpr bool IsScalar = true;
                static constexpr bool IsSimd = false;

                [[nodiscard]] static constexpr auto Add(Concept::NumericScalar auto x,
                                                        Concept::NumericScalar auto... args) {
                    return (x + ... + args);
                }

                [[nodiscard]] static constexpr auto Mul(Concept::NumericScalar auto x,
                                                        Concept::NumericScalar auto... args) {
                    return (x * ... * args);
                }

                [[nodiscard]] static constexpr auto Sub(Concept::NumericScalar auto x, Concept::NumericScalar auto y) {
                    return x - y;
                }

                [[nodiscard]] static constexpr auto Div(Concept::NumericScalar auto x, Concept::NumericScalar auto y) {
                    return x / y;
                }

                [[nodiscard]] static constexpr auto Neg(Concept::NumericScalar auto x) { return -x; }

                [[nodiscard]] static constexpr auto Inv(Concept::NumericScalar auto x) { return 1 / x; }

                [[nodiscard]] static constexpr auto Square(Concept::NumericScalar auto x) { return x * x; }

                [[nodiscard]] static constexpr auto Abs(Concept::NumericScalar auto x) { return std::abs(x); }

                [[nodiscard]] static constexpr auto Sin(Concept::NumericScalar auto x) { return std::sin(x); }

                [[nodiscard]] static constexpr auto Cos(Concept::NumericScalar auto x) { return std::cos(x); }

                [[nodiscard]] static constexpr auto Exp(Concept::NumericScalar auto x) { return std::exp(x); }

                [[nodiscard]] static constexpr auto Log(Concept::NumericScalar auto x) { return std::log(x); }

                [[nodiscard]] static constexpr auto Pow(Concept::NumericScalar auto base,
                                                        Concept::NumericScalar auto exp) {
                    return std::pow(base, exp);
                }

                [[nodiscard]] static constexpr auto Sqrt(Concept::NumericScalar auto x) { return std::sqrt(x); }

                [[nodiscard]] static constexpr auto Atan(Concept::NumericScalar auto x) { return std::atan(x); }

                [[nodiscard]] static constexpr auto Atan2(Concept::NumericScalar auto y,
                                                          Concept::NumericScalar auto x) {
                    return std::atan2(y, x);
                }

                [[nodiscard]] static constexpr auto Asin(Concept::NumericScalar auto x) { return std::asin(x); }

                [[nodiscard]] static constexpr auto Acos(Concept::NumericScalar auto x) { return std::acos(x); }

                [[nodiscard]] static constexpr auto Tan(Concept::NumericScalar auto x) { return std::tan(x); }

                [[nodiscard]] static constexpr auto Clamp(Concept::NumericScalar auto v, Concept::NumericScalar auto lo,
                                                          Concept::NumericScalar auto hi) {
                    return std::clamp(v, lo, hi);
                }

                [[nodiscard]] static constexpr auto Saturate(Concept::NumericScalar auto v) {
                    return Clamp(v, decltype(v)(0), decltype(v)(1));
                }

                [[nodiscard]] static constexpr auto Lerp(Concept::NumericScalar auto a, Concept::NumericScalar auto b,
                                                         Concept::NumericScalar auto t) {
                    if constexpr (requires { std::lerp(a, b, t); }) {
                        return std::lerp(a, b, t);
                    } else {
                        return a + (b - a) * t;
                    }
                }

                [[nodiscard]] static constexpr auto Mfa(Concept::NumericScalar auto a, Concept::NumericScalar auto b,
                                                        Concept::NumericScalar auto c) {
                    if constexpr (requires { std::fma(a, b, c); }) {
                        return std::fma(a, b, c);
                    } else {
                        return a * b + c;
                    }
                }

                [[nodiscard]] static constexpr auto Mfs(Concept::NumericScalar auto a, Concept::NumericScalar auto b,
                                                        Concept::NumericScalar auto c) {
                    if constexpr (requires { std::fma(a, b, -c); }) {
                        return std::fma(a, b, -c);
                    } else {
                        return a * b - c;
                    }
                }

                [[nodiscard]] static constexpr auto Nms(Concept::NumericScalar auto a, Concept::NumericScalar auto b,
                                                        Concept::NumericScalar auto c) {
                    if constexpr (requires { std::fma(-a, b, -c); }) {
                        return std::fma(-a, b, -c);
                    } else {
                        return -a * b - c;
                    }
                }

                [[nodiscard]] static constexpr auto Nma(Concept::NumericScalar auto a, Concept::NumericScalar auto b,
                                                        Concept::NumericScalar auto c) {
                    if constexpr (requires { std::fma(-a, b, c); }) {
                        return std::fma(-a, b, c);
                    } else {
                        return -a * b + c;
                    }
                }

                [[nodiscard]] static constexpr auto Sign(Concept::SignedArithmetic auto x) {
                    using T = std::remove_cvref_t<decltype(x)>;
                    return x > T(0) ? T(1) : (x < T(0) ? T(-1) : T(0));
                }
            };

            namespace Concept {

                template<typename T, std::size_t N>
                concept FixedSimdValue = Simd::SimdVecType<std::remove_cvref_t<T>> &&
                                         Sora::Concept::NumericScalar<typename std::remove_cvref_t<T>::ValueType> &&
                                         std::remove_cvref_t<T>::kSize.value == N;

                template<typename T, std::size_t N>
                concept FixedSimdFloatingValue =
                    FixedSimdValue<T, N> && std::floating_point<typename std::remove_cvref_t<T>::ValueType>;

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

                static constexpr bool IsScalar = false;
                static constexpr bool IsSimd = true;

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
                [[nodiscard]] static constexpr V Mfa(const V& a, const V& b, const V& c) {
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

            template<std::meta::info P>
            consteval std::meta::info GetBackend() {
                using namespace std::meta;

                constexpr info carrier = dealias(remove_cvref(P));
                if constexpr (is_arithmetic_type(carrier)) {
                    return ^^Backend::ScalarCPU;
                }

                if constexpr (has_template_arguments(carrier) &&
                              template_of(carrier) == ^^Sora::Math::Simd::BasicVector) {
                    using Carrier = [:carrier:];
                    constexpr std::size_t width = static_cast<std::size_t>(Carrier::kSize.value);
                    return substitute(^^Backend::FixedSimdCPU, {reflect_constant(width)});
                }

                throw std::logic_error("GetBackend: unsupported type");
            }

            static_assert(GetBackend<^^int>() == ^^Backend::ScalarCPU, "GetBackend must return a std::meta::info");
            static_assert(GetBackend<^^Sora::Simd::F32<2>>() == ^^Backend::FixedSimdCPU<2>,
                          "GetBackend must return a std::meta::info");

        } // namespace Meta

        namespace Traits {

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
