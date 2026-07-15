/**
 * @file VectorOperations.h
 * @brief Low-level compiler-vector operations used by the SIMD implementation.
 * @ingroup Math
 */
#pragma once

#include "Details.h"

#include <bit>
#include <utility>

namespace Sora::Math::Simd {

    template<std::signed_integral Tp>
    constexpr bool SignedHasSingleBit(Tp x) {
        return std::has_single_bit(std::make_unsigned_t<Tp>(x));
    }

    /**
     * Alias for a vector builtin with given value type and total sizeof.
     */
    template<Vectorizable Tp, std::size_t Bytes>
        requires(std::has_single_bit(Bytes))
    using VecBuiltinTypeBytes [[gnu::vector_size(Bytes)]] = Tp;

    /**
     * Alias for a vector builtin with given value type @p Tp and @p Width.
     */
    template<Vectorizable Tp, SimdSizeType Width>
        requires(SignedHasSingleBit(Width))
    using VecBuiltinType = VecBuiltinTypeBytes<Tp, sizeof(Tp) * Width>;

    /**
     * Constrain to any vector builtin with given value type and optional width.
     */
    template<typename Tp, typename ValueType, SimdSizeType Width = sizeof(Tp) / sizeof(ValueType)>
    concept VecBuiltinOf =
        !std::is_class_v<Tp> && !std::is_pointer_v<Tp> && !std::is_arithmetic_v<Tp> && Vectorizable<ValueType> &&
        Width >= 1 && sizeof(Tp) / sizeof(ValueType) == Width &&
        std::same_as<VecBuiltinTypeBytes<ValueType, sizeof(Tp)>, Tp> && requires(Tp& v, ValueType x) { v[0] = x; };

    /**
     * Constrain to any vector builtin.
     */
    template<typename Tp>
    concept VecBuiltin = VecBuiltinOf<Tp, std::remove_cvref_t<decltype(std::declval<const Tp>()[0])>>;

    /**
     * Alias for the value type of the given VecBuiltin type @p Tp.
     */
    template<VecBuiltin Tp>
    using VecValueType = std::remove_cvref_t<decltype(std::declval<const Tp>()[0])>;

    /**
     * The width (number of ValueType elements) of the given vector builtin or arithmetic type.
     */
    template<typename Tp>
    inline constexpr SimdSizeType kWidthOf = 1;

    template<typename Tp>
        requires VecBuiltin<Tp>
    inline constexpr SimdSizeType kWidthOf<Tp> = sizeof(Tp) / sizeof(VecValueType<Tp>);

    /**
     * Alias for a vector builtin with equal value type and new width @p Np.
     */
    template<SimdSizeType Np, VecBuiltin TV>
    using ResizeVecBuiltinT = VecBuiltinType<VecValueType<TV>, Np>;

    template<VecBuiltin TV>
        requires(kWidthOf<TV> > 1)
    using HalfVecBuiltinT = ResizeVecBuiltinT<kWidthOf<TV> / 2, TV>;

    template<VecBuiltin TV>
    using DoubleVecBuiltinT = ResizeVecBuiltinT<kWidthOf<TV> * 2, TV>;

    template<typename Up, VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr VecBuiltinTypeBytes<Up, sizeof(TV)> VecBitCast(TV v) {
        return reinterpret_cast<VecBuiltinTypeBytes<Up, sizeof(TV)>>(v);
    }

    template<int Np, VecBuiltin TV>
        requires std::signed_integral<VecValueType<TV>>
    static constexpr TV kVecImplicitMask = []<int... Is>(std::integer_sequence<int, Is...>) {
        return TV{(Is < Np ? -1 : 0)...};
    }(std::make_integer_sequence<int, kWidthOf<TV>>());

    /**
     * Helper function to work around Clang not allowing v[i] in constant expressions.
     */
    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr VecValueType<TV> VecGet(TV v, int i) {
#ifdef SORA_SIMD_CLANG
        if consteval {
            return __builtin_bit_cast(std::array<VecValueType<TV>, kWidthOf<TV>>, v)[i];
        } else
#endif
        {
            return v[i];
        }
    }

    /**
     * Helper function to work around Clang and GCC not allowing assignment to v[i] in constant
     * expressions.
     */
    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr void VecSet(TV& v, int i, VecValueType<TV> x) {
        if consteval {
#ifdef SORA_SIMD_CLANG
            auto arr = __builtin_bit_cast(std::array<VecValueType<TV>, kWidthOf<TV>>, v);
            arr[i] = x;
            v = __builtin_bit_cast(TV, arr);
#else
            const auto& [... j] = Detail::kIotaArray<kWidthOf<TV>>;
            v = TV{(i == j ? x : v[j])...};
#endif
        } else {
            v[i] = x;
        }
    }

