/**
 * @file Mask.h
 * @brief SIMD mask types, conversions, and structural operations.
 * @ingroup Math
 */
#pragma once

#include "Iterator.h"
#include "VectorOperations.h"
#if SORA_SIMD_X86
#    include "X86.h"
#endif

#include <bit>
#include <bitset>

// psabi warnings are bogus because the ABI of the internal types never leaks into user code
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

namespace Sora::Math::Simd {

    template<unsigned Np>
    struct SwapNeighbors {
        consteval unsigned operator()(unsigned i, unsigned size) const {
            if (size % (2 * Np) != 0) {
                __builtin_abort(); // SwapNeighbors<N> permutation requires a multiple of 2N elements
            } else if (std::has_single_bit(Np)) {
                return i ^ Np;
            } else if (i % (2 * Np) >= Np) {
                return i - Np;
            } else {
                return i + Np;
            }
        }
    };

    template<std::size_t Np, std::size_t Mp>
    constexpr auto BitsetSplit(const std::bitset<Mp>& b) {
        constexpr auto bitsPerWord = __CHAR_BIT__ * __SIZEOF_LONG__;
        if constexpr (Np % bitsPerWord == 0) {
            struct Tmp {
                std::bitset<Np> lo;
                std::bitset<Mp - Np> hi;
            };
            return __builtin_bit_cast(Tmp, b);
        } else {
            constexpr auto bitsPerUllong = __CHAR_BIT__ * __SIZEOF_LONG_LONG__;
            static_assert(Mp <= bitsPerUllong);
            using Lo = Bitmask<Np>;
            using Hi = Bitmask<Mp - Np>;
            struct Tmp {
                Lo lo;
                Hi hi;
            };
            return Tmp{static_cast<Lo>(b.ToUllong()), static_cast<Hi>(b.ToUllong() >> Np)};
        }
    }

    static_assert(BitsetSplit<64>(std::bitset<128>(1)).lo == std::bitset<64>(1));
    static_assert(BitsetSplit<64>(std::bitset<128>(1)).hi == std::bitset<64>(0));

    // [simd.traits]
    // --- RebindTraits ---
    template<typename Tp, typename Vp, ArchTraits Traits = {}>
    struct RebindTraits {};

    /**
     * Computes a member @c type `BasicVector<Tp, Abi>`, where @c Abi is chosen such that the
     * number of elements is equal to `Vp::kSize()` and features of the ABI tag (such as the
     * internal representation of masks, or storage order of std::complex components) are preserved.
     */
    template<Vectorizable Tp, SimdVecType Vp, ArchTraits Traits>
    // requires requires { typename DeduceAbiT<Tp, Vp::kSize()>; }
    struct RebindTraits<Tp, Vp, Traits> {
        using Type = SimilarVec<Tp, Vp::kSize(), typename Vp::AbiType>;
    };

    /**
     * As above, except for @c BasicMask.
     */
    template<Vectorizable Tp, SimdMaskType Mp, ArchTraits Traits>
    // requires requires { typename DeduceAbiT<Tp, Mp::kSize()>; }
    struct RebindTraits<Tp, Mp, Traits> {
        using Type = SimilarMask<Tp, Mp::kSize(), typename Mp::AbiType>;
    };

    template<typename Tp, typename Vp>
    using Rebind = typename RebindTraits<Tp, Vp>::Type;

    // --- ResizeTraits ---
    template<SimdSizeType Np, typename Vp, ArchTraits Traits = {}>
    struct ResizeTraits {};

    template<SimdSizeType Np, SimdVecType Vp, ArchTraits Traits>
        requires(Np >= 1)
    // requires requires { typename DeduceAbiT<typename Vp::ValueType, Np>; }
    struct ResizeTraits<Np, Vp, Traits> {
        using Type = SimilarVec<typename Vp::ValueType, Np, typename Vp::AbiType>;
    };

    template<SimdSizeType Np, SimdMaskType Mp, ArchTraits Traits>
        requires(Np >= 1)
    // requires requires { typename DeduceAbiT<typename Mp::ValueType, Np>; }
    struct ResizeTraits<Np, Mp, Traits> {
        using A1 = decltype(AbiRebind<kMaskElementSize<Mp>, Np, typename Mp::AbiType, true>());

        static_assert(AbiTag<A1>);

        static_assert(Mp::AbiType::kVariant == A1::kVariant || ScalarAbiTag<A1> || ScalarAbiTag<typename Mp::AbiType>);

        using Type = BasicMask<kMaskElementSize<Mp>, A1>;
    };

    template<SimdSizeType Np, typename Vp>
    using Resize = typename ResizeTraits<Np, Vp>::Type;

    // [simd.syn]
    inline constexpr SimdSizeType kZeroElement = std::numeric_limits<int>::min();

    inline constexpr SimdSizeType kUninitElement = kZeroElement + 1;

    // [simd.Permute.static]
    template<SimdSizeType Np = 0, SimdVecOrMaskType Vp, IndexPermutationFunction<Vp> IdxMap>
    [[gnu::always_inline]]
    constexpr Resize<Np == 0 ? Vp::kSize() : Np, Vp> Permute(const Vp& v, IdxMap&& idxmap) {
        return Resize < Np == 0 ? Vp::kSize() : Np, Vp > ::StaticPermute(v, idxmap);
    }

    // [simd.Permute.dynamic]
    template<SimdVecOrMaskType Vp, SimdIntegral Ip>
    [[gnu::always_inline]]
    constexpr Resize<Ip::kSize(), Vp> Permute(const Vp& v, const Ip& indices) {
        return v[indices];
    }

    // [simd.creation] ----------------------------------------------------------
    template<SimdVecType Vp, typename Ap>
    [[gnu::always_inline]]
    constexpr auto Chunk(const BasicVector<typename Vp::ValueType, Ap>& x) noexcept {
        return x.template ChunkStorage<Vp>();
    }

    template<SimdMaskType Mp, typename Ap>
    [[gnu::always_inline]]
    constexpr auto Chunk(const BasicMask<kMaskElementSize<Mp>, Ap>& x) noexcept {
        return x.template ChunkStorage<Mp>();
    }

    template<SimdSizeType Np, typename Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr auto Chunk(const BasicVector<Tp, Ap>& x) noexcept -> decltype(Chunk<Resize<Np, BasicVector<Tp, Ap>>>(x)) {
        return Chunk<Resize<Np, BasicVector<Tp, Ap>>>(x);
    }

    template<SimdSizeType Np, std::size_t Bytes, typename Ap>
    [[gnu::always_inline]]
    constexpr auto Chunk(const BasicMask<Bytes, Ap>& x) noexcept
        -> decltype(Chunk<Resize<Np, BasicMask<Bytes, Ap>>>(x)) {
        return Chunk<Resize<Np, BasicMask<Bytes, Ap>>>(x);
    }

    // LWG???? (reported 2025-11-25)
    template<typename Tp, typename A0, typename... Abis>
    constexpr Resize<(A0::kStorageSize + ... + Abis::kStorageSize), BasicVector<Tp, A0>>
    Cat(const BasicVector<Tp, A0>& x0, const BasicVector<Tp, Abis>&... xs) noexcept {
        return Resize<(A0::kStorageSize + ... + Abis::kStorageSize), BasicVector<Tp, A0>>::Concat(x0, xs...);
    }

    // LWG???? (reported 2025-11-25)
    template<std::size_t Bytes, typename A0, typename... Abis>
    constexpr Resize<(A0::kStorageSize + ... + Abis::kStorageSize), BasicMask<Bytes, A0>>
    Cat(const BasicMask<Bytes, A0>& x0, const BasicMask<Bytes, Abis>&... xs) noexcept {
        return Resize<(A0::kStorageSize + ... + Abis::kStorageSize), BasicMask<Bytes, A0>>::Concat(x0, xs...);
    }

    // implementation helper for Chunk and Cat
    consteval int PacksToSkipAtFront(int offset, std::initializer_list<int> sizes) {
        int i = 0;
        int n = 0;
        for (int s : sizes) {
            n += s;
            if (n > offset) {
                return i;
            }
            ++i;
        }
        __builtin_trap(); // called out of contract
    }

    consteval int PacksToSkipAtBack(int offset, int maxValue, std::initializer_list<int> sizes) {
        int i = 0;
        int n = -offset;
        for (int s : sizes) {
            ++i;
            n += s;
            if (n >= maxValue) {
                return int(sizes.size()) - i;
            }
        }
        return 0;
    }

    // in principle, this overload allows conversions to Dst - and it wouldn't be wrong - but the
    // general overload below is still a better candidate in overload resolution
    template<typename Dst>
    [[gnu::always_inline]]
    constexpr Dst ExtractSimdAt(auto offset, const Dst& r, const auto&...)
        requires(offset.value == 0)
    {
        return r;
    }

