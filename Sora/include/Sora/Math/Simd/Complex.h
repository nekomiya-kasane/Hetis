/**
 * @file Complex.h
 * @brief SIMD support for complex-valued vectors and masks.
 * @ingroup Math
 */
#pragma once

#include "Vector.h"

#include <complex>

// psabi warnings are bogus because the ABI of the internal types never leaks into user code
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

namespace Sora::Math::Simd {

    /** @internal
     * @brief Return a CxIleav Mask that holds @p k as its data member.
     *
     * @note If the resulting Mask type has size 1, then it will actually Store a single bool, rather
     * than the given Mask object.
     */
    template<std::size_t Bytes, typename Ap,
             AbiTag Aret = decltype(AbiRebind<std::complex<Detail::FloatForSize<Bytes>>, Ap::kStorageSize / 2, Ap>())>
    [[gnu::always_inline]]
    constexpr BasicMask<Bytes * 2, Aret> ToCxIleav(const BasicMask<Bytes, Ap>& k) {
        static_assert(Ap::kStorageSize % 2 == 0 &&
                      (FilterAbiVariant(Ap::kVariant, AbiVariant::kCxVariants) == AbiVariant()));
        if constexpr (Aret::kStorageSize == 1) {
            return BasicMask<Bytes * 2, Aret>(k[0]);
        } else {
            return BasicMask<Bytes * 2, Aret>::Init(k);
        }
    }

    constexpr void CheckHiBitsForZero(std::unsigned_integral auto x) {
        SORA_SIMD_PRECONDITION(x == 0, "ToUllong called on Mask with 'true' elements at indices"
                                       "higher than 64");
    }

    template<typename T0, typename T1>
    constexpr void CheckHiBitsForZero(const TrivialPair<T0, T1>& p) {
        Sora::Math::Simd::CheckHiBitsForZero(p.first);
        Sora::Math::Simd::CheckHiBitsForZero(p.second);
    }

    constexpr unsigned long long UnwrapPairsToUllong(std::unsigned_integral auto x) {
        return x;
    }

    template<typename T0, typename T1>
    constexpr unsigned long long UnwrapPairsToUllong(const TrivialPair<T0, T1>& p) {
        Sora::Math::Simd::CheckHiBitsForZero(p.second);
        return Sora::Math::Simd::UnwrapPairsToUllong(p.first);
    }

    template<int Np>
    constexpr std::bitset<Np> UnwrapPairsToBitset(std::unsigned_integral auto x) {
        static_assert(Np <= 64);
        return x;
    }

    template<std::size_t Np, typename T0, typename T1>
    constexpr std::bitset<Np> UnwrapPairsToBitset(const TrivialPair<T0, T1>& p) {
        constexpr std::size_t n0 = std::bit_floor(Np);
        constexpr std::size_t n1 = Np - n0;
        static_assert(n0 % 64 == 0);
        struct Tmp {
            std::bitset<std::bit_floor(Np)> lo;
            std::bitset<Np - std::bit_floor(Np)> hi;
        };
        Tmp tmp = {Sora::Math::Simd::UnwrapPairsToBitset<n0>(p.first),
                   Sora::Math::Simd::UnwrapPairsToBitset<n1>(p.second)};
        return __builtin_bit_cast(std::bitset<Np>, tmp);
    }

    template<std::size_t Bytes>
    consteval auto TreeOfUlong() {
        static constexpr std::size_t kN0 = std::bit_floor(Bytes - 1);
        static constexpr std::size_t kN1 = Bytes - kN0;
        if constexpr (Bytes <= sizeof(unsigned long)) {
            return 0ul;
        } else {
            return TrivialPair{TreeOfUlong<kN0>(), TreeOfUlong<kN1>()};
        }
    }

    template<std::size_t Bytes>
    using TreeOfUlongT = decltype(TreeOfUlong<Bytes>());

    template<std::size_t Np>
    constexpr auto BitsetToPairs(const std::bitset<Np>& b) noexcept {
        if constexpr (Np <= 64) {
            return b.ToUllong();
        } else {
            return __builtin_bit_cast(TreeOfUlongT<DivCeil(Np, std::size_t(__CHAR_BIT__))>, b);
        }
    }

    // std::complex interleaved (CxIleav) -------------------------------------------

    /** @internal
     * @brief Functions acting on / recursing into the non-std::complex fp Vector objects, interpreting even
     * elements as Real and odd elements as imaginary.
     */
    namespace Cxileav {
        /** @internal
         * @brief Set even (Real) elements in @p x to the values in @p re.
         */
        template<typename Tp, typename Ap>
        [[gnu::always_inline]]
        constexpr void SetReal(BasicVector<Tp, Ap>& x, const SimilarVec<Tp, Ap::kStorageSize / 2, Ap>& re) noexcept {
            if constexpr (ScalarAbiTag<Ap> && Ap::kStorageSize == 2) {
                x.GetLow() = re;
            } else if constexpr (Ap::kNreg >= 2) { // recurse
                constexpr int n0 = x.GetLow().kSize();
                const auto& [lo, hi] = re.template ChunkStorage<SimilarVec<Tp, n0 / 2, Ap>>();
                SetReal(x.GetLow(), lo);
                SetReal(x.GetHigh(), hi);
            } else {
                using DataType = typename Ap::template DataType<Tp>;
                DataType& xv = x.Get();
                const auto rv = re.Get();
                if constexpr (Ap::kStorageSize == 2) {
                    VecSet(xv, 0, rv);
                } else if (IsConstKnown(x, re)) {
                    const auto& [... indices] = Detail::kIotaArray<Ap::kStorageSize>;
                    xv = DataType{((indices & 1) == 0 ? rv[indices / 2] : xv[indices])...};
                } else {
                    VecOps<DataType>::OverwriteEvenElements(xv, rv);
                }
            }
        }

        /** @internal
         * @brief Set odd (imaginary) elements in @p x to the values in @p im.
         */
        template<typename Tp, typename Ap>
        [[gnu::always_inline]]
        constexpr void SetImag(BasicVector<Tp, Ap>& x, const SimilarVec<Tp, Ap::kStorageSize / 2, Ap>& im) noexcept {
            if constexpr (ScalarAbiTag<Ap> && Ap::kStorageSize == 2) {
                x.GetHigh() = im;
            } else if constexpr (Ap::kNreg >= 2) { // recurse
                constexpr int n0 = x.GetLow().kSize();
                const auto& [lo, hi] = im.template ChunkStorage<SimilarVec<Tp, n0 / 2, Ap>>();
                SetImag(x.GetLow(), lo);
                SetImag(x.GetHigh(), hi);
            } else {
                using DataType = typename Ap::template DataType<Tp>;
                DataType& xv = x.Get();
                const auto iv = im.Get();
                if constexpr (Ap::kStorageSize == 2) {
                    VecSet(xv, 1, iv);
                } else if (IsConstKnown(x, im)) {
                    const auto& [... indices] = Detail::kIotaArray<Ap::kStorageSize>;
                    xv = DataType{((indices & 1) == 1 ? iv[indices / 2] : xv[indices])...};
                } else {
                    VecOps<DataType>::OverwriteOddElements(xv, iv);
                }
            }
        }

        /** @internal
         * @brief Return @p x after flipping the sign of odd (imaginary) elements.
         */
        template<typename Tp, typename Ap>
        [[gnu::always_inline]]
        constexpr BasicVector<Tp, Ap> NegateImag(const BasicVector<Tp, Ap>& x) {
            if constexpr (ScalarAbiTag<Ap> && Ap::kStorageSize == 2) {
                return BasicVector<Tp, Ap>::Init(x.GetLow(), -x.GetHigh());
            } else if constexpr (Ap::kNreg >= 2) { // recurse
                return BasicVector<Tp, Ap>::Init(NegateImag(x.GetLow()), NegateImag(x.GetHigh()));
            } else {
                return VecOps<typename Ap::template DataType<Tp>>::ComplexNegateImag(x.Get());
            }
        }

        /** @internal
         * @brief Recompute all std::complex multiplications where @p nan is true using @p Cx's
         * multiplication operator.
         *
         * @todo use coarser TargetTraits and std::move into .so
         */
        template<typename Cx, TargetTraits, VecBuiltin TV>
        [[__gnu__::__cold__]]
        constexpr TV RedoMul(TV r, const TV x, const TV y, const auto nan, const int n) {
            // redo multiplication using scalar std::complex-mul on (NaN, NaN) results
            for (int i = 0; i < n; i += 2) {
                if (nan[i] && nan[i + 1]) {
                    using Tc = typename Cx::value_type;
                    const Cx cx{Tc(x[i]), Tc(x[i + 1])};
                    const Cx cy{Tc(y[i]), Tc(y[i + 1])};
                    const Cx cr = cx * cy;
                    VecSet(r, i, cr.Real());
                    VecSet(r, i + 1, cr.Imag());
                }
            }
            return r;
        }

