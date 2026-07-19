/**
 * @file Flags.h
 * @brief Flags controlling SIMD conversion, alignment, load, and Store operations.
 * @ingroup Math
 */
#pragma once

#include "Details.h"

#include <memory> // std::assume_aligned

namespace Sora::Math::Simd {
    
    // [simd.traits]
    // --- Alignment ---
    template<typename Tp, typename Up = typename Tp::value_type>
    struct Alignment {};

    template<typename Tp, typename Ap, Vectorizable Up>
    struct Alignment<BasicVector<Tp, Ap>, Up> : std::integral_constant<size_t, alignof(BasicVector<Tp, Ap>)> {};

    template<typename Tp, typename Up = typename Tp::value_type>
    constexpr size_t kAlignment = Alignment<Tp, Up>::value;

    // [simd.Flags] -------------------------------------------------------------
    struct LoadStoreTag {};

    /** @internal
     * `struct convert-flag`
     *
     * C++26 [simd.expos] / [simd.Flags]
     */
    struct ConvertFlag : LoadStoreTag {};

    /** @internal
     * `struct aligned-flag`
     *
     * C++26 [simd.expos] / [simd.Flags]
     */
    struct AlignedFlag : LoadStoreTag {
        template<typename Tp, typename Up>
        [[gnu::always_inline]]
        static constexpr Up* AdjustPointer(Up* ptr) {
            return std::assume_aligned<Sora::Math::Simd::kAlignment<Tp, std::remove_cv_t<Up>>>(ptr);
        }
    };

    /** @internal
     * `template<size_t N> struct overaligned-flag`
     *
     * @tparam Np  Alignment in bytes
     *
     * C++26 [simd.expos] / [simd.Flags]
     */
    template<size_t Np>
    struct OveralignedFlag : LoadStoreTag {
        static_assert(std::has_single_bit(Np));

        template<typename, typename Up>
        [[gnu::always_inline]]
        static constexpr Up* AdjustPointer(Up* ptr) {
            return std::assume_aligned<Np>(ptr);
        }
    };

    struct PartialLoadstoreFlag : LoadStoreTag {};

    template<typename Tp>
    concept LoadstoreTag = std::is_base_of_v<LoadStoreTag, Tp>;

    template<typename...>
    struct Flags;

    template<typename... FlagTypes>
        requires(LoadstoreTag<FlagTypes> && ...)
    struct Flags<FlagTypes...> {
        /** @internal
         * Returns @c true if the given argument is part of this specialization, otherwise returns @c
         * false.
         */
        template<typename F0>
        static consteval bool Test(Flags<F0>) {
            return (std::is_same_v<FlagTypes, F0> || ...);
        }

        friend consteval Flags operator|(Flags, Flags<>) { return Flags{}; }

        template<typename T0, typename... More>
        friend consteval auto operator|(Flags, Flags<T0, More...>) {
            if constexpr ((std::same_as<FlagTypes, T0> || ...)) {
                return Flags<FlagTypes...>{} | Flags<More...>{};
            } else {
                return Flags<FlagTypes..., T0>{} | Flags<More...>{};
            }
        }

        /** @internal
         * Adjusts a pointer according to the Alignment requirements of the Flags.
         *
         * This function iterates over all Flags in the pack and applies each flag's
         * `AdjustPointer` method to the input pointer. Flags that don't provide
         * this method are ignored.
         *
         * @tparam Tp  A BasicVector type for which a load/Store pointer is adjusted
         * @tparam Up  The value-type of the input/output range
         * @param ptr  The pointer to the range
         * @return The adjusted pointer
         */
        template<typename Tp, typename Up>
        static constexpr Up* AdjustPointer(Up* ptr) {
            template for ([[maybe_unused]] constexpr auto f : {FlagTypes()...}) {
                if constexpr (requires { f.template AdjustPointer<Tp>(ptr); }) {
                    ptr = f.template AdjustPointer<Tp>(ptr);
                }
            }
            return ptr;
        }
    };

    inline constexpr Flags<> kDefaultFlag{};

    inline constexpr Flags<ConvertFlag> kConvertFlag{};

    inline constexpr Flags<AlignedFlag> kAlignedFlag{};

    template<size_t Np>
        requires(std::has_single_bit(Np))
    inline constexpr Flags<OveralignedFlag<Np>> kOveralignedFlag{};

    /** @internal
     * Pass to UncheckedLoad or UncheckedStore to make it behave like PartialLoad / PartialStore.
     */
    inline constexpr Flags<PartialLoadstoreFlag> kAllowPartialLoadstore{};

} // namespace Sora::Math::Simd
