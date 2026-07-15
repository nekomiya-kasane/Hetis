/**
 * @file LoadStore.h
 * @brief Range and iterator based SIMD load and Store operations.
 * @ingroup Math
 */
#pragma once

#include "Vector.h"

// [simd.reductions] ----------------------------------------------------------
namespace Sora::Math::Simd {

    template<typename Vp, typename Tp>
    struct VecLoadReturn {
        using Type = Vp;
    };

    template<typename Tp>
    struct VecLoadReturn<void, Tp> {
        using Type = BasicVector<Tp>;
    };

    template<typename Vp, typename Tp>
    using VecLoadReturnT = typename VecLoadReturn<Vp, Tp>::Type;

    template<typename Vp, typename Tp>
    using LoadMaskTypeT = typename VecLoadReturnT<Vp, Tp>::MaskType;

    template<typename Tp>
    concept SizedContiguousRange = std::ranges::contiguous_range<Tp> && std::ranges::sized_range<Tp>;

    template<typename Vp = void, SizedContiguousRange Rg, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::ranges::range_value_t<Rg>> UncheckedLoad(Rg&& r, Flags<FlagTypes...> f = {}) {
        using Tp = std::ranges::range_value_t<Rg>;
        using RV = VecLoadReturnT<Vp, Tp>;
        using Rp = typename RV::ValueType;
        static_assert(LoadstoreConvertibleTo<std::ranges::range_value_t<Rg>, Rp, FlagTypes...>,
                      "'kConvertFlag' must be used for conversions that are not value-preserving");

        constexpr bool allowOutOfBounds = f.Test(kAllowPartialLoadstore);
        constexpr std::size_t staticSize = StaticRangeSize(r);

        if constexpr (!allowOutOfBounds && Detail::StaticSizedRange<Rg>) {
            static_assert(std::ranges::size(r) >= RV::kSize(), "given range must have sufficient size");
        }

        const auto* ptr = f.template AdjustPointer<RV>(std::ranges::data(r));
        const auto rgSize = std::ranges::size(r);
        if constexpr (!allowOutOfBounds) {
            SORA_SIMD_PRECONDITION(std::ranges::size(r) >= RV::kSize(),
                                   "Input range is too small. Did you mean to use 'PartialLoad'?");
        }

        if consteval {
            return RV([&](std::size_t i) -> Rp {
                if (i >= rgSize) {
                    return Rp();
                } else if constexpr (ComplexLike<Rp> && !ComplexLike<Tp>) {
                    return static_cast<typename Rp::value_type>(r[i]);
                } else {
                    return static_cast<Rp>(r[i]);
                }
            });
        } else {
            if constexpr ((staticSize != std::dynamic_extent && staticSize >= std::size_t(RV::kSize())) ||
                          !allowOutOfBounds) {
                return RV(LoadCtorTag(), ptr);
            } else {
                return RV::PartialLoad(ptr, rgSize);
            }
        }
    }