        /** @internal
         * @brief Complex multiplication of @p x and @p y, returning the result in @p x.
         */
        template<typename Cx, TargetTraits Traits, typename Tp, typename Ap>
        [[gnu::always_inline]]
        constexpr void Mul(BasicVector<Tp, Ap>& x, const BasicVector<Tp, Ap>& y) {
            static_assert(ComplexLike<Cx>);
            if constexpr (ScalarAbiTag<Ap> && Ap::kStorageSize == 2) {
                const Cx c = Cx(x[0], x[1]) * Cx(y[0], y[1]);
                x.GetLow() = c.Real();
                x.GetHigh() = c.Imag();
            } else if constexpr (Ap::kNreg >= 2) { // recurse
                Mul<Cx, Traits>(x.GetLow(), y.GetLow());
                Mul<Cx, Traits>(x.GetHigh(), y.GetHigh());
            } else if constexpr (Traits.template EvalAsF32<Tp>()) { // eval float16_t as float
                using Vf32 = Rebind<float, BasicVector<Tp, Ap>>;
                Vf32 xf32(x);
                Mul<Cx, Traits>(xf32, Vf32(y));
                x = static_cast<BasicVector<Tp, Ap>>(xf32);
            } else {
                using DataType = typename Ap::template DataType<Tp>;
                const DataType xv = x.Get();
                const DataType yv = y.Get();
                using VO = VecOps<DataType>;                    // don't care for actual numer of elements
                using VOS = VecOps<DataType, Ap::kStorageSize>; // to check for const-prop values
                if (VOS::ComplexImagIsConstKnownZero(xv)) {
                    if (VOS::ComplexImagIsConstKnownZero(yv)) {
                        x = xv * yv;
                    } else {
                        if (Traits.ConformingToStdcAnnexG()) { // handle negative zero (0 * y can be -0)
                            auto a = VO::DupEven(xv) * yv;
                            auto b = DataType() * VO::SwapNeighbors(yv);
                            x = VO::Addsub(a, b);
                        } else {
                            x = VO::DupEven(xv) * yv;
                        }
                    }
                } else if (VOS::ComplexImagIsConstKnownZero(yv)) {
                    if (Traits.ConformingToStdcAnnexG()) {
                        x = VO::Addsub(VO::DupEven(yv) * xv, DataType() * VO::SwapNeighbors(xv));
                    } else {
                        x = VO::DupEven(yv) * xv;
                    }
                } else if (VOS::ComplexRealIsConstKnownZero(yv)) {
                    if (Traits.ConformingToStdcAnnexG()) {
                        x = VO::Addsub(DataType(), VO::DupOdd(yv) * VO::SwapNeighbors(xv));
                    } else {
                        x = VO::DupOdd(yv) * VO::ComplexNegateReal(VO::SwapNeighbors(xv));
                    }
                } else if (VOS::ComplexRealIsConstKnownZero(xv)) {
                    if (Traits.ConformingToStdcAnnexG()) {
                        x = VO::Addsub(DataType(), VO::DupOdd(xv) * VO::SwapNeighbors(yv));
                    } else {
                        x = VO::DupOdd(xv) * VO::ComplexNegateReal(VO::SwapNeighbors(yv));
                    }
                } else {
#if SORA_SIMD_X86
                    if (Traits.HaveFma() && !IsConstKnown(xv, yv)) {
                        if constexpr (Traits.HaveFma()) {
                            x = X86ComplexMultiplies(xv, yv);
                        }
                    } else
#endif
                        x = VO::Addsub(VO::DupEven(xv) * yv, VO::DupOdd(xv) * VO::SwapNeighbors(yv));
                    const auto nan = x.Isnan();
                    if (Traits.ConformingToStdcAnnexG() && nan.AnyOf()) {
                        x = RedoMul<Cx, Traits>(x.Get(), xv, yv, nan, Ap::kStorageSize);
                    }
                }
            }
        }
    } // namespace Cxileav

    template<std::size_t Bytes, AbiTag Ap>
        requires Ap::kIsCxIleav && (Ap::kStorageSize >= 2) // size 1 is in simd_mask.h
    class BasicMask<Bytes, Ap> : public MaskBase<Bytes, Ap> {
        using Base = MaskBase<Bytes, Ap>;

        using VecType = Base::VecType;

        template<std::size_t, typename>
        friend class BasicMask;

        template<typename, typename>
        friend class BasicVector;

        static constexpr int kStorageSize = Ap::kStorageSize;

        using DataType = ComponentMaskForIleav<Bytes, Ap>;

        static constexpr bool kIsScalar = DataType::kIsScalar;

        static constexpr bool kUseBitmask = DataType::kUseBitmask;

        static constexpr int kFullSize = DataType::kFullSize / 2;

        static constexpr bool kIsPartial = DataType::kIsPartial;

        static constexpr bool kHasBoolMember = DataType::kHasBoolMember;

        static constexpr std::size_t kPaddingBytes = DataType::kPaddingBytes;

        DataType data;

    public:
        using ValueType = bool;

        using AbiType = Ap;

        using IteratorType = Base::IteratorType;

        using ConstIteratorType = Base::ConstIteratorType;

        // internal but public API ----------------------------------------------
        [[gnu::always_inline]]
        static constexpr BasicMask Init(const DataType& x) {
            BasicMask r;
            r.data = x;
            return r;
        }

        [[gnu::always_inline]]
        constexpr auto ConcatData() const {
            return data.ConcatData();
        }

        [[gnu::always_inline]]
        constexpr const DataType& GetIleavData() const {
            return data;
        }

        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] static constexpr BasicMask PartialMaskOfN(int n) {
            return Init(DataType::PartialMaskOfN(n * 2));
        }

        [[gnu::always_inline]]
        static constexpr BasicMask AndNeighbors(DataType k) {
            return Init(k.AndNeighbors());
        }

        [[gnu::always_inline]]
        static constexpr BasicMask OrNeighbors(DataType k) {
            return Init(k.OrNeighbors());
        }

        template<typename Mp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            if constexpr (Mp::AbiType::kVariant != Ap::kVariant) {
                using M2 = Resize<kStorageSize, Mp>;
                static_assert(!std::is_same_v<M2, BasicMask>);
                return static_cast<M2>(*this).template ChunkStorage<Mp>();
            } else if constexpr (Mp::kStorageSize == 1) {
                const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                return std::array{Mp(data[indices])...};
            } else // Mp is the same partial specialization
            {
                constexpr int rem = kStorageSize % Mp::kStorageSize;
                const auto [... xs] = data.template ChunkStorage<typename Mp::DataType>();
                static_assert(std::is_same_v<decltype(ToCxIleav(xs...[0])), Mp>);
                if constexpr (rem == 0) {
                    return std::array{ToCxIleav(xs)...};
                } else {
                    return std::tuple(ToCxIleav(xs)...);
                }
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
            return BasicMask::Init(DataType::Concat(xs.GetIleavData()...));
        }

        // [simd.Mask.overview] default constructor -----------------------------
        BasicMask() = default;

        // [simd.Mask.overview] conversion extensions ---------------------------
        template<VecBuiltin TV>
        [[gnu::always_inline]]
        constexpr BasicMask(const TV& x)
            requires std::convertible_to<TV, DataType>
            : data(x) {}

        template<VecBuiltin TV>
        [[gnu::always_inline]]
        constexpr operator TV()
            requires std::convertible_to<DataType, TV>
        {
            return data;
        }

        // [simd.Mask.ctor] broadcast constructor -------------------------------
        [[gnu::always_inline]]
        constexpr explicit BasicMask(std::same_as<bool> auto x) noexcept // LWG 4382.
            : data(x) {}