    /** @internal
     * Return vector builtin with all values from @p a and @p b.
     */
    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr VecBuiltinType<VecValueType<TV>, kWidthOf<TV> * 2> VecConcat(TV a, TV b) {
        const auto& [... indices] = Detail::kIotaArray<kWidthOf<TV> * 2>;
        return __builtin_shufflevector(a, b, indices...);
    }

    /** @internal
     * Concatenate the first @p N0 elements from @p a with the first @p N1 elements from @p b
     * with the elements from applying this function recursively to @p rest.
     *
     * @pre N0 <= kWidthOf<TV0> && N1 <= kWidthOf<TV1> && Ns <= kWidthOf<TVs> && ...
     *
     * Strategy: Aim for a power-of-2 tree Concat. E.g.
     * - Cat(2, 2, 2, 2) -> Cat(4, 2, 2) -> Cat(4, 4)
     * - Cat(2, 2, 2, 2, 8) -> Cat(4, 2, 2, 8) -> Cat(4, 4, 8) -> Cat(8, 8)
     */
    template<int N0, int N1, int... Ns, VecBuiltin TV0, VecBuiltin TV1, VecBuiltin... TVs>
    [[gnu::always_inline]]
    constexpr VecBuiltinType<VecValueType<TV0>, std::bit_ceil(unsigned(N0 + (N1 + ... + Ns)))>
    VecConcatSized(const TV0& a, const TV1& b, const TVs&... rest);

    template<int N0, int N1, int N2, int... Ns, VecBuiltin TV0, VecBuiltin TV1, VecBuiltin TV2, VecBuiltin... TVs>
        requires(std::has_single_bit(unsigned(N0))) && (N0 >= (N1 + N2))
    [[gnu::always_inline]]
    constexpr VecBuiltinType<VecValueType<TV0>, std::bit_ceil(unsigned(N0 + N1 + (N2 + ... + Ns)))>
    VecConcatSized(const TV0& a, const TV1& b, const TV2& c, const TVs&... rest) {
        return VecConcatSized<N0, N1 + N2, Ns...>(a, VecConcatSized<N1, N2>(b, c), rest...);
    }

    template<int N0, int N1, int... Ns, VecBuiltin TV0, VecBuiltin TV1, VecBuiltin... TVs>
    [[gnu::always_inline]]
    constexpr VecBuiltinType<VecValueType<TV0>, std::bit_ceil(unsigned(N0 + (N1 + ... + Ns)))>
    VecConcatSized(const TV0& a, const TV1& b, const TVs&... rest) {
        // indices is rounded up because we need to generate a power-of-2 vector:
        const auto& [... indices] = Detail::kIotaArray<std::bit_ceil(unsigned(N0 + N1)), int>;
        const auto ab = __builtin_shufflevector(a, b, [](int i) consteval {
            if (i < N0) { // copy from a
                return i;
            } else if (i < N0 + N1) {          // copy from b
                return i - N0 + kWidthOf<TV0>; // N0 <= kWidthOf<TV0>
            } else {                           // can't index into rest
                return -1;                     // don't care
            }
        }(indices)...);
        if constexpr (sizeof...(rest) == 0) {
            return ab;
        } else {
            return VecConcatSized<N0 + N1, Ns...>(ab, rest...);
        }
    }

    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr HalfVecBuiltinT<TV> VecSplitLo(TV v) {
        constexpr int n = kWidthOf<TV> / 2;
        const auto& [... indices] = Detail::kIotaArray<n>;
        return __builtin_shufflevector(v, v, indices...);
    }

    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr HalfVecBuiltinT<TV> VecSplitHi(TV v) {
        constexpr int n = kWidthOf<TV> / 2;
        const auto& [... indices] = Detail::kIotaArray<n>;
        return __builtin_shufflevector(v, v, (n + indices)...);
    }

    /** @internal
     * Return @p x zero-padded to @p Bytes bytes.
     *
     * Use this function when you need two objects of the same size (e.g. for VecConcat).
     */
    template<std::size_t Bytes, VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr auto VecZeroPadTo(TV x) {
        if constexpr (sizeof(TV) == Bytes) {
            return x;
        } else if constexpr (sizeof(TV) <= sizeof(0ull)) {
            using Up = UInt<sizeof(TV)>;
            VecBuiltinTypeBytes<Up, Bytes> tmp = {__builtin_bit_cast(Up, x)};
            return __builtin_bit_cast(VecBuiltinTypeBytes<VecValueType<TV>, Bytes>, tmp);
        } else if constexpr (sizeof(TV) < Bytes) {
            return VecZeroPadTo<Bytes>(VecConcat(x, TV()));
        } else {
            static_assert(false);
        }
    }