    template<typename Vp = void, SizedContiguousRange Rg, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::ranges::range_value_t<Rg>>
    UncheckedLoad(Rg&& r, const LoadMaskTypeT<Vp, std::ranges::range_value_t<Rg>>& mask, Flags<FlagTypes...> f = {}) {
        using Tp = std::ranges::range_value_t<Rg>;
        using RV = VecLoadReturnT<Vp, Tp>;
        using Rp = typename RV::ValueType;
        static_assert(Vectorizable<Tp>);
        static_assert(ExplicitlyConvertibleTo<Tp, Rp>);
        static_assert(LoadstoreConvertibleTo<Tp, Rp, FlagTypes...>,
                      "'kConvertFlag' must be used for conversions that are not value-preserving");

        constexpr bool allowOutOfBounds = f.Test(kAllowPartialLoadstore);
        constexpr auto staticSize = StaticRangeSize(r);

        if constexpr (!allowOutOfBounds && Detail::StaticSizedRange<Rg>) {
            static_assert(std::ranges::size(r) >= RV::kSize(), "given range must have sufficient size");
        }

        const auto* ptr = f.template AdjustPointer<RV>(std::ranges::data(r));

        if constexpr (!allowOutOfBounds) {
            SORA_SIMD_PRECONDITION(std::ranges::size(r) >= std::size_t(RV::kSize()),
                                   "Input range is too small. Did you mean to use 'PartialLoad'?");
        }

        const std::size_t rgSize = std::ranges::size(r);
        if consteval {
            return RV([&](std::size_t i) -> Rp {
                if (i >= rgSize || !mask[int(i)]) {
                    return Rp();
                } else if constexpr (ComplexLike<Rp> && !ComplexLike<Tp>) {
                    return static_cast<typename Rp::value_type>(r[i]);
                } else {
                    return static_cast<Rp>(r[i]);
                }
            });
        } else {
            constexpr bool noSizeCheck =
                !allowOutOfBounds || (staticSize != std::dynamic_extent && staticSize >= std::size_t(RV::kSize.value));
            if constexpr (RV::kSize() == 1) {
                return mask[0] && (noSizeCheck || rgSize > 0) ? RV(LoadCtorTag(), ptr) : RV();
            } else if constexpr (noSizeCheck || rgSize >= std::size_t(RV::kSize())) {
                return RV::MaskedLoad(ptr, mask);
            } else if (rgSize > 0) {
                return RV::MaskedLoad(ptr, mask && RV::MaskType::PartialMaskOfN(int(rgSize)));
            } else {
                return RV();
            }
        }
    }

    template<typename Vp = void, std::contiguous_iterator It, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>> UncheckedLoad(It first, std::iter_difference_t<It> n,
                                                                      Flags<FlagTypes...> f = {}) {
        return Sora::Math::Simd::UncheckedLoad<Vp>(std::span<const std::iter_value_t<It>>(first, n), f);
    }

    template<typename Vp = void, std::contiguous_iterator It, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>>
    UncheckedLoad(It first, std::iter_difference_t<It> n, const LoadMaskTypeT<Vp, std::iter_value_t<It>>& mask,
                  Flags<FlagTypes...> f = {}) {
        return Sora::Math::Simd::UncheckedLoad<Vp>(std::span<const std::iter_value_t<It>>(first, n), mask, f);
    }

    template<typename Vp = void, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>> UncheckedLoad(It first, Sp last, Flags<FlagTypes...> f = {}) {
        return Sora::Math::Simd::UncheckedLoad<Vp>(std::span<const std::iter_value_t<It>>(first, last), f);
    }

    template<typename Vp = void, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>>
    UncheckedLoad(It first, Sp last, const LoadMaskTypeT<Vp, std::iter_value_t<It>>& mask, Flags<FlagTypes...> f = {}) {
        return Sora::Math::Simd::UncheckedLoad<Vp>(std::span<const std::iter_value_t<It>>(first, last), mask, f);
    }

    template<typename Vp = void, SizedContiguousRange Rg, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::ranges::range_value_t<Rg>> PartialLoad(Rg&& r, Flags<FlagTypes...> f = {}) {
        return Sora::Math::Simd::UncheckedLoad<Vp>(r, f | kAllowPartialLoadstore);
    }

    template<typename Vp = void, SizedContiguousRange Rg, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::ranges::range_value_t<Rg>>
    PartialLoad(Rg&& r, const LoadMaskTypeT<Vp, std::ranges::range_value_t<Rg>>& mask, Flags<FlagTypes...> f = {}) {
        return Sora::Math::Simd::UncheckedLoad<Vp>(r, mask, f | kAllowPartialLoadstore);
    }

    template<typename Vp = void, std::contiguous_iterator It, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>> PartialLoad(It first, std::iter_difference_t<It> n,
                                                                    Flags<FlagTypes...> f = {}) {
        return PartialLoad<Vp>(std::span<const std::iter_value_t<It>>(first, n), f);
    }

    template<typename Vp = void, std::contiguous_iterator It, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>>
    PartialLoad(It first, std::iter_difference_t<It> n, const LoadMaskTypeT<Vp, std::iter_value_t<It>>& mask,
                Flags<FlagTypes...> f = {}) {
        return PartialLoad<Vp>(std::span<const std::iter_value_t<It>>(first, n), mask, f);
    }

