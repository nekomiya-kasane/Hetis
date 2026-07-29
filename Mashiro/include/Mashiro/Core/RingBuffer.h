/**
 * @file RingBuffer.h
 * @brief Compile-time-specialized bounded ring buffers for local, SPSC, MPSC, SPMC, and MPMC topologies.
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/TypeTraits.h"

#include <atomic>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace Mashiro::Concurrency {

    /** @brief Number of execution agents permitted to invoke one queue endpoint concurrently. */
    enum class QueueCardinality : std::uint8_t {
        Single,
        Multiple,
    };

    /** @brief Whether both endpoints are confined to one thread or may execute concurrently. */
    enum class QueueSharing : std::uint8_t {
        SingleThread,
        Concurrent,
    };

    /** @brief Structural compile-time description of a queue's producer/consumer topology. */
    struct QueueTopology {
        QueueCardinality producers;
        QueueCardinality consumers;
        QueueSharing sharing;

        constexpr auto operator<=>(const QueueTopology&) const = default;
    };

    namespace QueueTopologies {

        inline constexpr QueueTopology Local{
            QueueCardinality::Single,
            QueueCardinality::Single,
            QueueSharing::SingleThread,
        };
        inline constexpr QueueTopology Spsc{
            QueueCardinality::Single,
            QueueCardinality::Single,
            QueueSharing::Concurrent,
        };
        inline constexpr QueueTopology Mpsc{
            QueueCardinality::Multiple,
            QueueCardinality::Single,
            QueueSharing::Concurrent,
        };
        inline constexpr QueueTopology Spmc{
            QueueCardinality::Single,
            QueueCardinality::Multiple,
            QueueSharing::Concurrent,
        };
        inline constexpr QueueTopology Mpmc{
            QueueCardinality::Multiple,
            QueueCardinality::Multiple,
            QueueSharing::Concurrent,
        };

    } // namespace QueueTopologies

    /**
     * @brief Reduce exact participant counts to the topology that affects generated synchronization.
     * @tparam Producers Number of producer threads.
     * @tparam Consumers Number of consumer threads.
     * @tparam SharedThreads Threads that may invoke both producer and consumer operations.
     */
    template<std::size_t Producers, std::size_t Consumers, std::size_t SharedThreads = 0>
    [[nodiscard]] consteval QueueTopology StaticQueueTopology() {
        static_assert(Producers > 0 && Consumers > 0, "a queue requires at least one producer and one consumer");
        static_assert(SharedThreads <= Producers && SharedThreads <= Consumers,
                      "shared queue threads must belong to both endpoint sets");
        constexpr std::size_t uniqueThreads = Producers + Consumers - SharedThreads;
        return {
            Producers == 1 ? QueueCardinality::Single : QueueCardinality::Multiple,
            Consumers == 1 ? QueueCardinality::Single : QueueCardinality::Multiple,
            uniqueThreads == 1 ? QueueSharing::SingleThread : QueueSharing::Concurrent,
        };
    }

} // namespace Mashiro::Concurrency

namespace Mashiro {

    namespace Detail {

        using Concurrency::QueueCardinality;
        using Concurrency::QueueSharing;
        using Concurrency::QueueTopology;

        /** @brief Validate a power-of-two capacity that leaves one sign bit for modular ticket comparisons. */
        [[nodiscard]] consteval bool ValidateRingCapacity(std::size_t capacity) {
            return capacity >= 2 && std::has_single_bit(capacity) &&
                   capacity <= std::bit_floor(std::numeric_limits<std::size_t>::max());
        }

        /** @brief Validate combinations expressible by the queue protocol family. */
        [[nodiscard]] consteval bool ValidateQueueTopology(QueueTopology topology) {
            return topology.sharing == QueueSharing::Concurrent ||
                   (topology.producers == QueueCardinality::Single && topology.consumers == QueueCardinality::Single);
        }

