/**
 * @file SpscByteRing.h
 * @brief Wait-free SPSC byte ring for variable-length messages.
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace Mashiro {

    // =========================================================================
    // SpscByteRing<Capacity> — variable-length byte message queue
    // =========================================================================

    /**
     * @brief Wait-free bounded SPSC byte ring for variable-length messages.
     *
     * @tparam Capacity Ring size in bytes (must be power of 2, ≥ 64).
     *
     * Each message is prefixed with a 4-byte length header. Supports wrap-around
     * via two-part copy. Designed for the structured logger's serialized entries.
     *
     * Uses cached cross-endpoint counters to minimize
     * cross-core traffic. Consumption is **zero-copy** whenever a message is
     * contiguous in the ring (the common case); only messages that wrap the
     * ring boundary are reassembled into a private staging buffer.
     */
    template<uint32_t Capacity = 64 * 1024>
        requires(std::has_single_bit(Capacity) && Capacity >= 64)
    class SpscByteRing {
        static constexpr uint32_t kMask = Capacity - 1;
        static constexpr uint32_t kHeaderSize = sizeof(uint32_t);

        /// @brief Largest accepted payload: half the ring, capped at 8 KB (staging size).
        static consteval uint32_t ComputeMaxMessageSize() {
            constexpr uint32_t kHalf = Capacity / 2;
            constexpr uint32_t kCap = 8192;
            return kHalf < kCap ? kHalf : kCap;
        }
        static constexpr uint32_t kMaxMessageSize = ComputeMaxMessageSize();

        static_assert(std::atomic<uint32_t>::is_always_lock_free, "SpscByteRing requires lock-free 32-bit atomics");

    public:
        /// @name Construction
        /// @{
        SpscByteRing() = default;
        SpscByteRing(const SpscByteRing&) = delete;
        SpscByteRing& operator=(const SpscByteRing&) = delete;
        /// @}

        /// @name Producer
        /// @{

        /**
         * @brief Try to write a variable-length message.
         * @param data Byte span of message payload (1 .. `GetMaxMessageSize()` bytes).
         * @return true if written, false on empty/oversized payload or insufficient space.
         */
        [[nodiscard]] bool TryWrite(std::span<const std::byte> data) noexcept {
            if (data.empty() || data.size() > kMaxMessageSize) [[unlikely]] {
                return false;
            }

            const auto size = static_cast<uint32_t>(data.size());
            const uint32_t totalSize = size + kHeaderSize; // length prefix
            const uint32_t wp = writePos_.load(std::memory_order_relaxed);
            if (totalSize > Capacity - (wp - cachedReadPos_)) {
                // Re-read the actual read position; acquire pairs with the consumer's
                // release store, so consumed bytes are safe to overwrite.
                cachedReadPos_ = readPos_.load(std::memory_order_acquire);
                if (totalSize > Capacity - (wp - cachedReadPos_)) {
                    return false; // truly full
                }
            }

            WriteRaw(&size, kHeaderSize, wp);
            WriteRaw(data.data(), size, wp + kHeaderSize);
            writePos_.store(wp + totalSize, std::memory_order_release);
            return true;
        }

        /// @brief Convenience: write from raw pointer + size.
        [[nodiscard]] bool TryWrite(const void* data, uint32_t size) noexcept {
            if (data == nullptr) [[unlikely]] {
                return false;
            }

            return TryWrite(std::span<const std::byte>{static_cast<const std::byte*>(data), size});
        }

        /// @}

        /// @name Consumer
        /// @{

        /**
         * @brief Read all messages published before the call, invoking a callable per message.
         *
         * The callable receives a `std::span<const std::byte>` view over the
         * message payload (excluding the 4-byte length prefix). The span is
         * valid only for the duration of the call: it aliases the ring buffer
         * directly when the message is contiguous (zero-copy fast path) and an
         * internal staging buffer when the message wraps the ring boundary.
         *
         * **Exception safety (at-most-once delivery):** the consumed position
         * is published on scope exit even if @p fn throws. A message whose
         * callback throws counts as consumed and is *not* redelivered, so a
         * poison message can neither be processed twice nor livelock the
         * consumer.
         *
         * Messages written while the call is in progress (including reentrant
         * `TryWrite` from @p fn on the consumer thread) are deferred to the
         * next call.
         *
         * @tparam Fn Callable with signature compatible with `void(std::span<const std::byte>)`.
         * @param fn Callback invoked per message.
         * @return Number of messages read.
         *
         * @code
         * ring.ReadAll([](std::span<const std::byte> payload) {
         *     // deserialize payload...
         * });
         * @endcode
         */
        template<typename Fn>
            requires std::invocable<Fn&, std::span<const std::byte>>
        uint32_t ReadAll(Fn&& fn) noexcept(std::is_nothrow_invocable_v<Fn&, std::span<const std::byte>>) {
            const uint32_t wp = writePos_.load(std::memory_order_acquire);
            uint32_t rp = readPos_.load(std::memory_order_relaxed);
            uint32_t count = 0;

            // Publishes consumed bytes on normal exit *and* on unwind; the release
            // store pairs with the producer's acquire reload of readPos_.
            struct CommitGuard {
                std::atomic<uint32_t>& pos;
                const uint32_t& value;
                ~CommitGuard() { pos.store(value, std::memory_order_release); }
            } commit{readPos_, rp};

            while (rp != wp) {
                const uint32_t available = wp - rp;
                if (available < kHeaderSize) [[unlikely]] {
                    rp = wp; // corrupted producer state — discard
                    break;
                }

                uint32_t entrySize = 0;
                ReadRaw(&entrySize, kHeaderSize, rp);

                if (entrySize == 0 || entrySize > kMaxMessageSize || kHeaderSize + entrySize > available) [[unlikely]] {
                    // Corrupted — skip to write pos
                    rp = wp;
                    break;
                }

                const uint32_t payloadPos = rp + kHeaderSize;
                // Commit before invoking fn: at-most-once delivery.
                rp = payloadPos + entrySize;
                ++count;

                const uint32_t idx = payloadPos & kMask;
                if (entrySize <= Capacity - idx) [[likely]] {
                    // Zero-copy fast path: payload is contiguous in the ring.
                    fn(std::span<const std::byte>{&buffer_[idx], entrySize});
                } else {
                    // Wrapping payload: reassemble in the staging buffer.
                    ReadRaw(staging_, entrySize, payloadPos);
                    fn(std::span<const std::byte>{staging_, entrySize});
                }
            }
            return count;
        }

        /// @}

        /// @name Control / queries
        /// @{

        /// @brief Discard all pending data. Must be called from the consumer thread
        ///        (it publishes the consumer-owned read position).
        void Reset() noexcept {
            const uint32_t wp = writePos_.load(std::memory_order_acquire);
            readPos_.store(wp, std::memory_order_release);
        }

        /// @brief Check if there is pending data (racy but safe for diagnostics).
        [[nodiscard]] bool HasData() const noexcept {
            // Consumer counter first: readPos_ is monotonic, so the later-loaded
            // writePos_ is never behind the snapshot of readPos_.
            const uint32_t rp = readPos_.load(std::memory_order_acquire);
            return writePos_.load(std::memory_order_acquire) != rp;
        }

        /// @brief Approximate bytes pending (headers included), clamped to capacity.
        [[nodiscard]] uint32_t BytesPending() const noexcept {
            const uint32_t rp = readPos_.load(std::memory_order_acquire);
            const uint32_t wp = writePos_.load(std::memory_order_acquire);
            const uint32_t pending = wp - rp;
            return pending <= Capacity ? pending : Capacity;
        }

        /// @brief Compile-time capacity in bytes.
        [[nodiscard]] static constexpr uint32_t GetCapacity() noexcept { return Capacity; }

        /// @brief Compile-time maximum payload size accepted by `TryWrite`.
        [[nodiscard]] static constexpr uint32_t GetMaxMessageSize() noexcept { return kMaxMessageSize; }

        /// @}

    private:
        /// @brief Write bytes with wrap-around.
        void WriteRaw(const void* src, uint32_t len, uint32_t pos) noexcept {
            const uint32_t idx = pos & kMask;
            const uint32_t first = Capacity - idx;
            if (first >= len) [[likely]] {
                std::memcpy(&buffer_[idx], src, len);
            } else {
                std::memcpy(&buffer_[idx], src, first);
                std::memcpy(&buffer_[0], static_cast<const std::byte*>(src) + first, len - first);
            }
        }

        /// @brief Read bytes with wrap-around.
        void ReadRaw(void* dst, uint32_t len, uint32_t pos) noexcept {
            const uint32_t idx = pos & kMask;
            const uint32_t first = Capacity - idx;
            if (first >= len) [[likely]] {
                std::memcpy(dst, &buffer_[idx], len);
            } else {
                std::memcpy(dst, &buffer_[idx], first);
                std::memcpy(static_cast<std::byte*>(dst) + first, &buffer_[0], len - first);
            }
        }

        // --- Producer state (own cache line) ---
        [[= Concurrency::ProducerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<uint32_t> writePos_{0};
        [[= Concurrency::ProducerOwned{}]] uint32_t cachedReadPos_ = 0; ///< Producer's cached read position.

        // --- Consumer state (own cache line) ---
        [[= Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<uint32_t> readPos_{0};

        // --- Staging buffer: consumer-private scratch, used only for messages
        //     that wrap the ring boundary (uninitialised by design) ---
        [[= Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize) std::byte staging_[kMaxMessageSize];

        // --- Ring buffer storage (uninitialised by design: bytes are always
        //     written before they are read) ---
        [[= Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize) std::byte buffer_[Capacity];
    };

    // =========================================================================
    // Compile-time layout audit (representative instantiations)
    // =========================================================================

    /** @cond INTERNAL */
    consteval {
        static_assert(Concurrency::AuditFalseSharing<SpscByteRing<64 * 1024>>(),
                      "SpscByteRing layout exhibits cross-role false sharing");
    }
    /** @endcond */

} // namespace Mashiro
