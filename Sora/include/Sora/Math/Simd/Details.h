/**
 * @file Details.h
 * @brief Core concepts, ABI tags, traits, and utilities for SIMD types.
 * @ingroup Math
 */
#pragma once

#include "Portability.h"

#include <bit>

#include <functional> // std::plus, std::minus, std::multiplies, ...
#include <utility>    // std::integer_sequence, etc.
#include <cmath>      // for math_errhandling :(
#include <concepts>
#include <cstdint>
#include <limits>
#include <span> // for std::dynamic_extent

#if __CHAR_BIT__ != 8
// There are simply too many constants and bit operators that currently depend on CHAR_BIT == 8.
// Generalization to CHAR_BIT != 8 does not make sense without testability (i.e. a Test target).
#    error "<simd> is not supported for CHAR_BIT != 8"
#endif

#ifndef SORA_SIMD_NOEXCEPT
/** @internal
 * For unit-testing preconditions, use this macro to remove noexcept.
 */
#    define SORA_SIMD_NOEXCEPT noexcept
#endif

#define SORA_SIMD_TOSTRING_IMPL(x) #x
#define SORA_SIMD_TOSTRING(x) SORA_SIMD_TOSTRING_IMPL(x)

// This is used for unit-testing precondition checking
#define SORA_SIMD_PRECONDITION(expr, msg, ...) SORA_SIMD_ASSERT(expr)

namespace Sora::Math::Simd {

    template<typename Tp>
    inline constexpr Tp kIota = [] { static_assert(false, "invalid kIota specialization"); }();

    // [simd.general] vectorizable types
    template<typename Tp>
    concept ComplexLikeImpl = std::same_as<Tp, std::complex<typename Tp::value_type>>;

    /** @internal
     * Satisfied if @p Tp implements the std::complex interface.
     */
    template<typename Tp>
    concept ComplexLike = ComplexLikeImpl<std::remove_cvref_t<Tp>>;

    template<typename Tp>
    concept VectorizableScalar = std::same_as<std::remove_cv_t<Tp>, Tp>
#ifdef __STDCPP_BFLOAT16_T__
                                 && !std::same_as<Tp, std::bfloat16_t>
#endif
                                 && ((std::integral<Tp> && sizeof(Tp) <= sizeof(0ULL) && !std::same_as<Tp, bool>) ||
                                     (std::floating_point<Tp> && sizeof(Tp) <= sizeof(double)));

    // [simd.general] p2
    template<typename Tp>
    concept Vectorizable =
        VectorizableScalar<Tp> || (ComplexLikeImpl<Tp> && VectorizableScalar<typename Tp::value_type> &&
                                   std::floating_point<typename Tp::value_type>);

    /** @internal
     * Describes variants of Abi.
     */
    enum class AbiVariant : uint64_t {
        kBitMask = 0x01,      // AVX512 bit-masks
        kMaskVariants = 0x0f, // vector masks if bits [0:3] are 0
        kCxIleav = 0x10,      // Store std::complex components interleaved (ririri...)
                              // Mask elements are stored for both    (001122...)
        kCxCtgus = 0x20,      // ... or Store std::complex components contiguously (rrrr iiii)
                              // Mask elements are Store for one component    (0123)
        kCxVariants = kCxIleav | kCxCtgus,
        _ = std::numeric_limits<uint64_t>::max()
    };

    /** @internal
     * Return @p input with only bits set that are set in any of @p toKeep.
     */
    consteval AbiVariant FilterAbiVariant(AbiVariant input, std::same_as<AbiVariant> auto... toKeep) {
        using Up = std::underlying_type_t<AbiVariant>;
        return static_cast<AbiVariant>(static_cast<Up>(input) & (static_cast<Up>(toKeep) | ...));
    }

    /** @internal
     * Alias for an unsigned integer type T such that sizeof(T) equals Bytes.
     */
    template<std::size_t Bytes>
    using UInt = std::make_unsigned_t<Detail::IntegerForSize<Bytes>>;

    /** @internal
     * Divide @p x by @p y while rounding up instead of down.
     *
     * Preconditions: x >= 0 && y > 0.
     */
    template<typename Tp>
    consteval Tp DivCeil(Tp x, Tp y) {
        return (x + y - 1) / y;
    }

    /** @internal
     * Alias for an unsigned integer type that can Store at least @p NBits bits.
     */
    template<int NBits>
        requires(NBits > 0 && NBits <= std::numeric_limits<unsigned long long>::digits)
    using Bitmask = UInt<DivCeil(std::bit_ceil(unsigned(NBits)), unsigned(__CHAR_BIT__))>;

    /** @internal
     * Map a given type @p Tp to an equivalent type.
     *
     * This helps with reducing the necessary branches && casts in the implementation as well as
     * reducing the number of template instantiations.
     */
    template<typename Tp>
    struct CanonicalVecType {
        using Type = Tp;
    };

    template<typename Tp>
    using CanonicalVecTypeT = typename CanonicalVecType<Tp>::Type;

#if __SIZEOF_INT__ == __SIZEOF_LONG__
    template<>
    struct CanonicalVecType<long> {
        using Type = int;
    };

    template<>
    struct CanonicalVecType<unsigned long> {
        using Type = unsigned int;
    };
#elif __SIZEOF_LONG_LONG__ == __SIZEOF_LONG__
    template<>
    struct CanonicalVecType<long> {
        using type = long long;
    };

    template<>
    struct CanonicalVecType<unsigned long> {
        using type = unsigned long long;
    };
#endif

    template<typename Tp>
        requires std::is_enum_v<Tp>
    struct CanonicalVecType<Tp> {
        using Type = CanonicalVecType<std::underlying_type_t<Tp>>::type;
    };

    template<>
    struct CanonicalVecType<char>
#if __CHAR_UNSIGNED__
    {
        using type = unsigned char;
    };
#else
    {
        using Type = signed char;
    };
#endif

    template<>
    struct CanonicalVecType<char8_t> {
        using Type = unsigned char;
    };

    template<>
    struct CanonicalVecType<char16_t> {
        using Type = std::uint_least16_t;
    };

    template<>
    struct CanonicalVecType<char32_t> {
        using Type = std::uint_least32_t;
    };