    template<typename Dst, typename V0>
    [[gnu::always_inline]]
    constexpr Dst ExtractSimdAt(auto offset, const V0&, const Dst& r, const auto&...)
        requires(offset.value == V0::kSize.value)
    {
        return r;
    }

    template<typename Dst, typename... Vs>
    [[gnu::always_inline]]
    constexpr Dst ExtractSimdAt(auto offset, const Vs&... xs) {
        using Adst = typename Dst::AbiType;
        if constexpr (Adst::kNreg >= 2) {
            using Dst0 = std::remove_cvref_t<decltype(std::declval<Dst>().GetLow())>;
            using Dst1 = std::remove_cvref_t<decltype(std::declval<Dst>().GetHigh())>;
            return Dst::Init(ExtractSimdAt<Dst0>(offset, xs...), ExtractSimdAt<Dst1>(offset + Dst0::kSize, xs...));
        } else {
            using Ret = std::remove_cvref_t<decltype(std::declval<Dst>().Get())>;
            constexpr bool useBitmask = SimdMaskType<Dst> && Adst::kIsBitmask;
            constexpr int dstFullSize = std::bit_ceil(unsigned(Adst::kStorageSize));
            constexpr int nargs = sizeof...(xs);
            using Afirst = typename Vs...[0] ::AbiType;
            using Alast = typename Vs...[nargs - 1] ::AbiType;
            const auto& x0 = xs...[0];
            const auto& xlast = xs...[nargs - 1];
            constexpr int ninputs = (Vs::kSize.value + ...);
            if constexpr (offset.value >= Afirst::kStorageSize ||
                          ninputs - offset.value - Alast::kStorageSize >=
                              Adst::kStorageSize) { // can drop inputs at the front and/or back of the pack
                constexpr int skipFront = PacksToSkipAtFront(offset.value, {Vs::kSize.value...});
                constexpr int skipBack = PacksToSkipAtBack(offset.value, Adst::kStorageSize, {Vs::kSize.value...});
                static_assert(skipFront > 0 || skipBack > 0);
                const auto& [... skippedIndices] = Detail::kIotaArray<skipFront>;
                const auto& [... indices] = Detail::kIotaArray<nargs - skipFront - skipBack>;
                constexpr int newOffset = offset.value - (0 + ... + Vs...[skippedIndices] ::size.value);
                return ExtractSimdAt<Dst>(Detail::kConstant<newOffset>, xs...[indices + skipFront]...);
            } else if constexpr (Adst::kStorageSize == 1) { // trivial conversion to one ValueType
                return Dst(x0[offset.value]);
            } else if constexpr (Afirst::kNreg >= 2 ||
                                 Alast::kNreg >= 2) { // flatten first and/or last multi-register argument
                constexpr bool flattenFirst = Afirst::kNreg >= 2;
                constexpr bool flattenLast = nargs > 1 && Alast::kNreg >= 2;
                const auto& [... indices] = Detail::kIotaArray<nargs - flattenFirst - flattenLast>;
                if constexpr (flattenFirst && flattenLast) {
                    return ExtractSimdAt<Dst>(offset, x0.GetLow(), x0.GetHigh(), xs...[indices + 1]..., xlast.GetLow(),
                                              xlast.GetHigh());
                } else if constexpr (flattenFirst) {
                    return ExtractSimdAt<Dst>(offset, x0.GetLow(), x0.GetHigh(), xs...[indices + 1]...);
                } else {
                    return ExtractSimdAt<Dst>(offset, xs...[indices]..., xlast.GetLow(), xlast.GetHigh());
                }
            } else if constexpr (SimdMaskType<Dst> &&
                                 ((Adst::kVariant != Vs::AbiType::kVariant && !ScalarAbiTag<typename Vs::AbiType>) ||
                                  ...)) { // convert ABI tag if incompatible
                return ExtractSimdAt<Dst>(offset, static_cast<const Resize<Vs::kSize.value, Dst>&>(xs)...);
            }

            // at this point xs should be as small as possible; there may be some corner cases left

            else if constexpr (nargs == 1) { // simple and optimal
                if constexpr (useBitmask) {
                    return Dst(Ret(x0.ToUint() >> offset.value));
                } else {
                    return VecOps<Ret>::Extract(x0.ConcatData(false), offset);
                }
            } else if constexpr (useBitmask) { // fairly simple and optimal bit shifting solution
                static_assert(Afirst::kNreg == 1);
                static_assert(offset.value < Afirst::kStorageSize);
                int runningOffset = -offset.value;
                Ret r;
                template for (const auto& x : {xs...}) {
                    if (runningOffset <= 0) {
                        r = Ret(x.ToUint() >> -runningOffset);
                    } else if (runningOffset < Adst::kStorageSize) {
                        r |= Ret(Ret(x.ToUint()) << runningOffset);
                    }
                    runningOffset += x.size.value;
                }
                return Dst(r);
            } else if constexpr (nargs == 2 && offset == 0 && Adst::kNreg == 1 &&
                                 Afirst::kStorageSize >= Alast::kStorageSize &&
                                 std::has_single_bit(unsigned(Afirst::kStorageSize))) { // simple VecConcat
                if constexpr (Afirst::kStorageSize == 1) {
                    // even simpler Init from two values
                    return Ret{x0.ConcatData()[0], xlast.ConcatData()[0]};
                } else {
                    const auto v0 = x0.ConcatData();
                    const auto v1 = VecZeroPadTo<sizeof(v0)>(xlast.ConcatData());
                    return VecConcat(v0, v1);
                }
            } else if constexpr (nargs == 2 && Adst::kNreg == 1 && offset == 0 && Afirst::kNreg == 1 &&
                                 Alast::kStorageSize == 1) { // optimize insertion of one element at the End
                Ret r = VecZeroPadTo<sizeof(Ret)>(x0.Get());
                VecSet(r, Afirst::kStorageSize, xlast.ConcatData()[0]);
                return r;
            } else if constexpr (nargs == 2 && Adst::kNreg == 1 && offset == 0 && Afirst::kNreg == 1 &&
                                 Alast::kStorageSize == 2) { // optimize insertion of two elements at the End
                Ret r = VecZeroPadTo<sizeof(Ret)>(x0.ConcatData());
                const auto x1 = xlast.ConcatData();
                if constexpr (sizeof(x1) <= sizeof(double) &&
                              (Afirst::kStorageSize & 1) == 0) { // can use a single insert instruction
                    using Up = std::conditional_t<std::is_floating_point_v<VecValueType<Ret>>,
                                                  std::conditional_t<sizeof(x1) == sizeof(double), double, float>,
                                                  Detail::IntegerForSize<sizeof(x1)>>;
                    auto r2 = VecBitCast<Up>(r);
                    VecSet(r2, Afirst::kStorageSize / 2, VecBitCast<Up>(x1)[0]);
                    r = reinterpret_cast<Ret>(r2);
                } else {
                    VecSet(r, Afirst::kStorageSize, x1[0]);
                    VecSet(r, Afirst::kStorageSize + 1, x1[1]);
                }
                return r;
            } else if constexpr (nargs == 2 && Afirst::kNreg == 1 &&
                                 Alast::kNreg == 1) { // optimize Concat of two input vectors (e.g. using palignr)
                const auto& [... indices] = Detail::kIotaArray<dstFullSize>;
                constexpr int v2Offset = kWidthOf<decltype(x0.ConcatData())>;
                return __builtin_shufflevector(x0.ConcatData(), xlast.ConcatData(), [](int i) consteval {
                    if (i < Afirst::kStorageSize) {
                        return i;
                    }
                    i -= Afirst::kStorageSize;
                    if (i < Alast::kStorageSize) {
                        return i + v2Offset;
                    } else {
                        return -1;
                    }
                }(indices + offset.value)...);
            } else if (IsConstKnown(xs...) || ninputs == Adst::kStorageSize) { // hard to optimize for the compiler, but
                                                                               // necessary in constant expressions
                return VecOps<Ret>::Extract(VecConcatSized<xs.size.value...>(xs.ConcatData(false)...), offset);
            } else { // fallback to concatenation in memory => load the result
                alignas(Ret) VecValueType<Ret> tmp[std::max(ninputs, offset.value + dstFullSize)] = {};
                int runningOffset = 0;
                template for (const auto& x : {xs...}) {
                    if constexpr (SimdMaskType<Dst>) {
                        (-x).Store(tmp + runningOffset);
                    } else {
                        x.Store(tmp + runningOffset);
                    }
                    runningOffset += x.size.value;
                }
                Ret r;
                __builtin_memcpy(&r, tmp + offset.value, sizeof(Ret));
                return r;
            }
        }
    }