    /** @internal
     * Return a type with sizeof 16, add zero-padding to @p x. The input must be smaller.
     *
     * Use this function instead of the above when you need to pad an argument for a SIMD builtin.
     */
    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr auto VecZeroPadTo16(TV x) {
        static_assert(sizeof(TV) < 16);
        return VecZeroPadTo<16>(x);
    }

    // work around __builtin_constant_p returning false unless passed a variable
    // (__builtin_constant_p(x[0]) is false while IsConstKnown(x[0]) is true)
    template<typename Tp>
    [[gnu::always_inline]]
    constexpr bool IsConstKnown(const Tp& x) {
        if constexpr (ComplexLike<Tp>) {
            return IsConstKnown(x.Real()) && IsConstKnown(x.Imag());
        } else {
            return __builtin_constant_p(x);
        }
    }

    [[gnu::always_inline]]
    constexpr bool IsConstKnown(const auto&... xs)
        requires(sizeof...(xs) >= 2)
    {
        if consteval {
            return true;
        } else {
            return (IsConstKnown(xs) && ...);
        }
    }

    [[gnu::always_inline]]
    constexpr bool IsConstKnownEqualTo(const auto& x, const auto& expect) {
        return IsConstKnown(x == expect) && x == expect;
    }

#if SORA_SIMD_X86
    template<VecBuiltin UV, VecBuiltin TV>
    inline UV X86CvtF16c(TV v);
#endif

    /** @internal
     * Simple wrapper around __builtin_convertvector to provide static_cast-like syntax.
     *
     * Works around GCC failing to use the F16C/AVX512F cvtps2ph/cvtph2ps instructions.
     */
    template<VecBuiltin UV, VecBuiltin TV, ArchTraits Traits = {}>
    [[gnu::always_inline]]
    constexpr UV VecCast(TV v) {
        static_assert(kWidthOf<UV> == kWidthOf<TV>);
#if SORA_SIMD_X86
        using Up = VecValueType<UV>;
        using Tp = VecValueType<TV>;
        constexpr bool toF16 = std::is_same_v<Up, _Float16>;
        constexpr bool fromF16 = std::is_same_v<Tp, _Float16>;
        constexpr bool needsF16c = Traits.HaveF16c() && !Traits.HaveAvx512fp16() && (toF16 || fromF16);
        if (needsF16c && !IsConstKnown(v)) { // Work around PR121688
            if constexpr (needsF16c) {
                return X86CvtF16c<UV>(v);
            }
        }
        if constexpr (std::is_floating_point_v<Tp> && std::is_integral_v<Up> && sizeof(UV) < sizeof(TV) &&
                      sizeof(Up) < sizeof(int)) {
            using Ip = Detail::IntegerForSize<std::min(sizeof(int), sizeof(Tp))>;
            using IV = VecBuiltinType<Ip, kWidthOf<TV>>;
            return VecCast<UV>(VecCast<IV>(v));
        }
#endif
        return __builtin_convertvector(v, UV);
    }

    /** @internal
     * Overload of the above cast function that determines the destination vector type from a given
     * element type @p Up and the `kWidthOf` the argument type.
     *
     * Calls the above overload.
     */
    template<Vectorizable Up, VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr VecBuiltinType<Up, kWidthOf<TV>> VecCast(TV v) {
        return VecCast<VecBuiltinType<Up, kWidthOf<TV>>>(v);
    }