    template<>
    struct CanonicalVecType<wchar_t> {
        using Type =
            std::conditional_t<std::is_signed_v<wchar_t>, Sora::Math::Simd::Detail::IntegerForSize<sizeof(wchar_t)>,
                               Sora::Math::Simd::UInt<sizeof(wchar_t)>>;
    };

#if defined(__FLT64_DIG__) && defined(SORA_SIMD_DOUBLE_IS_IEEE_BINARY64)
    template<>
    struct CanonicalVecType<_Float64> {
        using type = double;
    };
#endif

#if defined(__FLT32_DIG__) && defined(SORA_SIMD_FLOAT_IS_IEEE_BINARY32)
    template<>
    struct CanonicalVecType<_Float32> {
        using type = float;
    };
#endif

    /** @internal
     * @brief This ABI tag determines the data member(s) of BasicVector and BasicMask.
     *
     * `Nreg` determines the number of recursive BasicVector/BasicMask data members where `Nreg` is
     * equal to 1. With `Nreg` equal to 1, the BasicVector/BasicMask holds one vector builtin ( `Np`
     * std::greater than 1) or a scalar (`Np` equal to 1).
     * @f$\lceil\frac{\mathtt{Np}}{\mathtt{Nreg}}\rceil@f$ therefore determines the number of elements
     * in a register (except for a remainder where it can be smaller). If `Np` equals `Nreg`, (the
     * aforementioned quotient is 1), then BasicVector (recursively) holds non-vector data members and
     * BasicMask holds bools.
     *
     * The `Var` parameter determines details about the data member in the one register case. Masks
     * can be represented as vector masks (the default comparison result of GNU vector builtins),
     * bit-masks as used by AVX-512, bit-masks as used by ARM SVE (not yet implemented), or a single
     * bool (for the `Np` equals 1 case). For BasicMask it determines the actual data layout and
     * for BasicMask it determines the result of compares.
     *
     * @tparam Np    The number of elements.
     * @tparam Nreg  The number of registers needed to Store `Np` elements.
     * @tparam Var   Determines how std::complex value-types are laid out and whether Mask types use
     *                bit-masks or vector-masks.
     */
    template<int Np, int Nreg, std::underlying_type_t<AbiVariant> Var>
    struct Abi {
        static constexpr int kStorageSize = Np;

        /** @internal
         * The number of registers needed to represent one BasicVector for the element type that was
         * used on ABI deduction.
         *
         * For CxCtgus the value applies twice, once per reals and once per imags.
         *
         * Examples:
         * - 'Abi< 8, 2>' for 'int' is 2x 128-bit
         * - 'Abi< 9, 3>' for 'int' is 2x 128-bit and 1x 32-bit
         * - 'Abi<10, 3>' for 'int' is 2x 128-bit and 1x 64-bit
         * - 'Abi<10, 1>' for 'int' is 1x 512-bit
         * - 'Abi<10, 2>' for 'int' is 1x 256-bit and 1x 64-bit
         * - 'Abi< 8, 2, CxIleav>' for 'std::complex<float>' is 2x 256-bit
         * - 'Abi< 9, 2, CxIleav>' for 'std::complex<float>' is 1x 512-bit and 1x 64-bit
         * - 'Abi< 8, 1, CxCtgus>' for 'std::complex<float>' is 2x 256-bit
         */
        static constexpr int kNreg = Nreg;

        static_assert(kStorageSize > 0);
        static_assert(kNreg > 0);

        static constexpr AbiVariant kVariant = static_cast<AbiVariant>(Var);

        static constexpr bool kIsCxIleav = FilterAbiVariant(kVariant, AbiVariant::kCxIleav) == AbiVariant::kCxIleav;

        static constexpr bool kIsCxCtgus = FilterAbiVariant(kVariant, AbiVariant::kCxCtgus) == AbiVariant::kCxCtgus;

        static_assert(!(kIsCxIleav && kIsCxCtgus)); // can't be both

        static_assert(kStorageSize >= kNreg || (kIsCxIleav && kStorageSize * 2 >= kNreg));

        static constexpr bool kIsBitmask = FilterAbiVariant(kVariant, AbiVariant::kBitMask) == AbiVariant::kBitMask;

        static constexpr bool kIsVecmask = !kIsBitmask;

        template<typename Tp>
        using DataType = decltype([] {
            static_assert(kNreg == 1 || !std::same_as<Tp, Tp>);
            if constexpr (kStorageSize == 1) {
                return CanonicalVecTypeT<Tp>();
            } else {
                constexpr int n = std::bit_ceil(unsigned(kStorageSize));
                using Vp [[gnu::vector_size(sizeof(Tp) * n)]] = CanonicalVecTypeT<Tp>;
                return Vp();
            }
        }());

        template<std::size_t Bytes>
        using MaskDataType = decltype([] {
            static_assert(kNreg == 1 || Bytes != Bytes);
            if constexpr (kStorageSize == 1) {
                return bool();
            } else if constexpr (kIsVecmask) {
                constexpr unsigned vbytes = Bytes * std::bit_ceil(unsigned(kStorageSize));
                using Vp [[gnu::vector_size(vbytes)]] = Detail::IntegerForSize<Bytes>;
                return Vp();
            } else if constexpr (Nreg > 1) {
                return Detail::InvalidInteger();
            } else {
                return Bitmask<kStorageSize>();
            }
        }());

        template<int N2, int Nreg2 = DivCeil(N2, kStorageSize)>
        static consteval auto Resize() {
            if constexpr (N2 == 1) {
                return Abi<1, 1, Var>();
            } else {
                return Abi<N2, Nreg2, Var>();
            }
        }
    };

    /** @internal
     * Alias for an Abi specialization where the AbiVariant bits are combined into a single integer
     * value.
     *
     * Rationale: Consider diagnostic output and mangling of e.g. Vector<int, 4> with AVX512. That's an
     * alias for Sora::Math::Simd::BasicVector<int, Sora::Math::Simd::Abi<4, 1, 1ull>>. If AbiVariant were the
     * template argument type of Abi, the diagnostic output would be 'Sora::Math::Simd::BasicVector<int,
     * Sora::Math::Simd::Abi<4, 1, (Sora::Math::Simd::AbiVariant)Sora::Math::Simd::AbiVariant::BitMask>>'. That's a lot
     * longer, requires longer mangled names, and bakes the names of the enumerators into the ABI. As
     * soon as bits of multiple AbiVariants are combined, this becomes hard to parse for humans
     * anyway.
     */
    template<int Np, int Nreg, AbiVariant... Vs>
    using AbiT = Abi<Np, Nreg, (static_cast<std::underlying_type_t<AbiVariant>>(Vs) | ... | 0)>;

    /** @internal
     * This type is used whenever ABI tag deduction can't give a useful answer.
     */
    struct InvalidAbi {
        static constexpr int kStorageSize = 0;
    };