    // [simd.Mask] --------------------------------------------------------------
    template<std::size_t Bytes, typename Ap>
    class BasicMask {
    public:
        using ValueType = bool;

        using AbiType = Ap;

#define SORA_SIMD_DELETE                                                                                               \
    "This specialization is disabled because of an invalid combination "                                               \
    "of template arguments to BasicMask."

        BasicMask() = delete (SORA_SIMD_DELETE);

        ~BasicMask() = delete (SORA_SIMD_DELETE);

        BasicMask(const BasicMask&) = delete (SORA_SIMD_DELETE);

        BasicMask& operator=(const BasicMask&) = delete (SORA_SIMD_DELETE);

#undef SORA_SIMD_DELETE
    };

    template<std::size_t Bytes, typename Ap>
    class MaskBase {
        using Mp = BasicMask<Bytes, Ap>;

    protected:
        using VecType = SimdVecFromMaskT<Bytes, Ap>;

        static_assert(std::destructible<VecType> || Bytes > sizeof(0ull));

    public:
        using IteratorType = Iterator<Mp>;

        using ConstIteratorType = Iterator<const Mp>;

        constexpr IteratorType Begin() noexcept { return {static_cast<Mp&>(*this), 0}; }

        constexpr ConstIteratorType Begin() const noexcept { return Cbegin(); }

        constexpr ConstIteratorType Cbegin() const noexcept { return {static_cast<const Mp&>(*this), 0}; }

        constexpr std::default_sentinel_t End() const noexcept { return {}; }

        constexpr std::default_sentinel_t Cend() const noexcept { return {}; }

        static constexpr auto kSize = kSimdSizeC<Ap::kStorageSize>;

        MaskBase() = default;

        // LWG issue from 2026-03-04 / P4042R0
        template<std::size_t UBytes, typename UAbi>
            requires(Ap::kStorageSize != UAbi::kStorageSize)
        explicit MaskBase(const BasicMask<UBytes, UAbi>&) = delete ("size mismatch");

        template<typename Up, typename UAbi>
        explicit MaskBase(const BasicVector<Up, UAbi>&) =
            delete ("use operator! or a comparison to convert a Vector into a Mask");

        template<typename Up, typename UAbi>
            requires(Ap::kStorageSize != UAbi::kStorageSize)
        operator BasicVector<Up, UAbi>() const = delete ("size mismatch");
    };

    template<std::size_t Bytes, AbiTag Ap>
        requires(Ap::kNreg == 1) && (FilterAbiVariant(Ap::kVariant, AbiVariant::kCxVariants) == AbiVariant() ||
                                     Ap::kStorageSize == 1) // Abi<1, 1, CxIleav> and Abi<1, 1, CxCtgus> go here
    class BasicMask<Bytes, Ap> : public MaskBase<Bytes, Ap> {
        using Base = MaskBase<Bytes, Ap>;

        using VecType = Base::VecType;

        template<std::size_t, typename>
        friend class BasicMask;

        template<typename, typename>
        friend class BasicVector;

        static constexpr int kStorageSize = Ap::kStorageSize;

        using DataType = typename Ap::template MaskDataType<Bytes>;

        static constexpr bool kHasBoolMember = std::is_same_v<DataType, bool>;

        static constexpr bool kIsScalar = kHasBoolMember;

        static constexpr bool kUseBitmask = Ap::kIsBitmask && !kIsScalar;

        static constexpr int kFullSize = [] {
            if constexpr (kIsScalar) {
                return kStorageSize;
            } else if constexpr (kUseBitmask && kStorageSize < __CHAR_BIT__) {
                return __CHAR_BIT__;
            } else {
                return std::bit_ceil(unsigned(kStorageSize));
            }
        }();

        static constexpr bool kIsPartial = kStorageSize != kFullSize;

        static constexpr DataType kImplicitMask = [] {
            if constexpr (kIsScalar) {
                return true;
            } else if (!kIsPartial) {
                return DataType(~DataType());
            } else if constexpr (kUseBitmask) {
                return DataType((DataType(1) << kStorageSize) - 1);
            } else {
                const auto& [... indices] = Detail::kIotaArray<kFullSize>;
                return DataType{(indices < kStorageSize ? -1 : 0)...};
            }
        }();

        // Actual padding bytes, not padding elements.
        // => kPaddingBytes is 0 even if kIsPartial is true.
        static constexpr std::size_t kPaddingBytes = 0;

        DataType data;

    public:
        using ValueType = bool;

        using AbiType = Ap;

        using IteratorType = Base::IteratorType;

        using ConstIteratorType = Base::ConstIteratorType;

        // internal but public API ----------------------------------------------
        [[gnu::always_inline]]
        static constexpr BasicMask Init(DataType x) {
            BasicMask r;
            r.data = x;
            return r;
        }

        [[gnu::always_inline]]
        static constexpr BasicMask Init(std::unsigned_integral auto bits) {
            return BasicMask(bits);
        }

        [[gnu::always_inline]]
        constexpr const DataType& Get() const {
            return data;
        }

        /** @internal
         * @brief Converts the type of the Mask without changing the data member.
         *
         * Since Abi<1, 1, CxCtgus> uses this partial specialization of BasicMask, the data
         * member cannot be used as Mask that matches the BasicVector elements.
         */
        [[gnu::always_inline]]
        constexpr auto GetIleavData() const noexcept
            requires Ap::kIsCxIleav
        {
            return ComponentMaskForIleav<Bytes, Ap>(data);
        }

        /** @internal
         * @brief Converts the type of the Mask from a scalar (bool) into a Mask of 2 elements.
         *
         * Since Abi<1, 1, CxIleav> uses this partial specialization of BasicMask, the data
         * member cannot be used as Mask that matches the BasicVector elements.
         */
        [[gnu::always_inline]]
        constexpr auto GetCtgusData() const noexcept
            requires Ap::kIsCxCtgus
        {
            return ComponentMaskForCtgus<Bytes, Ap>(data);
        }

        /** @internal
         * Bit-cast the given object @p x to BasicMask.
         *
         * This is necessary for kNreg > 1 where the last element can be bool or when the sizeof
         * doesn't match because of different Alignment requirements of the sub-masks.
         */
        template<std::size_t UBytes, typename UAbi>
        [[gnu::always_inline]]
        static constexpr BasicMask RecursiveBitCast(const BasicMask<UBytes, UAbi>& x) {
            return __builtin_bit_cast(BasicMask, x.ConcatData());
        }

        [[gnu::always_inline]]
        constexpr auto ConcatData(bool doSanitize = kIsPartial) const {
            if constexpr (kIsScalar) {
                return VecBuiltinType<Detail::IntegerForSize<Bytes>, 1>{Detail::IntegerForSize<Bytes>(-data)};
            } else {
                if constexpr (kIsPartial) {
                    if (doSanitize) {
                        return DataType(data & kImplicitMask);
                    }
                }
                return data;
            }
        }

        /** @internal
         * Returns a Mask where the first @p n elements are true. All remaining elements are false.
         *
         * @pre @p n > 0 && @p n < kStorageSize
         */
        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] static constexpr BasicMask PartialMaskOfN(int n) {
            static_assert(!kIsScalar);
            if constexpr (!kUseBitmask) {
                using Ip = Detail::IntegerForSize<Bytes>;
                SORA_SIMD_PRECONDITION(n >= 0 && n <= std::numeric_limits<Ip>::max(),
                                       "PartialMaskOfN without kUseBitmask requires "
                                       "positive n that does not overflow.");
                constexpr DataType sequentialIndices = __builtin_bit_cast(DataType, Detail::kIotaArray<Ip(kFullSize)>);
                return BasicMask(sequentialIndices < Ip(n));
            } else {
                SORA_SIMD_PRECONDITION(n >= 0 && n <= 255, "The x86 BZHI instruction requires n to "
                                                           "only use bits 0:7");
#if __has_builtin(__builtin_ia32_bzhi_si)
                if constexpr (kStorageSize <= 32 && Traits.HaveBmi2()) {
                    return Init(Bitmask<kStorageSize>(__builtin_ia32_bzhi_si(~0u >> (32 - kStorageSize), unsigned(n))));
                }
#endif
#if __has_builtin(__builtin_ia32_bzhi_di)
                else if constexpr (kStorageSize <= 64 && Traits.HaveBmi2()) {
                    return Init(__builtin_ia32_bzhi_di(~0ull >> (64 - kStorageSize), unsigned(n)));
                }
#endif
                if constexpr (kStorageSize <= 32) {
                    SORA_SIMD_PRECONDITION(n < 32, "invalid shift");
                    return Init(Bitmask<kStorageSize>((1u << unsigned(n)) - 1));
                } else if constexpr (kStorageSize <= 64) {
                    SORA_SIMD_PRECONDITION(n < 64, "invalid shift");
                    return Init((1ull << unsigned(n)) - 1);
                } else {
                    static_assert(false);
                }
            }
        }

