/**
 * @file X86.h
 * @brief x86 and x86-64 target-specific SIMD operations.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Simd/VectorOperations.h>

#if !SORA_SIMD_X86
#    error "wrong include for this target"
#endif

namespace Sora::Math::Simd {

    static constexpr std::size_t kX86MaxGeneralRegisterSize
#ifdef __x86_64__
        = 8;
#else
        = 4;
#endif

    /** @internal
     * Return a bit-Mask for the given vector-Mask.
     *
     * Caveats:
     * 1. The bit-Mask of 2-Byte vector-masks has duplicated entries (because of missing instruction)
     * 2. The return type internally is 'int', but that fails on conversion to uint64 if the MSB of a
     * YMM 1/2-Byte vector-Mask is set (sign extension). Therefore these helper functions return
     * unsigned instead.
     * 3. ZMM inputs are not supported.
     */
    [[gnu::always_inline]]
    inline unsigned X86Movmsk(VecBuiltinTypeBytes<Detail::IntegerForSize<8>, 16> x) {
        return __builtin_ia32_movmskpd(VecBitCast<double>(x));
    }

    [[gnu::always_inline]]
    inline unsigned X86Movmsk(VecBuiltinTypeBytes<Detail::IntegerForSize<8>, 32> x) {
        return __builtin_ia32_movmskpd256(VecBitCast<double>(x));
    }

    [[gnu::always_inline]]
    inline unsigned X86Movmsk(VecBuiltinTypeBytes<Detail::IntegerForSize<4>, 16> x) {
        return __builtin_ia32_movmskps(VecBitCast<float>(x));
    }

    template<ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline Bitmask<8> X86Movmsk(VecBuiltinTypeBytes<Detail::IntegerForSize<4>, 8> x) {
#if __has_builtin(__builtin_ia32_pext_di)
        if constexpr (Traits.HaveBmi2()) {
            return Bitmask<8>(
                __builtin_ia32_pext_di(__builtin_bit_cast(unsigned long long, x), 0x80000000'80000000ULL));
        }
#endif
        return Bitmask<8>(X86Movmsk(VecZeroPadTo16(x)));
    }

    [[gnu::always_inline]]
    inline unsigned X86Movmsk(VecBuiltinTypeBytes<Detail::IntegerForSize<4>, 32> x) {
        return __builtin_ia32_movmskps256(VecBitCast<float>(x));
    }

    template<VecBuiltin TV, auto Traits = ArchTraits()>
        requires(sizeof(VecValueType<TV>) <= 2)
    [[gnu::always_inline]]
    inline unsigned X86Movmsk(TV x) {
        static_assert(kWidthOf<TV> > 1);
        if constexpr (sizeof(x) == 32) {
            return __builtin_ia32_pmovmskb256(VecBitCast<char>(x));
        } else if constexpr (sizeof(x) == 16) {
            return __builtin_ia32_pmovmskb128(VecBitCast<char>(x));
        } else if constexpr (sizeof(x) == 8) {
#if __has_builtin(__builtin_ia32_pext_di)
            if constexpr (Traits.HaveBmi2()) {
                return __builtin_ia32_pext_di(__builtin_bit_cast(unsigned long long, x), 0x8080'8080'8080'8080ULL);
            }
#endif
            return X86Movmsk(VecZeroPadTo16(x));
        } else if constexpr (sizeof(x) == 4) {
#if __has_builtin(__builtin_ia32_pext_si)
            if constexpr (Traits.HaveBmi2()) {
                return __builtin_ia32_pext_si(__builtin_bit_cast(unsigned int, x), 0x80808080u);
            }
#endif
            (void)1; // suppress warning
            return X86Movmsk(VecZeroPadTo16(x));
        } else if constexpr (sizeof(x) == 2) {
            auto bits = __builtin_bit_cast(unsigned short, x);
#if __has_builtin(__builtin_ia32_pext_si)
            if constexpr (Traits.HaveBmi2()) {
                return __builtin_ia32_pext_si(bits, 0x00008080u);
            }
#endif
            return ((bits >> 7) & 1) | ((bits & 0x8000) >> 14);
        } else {
            static_assert(false);
        }
    }

    template<VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline bool X86VecIsZero(TV a) {
        using Tp = VecValueType<TV>;
        static_assert(std::is_integral_v<Tp>);
        if constexpr (sizeof(TV) <= kX86MaxGeneralRegisterSize) {
            return __builtin_bit_cast(Detail::IntegerForSize<sizeof(TV)>, a) == 0;
        } else if constexpr (Traits.HaveAvx()) {
            if constexpr (sizeof(TV) == 32) {
                return __builtin_ia32_ptestz256(VecBitCast<long long>(a), VecBitCast<long long>(a));
            } else if constexpr (sizeof(TV) == 16) {
                return __builtin_ia32_ptestz128(VecBitCast<long long>(a), VecBitCast<long long>(a));
            } else if constexpr (sizeof(TV) < 16) {
                return X86VecIsZero(VecZeroPadTo16(a));
            } else {
                static_assert(false);
            }
        } else if constexpr (Traits.HaveSse41()) {
            if constexpr (sizeof(TV) == 16) {
                return __builtin_ia32_ptestz128(VecBitCast<long long>(a), VecBitCast<long long>(a));
            } else if constexpr (sizeof(TV) < 16) {
                return X86VecIsZero(VecZeroPadTo16(a));
            } else {
                static_assert(false);
            }
        } else {
            return X86Movmsk(a) == 0;
        }
    }

    template<VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline int X86VecTestz(TV a, TV b) {
        static_assert(sizeof(TV) == 16 || sizeof(TV) == 32);
        static_assert(Traits.HaveSse41());
        if constexpr (sizeof(TV) == 32) {
            return __builtin_ia32_ptestz256(VecBitCast<long long>(a), VecBitCast<long long>(b));
        } else {
            return __builtin_ia32_ptestz128(VecBitCast<long long>(a), VecBitCast<long long>(b));
        }
    }

    template<VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline int X86VecTestc(TV a, TV b) {
        static_assert(sizeof(TV) == 16 || sizeof(TV) == 32);
        static_assert(Traits.HaveSse41());
        if constexpr (sizeof(TV) == 32) {
            return __builtin_ia32_ptestc256(VecBitCast<long long>(a), VecBitCast<long long>(b));
        } else {
            return __builtin_ia32_ptestc128(VecBitCast<long long>(a), VecBitCast<long long>(b));
        }
    }

    template<int Np, VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline bool X86VecmaskAll(TV k) {
        using Tp = VecValueType<TV>;
        static_assert(std::is_integral_v<Tp> && std::is_signed_v<Tp>);
        constexpr int width = kWidthOf<TV>;
        static_assert(sizeof(k) <= 32);
        if constexpr (Np == width) {
            if constexpr (sizeof(k) <= kX86MaxGeneralRegisterSize) {
                using Ip = Detail::IntegerForSize<sizeof(k)>;
                return __builtin_bit_cast(Ip, k) == ~Ip();
            } else if constexpr (!Traits.HaveSse41()) {
                constexpr unsigned validBits = (1u << (sizeof(Tp) == 2 ? Np * 2 : Np)) - 1;
                return X86Movmsk(k) == validBits;
            } else if constexpr (sizeof(k) < 16) {
                return X86VecmaskAll<Np>(VecZeroPadTo16(k));
            } else {
                return 0 != X86VecTestc(k, ~TV());
            }
        } else if constexpr (sizeof(k) <= kX86MaxGeneralRegisterSize) {
            using Ip = Detail::IntegerForSize<sizeof(k)>;
            constexpr Ip validBits = (Ip(1) << (Np * sizeof(Tp) * __CHAR_BIT__)) - 1;
            return (__builtin_bit_cast(Ip, k) & validBits) == validBits;
        } else if constexpr (!Traits.HaveSse41()) {
            constexpr unsigned validBits = (1u << (sizeof(Tp) == 2 ? Np * 2 : Np)) - 1;
            return (X86Movmsk(k) & validBits) == validBits;
        } else if constexpr (sizeof(k) < 16) {
            return X86VecmaskAll<Np>(VecZeroPadTo16(k));
        } else {
            return 0 != X86VecTestc(k, kVecImplicitMask<Np, TV>);
        }
    }

    template<int Np, VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline bool X86VecmaskAny(TV k) {
        using Tp = VecValueType<TV>;
        static_assert(std::is_integral_v<Tp> && std::is_signed_v<Tp>);
        constexpr int width = kWidthOf<TV>;
        static_assert(sizeof(k) <= 32);
        if constexpr (Np == width) {
            return !X86VecIsZero(k);
        } else if constexpr (sizeof(k) <= kX86MaxGeneralRegisterSize) {
            using Ip = Detail::IntegerForSize<sizeof(k)>;
            constexpr Ip validBits = (Ip(1) << (Np * sizeof(Tp) * __CHAR_BIT__)) - 1;
            return (__builtin_bit_cast(Ip, k) & validBits) != Ip();
        } else if constexpr (!Traits.HaveSse41()) {
            constexpr unsigned validBits = (1u << (sizeof(Tp) == 2 ? Np * 2 : Np)) - 1;
            return (X86Movmsk(k) & validBits) != 0;
        } else if constexpr (sizeof(k) < 16) {
            return X86VecmaskAny<Np>(VecZeroPadTo16(k));
        } else {
            return 0 == X86VecTestz(k, kVecImplicitMask<Np, TV>);
        }
    }

    template<int Np, VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline bool X86VecmaskNone(TV k) {
        using Tp = VecValueType<TV>;
        static_assert(std::is_integral_v<Tp> && std::is_signed_v<Tp>);
        constexpr int width = kWidthOf<TV>;
        static_assert(sizeof(k) <= 32);
        if constexpr (Np == width) {
            return X86VecIsZero(k);
        } else if constexpr (sizeof(k) <= kX86MaxGeneralRegisterSize) {
            using Ip = Detail::IntegerForSize<sizeof(k)>;
            constexpr Ip validBits = (Ip(1) << (Np * sizeof(Tp) * __CHAR_BIT__)) - 1;
            return (__builtin_bit_cast(Ip, k) & validBits) == Ip();
        } else if constexpr (!Traits.HaveSse41()) {
            constexpr unsigned validBits = (1u << (sizeof(Tp) == 2 ? Np * 2 : Np)) - 1;
            return (X86Movmsk(k) & validBits) == 0;
        } else if constexpr (sizeof(k) < 16) {
            return X86VecmaskNone<Np>(VecZeroPadTo16(k));
        } else {
            return 0 != X86VecTestz(k, kVecImplicitMask<Np, TV>);
        }
    }

    enum class X86Cmp : uint8_t {
        kEq = 0,
        kLt = 1,
        kLe = 2,
        kUnord = 3,
        kNeq = 4,
        kNlt = 5,
        kNle = 6,
    };

    template<X86Cmp Cmp, VecBuiltin TV, ArchTraits Traits = {}>
        requires std::is_floating_point_v<VecValueType<TV>>
    [[gnu::always_inline]]
    inline auto X86BitmaskCmp(TV x, TV y) {
        constexpr int c = int(Cmp);
        using Tp = VecValueType<TV>;
        if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 8) {
            return __builtin_ia32_cmppd512_mask(x, y, c, -1, 4);
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 4) {
            return __builtin_ia32_cmpps512_mask(x, y, c, -1, 4);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
            return __builtin_ia32_cmppd256_mask(x, y, c, -1);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
            return __builtin_ia32_cmpps256_mask(x, y, c, -1);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
            return __builtin_ia32_cmppd128_mask(x, y, c, -1);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
            return __builtin_ia32_cmpps128_mask(x, y, c, -1);
        } else if constexpr (std::is_same_v<Tp, _Float16>) {
            if constexpr (sizeof(TV) == 64 && Traits.HaveAvx512fp16()) {
                return __builtin_ia32_cmpph512_mask(x, y, c, -1);
            } else if constexpr (sizeof(TV) == 32 && Traits.HaveAvx512fp16()) {
                return __builtin_ia32_cmpph256_mask(x, y, c, -1);
            } else if constexpr (sizeof(TV) == 16 && Traits.HaveAvx512fp16()) {
                return __builtin_ia32_cmpph128_mask(x, y, c, -1);
            } else if constexpr (sizeof(TV) < 16 && Traits.HaveAvx512fp16()) {
                return X86BitmaskCmp<Cmp>(VecZeroPadTo16(x), VecZeroPadTo16(y));
            } else {
                // without AVX512_FP16, float16_t size needs to match float32_t size
                // (cf. NativeAbi())
                static_assert(sizeof(TV) <= 32);
                return X86BitmaskCmp<Cmp>(VecCast<float>(x), VecCast<float>(y));
            }
        } else if constexpr (sizeof(TV) < 16) {
            return X86BitmaskCmp<Cmp>(VecZeroPadTo16(x), VecZeroPadTo16(y));
        } else {
            static_assert(false);
        }
    }

    template<typename Tp>
    [[nodiscard]] consteval auto X86IntrinIntType() {
        if constexpr (sizeof(Tp) == 1) {
            return std::type_identity<char>{};
        } else {
            return std::type_identity<Detail::IntegerForSize<sizeof(Tp)>>{};
        }
    }

    template<typename Tp>
    using X86IntrinInt = typename decltype(X86IntrinIntType<Tp>())::type;

    template<typename Tp>
    [[nodiscard]] consteval auto X86IntrinTypeImpl() {
        if constexpr (std::is_integral_v<Tp> || sizeof(Tp) <= 2) {
            return std::type_identity<X86IntrinInt<Tp>>{};
        } else {
            return std::type_identity<CanonicalVecTypeT<Tp>>{};
        }
    }

    template<typename Tp>
    using X86IntrinType = typename decltype(X86IntrinTypeImpl<Tp>())::type;

    template<typename Tp>
    [[nodiscard]] consteval auto X86IntelIntrinValueTypeImpl() {
        if constexpr (std::is_integral_v<Tp>) {
            return std::type_identity<long long>{};
        } else if constexpr (sizeof(Tp) == 8) {
            return std::type_identity<double>{};
        } else if constexpr (sizeof(Tp) == 4) {
            return std::type_identity<float>{};
        } else if constexpr (sizeof(Tp) == 2) {
            return std::type_identity<_Float16>{};
        }
    }

    template<typename Tp>
    using X86IntelIntrinValueType = typename decltype(X86IntelIntrinValueTypeImpl<Tp>())::type;
#if !SORA_SIMD_CLANG
    // overload VecAndnot from simd_detail.h
    template<VecBuiltin TV>
        requires(sizeof(TV) >= 16)
    [[gnu::always_inline]]
    constexpr TV VecAndnot(TV a, TV b) {
        constexpr TargetTraits Traits = {};
        using Tp = VecValueType<TV>;
        using UV = VecBuiltinType<UInt<sizeof(Tp)>, kWidthOf<TV>>;
        if (__builtin_is_constant_evaluated() || (__builtin_constant_p(a) && __builtin_constant_p(b))) {
            return reinterpret_cast<TV>(~reinterpret_cast<UV>(a) & reinterpret_cast<UV>(b));
        } else if constexpr (std::is_same_v<Tp, _Float16>) {
            return reinterpret_cast<TV>(VecAndnot(VecBitCast<float>(a), VecBitCast<float>(b)));
        } else if constexpr (sizeof(TV) == 16 && std::is_same_v<Tp, float>) {
            return __builtin_ia32_andnps(a, b);
        } else if constexpr (sizeof(TV) == 16 && std::is_same_v<Tp, double>) {
            return __builtin_ia32_andnpd(a, b);
        } else if constexpr (sizeof(TV) == 32 && std::is_same_v<Tp, float>) {
            return __builtin_ia32_andnps256(a, b);
        } else if constexpr (sizeof(TV) == 32 && std::is_same_v<Tp, double>) {
            return __builtin_ia32_andnpd256(a, b);
        } else if constexpr (sizeof(TV) == 64 && std::is_same_v<Tp, float> && Traits.HaveAvx512dq()) {
            return __builtin_ia32_andnps512_mask(a, b, TV{}, -1);
        } else if constexpr (sizeof(TV) == 64 && std::is_same_v<Tp, double> && Traits.HaveAvx512dq()) {
            return __builtin_ia32_andnpd512_mask(a, b, TV{}, -1);
        } else {
            auto aLongLong = VecBitCast<long long>(a);
            auto bLongLong = VecBitCast<long long>(b);
            if constexpr (sizeof(TV) == 16 && std::is_integral_v<Tp>) {
                return reinterpret_cast<TV>(__builtin_ia32_pandn128(aLongLong, bLongLong));
            } else if constexpr (sizeof(TV) == 32 && std::is_integral_v<Tp> && Traits.HaveAvx2()) {
                return reinterpret_cast<TV>(__builtin_ia32_andnotsi256(aLongLong, bLongLong));
            } else if constexpr (sizeof(TV) == 32 && std::is_integral_v<Tp>) {
                return reinterpret_cast<TV>(__builtin_ia32_andnpd256(VecBitCast<double>(a), VecBitCast<double>(b)));
            } else if constexpr (sizeof(TV) == 64) {
                auto aInt = VecBitCast<int>(a);
                auto bInt = VecBitCast<int>(b);
                return reinterpret_cast<TV>(__builtin_ia32_pandnd512_mask(aInt, bInt, decltype(aInt)(), -1));
            }
        }
    }
#endif // not Clang

    template<X86Cmp Cmp, VecBuiltin TV, ArchTraits Traits = {}>
        requires std::is_integral_v<VecValueType<TV>>
    [[gnu::always_inline]]
    inline auto X86BitmaskCmp(TV x, TV y) {
        constexpr int c = int(Cmp);
        using Tp = VecValueType<TV>;
        if constexpr (sizeof(TV) < 16) {
            return X86BitmaskCmp<Cmp>(VecZeroPadTo16(x), VecZeroPadTo16(y));
        } else if constexpr (std::is_signed_v<Tp>) {
            const auto xi = VecBitCast<X86IntrinInt<Tp>>(x);
            const auto yi = VecBitCast<X86IntrinInt<Tp>>(y);
            if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 8) {
                return __builtin_ia32_cmpq512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 4) {
                return __builtin_ia32_cmpd512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 2) {
                return __builtin_ia32_cmpw512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 1) {
                return __builtin_ia32_cmpb512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
                return __builtin_ia32_cmpq256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
                return __builtin_ia32_cmpd256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 2) {
                return __builtin_ia32_cmpw256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 1) {
                return __builtin_ia32_cmpb256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
                return __builtin_ia32_cmpq128_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
                return __builtin_ia32_cmpd128_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 2) {
                return __builtin_ia32_cmpw128_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 1) {
                return __builtin_ia32_cmpb128_mask(xi, yi, c, -1);
            } else {
                static_assert(false);
            }
        } else {
            const auto xi = VecBitCast<X86IntrinInt<Tp>>(x);
            const auto yi = VecBitCast<X86IntrinInt<Tp>>(y);
            if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 8) {
                return __builtin_ia32_ucmpq512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 4) {
                return __builtin_ia32_ucmpd512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 2) {
                return __builtin_ia32_ucmpw512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 1) {
                return __builtin_ia32_ucmpb512_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
                return __builtin_ia32_ucmpq256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
                return __builtin_ia32_ucmpd256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 2) {
                return __builtin_ia32_ucmpw256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 1) {
                return __builtin_ia32_ucmpb256_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
                return __builtin_ia32_ucmpq128_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
                return __builtin_ia32_ucmpd128_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 2) {
                return __builtin_ia32_ucmpw128_mask(xi, yi, c, -1);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 1) {
                return __builtin_ia32_ucmpb128_mask(xi, yi, c, -1);
            } else {
                static_assert(false);
            }
        }
    }

    template<VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline auto X86BitmaskIsinf(TV x) {
        static_assert(Traits.HaveAvx512dq());
        using Tp = VecValueType<TV>;
        static_assert(std::is_floating_point_v<Tp>);
        if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 8) {
            return __builtin_ia32_fpclasspd512_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
            return __builtin_ia32_fpclasspd256_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
            return __builtin_ia32_fpclasspd128_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 4) {
            return __builtin_ia32_fpclassps512_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
            return __builtin_ia32_fpclassps256_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
            return __builtin_ia32_fpclassps128_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 2 && Traits.HaveAvx512fp16()) {
            return __builtin_ia32_fpclassph512_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 2 && Traits.HaveAvx512fp16()) {
            return __builtin_ia32_fpclassph256_mask(x, 0x18, -1);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 2 && Traits.HaveAvx512fp16()) {
            return __builtin_ia32_fpclassph128_mask(x, 0x18, -1);
        } else if constexpr (sizeof(Tp) == 2 && !Traits.HaveAvx512fp16()) {
            return X86BitmaskIsinf(VecCast<float>(x));
        } else if constexpr (sizeof(TV) < 16) {
            return X86BitmaskIsinf(VecZeroPadTo16(x));
        } else {
            static_assert(false);
        }
    }

    template<VecBuiltin KV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline KV X86BitToVecmask(std::integral auto bits) {
        using Kp = VecValueType<KV>;
        static_assert((sizeof(bits) * __CHAR_BIT__ == kWidthOf<KV>) ||
                      (sizeof(bits) == 1 && __CHAR_BIT__ > kWidthOf<KV>));

        if constexpr (sizeof(Kp) == 1 && sizeof(KV) == 64) {
            return __builtin_ia32_cvtmask2b512(bits);
        } else if constexpr (sizeof(Kp) == 1 && sizeof(KV) == 32) {
            return __builtin_ia32_cvtmask2b256(bits);
        } else if constexpr (sizeof(Kp) == 1 && sizeof(KV) == 16) {
            return __builtin_ia32_cvtmask2b128(bits);
        } else if constexpr (sizeof(Kp) == 1 && sizeof(KV) <= 8) {
            return VecOps<KV>::Extract(__builtin_ia32_cvtmask2b128(bits));
        }

        else if constexpr (sizeof(Kp) == 2 && sizeof(KV) == 64) {
            return __builtin_ia32_cvtmask2w512(bits);
        } else if constexpr (sizeof(Kp) == 2 && sizeof(KV) == 32) {
            return __builtin_ia32_cvtmask2w256(bits);
        } else if constexpr (sizeof(Kp) == 2 && sizeof(KV) == 16) {
            return __builtin_ia32_cvtmask2w128(bits);
        } else if constexpr (sizeof(Kp) == 2 && sizeof(KV) <= 8) {
            return VecOps<KV>::Extract(__builtin_ia32_cvtmask2w128(bits));
        }

        else if constexpr (sizeof(Kp) == 4 && sizeof(KV) == 64) {
            return __builtin_ia32_cvtmask2d512(bits);
        } else if constexpr (sizeof(Kp) == 4 && sizeof(KV) == 32) {
            return __builtin_ia32_cvtmask2d256(bits);
        } else if constexpr (sizeof(Kp) == 4 && sizeof(KV) <= 16) {
            return VecOps<KV>::Extract(__builtin_ia32_cvtmask2d128(bits));
        }

        else if constexpr (sizeof(Kp) == 8 && sizeof(KV) == 64) {
            return __builtin_ia32_cvtmask2q512(bits);
        } else if constexpr (sizeof(Kp) == 8 && sizeof(KV) == 32) {
            return __builtin_ia32_cvtmask2q256(bits);
        } else if constexpr (sizeof(Kp) == 8 && sizeof(KV) == 16) {
            return __builtin_ia32_cvtmask2q128(bits);
        }

        else {
            static_assert(false);
        }
    }

    template<std::unsigned_integral Kp, VecBuiltin TV, ArchTraits Traits = {}>
        requires std::is_integral_v<VecValueType<TV>>
    [[gnu::always_inline]]
    constexpr inline TV X86BitmaskBlend(Kp k, TV t, TV f) {
        using Tp = VecValueType<TV>;
        using Ip = X86IntrinInt<Tp>;
        if constexpr (!std::is_same_v<Ip, Tp>) {
            return reinterpret_cast<TV>(X86BitmaskBlend(k, VecBitCast<Ip>(t), VecBitCast<Ip>(f)));
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 8) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectq_512, __builtin_ia32_blendmq_512_mask);
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 4) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectd_512, __builtin_ia32_blendmd_512_mask);
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 2) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectw_512, __builtin_ia32_blendmw_512_mask);
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 1) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectb_512, __builtin_ia32_blendmb_512_mask);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectq_256, __builtin_ia32_blendmq_256_mask);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectd_256, __builtin_ia32_blendmd_256_mask);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 2) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectw_256, __builtin_ia32_blendmw_256_mask);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 1) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectb_256, __builtin_ia32_blendmb_256_mask);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectq_128, __builtin_ia32_blendmq_128_mask);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectd_128, __builtin_ia32_blendmd_128_mask);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 2) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectw_128, __builtin_ia32_blendmw_128_mask);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 1) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectb_128, __builtin_ia32_blendmb_128_mask);
        } else if constexpr (sizeof(TV) < 16) {
            return VecOps<TV>::Extract(X86BitmaskBlend(k, VecZeroPadTo16(t), VecZeroPadTo16(f)));
        } else {
            static_assert(false);
        }
    }

    template<std::unsigned_integral Kp, VecBuiltin TV, ArchTraits Traits = {}>
        requires std::is_floating_point_v<VecValueType<TV>>
    [[gnu::always_inline]]
    constexpr inline TV X86BitmaskBlend(Kp k, TV t, TV f) {
        using Tp = VecValueType<TV>;
        if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 8) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectpd_512, __builtin_ia32_blendmpd_512_mask);
        } else if constexpr (sizeof(TV) == 64 && sizeof(Tp) == 4) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectps_512, __builtin_ia32_blendmps_512_mask);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectpd_256, __builtin_ia32_blendmpd_256_mask);
        } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectps_256, __builtin_ia32_blendmps_256_mask);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectpd_128, __builtin_ia32_blendmpd_128_mask);
        } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
            return SORA_SIMD_X86_BLEND(f, t, k, __builtin_ia32_selectps_128, __builtin_ia32_blendmps_128_mask);
        } else if constexpr (std::is_same_v<Tp, _Float16>) {
            using Up = Detail::IntegerForSize<sizeof(Tp)>;
            return VecBitCast<_Float16>(X86BitmaskBlend(k, VecBitCast<Up>(t), VecBitCast<Up>(f)));
        } else if constexpr (sizeof(TV) < 16) {
            return VecOps<TV>::Extract(X86BitmaskBlend(k, VecZeroPadTo16(t), VecZeroPadTo16(f)));
        } else {
            static_assert(false);
        }
    }

    template<int OutputBits = 4, ArchTraits Traits = {}>
    constexpr Bitmask<1> BitExtractEven(UInt<1> x) {
        static_assert(OutputBits <= 4);
        constexpr UInt<1> mask = 0x55u >> ((4 - OutputBits) * 2);
#if __has_builtin(__builtin_ia32_pext_si)
        if constexpr (Traits.HaveBmi2()) {
            return __builtin_ia32_pext_si(x, mask);
        }
#endif
        x &= mask;
        x |= x >> 1;
        x &= 0x33u;
        x |= x >> 2;
        x &= 0x0Fu;
        return x;
    }

    template<int OutputBits = 8, ArchTraits Traits = {}>
    constexpr Bitmask<1> BitExtractEven(UInt<2> x) {
        if constexpr (OutputBits <= 4) {
            return BitExtractEven<OutputBits>(UInt<1>(x));
        } else {
            static_assert(OutputBits <= 8);
            constexpr UInt<2> mask = 0x5555u >> ((8 - OutputBits) * 2);
#if __has_builtin(__builtin_ia32_pext_si)
            if constexpr (Traits.HaveBmi2()) {
                return __builtin_ia32_pext_si(x, mask);
            }
#endif
            x &= mask;
            x |= x >> 1;
            x &= 0x3333u;
            x |= x >> 2;
            x &= 0x0F0Fu;
            x |= x >> 4;
            return x;
        }
    }

    template<int OutputBits = 16, ArchTraits Traits = {}>
    constexpr Bitmask<OutputBits> BitExtractEven(UInt<4> x) {
        if constexpr (OutputBits <= 4) {
            return BitExtractEven<OutputBits>(UInt<1>(x));
        } else if constexpr (OutputBits <= 8) {
            return BitExtractEven<OutputBits>(UInt<2>(x));
        } else {
            static_assert(OutputBits <= 16);
            constexpr UInt<4> mask = 0x5555'5555u >> ((16 - OutputBits) * 2);
#if __has_builtin(__builtin_ia32_pext_si)
            if constexpr (Traits.HaveBmi2()) {
                return __builtin_ia32_pext_si(x, mask);
            }
#endif
            x &= mask;
            x |= x >> 1;
            x &= 0x3333'3333u;
            x |= x >> 2;
            x &= 0x0F0F'0F0Fu;
            x |= x >> 4;
            x &= 0x00FF'00FFu;
            x |= x >> 8;
            return x;
        }
    }

    template<int OutputBits = 32, ArchTraits Traits = {}>
    constexpr Bitmask<OutputBits> BitExtractEven(UInt<8> x) {
        if constexpr (OutputBits <= 4) {
            return BitExtractEven<OutputBits>(UInt<1>(x));
        } else if constexpr (OutputBits <= 8) {
            return BitExtractEven<OutputBits>(UInt<2>(x));
        } else if constexpr (OutputBits <= 16) {
            return BitExtractEven<OutputBits>(UInt<4>(x));
        } else {
            static_assert(OutputBits <= 32);
            constexpr UInt<8> mask = 0x5555'5555'5555'5555ull >> ((32 - OutputBits) * 2);
#if __has_builtin(__builtin_ia32_pext_si)
            if constexpr (Traits.HaveBmi2()) {
#    if __has_builtin(__builtin_ia32_pext_di)
                return __builtin_ia32_pext_di(x, mask);
#    else
                return __builtin_ia32_pext_si(x, static_cast<unsigned>(mask)) |
                       (__builtin_ia32_pext_si(x >> 32, mask >> 32) << 16);
#    endif
            }
#endif
            x &= mask;
            x |= x >> 1;
            x &= 0x3333'3333'3333'3333ull;
            x |= x >> 2;
            x &= 0x0F0F'0F0F'0F0F'0F0Full;
            x |= x >> 4;
            x &= 0x00FF'00FF'00FF'00FFull;
            x |= x >> 8;
            x &= 0x0000'FFFF'0000'FFFFull;
            x |= x >> 16;
            return x;
        }
    }

    // input bits must be 0 for all bits > InputBits
    template<int InputBits = -1, ArchTraits Traits = {}>
    constexpr auto DuplicateEachBit(std::unsigned_integral auto x) {
        constexpr int inputBits = InputBits == -1 ? sizeof(x) * __CHAR_BIT__ : InputBits;
        static_assert(inputBits >= 1);
        static_assert(sizeof(x) * __CHAR_BIT__ >= inputBits);
        if constexpr (inputBits <= 8) {
            constexpr UInt<2> mask = 0x5555u >> ((8 - inputBits) * 2);
            if constexpr (inputBits == 1) {
                return UInt<1>(x * 3u);
            }
#if __has_builtin(__builtin_ia32_pdep_si)
            else if constexpr (Traits.HaveBmi2()) {
                return Bitmask<inputBits * 2>(3u * __builtin_ia32_pdep_si(x, mask));
            }
#endif
            else if constexpr (inputBits == 2) {                // 0000'00BA
                return UInt<1>(((x + 0b0010u) & 0b0101u) * 3u); // 0B?A -> 0B0A -> BBAA
            } else if constexpr (inputBits <= 4)                // 0000'DCBA
            {
                x = ((x << 2) | x) & 0b0011'0011u;                // 00DC'??BA -> 00DC'00BA
                return UInt<1>(((x + 0b0010'0010u) & mask) * 3u); // -> DDCC'BBAA
            } else {                                              // HGFE'DCBA
                UInt<2> y = ((x << 4) | x) & 0x0F0Fu;             // HGFE'0000'DCBA
                y |= y << 2;                                      // 00HG'??FE'00DC'??BA
                y &= 0x3333u;                                     // 00HG'00FE'00DC'00BA
                y += 0x2222u;                                     // 0H?G'0F?E'0D?C'0B?A
                return UInt<2>((y & mask) * 3u);                  // HHGG'FFEE'DDCC'BBAA
            }
        } else if constexpr (inputBits <= 16) {
            constexpr UInt<4> mask = 0x5555'5555u >> ((16 - inputBits) * 2);
#if __has_builtin(__builtin_ia32_pdep_si)
            if constexpr (Traits.HaveBmi2()) {
                return 3u * __builtin_ia32_pdep_si(x, mask);
            }
#endif
            UInt<4> y = ((x << 8) | x) & 0x00FF00FFu;
            y |= y << 4;
            y &= 0x0F0F'0F0Fu;
            y |= y << 2;
            y &= 0x3333'3333u;
            return ((y + 0x2222'2222u) & mask) * 3;
        } else if constexpr (inputBits <= 32) {
            constexpr UInt<8> mask = 0x5555'5555'5555'5555u >> ((32 - inputBits) * 2);
#if __has_builtin(__builtin_ia32_pdep_si)
            if constexpr (Traits.HaveBmi2()) {
#    if __has_builtin(__builtin_ia32_pdep_di)
                return 3ull * __builtin_ia32_pdep_di(x, mask);
#    else
                const UInt<8> hi = 3 * __builtin_ia32_pdep_si(x >> 16, mask >> 32);
                return (3u * __builtin_ia32_pdep_si(x, static_cast<unsigned>(mask))) | hi << 32;
#    endif
            }
#endif
            UInt<8> y = ((x & 0xFFFF'0000ull) << 16) | (x & 0x0000'FFFFu);
            y |= y << 8;
            y &= 0x00FF'00FF'00FF'00FFull;
            y |= y << 4;
            y &= 0x0F0F'0F0F'0F0F'0F0Full;
            y |= y << 2;
            y &= 0x3333'3333'3333'3333ull;
            return ((y + 0x2222'2222'2222'2222ull) & mask) * 3;
        } else {
            return TrivialPair{DuplicateEachBit(UInt<4>(x)),
                               DuplicateEachBit<inputBits - 32>(Bitmask<inputBits - 32>(x >> 32))};
        }
    }

    template<int InputBits = -1, typename U0, typename U1>
    constexpr auto DuplicateEachBit(const TrivialPair<U0, U1>& x) {
        static_assert(InputBits != -1 || std::is_unsigned_v<U1>);
        constexpr int inputBits = InputBits == -1 ? (sizeof(U0) + sizeof(U1)) * __CHAR_BIT__ : InputBits;
        constexpr int in0 = Min(int(sizeof(U0)) * __CHAR_BIT__, inputBits);
        constexpr int in1 = inputBits - in0;
        if constexpr (in1 == 0) {
            return DuplicateEachBit<in0>(x.first);
        } else {
            return TrivialPair{DuplicateEachBit<in0>(x.first), DuplicateEachBit<in1>(x.second)};
        }
    }

    template<VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline TV X86ComplexMultiplies(TV x, TV y) {
        using Tp = VecValueType<TV>;
        using VO = VecOps<TV>;

        static_assert(Traits.HaveFma());
        static_assert(std::is_floating_point_v<Tp>);

        if constexpr (!Traits.HaveAvx512fp16() && sizeof(Tp) == 2) {
            return VecCast<Tp>(X86ComplexMultiplies(VecCast<float>(x), VecCast<float>(y)));
        } else if constexpr (sizeof(TV) < 16) {
            return VO::Extract(X86ComplexMultiplies(VecZeroPadTo16(x), VecZeroPadTo16(y)));
        }

        else {
            TV xReal = VO::DupEven(x);
            TV xImag = VO::DupOdd(x);
            TV ySwapped = VO::SwapNeighbors(y);

            if constexpr (sizeof(x) == 16 && sizeof(Tp) == 2) {
                return __builtin_ia32_vfmaddsubph128_mask(xReal, y, xImag * ySwapped, -1);
            } else if constexpr (sizeof(x) == 32 && sizeof(Tp) == 2) {
                return __builtin_ia32_vfmaddsubph256_mask(xReal, y, xImag * ySwapped, -1);
            } else if constexpr (sizeof(x) == 64 && sizeof(Tp) == 2) {
                return __builtin_ia32_vfmaddsubph512_mask(xReal, y, xImag * ySwapped, -1, 0x04);
            }

            else if constexpr (sizeof(x) == 16 && sizeof(Tp) == 4) {
                return __builtin_ia32_vfmaddsubps(xReal, y, xImag * ySwapped);
            } else if constexpr (sizeof(x) == 32 && sizeof(Tp) == 4) {
                return __builtin_ia32_vfmaddsubps256(xReal, y, xImag * ySwapped);
            } else if constexpr (sizeof(x) == 64 && sizeof(Tp) == 4) {
                return __builtin_ia32_vfmaddsubps512_mask(xReal, y, xImag * ySwapped, -1, 0x04);
            }

            else if constexpr (sizeof(x) == 16 && sizeof(Tp) == 8) {
                return __builtin_ia32_vfmaddsubpd(xReal, y, xImag * ySwapped);
            } else if constexpr (sizeof(x) == 32 && sizeof(Tp) == 8) {
                return __builtin_ia32_vfmaddsubpd256(xReal, y, xImag * ySwapped);
            } else if constexpr (sizeof(x) == 64 && sizeof(Tp) == 8) {
                return __builtin_ia32_vfmaddsubpd512_mask(xReal, y, xImag * ySwapped, -1, 0x04);
            }

            else {
                static_assert(false);
            }
        }
    }

    // FIXME: Work around PR121688
    template<VecBuiltin UV, VecBuiltin TV>
    [[gnu::always_inline]]
    inline UV X86CvtF16c(TV v) {
        constexpr bool fromF16 = std::is_same_v<VecValueType<TV>, _Float16>;
        constexpr bool toF16 = !fromF16;
        if constexpr (toF16 && !std::is_same_v<VecValueType<TV>, float>) {
            return X86CvtF16c<UV>(VecCast<float>(v));
        } else if constexpr (fromF16 && !std::is_same_v<VecValueType<UV>, float>) {
            return VecCast<UV>(X86CvtF16c<VecBuiltinType<float, kWidthOf<TV>>>(v));
        } else if constexpr (fromF16) {
            const auto vi = VecBitCast<X86IntrinInt<_Float16>>(v);
            if constexpr (sizeof(TV) == 4) {
                return VecSplitLo(__builtin_ia32_vcvtph2ps(VecZeroPadTo16(vi)));
            } else if constexpr (sizeof(TV) == 8) {
                return __builtin_ia32_vcvtph2ps(VecZeroPadTo16(vi));
            } else if constexpr (sizeof(TV) == 16) {
                return __builtin_ia32_vcvtph2ps256(vi);
            } else if constexpr (sizeof(TV) == 32) {
                return __builtin_ia32_vcvtph2ps512_mask(vi, VecBuiltinType<float, 16>(), -1, 4);
            } else if constexpr (sizeof(TV) >= 64) {
                return VecConcat(X86CvtF16c<HalfVecBuiltinT<UV>>(VecSplitLo(v)),
                                 X86CvtF16c<HalfVecBuiltinT<UV>>(VecSplitHi(v)));
            } else {
                static_assert(false);
            }
        } else if constexpr (sizeof(TV) == 8) {
            return reinterpret_cast<UV>(VecSplitLo(VecSplitLo(__builtin_ia32_vcvtps2ph(VecZeroPadTo16(v), 4))));
        } else if constexpr (sizeof(TV) == 16) {
            return reinterpret_cast<UV>(VecSplitLo(__builtin_ia32_vcvtps2ph(v, 4)));
        } else if constexpr (sizeof(TV) == 32) {
            return reinterpret_cast<UV>(__builtin_ia32_vcvtps2ph256(v, 4));
        } else if constexpr (sizeof(TV) == 64) {
            return reinterpret_cast<UV>(__builtin_ia32_vcvtps2ph512_mask(v, 4, VecBuiltinType<short, 16>(), -1));
        } else if constexpr (sizeof(TV) >= 128) {
            return VecConcat(X86CvtF16c<HalfVecBuiltinT<UV>>(VecSplitLo(v)),
                             X86CvtF16c<HalfVecBuiltinT<UV>>(VecSplitHi(v)));
        } else {
            static_assert(false);
        }
    }

    /** @internal
     * AVX instructions typically work per 128-bit Chunk. Horizontal operations thus produce vectors
     * where the two 128-bit chunks in the center are swapped. This function works as a fix-up step.
     */
    template<VecBuiltin TV>
    [[gnu::always_inline]]
    inline TV X86Swizzle4x64Acbd(TV x) {
        static_assert(sizeof(TV) == 32);
        using UV = VecBuiltinTypeBytes<long long, 32>;
        return reinterpret_cast<TV>(__builtin_shufflevector(reinterpret_cast<UV>(x), UV(), 0, 2, 1, 3));
    }

    /** @internal
     * Like __builtin_convertvector but with a precondition that input values are either 0 or -1.
     */
    template<VecBuiltin To, VecBuiltin From>
    [[gnu::always_inline]]
    inline To X86CvtVecmask(From k) {
        using T0 = VecValueType<From>;
        using T1 = VecValueType<To>;
        if constexpr (sizeof(From) > sizeof(To) && sizeof(From) < 16) {
            using ToPadded = VecBuiltinTypeBytes<T1, sizeof(To) * 16 / sizeof(From)>;
            return VecOps<To>::Extract(X86CvtVecmask<ToPadded>(VecZeroPadTo16(k)));
        } else if constexpr (sizeof(T0) == 2 && sizeof(T1) == 1) // -> packsswb
        {
            if constexpr (sizeof(k) == 16) {
                return reinterpret_cast<To>(VecSplitLo(__builtin_ia32_packsswb128(k, k)));
            } else if constexpr (sizeof(k) == 32) {
                return reinterpret_cast<To>(VecSplitLo(X86Swizzle4x64Acbd(__builtin_ia32_packsswb256(k, k))));
            } else {
                static_assert(false);
            }
        } else {
            static_assert(false, "TODO");
        }
    }

    /** @internal
     * Overload that concatenates @p k0 and @p k1 while converting.
     */
    template<VecBuiltin To, VecBuiltin From>
    [[gnu::always_inline]]
    inline To X86CvtVecmask(From k0, From k1) {
        using T0 = VecValueType<From>;
        using T1 = VecValueType<To>;
        static_assert(sizeof(From) >= 16);
        if constexpr (sizeof(T0) == 2 && sizeof(T1) == 1) // -> packsswb
        {
            if constexpr (sizeof(k0) == 16) {
                return reinterpret_cast<To>(__builtin_ia32_packsswb128(k0, k1));
            } else if constexpr (sizeof(k0) == 32) {
                return reinterpret_cast<To>(X86Swizzle4x64Acbd(__builtin_ia32_packsswb256(k0, k1)));
            } else {
                static_assert(false);
            }
        } else {
            static_assert(false, "TODO");
        }
    }

    template<ArchTraits Traits = {}, VecBuiltin TV>
    [[gnu::always_inline]]
    inline Bitmask<kWidthOf<TV>> X86CvtVecmaskToBitmask(const TV k) {
        using Tp = VecValueType<TV>;
        constexpr int bytes = sizeof(Tp);
        constexpr int np = kWidthOf<TV>;
        constexpr bool vl = Traits.HaveAvx512vl();
        constexpr bool bw = Traits.HaveAvx512bw();
        constexpr bool dq = Traits.HaveAvx512dq();
        if constexpr (vl && bw && bytes == 1 && sizeof(k) == 16) {
            return __builtin_ia32_cvtb2mask128(k);
        } else if constexpr (vl && bw && bytes == 1 && sizeof(k) == 32) {
            return __builtin_ia32_cvtb2mask256(k);
        } else if constexpr (bw && bytes == 1 && sizeof(k) == 64) {
            return __builtin_ia32_cvtb2mask512(k);
        } else if constexpr (vl && bw && bytes == 2 && sizeof(k) == 16) {
            return __builtin_ia32_cvtw2mask128(k);
        } else if constexpr (vl && bw && bytes == 2 && sizeof(k) == 32) {
            return __builtin_ia32_cvtw2mask256(k);
        } else if constexpr (bw && bytes == 2 && sizeof(k) == 64) {
            return __builtin_ia32_cvtw2mask512(k);
        } else if constexpr (vl && dq && bytes == 4 && sizeof(k) == 16) {
            return __builtin_ia32_cvtd2mask128(k);
        } else if constexpr (vl && dq && bytes == 4 && sizeof(k) == 32) {
            return __builtin_ia32_cvtd2mask256(k);
        } else if constexpr (dq && bytes == 4 && sizeof(k) == 64) {
            return __builtin_ia32_cvtd2mask512(k);
        } else if constexpr (vl && dq && bytes == 8 && sizeof(k) == 16) {
            return __builtin_ia32_cvtq2mask128(k);
        } else if constexpr (vl && dq && bytes == 8 && sizeof(k) == 32) {
            return __builtin_ia32_cvtq2mask256(k);
        } else if constexpr (dq && bytes == 8 && sizeof(k) == 64) {
            return __builtin_ia32_cvtq2mask512(k);
        } else if constexpr (vl && dq && bw && sizeof(k) < 16) {
            return X86CvtVecmaskToBitmask(VecZeroPadTo16(k));
        } else if constexpr (bytes != 2) { // movmskb would duplicate each bit
            return X86Movmsk(k);
        } else if constexpr (Traits.HaveBmi2()) {
            return BitExtractEven<np>(X86Movmsk(k));
        } else {
            return X86CvtVecmaskToBitmask(X86CvtVecmask<VecBuiltinType<char, np>>(k));
        }
    }

    /** @internal
     * AVX512 masked (converting) loads
     *
     * @note AVX512VL and AVX512BW is required
     */
    template<VecBuiltin TV, typename Up, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline TV X86MaskedLoad(const Up* mem, std::unsigned_integral auto k) {
        static_assert(Traits.HaveAvx512vl() && Traits.HaveAvx512bw());
        using Tp = VecValueType<TV>;
        constexpr int n = kWidthOf<TV>;
        if constexpr (!ConvertsTrivially<Up, Tp>) {
            const auto uvec = X86MaskedLoad<VecBuiltinType<CanonicalVecTypeT<Up>, n>>(mem, k);
            return VecCast<TV>(uvec);
        } else if constexpr (sizeof(TV) < 16) {
            return VecOps<TV>::Extract(X86MaskedLoad<VecBuiltinTypeBytes<Tp, 16>>(mem, k));
        } else if constexpr (sizeof(TV) > 64) {
            return VecConcat(X86MaskedLoad<VecBuiltinType<Tp, n / 2>>(mem, k),
                             X86MaskedLoad<VecBuiltinType<Tp, n / 2>>(mem + n / 2, k >> n / 2));
        } else if constexpr (sizeof(TV) == 64) {
            const auto* src = reinterpret_cast<const X86IntrinType<Up>*>(mem);
            const VecBuiltinTypeBytes<X86IntrinType<Up>, 64> z = {};
            if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 4) {
                return __builtin_ia32_loadups512_mask(src, z, k);
            } else if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 8) {
                return __builtin_ia32_loadupd512_mask(src, z, k);
            } else if constexpr (sizeof(Tp) == 1) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddquqi512_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 2) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddquhi512_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 4) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddqusi512_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 8) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddqudi512_mask(src, z, k));
            } else {
                static_assert(false);
            }
        } else if constexpr (sizeof(TV) == 32) {
            const auto* src = reinterpret_cast<const X86IntrinType<Up>*>(mem);
            const VecBuiltinTypeBytes<X86IntrinType<Up>, 32> z = {};
            if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 4) {
                return __builtin_ia32_loadups256_mask(src, z, k);
            } else if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 8) {
                return __builtin_ia32_loadupd256_mask(src, z, k);
            } else if constexpr (sizeof(Tp) == 1) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddquqi256_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 2) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddquhi256_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 4) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddqusi256_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 8) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddqudi256_mask(src, z, k));
            } else {
                static_assert(false);
            }
        } else if constexpr (sizeof(TV) == 16) {
            const auto* src = reinterpret_cast<const X86IntrinType<Up>*>(mem);
            const VecBuiltinTypeBytes<X86IntrinType<Up>, 16> z = {};
            if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 4) {
                return __builtin_ia32_loadups128_mask(src, z, k);
            } else if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 8) {
                return __builtin_ia32_loadupd128_mask(src, z, k);
            } else if constexpr (sizeof(Tp) == 1) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddquqi128_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 2) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddquhi128_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 4) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddqusi128_mask(src, z, k));
            } else if constexpr (sizeof(Tp) == 8) {
                return reinterpret_cast<TV>(__builtin_ia32_loaddqudi128_mask(src, z, k));
            } else {
                static_assert(false);
            }
        } else {
            static_assert(false);
        }
    }

    /** @internal
     * AVX(2) masked loads (only trivial conversions)
     */
    template<VecBuiltin TV, typename Up, VecBuiltin KV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline TV X86MaskedLoad(const Up* mem, const KV k) {
        using Tp = VecValueType<TV>;
        static_assert(Traits.HaveAvx() && ConvertsTrivially<Up, Tp> && sizeof(Up) >= 4);
        constexpr int n = kWidthOf<TV>;
        using IV = VecBuiltinType<X86IntrinInt<Tp>, n>;
        const auto vk = reinterpret_cast<IV>(k);
        if constexpr (sizeof(TV) < 16) {
            return VecOps<TV>::Extract(X86MaskedLoad<VecBuiltinTypeBytes<Tp, 16>>(mem, VecZeroPadTo16(k)));
        } else if constexpr (Traits.HaveAvx2() && std::is_integral_v<Up>) {
            const auto* src = reinterpret_cast<const VecBuiltinType<X86IntrinInt<Up>, n>*>(mem);
            if constexpr (sizeof(Up) == 4 && sizeof(TV) == 32) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadd256(src, vk));
            } else if constexpr (sizeof(Up) == 4 && sizeof(TV) == 16) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadd(src, vk));
            } else if constexpr (sizeof(Up) == 8 && sizeof(TV) == 32) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadq256(src, vk));
            } else if constexpr (sizeof(Up) == 8 && sizeof(TV) == 16) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadq(src, vk));
            } else {
                static_assert(false);
            }
        } else if constexpr (sizeof(Up) == 4) {
            const auto* src = reinterpret_cast<const VecBuiltinType<float, n>*>(mem);
            if constexpr (sizeof(TV) == 32) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadps256(src, vk));
            } else if constexpr (sizeof(TV) == 16) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadps(src, vk));
            } else {
                static_assert(false);
            }
        } else {
            const auto* src = reinterpret_cast<const VecBuiltinType<double, n>*>(mem);
            if constexpr (sizeof(TV) == 32) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadpd256(src, vk));
            } else if constexpr (sizeof(TV) == 16) {
                return reinterpret_cast<TV>(__builtin_ia32_maskloadpd(src, vk));
            } else {
                static_assert(false);
            }
        }
    }

    /** @internal
     * AVX512 masked stores
     *
     * @note AVX512VL is required
     */
    template<VecBuiltin TV, typename Up>
    [[gnu::always_inline]]
    inline void X86MaskedStore(const TV v, Up* mem, std::unsigned_integral auto k) {
        using Tp = VecValueType<TV>;
        constexpr int n = kWidthOf<TV>;
        [[maybe_unused]] const auto w = VecBitCast<X86IntrinType<Tp>>(v);
        if constexpr (sizeof(TV) == 64) {
            if constexpr (sizeof(Tp) > sizeof(Up) && std::is_integral_v<Tp> && std::is_integral_v<Up>) {
                auto* dst = reinterpret_cast<VecBuiltinType<X86IntrinInt<Up>, n>*>(mem);
                if constexpr (sizeof(Tp) == 2) {
                    __builtin_ia32_pmovwb512mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4 && sizeof(Up) == 1) {
                    __builtin_ia32_pmovdb512mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4 && sizeof(Up) == 2) {
                    __builtin_ia32_pmovdw512mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 1) {
                    __builtin_ia32_pmovqb512mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 2) {
                    __builtin_ia32_pmovqw512mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 4) {
                    __builtin_ia32_pmovqd512mem_mask(dst, w, k);
                } else {
                    static_assert(false);
                }
            } else if constexpr (ConvertsTrivially<Tp, Up>) {
                auto* dst = reinterpret_cast<X86IntrinType<Up>*>(mem);
                if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 4) {
                    __builtin_ia32_storeups512_mask(dst, w, k);
                } else if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 8) {
                    __builtin_ia32_storeupd512_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 1) {
                    __builtin_ia32_storedquqi512_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 2) {
                    __builtin_ia32_storedquhi512_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4) {
                    __builtin_ia32_storedqusi512_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8) {
                    __builtin_ia32_storedqudi512_mask(dst, w, k);
                } else {
                    static_assert(false);
                }
            } else if constexpr (sizeof(Tp) >= sizeof(Up)) {
                if constexpr (std::is_floating_point_v<Tp> && std::is_integral_v<Up> && sizeof(Tp) > sizeof(Up)) {
                    X86MaskedStore(VecCast<Detail::IntegerForSize<sizeof(Tp)>>(v), mem, k);
                } else {
                    X86MaskedStore(VecCast<Up>(v), mem, k);
                }
            } else {
                X86MaskedStore(VecSplitLo(v), mem, Bitmask<n / 2>(k));
                X86MaskedStore(VecSplitHi(v), mem + n / 2, Bitmask<n / 2>(k >> (n / 2)));
            }
        } else if constexpr (sizeof(TV) == 32) {
            if constexpr (sizeof(Tp) > sizeof(Up) && std::is_integral_v<Tp> && std::is_integral_v<Up>) {
                auto* dst = reinterpret_cast<VecBuiltinType<X86IntrinInt<Up>, n>*>(mem);
                if constexpr (sizeof(Tp) == 2) {
                    __builtin_ia32_pmovwb256mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4 && sizeof(Up) == 1) {
                    __builtin_ia32_pmovdb256mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4 && sizeof(Up) == 2) {
                    __builtin_ia32_pmovdw256mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 1) {
                    __builtin_ia32_pmovqb256mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 2) {
                    __builtin_ia32_pmovqw256mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 4) {
                    __builtin_ia32_pmovqd256mem_mask(dst, w, k);
                } else {
                    static_assert(false);
                }
            } else if constexpr (ConvertsTrivially<Tp, Up>) {
                auto* dst = reinterpret_cast<X86IntrinType<Up>*>(mem);
                if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 4) {
                    __builtin_ia32_storeups256_mask(dst, w, k);
                } else if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 8) {
                    __builtin_ia32_storeupd256_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 1) {
                    __builtin_ia32_storedquqi256_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 2) {
                    __builtin_ia32_storedquhi256_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4) {
                    __builtin_ia32_storedqusi256_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8) {
                    __builtin_ia32_storedqudi256_mask(dst, w, k);
                } else {
                    static_assert(false);
                }
            } else if constexpr (2 * sizeof(Tp) >= sizeof(Up)) {
                X86MaskedStore(VecCast<Up>(v), mem, k);
            } else {
                X86MaskedStore(VecSplitLo(v), mem, Bitmask<n / 2>(k));
                X86MaskedStore(VecSplitHi(v), mem + n / 2, Bitmask<n / 2>(k >> (n / 2)));
            }
        } else if constexpr (sizeof(TV) == 16) {
            if constexpr (sizeof(Tp) > sizeof(Up) && std::is_integral_v<Tp> && std::is_integral_v<Up>) {
                auto* dst = reinterpret_cast<VecBuiltinType<X86IntrinInt<Up>, n>*>(mem);
                if constexpr (sizeof(Tp) == 2) {
                    __builtin_ia32_pmovwb128mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4 && sizeof(Up) == 1) {
                    __builtin_ia32_pmovdb128mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4 && sizeof(Up) == 2) {
                    __builtin_ia32_pmovdw128mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 1) {
                    __builtin_ia32_pmovqb128mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 2) {
                    __builtin_ia32_pmovqw128mem_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8 && sizeof(Up) == 4) {
                    __builtin_ia32_pmovqd128mem_mask(reinterpret_cast<unsigned long long*>(mem), w, k);
                } else {
                    static_assert(false);
                }
            } else if constexpr (ConvertsTrivially<Tp, Up>) {
                auto* dst = reinterpret_cast<X86IntrinType<Up>*>(mem);
                if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 4) {
                    __builtin_ia32_storeups128_mask(dst, w, k);
                } else if constexpr (std::is_floating_point_v<Tp> && sizeof(Tp) == 8) {
                    __builtin_ia32_storeupd128_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 1) {
                    __builtin_ia32_storedquqi128_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 2) {
                    __builtin_ia32_storedquhi128_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 4) {
                    __builtin_ia32_storedqusi128_mask(dst, w, k);
                } else if constexpr (sizeof(Tp) == 8) {
                    __builtin_ia32_storedqudi128_mask(dst, w, k);
                } else {
                    static_assert(false);
                }
            } else if constexpr (4 * sizeof(Tp) >= sizeof(Up)) {
                X86MaskedStore(VecCast<Up>(v), mem, k);
            } else {
                X86MaskedStore(VecCast<Up>(VecSplitLo(v)), mem, Bitmask<n / 2>(k));
                X86MaskedStore(VecCast<Up>(VecSplitHi(v)), mem + n / 2, Bitmask<n / 2>(k >> (n / 2)));
            }
        } else {
            X86MaskedStore(VecZeroPadTo16(v), mem, k);
        }
    }

    /** @internal
     * AVX(2) masked stores
     */
    template<VecBuiltin TV, typename Up, VecBuiltin KV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    inline void X86MaskedStore(const TV v, Up* mem, const KV k) {
        using Tp = VecValueType<TV>;
        constexpr int n = kWidthOf<TV>;
        static_assert(sizeof(Tp) == 4 || sizeof(Tp) == 8);
        auto* dst = reinterpret_cast<VecBuiltinType<X86IntrinType<Up>, n>*>(mem);
        [[maybe_unused]] const auto w = VecBitCast<X86IntrinType<Tp>>(v);
        if constexpr (sizeof(TV) < 16) {
            X86MaskedStore(VecZeroPadTo16(v), mem, VecZeroPadTo16(k));
        } else if constexpr (Traits.HaveAvx2() && std::is_integral_v<Tp>) {
            if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
                __builtin_ia32_maskstored256(dst, k, w);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
                __builtin_ia32_maskstored(dst, k, w);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
                __builtin_ia32_maskstoreq256(dst, k, w);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
                __builtin_ia32_maskstoreq(dst, k, w);
            } else {
                static_assert(false);
            }
        } else {
            if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 4) {
                __builtin_ia32_maskstoreps256(dst, k, w);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 4) {
                __builtin_ia32_maskstoreps(dst, k, w);
            } else if constexpr (sizeof(TV) == 32 && sizeof(Tp) == 8) {
                __builtin_ia32_maskstorepd256(dst, k, w);
            } else if constexpr (sizeof(TV) == 16 && sizeof(Tp) == 8) {
                __builtin_ia32_maskstorepd(dst, k, w);
            } else {
                static_assert(false);
            }
        }
    }

} // namespace Sora::Math::Simd
