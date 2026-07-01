/**
 * @file QueueAdapter.h
 * @brief Compile-time adapters from Core queue storage types to experimental async queue semantics.
 * @ingroup Concurrency
 */
#pragma once

#include "Mashiro/Core/MpscQueue.h"
#include "Mashiro/Core/SpscRingBuffer.h"
#include "Mashiro/Experimental/Concurrency/QueueBatch.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace Mashiro::Experimental::Concurrency {

    /** @brief Compile-time role cardinality preserved by async queue ports. */
    enum class QueueRoleCardinality : std::uint8_t {
        Single,
        Multi,
    };

    /** @brief Result of a batch transfer attempt against bounded queue storage. */
    enum class QueueTransferStatus : std::uint8_t {
        Committed,
        BackPressure,
        BatchTooLarge,
        ContractViolation,
    };

    /** @brief Primary adapter declaration. Specialise for storage types. */
    template<class Storage>
    struct QueueAdapter;

    /** @brief Adapter concept consumed by @ref AsyncQueue. */
    template<class Adapter>
    concept AsyncQueueAdapter = requires(typename Adapter::storage_type storage,
                                         typename Adapter::value_type& value,
                                         typename Adapter::batch_type& batch) {
        typename Adapter::storage_type;
        typename Adapter::value_type;
        typename Adapter::batch_type;
        { Adapter::producer_cardinality } -> std::same_as<const QueueRoleCardinality&>;
        { Adapter::consumer_cardinality } -> std::same_as<const QueueRoleCardinality&>;
        { Adapter::capacity } -> std::convertible_to<std::size_t>;
        { Adapter::TryPush(storage, value) } noexcept -> std::same_as<bool>;
        { Adapter::TryPop(storage) } noexcept -> std::same_as<std::optional<typename Adapter::value_type>>;
        { Adapter::TryPushBatch(storage, batch) } noexcept -> std::same_as<QueueTransferStatus>;
        { Adapter::TryPopBatch(storage) } noexcept -> std::same_as<typename Adapter::batch_type>;
        { Adapter::IsEmpty(storage) } noexcept -> std::convertible_to<bool>;
        { Adapter::SizeApprox(storage) } noexcept -> std::convertible_to<std::size_t>;
        { batch.empty() } noexcept -> std::convertible_to<bool>;
        { batch.size() } noexcept -> std::convertible_to<std::size_t>;
        { batch.oversized() } noexcept -> std::convertible_to<bool>;
    };

    namespace Detail {

        template<class Adapter>
        [[nodiscard]] QueueTransferStatus BatchPreflight(typename Adapter::storage_type& storage,
                                                         const typename Adapter::batch_type& batch) noexcept {
            if (batch.oversized() || batch.size() > Adapter::capacity) {
                return QueueTransferStatus::BatchTooLarge;
            }
            if (batch.empty()) {
                return QueueTransferStatus::Committed;
            }
            const std::size_t used = Adapter::SizeApprox(storage);
            if (used > Adapter::capacity) [[unlikely]] {
                return QueueTransferStatus::ContractViolation;
            }
            if (Adapter::capacity - used < batch.size()) {
                return QueueTransferStatus::BackPressure;
            }
            return QueueTransferStatus::Committed;
        }

        template<class Adapter>
        [[nodiscard]] QueueTransferStatus CommitBatchAfterPreflight(typename Adapter::storage_type& storage,
                                                                    typename Adapter::batch_type& batch) noexcept {
            for (typename Adapter::value_type& value : batch) {
                if (!Adapter::TryPush(storage, value)) [[unlikely]] {
                    std::terminate();
                }
            }
            batch.Clear();
            return QueueTransferStatus::Committed;
        }

    } // namespace Detail

    /** @brief Adapter for the project's bounded lock-free many-producer/single-consumer mailbox. */
    template<class T, std::size_t Capacity>
    struct QueueAdapter<MpscQueue<T, Capacity>> {
        using storage_type = MpscQueue<T, Capacity>;
        using value_type = T;
        using batch_type = QueueBatch<T, Capacity>;

        static constexpr QueueRoleCardinality producer_cardinality = QueueRoleCardinality::Multi;
        static constexpr QueueRoleCardinality consumer_cardinality = QueueRoleCardinality::Single;
        static constexpr std::size_t capacity = Capacity;
        static constexpr bool push_failure_preserves_input = true;

        [[nodiscard]] static bool TryPush(storage_type& storage, value_type& value) noexcept {
            return storage.TryPush(std::move(value));
        }

        [[nodiscard]] static std::optional<value_type> TryPop(storage_type& storage) noexcept {
            return storage.TryPop();
        }

        /** @brief All-or-nothing while the async facade owns the only mutation path. */
        [[nodiscard]] static QueueTransferStatus TryPushBatch(storage_type& storage, batch_type& batch) noexcept {
            const QueueTransferStatus preflight = Detail::BatchPreflight<QueueAdapter>(storage, batch);
            if (preflight != QueueTransferStatus::Committed || batch.empty()) {
                return preflight;
            }
            return Detail::CommitBatchAfterPreflight<QueueAdapter>(storage, batch);
        }

        [[nodiscard]] static batch_type TryPopBatch(storage_type& storage) noexcept {
            batch_type batch;
            storage.Drain([&batch](value_type&& value) noexcept { batch.Push(std::move(value)); });
            return batch;
        }

        [[nodiscard]] static bool IsEmpty(storage_type& storage) noexcept { return storage.Empty(); }
        [[nodiscard]] static std::size_t SizeApprox(storage_type& storage) noexcept { return storage.ApproxSize(); }
    };

    /** @brief Adapter for the project's wait-free single-producer/single-consumer ring. */
    template<class T, std::uint32_t Capacity>
    struct QueueAdapter<SpscRingBuffer<T, Capacity>> {
        using storage_type = SpscRingBuffer<T, Capacity>;
        using value_type = T;
        using batch_type = QueueBatch<T, Capacity>;

        static constexpr QueueRoleCardinality producer_cardinality = QueueRoleCardinality::Single;
        static constexpr QueueRoleCardinality consumer_cardinality = QueueRoleCardinality::Single;
        static constexpr std::size_t capacity = Capacity;
        static constexpr bool push_failure_preserves_input = true;

        [[nodiscard]] static bool TryPush(storage_type& storage, value_type& value) noexcept {
            return storage.TryPush(std::move(value));
        }

        [[nodiscard]] static std::optional<value_type> TryPop(storage_type& storage) noexcept {
            return storage.TryPop();
        }

        /** @brief All-or-nothing while the async facade owns the only mutation path. */
        [[nodiscard]] static QueueTransferStatus TryPushBatch(storage_type& storage, batch_type& batch) noexcept {
            const QueueTransferStatus preflight = Detail::BatchPreflight<QueueAdapter>(storage, batch);
            if (preflight != QueueTransferStatus::Committed || batch.empty()) {
                return preflight;
            }
            return Detail::CommitBatchAfterPreflight<QueueAdapter>(storage, batch);
        }

        [[nodiscard]] static batch_type TryPopBatch(storage_type& storage) noexcept {
            batch_type batch;
            storage.Drain([&batch](value_type&& value) noexcept { batch.Push(std::move(value)); });
            return batch;
        }

        [[nodiscard]] static bool IsEmpty(storage_type& storage) noexcept { return storage.Empty(); }
        [[nodiscard]] static std::size_t SizeApprox(storage_type& storage) noexcept { return storage.SizeApprox(); }
    };

} // namespace Mashiro::Experimental::Concurrency