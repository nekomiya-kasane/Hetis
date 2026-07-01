/**
 * @file AsyncQueue.h
 * @brief Generic sender facade for bounded asynchronous queues.
 * @ingroup Concurrency
 */
#pragma once

#include <stdexec/execution.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "Mashiro/Concurrency/AtomicEventCount.h"

namespace Mashiro {

    template<class Storage>
    class AsyncQueue;

    namespace Detail {

        /** @brief Batch types must expose a no-throw emptiness predicate. */
        template<class Batch>
        concept AsyncQueueBatch = requires(const Batch batch) {
            { batch.empty() } noexcept -> std::convertible_to<bool>;
        };

        /**
         * @brief Storage contract required by @ref AsyncQueue.
         *
         * Push operations take lvalue references intentionally: a failed push must not consume its input, because the
         * sender may need to retry after waiting for space. @c PushBatch is all-or-nothing by the same rule. A
         * submitted batch must be capable of fitting into an empty storage instance; otherwise this five-function
         * contract cannot distinguish transient back-pressure from a permanently unsatisfiable request.
         */
        template<class Storage>
        concept AsyncQueueStorage =
            requires(Storage storage, typename Storage::value_type& value, typename Storage::batch_type& batch) {
                typename Storage::value_type;
                typename Storage::batch_type;
                requires AsyncQueueBatch<typename Storage::batch_type>;
                { storage.Pop() } noexcept -> std::same_as<std::optional<typename Storage::value_type>>;
                { storage.PopBatch() } noexcept -> std::same_as<typename Storage::batch_type>;
                { storage.Push(value) } noexcept -> std::same_as<bool>;
                { storage.PushBatch(batch) } noexcept -> std::same_as<bool>;
                { storage.IsEmpty() } noexcept -> std::convertible_to<bool>;
            };

        template<class Policy, class Receiver>
        struct QueueOpState {
            using Owner = typename Policy::owner_type;

            Owner* owner;
            Receiver receiver;
            [[no_unique_address]] Policy policy;

            struct OnStop {
                Owner* owner;
                void operator()() const noexcept { owner->NotifyStopWaiters(); }
            };

            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
            using StopCallback = typename StopToken::template callback_type<OnStop>;

            std::optional<StopCallback> stopCallback;

            void start() & noexcept {
                auto complete = [&]<class... Args>(Args&&... args) noexcept {
                    stopCallback.reset();
                    stdexec::set_value(std::move(receiver), std::forward<Args>(args)...);
                };

                if (Policy::TryRun(*owner, policy, complete)) {
                    return;
                }

                auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
                if (token.stop_requested()) {
                    stdexec::set_stopped(std::move(receiver));
                    return;
                }

                stopCallback.emplace(token, OnStop{owner});
                auto snapshot = Policy::LoadEpoch(*owner);

                for (;;) {
                    if (Policy::TryRun(*owner, policy, complete)) {
                        return;
                    }
                    if (token.stop_requested()) {
                        stopCallback.reset();
                        stdexec::set_stopped(std::move(receiver));
                        return;
                    }
                    Policy::WaitEpoch(*owner, snapshot);
                    snapshot = Policy::LoadEpoch(*owner);
                }
            }
        };

        template<class Policy>
        struct QueueSender {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = typename Policy::completion_signatures;

            typename Policy::owner_type* owner;
            [[no_unique_address]] Policy policy;

            template<class Receiver>
                requires std::copy_constructible<Policy>
            [[nodiscard]] auto connect(Receiver&& receiver) const& noexcept
                -> QueueOpState<Policy, std::remove_cvref_t<Receiver>> {
                return QueueOpState<Policy, std::remove_cvref_t<Receiver>>{
                    .owner = owner,
                    .receiver = std::forward<Receiver>(receiver),
                    .policy = policy,
                };
            }

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) && noexcept
                -> QueueOpState<Policy, std::remove_cvref_t<Receiver>> {
                return QueueOpState<Policy, std::remove_cvref_t<Receiver>>{
                    .owner = owner,
                    .receiver = std::forward<Receiver>(receiver),
                    .policy = std::move(policy),
                };
            }
        };

        template<class Owner>
        struct PopPolicy {
            using owner_type = Owner;
            using value_type = typename Owner::value_type;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(value_type), stdexec::set_stopped_t()>;

            [[nodiscard]] static uint64_t LoadEpoch(Owner& owner) noexcept { return owner.DataEpoch().Load(); }
            static void WaitEpoch(Owner& owner, uint64_t snapshot) noexcept { owner.DataEpoch().Wait(snapshot); }

            template<class Complete>
            [[nodiscard]] static bool TryRun(Owner& owner, PopPolicy&, Complete&& complete) noexcept {
                std::optional<value_type> value = owner.Storage().Pop();
                if (!value.has_value()) {
                    return false;
                }
                owner.NotifySpaceAvailable();
                complete(std::move(*value));
                return true;
            }
        };

        template<class Owner>
        struct PopBatchPolicy {
            using owner_type = Owner;
            using batch_type = typename Owner::batch_type;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(batch_type), stdexec::set_stopped_t()>;

            [[nodiscard]] static uint64_t LoadEpoch(Owner& owner) noexcept { return owner.DataEpoch().Load(); }
            static void WaitEpoch(Owner& owner, uint64_t snapshot) noexcept { owner.DataEpoch().Wait(snapshot); }

            template<class Complete>
            [[nodiscard]] static bool TryRun(Owner& owner, PopBatchPolicy&, Complete&& complete) noexcept {
                batch_type batch = owner.Storage().PopBatch();
                if (batch.empty()) {
                    return false;
                }
                owner.NotifySpaceAvailable();
                complete(std::move(batch));
                return true;
            }
        };

