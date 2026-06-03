/**
 * @file Flags.h
 * @brief Type-safe bitfield wrapper for scoped enumerations.
 * @copyright Copyright (c) 2024-2026 mitsuki. All rights reserved.
 * @license MIT
 * @ingroup Core
 */
#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <type_traits>

namespace Mashiro {

    /**
     * @brief Type-safe bitfield wrapper for scoped enums.
     *
     * Wraps a scoped `enum class` whose enumerators are powers of two into
     * a value-semantic bitfield with query, mutation, and iteration support.
     *
     * @tparam E Scoped enum type whose enumerators are power-of-2 values.
     *
     * Example usage:
     * @code
     * enum class MyFlags : uint32_t { None = 0, A = 1, B = 2, C = 4 };
     * using MyFlagsSet = EnumFlags<MyFlags>;
     * MyFlagsSet flags = MyFlags::A | MyFlags::B;
     * if (flags.Has(MyFlags::A)) { /* ... */ }
     * @endcode
     */
    template <typename E>
        requires std::is_enum_v<E>
    class EnumFlags {
       public:
        using EnumType = E;              ///< The wrapped enum type.
        using UnderlyingType = std::underlying_type_t<E>; ///< Integer representation.

        /// @name Constructors
        /// @{
        constexpr EnumFlags() noexcept = default;
        /// @brief Implicit conversion from a single enumerator.
        constexpr EnumFlags(E value) noexcept : bits_(static_cast<UnderlyingType>(value)) {}
        /// @brief Explicit construction from a raw bitmask.
        constexpr explicit EnumFlags(UnderlyingType bits) noexcept : bits_(bits) {}
        /// @}

        /// @name Queries
        /// @{

        /// @brief Test whether a single flag bit is set.
        [[nodiscard]] constexpr auto Has(E flag) const noexcept -> bool {
            auto f = static_cast<UnderlyingType>(flag);
            return (bits_ & f) == f;
        }

        /// @brief Test whether *any* bit in @p other is also set here.
        [[nodiscard]] constexpr auto HasAny(EnumFlags other) const noexcept -> bool {
            return (bits_ & other.bits_) != 0;
        }

        /// @brief Test whether *all* bits in @p other are also set here.
        [[nodiscard]] constexpr auto HasAll(EnumFlags other) const noexcept -> bool {
            return (bits_ & other.bits_) == other.bits_;
        }

        /// @}

        /// @name Mutators
        /// @{

        /// @brief Set a single flag bit.
        constexpr auto Set(E flag) noexcept -> EnumFlags& {
            bits_ |= static_cast<UnderlyingType>(flag);
            return *this;
        }

        /// @brief Clear a single flag bit.
        constexpr auto Clear(E flag) noexcept -> EnumFlags& {
            bits_ &= ~static_cast<UnderlyingType>(flag);
            return *this;
        }

        /// @brief Toggle (XOR) a single flag bit.
        constexpr auto Toggle(E flag) noexcept -> EnumFlags& {
            bits_ ^= static_cast<UnderlyingType>(flag);
            return *this;
        }

        /// @brief Clear all bits (set to zero).
        constexpr auto ClearAll() noexcept -> EnumFlags& {
            bits_ = 0;
            return *this;
        }

        /// @}

        /// @name Accessors
        /// @{
        [[nodiscard]] constexpr auto IsEmpty()  const noexcept -> bool           { return bits_ == 0; }            ///< True if no bits are set.
        [[nodiscard]] constexpr auto GetRaw()   const noexcept -> UnderlyingType { return bits_; }                 ///< Raw integer bitmask.
        [[nodiscard]] constexpr auto PopCount() const noexcept -> int            { return std::popcount(bits_); }  ///< Number of set bits.
        /// @}

        /// @name Bitwise compound-assignment operators
        /// @{
        constexpr auto operator|=(EnumFlags other) noexcept -> EnumFlags& { bits_ |= other.bits_; return *this; }
        constexpr auto operator&=(EnumFlags other) noexcept -> EnumFlags& { bits_ &= other.bits_; return *this; }
        constexpr auto operator^=(EnumFlags other) noexcept -> EnumFlags& { bits_ ^= other.bits_; return *this; }
        /// @}

        /// @name Bitwise operators (hidden friends)
        /// @{
        [[nodiscard]] friend constexpr auto operator|(EnumFlags a, EnumFlags b) noexcept -> EnumFlags {
            return EnumFlags{static_cast<UnderlyingType>(a.bits_ | b.bits_)};
        }
        [[nodiscard]] friend constexpr auto operator&(EnumFlags a, EnumFlags b) noexcept -> EnumFlags {
            return EnumFlags{static_cast<UnderlyingType>(a.bits_ & b.bits_)};
        }
        [[nodiscard]] friend constexpr auto operator^(EnumFlags a, EnumFlags b) noexcept -> EnumFlags {
            return EnumFlags{static_cast<UnderlyingType>(a.bits_ ^ b.bits_)};
        }
        [[nodiscard]] friend constexpr auto operator~(EnumFlags a) noexcept -> EnumFlags {
            return EnumFlags{static_cast<UnderlyingType>(~a.bits_)};
        }
        /// @}

        /// @name Comparison operators (hidden friends)
        /// @{
        [[nodiscard]] friend constexpr auto operator==(EnumFlags a, EnumFlags b) noexcept -> bool {
            return a.bits_ == b.bits_;
        }
        [[nodiscard]] friend constexpr auto operator!=(EnumFlags a, EnumFlags b) noexcept -> bool {
            return a.bits_ != b.bits_;
        }
        /// @}

        /// @brief Enable `MyFlags::A | MyFlags::B` to produce an EnumFlags directly.
        [[nodiscard]] friend constexpr auto operator|(E a, E b) noexcept -> EnumFlags {
            return EnumFlags{a} | EnumFlags{b};
        }

        /**
         * @brief Forward iterator that yields each set bit as an enumerator.
         *
         * Iterates from the lowest set bit to the highest, extracting one
         * enumerator per step. Satisfies `std::forward_iterator`.
         */
        class Iterator {
           public:
            using iterator_category = std::forward_iterator_tag;
            using value_type        = E;
            using difference_type   = std::ptrdiff_t;
            using pointer           = const E*;
            using reference         = E;

            constexpr Iterator() noexcept = default;
            /// @brief Construct from the raw bitmask to iterate.
            constexpr explicit Iterator(UnderlyingType bits) noexcept : remaining_(bits) { Advance(); }

            /// @brief Dereference: returns the current flag as an enumerator.
            [[nodiscard]] constexpr auto operator*() const noexcept -> E { return static_cast<E>(current_); }

            constexpr auto operator++() noexcept -> Iterator& {
                remaining_ &= ~current_;
                Advance();
                return *this;
            }

            constexpr auto operator++(int) noexcept -> Iterator {
                auto tmp = *this;
                ++(*this);
                return tmp;
            }

            [[nodiscard]] friend constexpr auto operator==(const Iterator& a, const Iterator& b) noexcept -> bool {
                return a.remaining_ == b.remaining_;
            }
            [[nodiscard]] friend constexpr auto operator!=(const Iterator& a, const Iterator& b) noexcept -> bool {
                return a.remaining_ != b.remaining_;
            }

           private:
            /// @brief Isolate the lowest set bit into @c current_.
            constexpr void Advance() noexcept {
                if (remaining_ != 0) {
                    current_ = remaining_ & (~remaining_ + 1);
                } else {
                    current_ = 0;
                }
            }

            UnderlyingType remaining_ = 0; ///< Bits not yet yielded.
            UnderlyingType current_   = 0; ///< Isolated current bit.
        };

        /// @name Range interface
        /// @{
        [[nodiscard]] constexpr auto begin() const noexcept -> Iterator { return Iterator{bits_}; }
        [[nodiscard]] constexpr auto end()   const noexcept -> Iterator { return Iterator{0}; }
        /// @}

       private:
        UnderlyingType bits_ = 0;
    };

/**
 * @brief Convenience macro: injects a free `operator|` so that two bare
 *        enumerators combine into an @c EnumFlags without an explicit cast.
 *
 * Place this in the same namespace as @p EnumType (or at global scope).
 *
 * @code
 * enum class Access : uint8_t { Read = 1, Write = 2, Exec = 4 };
 * MIKI_ENABLE_ENUM_FLAGS(Access);
 * auto rw = Access::Read | Access::Write; // EnumFlags<Access>
 * @endcode
 */
#define MIKI_ENABLE_ENUM_FLAGS(EnumType)                                                    \
    [[nodiscard]] inline constexpr auto operator|(EnumType a, EnumType b) noexcept          \
        -> ::miki::core::EnumFlags<EnumType> {                                              \
        return ::miki::core::EnumFlags<EnumType>{a} | ::miki::core::EnumFlags<EnumType>{b}; \
    }

}  // namespace Mashiro