    /** @internal
     * Satisfied if @p Tp is a valid simd ABI tag. This is a necessary but not sufficient condition
     * for an enabled BasicVector/BasicMask specialization.
     */
    template<typename Tp>
    concept AbiTag = std::same_as<decltype(Tp::kVariant), const AbiVariant> && (Tp::kStorageSize >= Tp::kNreg) &&
                     (Tp::kNreg >= 1) && requires(Tp x) {
                         { x.template Resize<Tp::kStorageSize, Tp::kNreg>() } -> std::same_as<Tp>;
                     };

    /** @internal
     * Satisfied if `Tp` is a valid simd ABI tag and one element is stored per register (number of
     * registers equals size).
     */
    template<typename Tp>
    concept ScalarAbiTag = std::same_as<Tp, AbiT<Tp::kStorageSize, Tp::kStorageSize, Tp::kVariant>> && AbiTag<Tp>;

    // Determine if math functions must *raise* floating-point exceptions.
    // math_errhandling may expand to an extern symbol, in which case we must assume fp exceptions
    // need to be considered. A conforming C library must define math_errhandling, but in case it
    // isn't defined we simply use the fallback.
#ifdef math_errhandling
    template<int = 0>
        requires requires { typename std::bool_constant<0 != (math_errhandling & MATH_ERREXCEPT)>; }
    consteval bool HandleFpexceptImpl(int) {
        return 0 != (math_errhandling & MATH_ERREXCEPT);
    }
#endif

    // Fallback if math_errhandling doesn't work: implement correct exception behavior.
    consteval bool HandleFpexceptImpl(float) {
        return true;
    }

    /** @internal
     * This type can be used as a template parameter for avoiding ODR violations, where code needs to
     * differ depending on optimization Flags (mostly fp-math related).
     */
    struct OptTraits {
        consteval bool Test(int bit) const { return ((buildFlags >> bit) & 1) == 1; }

        // true iff floating-point operations can signal an exception (allow non-default handler)
        consteval bool FpMaySignal() const { return Test(0); }

        // true iff floating-point operations can raise an exception flag
        consteval bool FpMayRaise() const { return Test(12); }

        consteval bool FastMath() const { return Test(1); }

        consteval bool FiniteMathOnly() const { return Test(2); }

        consteval bool NoSignedZeros() const { return Test(3); }

        consteval bool SignedZeros() const { return !Test(3); }

        consteval bool ReciprocalMath() const { return Test(4); }

        consteval bool NoMathErrno() const { return Test(5); }

        consteval bool MathErrno() const { return !Test(5); }

        consteval bool AssociativeMath() const { return Test(6); }

        consteval bool ConformingToStdcAnnexG() const { return Test(10) && !FiniteMathOnly(); }

        consteval bool SupportSnan() const { return Test(11); }

        __UINT64_TYPE__ buildFlags = 0
#if !__NO_TRAPPING_MATH__
                                     + (1 << 0)
#endif
                                     + (HandleFpexceptImpl(0) << 12)
#if __FAST_MATH__
                                     + (1 << 1)
#endif
#if __FINITE_MATH_ONLY__
                                     + (1 << 2)
#endif
#if __NO_SIGNED_ZEROS__
                                     + (1 << 3)
#endif
#if __RECIPROCAL_MATH__
                                     + (1 << 4)
#endif
#if __NO_MATH_ERRNO__
                                     + (1 << 5)
#endif
#if __ASSOCIATIVE_MATH__
                                     + (1 << 6)
#endif
        // bits 7, 8, and 9 reserved for __FLT_EVAL_METHOD__
#if __FLT_EVAL_METHOD__ == 1
                                     + (1 << 7)
#elif __FLT_EVAL_METHOD__ == 2
                                     + (2 << 7)
#elif __FLT_EVAL_METHOD__ != 0
                                     + (3 << 7)
#endif

        // C Annex G defines the behavior of std::complex<T> where T is IEC60559 floating-point. If
        // __STDC_IEC_60559_COMPLEX__ is defined then Annex G is implemented - and simd<std::complex>
        // will do so as well. However, Clang never defines the macro.
#if defined __STDC_IEC_60559_COMPLEX__ || defined __STDC_IEC_559_COMPLEX__ || defined SORA_SIMD_CLANG
                                     + (1 << 10)
#endif
#if __SUPPORT_SNAN__
                                     + (1 << 11)
#endif
            ;
    };

    /** @internal
     * Return true iff @p s equals "1".
     */
    consteval bool StreqTo1(const char* s) {
        return s != nullptr && s[0] == '1' && s[1] == '\0';
    }

    /** @internal
     * If the macro given as @p feat is defined to 1, expands to a bit set at position @p off.
     * Otherwise, expand to zero.
     */
#define SORA_SIMD_ARCH_FLAG(off, feat)                                                                                 \
    (static_cast<__UINT64_TYPE__>(Sora::Math::Simd::StreqTo1(SORA_SIMD_TOSTRING_IMPL(feat))) << (off))

#if SORA_SIMD_X86

#    define SORA_SIMD_ARCH_TRAITS_INIT                                                                                 \
        {SORA_SIMD_ARCH_FLAG(0, __MMX__) | SORA_SIMD_ARCH_FLAG(1, __SSE__) | SORA_SIMD_ARCH_FLAG(2, __SSE2__) |        \
         SORA_SIMD_ARCH_FLAG(3, __SSE3__) | SORA_SIMD_ARCH_FLAG(4, __SSSE3__) | SORA_SIMD_ARCH_FLAG(5, __SSE4_1__) |   \
         SORA_SIMD_ARCH_FLAG(6, __SSE4_2__) | SORA_SIMD_ARCH_FLAG(7, __POPCNT__) | SORA_SIMD_ARCH_FLAG(8, __AVX__) |   \
         SORA_SIMD_ARCH_FLAG(9, __F16C__) | SORA_SIMD_ARCH_FLAG(10, __BMI__) | SORA_SIMD_ARCH_FLAG(11, __BMI2__) |     \
         SORA_SIMD_ARCH_FLAG(12, __LZCNT__) | SORA_SIMD_ARCH_FLAG(13, __AVX2__) | SORA_SIMD_ARCH_FLAG(14, __FMA__) |   \
         SORA_SIMD_ARCH_FLAG(15, __AVX512F__) | SORA_SIMD_ARCH_FLAG(16, __AVX512CD__) |                                \
         SORA_SIMD_ARCH_FLAG(17, __AVX512DQ__) | SORA_SIMD_ARCH_FLAG(18, __AVX512BW__) |                               \
         SORA_SIMD_ARCH_FLAG(19, __AVX512VL__) | SORA_SIMD_ARCH_FLAG(20, __AVX512BITALG__) |                           \
         SORA_SIMD_ARCH_FLAG(21, __AVX512VBMI__) | SORA_SIMD_ARCH_FLAG(22, __AVX512VBMI2__) |                          \
         SORA_SIMD_ARCH_FLAG(23, __AVX512IFMA__) | SORA_SIMD_ARCH_FLAG(24, __AVX512VNNI__) |                           \
         SORA_SIMD_ARCH_FLAG(25, __AVX512VPOPCNTDQ__) | SORA_SIMD_ARCH_FLAG(26, __AVX512FP16__) |                      \
         SORA_SIMD_ARCH_FLAG(27, __AVX512BF16__) | SORA_SIMD_ARCH_FLAG(28, __AVXIFMA__) |                              \
         SORA_SIMD_ARCH_FLAG(29, __AVXNECONVERT__) | SORA_SIMD_ARCH_FLAG(30, __AVXVNNI__) |                            \
         SORA_SIMD_ARCH_FLAG(31, __AVXVNNIINT8__) | SORA_SIMD_ARCH_FLAG(32, __AVXVNNIINT16__) |                        \
         SORA_SIMD_ARCH_FLAG(33, __AVX10_1__) | SORA_SIMD_ARCH_FLAG(34, __AVX10_2__) |                                 \
         SORA_SIMD_ARCH_FLAG(35, __AVX512VP2INTERSECT__) | SORA_SIMD_ARCH_FLAG(36, __SSE4A__) |                        \
         SORA_SIMD_ARCH_FLAG(37, __FMA4__) | SORA_SIMD_ARCH_FLAG(38, __XOP__)}
    // Should this include __APX_F__? I don't think it's relevant for use in constexpr-if branches =>
    // no ODR issue? The same could be said about several other Flags above that are not checked
    // anywhere.