    template<typename Vp = void, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>> PartialLoad(It first, Sp last, Flags<FlagTypes...> f = {}) {
        return PartialLoad<Vp>(std::span<const std::iter_value_t<It>>(first, last), f);
    }

    template<typename Vp = void, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp, typename... FlagTypes>
    [[gnu::always_inline]]
    constexpr VecLoadReturnT<Vp, std::iter_value_t<It>>
    PartialLoad(It first, Sp last, const LoadMaskTypeT<Vp, std::iter_value_t<It>>& mask, Flags<FlagTypes...> f = {}) {
        return PartialLoad<Vp>(std::span<const std::iter_value_t<It>>(first, last), mask, f);
    }

    template<typename Tp, typename Ap, SizedContiguousRange Rg, typename... FlagTypes>
        requires std::indirectly_writable<std::ranges::iterator_t<Rg>, Tp>
    [[gnu::always_inline]]
    constexpr void UncheckedStore(const BasicVector<Tp, Ap>& v, Rg&& r, Flags<FlagTypes...> f = {}) {
        using TV = BasicVector<Tp, Ap>;
        static_assert(std::destructible<TV>);
        static_assert(LoadstoreConvertibleTo<Tp, std::ranges::range_value_t<Rg>, FlagTypes...>,
                      "'kConvertFlag' must be used for conversions that are not value-preserving");

        constexpr bool allowOutOfBounds = f.Test(kAllowPartialLoadstore);
        if constexpr (!allowOutOfBounds && Detail::StaticSizedRange<Rg>) {
            static_assert(std::ranges::size(r) >= TV::kSize(), "given range must have sufficient size");
        }

        auto* ptr = f.template AdjustPointer<TV>(std::ranges::data(r));
        const auto rgSize = std::ranges::size(r);
        if constexpr (!allowOutOfBounds) {
            SORA_SIMD_PRECONDITION(std::ranges::size(r) >= TV::kSize(),
                                   "output range is too small. Did you mean to use 'PartialStore'?");
        }

        if consteval {
            for (unsigned i = 0; i < rgSize && i < TV::kSize(); ++i) {
                ptr[i] = static_cast<std::ranges::range_value_t<Rg>>(v[i]);
            }
        } else {
            if constexpr (!allowOutOfBounds) {
                v.Store(ptr);
            } else {
                TV::PartialStore(v, ptr, rgSize);
            }
        }
    }

