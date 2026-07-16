/**
 * @file MemoryLayout.h
 * @brief Strong alignment values and constexpr byte-layout helpers.
 * @ingroup Core
 */
#pragma once

#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace Sora {

    /** @brief Return the largest power of two not greater than @p value, or zero when @p value is zero. */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr T FloorPowerOfTwo(T value) noexcept {
        return std::bit_floor(value);
    }

    /**
     * @brief Return the smallest representable power of two not less than @p value.
     * @details Zero and one map to one. Values above the largest representable power of two return @c std::nullopt
     * instead of invoking the undefined overflow case of @c std::bit_ceil.
     */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr std::optional<T> TryCeilPowerOfTwo(T value) noexcept {
        if (value <= T{1}) {
            return T{1};
        }
        if (value > std::bit_floor(std::numeric_limits<T>::max())) {
            return std::nullopt;
        }
        return std::bit_ceil(value);
    }

    /** @brief Return floor(log2(@p value)), or @c std::nullopt when @p value is zero. */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr std::optional<unsigned> TryFloorLog2(T value) noexcept {
        if (value == 0) {
            return std::nullopt;
        }
        return static_cast<unsigned>(std::bit_width(value) - 1);
    }

    /** @brief Return ceil(log2(@p value)), or @c std::nullopt when @p value is zero. */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr std::optional<unsigned> TryCeilLog2(T value) noexcept {
        if (value == 0) {
            return std::nullopt;
        }
        return static_cast<unsigned>(std::bit_width(static_cast<T>(value - 1)));
    }

    /** @brief Return the minimum number of bits needed to represent @p value. Zero requires zero bits. */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr unsigned BitWidth(T value) noexcept {
        return static_cast<unsigned>(std::bit_width(value));
    }

    /** @brief Return the minimum number of bytes needed to represent @p value. Zero requires zero bytes. */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr unsigned ByteWidth(T value) noexcept {
        return (BitWidth(value) + 7u) / 8u;
    }

    /** @brief Return ceil(@p numerator / @p denominator), or @c std::nullopt for a zero denominator. */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr std::optional<T> TryCeilDivide(T numerator, T denominator) noexcept {
        if (denominator == 0) {
            return std::nullopt;
        }
        return static_cast<T>(numerator / denominator + (numerator % denominator != 0 ? T{1} : T{0}));
    }

    /**
     * @brief Round @p value upward to a multiple of @p step.
     * @return Rounded value, or @c std::nullopt when @p step is zero or the result is not representable.
     */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr std::optional<T> TryRoundUp(T value, T step) noexcept {
        if (step == 0) {
            return std::nullopt;
        }
        const T remainder = value % step;
        if (remainder == 0) {
            return value;
        }
        const T adjustment = step - remainder;
        if (value > std::numeric_limits<T>::max() - adjustment) {
            return std::nullopt;
        }
        return static_cast<T>(value + adjustment);
    }

    /** @brief Round @p value downward to a multiple of @p step, or return @c std::nullopt when @p step is zero. */
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr std::optional<T> TryRoundDown(T value, T step) noexcept {
        if (step == 0) {
            return std::nullopt;
        }
        return static_cast<T>(value - value % step);
    }

    /** @brief Return whether @p value can represent a concrete byte alignment. */
    [[nodiscard]] constexpr bool IsValidAlignment(uint64_t value) noexcept {
        return std::has_single_bit(value);
    }

    /** @brief Return log2(@p value), where @p value must be a non-zero power of two. */
    [[nodiscard]] constexpr uint8_t Log2OfPowerOfTwo(uint64_t value) noexcept {
        return static_cast<uint8_t>(std::countr_zero(value));
    }

    /** @brief Compact representation of a concrete non-zero power-of-two byte alignment. */
    class Align {
    public:
        /** @brief Construct byte alignment, which means no stronger alignment requirement. */
        constexpr Align() noexcept = default;

        /** @brief Construct from a concrete non-zero power-of-two byte alignment. */
        explicit constexpr Align(uint64_t value) : log2_(CheckedLog2(value)) {}

        /** @brief Construct from an already validated log2 alignment. */
        [[nodiscard]] static constexpr Align FromLog2(uint8_t log2) {
            if (log2 >= 64) {
                throw "Sora::Align::FromLog2: log2 must be smaller than 64.";
            }
            return Align(log2, TrustedTag{});
        }

        /** @brief Construct a compile-time constant alignment. */
        template<uint64_t Value>
        [[nodiscard]] static consteval Align Constant() {
            static_assert(IsValidAlignment(Value), "Sora::Align requires a non-zero power of two.");
            return Align::FromLog2(Log2OfPowerOfTwo(Value));
        }

        /** @brief Return @c alignof(T) as an alignment value. */
        template<typename T>
        [[nodiscard]] static consteval Align Of() {
            return Constant<alignof(T)>();
        }

        /** @brief Return the byte value of this alignment. */
        [[nodiscard]] constexpr uint64_t Value() const noexcept { return uint64_t{1} << log2_; }

        /** @brief Return log2 of this alignment. */
        [[nodiscard]] constexpr uint8_t Log2() const noexcept { return log2_; }

        /** @brief Return the previous weaker power-of-two alignment, or @c std::nullopt for byte alignment. */
        [[nodiscard]] constexpr std::optional<Align> Previous() const noexcept {
            if (log2_ == 0) {
                return std::nullopt;
            }
            return Align(static_cast<uint8_t>(log2_ - 1), TrustedTag{});
        }

        [[nodiscard]] constexpr bool operator==(const Align&) const noexcept = default;
        [[nodiscard]] constexpr auto operator<=>(const Align&) const noexcept = default;

    private:
        struct TrustedTag {};

        uint8_t log2_ = 0;

        constexpr Align(uint8_t log2, TrustedTag) noexcept : log2_(log2) {}

        [[nodiscard]] static constexpr uint8_t CheckedLog2(uint64_t value) {
            if (!IsValidAlignment(value)) {
                throw "Sora::Align requires a non-zero power-of-two value.";
            }
            return Log2OfPowerOfTwo(value);
        }
    };

    /** @brief Alignment spelling used when the noun form reads better than @ref Align. */
    using Alignment = Align;

    /** @brief Optional alignment value. Undefined means no explicit alignment was supplied. */
    class MaybeAlign {
    public:
        /** @brief Construct an undefined optional alignment. */
        constexpr MaybeAlign() noexcept = default;

        /** @brief Construct an undefined optional alignment. */
        constexpr MaybeAlign(std::nullopt_t) noexcept {}

        /** @brief Construct from a concrete alignment. */
        constexpr MaybeAlign(Align value) noexcept : value_(value) {}

        /** @brief Construct from @p value, treating zero as undefined. */
        explicit constexpr MaybeAlign(uint64_t value) {
            if (value != 0) {
                value_.emplace(value);
            }
        }

        /** @brief Return whether this value carries a concrete alignment. */
        [[nodiscard]] constexpr bool HasValue() const noexcept { return value_.has_value(); }

        /** @brief Return whether this value carries a concrete alignment. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return HasValue(); }

        /** @brief Return the concrete alignment, or throw if this value is undefined. */
        [[nodiscard]] constexpr Align Value() const {
            if (!value_) {
                throw "Sora::MaybeAlign::Value: alignment is undefined.";
            }
            return *value_;
        }

        /** @brief Return the concrete alignment, or byte alignment when this value is undefined. */
        [[nodiscard]] constexpr Align ValueOrOne() const noexcept { return value_.value_or(Align{}); }

        /** @brief Encode undefined as zero and concrete alignments as @c log2(value) + 1. */
        [[nodiscard]] constexpr unsigned Encode() const noexcept {
            return value_ ? static_cast<unsigned>(value_->Log2()) + 1u : 0u;
        }

        /** @brief Decode the representation produced by @ref Encode. */
        [[nodiscard]] static constexpr MaybeAlign Decode(unsigned encoded) {
            if (encoded == 0) {
                return MaybeAlign{};
            }
            if (encoded > 64) {
                throw "Sora::MaybeAlign::Decode: encoded value is out of range.";
            }
            return MaybeAlign{Align::FromLog2(static_cast<uint8_t>(encoded - 1u))};
        }

        [[nodiscard]] constexpr bool operator==(const MaybeAlign&) const noexcept = default;

    private:
        std::optional<Align> value_{};
    };

    /** @brief Encode a concrete alignment as @c log2(value) + 1. */
    [[nodiscard]] constexpr unsigned Encode(Align alignment) noexcept {
        return static_cast<unsigned>(alignment.Log2()) + 1u;
    }

    /** @brief Encode an optional alignment, using zero for undefined. */
    [[nodiscard]] constexpr unsigned Encode(MaybeAlign alignment) noexcept {
        return alignment.Encode();
    }

    /** @brief Decode an optional alignment from the representation produced by @ref Encode. */
    [[nodiscard]] constexpr MaybeAlign DecodeMaybeAlign(unsigned encoded) {
        return MaybeAlign::Decode(encoded);
    }

    /** @brief Return @p value + @p size, saturated to the largest @c uint64_t on overflow. */
    [[nodiscard]] constexpr uint64_t SaturatingEnd(uint64_t value, uint64_t size) noexcept {
        if (size > std::numeric_limits<uint64_t>::max() - value) {
            return std::numeric_limits<uint64_t>::max();
        }
        return value + size;
    }

    /** @brief Return whether two half-open byte ranges @c [offset, offset + size) overlap. */
    [[nodiscard]] constexpr bool RangesOverlap(uint64_t aOffset, uint64_t aSize, uint64_t bOffset,
                                               uint64_t bSize) noexcept {
        if (aSize == 0 || bSize == 0) {
            return false;
        }
        const auto aEnd = SaturatingEnd(aOffset, aSize);
        const auto bEnd = SaturatingEnd(bOffset, bSize);
        return aOffset < bEnd && bOffset < aEnd;
    }

    /** @brief Return whether @p value is aligned to @p alignment; zero alignment is treated as unconstrained. */
    [[nodiscard]] constexpr bool IsAligned(uint64_t value, uint64_t alignment) noexcept {
        return alignment == 0 || value % alignment == 0;
    }

    /** @brief Return whether @p value is aligned to @p alignment. */
    [[nodiscard]] constexpr bool IsAligned(uint64_t value, Align alignment) noexcept {
        return (value & (alignment.Value() - 1u)) == 0;
    }

    /** @brief Return whether @p address is aligned to @p alignment. */
    [[nodiscard]] inline bool IsAddressAligned(const void* address, Align alignment) noexcept {
        return IsAligned(reinterpret_cast<uintptr_t>(address), alignment);
    }

    /** @brief Return the largest power of two that divides both @p a and @p b, or zero when both are zero. */
    [[nodiscard]] constexpr uint64_t MinimumAlignment(uint64_t a, uint64_t b) noexcept {
        const uint64_t combined = a | b;
        return combined & (~combined + 1u);
    }

    /** @brief Return the alignment preserved after adding @p offset to a value with known @p alignment. */
    [[nodiscard]] constexpr Align CommonAlignment(Align alignment, uint64_t offset) {
        if (offset == 0) {
            return alignment;
        }
        return Align{MinimumAlignment(alignment.Value(), offset)};
    }

    /** @brief Align @p value upward modulo @c 2^64. */
    [[nodiscard]] constexpr uint64_t AlignUpModulo(uint64_t value, Align alignment) noexcept {
        const uint64_t mask = alignment.Value() - 1u;
        return (value + mask) & ~mask;
    }

    /** @brief Align @p value downward to the nearest multiple of @p alignment. */
    [[nodiscard]] constexpr uint64_t AlignDown(uint64_t value, Align alignment) noexcept {
        return value & ~(alignment.Value() - 1u);
    }

    /** @brief Align @p value upward without overflow; return @c std::nullopt when the result is not representable. */
    [[nodiscard]] constexpr std::optional<uint64_t> TryAlignUp(uint64_t value, Align alignment) noexcept {
        const uint64_t mask = alignment.Value() - 1u;
        if (value > std::numeric_limits<uint64_t>::max() - mask) {
            return std::nullopt;
        }
        return (value + mask) & ~mask;
    }

    /**
     * @brief Align @p value upward to @p alignment without overflow.
     * @details A zero raw alignment is treated as unconstrained; non-zero raw alignments must be powers of two.
     */
    [[nodiscard]] constexpr std::optional<uint64_t> TryAlignUp(uint64_t value, uint64_t alignment) {
        if (alignment == 0) {
            return value;
        }
        if (!IsValidAlignment(alignment)) {
            return std::nullopt;
        }
        return TryAlignUp(value, Align::FromLog2(Log2OfPowerOfTwo(alignment)));
    }

    /** @brief Align @p value upward to the congruence class @c alignment * n + skew without overflow. */
    [[nodiscard]] constexpr std::optional<uint64_t> TryAlignUp(uint64_t value, Align alignment, uint64_t skew) {
        const uint64_t align = alignment.Value();
        skew %= align;
        if (value <= skew) {
            return skew;
        }
        auto aligned = TryAlignUp(value - skew, alignment);
        if (!aligned || *aligned > std::numeric_limits<uint64_t>::max() - skew) {
            return std::nullopt;
        }
        return *aligned + skew;
    }

    /** @brief Return the adjustment needed to align @p value upward, or @c std::nullopt on overflow. */
    [[nodiscard]] constexpr std::optional<uint64_t> TryOffsetToAlignment(uint64_t value, Align alignment) noexcept {
        auto aligned = TryAlignUp(value, alignment);
        if (!aligned) {
            return std::nullopt;
        }
        return *aligned - value;
    }

    /** @brief Return the modular adjustment needed to align @p value upward. */
    [[nodiscard]] constexpr uint64_t OffsetToAlignmentModulo(uint64_t value, Align alignment) noexcept {
        return AlignUpModulo(value, alignment) - value;
    }

    /** @brief Return the adjustment needed to align @p address upward, or @c std::nullopt on overflow. */
    [[nodiscard]] inline std::optional<uint64_t> TryOffsetToAlignedAddress(const void* address,
                                                                           Align alignment) noexcept {
        return TryOffsetToAlignment(reinterpret_cast<uintptr_t>(address), alignment);
    }

    /** @brief Align @p value upward without overflow; return @c std::nullopt when the result is not representable. */
    [[nodiscard]] constexpr auto CheckedAlignUp(uint64_t value, uint64_t alignment) -> std::optional<uint64_t> {
        auto aligned = Sora::TryAlignUp(value, alignment);
        if (!aligned) {
            return {};
        }
        return aligned;
    }

} // namespace Sora
