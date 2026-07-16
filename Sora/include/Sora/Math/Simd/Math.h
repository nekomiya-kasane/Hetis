/**
 * @file Math.h
 * @brief Mathematical operations over SIMD vectors.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/ConstexprMath.h>

#include "Vector.h"

#include <cmath>

// [simd.math] ----------------------------------------------------------------
namespace Sora::Math::Simd {

    namespace Detail {

        template<typename V, typename ScalarFunction, typename VectorFunction>
        [[gnu::always_inline]] constexpr V ElementwiseUnary(const V& x, ScalarFunction scalarFunction,
                                                            VectorFunction vectorFunction) noexcept {
            if consteval {
                return V([&](std::size_t i) { return scalarFunction(x[i]); });
            } else {
                if constexpr (V::AbiType::kNreg == 1) {
                    return V::Init(vectorFunction(x.Get()));
                } else {
                    return V::Init(ElementwiseUnary(x.GetLow(), scalarFunction, vectorFunction),
                                   ElementwiseUnary(x.GetHigh(), scalarFunction, vectorFunction));
                }
            }
        }

        template<typename V, typename ScalarFunction, typename VectorFunction>
        [[gnu::always_inline]] constexpr V ElementwiseBinary(const V& x, const V& y, ScalarFunction scalarFunction,
                                                             VectorFunction vectorFunction) noexcept {
            if consteval {
                return V([&](std::size_t i) { return scalarFunction(x[i], y[i]); });
            } else {
                if constexpr (V::AbiType::kNreg == 1) {
                    return V::Init(vectorFunction(x.Get(), y.Get()));
                } else {
                    return V::Init(ElementwiseBinary(x.GetLow(), y.GetLow(), scalarFunction, vectorFunction),
                                   ElementwiseBinary(x.GetHigh(), y.GetHigh(), scalarFunction, vectorFunction));
                }
            }
        }

        template<typename V, typename ScalarFunction, typename VectorFunction>
        [[gnu::always_inline]] constexpr V ElementwiseTernary(const V& x, const V& y, const V& z,
                                                              ScalarFunction scalarFunction,
                                                              VectorFunction vectorFunction) noexcept {
            if consteval {
                return V([&](std::size_t i) { return scalarFunction(x[i], y[i], z[i]); });
            } else {
                if constexpr (V::AbiType::kNreg == 1) {
                    return V::Init(vectorFunction(x.Get(), y.Get(), z.Get()));
                } else {
                    return V::Init(
                        ElementwiseTernary(x.GetLow(), y.GetLow(), z.GetLow(), scalarFunction, vectorFunction),
                        ElementwiseTernary(x.GetHigh(), y.GetHigh(), z.GetHigh(), scalarFunction, vectorFunction));
                }
            }
        }

    } // namespace Detail

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Sin(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x, [](T value) { return Sora::Math::Detail::CtSinCos(value).sin; },
            [](auto value) { return __builtin_elementwise_sin(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Cos(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x, [](T value) { return Sora::Math::Detail::CtSinCos(value).cos; },
            [](auto value) { return __builtin_elementwise_cos(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Tan(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x,
            [](T value) {
                const auto result = Sora::Math::Detail::CtSinCos(value);
                return result.sin / result.cos;
            },
            [](auto value) { return __builtin_elementwise_tan(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Asin(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x,
            [](T value) {
                if (value < T{-1} || value > T{1}) {
                    return std::numeric_limits<T>::quiet_NaN();
                }
                return Sora::Math::Detail::CtAtan2(value, Sora::Math::Detail::CtSqrt(T{1} - value * value));
            },
            [](auto value) { return __builtin_elementwise_asin(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Acos(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x,
            [](T value) {
                if (value < T{-1} || value > T{1}) {
                    return std::numeric_limits<T>::quiet_NaN();
                }
                return Sora::Math::Detail::CtAtan2(Sora::Math::Detail::CtSqrt(T{1} - value * value), value);
            },
            [](auto value) { return __builtin_elementwise_acos(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Atan(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x, [](T value) { return Sora::Math::Detail::CtAtan(value); },
            [](auto value) { return __builtin_elementwise_atan(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Exp(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x, [](T value) { return Sora::Math::Detail::CtExp(value); },
            [](auto value) { return __builtin_elementwise_exp(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Log(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x, [](T value) { return Sora::Math::Detail::CtLog(value); },
            [](auto value) { return __builtin_elementwise_log(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Sqrt(const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseUnary(
            x, [](T value) { return Sora::Math::Detail::CtSqrt(value); },
            [](auto value) { return __builtin_elementwise_sqrt(value); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Pow(const BasicVector<T, Abi>& x,
                                                             const BasicVector<T, Abi>& y) noexcept {
        return Detail::ElementwiseBinary(
            x, y, [](T left, T right) { return Sora::Math::Detail::CtPow(left, right); },
            [](auto left, auto right) { return __builtin_elementwise_pow(left, right); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Atan2(const BasicVector<T, Abi>& y,
                                                               const BasicVector<T, Abi>& x) noexcept {
        return Detail::ElementwiseBinary(
            y, x, [](T yValue, T xValue) { return Sora::Math::Detail::CtAtan2(yValue, xValue); },
            [](auto yValue, auto xValue) { return __builtin_elementwise_atan2(yValue, xValue); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> Fma(const BasicVector<T, Abi>& x, const BasicVector<T, Abi>& y,
                                                             const BasicVector<T, Abi>& z) noexcept {
        return Detail::ElementwiseTernary(
            x, y, z, [](T left, T right, T addend) { return left * right + addend; },
            [](auto left, auto right, auto addend) { return __builtin_elementwise_fma(left, right, addend); });
    }

    template<std::floating_point T, typename Abi>
    [[gnu::always_inline]] constexpr BasicVector<T, Abi> CopySign(const BasicVector<T, Abi>& magnitude,
                                                                  const BasicVector<T, Abi>& sign) noexcept {
        return Detail::ElementwiseBinary(
            magnitude, sign, [](T value, T signValue) { return Sora::Math::Detail::CtCopySign(value, signValue); },
            [](auto value, auto signValue) { return __builtin_elementwise_copysign(value, signValue); });
    }

    template<std::signed_integral T, typename Abi>
    [[gnu::always_inline]]
    constexpr BasicVector<T, Abi> Abs(const BasicVector<T, Abi>& x) noexcept {
        return x.Abs();
    }

    template<MathFloatingPoint Vp>
    [[gnu::always_inline]]
    constexpr typename DeducedVecT<Vp>::MaskType Isinf(const Vp& x) noexcept {
        return static_cast<const DeducedVecT<Vp>&>(x).Isinf();
    }

    template<MathFloatingPoint Vp>
    [[gnu::always_inline]]
    constexpr typename DeducedVecT<Vp>::MaskType Isnan(const Vp& x) noexcept {
        return static_cast<const DeducedVecT<Vp>&>(x).Isnan();
    }

} // namespace Sora::Math::Simd

// [simd.std::complex.math] --------------------------------------------------------
namespace Sora::Math::Simd {

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Rebind<SimdComplexValueType<Vp>, Vp> Real(const Vp& x) noexcept {
        return x.Real();
    }

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Rebind<SimdComplexValueType<Vp>, Vp> Imag(const Vp& x) noexcept {
        return x.Imag();
    }

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Rebind<SimdComplexValueType<Vp>, Vp> Abs(const Vp& x) noexcept {
        using R = Rebind<SimdComplexValueType<Vp>, Vp>;
        using T = typename R::ValueType;
        const R real = x.Real().Fabs();
        const R imag = x.Imag().Fabs();
        const auto realIsSmaller = real < imag;
        const R maximum = SelectImpl(realIsSmaller, imag, real);
        const R minimum = SelectImpl(realIsSmaller, real, imag);
        const R ratio = minimum / maximum;
        const R scaled = maximum * Sqrt(R(T{1}) + ratio * ratio);
        R result = SelectImpl(maximum == R(T{}), maximum, scaled);

        const auto special = real.Isinf() || imag.Isinf() || real.Isnan() || imag.Isnan();
        if (special.AnyOf()) {
            const R standardResult([&](std::size_t i) { return std::abs(std::complex<T>(x[i])); });
            result = SelectImpl(special, standardResult, result);
        }
        return result;
    }

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Rebind<SimdComplexValueType<Vp>, Vp> Arg(const Vp& x) noexcept {
        return Atan2(x.Imag(), x.Real());
    }

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Rebind<SimdComplexValueType<Vp>, Vp> Norm(const Vp& x) noexcept {
        return x.Norm();
    }

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Vp Conj(const Vp& x) noexcept {
        return x.Conj();
    }

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Vp Proj(const Vp& x) noexcept {
        using T = SimdComplexValueType<Vp>;
        using R = Rebind<T, Vp>;
        const R real = x.Real();
        const R imag = x.Imag();
        const auto infinite = real.Isinf() || imag.Isinf();
        return Vp(SelectImpl(infinite, R(std::numeric_limits<T>::infinity()), real),
                  SelectImpl(infinite, CopySign(R(T{}), imag), imag));
    }

} // namespace Sora::Math::Simd
