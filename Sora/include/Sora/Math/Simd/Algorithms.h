/**
 * @file Algorithms.h
 * @brief Element-wise SIMD algorithms.
 * @ingroup Math
 */
#pragma once

#include "Vector.h"

// [simd.alg] -----------------------------------------------------------------
namespace Sora::Math::Simd {

    template<typename Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr BasicVector<Tp, Ap> Min(const BasicVector<Tp, Ap>& a, const BasicVector<Tp, Ap>& b) noexcept {
        return SelectImpl(a < b, a, b);
    }

    template<typename Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr BasicVector<Tp, Ap> Max(const BasicVector<Tp, Ap>& a, const BasicVector<Tp, Ap>& b) noexcept {
        return SelectImpl(a < b, b, a);
    }

    template<typename Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr std::pair<BasicVector<Tp, Ap>, BasicVector<Tp, Ap>> Minmax(const BasicVector<Tp, Ap>& a,
                                                                         const BasicVector<Tp, Ap>& b) noexcept {
        return {Min(a, b), Max(a, b)};
    }

    template<typename Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr BasicVector<Tp, Ap> Clamp(const BasicVector<Tp, Ap>& v, const BasicVector<Tp, Ap>& lo,
                                        const BasicVector<Tp, Ap>& hi) {
        SORA_SIMD_PRECONDITION(NoneOf(lo > hi), "lower bound is larger than upper bound");
        return SelectImpl(v < lo, lo, SelectImpl(hi < v, hi, v));
    }

    template<typename Tp, typename Up>
    constexpr auto Select(bool c, const Tp& a, const Up& b) -> std::remove_cvref_t<decltype(c ? a : b)> {
        return c ? a : b;
    }

    template<size_t Bytes, typename Ap, typename Tp, typename Up>
    [[gnu::always_inline]]
    constexpr auto Select(const BasicMask<Bytes, Ap>& c, const Tp& a, const Up& b) noexcept
        -> decltype(SelectImpl(c, a, b)) {
        return SelectImpl(c, a, b);
    }

} // namespace Sora::Math::Simd