    struct ArchTraits {
        __UINT64_TYPE__ flags = SORA_SIMD_ARCH_TRAITS_INIT;

        consteval bool Test(int bit) const { return ((flags >> bit) & 1) == 1; }

        consteval bool HaveMmx() const { return Test(0); }

        consteval bool HaveSse() const { return Test(1); }

        consteval bool HaveSse2() const { return Test(2); }

        consteval bool HaveSse3() const { return Test(3); }

        consteval bool HaveSsse3() const { return Test(4); }

        consteval bool HaveSse41() const { return Test(5); }

        consteval bool HaveSse42() const { return Test(6); }

        consteval bool HavePopcnt() const { return Test(7); }

        consteval bool HaveAvx() const { return Test(8); }

        consteval bool HaveF16c() const { return Test(9); }

        consteval bool HaveBmi() const { return Test(10); }

        consteval bool HaveBmi2() const { return Test(11); }

        consteval bool HaveLzcnt() const { return Test(12); }

        consteval bool HaveAvx2() const { return Test(13); }

        consteval bool HaveFma() const { return Test(14); }

        consteval bool HaveAvx512f() const { return Test(15); }

        consteval bool HaveAvx512cd() const { return Test(16); }

        consteval bool HaveAvx512dq() const { return Test(17); }

        consteval bool HaveAvx512bw() const { return Test(18); }

        consteval bool HaveAvx512vl() const { return Test(19); }

        consteval bool HaveAvx512bitalg() const { return Test(20); }

        consteval bool HaveAvx512vbmi() const { return Test(21); }

        consteval bool HaveAvx512vbmi2() const { return Test(22); }

        consteval bool HaveAvx512ifma() const { return Test(23); }

        consteval bool HaveAvx512vnni() const { return Test(24); }

        consteval bool HaveAvx512vpopcntdq() const { return Test(25); }

        consteval bool HaveAvx512fp16() const { return Test(26); }

        consteval bool HaveAvx512bf16() const { return Test(27); }

        consteval bool HaveAvxifma() const { return Test(28); }

        consteval bool HaveAvxneconvert() const { return Test(29); }

        consteval bool HaveAvxvnni() const { return Test(30); }

        consteval bool HaveAvxvnniint8() const { return Test(31); }

        consteval bool HaveAvxvnniint16() const { return Test(32); }

        consteval bool HaveAvx101() const { return Test(33); }

        consteval bool HaveAvx102() const { return Test(34); }

        consteval bool HaveAvx512vp2intersect() const { return Test(35); }

        consteval bool HaveSse4a() const { return Test(36); }

        consteval bool HaveFma4() const { return Test(37); }

        consteval bool HaveXop() const { return Test(38); }

        template<typename Tp>
        consteval bool EvalAsF32() const {
            return std::is_same_v<Tp, _Float16> && !HaveAvx512fp16();
        }

        consteval bool HaveAddsub() const { return HaveSse3(); }
    };

    template<typename Tp, ArchTraits Traits = {}>
    consteval auto NativeAbi() {
        constexpr int adjSizeof = sizeof(Tp) * (1 + std::is_same_v<Tp, _Float16>);
        if constexpr (!Vectorizable<Tp>) {
            return InvalidAbi();
        } else if constexpr (ComplexLike<Tp>) {
            constexpr auto underlying = Sora::Math::Simd::NativeAbi<typename Tp::value_type>();
            constexpr int cxSize = underlying.kStorageSize / (underlying.kStorageSize == 1 ? 1 : 2);
            return AbiT<cxSize, 1, underlying.kVariant, AbiVariant::kCxIleav>();
        } else if constexpr (Traits.HaveAvx512fp16()) {
            return AbiT<64 / sizeof(Tp), 1, AbiVariant::kBitMask>();
        } else if constexpr (Traits.HaveAvx512f()) {
            return AbiT<64 / adjSizeof, 1, AbiVariant::kBitMask>();
        } else if constexpr (std::is_same_v<Tp, _Float16> && !Traits.HaveF16c()) {
            return AbiT<1, 1>();
        } else if constexpr (Traits.HaveAvx2()) {
            return AbiT<32 / adjSizeof, 1>();
        } else if constexpr (Traits.HaveAvx() && std::is_floating_point_v<Tp>) {
            return AbiT<32 / adjSizeof, 1>();
        } else if constexpr (Traits.HaveSse2()) {
            return AbiT<16 / adjSizeof, 1>();
        } else if constexpr (Traits.HaveSse() && std::is_floating_point_v<Tp> && sizeof(Tp) == sizeof(float)) {
            return AbiT<16 / adjSizeof, 1>();
        } else {
            // no MMX: we can't emit EMMS where it would be necessary
            return AbiT<1, 1>();
        }
    }

#else

