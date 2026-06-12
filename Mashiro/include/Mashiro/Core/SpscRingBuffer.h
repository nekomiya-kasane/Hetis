/**
 * @file SpscRingBuffer.h
 * @brief High-performance lock-free Single-Producer Single-Consumer ring buffer.
 *
 * Design based on Rigtorp SPSCQueue / Folly ProducerConsumerQueue principles:
 * - **Cached counters**: producer caches the consumer's read position locally,
 *   avoiding cross-core atomic loads on every write (only re-fetches when the
 *   local cache indicates "full"). Same for consumer caching write position.
 * - **False-sharing elimination**: producer and consumer state on separate
 *   cache lines (`hardware_destructive_interference_size`), *proven* at
 *   compile time by a reflection-driven layout audit (see below).
 * - **Power-of-2 capacity** enforced at compile time via constraints.
 * - **Typed variant** (`SpscQueue<T, N>`) for structured elements — no
 *   serialize/deserialize overhead, plus zero-copy `Front()`/`PopFront()`.
 * - **Byte variant** (`SpscByteRing<N>`) for variable-length messages (logger),
 *   with zero-copy consumption of non-wrapping messages.
 *
 * All operations are wait-free (bounded worst-case time). Memory ordering
 * is minimal: `relaxed` for locally-owned counters, `acquire`/`release` only
 * on the shared atomic counters. Counters are free-running `uint32_t`; all
 * index arithmetic is performed modulo 2^32, which stays exact because the
 * live span never exceeds `Capacity` (≤ 2^31).
 *
 * ### Compile-time concurrency audit (C++26)
 * Every data member is tagged with a thread-ownership annotation
 * (`[[=Concurrency::ProducerOwned{}]]`, `ConsumerOwned`, `SharedStorage`).
 * `Concurrency::AuditFalseSharing<Ring>()` walks the members with P2996
 * static reflection and verifies that members of different roles never
 * overlap a cache line, so the false-sharing claims above are checked by
 * the compiler in the `consteval` block at the end of this header.
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
#include <memory>
#include <meta>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Mashiro {

    // =========================================================================
    // Concurrency role annotations + reflection-driven false-sharing audit
    // =========================================================================

    /**
     * @brief Thread-ownership annotations and compile-time layout audits.
     *
     * Members of concurrent data structures are tagged with one of the role
     * annotations below; `AuditFalseSharing` then proves — via C++26 static
     * reflection over the annotated members — that no two members of
     * different roles ever share a cache line.
     */
    namespace Concurrency {

        struct ProducerOwned {}; ///< Annotation: written by the producer thread only.
        struct ConsumerOwned {}; ///< Annotation: written by the consumer thread only.
        struct SharedStorage {}; ///< Annotation: accessed by both threads at disjoint offsets.

        /** @cond INTERNAL */
        namespace Detail {

            /// @brief Byte offset of a reflected non-static data member (tolerant of
            ///        `offset_of` returning `member_offset` or a raw integer).
            consteval size_t MemberOffsetBytes(std::meta::info member) {
                return static_cast<size_t>(std::meta::offset_of(member).bytes);
            }

        } // namespace Detail
        /** @endcond */

        /**
         * @brief Reflection-driven false-sharing audit.
         *
         * Verifies, entirely at compile time, that:
         * 1. every non-static data member of @p Ring carries exactly one
         *    `Concurrency` role annotation, and
         * 2. members of *different* roles never overlap the same cache line
         *    (`Platform::kCacheLineSize` granularity).
         *
         * @return true if the layout is provably free of cross-role false sharing.
         */
        template<typename Ring>
        consteval bool AuditFalseSharing() {
            enum class Role : uint8_t { Producer, Consumer, Shared };
            struct Extent {
                size_t firstLine;
                size_t lastLine;
                Role role;
            };

            std::vector<Extent> extents;
            for (auto member : Traits::Members<Ring>) {
                const bool producer = Traits::Anno::Has<ProducerOwned>(member);
                const bool consumer = Traits::Anno::Has<ConsumerOwned>(member);
                const bool shared = Traits::Anno::Has<SharedStorage>(member);
                if (int{producer} + int{consumer} + int{shared} != 1) {
                    return false; // every member must declare exactly one role
                }
                const size_t begin = Detail::MemberOffsetBytes(member);
                const size_t size = std::meta::size_of(std::meta::type_of(member));
                const Role role = producer  ? Role::Producer
                                  : consumer ? Role::Consumer
                                             : Role::Shared;
                extents.push_back(Extent{begin / Platform::kCacheLineSize,
                                         (begin + size - 1) / Platform::kCacheLineSize, role});
            }

            for (size_t i = 0; i < extents.size(); ++i) {
                for (size_t j = i + 1; j < extents.size(); ++j) {
                    if (extents[i].role == extents[j].role) {
                        continue; // same owner may share a line by design
                    }
                    const bool disjoint = extents[i].lastLine < extents[j].firstLine ||
                                          extents[j].lastLine < extents[i].firstLine;
                    if (!disjoint) {
                        return false; // cross-role cache-line overlap = false sharing
                    }
                }
            }
            return true;
        }

    } // namespace Concurrency

    // =========================================================================
    // SpscQueue<T, Capacity> — typed fixed-size element queue
    // =========================================================================

    /**
     * @brief Wait-free bounded SPSC queue for fixed-size elements.
     *
     * @tparam T        Element type (nothrow-destructible object type; the
     *                  value-returning pop APIs additionally require move
     *                  constructibility, enforced per member).
     * @tparam Capacity Number of slots (must be power of 2, ≥ 2).
     *
     * Producer and consumer state are on separate cache lines to eliminate
     * false sharing (verified at compile time, see file header). Each side
     * caches the other's counter to avoid cross-core atomic reads on the
     * fast path.
     *
     * @code
     * SpscQueue<int, 1024> q;
     * q.TryPush(42);       // producer
     * int val;
     * q.TryPop(val);       // consumer
     * @endcode
     */
    template<typename T, uint32_t Capacity>
        requires(std::has_single_bit(Capacity) && Capacity >= 2 && std::is_object_v<T> &&
                 std::is_nothrow_destructible_v<T>)
    class SpscQueue {
        static constexpr uint32_t kMask = Capacity - 1;

        static_assert(std::atomic<uint32_t>::is_always_lock_free,
                      "SpscQueue requires lock-free 32-bit atomics");

    public:
        /// @name Construction
        /// @{
        SpscQueue() = default;
        ~SpscQueue() {
            // Destroy remaining elements (single-threaded teardown, no atomics needed)
            uint32_t head = head_.load(std::memory_order_relaxed);
            uint32_t tail = tail_.load(std::memory_order_relaxed);
            while (head != tail) {
                std::destroy_at(SlotPtr(head));
                ++head;
            }
        }
        SpscQueue(const SpscQueue&) = delete;
        SpscQueue& operator=(const SpscQueue&) = delete;
        /// @}

        /// @name Producer (single thread)
        /// @{

        /// @brief Emplace an element in-place. Returns false if full.
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        [[nodiscard]] bool
        TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            const uint32_t tail = tail_.load(std::memory_order_relaxed);
            // Check if full using cached head
            if (tail - cachedHead_ == Capacity) {
                // Re-read the actual head; acquire pairs with the consumer's release
                // store, so the slot's destruction happens-before its reuse here.
                cachedHead_ = head_.load(std::memory_order_acquire);
                if (tail - cachedHead_ == Capacity) {
                    return false; // truly full
                }
            }
            ::new (SlotStorage(tail)) T(std::forward<Args>(args)...);
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        /// @brief Try to push an element. Returns false if full.
        template<typename U>
            requires std::constructible_from<T, U&&>
        [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            return TryEmplace(std::forward<U>(value));
        }

        /// @brief Push, spinning until space is available. Yields between attempts.
        ///
        /// @p value is only moved-from on the attempt that succeeds, so
        /// re-forwarding inside the retry loop is safe.
        template<typename U>
            requires std::constructible_from<T, U&&>
        void Push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            while (!TryPush(std::forward<U>(value))) {
                std::this_thread::yield();
            }
        }

        /// @}

        /// @name Consumer (single thread)
        /// @{

        /// @brief Try to pop an element. Returns false if empty.
        [[nodiscard]] bool TryPop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
            requires std::is_move_assignable_v<T>
        {
            const uint32_t head = head_.load(std::memory_order_relaxed);
            if (head == cachedTail_) {
                cachedTail_ = tail_.load(std::memory_order_acquire);
                if (head == cachedTail_) {
                    return false; // truly empty
                }
            }
            T* ptr = SlotPtr(head);
            out = std::move(*ptr);
            std::destroy_at(ptr);
            head_.store(head + 1, std::memory_order_release);
            return true;
        }

        /// @brief Try to pop, returning `std::optional<T>` (empty if queue is empty).
        [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            const uint32_t head = head_.load(std::memory_order_relaxed);
            if (head == cachedTail_) {
                cachedTail_ = tail_.load(std::memory_order_acquire);
                if (head == cachedTail_) {
                    return std::nullopt;
                }
            }
            T* ptr = SlotPtr(head);
            std::optional<T> result{std::move(*ptr)};
            std::destroy_at(ptr);
            head_.store(head + 1, std::memory_order_release);
            return result;
        }

        /// @brief Pop, spinning until data is available. Yields between attempts.
        [[nodiscard]] T Pop() noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            while (true) {
                if (auto val = TryPop()) {
                    return std::move(*val);
                }
                std::this_thread::yield();
            }
        }

        /// @brief Zero-copy peek at the front element, or nullptr if empty.
        ///
        /// The pointer stays valid until the element is consumed via
        /// `PopFront()` / `TryPop()`. Wait-free; consumer thread only.
        [[nodiscard]] T* Front() noexcept {
            const uint32_t head = head_.load(std::memory_order_relaxed);
            if (head == cachedTail_) {
                cachedTail_ = tail_.load(std::memory_order_acquire);
                if (head == cachedTail_) {
                    return nullptr;
                }
            }
            return SlotPtr(head);
        }

        /// @brief Destroy the front element and release its slot.
        /// @pre The queue is non-empty (`Front()` returned non-null).
        void PopFront() noexcept {
            const uint32_t head = head_.load(std::memory_order_relaxed);
            std::destroy_at(SlotPtr(head));
            head_.store(head + 1, std::memory_order_release);
        }

        /// @}

        /// @name Queries (racy-but-safe diagnostics, callable from any thread)
        /// @{

        /// @brief Approximate number of elements available to read.
        ///
        /// The consumer counter is loaded *first*: `head_` is monotonic, so the
        /// later-loaded `tail_` is never behind the snapshot of `head_` and the
        /// unsigned difference cannot wrap. The result is clamped to `Capacity`
        /// against overcounting from concurrent progress between the two loads.
        [[nodiscard]] uint32_t SizeApprox() const noexcept {
            const uint32_t head = head_.load(std::memory_order_acquire);
            const uint32_t tail = tail_.load(std::memory_order_acquire);
            const uint32_t size = tail - head;
            return size <= Capacity ? size : Capacity;
        }

        /// @brief True if the queue appears empty (racy but safe for diagnostics).
        [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

        /// @brief Compile-time capacity.
        [[nodiscard]] static constexpr uint32_t GetCapacity() noexcept { return Capacity; }

        /// @}

    private:
        [[nodiscard]] void* SlotStorage(uint32_t position) noexcept {
            return &slots_[size_t{position & kMask} * sizeof(T)];
        }

        [[nodiscard]] T* SlotPtr(uint32_t position) noexcept {
            return std::assume_aligned<alignof(T)>(
                std::launder(reinterpret_cast<T*>(SlotStorage(position))));
        }

        // --- Producer state (own cache line) ---
        [[=Concurrency::ProducerOwned{}]] alignas(Platform::kCacheLineSize)
        std::atomic<uint32_t> tail_{0};
        [[=Concurrency::ProducerOwned{}]]
        uint32_t cachedHead_ = 0; ///< Producer's local copy of head_ (avoids cross-core read).

        // --- Consumer state (own cache line) ---
        [[=Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize)
        std::atomic<uint32_t> head_{0};
        [[=Concurrency::ConsumerOwned{}]]
        uint32_t cachedTail_ = 0; ///< Consumer's local copy of tail_.

        // --- Slot storage (deliberately uninitialised raw bytes: every slot is
        //     placement-new constructed before it is ever read) ---
        [[=Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize) alignas(T)
        std::byte slots_[Capacity * sizeof(T)];
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

        static_assert(std::atomic<uint32_t>::is_always_lock_free,
                      "SpscByteRing requires lock-free 32-bit atomics");

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
        uint32_t
        ReadAll(Fn&& fn) noexcept(std::is_nothrow_invocable_v<Fn&, std::span<const std::byte>>) {
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

                if (entrySize == 0 || entrySize > kMaxMessageSize ||
                    kHeaderSize + entrySize > available) [[unlikely]] {
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
        [[nodiscard]] static constexpr uint32_t GetMaxMessageSize() noexcept {
            return kMaxMessageSize;
        }

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
        [[=Concurrency::ProducerOwned{}]] alignas(Platform::kCacheLineSize)
        std::atomic<uint32_t> writePos_{0};
        [[=Concurrency::ProducerOwned{}]]
        uint32_t cachedReadPos_ = 0; ///< Producer's cached read position.

        // --- Consumer state (own cache line) ---
        [[=Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize)
        std::atomic<uint32_t> readPos_{0};

        // --- Staging buffer: consumer-private scratch, used only for messages
        //     that wrap the ring boundary (uninitialised by design) ---
        [[=Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize)
        std::byte staging_[kMaxMessageSize];

        // --- Ring buffer storage (uninitialised by design: bytes are always
        //     written before they are read) ---
        [[=Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize)
        std::byte buffer_[Capacity];
    };

    // =========================================================================
    // Compile-time layout audit (representative instantiations)
    // =========================================================================

    /** @cond INTERNAL */
    consteval {
        static_assert(Concurrency::AuditFalseSharing<SpscQueue<uint64_t, 1024>>(),
                      "SpscQueue layout exhibits cross-role false sharing");
        static_assert(Concurrency::AuditFalseSharing<SpscByteRing<64 * 1024>>(),
                      "SpscByteRing layout exhibits cross-role false sharing");
    }
    /** @endcond */

} // namespace Mashiro