        // [simd.Mask.ctor] conversion constructor ------------------------------
        template<std::size_t UBytes, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize)
        [[gnu::always_inline]]
        constexpr explicit(IsMaskConversionExplicit<Ap, UAbi>(Bytes, UBytes))
            BasicMask(const BasicMask<UBytes, UAbi>& x) noexcept
            : data([&] {
                  using UV = BasicMask<UBytes, UAbi>;
                  if constexpr (UAbi::kIsCxIleav) {
                      // CxIleav -> CxIleav => we can simply convert the contained Mask
                      return x.data; // calls conversion ctor on DataType
                  }

                  // x is not CxIleav from here on
                  else if constexpr (kUseBitmask || UV::kUseBitmask) {
                      return DataType::Init(DuplicateEachBit<kStorageSize>(x.ToUint()));
                  }

                  // Vector-Mask to Vector-Mask from here on
                  else if constexpr (UAbi::kIsCxCtgus) {
                      // unwrap CxCtgus Mask and recurse
                      return BasicMask(x.data).data;
                  }

                  else if constexpr (UV::kIsScalar || kIsScalar) {
                      // need to duplicate & convert one vector element into two bools
                      return DataType([&](int i) { return x[i / 2]; }); // TODO: optimize
                  }

                  else if constexpr (Bytes == UBytes) {
                      return DataType::RecursiveBitCast(x);
                  } else if constexpr (Bytes <= sizeof(0ll)) {
                      using U2 = SimilarMask<Detail::IntegerForSize<Bytes>, kStorageSize, UAbi>;
                      return DataType::RecursiveBitCast(U2(x));
                  } else if constexpr (UBytes > 1) {
                      using U2 = SimilarMask<Detail::IntegerForSize<UBytes / 2>, kStorageSize * 2, UAbi>;
                      return U2::RecursiveBitCast(x); // calls conversion ctor on DataType
                  } else {                            // Bytes == 16 && UBytes == 1
                      // convert twice (1 -> 2 -> 16)
                      // The conversion to short keeps the intermediate Mask as small as possible and thus
                      // requires fewer across-128bit boundary shuffles.
                      return BasicMask(SimilarMask<short, UV::kStorageSize, UAbi>(x)).data;
                  }
              }()) {}

        using Base::MaskBase;

        // [simd.Mask.ctor] generator constructor -------------------------------
        template<SimdGeneratorInvokable<bool, kStorageSize> Fp>
        [[gnu::always_inline]] constexpr explicit BasicMask(Fp&& gen)
            : data([&] [[gnu::always_inline]] {
                  // for CxIleav, the results of each gen call need to initialize two
                  // neighboring elements
                  const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                  bool tmp[kStorageSize] = {gen(kSimdSizeC<indices>)...};
                  return DataType([&] [[gnu::always_inline]] (std::size_t i) { return tmp[i / 2]; });
              }()) {}

        // [simd.Mask.ctor] std::bitset constructor ----------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask(const std::same_as<std::bitset<kStorageSize>> auto& b) noexcept // LWG 4382.
            : data(DataType::Init(DuplicateEachBit<kStorageSize>(Sora::Math::Simd::BitsetToPairs(b)))) {}

        // [simd.Mask.ctor] uint constructor ------------------------------------
        template<std::unsigned_integral Tp>
            requires(!std::same_as<Tp, bool>) // LWG 4382.
        [[gnu::always_inline]]
        constexpr explicit BasicMask(Tp val) noexcept
            : data(DuplicateEachBit<kStorageSize>(val)) {}

        // [simd.Mask.subscr] ---------------------------------------------------
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            return data[i * 2];
        }

        // [simd.Mask.unary] ----------------------------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask operator!() const noexcept {
            return Init(!data);
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
            if constexpr (kUseBitmask) {
                return SelectImpl(*this, Ip(-1), Ip());
            } else {
                return __builtin_bit_cast(VecType, -data);
            }
        }

        constexpr VecType operator-() const noexcept = delete;

        [[gnu::always_inline]]
        constexpr VecType operator~() const noexcept
            requires std::destructible<VecType>
        {
            using Ip = typename VecType::ValueType;
            if constexpr (kUseBitmask) {
                return SelectImpl(*this, Ip(-2), Ip(-1));
            } else {
                return __builtin_bit_cast(VecType, data) - Ip(1);
            }
        }

        constexpr VecType operator~() const noexcept = delete;

        // [simd.Mask.conv] -----------------------------------------------------
        template<typename Up, typename UAbi>
            requires(UAbi::kStorageSize == kStorageSize)
        [[gnu::always_inline]]
        constexpr explicit(sizeof(Up) != Bytes) operator BasicVector<Up, UAbi>() const noexcept {
            using Mp = typename BasicVector<Up, UAbi>::MaskType;
            return SelectImpl(Mp(*this), BasicVector<Up, UAbi>(1), BasicVector<Up, UAbi>(0));
        }

        // [simd.Mask.namedconv] ------------------------------------------------
        [[gnu::always_inline]]
        constexpr std::bitset<kStorageSize> ToBitset() const noexcept {
            return Sora::Math::Simd::UnwrapPairsToBitset<kStorageSize>(ToUint());
        }

        template<int Offset = 0, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr auto ToUint() const {
            return data.template ToUint<Offset, true>();
        }

        [[gnu::always_inline]]
        constexpr unsigned long long ToUllong() const {
            return Sora::Math::Simd::UnwrapPairsToUllong(ToUint());
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
            return Init(x.data == y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator!=(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data != y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>=(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data >= y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<=(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data <= y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data > y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data < y.data);
        }

        // [simd.Mask.cond] -----------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, const BasicMask& t, const BasicMask& f) noexcept {
            return Init(SelectImpl(k.data, t.data, f.data));
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, std::same_as<bool> auto t,
                                              std::same_as<bool> auto f) noexcept {
            return Init(SelectImpl(k.data, t, f));
        }

        template<Vectorizable T0, std::same_as<T0> T1>
            requires(sizeof(T0) == Bytes)
        [[gnu::always_inline]]
        friend constexpr Vector<T0, kStorageSize> SelectImpl(const BasicMask& k, const T0& t, const T1& f) noexcept {
            using Vp = Vector<T0, kStorageSize>;
            return SelectImpl(static_cast<typename Vp::MaskType>(k), Vp(t), Vp(f));
        }

        // [simd.Mask.reductions] implementation --------------------------------
        [[gnu::always_inline]]
        constexpr bool AllOf() const noexcept {
            return data.AllOf();
        }

        [[gnu::always_inline]]
        constexpr bool AnyOf() const noexcept {
            return data.AnyOf();
        }

        [[gnu::always_inline]]
        constexpr bool NoneOf() const noexcept {
            return data.NoneOf();
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceCount() const noexcept {
            return data.ReduceCount() / 2;
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMinIndex() const {
            return data.ReduceMinIndex() / 2;
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMaxIndex() const {
            return data.ReduceMaxIndex() / 2;
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicMask& x) {
            return IsConstKnown(x.data);
        }
    };

    template<Vectorizable Tp, AbiTag Ap>
        requires ComplexLike<Tp> && Ap::kIsCxIleav && (Ap::kStorageSize >= 2) // size 1 is below
    class BasicVector<Tp, Ap> : public VecBase<Tp, Ap> {
        template<typename, typename>
        friend class BasicVector;

        static constexpr int kStorageSize = Ap::kStorageSize;

        static constexpr int kFullSize = std::bit_ceil(unsigned(kStorageSize));

        using T0 = typename Tp::value_type;

        using TSimd = SimilarVec<T0, 2 * kStorageSize, Ap>;

        using RealSimd = SimilarVec<T0, kStorageSize, Ap>;

        TSimd data = {};

        static constexpr bool kUseBitmask = TSimd::kUseBitmask;

        static constexpr bool kIsPartial = sizeof(data) > sizeof(Tp) * kStorageSize;

        [[gnu::always_inline]]
        static constexpr BasicVector Init(const TSimd& x) {
            BasicVector r;
            r.data = x;
            return r;
        }

    public:
        using ValueType = Tp;

        using MaskType = VecBase<Tp, Ap>::MaskType;

        // internal but public API ----------------------------------------------
        [[gnu::always_inline]]
        constexpr const TSimd& GetIleavData() const {
            return data;
        }

        [[gnu::always_inline]]
        constexpr const auto& GetLow() const
            requires(Ap::kNreg >= 2)
        {
            return data.GetLow();
        }

        [[gnu::always_inline]]
        constexpr const auto& GetHigh() const
            requires(Ap::kNreg >= 2)
        {
            return data.GetHigh();
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicVector& x) {
            return IsConstKnown(x.data);
        }

        template<typename Vp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            if constexpr (Vp::AbiType::kIsCxIleav) {
                constexpr int n = kStorageSize / Vp::kStorageSize;
                constexpr int rem = kStorageSize % Vp::kStorageSize;
                const auto chunked = data.template ChunkStorage<Resize<Vp::kStorageSize * 2, TSimd>>();
                const auto& [... indices] = Detail::kIotaArray<n>;
                if constexpr (rem == 0) {
                    return std::array<Vp, n>{Vp::Init(chunked[indices])...};
                } else {
                    using Rest = Resize<rem, Vp>;
                    return std::tuple(Vp::Init(Get<indices>(chunked))..., Rest::Init(Get<n>(chunked)));
                }
            } else {
                return Resize<kStorageSize, Vp>(*this).template ChunkStorage<Vp>();
            }
        }

        [[gnu::always_inline]]
        static constexpr const BasicVector& Concat(const BasicVector& x0) noexcept {
            return x0;
        }

        template<typename... As>
            requires(sizeof...(As) > 1)
        [[gnu::always_inline]]
        static constexpr BasicVector Concat(const BasicVector<ValueType, As>&... xs) noexcept {
            return BasicVector::Init(TSimd::Concat(xs.GetIleavData()...));
        }

        template<typename BinaryOp>
        [[gnu::always_inline]]
        constexpr auto ReduceToRegister(BinaryOp binaryOp) const {
            if constexpr (TSimd::AbiType::kNreg == 1) {
                return *this;
            } else {
                auto [lo, hi] = ChunkStorage<Resize<std::bit_ceil(unsigned(kStorageSize)) / 2, BasicVector>>();
                auto a = lo.ReduceToRegister(binaryOp);
                auto b = hi.ReduceToRegister(binaryOp);
                if constexpr (a.kStorageSize == b.kStorageSize) {
                    return binaryOp(a, b);
                } else {
                    using V1 = Resize<1, BasicVector>;
                    return binaryOp(V1(a.Reduce(binaryOp)), V1(b.Reduce(binaryOp)));
                }
            }
        }

        template<typename BinaryOp, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr ValueType Reduce(BinaryOp binaryOp) const {
            if constexpr (kStorageSize == 1) {
                return operator[](0);
            } else if constexpr (Traits.template EvalAsF32<T0>()) {
                return ValueType(Rebind<std::complex<float>, BasicVector>(*this).Reduce(binaryOp));
            } else if constexpr (TSimd::AbiType::kNreg >= 2) {
                return ReduceToRegister(binaryOp).Reduce(binaryOp);
            } else if constexpr (std::has_single_bit(unsigned(kStorageSize))) {
                const auto [a, b] = ChunkStorage<Resize<kStorageSize / 2, BasicVector>>();
                return binaryOp(a, b).Reduce(binaryOp);
            } else {
                const auto [a, b, c, ... rest] =
                    ChunkStorage<Resize<std::bit_floor(unsigned(kStorageSize)) / 2, BasicVector>>();
                const auto ab = binaryOp(a, b);
                static_assert(sizeof...(rest) <= 1);
                if constexpr (a.kStorageSize != c.kStorageSize) {
                    return Cat(ab, c).Reduce(binaryOp);
                } else {
                    return Cat(binaryOp(ab, c), rest...).Reduce(binaryOp);
                }
            }
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline BasicVector PartialLoad(const Up* mem, std::size_t n) {
            if constexpr (ComplexLike<Up>) {
                return Init(TSimd::PartialLoad(reinterpret_cast<const typename Up::value_type*>(mem), n * 2));
            } else {
                return BasicVector(RealSimd::PartialLoad(mem, n));
            }
        }

        template<typename Up, ArchTraits Traits = {}>
        static inline BasicVector MaskedLoad(const Up* mem, MaskType k) {
            if constexpr (ComplexLike<Up>) {
                return Init(TSimd::MaskedLoad(reinterpret_cast<const typename Up::value_type*>(mem), k.data));
            } else {
                return BasicVector(RealSimd::MaskedLoad(mem, typename RealSimd::MaskType(k)));
            }
        }

        template<typename Up>
        [[gnu::always_inline]]
        inline void Store(Up* mem) const {
            static_assert(ComplexLike<Up>);
            data.Store(reinterpret_cast<typename Up::value_type*>(mem));
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline void PartialStore(const BasicVector& v, Up* mem, std::size_t n) {
            static_assert(ComplexLike<Up>);
            TSimd::PartialStore(v.data, reinterpret_cast<typename Up::value_type*>(mem), n * 2);
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline void MaskedStore(const BasicVector& v, Up* mem, const MaskType& k) {
            static_assert(ComplexLike<Up>);
            TSimd::MaskedStore(v.data, reinterpret_cast<typename Up::value_type*>(mem), k.data);
        }

        BasicVector() = default;

        // TODO: conversion extensions

        // [simd.ctor] broadcast constructor ------------------------------------
        template<BroadcastConstructible<ValueType> Up>
        [[gnu::always_inline]]
        constexpr BasicVector(Up&& x) noexcept
            : data([&](int i) {
                  if constexpr (ComplexLike<Up>) {
                      return (i & 1) == 0 ? x.Real() : x.Imag();
                  } else {
                      return (i & 1) == 0 ? x : T0();
                  }
              }) {}

        // [simd.ctor] conversion constructor -----------------------------------
        template<ComplexLike Up, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType> && UAbi::kIsCxIleav
        [[gnu::always_inline]]
        constexpr explicit(!std::convertible_to<Up, ValueType>) BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : data(x.data) {}

        template<ComplexLike Up, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType> &&
                    (!UAbi::kIsCxIleav)
        [[gnu::always_inline]]
        constexpr explicit(!std::convertible_to<Up, ValueType>) BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : BasicVector(static_cast<RealSimd>(x.realData), static_cast<RealSimd>(x.imagData)) {}

        template<typename Up, typename UAbi>
            requires(!ComplexLike<Up>) && (kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType>
        [[gnu::always_inline]]
        constexpr explicit(!std::convertible_to<Up, ValueType>) BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : BasicVector(RealSimd(x)) {}

        using VecBase<Tp, Ap>::VecBase;

        // [simd.ctor] generator constructor ------------------------------------
        template<SimdGeneratorInvokable<ValueType, kStorageSize> Fp>
        [[gnu::always_inline]]
        constexpr explicit BasicVector(Fp&& gen)
            : data([&] {
                  using Arr = std::array<ValueType, sizeof(TSimd) / sizeof(ValueType)>;
                  const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                  const Arr tmp = {static_cast<ValueType>(gen(kSimdSizeC<indices>))...};
                  return __builtin_bit_cast(TSimd, tmp);
              }()) {}

        // [simd.ctor] load constructor -----------------------------------------
        template<ComplexLike Up>
        [[gnu::always_inline]]
        constexpr BasicVector(LoadCtorTag, const Up* ptr)
            : data([&] {
                  if consteval {
                      return TSimd([&](int i) {
                          const Up& cx = ptr[i / 2];
                          return static_cast<T0>(i % 2 == 0 ? cx.Real() : cx.Imag());
                      });
                  } else {
                      return TSimd(LoadCtorTag(), reinterpret_cast<const typename Up::value_type*>(ptr));
                  }
              }()) {}

        template<typename Up>
        [[gnu::always_inline]]
        constexpr BasicVector(LoadCtorTag, const Up* ptr)
            : BasicVector(RealSimd(LoadCtorTag(), ptr)) {}

        template<std::ranges::contiguous_range Rg, typename... FlagTypes>
            requires Detail::StaticSizedRange<Rg> && (Detail::StaticSize<Rg>() == kStorageSize) &&
                     Vectorizable<std::ranges::range_value_t<Rg>> &&
                     ExplicitlyConvertibleTo<std::ranges::range_value_t<Rg>, ValueType>
        [[gnu::always_inline]]
        constexpr BasicVector(Rg&& range, Flags<FlagTypes...> flags = {})
            : BasicVector(LoadCtorTag(), flags.template AdjustPointer<BasicVector>(std::ranges::data(range))) {
            static_assert(LoadstoreConvertibleTo<std::ranges::range_value_t<Rg>, ValueType, FlagTypes...>);
        }

        // [simd.ctor] std::complex Init ---------------------------------------------
        // This uses RealSimd as proposed in LWG4230
        [[gnu::always_inline]]
        constexpr BasicVector(const RealSimd& re, const RealSimd& im = {}) noexcept {
            Cxileav::SetReal(data, re);
            Cxileav::SetImag(data, im);
        }

        // [simd.subscr] --------------------------------------------------------
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            return ValueType(data[i * 2], data[i * 2 + 1]);
        }

        // [simd.unary] unary operators -----------------------------------------
        [[gnu::always_inline]]
        constexpr BasicVector& operator++() noexcept
            requires requires(ValueType a) { ++a; }
        {
            data += ValueType(T0(1));
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator++(int) noexcept
            requires requires(ValueType a) { a++; }
        {
            BasicVector r = *this;
            data += ValueType(T0(1));
            return r;
        }

        [[gnu::always_inline]]
        constexpr BasicVector& operator--() noexcept
            requires requires(ValueType a) { --a; }
        {
            data -= ValueType(T0(1));
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator--(int) noexcept
            requires requires(ValueType a) { a--; }
        {
            BasicVector r = *this;
            data -= ValueType(T0(1));
            return r;
        }

        [[gnu::always_inline]]
        constexpr MaskType operator!() const noexcept
            requires requires(ValueType a) { !a; }
        {
            return Init(!data);
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
            BasicVector r = *this;
            r.data = -data;
            return r;
        }

        // [simd.cassign] compound assignment -----------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator+=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a + a; }
        {
            x.data += y.data;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator-=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a - a; }
        {
            x.data -= y.data;
            return x;
        }

        template<TargetTraits Traits = {}>
        [[gnu::always_inline]] friend constexpr BasicVector& operator*=(BasicVector & x, const BasicVector & y) noexcept
            requires requires(ValueType a) { a * a; }
        {
            Cxileav::Mul<ValueType, Traits>(x.data, y.data);
            return x;
        }

        template<int RemoveMe = 0>
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator/=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a / a; }
        {
            static_assert(false, "TODO");
        }

        // [simd.comparison] compare operators ----------------------------------
        [[gnu::always_inline]]
        friend constexpr MaskType operator==(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::AndNeighbors(x.data == y.data);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator!=(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType::OrNeighbors(x.data != y.data);
        }

        // [simd.std::complex.access] std::complex-value accessors ------------------------
        // LWG4230: returns RealSimd instead of auto
        [[gnu::always_inline]]
        constexpr RealSimd Real() const noexcept {
            return Permute<kStorageSize>(data, [](int i) { return i * 2; });
        }

        [[gnu::always_inline]]
        constexpr RealSimd Imag() const noexcept {
            return Permute<kStorageSize>(data, [](int i) { return i * 2 + 1; });
        }

        [[gnu::always_inline]]
        constexpr void Real(const RealSimd& x) noexcept {
            Cxileav::SetReal(data, x);
        }

        [[gnu::always_inline]]
        constexpr void Imag(const RealSimd& x) noexcept {
            Cxileav::SetImag(data, x);
        }

        // [simd.cond] ---------------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicVector SelectImpl(const MaskType& k, const BasicVector& t,
                                                const BasicVector& f) noexcept {
            return Init(SelectImpl(k.data, t.data, f.data));
        }

        // [simd.std::complex.math] internals ---------------------------------------
        [[gnu::always_inline]]
        constexpr RealSimd Abs() const; // TODO: depends on [simd.math]

        // associated functions
        [[gnu::always_inline]]
        constexpr RealSimd Norm() const {
            auto re = Real();
            auto im = Imag();
            return re * re + im * im;
        }

        [[gnu::always_inline]]
        constexpr BasicVector Conj() const {
            return Init(Cxileav::NegateImag(data));
        }
    };

    // std::complex contiguous (CxCtgus) --------------------------------------------
    // (and CxIleav BasicVector with size 1)

    /** @internal
     * @brief Functions acting on / recursing into the non-std::complex fp Vector objects, where Real and
     * imaginary parts are stored in separate Vector objects.
     */
    namespace Cxctgus {
        /** @internal
         * @brief Recompute all std::complex multiplications where @p nan is true using @p Cx's
         * multiplication operator.
         *
         * @todo use coarser TargetTraits and std::move into .so
         */
        template<typename Cx, TargetTraits, VecBuiltin TV, typename Kp>
        [[__gnu__::__cold__, __gnu__::__noinline__]]
        constexpr void RedoMul(TV& re, TV& im, const TV re0, const TV im0, const TV re1, const TV im1, const Kp nan,
                               int n) {
            for (int i = 0; i < n; ++i) {
                bool isNanValue;
                if constexpr (std::is_integral_v<Kp>) {
                    isNanValue = (nan & (Kp(1) << i)) != 0;
                } else {
                    isNanValue = nan[i] != 0;
                }
                if (isNanValue) {
                    const Cx c0(re0[i], im0[i]);
                    const Cx c1(re1[i], im1[i]);
                    const Cx cr = c0 * c1;
                    VecSet(re, i, cr.Real());
                    VecSet(im, i, cr.Imag());
                }
            }
        }

        /** @internal
         * @brief Complex multiplication of (@p re0, @p im0) and (@p re1, @p im1), returning the
         * result in @p re0 and @p im0.
         */
        template<typename Cx, TargetTraits Traits, typename Tp, typename Ap>
        [[gnu::always_inline]]
        constexpr void Mul(BasicVector<Tp, Ap>& re0, BasicVector<Tp, Ap>& im0, const BasicVector<Tp, Ap>& re1,
                           const BasicVector<Tp, Ap>& im1) {
            static_assert(ComplexLike<Cx>);
            if constexpr (Ap::kNreg >= 2) {
                Mul<Cx, Traits>(re0.GetLow(), im0.GetLow(), re1.GetLow(), im1.GetLow());
                Mul<Cx, Traits>(re0.GetHigh(), im0.GetHigh(), re1.GetHigh(), im1.GetHigh());
            } else if constexpr (Ap::kStorageSize == 1) { // use Cx::operator*
                const Cx c0(re0.Get(), im0.Get());
                const Cx c1(re1.Get(), im1.Get());
                const Cx cr = c0 * c1;
                re0.Get() = cr.Real();
                im0.Get() = cr.Imag();
            } else if constexpr (Traits.template EvalAsF32<Tp>()) {
                using Vf = Rebind<float, BasicVector<Tp, Ap>>;
                using Cf = std::complex<float>;
                Vf re0f = re0;
                Vf im0f = im0;
                Mul<Cf, Traits, float, typename Vf::AbiType>(re0f, im0f, re1, im1);
                re0 = static_cast<BasicVector<Tp, Ap>>(re0f);
                im0 = static_cast<BasicVector<Tp, Ap>>(im0f);
            } else {
                BasicVector<Tp, Ap> re = re0 * re1 - im0 * im1;
                BasicVector<Tp, Ap> im = re0 * im1 + im0 * re1;
                const auto nan = re.Isunordered(im);
                if (nan.AnyOf()) [[unlikely]] {
                    RedoMul<Cx, Traits>(re.Get(), im.Get(), re0.Get(), im0.Get(), re1.Get(), im1.Get(),
                                        nan.ConcatData(), Ap::kStorageSize);
                }
                re0 = re;
                im0 = im;
            }
        }
    } // namespace Cxctgus

    template<std::size_t Bytes, AbiTag Ap>
        requires Ap::kIsCxCtgus && (Ap::kStorageSize >= 2) // size 1 is in simd_mask.h
    class BasicMask<Bytes, Ap> : public MaskBase<Bytes, Ap> {
        using Base = MaskBase<Bytes, Ap>;

        using VecType = Base::VecType;

        template<std::size_t, typename>
        friend class BasicMask;

        template<typename, typename>
        friend class BasicVector;

        static constexpr int kStorageSize = Ap::kStorageSize;

        using DataType = ComponentMaskForCtgus<Bytes, Ap>;

        static_assert(DataType::AbiType::kNreg == Ap::kNreg);

        static constexpr bool kIsScalar = DataType::kIsScalar;

        static constexpr bool kUseBitmask = DataType::kUseBitmask;

        static constexpr int kFullSize = DataType::kFullSize;

        static constexpr bool kIsPartial = DataType::kIsPartial;

        static constexpr bool kHasBoolMember = DataType::kHasBoolMember;

        static constexpr std::size_t kPaddingBytes = DataType::kPaddingBytes;

        DataType data;

    public:
        using ValueType = bool;

        using AbiType = Ap;

        // internal but public API ----------------------------------------------
        [[gnu::always_inline]]
        static constexpr BasicMask Init(const DataType& x) {
            BasicMask r;
            r.data = x;
            return r;
        }

        [[gnu::always_inline]]
        constexpr const DataType& Get() const {
            return data;
        }

        [[gnu::always_inline]]
        constexpr auto ConcatData() const {
            return data.ConcatData();
        }

        template<ArchTraits Traits = {}>
        [[gnu::always_inline]] static constexpr BasicMask PartialMaskOfN(int n) {
            return Init(DataType::PartialMaskOfN(n));
        }

        template<typename Mp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            if constexpr (Mp::AbiType::kVariant != Ap::kVariant) {
                using M2 = Resize<kStorageSize, Mp>;
                static_assert(!std::is_same_v<M2, BasicMask>);
                return static_cast<M2>(*this).template ChunkStorage<Mp>();
            } else if constexpr (Mp::kStorageSize == 1) {
                const auto& [... indices] = Detail::kIotaArray<kStorageSize>;
                return std::array{Mp(data[indices])...};
            } else // Mp is the same partial specialization
            {
                constexpr int rem = kStorageSize % Mp::kStorageSize;
                const auto [... xs, last] = data.template ChunkStorage<typename Mp::DataType>();
                if constexpr (rem == 0) {
                    return std::array{Mp::Init(xs)..., Mp::Init(last)};
                } else {
                    return std::tuple(Mp::Init(xs)..., Resize<rem, Mp>(last));
                }
            }
        }

        [[gnu::always_inline]]
        static constexpr const BasicMask& Concat(const BasicMask& x0) noexcept {
            return x0;
        }

        /** @internal
         * @brief Adjust the Mask type to match RealSimd.
         *
         * This is a trivial unwrap for this partial specialization of BasicMask. However, for
         * Abi<1, 1, CxCtgus> data is the bool object and needs to be converted.
         */
        [[gnu::always_inline]]
        constexpr const DataType& GetCtgusData() const noexcept {
            return data;
        }

        template<typename... As>
            requires(sizeof...(As) > 1)
        [[gnu::always_inline]]
        static constexpr BasicMask Concat(const BasicMask<Bytes, As>&... xs) noexcept {
            return BasicMask::Init(DataType::Concat(xs.GetCtgusData()...));
        }

        // [simd.Mask.overview] default constructor -----------------------------
        BasicMask() = default;

        // [simd.Mask.overview] conversion extensions ---------------------------
        template<VecBuiltin TV>
        [[gnu::always_inline]]
        constexpr BasicMask(const TV& x)
            requires std::convertible_to<TV, DataType>
            : data(x) {}

        template<VecBuiltin TV>
        [[gnu::always_inline]]
        constexpr operator TV()
            requires std::convertible_to<DataType, TV>
        {
            return data;
        }

        // [simd.Mask.ctor] broadcast constructor -------------------------------
        [[gnu::always_inline]]
        constexpr explicit BasicMask(std::same_as<bool> auto x) noexcept // LWG 4382.
            : data(x) {}

        // [simd.Mask.ctor] conversion constructor ------------------------------
        template<std::size_t UBytes, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize)
        [[gnu::always_inline]]
        constexpr explicit(IsMaskConversionExplicit<Ap, UAbi>(Bytes, UBytes))
            BasicMask(const BasicMask<UBytes, UAbi>& x) noexcept
            : data(x) {}

        using Base::MaskBase;

        // [simd.Mask.ctor] generator constructor -------------------------------
        template<SimdGeneratorInvokable<bool, kStorageSize> Fp>
        [[gnu::always_inline]]
        constexpr explicit BasicMask(Fp&& gen)
            : data(gen) {}

        // [simd.Mask.ctor] std::bitset constructor ----------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask(const std::same_as<std::bitset<kStorageSize>> auto& b) noexcept // LWG 4382.
            : data(b) {}

        // [simd.Mask.ctor] uint constructor ------------------------------------
        template<std::unsigned_integral Tp>
            requires(!std::same_as<Tp, bool>) // LWG 4382.
        [[gnu::always_inline]]
        constexpr explicit BasicMask(Tp val) noexcept
            : data(val) {}

        // [simd.Mask.subscr] ---------------------------------------------------
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            return data[i];
        }

        // [simd.Mask.unary] ----------------------------------------------------
        [[gnu::always_inline]]
        constexpr BasicMask operator!() const noexcept {
            return Init(!data);
        }

        [[gnu::always_inline]]
        constexpr VecType operator+() const noexcept
            requires std::destructible<VecType>
        {
            return static_cast<VecType>(data);
        }

        constexpr VecType operator+() const noexcept = delete;

        [[gnu::always_inline]]
        constexpr VecType operator-() const noexcept
            requires std::destructible<VecType>
        {
            using Ip = typename VecType::ValueType;
            if constexpr (kUseBitmask) {
                return SelectImpl(*this, Ip(-1), Ip());
            } else {
                return -data; // sign-extends
            }
        }

        constexpr VecType operator-() const noexcept = delete;

        [[gnu::always_inline]]
        constexpr VecType operator~() const noexcept
            requires std::destructible<VecType>
        {
            using Ip = typename VecType::ValueType;
            if constexpr (kUseBitmask) {
                return SelectImpl(*this, Ip(-2), Ip(-1));
            } else {
                return ~data; // sign-extends
            }
        }

        constexpr VecType operator~() const noexcept = delete;

        // [simd.Mask.conv] -----------------------------------------------------
        template<typename Up, typename UAbi>
            requires(UAbi::kStorageSize == kStorageSize)
        [[gnu::always_inline]]
        constexpr explicit(sizeof(Up) != Bytes) operator BasicVector<Up, UAbi>() const noexcept {
            using UV = BasicVector<Up, UAbi>;
            using Mp = typename UV::MaskType;
            return SelectImpl(static_cast<Mp>(data), UV(1), UV(0));
        }

        // [simd.Mask.namedconv] ------------------------------------------------
        [[gnu::always_inline]]
        constexpr std::bitset<kStorageSize> ToBitset() const noexcept {
            return data.ToBitset();
        }

        template<int Offset = 0, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr auto ToUint() const {
            return data.template ToUint<Offset>();
        }

        [[gnu::always_inline]]
        constexpr unsigned long long ToUllong() const {
            return data.ToUllong();
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
            return Init(x.data == y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator!=(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data != y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>=(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data >= y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<=(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data <= y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator>(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data > y.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask operator<(const BasicMask& x, const BasicMask& y) noexcept {
            return Init(x.data < y.data);
        }

        // [simd.Mask.cond] -----------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, const BasicMask& t, const BasicMask& f) noexcept {
            return SelectImpl(k.data, t.data, f.data);
        }

        [[gnu::always_inline]]
        friend constexpr BasicMask SelectImpl(const BasicMask& k, std::same_as<bool> auto t,
                                              std::same_as<bool> auto f) noexcept {
            return Init(SelectImpl(k.data, t, f));
        }

        template<Vectorizable T0, std::same_as<T0> T1>
            requires(sizeof(T0) == Bytes)
        [[gnu::always_inline]]
        friend constexpr Vector<T0, kStorageSize> SelectImpl(const BasicMask& k, const T0& t, const T1& f) noexcept {
            using Vp = Vector<T0, kStorageSize>;
            return SelectImpl(static_cast<typename Vp::MaskType>(k), Vp(t), Vp(f));
        }

        // [simd.Mask.reductions] implementation --------------------------------
        [[gnu::always_inline]]
        constexpr bool AllOf() const noexcept {
            return data.AllOf();
        }

        [[gnu::always_inline]]
        constexpr bool AnyOf() const noexcept {
            return data.AnyOf();
        }

        [[gnu::always_inline]]
        constexpr bool NoneOf() const noexcept {
            return data.NoneOf();
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceCount() const noexcept {
            return data.ReduceCount();
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMinIndex() const {
            return data.ReduceMinIndex();
        }

        [[gnu::always_inline]]
        constexpr SimdSizeType ReduceMaxIndex() const {
            return data.ReduceMaxIndex();
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicMask& x) {
            return IsConstKnown(x.data);
        }
    };

    template<Vectorizable Tp, AbiTag Ap>
        requires ComplexLike<Tp> && (Ap::kIsCxCtgus || Ap::kStorageSize == 1)
    class BasicVector<Tp, Ap> : public VecBase<Tp, Ap> {
        template<typename, typename>
        friend class BasicVector;

        static constexpr int kStorageSize = Ap::kStorageSize;

        static constexpr int kFullSize = std::bit_ceil(unsigned(kStorageSize));

        using T0 = typename Tp::value_type;

        using RealSimd = SimilarVec<T0, kStorageSize, Ap>;

        RealSimd realData = {};

        RealSimd imagData = {};

        static constexpr bool kIsScalar = RealSimd::kIsScalar;

        static constexpr bool kUseBitmask = RealSimd::kUseBitmask;

        static constexpr bool kIsPartial = RealSimd::kIsPartial;

    public:
        using ValueType = Tp;

        using MaskType = VecBase<Tp, Ap>::MaskType;

        // internal but public API ----------------------------------------------
        [[gnu::always_inline]]
        constexpr RealSimd& GetReal() noexcept {
            return realData;
        }

        [[gnu::always_inline]]
        constexpr const RealSimd& GetReal() const noexcept {
            return realData;
        }

        [[gnu::always_inline]]
        constexpr RealSimd& GetImag() noexcept {
            return imagData;
        }

        [[gnu::always_inline]]
        constexpr const RealSimd& GetImag() const noexcept {
            return imagData;
        }

        [[gnu::always_inline]]
        constexpr auto GetLow() const
            requires(Ap::kNreg >= 2)
        {
            return Resize<realData.N0, BasicVector>(realData.GetLow(), imagData.GetLow());
        }

        [[gnu::always_inline]]
        constexpr auto GetHigh() const
            requires(Ap::kNreg >= 2)
        {
            return Resize<realData.N1, BasicVector>(realData.GetHigh(), imagData.GetHigh());
        }

        [[gnu::always_inline]]
        constexpr auto ConcatData(bool /*do_sanitize*/ = false) const
            requires(kStorageSize == 1) // only for CxCtgus of size 1
        {
            return VecBuiltinType<CanonicalVecTypeT<T0>, 2>{realData.data, imagData.data};
        }

        [[gnu::always_inline]]
        constexpr auto GetIleavData() const
            requires(kStorageSize == 1 && Ap::kIsCxIleav)
        {
            return __builtin_bit_cast(SimilarVec<T0, 2, Ap>, *this);
        }

        [[gnu::always_inline]]
        static constexpr BasicVector Init(const SimilarVec<T0, 2, Ap>& x)
            requires(kStorageSize == 1 && Ap::kIsCxIleav)
        {
            return __builtin_bit_cast(BasicVector, x);
        }

        [[gnu::always_inline]]
        friend constexpr bool IsConstKnown(const BasicVector& x) {
            return IsConstKnown(x.realData) && IsConstKnown(x.imagData);
        }

        template<typename Vp>
        [[gnu::always_inline]]
        constexpr auto ChunkStorage() const noexcept {
            constexpr int n = kStorageSize / Vp::kStorageSize;
            constexpr int rem = kStorageSize % Vp::kStorageSize;
            const auto [... realChunks, lastReal] = realData.template ChunkStorage<typename Vp::RealSimd>();
            const auto [... indices, lastImag] = imagData.template ChunkStorage<typename Vp::RealSimd>();
            if constexpr (rem == 0) {
                return std::array<Vp, n>{Vp(realChunks, indices)..., Vp(lastReal, lastImag)};
            } else {
                return std::tuple(Vp(realChunks, indices)..., Resize<rem, Vp>(lastReal, lastImag));
            }
        }

        template<typename A0>
        [[gnu::always_inline]]
        static constexpr BasicVector Concat(const BasicVector<ValueType, A0>& x0) noexcept {
            return static_cast<BasicVector>(x0);
        }

        template<typename... As>
            requires(sizeof...(As) > 1)
        [[gnu::always_inline]]
        static constexpr BasicVector Concat(const BasicVector<ValueType, As>&... xs) noexcept {
            return {RealSimd::Concat(xs.realData...), RealSimd::Concat(xs.imagData...)};
        }

        template<typename BinaryOp>
        [[gnu::always_inline]]
        constexpr auto ReduceToRegister(BinaryOp binaryOp) const {
            if constexpr (RealSimd::AbiType::kNreg == 1) {
                return *this;
            } else {
                auto [lo, hi] = ChunkStorage<Resize<RealSimd::N0, BasicVector>>();
                auto a = lo.ReduceToRegister(binaryOp);
                auto b = hi.ReduceToRegister(binaryOp);
                if constexpr (a.kStorageSize == b.kStorageSize) {
                    return binaryOp(a, b);
                } else {
                    using V1 = Resize<1, BasicVector>;
                    return binaryOp(V1(a.Reduce(binaryOp)), V1(b.Reduce(binaryOp)));
                }
            }
        }

        template<typename BinaryOp, ArchTraits Traits = {}>
        [[gnu::always_inline]] constexpr ValueType Reduce(BinaryOp binaryOp) const {
            if constexpr (kStorageSize == 1) {
                return operator[](0);
            } else if constexpr (Traits.template EvalAsF32<T0>()) {
                return ValueType(Rebind<std::complex<float>, BasicVector>(*this).Reduce(binaryOp));
            } else if constexpr (RealSimd::AbiType::kNreg >= 2) {
                return ReduceToRegister(binaryOp).Reduce(binaryOp);
            } else if constexpr (std::has_single_bit(unsigned(kStorageSize))) {
                const auto [a, b] = ChunkStorage<Resize<kStorageSize / 2, BasicVector>>();
                return binaryOp(a, b).Reduce(binaryOp);
            } else {
                const auto [a, b, c, ... rest] =
                    ChunkStorage<Resize<std::bit_floor(unsigned(kStorageSize)) / 2, BasicVector>>();
                const auto ab = binaryOp(a, b);
                static_assert(sizeof...(rest) <= 1);
                if constexpr (a.kStorageSize != c.kStorageSize) {
                    return Cat(ab, c).Reduce(binaryOp);
                } else {
                    return Cat(binaryOp(ab, c), rest...).Reduce(binaryOp);
                }
            }
        }

        /** @internal
         * Implementation of @ref PartialLoad.
         *
         * If @p mem stores std::complex numbers, this needs to load @c abcdefgh from memory into two
         * BasicVector: @c aceg and @c bdfh.
         *
         * @param mem  A pointer to an std::array of @p n values. Can be std::complex or Real.
         * @param n    Read no more than @p n values from memory.
         *
         * @todo Optimize with deinterleaving loads or loads + deinterleaving fixup.
         */
        template<typename Up>
        [[gnu::always_inline]]
        static inline BasicVector PartialLoad(const Up* mem, std::size_t n) {
            if constexpr (ComplexLike<Up>) {
                return BasicVector(RealSimd([&](std::size_t i) -> T0 { return i < n ? mem[i].Real() : T0(); }),
                                   RealSimd([&](std::size_t i) -> T0 { return i < n ? mem[i].Imag() : T0(); }));
            } else {
                return BasicVector(RealSimd::PartialLoad(mem, n));
            }
        }

        /** @internal
         *
         * @todo Optimize with deinterleaving loads or loads + deinterleaving fixup.
         */
        template<typename Up, ArchTraits Traits = {}>
        static inline BasicVector MaskedLoad(const Up* mem, MaskType k) {
            if constexpr (ComplexLike<Up>) { // TODO: optimize
                return BasicVector(RealSimd([&](int i) { return k[i] ? mem[i].Real() : T0(); }),
                                   RealSimd([&](int i) { return k[i] ? mem[i].Imag() : T0(); }));
            } else {
                return BasicVector(RealSimd::MaskedLoad(mem, typename RealSimd::MaskType(k)));
            }
        }

        template<typename Up>
        [[gnu::always_inline]]
        inline void Store(Up* mem) const {
            static_assert(ComplexLike<Up>);
            for (int i = 0; i < kStorageSize; ++i) {
                mem[i].Real(realData[i]);
                mem[i].Imag(imagData[i]);
            }
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline void PartialStore(const BasicVector& v, Up* mem, std::size_t n) {
            static_assert(ComplexLike<Up>);
            for (std::size_t i = 0; i < std::min(n, std::size_t(kStorageSize)); ++i) {
                mem[i].Real(v.realData[i]);
                mem[i].Imag(v.imagData[i]);
            }
        }

        template<typename Up>
        [[gnu::always_inline]]
        static inline void MaskedStore(const BasicVector& v, Up* mem, const MaskType& k) {
            // TODO: optimize
            static_assert(ComplexLike<Up>);
            for (int i = 0; i < kStorageSize; ++i) {
                if (k[i]) {
                    mem[i] = v[i];
                }
            }
        }

        BasicVector() = default;

        // TODO: conversion extensions

        // [simd.ctor] broadcast constructor ------------------------------------
        template<BroadcastConstructible<ValueType> Up>
            requires ComplexLike<Up>
        [[gnu::always_inline]]
        constexpr BasicVector(Up&& x) noexcept
            : realData(x.Real()), imagData(x.Imag()) {}

        template<BroadcastConstructible<ValueType> Up>
        [[gnu::always_inline]]
        constexpr BasicVector(Up&& x) noexcept
            : realData(x), imagData() {}

        // [simd.ctor] conversion constructor -----------------------------------
        template<ComplexLike Up, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType> && UAbi::kIsCxIleav
        [[gnu::always_inline]]
        constexpr explicit(!std::convertible_to<Up, ValueType>) BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : realData(x.Real()), imagData(x.Imag()) {}

        template<ComplexLike Up, typename UAbi>
            requires(kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType> &&
                        (!UAbi::kIsCxIleav)
        [[gnu::always_inline]]
        constexpr explicit(!std::convertible_to<Up, ValueType>) BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : realData(x.realData),
              imagData(x.imagData) // using Real() instead of realData is possible
                                   // but potentially leads to memcpy because of oversized realData (likewise for Imag)
        {}

        template<typename Up, typename UAbi> // Up is not std::complex!
            requires(!ComplexLike<Up>) && (kStorageSize == UAbi::kStorageSize) && ExplicitlyConvertibleTo<Up, ValueType>
        [[gnu::always_inline]]
        constexpr explicit(!std::convertible_to<Up, ValueType>) BasicVector(const BasicVector<Up, UAbi>& x) noexcept
            : realData(x), imagData() {}

        using VecBase<Tp, Ap>::VecBase;

        // [simd.ctor] generator constructor ------------------------------------
        template<SimdGeneratorInvokable<ValueType, kStorageSize> Fp>
        [[gnu::always_inline]]
        constexpr explicit BasicVector(Fp&& gen)
            : realData(), imagData([&] {
                  T0 re[sizeof(RealSimd) / sizeof(T0)] = {};
                  T0 im[sizeof(RealSimd) / sizeof(T0)] = {};
                  template for (constexpr int i : Detail::kIotaArray<kStorageSize>) {
                      const ValueType c = static_cast<ValueType>(gen(kSimdSizeC<i>));
                      re[i] = c.Real();
                      im[i] = c.Imag();
                  }
                  realData = __builtin_bit_cast(RealSimd, re);
                  return __builtin_bit_cast(RealSimd, im);
              }()) {}

        // [simd.ctor] load constructor -----------------------------------------
        template<ComplexLike Up>
        [[gnu::always_inline]]
        constexpr BasicVector(LoadCtorTag, const Up* ptr)
            : realData([&](int i) -> T0 { return ptr[i].Real(); }),
              imagData([&](int i) -> T0 { return ptr[i].Imag(); }) {}

        template<typename Up>
        [[gnu::always_inline]]
        constexpr BasicVector(LoadCtorTag, const Up* ptr)
            : realData(LoadCtorTag(), ptr), imagData() {}

        template<std::ranges::contiguous_range Rg, typename... FlagTypes>
            requires Detail::StaticSizedRange<Rg> && (Detail::StaticSize<Rg>() == kStorageSize) &&
                     Vectorizable<std::ranges::range_value_t<Rg>> &&
                     ExplicitlyConvertibleTo<std::ranges::range_value_t<Rg>, ValueType>
        [[gnu::always_inline]]
        constexpr BasicVector(Rg&& range, Flags<FlagTypes...> flags = {})
            : BasicVector(LoadCtorTag(), flags.template AdjustPointer<BasicVector>(std::ranges::data(range))) {
            static_assert(LoadstoreConvertibleTo<std::ranges::range_value_t<Rg>, ValueType, FlagTypes...>);
        }

        // [simd.ctor] std::complex Init ---------------------------------------------
        // This uses RealSimd as proposed in LWG4230
        [[gnu::always_inline]]
        constexpr BasicVector(const RealSimd& re, const RealSimd& im = {}) noexcept
            : realData(re), imagData(im) {}

        // [simd.subscr] --------------------------------------------------------
        [[gnu::always_inline]]
        constexpr ValueType operator[](SimdSizeType i) const {
            return ValueType(realData[i], imagData[i]);
        }

        // [simd.unary] unary operators -----------------------------------------
        [[gnu::always_inline]]
        constexpr BasicVector& operator++() noexcept
            requires requires(ValueType a) { ++a; }
        {
            ++realData;
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator++(int) noexcept
            requires requires(ValueType a) { a++; }
        {
            BasicVector r = *this;
            ++realData;
            return r;
        }

        [[gnu::always_inline]]
        constexpr BasicVector& operator--() noexcept
            requires requires(ValueType a) { --a; }
        {
            --realData;
            return *this;
        }

        [[gnu::always_inline]]
        constexpr BasicVector operator--(int) noexcept
            requires requires(ValueType a) { a--; }
        {
            BasicVector r = *this;
            --realData;
            return r;
        }

        [[gnu::always_inline]]
        constexpr MaskType operator!() const noexcept
            requires requires(ValueType a) { !a; }
        {
            return !realData && !imagData;
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
            return BasicVector(-realData, -imagData);
        }

        // [simd.cassign] compound assignment -----------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicVector& operator+=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a + a; }
        {
            x.realData += y.realData;
            x.imagData += y.imagData;
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator-=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a - a; }
        {
            x.realData -= y.realData;
            x.imagData -= y.imagData;
            return x;
        }

        template<TargetTraits Traits = {}>
        [[gnu::always_inline]] friend constexpr BasicVector& operator*=(BasicVector & x, const BasicVector & y) noexcept
            requires requires(ValueType a) { a * a; }
        {
            Cxctgus::Mul<ValueType, Traits>(x.realData, x.imagData, y.realData, y.imagData);
            return x;
        }

        [[gnu::always_inline]]
        friend constexpr BasicVector& operator/=(BasicVector& x, const BasicVector& y) noexcept
            requires requires(ValueType a) { a / a; }
        {
            const RealSimd r = x.realData * y.realData + x.imagData * y.imagData;
            const RealSimd n = y.Norm();
            x.imagData = (x.imagData * y.realData - x.realData * y.imagData) / n;
            x.realData = r / n;
            return x;
        }

        // [simd.comparison] compare operators ----------------------------------
        [[gnu::always_inline]]
        friend constexpr MaskType operator==(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType(x.realData == y.realData && x.imagData == y.imagData);
        }

        [[gnu::always_inline]]
        friend constexpr MaskType operator!=(const BasicVector& x, const BasicVector& y) noexcept {
            return MaskType(x.realData != y.realData || x.imagData != y.imagData);
        }

        // [simd.std::complex.access] std::complex-value accessors ------------------------
        // LWG4230: returns RealSimd instead of auto
        [[gnu::always_inline]]
        constexpr RealSimd Real() const noexcept {
            return realData;
        }

        [[gnu::always_inline]]
        constexpr RealSimd Imag() const noexcept {
            return imagData;
        }

        [[gnu::always_inline]]
        constexpr void Real(const RealSimd& x) noexcept {
            realData = x;
        }

        [[gnu::always_inline]]
        constexpr void Imag(const RealSimd& x) noexcept {
            imagData = x;
        }

        // [simd.cond] ---------------------------------------------------------
        [[gnu::always_inline]]
        friend constexpr BasicVector SelectImpl(const MaskType& k, const BasicVector& t,
                                                const BasicVector& f) noexcept {
            typename BasicVector::RealSimd::MaskType kk(k);
            return BasicVector(SelectImpl(kk, t.realData, f.realData), SelectImpl(kk, t.imagData, f.imagData));
        }

        // [simd.std::complex.math] internals ---------------------------------------
        [[gnu::always_inline]]
        constexpr RealSimd Abs() const; // TODO: depends on [simd.math]

        // associated functions
        [[gnu::always_inline]]
        constexpr RealSimd Norm() const {
            return realData * realData + imagData * imagData;
        }

        [[gnu::always_inline]]
        constexpr BasicVector Conj() const {
            return BasicVector(realData, -imagData);
        }
    };

    // [P3319R5] (extension) ----------------------------------------------------
    template<ComplexLike Tp, typename Ap>
    inline constexpr BasicVector<Tp, Ap> kIota<BasicVector<Tp, Ap>> =
        BasicVector<Tp, Ap>([](typename Tp::value_type i) -> typename Tp::value_type {
            static_assert(Ap::kStorageSize - 1 <= std::numeric_limits<typename Tp::value_type>::max(),
                          "iota object would overflow");
            return i;
        });

} // namespace Sora::Math::Simd

#pragma GCC diagnostic pop
