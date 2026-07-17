/**
 * @file Handle.h
 * @brief Define compact generational handles for index-addressed Sora containers.
 * @details
 *
 * @ref Sora::Handle stores a 32-bit index and 32-bit generation in eight bytes. @ref Sora::ShortHandle is the
 *
 * corresponding four-byte representation for containers whose index and generation each fit in 16 bits. Index zero
 *
 * denotes a null handle; individual containers define generation validation and ownership rules.
 * @ingroup Core
 */

#pragma once

#include <compare>
#include <cstdint>

namespace Sora {

    /** @brief Eight-byte generational handle for containers with 32-bit sparse indices. */
    struct Handle {
        uint32_t index = 0;      /**< Sparse slot index; zero denotes the null handle. */
        uint32_t generation = 0; /**< Slot generation captured when the value was inserted. */

        /** @brief Return whether this handle is non-null. */
        [[nodiscard]] constexpr bool IsValid() const noexcept { return index != 0; }

        /** @brief Convert to @c true when this handle is non-null. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }

        /** @brief Return the null handle. */
        [[nodiscard]] static constexpr Handle Null() noexcept { return {}; }

        friend constexpr bool operator==(const Handle&, const Handle&) noexcept = default;
        friend constexpr auto operator<=>(const Handle& lhs, const Handle& rhs) noexcept {
            if (lhs.index != rhs.index) {
                return lhs.index <=> rhs.index;
            }
            return lhs.generation <=> rhs.generation;
        }
    };

    /** @brief Four-byte generational handle for containers with 16-bit sparse indices. */
    struct ShortHandle {
        uint16_t index = 0;      /**< Sparse slot index; zero denotes the null handle. */
        uint16_t generation = 0; /**< Slot generation captured when the value was inserted. */

        /** @brief Return whether this handle is non-null. */
        [[nodiscard]] constexpr bool IsValid() const noexcept { return index != 0; }

        /** @brief Convert to @c true when this handle is non-null. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }

        /** @brief Return the null handle. */
        [[nodiscard]] static constexpr ShortHandle Null() noexcept { return {}; }

        friend constexpr bool operator==(const ShortHandle&, const ShortHandle&) noexcept = default;
        friend constexpr auto operator<=>(const ShortHandle& lhs, const ShortHandle& rhs) noexcept {
            if (lhs.index != rhs.index) {
                return lhs.index <=> rhs.index;
            }
            return lhs.generation <=> rhs.generation;
        }
    };

    static_assert(sizeof(Handle) == 8);
    static_assert(sizeof(ShortHandle) == 4);

} // namespace Sora
