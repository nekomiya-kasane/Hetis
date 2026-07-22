/**
 * @file Flags.h
 * @brief Reflection-driven bitfield operations and fixed-capacity sets for scoped enumerations.
 *
 * This header supports two distinct enum models. A @ref Sora::Concept::BitfieldEnum stores membership directly in
 * power-of-two enumerator values, while a @ref Sora::Concept::SequentialEnum stores membership separately in @ref
 * Sora::EnumSet. Both models are validated and sized through C++26 reflection.
 *
 * For bitfield enums this header provides:
 *
 * - Bitwise operators (@c |, @c &, @c ^, @c ~, @c |=, @c &=, @c ^=) that return the enum type itself; no wrapper
 *   class, no macros, no opt-in boilerplate.
 * - Query functions: @ref HasFlag, @ref HasAny, @ref HasAll, @ref IsEmpty, @ref PopCount.
 * - Mutation functions: @ref SetFlag, @ref ClearFlag, @ref ToggleFlag.
 * - Iteration: @ref EachFlag returns a lightweight range that yields each set bit as its enumerator, lowest first.
 * - Compile-time safety: @ref Sora::Concept::BitfieldEnum is validated at compile time via P2996 reflection;
 *   a non-power-of-2 enumerator is a hard error.
 * - Masked complement: @c operator~ only flips bits within the valid flag space (OR of all enumerators), never
 *   sets high garbage bits.
 *
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/Assertion.h"
#include "Sora/Core/Traits/EnumTraits.h"

#include <bit>
#include <bitset>
#include <cstddef>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <ranges>
#include <type_traits>
#include <utility>

// =========================================================================
// Bitwise operators: return E, not a wrapper
// =========================================================================

/**
 * @name Bitwise operators for Sora::Concept::BitfieldEnum
 * @{
 */

/**
 * @brief Bitwise OR for any reflected Sora bitfield enum.
 *
 * @details The operator intentionally lives in the global namespace. ADL for an enum declared in @c Sora::Nested
 * associates @c Sora::Nested, not necessarily @c Sora, so a parent-namespace operator is not a stable
 * zero-boilerplate solution for nested Sora modules.
 */
template<Sora::Concept::BitfieldEnum E>
[[nodiscard]] constexpr E operator|(E a, E b) noexcept {
    using U = std::make_unsigned_t<std::underlying_type_t<E>>;
    return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
}

/** @brief Bitwise AND. */
template<Sora::Concept::BitfieldEnum E>
[[nodiscard]] constexpr E operator&(E a, E b) noexcept {
    using U = std::make_unsigned_t<std::underlying_type_t<E>>;
    return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
}

/** @brief Bitwise XOR. */
template<Sora::Concept::BitfieldEnum E>
[[nodiscard]] constexpr E operator^(E a, E b) noexcept {
    using U = std::make_unsigned_t<std::underlying_type_t<E>>;
    return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));
}

/** @brief Masked complement that only flips bits within the valid flag space. */
template<Sora::Concept::BitfieldEnum E>
[[nodiscard]] constexpr E operator~(E a) noexcept {
    using U = std::make_unsigned_t<std::underlying_type_t<E>>;
    return static_cast<E>(~static_cast<U>(a) & Sora::Traits::BitfieldMask<E>);
}

/** @brief Compound OR. */
template<Sora::Concept::BitfieldEnum E>
constexpr E& operator|=(E& a, E b) noexcept {
    return a = a | b;
}

/** @brief Compound AND. */
template<Sora::Concept::BitfieldEnum E>
constexpr E& operator&=(E& a, E b) noexcept {
    return a = a & b;
}

/** @brief Compound XOR. */
template<Sora::Concept::BitfieldEnum E>
constexpr E& operator^=(E& a, E b) noexcept {
    return a = a ^ b;
}

/** @} */

namespace Sora {

    // =========================================================================
    // Query functions
    // =========================================================================

    /**
     * @name Bitfield queries
     * @{
     */

    /**
     * @brief Test whether all bits of @p flag are set in @p set.
     */
    template<Concept::BitfieldEnum E>
    [[nodiscard]] constexpr bool HasFlag(E set, E flag) noexcept {
        using U = std::make_unsigned_t<std::underlying_type_t<E>>;
        auto f = static_cast<U>(flag);
        return f == 0 ? static_cast<U>(set) == 0 : (static_cast<U>(set) & f) == f;
    }

