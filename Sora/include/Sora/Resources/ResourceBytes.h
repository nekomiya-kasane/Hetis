/**
 * @file ResourceBytes.h
 * @brief Borrowed resource byte views independent of storage origin.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Resources/ResourceId.h>

#include <cstddef>
#include <span>

namespace Sora::Resources {

    /** @brief Non-owning view of a resource identity and immutable payload bytes. */
    struct ResourceBytesView : ResourceId {
        const unsigned char* data = nullptr; /**< First payload byte, or @c nullptr for an empty payload. */
        size_t size = 0;                     /**< Payload size in bytes. */

        /** @brief Payload as immutable bytes. */
        [[nodiscard]] constexpr auto Bytes() const noexcept -> std::span<const std::byte> {
            if (size == 0) {
                return {};
            }
            return std::as_bytes(std::span{data, size});
        }
    };

} // namespace Sora::Resources
