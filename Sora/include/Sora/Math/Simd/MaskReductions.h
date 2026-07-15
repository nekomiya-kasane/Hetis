/**
 * @file MaskReductions.h
 * @brief Boolean and index reductions over SIMD masks.
 * @ingroup Math
 */
#pragma once

#include "Mask.h"

// [simd.Mask.reductions] -----------------------------------------------------
namespace Sora::Math::Simd {

    template<std::size_t Bytes, typename Ap>
    [[gnu::always_inline]]
    constexpr bool AllOf(const BasicMask<Bytes, Ap>& k) noexcept {
        return k.AllOf();
    }

    template<std::size_t Bytes, typename Ap>
    [[gnu::always_inline]]
    constexpr bool AnyOf(const BasicMask<Bytes, Ap>& k) noexcept {
        return k.AnyOf();
    }

    template<std::size_t Bytes, typename Ap>
    [[gnu::always_inline]]
    constexpr bool NoneOf(const BasicMask<Bytes, Ap>& k) noexcept {
        return k.NoneOf();
    }

    template<std::size_t Bytes, typename Ap>
    [[gnu::always_inline]]
    constexpr SimdSizeType ReduceCount(const BasicMask<Bytes, Ap>& k) noexcept {
        if constexpr (Ap::kStorageSize == 1) {
            return +k[0];
        } else if constexpr (Ap::kIsVecmask && Bytes <= sizeof(0ll)) {
            return -Reduce(-k);
        } else {
            return k.ReduceCount();
        }
    }

    template<std::size_t Bytes, typename Ap>
    [[gnu::always_inline]]
    constexpr SimdSizeType ReduceMinIndex(const BasicMask<Bytes, Ap>& k) {
        return k.ReduceMinIndex();
    }

    template<std::size_t Bytes, typename Ap>
    [[gnu::always_inline]]
    constexpr SimdSizeType ReduceMaxIndex(const BasicMask<Bytes, Ap>& k) {
        return k.ReduceMaxIndex();
    }

    constexpr bool AllOf(std::same_as<bool> auto x) noexcept {
        return x;
    }

    constexpr bool AnyOf(std::same_as<bool> auto x) noexcept {
        return x;
    }

    constexpr bool NoneOf(std::same_as<bool> auto x) noexcept {
        return !x;
    }

    constexpr SimdSizeType ReduceCount(std::same_as<bool> auto x) noexcept {
        return x;
    }

    constexpr SimdSizeType ReduceMinIndex(std::same_as<bool> auto x) {
        return 0;
    }

    constexpr SimdSizeType ReduceMaxIndex(std::same_as<bool> auto x) {
        return 0;
    }

} // namespace Sora::Math::Simd