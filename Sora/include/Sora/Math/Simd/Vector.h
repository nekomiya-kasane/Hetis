/**
 * @file Vector.h
 * @brief SIMD vector types and arithmetic operations.
 * @ingroup Math
 */
#pragma once

#include "Mask.h"
#include "Flags.h"

#include <utility>
#include <functional>
#include <cmath>

namespace Sora::Math::Simd {

    // disabled BasicVector
    template<typename Tp, typename Ap>
    class BasicVector {
    public:
        using ValueType = Tp;

        using AbiType = Ap;

        using MaskType = BasicMask<0, void>; // disabled

#define SORA_SIMD_DELETE                                                                                               \
    "This specialization is disabled because of an invalid combination "                                               \
    "of template arguments to BasicVector."

        BasicVector() = delete (SORA_SIMD_DELETE);

        ~BasicVector() = delete (SORA_SIMD_DELETE);

        BasicVector(const BasicVector&) = delete (SORA_SIMD_DELETE);

        BasicVector& operator=(const BasicVector&) = delete (SORA_SIMD_DELETE);

#undef SORA_SIMD_DELETE
    };

    template<typename Tp, typename Ap>
    class VecBase {
        using Vp = BasicVector<Tp, Ap>;

    public:
        using ValueType = Tp;

        using AbiType = Ap;

        using MaskType = BasicMask<sizeof(Tp), AbiType>;

        using IteratorType = Iterator<Vp>;

        using ConstIteratorType = Iterator<const Vp>;

        constexpr IteratorType Begin() noexcept { return {static_cast<Vp&>(*this), 0}; }

        constexpr ConstIteratorType Begin() const noexcept { return Cbegin(); }

        constexpr ConstIteratorType Cbegin() const noexcept { return {static_cast<const Vp&>(*this), 0}; }

        constexpr std::default_sentinel_t End() const noexcept { return {}; }

        constexpr std::default_sentinel_t Cend() const noexcept { return {}; }

        static constexpr auto kSize = kSimdSizeC<Ap::kStorageSize>;

        VecBase() = default;

        // LWG issue from 2026-03-04 / P4042R0
        template<typename Up, typename UAbi>
            requires(Ap::kStorageSize != UAbi::kStorageSize)
        VecBase(const BasicVector<Up, UAbi>&) = delete ("size mismatch");

        template<typename Up, typename UAbi>
            requires(Ap::kStorageSize == UAbi::kStorageSize) && (!ExplicitlyConvertibleTo<Up, Tp>)
        explicit VecBase(const BasicVector<Up, UAbi>&) = delete ("the value types are not convertible");

