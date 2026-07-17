/**
 * @file MpscQueue.h
 * @brief Bounded allocation-free multi-producer/single-consumer FIFO storage.
 * @ingroup Core
 *
 * @details This sequence-cell ring provides non-blocking try operations without allocation, mutexes, or kernel waits.
 * Producers reserve monotonically numbered cells through one contended ticket word; the sole consumer advances its
 * cursor without CAS. The implementation deliberately does not claim formal lock-free FIFO progress: a producer
 * descheduled after reserving the oldest cell can temporarily prevent that cell and all later cells from being
 * consumed.
 */
#pragma once

#include "Mashiro/Core/Annotation.h"
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
         * @brief Sequence counter and raw payload storage for one MPSC ring position.
         * @tparam T Stored payload type.
         */
        template<class T>
        struct alignas(Platform::kCacheLineSize) MpscCell {
            std::atomic<std::size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] void* Storage() noexcept { return storage; }

            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        /** @brief Validate a power-of-two capacity that leaves one sign bit for modular ticket comparisons. */
        [[nodiscard]] consteval bool ValidateMpscCapacity(std::size_t capacity) {
            return capacity >= 2 && std::has_single_bit(capacity) &&
                   capacity <= std::bit_floor(std::numeric_limits<std::size_t>::max());
        }

        /** @brief Interpret a modulo ticket subtraction as its two's-complement signed difference. */
        [[nodiscard]] constexpr std::make_signed_t<std::size_t> MpscSequenceDifference(std::size_t lhs,
                                                                                       std::size_t rhs) noexcept {
            return std::bit_cast<std::make_signed_t<std::size_t>>(lhs - rhs);
        }

    } // namespace Detail

    /**
     * @brief Fixed-capacity MPSC FIFO storage with non-blocking try operations.
     *
     * @tparam T Payload type satisfying @ref Traits::ConcurrentQueueElement.
     * @tparam Capacity Positive power-of-two cell count.
     *
     * @details Any number of producers may call @ref TryEmplace and @ref TryPush concurrently. Exactly one consumer may
     * call @ref TryPop or @ref Drain. A failed producer attempt does not consume its input. Payload construction must
     * be non-throwing because an exception after ticket reservation would leave a permanent unpublished hole.
     */
    template<class T, std::size_t Capacity = 1024>
        requires Traits::ConcurrentQueueElement<T> && (Detail::ValidateMpscCapacity(Capacity))
    class MpscQueue {
    public:
        using value_type = T;
        static constexpr std::size_t kCapacity = Capacity;

        /** @brief Construct an empty ring and prime each cell for its first producer ticket. */
        MpscQueue() noexcept {
            for (std::size_t index = 0; index != Capacity; ++index) {
                cells_[index].sequence.store(index, std::memory_order_relaxed);
            }
        }

        MpscQueue(const MpscQueue&) = delete;
        MpscQueue& operator=(const MpscQueue&) = delete;
        MpscQueue(MpscQueue&&) = delete;
        MpscQueue& operator=(MpscQueue&&) = delete;

        /** @brief Destroy every committed payload during single-threaded teardown. */
        ~MpscQueue() {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                while (TryPop().has_value()) {
                }
            }
        }

        /**
         * @brief Reserve a cell and construct one payload, returning @c false when no cell is reusable.
         * @tparam Args Nothrow constructor argument types for @p T.
         * @param[in] args Arguments forwarded to the payload constructor after reservation succeeds.
         * @return @c true after publication; @c false on producer-side backpressure.
         */
        template<class... Args>
            requires std::is_nothrow_constructible_v<T, Args&&...>
        [[nodiscard]] bool TryEmplace(Args&&... args) noexcept {
            std::size_t position = tail_.load(std::memory_order_relaxed);
            for (;;) {
                Detail::MpscCell<T>& cell = cells_[position & kIndexMask];
                const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                const auto difference = Detail::MpscSequenceDifference(sequence, position);
                if (difference == 0) {
                    if (tail_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                        ::new (cell.Storage()) T(std::forward<Args>(args)...);
                        cell.sequence.store(position + 1, std::memory_order_release);
                        return true;
                    }
                } else if (difference < 0) {
                    return false;
                } else {
                    position = tail_.load(std::memory_order_relaxed);
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

        /**
         * @brief Consume the oldest published payload, or return @c std::nullopt when the head cell is unavailable.
         *
         * @details Only the designated consumer may call this function. An unavailable head can mean either that no
         * producer has reserved a cell or that the oldest producer has reserved its cell but has not published it yet.
         */
        [[nodiscard]] std::optional<T> TryPop() noexcept {
            std::optional<T> value;
            if (!TryConsume([&value](T&& source) noexcept { value.emplace(std::move(source)); })) {
                return std::nullopt;
            }
            return value;
        }

        /** @brief Move the oldest published payload into @p value, returning @c false when unavailable. */
        [[nodiscard]] bool TryPop(T& value) noexcept
            requires std::is_nothrow_move_assignable_v<T>
        {
            return TryConsume([&value](T&& source) noexcept { value = std::move(source); });
        }

        /**
         * @brief Consume every consecutively published payload currently reachable from the head.
         * @tparam Sink Callable accepting @p T as an rvalue.
         * @param[in] sink Invoked once for each consumed payload in FIFO order.
         * @return Number of payloads consumed.
         */
        template<class Sink>
            requires std::invocable<Sink&, T&&>
        std::size_t Drain(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
            std::size_t drained = 0;
            std::size_t position = head_.load(std::memory_order_relaxed);
            for (;;) {
                Detail::MpscCell<T>& cell = cells_[position & kIndexMask];
                const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                if (Detail::MpscSequenceDifference(sequence, position + 1) != 0) {
                    break;
                }
                T value(std::move(*cell.Value()));
                std::destroy_at(cell.Value());
                cell.sequence.store(position + Capacity, std::memory_order_release);
                ++position;
                ++drained;
                head_.store(position, std::memory_order_relaxed);
                sink(std::move(value));
            }
            return drained;
        }

        /**
         * @brief Return whether no producer ticket is outstanding at the sampled head and tail positions.
         * @note Concurrent progress may invalidate the result immediately.
         */
        [[nodiscard]] bool Empty() const noexcept {
            return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_acquire);
        }

        /** @brief Return the approximate number of reserved or published cells. */
        [[nodiscard]] std::size_t ApproxSize() const noexcept {
            const std::size_t tail = tail_.load(std::memory_order_acquire);
            const std::size_t head = head_.load(std::memory_order_relaxed);
            return tail - head;
        }

        /** @brief Return the compile-time false-sharing layout analysis for this specialization. */
        [[nodiscard]] static consteval Concurrency::CacheLayoutReport Layout() {
            return Concurrency::AnalyzeCacheLayout<MpscQueue>();
        }

    private:
        static constexpr std::size_t kIndexMask = Capacity - 1;

        /** @brief Claim, consume, destroy, and release one head cell through a non-throwing receiver. */
        template<typename Consumer>
            requires std::is_nothrow_invocable_v<Consumer&, T&&>
        [[nodiscard]] bool TryConsume(Consumer&& consumer) noexcept {
            const std::size_t position = head_.load(std::memory_order_relaxed);
            Detail::MpscCell<T>& cell = cells_[position & kIndexMask];
            const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
            if (Detail::MpscSequenceDifference(sequence, position + 1) != 0) {
                return false;
            }

            consumer(std::move(*cell.Value()));
            std::destroy_at(cell.Value());
            cell.sequence.store(position + Capacity, std::memory_order_release);
            head_.store(position + 1, std::memory_order_relaxed);
            return true;
        }

        [[= Concurrency::Contended{}]] alignas(Platform::kCacheLineSize) std::atomic<std::size_t> tail_{0};
        [[= Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<std::size_t> head_{0};
        [[= Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize) Detail::MpscCell<T> cells_[Capacity]{};
    };

    /** @cond INTERNAL */
    consteval {
        using Concurrency::AuditFalseSharing;
        using Concurrency::ConsumerOwned;
        using Concurrency::Contended;
        using Concurrency::DomainStartsLine;

        static_assert(AuditFalseSharing<MpscQueue<std::uint64_t>>(),
                      "MpscQueue<uint64_t> failed the internal false-sharing audit");
        static_assert(DomainStartsLine<MpscQueue<std::uint64_t>, Contended>(),
                      "MpscQueue tail must start its own cache line");
        static_assert(DomainStartsLine<MpscQueue<std::uint64_t>, ConsumerOwned>(),
                      "MpscQueue head must start its own cache line");
    }
    /** @endcond */

} // namespace Mashiro