    template<typename Tp, typename Ap, SizedContiguousRange Rg, typename... FlagTypes>
        requires std::indirectly_writable<std::ranges::iterator_t<Rg>, Tp>
    [[gnu::always_inline]]
    constexpr void UncheckedStore(const BasicVector<Tp, Ap>& v, Rg&& r,
                                  const typename BasicVector<Tp, Ap>::MaskType& mask, Flags<FlagTypes...> f = {}) {
        using TV = BasicVector<Tp, Ap>;
        static_assert(LoadstoreConvertibleTo<Tp, std::ranges::range_value_t<Rg>, FlagTypes...>,
                      "'kConvertFlag' must be used for conversions that are not value-preserving");

        constexpr bool allowOutOfBounds = f.Test(kAllowPartialLoadstore);
        if constexpr (!allowOutOfBounds && Detail::StaticSizedRange<Rg>) {
            static_assert(std::ranges::size(r) >= TV::kSize(), "given range must have sufficient size");
        }

        auto* ptr = f.template AdjustPointer<TV>(std::ranges::data(r));

        if constexpr (!allowOutOfBounds) {
            SORA_SIMD_PRECONDITION(std::ranges::size(r) >= std::size_t(TV::kSize()),
                                   "output range is too small. Did you mean to use 'PartialStore'?");
        }

        const std::size_t rgSize = std::ranges::size(r);
        if consteval {
            for (int i = 0; i < TV::kSize(); ++i) {
                if (mask[i] && (!allowOutOfBounds || std::size_t(i) < rgSize)) {
                    ptr[i] = static_cast<std::ranges::range_value_t<Rg>>(v[i]);
                }
            }
        } else {
            if (allowOutOfBounds && rgSize < std::size_t(TV::kSize())) {
                TV::MaskedStore(v, ptr, mask && TV::MaskType::PartialMaskOfN(int(rgSize)));
            } else {
                TV::MaskedStore(v, ptr, mask);
            }
        }
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void UncheckedStore(const BasicVector<Tp, Ap>& v, It first, std::iter_difference_t<It> n,
                                  Flags<FlagTypes...> f = {}) {
        Sora::Math::Simd::UncheckedStore(v, std::span<std::iter_value_t<It>>(first, n), f);
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void UncheckedStore(const BasicVector<Tp, Ap>& v, It first, std::iter_difference_t<It> n,
                                  const typename BasicVector<Tp, Ap>::MaskType& mask, Flags<FlagTypes...> f = {}) {
        Sora::Math::Simd::UncheckedStore(v, std::span<std::iter_value_t<It>>(first, n), mask, f);
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp,
             typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void UncheckedStore(const BasicVector<Tp, Ap>& v, It first, Sp last, Flags<FlagTypes...> f = {}) {
        Sora::Math::Simd::UncheckedStore(v, std::span<std::iter_value_t<It>>(first, last), f);
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp,
             typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void UncheckedStore(const BasicVector<Tp, Ap>& v, It first, Sp last,
                                  const typename BasicVector<Tp, Ap>::MaskType& mask, Flags<FlagTypes...> f = {}) {
        Sora::Math::Simd::UncheckedStore(v, std::span<std::iter_value_t<It>>(first, last), mask, f);
    }

    template<typename Tp, typename Ap, SizedContiguousRange Rg, typename... FlagTypes>
        requires std::indirectly_writable<std::ranges::iterator_t<Rg>, Tp>
    [[gnu::always_inline]]
    constexpr void PartialStore(const BasicVector<Tp, Ap>& v, Rg&& r, Flags<FlagTypes...> f = {}) {
        Sora::Math::Simd::UncheckedStore(v, r, f | kAllowPartialLoadstore);
    }

    template<typename Tp, typename Ap, SizedContiguousRange Rg, typename... FlagTypes>
        requires std::indirectly_writable<std::ranges::iterator_t<Rg>, Tp>
    [[gnu::always_inline]]
    constexpr void PartialStore(const BasicVector<Tp, Ap>& v, Rg&& r,
                                const typename BasicVector<Tp, Ap>::MaskType& mask, Flags<FlagTypes...> f = {}) {
        Sora::Math::Simd::UncheckedStore(v, r, mask, f | kAllowPartialLoadstore);
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void PartialStore(const BasicVector<Tp, Ap>& v, It first, std::iter_difference_t<It> n,
                                Flags<FlagTypes...> f = {}) {
        PartialStore(v, std::span(first, n), f);
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void PartialStore(const BasicVector<Tp, Ap>& v, It first, std::iter_difference_t<It> n,
                                const typename BasicVector<Tp, Ap>::MaskType& mask, Flags<FlagTypes...> f = {}) {
        PartialStore(v, std::span(first, n), mask, f);
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp,
             typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void PartialStore(const BasicVector<Tp, Ap>& v, It first, Sp last, Flags<FlagTypes...> f = {}) {
        PartialStore(v, std::span(first, last), f);
    }

    template<typename Tp, typename Ap, std::contiguous_iterator It, std::sized_sentinel_for<It> Sp,
             typename... FlagTypes>
        requires std::indirectly_writable<It, Tp>
    [[gnu::always_inline]]
    constexpr void PartialStore(const BasicVector<Tp, Ap>& v, It first, Sp last,
                                const typename BasicVector<Tp, Ap>::MaskType& mask, Flags<FlagTypes...> f = {}) {
        PartialStore(v, std::span(first, last), mask, f);
    }

} // namespace Sora::Math::Simd