/**
 * @file QueueAdapter.h
 * @brief Explicit capability model connecting specialized queue storage to shared asynchronous endpoints.
 * @ingroup Concurrency
 */
#pragma once

#include "Mashiro/Concurrency/BoundedByteRecord.h"
#include "Mashiro/Concurrency/QueueBatch.h"
#include "Mashiro/Core/BoundedStack.h"
#include "Mashiro/Core/RecordRingBuffer.h"
#include "Mashiro/Core/RingBuffer.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace Mashiro::Concurrency {

    /**
     * @brief Strongest formally claimed progress guarantee for an eligible storage transfer.
     * @details A prompt `try_*` failure is an API execution property, not evidence of lock-free progress. In
     * particular, a reservation-based FIFO may return promptly while a descheduled reservation owner prevents every
     * useful transfer at the queue head. Sender suspension and backpressure policy are separate properties again.
     */
    enum class QueueProgress : std::uint8_t {
        WaitFree,        /**< Every participating operation completes in a bounded number of its own steps. */
        LockFree,        /**< The system continually completes some operation, though one participant may starve. */
        ObstructionFree, /**< An operation completes when it eventually executes without interference. */
        Unspecified,     /**< No formal progress theorem is claimed beyond the documented API behavior. */
    };

    /** @brief Result of one non-waiting adapter transfer attempt. */
    enum class QueueTransferStatus : std::uint8_t {
        Committed,     /**< The input was transferred and ownership effects are committed. */
        BackPressure,  /**< Transient capacity or ordering pressure prevented a transfer. */
        BatchTooLarge, /**< A batch cannot fit the adapter's fixed element domain. */
        InvalidValue,  /**< The value is outside the storage's permanent value domain. */
    };

    /** @brief Primary adapter declaration. Unsupported storage has no accidental fallback. */
    template<class Storage>
    struct QueueAdapter;

    /** @brief Common storage contract required by @ref AsyncQueue. */
    template<class Adapter>
    concept AsyncQueueAdapter =
        requires(typename Adapter::storage_type& storage, const typename Adapter::storage_type& constStorage,
                 typename Adapter::value_type& value) {
            typename Adapter::storage_type;
            typename Adapter::value_type;
            { Adapter::order } -> std::same_as<const QueueOrder&>;
            { Adapter::producerCardinality } -> std::same_as<const QueueCardinality&>;
            { Adapter::consumerCardinality } -> std::same_as<const QueueCardinality&>;
            { Adapter::pushProgress } -> std::same_as<const QueueProgress&>;
            { Adapter::popProgress } -> std::same_as<const QueueProgress&>;
            { Adapter::pushFailurePreservesInput } -> std::same_as<const bool&>;
            requires Adapter::pushFailurePreservesInput;
            { Adapter::TryPush(storage, value) } noexcept -> std::same_as<QueueTransferStatus>;
            { Adapter::TryPop(storage) } noexcept -> std::same_as<std::optional<typename Adapter::value_type>>;
            { Adapter::IsDrained(constStorage) } noexcept -> std::same_as<bool>;
            requires std::is_nothrow_move_constructible_v<typename Adapter::value_type>;
            requires std::is_nothrow_destructible_v<typename Adapter::value_type>;
        };

    /** @brief Adapter additionally exposing a transient element-count estimate. */
    template<class Adapter>
    concept ApproximateSizedQueueAdapter =
        AsyncQueueAdapter<Adapter> && requires(const typename Adapter::storage_type& storage) {
            { Adapter::SizeApprox(storage) } noexcept -> std::same_as<std::size_t>;
        };

    /** @brief Adapter additionally providing an allocation-free pop batch. */
    template<class Adapter>
    concept BatchQueueAdapter = AsyncQueueAdapter<Adapter> &&
                                requires(typename Adapter::storage_type& storage, typename Adapter::batch_type& batch) {
                                    typename Adapter::batch_type;
                                    { Adapter::capacity } -> std::same_as<const std::size_t&>;
                                    { Adapter::TryPopBatch(storage, batch) } noexcept -> std::same_as<bool>;
                                    requires std::default_initializable<typename Adapter::batch_type>;
                                    requires std::is_nothrow_move_constructible_v<typename Adapter::batch_type>;
                                    requires std::is_nothrow_destructible_v<typename Adapter::batch_type>;
                                };

    /** @brief Adapter additionally providing an all-or-nothing batch push primitive. */
    template<class Adapter>
    concept TransactionalBatchQueueAdapter =
        BatchQueueAdapter<Adapter> && ApproximateSizedQueueAdapter<Adapter> && Adapter::transactionalPushBatch &&
        requires(typename Adapter::storage_type& storage, typename Adapter::batch_type& batch) {
            { Adapter::TryPushBatch(storage, batch) } noexcept -> std::same_as<QueueTransferStatus>;
        };

    /** @brief Adapter contract bound to the storage type physically owned by an asynchronous queue. */
    template<class Adapter, class Storage>
    concept AsyncQueueAdapterFor = AsyncQueueAdapter<Adapter> && std::same_as<typename Adapter::storage_type, Storage>;

    namespace Detail {

        /** @brief Conservatively classify rings whose in-flight owners can create publication or reclamation holes. */
        [[nodiscard]] consteval QueueProgress RingProgress(QueueTopology topology) noexcept {
            if (topology.producers == QueueCardinality::Single && topology.consumers == QueueCardinality::Single) {
                return QueueProgress::WaitFree;
            }
            return QueueProgress::Unspecified;
        }

        /** @brief Local and dual-ring SPSC stacks are wait-free; shared tagged heads are lock-free. */
        [[nodiscard]] consteval QueueProgress StackProgress(QueueTopology topology) noexcept {
            return topology.sharing == QueueSharing::SingleThread || topology == QueueTopologies::Spsc
                       ? QueueProgress::WaitFree
                       : QueueProgress::LockFree;
        }

        template<class Adapter>
            requires BatchQueueAdapter<Adapter>
        [[nodiscard]] bool PopAvailable(typename Adapter::storage_type& storage,
                                        typename Adapter::batch_type& batch) noexcept {
            while (batch.size() != Adapter::capacity) {
                auto value = Adapter::TryPop(storage);
                if (!value.has_value()) {
                    break;
                }
                batch.Push(std::move(*value));
            }
            return !batch.empty();
        }

        template<class Adapter>
        [[nodiscard]] QueueTransferStatus PushSingleProducerBatch(typename Adapter::storage_type& storage,
                                                                  typename Adapter::batch_type& batch) noexcept {
            if (batch.oversized() || batch.size() > Adapter::capacity) {
                return QueueTransferStatus::BatchTooLarge;
            }
            if (batch.empty()) {
                return QueueTransferStatus::Committed;
            }
            const std::size_t used = Adapter::SizeApprox(storage);
            if (Adapter::capacity - used < batch.size()) {
                return QueueTransferStatus::BackPressure;
            }
            for (typename Adapter::value_type& value : batch) {
                if (Adapter::TryPush(storage, value) != QueueTransferStatus::Committed) [[unlikely]] {
                    std::terminate();
                }
            }
            batch.Clear();
            return QueueTransferStatus::Committed;
        }

    } // namespace Detail

    /** @brief Adapter for every compile-time-specialized bounded LIFO topology. */
    template<Mashiro::Detail::BoundedStackStorage Storage>
    struct QueueAdapter<Storage> {
        using storage_type = Storage;
        using value_type = typename Storage::value_type;
        using batch_type = QueueBatch<value_type, Storage::kCapacity>;

        static constexpr QueueOrder order = Storage::order;
        static constexpr QueueCardinality producerCardinality = Storage::topology.producers;
        static constexpr QueueCardinality consumerCardinality = Storage::topology.consumers;
        static constexpr QueueProgress pushProgress = Detail::StackProgress(Storage::topology);
        static constexpr QueueProgress popProgress = Detail::StackProgress(Storage::topology);
        static constexpr std::size_t capacity = Storage::kCapacity;
        static constexpr bool pushFailurePreservesInput = true;
        static constexpr bool transactionalPushBatch = true;

        [[nodiscard]] static QueueTransferStatus TryPush(storage_type& storage, value_type& value) noexcept {
            return storage.TryPush(std::move(value)) ? QueueTransferStatus::Committed
                                                     : QueueTransferStatus::BackPressure;
        }

        [[nodiscard]] static std::optional<value_type> TryPop(storage_type& storage) noexcept {
            return storage.TryPop();
        }

        [[nodiscard]] static QueueTransferStatus TryPushBatch(storage_type& storage, batch_type& batch) noexcept {
            if (batch.oversized() || batch.size() > capacity) {
                return QueueTransferStatus::BatchTooLarge;
            }
            if (!storage.TryPushBatch(batch)) {
                return QueueTransferStatus::BackPressure;
            }
            batch.Clear();
            return QueueTransferStatus::Committed;
        }

        [[nodiscard]] static bool TryPopBatch(storage_type& storage, batch_type& batch) noexcept {
            return Detail::PopAvailable<QueueAdapter>(storage, batch);
        }

        [[nodiscard]] static bool IsDrained(const storage_type& storage) noexcept { return storage.Empty(); }
        [[nodiscard]] static std::size_t SizeApprox(const storage_type& storage) noexcept {
            return storage.SizeApprox();
        }
    };

    /** @brief Uniform adapter for every compile-time-specialized ring-buffer topology. */
    template<Mashiro::Detail::RingBufferStorage Storage>
    struct QueueAdapter<Storage> {
        using storage_type = Storage;
        using value_type = typename Storage::value_type;
        using batch_type = QueueBatch<value_type, Storage::kCapacity>;

        static constexpr QueueOrder order = Storage::order;
        static constexpr QueueCardinality producerCardinality = Storage::topology.producers;
        static constexpr QueueCardinality consumerCardinality = Storage::topology.consumers;
        static constexpr QueueProgress pushProgress = Detail::RingProgress(Storage::topology);
        static constexpr QueueProgress popProgress = Detail::RingProgress(Storage::topology);
        static constexpr std::size_t capacity = Storage::kCapacity;
        static constexpr bool pushFailurePreservesInput = true;
        static constexpr bool transactionalPushBatch = Storage::transactionalPushBatch;

        [[nodiscard]] static QueueTransferStatus TryPush(storage_type& storage, value_type& value) noexcept {
            return storage.TryPush(std::move(value)) ? QueueTransferStatus::Committed
                                                     : QueueTransferStatus::BackPressure;
        }

        [[nodiscard]] static std::optional<value_type> TryPop(storage_type& storage) noexcept {
            return storage.TryPop();
        }

        [[nodiscard]] static QueueTransferStatus TryPushBatch(storage_type& storage, batch_type& batch) noexcept
            requires transactionalPushBatch
        {
            return Detail::PushSingleProducerBatch<QueueAdapter>(storage, batch);
        }

        [[nodiscard]] static bool TryPopBatch(storage_type& storage, batch_type& batch) noexcept {
            return Detail::PopAvailable<QueueAdapter>(storage, batch);
        }

        [[nodiscard]] static bool IsDrained(const storage_type& storage) noexcept { return storage.Empty(); }
        [[nodiscard]] static std::size_t SizeApprox(const storage_type& storage) noexcept {
            return storage.SizeApprox();
        }
    };

    /** @brief Adapter from packed variable-length byte storage to owning asynchronous queue values. */
    template<Mashiro::Detail::RecordRingBufferStorage Storage>
    struct QueueAdapter<Storage> {
        using storage_type = Storage;
        using value_type = BoundedByteRecord<Storage::kMaxRecordBytes>;

        static constexpr QueueOrder order = Storage::order;
        static constexpr QueueCardinality producerCardinality = Storage::topology.producers;
        static constexpr QueueCardinality consumerCardinality = Storage::topology.consumers;
        static constexpr QueueProgress pushProgress = Detail::RingProgress(Storage::topology);
        static constexpr QueueProgress popProgress = Detail::RingProgress(Storage::topology);
        static constexpr bool pushFailurePreservesInput = true;

        [[nodiscard]] static QueueTransferStatus TryPush(storage_type& storage, value_type& value) noexcept {
            if (value.empty()) {
                return QueueTransferStatus::InvalidValue;
            }
            return storage.TryWrite(value.Bytes()) ? QueueTransferStatus::Committed : QueueTransferStatus::BackPressure;
        }

        [[nodiscard]] static std::optional<value_type> TryPop(storage_type& storage) noexcept {
            value_type value;
            if (!TryPopInto(storage, value)) {
                return std::nullopt;
            }
            return std::optional<value_type>{std::move(value)};
        }

        [[nodiscard]] static bool TryPopInto(storage_type& storage, value_type& value) noexcept {
            return storage.TryConsume([&value](std::span<const std::byte> bytes) noexcept {
                if (!value.TryAssign(bytes)) [[unlikely]] {
                    std::terminate();
                }
            });
        }

        [[nodiscard]] static bool IsDrained(const storage_type& storage) noexcept { return storage.Empty(); }
    };

} // namespace Mashiro::Concurrency
