/**
 * @file Portability.h
 * @brief Compiler and standard-library adaptation layer for the GCC SIMD implementation.
 * @ingroup Math
 */

#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#if __has_include(<stdfloat>)
#    include <stdfloat>
#endif

#if defined(__clang__)
#    define SORA_SIMD_CLANG 1
#endif

#if __FLT_RADIX__ == 2 && __FLT_MANT_DIG__ == 24 && __FLT_MAX_EXP__ == 128
#    define SORA_SIMD_FLOAT_IS_IEEE_BINARY32 1
#endif

#if __FLT_RADIX__ == 2 && __DBL_MANT_DIG__ == 53 && __DBL_MAX_EXP__ == 1024
#    define SORA_SIMD_DOUBLE_IS_IEEE_BINARY64 1
#endif

#if defined(__x86_64__) || defined(__i386__)
#    define SORA_SIMD_X86 1
#else
#    define SORA_SIMD_X86 0
#endif

#ifndef SORA_SIMD_THROW_ON_BAD_VALUE
#    define SORA_SIMD_THROW_ON_BAD_VALUE 0
#endif

#ifndef SORA_SIMD_ASSERT
#    define SORA_SIMD_ASSERT(expression) assert(expression)
#endif

#if SORA_SIMD_CLANG && SORA_SIMD_X86
#    define SORA_SIMD_X86_PSLLDQI128(value, bits) __builtin_ia32_pslldqi128_byteshift((value), (bits) / __CHAR_BIT__)
#    define SORA_SIMD_X86_PSRLDQI128(value, bits) __builtin_ia32_psrldqi128_byteshift((value), (bits) / __CHAR_BIT__)
#    define SORA_SIMD_X86_BLEND(falseValue, trueValue, mask, selectBuiltin, blendBuiltin)                              \
        selectBuiltin((mask), (trueValue), (falseValue))
#else
#    define SORA_SIMD_X86_PSLLDQI128(value, bits) __builtin_ia32_pslldqi128((value), (bits))
#    define SORA_SIMD_X86_PSRLDQI128(value, bits) __builtin_ia32_psrldqi128((value), (bits))
#    define SORA_SIMD_X86_BLEND(falseValue, trueValue, mask, selectBuiltin, blendBuiltin)                              \
        blendBuiltin((falseValue), (trueValue), (mask))
#endif

namespace Sora::Math::Simd::Detail {

    struct InvalidInteger {};

    template<std::size_t Bytes>
    [[nodiscard]] consteval auto IntegerTypeForSize() {
        if constexpr (sizeof(signed char) == Bytes) {
            return std::type_identity<signed char>{};
        } else if constexpr (sizeof(signed short) == Bytes) {
            return std::type_identity<signed short>{};
        } else if constexpr (sizeof(signed int) == Bytes) {
            return std::type_identity<signed int>{};
        } else if constexpr (sizeof(signed long long) == Bytes) {
            return std::type_identity<signed long long>{};
        } else {
            return std::type_identity<InvalidInteger>{};
        }
    }

    template<std::size_t Bytes>
    using IntegerForSize = typename decltype(IntegerTypeForSize<Bytes>())::type;

    template<std::size_t Bytes>
    [[nodiscard]] consteval auto FloatTypeForSize() {
        if constexpr (sizeof(double) == Bytes) {
            return std::type_identity<double>{};
        } else if constexpr (sizeof(float) == Bytes) {
            return std::type_identity<float>{};
        } else if constexpr (sizeof(_Float16) == Bytes) {
            return std::type_identity<_Float16>{};
        }
    }

    template<std::size_t Bytes>
    using FloatForSize = typename decltype(FloatTypeForSize<Bytes>())::type;

    template<typename Range>
    concept StaticSizedRange = std::ranges::sized_range<Range> && requires(Range& range) {
        static_cast<char (*)[std::size_t(std::ranges::size(range) >= 0)]>(nullptr);
    };

    template<StaticSizedRange Range>
    [[nodiscard]] consteval auto StaticSize() -> std::ranges::range_size_t<Range> {
        auto conjure = [](Range& range) {
            if constexpr (std::ranges::size(range) <= std::size_t(-1)) {
                return std::integral_constant<std::size_t, std::size_t(std::ranges::size(range))>{};
            } else {
                return std::integral_constant<std::ranges::range_size_t<Range>, std::ranges::size(range)>{};
            }
        };
        return std::ranges::range_size_t<Range>(decltype(conjure(std::declval<Range&>()))::value);
    }

    template<typename T>
    concept ConstexprWrapperLike =
        std::convertible_to<T, decltype(T::value)> && std::equality_comparable_with<T, decltype(T::value)> &&
        std::bool_constant<T() == T::value>::value &&
        std::bool_constant<static_cast<decltype(T::value)>(T()) == T::value>::value;

    template<typename T>
    concept UnsignedInteger = std::is_integral_v<T> && std::is_unsigned_v<T>;

    template<typename T>
    struct MakeUnsigned {
        using Type = T;
    };

    template<std::integral T>
    struct MakeUnsigned<T> {
        using Type = std::make_unsigned_t<T>;
    };

    template<typename T>
    using MakeUnsignedT = typename MakeUnsigned<T>::Type;

    template<auto Size, typename T = decltype(Size), T... Indices>
    [[nodiscard]] consteval auto MakeIotaArray(std::integer_sequence<T, Indices...>) {
        return std::array<T, std::size_t(Size)>{Indices...};
    }

    template<auto Size, typename T = decltype(Size)>
    inline constexpr auto kIotaArray = MakeIotaArray<Size, T>(std::make_integer_sequence<T, T(Size)>{});

    template<auto Value>
    inline constexpr auto kConstant = std::integral_constant<decltype(Value), Value>{};

    template<typename T, typename... Candidates>
    using IsOneOf = std::bool_constant<(std::same_as<T, Candidates> || ...)>;

    template<std::size_t Alignment, typename T>
    [[nodiscard]] constexpr bool IsSufficientlyAligned(const T* pointer) noexcept
        requires(Alignment != 0 && (Alignment & (Alignment - 1)) == 0)
    {
        return std::bit_cast<std::uintptr_t>(pointer) % Alignment == 0;
    }

} // namespace Sora::Math::Simd::Detail
