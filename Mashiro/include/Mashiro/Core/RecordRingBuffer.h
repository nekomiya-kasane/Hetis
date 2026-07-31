/**
 * @file RecordRingBuffer.h
 * @brief Bounded packed-record ring buffers specialized by compile-time producer and consumer topology.
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/Attributes.h"
#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/QueueTopology.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <meta>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace Mashiro {

    namespace Detail {

        inline constexpr std::size_t kRecordAlignment = 16;
        inline constexpr std::size_t kRecordHeaderBytes = 16;
        inline constexpr std::uint32_t kPaddingRecord = std::numeric_limits<std::uint32_t>::max();

        /** @brief Writers participating in the same multi-producer reservation protocol. */
        struct RecordProducerContention : Concurrency::ContentionDomain {};

        /** @brief Readers participating in the same multi-consumer claim and reclaim protocol. */
        struct RecordConsumerContention : Concurrency::ContentionDomain {};

        enum class RecordField : std::uint8_t {
            ProducerPosition,
            CachedReclaimPosition,
            ConsumerClaimPosition,
            ConsumerReclaimPosition,
        };

        /** @brief Round one byte count up to the physical record alignment. */
        [[nodiscard]] constexpr std::size_t AlignRecordBytes(std::size_t bytes) noexcept {
            return (bytes + kRecordAlignment - 1) & ~(kRecordAlignment - 1);
        }

        /** @brief Validate packed-record arena capacity and modular-position constraints. */
        [[nodiscard]] consteval bool ValidateRecordRingCapacity(std::size_t capacity) {
            return capacity >= 128 && std::has_single_bit(capacity) &&
                   capacity <= std::numeric_limits<std::uint32_t>::max();
        }

        /** @brief Interpret a modular position subtraction as its two's-complement signed difference. */
        [[nodiscard]] constexpr std::int64_t RecordPositionDifference(std::uint64_t lhs,
                                                                      std::uint64_t rhs) noexcept {
            return std::bit_cast<std::int64_t>(lhs - rhs);
        }

    } // namespace Detail

    namespace Meta {

        /** @brief Select the physical type of one packed-record ring position field. */
        template<Detail::RecordField Field, Concurrency::QueueTopology Topology>
        [[nodiscard]] consteval std::meta::info DispatchRecordRingFieldType() {
            using Empty = std::integral_constant<Detail::RecordField, Field>;
            constexpr bool local = Topology.sharing == Concurrency::QueueSharing::SingleThread;
            constexpr bool multipleProducers = Topology.producers == Concurrency::QueueCardinality::Multiple;
            constexpr bool multipleConsumers = Topology.consumers == Concurrency::QueueCardinality::Multiple;

            if constexpr (Field == Detail::RecordField::CachedReclaimPosition) {
                if constexpr (local) {
                    return ^^Empty;
                } else if constexpr (multipleProducers) {
                    using Type = std::atomic<std::uint64_t>;
                    return ^^Type;
                } else {
                    using Type = std::uint64_t;
                    return ^^Type;
                }
            } else if constexpr (Field == Detail::RecordField::ConsumerClaimPosition) {
                if constexpr (multipleConsumers) {
                    using Type = std::atomic<std::uint64_t>;
                    return ^^Type;
                } else {
                    return ^^Empty;
                }
            } else if constexpr (local) {
                using Type = std::uint64_t;
                return ^^Type;
            } else {
                using Type = std::atomic<std::uint64_t>;
                return ^^Type;
            }
        }

        /** @brief Select the reflected contention domain of one packed-record ring position field. */
        template<Detail::RecordField Field, Concurrency::QueueTopology Topology>
        [[nodiscard]] consteval std::meta::info DispatchRecordRingFieldDomain() {
            constexpr bool local = Topology.sharing == Concurrency::QueueSharing::SingleThread;
            constexpr bool multipleProducers = Topology.producers == Concurrency::QueueCardinality::Multiple;
            constexpr bool multipleConsumers = Topology.consumers == Concurrency::QueueCardinality::Multiple;

            if constexpr (local) {
                return ^^Concurrency::Immutable;
            } else if constexpr (Field == Detail::RecordField::ProducerPosition ||
                                 Field == Detail::RecordField::CachedReclaimPosition) {
                if constexpr (multipleProducers) {
                    return ^^Detail::RecordProducerContention;
                } else {
                    return ^^Concurrency::ProducerOwned;
                }
            } else if constexpr (Field == Detail::RecordField::ConsumerClaimPosition) {
                if constexpr (multipleConsumers) {
                    return ^^Detail::RecordConsumerContention;
                } else {
                    return ^^Concurrency::Immutable;
                }
            } else if constexpr (multipleConsumers) {
                return ^^Detail::RecordConsumerContention;
            } else {
                return ^^Concurrency::ConsumerOwned;
            }
        }

    } // namespace Meta

    namespace Detail {

        template<std::size_t ByteCapacity, Concurrency::QueueTopology Topology>
        concept ValidRecordRing = ValidateRecordRingCapacity(ByteCapacity) &&
                                  Concurrency::IsQueueTopologyValid(Topology);

        /**
         * @brief One packed-record ring whose synchronization protocol is generated from @p Topology.
         * @details Records remain contiguous by consuming the physical tail as an explicit padding record. Multi-
         * producer variants reserve bytes before publication, so a preempted producer can temporarily create a FIFO
         * publication hole. Multi-consumer variants separately claim records and reclaim completed byte ranges,
         * preventing producers from overwriting payloads while callbacks still access them.
         */
        template<std::size_t ByteCapacity, Concurrency::QueueTopology Topology>
            requires ValidRecordRing<ByteCapacity, Topology>
        class RecordRingBufferImplementation {
            static constexpr bool kLocal = Topology.sharing == Concurrency::QueueSharing::SingleThread;
            static constexpr bool kMultipleProducers =
                Topology.producers == Concurrency::QueueCardinality::Multiple;
            static constexpr bool kMultipleConsumers =
                Topology.consumers == Concurrency::QueueCardinality::Multiple;
            static constexpr std::size_t kMask = ByteCapacity - 1;
            static constexpr std::size_t kWordCount = ByteCapacity / sizeof(std::uint64_t);
            static constexpr std::uint64_t kCommitted = 1;
            static constexpr std::uint64_t kCompleted = 3;
            static constexpr std::size_t kPositionAlignment = kLocal ? alignof(std::uint64_t)
                                                                     : Platform::kCacheLineSize;

            using ProducerPosition =
                [:Meta::DispatchRecordRingFieldType<RecordField::ProducerPosition, Topology>():];
            using CachedReclaimPosition =
                [:Meta::DispatchRecordRingFieldType<RecordField::CachedReclaimPosition, Topology>():];
            using ConsumerClaimPosition =
                [:Meta::DispatchRecordRingFieldType<RecordField::ConsumerClaimPosition, Topology>():];
            using ConsumerReclaimPosition =
                [:Meta::DispatchRecordRingFieldType<RecordField::ConsumerReclaimPosition, Topology>():];
            using ProducerDomain =
                [:Meta::DispatchRecordRingFieldDomain<RecordField::ProducerPosition, Topology>():];
            using CachedReclaimDomain =
                [:Meta::DispatchRecordRingFieldDomain<RecordField::CachedReclaimPosition, Topology>():];
            using ConsumerClaimDomain =
                [:Meta::DispatchRecordRingFieldDomain<RecordField::ConsumerClaimPosition, Topology>():];
            using ConsumerReclaimDomain =
                [:Meta::DispatchRecordRingFieldDomain<RecordField::ConsumerReclaimPosition, Topology>():];

            struct Reservation {
                std::uint64_t position;
                std::size_t index;
                std::size_t padding;
            };

            static_assert(kLocal || std::atomic<std::uint64_t>::is_always_lock_free,
                          "concurrent RecordRingBuffer requires lock-free 64-bit atomics");

        public:
            static constexpr std::size_t kByteCapacity = ByteCapacity;
            // A record occupying at most half the arena can always follow wrap padding from any aligned position.
            static constexpr std::size_t kMaxRecordBytes = ByteCapacity / 2 - kRecordHeaderBytes;
            static constexpr Concurrency::QueueTopology topology = Topology;

            RecordRingBufferImplementation() = default;
            RecordRingBufferImplementation(const RecordRingBufferImplementation&) = delete;
            RecordRingBufferImplementation& operator=(const RecordRingBufferImplementation&) = delete;
            RecordRingBufferImplementation(RecordRingBufferImplementation&&) = delete;
            RecordRingBufferImplementation& operator=(RecordRingBufferImplementation&&) = delete;

            /** @brief Try to publish one non-empty record without allocation or blocking. */
            [[nodiscard]] bool TryWrite(std::span<const std::byte> record) noexcept {
                if (record.empty() || record.size() > kMaxRecordBytes) [[unlikely]] {
                    return false;
                }

                const std::size_t recordBytes = AlignRecordBytes(kRecordHeaderBytes + record.size());
                const auto reservation = TryReserve(recordBytes);
                if (!reservation.has_value()) {
                    return false;
                }

                const std::size_t recordIndex =
                    reservation->padding == 0 ? reservation->index : 0;
                WriteMetadata(recordIndex, static_cast<std::uint32_t>(record.size()));
                std::memcpy(Bytes() + recordIndex + kRecordHeaderBytes, record.data(), record.size());
                Publish(recordIndex, reservation->position + reservation->padding);

                if (reservation->padding != 0) {
                    WriteMetadata(reservation->index, kPaddingRecord);
                    Publish(reservation->index, reservation->position);
                }
                return true;
            }

            /** @brief Convenience overload for a raw pointer and byte count. */
            [[nodiscard]] bool TryWrite(const void* data, std::size_t size) noexcept {
                if (data == nullptr) [[unlikely]] {
                    return false;
                }
                return TryWrite({static_cast<const std::byte*>(data), size});
            }

            /**
             * @brief Try to claim and consume one committed record.
             * @details The record is completed on normal return and stack unwinding, providing at-most-once callback
             * delivery. Multiple consumers claim in FIFO order, but their callbacks may complete concurrently.
             */
            template<class Consumer>
                requires std::invocable<Consumer&, std::span<const std::byte>>
            [[nodiscard]] bool TryConsume(Consumer&& consumer)
                noexcept(std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>) {
                if constexpr (kMultipleConsumers) {
                    return TryConsumeMultiple(std::forward<Consumer>(consumer));
                } else {
                    return TryConsumeSingle(std::forward<Consumer>(consumer));
                }
            }

            /** @brief Drain records reserved before function entry until a publication hole or snapshot end. */
            template<class Consumer>
                requires std::invocable<Consumer&, std::span<const std::byte>>
            std::size_t Drain(Consumer&& consumer)
                noexcept(std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>) {
                const std::uint64_t end = LoadProducerPosition(std::memory_order_acquire);
                std::size_t count = 0;
                while (RecordPositionDifference(LoadConsumerClaimPosition(), end) < 0) {
                    if (!TryConsume(consumer)) [[unlikely]] {
                        break;
                    }
                    ++count;
                }
                return count;
            }

            /** @brief Discard occupied records; all producer and consumer endpoints must be quiescent. */
            void Reset() noexcept {
                const std::uint64_t end = LoadProducerPosition(std::memory_order_acquire);
                if constexpr (kMultipleConsumers) {
                    consumerClaimPosition_.store(end, std::memory_order_relaxed);
                }
                StoreConsumerReclaimPosition(end, std::memory_order_release);
            }

            /** @brief Return whether one concurrent cursor snapshot observes no reserved or retained arena bytes. */
            [[nodiscard]] bool Empty() const noexcept { return BytesPending() == 0; }

            /** @brief Return an approximate occupied-byte count, including headers and wrap padding. */
            [[nodiscard]] std::size_t BytesPending() const noexcept {
                const std::uint64_t reclaimed = LoadConsumerReclaimPosition(std::memory_order_acquire);
                const std::uint64_t produced = LoadProducerPosition(std::memory_order_acquire);
                return std::min<std::uint64_t>(produced - reclaimed, ByteCapacity);
            }

            [[nodiscard]] static consteval std::size_t GetByteCapacity() noexcept { return ByteCapacity; }
            [[nodiscard]] static consteval std::size_t GetMaxRecordBytes() noexcept { return kMaxRecordBytes; }

        private:
            [[nodiscard]] std::optional<Reservation> TryReserve(std::size_t recordBytes) noexcept {
                if constexpr (kMultipleProducers) {
                    return TryReserveMultiple(recordBytes);
                } else {
                    std::uint64_t position = LoadProducerPosition(std::memory_order_relaxed);
                    const std::size_t index = static_cast<std::size_t>(position) & kMask;
                    const std::size_t padding = recordBytes > ByteCapacity - index ? ByteCapacity - index : 0;
                    const std::size_t required = padding + recordBytes;
                    if (!HasSingleProducerCapacity(position, required)) {
                        return std::nullopt;
                    }
                    StoreProducerPosition(position + required, std::memory_order_relaxed);
                    return Reservation{position, index, padding};
                }
            }

            [[nodiscard]] std::optional<Reservation> TryReserveMultiple(std::size_t recordBytes) noexcept
                requires kMultipleProducers
            {
                std::uint64_t reclaimed = cachedReclaimPosition_.load(std::memory_order_relaxed);
                std::uint64_t position = producerPosition_.load(std::memory_order_relaxed);
                for (;;) {
                    const std::size_t index = static_cast<std::size_t>(position) & kMask;
                    const std::size_t padding = recordBytes > ByteCapacity - index ? ByteCapacity - index : 0;
                    const std::size_t required = padding + recordBytes;
                    if (!HasCapacity(position, reclaimed, required)) {
                        reclaimed = consumerReclaimPosition_.load(std::memory_order_acquire);
                        if (!HasCapacity(position, reclaimed, required)) {
                            return std::nullopt;
                        }
                        cachedReclaimPosition_.store(reclaimed, std::memory_order_relaxed);
                    }

                    if (producerPosition_.compare_exchange_weak(position, position + required,
                                                                std::memory_order_relaxed)) {
                        return Reservation{position, index, padding};
                    }
                }
            }

            [[nodiscard]] bool HasSingleProducerCapacity(std::uint64_t position, std::size_t required) noexcept {
                if constexpr (kLocal) {
                    return HasCapacity(position, consumerReclaimPosition_, required);
                } else {
                    if (HasCapacity(position, cachedReclaimPosition_, required)) {
                        return true;
                    }
                    cachedReclaimPosition_ = consumerReclaimPosition_.load(std::memory_order_acquire);
                    return HasCapacity(position, cachedReclaimPosition_, required);
                }
            }

            [[nodiscard]] static bool HasCapacity(std::uint64_t produced, std::uint64_t reclaimed,
                                                  std::size_t required) noexcept {
                const std::uint64_t occupied = produced - reclaimed;
                return occupied <= ByteCapacity && required <= ByteCapacity - occupied;
            }

            template<class Consumer>
            [[nodiscard]] bool TryConsumeSingle(Consumer&& consumer)
                noexcept(std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>) {
                std::uint64_t position = LoadConsumerReclaimPosition(std::memory_order_relaxed);
                for (;;) {
                    const std::size_t index = static_cast<std::size_t>(position) & kMask;
                    if (!IsCommitted(index, position)) {
                        return false;
                    }

                    const std::uint32_t payloadBytes = ReadMetadata(index);
                    const std::uint64_t next = NextPosition(position, index, payloadBytes);
                    if (next == position) [[unlikely]] {
                        return false;
                    }
                    if (payloadBytes == kPaddingRecord) {
                        position = next;
                        StoreConsumerReclaimPosition(position, std::memory_order_release);
                        continue;
                    }

                    struct ReclaimGuard {
                        RecordRingBufferImplementation& ring;
                        std::uint64_t next;
                        ~ReclaimGuard() { ring.StoreConsumerReclaimPosition(next, std::memory_order_release); }
                    } reclaim{*this, next};
                    std::forward<Consumer>(consumer)(
                        std::span<const std::byte>{Bytes() + index + kRecordHeaderBytes, payloadBytes});
                    return true;
                }
            }

            template<class Consumer>
            [[nodiscard]] bool TryConsumeMultiple(Consumer&& consumer)
                noexcept(std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>)
                requires kMultipleConsumers
            {
                std::uint64_t position = consumerClaimPosition_.load(std::memory_order_relaxed);
                for (;;) {
                    const std::size_t index = static_cast<std::size_t>(position) & kMask;
                    if (!IsCommitted(index, position)) {
                        return false;
                    }

                    const std::uint32_t payloadBytes = ReadMetadata(index);
                    const std::uint64_t next = NextPosition(position, index, payloadBytes);
                    if (next == position) [[unlikely]] {
                        return false;
                    }
                    if (!consumerClaimPosition_.compare_exchange_weak(position, next,
                                                                       std::memory_order_relaxed)) {
                        continue;
                    }

                    if (payloadBytes == kPaddingRecord) {
                        Complete(index, position);
                        AdvanceReclaimPosition(position);
                        position = consumerClaimPosition_.load(std::memory_order_relaxed);
                        continue;
                    }

                    struct CompletionGuard {
                        RecordRingBufferImplementation& ring;
                        std::size_t index;
                        std::uint64_t position;
                        ~CompletionGuard() {
                            ring.Complete(index, position);
                            ring.AdvanceReclaimPosition(position);
                        }
                    } completion{*this, index, position};
                    std::forward<Consumer>(consumer)(
                        std::span<const std::byte>{Bytes() + index + kRecordHeaderBytes, payloadBytes});
                    return true;
                }
            }

            /**
             * @brief Reclaim the longest contiguous prefix beginning at @p completedPosition.
             * @details An out-of-order completer contributes an acquire-release no-op RMW to the reclaim cursor.
             * The eventual successful cursor CAS must read the latest preceding RMW, importing every completion
             * release that the RMW chain carries before scanning the next header. This prevents a stale completed-
             * header read from losing the only handoff that could resume reclamation.
             */
            void AdvanceReclaimPosition(std::uint64_t completedPosition) noexcept
                requires kMultipleConsumers
            {
                std::uint64_t position = consumerReclaimPosition_.load(std::memory_order_relaxed);
                if (position != completedPosition) {
                    position = consumerReclaimPosition_.fetch_add(0, std::memory_order_acq_rel);
                    if (position != completedPosition) {
                        return;
                    }
                }
                for (;;) {
                    const std::size_t index = static_cast<std::size_t>(position) & kMask;
                    if (!IsCompleted(index, position)) {
                        return;
                    }
                    const std::uint64_t next = NextPosition(position, index, ReadMetadata(index));
                    if (next == position) [[unlikely]] {
                        return;
                    }
                    if (!consumerReclaimPosition_.compare_exchange_weak(position, next,
                                                                         std::memory_order_acq_rel,
                                                                         std::memory_order_relaxed)) {
                        continue;
                    }
                    position = next;
                }
            }

            [[nodiscard]] static std::uint64_t NextPosition(std::uint64_t position, std::size_t index,
                                                            std::uint32_t payloadBytes) noexcept {
                if (payloadBytes == kPaddingRecord) {
                    return position + ByteCapacity - index;
                }
                if (payloadBytes == 0 || payloadBytes > kMaxRecordBytes) [[unlikely]] {
                    return position;
                }
                return position + AlignRecordBytes(kRecordHeaderBytes + payloadBytes);
            }

            [[nodiscard]] std::uint64_t LoadProducerPosition(std::memory_order order) const noexcept {
                if constexpr (kLocal) {
                    return producerPosition_;
                } else {
                    return producerPosition_.load(order);
                }
            }

            void StoreProducerPosition(std::uint64_t position, std::memory_order order) noexcept {
                if constexpr (kLocal) {
                    producerPosition_ = position;
                } else {
                    producerPosition_.store(position, order);
                }
            }

            [[nodiscard]] std::uint64_t LoadConsumerClaimPosition() const noexcept {
                if constexpr (kMultipleConsumers) {
                    return consumerClaimPosition_.load(std::memory_order_relaxed);
                } else {
                    return LoadConsumerReclaimPosition(std::memory_order_relaxed);
                }
            }

            [[nodiscard]] std::uint64_t LoadConsumerReclaimPosition(std::memory_order order) const noexcept {
                if constexpr (kLocal) {
                    return consumerReclaimPosition_;
                } else {
                    return consumerReclaimPosition_.load(order);
                }
            }

            void StoreConsumerReclaimPosition(std::uint64_t position, std::memory_order order) noexcept {
                if constexpr (kLocal) {
                    consumerReclaimPosition_ = position;
                } else {
                    consumerReclaimPosition_.store(position, order);
                }
            }

            [[nodiscard]] std::atomic_ref<std::uint64_t> Sequence(std::size_t index) noexcept {
                return std::atomic_ref<std::uint64_t>{words_[index / sizeof(std::uint64_t)]};
            }

            void Publish(std::size_t index, std::uint64_t position) noexcept {
                if constexpr (kLocal) {
                    words_[index / sizeof(std::uint64_t)] = position | kCommitted;
                } else {
                    Sequence(index).store(position | kCommitted, std::memory_order_release);
                }
            }

            [[nodiscard]] bool IsCommitted(std::size_t index, std::uint64_t position) noexcept {
                if constexpr (kLocal) {
                    return words_[index / sizeof(std::uint64_t)] == (position | kCommitted);
                } else {
                    return Sequence(index).load(std::memory_order_acquire) == (position | kCommitted);
                }
            }

            [[nodiscard]] bool IsCompleted(std::size_t index, std::uint64_t position) noexcept {
                return Sequence(index).load(std::memory_order_acquire) == (position | kCompleted);
            }

            void Complete(std::size_t index, std::uint64_t position) noexcept {
                Sequence(index).store(position | kCompleted, std::memory_order_release);
            }

            void WriteMetadata(std::size_t index, std::uint32_t payloadBytes) noexcept {
                words_[index / sizeof(std::uint64_t) + 1] = payloadBytes;
            }

            [[nodiscard]] std::uint32_t ReadMetadata(std::size_t index) const noexcept {
                return static_cast<std::uint32_t>(words_[index / sizeof(std::uint64_t) + 1]);
            }

            [[nodiscard]] std::byte* Bytes() noexcept { return reinterpret_cast<std::byte*>(words_.data()); }

            [[= ProducerDomain{}]] alignas(kPositionAlignment) ProducerPosition producerPosition_{0};
            [[= CachedReclaimDomain{}]] MASHIRO_NO_UNIQUE_ADDRESS CachedReclaimPosition cachedReclaimPosition_{};
            [[= ConsumerClaimDomain{}]] MASHIRO_NO_UNIQUE_ADDRESS alignas(kPositionAlignment)
                ConsumerClaimPosition consumerClaimPosition_{};
            [[= ConsumerReclaimDomain{}]] alignas(kPositionAlignment)
                ConsumerReclaimPosition consumerReclaimPosition_{0};
            [[= Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize)
                std::array<std::uint64_t, kWordCount> words_{};
        };

    } // namespace Detail

    /**
     * @brief Bounded packed variable-length record ring selected by compile-time endpoint topology.
     * @tparam ByteCapacity Power-of-two byte capacity of the inline arena.
     * @tparam Topology Producer/consumer cardinality and sharing domain.
     */
    template<std::size_t ByteCapacity,
             Concurrency::QueueTopology Topology = Concurrency::QueueTopologies::Spsc>
    using RecordRingBuffer = Detail::RecordRingBufferImplementation<ByteCapacity, Topology>;

    /** @cond INTERNAL */
    consteval {
        using Concurrency::AuditFalseSharing;
        using Concurrency::QueueTopologies::Mpmc;
        using Concurrency::QueueTopologies::Mpsc;
        using Concurrency::QueueTopologies::Spmc;
        using Concurrency::QueueTopologies::Spsc;

        static_assert(AuditFalseSharing<RecordRingBuffer<4096, Spsc>>());
        static_assert(AuditFalseSharing<RecordRingBuffer<4096, Mpsc>>());
        static_assert(AuditFalseSharing<RecordRingBuffer<4096, Spmc>>());
        static_assert(AuditFalseSharing<RecordRingBuffer<4096, Mpmc>>());
    }
    /** @endcond */

} // namespace Mashiro
