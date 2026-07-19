/**
 * @file DirectBuffer.h
 * @brief Own runtime-aligned byte storage for direct transfers and other alignment-constrained operations.
 * @details @ref Sora::DirectBuffer is independent of files and platform APIs. It owns one contiguous allocation,
 * exposes it as byte spans, and preserves the actual allocation alignment across moves. The storage is uninitialized.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Memory/MemoryLayout.h>
#include <Sora/ErrorCode.h>

#include <cstddef>
#include <span>

namespace Sora {

    /** @brief Move-only owner of contiguous runtime-aligned byte storage. */
    class DirectBuffer {
    public:
        /** @brief Construct an empty buffer. */
        constexpr DirectBuffer() noexcept = default;

        /** @brief Release the owned allocation. */
        ~DirectBuffer();

        DirectBuffer(const DirectBuffer&) = delete;
        DirectBuffer& operator=(const DirectBuffer&) = delete;

        /** @brief Transfer ownership from @p other. */
        DirectBuffer(DirectBuffer&& other) noexcept;

        /** @brief Release current storage and transfer ownership from @p other. */
        DirectBuffer& operator=(DirectBuffer&& other) noexcept;

        /**
         * @brief Allocate @p size uninitialized bytes with at least @p alignment.
         * @return The buffer, or @ref ErrorCode::OutOfMemory when allocation fails.
         */
        [[nodiscard]] static Result<DirectBuffer> Allocate(size_t size, Align alignment) noexcept;

        /**
         * @brief Validate a runtime alignment and allocate @p size uninitialized bytes.
         * @return The buffer, @ref ErrorCode::InvalidArgument for a non-power-of-two alignment, or
         * @ref ErrorCode::OutOfMemory when allocation fails.
         */
        [[nodiscard]] static Result<DirectBuffer> Allocate(size_t size, size_t alignment) noexcept;

        /** @brief Return the first mutable byte, or @c nullptr when empty. */
        [[nodiscard]] constexpr std::byte* Data() noexcept { return data_; }

        /** @brief Return the first immutable byte, or @c nullptr when empty. */
        [[nodiscard]] constexpr const std::byte* Data() const noexcept { return data_; }

        /** @brief Return all mutable bytes. */
        [[nodiscard]] constexpr std::span<std::byte> Bytes() noexcept { return {data_, size_}; }

        /** @brief Return all immutable bytes. */
        [[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept { return {data_, size_}; }

        /** @brief Return the storage size in bytes. */
        [[nodiscard]] constexpr size_t Size() const noexcept { return size_; }

        /** @brief Return whether the buffer contains no bytes. */
        [[nodiscard]] constexpr bool Empty() const noexcept { return size_ == 0; }

        /** @brief Return the actual alignment used by the owned allocation. */
        [[nodiscard]] constexpr Align Alignment() const noexcept { return alignment_; }

        /** @brief Release the owned allocation and restore the empty state. */
        void Reset() noexcept;

        /** @brief Exchange ownership and metadata with @p other. */
        void Swap(DirectBuffer& other) noexcept;

        /** @brief Exchange @p left and @p right. */
        friend void swap(DirectBuffer& left, DirectBuffer& right) noexcept { left.Swap(right); }

    private:
        constexpr DirectBuffer(std::byte* data, size_t size, Align alignment) noexcept
            : data_{data}, size_{size}, alignment_{alignment} {}

        std::byte* data_ = nullptr;
        size_t size_ = 0;
        Align alignment_{};
    };

} // namespace Sora