    /**
     * @brief Test whether *any* bit of @p test is set in @p set.
     */
    template<Concept::BitfieldEnum E>
    [[nodiscard]] constexpr bool HasAny(E set, E test) noexcept {
        using U = std::make_unsigned_t<std::underlying_type_t<E>>;
        return (static_cast<U>(set) & static_cast<U>(test)) != 0;
    }

    /**
     * @brief Test whether *all* bits of @p test are set in @p set.
     */
    template<Concept::BitfieldEnum E>
    [[nodiscard]] constexpr bool HasAll(E set, E test) noexcept {
        using U = std::make_unsigned_t<std::underlying_type_t<E>>;
        return (static_cast<U>(set) & static_cast<U>(test)) == static_cast<U>(test);
    }

    /**
     * @brief True if no bits are set.
     */
    template<Concept::BitfieldEnum E>
    [[nodiscard]] constexpr bool IsEmpty(E e) noexcept {
        return static_cast<std::make_unsigned_t<std::underlying_type_t<E>>>(e) == 0;
    }

    /** @brief Return whether @p set contains no bits outside the reflected enumerator mask. */
    template<Concept::BitfieldEnum E>
    [[nodiscard]] constexpr bool IsValidFlagSet(E set) noexcept {
        using U = std::make_unsigned_t<std::underlying_type_t<E>>;
        return (static_cast<U>(set) & ~static_cast<U>(Traits::BitfieldMask<E>)) == 0;
    }

    /**
     * @brief Number of set bits.
     */
    template<Concept::BitfieldEnum E>
    [[nodiscard]] constexpr int PopCount(E e) noexcept {
        return std::popcount(static_cast<std::make_unsigned_t<std::underlying_type_t<E>>>(e));
    }

    /** @} */

    // =========================================================================
    // Mutation functions
    // =========================================================================

    /**
     * @name Bitfield mutation
     * @{
     */

    /**
     * @brief Set flag bit(s).
     */
    template<Concept::BitfieldEnum E>
    constexpr E& SetFlag(E& set, E flag) noexcept {
        return set |= flag;
    }

    /**
     * @brief Clear flag bit(s).
     */
    template<Concept::BitfieldEnum E>
    constexpr E& ClearFlag(E& set, E flag) noexcept {
        return set = set & ~flag;
    }

    /**
     * @brief Toggle flag bit(s).
     */
    template<Concept::BitfieldEnum E>
    constexpr E& ToggleFlag(E& set, E flag) noexcept {
        return set ^= flag;
    }

    /** @} */

    // =========================================================================
    // Iteration: yields each set bit as its enumerator (lowest first)
    // =========================================================================

    /**
     * @brief Lightweight input range over the set bits of a bitfield enum.
     */
    template<Concept::BitfieldEnum E>
    class FlagRange {
        using U = std::make_unsigned_t<std::underlying_type_t<E>>;
        U bits_;

    public:
        constexpr explicit FlagRange(E flags) noexcept : bits_{static_cast<U>(flags)} {}

        struct Sentinel {};

        struct Iterator {
            using difference_type = std::ptrdiff_t;
            using value_type = E;

            U bits;

            constexpr E operator*() const noexcept { return static_cast<E>(bits & static_cast<U>(-bits)); }
            constexpr Iterator& operator++() noexcept {
                bits &= bits - 1; // clear lowest set bit
                return *this;
            }
            constexpr void operator++(int) noexcept { ++*this; }
            friend constexpr bool operator==(Iterator it, Sentinel) noexcept { return it.bits == 0; }
        };

        [[nodiscard]] constexpr Iterator begin() const noexcept { return {bits_}; }
        [[nodiscard]] constexpr Sentinel end() const noexcept { return {}; }
    };

    /**
     * @brief Lazily yields each set flag bit as its enumerator, lowest first.
     *
     * @code{.cpp}
     * for (auto f : EachFlag(Access::Read | Access::Exec)) {  Read, Exec  }
     * @endcode
     */
    template<Concept::BitfieldEnum E>
    [[nodiscard]] constexpr FlagRange<E> EachFlag(E flags) noexcept {
        return FlagRange<E>{flags};
    }