        [[gnu::always_inline]]
        constexpr BasicMask& AndNeighbors() {
            if constexpr (kUseBitmask) {
                data &= ((data >> 1) & 0x5555'5555'5555'5555ull) | ((data << 1) & ~0x5555'5555'5555'5555ull);
            } else {
                data &= VecOps<DataType>::SwapNeighbors(data);
            }
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicMask& OrNeighbors() {
            if constexpr (kUseBitmask) {
                data |= ((data >> 1) & 0x5555'5555'5555'5555ull) | ((data << 1) & ~0x5555'5555'5555'5555ull);
            } else {
                data |= VecOps<DataType>::SwapNeighbors(data);
            }
            return *this;
        }

        template<typename Mp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            constexpr int n = kStorageSize / Mp::kStorageSize;
            constexpr int rem = kStorageSize % Mp::kStorageSize;
            const auto& [... indices] = Detail::kIotaArray<n>;
            if constexpr (rem == 0) {
                return std::array<Mp, n>{ExtractSimdAt<Mp>(Detail::kConstant<Mp::kStorageSize * indices>, *this)...};
            } else {
                using Rest = Resize<rem, Mp>;
                return std::tuple(ExtractSimdAt<Mp>(Detail::kConstant<Mp::kStorageSize * indices>, *this)...,
                                  ExtractSimdAt<Rest>(Detail::kConstant<Mp::kStorageSize * n>, *this));
            }
        }

        [[gnu::always_inline]]
        static constexpr const BasicMask& Concat(const BasicMask& x0) noexcept {
            return x0;
        }

        template<typename... As>
            requires(sizeof...(As) > 1)
        [[gnu::always_inline]]
        static constexpr BasicMask Concat(const BasicMask<Bytes, As>&... xs) noexcept {
            static_assert(kStorageSize == (As::kStorageSize + ...));
            return ExtractSimdAt<BasicMask>(Detail::kConstant<0>, xs...);
        }

        // [simd.Mask.overview] default constructor -----------------------------
        BasicMask() = default;

        // [simd.Mask.overview] conversion extensions ---------------------------
        [[gnu::always_inline]]
        constexpr BasicMask(DataType x)
            requires(!kIsScalar && !kUseBitmask)
            : data(x) {}

        [[gnu::always_inline]]
        constexpr operator DataType()
            requires(!kIsScalar && !kUseBitmask)
        {
            return data;
        }

        // [simd.Mask.ctor] broadcast constructor -------------------------------
        [[gnu::always_inline]]
        constexpr explicit BasicMask(std::same_as<bool> auto x) noexcept // LWG 4382.
            : data(x ? kImplicitMask : DataType()) {}

