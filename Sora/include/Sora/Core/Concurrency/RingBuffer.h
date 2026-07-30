/**
 * @file RingBuffer.h
 * @brief Compile-time-specialized bounded ring buffers for local, SPSC, MPSC, SPMC, and MPMC topologies.
 * @ingroup Core
 */

#pragma once

#include "Sora/Core/Concurrency/ContentionDomain.h"
#include "Sora/Core/Concurrency/FalseSharingChecker.h"
#include "Sora/Core/Concurrency/QueueTopology.h"
#include "Sora/Core/Traits/TypeTraits.h"
#include "Sora/Platform.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <meta>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Concurrency {

        enum class RingBufferProtocol : uint8_t {
            Local,
            PublishedCursor,
            SequenceCell,
        };

        namespace Meta {

            /** @brief Match a queue topology to the protocol that determines generated code and physical layout. */
            [[nodiscard]] consteval RingBufferProtocol GetRingBufferProtocol(QueueTopology topology) {
                if (topology == QueueTopologies::Local) {
                    return RingBufferProtocol::Local;
                } else if (topology == QueueTopologies::Spsc) {
                    return RingBufferProtocol::PublishedCursor;
                } else {
                    /**
                     * @note We have to maintain status in every cell in order not to encounter
                     * applied-but-not-constructed objects when there are multiple producers or multiple consumers.
                     */
                    return RingBufferProtocol::SequenceCell;
                }
            }

        } // namespace Meta

        namespace Concept {

            template<size_t Capacity>
            concept ValidRingBufferCapacity =
                Capacity >= 2 && std::has_single_bit(Capacity) &&
                Capacity <= std::bit_floor(std::numeric_limits<size_t>::max()) && Capacity <= (size_t{1} << 31);

            template<QueueTopology Topo>
            concept ValidRingBufferTopology =
                Topo.sharing == QueueSharing::Concurrent ||
                (Topo.producers == QueueCardinality::Single && Topo.consumers == QueueCardinality::Single);

            template<typename T, QueueTopology Topo>
            concept RingBufferElement = std::is_object_v<T> && std::is_nothrow_destructible_v<T> && [] consteval {
                if constexpr (Meta::GetRingBufferProtocol(Topo) == RingBufferProtocol::SequenceCell) {
                    return std::move_constructible<T> && std::is_nothrow_move_constructible_v<T>;
                } else {
                    return true;
                }
            }();

        } // namespace Concept

        /**
         * @brief Raw storage for one sequence-cell ring position, optionally aligned to a cache line.
         * @details The sequence number distinguishes writable and readable generations of the same physical slot.
         */
        template<class T, bool CacheLineAligned>
        struct SequenceCell;

        /** @brief Raw storage for one sequence-cell ring position without cache line alignment. */
        template<class T>
        struct SequenceCell<T, false> {
            std::atomic<size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] void* Storage() noexcept { return storage; }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        /** @brief Raw storage for one sequence-cell ring position with cache line alignment. */
        template<class T>
        struct alignas(Platform::kCacheLineSize) SequenceCell<T, true> {
            std::atomic<size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] void* Storage() noexcept { return storage; }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        /** @brief Raw storage for a contiguous block of bytes, aligned to the element type. */
        template<typename T, size_t Capacity>
        struct BytesArena {
            alignas(T) std::byte slots[Capacity * sizeof(T)];
        };

        /** @brief Cells for a sequence-cell ring buffer. */
        template<typename T, size_t Capacity, bool CacheLineAligned>
        struct CellsArena {
            SequenceCell<T, CacheLineAligned> cells[Capacity]{};
        };

        namespace Detail {

            /** @brief Physical roles used to give every compile-time-erased field a distinct empty type. */
            enum class RingBufferField : uint8_t {
                PublishedSlotCount,
                AllocatedSlots,
                CachedReadPosition,
                CachedWritePosition,
                Arena,
            };

            template<RingBufferField Field>
            using EmptyRingBufferField = std::integral_constant<RingBufferField, Field>;

            /** @brief Interpret an unsigned ticket subtraction as its two's-complement signed difference. */
            [[nodiscard]] constexpr std::make_signed_t<size_t> SequenceDifference(size_t lhs, size_t rhs) noexcept {
                return std::bit_cast<std::make_signed_t<size_t>>(lhs - rhs);
            }

            /**
             * @brief One bounded ring-buffer class whose protocol, state, and synchronization are specialized at
             * compile time.
             * @details Local, published-cursor SPSC, and sequence-cell protocols remain distinct mathematical state
             * machines. `if constexpr` erases unselected algorithms, while conditional field types and the MS-ABI
             * no-unique-address attribute erase unselected state. No protocol value or branch survives at runtime.
             */
            template<class T, size_t Capacity, QueueTopology Topo,
                     bool CacheLineAligned =
                         Topo.producers == QueueCardinality::Multiple || Topo.consumers == QueueCardinality::Multiple>
                requires Concept::ValidRingBufferCapacity<Capacity> && Concept::ValidRingBufferTopology<Topo> &&
                         Concept::RingBufferElement<T, Topo>
            class RingBufferImpl {
                static constexpr RingBufferProtocol kProtocol = Meta::GetRingBufferProtocol(Topo);
                static constexpr size_t kIndexMask = Capacity - 1;
                static constexpr size_t kSlotCount = Capacity + 1;
                static constexpr size_t kPadding = (Platform::kCacheLineSize - 1) / sizeof(T) + 1;
                static constexpr size_t kPositionAlignment =
                    kProtocol == RingBufferProtocol::Local ? alignof(size_t) : Platform::kCacheLineSize;
                static constexpr size_t kCacheAlignment =
                    kProtocol == RingBufferProtocol::PublishedCursor ? Platform::kCacheLineSize : 1;
                static constexpr size_t kArenaAlignment = [] consteval {
                    if constexpr (kProtocol == RingBufferProtocol::SequenceCell) {
                        return Platform::kCacheLineSize;
                    } else if constexpr (kProtocol == RingBufferProtocol::Local) {
                        return alignof(T);
                    } else {
                        return 1;
                    }
                }();

                using PublishedSlotCount =
                    std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, const size_t,
                                       EmptyRingBufferField<RingBufferField::PublishedSlotCount>>;
                using AllocatedSlots = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, T*,
                                                          EmptyRingBufferField<RingBufferField::AllocatedSlots>>;
                using Allocator = std::allocator<T>;
                using Position =
                    std::conditional_t<kProtocol == RingBufferProtocol::Local, size_t, std::atomic<size_t>>;
                using CachedReadPosition =
                    std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, size_t,
                                       EmptyRingBufferField<RingBufferField::CachedReadPosition>>;
                using CachedWritePosition =
                    std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, size_t,
                                       EmptyRingBufferField<RingBufferField::CachedWritePosition>>;
                using Arena = std::conditional_t<kProtocol == RingBufferProtocol::Local, BytesArena<T, Capacity>,
                                                 std::conditional_t<kProtocol == RingBufferProtocol::SequenceCell,
                                                                    CellsArena<T, Capacity, CacheLineAligned>,
                                                                    EmptyRingBufferField<RingBufferField::Arena>>>;
                using WriteDomain =
                    std::conditional_t<Topo.producers == QueueCardinality::Multiple, $::Contended, $::ProducerOwned>;
                using ReadDomain =
                    std::conditional_t<Topo.consumers == QueueCardinality::Multiple, $::Contended, $::ConsumerOwned>;
                using CachedReadDomain = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor,
                                                            $::ProducerOwned, $::Immutable>;
                using CachedWriteDomain = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor,
                                                             $::ConsumerOwned, $::Immutable>;
                using ArenaDomain = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, $::Immutable,
                                                       $::SharedStorage>;
                using Cell = SequenceCell<T, CacheLineAligned>;

                class SuccessfulPopCommit {
                public:
                    explicit SuccessfulPopCommit(RingBufferImpl& ring) noexcept
                        : ring_(ring), exceptionCount_(std::uncaught_exceptions()) {}

                    ~SuccessfulPopCommit() noexcept {
                        if (std::uncaught_exceptions() == exceptionCount_) {
                            ring_.PopFront();
                        }
                    }

                private:
                    RingBufferImpl& ring_;
                    int exceptionCount_;
                };

                static_assert(kProtocol == RingBufferProtocol::Local || std::atomic<size_t>::is_always_lock_free,
                              "concurrent RingBuffer requires lock-free size_t atomics");

            public:
                using ValueType = T;

                static constexpr size_t kCapacity = Capacity;
                static constexpr QueueTopology topology = Topo;
                static constexpr bool transactionalPushBatch = kProtocol != RingBufferProtocol::SequenceCell;
                static constexpr bool isolatesCells = kProtocol == RingBufferProtocol::SequenceCell && CacheLineAligned;

                RingBufferImpl() noexcept(kProtocol != RingBufferProtocol::PublishedCursor) {
                    if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        allocatedSlots_ = allocator_.allocate(slotCount_ + 2 * kPadding);
                    } else if constexpr (kProtocol == RingBufferProtocol::SequenceCell) {
                        for (size_t index = 0; index != Capacity; ++index) {
                            arena_.cells[index].sequence.store(index, std::memory_order_relaxed);
                        }
                    }
                }

                RingBufferImpl(const RingBufferImpl&) = delete;
                RingBufferImpl& operator=(const RingBufferImpl&) = delete;
                RingBufferImpl(RingBufferImpl&&) = delete;
                RingBufferImpl& operator=(RingBufferImpl&&) = delete;

                /** @brief Destroy all remaining elements after every endpoint has reached quiescence. */
                ~RingBufferImpl() noexcept {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        while (readPosition_ != writePosition_) {
                            std::destroy_at(Slot(readPosition_));
                            ++readPosition_;
                        }
                    } else if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        size_t read = readPosition_.load(std::memory_order_relaxed);
                        const size_t write = writePosition_.load(std::memory_order_relaxed);
                        while (read != write) {
                            std::destroy_at(Slot(read));
                            read = Next(read);
                        }
                        allocator_.deallocate(allocatedSlots_, slotCount_ + 2 * kPadding);
                    } else if constexpr (!std::is_trivially_destructible_v<T>) {
                        while (TryConsume([](T&&) noexcept {})) {
                        }
                    }
                }

                /** @brief Construct and publish one element when a slot is immediately available. */
                template<class... Args>
                    requires std::constructible_from<T, Args&&...> && (kProtocol != RingBufferProtocol::SequenceCell ||
                                                                       Topo.producers != QueueCardinality::Multiple ||
                                                                       std::is_nothrow_constructible_v<T, Args && ...>)
                [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        if (writePosition_ - readPosition_ == Capacity) {
                            return false;
                        }
                        ::new (Storage(writePosition_)) T(std::forward<Args>(args)...);
                        ++writePosition_;
                        return true;
                    } else if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        const size_t write = writePosition_.load(std::memory_order_relaxed);
                        const size_t next = Next(write);
                        if (next == cachedReadPosition_) {
                            cachedReadPosition_ = readPosition_.load(std::memory_order_acquire);
                            if (next == cachedReadPosition_) {
                                return false;
                            }
                        }
                        ::new (Storage(write)) T(std::forward<Args>(args)...);
                        writePosition_.store(next, std::memory_order_release);
                        return true;
                    } else {
                        return TryEmplaceSequence(std::forward<Args>(args)...);
                    }
                }

                /** @brief Forward one value to @ref TryEmplace. */
                template<class U>
                    requires std::constructible_from<T, U&&> &&
                             (kProtocol != RingBufferProtocol::SequenceCell ||
                              Topo.producers != QueueCardinality::Multiple || std::is_nothrow_constructible_v<T, U &&>)
                [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                    return TryEmplace(std::forward<U>(value));
                }

                /** @brief Yield until the SPSC producer can construct and publish one element. */
                template<class... Args>
                    requires(kProtocol == RingBufferProtocol::PublishedCursor) && std::constructible_from<T, Args&&...>
                void Emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                    while (!TryEmplace(std::forward<Args>(args)...)) {
                        std::this_thread::yield();
                    }
                }

                /** @brief Yield until the SPSC producer can publish one value. */
                template<class U>
                    requires(kProtocol == RingBufferProtocol::PublishedCursor) && std::constructible_from<T, U&&>
                void Push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                    while (!TryPush(std::forward<U>(value))) {
                        std::this_thread::yield();
                    }
                }

                /** @brief Move and consume the oldest immediately available element. */
                [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
                    requires std::move_constructible<T>
                {
                    if constexpr (kProtocol != RingBufferProtocol::SequenceCell) {
                        T* value = Front();
                        if (value == nullptr) {
                            return std::nullopt;
                        }
                        if constexpr (std::is_nothrow_move_constructible_v<T>) {
                            std::optional<T> result{std::move(*value)};
                            PopFront();
                            return result;
                        } else {
                            SuccessfulPopCommit commit(*this);
                            return std::optional<T>{std::in_place, std::move(*value)};
                        }
                    } else {
                        std::optional<T> result;
                        if (!TryConsume([&result](T&& source) noexcept { result.emplace(std::move(source)); })) {
                            return std::nullopt;
                        }
                        return result;
                    }
                }

                /** @brief Move-assign and consume the oldest immediately available element. */
                [[nodiscard]] bool TryPop(T& output) noexcept(std::is_nothrow_move_assignable_v<T>)
                    requires std::is_move_assignable_v<T> &&
                             (kProtocol != RingBufferProtocol::SequenceCell || std::is_nothrow_move_assignable_v<T>)
                {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        const size_t read = readPosition_;
                        if (read == writePosition_) {
                            return false;
                        }
                        T* value = Slot(read);
                        output = std::move(*value);
                        std::destroy_at(value);
                        readPosition_ = read + 1;
                        return true;
                    } else if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        T* value = Front();
                        if (value == nullptr) {
                            return false;
                        }
                        output = std::move(*value);
                        PopFront();
                        return true;
                    } else {
                        return TryConsume([&output](T&& source) noexcept { output = std::move(source); });
                    }
                }

                /** @brief Yield until the SPSC consumer can return one element. */
                [[nodiscard]] T Pop() noexcept(std::is_nothrow_move_constructible_v<T>)
                    requires(kProtocol == RingBufferProtocol::PublishedCursor) && std::move_constructible<T>
                {
                    for (;;) {
                        T* value = Front();
                        if (value == nullptr) {
                            std::this_thread::yield();
                            continue;
                        }
                        if constexpr (std::is_nothrow_move_constructible_v<T>) {
                            T result(std::move(*value));
                            PopFront();
                            return result;
                        } else {
                            SuccessfulPopCommit commit(*this);
                            return T(std::move(*value));
                        }
                    }
                }

                /** @brief Consume the currently available FIFO prefix through @p sink. */
                template<class Sink>
                    requires std::invocable<Sink&, T&&> && (kProtocol != RingBufferProtocol::SequenceCell ||
                                                            Topo.consumers == QueueCardinality::Single)
                size_t Drain(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        const size_t end = writePosition_;
                        size_t count = 0;
                        while (readPosition_ != end) {
                            T* value = Slot(readPosition_);
                            std::invoke(sink, std::move(*value));
                            std::destroy_at(value);
                            ++readPosition_;
                            ++count;
                        }
                        return count;
                    } else if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        const size_t begin = readPosition_.load(std::memory_order_relaxed);
                        const size_t end = writePosition_.load(std::memory_order_acquire);
                        cachedWritePosition_ = end;
                        size_t count = 0;
                        if constexpr (std::is_nothrow_invocable_v<Sink&, T&&>) {
                            for (size_t position = begin; position != end; position = Next(position)) {
                                T* value = Slot(position);
                                std::invoke(sink, std::move(*value));
                                std::destroy_at(value);
                                ++count;
                            }
                            if (begin != end) {
                                readPosition_.store(end, std::memory_order_release);
                            }
                        } else {
                            size_t position = begin;
                            while (position != end) {
                                T* value = Slot(position);
                                std::invoke(sink, std::move(*value));
                                std::destroy_at(value);
                                position = Next(position);
                                readPosition_.store(position, std::memory_order_release);
                                ++count;
                            }
                        }
                        return count;
                    } else {
                        return DrainSequence(std::forward<Sink>(sink));
                    }
                }

                /** @brief Return the oldest element without consuming it, or null when none is available. */
                [[nodiscard]] T* Front() noexcept
                    requires(kProtocol != RingBufferProtocol::SequenceCell)
                {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        return readPosition_ == writePosition_ ? nullptr : Slot(readPosition_);
                    } else {
                        const size_t read = readPosition_.load(std::memory_order_relaxed);
                        return RefreshPublishedWrite(read) ? Slot(read) : nullptr;
                    }
                }

                /** @brief Destroy and release the element returned by the preceding successful @ref Front call. */
                void PopFront() noexcept
                    requires(kProtocol != RingBufferProtocol::SequenceCell)
                {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        std::destroy_at(Slot(readPosition_));
                        ++readPosition_;
                    } else {
                        const size_t read = readPosition_.load(std::memory_order_relaxed);
                        std::destroy_at(Slot(read));
                        readPosition_.store(Next(read), std::memory_order_release);
                    }
                }

                /** @brief Return an approximate element count from one cursor snapshot. */
                [[nodiscard]] size_t SizeApprox() const noexcept {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        return writePosition_ - readPosition_;
                    } else if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        const size_t read = readPosition_.load(std::memory_order_acquire);
                        const size_t write = writePosition_.load(std::memory_order_acquire);
                        return write >= read ? write - read : write + slotCount_ - read;
                    } else {
                        const size_t consumed = readPosition_.load(std::memory_order_acquire);
                        const size_t produced = writePosition_.load(std::memory_order_acquire);
                        const size_t size = produced - consumed;
                        return size <= Capacity ? size : Capacity;
                    }
                }

                /** @brief Return whether one approximate cursor snapshot appears empty. */
                [[nodiscard]] bool IsEmpty() const noexcept { return SizeApprox() == 0; }

                /** @brief Return whether one approximate cursor snapshot appears full. */
                [[nodiscard]] bool IsFull() const noexcept { return SizeApprox() == Capacity; }

                /** @brief Return the translation-time physical-layout report for this specialization. */
                [[nodiscard]] static consteval Meta::CacheLayoutReport Layout() {
                    return Meta::AnalyzeCacheLayout<RingBufferImpl>();
                }

            private:
                [[nodiscard]] size_t Next(size_t position) const noexcept
                    requires(kProtocol == RingBufferProtocol::PublishedCursor)
                {
                    ++position;
                    return position == slotCount_ ? 0 : position;
                }

                [[nodiscard]] bool RefreshPublishedWrite(size_t read) noexcept
                    requires(kProtocol == RingBufferProtocol::PublishedCursor)
                {
                    if (read != cachedWritePosition_) {
                        return true;
                    }
                    cachedWritePosition_ = writePosition_.load(std::memory_order_acquire);
                    return read != cachedWritePosition_;
                }

                [[nodiscard]] void* Storage(size_t position) noexcept
                    requires(kProtocol != RingBufferProtocol::SequenceCell)
                {
                    if constexpr (kProtocol == RingBufferProtocol::Local) {
                        return &arena_.slots[(position & kIndexMask) * sizeof(T)];
                    } else {
                        return allocatedSlots_ + kPadding + position;
                    }
                }

                [[nodiscard]] T* Slot(size_t position) noexcept
                    requires(kProtocol != RingBufferProtocol::SequenceCell)
                {
                    return std::assume_aligned<alignof(T)>(std::launder(reinterpret_cast<T*>(Storage(position))));
                }

                template<class... Args>
                    requires(kProtocol == RingBufferProtocol::SequenceCell) && std::constructible_from<T, Args&&...> &&
                            (Topo.producers != QueueCardinality::Multiple ||
                             std::is_nothrow_constructible_v<T, Args && ...>)
                [[nodiscard]] bool
                TryEmplaceSequence(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                    size_t position = writePosition_.load(std::memory_order_relaxed);
                    if constexpr (Topo.producers == QueueCardinality::Single) {
                        Cell& cell = arena_.cells[position & kIndexMask];
                        const size_t sequence = cell.sequence.load(std::memory_order_acquire);
                        if (SequenceDifference(sequence, position) != 0) {
                            return false;
                        }
                        ::new (cell.Storage()) T(std::forward<Args>(args)...);
                        writePosition_.store(position + 1, std::memory_order_relaxed);
                        cell.sequence.store(position + 1, std::memory_order_release);
                        return true;
                    } else {
                        for (;;) {
                            Cell& cell = arena_.cells[position & kIndexMask];
                            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
                            const auto difference = SequenceDifference(sequence, position);
                            if (difference == 0) {
                                if (!writePosition_.compare_exchange_weak(
                                        position, position + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                                    continue;
                                }
                                ::new (cell.Storage()) T(std::forward<Args>(args)...);
                                cell.sequence.store(position + 1, std::memory_order_release);
                                return true;
                            }
                            if (difference < 0) {
                                return false;
                            }
                            position = writePosition_.load(std::memory_order_relaxed);
                        }
                    }
                }

                template<class Sink>
                    requires(kProtocol == RingBufferProtocol::SequenceCell) &&
                            (Topo.consumers == QueueCardinality::Single) && std::invocable<Sink&, T&&>
                size_t DrainSequence(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
                    size_t drained = 0;
                    size_t position = readPosition_.load(std::memory_order_relaxed);
                    for (;;) {
                        Cell& cell = arena_.cells[position & kIndexMask];
                        const size_t sequence = cell.sequence.load(std::memory_order_acquire);
                        if (SequenceDifference(sequence, position + 1) != 0) {
                            break;
                        }
                        T value(std::move(*cell.Value()));
                        std::destroy_at(cell.Value());
                        cell.sequence.store(position + Capacity, std::memory_order_release);
                        readPosition_.store(++position, std::memory_order_relaxed);
                        ++drained;
                        std::invoke(sink, std::move(value));
                    }
                    return drained;
                }

                template<class Consumer>
                    requires(kProtocol == RingBufferProtocol::SequenceCell) &&
                            std::is_nothrow_invocable_v<Consumer&, T&&>
                [[nodiscard]] bool TryConsume(Consumer&& consumer) noexcept {
                    size_t position = readPosition_.load(std::memory_order_relaxed);
                    for (;;) {
                        Cell& cell = arena_.cells[position & kIndexMask];
                        const size_t sequence = cell.sequence.load(std::memory_order_acquire);
                        const auto difference = SequenceDifference(sequence, position + 1);
                        if (difference == 0) {
                            if constexpr (Topo.consumers == QueueCardinality::Multiple) {
                                if (!readPosition_.compare_exchange_weak(
                                        position, position + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                                    continue;
                                }
                            }
                            std::invoke(consumer, std::move(*cell.Value()));
                            std::destroy_at(cell.Value());
                            cell.sequence.store(position + Capacity, std::memory_order_release);
                            if constexpr (Topo.consumers == QueueCardinality::Single) {
                                readPosition_.store(position + 1, std::memory_order_relaxed);
                            }
                            return true;
                        }
                        if (difference < 0) {
                            return false;
                        }
                        if constexpr (Topo.consumers == QueueCardinality::Multiple) {
                            position = readPosition_.load(std::memory_order_relaxed);
                        } else {
                            return false;
                        }
                    }
                }

                [[nodiscard]] static consteval PublishedSlotCount InitialSlotCount() {
                    if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        return kSlotCount;
                    } else {
                        return {};
                    }
                }

                // clang-format off
                [[= $::Immutable{}]]
                SORA_NO_UNIQUE_ADDRESS PublishedSlotCount slotCount_ = InitialSlotCount();
                [[= $::Immutable{}]]
                SORA_NO_UNIQUE_ADDRESS AllocatedSlots allocatedSlots_{};
                [[= $::Immutable{}]]
                SORA_NO_UNIQUE_ADDRESS Allocator allocator_{};
                [[= WriteDomain{}]]
                alignas(kPositionAlignment) Position writePosition_{0};
                [[= CachedReadDomain{}]]
                SORA_NO_UNIQUE_ADDRESS alignas(kCacheAlignment) CachedReadPosition cachedReadPosition_{};
                [[= ReadDomain{}]]
                alignas(kPositionAlignment) Position readPosition_{0};
                [[= CachedWriteDomain{}]]
                SORA_NO_UNIQUE_ADDRESS alignas(kCacheAlignment) CachedWritePosition cachedWritePosition_{};
                [[= ArenaDomain{}]]
                SORA_NO_UNIQUE_ADDRESS alignas(kArenaAlignment) Arena arena_{};
                // clang-format on
            };

        } // namespace Detail

        /**
         * @brief Bounded ring buffer whose synchronization protocol is selected entirely at compile time.
         * @tparam T Stored payload type.
         * @tparam Capacity Power-of-two logical capacity.
         * @tparam Topo Producer, consumer, and sharing topology.
         */
        template<class T, size_t Capacity, QueueTopology Topo>
        using RingBuffer = Detail::RingBufferImpl<T, Capacity, Topo>;

    } // namespace Concurrency

} // namespace Sora
