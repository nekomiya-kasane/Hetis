/**
 * @file BufferSlice.h
 * @brief Owning contiguous byte slices of logical compute buffers.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/Buffer.h>

#include <cstdint>
#include <expected>
#include <utility>

namespace Sora::Compute {

    /** @brief Stable reason a contiguous logical buffer slice could not be formed. */
    enum class BufferSliceError : uint8_t {
        InvalidBuffer,
        InvalidRegion,
    };

    /** @brief Owning logical Buffer lease restricted to one non-empty contiguous byte interval. */
    class BufferSlice {
    public:
        BufferSlice() noexcept = default;

        /** @brief Validate @p region against @p buffer and retain its logical lifetime. */
        [[nodiscard]] static std::expected<BufferSlice, BufferSliceError> Create(Buffer buffer,
                                                                                 ByteRegion region) noexcept {
            if (!buffer) {
                return std::unexpected(BufferSliceError::InvalidBuffer);
            }
            const auto whole = ByteRegion::Make(0, buffer.Describe().sizeBytes);
            if (!whole || region.Size() == 0 || !whole->Contains(region)) {
                return std::unexpected(BufferSliceError::InvalidRegion);
            }
            return BufferSlice{std::move(buffer), region};
        }

        /** @brief Return whether this handle denotes a validated slice. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(buffer_); }

        /** @brief Return the retained logical buffer. @pre This slice is non-empty. */
        [[nodiscard]] const Buffer& LogicalBuffer() const noexcept { return buffer_; }

        /** @brief Return the underlying logical buffer descriptor. @pre This slice is non-empty. */
        [[nodiscard]] BufferDesc BufferDescriptor() const noexcept { return buffer_.Describe(); }

        /** @brief Return the contiguous byte interval. */
        [[nodiscard]] ByteRegion Region() const noexcept { return region_; }

    private:
        BufferSlice(Buffer buffer, ByteRegion region) noexcept : buffer_(std::move(buffer)), region_(region) {}

        Buffer buffer_{};
        ByteRegion region_{};
    };

} // namespace Sora::Compute