    // scalar fallback
    struct ArchTraits {
        __UINT64_TYPE__ flags = 0;

        constexpr bool Test(int bit) const { return ((flags >> bit) & 1) == 1; }
    };

    template<typename Tp>
    consteval auto NativeAbi() {
        if constexpr (!Vectorizable<Tp>) {
            return InvalidAbi();
        } else if constexpr (ComplexLike<Tp>) {
            return AbiT<1, 1, AbiVariant::CxIleav>();
        } else {
            return AbiT<1, 1>();
        }
    }

#endif

    /** @internal
     * You must use this type as template argument to function templates that are not declared
     * always_inline (to avoid issues when linking code compiled with different compiler Flags).
     */
    struct TargetTraits : ArchTraits, OptTraits {};

    /** @internal
     * Alias for an ABI tag such that BasicVector<Tp, NativeAbiT<Tp>> stores one SIMD register of
     * optimal width.
     *
     * @tparam Tp  A vectorizable type.
     *
     * C++26 [simd.expos.abi]
     */
    template<typename Tp>
    using NativeAbiT = decltype(Sora::Math::Simd::NativeAbi<Tp>());

    template<typename Tp, int Np, TargetTraits Target = {}>
    consteval auto DeduceAbi() {
        constexpr auto native = Sora::Math::Simd::NativeAbi<Tp>();
        if constexpr (0 == native.kStorageSize || Np <= 0) {
            return InvalidAbi();
        } else if constexpr (Np == native.kStorageSize) {
            return native;
        } else {
            return native.template Resize<Np>();
        }
    }

    /** @internal
     * Alias for an ABI tag @c A such that `BasicVector<Tp, A>` stores @p Np elements.
     *
     * C++26 [simd.expos.abi]
     */
    template<typename Tp, int Np>
    using DeduceAbiT = decltype(Sora::Math::Simd::DeduceAbi<Tp, Np>());

    /** @internal
     * \c RebindTraits implementation detail for BasicVector, and BasicMask where we know the destination
     * value-type
     */
    template<typename Tp, int Np, AbiTag A0, ArchTraits = {}>
    consteval auto AbiRebind() {
        if constexpr (Np <= 0 || !Vectorizable<Tp>) {
            return InvalidAbi();
        }

        else {
            using Native = std::remove_const_t<decltype(Sora::Math::Simd::NativeAbi<Tp>())>;
            static_assert(0 != Native::kStorageSize);
            constexpr int kNreg = DivCeil(Np, Native::kStorageSize);

            // ScalarAbiTag is sticky (unless we reach size 1, where we can't know whether it was
            // an explicit ScalarAbiTag before some Resize)
            if constexpr (ScalarAbiTag<Native> || (ScalarAbiTag<A0> && A0::kStorageSize >= 2)) {
                constexpr bool removeCx =
                    FilterAbiVariant(A0::kVariant, AbiVariant::kCxVariants) != AbiVariant() && !ComplexLike<Tp>;
                constexpr bool addCx =
                    FilterAbiVariant(A0::kVariant, AbiVariant::kCxVariants) == AbiVariant() && ComplexLike<Tp>;

                if constexpr (removeCx) {
                    return AbiT<Np, Np, FilterAbiVariant(A0::kVariant, AbiVariant::kMaskVariants)>();
                } else if constexpr (addCx) {
                    return AbiT<Np, Np, A0::kVariant, FilterAbiVariant(Native::kVariant, AbiVariant::kCxVariants)>();
                } else {
                    return A0::template Resize<Np, Np>();
                }
            }

            else if constexpr (ComplexLike<Tp> && A0::kIsCxCtgus && Native::kIsCxIleav) {
                // we need half the number of registers since the number applies twice, to reals and
                // imaginaries.
                return A0::template Resize<Np, DivCeil(kNreg, 2)>();
            }

            else if constexpr (ComplexLike<Tp> && A0::kIsCxIleav && Native::kIsCxCtgus) {
                return A0::template Resize<Np, kNreg * 2>();
            }

            else if constexpr (ComplexLike<Tp> && (A0::kIsCxCtgus || A0::kIsCxIleav)) {
                return A0::template Resize<Np, kNreg>();
            }

            else if constexpr (ComplexLike<Tp>) {
                // Bit vs. Vec Mask determined by A0, CxVariant determined by Native
                return AbiT<Native::kStorageSize, 1, A0::kVariant,
                            FilterAbiVariant(Native::kVariant, AbiVariant::kCxVariants)>::template Resize<Np, kNreg>();
            }

            else {
                return AbiT<Native::kStorageSize, 1,
                            FilterAbiVariant(A0::kVariant, AbiVariant::kMaskVariants)>::template Resize<Np, kNreg>();
            }
        }
    }

    /** @internal
     * @c RebindTraits implementation detail for BasicMask.
     *
     * The important difference here is that we have no information about the actual value-type other
     * than its @c sizeof. So `Bytes == 8` could mean `std::complex<float>`, @c double, or @c std::int64_t.
     * E.g. `Np == 4` with AVX w/o AVX2 that's `vector(4) int`, `vector(4) long long`, or `2x
     * vector(2) long long`.
     * That's why this overload has the additional @p IsOnlyResize parameter, which tells us that the
     * value-type doesn't change.
     */
    template<std::size_t Bytes, int Np, AbiTag A0, bool IsOnlyResize, ArchTraits Traits = {}>
    consteval auto AbiRebind() {
        constexpr bool fromCx = A0::kIsCxCtgus || A0::kIsCxIleav;

        if constexpr (Bytes == 0 || Np <= 0) {
            return InvalidAbi();
        }

        // If the source ABI is std::complex, Bytes == sizeof(std::complex<float>) or
        // sizeof(std::complex<float16_t>), and IsOnlyResize is true, then it's a Mask<std::complex<float>,
        // Np>
        else if constexpr (fromCx && IsOnlyResize && Bytes == 2 * sizeof(double)) {
            return AbiRebind<std::complex<double>, Np, A0>();
        } else if constexpr (fromCx && IsOnlyResize && Bytes == 2 * sizeof(float)) {
            return AbiRebind<std::complex<float>, Np, A0>();
        } else if constexpr (fromCx && IsOnlyResize && Bytes == 2 * sizeof(_Float16)) {
            return AbiRebind<std::complex<_Float16>, Np, A0>();
        }

#if SORA_SIMD_X86
        // AVX w/o AVX2:
        // e.g. Resize<8, Mask<float, Whatever>> needs to be Abi<8, 1> not Abi<8, 2>
        // We determine whether A0 identifies an AVX vector by looking at the size of a native
        // register. If it's 32, it's a YMM register, otherwise it's 16 or std::less.
        else if constexpr (IsOnlyResize && Traits.HaveAvx() && !Traits.HaveAvx2() &&
                           std::bit_ceil(DivCeil<unsigned>(A0::kStorageSize, A0::kNreg)) * Bytes == 32) {
            if constexpr (Bytes == sizeof(double)) {
                return AbiRebind<double, Np, A0>();
            } else if constexpr (Bytes == sizeof(float)) {
                return AbiRebind<float, Np, A0>();
            } else if constexpr (Traits.HaveF16c() && Bytes == sizeof(_Float16)) {
                return AbiRebind<_Float16, Np, A0>();
            } else { // impossible
                static_assert(false);
            }
        }
#endif

        else {
            return AbiRebind<Detail::IntegerForSize<Bytes>, Np, A0>();
        }
    }