        template<class Owner>
        struct PushPolicy {
            using owner_type = Owner;
            using value_type = typename Owner::value_type;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;

            value_type value;

            [[nodiscard]] static uint64_t LoadEpoch(Owner& owner) noexcept { return owner.SpaceEpoch().Load(); }
            static void WaitEpoch(Owner& owner, uint64_t snapshot) noexcept { owner.SpaceEpoch().Wait(snapshot); }

            template<class Complete>
            [[nodiscard]] static bool TryRun(Owner& owner, PushPolicy& policy, Complete&& complete) noexcept {
                if (!owner.Storage().Push(policy.value)) {
                    return false;
                }
                owner.NotifyDataAvailable();
                complete();
                return true;
            }
        };

        template<class Owner>
        struct PushBatchPolicy {
            using owner_type = Owner;
            using batch_type = typename Owner::batch_type;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;

            batch_type batch;

            [[nodiscard]] static uint64_t LoadEpoch(Owner& owner) noexcept { return owner.SpaceEpoch().Load(); }
            static void WaitEpoch(Owner& owner, uint64_t snapshot) noexcept { owner.SpaceEpoch().Wait(snapshot); }

            template<class Complete>
            [[nodiscard]] static bool TryRun(Owner& owner, PushBatchPolicy& policy, Complete&& complete) noexcept {
                const bool publishesData = !policy.batch.empty();
                if (!owner.Storage().PushBatch(policy.batch)) {
                    return false;
                }
                if (publishesData) {
                    owner.NotifyDataAvailable();
                }
                complete();
                return true;
            }
        };

    } // namespace Detail

    /**
     * @brief Sender facade over a bounded queue exposing Pop, PopBatch, Push, PushBatch, and IsEmpty.
     *
     * @details @ref AsyncQueue owns no payload storage. It borrows a storage object and adds two wake planes: a data
     * epoch for consumers waiting on @ref Pop / @ref PopBatch, and a space epoch for producers waiting on @ref Push /
     * @ref PushBatch. All asynchronous access must go through this facade; bypassing it can mutate storage without
     * advancing the matching epoch, leaving waiters asleep.
     *
     * The facade preserves the storage's concurrency contract. For example, wrapping an SPSC queue still permits only
     * one producer and one consumer; wrapping an MPSC queue still permits only one consumer.
     */
    template<Detail::AsyncQueueStorage Storage>
    class AsyncQueue {
    public:
        using storage_type = Storage;
        using value_type = typename Storage::value_type;
        using batch_type = typename Storage::batch_type;

        explicit AsyncQueue(Storage& storage) noexcept : storage_(&storage) {}

        AsyncQueue(const AsyncQueue&) = delete;
        AsyncQueue& operator=(const AsyncQueue&) = delete;
        AsyncQueue(AsyncQueue&&) = delete;
        AsyncQueue& operator=(AsyncQueue&&) = delete;

        /** @brief Sender completing with one popped value when data becomes available. */
        [[nodiscard]] auto Pop() noexcept {
            return Detail::QueueSender<Detail::PopPolicy<AsyncQueue>>{.owner = this, .policy = {}};
        }

        /** @brief Sender completing with one non-empty popped batch when data becomes available. */
        [[nodiscard]] auto PopBatch() noexcept {
            return Detail::QueueSender<Detail::PopBatchPolicy<AsyncQueue>>{.owner = this, .policy = {}};
        }

        /** @brief Sender completing after @p value has been committed, waiting for space if necessary. */
        template<class Value>
            requires std::constructible_from<value_type, Value&&>
        [[nodiscard]] auto Push(Value&& value) noexcept(std::is_nothrow_constructible_v<value_type, Value&&>) {
            return Detail::QueueSender<Detail::PushPolicy<AsyncQueue>>{
                .owner = this,
                .policy = {.value = value_type(std::forward<Value>(value))},
            };
        }

        /** @brief Sender completing after @p batch has been committed all-or-nothing, waiting for enough space. */
        template<class Batch>
            requires std::constructible_from<batch_type, Batch&&>
        [[nodiscard]] auto PushBatch(Batch&& batch) noexcept(std::is_nothrow_constructible_v<batch_type, Batch&&>) {
            return Detail::QueueSender<Detail::PushBatchPolicy<AsyncQueue>>{
                .owner = this,
                .policy = {.batch = batch_type(std::forward<Batch>(batch))},
            };
        }

        /** @brief Direct non-waiting emptiness observation forwarded to storage. */
        [[nodiscard]] bool IsEmpty() const noexcept { return storage_->IsEmpty(); }

    private:
        template<class>
        friend struct Detail::PopPolicy;
        template<class>
        friend struct Detail::PopBatchPolicy;
        template<class>
        friend struct Detail::PushPolicy;
        template<class>
        friend struct Detail::PushBatchPolicy;
        template<class, class>
        friend struct Detail::QueueOpState;

        [[nodiscard]] Storage& GetStorage() noexcept { return *storage_; }
        [[nodiscard]] AtomicEventCount& DataEpoch() noexcept { return dataEpoch_; }
        [[nodiscard]] AtomicEventCount& SpaceEpoch() noexcept { return spaceEpoch_; }

        void NotifyDataAvailable() noexcept { dataEpoch_.NotifyOne(); }
        void NotifySpaceAvailable() noexcept { spaceEpoch_.NotifyAll(); }

        void NotifyStopWaiters() noexcept {
            dataEpoch_.NotifyAll();
            spaceEpoch_.NotifyAll();
        }

        Storage* storage_;
        AtomicEventCount dataEpoch_{};
        AtomicEventCount spaceEpoch_{};
    };

    template<class Storage>
    AsyncQueue(Storage&) -> AsyncQueue<Storage>;

} // namespace Mashiro
