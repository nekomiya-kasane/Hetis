/**
 * @file Reductions.h
 * @brief Arithmetic and ordered reductions over SIMD vectors.
 * @ingroup Math
 */
#pragma once

#include "Vector.h"

// [simd.reductions] ----------------------------------------------------------
namespace Sora::Math::Simd {

    template<typename Tp, typename Ap, ReductionBinaryOperation<Tp> BinaryOperation = std::plus<>>
    [[gnu::always_inline]]
    constexpr Tp Reduce(const BasicVector<Tp, Ap>& x, BinaryOperation binaryOp = {}) {
        return x.Reduce(binaryOp);
    }

    template<typename Tp, typename Ap, ReductionBinaryOperation<Tp> BinaryOperation = std::plus<>>
    [[gnu::always_inline]]
    constexpr Tp Reduce(const BasicVector<Tp, Ap>& x, const typename BasicVector<Tp, Ap>::MaskType& mask,
                        BinaryOperation binaryOp = {},
                        std::type_identity_t<Tp> identityElement = DefaultIdentityElement<Tp, BinaryOperation>()) {
        return Reduce(SelectImpl(mask, x, identityElement), binaryOp);
    }

    template<std::totally_ordered Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr Tp ReduceMin(const BasicVector<Tp, Ap>& x) noexcept {
        return Reduce(x, []<typename UV>(const UV& a, const UV& b) { return SelectImpl(a < b, a, b); });
    }

    template<std::totally_ordered Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr Tp ReduceMin(const BasicVector<Tp, Ap>& x, const typename BasicVector<Tp, Ap>::MaskType& mask) noexcept {
        return Reduce(SelectImpl(mask, x, std::numeric_limits<Tp>::max()),
                      []<typename UV>(const UV& a, const UV& b) { return SelectImpl(a < b, a, b); });
    }

    template<std::totally_ordered Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr Tp ReduceMax(const BasicVector<Tp, Ap>& x) noexcept {
        return Reduce(x, []<typename UV>(const UV& a, const UV& b) { return SelectImpl(a < b, b, a); });
    }

    template<std::totally_ordered Tp, typename Ap>
    [[gnu::always_inline]]
    constexpr Tp ReduceMax(const BasicVector<Tp, Ap>& x, const typename BasicVector<Tp, Ap>::MaskType& mask) noexcept {
        return Reduce(SelectImpl(mask, x, std::numeric_limits<Tp>::lowest()),
                      []<typename UV>(const UV& a, const UV& b) { return SelectImpl(a < b, b, a); });
    }

} // namespace Sora::Math::Simd