    /** @internal
     * Returns true unless SORA_SIMD_COND_EXPLICIT_MASK_CONVERSION is defined.
     *
     * On IvyBridge, (Vector<float> == 0.f) == (Rebind<int, Vector<float>> == 0) does not compile. It does
     * compile on basically every other target, though. This is due to the difference in ABI tag:
     * Abi<8, 1, [...]> vs. Abi<8, 2, [...]> (8 elements, 1 vs. 2 registers).
     * I know how to define this function for libstdc++ to avoid interconvertible masks. The question
     * is whether we can specify this in general for C++29.
     *
     * Idea: Is Rebind<integer-from<...>, Mask>::AbiType the same type as
     *   deduce-t<integer-from<...>, Mask::size()>? If yes, it's the "better" ABI tag. However, this
     *   makes the conversion behavior dependent on compiler Flags. Probably not what we want.
     */
    template<typename To, typename From>
    consteval bool IsMaskConversionExplicit([[maybe_unused]] std::size_t b0, [[maybe_unused]] std::size_t b1) {
        constexpr int n = To::kStorageSize;
        static_assert(n == From::kStorageSize);
#ifndef SORA_SIMD_COND_EXPLICIT_MASK_CONVERSION
        // C++26 [simd.mask.ctor] uses unconditional explicit
        return true;
#else
        if (b0 != b1) {
            return true;
        }

        // converting to a bit-Mask is better
        else if constexpr (To::kIsVecmask != From::kIsVecmask) {
            return To::kIsVecmask; // to vector-Mask is explicit
        }

        // with Vector-masks, fewer registers is better
        else if constexpr (From::kNreg != To::kNreg) {
            return From::kNreg < To::kNreg;
        }

        // differ only on Cx Flags
        // interleaved std::complex is worse
        else if constexpr (To::kIsCxIleav) {
            return true;
        } else if constexpr (From::kIsCxIleav) {
            return false;
        }

        // prefer non-Cx over CxCtgus
        else if constexpr (To::kIsCxCtgus) {
            return true;
        } else if constexpr (From::kIsCxCtgus) {
            return false;
        } else {
            __builtin_unreachable();
        }
#endif
    }

    /** @internal
     * An alias for a signed integer type.
     *
     * libstdc++ unconditionally uses @c int here, since it matches the return type of
     * 'Bit Operation Builtins' in GCC.
     *
     * C++26 [simd.expos.defn]
     */
    using SimdSizeType = int;

    // std::integral_constant shortcut
    template<SimdSizeType Xp>
    inline constexpr std::integral_constant<SimdSizeType, Xp> kSimdSizeC = {};

    // [simd.syn]
    template<typename Tp, typename Ap = NativeAbiT<Tp>>
    class BasicVector;

    template<typename Tp, SimdSizeType Np = NativeAbiT<Tp>::kStorageSize>
    using Vector = BasicVector<Tp, DeduceAbiT<Tp, Np>>;

    template<std::size_t Bytes, typename Ap = NativeAbiT<Detail::IntegerForSize<Bytes>>>
    class BasicMask;

    template<typename Tp, SimdSizeType Np = NativeAbiT<Tp>::kStorageSize>
    using Mask = BasicMask<sizeof(Tp), DeduceAbiT<Tp, Np>>;

    // [simd.ctor] load constructor constraints
    template<typename Rg>
    consteval std::size_t StaticRangeSize(Rg& r) {
        if constexpr (Detail::StaticSizedRange<Rg>) {
            return std::ranges::size(r);
        } else {
            return std::dynamic_extent;
        }
    }

    // [simd.general] value-preserving
    template<typename From, typename To>
    concept ArithmeticOnlyValuePreservingConvertibleTo =
        std::convertible_to<From, To> && std::is_arithmetic_v<From> && std::is_arithmetic_v<To> &&
        !(std::is_signed_v<From> && std::is_unsigned_v<To>) &&
        std::numeric_limits<From>::digits <= std::numeric_limits<To>::digits &&
        std::numeric_limits<From>::max() <= std::numeric_limits<To>::max() &&
        std::numeric_limits<From>::lowest() >= std::numeric_limits<To>::lowest();

    /** @internal
     * Satisfied if the conversion from @p From to @p To is a value-preserving conversion.
     *
     * C++26 [simd.general]
     */
    template<typename From, typename To>
    concept ValuePreservingConvertibleTo =
        ArithmeticOnlyValuePreservingConvertibleTo<From, To> ||
        (ComplexLike<To> && ArithmeticOnlyValuePreservingConvertibleTo<From, typename To::value_type>);

    // LWG4420
    template<typename From, typename To>
    concept ExplicitlyConvertibleTo = requires { static_cast<To>(std::declval<From>()); };

    /** @internal
     * C++26 [simd.expos]
     */
    // [simd.ctor] explicit(...) of broadcast ctor
    template<auto From, typename To>
    concept NonNarrowingConstexprConversion =
        std::is_arithmetic_v<decltype(From)> && static_cast<decltype(From)>(static_cast<To>(From)) == From &&
        !(std::unsigned_integral<To> && From < decltype(From)()) && From <= std::numeric_limits<To>::max() &&
        From >= std::numeric_limits<To>::lowest();