        /** @brief Interpret a modulo ticket subtraction as its two's-complement signed difference. */
        [[nodiscard]] constexpr std::make_signed_t<std::size_t> SequenceDifference(std::size_t lhs,
                                                                                   std::size_t rhs) noexcept {
            return std::bit_cast<std::make_signed_t<std::size_t>>(lhs - rhs);
        }

        /** @brief Raw storage for one sequence-cell ring position. */
        template<class T, bool Isolated>
        struct SequenceCell;

        template<class T>
        struct SequenceCell<T, false> {
            std::atomic<std::size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] void* Storage() noexcept { return storage; }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        template<class T>
        struct alignas(Platform::kCacheLineSize) SequenceCell<T, true> {
            std::atomic<std::size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] void* Storage() noexcept { return storage; }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        /** @brief Single-thread ring with plain counters and no synchronization metadata. */
        template<class T, std::size_t Capacity>
            requires(ValidateRingCapacity(Capacity) && std::is_object_v<T> && std::is_nothrow_destructible_v<T>)
        class LocalRingBuffer {
        public:
            using value_type = T;
            static constexpr std::size_t kCapacity = Capacity;
            static constexpr QueueTopology topology = Concurrency::QueueTopologies::Local;
            static constexpr bool transactionalPushBatch = true;

            LocalRingBuffer() = default;
            LocalRingBuffer(const LocalRingBuffer&) = delete;
            LocalRingBuffer& operator=(const LocalRingBuffer&) = delete;
            LocalRingBuffer(LocalRingBuffer&&) = delete;
            LocalRingBuffer& operator=(LocalRingBuffer&&) = delete;

            ~LocalRingBuffer() {
                while (head_ != tail_) {
                    std::destroy_at(Slot(head_++));
                }
            }

            template<class... Args>
                requires std::constructible_from<T, Args&&...>
            [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                if (tail_ - head_ == Capacity) {
                    return false;
                }
                ::new (Storage(tail_)) T(std::forward<Args>(args)...);
                ++tail_;
                return true;
            }

            template<class U>
                requires std::constructible_from<T, U&&>
            [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                return TryEmplace(std::forward<U>(value));
            }

