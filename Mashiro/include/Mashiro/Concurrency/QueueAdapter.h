/**
 * @file QueueAdapter.h
 * @brief Explicit capability model connecting specialized queue storage to shared asynchronous endpoints.
 * @ingroup Concurrency
 */
#pragma once

#include "Mashiro/Concurrency/QueueBatch.h"
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
     * @brief Per-operation progress vocabulary.
     *
     * @details @c NonBlocking means the try operation performs no lock or kernel wait but does not promise formal
     * lock-free system progress, for example when an earlier reserved FIFO cell can delay visibility.
     */
    enum class QueueProgress : std::uint8_t {
        WaitFree,
        LockFree,
        NonBlocking,
    };

    /** @brief Result of an optional transactional batch push. */
    enum class QueueTransferStatus : std::uint8_t {
        Committed,
        BackPressure,
        BatchTooLarge,
    };

    /** @brief Primary adapter declaration. Unsupported storage has no accidental fallback. */
    template<class Storage>
    struct QueueAdapter;

    /** @brief Common storage contract required by @ref AsyncQueue. */
    template<class Adapter>
    concept AsyncQueueAdapter =
        requires(typename Adapter::storage_type& storage, const typename Adapter::storage_type& constStorage,
                 typename Adapter::value_type& value, typename Adapter::batch_type& batch) {
            typename Adapter::storage_type;
            typename Adapter::value_type;
            typename Adapter::batch_type;
            { Adapter::producerCardinality } -> std::same_as<const QueueCardinality&>;
            { Adapter::consumerCardinality } -> std::same_as<const QueueCardinality&>;
            { Adapter::pushProgress } -> std::same_as<const QueueProgress&>;
            { Adapter::popProgress } -> std::same_as<const QueueProgress&>;
            { Adapter::capacity } -> std::same_as<const std::size_t&>;
            { Adapter::pushFailurePreservesInput } -> std::same_as<const bool&>;
            { Adapter::transactionalPushBatch } -> std::same_as<const bool&>;
            requires Adapter::pushFailurePreservesInput;
            { Adapter::TryPush(storage, value) } noexcept -> std::same_as<bool>;
            { Adapter::TryPop(storage) } noexcept -> std::same_as<std::optional<typename Adapter::value_type>>;
            { Adapter::TryPopBatch(storage, batch) } noexcept -> std::same_as<bool>;
            { Adapter::IsDrained(constStorage) } noexcept -> std::same_as<bool>;
            { Adapter::SizeApprox(constStorage) } noexcept -> std::same_as<std::size_t>;
            requires std::default_initializable<typename Adapter::batch_type>;
            requires std::is_nothrow_move_constructible_v<typename Adapter::value_type>;
            requires std::is_nothrow_move_constructible_v<typename Adapter::batch_type>;
            requires std::is_nothrow_destructible_v<typename Adapter::value_type>;
            requires std::is_nothrow_destructible_v<typename Adapter::batch_type>;
        };

    /** @brief Adapter additionally providing an all-or-nothing batch push primitive. */
    template<class Adapter>
    concept TransactionalBatchQueueAdapter =
        AsyncQueueAdapter<Adapter> && Adapter::transactionalPushBatch &&
        requires(typename Adapter::storage_type& storage, typename Adapter::batch_type& batch) {
            { Adapter::TryPushBatch(storage, batch) } noexcept -> std::same_as<QueueTransferStatus>;
        };

    /** @brief Adapter contract bound to the storage type physically owned by an asynchronous queue. */
    template<class Adapter, class Storage>
    concept AsyncQueueAdapterFor = AsyncQueueAdapter<Adapter> && std::same_as<typename Adapter::storage_type, Storage>;

    namespace Detail {

        template<class Adapter>
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
                if (!Adapter::TryPush(storage, value)) [[unlikely]] {
                    std::terminate();
                }
            }
            batch.Clear();
            return QueueTransferStatus::Committed;
        }

    } // namespace Detail

    /** @brief Uniform adapter for every compile-time-specialized ring-buffer topology. */
    template<Mashiro::Detail::RingBufferStorage Storage>
    struct QueueAdapter<Storage> {
        using storage_type = Storage;
        using value_type = typename Storage::value_type;
        using batch_type = QueueBatch<value_type, Storage::kCapacity>;

        static constexpr QueueCardinality producerCardinality = Storage::topology.producers;
        static constexpr QueueCardinality consumerCardinality = Storage::topology.consumers;
        static constexpr QueueProgress pushProgress =
            producerCardinality == QueueCardinality::Single ? QueueProgress::WaitFree : QueueProgress::NonBlocking;
        static constexpr QueueProgress popProgress =
            consumerCardinality == QueueCardinality::Single ? QueueProgress::WaitFree : QueueProgress::NonBlocking;
        static constexpr std::size_t capacity = Storage::kCapacity;
        static constexpr bool pushFailurePreservesInput = true;
        static constexpr bool transactionalPushBatch = Storage::transactionalPushBatch;

        [[nodiscard]] static bool TryPush(storage_type& storage, value_type& value) noexcept {
            return storage.TryPush(std::move(value));
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

} // namespace Mashiro::Concurrency
