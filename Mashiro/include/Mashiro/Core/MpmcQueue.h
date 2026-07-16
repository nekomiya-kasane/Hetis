/**
 * @file MpmcQueue.h
 * @brief Bounded allocation-free multi-producer/multi-consumer queue storage.
 * @ingroup Core
 *
 * @details This is the bounded sequence-cell algorithm commonly associated with Vyukov. Every operation is a
 * non-blocking try operation and performs no allocation or kernel wait. The queue deliberately does not claim a formal
 * lock-free system-progress guarantee: a thread descheduled after reserving the oldest cell can delay FIFO visibility
 * for other consumers. Use the explicit progress metadata exposed by @c QueueAdapter instead of inferring progress from
 * the absence of a mutex.
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/TypeTraits.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace Mashiro {

    namespace Detail {

        /**
         * @brief Sequence counter and raw payload storage for one MPMC ring position.
         * @tparam T Stored payload type.
         */
        template<class T>
        struct MpmcCell {
            std::atomic<std::size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)];

            [[nodiscard]] void* Storage() noexcept { return storage; }

            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        /** @brief Validate a power-of-two capacity that leaves one sign bit for modular ticket comparisons. */
        [[nodiscard]] consteval bool ValidateMpmcCapacity(std::size_t capacity) {
            return capacity >= 2 && std::has_single_bit(capacity) &&
                   capacity <= std::bit_floor(std::numeric_limits<std::size_t>::max());
        }

        /** @brief Interpret a modulo ticket subtraction as its two's-complement signed difference. */
        [[nodiscard]] constexpr std::make_signed_t<std::size_t> MpmcSequenceDifference(std::size_t lhs,
                                                                                       std::size_t rhs) noexcept {
            return std::bit_cast<std::make_signed_t<std::size_t>>(lhs - rhs);
        }

    } // namespace Detail

    /**
     * @brief Fixed-capacity MPMC FIFO storage with non-blocking try operations.
     * @tparam T Payload type satisfying @ref Traits::ConcurrentQueueElement.
     * @tparam Capacity Positive power-of-two cell count.
     *
     * @details Producers and consumers may call their respective try operations concurrently. Payload construction,
     * move-out, and destruction are non-throwing because a reservation cannot be rolled back after its ticket CAS.
     */
    template<class T, std::size_t Capacity = 1024>
        requires Traits::ConcurrentQueueElement<T> && (Detail::ValidateMpmcCapacity(Capacity))
    class MpmcQueue {
    public:
        using value_type = T;
        static constexpr std::size_t kCapacity = Capacity;

        /** @brief Construct an empty ring and prime every cell for its first producer ticket. */
        MpmcQueue() noexcept {
            for (std::size_t index = 0; index < Capacity; ++index) {
                cells_[index].sequence.store(index, std::memory_order_relaxed);
            }
        }

        MpmcQueue(const MpmcQueue&) = delete;
        MpmcQueue& operator=(const MpmcQueue&) = delete;
        MpmcQueue(MpmcQueue&&) = delete;
        MpmcQueue& operator=(MpmcQueue&&) = delete;

        /** @brief Destroy every committed payload during single-threaded teardown. */
        ~MpmcQueue() {
            while (TryPop().has_value()) {
            }
        }

        /** @brief Reserve, construct, and publish one payload, returning @c false under backpressure. */
        template<class... Args>
            requires std::is_nothrow_constructible_v<T, Args&&...>
        [[nodiscard]] bool TryEmplace(Args&&... args) noexcept {
            std::size_t position = enqueuePosition_.load(std::memory_order_relaxed);
            for (;;) {
                Detail::MpmcCell<T>& cell = cells_[position & kIndexMask];
                const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                const auto difference = Detail::MpmcSequenceDifference(sequence, position);
                if (difference == 0) {
                    if (enqueuePosition_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                        ::new (cell.Storage()) T(std::forward<Args>(args)...);
                        cell.sequence.store(position + 1, std::memory_order_release);
                        return true;
                    }
                } else if (difference < 0) {
                    return false;
                } else {
                    position = enqueuePosition_.load(std::memory_order_relaxed);
                }
            }
        }

        /** @brief Move @p value into the ring, preserving it when the ring applies backpressure. */
        [[nodiscard]] bool TryPush(T&& value) noexcept { return TryEmplace(std::move(value)); }

        /** @brief Copy @p value into the ring when copying is non-throwing. */
        [[nodiscard]] bool TryPush(const T& value) noexcept
            requires std::is_nothrow_copy_constructible_v<T>
        {
            return TryEmplace(value);
        }

        /** @brief Consume one published head payload, or return @c std::nullopt when the head is unavailable. */
        [[nodiscard]] std::optional<T> TryPop() noexcept {
            std::size_t position = dequeuePosition_.load(std::memory_order_relaxed);
            for (;;) {
                Detail::MpmcCell<T>& cell = cells_[position & kIndexMask];
                const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                const auto difference = Detail::MpmcSequenceDifference(sequence, position + 1);
                if (difference == 0) {
                    if (dequeuePosition_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                        T value(std::move(*cell.Value()));
                        std::destroy_at(cell.Value());
                        cell.sequence.store(position + Capacity, std::memory_order_release);
                        return std::optional<T>{std::move(value)};
                    }
                } else if (difference < 0) {
                    return std::nullopt;
                } else {
                    position = dequeuePosition_.load(std::memory_order_relaxed);
                }
            }
        }

        /** @brief Return an approximate count of reserved or published cells. */
        [[nodiscard]] std::size_t ApproxSize() const noexcept {
            const std::size_t consumed = dequeuePosition_.load(std::memory_order_acquire);
            const std::size_t produced = enqueuePosition_.load(std::memory_order_acquire);
            const std::size_t size = produced - consumed;
            return size < Capacity ? size : Capacity;
        }

        /** @brief Return a transient empty hint derived from @ref ApproxSize. */
        [[nodiscard]] bool Empty() const noexcept { return ApproxSize() == 0; }

    private:
        static constexpr std::size_t kIndexMask = Capacity - 1;

        [[= Concurrency::Contended{}]] alignas(Platform::kCacheLineSize) std::atomic<std::size_t> enqueuePosition_{0};
        [[= Concurrency::Contended{}]] alignas(Platform::kCacheLineSize) std::atomic<std::size_t> dequeuePosition_{0};
        [[= Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize) Detail::MpmcCell<T> cells_[Capacity]{};
    };

} // namespace Mashiro