    /**
     * @brief Fixed-capacity set over a reflected sequential enum.
     * @tparam E Enum whose ordinary enumerators have unique underlying values.
     *
     * @details Bits correspond to ordinary enumerators in declaration order after excluding @ref
     * Concept::SpecialEnumerator values. Reflection derives the capacity, so no synthetic @c Count sentinel is needed.
     * An excluded enumerator that aliases an ordinary value necessarily denotes the same membership because C++ enum
     * values do not retain their source enumerator identity. The type performs no dynamic allocation.
     */
    template<Concept::SequentialEnum E>
    class EnumSet {
    public:
        /** @brief Number of values in the reflected enum domain. */
        inline static constexpr size_t kSize = Traits::OrdinaryEnumeratorsCountOf<E>;

        /** @brief Exact fixed-size storage used by this set. */
        using Storage = std::bitset<kSize>;

        /** @brief Construct an empty set. */
        constexpr EnumSet() noexcept = default;

        /** @brief Construct a singleton containing @p value. */
        constexpr explicit EnumSet(E value) noexcept { Add(value); }

        /** @brief Construct a set containing every value in @p values. */
        constexpr EnumSet(std::initializer_list<E> values) noexcept {
            for (E value : values) {
                Add(value);
            }
        }

        /** @brief Construct from an exact-width bitset representation. */
        constexpr explicit EnumSet(Storage bits) noexcept : bits_{bits} {}

        /** @brief Return the set containing every reflected enum value. */
        [[nodiscard]] static constexpr EnumSet All() noexcept {
            EnumSet result;
            result.bits_.set();
            return result;
        }

        /** @brief Return whether @p value belongs to this set. */
        [[nodiscard]] constexpr bool Contains(E value) const noexcept {
            const size_t index = IndexOf(value);
            return index < kSize && bits_[index];
        }

        /** @brief Return whether this set contains at least one value from @p other. */
        [[nodiscard]] constexpr bool ContainsAny(const EnumSet& other) const noexcept {
            return (bits_ & other.bits_).any();
        }

        /** @brief Return whether this set contains every value from @p other. */
        [[nodiscard]] constexpr bool ContainsAll(const EnumSet& other) const noexcept {
            return (bits_ & other.bits_) == other.bits_;
        }

        /** @brief Return whether this set contains no values. */
        [[nodiscard]] constexpr bool Empty() const noexcept { return bits_.none(); }

        /** @brief Return whether this set contains at least one value. */
        [[nodiscard]] constexpr bool Any() const noexcept { return bits_.any(); }

        /** @brief Return the number of contained values. */
        [[nodiscard]] constexpr size_t Count() const noexcept { return bits_.count(); }

        /** @brief Return the exact fixed-size bitset representation. */
        [[nodiscard]] constexpr const Storage& Bits() const& noexcept { return bits_; }

        /** @brief Set membership of @p value to @p enabled. */
        constexpr EnumSet& Set(E value, bool enabled = true) noexcept {
            const size_t index = IndexOf(value);
            if (index < kSize) {
                bits_[index] = enabled;
            }
            return *this;
        }

        /** @brief Add @p value. */
        constexpr EnumSet& Add(E value) noexcept { return Set(value); }

        /** @brief Add every value in @p values. */
        constexpr EnumSet& Add(std::initializer_list<E> values) noexcept {
            for (E value : values) {
                Add(value);
            }
            return *this;
        }

        /** @brief Remove @p value. */
        constexpr EnumSet& Remove(E value) noexcept { return Set(value, false); }

        /** @brief Remove every value in @p values. */
        constexpr EnumSet& Remove(std::initializer_list<E> values) noexcept {
            for (E value : values) {
                Remove(value);
            }
            return *this;
        }

        /** @brief Toggle membership of @p value. */
        constexpr EnumSet& Toggle(E value) noexcept {
            const size_t index = IndexOf(value);
            if (index < kSize) {
                bits_.flip(index);
            }
            return *this;
        }

        /** @brief Remove every value. */
        constexpr void Clear() noexcept { bits_.reset(); }