    // [simd.ctor] p4
    // This implements LWG4436 (submitted on 2025-10-28)
    template<typename From, typename To>
    concept BroadcastConstructible =
        ((std::convertible_to<From, To> && !std::is_arithmetic_v<std::remove_cvref_t<From>> &&
          !Detail::ConstexprWrapperLike<std::remove_cvref_t<From>>)     // 4.1
         || ValuePreservingConvertibleTo<std::remove_cvref_t<From>, To> // 4.2
         || (Detail::ConstexprWrapperLike<std::remove_cvref_t<From>>    // 4.3
             && NonNarrowingConstexprConversion<auto(std::remove_cvref_t<From>::value), To>));

    // HigherFloatingPointRankThan<Tp, U> (Tp has higher or equal floating point rank than U)
    template<typename From, typename To>
    consteval bool HigherFloatingPointRankThan() {
        return std::floating_point<From> && std::floating_point<To> &&
               std::is_same_v<std::common_type_t<From, To>, From> && !std::is_same_v<From, To>;
    }

    // HigherIntegerRankThan<Tp, U> (Tp has higher or equal integer rank than U)
    template<typename From, typename To>
    consteval bool HigherIntegerRankThan() {
        return std::integral<From> && std::integral<To> &&
               (sizeof(From) > sizeof(To) || std::is_same_v<std::common_type_t<From, To>, From>) &&
               !std::is_same_v<From, To>;
    }

    template<typename From, typename To>
    concept HigherRankThan = HigherFloatingPointRankThan<From, To>() || HigherIntegerRankThan<From, To>();

    struct ConvertFlag;

    template<typename From, typename To, typename... FlagTypes>
    concept LoadstoreConvertibleTo =
        std::same_as<From, To> ||
        (Vectorizable<From> && Vectorizable<To> &&
         (ValuePreservingConvertibleTo<From, To> ||
          (ExplicitlyConvertibleTo<From, To> && (std::is_same_v<FlagTypes, ConvertFlag> || ...))));

    template<typename From, typename To>
    concept SimdGeneratorConvertibleTo =
        std::convertible_to<From, To> && (!std::is_arithmetic_v<From> || ValuePreservingConvertibleTo<From, To>);

    template<typename Fp, typename Tp, SimdSizeType... Is>
        requires(SimdGeneratorConvertibleTo<decltype(std::declval<Fp>()(kSimdSizeC<Is>)), Tp> && ...)
    constexpr void SimdGeneratorInvokableImpl(std::integer_sequence<SimdSizeType, Is...>);

    template<typename Fp, typename Tp, SimdSizeType Np>
    concept SimdGeneratorInvokable =
        requires { SimdGeneratorInvokableImpl<Fp, Tp>(std::make_integer_sequence<SimdSizeType, Np>()); };

    template<typename Fp>
    concept IndexPermutationFunctionSized = requires(Fp const& f) {
        { f(0, 0) } -> std::integral;
    };

    template<typename Fp, typename Simd>
    concept IndexPermutationFunction = IndexPermutationFunctionSized<Fp> || requires(Fp const& f) {
        { f(0) } -> std::integral;
    };

    /** @internal
     * The value of the @c Bytes template argument to a @c BasicMask specialization.
     *
     * C++26 [simd.expos.defn]
     */
    template<typename Tp>
    constexpr std::size_t kMaskElementSize = 0;

    template<std::size_t Bytes, AbiTag Ap>
    constexpr std::size_t kMaskElementSize<BasicMask<Bytes, Ap>> = Bytes;

    // [simd.expos]
    template<typename Vp>
    concept SimdVecType = std::same_as<Vp, BasicVector<typename Vp::ValueType, typename Vp::AbiType>> &&
                          std::is_default_constructible_v<Vp>;

    template<typename Vp>
    concept SimdMaskType =
        std::same_as<Vp, BasicMask<kMaskElementSize<Vp>, typename Vp::AbiType>> && std::is_default_constructible_v<Vp>;

    /** @internal
     * Satisfied if @p Tp is a data-parallel type.
     */
    template<typename Vp>
    concept SimdVecOrMaskType = SimdVecType<Vp> || SimdMaskType<Vp>;

    template<typename Vp>
    concept SimdFloatingPoint = SimdVecType<Vp> && std::floating_point<typename Vp::ValueType>;

    template<typename Vp>
    concept SimdIntegral = SimdVecType<Vp> && std::integral<typename Vp::ValueType>;

    template<typename Vp>
    concept SimdUnsignedInteger = SimdVecType<Vp> && Detail::UnsignedInteger<typename Vp::ValueType>;

    template<typename Vp>
    using SimdComplexValueType = typename Vp::ValueType::value_type;

    template<typename Vp>
    concept SimdComplex = SimdVecType<Vp> && ComplexLikeImpl<typename Vp::ValueType>;

    template<typename Tp>
    concept ConvertsToVec = SimdVecType<decltype(std::declval<const Tp&>() + std::declval<const Tp&>())>;

    template<ConvertsToVec Tp>
    using DeducedVecT = decltype(std::declval<const Tp&>() + std::declval<const Tp&>());

    template<typename Vp, typename Tp>
    using MakeCompatibleSimdT = decltype([] {
        using Up = decltype(std::declval<const Tp&>() + std::declval<const Tp&>());
        if constexpr (SimdVecType<Up>) {
            return Up();
        } else {
            return Vector<Up, Vp::kSize()>();
        }
    }());

    template<typename Tp>
    concept MathFloatingPoint = SimdFloatingPoint<DeducedVecT<Tp>>;

    template<typename BinaryOperation, typename Tp>
    concept ReductionBinaryOperation = requires(const BinaryOperation binaryOp, const Vector<Tp, 1> v) {
        { binaryOp(v, v) } -> std::same_as<Vector<Tp, 1>>;
    };

    /** @internal
     * Returns the highest index @c i where `(bits >> i) & 1` equals @c 1.
     */
    [[gnu::always_inline]]
    constexpr SimdSizeType HighestBit(std::unsigned_integral auto bits) {
        constexpr auto nd = std::numeric_limits<decltype(bits)>::digits;
        return nd - 1 - std::countl_zero(bits);
    }

    template<Vectorizable Tp, SimdSizeType Np, AbiTag Ap>
    using SimilarMask = BasicMask<sizeof(Tp), decltype(AbiRebind<Tp, Np, Ap>())>;

    template<std::size_t Bytes, AbiTag Ap>
    using ComponentMaskForIleav =
        BasicMask<Bytes / 2, decltype(AbiRebind<Detail::FloatForSize<Bytes / 2>, Ap::kStorageSize * 2, Ap>())>;

    template<std::size_t Bytes, AbiTag Ap>
    using ComponentMaskForCtgus =
        BasicMask<Bytes / 2, decltype(AbiRebind<Detail::FloatForSize<Bytes / 2>, Ap::kStorageSize, Ap>())>;

