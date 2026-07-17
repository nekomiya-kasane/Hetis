/**
 * @file SpscChannel.h
 * @brief SPSC payload channel with directional wake sequences for readable and writable predicate changes.
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/SpscRingBuffer.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace Mashiro {

    /**
     * @brief Wait-free bounded SPSC storage paired with directional atomic wait/notify sequences.
     *
     * The queue remains the source of truth. The sequences are only a wake plane: a successful push advances the
     * consumer's readable sequence, while a successful pop or drain advances the producer's writable sequence. Keeping
     * these write paths on separate cache lines prevents the notification layer from serializing otherwise independent
     * producer and consumer progress. Observers use acquire loads, so a changed sequence also observes the
     * corresponding queue transition.
     *
     * @tparam T Element type stored by the channel.
     * @tparam Capacity Power-of-two queue capacity.
     */
    template<typename T, uint32_t Capacity>
        requires(std::has_single_bit(Capacity) && Capacity >= 2 && std::is_object_v<T> &&
                 std::is_nothrow_destructible_v<T>)
    class SpscChannel {
    public:
        SpscChannel() = default;
        SpscChannel(const SpscChannel&) = delete;
        SpscChannel& operator=(const SpscChannel&) = delete;

        /** @brief Return the producer-owned readable-predicate sequence. */
        [[nodiscard]] uint64_t ObserveReadable() const noexcept {
            return readableEpoch_.load(std::memory_order_acquire);
        }

        /** @brief Block the consumer until the readable-predicate sequence differs from @p snapshot. */
        void WaitReadable(uint64_t snapshot) const noexcept {
            readableEpoch_.wait(snapshot, std::memory_order_acquire);
        }

        /** @brief Return the consumer-owned writable-predicate sequence. */
        [[nodiscard]] uint64_t ObserveWritable() const noexcept {
            return writableEpoch_.load(std::memory_order_acquire);
        }

        /** @brief Block the producer until the writable-predicate sequence differs from @p snapshot. */
        void WaitWritable(uint64_t snapshot) const noexcept {
            writableEpoch_.wait(snapshot, std::memory_order_acquire);
        }

        /** @brief Publish an external predicate change to both endpoints. */
        void SignalPredicateChanged() noexcept {
            Signal(readableEpoch_);
            Signal(writableEpoch_);
        }

        /**
         * @brief Construct an element in place and signal the consumer when successful.
         * @tparam Args Constructor argument types.
         * @param[in] args Arguments forwarded to @c T's constructor.
         * @return @c true when the element was published; @c false when the channel was full.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            if (!queue_.TryEmplace(std::forward<Args>(args)...)) {
                return false;
            }
            Signal(readableEpoch_);
            return true;
        }

        /**
         * @brief Publish an element and signal the consumer when successful.
         * @tparam U Source value type.
         * @param[in] value Value forwarded into the queue.
         * @return @c true when the element was published; @c false when the channel was full.
         */
        template<typename U>
            requires std::constructible_from<T, U&&>
        [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            return TryEmplace(std::forward<U>(value));
        }

        /**
         * @brief Consume one element and signal the producer when successful.
         * @param[out] value Destination receiving the element.
         * @return @c true when an element was consumed; @c false when the channel was empty.
         */
        [[nodiscard]] bool TryPop(T& value) noexcept(std::is_nothrow_move_assignable_v<T>)
            requires std::is_move_assignable_v<T>
        {
            if (!queue_.TryPop(value)) {
                return false;
            }
            Signal(writableEpoch_);
            return true;
        }

        /** @brief Consume one element, returning an empty optional when the channel is empty. */
        [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            auto value = queue_.TryPop();
            if (value) {
                Signal(writableEpoch_);
            }
            return value;
        }

        /**
         * @brief Drain one committed queue snapshot and signal the producer when slots were released.
         * @tparam Sink Nothrow callable accepting @c T&&.
         * @param[in] sink Consumer invoked once for each drained element.
         * @return Number of drained elements.
         */
        template<typename Sink>
            requires std::invocable<Sink&, T&&> && std::is_nothrow_invocable_v<Sink&, T&&>
        uint32_t Drain(Sink&& sink) noexcept {
            const uint32_t count = queue_.Drain(std::forward<Sink>(sink));
            if (count != 0) {
                Signal(writableEpoch_);
            }
            return count;
        }

        /** @brief Return whether the channel currently contains no committed elements. */
        [[nodiscard]] bool Empty() const noexcept { return queue_.Empty(); }

        /** @brief Return an approximate element count suitable for diagnostics. */
        [[nodiscard]] uint32_t SizeApprox() const noexcept { return queue_.SizeApprox(); }

        /** @brief Return the compile-time channel capacity. */
        [[nodiscard]] static consteval uint32_t GetCapacity() noexcept { return Capacity; }

    private:
        /** @brief Advance one directional predicate sequence and notify its sole possible waiter. */
        static void Signal(std::atomic<uint64_t>& epoch) noexcept {
            epoch.fetch_add(1, std::memory_order_release);
            epoch.notify_one();
        }

        SpscRingBuffer<T, Capacity> queue_{};
        [[=Concurrency::ProducerOwned{}]] alignas(Platform::kCacheLineSize) mutable std::atomic<uint64_t>
            readableEpoch_{0};
        [[=Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize) mutable std::atomic<uint64_t>
            writableEpoch_{0};
    };

} // namespace Mashiro