            [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
                requires std::move_constructible<T>
            {
                if (head_ == tail_) {
                    return std::nullopt;
                }
                T* value = Slot(head_++);
                std::optional<T> result{std::move(*value)};
                std::destroy_at(value);
                return result;
            }

            [[nodiscard]] bool TryPop(T& output) noexcept(std::is_nothrow_move_assignable_v<T>)
                requires std::is_move_assignable_v<T>
            {
                if (head_ == tail_) {
                    return false;
                }
                T* value = Slot(head_++);
                output = std::move(*value);
                std::destroy_at(value);
                return true;
            }

            template<class Sink>
                requires std::invocable<Sink&, T&&> && std::is_nothrow_invocable_v<Sink&, T&&>
            std::size_t Drain(Sink&& sink) noexcept {
                const std::size_t end = tail_;
                std::size_t count = 0;
                while (head_ != end) {
                    T* value = Slot(head_++);
                    sink(std::move(*value));
                    std::destroy_at(value);
                    ++count;
                }
                return count;
            }

            [[nodiscard]] T* Front() noexcept { return head_ == tail_ ? nullptr : Slot(head_); }

            void PopFront() noexcept {
                std::destroy_at(Slot(head_));
                ++head_;
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept { return tail_ - head_; }
            [[nodiscard]] bool Empty() const noexcept { return head_ == tail_; }
            [[nodiscard]] static constexpr std::size_t GetCapacity() noexcept { return Capacity; }

        private:
            static constexpr std::size_t kIndexMask = Capacity - 1;

            [[nodiscard]] void* Storage(std::size_t position) noexcept {
                return &slots_[(position & kIndexMask) * sizeof(T)];
            }

            [[nodiscard]] T* Slot(std::size_t position) noexcept {
                return std::assume_aligned<alignof(T)>(std::launder(reinterpret_cast<T*>(Storage(position))));
            }

            std::size_t tail_ = 0;
            std::size_t head_ = 0;
            alignas(T) std::byte slots_[Capacity * sizeof(T)];
        };

        /** @brief Wait-free cross-thread SPSC ring using cached publication cursors. */
        template<class T, std::size_t Capacity>
            requires(ValidateRingCapacity(Capacity) && Capacity <= (std::size_t{1} << 31) && std::is_object_v<T> &&
                     std::is_nothrow_destructible_v<T>)
        class PublishedCursorRingBuffer {
            static constexpr std::uint32_t kIndexMask = static_cast<std::uint32_t>(Capacity - 1);

            static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                          "SPSC RingBuffer requires lock-free 32-bit atomics");

        public:
            using value_type = T;
            static constexpr std::size_t kCapacity = Capacity;
            static constexpr QueueTopology topology = Concurrency::QueueTopologies::Spsc;
            static constexpr bool transactionalPushBatch = true;

            PublishedCursorRingBuffer() = default;
            PublishedCursorRingBuffer(const PublishedCursorRingBuffer&) = delete;
            PublishedCursorRingBuffer& operator=(const PublishedCursorRingBuffer&) = delete;
            PublishedCursorRingBuffer(PublishedCursorRingBuffer&&) = delete;
            PublishedCursorRingBuffer& operator=(PublishedCursorRingBuffer&&) = delete;

            ~PublishedCursorRingBuffer() {
                std::uint32_t head = head_.load(std::memory_order_relaxed);
                const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
                while (head != tail) {
                    std::destroy_at(Slot(head++));
                }
            }

            template<class... Args>
                requires std::constructible_from<T, Args&&...>
            [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
                if (tail - cachedHead_ == Capacity) {
                    cachedHead_ = head_.load(std::memory_order_acquire);
                    if (tail - cachedHead_ == Capacity) {
                        return false;
                    }
                }
                ::new (Storage(tail)) T(std::forward<Args>(args)...);
                tail_.store(tail + 1, std::memory_order_release);
                return true;
            }

            template<class U>
                requires std::constructible_from<T, U&&>
            [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                return TryEmplace(std::forward<U>(value));
            }

            template<class U>
                requires std::constructible_from<T, U&&>
            void Push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                while (!TryPush(std::forward<U>(value))) {
                    std::this_thread::yield();
                }
            }

            [[nodiscard]] bool TryPop(T& output) noexcept(std::is_nothrow_move_assignable_v<T>)
                requires std::is_move_assignable_v<T>
            {
                const std::uint32_t head = head_.load(std::memory_order_relaxed);
                if (!RefreshTail(head)) {
                    return false;
                }
                T* value = Slot(head);
                output = std::move(*value);
                std::destroy_at(value);
                head_.store(head + 1, std::memory_order_release);
                return true;
            }

