/**
 * @file BoundedByteRecord.h
 * @brief Allocation-free owning byte record for asynchronous queue transfer.
 * @ingroup Concurrency
 */
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace Sora::Concurrency {

    /**
     * @brief Fixed-capacity owning byte record whose copy cost is proportional to its current size.
     * @details Sender operation states must own values across suspension. This type provides that ownership without
     * allocation, while retaining the variable-length record semantics of @ref Sora::RecordRingBuffer.
     * Empty is a staging/moved-from state and is outside the queue's enqueue domain. Mutable byte access may alter the
     * existing payload but cannot change its logical size. The value owns no synchronization for concurrent mutation.
     * @tparam Capacity Maximum non-zero payload size in bytes.
     */
    template<std::size_t Capacity>
        requires(Capacity > 0 && Capacity <= std::numeric_limits<std::uint32_t>::max())
    class BoundedByteRecord {
    public:
        static constexpr std::size_t kCapacity = Capacity;

        /** @brief Construct an empty staging value. Empty values cannot be enqueued. */
        BoundedByteRecord() noexcept = default;

        /** @brief Copy one valid non-empty record or throw when it exceeds this value's domain. */
        explicit BoundedByteRecord(std::span<const std::byte> bytes) {
            if (!TryAssign(bytes)) {
                throw std::length_error("BoundedByteRecord requires 1..Capacity bytes");
            }
        }

        BoundedByteRecord(const BoundedByteRecord& other) noexcept : size_(other.size_) {
            std::memcpy(bytes_.data(), other.bytes_.data(), size_);
        }

        BoundedByteRecord& operator=(const BoundedByteRecord& other) noexcept {
            if (this != &other) {
                std::memcpy(bytes_.data(), other.bytes_.data(), other.size_);
                size_ = other.size_;
            }
            return *this;
        }

        BoundedByteRecord(BoundedByteRecord&& other) noexcept : size_(other.size_) {
            std::memcpy(bytes_.data(), other.bytes_.data(), size_);
            other.size_ = 0;
        }

        BoundedByteRecord& operator=(BoundedByteRecord&& other) noexcept {
            if (this != &other) {
                std::memcpy(bytes_.data(), other.bytes_.data(), other.size_);
                size_ = other.size_;
                other.size_ = 0;
            }
            return *this;
        }

        /** @brief Replace the value with @p bytes; preserve the previous value when the input is invalid. */
        [[nodiscard]] bool TryAssign(std::span<const std::byte> bytes) noexcept {
            if (bytes.empty() || bytes.size() > Capacity) {
                return false;
            }
            std::memcpy(bytes_.data(), bytes.data(), bytes.size());
            size_ = static_cast<std::uint32_t>(bytes.size());
            return true;
        }

        /** @brief Discard the logical payload without clearing inactive storage. */
        void Clear() noexcept { size_ = 0; }

        /** @brief Return the owned payload view. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return {bytes_.data(), size_}; }

        /** @brief Return the mutable payload view without changing its logical size. */
        [[nodiscard]] std::span<std::byte> Bytes() noexcept { return {bytes_.data(), size_}; }

        [[nodiscard]] std::byte* data() noexcept { return bytes_.data(); }
        [[nodiscard]] const std::byte* data() const noexcept { return bytes_.data(); }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] friend bool operator==(const BoundedByteRecord& left, const BoundedByteRecord& right) noexcept {
            return std::ranges::equal(left.Bytes(), right.Bytes());
        }

    private:
        std::array<std::byte, Capacity> bytes_;
        std::uint32_t size_{0};
    };

} // namespace Sora::Concurrency
