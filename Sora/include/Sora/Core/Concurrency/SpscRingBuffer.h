/**
 * @file SpscRingBuffer.h
 * @brief Provide a bounded wait-free single-producer single-consumer ring buffer.
 * @details The producer and consumer own independent free-running 32-bit cursors and cache the cursor published by the
 * opposite endpoint. Physical slots are selected with a power-of-two mask, while unsigned cursor subtraction preserves
 * the live queue span across counter wraparound. Producer and consumer state occupy distinct cache lines and are
 * audited at translation time through the project-wide contention-domain reflection facilities.
 *
 * The nonblocking operations are wait-free. @ref SpscRingBuffer::Emplace, @ref SpscRingBuffer::Push, and
 * @ref SpscRingBuffer::Pop are convenience blocking loops that yield while the opposite endpoint makes progress.
 * Queue shutdown and endpoint lifetime belong to a higher-level channel protocol; destruction requires both endpoints
 * to have reached quiescence.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/FalseSharingChecker.h>

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    /**
     * @brief Store a bounded FIFO sequence between exactly one producer and exactly one consumer.
     * @tparam T Nothrow-destructible object type stored directly in the ring slots.
     * @tparam Capacity Number of slots; must be a power of two and at least two.
     * @note Producer operations must be serialized on one producer endpoint. Consumer operations must independently be
     * serialized on one consumer endpoint. Diagnostic queries may be called concurrently but return approximate values.
     */
    template<typename T, std::uint32_t Capacity>
        requires(std::has_single_bit(Capacity) && Capacity >= 2 && std::is_object_v<T> &&
                 std::is_nothrow_destructible_v<T>)
    class SpscRingBuffer {
    public:
        using ValueType = T;            /**< Stored element type. */
        using SizeType = std::uint32_t; /**< Cursor, capacity, and element-count type. */

        inline static constexpr SizeType kCapacity = Capacity; /**< Number of physical slots. */

        /** @name Lifetime @{ -------------------------------------------------------------------------------------- */

        /** @brief Construct an empty ring buffer. */
        SpscRingBuffer() noexcept = default;

        /**
         * @brief Destroy every element that remains queued.
         * @pre The producer and consumer have both reached quiescence and will perform no further operation.
         */
        ~SpscRingBuffer() noexcept {
            SizeType readPos = readPos_.load(std::memory_order_relaxed);
            const SizeType writePos = writePos_.load(std::memory_order_relaxed);
            while (readPos != writePos) {
                std::destroy_at(SlotPtr(readPos));
                ++readPos;
            }
        }

        SpscRingBuffer(const SpscRingBuffer&) = delete;
        SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
        SpscRingBuffer(SpscRingBuffer&&) = delete;
        SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Producer @{ -------------------------------------------------------------------------------------- */

        /**
         * @brief Construct an element in the next producer slot when capacity is available.
         * @tparam Args Constructor argument types.
         * @param[in] args Arguments forwarded to the element constructor only after a free slot is acquired.
         * @return @c true when the element was published; @c false when the ring was full.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            const SizeType writePos = writePos_.load(std::memory_order_relaxed);
            if (writePos - cachedReadPos_ == Capacity) {
                cachedReadPos_ = readPos_.load(std::memory_order_acquire);
                if (writePos - cachedReadPos_ == Capacity) {
                    return false;
                }
            }

            ::new (SlotStorage(writePos)) T(std::forward<Args>(args)...);
            writePos_.store(writePos + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief Publish @p value when capacity is available.
         * @tparam U Source value type.
         * @param[in] value Value forwarded to the element constructor only on success.
         * @return @c true when the value was published; @c false when the ring was full.
         */
        template<typename U>
            requires std::constructible_from<T, U&&>
        [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            return TryEmplace(std::forward<U>(value));
        }

        /**
         * @brief Yield until a slot becomes available, then construct an element in place.
         * @tparam Args Constructor argument types.
         * @param[in] args Arguments forwarded only by the successful attempt.
         * @warning This operation cannot complete unless the consumer eventually releases a slot.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        void Emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            while (!TryEmplace(std::forward<Args>(args)...)) {
                std::this_thread::yield();
            }
        }

        /**
         * @brief Yield until a slot becomes available, then publish @p value.
         * @tparam U Source value type.
         * @param[in] value Value forwarded only by the successful attempt.
         * @warning This operation cannot complete unless the consumer eventually releases a slot.
         */
        template<typename U>
            requires std::constructible_from<T, U&&>
        void Push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            while (!TryPush(std::forward<U>(value))) {
                std::this_thread::yield();
            }
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Consumer @{ -------------------------------------------------------------------------------------- */

        /**
         * @brief Move the oldest element into @p out when one is available.
         * @param[out] out Destination assigned only when an element is consumed.
         * @return @c true when an element was consumed; @c false when the ring was empty.
         */
        [[nodiscard]] bool TryPop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
            requires std::is_move_assignable_v<T>
        {
            const SizeType readPos = PrepareRead();
            if (readPos == cachedWritePos_) {
                return false;
            }

            T* element = SlotPtr(readPos);
            out = std::move(*element);
            std::destroy_at(element);
            readPos_.store(readPos + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief Move the oldest element out of the ring when one is available.
         * @return The consumed element, or an empty optional when the ring was empty.
         */
        [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            const SizeType readPos = PrepareRead();
            if (readPos == cachedWritePos_) {
                return std::nullopt;
            }

            T* element = SlotPtr(readPos);
            std::optional<T> result{std::move(*element)};
            std::destroy_at(element);
            readPos_.store(readPos + 1, std::memory_order_release);
            return result;
        }

        /**
         * @brief Yield until an element becomes available, then return it.
         * @return Oldest queued element.
         * @warning This operation cannot complete unless the producer eventually publishes an element.
         */
        [[nodiscard]] T Pop() noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            while (true) {
                if (std::optional<T> value = TryPop()) {
                    return std::move(*value);
                }
                std::this_thread::yield();
            }
        }

        /**
         * @brief Consume every element committed at one consumer-side write-cursor snapshot.
         * @details Elements published after the acquire snapshot remain queued for the next call. Slots are released to
         * the producer by one final release store, avoiding one cross-core publication per element.
         * @tparam Sink Nothrow callable accepting one @c T&& argument.
         * @param[in] sink Callable invoked in FIFO order for each element in the snapshot.
         * @return Number of elements consumed.
         */
        template<typename Sink>
            requires std::invocable<Sink&, T&&> && std::is_nothrow_invocable_v<Sink&, T&&>
        SizeType Drain(Sink&& sink) noexcept {
            const SizeType begin = readPos_.load(std::memory_order_relaxed);
            const SizeType end = writePos_.load(std::memory_order_acquire);
            cachedWritePos_ = end;

            for (SizeType pos = begin; pos != end; ++pos) {
                T* element = SlotPtr(pos);
                std::invoke(sink, std::move(*element));
                std::destroy_at(element);
            }
            if (begin != end) {
                readPos_.store(end, std::memory_order_release);
            }
            return end - begin;
        }

        /**
         * @brief Return direct access to the oldest element without consuming it.
         * @return Pointer valid until the next consumer operation, or @c nullptr when the ring is empty.
         */
        [[nodiscard]] T* Front() noexcept {
            const SizeType readPos = PrepareRead();
            return readPos == cachedWritePos_ ? nullptr : SlotPtr(readPos);
        }

        /**
         * @brief Destroy and release the oldest element.
         * @pre The ring is nonempty and the caller is the designated consumer.
         */
        void PopFront() noexcept {
            const SizeType readPos = readPos_.load(std::memory_order_relaxed);
            std::destroy_at(SlotPtr(readPos));
            readPos_.store(readPos + 1, std::memory_order_release);
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Diagnostics @{ ----------------------------------------------------------------------------------- */

        /** @brief Return an approximate element count clamped to the ring capacity. */
        [[nodiscard]] SizeType SizeApprox() const noexcept {
            const auto [readPos, writePos] = CursorSnapshot();
            const SizeType size = writePos - readPos;
            return size <= Capacity ? size : Capacity;
        }

        /** @brief Return whether one concurrent cursor snapshot appears empty. */
        [[nodiscard]] bool IsEmpty() const noexcept { return SizeApprox() == 0; }

        /** @brief Return whether one concurrent cursor snapshot appears full. */
        [[nodiscard]] bool IsFull() const noexcept { return SizeApprox() == Capacity; }

        /** @brief Return the translation-time physical-layout report for this specialization. */
        [[nodiscard]] static consteval Meta::CacheLayoutReport Layout() {
            return Concurrency::Meta::AnalyzeCacheLayout<SpscRingBuffer>();
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

    private:
        inline static constexpr SizeType kMask = Capacity - 1;

        static_assert(std::atomic<SizeType>::is_always_lock_free, "SpscRingBuffer requires lock-free 32-bit atomics");

        [[nodiscard]] SizeType PrepareRead() noexcept {
            const SizeType readPos = readPos_.load(std::memory_order_relaxed);
            if (readPos == cachedWritePos_) {
                cachedWritePos_ = writePos_.load(std::memory_order_acquire);
            }
            return readPos;
        }

        [[nodiscard]] std::pair<SizeType, SizeType> CursorSnapshot() const noexcept {
            const SizeType readPos = readPos_.load(std::memory_order_acquire);
            const SizeType writePos = writePos_.load(std::memory_order_acquire);
            return {readPos, writePos};
        }

        [[nodiscard]] void* SlotStorage(SizeType pos) noexcept {
            return &slots_[static_cast<std::size_t>(pos & kMask) * sizeof(T)];
        }

        [[nodiscard]] T* SlotPtr(SizeType pos) noexcept {
            return std::assume_aligned<alignof(T)>(std::launder(reinterpret_cast<T*>(SlotStorage(pos))));
        }

        [[= $::ProducerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<SizeType> writePos_{0};
        [[= $::ProducerOwned{}]] SizeType cachedReadPos_ = 0;

        [[= $::ConsumerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<SizeType> readPos_{0};
        [[= $::ConsumerOwned{}]] SizeType cachedWritePos_ = 0;

        [[= $::SharedStorage{}]] alignas(Platform::kCacheLineSize) alignas(
            T) std::byte slots_[static_cast<std::size_t>(Capacity) * sizeof(T)];
    };

    /** @cond INTERNAL */
    consteval {
        using Concurrency::Meta::AuditFalseSharing;
        using Concurrency::Traits::IsOccupyingWholeLines;

        static_assert(AuditFalseSharing<SpscRingBuffer<std::uint64_t, 1024>>(),
                      "SpscRingBuffer failed the internal false-sharing audit");
        static_assert(IsOccupyingWholeLines<SpscRingBuffer<std::uint64_t, 1024>>,
                      "SpscRingBuffer must occupy whole cache lines");
    }
    /** @endcond */

} // namespace Sora::Concurrency