            [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
                requires std::move_constructible<T>
            {
                const std::uint32_t head = head_.load(std::memory_order_relaxed);
                if (!RefreshTail(head)) {
                    return std::nullopt;
                }
                T* value = Slot(head);
                std::optional<T> result{std::move(*value)};
                std::destroy_at(value);
                head_.store(head + 1, std::memory_order_release);
                return result;
            }

            [[nodiscard]] T Pop() noexcept(std::is_nothrow_move_constructible_v<T>)
                requires std::move_constructible<T>
            {
                for (;;) {
                    if (auto value = TryPop()) {
                        return std::move(*value);
                    }
                    std::this_thread::yield();
                }
            }

            template<class Sink>
                requires std::invocable<Sink&, T&&> && std::is_nothrow_invocable_v<Sink&, T&&>
            std::size_t Drain(Sink&& sink) noexcept {
                const std::uint32_t begin = head_.load(std::memory_order_relaxed);
                const std::uint32_t end = tail_.load(std::memory_order_acquire);
                cachedTail_ = end;
                for (std::uint32_t position = begin; position != end; ++position) {
                    T* value = Slot(position);
                    sink(std::move(*value));
                    std::destroy_at(value);
                }
                head_.store(end, std::memory_order_release);
                return end - begin;
            }

            [[nodiscard]] T* Front() noexcept {
                const std::uint32_t head = head_.load(std::memory_order_relaxed);
                return RefreshTail(head) ? Slot(head) : nullptr;
            }

            void PopFront() noexcept {
                const std::uint32_t head = head_.load(std::memory_order_relaxed);
                std::destroy_at(Slot(head));
                head_.store(head + 1, std::memory_order_release);
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                const std::uint32_t head = head_.load(std::memory_order_acquire);
                const std::uint32_t tail = tail_.load(std::memory_order_acquire);
                const std::uint32_t size = tail - head;
                return size <= Capacity ? size : Capacity;
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }
            [[nodiscard]] static constexpr std::size_t GetCapacity() noexcept { return Capacity; }

        private:
            [[nodiscard]] bool RefreshTail(std::uint32_t head) noexcept {
                if (head != cachedTail_) {
                    return true;
                }
                cachedTail_ = tail_.load(std::memory_order_acquire);
                return head != cachedTail_;
            }

            [[nodiscard]] void* Storage(std::uint32_t position) noexcept {
                return &slots_[std::size_t{position & kIndexMask} * sizeof(T)];
            }

            [[nodiscard]] T* Slot(std::uint32_t position) noexcept {
                return std::assume_aligned<alignof(T)>(std::launder(reinterpret_cast<T*>(Storage(position))));
            }

            [[= Concurrency::ProducerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<std::uint32_t> tail_{0};
            [[= Concurrency::ProducerOwned{}]] std::uint32_t cachedHead_ = 0;
            [[= Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<std::uint32_t> head_{0};
            [[= Concurrency::ConsumerOwned{}]] std::uint32_t cachedTail_ = 0;
            [[= Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize) alignas(
                T) std::byte slots_[Capacity * sizeof(T)];
        };

        /**
         * @brief Sequence-cell ring whose endpoint claim operations are selected entirely at compile time.
         * @details Multiple producers isolate adjacent cells to prevent publication false sharing; SPMC stays compact.
         */
        template<class T, std::size_t Capacity, QueueTopology Topology,
                 bool IsolateCells = Topology.producers == QueueCardinality::Multiple>
            requires Traits::ConcurrentQueueElement<T> && (ValidateRingCapacity(Capacity)) &&
                     (ValidateQueueTopology(Topology)) && (Topology.sharing == QueueSharing::Concurrent) &&
                     (Topology.producers == QueueCardinality::Multiple ||
                      Topology.consumers == QueueCardinality::Multiple)
        class SequenceCellRingBuffer {
            static constexpr bool kMultipleProducers = Topology.producers == QueueCardinality::Multiple;
            static constexpr bool kMultipleConsumers = Topology.consumers == QueueCardinality::Multiple;
            static constexpr std::size_t kIndexMask = Capacity - 1;
            using Cell = SequenceCell<T, IsolateCells>;
            using ProducerDomain =
                std::conditional_t<kMultipleProducers, Concurrency::Contended, Concurrency::ProducerOwned>;
            using ConsumerDomain =
                std::conditional_t<kMultipleConsumers, Concurrency::Contended, Concurrency::ConsumerOwned>;

            static_assert(std::atomic<std::size_t>::is_always_lock_free,
                          "concurrent RingBuffer requires lock-free size_t atomics");

        public:
            using value_type = T;
            static constexpr std::size_t kCapacity = Capacity;
            static constexpr QueueTopology topology = Topology;
            static constexpr bool transactionalPushBatch = false;
            static constexpr bool isolatesCells = IsolateCells;

            SequenceCellRingBuffer() noexcept {
                for (std::size_t index = 0; index != Capacity; ++index) {
                    cells_[index].sequence.store(index, std::memory_order_relaxed);
                }
            }

            SequenceCellRingBuffer(const SequenceCellRingBuffer&) = delete;
            SequenceCellRingBuffer& operator=(const SequenceCellRingBuffer&) = delete;
            SequenceCellRingBuffer(SequenceCellRingBuffer&&) = delete;
            SequenceCellRingBuffer& operator=(SequenceCellRingBuffer&&) = delete;

            ~SequenceCellRingBuffer() {
                if constexpr (!std::is_trivially_destructible_v<T>) {
                    while (TryPop().has_value()) {
                    }
                }
            }

            template<class... Args>
                requires std::constructible_from<T, Args&&...> &&
                         (!kMultipleProducers || std::is_nothrow_constructible_v<T, Args && ...>)
            [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                std::size_t position = enqueuePosition_.load(std::memory_order_relaxed);
                if constexpr (!kMultipleProducers) {
                    Cell& cell = cells_[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    if (SequenceDifference(sequence, position) != 0) {
                        return false;
                    }
                    ::new (cell.Storage()) T(std::forward<Args>(args)...);
                    enqueuePosition_.store(position + 1, std::memory_order_relaxed);
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }

                for (;;) {
                    Cell& cell = cells_[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    const auto difference = SequenceDifference(sequence, position);
                    if (difference == 0) {
                        if (!enqueuePosition_.compare_exchange_weak(position, position + 1,
                                                                    std::memory_order_relaxed)) {
                            continue;
                        }
                        ::new (cell.Storage()) T(std::forward<Args>(args)...);
                        cell.sequence.store(position + 1, std::memory_order_release);
                        return true;
                    }
                    if (difference < 0) {
                        return false;
                    }
                    position = enqueuePosition_.load(std::memory_order_relaxed);
                }
            }

            template<class U>
                requires std::constructible_from<T, U&&> &&
                         (!kMultipleProducers || std::is_nothrow_constructible_v<T, U &&>)
            [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                return TryEmplace(std::forward<U>(value));
            }

            [[nodiscard]] std::optional<T> TryPop() noexcept {
                std::optional<T> value;
                if (!TryConsume([&value](T&& source) noexcept { value.emplace(std::move(source)); })) {
                    return std::nullopt;
                }
                return value;
            }

            [[nodiscard]] bool TryPop(T& value) noexcept
                requires std::is_nothrow_move_assignable_v<T>
            {
                return TryConsume([&value](T&& source) noexcept { value = std::move(source); });
            }

            template<class Sink>
                requires(!kMultipleConsumers) && std::invocable<Sink&, T&&>
            std::size_t Drain(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
                std::size_t drained = 0;
                std::size_t position = dequeuePosition_.load(std::memory_order_relaxed);
                for (;;) {
                    Cell& cell = cells_[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    if (SequenceDifference(sequence, position + 1) != 0) {
                        break;
                    }
                    T value(std::move(*cell.Value()));
                    std::destroy_at(cell.Value());
                    cell.sequence.store(position + Capacity, std::memory_order_release);
                    dequeuePosition_.store(++position, std::memory_order_relaxed);
                    ++drained;
                    sink(std::move(value));
                }
                return drained;
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                const std::size_t consumed = dequeuePosition_.load(std::memory_order_acquire);
                const std::size_t produced = enqueuePosition_.load(std::memory_order_acquire);
                const std::size_t size = produced - consumed;
                return size <= Capacity ? size : Capacity;
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }
            [[nodiscard]] static constexpr std::size_t GetCapacity() noexcept { return Capacity; }

            [[nodiscard]] static consteval Concurrency::CacheLayoutReport Layout() {
                return Concurrency::AnalyzeCacheLayout<SequenceCellRingBuffer>();
            }

        private:
            template<class Consumer>
                requires std::is_nothrow_invocable_v<Consumer&, T&&>
            [[nodiscard]] bool TryConsume(Consumer&& consumer) noexcept {
                std::size_t position = dequeuePosition_.load(std::memory_order_relaxed);
                for (;;) {
                    Cell& cell = cells_[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    const auto difference = SequenceDifference(sequence, position + 1);
                    if (difference == 0) {
                        if constexpr (kMultipleConsumers) {
                            if (!dequeuePosition_.compare_exchange_weak(position, position + 1,
                                                                        std::memory_order_relaxed)) {
                                continue;
                            }
                        }
                        consumer(std::move(*cell.Value()));
                        std::destroy_at(cell.Value());
                        cell.sequence.store(position + Capacity, std::memory_order_release);
                        if constexpr (!kMultipleConsumers) {
                            dequeuePosition_.store(position + 1, std::memory_order_relaxed);
                        }
                        return true;
                    }
                    if (difference < 0) {
                        return false;
                    }
                    if constexpr (kMultipleConsumers) {
                        position = dequeuePosition_.load(std::memory_order_relaxed);
                    } else {
                        return false;
                    }
                }
            }

            [[= ProducerDomain{}]] alignas(Platform::kCacheLineSize) std::atomic<std::size_t> enqueuePosition_{0};
            [[= ConsumerDomain{}]] alignas(Platform::kCacheLineSize) std::atomic<std::size_t> dequeuePosition_{0};
            [[= Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize) Cell cells_[Capacity]{};
        };

        /** @brief Select the concrete protocol and data layout for one compile-time topology. */
        template<class T, std::size_t Capacity, QueueTopology Topology>
        [[nodiscard]] consteval auto SelectRingBuffer() {
            static_assert(ValidateRingCapacity(Capacity), "RingBuffer capacity must be a power of two of at least two");
            static_assert(ValidateQueueTopology(Topology),
                          "single-thread RingBuffer topology requires one producer and one consumer");
            if constexpr (Topology.sharing == QueueSharing::SingleThread) {
                return std::type_identity<LocalRingBuffer<T, Capacity>>{};
            } else if constexpr (Topology.producers == QueueCardinality::Single &&
                                 Topology.consumers == QueueCardinality::Single) {
                return std::type_identity<PublishedCursorRingBuffer<T, Capacity>>{};
            } else {
                return std::type_identity<SequenceCellRingBuffer<T, Capacity, Topology>>{};
            }
        }

        template<class Storage>
        concept RingBufferStorage = requires {
            typename Storage::value_type;
            { Storage::kCapacity } -> std::convertible_to<std::size_t>;
            { Storage::topology } -> std::same_as<const QueueTopology&>;
            { Storage::transactionalPushBatch } -> std::same_as<const bool&>;
        };

    } // namespace Detail

    /**
     * @brief Bounded ring buffer whose synchronization protocol is selected entirely at compile time.
     * @tparam T Stored payload type.
     * @tparam Capacity Power-of-two slot count.
     * @tparam Topology Producer/consumer cardinality and sharing domain.
     */
    template<class T, std::size_t Capacity, Concurrency::QueueTopology Topology>
    using RingBuffer = typename decltype(Detail::SelectRingBuffer<T, Capacity, Topology>())::type;

    /** @cond INTERNAL */
    consteval {
        using Concurrency::AuditFalseSharing;
        using Concurrency::QueueTopologies::Mpmc;
        using Concurrency::QueueTopologies::Mpsc;
        using Concurrency::QueueTopologies::Spmc;
        using Concurrency::QueueTopologies::Spsc;

        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Spsc>>());
        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Mpsc>>());
        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Spmc>>());
        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Mpmc>>());
    }
    /** @endcond */

} // namespace Mashiro
