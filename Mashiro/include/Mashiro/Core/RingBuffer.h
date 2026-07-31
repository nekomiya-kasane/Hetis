/**
 * @file RingBuffer.h
 * @brief Compile-time-specialized bounded ring buffers for local, SPSC, MPSC, SPMC, and MPMC topologies.
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/Attributes.h"
#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/QueueTopology.h"
#include "Mashiro/Core/TypeTraits.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

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
            return Concurrency::IsQueueTopologyValid(topology);
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

        enum class RingProtocol : std::uint8_t {
            Local,
            PublishedCursor,
            SequenceCell,
        };

        /** @brief Physical member roles whose types and contention domains are dispatched at compile time. */
        enum class RingField : std::uint8_t {
            PublishedSlotCount,
            AllocatedSlots,
            Allocator,
            WritePosition,
            CachedReadPosition,
            ReadPosition,
            CachedWritePosition,
            Arena,
        };

        /** @brief Inline raw payload arena used only by the single-thread protocol. */
        template<class T, std::size_t Capacity>
        struct InlineSlotArena {
            alignas(T) std::byte slots[Capacity * sizeof(T)];
        };

        /** @brief Sequence cells used by every topology with a multi-participant endpoint. */
        template<class T, std::size_t Capacity, bool IsolateCells>
        struct SequenceCellArena {
            SequenceCell<T, IsolateCells> cells[Capacity]{};
        };

    } // namespace Detail

    namespace Meta {

        /** @brief Match a queue topology to the protocol that determines generated code and physical layout. */
        [[nodiscard]] consteval Detail::RingProtocol DispatchRingProtocol(Concurrency::QueueTopology topology) {
            if (topology.sharing == Concurrency::QueueSharing::SingleThread) {
                return Detail::RingProtocol::Local;
            }
            if (topology.producers == Concurrency::QueueCardinality::Single &&
                topology.consumers == Concurrency::QueueCardinality::Single) {
                return Detail::RingProtocol::PublishedCursor;
            }
            return Detail::RingProtocol::SequenceCell;
        }

        /**
         * @brief Match one ring-buffer field role and topology to its concrete reflected type.
         * @details An inactive field becomes a distinct empty @c integral_constant keyed by @p Field. This preserves
         * unique member identities while allowing @c MASHIRO_NO_UNIQUE_ADDRESS to erase all inactive storage.
         */
        template<Detail::RingField Field, class T, std::size_t Capacity, Concurrency::QueueTopology Topology,
                 bool IsolateCells>
        [[nodiscard]] consteval std::meta::info DispatchRingFieldType() {
            constexpr Detail::RingProtocol protocol = DispatchRingProtocol(Topology);
            using Empty = std::integral_constant<Detail::RingField, Field>;

            if constexpr (Field == Detail::RingField::PublishedSlotCount) {
                if constexpr (protocol == Detail::RingProtocol::PublishedCursor) {
                    using Type = const std::size_t;
                    return ^^Type;
                } else {
                    return ^^Empty;
                }
            } else if constexpr (Field == Detail::RingField::AllocatedSlots) {
                if constexpr (protocol == Detail::RingProtocol::PublishedCursor) {
                    using Type = T*;
                    return ^^Type;
                } else {
                    return ^^Empty;
                }
            } else if constexpr (Field == Detail::RingField::Allocator) {
                using Type = std::allocator<T>;
                return ^^Type;
            } else if constexpr (Field == Detail::RingField::WritePosition ||
                                 Field == Detail::RingField::ReadPosition) {
                if constexpr (protocol == Detail::RingProtocol::Local) {
                    return ^^std::size_t;
                } else {
                    using Type = std::atomic<std::size_t>;
                    return ^^Type;
                }
            } else if constexpr (Field == Detail::RingField::CachedReadPosition ||
                                 Field == Detail::RingField::CachedWritePosition) {
                if constexpr (protocol == Detail::RingProtocol::PublishedCursor) {
                    return ^^std::size_t;
                } else {
                    return ^^Empty;
                }
            } else {
                static_assert(Field == Detail::RingField::Arena);
                if constexpr (protocol == Detail::RingProtocol::Local) {
                    using Type = Detail::InlineSlotArena<T, Capacity>;
                    return ^^Type;
                } else if constexpr (protocol == Detail::RingProtocol::SequenceCell) {
                    using Type = Detail::SequenceCellArena<T, Capacity, IsolateCells>;
                    return ^^Type;
                } else {
                    return ^^Empty;
                }
            }
        }

        /** @brief Match one ring-buffer field role and topology to its reflected contention-domain type. */
        template<Detail::RingField Field, Concurrency::QueueTopology Topology>
        [[nodiscard]] consteval std::meta::info DispatchRingFieldDomain() {
            constexpr Detail::RingProtocol protocol = DispatchRingProtocol(Topology);

            if constexpr (Field == Detail::RingField::WritePosition) {
                if constexpr (Topology.producers == Concurrency::QueueCardinality::Multiple) {
                    return ^^Concurrency::Contended;
                } else {
                    return ^^Concurrency::ProducerOwned;
                }
            } else if constexpr (Field == Detail::RingField::ReadPosition) {
                if constexpr (Topology.consumers == Concurrency::QueueCardinality::Multiple) {
                    return ^^Concurrency::Contended;
                } else {
                    return ^^Concurrency::ConsumerOwned;
                }
            } else if constexpr (Field == Detail::RingField::CachedReadPosition) {
                if constexpr (protocol == Detail::RingProtocol::PublishedCursor) {
                    return ^^Concurrency::ProducerOwned;
                } else {
                    return ^^Concurrency::Immutable;
                }
            } else if constexpr (Field == Detail::RingField::CachedWritePosition) {
                if constexpr (protocol == Detail::RingProtocol::PublishedCursor) {
                    return ^^Concurrency::ConsumerOwned;
                } else {
                    return ^^Concurrency::Immutable;
                }
            } else if constexpr (Field == Detail::RingField::Arena) {
                if constexpr (protocol == Detail::RingProtocol::PublishedCursor) {
                    return ^^Concurrency::Immutable;
                } else {
                    return ^^Concurrency::SharedStorage;
                }
            } else {
                return ^^Concurrency::Immutable;
            }
        }

    } // namespace Meta

    namespace Detail {

        template<class T, std::size_t Capacity, QueueTopology Topology>
        concept ValidRingBuffer = ValidateRingCapacity(Capacity) && ValidateQueueTopology(Topology) &&
                                  std::is_object_v<T> && std::is_nothrow_destructible_v<T> &&
                                  (Meta::DispatchRingProtocol(Topology) != RingProtocol::PublishedCursor ||
                                   Capacity <= (std::size_t{1} << 31)) &&
                                  (Meta::DispatchRingProtocol(Topology) != RingProtocol::SequenceCell ||
                                   Traits::ConcurrentQueueElement<T>);

        /**
         * @brief One bounded ring-buffer class whose protocol, state, and synchronization are specialized at compile
         * time.
         * @details Local, published-cursor SPSC, and sequence-cell protocols remain distinct mathematical state
         * machines. `if constexpr` erases unselected algorithms, while conditional field types and the MS-ABI
         * no-unique-address attribute erase unselected state. No protocol value or branch survives at runtime.
         */
        template<class T, std::size_t Capacity, QueueTopology Topology,
                 bool IsolateCells = Topology.producers == QueueCardinality::Multiple ||
                                     Topology.consumers == QueueCardinality::Multiple>
            requires ValidRingBuffer<T, Capacity, Topology>
        class RingBufferImplementation {
            static constexpr RingProtocol kProtocol = Meta::DispatchRingProtocol(Topology);
            static constexpr bool kLocal = kProtocol == RingProtocol::Local;
            static constexpr bool kPublishedCursor = kProtocol == RingProtocol::PublishedCursor;
            static constexpr bool kSequenceCell = kProtocol == RingProtocol::SequenceCell;
            static constexpr bool kMultipleProducers = Topology.producers == QueueCardinality::Multiple;
            static constexpr bool kMultipleConsumers = Topology.consumers == QueueCardinality::Multiple;
            static constexpr std::size_t kIndexMask = Capacity - 1;
            static constexpr std::size_t kSlotCount = Capacity + 1;
            static constexpr std::size_t kPadding = (Platform::kCacheLineSize - 1) / sizeof(T) + 1;
            static constexpr std::size_t kPositionAlignment = kLocal ? alignof(std::size_t) : Platform::kCacheLineSize;
            static constexpr std::size_t kCacheAlignment = kPublishedCursor ? Platform::kCacheLineSize : 1;
            static constexpr std::size_t kArenaAlignment =
                kSequenceCell ? Platform::kCacheLineSize : (kLocal ? alignof(T) : 1);

            using PublishedSlotCount =
                [:Meta::DispatchRingFieldType<RingField::PublishedSlotCount, T, Capacity, Topology, IsolateCells>():];
            using AllocatedSlots =
                [:Meta::DispatchRingFieldType<RingField::AllocatedSlots, T, Capacity, Topology, IsolateCells>():];
            using Allocator =
                [:Meta::DispatchRingFieldType<RingField::Allocator, T, Capacity, Topology, IsolateCells>():];
            using Position =
                [:Meta::DispatchRingFieldType<RingField::WritePosition, T, Capacity, Topology, IsolateCells>():];
            using CachedReadPosition =
                [:Meta::DispatchRingFieldType<RingField::CachedReadPosition, T, Capacity, Topology, IsolateCells>():];
            using CachedWritePosition =
                [:Meta::DispatchRingFieldType<RingField::CachedWritePosition, T, Capacity, Topology, IsolateCells>():];
            using Arena = [:Meta::DispatchRingFieldType<RingField::Arena, T, Capacity, Topology, IsolateCells>():];
            using WriteDomain = [:Meta::DispatchRingFieldDomain<RingField::WritePosition, Topology>():];
            using ReadDomain = [:Meta::DispatchRingFieldDomain<RingField::ReadPosition, Topology>():];
            using CachedReadDomain = [:Meta::DispatchRingFieldDomain<RingField::CachedReadPosition, Topology>():];
            using CachedWriteDomain = [:Meta::DispatchRingFieldDomain<RingField::CachedWritePosition, Topology>():];
            using ArenaDomain = [:Meta::DispatchRingFieldDomain<RingField::Arena, Topology>():];
            using Cell = SequenceCell<T, IsolateCells>;

            static_assert(kLocal || std::atomic<std::size_t>::is_always_lock_free,
                          "concurrent RingBuffer requires lock-free size_t atomics");

        public:
            using value_type = T;
            static constexpr std::size_t kCapacity = Capacity;
            static constexpr QueueTopology topology = Topology;
            static constexpr bool transactionalPushBatch = !kSequenceCell;
            static constexpr bool isolatesCells = kSequenceCell && IsolateCells;

            RingBufferImplementation() noexcept(!kPublishedCursor) {
                if constexpr (kPublishedCursor) {
                    allocatedSlots_ = allocator_.allocate(slotCount_ + 2 * kPadding);
                } else if constexpr (kSequenceCell) {
                    for (std::size_t index = 0; index != Capacity; ++index) {
                        arena_.cells[index].sequence.store(index, std::memory_order_relaxed);
                    }
                }
            }

            RingBufferImplementation(const RingBufferImplementation&) = delete;
            RingBufferImplementation& operator=(const RingBufferImplementation&) = delete;
            RingBufferImplementation(RingBufferImplementation&&) = delete;
            RingBufferImplementation& operator=(RingBufferImplementation&&) = delete;

            ~RingBufferImplementation() {
                if constexpr (kLocal) {
                    while (readPosition_ != writePosition_) {
                        std::destroy_at(Slot(readPosition_++));
                    }
                } else if constexpr (kPublishedCursor) {
                    std::size_t read = readPosition_.load(std::memory_order_relaxed);
                    const std::size_t write = writePosition_.load(std::memory_order_relaxed);
                    while (read != write) {
                        std::destroy_at(Slot(read));
                        read = Next(read);
                    }
                    allocator_.deallocate(allocatedSlots_, slotCount_ + 2 * kPadding);
                } else if constexpr (!std::is_trivially_destructible_v<T>) {
                    while (TryPop().has_value()) {
                    }
                }
            }

            template<class... Args>
                requires std::constructible_from<T, Args&&...> &&
                         (!kSequenceCell || !kMultipleProducers ||
                          std::is_nothrow_constructible_v<T, Args && ...>)
            [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                if constexpr (kLocal) {
                    if (writePosition_ - readPosition_ == Capacity) {
                        return false;
                    }
                    ::new (Storage(writePosition_)) T(std::forward<Args>(args)...);
                    ++writePosition_;
                    return true;
                } else if constexpr (kPublishedCursor) {
                    const std::size_t write = writePosition_.load(std::memory_order_relaxed);
                    const std::size_t next = Next(write);
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

            template<class U>
                requires std::constructible_from<T, U&&> &&
                         (!kSequenceCell || !kMultipleProducers || std::is_nothrow_constructible_v<T, U &&>)
            [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                return TryEmplace(std::forward<U>(value));
            }

            template<class U>
                requires kPublishedCursor && std::constructible_from<T, U&&>
            void Push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                while (!TryPush(std::forward<U>(value))) {
                    std::this_thread::yield();
                }
            }

            [[nodiscard]] std::optional<T> TryPop() noexcept(kSequenceCell ||
                                                             std::is_nothrow_move_constructible_v<T>)
                requires std::move_constructible<T>
            {
                if constexpr (kLocal) {
                    if (readPosition_ == writePosition_) {
                        return std::nullopt;
                    }
                    T* value = Slot(readPosition_++);
                    std::optional<T> result{std::move(*value)};
                    std::destroy_at(value);
                    return result;
                } else if constexpr (kPublishedCursor) {
                    T* value = Front();
                    if (value == nullptr) {
                        return std::nullopt;
                    }
                    std::optional<T> result{std::move(*value)};
                    PopFront();
                    return result;
                } else {
                    std::optional<T> result;
                    if (!TryConsume([&result](T&& source) noexcept { result.emplace(std::move(source)); })) {
                        return std::nullopt;
                    }
                    return result;
                }
            }

            [[nodiscard]] bool TryPop(T& output) noexcept(kSequenceCell || std::is_nothrow_move_assignable_v<T>)
                requires std::is_move_assignable_v<T> &&
                         (!kSequenceCell || std::is_nothrow_move_assignable_v<T>)
            {
                if constexpr (kLocal) {
                    if (readPosition_ == writePosition_) {
                        return false;
                    }
                    T* value = Slot(readPosition_++);
                    output = std::move(*value);
                    std::destroy_at(value);
                    return true;
                } else if constexpr (kPublishedCursor) {
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

            [[nodiscard]] T Pop() noexcept(std::is_nothrow_move_constructible_v<T>)
                requires kPublishedCursor && std::move_constructible<T>
            {
                for (;;) {
                    if (auto value = TryPop()) {
                        return std::move(*value);
                    }
                    std::this_thread::yield();
                }
            }

            template<class Sink>
                requires std::invocable<Sink&, T&&> && (!kSequenceCell || !kMultipleConsumers) &&
                         (kSequenceCell || std::is_nothrow_invocable_v<Sink&, T&&>)
            std::size_t Drain(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
                if constexpr (kLocal) {
                    const std::size_t end = writePosition_;
                    std::size_t count = 0;
                    while (readPosition_ != end) {
                        T* value = Slot(readPosition_++);
                        sink(std::move(*value));
                        std::destroy_at(value);
                        ++count;
                    }
                    return count;
                } else if constexpr (kPublishedCursor) {
                    const std::size_t begin = readPosition_.load(std::memory_order_relaxed);
                    const std::size_t end = writePosition_.load(std::memory_order_acquire);
                    cachedWritePosition_ = end;
                    std::size_t count = 0;
                    for (std::size_t position = begin; position != end; position = Next(position)) {
                        T* value = Slot(position);
                        sink(std::move(*value));
                        std::destroy_at(value);
                        ++count;
                    }
                    readPosition_.store(end, std::memory_order_release);
                    return count;
                } else {
                    return DrainSequence(std::forward<Sink>(sink));
                }
            }

            [[nodiscard]] T* Front() noexcept
                requires(!kSequenceCell)
            {
                if constexpr (kLocal) {
                    return readPosition_ == writePosition_ ? nullptr : Slot(readPosition_);
                } else {
                    const std::size_t read = readPosition_.load(std::memory_order_relaxed);
                    return RefreshPublishedWrite(read) ? Slot(read) : nullptr;
                }
            }

            /**
             * @brief Destroy and release the element returned by the preceding successful @ref Front call.
             * @pre Front() returned non-null and no consumer operation has executed since that call.
             */
            void PopFront() noexcept
                requires(!kSequenceCell)
            {
                if constexpr (kLocal) {
                    std::destroy_at(Slot(readPosition_));
                    ++readPosition_;
                } else {
                    const std::size_t read = readPosition_.load(std::memory_order_relaxed);
                    std::destroy_at(Slot(read));
                    readPosition_.store(Next(read), std::memory_order_release);
                }
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                if constexpr (kLocal) {
                    return writePosition_ - readPosition_;
                } else if constexpr (kPublishedCursor) {
                    const std::size_t read = readPosition_.load(std::memory_order_acquire);
                    const std::size_t write = writePosition_.load(std::memory_order_acquire);
                    return write >= read ? write - read : write + slotCount_ - read;
                } else {
                    const std::size_t consumed = readPosition_.load(std::memory_order_acquire);
                    const std::size_t produced = writePosition_.load(std::memory_order_acquire);
                    const std::size_t size = produced - consumed;
                    return size <= Capacity ? size : Capacity;
                }
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }
            [[nodiscard]] static constexpr std::size_t GetCapacity() noexcept { return Capacity; }

            [[nodiscard]] static consteval Concurrency::CacheLayoutReport Layout()
                requires kSequenceCell
            {
                return Concurrency::AnalyzeCacheLayout<RingBufferImplementation>();
            }

        private:
            [[nodiscard]] std::size_t Next(std::size_t position) const noexcept
                requires kPublishedCursor
            {
                ++position;
                return position == slotCount_ ? 0 : position;
            }

            [[nodiscard]] bool RefreshPublishedWrite(std::size_t read) noexcept
                requires kPublishedCursor
            {
                if (read != cachedWritePosition_) {
                    return true;
                }
                cachedWritePosition_ = writePosition_.load(std::memory_order_acquire);
                return read != cachedWritePosition_;
            }

            [[nodiscard]] void* Storage(std::size_t position) noexcept
                requires(!kSequenceCell)
            {
                if constexpr (kLocal) {
                    return &arena_.slots[(position & kIndexMask) * sizeof(T)];
                } else {
                    return allocatedSlots_ + kPadding + position;
                }
            }

            [[nodiscard]] T* Slot(std::size_t position) noexcept
                requires(!kSequenceCell)
            {
                return std::assume_aligned<alignof(T)>(std::launder(reinterpret_cast<T*>(Storage(position))));
            }

            template<class... Args>
                requires kSequenceCell && std::constructible_from<T, Args&&...> &&
                         (!kMultipleProducers || std::is_nothrow_constructible_v<T, Args && ...>)
            [[nodiscard]] bool TryEmplaceSequence(Args&&... args) noexcept(
                std::is_nothrow_constructible_v<T, Args&&...>) {
                std::size_t position = writePosition_.load(std::memory_order_relaxed);
                if constexpr (!kMultipleProducers) {
                    Cell& cell = arena_.cells[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    if (SequenceDifference(sequence, position) != 0) {
                        return false;
                    }
                    ::new (cell.Storage()) T(std::forward<Args>(args)...);
                    writePosition_.store(position + 1, std::memory_order_relaxed);
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }

                for (;;) {
                    Cell& cell = arena_.cells[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    const auto difference = SequenceDifference(sequence, position);
                    if (difference == 0) {
                        if (!writePosition_.compare_exchange_weak(position, position + 1,
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
                    position = writePosition_.load(std::memory_order_relaxed);
                }
            }

            template<class Sink>
                requires kSequenceCell && (!kMultipleConsumers) && std::invocable<Sink&, T&&>
            std::size_t DrainSequence(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
                std::size_t drained = 0;
                std::size_t position = readPosition_.load(std::memory_order_relaxed);
                for (;;) {
                    Cell& cell = arena_.cells[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    if (SequenceDifference(sequence, position + 1) != 0) {
                        break;
                    }
                    T value(std::move(*cell.Value()));
                    std::destroy_at(cell.Value());
                    cell.sequence.store(position + Capacity, std::memory_order_release);
                    readPosition_.store(++position, std::memory_order_relaxed);
                    ++drained;
                    sink(std::move(value));
                }
                return drained;
            }

            template<class Consumer>
                requires kSequenceCell && std::is_nothrow_invocable_v<Consumer&, T&&>
            [[nodiscard]] bool TryConsume(Consumer&& consumer) noexcept {
                std::size_t position = readPosition_.load(std::memory_order_relaxed);
                for (;;) {
                    Cell& cell = arena_.cells[position & kIndexMask];
                    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
                    const auto difference = SequenceDifference(sequence, position + 1);
                    if (difference == 0) {
                        if constexpr (kMultipleConsumers) {
                            if (!readPosition_.compare_exchange_weak(position, position + 1,
                                                                     std::memory_order_relaxed)) {
                                continue;
                            }
                        }
                        consumer(std::move(*cell.Value()));
                        std::destroy_at(cell.Value());
                        cell.sequence.store(position + Capacity, std::memory_order_release);
                        if constexpr (!kMultipleConsumers) {
                            readPosition_.store(position + 1, std::memory_order_relaxed);
                        }
                        return true;
                    }
                    if (difference < 0) {
                        return false;
                    }
                    if constexpr (kMultipleConsumers) {
                        position = readPosition_.load(std::memory_order_relaxed);
                    } else {
                        return false;
                    }
                }
            }

            [[nodiscard]] static consteval PublishedSlotCount InitialSlotCount() {
                if constexpr (kPublishedCursor) {
                    return kSlotCount;
                } else {
                    return {};
                }
            }

            [[= Concurrency::Immutable{}]] MASHIRO_NO_UNIQUE_ADDRESS PublishedSlotCount slotCount_ =
                InitialSlotCount();
            [[= Concurrency::Immutable{}]] MASHIRO_NO_UNIQUE_ADDRESS AllocatedSlots allocatedSlots_{};
            [[= Concurrency::Immutable{}]] MASHIRO_NO_UNIQUE_ADDRESS Allocator allocator_{};
            [[= WriteDomain{}]] alignas(kPositionAlignment) Position writePosition_{0};
            [[= CachedReadDomain{}]] MASHIRO_NO_UNIQUE_ADDRESS alignas(kCacheAlignment)
                CachedReadPosition cachedReadPosition_{};
            [[= ReadDomain{}]] alignas(kPositionAlignment) Position readPosition_{0};
            [[= CachedWriteDomain{}]] MASHIRO_NO_UNIQUE_ADDRESS alignas(kCacheAlignment)
                CachedWritePosition cachedWritePosition_{};
            [[= ArenaDomain{}]] MASHIRO_NO_UNIQUE_ADDRESS alignas(kArenaAlignment) Arena arena_{};
        };

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
    using RingBuffer = Detail::RingBufferImplementation<T, Capacity, Topology>;

    /** @cond INTERNAL */
    consteval {
        using Concurrency::AuditFalseSharing;
        using Concurrency::QueueTopologies::Local;
        using Concurrency::QueueTopologies::Mpmc;
        using Concurrency::QueueTopologies::Mpsc;
        using Concurrency::QueueTopologies::Spmc;
        using Concurrency::QueueTopologies::Spsc;

        static_assert(sizeof(RingBuffer<std::uint64_t, 1024, Local>) ==
                      2 * sizeof(std::size_t) + 1024 * sizeof(std::uint64_t));
        static_assert(sizeof(RingBuffer<std::uint64_t, 1024, Spsc>) == 5 * Platform::kCacheLineSize);
        static_assert(sizeof(RingBuffer<std::uint64_t, 1024, Mpsc>) ==
                      (1024 + 2) * Platform::kCacheLineSize);
        static_assert(sizeof(RingBuffer<std::uint64_t, 1024, Spmc>) ==
                      (1024 + 2) * Platform::kCacheLineSize);
        static_assert(sizeof(RingBuffer<std::uint64_t, 1024, Mpmc>) ==
                      (1024 + 2) * Platform::kCacheLineSize);
        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Spsc>>());
        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Mpsc>>());
        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Spmc>>());
        static_assert(AuditFalseSharing<RingBuffer<std::uint64_t, 1024, Mpmc>>());
    }
    /** @endcond */

} // namespace Mashiro