    /** @internal
     * As above, but with additional precondition on possible values of the argument.
     *
     * Precondition: k[i] is either 0 or -1 for all i.
     */
    template<VecBuiltin UV, VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr UV VecMaskCast(TV k) {
        static_assert(std::signed_integral<VecValueType<UV>>);
        static_assert(std::signed_integral<VecValueType<TV>>);
        // TODO: __builtin_convertvector cannot be optimal because it doesn't consider input and
        // output can only be 0 or -1.
        return __builtin_convertvector(k, UV);
    }

    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr TV VecXor(TV a, TV b) {
        using Tp = VecValueType<TV>;
        if constexpr (std::is_floating_point_v<Tp>) {
            using UV = VecBuiltinType<Detail::IntegerForSize<sizeof(Tp)>, kWidthOf<TV>>;
            return __builtin_bit_cast(TV, __builtin_bit_cast(UV, a) ^ __builtin_bit_cast(UV, b));
        } else {
            return a ^ b;
        }
    }

    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr TV VecOr(TV a, TV b) {
        using Tp = VecValueType<TV>;
        if constexpr (std::is_floating_point_v<Tp>) {
            using UV = VecBuiltinType<Detail::IntegerForSize<sizeof(Tp)>, kWidthOf<TV>>;
            return __builtin_bit_cast(TV, __builtin_bit_cast(UV, a) | __builtin_bit_cast(UV, b));
        } else {
            return a | b;
        }
    }

    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr TV VecAnd(TV a, TV b) {
        using Tp = VecValueType<TV>;
        if constexpr (std::is_floating_point_v<Tp>) {
            using UV = VecBuiltinType<Detail::IntegerForSize<sizeof(Tp)>, kWidthOf<TV>>;
            return __builtin_bit_cast(TV, __builtin_bit_cast(UV, a) & __builtin_bit_cast(UV, b));
        } else {
            return a & b;
        }
    }

    /** @internal
     * Returns the bit-wise and of not @p a and @p b.
     *
     * Use VecAnd(VecNot(a), b) unless an andnot instruction is necessary for optimization.
     *
     * @see VecAndnot in simd_x86.h
     */
    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr TV VecAndnot(TV a, TV b) {
        using Tp = VecValueType<TV>;
        using UV = VecBuiltinType<Detail::IntegerForSize<sizeof(Tp)>, kWidthOf<TV>>;
        return __builtin_bit_cast(TV, ~__builtin_bit_cast(UV, a) & __builtin_bit_cast(UV, b));
    }

    template<VecBuiltin TV>
    [[gnu::always_inline]]
    constexpr TV VecNot(TV a) {
        using Tp = VecValueType<TV>;
        using UV = VecBuiltinTypeBytes<Detail::IntegerForSize<sizeof(Tp)>, sizeof(TV)>;
        if constexpr (std::is_floating_point_v<VecValueType<TV>>) {
            return __builtin_bit_cast(TV, ~__builtin_bit_cast(UV, a));
        } else {
            return ~a;
        }
    }

    /**
     * An object of given type where only the sign bits are 1.
     */
    template<VecBuiltin V>
        requires std::floating_point<VecValueType<V>>
    constexpr V kSignmask = VecXor(V() + 1, V() - 1);

    template<VecBuiltin TV, int Np = kWidthOf<TV>, typename = std::make_integer_sequence<int, Np>>
    struct VecOps;

    template<VecBuiltin TV, int Np, int... Is>
    struct VecOps<TV, Np, std::integer_sequence<int, Is...>> {
        static_assert(Np <= kWidthOf<TV>);

        using Tp = VecValueType<TV>;

        using HV = HalfVecBuiltinT<std::conditional_t<Np >= 2, TV, DoubleVecBuiltinT<TV>>>;

        [[gnu::always_inline]]
        static constexpr TV BroadcastToEven(Tp initialValue) {
            return TV{((Is & 1) == 0 ? initialValue : Tp())...};
        }

        [[gnu::always_inline]]
        static constexpr TV BroadcastToOdd(Tp initialValue) {
            return TV{((Is & 1) == 1 ? initialValue : Tp())...};
        }

        [[gnu::always_inline]]
        static constexpr bool AllOf(TV k) noexcept {
            return (... && (k[Is] != 0));
        }

        [[gnu::always_inline]]
        static constexpr bool AnyOf(TV k) noexcept {
            return (... || (k[Is] != 0));
        }

        [[gnu::always_inline]]
        static constexpr bool NoneOf(TV k) noexcept {
            return (... && (k[Is] == 0));
        }

        template<typename Offset = std::integral_constant<int, 0>>
        [[gnu::always_inline]]
        static constexpr TV Extract(VecBuiltin auto x, Offset = {}) {
            static_assert(std::is_same_v<VecValueType<TV>, VecValueType<decltype(x)>>);
            return __builtin_shufflevector(x, decltype(x)(), (Is + Offset::value)...);
        }

        // swap neighboring elements
        [[gnu::always_inline]]
        static constexpr TV SwapNeighbors(TV x) {
            return __builtin_shufflevector(x, x, (Is ^ 1)...);
        }

        // duplicate even indexed elements, dropping the odd ones
        [[gnu::always_inline]]
        static constexpr TV DupEven(TV x) {
            return __builtin_shufflevector(x, x, (Is & ~1)...);
        }

        // duplicate odd indexed elements, dropping the even ones
        [[gnu::always_inline]]
        static constexpr TV DupOdd(TV x) {
            return __builtin_shufflevector(x, x, (Is | 1)...);
        }