    // Allow Tp to be Detail::InvalidInteger for Detail::IntegerForSize<16>
    template<typename Tp, SimdSizeType Np, AbiTag Ap>
    using SimilarVec = BasicVector<Tp, decltype(AbiRebind<Tp, Np, Ap>())>;

    // LWG4470 [simd.expos]
    template<std::size_t Bytes, typename Ap>
    using SimdVecFromMaskT = SimilarVec<Detail::IntegerForSize<Bytes>, Ap::kStorageSize, Ap>;

#if SORA_SIMD_THROW_ON_BAD_VALUE // used for unit tests (also see P3844)
    class BadValuePreservingCast {};

#    define SORA_SIMD_ON_BAD_VALUE_PRESERVING_CAST throw BadValuePreservingCast
#else
    void HandleBadValuePreservingCast(); // not defined

#    define SORA_SIMD_ON_BAD_VALUE_PRESERVING_CAST HandleBadValuePreservingCast
#endif

    template<typename To, typename From>
#if SORA_SIMD_THROW_ON_BAD_VALUE            // see P3844
    [[__gnu__::__optimize__("exceptions")]] // work around potential -fno-exceptions
#endif
    consteval To ValuePreservingCast(const From& x) {
        static_assert(std::is_arithmetic_v<From>);
        if constexpr (!ValuePreservingConvertibleTo<From, To>) {
            using Up = Detail::MakeUnsignedT<From>;
            if (static_cast<Up>(static_cast<To>(x)) != static_cast<Up>(x)) {
                SORA_SIMD_ON_BAD_VALUE_PRESERVING_CAST();
            } else if constexpr (std::is_signed_v<From> && std::is_unsigned_v<To>) {
                if (x < From()) {
                    SORA_SIMD_ON_BAD_VALUE_PRESERVING_CAST();
                }
            } else if constexpr (std::unsigned_integral<From> && std::signed_integral<To>) {
                if (x > std::numeric_limits<To>::max()) {
                    SORA_SIMD_ON_BAD_VALUE_PRESERVING_CAST();
                }
            }
        }
        return static_cast<To>(x);
    }

    /** @internal
     * std::pair is not trivially copyable, this one is
     */
    template<typename T0, typename T1>
    struct TrivialPair {
        T0 first;
        T1 second;
    };

    template<typename From, typename To>
    concept ConvertsTrivially = std::convertible_to<From, To> && sizeof(From) == sizeof(To) &&
                                std::is_integral_v<From> == std::is_integral_v<To> &&
                                std::is_floating_point_v<From> == std::is_floating_point_v<To>;

    [[gnu::always_inline]]
    constexpr void BitForeach(std::unsigned_integral auto bits, auto&& fun) {
        static_assert(sizeof(bits) >= sizeof(int)); // avoid promotion to int
        while (bits) {
            fun(std::countr_zero(bits));
            bits &= (bits - 1);
        }
    }

    /** @internal
     * Optimized @c memcpy for use in partial loads and stores.
     *
     * The implementation uses at most two fixed-size power-of-2 @c memcpy calls and reduces the
     * number of branches to a minimum. The variable size is achieved by overlapping two @c memcpy
     * calls.
     *
     * @tparam Chunk   Copies @p n times @p Chunk bytes.
     * @tparam Max     Copy no more than @p Max bytes.
     *
     * @param  dst    The destination pointer.
     * @param  src    The source pointer.
     * @param  n      Thu number of chunks that need to be copied.
     */
    template<std::size_t Chunk, std::size_t Max>
    inline void MemcpyChunks(std::byte* __restrict__ dst, const std::byte* __restrict__ src, std::size_t n) {
        static_assert(Max <= 64);
        static_assert(std::has_single_bit(Chunk) && Chunk <= 8);
        std::size_t bytes = Chunk * n;
        if (__builtin_constant_p(
                bytes)) { // If n is known via constant propagation use a single memcpy call. Since this is still
            // a fixed-size memcpy to the compiler, this leaves more room for optimization.
            __builtin_memcpy(dst, src, bytes);
        } else if (bytes > 32 && Max > 32) {
            __builtin_memcpy(dst, src, 32);
            bytes -= 32;
            __builtin_memcpy(dst + bytes, src + bytes, 32);
        } else if (bytes > 16 && Max > 16) {
            __builtin_memcpy(dst, src, 16);
            if constexpr (Chunk == 8) {
                bytes -= 8;
                __builtin_memcpy(dst + bytes, src + bytes, 8);
            } else {
                bytes -= 16;
                __builtin_memcpy(dst + bytes, src + bytes, 16);
            }
        } else if (bytes > 8 && Max > 8) {
            __builtin_memcpy(dst, src, 8);
            if constexpr (Chunk == 4) {
                bytes -= 4;
                __builtin_memcpy(dst + bytes, src + bytes, 4);
            } else if constexpr (Chunk < 4) {
                bytes -= 8;
                __builtin_memcpy(dst + bytes, src + bytes, 8);
            }
        } else if (bytes > 4 && Max > 4) {
            __builtin_memcpy(dst, src, 4);
            if constexpr (Chunk == 2) {
                bytes -= 2;
                __builtin_memcpy(dst + bytes, src + bytes, 2);
            } else if constexpr (Chunk == 1) {
                bytes -= 4;
                __builtin_memcpy(dst + bytes, src + bytes, 4);
            }
        } else if (bytes >= 2) {
            __builtin_memcpy(dst, src, 2);
            if constexpr (Chunk == 2) {
                bytes -= 2;
                __builtin_memcpy(dst + bytes, src + bytes, 2);
            } else if constexpr (Chunk == 1) {
                bytes -= 1;
                __builtin_memcpy(dst + bytes, src + bytes, 1);
            }
        } else if (bytes == 1) {
            __builtin_memcpy(dst, src, 1);
        }
    }

    // [simd.reductions] identity_element = *see below*
    template<typename Tp, typename BinaryOperation>
        requires Detail::IsOneOf<BinaryOperation, std::plus<>, std::multiplies<>, std::bit_and<>, std::bit_or<>,
                                 std::bit_xor<>>::value
    consteval Tp DefaultIdentityElement() {
        if constexpr (std::same_as<BinaryOperation, std::multiplies<>>) {
            return Tp(1);
        } else if constexpr (std::same_as<BinaryOperation, std::bit_and<>>) {
            return Tp(~Tp());
        } else {
            return Tp(0);
        }
    }

} // namespace Sora::Math::Simd
