/**
 * @file RecordRingBuffer.h
 * @brief Bounded packed-record ring buffers specialized by compile-time producer and consumer topology.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/Concurrency/ContentionDomain.h"
#include "Sora/Core/Concurrency/FalseSharingChecker.h"
#include "Sora/Core/Concurrency/QueueTopology.h"
#include "Sora/Core/Memory/MemoryLayout.h"
#include "Sora/Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    namespace Concept {

        /** @brief Determine whether @p ByteCapacity can support the packed-record generation protocol. */
        template<size_t ByteCapacity>
        concept ValidRecordRingBufferCapacity = ByteCapacity >= 128 && std::has_single_bit(ByteCapacity) &&
                                                ByteCapacity <= std::numeric_limits<uint32_t>::max();

        /** @brief Determine whether @p Topology belongs to the supported endpoint topology family. */
        template<QueueTopology Topology>
        concept ValidRecordRingBufferTopology = Topology.IsValid();

    } // namespace Concept

    namespace Detail {

        inline constexpr uint32_t kPaddingRecord = std::numeric_limits<uint32_t>::max();

        /** @brief Physical roles used to give every compile-time-erased cursor a distinct empty type. */
        enum class RecordRingBufferField : uint8_t {
            CachedReclaimPosition,
            ConsumerClaimPosition,
        };

        template<RecordRingBufferField Field>
        using EmptyRecordRingBufferField = std::integral_constant<RecordRingBufferField, Field>;

        struct RecordRingSingleProducerDomain : $::CohesiveSingleWriterContentionDomain {};
        struct RecordRingMultiProducerDomain : $::CohesiveMultiWriterContentionDomain {};

        /** @brief Interpret an unsigned position subtraction as its two's-complement signed difference. */
        [[nodiscard]] constexpr int64_t PositionDifference(uint64_t lhs, uint64_t rhs) noexcept {
            return std::bit_cast<int64_t>(lhs - rhs);
        }

        /**
         * @brief One packed-record ring whose synchronization protocol is generated from @p Topology.
         * @details Records remain contiguous by consuming the physical tail as an explicit padding record.
         * Local variants publish their contiguous prefix with one cursor store. Concurrent variants publish each
         * record through a generation token, keeping consumers off the producer-owned reservation cursor during the
         * per-record hot path. Multi-producer variants can temporarily expose a FIFO publication hole when a producer
         * is preempted. Multi-consumer variants separately claim records and reclaim completed byte ranges, preventing
         * producers from overwriting payloads while callbacks still access them.
         */
        template<size_t ByteCapacity, QueueTopology Topology>
            requires Concept::ValidRecordRingBufferCapacity<ByteCapacity> &&
                     Concept::ValidRecordRingBufferTopology<Topology>
        class RecordRingBufferImpl {
            static constexpr bool kLocal = Topology.sharing == QueueSharing::SingleThread;
            static constexpr bool kMultipleProducers = Topology.producers == QueueCardinality::Multiple;
            static constexpr bool kMultipleConsumers = Topology.consumers == QueueCardinality::Multiple;
            static constexpr Align kRecordAlignment =
                kLocal ? Align::Of<uint32_t>() : Align::Constant<2 * alignof(uint64_t)>();
            static constexpr size_t kRecordHeaderBytes = !kLocal ? 16 : sizeof(uint32_t);
            static constexpr size_t kMask = ByteCapacity - 1;
            static constexpr size_t kWordCount = ByteCapacity / sizeof(uint64_t);
            static constexpr uint64_t kCommitted = 1;
            static constexpr uint64_t kCompleted = 3;
            static constexpr size_t kPositionAlignment = kLocal ? alignof(uint64_t) : Platform::kCacheLineSize;

            using ProducerPosition = std::conditional_t<kLocal, uint64_t, std::atomic<uint64_t>>;
            using CachedReclaimPosition =
                std::conditional_t<kLocal, EmptyRecordRingBufferField<RecordRingBufferField::CachedReclaimPosition>,
                                   std::conditional_t<kMultipleProducers, std::atomic<uint64_t>, uint64_t>>;
            using ConsumerClaimPosition =
                std::conditional_t<kMultipleConsumers, std::atomic<uint64_t>,
                                   EmptyRecordRingBufferField<RecordRingBufferField::ConsumerClaimPosition>>;
            using ConsumerReclaimPosition = std::conditional_t<kLocal, uint64_t, std::atomic<uint64_t>>;

            using ProducerDomain = std::conditional_t<
                kLocal, $::Inactive,
                std::conditional_t<kMultipleProducers, RecordRingMultiProducerDomain, RecordRingSingleProducerDomain>>;
            using CachedReclaimDomain = ProducerDomain;
            using ConsumerClaimDomain = std::conditional_t<kMultipleConsumers, $::Contended, $::Inactive>;
            using ConsumerReclaimDomain =
                std::conditional_t<kLocal, $::Inactive,
                                   std::conditional_t<kMultipleConsumers, $::Contended, $::ConsumerOwned>>;
            using StorageDomain = std::conditional_t<kLocal, $::Inactive, $::SharedStorage>;

            struct Reservation {
                uint64_t position;
                size_t index;
                size_t padding;
            };

            static_assert(kLocal || std::atomic<uint64_t>::is_always_lock_free,
                          "concurrent RecordRingBuffer requires lock-free 64-bit atomics");

        public:
            static constexpr size_t kByteCapacity = ByteCapacity;
            // A record occupying at most half the arena can always follow wrap padding from any aligned position.
            static constexpr size_t kMaxPayloadBytes = ByteCapacity / 2 - kRecordHeaderBytes;
            static constexpr QueueTopology topology = Topology;
            static constexpr QueueOrder order = QueueOrder::Fifo;

            RecordRingBufferImpl() = default;
            RecordRingBufferImpl(const RecordRingBufferImpl&) = delete;
            RecordRingBufferImpl& operator=(const RecordRingBufferImpl&) = delete;
            RecordRingBufferImpl(RecordRingBufferImpl&&) = delete;
            RecordRingBufferImpl& operator=(RecordRingBufferImpl&&) = delete;

            /** @brief Try to publish one non-empty record without allocation or blocking. */
            [[nodiscard]] bool TryWrite(std::span<const std::byte> record) noexcept {
                if (record.empty() || record.size_bytes() > kMaxPayloadBytes) [[unlikely]] {
                    return false;
                }

                const size_t recordBytes = AlignRecordBytes(kRecordHeaderBytes + record.size_bytes());
                const std::optional<Reservation> reservation = TryReserve(recordBytes);
                if (!reservation) {
                    return false;
                }

                const size_t recordIndex = reservation->padding == 0 ? reservation->index : 0;
                WriteMetadata(recordIndex, static_cast<uint32_t>(record.size_bytes()));
                std::memcpy(Bytes() + recordIndex + kRecordHeaderBytes, record.data(), record.size_bytes());

                if constexpr (!kLocal) {
                    Publish(recordIndex, reservation->position + reservation->padding);
                    if (reservation->padding != 0) {
                        WriteMetadata(reservation->index, kPaddingRecord);
                        Publish(reservation->index, reservation->position);
                    }
                } else {
                    if (reservation->padding != 0) {
                        WriteMetadata(reservation->index, kPaddingRecord);
                    }
                    StoreProducerPosition(reservation->position + reservation->padding + recordBytes,
                                          std::memory_order_release);
                }
                return true;
            }

            /** @brief Convenience overload for a raw pointer and byte count. */
            [[nodiscard]] bool TryWrite(const void* data, size_t size) noexcept {
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
            [[nodiscard]] bool TryConsume(Consumer&& consumer) noexcept(
                std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>) {
                if constexpr (kMultipleConsumers) {
                    return TryConsumeMultiple(std::forward<Consumer>(consumer));
                } else {
                    return TryConsumeSingle(std::forward<Consumer>(consumer));
                }
            }

            /** @brief Drain records reserved before function entry until a publication hole or snapshot end. */
            template<class Consumer>
                requires std::invocable<Consumer&, std::span<const std::byte>>
            size_t
            Drain(Consumer&& consumer) noexcept(std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>) {
                const uint64_t end = LoadProducerPosition(std::memory_order_acquire);
                if constexpr (!kMultipleConsumers &&
                              std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>) {
                    return DrainSingleNoexcept(consumer, end);
                }
                size_t count = 0;
                while (PositionDifference(LoadConsumerClaimPosition(), end) < 0) {
                    if (!TryConsume(consumer)) [[unlikely]] {
                        break;
                    }
                    ++count;
                }
                return count;
            }

            /** @brief Discard occupied records after every producer and consumer endpoint is quiescent. */
            void Reset() noexcept {
                const uint64_t end = LoadProducerPosition(std::memory_order_acquire);
                if constexpr (kMultipleConsumers) {
                    consumerClaimPosition_.store(end, std::memory_order_relaxed);
                }
                StoreConsumerReclaimPosition(end, std::memory_order_release);
            }

            /** @brief Return whether one concurrent cursor snapshot observes no reserved or retained arena bytes. */
            [[nodiscard]] bool IsEmpty() const noexcept { return OccupiedBytesApprox() == 0; }

            /** @brief Return an approximate occupied-byte count, including headers and wrap padding. */
            [[nodiscard]] size_t OccupiedBytesApprox() const noexcept {
                const uint64_t reclaimed = LoadConsumerReclaimPosition(std::memory_order_acquire);
                const uint64_t produced = LoadProducerPosition(std::memory_order_acquire);
                return static_cast<size_t>(std::min<uint64_t>(produced - reclaimed, ByteCapacity));
            }

            /** @brief Return the translation-time physical-layout report for this specialization. */
            [[nodiscard]] static consteval Meta::CacheLayoutReport Layout() {
                return Meta::AnalyzeCacheLayout<RecordRingBufferImpl>();
            }

        private:
            /** @brief Round one byte count up to this topology's physical record alignment. */
            [[nodiscard]] static constexpr size_t AlignRecordBytes(size_t bytes) noexcept {
                return static_cast<size_t>(Sora::AlignUpModulo(bytes, kRecordAlignment));
            }

            [[nodiscard]] std::optional<Reservation> TryReserve(size_t recordBytes) noexcept {
                if constexpr (kMultipleProducers) {
                    return TryReserveMultiple(recordBytes);
                } else {
                    const uint64_t position = LoadProducerPosition(std::memory_order_relaxed);
                    const size_t index = static_cast<size_t>(position) & kMask;
                    const size_t padding = recordBytes > ByteCapacity - index ? ByteCapacity - index : 0;
                    const size_t required = padding + recordBytes;
                    if (!HasSingleProducerCapacity(position, required)) {
                        return std::nullopt;
                    }
                    if constexpr (!kLocal) {
                        StoreProducerPosition(position + required, std::memory_order_relaxed);
                    }
                    return Reservation{position, index, padding};
                }
            }

            [[nodiscard]] std::optional<Reservation> TryReserveMultiple(size_t recordBytes) noexcept
                requires kMultipleProducers
            {
                uint64_t reclaimed = cachedReclaimPosition_.load(std::memory_order_relaxed);
                uint64_t position = producerPosition_.load(std::memory_order_relaxed);
                for (;;) {
                    const size_t index = static_cast<size_t>(position) & kMask;
                    const size_t padding = recordBytes > ByteCapacity - index ? ByteCapacity - index : 0;
                    const size_t required = padding + recordBytes;
                    if (!HasCapacity(position, reclaimed, required)) {
                        reclaimed = consumerReclaimPosition_.load(std::memory_order_acquire);
                        if (!HasCapacity(position, reclaimed, required)) {
                            return std::nullopt;
                        }
                        cachedReclaimPosition_.store(reclaimed, std::memory_order_relaxed);
                    }

                    if (producerPosition_.compare_exchange_weak(position, position + required,
                                                                std::memory_order_relaxed, std::memory_order_relaxed)) {
                        return Reservation{position, index, padding};
                    }
                }
            }

            [[nodiscard]] bool HasSingleProducerCapacity(uint64_t position, size_t required) noexcept {
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

            [[nodiscard]] static bool HasCapacity(uint64_t produced, uint64_t reclaimed, size_t required) noexcept {
                const uint64_t occupied = produced - reclaimed;
                return occupied <= ByteCapacity && required <= ByteCapacity - occupied;
            }

            template<class Consumer>
            [[nodiscard]] bool TryConsumeSingle(Consumer&& consumer) noexcept(
                std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>) {
                uint64_t position = LoadConsumerReclaimPosition(std::memory_order_relaxed);
                for (;;) {
                    const size_t index = static_cast<size_t>(position) & kMask;
                    if (!IsPublished(index, position)) {
                        return false;
                    }

                    const uint32_t payloadBytes = ReadMetadata(index);
                    const uint64_t next = NextPosition(position, index, payloadBytes);
                    if (next == position) [[unlikely]] {
                        return false;
                    }
                    if (payloadBytes == kPaddingRecord) {
                        position = next;
                        StoreConsumerReclaimPosition(position, std::memory_order_release);
                        continue;
                    }

                    struct ReclaimGuard {
                        RecordRingBufferImpl& ring;
                        uint64_t next;
                        ~ReclaimGuard() { ring.StoreConsumerReclaimPosition(next, std::memory_order_release); }
                    } reclaim{*this, next};
                    std::invoke(std::forward<Consumer>(consumer),
                                std::span<const std::byte>{Bytes() + index + kRecordHeaderBytes, payloadBytes});
                    return true;
                }
            }

            template<class Consumer>
            [[nodiscard]] bool TryConsumeMultiple(Consumer&& consumer) noexcept(
                std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>)
                requires kMultipleConsumers
            {
                uint64_t position = consumerClaimPosition_.load(std::memory_order_relaxed);
                for (;;) {
                    const size_t index = static_cast<size_t>(position) & kMask;
                    if (!IsPublished(index, position)) {
                        return false;
                    }

                    const uint32_t payloadBytes = ReadMetadata(index);
                    const uint64_t next = NextPosition(position, index, payloadBytes);
                    if (next == position) [[unlikely]] {
                        return false;
                    }
                    if (!consumerClaimPosition_.compare_exchange_weak(position, next, std::memory_order_relaxed,
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
                        RecordRingBufferImpl& ring;
                        size_t index;
                        uint64_t position;
                        ~CompletionGuard() {
                            ring.Complete(index, position);
                            ring.AdvanceReclaimPosition(position);
                        }
                    } completion{*this, index, position};
                    std::invoke(std::forward<Consumer>(consumer),
                                std::span<const std::byte>{Bytes() + index + kRecordHeaderBytes, payloadBytes});
                    return true;
                }
            }

            /** @brief Drain one single-consumer snapshot and publish the reclaimed prefix once. */
            template<class Consumer>
            [[nodiscard]] size_t DrainSingleNoexcept(Consumer& consumer, uint64_t end) noexcept
                requires(!kMultipleConsumers && std::is_nothrow_invocable_v<Consumer&, std::span<const std::byte>>)
            {
                const uint64_t begin = LoadConsumerReclaimPosition(std::memory_order_relaxed);
                uint64_t position = begin;
                size_t count = 0;
                while (PositionDifference(position, end) < 0) {
                    const size_t index = static_cast<size_t>(position) & kMask;
                    if constexpr (!kLocal) {
                        if (!IsCommitted(index, position)) {
                            break;
                        }
                    }

                    const uint32_t payloadBytes = ReadMetadata(index);
                    const uint64_t next = NextPosition(position, index, payloadBytes);
                    if (next == position) [[unlikely]] {
                        break;
                    }
                    position = next;
                    if (payloadBytes == kPaddingRecord) {
                        continue;
                    }

                    std::invoke(consumer,
                                std::span<const std::byte>{Bytes() + index + kRecordHeaderBytes, payloadBytes});
                    ++count;
                }
                if (position != begin) {
                    StoreConsumerReclaimPosition(position, std::memory_order_release);
                }
                return count;
            }

            /**
             * @brief Reclaim the longest contiguous prefix beginning at @p completedPosition.
             * @details An out-of-order completer contributes an acquire-release no-op RMW to the reclaim cursor.
             * The eventual successful cursor CAS must read the latest preceding RMW, importing every completion
             * release that the RMW chain carries before scanning the next header. This prevents a stale completed
             * header read from losing the only handoff that could resume reclamation.
             */
            void AdvanceReclaimPosition(uint64_t completedPosition) noexcept
                requires kMultipleConsumers
            {
                uint64_t position = consumerReclaimPosition_.load(std::memory_order_relaxed);
                if (position != completedPosition) {
                    position = consumerReclaimPosition_.fetch_add(0, std::memory_order_acq_rel);
                    if (position != completedPosition) {
                        return;
                    }
                }
                for (;;) {
                    const size_t index = static_cast<size_t>(position) & kMask;
                    if (!IsCompleted(index, position)) {
                        return;
                    }
                    const uint64_t next = NextPosition(position, index, ReadMetadata(index));
                    if (next == position) [[unlikely]] {
                        return;
                    }
                    if (!consumerReclaimPosition_.compare_exchange_weak(position, next, std::memory_order_acq_rel,
                                                                        std::memory_order_relaxed)) {
                        continue;
                    }
                    position = next;
                }
            }

            [[nodiscard]] static uint64_t NextPosition(uint64_t position, size_t index,
                                                       uint32_t payloadBytes) noexcept {
                if (payloadBytes == kPaddingRecord) {
                    return position + ByteCapacity - index;
                }
                if (payloadBytes == 0 || payloadBytes > kMaxPayloadBytes) [[unlikely]] {
                    return position;
                }
                return position + AlignRecordBytes(kRecordHeaderBytes + payloadBytes);
            }

            [[nodiscard]] uint64_t LoadProducerPosition(std::memory_order order) const noexcept {
                if constexpr (kLocal) {
                    return producerPosition_;
                } else {
                    return producerPosition_.load(order);
                }
            }

            void StoreProducerPosition(uint64_t position, std::memory_order order) noexcept {
                if constexpr (kLocal) {
                    producerPosition_ = position;
                } else {
                    producerPosition_.store(position, order);
                }
            }

            [[nodiscard]] uint64_t LoadConsumerClaimPosition() const noexcept {
                if constexpr (kMultipleConsumers) {
                    return consumerClaimPosition_.load(std::memory_order_relaxed);
                } else {
                    return LoadConsumerReclaimPosition(std::memory_order_relaxed);
                }
            }

            [[nodiscard]] uint64_t LoadConsumerReclaimPosition(std::memory_order order) const noexcept {
                if constexpr (kLocal) {
                    return consumerReclaimPosition_;
                } else {
                    return consumerReclaimPosition_.load(order);
                }
            }

            void StoreConsumerReclaimPosition(uint64_t position, std::memory_order order) noexcept {
                if constexpr (kLocal) {
                    consumerReclaimPosition_ = position;
                } else {
                    consumerReclaimPosition_.store(position, order);
                }
            }

            [[nodiscard]] std::atomic_ref<uint64_t> Sequence(size_t index) noexcept {
                return std::atomic_ref<uint64_t>{words_[index / sizeof(uint64_t)]};
            }

            void Publish(size_t index, uint64_t position) noexcept {
                Sequence(index).store(position | kCommitted, std::memory_order_release);
            }

            [[nodiscard]] bool IsCommitted(size_t index, uint64_t position) noexcept {
                return Sequence(index).load(std::memory_order_acquire) == (position | kCommitted);
            }

            [[nodiscard]] bool IsPublished(size_t index, uint64_t position) noexcept {
                if constexpr (!kLocal) {
                    return IsCommitted(index, position);
                } else {
                    return PositionDifference(position, LoadProducerPosition(std::memory_order_acquire)) < 0;
                }
            }

            [[nodiscard]] bool IsCompleted(size_t index, uint64_t position) noexcept {
                return Sequence(index).load(std::memory_order_acquire) == (position | kCompleted);
            }

            void Complete(size_t index, uint64_t position) noexcept {
                Sequence(index).store(position | kCompleted, std::memory_order_release);
            }

            void WriteMetadata(size_t index, uint32_t payloadBytes) noexcept {
                if constexpr (!kLocal) {
                    words_[index / sizeof(uint64_t) + 1] = payloadBytes;
                } else {
                    std::memcpy(Bytes() + index, &payloadBytes, sizeof(payloadBytes));
                }
            }

            [[nodiscard]] uint32_t ReadMetadata(size_t index) const noexcept {
                if constexpr (!kLocal) {
                    return static_cast<uint32_t>(words_[index / sizeof(uint64_t) + 1]);
                } else {
                    uint32_t payloadBytes = 0;
                    std::memcpy(&payloadBytes, Bytes() + index, sizeof(payloadBytes));
                    return payloadBytes;
                }
            }

            [[nodiscard]] std::byte* Bytes() noexcept { return reinterpret_cast<std::byte*>(words_.data()); }
            [[nodiscard]] const std::byte* Bytes() const noexcept {
                return reinterpret_cast<const std::byte*>(words_.data());
            }

            // clang-format off
            [[= ProducerDomain{}]] [[= $::CohesiveContentionRegion{}]]
                alignas(kPositionAlignment) ProducerPosition producerPosition_{0};
            [[= CachedReclaimDomain{}]] [[= $::CohesiveContentionRegion{}]]
                SORA_NO_UNIQUE_ADDRESS CachedReclaimPosition cachedReclaimPosition_{};
            [[= ConsumerClaimDomain{}]] [[= $::CohesiveContentionRegion{}]]
                SORA_NO_UNIQUE_ADDRESS alignas(kPositionAlignment)
                ConsumerClaimPosition consumerClaimPosition_{};
            [[= ConsumerReclaimDomain{}]] [[= $::CohesiveContentionRegion{}]]
                alignas(kPositionAlignment)
                ConsumerReclaimPosition consumerReclaimPosition_{0};
            [[= StorageDomain{}]] alignas(Platform::kCacheLineSize) std::array<uint64_t, kWordCount> words_{};
            // clang-format on
        };

    } // namespace Detail

    /**
     * @brief Bounded packed variable-length record ring selected by compile-time endpoint topology.
     * @tparam ByteCapacity Power-of-two byte capacity of the inline arena.
     * @tparam Topology Producer/consumer cardinality and sharing domain.
     */
    template<size_t ByteCapacity, QueueTopology Topology = QueueTopologies::Spsc>
    using RecordRingBuffer = Detail::RecordRingBufferImpl<ByteCapacity, Topology>;

    namespace Concept {

        /** @brief Storage capability implemented by Sora's packed variable-length FIFO rings. */
        template<class Storage>
        concept RecordRingBufferStorage = requires {
            { Storage::kByteCapacity } -> std::convertible_to<std::size_t>;
            { Storage::kMaxPayloadBytes } -> std::convertible_to<std::size_t>;
            { Storage::topology } -> std::same_as<const QueueTopology&>;
            { Storage::order } -> std::same_as<const QueueOrder&>;
            requires Storage::order == QueueOrder::Fifo;
        };

    } // namespace Concept

    /** @cond INTERNAL */
    consteval {
        static_assert(Meta::AuditDirectMemberFalseSharing<RecordRingBuffer<4096, QueueTopologies::Spsc>>());
        static_assert(Meta::AuditDirectMemberFalseSharing<RecordRingBuffer<4096, QueueTopologies::Mpsc>>());
        static_assert(Meta::AuditDirectMemberFalseSharing<RecordRingBuffer<4096, QueueTopologies::Spmc>>());
        static_assert(Meta::AuditDirectMemberFalseSharing<RecordRingBuffer<4096, QueueTopologies::Mpmc>>());
    }
    /** @endcond */

} // namespace Sora::Concurrency