        [[gnu::always_inline]]
        friend constexpr Vp operator+(const Vp& x, const Vp& y) noexcept {
            Vp r = x;
            r += y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator-(const Vp& x, const Vp& y) noexcept {
            Vp r = x;
            r -= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator*(const Vp& x, const Vp& y) noexcept {
            Vp r = x;
            r *= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator/(const Vp& x, const Vp& y) noexcept {
            Vp r = x;
            r /= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator%(const Vp& x, const Vp& y) noexcept
            requires requires(Tp a) { a % a; }
        {
            Vp r = x;
            r %= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator&(const Vp& x, const Vp& y) noexcept
            requires requires(Tp a) { a & a; }
        {
            Vp r = x;
            r &= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator|(const Vp& x, const Vp& y) noexcept
            requires requires(Tp a) { a | a; }
        {
            Vp r = x;
            r |= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator^(const Vp& x, const Vp& y) noexcept
            requires requires(Tp a) { a ^ a; }
        {
            Vp r = x;
            r ^= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator<<(const Vp& x, const Vp& y) SORA_SIMD_NOEXCEPT
            requires requires(Tp a) { a << a; }
        {
            Vp r = x;
            r <<= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator<<(const Vp& x, SimdSizeType y) SORA_SIMD_NOEXCEPT
            requires requires(Tp a, SimdSizeType b) { a << b; }
        {
            Vp r = x;
            r <<= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator>>(const Vp& x, const Vp& y) SORA_SIMD_NOEXCEPT
            requires requires(Tp a) { a >> a; }
        {
            Vp r = x;
            r >>= y;
            return r;
        }

        [[gnu::always_inline]]
        friend constexpr Vp operator>>(const Vp& x, SimdSizeType y) SORA_SIMD_NOEXCEPT
            requires requires(Tp a, SimdSizeType b) { a >> b; }
        {
            Vp r = x;
            r >>= y;
            return r;
        }
    };

    struct LoadCtorTag {};

    template<std::integral Tp>
    inline constexpr Tp kMaxShift = (sizeof(Tp) < sizeof(int) ? sizeof(int) : sizeof(Tp)) * __CHAR_BIT__;

    template<Vectorizable Tp, AbiTag Ap>
        requires(Ap::kNreg == 1) && (!ComplexLike<Tp>)
    class BasicVector<Tp, Ap> : public VecBase<Tp, Ap> {
        template<typename, typename>
        friend class BasicVector;

        template<std::size_t, typename>
        friend class BasicMask;

        static constexpr int kStorageSize = Ap::kStorageSize;

        static constexpr int kFullSize = std::bit_ceil(unsigned(kStorageSize));

        static constexpr bool kIsScalar = kStorageSize == 1;

        static constexpr bool kUseBitmask = Ap::kIsBitmask && !kIsScalar;

        using DataType = typename Ap::template DataType<Tp>;

        /** @internal
         * @brief Underlying vector data storage.
         *
         * This member holds the vector object using a GNU vector type or a platform-specific vector
         * type determined by the ABI tag. For size 1 vectors, this is a single value (Tp).
         */
        DataType data;

        static constexpr bool kIsPartial = sizeof(data) > sizeof(Tp) * kStorageSize;

        using CanonValueType = CanonicalVecTypeT<Tp>;

    public:
        using ValueType = Tp;

        using MaskType = VecBase<Tp, Ap>::MaskType;

        // internal but public API ----------------------------------------------
        [[gnu::always_inline]]
        static constexpr BasicVector Init(DataType x) {
            BasicVector r;
            r.data = x;
            return r;
        }

        [[gnu::always_inline]]
        constexpr DataType& Get() noexcept {
            return data;
        }

        [[gnu::always_inline]]
        constexpr const DataType& Get() const noexcept {
            return data;
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicVector& x) {
            return __builtin_constant_p(x.data);
        }

        [[gnu::always_inline]]
        constexpr auto ConcatData([[maybe_unused]] bool doSanitize = false) const {
            if constexpr (kIsScalar) {
                return VecBuiltinType<CanonValueType, 1>{data};
            } else {
                return data;
            }
        }

        template<int Size = kStorageSize, int Offset = 0, typename A0, typename Fp>
        [[gnu::always_inline]]
        static constexpr BasicVector StaticPermute(const BasicVector<ValueType, A0>& x, Fp&& idxmap) {
            using Xp = BasicVector<ValueType, A0>;
            BasicVector r;
            if constexpr (kIsScalar) {
                constexpr SimdSizeType j = [&] consteval {
                    if constexpr (IndexPermutationFunctionSized<Fp>) {
                        return idxmap(Offset, Size);
                    } else {
                        return idxmap(Offset);
                    }
                }();
                if constexpr (j == Sora::Math::Simd::kZeroElement || j == Sora::Math::Simd::kUninitElement) {
                    return BasicVector();
                } else {
                    static_assert(j >= 0 && j < Xp::kStorageSize);
                }
                r.data = x[j];
            } else {
                auto idxmap2 = [=](auto i) consteval {
                    if constexpr (int(i + Offset) >= Size) { // kFullSize > Size
                        return kSimdSizeC<Sora::Math::Simd::kUninitElement>;
                    } else if constexpr (IndexPermutationFunctionSized<Fp>) {
                        return kSimdSizeC<idxmap(i + Offset, Size)>;
                    } else {
                        return kSimdSizeC<idxmap(i + Offset)>;
                    }
                };
                constexpr auto adjIdx = [](auto i) {
                    constexpr int j = i;
                    if constexpr (j == Sora::Math::Simd::kZeroElement) {
                        return kSimdSizeC<std::bit_ceil(unsigned(Xp::kStorageSize))>;
                    } else if constexpr (j == Sora::Math::Simd::kUninitElement) {
                        return kSimdSizeC<-1>;
                    } else {
                        static_assert(j >= 0 && j < Xp::kStorageSize);
                        return kSimdSizeC<j>;
                    }
                };
                const auto& [... initialIndices] = Detail::kIotaArray<kStorageSize>;
                constexpr bool needsZeroElement =
                    ((idxmap2(kSimdSizeC<initialIndices>).value == Sora::Math::Simd::kZeroElement) || ...);
                const auto& [... fullIndices] = Detail::kIotaArray<kFullSize>;
                if constexpr (A0::kNreg == 2 && !needsZeroElement) {
                    r.data = __builtin_shufflevector(x.data0.data, x.data1.data,
                                                     adjIdx(idxmap2(kSimdSizeC<fullIndices>)).value...);
                } else {
                    r.data = __builtin_shufflevector(x.ConcatData(), decltype(x.ConcatData())(),
                                                     adjIdx(idxmap2(kSimdSizeC<fullIndices>)).value...);
                }
            }
            return r;
        }

        template<typename Vp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            constexpr int n = kStorageSize / Vp::kStorageSize;
            constexpr int rem = kStorageSize % Vp::kStorageSize;
            const auto& [... indices] = Detail::kIotaArray<n>;
            if constexpr (rem == 0) {
                return std::array<Vp, n>{ExtractSimdAt<Vp>(Detail::kConstant<Vp::kStorageSize * indices>, *this)...};
            } else {
                using Rest = Resize<rem, Vp>;
                return std::tuple(ExtractSimdAt<Vp>(Detail::kConstant<Vp::kStorageSize * indices>, *this)...,
                                  ExtractSimdAt<Rest>(Detail::kConstant<Vp::kStorageSize * n>, *this));
            }
        }

        [[gnu::always_inline]]
        static constexpr BasicVector Concat(const BasicVector& x0) noexcept {
            return x0;
        }

        template<typename... As>
            requires(sizeof...(As) > 1)
        [[gnu::always_inline]]
        static constexpr BasicVector Concat(const BasicVector<ValueType, As>&... xs) noexcept {
            static_assert(kStorageSize == (As::kStorageSize + ...));
            return ExtractSimdAt<BasicVector>(Detail::kConstant<0>, xs...);
        }

        /** @internal
         * Shifts elements to the front by @p Shift positions (or to the back for negative @p
         * Shift).
         *
         * This function moves elements towards lower indices (front of the vector).
         * Elements that would shift beyond the vector bounds are replaced with zero. Negative shift
         * values shift in the opposite direction.
         *
         * @warning The naming can be confusing due to little-endian std::byte order:
         * - Despite the name "shifted_to_front", the underlying hardware instruction
         *   shifts bits to the right (psrl...)
         * - The function name refers to element indices, not bit positions
         *
         * @tparam Shift Number of positions to shift elements towards the front.
         *                Must be -size() < Shift < size().
         *
         * @return A new vector with elements shifted to front or back.
         *
         * Example:
         * @code
         * kIota<Vector<int, 4>>.ElementsShiftedToFront<2>(); // {2, 3, 0, 0}
         * kIota<Vector<int, 4>>.ElementsShiftedToFront<-2>(); // {0, 0, 0, 1}
         * @endcode
         */
        template<int Shift, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr BasicVector ElementsShiftedToFront() const {
            static_assert(Shift < kStorageSize && -Shift < kStorageSize);
            if constexpr (Shift == 0) {
                return *this;
            }
#ifdef __SSE2__
            else if (!IsConstKnown(*this)) {
                if constexpr (sizeof(data) == 16 && Shift > 0) {
                    return reinterpret_cast<DataType>(
                        SORA_SIMD_X86_PSRLDQI128(VecBitCast<long long>(data), Shift * sizeof(ValueType) * 8));
                } else if constexpr (sizeof(data) == 16 && Shift < 0) {
                    return reinterpret_cast<DataType>(
                        SORA_SIMD_X86_PSLLDQI128(VecBitCast<long long>(data), -Shift * sizeof(ValueType) * 8));
                } else if constexpr (sizeof(data) < 16) {
                    auto x = reinterpret_cast<VecBuiltinTypeBytes<long long, 16>>(VecZeroPadTo16(data));
                    if constexpr (Shift > 0) {
                        x = SORA_SIMD_X86_PSRLDQI128(x, Shift * sizeof(ValueType) * 8);
                    } else {
                        x = SORA_SIMD_X86_PSLLDQI128(x, -Shift * sizeof(ValueType) * 8);
                    }
                    return VecOps<DataType>::Extract(VecBitCast<CanonValueType>(x));
                }
            }
#endif
            return StaticPermute(*this, [](int i) consteval {
                int off = i + Shift;
                return off >= kStorageSize || off < 0 ? kZeroElement : off;
            });
        }

        /** @internal
         * @brief Set padding elements to @p identityValue; add more padding elements if necessary.
         *
         * @note This function can rearrange the element order since the result is only used for
         * reductions.
         */
        template<typename Vp, CanonValueType IdentityValue>
        [[gnu::always_inline]]
        constexpr Vp PadToTWithValue() const noexcept {
            static_assert(!Vp::kIsPartial);
            static_assert(Ap::kNreg == 1);
            if constexpr (sizeof(Vp) == 32) { // when we need to Reduce from a 512-bit register
                static_assert(sizeof(data) == 32);
                constexpr auto k = Vp::MaskType::PartialMaskOfN(kStorageSize);
                return SelectImpl(k, Vp::Init(data), IdentityValue);
            } else {
                static_assert(sizeof(Vp) <= 16); // => Max. 7 Bytes need to be zeroed
                static_assert(sizeof(data) <= sizeof(Vp));
                Vp v1 = VecZeroPadTo<sizeof(Vp)>(data);
                if constexpr (IdentityValue == 0 && kIsPartial) {
                    // cheapest solution: shift values to the back while shifting in zeros
                    // This is valid because we shift out padding elements and use all elements in a
                    // subsequent reduction.
                    v1 = v1.template ElementsShiftedToFront<-(Vp::kStorageSize - kStorageSize)>();
                } else if constexpr (Vp::kStorageSize - kStorageSize == 1) {
                    // if a single element needs to be changed, use an insert instruction
                    VecSet(v1.data, Vp::kStorageSize - 1, IdentityValue);
                } else if constexpr (std::has_single_bit(unsigned(
                                         Vp::kStorageSize - kStorageSize))) { // if 2^n elements need to be changed, use
                                                                              // a single insert instruction
                    constexpr int n = Vp::kStorageSize - kStorageSize;
                    using Ip = Detail::IntegerForSize<n * sizeof(CanonValueType)>;
                    const auto& [... indices] = Detail::kIotaArray<n>;
                    constexpr CanonValueType idn[n] = {((void)indices, IdentityValue)...};
                    auto vn = VecBitCast<Ip>(v1.data);
                    VecSet(vn, Vp::kStorageSize / n - 1, __builtin_bit_cast(Ip, idn));
                    v1.data = reinterpret_cast<typename Vp::DataType>(vn);
                } else if constexpr (IdentityValue != 0 && !kIsPartial) { // if VecZeroPadTo added zeros in all the
                                                                          // places where we need identityValue, a
                    // bitwise or is sufficient (needs a vector constant for the identityValue vector, which
                    // isn't optimal)
                    constexpr Vp idn([](int i) { return i >= kStorageSize ? IdentityValue : CanonValueType(); });
                    v1.data = VecOr(v1.data, idn.data);
                } else if constexpr (IdentityValue != 0 || kIsPartial) { // fallback
                    constexpr auto k = Vp::MaskType::PartialMaskOfN(kStorageSize);
                    v1 = SelectImpl(k, v1, IdentityValue);
                }
                return v1;
            }
        }

        [[gnu::always_inline]]
        constexpr auto ReduceToHalf(auto binaryOp) const {
            static_assert(std::has_single_bit(unsigned(kStorageSize)));
            auto [a, b] = Chunk<kStorageSize / 2>(*this);
            return binaryOp(a, b);
        }

        template<typename Rest, typename BinaryOp>
        [[gnu::always_inline]]
        constexpr ValueType ReduceTail(const Rest& rest, BinaryOp binaryOp) const {
            if constexpr (kIsScalar) {
                return binaryOp(*this, rest).data;
            } else if constexpr (Rest::kStorageSize == kStorageSize) {
                return binaryOp(*this, rest).Reduce(binaryOp);
            } else if constexpr (Rest::kStorageSize > kStorageSize) {
                auto [a, b] = rest.template ChunkStorage<BasicVector>();
                return binaryOp(*this, a).ReduceTail(b, binaryOp);
            } else if constexpr (Rest::kStorageSize == 1) {
                return binaryOp(Rest(Reduce(binaryOp)), rest)[0];
            } else if constexpr (sizeof(data) <= 16 && requires {
                                     DefaultIdentityElement<CanonValueType, BinaryOp>();
                                 }) { // extend rest with identity element for more parallelism
                constexpr CanonValueType identityValue = DefaultIdentityElement<CanonValueType, BinaryOp>();
                return binaryOp(data, rest.template PadToTWithValue<BasicVector, identityValue>()).Reduce(binaryOp);
            } else {
                return ReduceToHalf(binaryOp).ReduceTail(rest, binaryOp);
            }
        }

        /** @internal
         * @brief Reduction over @p binaryOp of all (non-padding) elements.
         *
         * @note The implementation assumes it is most efficient to first Reduce to one 128-bit SIMD
         * register and then shuffle elements while sticking to 128-bit registers.
         */
        template<typename BinaryOp, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr ValueType Reduce(BinaryOp binaryOp) const {
            constexpr bool haveIdElem = requires { DefaultIdentityElement<CanonValueType, BinaryOp>(); };
            if constexpr (kStorageSize == 1) {
                return operator[](0);
            } else if constexpr (Traits.template EvalAsF32<ValueType>() &&
                                 (std::is_same_v<BinaryOp, std::plus<>> ||
                                  std::is_same_v<BinaryOp, std::multiplies<>>)) {
                return ValueType(Rebind<float, BasicVector>(*this).Reduce(binaryOp));
            }
#ifdef __SSE2__
            else if constexpr (std::is_integral_v<ValueType> && sizeof(ValueType) == 1 &&
                               std::is_same_v<decltype(binaryOp), std::multiplies<>>) {
                // convert to unsigned short because of missing 8-bit mul instruction
                // we don't need to preserve the order of elements
                //
                // The left columns under Latency and Throughput show bit-cast to ushort with shift by
                // 8. The right column uses the alternative in the else branch.
                // Benchmark on Intel Ultra 7 165U (AVX2)
                //   TYPE            Latency      Throughput
                //             [cycles/call]   [cycles/call]
                // schar, 2        9.11  7.73      3.17  3.21
                // schar, 4        31.6  34.9      5.11  6.97
                // schar, 8        35.7  41.5      7.77  7.17
                // schar, 16       36.7  44.1      6.66  8.96
                // schar, 32       42.2  61.1      8.82  10.1
                if constexpr (!kIsPartial) { // If all elements participate in the reduction we can take this shortcut
                    using V16 = Resize<kStorageSize / 2, Rebind<unsigned short, BasicVector>>;
                    auto a = __builtin_bit_cast(V16, *this);
                    return binaryOp(a, a >> 8).Reduce(binaryOp);
                } else {
                    using V16 = Rebind<unsigned short, BasicVector>;
                    return V16(*this).Reduce(binaryOp);
                }
            }
#endif
            else if constexpr (std::has_single_bit(unsigned(kStorageSize))) {
                if constexpr (sizeof(data) > 16) {
                    return ReduceToHalf(binaryOp).Reduce(binaryOp);
                } else if constexpr (kStorageSize == 2) {
                    return ReduceToHalf(binaryOp)[0];
                } else {
                    static_assert(kStorageSize <= 16);
                    auto x = *this;
#ifdef __SSE2__
                    if constexpr (sizeof(data) <= 16 && std::is_integral_v<ValueType>) {
                        if constexpr (kStorageSize > 8) {
                            x = binaryOp(x, x.template ElementsShiftedToFront<8>());
                        }
                        if constexpr (kStorageSize > 4) {
                            x = binaryOp(x, x.template ElementsShiftedToFront<4>());
                        }
                        if constexpr (kStorageSize > 2) {
                            x = binaryOp(x, x.template ElementsShiftedToFront<2>());
                        }
                        // We could also call binaryOp with Vector<T, 1> arguments. However,
                        // micro-benchmarking on Intel Ultra 7 165U showed this to be more efficient:
                        return binaryOp(x, x.template ElementsShiftedToFront<1>())[0];
                    }
#endif
                    if constexpr (kStorageSize > 8) {
                        x = binaryOp(x, StaticPermute(x, SwapNeighbors<8>()));
                    }
                    if constexpr (kStorageSize > 4) {
                        x = binaryOp(x, StaticPermute(x, SwapNeighbors<4>()));
                    }
#ifdef __SSE2__
                    // avoid pshufb by "promoting" to int
                    if constexpr (std::is_integral_v<ValueType> && sizeof(ValueType) <= 1) {
                        return ValueType(Resize<4, Rebind<int, BasicVector>>(Chunk<4>(x)[0]).Reduce(binaryOp));
                    }
#endif
                    if constexpr (kStorageSize > 2) {
                        x = binaryOp(x, StaticPermute(x, SwapNeighbors<2>()));
                    }
                    if constexpr (std::is_integral_v<ValueType> && sizeof(ValueType) == 2) {
                        return binaryOp(x, StaticPermute(x, SwapNeighbors<1>()))[0];
                    } else {
                        return binaryOp(Vector<ValueType, 1>(x[0]), Vector<ValueType, 1>(x[1]))[0];
                    }
                }
            } else if constexpr (sizeof(data) == 32) {
                const auto [lo, hi] = Chunk<std::bit_floor(unsigned(kStorageSize))>(*this);
                return lo.ReduceTail(hi, binaryOp);
            } else if constexpr (sizeof(data) == 64) {
                // e.g. kStorageSize = 16 + 16 + 15 (Vector<char, 47>)
                // -> 8 + 8 + 7 -> 4 + 4 + 3 -> 2 + 2 + 1 -> 1
                auto chunked = Chunk<std::bit_floor(unsigned(kStorageSize)) / 2>(*this);
                using Cp = decltype(chunked);
                if constexpr (std::tuple_size_v<Cp> == 4) {
                    const auto& [a, b, c, rest] = chunked;
                    constexpr bool amdCpu = Traits.HaveSse4a();
                    if constexpr (haveIdElem && rest.kStorageSize > 1 &&
                                  amdCpu) { // do one 256-bit op -> one 128-bit op
                        // 4 cycles on Zen4/5 until Reduce (short, 26, std::plus<>)
                        // 9 cycles on Skylake-AVX512 until Reduce
                        // 9 cycles on Zen4/5 until Reduce (short, 27, std::multiplies<>)
                        // 17 cycles on Skylake-AVX512 until Reduce (short, 27, std::multiplies<>)
                        const auto& [a, rest] = Chunk<std::bit_floor(unsigned(kStorageSize))>(*this);
                        using Vp = std::remove_cvref_t<decltype(a)>;
                        constexpr CanonValueType identityValue = DefaultIdentityElement<CanonValueType, BinaryOp>();
                        const Vp b = rest.template PadToTWithValue<Vp, identityValue>();
                        return binaryOp(a, b).Reduce(binaryOp);
                    } else if constexpr (haveIdElem && rest.kStorageSize > 1) { // do two 128-bit ops -> one 128-bit op
                        // 5 cycles on Zen4/5 until Reduce (short, 26, std::plus<>)
                        // 7 cycles on Skylake-AVX512 until Reduce (short, 26, std::plus<>)
                        // 9 cycles on Zen4/5 until Reduce (short, 27, std::multiplies<>)
                        // 16 cycles on Skylake-AVX512 until Reduce (short, 27, std::multiplies<>)
                        using Vp = std::remove_cvref_t<decltype(a)>;
                        constexpr CanonValueType identityValue = DefaultIdentityElement<CanonValueType, BinaryOp>();
                        const Vp d = rest.template PadToTWithValue<Vp, identityValue>();
                        return binaryOp(binaryOp(a, b), binaryOp(c, d)).Reduce(binaryOp);
                    } else {
                        return binaryOp(binaryOp(a, b), c).ReduceTail(rest, binaryOp);
                    }
                } else if constexpr (std::tuple_size_v<Cp> == 3) {
                    const auto& [a, b, rest] = chunked;
                    return binaryOp(a, b).ReduceTail(rest, binaryOp);
                } else {
                    static_assert(false);
                }
            } else if constexpr (haveIdElem) {
                constexpr CanonValueType identityValue = DefaultIdentityElement<CanonValueType, BinaryOp>();
                using Vp = Resize<std::bit_ceil(unsigned(kStorageSize)), BasicVector>;
                return PadToTWithValue<Vp, identityValue>().Reduce(binaryOp);
            } else {
                const auto& [a, rest] = Chunk<std::bit_floor(unsigned(kStorageSize))>(*this);
                return a.ReduceTail(rest, binaryOp);
            }
        }

        // [simd.math] ----------------------------------------------------------
        //
        // ISO/IEC 60559 on the classification operations (5.7.2 General Operations):
        // "They are never exceptional, even for signaling NaNs."
        //
        template<OptTraits Traits = {}>
        [[gnu::always_inline]] constexpr MaskType Isnan() const
            requires std::is_floating_point_v<ValueType>
        {
            if constexpr (Traits.FiniteMathOnly()) {
                return MaskType(false);
            } else if constexpr (kIsScalar) {
                return MaskType(std::isnan(data));
            } else if constexpr (kUseBitmask) {
                return Isunordered(*this);
            } else if constexpr (!Traits.SupportSnan()) {
                return !(*this == *this);
            } else if (IsConstKnown(data)) {
                return MaskType([&](int i) { return std::isnan(data[i]); });
            } else {
                // 60559: NaN is represented as Inf + non-zero mantissa bits
                using Ip = Detail::IntegerForSize<sizeof(ValueType)>;
                return __builtin_bit_cast(Ip, std::numeric_limits<ValueType>::infinity()) <
                       __builtin_bit_cast(Rebind<Ip, BasicVector>, Fabs());
            }
        }

        template<TargetTraits Traits = {}>
        [[gnu::always_inline]] constexpr MaskType Isinf() const
            requires std::is_floating_point_v<ValueType>
        {
            if constexpr (Traits.FiniteMathOnly()) {
                return MaskType(false);
            } else if constexpr (kIsScalar) {
                return MaskType(std::isinf(data));
            } else if (IsConstKnown(data)) {
                return MaskType([&](int i) { return std::isinf(data[i]); });
            }
#ifdef SORA_SIMD_X86
            else if constexpr (kUseBitmask) {
                return MaskType::Init(X86BitmaskIsinf(data));
            } else if constexpr (Traits.HaveAvx512dq()) {
                return X86BitToVecmask<typename MaskType::DataType>(X86BitmaskIsinf(data));
            }
#endif
            else {
                using Ip = Detail::IntegerForSize<sizeof(ValueType)>;
                return VecBitCast<Ip>(Fabs().data) ==
                       __builtin_bit_cast(Ip, std::numeric_limits<ValueType>::infinity());
            }
        }

        [[gnu::always_inline]]
        constexpr BasicVector Abs() const
            requires std::signed_integral<ValueType>
        {
            return data < 0 ? -data : data;
        }

        [[gnu::always_inline]]
        constexpr BasicVector Fabs() const
            requires std::floating_point<ValueType>
        {
            if constexpr (kIsScalar) {
                return std::fabs(data);
            } else {
                return VecAnd(VecNot(kSignmask<DataType>), data);
            }
        }

        template<TargetTraits Traits = {}>
        [[gnu::always_inline]] constexpr MaskType Isunordered(BasicVector y) const
            requires std::is_floating_point_v<ValueType>
        {
            if constexpr (Traits.FiniteMathOnly()) {
                return MaskType(false);
            } else if constexpr (kIsScalar) {
                return MaskType(std::isunordered(data, y.data));
            }
#ifdef SORA_SIMD_X86
            else if constexpr (kUseBitmask) {
                return BitmaskCmp<X86Cmp::kUnord>(y.data);
            }
#endif
            else {
                return MaskType([&](int i) { return std::isunordered(data[i], y.data[i]); });
            }
        }

        /** @internal
         * Implementation of @ref PartialLoad.
         *
         * @param mem  A pointer to an std::array of @p n values. Can be std::complex or Real.
         * @param n    Read no more than @p n values from memory. However, depending on @p mem
         *               Alignment, out of bounds reads are benign.
         */
        template<typename Up, ArchTraits Traits = {}>
        static inline BasicVector PartialLoad(const Up* mem, std::size_t n) {
            if constexpr (kIsScalar) {
                return n == 0 ? BasicVector() : BasicVector(static_cast<ValueType>(*mem));
            } else if (IsConstKnownEqualTo(n >= std::size_t(kStorageSize), true)) {
                return BasicVector(LoadCtorTag(), mem);
            } else if constexpr (!ConvertsTrivially<Up, ValueType>) {
                return static_cast<BasicVector>(Rebind<Up, BasicVector>::PartialLoad(mem, n));
            } else {
#if SORA_SIMD_X86
                if constexpr (Traits.HaveAvx512f() || (Traits.HaveAvx() && sizeof(Up) >= 4)) {
                    const auto k = n < kStorageSize ? MaskType::PartialMaskOfN(int(n)) : MaskType(true);
                    return MaskedLoad(mem, MaskType::PartialMaskOfN(int(n)));
                }
#endif
                if (n >= std::size_t(kStorageSize)) [[unlikely]] {
                    return BasicVector(LoadCtorTag(), mem);
                }
#if SORA_SIMD_X86 // TODO: where else is this "safe"?
                // allow out-of-bounds read when it cannot lead to a #GP
                else if (IsConstKnownEqualTo(Detail::IsSufficientlyAligned<sizeof(Up) * kFullSize>(mem), true)) {
                    return SelectImpl(MaskType::PartialMaskOfN(int(n)), BasicVector(LoadCtorTag(), mem), BasicVector());
                }
#endif
                else if constexpr (kStorageSize > 4) {
                    alignas(DataType) std::byte dst[sizeof(DataType)] = {};
                    const std::byte* src = reinterpret_cast<const std::byte*>(mem);
                    MemcpyChunks<sizeof(Up), sizeof(DataType)>(dst, src, n);
                    return __builtin_bit_cast(DataType, dst);
                } else if (n == 0) [[unlikely]] {
                    return BasicVector();
                } else if constexpr (kStorageSize == 2) {
                    return DataType{static_cast<ValueType>(mem[0]), 0};
                } else {
                    const auto& [... indices] = Detail::kIotaArray<kStorageSize - 2>;
                    return DataType{static_cast<ValueType>(mem[0]),
                                    static_cast<ValueType>(indices + 1 < n ? mem[indices + 1] : 0)...};
                }
            }
        }

        /** @internal
         * Loads elements from @p mem according to Mask @p k.
         *
         * @param mem Pointer (in)to std::array.
         * @param k   Mask controlling which elements to load. For each bit i in the Mask:
         *              - If bit i is 1: copy mem[i] into result[i]
         *              - If bit i is 0: result[i] is default initialized
         *
         * @note This function assumes it's called after determining that no other method
         *       (like full load) is more appropriate. Calling with all Mask bits set to 1
         *       is suboptimal for performance but still correct.
         */
        template<typename Up, ArchTraits Traits = {}>
        static inline BasicVector MaskedLoad(const Up* mem, MaskType k) {
            if constexpr (kStorageSize == 1) {
                return k[0] ? static_cast<ValueType>(mem[0]) : ValueType();
            }
#if SORA_SIMD_X86
            else if constexpr (Traits.HaveAvx512f()) {
                return X86MaskedLoad<DataType>(mem, k.data);
            } else if constexpr (Traits.HaveAvx() && (sizeof(Up) == 4 || sizeof(Up) == 8)) {
                if constexpr (ConvertsTrivially<Up, ValueType>) {
                    return X86MaskedLoad<DataType>(mem, k.data);
                } else {
                    using UV = Rebind<Up, BasicVector>;
                    return BasicVector(UV::MaskedLoad(mem, typename UV::MaskType(k)));
                }
            }
#endif
            else if (k.NoneOf()) [[unlikely]] {
                return BasicVector();
            } else if constexpr (kIsScalar) {
                return BasicVector(static_cast<ValueType>(*mem));
            } else {
                // Use at least 4-std::byte bits in BitForeach for better code-gen
                Bitmask < kStorageSize<32 ? 32 : kStorageSize> bits = k.ToUint();
                [[assume(bits != 0)]]; // because of 'k.NoneOf()' branch above
                if constexpr (ConvertsTrivially<Up, ValueType>) {
                    DataType r = {};
                    BitForeach(bits, [&] [[gnu::always_inline]] (int i) { r[i] = mem[i]; });
                    return r;
                } else {
                    using UV = Rebind<Up, BasicVector>;
                    alignas(UV) Up tmp[sizeof(UV) / sizeof(Up)] = {};
                    BitForeach(bits, [&] [[gnu::always_inline]] (int i) { tmp[i] = mem[i]; });
                    return BasicVector(__builtin_bit_cast(UV, tmp));
                }
            }
        }

        template<typename Up>
        [[gnu::always_inline]]
        inline void Store(Up* mem) const {
            if constexpr (ConvertsTrivially<ValueType, Up>) {
                __builtin_memcpy(mem, &data, sizeof(Up) * kStorageSize);
            } else {
                Rebind<Up, BasicVector>(*this).Store(mem);
            }
        }

        /** @internal
         * Implementation of @ref PartialStore.
         *
         * @note This is a static function to allow passing @p v via register in case the function
         * is not inlined.
         *
         * @note The function is not marked @c __always_inline__ since code-gen can become fairly
         * long.
         */
        template<typename Up, ArchTraits Traits = {}>
        static inline void PartialStore(const BasicVector v, Up* mem, std::size_t n) {
            if (IsConstKnownEqualTo(n >= kStorageSize, true)) {
                v.Store(mem);
            }
#if SORA_SIMD_X86
            else if constexpr (Traits.HaveAvx512f() && !kIsScalar) {
                const auto k = n < kStorageSize ? MaskType::PartialMaskOfN(int(n)) : MaskType(true);
                return MaskedStore(v, mem, k);
            }
#endif
            else if (n >= kStorageSize) [[unlikely]] {
                (void)v.Store(mem);
            } else if (n == 0) [[unlikely]] {
                return;
            } else if constexpr (ConvertsTrivially<ValueType, Up>) {
                std::byte* dst = reinterpret_cast<std::byte*>(mem);
                const std::byte* src = reinterpret_cast<const std::byte*>(&v.data);
                MemcpyChunks<sizeof(Up), sizeof(data)>(dst, src, n);
            } else {
                using UV = Rebind<Up, BasicVector>;
                UV::PartialStore(UV(v), mem, n);
            }
        }

        /** @internal
         * Stores elements of @p v to @p mem according to Mask @p k.
         *
         * @param v   Values to Store to @p mem.
         * @param mem Pointer (in)to std::array.
         * @param k   Mask controlling which elements to Store. For each bit i in the Mask:
         *              - If bit i is 1: Store v[i] to mem[i]
         *              - If bit i is 0: mem[i] is left unchanged
         *
         * @note This function assumes it's called after determining that no other method
         *       (like full Store) is more appropriate. Calling with all Mask bits set to 1
         *       is suboptimal for performance but still correct.
         */
        template<typename Up, ArchTraits Traits = {}>
        //[[gnu::always_inline]]
        static inline void MaskedStore(const BasicVector v, Up* mem, const MaskType k) {
#if SORA_SIMD_X86
            if constexpr (Traits.HaveAvx512f()) {
                X86MaskedStore(v.data, mem, k.data);
                return;
            } else if constexpr (Traits.HaveAvx() && (sizeof(Up) == 4 || sizeof(Up) == 8)) {
                if constexpr (ConvertsTrivially<ValueType, Up>) {
                    X86MaskedStore(v.data, mem, k.data);
                } else {
                    using UV = Rebind<Up, BasicVector>;
                    UV::MaskedStore(UV(v), mem, typename UV::MaskType(k));
                }
                return;
            }
#endif
            if (k.NoneOf()) [[unlikely]] {
                return;
            } else if constexpr (kIsScalar) {
                mem[0] = v.data;
            } else {
                // Use at least 4-std::byte bits in BitForeach for better code-gen
                Bitmask < kStorageSize<32 ? 32 : kStorageSize> bits = k.ToUint();
                [[assume(bits != 0)]]; // because of 'k.NoneOf()' branch above
                if constexpr (ConvertsTrivially<ValueType, Up>) {
                    BitForeach(bits, [&] [[gnu::always_inline]] (int i) { mem[i] = v[i]; });
                } else {
                    const Rebind<Up, BasicVector> cvted(v);
                    BitForeach(bits, [&] [[gnu::always_inline]] (int i) { mem[i] = cvted[i]; });
                }
            }
        }

        // [simd.overview] default constructor ----------------------------------
        BasicVector() = default;

        // [simd.overview] p2 impl-def conversions ------------------------------
        using NativeVecType = decltype([] {
            if constexpr (kIsScalar) {
                return VecBuiltinType<CanonValueType, 1>();
            } else {
                return DataType();
            }
        }());
        /**
         * @brief Converting constructor from GCC vector builtins.
         *
         * This constructor enables direct construction from GCC vector builtins
         * (`[[gnu::vector_size(N)]]`).
         *
         * @param x GCC vector builtin to convert from.
         *
         * @note This constructor is not available when size() equals 1.
         *
         * @see operator NativeVecType() for the reverse conversion.
         */
        constexpr BasicVector(NativeVecType x)
            : data([&] [[gnu::always_inline]] {
                  if constexpr (kIsScalar) {
                      return x[0];
                  } else {
                      return x;
                  }
              }()) {}

        /**
         * @brief Conversion operator to GCC vector builtins.
         *
         * This operator enables implicit conversion from BasicVector to GCC vector builtins.
         *
         * @note This operator is not available when size() equals 1.
         *
         * @see BasicVector(NativeVecType) for the reverse conversion.
         */
        constexpr operator NativeVecType() const {
            if constexpr (kIsScalar) {
                return NativeVecType{data};
            } else {
                return data;
            }
        }

#if SORA_SIMD_X86
        /**
         * @brief Converting constructor from Intel Intrinsics (__m128, __m128i, ...).
         */
        template<VecBuiltin IV>
            requires std::same_as<X86IntelIntrinValueType<ValueType>, VecValueType<IV>> &&
                     (sizeof(IV) == sizeof(DataType) && sizeof(IV) >= 16 && !std::is_same_v<IV, DataType>)
        constexpr BasicVector(IV x) : data(reinterpret_cast<DataType>(x)) {}

        /**
         * @brief Conversion operator to Intel Intrinsics (__m128, __m128i, ...).
         */
        template<VecBuiltin IV>
            requires std::same_as<X86IntelIntrinValueType<ValueType>, VecValueType<IV>> &&
                     (sizeof(IV) == sizeof(DataType) && sizeof(IV) >= 16 && !std::is_same_v<IV, DataType>)
        constexpr operator IV() const {
            return reinterpret_cast<IV>(data);
        }
#endif

        // [simd.ctor] broadcast constructor ------------------------------------
        /**
         * @brief Broadcast constructor from scalar value.
         *
         * Constructs a vector where all elements are initialized to the same scalar value.
         * The scalar value is converted to the vector's element type.
         *
         * @param x Scalar value to broadcast to all vector elements.
         * @tparam Up Type of scalar value (must be explicitly convertible to ValueType).
         *
         * @note The constructor is implicit if the conversion (if any) is value-preserving.
         */
        template<BroadcastConstructible<ValueType> Up>
        [[gnu::always_inline]]
        constexpr BasicVector(Up&& x) noexcept
            : data(DataType() == DataType() ? static_cast<ValueType>(x) : ValueType()) {}

        // [simd.ctor] conversion constructor -----------------------------------
        template<typename Up, typename UAbi, TargetTraits Traits = {}>
            requires(kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType>
        [[gnu::always_inline]] constexpr explicit(!ValuePreservingConvertibleTo<Up, ValueType> ||
                                                  HigherRankThan<Up, ValueType>)
            BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : data([&] [[gnu::always_inline]] {
                  if constexpr (kIsScalar) {
                      return static_cast<ValueType>(x[0]);
                  } else if constexpr (UAbi::kNreg >= 2) {
                      // __builtin_convertvector (VecCast) is inefficient for over-sized inputs.
                      // Also e.g. Vector<float, 12> -> Vector<char, 12> (with SSE2) would otherwise emit 4
                      // vcvttps2dq instructions, where only 3 are needed
                      return Concat(Resize<x.N0, BasicVector>(x.data0), Resize<x.N1, BasicVector>(x.data1)).data;
                  } else {
                      return VecCast<DataType>(x.ConcatData());
                  }
              }()) {}

        using VecBase<Tp, Ap>::VecBase;

        // [simd.ctor] generator constructor ------------------------------------
        template<SimdGeneratorInvokable<ValueType, kStorageSize> Fp>
        [[gnu::always_inline]] constexpr explicit BasicVector(Fp&& gen)
            : data([&] [[gnu::always_inline]] {
                  const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                  return DataType{static_cast<ValueType>(gen(kSimdSizeC<indices>))...};
              }()) {}

        // [simd.ctor] load constructor -----------------------------------------
        template<typename Up>
        [[gnu::always_inline]]
        constexpr BasicVector(LoadCtorTag, const Up* ptr)
            : data() {
            if constexpr (kIsScalar) {
                data = static_cast<ValueType>(ptr[0]);
            } else if consteval {
                const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                data = DataType{static_cast<ValueType>(ptr[indices])...};
            } else {
                if constexpr (ConvertsTrivially<Up, ValueType>) {
                    // This assumes std::floatN_t to be bitwise equal to float/double
                    __builtin_memcpy(&data, ptr, sizeof(ValueType) * kStorageSize);
                } else {
                    VecBuiltinType<Up, kFullSize> tmp = {};
                    __builtin_memcpy(&tmp, ptr, sizeof(Up) * kStorageSize);
                    data = VecCast<DataType>(tmp);
                }
            }
        }

        template<std::ranges::contiguous_range Rg, typename... FlagTypes>
            requires Detail::StaticSizedRange<Rg> && Vectorizable<std::ranges::range_value_t<Rg>> &&
                     ExplicitlyConvertibleTo<std::ranges::range_value_t<Rg>, ValueType>
                     [[gnu::always_inline]]
                     constexpr BasicVector(Rg&& range, Flags<FlagTypes...> flags = {})
                         requires(std::ranges::size(range) == kStorageSize)
            : BasicVector(LoadCtorTag(), flags.template AdjustPointer<BasicVector>(std::ranges::data(range))) {
            static_assert(LoadstoreConvertibleTo<std::ranges::range_value_t<Rg>, ValueType, FlagTypes...>);
        }

        // [simd.subscr] --------------------------------------------------------
        /**
         * @brief Return the value of the element at index @p i.
         *
         * @pre i >= 0 && i < size().
         */
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            SORA_SIMD_PRECONDITION(i >= 0 && i < kStorageSize, "subscript is out of bounds");
            if constexpr (kIsScalar) {
                return data;
            } else {
                return data[i];
            }
        }

        // [simd.unary] unary operators -----------------------------------------
        // increment and decrement are implemented in terms of operator+=/-= which avoids UB on
        // padding elements while not breaking UBsan
        [[gnu::always_inline]]
        constexpr BasicVector& operator++() noexcept
            requires requires(ValueType a) { ++a; }
        {
            return *this += ValueType(1);
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator++(int) noexcept
            requires requires(ValueType a) { a++; }
        {
            BasicVector r = *this;
            *this += ValueType(1);
            return r;
        }

        [[gnu::always_inline]]
        constexpr BasicVector& operator--() noexcept
            requires requires(ValueType a) { --a; }
        {
            return *this -= ValueType(1);
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator--(int) noexcept
            requires requires(ValueType a) { a--; }
        {
            BasicVector r = *this;
            *this -= ValueType(1);
            return r;
        }

        [[gnu::always_inline]]
        constexpr MaskType operator!() const noexcept
            requires requires(ValueType a) { !a; }
        {
            return *this == ValueType();
        }

        /**
         * @brief Unary std::plus operator (no-op).
         *
         * Returns an unchanged copy of the object.
         */
        [[gnu::always_inline]]
        constexpr BasicVector operator+() const noexcept
            requires requires(ValueType a) { +a; }
        {
            return *this;
        }

        /**
         * @brief Unary negation operator.
         *
         * Returns a new SIMD vector after element-wise negation.
         */
        [[gnu::always_inline]]
        constexpr BasicVector operator-() const noexcept
            requires requires(ValueType a) { -a; }
        {
            return Init(-data);
        }

        /**
         * @brief Bitwise NOT / complement operator.
         *
         * Returns a new SIMD vector after element-wise complement.
         */
        [[gnu::always_inline]]
        constexpr BasicVector operator~() const noexcept
            requires requires(ValueType a) { ~a; }
        {
            return Init(~data);
        }

        // [simd.cassign] binary operators
        /**
         * @brief Bitwise AND operator.
         *
         * Returns a new SIMD vector after element-wise AND.
         */
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator&=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a & a; }
        {
            x.data &= y.data;
            return x;
        }

        /**
         * @brief Bitwise OR operator.
         *
         * Returns a new SIMD vector after element-wise OR.
         */
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator|=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a | a; }
        {
            x.data |= y.data;
            return x;
        }

        /**
         * @brief Bitwise XOR operator.
         *
         * Returns a new SIMD vector after element-wise XOR.
         */
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator^=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a ^ a; }
        {
            x.data ^= y.data;
            return x;
        }

        /**
         * @brief Applies the compound assignment operator element-wise.
         *
         * @pre If @c ValueType is a signed std::integral type, the result is representable by @c
         * ValueType. (This does not apply to padding elements the implementation might add for
         * non-power-of-2 widths.) UBsan will only see a call to @c unreachable() on overflow.
         *
         * @note The overflow detection code is discarded unless UBsan is active.
         */
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator+=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a + a; }
        {
            if constexpr (kIsPartial && std::is_integral_v<ValueType> &&
                          std::is_signed_v<ValueType>) { // avoid spurious UB on signed integer overflow of the padding
                                                         // element(s). But don't
                // remove UB of the active elements (so that UBsan can still do its job).
                //
                // This check is essentially free (at runtime) because DCE removes everything except
                // the final change to data. The overflow check is only emitted if UBsan is active.
                //
                // The alternative would be to always zero padding elements after operations that can
                // produce non-zero values. However, right now:
                // - auto f(Sora::Math::Simd::Mask<int, 3> k) { return +k; } is a single VPABSD and would have to
                //   sanitize
                // - std::bit_cast to BasicVector with non-zero padding elements is fine
                // - conversion from intrinsics can create non-zero padding elements
                // - shuffles are allowed to put whatever they want into padding elements for
                //   optimization purposes (e.g. for better instruction selection)
                using UV = typename Ap::template DataType<std::make_unsigned_t<ValueType>>;
                const DataType result =
                    reinterpret_cast<DataType>(reinterpret_cast<UV>(x.data) + reinterpret_cast<UV>(y.data));
                const auto positive = y > ValueType();
                const auto overflow = positive != (BasicVector(result) > x);
                if (overflow.AnyOf()) {
                    __builtin_unreachable(); // trigger UBsan
                }
                x.data = result;
            } else if constexpr (TargetTraits().EvalAsF32<ValueType>()) {
                x = BasicVector(Rebind<float, BasicVector>(x) + y);
            } else {
                x.data += y.data;
            }
            return x;
        }

        /** @copydoc operator+=
         */
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator-=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a - a; }
        {
            if constexpr (kIsPartial && std::is_integral_v<ValueType> &&
                          std::is_signed_v<ValueType>) { // see comment on operator+=
                using UV = typename Ap::template DataType<std::make_unsigned_t<ValueType>>;
                const DataType result =
                    reinterpret_cast<DataType>(reinterpret_cast<UV>(x.data) - reinterpret_cast<UV>(y.data));
                const auto positive = y > ValueType();
                const auto overflow = positive != (BasicVector(result) < x);
                if (overflow.AnyOf()) {
                    __builtin_unreachable(); // trigger UBsan
                }
                x.data = result;
            } else if constexpr (TargetTraits().EvalAsF32<ValueType>()) {
                x = BasicVector(Rebind<float, BasicVector>(x) - y);
            } else {
                x.data -= y.data;
            }
            return x;
        }

        /** @copydoc operator+=
         */
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator*=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a * a; }
        {
            if constexpr (kIsPartial && std::is_integral_v<ValueType> &&
                          std::is_signed_v<ValueType>) { // see comment on operator+=
                for (int i = 0; i < kStorageSize; ++i) {
                    if (__builtin_mul_overflow_p(x.data[i], y.data[i], ValueType())) {
                        __builtin_unreachable();
                    }
                }
                using UV = typename Ap::template DataType<std::make_unsigned_t<ValueType>>;
                x.data = reinterpret_cast<DataType>(reinterpret_cast<UV>(x.data) * reinterpret_cast<UV>(y.data));
            }

            // 'uint16 * uint16' promotes to int and can therefore lead to UB. The standard does not
            // require to avoid the undefined behavior. It's unnecessary and easy to avoid. It's also
            // unexpected because there's no UB on the vector types (which don't promote).
            else if constexpr (kIsScalar && std::is_unsigned_v<ValueType> &&
                               std::is_signed_v<decltype(ValueType() * ValueType())>) {
                x.data = unsigned(x.data) * unsigned(y.data);
            }

            else if constexpr (TargetTraits().EvalAsF32<ValueType>()) {
                x = BasicVector(Rebind<float, BasicVector>(x) * y);
            }

            else {
                x.data *= y.data;
            }
            return x;
        }

        template<TargetTraits Traits = {}>
        [[gnu::always_inline]] friend constexpr BasicVector& operator/=(BasicVector & x, const BasicVector & y) noexcept
            requires requires(ValueType a) { a / a; }
        {
            const BasicVector result([&](int i) -> ValueType { return x[i] / y[i]; });
            if (IsConstKnown(result)) {
                // the optimizer already knows the values of the result
                return x = result;
            }

#ifdef __SSE2__
            // x86 doesn't have std::integral SIMD division instructions
            // While division is faster, the required conversions are still a problem:
            // see PR121274, PR121284, and PR121296 for missed optimizations wrt. conversions
            //
            // With only 1 or 2 divisions, the conversion to and from fp is too expensive.
            if constexpr (std::is_integral_v<ValueType> && kStorageSize > 2 &&
                          ValuePreservingConvertibleTo<ValueType, double>) {
                // If the denominator (y) is known to the optimizer, don't convert to fp because the
                // std::integral division can be translated into shifts/multiplications.
                if (!IsConstKnown(y)) {
                    // With AVX512FP16 use vdivph for 8-bit integers
                    if constexpr (Traits.HaveAvx512fp16() && ValuePreservingConvertibleTo<ValueType, _Float16>) {
                        return x = BasicVector(Rebind<_Float16, BasicVector>(x) / y);
                    } else if constexpr (ValuePreservingConvertibleTo<ValueType, float>) {
                        return x = BasicVector(Rebind<float, BasicVector>(x) / y);
                    } else {
                        return x = BasicVector(Rebind<double, BasicVector>(x) / y);
                    }
                }
            }
#endif
            if constexpr (Traits.EvalAsF32<ValueType>()) {
                return x = BasicVector(Rebind<float, BasicVector>(x) / y);
            }

            BasicVector y1 = y;
            if constexpr (kIsPartial) {
                if constexpr (std::is_integral_v<ValueType>) {
                    // Assume std::integral division doesn't have SIMD instructions and must be done per
                    // element anyway. Partial vectors should skip their padding elements.
                    for (int i = 0; i < kStorageSize; ++i) {
                        x.data[i] /= y.data[i];
                    }
                    return x;
                } else {
                    y1 = SelectImpl(MaskType::Init(MaskType::kImplicitMask), y, BasicVector(ValueType(1)));
                }
            }
            x.data /= y1.data;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator%=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a % a; }
        {
            static_assert(std::is_integral_v<ValueType>);
            if constexpr (kIsPartial) {
                const BasicVector y1 =
                    SelectImpl(MaskType::Init(MaskType::kImplicitMask), y, BasicVector(ValueType(1)));
                if (IsConstKnown(y1)) {
                    x.data %= y1.data;
                } else {
                    // Assume std::integral division doesn't have SIMD instructions and must be done per
                    // element anyway. Partial vectors should skip their padding elements.
                    for (int i = 0; i < kStorageSize; ++i) {
                        x.data[i] %= y.data[i];
                    }
                }
            } else {
                x.data %= y.data;
            }
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator<<=(BasicVector& x, const BasicVector& y) SORA_SIMD_NOEXCEPT
            requires requires(ValueType a) { a << a; }
        {
            SORA_SIMD_PRECONDITION(std::is_unsigned_v<ValueType> || AllOf(y >= ValueType()),
                                   "negative shift is undefined behavior");
            SORA_SIMD_PRECONDITION(AllOf(y < kMaxShift<ValueType>), "too large shift invokes undefined behavior");
            x.data <<= y.data;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator>>=(BasicVector& x, const BasicVector& y) SORA_SIMD_NOEXCEPT
            requires requires(ValueType a) { a >> a; }
        {
            SORA_SIMD_PRECONDITION(std::is_unsigned_v<ValueType> || AllOf(y >= ValueType()),
                                   "negative shift is undefined behavior");
            SORA_SIMD_PRECONDITION(AllOf(y < kMaxShift<ValueType>), "too large shift invokes undefined behavior");
            x.data >>= y.data;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator<<=(BasicVector& x, SimdSizeType y) SORA_SIMD_NOEXCEPT
            requires requires(ValueType a, SimdSizeType b) { a << b; }
        {
            SORA_SIMD_PRECONDITION(y >= 0, "negative shift is undefined behavior");
            SORA_SIMD_PRECONDITION(y < int(kMaxShift<ValueType>), "too large shift invokes undefined behavior");
            x.data <<= y;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator>>=(BasicVector& x, SimdSizeType y) SORA_SIMD_NOEXCEPT
            requires requires(ValueType a, SimdSizeType b) { a >> b; }
        {
            SORA_SIMD_PRECONDITION(y >= 0, "negative shift is undefined behavior");
            SORA_SIMD_PRECONDITION(y < int(kMaxShift<ValueType>), "too large shift invokes undefined behavior");
            x.data >>= y;
            return x;
        }

        // [simd.comparison] ----------------------------------------------------
#if SORA_SIMD_X86
        template<X86Cmp Cmp>
        [[gnu::always_inline]]
        constexpr MaskType BitmaskCmp(DataType y) const {
            static_assert(kUseBitmask);
            if (IsConstKnown(data, y)) {
                const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                constexpr auto cmpOp = [] [[gnu::always_inline]]
                                       (ValueType a, ValueType b) {
                                           if constexpr (Cmp == X86Cmp::kEq) {
                                               return a == b;
                                           } else if constexpr (Cmp == X86Cmp::kLt) {
                                               return a < b;
                                           } else if constexpr (Cmp == X86Cmp::kLe) {
                                               return a <= b;
                                           } else if constexpr (Cmp == X86Cmp::kUnord) {
                                               return std::isunordered(a, b);
                                           } else if constexpr (Cmp == X86Cmp::kNeq) {
                                               return a != b;
                                           } else if constexpr (Cmp == X86Cmp::kNlt) {
                                               return !(a < b);
                                           } else if constexpr (Cmp == X86Cmp::kNle) {
                                               return !(a <= b);
                                           } else {
                                               static_assert(false);
                                           }
                                       };
                const Bitmask<kStorageSize> bits =
                    ((cmpOp(VecGet(data, indices), VecGet(y, indices)) ? (1ULL << indices) : 0) | ...);
                return MaskType::Init(bits);
            } else {
                return MaskType::Init(X86BitmaskCmp<Cmp>(data, y));
            }
        }
#endif

        [[gnu::always_inline]]
        friend constexpr MaskType operator==(const BasicVector& x, const BasicVector& y) noexcept {
#if SORA_SIMD_X86
            if constexpr (kUseBitmask) {
                return x.BitmaskCmp<X86Cmp::kEq>(y.data);
            } else
#endif
                return MaskType::Init(x.data == y.data);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator!=(const BasicVector& x, const BasicVector& y) noexcept {
#if SORA_SIMD_X86
            if constexpr (kUseBitmask) {
                return x.BitmaskCmp<X86Cmp::kNeq>(y.data);
            } else
#endif
                return MaskType::Init(x.data != y.data);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator<(const BasicVector& x, const BasicVector& y) noexcept {
#if SORA_SIMD_X86
            if constexpr (kUseBitmask) {
                return x.BitmaskCmp<X86Cmp::kLt>(y.data);
            } else
#endif
                return MaskType::Init(x.data < y.data);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator<=(const BasicVector& x, const BasicVector& y) noexcept {
#if SORA_SIMD_X86
            if constexpr (kUseBitmask) {
                return x.BitmaskCmp<X86Cmp::kLe>(y.data);
            } else
#endif
                return MaskType::Init(x.data <= y.data);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator>(const BasicVector& x, const BasicVector& y) noexcept {
            return y < x;
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator>=(const BasicVector& x, const BasicVector& y) noexcept {
            return y <= x;
        }

        // [simd.cond] ---------------------------------------------------------
        template<TargetTraits Traits = {}>
        [[gnu::always_inline]] friend constexpr BasicVector SelectImpl(const MaskType& k, const BasicVector& t,
                                                                       const BasicVector& f) noexcept {
            if constexpr (kStorageSize == 1) {
                return k[0] ? t : f;
            } else if constexpr (kUseBitmask) {
#if SORA_SIMD_X86
                if (IsConstKnown(k, t, f)) {
                    return BasicVector([&](int i) { return k[i] ? t[i] : f[i]; });
                } else {
                    return X86BitmaskBlend(k.data, t.data, f.data);
                }
#else
                static_assert(false, "TODO");
#endif
            } else if consteval {
                return k.data ? t.data : f.data;
            } else {
                constexpr bool usesSimdRegister = sizeof(data) >= 8;
                using VO = VecOps<DataType>;
                if (VO::AreElementsConstKnownEqualTo(f.data, 0)) {
                    if (std::is_integral_v<ValueType> && usesSimdRegister &&
                        VO::AreElementsConstKnownEqualTo(t.data, 1)) {
                        // This is equivalent to converting the Mask into a Vector of 0s and 1s. So +k.
                        // However, BasicMask::operator+ arrives here; returning +k would be
                        // recursive. Instead we use -k (which is a no-op for vector-masks) and then
                        // flip all -1 elements to +1 by taking the absolute value.
                        return BasicVector((-k).Abs());
                    } else {
                        return VecAnd(reinterpret_cast<DataType>(k.data), t.data);
                    }
                } else if (VecOps<DataType>::AreElementsConstKnownEqualTo(t.data, 0)) {
                    if (std::is_integral_v<ValueType> && usesSimdRegister &&
                        VO::AreElementsConstKnownEqualTo(f.data, 1)) {
                        return ValueType(1) + BasicVector(-k);
                    } else {
                        return VecAnd(reinterpret_cast<DataType>(VecNot(k.data)), f.data);
                    }
                } else {
#if SORA_SIMD_X86
                    // this works around bad code-gen when the compiler can't see that k is a vector-Mask.
                    // This pattern, is recognized to match the x86 blend instructions, which only consider
                    // the sign bit of the Mask register. Also, without SSE4, if the compiler knows that k
                    // is a vector-Mask, then the '< 0' is elided.
                    return k.data < 0 ? t.data : f.data;
#endif
                    return k.data ? t.data : f.data;
                }
            }
        }
    };

    template<Vectorizable Tp, AbiTag Ap>
        requires(Ap::kNreg > 1) && (!ComplexLike<Tp>)
    class BasicVector<Tp, Ap> : public VecBase<Tp, Ap> {
        template<typename, typename>
        friend class BasicVector;

        template<std::size_t, typename>
        friend class BasicMask;

        static constexpr int kStorageSize = Ap::kStorageSize;

        static constexpr int kN0 = std::bit_ceil(unsigned(kStorageSize)) / 2;

        static constexpr int kN1 = kStorageSize - kN0;

        using DataType0 = SimilarVec<Tp, kN0, Ap>;

        // the implementation (and users) depend on elements being contiguous in memory
        static_assert(kN0 * sizeof(Tp) == sizeof(DataType0));

        using DataType1 = SimilarVec<Tp, kN1, Ap>;

        static_assert(DataType0::AbiType::kNreg + DataType1::AbiType::kNreg == Ap::kNreg);

        static constexpr bool kIsScalar = DataType0::kIsScalar;

        DataType0 data0;

        DataType1 data1;

        static constexpr bool kUseBitmask = DataType0::kUseBitmask;

        static constexpr bool kIsPartial = DataType1::kIsPartial;

    public:
        using ValueType = Tp;

        using MaskType = VecBase<Tp, Ap>::MaskType;

        [[gnu::always_inline]]
        static constexpr BasicVector Init(const DataType0& x, const DataType1& y) {
            BasicVector r;
            r.data0 = x;
            r.data1 = y;
            return r;
        }

        [[gnu::always_inline]]
        constexpr DataType0& GetLow() noexcept {
            return data0;
        }

        [[gnu::always_inline]]
        constexpr const DataType0& GetLow() const noexcept {
            return data0;
        }

        [[gnu::always_inline]]
        constexpr DataType1& GetHigh() noexcept {
            return data1;
        }

        [[gnu::always_inline]]
        constexpr const DataType1& GetHigh() const noexcept {
            return data1;
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicVector& x) {
            return IsConstKnown(x.data0) && IsConstKnown(x.data1);
        }

        [[gnu::always_inline]]
        constexpr auto ConcatData([[maybe_unused]] bool doSanitize = false) const {
            return VecConcat(data0.ConcatData(false), VecZeroPadTo<sizeof(data0)>(data1.ConcatData(doSanitize)));
        }

        template<int Size = kStorageSize, int Offset = 0, typename A0, typename Fp>
        [[gnu::always_inline]]
        static constexpr BasicVector StaticPermute(const BasicVector<ValueType, A0>& x, Fp&& idxmap) {
            return Init(DataType0::template StaticPermute<Size, Offset>(x, idxmap),
                        DataType1::template StaticPermute<Size, Offset + kN0>(x, idxmap));
        }

        template<typename Vp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            constexpr int n = kStorageSize / Vp::kStorageSize;
            constexpr int rem = kStorageSize % Vp::kStorageSize;
            const auto& [... indices] = Detail::kIotaArray<n>;
            if constexpr (rem == 0) {
                return std::array<Vp, n>{
                    ExtractSimdAt<Vp>(Detail::kConstant<Vp::kStorageSize * indices>, data0, data1)...};
            } else {
                using Rest = Resize<rem, Vp>;
                return std::tuple(ExtractSimdAt<Vp>(Detail::kConstant<Vp::kStorageSize * indices>, data0, data1)...,
                                  ExtractSimdAt<Rest>(Detail::kConstant<Vp::kStorageSize * n>, data0, data1));
            }
        }

        [[gnu::always_inline]]
        static constexpr const BasicVector& Concat(const BasicVector& x0) noexcept {
            return x0;
        }

        // By Nekomiya to suppress warnings
        static constexpr const BasicVector& Concat(const BasicVector&& x0) noexcept = delete;
        static constexpr const BasicVector& Concat(BasicVector&& x0) noexcept = delete;

        template<typename... As>
            requires(sizeof...(As) >= 2)
        [[gnu::always_inline]]
        static constexpr BasicVector Concat(const BasicVector<ValueType, As>&... xs) noexcept {
            static_assert(kStorageSize == (As::kStorageSize + ...));
            return Init(ExtractSimdAt<DataType0>(Detail::kConstant<0>, xs...),
                        ExtractSimdAt<DataType1>(Detail::kConstant<kN0>, xs...));
        }

        [[gnu::always_inline]]
        constexpr auto ReduceToHalf(auto binaryOp) const
            requires(kN0 == kN1)
        {
            return binaryOp(data0, data1);
        }

        [[gnu::always_inline]]
        constexpr ValueType ReduceTail(const auto& rest, auto binaryOp) const {
            if constexpr (rest.kSize() > kStorageSize) {
                auto [a, b] = rest.template ChunkStorage<BasicVector>();
                return binaryOp(*this, a).ReduceTail(b, binaryOp);
            } else if constexpr (rest.kSize() == kStorageSize) {
                return binaryOp(*this, rest).Reduce(binaryOp);
            } else {
                return ReduceToHalf(binaryOp).ReduceTail(rest, binaryOp);
            }
        }

        template<typename BinaryOp, TargetTraits Traits = {}>
        [[gnu::always_inline]] constexpr ValueType Reduce(BinaryOp binaryOp) const {
            if constexpr (Traits.template EvalAsF32<ValueType>() &&
                          (std::is_same_v<BinaryOp, std::plus<>> || std::is_same_v<BinaryOp, std::multiplies<>>)) {
                return ValueType(Rebind<float, BasicVector>(*this).Reduce(binaryOp));
            }
#ifdef __SSE2__
            else if constexpr (std::is_integral_v<ValueType> && sizeof(ValueType) == 1 &&
                               std::is_same_v<decltype(binaryOp), std::multiplies<>>) {
                // convert to unsigned short because of missing 8-bit mul instruction
                // we don't need to preserve the order of elements
                //
                // The left columns under Latency and Throughput show bit-cast to ushort with shift by
                // 8. The right column uses the alternative in the else branch.
                // Benchmark on Intel Ultra 7 165U (AVX2)
                //   TYPE             Latency           Throughput
                //              [cycles/call]        [cycles/call]
                // schar, 64        59.9  70.7           10.5  13.3
                // schar, 128       81.4  97.2           12.2    21
                // schar, 256       92.4   129           17.2  35.2
                if constexpr (DataType1::kIsScalar) {
                    return binaryOp(DataType1(data0.Reduce(binaryOp)), data1)[0];
                }
                // TODO: optimize trailing scalar (e.g. (8+8)+(8+1))
                else if constexpr (kStorageSize % 2 ==
                                   0) { // If all elements participate in the reduction we can take this shortcut
                    using V16 = Resize<kStorageSize / 2, Rebind<unsigned short, BasicVector>>;
                    auto a = __builtin_bit_cast(V16, *this);
                    return binaryOp(a, a >> __CHAR_BIT__).Reduce(binaryOp);
                } else {
                    using V16 = Rebind<unsigned short, BasicVector>;
                    return V16(*this).Reduce(binaryOp);
                }
            }
#endif
            else {
                return data0.ReduceTail(data1, binaryOp);
            }
        }

        [[gnu::always_inline]]
        constexpr MaskType Isnan() const
            requires std::is_floating_point_v<ValueType>
        {
            return MaskType::Init(data0.Isnan(), data1.Isnan());
        }

        [[gnu::always_inline]]
        constexpr MaskType Isinf() const
            requires std::is_floating_point_v<ValueType>
        {
            return MaskType::Init(data0.Isinf(), data1.Isinf());
        }

        [[gnu::always_inline]]
        constexpr MaskType Isunordered(BasicVector y) const
            requires std::is_floating_point_v<ValueType>
        {
            return MaskType::Init(data0.Isunordered(y.data0), data1.Isunordered(y.data1));
        }

        [[gnu::always_inline]]
        constexpr BasicVector Abs() const
            requires std::signed_integral<ValueType>
        {
            return Init(data0.Abs(), data1.Abs());
        }

        [[gnu::always_inline]]
        constexpr BasicVector Fabs() const
            requires std::floating_point<ValueType>
        {
            return Init(data0.Fabs(), data1.Fabs());
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline BasicVector PartialLoad(const Up* mem, std::size_t n) {
            if (n >= kN0) {
                return Init(DataType0(LoadCtorTag(), mem), DataType1::PartialLoad(mem + kN0, n - kN0));
            } else {
                return Init(DataType0::PartialLoad(mem, n), DataType1());
            }
        }

        template<typename Up, ArchTraits Traits = {}>
        static inline BasicVector MaskedLoad(const Up* mem, MaskType k) {
            return Init(DataType0::MaskedLoad(mem, k.data0), DataType1::MaskedLoad(mem + kN0, k.data1));
        }

        template<typename Up>
        [[gnu::always_inline]]
        inline void Store(Up* mem) const {
            data0.Store(mem);
            data1.Store(mem + kN0);
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline void PartialStore(const BasicVector& v, Up* mem, std::size_t n) {
            if (n >= kN0) {
                v.data0.Store(mem);
                DataType1::PartialStore(v.data1, mem + kN0, n - kN0);
            } else {
                DataType0::PartialStore(v.data0, mem, n);
            }
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline void MaskedStore(const BasicVector& v, Up* mem, const MaskType& k) {
            DataType0::MaskedStore(v.data0, mem, k.data0);
            DataType1::MaskedStore(v.data1, mem + kN0, k.data1);
        }

        BasicVector() = default;

        // [simd.overview] p2 impl-def conversions ------------------------------
        using NativeVecType = VecBuiltinType<ValueType, std::bit_ceil(unsigned(kStorageSize))>;

        [[gnu::always_inline]]
        constexpr BasicVector(const NativeVecType& x)
            : data0(VecOps<VecBuiltinType<ValueType, kN0>>::Extract(x)),
              data1(VecOps<VecBuiltinType<ValueType, std::bit_ceil(unsigned(kN1))>>::Extract(
                  x, std::integral_constant<int, kN0>())) {}

        [[gnu::always_inline]]
        constexpr operator NativeVecType() const {
            return ConcatData();
        }

        // [simd.ctor] broadcast constructor ------------------------------------
        template<BroadcastConstructible<ValueType> Up>
        [[gnu::always_inline]]
        constexpr BasicVector(Up&& x) noexcept
            : data0(static_cast<ValueType>(x)), data1(static_cast<ValueType>(x)) {}

        // [simd.ctor] conversion constructor -----------------------------------
        template<typename Up, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType>
        [[gnu::always_inline]]
        constexpr explicit(!ValuePreservingConvertibleTo<Up, ValueType> || HigherRankThan<Up, ValueType>)
            BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : data0(std::get<0>(Chunk<kN0>(x))), data1(std::get<1>(Chunk<kN0>(x))) {}

        using VecBase<Tp, Ap>::VecBase;

        // [simd.ctor] generator constructor ------------------------------------
        template<SimdGeneratorInvokable<ValueType, kStorageSize> Fp>
        [[gnu::always_inline]] constexpr explicit BasicVector(Fp&& gen)
            : data0(gen), data1([&] [[gnu::always_inline]] (auto i) { return gen(kSimdSizeC<i + kN0>); }) {}

        // [simd.ctor] load constructor -----------------------------------------
        template<typename Up>
        [[gnu::always_inline]]
        constexpr BasicVector(LoadCtorTag, const Up* ptr)
            : data0(LoadCtorTag(), ptr), data1(LoadCtorTag(), ptr + kN0) {}

        template<std::ranges::contiguous_range Rg, typename... FlagTypes>
            requires Detail::StaticSizedRange<Rg> && Vectorizable<std::ranges::range_value_t<Rg>> &&
                     ExplicitlyConvertibleTo<std::ranges::range_value_t<Rg>, ValueType>
                     constexpr BasicVector(Rg&& range, Flags<FlagTypes...> flags = {})
                         requires(std::ranges::size(range) == kStorageSize)
            : BasicVector(LoadCtorTag(), flags.template AdjustPointer<BasicVector>(std::ranges::data(range))) {
            static_assert(LoadstoreConvertibleTo<std::ranges::range_value_t<Rg>, ValueType, FlagTypes...>);
        }

        // [simd.subscr] --------------------------------------------------------
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            SORA_SIMD_PRECONDITION(i >= 0 && i < kStorageSize, "subscript is out of bounds");
            if (IsConstKnown(i)) {
                return i < kN0 ? data0[i] : data1[i - kN0];
            } else {
                using AliasingT [[__gnu__::__may_alias__]] = ValueType;
                return reinterpret_cast<const AliasingT*>(this)[i];
            }
        }

        // [simd.unary] unary operators -----------------------------------------
        [[gnu::always_inline]]
        constexpr BasicVector& operator++() noexcept
            requires requires(ValueType a) { ++a; }
        {
            ++data0;
            ++data1;
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator++(int) noexcept
            requires requires(ValueType a) { a++; }
        {
            BasicVector r = *this;
            ++data0;
            ++data1;
            return r;
        }

        [[gnu::always_inline]]
        constexpr BasicVector& operator--() noexcept
            requires requires(ValueType a) { --a; }
        {
            --data0;
            --data1;
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator--(int) noexcept
            requires requires(ValueType a) { a--; }
        {
            BasicVector r = *this;
            --data0;
            --data1;
            return r;
        }

        [[gnu::always_inline]]
        constexpr MaskType operator!() const noexcept
            requires requires(ValueType a) { !a; }
        {
            return MaskType::Init(!data0, !data1);
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator+() const noexcept
            requires requires(ValueType a) { +a; }
        {
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator-() const noexcept
            requires requires(ValueType a) { -a; }
        {
            return Init(-data0, -data1);
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator~() const noexcept
            requires requires(ValueType a) { ~a; }
        {
            return Init(~data0, ~data1);
        }

        // [simd.cassign] -------------------------------------------------------
#define SORA_SIMD_DEFINE_OP(sym)                                                                                       \
    [[gnu::always_inline]]                                                                                             \
    friend constexpr BasicVector& operator sym## = (BasicVector & x, const BasicVector& y) SORA_SIMD_NOEXCEPT {        \
        x.data0 sym## = y.data0;                                                                                       \
        x.data1 sym## = y.data1;                                                                                       \
        return x;                                                                                                      \
    }

        SORA_SIMD_DEFINE_OP(+)
        SORA_SIMD_DEFINE_OP(-)
        SORA_SIMD_DEFINE_OP(*)
        SORA_SIMD_DEFINE_OP(/)
        SORA_SIMD_DEFINE_OP(%)
        SORA_SIMD_DEFINE_OP(&)
        SORA_SIMD_DEFINE_OP(|)
        SORA_SIMD_DEFINE_OP(^)
        SORA_SIMD_DEFINE_OP(<<)
        SORA_SIMD_DEFINE_OP(>>)

#undef SORA_SIMD_DEFINE_OP

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator<<=(BasicVector& x, SimdSizeType y) SORA_SIMD_NOEXCEPT
            requires requires(ValueType a, SimdSizeType b) { a << b; }
        {
            x.data0 <<= y;
            x.data1 <<= y;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator>>=(BasicVector& x, SimdSizeType y) SORA_SIMD_NOEXCEPT
            requires requires(ValueType a, SimdSizeType b) { a >> b; }
        {
            x.data0 >>= y;
            x.data1 >>= y;
            return x;
        }

        // [simd.comparison] ----------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr MaskType operator==(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::Init(x.data0 == y.data0, x.data1 == y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator!=(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::Init(x.data0 != y.data0, x.data1 != y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator<(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::Init(x.data0 < y.data0, x.data1 < y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator<=(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::Init(x.data0 <= y.data0, x.data1 <= y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator>(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::Init(x.data0 > y.data0, x.data1 > y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator>=(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::Init(x.data0 >= y.data0, x.data1 >= y.data1);
        }

        // [simd.cond] ---------------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicVector SelectImpl(const MaskType& k, const BasicVector& t,
                                                const BasicVector& f) noexcept {
            return Init(SelectImpl(k.data0, t.data0, f.data0), SelectImpl(k.data1, t.data1, f.data1));
        }
    };

    // [simd.overview] deduction guide ------------------------------------------
    template<std::ranges::contiguous_range Rg, typename... Ts>
        requires Detail::StaticSizedRange<Rg>
    BasicVector(Rg&& r, Ts...) -> BasicVector<std::ranges::range_value_t<Rg>,
                                              DeduceAbiT<std::ranges::range_value_t<Rg>,
#if 0 // PR117849
				static_cast<SimdSizeType>(std::ranges::size(r))>>;
#else
                                                         static_cast<SimdSizeType>(decltype(std::span(r))::extent)>>;
#endif

    template<std::size_t Bytes, typename Ap>
    BasicVector(BasicMask<Bytes, Ap>)
        -> BasicVector<Detail::IntegerForSize<Bytes>,
                       decltype(AbiRebind<Detail::IntegerForSize<Bytes>, BasicMask<Bytes, Ap>::kSize.value, Ap>())>;

    // [P3319R5] ----------------------------------------------------------------
    template<Vectorizable Tp>
        requires std::is_arithmetic_v<Tp>
    inline constexpr Tp kIota<Tp> = Tp();

    template<typename Tp, typename Ap>
    inline constexpr BasicVector<Tp, Ap> kIota<BasicVector<Tp, Ap>> = BasicVector<Tp, Ap>([](Tp i) -> Tp {
        static_assert(Ap::kStorageSize - 1 <= std::numeric_limits<Tp>::max(), "iota object would overflow");
        return i;
    });

} // namespace Sora::Math::Simd