        /**
         * @brief Lazily enumerate contained values in declaration order.
         * @return A view borrowing this set.
         */
        [[nodiscard]] constexpr auto Values() const& noexcept {
            return std::views::iota(size_t{0}, kSize) |
                   std::views::filter([this](size_t index) noexcept { return bits_[index]; }) |
                   std::views::transform(
                       [](size_t index) noexcept { return Traits::OrdinaryEnumeratorsArrOf<E>[index]; });
        }

        /** @brief Invoke @p function once for each contained value in declaration order. */
        template<typename Function>
            requires std::invocable<Function&, E>
        constexpr void ForEach(Function&& function) const noexcept(std::is_nothrow_invocable_v<Function&, E>) {
            for (E value : Values()) {
                std::invoke(function, value);
            }
        }

        /** @brief Add every value from @p other. */
        constexpr EnumSet& operator|=(const EnumSet& other) noexcept {
            bits_ |= other.bits_;
            return *this;
        }

        /** @brief Retain only values also contained by @p other. */
        constexpr EnumSet& operator&=(const EnumSet& other) noexcept {
            bits_ &= other.bits_;
            return *this;
        }

        /** @brief Toggle every value contained by @p other. */
        constexpr EnumSet& operator^=(const EnumSet& other) noexcept {
            bits_ ^= other.bits_;
            return *this;
        }

        /** @brief Remove every value contained by @p other. */
        constexpr EnumSet& operator-=(const EnumSet& other) noexcept {
            bits_ &= ~other.bits_;
            return *this;
        }

        /** @brief Exchange storage with @p other. */
        constexpr void Swap(EnumSet& other) noexcept { std::swap(bits_, other.bits_); }

        /** @brief Compare exact membership. */
        friend constexpr bool operator==(const EnumSet&, const EnumSet&) noexcept = default;

        /** @brief Return the union of @p left and @p right. */
        friend constexpr EnumSet operator|(EnumSet left, const EnumSet& right) noexcept {
            left |= right;
            return left;
        }

        /** @brief Return the intersection of @p left and @p right. */
        friend constexpr EnumSet operator&(EnumSet left, const EnumSet& right) noexcept {
            left &= right;
            return left;
        }

        /** @brief Return the symmetric difference of @p left and @p right. */
        friend constexpr EnumSet operator^(EnumSet left, const EnumSet& right) noexcept {
            left ^= right;
            return left;
        }

        /** @brief Return values contained by @p left but not @p right. */
        friend constexpr EnumSet operator-(EnumSet left, const EnumSet& right) noexcept {
            left -= right;
            return left;
        }

        /** @brief Return the complement within the reflected enum domain. */
        friend constexpr EnumSet operator~(EnumSet set) noexcept {
            set.bits_.flip();
            return set;
        }

        /** @brief Exchange @p left and @p right. */
        friend constexpr void swap(EnumSet& left, EnumSet& right) noexcept { left.Swap(right); }

    private:
        /** @brief Validate @p value and return its bit index. */
        [[nodiscard]] static constexpr size_t IndexOf(E value) noexcept {
            if constexpr (Concept::OrdinalEnum<E>) {
                using Raw = std::underlying_type_t<E>;
                using Unsigned = std::make_unsigned_t<Raw>;

                const Raw raw = std::to_underlying(value);
                const bool nonnegative = [](Raw candidate) constexpr {
                    if constexpr (std::signed_integral<Raw>) {
                        return candidate >= 0;
                    } else {
                        return true;
                    }
                }(raw);
                const auto index = static_cast<size_t>(static_cast<Unsigned>(raw));
                if (nonnegative && index < kSize) {
                    return index;
                }
            } else {
                size_t index = 0;
                template for (constexpr auto enumerator : Meta::EnumeratorsOf(^^E)) {
                    if constexpr (!Concept::SpecialEnumerator<enumerator>) {
                        if (value == [:enumerator:]) {
                            return index;
                        }
                        ++index;
                    }
                }
            }
            return InvalidIndex();
        }

        /** @brief Report an enum value outside the ordinary reflected domain. */
        [[nodiscard]] static constexpr size_t InvalidIndex() noexcept {
            if consteval {
                std::unreachable();
            } else {
                static_cast<void>(Verify(false, "EnumSet received a value outside its reflected enum domain."));
            }
            return kSize;
        }

        Storage bits_{};
    };

} // namespace Sora
