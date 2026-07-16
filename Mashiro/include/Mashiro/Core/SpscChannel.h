/**
 * @file SpscChannel.h
 * @brief SPSC payload channel with a monotonic wake sequence for predicate changes.
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
     * @brief Wait-free bounded SPSC storage paired with an atomic wait/notify sequence.
     *
     * The queue remains the source of truth. The sequence is only a wake plane: every successful operation that can
     * change a waiting peer's predicate advances it after the queue's release publication. Observers use acquire loads,
     * so waking on a changed sequence also observes the corresponding queue transition.
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

        /** @brief Return the current wake-sequence snapshot with acquire semantics. */
        [[nodiscard]] uint64_t Observe() const noexcept { return epoch_.load(std::memory_order_acquire); }

        /** @brief Block until the wake sequence differs from @p snapshot. */
        void Wait(uint64_t snapshot) const noexcept { epoch_.wait(snapshot, std::memory_order_acquire); }

        /** @brief Publish an external predicate change to a waiting peer. */
        void SignalPredicateChanged() noexcept {
            epoch_.fetch_add(1, std::memory_order_release);
            epoch_.notify_one();
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
            SignalPredicateChanged();
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
            SignalPredicateChanged();
            return true;
        }

        /** @brief Consume one element, returning an empty optional when the channel is empty. */
        [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            auto value = queue_.TryPop();
            if (value) {
                SignalPredicateChanged();
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
                SignalPredicateChanged();
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
        SpscRingBuffer<T, Capacity> queue_{};
        [[=Concurrency::Contended{}]] alignas(Platform::kCacheLineSize) mutable std::atomic<uint64_t> epoch_{0};
    };

} // namespace Mashiro
