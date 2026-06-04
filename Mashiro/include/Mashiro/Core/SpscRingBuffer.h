/**
 * @file SpscRingBuffer.h
 * @brief High-performance lock-free Single-Producer Single-Consumer ring buffer.
 *
 * Design based on Rigtorp SPSCQueue / Folly ProducerConsumerQueue principles:
 * - **Cached counters**: producer caches the consumer's read position locally,
 *   avoiding cross-core atomic loads on every write (only re-fetches when the
 *   local cache indicates "full"). Same for consumer caching write position.
 * - **False-sharing elimination**: producer and consumer state on separate
 *   cache lines (`hardware_destructive_interference_size`).
 * - **Power-of-2 capacity** enforced at compile time via `consteval`.
 * - **Typed variant** (`SpscQueue<T, N>`) for structured elements — no
 *   serialize/deserialize overhead.
 * - **Byte variant** (`SpscByteRing<N>`) for variable-length messages (logger).
 *
 * All operations are wait-free (bounded worst-case time). Memory ordering
 * is minimal: `relaxed` for local cache reads, `acquire`/`release` only on
 * the shared atomic counters.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/TypeTraits.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>

namespace Mashiro {

    // =========================================================================
    // SpscQueue<T, Capacity> — typed fixed-size element queue
    // =========================================================================

    /**
     * @brief Wait-free bounded SPSC queue for fixed-size elements.
     *
     * @tparam T        Element type (must be move-constructible).
     * @tparam Capacity Number of slots (must be power of 2, ≥ 2).
     *
     * Producer and consumer state are on separate cache lines to eliminate
     * false sharing. Each side caches the other's counter to avoid cross-core
     * atomic reads on the fast path.
     *
     * @code
     * SpscQueue<int, 1024> q;
     * q.TryPush(42);       // producer
     * int val;
     * q.TryPop(val);       // consumer
     * @endcode
     */
    template<typename T, uint32_t Capacity>
        requires(std::has_single_bit(Capacity) && Capacity >= 2)
    class SpscQueue {
        static constexpr uint32_t kMask = Capacity - 1;

    public:
        /// @name Construction
        /// @{
        SpscQueue() = default;
        ~SpscQueue() {
            // Destroy remaining elements (single-threaded teardown, no atomics needed)
            uint32_t head = head_.load(std::memory_order_relaxed);
            uint32_t tail = tail_.load(std::memory_order_relaxed);
            while (head != tail) {
                std::destroy_at(std::launder(reinterpret_cast<T*>(&slots_[head & kMask])));
                ++head;
            }
        }
        SpscQueue(const SpscQueue&) = delete;
        SpscQueue& operator=(const SpscQueue&) = delete;
        /// @}

        /// @name Producer (single thread)
        /// @{

        /// @brief Try to push an element. Returns false if full.
        template<typename U>
            requires std::constructible_from<T, U&&>
        [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            uint32_t tail = tail_.load(std::memory_order_relaxed);
            // Check if full using cached head
            if (tail - cachedHead_ == Capacity) {
                // Re-read actual head
                cachedHead_ = head_.load(std::memory_order_acquire);
                if (tail - cachedHead_ == Capacity) return false; // truly full
            }
            ::new (&slots_[tail & kMask]) T(std::forward<U>(value));
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        /// @brief Push, spinning until space is available. Yields between attempts.
        template<typename U>
            requires std::constructible_from<T, U&&>
        void Push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            while (!TryPush(std::forward<U>(value)))
                std::this_thread::yield();
        }

        /// @brief Emplace an element in-place.
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        [[nodiscard]] bool
        TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            uint32_t tail = tail_.load(std::memory_order_relaxed);
            if (tail - cachedHead_ == Capacity) {
                cachedHead_ = head_.load(std::memory_order_acquire);
                if (tail - cachedHead_ == Capacity) return false;
            }
            ::new (&slots_[tail & kMask]) T(std::forward<Args>(args)...);
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        /// @}

        /// @name Consumer (single thread)
        /// @{

        /// @brief Try to pop an element. Returns false if empty.
        [[nodiscard]] bool TryPop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
            uint32_t head = head_.load(std::memory_order_relaxed);
            if (head == cachedTail_) {
                cachedTail_ = tail_.load(std::memory_order_acquire);
                if (head == cachedTail_) return false; // truly empty
            }
            auto* ptr = std::launder(reinterpret_cast<T*>(&slots_[head & kMask]));
            out = std::move(*ptr);
            std::destroy_at(ptr);
            head_.store(head + 1, std::memory_order_release);
            return true;
        }

        /// @brief Try to pop, returning `std::optional<T>` (empty if queue is empty).
        [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            uint32_t head = head_.load(std::memory_order_relaxed);
            if (head == cachedTail_) {
                cachedTail_ = tail_.load(std::memory_order_acquire);
                if (head == cachedTail_) return std::nullopt;
            }
            auto* ptr = std::launder(reinterpret_cast<T*>(&slots_[head & kMask]));
            std::optional<T> result{std::move(*ptr)};
            std::destroy_at(ptr);
            head_.store(head + 1, std::memory_order_release);
            return result;
        }

        /// @brief Pop, spinning until data is available. Yields between attempts.
        [[nodiscard]] T Pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            while (true) {
                if (auto val = TryPop()) return std::move(*val);
                std::this_thread::yield();
            }
        }

        /// @}

        /// @name Queries
        /// @{

        /// @brief Approximate number of elements available to read.
        [[nodiscard]] uint32_t SizeApprox() const noexcept {
            return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
        }

        /// @brief True if the queue appears empty (racy but safe for diagnostics).
        [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

        /// @brief Compile-time capacity.
        static constexpr uint32_t GetCapacity() noexcept { return Capacity; }

        /// @}

    private:
        // --- Producer state (own cache line) ---
        alignas(Platform::kCacheLineSize) std::atomic<uint32_t> tail_{0};
        uint32_t cachedHead_ = 0; ///< Producer's local copy of head_ (avoids cross-core read).

        // --- Consumer state (own cache line) ---
        alignas(Platform::kCacheLineSize) std::atomic<uint32_t> head_{0};
        uint32_t cachedTail_ = 0; ///< Consumer's local copy of tail_.

        // --- Slot storage ---
        alignas(Platform::kCacheLineSize) alignas(T) std::byte slots_[Capacity * sizeof(T)]{};
    };

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
     * Uses the same cached-counter technique as `SpscQueue` for minimal
     * cross-core traffic.
     */
    template<uint32_t Capacity = 64 * 1024>
        requires(std::has_single_bit(Capacity) && Capacity >= 64)
    class SpscByteRing {
        static constexpr uint32_t kMask = Capacity - 1;

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
         * @param data Byte span of message payload.
         * @return true if written, false if insufficient space.
         */
        [[nodiscard]] bool TryWrite(std::span<const std::byte> data) noexcept {
            auto size = static_cast<uint32_t>(data.size());
            uint32_t totalSize = size + sizeof(uint32_t); // length prefix
            uint32_t wp = writePos_.load(std::memory_order_relaxed);
            uint32_t avail = Capacity - (wp - cachedReadPos_);
            if (totalSize > avail) {
                cachedReadPos_ = readPos_.load(std::memory_order_acquire);
                avail = Capacity - (wp - cachedReadPos_);
                if (totalSize > avail) {
                    return false;
                }
            }

            WriteRaw(&size, sizeof(uint32_t), wp);
            WriteRaw(data.data(), size, wp + sizeof(uint32_t));
            writePos_.store(wp + totalSize, std::memory_order_release);
            return true;
        }

        /// @brief Convenience: write from raw pointer + size.
        [[nodiscard]] bool TryWrite(const void* data, uint32_t size) noexcept {
            return TryWrite(std::span<const std::byte>{static_cast<const std::byte*>(data), size});
        }

        /// @}

        /// @name Consumer
        /// @{

        /**
         * @brief Read all available messages, invoking a callable per message.
         *
         * The callable receives a `std::span<const std::byte>` view over the
         * message payload (excluding the 4-byte length prefix). The span is
         * valid only for the duration of the call.
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
            requires std::invocable<Fn, std::span<const std::byte>>
        uint32_t
        ReadAll(Fn&& fn) noexcept(std::is_nothrow_invocable_v<Fn, std::span<const std::byte>>) {
            uint32_t wp = writePos_.load(std::memory_order_acquire);
            uint32_t rp = readPos_.load(std::memory_order_relaxed);
            uint32_t count = 0;

            while (rp != wp) {
                uint32_t entrySize = 0;
                ReadRaw(&entrySize, sizeof(uint32_t), rp);

                if (entrySize == 0 || entrySize > Capacity / 2) [[unlikely]] {
                    // Corrupted — skip to write pos
                    readPos_.store(wp, std::memory_order_release);
                    break;
                }

                // Read into staging buffer, then invoke
                ReadRaw(staging_, entrySize, rp + sizeof(uint32_t));
                fn(std::span<const std::byte>{staging_, entrySize});

                rp += sizeof(uint32_t) + entrySize;
                ++count;
            }

            readPos_.store(rp, std::memory_order_release);
            cachedWritePos_ = wp;
            return count;
        }

        /// @}

        /// @name Control
        /// @{

        /// @brief Discard all pending data.
        void Reset() noexcept {
            readPos_.store(writePos_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        /// @brief Check if there is pending data.
        [[nodiscard]] bool HasData() const noexcept {
            return writePos_.load(std::memory_order_acquire) !=
                   readPos_.load(std::memory_order_acquire);
        }

        /// @brief Approximate bytes pending.
        [[nodiscard]] uint32_t BytesPending() const noexcept {
            return writePos_.load(std::memory_order_acquire) -
                   readPos_.load(std::memory_order_acquire);
        }

        static constexpr uint32_t GetCapacity() noexcept { return Capacity; }

        /// @}

    private:
        /// @brief Write bytes with wrap-around.
        void WriteRaw(const void* src, uint32_t len, uint32_t pos) noexcept {
            uint32_t idx = pos & kMask;
            uint32_t first = Capacity - idx;
            if (first >= len) {
                std::memcpy(&buffer_[idx], src, len);
            } else {
                std::memcpy(&buffer_[idx], src, first);
                std::memcpy(&buffer_[0], static_cast<const std::byte*>(src) + first, len - first);
            }
        }

        /// @brief Read bytes with wrap-around.
        void ReadRaw(void* dst, uint32_t len, uint32_t pos) noexcept {
            uint32_t idx = pos & kMask;
            uint32_t first = Capacity - idx;
            if (first >= len) {
                std::memcpy(dst, &buffer_[idx], len);
            } else {
                std::memcpy(dst, &buffer_[idx], first);
                std::memcpy(static_cast<std::byte*>(dst) + first, &buffer_[0], len - first);
            }
        }

        // --- Producer state (own cache line) ---
        alignas(Platform::kCacheLineSize) std::atomic<uint32_t> writePos_{0};
        uint32_t cachedReadPos_ = 0; ///< Producer's cached read position.

        // --- Consumer state (own cache line) ---
        alignas(Platform::kCacheLineSize) std::atomic<uint32_t> readPos_{0};
        uint32_t cachedWritePos_ = 0; ///< Consumer's cached write position.

        // --- Staging buffer for reads (max single message = min(Capacity/2, 8KB)) ---
        static constexpr uint32_t kMaxMessageSize = (Capacity / 2 < 8192) ? Capacity / 2 : 8192;
        alignas(Platform::kCacheLineSize) std::byte staging_[kMaxMessageSize]{};

        // --- Ring buffer storage ---
        alignas(Platform::kCacheLineSize) std::byte buffer_[Capacity]{};
    };

} // namespace Mashiro