        [[gnu::always_inline]]
        static constexpr void OverwriteEvenElements(TV& x, HV y)
            requires(Np > 1)
        {
            constexpr SimdSizeType n = kWidthOf<TV>;
            x = __builtin_shufflevector(x,
#ifdef SORA_SIMD_CLANG
                                        VecConcat(y, y),
#else
                                        y,
#endif
                                        ((Is & 1) == 0 ? n + Is / 2 : Is)...);
        }

        [[gnu::always_inline]]
        static constexpr void OverwriteEvenElements(TV& xl, TV& xh, TV y) {
            constexpr SimdSizeType nl = kWidthOf<TV>;
            constexpr SimdSizeType nh = nl * 3 / 2;
            xl = __builtin_shufflevector(xl, y, ((Is & 1) == 0 ? nl + Is / 2 : Is)...);
            xh = __builtin_shufflevector(xh, y, ((Is & 1) == 0 ? nh + Is / 2 : Is)...);
        }

        [[gnu::always_inline]]
        static constexpr void OverwriteOddElements(TV& x, HV y)
            requires(Np > 1)
        {
            constexpr SimdSizeType n = kWidthOf<TV>;
            x = __builtin_shufflevector(x,
#ifdef SORA_SIMD_CLANG
                                        VecConcat(y, y),
#else
                                        y,
#endif
                                        ((Is & 1) == 1 ? n + Is / 2 : Is)...);
        }

        [[gnu::always_inline]]
        static constexpr void OverwriteOddElements(TV& xl, TV& xh, TV y) {
            constexpr SimdSizeType nl = kWidthOf<TV>;
            constexpr SimdSizeType nh = nl * 3 / 2;
            xl = __builtin_shufflevector(xl, y, ((Is & 1) == 1 ? nl + Is / 2 : Is)...);
            xh = __builtin_shufflevector(xh, y, ((Is & 1) == 1 ? nh + Is / 2 : Is)...);
        }

        // std::negate every even element (Real part of interleaved std::complex)
        [[gnu::always_inline]]
        static constexpr TV ComplexNegateReal(TV x) {
            return VecXor(BroadcastToEven(kSignmask<TV>[0]), x);
        }

        // std::negate every odd element (imaginary part of interleaved std::complex)
        [[gnu::always_inline]]
        static constexpr TV ComplexNegateImag(TV x) {
            return VecXor(BroadcastToOdd(kSignmask<TV>[0]), x);
        }

        // Subtract elements with even index, add elements with odd index.
        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] static constexpr TV Addsub(TV x, TV y) {
            if constexpr (Traits.HaveAddsub()) {
                // GCC recognizes this pattern as Addsub
                return __builtin_shufflevector(x - y, x + y, (Is + (Is & 1) * kWidthOf<TV>)...);
            } else {
                return x + ComplexNegateReal(y);
            }
        }

        // true if all elements are know to be equal to ref at compile time
        [[gnu::always_inline]]
        static constexpr bool AreElementsConstKnownEqualTo(TV x, Tp ref) {
            return (IsConstKnownEqualTo(x[Is], ref) && ...);
        }

        // True iff all elements at even indexes are zero. This includes signed zeros only when
        // -fno-signed-zeros is in effect.
        template<OptTraits Traits = {}>
        [[gnu::always_inline]] static constexpr bool ComplexRealIsConstKnownZero(TV x) {
            if constexpr (Traits.ConformingToStdcAnnexG()) {
                using Up = UInt<sizeof(Tp)>;
                return (((Is & 1) == 1 || IsConstKnownEqualTo(__builtin_bit_cast(Up, x[Is]), Up())) && ...);
            } else {
                return (((Is & 1) == 1 || IsConstKnownEqualTo(x[Is], Tp())) && ...);
            }
        }

        // True iff all elements at odd indexes are zero. This includes signed zeros only when
        // -fno-signed-zeros is in effect.
        template<OptTraits Traits = {}>
        [[gnu::always_inline]] static constexpr bool ComplexImagIsConstKnownZero(TV x) {
            if constexpr (Traits.ConformingToStdcAnnexG()) {
                using Up = UInt<sizeof(Tp)>;
                return (((Is & 1) == 0 || IsConstKnownEqualTo(__builtin_bit_cast(Up, x[Is]), Up())) && ...);
            } else {
                return (((Is & 1) == 0 || IsConstKnownEqualTo(x[Is], Tp())) && ...);
            }
        }
    };

} // namespace Sora::Math::Simd