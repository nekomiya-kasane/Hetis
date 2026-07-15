/**
 * @file Bit.h
 * @brief Element-wise SIMD bit operations.
 * @ingroup Math
 */
#pragma once

#include "Vector.h"

// psabi warnings are bogus because the ABI of the internal types never leaks into user code
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

// [simd.bit] -----------------------------------------------------------------
namespace Sora::Math::Simd {

    template<SimdIntegral Vp>
    [[gnu::always_inline]]
    constexpr Vp Byteswap(const Vp& v) noexcept {
        if constexpr (sizeof(typename Vp::ValueType) == 1) {
            return v;
        } else {
            return Vp([&](int i) { return std::byteswap(v[i]); });
        }
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Vp BitCeil(const Vp& v) {
        using Tp = typename Vp::ValueType;
        constexpr Tp maxValue = Tp(1) << (sizeof(Tp) * __CHAR_BIT__ - 1);
        SORA_SIMD_PRECONDITION(AllOf(v <= maxValue), "BitCeil result is not representable");
        return Vp([&](int i) { return std::bit_ceil(v[i]); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Vp BitFloor(const Vp& v) noexcept {
        return Vp([&](int i) { return std::bit_floor(v[i]); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr typename Vp::MaskType HasSingleBit(const Vp& v) noexcept {
        return typename Vp::MaskType([&](int i) { return std::has_single_bit(v[i]); });
    }

    template<SimdUnsignedInteger V0, SimdIntegral V1>
        requires(V0::kSize() == V1::kSize()) && (sizeof(typename V0::ValueType) == sizeof(typename V1::ValueType))
    [[gnu::always_inline]]
    constexpr V0 Rotl(const V0& v, const V1& s) noexcept {
        return V0([&](int i) { return std::rotl(v[i], s[i]); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Vp Rotl(const Vp& v, int s) noexcept {
        return Vp([&](int i) { return std::rotl(v[i], s); });
    }

    template<SimdUnsignedInteger V0, SimdIntegral V1>
        requires(V0::kSize() == V1::kSize()) && (sizeof(typename V0::ValueType) == sizeof(typename V1::ValueType))
    [[gnu::always_inline]]
    constexpr V0 Rotr(const V0& v, const V1& s) noexcept {
        return V0([&](int i) { return std::rotr(v[i], s[i]); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Vp Rotr(const Vp& v, int s) noexcept {
        return Vp([&](int i) { return std::rotr(v[i], s); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Rebind<std::make_signed_t<typename Vp::ValueType>, Vp> BitWidth(const Vp& v) noexcept {
        using Ip = std::make_signed_t<typename Vp::ValueType>;
        return Rebind<Ip, Vp>([&](int i) { return static_cast<Ip>(std::bit_width(v[i])); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Rebind<std::make_signed_t<typename Vp::ValueType>, Vp> CountlZero(const Vp& v) noexcept {
        using Ip = std::make_signed_t<typename Vp::ValueType>;
        return Rebind<Ip, Vp>([&](int i) { return static_cast<Ip>(std::countl_zero(v[i])); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Rebind<std::make_signed_t<typename Vp::ValueType>, Vp> CountlOne(const Vp& v) noexcept {
        using Ip = std::make_signed_t<typename Vp::ValueType>;
        return Rebind<Ip, Vp>([&](int i) { return static_cast<Ip>(std::countl_one(v[i])); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Rebind<std::make_signed_t<typename Vp::ValueType>, Vp> CountrZero(const Vp& v) noexcept {
        using Ip = std::make_signed_t<typename Vp::ValueType>;
        return Rebind<Ip, Vp>([&](int i) { return static_cast<Ip>(std::countr_zero(v[i])); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Rebind<std::make_signed_t<typename Vp::ValueType>, Vp> CountrOne(const Vp& v) noexcept {
        using Ip = std::make_signed_t<typename Vp::ValueType>;
        return Rebind<Ip, Vp>([&](int i) { return static_cast<Ip>(std::countr_one(v[i])); });
    }

    template<SimdUnsignedInteger Vp>
    [[gnu::always_inline]]
    constexpr Rebind<std::make_signed_t<typename Vp::ValueType>, Vp> Popcount(const Vp& v) noexcept {
        using Ip = std::make_signed_t<typename Vp::ValueType>;
        return Rebind<Ip, Vp>([&](int i) { return static_cast<Ip>(std::popcount(v[i])); });
    }

} // namespace Sora::Math::Simd

namespace Sora::Math {

    using Simd::BitCeil;
    using Simd::BitFloor;
    using Simd::BitWidth;
    using Simd::Byteswap;
    using Simd::CountlOne;
    using Simd::CountlZero;
    using Simd::CountrOne;
    using Simd::CountrZero;
    using Simd::HasSingleBit;
    using Simd::Popcount;
    using Simd::Rotl;
    using Simd::Rotr;

} // namespace Sora::Math

#pragma GCC diagnostic pop
