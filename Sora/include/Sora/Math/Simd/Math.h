/**
 * @file Math.h
 * @brief Mathematical operations over SIMD vectors.
 * @ingroup Math
 */
#pragma once

#include "Vector.h"

// psabi warnings are bogus because the ABI of the internal types never leaks into user code
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

// [simd.math] ----------------------------------------------------------------
namespace Sora::Math::Simd {

    template<std::signed_integral T, typename Abi>
    [[gnu::always_inline]]
    constexpr BasicVector<T, Abi> Abs(const BasicVector<T, Abi>& x) {
        return x.Abs();
    }

    template<MathFloatingPoint Vp>
    [[gnu::always_inline]]
    constexpr typename DeducedVecT<Vp>::MaskType Isinf(const Vp& x) {
        return static_cast<const DeducedVecT<Vp>&>(x).Isinf();
    }

    template<MathFloatingPoint Vp>
    [[gnu::always_inline]]
    constexpr typename DeducedVecT<Vp>::MaskType Isnan(const Vp& x) {
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
        return x.Abs();
    }

    template<SimdComplex Vp>
    [[gnu::always_inline]]
    constexpr Rebind<SimdComplexValueType<Vp>, Vp> Arg(const Vp& x) noexcept {
        return x.Arg();
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
        return x.Proj();
    }

} // namespace Sora::Math::Simd

namespace Sora::Math {
    
    using Simd::Abs;
    using Simd::Arg;
    using Simd::Conj;
    using Simd::Imag;
    using Simd::Norm;
    using Simd::Proj;
    using Simd::Real;

} // namespace Sora::Math

#pragma GCC diagnostic pop