        // [simd.Mask.ctor] conversion constructor ------------------------------
        template<std::size_t UBytes, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize)
        [[gnu::always_inline]] constexpr explicit(IsMaskConversionExplicit<Ap, UAbi>(Bytes, UBytes))
            BasicMask(const BasicMask<UBytes, UAbi>& x) noexcept
            : data([&] [[gnu::always_inline]] {
                  using UV = BasicMask<UBytes, UAbi>;
                  // bool to bool
                  if constexpr (kIsScalar) {
                      return x[0];
                  }

                  // converting from an "std::array of bool"
                  else if constexpr (UV::kIsScalar) {
                      const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                      if constexpr (kUseBitmask) {
                          return ((DataType(x[indices]) << indices) | ...);
                      } else {
                          return DataType{VecValueType<DataType>(-x[indices])...};
                      }
                  }

                  // Vector-/bit-Mask to bit-Mask | bit-Mask to Vector-Mask
                  else if constexpr (kUseBitmask || UV::kUseBitmask) {
                      return BasicMask(x.ToBitset()).data;
                  }

                  // CxCtgus stores its masks matching the std::complex::value_type (UBytes/2)
                  else if constexpr (UAbi::kIsCxCtgus) {
                      return BasicMask(x.data).data;
                  }

                  // Vector-Mask to Vector-Mask
                  else if constexpr (Bytes == UBytes) {
                      return RecursiveBitCast(x).data;
                  }

                  // 2-Mask-elements wrapper to plain Mask
                  else if constexpr (UAbi::kIsCxIleav) {
                      if constexpr (UBytes <= sizeof(0ll)) {
                          // two step (bit-cast -> convert)
                          return BasicMask(SimilarMask<Detail::IntegerForSize<UBytes>, kStorageSize, UAbi>(x)).data;
                      } else if constexpr (Bytes == 1) { // 16 -> 1
                          const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                          using Ip = VecValueType<DataType>;
                          return DataType{Ip(x.data.ConcatData()[indices * 2])...};
                      } else // from std::complex<double>
                      {
                          const auto k2 =
                              SimilarMask<Detail::IntegerForSize<Bytes / 2>, 2 * kStorageSize, UAbi>(x.data);
                          return RecursiveBitCast(k2);
                      }
                  } else {
#if SORA_SIMD_X86
                      // TODO: turn this into a VecMaskCast overload in simd_x86.h
                      if constexpr (Bytes == 1 && UBytes == 2)
                          if (!IsConstKnown(x)) {
                              if constexpr (UAbi::kNreg == 1)
                                  return X86CvtVecmask<DataType>(x.data);
                              else if constexpr (UAbi::kNreg == 2) {
                                  auto lo = x.data0.data;
                                  auto hi = VecZeroPadTo<sizeof(lo)>(x.data1.ConcatData());
                                  return X86CvtVecmask<DataType>(lo, hi);
                              }
                          }
#endif
                      return VecMaskCast<DataType>(x.ConcatData());
                  }
              }()) {
        }

        using Base::MaskBase;

        // [simd.Mask.ctor] generator constructor -------------------------------
        template<SimdGeneratorInvokable<bool, kStorageSize> Fp>
        [[gnu::always_inline]] constexpr explicit BasicMask(Fp&& gen)
            : data([&] [[gnu::always_inline]] {
                  const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                  if constexpr (kIsScalar) {
                      return gen(kSimdSizeC<0>);
                  } else if constexpr (kUseBitmask) {
                      return DataType(((DataType(gen(kSimdSizeC<indices>)) << indices) | ...));
                  } else {
                      return DataType{VecValueType<DataType>(gen(kSimdSizeC<indices>) ? -1 : 0)...};
                  }
              }()) {}

        // [simd.Mask.ctor] std::bitset constructor ----------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask(const std::same_as<std::bitset<kStorageSize>> auto& b) noexcept // LWG 4382.
            : BasicMask(static_cast<Bitmask<kStorageSize>>(b.ToUllong())) {
            // more than 64 elements in one register? not yet.
            static_assert(kStorageSize <= std::numeric_limits<unsigned long long>::digits);
        }

        // [simd.Mask.ctor] uint constructor ------------------------------------
        template<std::unsigned_integral Tp>
            requires(!std::same_as<Tp, bool>) // LWG 4382.
        [[gnu::always_inline]] constexpr explicit BasicMask(Tp val) noexcept
            : data([&] [[gnu::always_inline]] () {
                  if constexpr (kUseBitmask) {
                      return val;
                  } else if constexpr (kIsScalar) {
                      return bool(val & 1);
                  } else if (IsConstKnown(val)) {
                      const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                      return DataType{VecValueType<DataType>((val & (1ull << indices)) == 0 ? 0 : -1)...};
                  } else {
                      using Ip = typename VecType::ValueType;
                      VecType v0 = Ip(val);
                      constexpr int bitsPerElement = sizeof(Ip) * __CHAR_BIT__;
                      constexpr VecType pow2 = VecType(Detail::kConstant<1>)
                                               << (kIota<VecType> % Detail::kConstant<bitsPerElement>);
                      if constexpr (kStorageSize < bitsPerElement) {
                          return ((v0 & pow2) > Detail::kConstant<0>).ConcatData();
                      } else if constexpr (kStorageSize == bitsPerElement) {
                          return ((v0 & pow2) != Detail::kConstant<0>).ConcatData();
                      } else {
                          static_assert(Bytes == 1);
                          static_assert(sizeof(Ip) == 1);
                          Bitmask<kStorageSize> bits = val;
                          static_assert(sizeof(VecType) % sizeof(bits) == 0);
                          if constexpr (sizeof(DataType) == 32) {
                              VecBuiltinType<UInt<8>, 4> v1 = {
                                  0xffu & (bits >> (0 * __CHAR_BIT__)),
                                  0xffu & (bits >> (1 * __CHAR_BIT__)),
                                  0xffu & (bits >> (2 * __CHAR_BIT__)),
                                  0xffu & (bits >> (3 * __CHAR_BIT__)),
                              };
                              v1 *= 0x0101'0101'0101'0101ull;
                              v0 = __builtin_bit_cast(VecType, v1);
                              return ((v0 & pow2) != Detail::kConstant<0>).data;
                          } else {
                              using V1 = Vector<Ip, sizeof(bits)>;
                              V1 v1 = __builtin_bit_cast(V1, bits);
                              v0 = VecType::StaticPermute(v1, [](int i) { return i / __CHAR_BIT__; });
                              return ((v0 & pow2) != Detail::kConstant<0>).data;
                          }
                      }
                  }
              }()) {}

        // Effects: Initializes the first M elements to the corresponding bit values in val, where M is
        // the smaller of size() and the number of bits in the value representation
        //([basic.types.general]) of the type of val. If M is std::less than size(), the remaining elements
        // are initialized to zero.

        // [simd.Mask.subscr] ---------------------------------------------------
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            SORA_SIMD_PRECONDITION(i >= 0 && i < kStorageSize, "subscript is out of bounds");
            if constexpr (kIsScalar) {
                return data;
            } else if constexpr (kUseBitmask) {
                return bool((data >> i) & 1);
            } else {
                return data[i] & 1;
            }
        }

        // [simd.Mask.unary] ----------------------------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask operator!() const noexcept {
            if constexpr (kIsScalar) {
                return Init(!data);
            } else {
                return Init(~data);
            }
        }

        [[gnu::always_inline]]
        constexpr VecType operator+() const noexcept
            requires std::destructible<VecType>
        {
            return operator VecType();
        }

        constexpr VecType operator+() const noexcept = delete;

        [[gnu::always_inline]]
        constexpr VecType operator-() const noexcept
            requires std::destructible<VecType>
        {
            using Ip = typename VecType::ValueType;
            if constexpr (kIsScalar) {
                return Ip(-int(data));
            } else if constexpr (kUseBitmask) {
                return SelectImpl(*this, Ip(-1), Ip());
            } else {
                static_assert(sizeof(VecType) == sizeof(data));
                return __builtin_bit_cast(VecType, data);
            }
        }

        constexpr VecType operator-() const noexcept = delete;

        [[gnu::always_inline]]
        constexpr VecType operator~() const noexcept
            requires std::destructible<VecType>
        {
            using Ip = typename VecType::ValueType;
            if constexpr (kIsScalar) {
                return Ip(~int(data));
            } else if constexpr (kUseBitmask) {
                return SelectImpl(*this, Ip(-2), Ip(-1));
            } else {
                static_assert(sizeof(VecType) == sizeof(data));
                return __builtin_bit_cast(VecType, data) - Ip(1);
            }
        }

        constexpr VecType operator~() const noexcept = delete;

        // [simd.Mask.conv] -----------------------------------------------------
        template<typename Up, typename UAbi>
            requires(UAbi::kStorageSize == kStorageSize)
        [[gnu::always_inline]]
        constexpr explicit(sizeof(Up) != Bytes) operator BasicVector<Up, UAbi>() const noexcept {
            if constexpr (kIsScalar) {
                return Up(data);
            } else {
                using UV = BasicVector<Up, UAbi>;
                return SelectImpl(static_cast<UV::MaskType>(*this), Up(1), UV());
            }
        }

        // [simd.Mask.namedconv] ------------------------------------------------
        [[gnu::always_inline]]
        constexpr std::bitset<kStorageSize> ToBitset() const noexcept {
            // more than 64 elements in one register? not yet.
            static_assert(kStorageSize <= std::numeric_limits<unsigned long long>::digits);
            return ToUllong();
        }

        /** @internal
         * Return the Mask as the smallest possible unsigned integer (up to 64 bits).
         *
         * @tparam Offset       Adjust the return type & value to start at bit @p Offset.
         * @tparam Use2For1  Store the value of every second element into one bit of the result.
         *                       (precondition: each even/odd std::pair stores the same value)
         */
        template<int Offset = 0, bool Use2For1 = false, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr Bitmask<kStorageSize / (Use2For1 + 1) + Offset> ToUint() const {
            constexpr int nbits = kStorageSize / (Use2For1 + 1);
            static_assert(nbits + Offset <= std::numeric_limits<unsigned long long>::digits);
            static_assert(!(kIsScalar && Use2For1));
            // before shifting
            using U0 = Bitmask<nbits>;
            // potentially wider type needed for shift by Offset
            using Ur = Bitmask<nbits + Offset>;
            if constexpr (kIsScalar || kUseBitmask) {
                auto bits = data;
                if constexpr (kIsPartial) {
                    bits &= kImplicitMask;
                }
                if constexpr (Use2For1) {
                    bits = BitExtractEven<nbits>(bits);
                }
                return Ur(bits) << Offset;
            } else if constexpr (Bytes == sizeof(0ll) && Use2For1) {
                const auto u32 = VecBitCast<unsigned>(data);
                if constexpr (sizeof(data) == 16) {
                    if constexpr (Offset < 32) {
                        return u32[0] & (1u << Offset);
                    } else {
                        return data[0] & (1ull << Offset);
                    }
                } else if constexpr (sizeof(data) == 32) {
                    if constexpr (Offset < 31) {
                        return (u32[4] & (2u << Offset)) | (u32[0] & (1u << Offset));
                    } else {
                        return (data[2] & (2ull << Offset)) | (data[0] & (1ull << Offset));
                    }
                } else {
                    static_assert(false);
                }
            } else if constexpr (Use2For1 && nbits == 1) {
                return Ur(operator[](0)) << Offset;
            } else {
#if SORA_SIMD_X86
                if (!IsConstKnown(*this)) {
                    U0 uint;
                    if constexpr (Use2For1) {
                        uint = X86CvtVecmaskToBitmask<Traits>(VecBitCast<Detail::IntegerForSize<Bytes * 2>>(data));
                    } else {
                        uint = X86CvtVecmaskToBitmask<Traits>(data);
                    }
                    if constexpr (kIsPartial) {
                        uint &= (U0(1) << kStorageSize) - 1;
                    }
                    return Ur(uint) << Offset;
                }
#endif
                using IV =
                    std::conditional_t<Use2For1, SimilarVec<Detail::IntegerForSize<Bytes * 2>, nbits, Ap>, VecType>;
                static_assert(std::destructible<IV>);
                const typename IV::MaskType& k = [&] [[gnu::always_inline]] () {
                    if constexpr (Use2For1) {
                        return typename IV::MaskType(ToCxIleav(*this));
                    } else if constexpr (std::is_same_v<typename IV::MaskType, BasicMask>) {
                        return *this;
                    } else {
                        return typename IV::MaskType(*this);
                    }
                }();
                constexpr int n = IV::kSize();
                if constexpr (Bytes * __CHAR_BIT__ >= n) // '1 << kIota' cannot overflow
                {                                        // Reduce(Select(k, powers_of_2, 0))
                    constexpr IV pow2 = IV(Detail::kConstant<1>) << kIota<IV>;
                    return Ur(U0(SelectImpl(k, pow2, IV()).Reduce(std::bit_or<>()))) << Offset;
                } else if constexpr (n % __CHAR_BIT__ != 0) { // recurse after splitting in two
                    constexpr int nLo = n - n % __CHAR_BIT__;
                    const auto [lo, hi] = Chunk<nLo>(k);
                    Ur bits = hi.template ToUint<Offset + nLo, Use2For1>();
                    return bits | lo.template ToUint<Offset, Use2For1>();
                } else { // limit powers_of_2 to 1, 2, 4, ..., 128
                    constexpr IV pow2 = IV(Detail::kConstant<1>) << (kIota<IV> % IV(Detail::kConstant<__CHAR_BIT__>));
                    IV x = SelectImpl(k, pow2, IV());
                    // partial reductions of 8 neighboring elements
                    x |= IV::StaticPermute(x, SwapNeighbors<4>());
                    x |= IV::StaticPermute(x, SwapNeighbors<2>());
                    x |= IV::StaticPermute(x, SwapNeighbors<1>());
                    // Permute partial reduction results to the front
                    x = IV::StaticPermute(x, [](int i) { return i * 8 < n ? i * 8 : kUninitElement; });
                    // Extract front as scalar unsigned
                    U0 bits = __builtin_bit_cast(SimilarVec<U0, n * Bytes / sizeof(U0), Ap>, x)[0];
                    // Mask off unused bits
                    if constexpr (!std::has_single_bit(unsigned(nbits))) {
                        bits &= (U0(1) << nbits) - 1;
                    }
                    return Ur(bits) << Offset;
                }
            }
        }

        [[gnu::always_inline]]
        constexpr unsigned long long ToUllong() const {
            return ToUint();
        }

        // [simd.Mask.binary] ---------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask operator&&(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data & y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator||(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data | y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator&(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data & y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator|(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data | y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator^(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data ^ y.data);
        }

        // [simd.Mask.cassign] --------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask& operator&=(BasicMask& x, const BasicMask& y) noexcept {
            x.data &= y.data;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask& operator|=(BasicMask& x, const BasicMask& y) noexcept {
            x.data |= y.data;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask& operator^=(BasicMask& x, const BasicMask& y) noexcept {
            x.data ^= y.data;
            return x;
        }

        // [simd.Mask.comparison] -----------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask operator==(const BasicMask& x, const BasicMask& y) noexcept {
            return !(x ^ y);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator!=(const BasicMask& x, const BasicMask& y) noexcept {
            return x ^ y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>=(const BasicMask& x, const BasicMask& y) noexcept {
            return x || !y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<=(const BasicMask& x, const BasicMask& y) noexcept {
            return !x || y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>(const BasicMask& x, const BasicMask& y) noexcept {
            return x && !y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<(const BasicMask& x, const BasicMask& y) noexcept {
            return !x && y;
        }

        // [simd.Mask.cond] -----------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, const BasicMask& t, const BasicMask& f) noexcept {
            if constexpr (!kUseBitmask) {
#if SORA_SIMD_X86
                // this works around bad code-gen when the compiler can't see that k is a vector-Mask.
                // This pattern, is recognized to match the x86 blend instructions, which only consider
                // the sign bit of the Mask register. Also, without SSE4, if the compiler knows that k
                // is a vector-Mask, then the '< 0' is elided.
                return k.data < 0 ? t.data : f.data;
#endif
                return k.data ? t.data : f.data;
            } else {
                return (k.data & t.data) | (~k.data & f.data);
            }
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, std::same_as<bool> auto t,
                                              std::same_as<bool> auto f) noexcept {
            if (t == f) {
                return BasicMask(t);
            } else {
                return t ? k : !k;
            }
        }

        template<Vectorizable T0, std::same_as<T0> T1>
            requires(sizeof(T0) == Bytes)
        [[gnu::always_inline]]
        friend constexpr Vector<T0, kStorageSize> SelectImpl(const BasicMask& k, const T0& t, const T1& f) noexcept {
            if constexpr (kIsScalar) {
                return k.data ? t : f;
            } else {
                using Vp = Vector<T0, kStorageSize>;
                using Mp = typename Vp::MaskType;
                return SelectImpl(Mp(k), Vp(t), Vp(f));
            }
        }

        // [simd.Mask.reductions] implementation --------------------------------
        [[gnu::always_inline]]
        constexpr bool AllOf() const noexcept {
            if constexpr (kIsScalar) {
                return data;
            } else if constexpr (kUseBitmask) {
                if constexpr (kIsPartial) {
                    // PR120925 (partial kortest pattern not recognized)
                    return (data & kImplicitMask) == kImplicitMask;
                } else {
                    return data == kImplicitMask;
                }
            }
#if SORA_SIMD_X86
            else if (!IsConstKnown(data))
                return X86VecmaskAll<kStorageSize>(data);
#endif
            else {
                return VecOps<DataType, kStorageSize>::AllOf(data);
            }
        }

        [[gnu::always_inline]]
        constexpr bool AnyOf() const noexcept {
            if constexpr (kIsScalar) {
                return data;
            } else if constexpr (kUseBitmask) {
                if constexpr (kIsPartial) {
                    // PR120925 (partial kortest pattern not recognized)
                    return (data & kImplicitMask) != 0;
                } else {
                    return data != 0;
                }
            }
#if SORA_SIMD_X86
            else if (!IsConstKnown(data))
                return X86VecmaskAny<kStorageSize>(data);
#endif
            else {
                return VecOps<DataType, kStorageSize>::AnyOf(data);
            }
        }

        [[gnu::always_inline]]
        constexpr bool NoneOf() const noexcept {
            if constexpr (kIsScalar) {
                return !data;
            } else if constexpr (kUseBitmask) {
                if constexpr (kIsPartial) {
                    // PR120925 (partial kortest pattern not recognized)
                    return (data & kImplicitMask) == 0;
                } else {
                    return data == 0;
                }
            }
#if SORA_SIMD_X86
            else if (!IsConstKnown(data))
                return X86VecmaskNone<kStorageSize>(data);
#endif
            else {
                return VecOps<DataType, kStorageSize>::NoneOf(data);
            }
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceCount() const noexcept {
            if constexpr (kIsScalar) {
                return int(data);
            } else if constexpr (kStorageSize <= std::numeric_limits<unsigned>::digits) {
                return __builtin_popcount(ToUint());
            } else {
                return __builtin_popcountll(ToUllong());
            }
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMinIndex() const {
            const auto bits = ToUint();
            SORA_SIMD_PRECONDITION(bits, "An empty Mask does not have a min_index.");
            if constexpr (kStorageSize == 1) {
                return 0;
            } else {
                return std::countr_zero(bits);
            }
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMaxIndex() const {
            const auto bits = ToUint();
            SORA_SIMD_PRECONDITION(bits, "An empty Mask does not have a max_index.");
            if constexpr (kStorageSize == 1) {
                return 0;
            } else {
                return HighestBit(bits);
            }
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicMask& x) {
            return __builtin_constant_p(x.data);
        }
    };

    template<std::size_t Bytes, AbiTag Ap>
        requires(Ap::kNreg > 1) && (FilterAbiVariant(Ap::kVariant, AbiVariant::kCxVariants) == AbiVariant())
    class BasicMask<Bytes, Ap> : public MaskBase<Bytes, Ap> {
        using Base = MaskBase<Bytes, Ap>;

        using VecType = Base::VecType;

        template<std::size_t, typename>
        friend class BasicMask;

        template<typename, typename>
        friend class BasicVector;

        static constexpr int kStorageSize = Ap::kStorageSize;

        static constexpr int kN0 = std::bit_ceil(unsigned(kStorageSize)) / 2;

        static constexpr int kN1 = kStorageSize - kN0;

        static constexpr int kNreg0 = std::bit_ceil(unsigned(Ap::kNreg)) / 2;

        static constexpr int kNreg1 = Ap::kNreg - kNreg0;

        // explicitly request kNreg0 rather than use AbiRebind. This way _Float16 can use half
        // of native registers (since they convert to full float32 registers).
        using Abi0 = decltype(Ap::template Resize<kN0, kNreg0>());

        using Abi1 = decltype(Ap::template Resize<kN1, kNreg1>());

        using Mask0 = BasicMask<Bytes, Abi0>;

        // the implementation (and users) depend on elements being contiguous in memory
        static_assert(Mask0::kPaddingBytes == 0 && !Mask0::kIsPartial);

        using Mask1 = BasicMask<Bytes, Abi1>;

        static constexpr bool kIsPartial = Mask1::kIsPartial;

        // Ap::kNreg determines how deep the recursion goes. E.g. BasicMask<4, Abi<8, 4>> cannot
        // use BasicMask<4, Abi<4, 1>> as Mask0/1 types.
        static_assert(Mask0::AbiType::kNreg + Mask1::AbiType::kNreg == Ap::kNreg);

        static constexpr bool kUseBitmask = Mask0::kUseBitmask;

        static constexpr bool kIsScalar = Mask0::kIsScalar;

        Mask0 data0;

        Mask1 data1;

        static constexpr bool kHasBoolMember = Mask1::kHasBoolMember;

        // by construction N0 >= N1
        // => sizeof(Mask0) >= sizeof(Mask1)
        //    and __alignof__(Mask0) >= __alignof__(Mask1)
        static constexpr std::size_t kPaddingBytes =
            (__alignof__(Mask0) == __alignof__(Mask1) ? 0 : __alignof__(Mask0) - (sizeof(Mask1) % __alignof__(Mask0))) +
            Mask1::kPaddingBytes;

    public:
        using ValueType = bool;

        using AbiType = Ap;

        using IteratorType = Base::IteratorType;

        using ConstIteratorType = Base::ConstIteratorType;

        [[gnu::always_inline]]
        static constexpr BasicMask Init(const Mask0& x, const Mask1& y) {
            BasicMask r;
            r.data0 = x;
            r.data1 = y;
            return r;
        }

        [[gnu::always_inline]]
        static constexpr BasicMask Init(std::unsigned_integral auto bits) {
            return BasicMask(bits);
        }

        template<typename U0, typename U1>
        [[gnu::always_inline]]
        static constexpr BasicMask Init(const TrivialPair<U0, U1>& bits) {
            if constexpr (std::is_unsigned_v<U0>) {
                static_assert(std::is_unsigned_v<U1>);
                return Init(Mask0(bits.first), Mask1(bits.second));
            } else if constexpr (std::is_unsigned_v<U1>) {
                return Init(Mask0::Init(bits.first), Mask1(bits.second));
            } else {
                return Init(Mask0::Init(bits.first), Mask1::Init(bits.second));
            }
        }

        [[gnu::always_inline]]
        constexpr const Mask0& GetLow() const {
            return data0;
        }

        [[gnu::always_inline]]
        constexpr const Mask1& GetHigh() const {
            return data1;
        }

        template<std::size_t UBytes, typename UAbi>
        [[gnu::always_inline]]
        static constexpr BasicMask RecursiveBitCast(const BasicMask<UBytes, UAbi>& x) {
            using Mp = BasicMask<UBytes, UAbi>;
            if constexpr (Mp::kHasBoolMember || sizeof(BasicMask) > sizeof(x) || Mp::kPaddingBytes != 0) {
                return Init(__builtin_bit_cast(Mask0, x.data0), Mask1::RecursiveBitCast(x.data1));
            } else if constexpr (sizeof(BasicMask) == sizeof(x)) {
                return __builtin_bit_cast(BasicMask, x);
            } else { // e.g. on IvyBridge (different Alignment => different sizeof)
                struct Tmp {
                    alignas(Mp) BasicMask data;
                };
                return __builtin_bit_cast(Tmp, x).data;
            }
        }

        [[gnu::always_inline]]
        constexpr auto ConcatData(bool doSanitize = kIsPartial) const {
            if constexpr (kUseBitmask) {
                static_assert(kStorageSize <= std::numeric_limits<unsigned long long>::digits,
                              "cannot Concat more than 64 bits");
                using Up = Bitmask<kStorageSize>;
                return Up(data0.ConcatData() | (Up(data1.ConcatData(doSanitize)) << kN0));
            } else {
                auto lo = data0.ConcatData();
                auto hi = VecZeroPadTo<sizeof(lo)>(data1.ConcatData(doSanitize));
                return VecConcat(lo, hi);
            }
        }

        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] static constexpr BasicMask PartialMaskOfN(int n) {
#if __has_builtin(__builtin_ia32_bzhi_di)
            if constexpr (kUseBitmask && kStorageSize <= 64 && Traits.HaveBmi2()) {
                return BasicMask(__builtin_ia32_bzhi_di(~0ull >> (64 - kStorageSize), unsigned(n)));
            }
#endif
            if constexpr (kN0 == 1) {
                static_assert(kStorageSize == 2); // => n == 1
                return Init(Mask0(true), Mask1(false));
            } else if (n < kN0) {
                return Init(Mask0::PartialMaskOfN(n), Mask1(false));
            } else if (n == kN0 || kN1 == 1) {
                return Init(Mask0(true), Mask1(false));
            } else if constexpr (kN1 != 1) {
                return Init(Mask0(true), Mask1::PartialMaskOfN(n - kN0));
            }
        }

        [[gnu::always_inline]]
        constexpr BasicMask& AndNeighbors() {
            if constexpr (kStorageSize == 2) {
                static_assert(kIsScalar);
                data0 = data1 = data0 && data1;
            } else {
                data0.AndNeighbors();
                data1.AndNeighbors();
            }
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicMask& OrNeighbors() {
            if constexpr (kStorageSize == 2) {
                static_assert(kIsScalar);
                data0 = data1 = data0 || data1;
            } else {
                data0.OrNeighbors();
                data1.OrNeighbors();
            }
            return *this;
        }

        template<typename Mp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            constexpr int n = kStorageSize / Mp::kStorageSize;
            constexpr int rem = kStorageSize % Mp::kStorageSize;
            const auto& [... indices] = Detail::kIotaArray<n>;
            if constexpr (rem == 0) {
                return std::array<Mp, n>{
                    ExtractSimdAt<Mp>(Detail::kConstant<Mp::kStorageSize * indices>, data0, data1)...};
            } else {
                using Rest = Resize<rem, Mp>;
                return std::tuple(ExtractSimdAt<Mp>(Detail::kConstant<Mp::kStorageSize * indices>, data0, data1)...,
                                  ExtractSimdAt<Rest>(Detail::kConstant<Mp::kStorageSize * n>, data0, data1));
            }
        }

        [[gnu::always_inline]]
        static constexpr BasicMask Concat(const BasicMask& x0) noexcept {
            return x0;
        }

        template<typename... As>
            requires(sizeof...(As) >= 2)
        [[gnu::always_inline]]
        static constexpr BasicMask Concat(const BasicMask<Bytes, As>&... xs) noexcept {
            static_assert(kStorageSize == (As::kStorageSize + ...));
            return Init(ExtractSimdAt<Mask0>(Detail::kConstant<0>, xs...),
                        ExtractSimdAt<Mask1>(Detail::kConstant<kN0>, xs...));
        }

        // [simd.Mask.overview] default constructor -----------------------------
        BasicMask() = default;

        // [simd.Mask.overview] conversion extensions ---------------------------
        // TODO: any?

        // [simd.Mask.ctor] broadcast constructor -------------------------------
        [[gnu::always_inline]]
        constexpr explicit BasicMask(std::same_as<bool> auto x) noexcept // LWG 4382.
            : data0(x), data1(x) {}

        // [simd.Mask.ctor] conversion constructor ------------------------------
        template<std::size_t UBytes, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize) && (UAbi::kIsCxCtgus)
        [[gnu::always_inline]]
        constexpr explicit(IsMaskConversionExplicit<Ap, UAbi>(Bytes, UBytes))
            BasicMask(const BasicMask<UBytes, UAbi>& x) noexcept
            : BasicMask(x.data) // unwrap CxCtgus BasicMask partial specialization
        {}

        template<std::size_t UBytes, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize) && (!UAbi::kIsCxCtgus)
        [[gnu::always_inline]]
        constexpr explicit(IsMaskConversionExplicit<Ap, UAbi>(Bytes, UBytes))
            BasicMask(const BasicMask<UBytes, UAbi>& x) noexcept
            : data0([&] {
                  if constexpr (UAbi::kNreg > 1) {
                      if constexpr (UAbi::kIsCxIleav) {
                          return ToCxIleav(x.data.data0);
                      } else {
                          return x.data0;
                      }
                  } else if constexpr (kN0 == 1) {
                      return Mask0(x[0]);
                  } else {
                      return Get<0>(Chunk<kN0>(x));
                  }
              }()),
              data1([&] {
                  if constexpr (UAbi::kNreg > 1) {
                      if constexpr (UAbi::kIsCxIleav) {
                          return ToCxIleav(x.data.data1);
                      } else {
                          return x.data1;
                      }
                  } else if constexpr (kN1 == 1) {
                      return Mask1(x[kN0]);
                  } else {
                      return Get<1>(Chunk<kN0>(x));
                  }
              }()) {}

        using Base::MaskBase;

        // [simd.Mask.ctor] generator constructor -------------------------------
        template<SimdGeneratorInvokable<bool, kStorageSize> Fp>
        [[gnu::always_inline]] constexpr explicit BasicMask(Fp&& gen)
            : data0(gen), data1([&] [[gnu::always_inline]] (auto i) { return gen(kSimdSizeC<i + kN0>); }) {}

        // [simd.Mask.ctor] std::bitset constructor ----------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask(const std::same_as<std::bitset<kStorageSize>> auto& b) noexcept // LWG 4382.
            : data0(BitsetSplit<kN0>(b).lo), data1(BitsetSplit<kN0>(b).hi) {}

        // [simd.Mask.ctor] uint constructor ------------------------------------------
        template<std::unsigned_integral Tp>
            requires(!std::same_as<Tp, bool>) // LWG 4382.
        [[gnu::always_inline]]
        constexpr explicit BasicMask(Tp val) noexcept
            : data0(static_cast<Bitmask<kN0>>(val)),
              data1(sizeof(Tp) * __CHAR_BIT__ > kN0 ? static_cast<Bitmask<kN1>>(val >> kN0) : Bitmask<kN1>()) {}

        // [simd.Mask.subscr] ---------------------------------------------------
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            SORA_SIMD_PRECONDITION(i >= 0 && i < kStorageSize, "subscript is out of bounds");
            if (IsConstKnown(i)) {
                return i < kN0 ? data0[i] : data1[i - kN0];
            } else if constexpr (data1.kHasBoolMember) {
                // in some cases the last element can be 'bool' instead of bit-/vector-Mask;
                // e.g. Mask<short, 17> is {Mask<short, 16>, Mask<short, 1>}, where the latter uses
                // Abi<1, 1>, which is stored as 'bool'
                return i < kN0 ? data0[i] : data1[i - kN0];
            } else if constexpr (AbiType::kIsBitmask) {
                using AliasingByte [[__gnu__::__may_alias__]] = unsigned char;
                return bool((reinterpret_cast<const AliasingByte*>(this)[i / __CHAR_BIT__] >> (i % __CHAR_BIT__)) & 1);
            } else {
                using AliasingInt [[__gnu__::__may_alias__]] = Detail::IntegerForSize<Bytes>;
                return reinterpret_cast<const AliasingInt*>(this)[i] != 0;
            }
        }

        // [simd.Mask.unary] ----------------------------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask operator!() const noexcept {
            return Init(!data0, !data1);
        }

        [[gnu::always_inline]]
        constexpr VecType operator+() const noexcept
            requires std::destructible<VecType>
        {
            return VecType::Concat(+data0, +data1);
        }

        constexpr VecType operator+() const noexcept = delete;

        [[gnu::always_inline]]
        constexpr VecType operator-() const noexcept
            requires std::destructible<VecType>
        {
            return VecType::Concat(-data0, -data1);
        }

        constexpr VecType operator-() const noexcept = delete;

        [[gnu::always_inline]]
        constexpr VecType operator~() const noexcept
            requires std::destructible<VecType>
        {
            return VecType::Concat(~data0, ~data1);
        }

        constexpr VecType operator~() const noexcept = delete;

        // [simd.Mask.conv] -----------------------------------------------------
        template<typename Up, typename UAbi>
            requires(UAbi::kStorageSize == kStorageSize)
        [[gnu::always_inline]]
        constexpr explicit(sizeof(Up) != Bytes) operator BasicVector<Up, UAbi>() const noexcept {
            using Rp = BasicVector<Up, UAbi>;
            return Rp::Init(static_cast<Rp::DataType0>(data0), static_cast<Rp::DataType1>(data1));
        }

        // [simd.Mask.namedconv] ------------------------------------------------
        [[gnu::always_inline]]
        constexpr std::bitset<kStorageSize> ToBitset() const noexcept {
            if constexpr (kStorageSize <= std::numeric_limits<unsigned long long>::digits) {
                return ToUllong();
            } else {
                static_assert(kN0 % std::numeric_limits<unsigned long long>::digits == 0);
                struct Tmp {
                    std::bitset<kN0> lo;
                    std::bitset<kN1> hi;
                } tmp = {data0.ToBitset(), data1.ToBitset()};
                return __builtin_bit_cast(std::bitset<kStorageSize>, tmp);
            }
        }

        template<int Offset = 0, bool Use2For1 = false, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr auto ToUint() const {
            constexpr int n0x = Use2For1 ? kN0 / 2 : kN0;
            if constexpr (Use2For1 && kIsScalar && kStorageSize == 2) {
                return data1.template ToUint<Offset>();
            } else if constexpr (n0x >= std::numeric_limits<unsigned long long>::digits) {
                static_assert(Offset == 0);
                return TrivialPair{data0.template ToUint<0, Use2For1>(), data1.template ToUint<0, Use2For1>()};
            } else {
#if SORA_SIMD_X86
                if constexpr (Bytes == 2 && !Traits.HaveBmi2() && Ap::kNreg == 2 && !kIsScalar && !kUseBitmask &&
                              !Use2For1) {
                    return SimilarMask<char, kStorageSize, Ap>(*this).template ToUint<Offset>();
                }
#endif
                auto uint = data1.template ToUint<n0x + Offset, Use2For1>();
                uint |= data0.template ToUint<Offset, Use2For1>();
                return uint;
            }
        }

        [[gnu::always_inline]]
        constexpr unsigned long long ToUllong() const {
            if constexpr (kStorageSize <= std::numeric_limits<unsigned long long>::digits) {
                return ToUint();
            } else {
                SORA_SIMD_PRECONDITION(data1.ToUllong() == 0, "ToUllong called on Mask with 'true' elements at indices"
                                                              "higher than representable in a ullong");
                return data0.ToUllong();
            }
        }

        // [simd.Mask.binary]
        [[gnu::always_inline]]
        friend constexpr BasicMask operator&&(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data0 && y.data0, x.data1 && y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator||(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data0 || y.data0, x.data1 || y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator&(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data0 & y.data0, x.data1 & y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator|(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data0 | y.data0, x.data1 | y.data1);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator^(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data0 ^ y.data0, x.data1 ^ y.data1);
        }

        // [simd.Mask.cassign]
        [[gnu::always_inline]]
        friend constexpr BasicMask& operator&=(BasicMask& x, const BasicMask& y) noexcept {
            x.data0 &= y.data0;
            x.data1 &= y.data1;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask& operator|=(BasicMask& x, const BasicMask& y) noexcept {
            x.data0 |= y.data0;
            x.data1 |= y.data1;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask& operator^=(BasicMask& x, const BasicMask& y) noexcept {
            x.data0 ^= y.data0;
            x.data1 ^= y.data1;
            return x;
        }

        // [simd.Mask.comparison] -----------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask operator==(const BasicMask& x, const BasicMask& y) noexcept {
            return !(x ^ y);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator!=(const BasicMask& x, const BasicMask& y) noexcept {
            return x ^ y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>=(const BasicMask& x, const BasicMask& y) noexcept {
            return x || !y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<=(const BasicMask& x, const BasicMask& y) noexcept {
            return !x || y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>(const BasicMask& x, const BasicMask& y) noexcept {
            return x && !y;
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<(const BasicMask& x, const BasicMask& y) noexcept {
            return !x && y;
        }

        // [simd.Mask.cond] -----------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, const BasicMask& t, const BasicMask& f) noexcept {
            return Init(SelectImpl(k.data0, t.data0, f.data0), SelectImpl(k.data1, t.data1, f.data1));
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, std::same_as<bool> auto t,
                                              std::same_as<bool> auto f) noexcept {
            if (t == f) {
                return BasicMask(t);
            } else {
                return t ? k : !k;
            }
        }

        template<Vectorizable T0, std::same_as<T0> T1>
            requires(sizeof(T0) == Bytes)
        [[gnu::always_inline]]
        friend constexpr Vector<T0, kStorageSize> SelectImpl(const BasicMask& k, const T0& t, const T1& f) noexcept {
            using Vp = Vector<T0, kStorageSize>;
            if constexpr (!std::is_same_v<BasicMask, typename Vp::MaskType>) {
                return SelectImpl(static_cast<Vp::MaskType>(k), t, f);
            } else if constexpr (ComplexLike<T0>) {
                return Vp::Concat(SelectImpl(k.data0, t, f), SelectImpl(k.data1, t, f));
            } else {
                return Vp::Init(SelectImpl(k.data0, t, f), SelectImpl(k.data1, t, f));
            }
        }

        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr bool AllOf() const {
            if constexpr (kN0 == kN1) {
                return (data0 && data1).AllOf();
            } else {
                return data0.AllOf() && data1.AllOf();
            }
        }

        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr bool AnyOf() const {
            if constexpr (kN0 == kN1) {
                return (data0 || data1).AnyOf();
            } else {
                return data0.AnyOf() || data1.AnyOf();
            }
        }

        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr bool NoneOf() const {
            if constexpr (kN0 == kN1) {
                return (data0 || data1).NoneOf();
            } else {
                return data0.NoneOf() && data1.NoneOf();
            }
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceCount() const noexcept {
            if constexpr (kIsScalar) {
                // SWAR could help. I don't think we care at the moment.
                return data0.ReduceCount() + data1.ReduceCount();
            } else if constexpr (kStorageSize <= std::numeric_limits<unsigned>::digits) {
                return __builtin_popcount(ToUint());
            } else if constexpr (kStorageSize <= std::numeric_limits<unsigned long long>::digits) {
                return __builtin_popcountll(ToUllong());
            } else {
                return data0.ReduceCount() + data1.ReduceCount();
            }
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMinIndex() const {
            if constexpr (kStorageSize <= std::numeric_limits<unsigned long long>::digits) {
                const auto bits = ToUint();
                SORA_SIMD_PRECONDITION(bits, "An empty Mask does not have a min_index.");
                return std::countr_zero(ToUint());
            } else if (data0.NoneOf()) {
                return data1.ReduceMinIndex() + kN0;
            } else {
                return data0.ReduceMinIndex();
            }
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMaxIndex() const {
            if constexpr (kStorageSize <= std::numeric_limits<unsigned long long>::digits) {
                const auto bits = ToUint();
                SORA_SIMD_PRECONDITION(bits, "An empty Mask does not have a max_index.");
                return HighestBit(ToUint());
            } else if (data1.NoneOf()) {
                return data0.ReduceMaxIndex();
            } else {
                return data1.ReduceMaxIndex() + kN0;
            }
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicMask& x) {
            return IsConstKnown(x.data0) && IsConstKnown(x.data1);
        }
    };

} // namespace Sora::Math::Simd

#pragma GCC diagnostic pop
